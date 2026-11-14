/*
   Copyright 2026 Bloomberg Finance L.P.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

/*
 * Ordered apply, serial step (docs/adr/0003-ordered-apply-with-row-intents.md,
 * staging step 1.1).
 *
 * A transaction in plan mode does not modify btrees as its operations run.
 * The ll_* write routines record each btree operation in the transaction's
 * plan instead (prepare).  When the block processor has run every operation,
 * the transaction takes its place in a single queue (sequence), and when its
 * turn comes it replays the plan through the same ll_* routines under its
 * Berkeley transaction (apply).  The turn is kept until the top-level
 * transaction commits or aborts, so at most one plan-mode transaction has
 * uncommitted changes on the pages at any time.
 *
 * A plan holds adds, deletes and updates of rows, blobs and keys.  What it
 * cannot hold refuses, which the caller sees as a deadlock; it then runs the
 * request again with page locks held to commit, as before.
 *
 * A transaction recording a plan reads without taking part in locking: each
 * read is under a transaction of its own that does not wait and is over at
 * once, and its result is laid over the plan, so the transaction sees its
 * own earlier operations.  Nothing protects what was read from changing
 * before the plan is applied.  The btree is the judge at apply time: a row is
 * updated or deleted by its genid, which names one version of the row and is
 * never used again, and a unique key is added with a put that refuses a
 * duplicate.  A plan that fails that way was built on something that is no
 * longer there; its request runs again without a plan and gets its answer
 * under page locks.
 *
 * A transaction waiting for its turn should hold no page lock, or it can
 * deadlock with the one that has the turn.  bdb_tran_plan_apply() checks
 * that.
 */

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "bdb_int.h"
#include "locks.h"
#include "sys_wrap.h"
#include "logmsg.h"
#include "thread_util.h"
#include "comdb2_atomic.h"
#include "genid.h"
#include <time.h>
#include <unistd.h>
#include <build/db.h>

enum { PLAN_DTA_ADD = 1, PLAN_KEY_ADD = 2, PLAN_DTA_DEL = 3, PLAN_KEY_DEL = 4, PLAN_DTA_UPD = 5, PLAN_KEY_UPD = 6 };

/* plan_op.upd: how ll_dta_upd_int() is to be called for a PLAN_DTA_UPD, and
 * whether a PLAN_KEY_UPD carries a payload */
enum { PLAN_UPD_BLOB_OPT = 1, PLAN_UPD_USE_NEW_GENID = 2, PLAN_UPD_HAS_DTA = 4 };

/* one btree operation; the key bytes and then the data bytes follow it */
struct plan_op {
    struct plan_op *next;
    struct plan_op *gnext; /* next operation on the same file */
    bdb_state_type *bdb_state;
    DB *dbp;
    unsigned long long genid;
    unsigned long long genid2; /* updates: the genid the row or key ends up with */
    int type;
    int upd;
    int stripeid; /* updates: participant stripe id */
    int ixnum;
    int dtafile;
    int dtastripe;
    int flags;
    int odhready;
    uint32_t keylen;
    uint32_t datalen;
};

struct plan_block {
    struct plan_block *next;
    size_t used;
    size_t size;
};

enum { PLAN_BLOCK_SIZE = 64 * 1024 };

struct bdb_plan {
    struct plan_op *first;
    struct plan_op *last;
    struct plan_block *blocks;
    int nops;
    int applying;    /* replaying: ll_* routines write for real */
    int unsupported; /* refused an operation: run the request without a plan */
    int deferred;    /* to be applied in a commit unit, not by this transaction */
    /* the unique keys this plan adds, hashed, to catch one added twice */
    struct plan_op **uniq;
    int nuniq;
    int uniqcap;
    int64_t bytes; /* estimate of the log this plan will write */
    int turn_held;
    /* the rows this plan has read, written or removed, hashed by genid */
    struct plan_row *rows;
    int nrows;
    int rowcap;
    int one_thread; /* has an operation that writes two files: not to be split by file */
    int row_reads;  /* rows read from a data file, with no lock kept on them */
};

/* What a plan knows about a row.  ROW_READ: it is in the data file, or was
 * when this plan read it, and `size` is its size there.  ROW_PLAN: this plan
 * wrote it, with operation `op`, whose payload is the row as it will be
 * stored.  ROW_GONE: this plan deleted it or updated it into another genid. */
enum { ROW_READ = 1, ROW_PLAN = 2, ROW_GONE = 3 };
struct plan_row {
    bdb_state_type *bdb_state;
    unsigned long long genid;
    int state;
    uint32_t size;
    struct plan_op *op;
};

/* The apply turn is a write lock in the Berkeley lock manager, held by the
 * top-level transaction.  The lock manager queues waiters in order and wakes
 * only the next one, and the deadlock detector sees a wait for the turn like
 * any other.  That matters when a transaction that is not in plan mode sits
 * between two that are: it can hold a page the turn's owner wants while it
 * waits for a table lock held by a transaction waiting for the turn. */
static char turn_name[16] = "ordered apply";

int berkdb_lock(DB_ENV *dbenv, int lid, int flags, DBT *lkname, int mode, DB_LOCK *lk);

int64_t gbl_ordered_apply_txns;      /* plans applied */
int64_t gbl_ordered_apply_ops;       /* btree operations applied from plans */
int64_t gbl_ordered_apply_fallbacks; /* plans given up for page locks to commit */
int64_t gbl_ordered_apply_waits;     /* plans that waited for the turn */
int64_t gbl_ordered_apply_wait_us;   /* time spent waiting for the turn */
int64_t gbl_ordered_apply_held_us;   /* time the turn was held */
int64_t gbl_ordered_apply_parallel;  /* plans applied a file per thread */
int64_t gbl_ordered_apply_apply_us;  /* time spent writing plans to the btrees */
int64_t gbl_ordered_apply_units;        /* commit units committed */
int64_t gbl_ordered_apply_unit_retries; /* commit units aborted and applied again */

/* On: plans are not applied by the transactions that made them.  One thread
 * takes the plans that are waiting, applies them all under one transaction of
 * its own together with their blkseq records, and commits them as a unit.
 * The transactions that made the plans then commit with nothing of their own
 * to write, at the unit's LSN.  There is no turn to hand from thread to
 * thread, and a unit costs one commit record however many plans are in it. */
int gbl_ordered_apply_unit = 0;
/* On: a plan can add to an index that allows no duplicates.  Off: such a
 * request runs without a plan, as it did before. */
int gbl_ordered_apply_unique = 1;
int64_t gbl_ordered_apply_dup_retries; /* plans sent back because a unique key they add was in the index at apply */
int64_t gbl_ordered_apply_dup_unread;  /* unique keys not looked for while planning because the page was locked */
/* On: a plan can update and delete rows.  Off: a request that does either
 * runs without a plan. */
int gbl_ordered_apply_update = 1;
int64_t gbl_ordered_apply_stale_retries; /* plans sent back because a row or key they change was gone at apply */
int64_t gbl_ordered_apply_read_locked;   /* rows a plan gave up reading because the page stayed locked */
/* On: a transaction whose plan is the only one in existence applies it
 * itself, as a unit of one, on its own thread; any that arrive meanwhile
 * queue until it is done.  A lone writer then never waits for another thread
 * to be woken.  With other plans being prepared the unit thread is about to
 * be needed anyway, and it applies them with its caches warm. */
int gbl_ordered_apply_unit_inline = 1;
static int64_t plans_alive; /* plans started and not yet freed */
int64_t gbl_ordered_apply_units_inline; /* units of one applied by their own transaction */
/* A member waiting for its unit sleeps on a condition variable, which the
 * deadlock detector cannot see.  After this many milliseconds it waits in
 * the lock manager instead, for a lock the unit being applied holds, so that
 * a cycle through the member's own locks is found. */
int gbl_ordered_apply_unit_detect_ms = 100;
int64_t gbl_ordered_apply_unit_slow_waits; /* members that went on to wait in the lock manager */
int64_t gbl_ordered_apply_unit_kicks;      /* times a member was a deadlock victim and aborted its waiters */
static char unit_name[16] = "ordered unit";
/* A unit stops taking plans once it has this many btree operations. */
int gbl_ordered_apply_unit_max_ops = 4096;

/* On: the next plan does not start applying until this one's transaction has
 * committed, which is serial apply as the ADR describes it.  Off: the next
 * plan starts as soon as this one is applied.  The page locks taken in apply
 * are held to commit either way, so with it off a later plan can be on other
 * pages while this one commits, and waits in the lock manager for any page
 * they share.  That wait cannot deadlock against another plan: a transaction
 * that is past apply never waits for one that is not. */
int gbl_ordered_apply_serial_commit = 1;

/* Off: a wait for the turn does not run the deadlock detector.  The detector
 * walks every locker with the lock region held, once for each wait, and with
 * many transactions queued for the turn that costs more than applying a small
 * plan.  For measurement only: with it off, a deadlock whose last edge is a
 * wait for the turn is found only when some other lock wait runs the
 * detector. */
int gbl_ordered_apply_turn_detect = 1;

/* Threads that help apply a plan, one file each; 0 applies every plan on the
 * thread that owns it. */
