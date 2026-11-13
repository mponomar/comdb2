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
 * This first cut plans only what writes no names: adds to data files, blob
 * files and indexes that allow duplicates.  Anything else refuses, which the
 * caller sees as a deadlock; it then runs the request again with page locks
 * held to commit, as before.
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
#include <time.h>
#include <build/db.h>

enum { PLAN_DTA_ADD = 1, PLAN_KEY_ADD = 2 };

/* one btree operation; the key bytes and then the data bytes follow it */
struct plan_op {
    struct plan_op *next;
    struct plan_op *gnext; /* next operation on the same file */
    bdb_state_type *bdb_state;
    DB *dbp;
    unsigned long long genid;
    int type;
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
    int turn_held;
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
    free(plan);
}

static struct plan_op *plan_new_op(struct bdb_plan *plan, int type, bdb_state_type *bdb_state, DBT *key, DBT *data)
{
    struct plan_op *op = plan_alloc(plan, sizeof(struct plan_op) + key->size + data->size);
    if (op == NULL)
        return NULL;
    memset(op, 0, sizeof(*op));
    op->type = type;
    op->bdb_state = bdb_state;
    op->keylen = key->size;
    op->datalen = data->size;
    uint8_t *buf = (uint8_t *)(op + 1);
    memcpy(buf, key->data, key->size);
    memcpy(buf + key->size, data->data, data->size);
    if (plan->last)
        plan->last->next = op;
    else
        plan->first = op;
    plan->last = op;
    plan->nops++;
    return op;
}

int bdb_tran_plan_start(tran_type *tran)
{
    if (tran == NULL || tran->tranclass != TRANCLASS_BERK || tran->plan)
        return -1;
    tran->plan = calloc(1, sizeof(struct bdb_plan));
    return tran->plan ? 0 : -1;
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

int bdb_tran_plan_unsupported(tran_type *tran)
{
    return tran && tran->plan && tran->plan->unsupported;
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
    return 0;
}

int bdb_plan_key_add(bdb_state_type *bdb_state, unsigned long long genid, tran_type *tran, int ixnum, DBT *dbt_key,
                     DBT *dbt_data)
{
    /* A unique key is a name: adding one needs a duplicate check and a stamp,
     * which this step does not have. */
    if (!bdb_state->ixdups[ixnum])
        return bdb_plan_refuse(tran);
    struct plan_op *op = plan_new_op(tran->plan, PLAN_KEY_ADD, bdb_state, dbt_key, dbt_data);
    if (op == NULL)
        return ENOMEM;
    op->genid = genid;
    op->ixnum = ixnum;
    return 0;
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
    if (op->type == PLAN_DTA_ADD)
        return ll_dta_add(op->bdb_state, op->genid, op->dbp, tran, op->dtafile, op->dtastripe, &key, &data, op->flags,
                          op->odhready);
    return ll_key_add(op->bdb_state, op->genid, tran, op->ixnum, &key, &data);
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
 */
enum { PLAN_MAX_FILES = 32 };

struct plan_file {
    DB *db;
    struct plan_op *first;
    struct plan_op *last;
    tran_type tran; /* stands in for the planning transaction in the ll_* routines */
    int rc;
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
    for (struct plan_op *op = f->first; op && rc == 0; op = op->gnext)
        rc = apply_op(op, &f->tran);
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
    return op->type == PLAN_DTA_ADD ? op->dbp : op->bdb_state->dbp_ix[op->ixnum];
}

/* Sets *done if it applied the plan, and returns the rc of that.  Otherwise
 * the plan is to be applied on this thread alone. */
static int apply_parallel(bdb_state_type *bdb_state, tran_type *tran, struct bdb_plan *plan, int *done)
{
    struct plan_file files[PLAN_MAX_FILES];
    int nfiles = 0;

    if (gbl_ordered_apply_threads <= 0 || plan->nops < gbl_ordered_apply_parallel_min_ops)
        return 0;

    for (struct plan_op *op = plan->first; op; op = op->next) {
        /* logical log records are chained through the top-level transaction */
        if (op->bdb_state->attr->llog || op->bdb_state->logical_live_sc)
            return 0;
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

        for (int i = 0; i < nfiles && rc == 0; i++)
            rc = files[i].rc;
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

    tran_type *top = tran;
    while (top->parent)
        top = top->parent;
    rc = turn_acquire(bdb_state, top);
    if (rc != 0) {
        *bdberr = (rc == BDBERR_DEADLOCK) ? BDBERR_DEADLOCK : BDBERR_MISC;
        return -1;
    }

    int done = 0;
    rc = apply_parallel(bdb_state, tran, plan, &done);
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
    if (rc == DB_LOCK_DEADLOCK || rc == DB_REP_HANDLE_DEAD) {
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
    if (tran->plan_turn_held) {
        /* the turn went with the rest of the transaction's locks */
        tran->plan_turn_held = 0;
        ATOMIC_ADD64(gbl_ordered_apply_held_us, now_us() - tran->plan_turn_start_us);
    }
}