int gbl_ordered_apply_threads = 3;
/* Plans with fewer operations than this are not worth handing out. */
int gbl_ordered_apply_parallel_min_ops = 64;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int turn_acquire(bdb_state_type *bdb_state, tran_type *top)
{
    DBT name = {0};
    name.data = turn_name;
    name.size = sizeof(turn_name);
    uint64_t start = now_us();
    int rc = berkdb_lock(bdb_state->dbenv, top->tid->txnid, gbl_ordered_apply_turn_detect ? 0 : DB_LOCK_NODETECT, &name,
                         DB_LOCK_WRITE, &top->plan_turn_lock);
    if (rc != 0)
        return rc;
    uint64_t waited = now_us() - start;
    if (waited > 20) {
        ATOMIC_ADD64(gbl_ordered_apply_waits, 1);
        ATOMIC_ADD64(gbl_ordered_apply_wait_us, waited);
    }
    top->plan_turn_held = 1;
    top->plan_turn_start_us = now_us();
    return 0;
}

static void *plan_alloc(struct bdb_plan *plan, size_t sz)
{
    sz = (sz + 7) & ~(size_t)7;
    struct plan_block *blk = plan->blocks;
    if (blk == NULL || blk->size - blk->used < sz) {
        size_t size = sz > PLAN_BLOCK_SIZE ? sz : PLAN_BLOCK_SIZE;
        blk = malloc(sizeof(struct plan_block) + size);
        if (blk == NULL)
            return NULL;
        blk->size = size;
        blk->used = 0;
        blk->next = plan->blocks;
        plan->blocks = blk;
    }
    void *out = (uint8_t *)(blk + 1) + blk->used;
    blk->used += sz;
    return out;
}

static void plan_free(struct bdb_plan *plan)
{
    struct plan_block *blk = plan->blocks;
    while (blk) {
        struct plan_block *next = blk->next;
        free(blk);
        blk = next;
    }
    free(plan->uniq);
    free(plan->rows);
    free(plan);
    ATOMIC_ADD64(plans_alive, -1);
}

static struct plan_op *plan_new_op(struct bdb_plan *plan, int type, bdb_state_type *bdb_state, DBT *key, DBT *data)
{
    /* Only plans made of adds are applied a file per thread.  A thread that
     * has finished its file keeps its page locks until every file is done,
     * so the files are locked in no particular order.  A reader that holds a
     * data page and wants the blob page behind it can then be in a cycle
     * with two threads of one unit, and such a cycle was seen to stand
     * (recovdlock_emptyblob, run 16).  Updates and deletes go to pages
     * readers are on; adds mostly do not. */
    if (type != PLAN_DTA_ADD && type != PLAN_KEY_ADD)
        plan->one_thread = 1;
    struct plan_op *op = plan_alloc(plan, sizeof(struct plan_op) + key->size + data->size);
    if (op == NULL)
        return NULL;
    memset(op, 0, sizeof(*op));
    op->type = type;
    op->bdb_state = bdb_state;
    op->keylen = key->size;
    op->datalen = data->size;
    uint8_t *buf = (uint8_t *)(op + 1);
    if (key->size)
        memcpy(buf, key->data, key->size);
    if (data->size)
        memcpy(buf + key->size, data->data, data->size);
    if (plan->last)
        plan->last->next = op;
    else
        plan->first = op;
    plan->last = op;
    plan->nops++;
    /* one add record for each operation: the item and its header */
    plan->bytes += key->size + data->size + 96;
    return op;
}

int bdb_tran_plan_start(tran_type *tran)
{
    if (tran == NULL || tran->tranclass != TRANCLASS_BERK || tran->plan)
        return -1;
    tran->plan = calloc(1, sizeof(struct bdb_plan));
    if (tran->plan == NULL)
        return -1;
    ATOMIC_ADD64(plans_alive, 1);
    return 0;
}

/* Is this transaction recording its writes instead of doing them? */
int bdb_plan_recording(tran_type *tran)
{
    return tran && tran->plan && !tran->plan->applying;
}

int bdb_tran_plan_active(tran_type *tran)
{
    return bdb_plan_recording(tran);
}

/* An estimate of the log a transaction's plan will write when applied, for
 * limits that are checked as a request runs. */
int64_t bdb_tran_plan_bytes(tran_type *tran)
{
    return (tran && tran->plan) ? tran->plan->bytes : 0;
}

int bdb_tran_plan_unsupported(tran_type *tran)
{
    return tran && tran->plan && tran->plan->unsupported;
}

/* Has a transaction that is recording a plan read or written a btree in the
 * ordinary way?  It holds a page lock if so.  Such a read did not see the
 * plan, so an error the request then fails with may be wrong: a row or key
 * "not found" may be in the plan. */
/* Has a transaction that is recording a plan read rows from a data file?
 * Nothing keeps such a row as it was read.  The plan is judged when it is
 * applied, but an error the request finds for itself is not judged by
 * anything: a later read may have seen what another transaction committed
 * to that row in the meantime, and reported a duplicate key where the row
 * read first was simply gone.  Only "no such row" can be believed, because
 * a genid that is gone does not come back. */
int bdb_tran_plan_has_reads(tran_type *tran)
{
    return bdb_plan_recording(tran) && tran->plan->row_reads > 0;
}

static int holds_page_locks(bdb_state_type *bdb_state, tran_type *tran);

int bdb_tran_plan_dirty(bdb_state_type *bdb_state, tran_type *tran)
{
    if (!bdb_plan_recording(tran))
        return 0;
    return holds_page_locks(bdb_state->parent ? bdb_state->parent : bdb_state, tran);
}

/* A plan-mode transaction reached something the plan cannot hold.  The caller
 * returns this rc, which every layer above already treats as a deadlock: the
 * transaction is aborted and the request runs again, this time without a
 * plan. */
int bdb_plan_refuse(tran_type *tran)
{
    tran->plan->unsupported = 1;
    return DB_LOCK_DEADLOCK;
}

void bdb_tran_plan_set_unsupported(tran_type *tran)
{
    if (tran && tran->plan)
        tran->plan->unsupported = 1;
}

static int row_note(struct bdb_plan *plan, bdb_state_type *bdb_state, unsigned long long genid, int state,
                    uint32_t size, struct plan_op *op);

int bdb_plan_dta_add(bdb_state_type *bdb_state, unsigned long long genid, DB *dbp, tran_type *tran, int dtafile,
                     int dtastripe, DBT *dbt_key, DBT *dbt_data, int flags, int odhready)
{
    /* Put the on-disk header on the row now, compressing it if the table
     * asks for that, so that apply writes the payload as it is. */
    DBT packed;
    void *freeptr = NULL;
    if (bdb_state->ondisk_header && !odhready) {
        int updateid = -1;
        if (ip_updates_enabled(bdb_state) && dbt_key->size >= 8)
            updateid = get_updateid_from_genid(bdb_state, *(unsigned long long *)dbt_key->data);
        if (bdb_prepare_put_pack_updateid(bdb_state, dtafile > 0 ? 1 : 0, dbt_data, &packed, updateid, &freeptr, NULL,
                                          0) != 0)
            return bdb_plan_refuse(tran);
        dbt_data = &packed;
        odhready = 1;
    }
    struct plan_op *op = plan_new_op(tran->plan, PLAN_DTA_ADD, bdb_state, dbt_key, dbt_data);
    free(freeptr);
    if (op == NULL)
        return ENOMEM;
    op->dbp = dbp;
    op->genid = genid;
    op->dtafile = dtafile;
    op->dtastripe = dtastripe;
    op->flags = flags;
    op->odhready = odhready;
    /* a later read of this row by genid is answered from the plan */
    if (dtafile == 0 && row_note(tran->plan, bdb_state, genid, ROW_PLAN, 0, op) != 0)
        return ENOMEM;
    return 0;
}

/*
 * Unique keys.
 *
 * Adding a key to an index that allows no duplicates is refused if the key
 * is there already.  A transaction writing as it goes finds that out from
 * the put.  A plan finds out in two steps:
 *
 * - When the add is recorded, the plan is searched for an earlier add of the
 *   same key, and the index is read for it.  The read never waits for a
 *   lock: a transaction recording a plan must not block on a page, because
 *   the lock may belong to a commit unit that is waiting for a lock of this
 *   transaction's, or to this transaction's own parent, and neither wait
 *   would ever end.  A key found gives the duplicate error the put would
 *   have, at the point in the request where the put would have given it.  A
 *   page that is locked gives nothing, and the add is recorded.
 * - When the plan is applied the put itself still refuses a duplicate: the
 *   key was behind a lock when the plan looked, or was added afterwards.
 *   The apply is abandoned and the request runs again without a plan, under
 *   page locks, where the put reports the duplicate in the ordinary way.  In
 *   a commit unit only the member whose key it was goes back; the others are
 *   applied again without it.
 *
 * So the btree stays the judge of uniqueness, as it is under page locks, and
 * a plan can be wrong only in the direction of trying again.
 */
static uint32_t uniq_hash(bdb_state_type *bdb_state, int ixnum, const uint8_t *key, uint32_t len)
{
    uint32_t h = 2166136261u ^ (uint32_t)(uintptr_t)bdb_state ^ ((uint32_t)ixnum * 16777619u);
    for (uint32_t i = 0; i < len; i++)
        h = (h ^ key[i]) * 16777619u;
    return h;
}

/* Finds the slot for this key: the op that adds it, or the empty slot where
 * it would go. */
static struct plan_op **uniq_slot(struct bdb_plan *plan, bdb_state_type *bdb_state, int ixnum, const uint8_t *key,
                                  uint32_t len)
{
    uint32_t i = uniq_hash(bdb_state, ixnum, key, len) & (plan->uniqcap - 1);
    for (;;) {
        struct plan_op *op = plan->uniq[i];
        if (op == NULL || (op->bdb_state == bdb_state && op->ixnum == ixnum && op->keylen == len &&
                           memcmp(op + 1, key, len) == 0))
            return &plan->uniq[i];
        i = (i + 1) & (plan->uniqcap - 1);
    }
}

static int uniq_grow(struct bdb_plan *plan)
{
    int oldcap = plan->uniqcap;
    struct plan_op **old = plan->uniq;
    int cap = oldcap ? oldcap * 2 : 64;
    struct plan_op **tbl = calloc(cap, sizeof(struct plan_op *));
    if (tbl == NULL)
        return ENOMEM;
    plan->uniq = tbl;
    plan->uniqcap = cap;
    for (int i = 0; i < oldcap; i++) {
        struct plan_op *op = old[i];
        if (op)
            *uniq_slot(plan, op->bdb_state, op->ixnum, (uint8_t *)(op + 1), op->keylen) = op;
    }
    free(old);
    return 0;
}

/* Is the key in the index now?  Returns DB_KEYEXIST if it is, and 0 if it is
 * not or if that could not be found out without waiting for a lock.  The
 * read is under a transaction of its own that does not wait and is over when
 * this returns, so it leaves no lock behind. */
static int uniq_read(bdb_state_type *bdb_state, int ixnum, DBT *dbt_key)
{
    DB_ENV *dbenv = bdb_state->parent ? bdb_state->parent->dbenv : bdb_state->dbenv;
    DB *dbp = bdb_state->dbp_ix[ixnum];
    DB_TXN *tid;
    DBC *dbcp;
    DBT key = {0}, data = {0};

    if (dbenv->txn_begin(dbenv, NULL, &tid, DB_TXN_NOWAIT) != 0)
        return 0;
    int rc = dbp->cursor(dbp, tid, &dbcp, 0);
    if (rc == 0) {
        key.data = dbt_key->data;
        key.size = dbt_key->size;
        /* nothing of the entry is wanted, only whether it is there */
        data.flags = DB_DBT_USERMEM | DB_DBT_PARTIAL;
        rc = dbcp->c_get(dbcp, &key, &data, DB_SET);
        dbcp->c_close(dbcp);
    } else {
        rc = DB_LOCK_NOTGRANTED;
    }
    tid->abort(tid);
    if (rc == 0)
        return DB_KEYEXIST;
    if (rc != DB_NOTFOUND)
        ATOMIC_ADD64(gbl_ordered_apply_dup_unread, 1);
    return 0;
}

int bdb_plan_key_add(bdb_state_type *bdb_state, unsigned long long genid, tran_type *tran, int ixnum, DBT *dbt_key,
                     DBT *dbt_data)
{
    struct bdb_plan *plan = tran->plan;
    struct plan_op **slot = NULL;

    if (!bdb_state->ixdups[ixnum]) {
        if (!gbl_ordered_apply_unique)
            return bdb_plan_refuse(tran);
        if (plan->nuniq * 2 >= plan->uniqcap && uniq_grow(plan) != 0)
            return ENOMEM;
        slot = uniq_slot(plan, bdb_state, ixnum, dbt_key->data, dbt_key->size);
        if (*slot && (*slot)->type != PLAN_KEY_DEL)
            return DB_KEYEXIST;
        /* A key this plan deletes first is in the index now and will not be
         * when the add is applied. */
        if (*slot == NULL) {
            int rc = uniq_read(bdb_state, ixnum, dbt_key);
            if (rc != 0)
                return rc;
        }
    }
    struct plan_op *op = plan_new_op(plan, PLAN_KEY_ADD, bdb_state, dbt_key, dbt_data);
    if (op == NULL)
        return ENOMEM;
    op->genid = genid;
    op->ixnum = ixnum;
    if (slot) {
        if (*slot == NULL)
            plan->nuniq++;
        *slot = op;
    }
    return 0;
}

/*
 * Rows.
 *
 * An update or a delete starts by reading the row, and what it does next
 * depends on what the row holds: which keys to delete, which to change.  A
 * plan reads the row without a lock that outlasts the read, so the row can be
 * updated or deleted by another transaction before the plan is applied.  The
 * genid settles it.  A genid names one version of one row; an update gives
 * the row a new one, and none is used twice.  The plan's update or delete of
 * the data file is by genid, and when it is applied it finds that genid or
 * it does not.  If it does, the row is the one the plan read, and the keys
 * the plan worked out from it are the keys in the indexes.  If it does not,
 * the request runs again without a plan and gets the verify error, or
 * whatever else is now the answer, under page locks.
 *
 * So the data file judges updates and deletes as an index judges a unique
 * key, and no stamp is kept for a row.
 *
 * The plan keeps a table of the rows it has met.  A read looks there first:
 * a row this plan added or updated is returned from the plan, and one it
 * deleted or updated away is not found.  Only then is the data file read.
 */
static struct plan_row *row_slot(struct bdb_plan *plan, bdb_state_type *bdb_state, unsigned long long genid)
{
    uint32_t i = (uint32_t)((genid * 0x9E3779B97F4A7C15ULL) >> 32) & (plan->rowcap - 1);
    for (;;) {
        struct plan_row *r = &plan->rows[i];
        if (r->state == 0 || (r->genid == genid && r->bdb_state == bdb_state))
            return r;
        i = (i + 1) & (plan->rowcap - 1);
    }
}

static struct plan_row *row_find(struct bdb_plan *plan, bdb_state_type *bdb_state, unsigned long long genid)
{
    if (plan->rowcap == 0)
        return NULL;
    struct plan_row *r = row_slot(plan, bdb_state, genid);
    return r->state ? r : NULL;
}

static int row_note(struct bdb_plan *plan, bdb_state_type *bdb_state, unsigned long long genid, int state,
                    uint32_t size, struct plan_op *op)
{
    if ((plan->nrows + 1) * 2 > plan->rowcap) {
        int oldcap = plan->rowcap;
        struct plan_row *old = plan->rows;
        int cap = oldcap ? oldcap * 2 : 32;
        struct plan_row *tbl = calloc(cap, sizeof(struct plan_row));
        if (tbl == NULL)
            return ENOMEM;
        plan->rows = tbl;
        plan->rowcap = cap;
        for (int i = 0; i < oldcap; i++) {
            if (old[i].state)
                *row_slot(plan, old[i].bdb_state, old[i].genid) = old[i];
        }
        free(old);
    }
    struct plan_row *r = row_slot(plan, bdb_state, genid);
    if (r->state == 0)
        plan->nrows++;
    r->bdb_state = bdb_state;
    r->genid = genid;
    r->state = state;
    r->size = size;
    r->op = op;
    return 0;
}

/* A page that is locked is tried again this many times, a little later each
 * time, before the plan is given up.  The lock is most often a commit unit's,
 * and is gone when the unit commits. */
enum { PLAN_READ_TRIES = 6 };

/* Reads a row from its data file, as it is stored.  Returns 0 with the row
 * in *out, for the caller to free; DB_NOTFOUND; or DB_LOCK_NOTGRANTED if the
 * page stayed locked.  Each try is under a transaction of its own that does
 * not wait and is over before this returns, as in uniq_read(). */
static int row_read(bdb_state_type *bdb_state, unsigned long long genid, DBT *out)
{
    DB_ENV *dbenv = bdb_state->parent ? bdb_state->parent->dbenv : bdb_state->dbenv;
    if (bdb_state->attr->dtastripe) {
        int stripe = get_dtafile_from_genid(genid);
        if (stripe < 0 || stripe >= bdb_state->attr->dtastripe)
            return DB_NOTFOUND;
    }
    DB *dbp = get_dbp_from_genid(bdb_state, 0, genid, NULL);
    unsigned long long search = get_search_genid(bdb_state, genid);

    for (int try = 0;; try++) {
        DB_TXN *tid;
        DBC *dbcp;
        DBT key = {0};
        memset(out, 0, sizeof(*out));
        out->flags = DB_DBT_MALLOC;
        int rc = dbenv->txn_begin(dbenv, NULL, &tid, DB_TXN_NOWAIT);
        if (rc == 0) {
            rc = dbp->cursor(dbp, tid, &dbcp, 0);
            if (rc == 0) {
                key.data = &search;
                key.size = sizeof(search);
                rc = dbcp->c_get(dbcp, &key, out, DB_SET);
                dbcp->c_close(dbcp);
            }
            tid->abort(tid);
        }
        if (rc == 0 || rc == DB_NOTFOUND)
            return rc;
        if (try + 1 >= PLAN_READ_TRIES)
            return DB_LOCK_NOTGRANTED;
        usleep(50 << try);
    }
}

/* Stands in for bdb_fetch_by_rrn_and_genid_tran() while a plan is being
 * recorded: the row with this genid, from the plan if the plan wrote it,
 * otherwise from the data file.  Returns 0 with the row in dta, its length
 * in *reqdtalen and its schema version in *ver, or -1 with bdberr set:
 * BDBERR_FETCH_DTA if there is no such row, BDBERR_DEADLOCK if the plan has
 * been given up and the request is to run again without one. */
int bdb_tran_plan_fetch_genid(bdb_state_type *bdb_state, tran_type *tran, unsigned long long genid, void *dta,
                              int dtalen, int *reqdtalen, uint8_t *ver, int *bdberr)
{
    struct bdb_plan *plan = tran->plan;
    struct plan_row *r = row_find(plan, bdb_state, genid);
    DBT row = {0};
    const void *stored;
    uint32_t storedlen;
    int updateid = -1;
    struct odh odh;

    *bdberr = BDBERR_NOERROR;
    if (r && r->state == ROW_GONE) {
        *bdberr = BDBERR_FETCH_DTA;
        return -1;
    }
    if (r && r->state == ROW_PLAN) {
        stored = (uint8_t *)(r->op + 1) + r->op->keylen;
        storedlen = r->op->datalen;
    } else {
        int rc = row_read(bdb_state, genid, &row);
        if (rc == DB_NOTFOUND) {
            *bdberr = BDBERR_FETCH_DTA;
            return -1;
        }
        if (rc != 0) {
            ATOMIC_ADD64(gbl_ordered_apply_read_locked, 1);
            plan->unsupported = 1;
            *bdberr = BDBERR_DEADLOCK;
            return -1;
        }
        stored = row.data;
        storedlen = row.size;
        /* the row at this place in the file may be a later version */
        if (ip_updates_enabled(bdb_state))
            updateid = get_updateid_from_genid(bdb_state, genid);
    }

    /* with no buffer the caller only asks whether the row is there, and the
     * row is not uncompressed */
    int rc = bdb_unpack(bdb_state, stored, storedlen, dta, dtalen, &odh, NULL);
    if (rc == 0 && updateid >= 0 && odh.updateid != updateid)
        rc = DB_NOTFOUND;
    if (rc == 0 && dta != NULL && odh.recptr != dta) {
        if (odh.length > dtalen)
            rc = EINVAL;
        else
            memcpy(dta, odh.recptr, odh.length);
    }
    if (rc == 0) {
        *reqdtalen = odh.length;
        *ver = odh.csc2vers;
        if (r == NULL || r->state != ROW_PLAN)
            plan->row_reads++;
        if (r == NULL && row_note(plan, bdb_state, genid, ROW_READ, storedlen, NULL) != 0)
            rc = ENOMEM;
    }
    free(row.data);
    if (rc == 0)
        return 0;
    if (rc == DB_NOTFOUND) {
        *bdberr = BDBERR_FETCH_DTA;
        return -1;
    }
    plan->unsupported = 1;
    *bdberr = BDBERR_DEADLOCK;
    return -1;
}

int bdb_plan_dta_del(bdb_state_type *bdb_state, tran_type *tran, unsigned long long genid, DB *dbp, int dtafile,
                     int dtastripe, DBT *dta_out)
{
    struct bdb_plan *plan = tran->plan;
    DBT none = {0};
    uint32_t size = 0;

    if (dta_out || !gbl_ordered_apply_update)
        return bdb_plan_refuse(tran);
    if (dtafile == 0) {
        struct plan_row *r = row_find(plan, bdb_state, genid);
        if (r && r->state == ROW_GONE)
            return DB_NOTFOUND;
        if (r)
            size = r->state == ROW_PLAN ? r->op->datalen : r->size;
    }
    /* A blob is deleted whether or not the row has one; apply does not mind
     * one that is not there.  A row this plan added is deleted like any
     * other: its add comes first in the plan. */
    struct plan_op *op = plan_new_op(plan, PLAN_DTA_DEL, bdb_state, &none, &none);
    if (op == NULL)
        return ENOMEM;
    op->dbp = dbp;
    op->genid = genid;
    op->dtafile = dtafile;
    op->dtastripe = dtastripe;
    plan->bytes += size; /* a delete logs what it removes */
    if (dtafile == 0 && row_note(plan, bdb_state, genid, ROW_GONE, 0, NULL) != 0)
        return ENOMEM;
    return 0;
}

int bdb_plan_key_del(bdb_state_type *bdb_state, tran_type *tran, int ixnum, void *key, int keylen,
                     unsigned long long genid)
{
    struct bdb_plan *plan = tran->plan;
    struct plan_op **slot = NULL;
    DBT dbt_key = {0}, none = {0};

    if (!gbl_ordered_apply_update)
        return bdb_plan_refuse(tran);
    dbt_key.data = key;
    dbt_key.size = keylen;
    if (!bdb_state->ixdups[ixnum]) {
        if (plan->nuniq * 2 >= plan->uniqcap && uniq_grow(plan) != 0)
            return ENOMEM;
        slot = uniq_slot(plan, bdb_state, ixnum, key, keylen);
    }
    struct plan_op *op = plan_new_op(plan, PLAN_KEY_DEL, bdb_state, &dbt_key, &none);
    if (op == NULL)
        return ENOMEM;
    op->genid = genid;
    op->ixnum = ixnum;
    /* the last thing this plan does with a unique key says whether a later
     * add of it is a duplicate */
    if (slot) {
        if (*slot == NULL)
            plan->nuniq++;
        *slot = op;
    }
    return 0;
}

int bdb_plan_key_upd(bdb_state_type *bdb_state, tran_type *tran, unsigned long long oldgenid,
                     unsigned long long genid, void *key, int ixnum, int keylen, void *dta, int dtalen)
{
    DBT dbt_key = {0}, dbt_dta = {0};

    if (!gbl_ordered_apply_update)
        return bdb_plan_refuse(tran);
    dbt_key.data = key;
    dbt_key.size = keylen;
    if (dta) {
        dbt_dta.data = dta;
        dbt_dta.size = dtalen;
    }
    struct plan_op *op = plan_new_op(tran->plan, PLAN_KEY_UPD, bdb_state, &dbt_key, &dbt_dta);
    if (op == NULL)
        return ENOMEM;
    op->genid = oldgenid;
    op->genid2 = genid;
    op->ixnum = ixnum;
    op->upd = dta ? PLAN_UPD_HAS_DTA : 0;
    return 0;
}

/* Takes the arguments of ll_dta_upd_int().
 *
 * A row's new genid is chosen here, since the caller builds keys from it
 * before the plan is applied.  An update that keeps the row's place in the
 * file is recorded as one operation.  One that moves it is recorded as a
 * delete and an add, which may be in two stripes and so in two files.
 *
 * A blob update is recorded as it was called and decided at apply, because
 * what it does depends on whether the row has that blob.  One that changes
 * the genid may write two files, and its plan is then applied on one
 * thread. */
int bdb_plan_dta_upd(bdb_state_type *bdb_state, unsigned long long oldgenid, unsigned long long *newgenid, DB *dbp,
                     tran_type *tran, int dtafile, int dtastripe, int participantstripid, int use_new_genid,
                     DBT *verify_dta, DBT *dta, DBT *old_dta_out, int has_blob_update_optimization,
                     int keep_genid_intact, int odhready)
{
    struct bdb_plan *plan = tran->plan;
    bdb_state_type *parent = bdb_state->parent ? bdb_state->parent : bdb_state;
    DBT none = {0};
    struct plan_op *op;

    if (!gbl_ordered_apply_update || keep_genid_intact || verify_dta || old_dta_out || newgenid == NULL)
        return bdb_plan_refuse(tran);

    if (dtafile > 0) {
        if (*newgenid == 0)
            return bdb_plan_refuse(tran);
        op = plan_new_op(plan, PLAN_DTA_UPD, bdb_state, &none, dta ? dta : &none);
        if (op == NULL)
            return ENOMEM;
        op->dbp = dbp;
        op->genid = oldgenid;
        op->genid2 = *newgenid;
        op->dtafile = dtafile;
        op->dtastripe = dtastripe;
        op->stripeid = participantstripid;
        op->odhready = odhready;
        op->upd = (has_blob_update_optimization ? PLAN_UPD_BLOB_OPT : 0) |
                  (use_new_genid ? PLAN_UPD_USE_NEW_GENID : 0) | (dta ? PLAN_UPD_HAS_DTA : 0);
        if (bdb_inplace_cmp_genids(bdb_state, oldgenid, *newgenid) != 0)
            plan->one_thread = 1;
        return 0;
    }

    /* A row.  The genid is chosen as ll_dta_upd_int() chooses it. */
    if (dta == NULL || *newgenid != 0 || !bdb_state->ondisk_header)
        return bdb_plan_refuse(tran);

    int64_t oldsize = -1;
    struct plan_row *r = row_find(plan, bdb_state, oldgenid);
    if (r) {
        if (r->state == ROW_GONE)
            return DB_NOTFOUND;
        oldsize = r->state == ROW_PLAN ? r->op->datalen : r->size;
    }

    DBT packed;
    void *freeptr = NULL;
    if (bdb_prepare_put_pack_updateid(bdb_state, 0, dta, &packed, -1, &freeptr, NULL, odhready) != 0)
        return bdb_plan_refuse(tran);

    int inplace = 0;
    int updateid = 0;
    unsigned long long genid;
    if (ip_updates_enabled(bdb_state)) {
        int newupd = 1 + get_updateid_from_genid(bdb_state, oldgenid);
        if (newupd <= max_updateid(bdb_state)) {
            inplace = 1;
            updateid = newupd;
        }
    }
    /* a row that grows is moved, so that a page in the middle of the file
     * does not split under a scan in page order */
    if (inplace && bdb_attr_get(bdb_state->attr, BDB_ATTR_PAGE_ORDER_TABLESCAN) &&
        (oldsize < 0 || packed.size > oldsize))
        inplace = 0;

    if (inplace) {
        genid = set_updateid(bdb_state, updateid, oldgenid);
    } else {
        int newstripe = bdb_state->attr->disable_update_stripe_change ? get_dtafile_from_genid(oldgenid)
                                                                      : bdb_get_active_stripe_int(bdb_state);
        genid = get_genid(bdb_state, newstripe);
    }
    if (parent->attr->updategenids && participantstripid > 0)
        genid = set_participant_stripeid(bdb_state, participantstripid, genid);
    else
        genid = set_participant_stripeid(bdb_state, 0, genid);

    int rc = ENOMEM;
    if (inplace) {
        op = plan_new_op(plan, PLAN_DTA_UPD, bdb_state, &none, &packed);
        if (op) {
            op->dbp = dbp;
            op->genid = oldgenid;
            op->genid2 = genid;
            op->dtastripe = dtastripe;
            op->stripeid = participantstripid;
            op->odhready = 1;
            /* the genid is settled: apply must not choose another */
            op->upd = PLAN_UPD_HAS_DTA | PLAN_UPD_USE_NEW_GENID;
            rc = 0;
        }
    } else {
        struct plan_op *del = plan_new_op(plan, PLAN_DTA_DEL, bdb_state, &none, &none);
        DBT dbt_key = {0};
        int addstripe = 0;
        DB *dbp_add = get_dbp_from_genid(bdb_state, 0, genid, &addstripe);
        dbt_key.data = &genid;
        dbt_key.size = sizeof(genid);
        op = del ? plan_new_op(plan, PLAN_DTA_ADD, bdb_state, &dbt_key, &packed) : NULL;
        if (op) {
            del->dbp = dbp;
            del->genid = oldgenid;
            del->dtastripe = dtastripe;
            if (oldsize > 0)
                plan->bytes += oldsize;
            op->dbp = dbp_add;
            op->genid = genid;
            op->dtastripe = addstripe;
            op->flags = DB_NOOVERWRITE;
            op->odhready = 1;
            poke_updateid((uint8_t *)(op + 1) + op->keylen, 0);
            rc = 0;
        }
    }
    free(freeptr);
    if (rc == 0 && (row_note(plan, bdb_state, oldgenid, ROW_GONE, 0, NULL) != 0 ||
                    row_note(plan, bdb_state, genid, ROW_PLAN, 0, op) != 0))
        rc = ENOMEM;
    if (rc == 0)
        *newgenid = genid;
    return rc;
}

/* A plan applied to the btrees fails this way when what it was built on is
 * no longer there: a unique key it adds is in the index, or a row or key it
 * changes is not. */
static int rc_is_stale(int rc)
{
    return rc == DB_KEYEXIST || rc == DB_NOTFOUND;
}

static void count_stale(int rc)
{
    if (rc == DB_KEYEXIST)
        ATOMIC_ADD64(gbl_ordered_apply_dup_retries, 1);
    else
        ATOMIC_ADD64(gbl_ordered_apply_stale_retries, 1);
}

static int apply_op(struct plan_op *op, tran_type *tran)
{
    DBT key = {0}, data = {0};
    uint8_t *buf = (uint8_t *)(op + 1);
    key.data = buf;
    key.size = key.ulen = op->keylen;
    key.flags = DB_DBT_USERMEM;
    data.data = buf + op->keylen;
    data.size = data.ulen = op->datalen;
    data.flags = DB_DBT_USERMEM;
    int rc;
    switch (op->type) {
    case PLAN_DTA_ADD:
        return ll_dta_add(op->bdb_state, op->genid, op->dbp, tran, op->dtafile, op->dtastripe, &key, &data, op->flags,
                          op->odhready);
    case PLAN_KEY_ADD:
        return ll_key_add(op->bdb_state, op->genid, tran, op->ixnum, &key, &data);
    case PLAN_DTA_DEL:
        rc = ll_dta_del(op->bdb_state, tran, 2, op->genid, op->dbp, op->dtafile, op->dtastripe, NULL);
        /* a blob that is null is not stored */
        return (rc == DB_NOTFOUND && op->dtafile > 0) ? 0 : rc;
    case PLAN_KEY_DEL:
        return ll_key_del(op->bdb_state, tran, op->ixnum, buf, op->keylen, 2, op->genid, NULL);
    case PLAN_KEY_UPD:
        return ll_key_upd(op->bdb_state, tran, op->bdb_state->name, op->genid, op->genid2, buf, op->ixnum, op->keylen,
                          (op->upd & PLAN_UPD_HAS_DTA) ? buf + op->keylen : NULL, op->datalen);
    case PLAN_DTA_UPD:
        rc = ll_dta_upd_planned(op->bdb_state, op->genid, op->genid2, op->dbp, tran, op->dtafile, op->dtastripe,
                                op->stripeid, (op->upd & PLAN_UPD_USE_NEW_GENID) != 0,
                                (op->upd & PLAN_UPD_HAS_DTA) ? &data : NULL, (op->upd & PLAN_UPD_BLOB_OPT) != 0,
                                op->odhready);
        return (rc == DB_NOTFOUND && op->dtafile > 0) ? 0 : rc;
    }
    return EINVAL;
}

/*
 * Applying a plan a file per thread.
 *
 * The plan's operations are split by btree file.  Each file gets a child of
 * the planning transaction, so that each thread writes its own chain of log
 * records; a transaction's records are linked through txn->last_lsn, which
 * only one thread may update.  The children are begun and committed on the
 * thread that owns the plan, and a child's commit writes the __txn_child
 * record that joins its chain to the parent's.  Replicants and recovery
 * already collect a transaction's children this way.
 *
 * The children are all begun on one thread, which has up to two transactions
 * open already, and a thread may have TXN_TD_MAX (64) open: berkdb/txn/txn.c.
 * A plan that touches more than PLAN_MAX_FILES files is applied on one
 * thread.
 */
enum { PLAN_MAX_FILES = 48 };

struct plan_file {
    DB *db;
    struct plan_op *first;
    struct plan_op *last;
    tran_type tran; /* stands in for the planning transaction in the ll_* routines */
    int rc;
    struct plan_op *failed; /* the operation that gave rc */
    struct plan_file *next; /* in the helpers' queue */
};

static pthread_mutex_t helper_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t helper_work = PTHREAD_COND_INITIALIZER;
static pthread_cond_t helper_done = PTHREAD_COND_INITIALIZER;
static struct plan_file *helper_first;
static struct plan_file *helper_last;
static int helper_pending;
static int nhelpers;

static void apply_file(struct plan_file *f)
{
    int rc = 0;
    for (struct plan_op *op = f->first; op && rc == 0; op = op->gnext) {
        rc = apply_op(op, &f->tran);
        if (rc != 0)
            f->failed = op;
    }
    f->rc = rc;
}

static void *plan_helper(void *arg)
{
    bdb_state_type *bdb_state = arg;
    thread_started("ordered apply");
    bdb_thread_event(bdb_state, BDBTHR_EVENT_START);
    Pthread_mutex_lock(&helper_lk);
    for (;;) {
        struct plan_file *f = helper_first;
        if (f == NULL) {
            Pthread_cond_wait(&helper_work, &helper_lk);
            continue;
        }
        helper_first = f->next;
        if (helper_first == NULL)
            helper_last = NULL;
        Pthread_mutex_unlock(&helper_lk);
        apply_file(f);
        Pthread_mutex_lock(&helper_lk);
        if (--helper_pending == 0)
            Pthread_cond_signal(&helper_done);
    }
    return NULL;
}

/* called with the apply turn held, so by one thread at a time */
static int start_helpers(bdb_state_type *bdb_state)
{
    int want = gbl_ordered_apply_threads;
    if (want > PLAN_MAX_FILES)
        want = PLAN_MAX_FILES;
    while (nhelpers < want) {
        pthread_t tid;
        pthread_attr_t attr;
        Pthread_attr_init(&attr);
        Pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        Pthread_attr_setstacksize(&attr, 1024 * 1024);
        int rc = pthread_create(&tid, &attr, plan_helper, bdb_state);
        Pthread_attr_destroy(&attr);
        if (rc != 0)
            break;
        nhelpers++;
    }
    return nhelpers;
}

static DB *op_file(struct plan_op *op)
{
    switch (op->type) {
    case PLAN_DTA_ADD:
    case PLAN_DTA_DEL:
    case PLAN_DTA_UPD:
        return op->dbp;
    }
    return op->bdb_state->dbp_ix[op->ixnum];
}

static int plan_needs_one_chain(struct bdb_plan *plan)
{
    /* logical log records are chained through the top-level transaction */
    for (struct plan_op *op = plan->first; op; op = op->next) {
        if (op->bdb_state->attr->llog || op->bdb_state->logical_live_sc)
            return 1;
    }
    return 0;
}

/* Sets *done if it applied the plan, and returns the rc of that, with the
 * operation that failed in *failed.  Otherwise the plan is to be applied on
 * this thread alone. */
static int apply_parallel(bdb_state_type *bdb_state, tran_type *tran, struct bdb_plan *plan, int *done,
                          struct plan_op **failed)
{
    struct plan_file files[PLAN_MAX_FILES];
    int nfiles = 0;

    if (gbl_ordered_apply_threads <= 0 || plan->nops < gbl_ordered_apply_parallel_min_ops)
        return 0;
    if (plan->one_thread || plan_needs_one_chain(plan))
        return 0;

    for (struct plan_op *op = plan->first; op; op = op->next) {
        DB *db = op_file(op);
        int i;
        for (i = 0; i < nfiles && files[i].db != db; i++)
            ;
        if (i == nfiles) {
            if (nfiles == PLAN_MAX_FILES)
                return 0;
            memset(&files[i], 0, sizeof(files[i]));
            files[i].db = db;
            nfiles++;
        }
        op->gnext = NULL;
        if (files[i].last)
            files[i].last->gnext = op;
        else
            files[i].first = op;
        files[i].last = op;
    }
    if (nfiles < 2 || start_helpers(bdb_state) == 0)
        return 0;

    *done = 1;
    DB_ENV *dbenv = bdb_state->dbenv;
    u_int32_t flags = bdb_state->attr->synctransactions ? 0 : DB_TXN_NOSYNC;
    int rc = 0;
    int begun = 0;
    for (; begun < nfiles; begun++) {
        struct plan_file *f = &files[begun];
        f->tran.tranclass = TRANCLASS_BERK;
        f->tran.parent = tran->parent;
        f->tran.threadid = tran->threadid;
        f->tran.plan_verify = 1;
        if ((rc = dbenv->txn_begin(dbenv, tran->tid, &f->tran.tid, flags)) != 0)
            break;
    }

    if (rc == 0) {
        /* the helpers take all but the first file; this thread applies that
         * one, then whatever the helpers have not started on */
        Pthread_mutex_lock(&helper_lk);
        for (int i = 1; i < nfiles; i++) {
            files[i].next = NULL;
            if (helper_last)
                helper_last->next = &files[i];
            else
                helper_first = &files[i];
            helper_last = &files[i];
            helper_pending++;
        }
        Pthread_cond_broadcast(&helper_work);
        Pthread_mutex_unlock(&helper_lk);

        apply_file(&files[0]);

        Pthread_mutex_lock(&helper_lk);
        for (;;) {
            struct plan_file *f = helper_first;
            if (f == NULL)
                break;
            helper_first = f->next;
            if (helper_first == NULL)
                helper_last = NULL;
            Pthread_mutex_unlock(&helper_lk);
            apply_file(f);
            Pthread_mutex_lock(&helper_lk);
            helper_pending--;
        }
        while (helper_pending > 0)
            Pthread_cond_wait(&helper_done, &helper_lk);
        Pthread_mutex_unlock(&helper_lk);

        /* If several files failed, report one that says a plan was stale:
         * that one names a request to send back. */
        for (int i = 0; i < nfiles; i++) {
            if (files[i].rc != 0 && (rc == 0 || (!rc_is_stale(rc) && rc_is_stale(files[i].rc)))) {
                rc = files[i].rc;
                *failed = files[i].failed;
            }
        }
    }

    /* Every child ends here, before the planning transaction writes anything
     * else.  If any file failed they all abort; the caller then aborts the
     * planning transaction. */
    for (int i = 0; i < begun; i++) {
        DB_TXN *tid = files[i].tran.tid;
        int crc = (rc == 0) ? tid->commit(tid, flags) : tid->abort(tid);
        if (crc != 0 && rc == 0)
            rc = crc;
    }
    if (rc == 0)
        ATOMIC_ADD64(gbl_ordered_apply_parallel, 1);
    return rc;
}

/*
 * The commit lock.
 *
 * The block processor has a lock that every request takes shared from the
 * point where it can no longer fail to the end of its commit, and that a
 * serializable or SELECTV request takes exclusively, so that nothing commits
 * between its check of what it read and its own commit.  It is a pthread
 * rwlock.  That was safe while a request took it with all its page locks
 * already in hand and nothing left to wait for.  A request with a plan takes
 * its page locks afterwards, itself or through the unit its plan is in.  So
 * a request can now wait for a page while it holds the commit lock, and the
 * holder of that page can be waiting for the commit lock; the deadlock
 * detector sees neither end of that.
 *
 * With ordered apply on, a request therefore takes this lock first: the same
 * lock in the lock manager, shared or exclusive in the same way, under its
 * top-level transaction, which gives it up when it commits or aborts.  A
 * wait for it is one the detector follows.  Once a request has it, the
 * rwlock behind it can only be held by requests that have nothing left to
 * wait for.
 */
static char commit_name[16] = "commit lock";

int bdb_tran_commit_gate(bdb_state_type *bdb_state, tran_type *tran, int exclusive, int *bdberr)
{
    DBT name = {0};
    DB_LOCK lk;

    *bdberr = BDBERR_NOERROR;
    if (tran == NULL || tran->tid == NULL)
        return 0;
    if (bdb_state->parent)
        bdb_state = bdb_state->parent;
    name.data = commit_name;
    name.size = sizeof(commit_name);
    int rc = berkdb_lock(bdb_state->dbenv, tran->tid->txnid, 0, &name, exclusive ? DB_LOCK_WRITE : DB_LOCK_READ, &lk);
    if (rc == 0)
        return 0;
    *bdberr = (rc == BDBERR_DEADLOCK) ? BDBERR_DEADLOCK : BDBERR_MISC;
    return -1;
}

static int holds_page_locks(bdb_state_type *bdb_state, tran_type *tran)
{
    DB_ENV *dbenv = bdb_state->dbenv;
    for (; tran; tran = tran->parent) {
        u_int32_t n = 0;
        if (tran->tid == NULL)
            continue;
        if (dbenv->lock_locker_pagelockcount(dbenv, tran->tid->txnid, &n) == 0 && n > 0)
            return 1;
    }
    return 0;
}

/* Sequence and apply.  Returns 0, or -1 with bdberr set: BDBERR_DEADLOCK means
 * abort and run the request again. */
int bdb_tran_plan_apply(bdb_state_type *bdb_state, tran_type *tran, int *bdberr)
{
    struct bdb_plan *plan = tran ? tran->plan : NULL;
    int rc = 0;

    *bdberr = BDBERR_NOERROR;
    if (plan == NULL || plan->applying)
        return 0;
    if (bdb_state->parent)
        bdb_state = bdb_state->parent;

    /* Given up while the request ran, by something whose error did not
     * make it back here: still not to be applied. */
    if (plan->unsupported) {
        *bdberr = BDBERR_DEADLOCK;
        return -1;
    }

    plan->applying = 1;
    if (plan->nops == 0)
        return 0;

    /* The transaction that has the turn may want any page.  A page lock
     * here means something read or wrote under this transaction without
     * going through the plan; waiting for the turn with it would deadlock
     * again and again. */
    if (holds_page_locks(bdb_state, tran)) {
        plan->unsupported = 1;
        *bdberr = BDBERR_DEADLOCK;
        return -1;
    }

    /* Commit units: leave the plan as it is.  It goes to the parent when
     * this transaction commits, and from there into a unit.  Logical log
     * records need the transaction that owns them, so those plans take the
     * turn as before. */
    if (gbl_ordered_apply_unit && tran->parent && !plan_needs_one_chain(plan)) {
        plan->applying = 0;
        plan->deferred = 1;
        return 0;
    }

    tran_type *top = tran;
    while (top->parent)
        top = top->parent;
    rc = turn_acquire(bdb_state, top);
    if (rc != 0) {
        *bdberr = (rc == BDBERR_DEADLOCK) ? BDBERR_DEADLOCK : BDBERR_MISC;
        return -1;
    }

    int done = 0;
    struct plan_op *failed = NULL;
    tran->plan_verify = 1;
    rc = apply_parallel(bdb_state, tran, plan, &done, &failed);
    if (!done) {
        for (struct plan_op *op = plan->first; op && rc == 0; op = op->next)
            rc = apply_op(op, tran);
    }

    ATOMIC_ADD64(gbl_ordered_apply_apply_us, now_us() - top->plan_turn_start_us);
    if (!gbl_ordered_apply_serial_commit) {
        top->plan_turn_held = 0;
        ATOMIC_ADD64(gbl_ordered_apply_held_us, now_us() - top->plan_turn_start_us);
        bdb_release_lock(bdb_state, &top->plan_turn_lock);
    }

    if (rc == 0) {
        ATOMIC_ADD64(gbl_ordered_apply_txns, 1);
        ATOMIC_ADD64(gbl_ordered_apply_ops, plan->nops);
        return 0;
    }
    if (rc_is_stale(rc)) {
        /* a unique key this plan adds is in the index, or a row it changes
         * is gone: run without a plan, and let that report it */
        count_stale(rc);
        plan->unsupported = 1;
        *bdberr = BDBERR_DEADLOCK;
    } else if (rc == DB_LOCK_DEADLOCK || rc == DB_REP_HANDLE_DEAD) {
        *bdberr = BDBERR_DEADLOCK;
    } else {
        logmsg(LOGMSG_ERROR, "%s: applying a plan failed rc %d\n", __func__, rc);
        *bdberr = BDBERR_MISC;
    }
    return -1;
}

/* A transaction must not commit with operations still in its plan: they
 * would be lost with no error. */
void bdb_plan_check_commit(tran_type *tran)
{
    if (tran->plan && tran->plan->deferred && tran->parent && tran->parent->unit_plan == NULL) {
        tran->parent->unit_plan = tran->plan;
        tran->plan = NULL;
    }
    if (tran->unit_plan) {
        logmsg(LOGMSG_FATAL, "%s: committing a transaction whose plan of %d operations is in no unit\n", __func__,
               tran->unit_plan->nops);
        abort();
    }
    if (tran->plan && !tran->plan->applying && tran->plan->nops > 0) {
        logmsg(LOGMSG_FATAL, "%s: committing a transaction with %d planned operations not applied\n", __func__,
               tran->plan->nops);
        abort();
    }
}

/* The transaction is over, committed or aborted.  Called just before the
 * tran object is freed. */
void bdb_plan_tran_done(tran_type *tran)
{
    if (tran->plan) {
        if (tran->plan->unsupported)
            ATOMIC_ADD64(gbl_ordered_apply_fallbacks, 1);
        plan_free(tran->plan);
        tran->plan = NULL;
    }
    if (tran->unit_plan) {
        /* aborted before its plan reached a unit */
        plan_free(tran->unit_plan);
        tran->unit_plan = NULL;
    }
    if (tran->plan_turn_held) {
        /* the turn went with the rest of the transaction's locks */
        tran->plan_turn_held = 0;
        ATOMIC_ADD64(gbl_ordered_apply_held_us, now_us() - tran->plan_turn_start_us);
    }
}

/*
 * Commit units.
 *
 * A transaction whose plan was left for a unit queues here as a member and
 * sleeps.  The unit thread takes the members that are waiting, begins a
 * transaction, and for each member writes its blkseq record and applies its
 * plan.  It commits once and wakes the members with the commit LSN.  A member
 * whose blkseq key is already there is a replay: none of its plan is applied
 * and it is told so.
 *
 * Every page lock a unit takes belongs to the one transaction, so plans in a
 * unit never wait for each other, and the log holds one ordinary transaction.
 * If the unit is picked as a deadlock victim against a transaction outside
 * it, it aborts, which also takes its blkseq records back out, and the same
 * members are applied again.
 *
 * The members keep their table locks while they wait, so the handles in
 * their plans stay valid.  They hold no page locks: bdb_tran_plan_apply()
 * checked that before leaving the plan for a unit.
 *
 * Deadlocks through a member.  A unit can wait for a page held by a
 * transaction that is itself waiting for a lock a member holds.  The member
 * sleeps on a condition variable, so the detector sees no cycle.  Two things
 * close that.  A transaction that applies its own plan does it under its own
 * Berkeley transaction, not a unit's, so its locks and its waits belong to
 * one family.  And every unit holds a write lock on one name; a member that
 * has waited gbl_ordered_apply_unit_detect_ms asks for that name in read
 * mode, which is a wait the detector follows.  If the member is then chosen
 * as the victim it cannot give up its locks, since its plan may be half
 * applied; it aborts the transactions waiting for them, and waits again.
 */
struct unit_member {
    struct unit_member *next;
    struct bdb_plan *plan;
    void *bskey;
    int bskeylen;
    void *bsdata;
    int bsdatalen;
    void **replay_data;
    int *replay_len;
    int rc;
    int bdberr;
    int dup;
    int stale; /* its plan no longer fits the btrees: left out, to run again without a plan */
    DB_LSN lsn;
    int done;
    pthread_cond_t cond;
};

static pthread_mutex_t unit_lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t unit_work = PTHREAD_COND_INITIALIZER;
static struct unit_member *unit_first;
static struct unit_member *unit_last;
static int unit_thread_started;
static int unit_busy; /* a unit is being applied, by the unit thread or by its own transaction */

enum { UNIT_MAX_RETRIES = 100 };

static void unit_fail(struct unit_member *batch, int bdberr)
{
    for (struct unit_member *m = batch; m; m = m->next) {
        m->rc = -1;
        m->bdberr = bdberr;
    }
}

static void unit_drop_replays(struct unit_member *batch)
{
    for (struct unit_member *m = batch; m; m = m->next) {
        if (m->dup && m->replay_data) {
            free(*m->replay_data);
            *m->replay_data = NULL;
        }
        m->dup = 0;
    }
}

static void unit_run(bdb_state_type *bdb_state, struct unit_member *batch)
{
    int retries = 0;

    for (;;) {
        int bdberr = 0;
        int rc = 0;
        int nplans = 0;
        struct bdb_plan all = {0};
        uint64_t start = now_us();

        tran_type *tran = bdb_tran_begin(bdb_state, NULL, &bdberr);
        if (tran == NULL) {
            unit_fail(batch, bdberr ? bdberr : BDBERR_MISC);
            return;
        }
        tran->plan_verify = 1;

        /* members that have waited long wait for this; it goes with the
         * transaction's other locks when it commits or aborts */
        {
            DBT name = {0};
            DB_LOCK lk;
            name.data = unit_name;
            name.size = sizeof(unit_name);
            rc = berkdb_lock(bdb_state->dbenv, tran->tid->txnid, 0, &name, DB_LOCK_WRITE, &lk);
        }

        /* blkseq records first: they say which members are replays.  The
         * plans of the rest are joined into one list of operations. */
        for (struct unit_member *m = batch; m && rc == 0; m = m->next) {
            if (m->stale)
                continue;
            if (m->bskey) {
                rc = bdb_blkseq_insert(bdb_state, tran, m->bskey, m->bskeylen, m->bsdata, m->bsdatalen, m->replay_data,
                                       m->replay_len, 0);
                if (rc == IX_DUP) {
                    m->dup = 1;
                    rc = 0;
                    continue;
                }
                if (rc != 0)
                    break;
            }
            m->plan->last->next = NULL;
            if (all.last)
                all.last->next = m->plan->first;
            else
                all.first = m->plan->first;
            all.last = m->plan->last;
            all.nops += m->plan->nops;
            all.one_thread |= m->plan->one_thread;
            nplans++;
        }

        struct plan_op *failed = NULL;
        if (rc == 0 && all.nops > 0) {
            int done = 0;
            rc = apply_parallel(bdb_state, tran, &all, &done, &failed);
            if (!done) {
                for (struct plan_op *op = all.first; op && rc == 0; op = op->next) {
                    rc = apply_op(op, tran);
                    if (rc != 0)
                        failed = op;
                }
            }
        }
        uint64_t applied = now_us();

        if (rc_is_stale(rc) && failed) {
            /* A unique key one member adds is in the index, or a row one
             * member changes is gone: another transaction did that after the
             * member read, or an earlier member of this unit did.  That
             * member goes back to run again; the rest are applied again
             * without it. */
            struct unit_member *whose = NULL;
            for (struct unit_member *m = batch; m && !whose; m = m->next) {
                if (m->stale || m->dup)
                    continue;
                for (struct plan_op *op = m->plan->first;; op = op->next) {
                    if (op == failed) {
                        whose = m;
                        break;
                    }
                    if (op == m->plan->last)
                        break;
                }
            }
            if (whose) {
                bdb_tran_abort(bdb_state, tran, &bdberr);
                unit_drop_replays(batch);
                whose->stale = 1;
                count_stale(rc);
                continue;
            }
        }

        if (rc != 0) {
            bdb_tran_abort(bdb_state, tran, &bdberr);
            unit_drop_replays(batch);
            if ((rc == DB_LOCK_DEADLOCK || rc == BDBERR_DEADLOCK || rc == DB_REP_HANDLE_DEAD) &&
                ++retries < UNIT_MAX_RETRIES) {
                ATOMIC_ADD64(gbl_ordered_apply_unit_retries, 1);
                continue;
            }
            if (rc != DB_LOCK_DEADLOCK && rc != BDBERR_DEADLOCK && rc != DB_REP_HANDLE_DEAD)
                logmsg(LOGMSG_ERROR, "%s: applying a unit failed rc %d\n", __func__, rc);
            unit_fail(batch, (rc == DB_LOCK_DEADLOCK || rc == BDBERR_DEADLOCK || rc == DB_REP_HANDLE_DEAD)
                                 ? BDBERR_DEADLOCK
                                 : BDBERR_MISC);
            return;
        }

        seqnum_type seqnum;
        uint64_t size = 0;
        memset(&seqnum, 0, sizeof(seqnum));
        rc = bdb_tran_commit_with_seqnum_size(bdb_state, tran, &seqnum, &size, &bdberr);
        if (rc != 0) {
            logmsg(LOGMSG_ERROR, "%s: committing a unit failed rc %d bdberr %d\n", __func__, rc, bdberr);
            unit_drop_replays(batch);
            unit_fail(batch, bdberr ? bdberr : BDBERR_MISC);
            return;
        }

        uint64_t end = now_us();
        for (struct unit_member *m = batch; m; m = m->next) {
            if (m->stale) {
                m->rc = -1;
                m->bdberr = BDBERR_ADD_DUPE;
                continue;
            }
            m->rc = m->dup ? IX_DUP : 0;
            memcpy(&m->lsn, &seqnum, sizeof(DB_LSN));
        }
        ATOMIC_ADD64(gbl_ordered_apply_units, 1);
        ATOMIC_ADD64(gbl_ordered_apply_txns, nplans);
        ATOMIC_ADD64(gbl_ordered_apply_ops, all.nops);
        ATOMIC_ADD64(gbl_ordered_apply_apply_us, applied - start);
        ATOMIC_ADD64(gbl_ordered_apply_held_us, end - start);
        return;
    }
}

/* A transaction applying its own plan: its blkseq record and its operations
 * go under its own transaction, which then commits them itself.  Returns
 * what bdb_tran_plan_unit_commit() does. */
static int unit_apply_own(bdb_state_type *bdb_state, tran_type *tran, struct unit_member *m, int *bdberr)
{
    struct bdb_plan *plan = m->plan;
    uint64_t start = now_us();
    int rc = 0;

    if (m->bskey) {
        rc = bdb_blkseq_insert(bdb_state, tran, m->bskey, m->bskeylen, m->bsdata, m->bsdatalen, m->replay_data,
                               m->replay_len, 0);
        if (rc == IX_DUP)
            return IX_DUP;
        if (rc != 0) {
            *bdberr = BDBERR_MISC;
            return -1;
        }
    }
    int done = 0;
    struct plan_op *failed = NULL;
    tran->plan_verify = 1;
    rc = apply_parallel(bdb_state, tran, plan, &done, &failed);
    if (!done) {
        for (struct plan_op *op = plan->first; op && rc == 0; op = op->next)
            rc = apply_op(op, tran);
    }
    uint64_t end = now_us();
    ATOMIC_ADD64(gbl_ordered_apply_apply_us, end - start);
    ATOMIC_ADD64(gbl_ordered_apply_held_us, end - start);
    if (rc == 0) {
        ATOMIC_ADD64(gbl_ordered_apply_units, 1);
        ATOMIC_ADD64(gbl_ordered_apply_units_inline, 1);
        ATOMIC_ADD64(gbl_ordered_apply_txns, 1);
        ATOMIC_ADD64(gbl_ordered_apply_ops, plan->nops);
        return 0;
    }
    if (rc_is_stale(rc)) {
        /* a unique key this plan adds is in the index, or a row it changes
         * is gone: run without a plan, and let that report it */
        count_stale(rc);
        *bdberr = BDBERR_ADD_DUPE;
    } else if (rc == DB_LOCK_DEADLOCK || rc == BDBERR_DEADLOCK || rc == DB_REP_HANDLE_DEAD) {
        *bdberr = BDBERR_DEADLOCK;
    } else {
        logmsg(LOGMSG_ERROR, "%s: applying a plan failed rc %d\n", __func__, rc);
        *bdberr = BDBERR_MISC;
    }
    return -1;
}

static void *unit_thread(void *arg)
{
    bdb_state_type *bdb_state = arg;
    thread_started("ordered apply unit");
    bdb_thread_event(bdb_state, BDBTHR_EVENT_START);
    Pthread_mutex_lock(&unit_lk);
    for (;;) {
        if (unit_first == NULL || unit_busy) {
            Pthread_cond_wait(&unit_work, &unit_lk);
            continue;
        }
        unit_busy = 1;
        /* take members until the unit is large enough; always at least one */
        struct unit_member *batch = unit_first;
        struct unit_member *tail = batch;
        int nops = batch->plan->nops;
        while (tail->next && nops < gbl_ordered_apply_unit_max_ops) {
            tail = tail->next;
            nops += tail->plan->nops;
        }
        unit_first = tail->next;
        if (unit_first == NULL)
            unit_last = NULL;
        tail->next = NULL;
        Pthread_mutex_unlock(&unit_lk);

        unit_run(bdb_state, batch);

        Pthread_mutex_lock(&unit_lk);
        unit_busy = 0;
        while (batch) {
            /* a member is on its thread's stack, gone once that thread runs */
            struct unit_member *next = batch->next;
            batch->done = 1;
            Pthread_cond_signal(&batch->cond);
            batch = next;
        }
    }
    return NULL;
}

int bdb_tran_plan_pending(tran_type *tran)
{
    return tran && tran->unit_plan != NULL;
}

int bdb_tran_plan_unit_commit(bdb_state_type *bdb_state, tran_type *tran, void *bskey, int bskeylen, void *bsdata,
                              int bsdatalen, void **replay_data, int *replay_len, int *bdberr)
{
    struct unit_member m = {0};

    *bdberr = BDBERR_NOERROR;
    if (tran == NULL || tran->unit_plan == NULL)
        return 0;
    if (bdb_state->parent)
        bdb_state = bdb_state->parent;

    m.plan = tran->unit_plan;
    m.bskey = bskey;
    m.bskeylen = bskeylen;
    m.bsdata = bsdata;
    m.bsdatalen = bsdatalen;
    m.replay_data = replay_data;
    m.replay_len = replay_len;
    Pthread_cond_init(&m.cond, NULL);

    uint64_t start = now_us();
    Pthread_mutex_lock(&unit_lk);
    if (gbl_ordered_apply_unit_inline && !unit_busy && unit_first == NULL && ATOMIC_LOAD64(plans_alive) <= 1) {
        /* no other plan anywhere: no need for the unit thread.  Beginning and committing the unit's transaction here
         * replaces this thread's note of its own top-level transaction. */
        unit_busy = 1;
        Pthread_mutex_unlock(&unit_lk);
        int rc = unit_apply_own(bdb_state, tran, &m, bdberr);
        Pthread_mutex_lock(&unit_lk);
        unit_busy = 0;
        if (unit_first)
            Pthread_cond_signal(&unit_work);
        Pthread_mutex_unlock(&unit_lk);
        Pthread_cond_destroy(&m.cond);
        plan_free(tran->unit_plan);
        tran->unit_plan = NULL;
        return rc;
    }
    if (!unit_thread_started) {
        pthread_t tid;
        pthread_attr_t attr;
        Pthread_attr_init(&attr);
        Pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        Pthread_attr_setstacksize(&attr, 1024 * 1024);
        int rc = pthread_create(&tid, &attr, unit_thread, bdb_state);
        Pthread_attr_destroy(&attr);
        if (rc != 0) {
            Pthread_mutex_unlock(&unit_lk);
            Pthread_cond_destroy(&m.cond);
            *bdberr = BDBERR_MISC;
            return -1;
        }
        unit_thread_started = 1;
    }
    if (unit_last)
        unit_last->next = &m;
    else
        unit_first = &m;
    unit_last = &m;
    Pthread_cond_signal(&unit_work);

    int slow = 0;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    int ms = gbl_ordered_apply_unit_detect_ms > 0 ? gbl_ordered_apply_unit_detect_ms : 0;
    until.tv_sec += ms / 1000;
    until.tv_nsec += (long)(ms % 1000) * 1000000;
    if (until.tv_nsec >= 1000000000) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000;
    }
    while (!m.done) {
        if (!slow) {
            if (pthread_cond_timedwait(&m.cond, &unit_lk, &until) != 0 && !m.done) {
                slow = 1;
                ATOMIC_ADD64(gbl_ordered_apply_unit_slow_waits, 1);
            }
            continue;
        }
        /* Wait where the deadlock detector can see it: every unit holds this
         * name in write mode for as long as it is being applied. */
        Pthread_mutex_unlock(&unit_lk);
        DBT name = {0};
        DB_LOCK lk;
        name.data = unit_name;
        name.size = sizeof(unit_name);
        int lrc = berkdb_lock(bdb_state->dbenv, tran->tid->txnid, 0, &name, DB_LOCK_READ, &lk);
        if (lrc == 0) {
            bdb_release_lock(bdb_state, &lk);
        } else if (lrc == BDBERR_DEADLOCK) {
            /* The unit this plan is in, or one ahead of it, waits for a
             * transaction that waits for a lock of ours. */
            bdb_abort_waiters(bdb_state, tran);
            ATOMIC_ADD64(gbl_ordered_apply_unit_kicks, 1);
        }
        Pthread_mutex_lock(&unit_lk);
        if (!m.done) {
            /* between units nothing holds the name */
            struct timespec nap;
            clock_gettime(CLOCK_REALTIME, &nap);
            nap.tv_nsec += 2000000;
            if (nap.tv_nsec >= 1000000000) {
                nap.tv_sec++;
                nap.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&m.cond, &unit_lk, &nap);
        }
    }
    Pthread_mutex_unlock(&unit_lk);
    Pthread_cond_destroy(&m.cond);

    ATOMIC_ADD64(gbl_ordered_apply_waits, 1);
    ATOMIC_ADD64(gbl_ordered_apply_wait_us, now_us() - start);

    /* applied or not, the plan is finished with */
    plan_free(tran->unit_plan);
    tran->unit_plan = NULL;

    if (m.rc == -1) {
        *bdberr = m.bdberr;
        return -1;
    }
    tran->unit_lsn = m.lsn;
    return m.rc;
}
