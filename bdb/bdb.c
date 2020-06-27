/*
   Copyright 2015, 2017, Bloomberg Finance L.P.

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

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stddef.h>

#include <build/db.h>
#include <epochlib.h>

#include <ctrace.h>

#include <net.h>
#include "bdb_int.h"
#include "locks.h"

#include "nodemap.h"
#include "thread_stats.h"
#include "logmsg.h"
#include "locks_wrap.h"

extern void berkdb_dumptrans(DB_ENV *);
extern int __db_panic(DB_ENV *dbenv, int err);

pthread_key_t bdb_key;
pthread_key_t lock_key;

const char *readlockstr = "READLOCK";
const char *writelockstr = "WRITELOCK";

void print(bdb_env_type *bdb_state, char *format, ...)
{
    va_list ap;

    va_start(ap, format);

    if (bdb_state && bdb_state->callback && bdb_state->callback->print_rtn)
        (bdb_state->callback->print_rtn)(format, ap);
    else
        logmsgvf(LOGMSG_USER, stderr, format, ap);

    va_end(ap);
}

extern bdb_env_type *gbl_bdb_state;

void bdb_set_key(bdb_env_type *bdb_state)
{
    if (gbl_bdb_state)
        return;

    gbl_bdb_state = bdb_state;

    Pthread_setspecific(bdb_key, bdb_state);
}

void *mymalloc(size_t size)
{
    /*
    if (size > 100000)
       fprintf(stderr, "mymalloc: size = %d\n", (int)size);
    */

    return (malloc(size));
}

void myfree(void *ptr)
{
    /*char *cptr;*/

    /*fprintf(stderr, "mymalloc: size = %d\n", (int)size);*/
    /* TODO why did we do this? seems unsafe since we sometimes malloc 0 len */
    /*cptr = (char *)ptr;*/
    /*cptr[0] = 0;*/

    free(ptr);
}

void *myrealloc(void *ptr, size_t size)
{
    /*fprintf(stderr, "myrealloc: size = %d\n", (int)size);*/

    return (realloc(ptr, size));
}

/* retrieve the user pointer associated with a bdb_handle */
void *bdb_env_get_usr_ptr(bdb_env_type *bdb_state) { return bdb_state->usr_ptr; }
void *bdb_table_get_usr_ptr(bdb_table_type *bdb_state) { return bdb_state->usr_ptr; }

char *bdb_strerror(int error)
{
    switch (error) {
    case DB_ODH_CORRUPT:
        return "Ondisk header corrupt";
    default:
        return db_strerror(error);
    }
}

extern long long time_micros(void);

int bdb_amimaster(bdb_env_type *bdb_state)
{
    /*
    ** if (bdb_state->repinfo->master_eid ==
    *net_get_mynode(bdb_state->repinfo->netinfo))
    **    return 1;
    ** else
    **    return 0;
    */

    repinfo_type *repinfo = bdb_state->repinfo;
    return repinfo->master_host == repinfo->myhost;
}

char *bdb_whoismaster(bdb_env_type *bdb_state)
{
    if (bdb_state->repinfo->master_host != db_eid_invalid)
        return bdb_state->repinfo->master_host;
    else
        return NULL;
}

int bdb_iam_master(bdb_env__type *bdb_state)
{
    char *master;
    bdb_state->dbenv->get_rep_master(bdb_state->dbenv, &master, NULL, NULL);
    return (master == bdb_state->repinfo->myhost);
}

int bdb_get_rep_master(bdb_env_type *bdb_state, char **master_out,
                       uint32_t *gen, uint32_t *egen)
{
    return bdb_state->dbenv->get_rep_master(bdb_state->dbenv, master_out, gen,
                                            egen);
}

int bdb_get_sanc_list(bdb_env_type *bdb_state, int max_nodes,
                      const char *nodes[REPMAX])
{

    return net_get_sanctioned_node_list(bdb_state->repinfo->netinfo, max_nodes,
                                        nodes);
}

int bdb_seqnum_compare(bdb_env_type *bdb_state, seqnum_type *seqnum1,
                       seqnum_type *seqnum2)
{
    if (bdb_state->attr->enable_seqnum_generations &&
        seqnum1->generation != seqnum2->generation)
        return (seqnum1->generation < seqnum2->generation ? -1 : 1);
    return log_compare(&(seqnum1->lsn), &(seqnum2->lsn));
}

char *bdb_format_seqnum(const seqnum_type *seqnum, char *buf, size_t bufsize)
{
    const DB_LSN *lsn = (const DB_LSN *)seqnum;
    snprintf(buf, bufsize, "%u:%u", (unsigned)lsn->file, (unsigned)lsn->offset);
    return buf;
}

int bdb_get_seqnum(bdb_env_type *bdb_state, seqnum_type *seqnum)
{
    DB_LSN our_lsn;
    DB_LOG_STAT *log_stats;
    int outrc;

    BDB_READLOCK("bdb_get_seqnum");

    /* XXX this continues to be retarted.  there has to be a lighter weight
       way to get the lsn */
    bzero(seqnum, sizeof(seqnum_type));
    bdb_state->dbenv->log_stat(bdb_state->dbenv, &log_stats, 0);
    if (log_stats) {
        make_lsn(&our_lsn, log_stats->st_cur_file, log_stats->st_cur_offset);
        free(log_stats);
        memcpy(seqnum, &our_lsn, sizeof(DB_LSN));
        outrc = 0;
    } else {
        outrc = -1;
    }
    BDB_RELLOCK();
    return outrc;
}

int bdb_get_lsn(bdb_env_type *bdb_state, int *logfile, int *offset)
{
    DB_LSN outlsn;
    __log_txn_lsn(bdb_state->dbenv, &outlsn, NULL, NULL);
    *logfile = outlsn.file;
    *offset = outlsn.offset;
    return 0;
}

int bdb_get_lsn_node(bdb_env_type *bdb_state, char *host, int *logfile,
                     int *offset)
{
    *logfile = bdb_state->seqnum_info->seqnums[nodeix(host)].lsn.file;
    *offset = bdb_state->seqnum_info->seqnums[nodeix(host)].lsn.offset;
    return 0;
}

void bdb_make_seqnum(seqnum_type *seqnum, uint32_t logfile, uint32_t logbyte)
{
    DB_LSN lsn;
    lsn.file = logfile;
    lsn.offset = logbyte;
    bzero(seqnum, sizeof(seqnum_type));
    memcpy(seqnum, &lsn, sizeof(DB_LSN));
}

void bdb_get_txn_stats(bdb_env_type *bdb_state, int64_t *active,
                       int64_t *maxactive, int64_t *commits, int64_t *aborts)
{
    DB_TXN_STAT *txn_stats;

    BDB_READLOCK("bdb_get_txn_stats");

    bdb_state->dbenv->txn_stat(bdb_state->dbenv, &txn_stats, 0);
    if (active)
        *active = txn_stats->st_nactive;
    if (maxactive)
        *maxactive = txn_stats->st_maxnactive;
    if (commits)
        *commits = txn_stats->st_ncommits;
    if (aborts)
        *aborts = txn_stats->st_naborts;

    BDB_RELLOCK();

    free(txn_stats);
}

void bdb_get_cache_stats(bdb_env_type *bdb_state, uint64_t *hits,
                         uint64_t *misses, uint64_t *reads, uint64_t *writes,
                         uint64_t *thits, uint64_t *tmisses)
{
    DB_MPOOL_STAT *mpool_stats;

    BDB_READLOCK("bdb_get_cache_stats");
    bdb_state->dbenv->memp_stat(bdb_state->dbenv, &mpool_stats, NULL,
                                DB_STAT_MINIMAL);

    /* We find leaf pages only a more useful metric. */
    if (hits)
        *hits = mpool_stats->st_cache_lhit;
    if (misses)
        *misses = mpool_stats->st_cache_lmiss;
    if (reads)
        *reads = mpool_stats->st_page_in;
    if (writes)
        *writes = mpool_stats->st_page_out;

    free(mpool_stats);

    bdb_temp_table_stat(bdb_state, &mpool_stats);
    if (thits)
        *thits = mpool_stats->st_cache_hit;
    if (tmisses)
        *tmisses = mpool_stats->st_cache_miss;

    BDB_RELLOCK();

    free(mpool_stats);
}

void add_dummy(bdb_env_type *bdb_state)
{
    if (bdb_state->exiting)
        return;

    int rc = bdb_add_dummy_llmeta();
    if (rc) {
        logmsg(LOGMSG_ERROR, "%s %s bdb_add_dummy_llmeta rc: %d\n", __FILE__,
                __func__, rc);
    }
    bdb_state->repinfo->repstats.dummy_adds++;
}

bdbtype_t bdb_get_type(bdb_table_type *bdb_state)
{
    if (bdb_state)
        return bdb_state->bdbtype;
    else
        return BDBTYPE_NONE;
}

int bdb_get_qdb_adds(bdb_table_type *bdb_state)
{
    // TODO: assert is queue?
    if (bdb_state)
        return bdb_state->qdb_adds;
    else
        return 0;
}

int bdb_get_qdb_cons(bdb_table_type *bdb_state)
{
    // TODO: assert is queue?
    if (bdb_state)
        return bdb_state->qdb_cons;
    else
        return 0;
}

int bdb_set_timeout(bdb_env_type *bdb_state, unsigned int timeout,
                    int *bdberr)
{
    int rc;
    unsigned int tm;

    *bdberr = 0;
    bdb_state->dbenv->get_timeout(bdb_state->dbenv, &tm, DB_SET_LOCK_TIMEOUT);
    logmsg(LOGMSG_DEBUG, "MP: %u\n", tm);

    rc = bdb_state->dbenv->set_timeout(bdb_state->dbenv, timeout,
                                       DB_SET_LOCK_TIMEOUT);
    if (rc) {
        *bdberr = rc;
        return -1;
    }

    return rc;
}

void bdb_tran_set_usrptr(tran_type *tran, void *usr) { tran->usrptr = usr; }

void *bdb_tran_get_usrptr(tran_type *tran) { return tran->usrptr; }

void bdb_start_request(bdb_env_type *bdb_state)
{
    BDB_READLOCK("bdb_start_request");
}

void bdb_end_request(bdb_env_type *bdb_state)
{
    BDB_RELLOCK();
}

/* Call this at the beginning of a request to reset all our per thread stats. */
void bdb_reset_thread_stats(void)
{
#ifdef BERKDB_4_2
    bb_berkdb_thread_stats_reset();
#endif
}

/* Call this at the end of a request to get our stats. */
const struct berkdb_thread_stats *bdb_get_thread_stats(void)
{
#ifdef BERKDB_4_2
    return (const struct berkdb_thread_stats *)bb_berkdb_get_thread_stats();
#else
    static struct berkdb_thread_stats zero = {0};
    return &zero;
#endif
}

/* Call this any time to get process wide stats (which get updated locklessly)
 */
const struct berkdb_thread_stats *bdb_get_process_stats(void)
{
#ifdef BERKDB_4_2
    return (const struct berkdb_thread_stats *)bb_berkdb_get_process_stats();
#else
    static struct berkdb_thread_stats zero = {0};
    return &zero;
#endif
}

/* Report bdb stats into the given logging function. */
void bdb_print_stats(const struct berkdb_thread_stats *st, const char *prefix,
                     int (*printfn)(const char *, void *), void *context)
{
    char s[128];
    if (st->n_lock_waits > 0) {
        snprintf(s, sizeof(s), "%s%u lock waits took %u ms (%u ms/wait)\n",
                 prefix, st->n_lock_waits, U2M(st->lock_wait_time_us),
                 U2M(st->lock_wait_time_us / st->n_lock_waits));
        printfn(s, context);
    }
    if (st->n_preads > 0) {
        snprintf(s, sizeof(s), "%s%u preads took %u ms total of %u bytes\n",
                 prefix, st->n_preads, U2M(st->pread_time_us), st->pread_bytes);
        printfn(s, context);
    }
    if (st->n_pwrites > 0) {
        snprintf(s, sizeof(s), "%s%u pwrites took %u ms total of %u bytes\n",
                 prefix, st->n_pwrites, U2M(st->pwrite_time_us),
                 st->pwrite_bytes);
        printfn(s, context);
    }
    if (st->n_memp_fgets > 0) {
        snprintf(s, sizeof(s), "%s%u __memp_fget calls took %u ms\n", prefix,
                 st->n_memp_fgets, U2M(st->memp_fget_time_us));
        printfn(s, context);
    }
    if (st->n_memp_pgs > 0) {
        snprintf(s, sizeof(s), "%s%u __memp_pg calls took %u ms\n", prefix,
                 st->n_memp_pgs, U2M(st->memp_pg_time_us));
        printfn(s, context);
    }
    if (st->n_shallocs > 0 || st->n_shalloc_frees > 0) {
        snprintf(s, sizeof(s),
                 "%s%u shallocs took %u ms, %u shalloc_frees took %u ms\n",
                 prefix, st->n_shallocs, U2M(st->shalloc_time_us),
                 st->n_shalloc_frees, U2M(st->shalloc_free_time_us));
        printfn(s, context);
    }
}

void bdb_fprintf_stats(const struct berkdb_thread_stats *st, const char *prefix,
                       FILE *out)
{
    bdb_print_stats(st, "  ", (int (*)(const char *, void *))fputs, out);
}

void bdb_register_rtoff_callback(bdb_env_type *bdb_state, void (*func)(void))
{
    bdb_state->signal_rtoff = func;
}

bdb_table_type *bdb_get_table_by_name(bdb_env_type *bdb_state, char *table)
{
    int i;

    for (i = 0; i < bdb_state->numchildren; i++) {
        bdb_table_type *child;
        child = bdb_state->children[i];
        if (child) {
            if (strcmp(child->name, table) == 0)
                return child;
        }
    }
    return NULL;
}

bdb_table_type *bdb_get_table_by_name_dbnum(bdb_envtype *bdb_state,
                                            char *table, int *dbnum)
{
    int i;

    for (i = 0; i < bdb_state->numchildren; i++) {
        bdb_table_type *child;
        child = parent->children[i];
        if (child) {
            if (strcmp(child->name, table) == 0) {
                *dbnum = i;
                return child;
            }
        }
    }
    return NULL;
}

uint32_t bdb_readonly_lock_id(bdb_env_type *bdb_state)
{
    uint32_t lid = 0;
    DB_ENV *dbenv = bdb_state->dbenv;
    dbenv->lock_id_flags(dbenv, &lid, DB_LOCK_ID_READONLY);
    return lid;
}

void bdb_free_lock_id(bdb_env_type *bdb_state, uint32_t lid)
{
    DB_ENV *dbenv = bdb_state->dbenv;
    DB_LOCKREQ rq = {0};
    rq.op = DB_LOCK_PUT_ALL;
    dbenv->lock_vec(dbenv, lid, 0, &rq, 1, NULL);
    dbenv->lock_id_free(dbenv, lid);
}

void bdb_lockspeed(bdb_env_type *bdb_state)
{
    u_int32_t lid;
    DB_ENV *dbenv = bdb_state->dbenv;
    int i;
    DB_LOCK lock;
    DBT lkname = {0};
    int start, end;

    dbenv->lock_id(dbenv, &lid);
    lkname.data = "hello";
    lkname.size = strlen("hello");
    start = comdb2_time_epochms();
    for (i = 0; i < 100000000; i++) {
        dbenv->lock_get(dbenv, lid, 0, &lkname, DB_LOCK_READ, &lock);
        dbenv->lock_put(dbenv, &lock);
    }
    end = comdb2_time_epochms();
    logmsg(LOGMSG_USER, "berkeley took %dms (%d per second)\n", end - start,
           100000000 / (end - start) * 1000);
    dbenv->lock_id_free(dbenv, lid);
}

void bdb_dumptrans(bdb_env_type *bdb_state)
{
    berkdb_dumptrans(bdb_state->dbenv);
}

void bdb_berkdb_iomap_set(bdb_env_type *bdb_state, int onoff)
{
    bdb_state->dbenv->setattr(bdb_state->dbenv, "iomap", NULL, onoff);
}

int bdb_berkdb_get_attr(bdb_env_type *bdb_state, char *attr, char **value,
                        int *ivalue)
{
    int rc;
    rc = bdb_state->dbenv->getattr(bdb_state->dbenv, attr, value, ivalue);
    return rc;
}

int bdb_berkdb_set_attr(bdb_env_type *bdb_state, char *attr, char *value,
                        int ivalue)
{
    int rc;

    rc = bdb_state->dbenv->setattr(bdb_state->dbenv, attr, value, ivalue);
    return rc;
}

int bdb_berkdb_set_attr_after_open(bdb_attr_type *bdb_attr, char *attr,
                                   char *value, int ivalue)
{
    struct deferred_berkdb_option *opt;
    opt = malloc(sizeof(struct deferred_berkdb_option));
    opt->attr = strdup(attr);
    opt->value = strdup(value);
    opt->ivalue = ivalue;
    listc_abl(&bdb_attr->deferred_berkdb_options, opt);

    return 0;
}

void bdb_berkdb_dump_attrs(bdb_env_type *bdb_state, FILE *f)
{
    bdb_state->dbenv->dumpattrs(bdb_state->dbenv, f);
}

int bdb_berkdb_blobmem_yield(bdb_env_type *bdb_state)
{
    return bdb_state->dbenv->blobmem_yield(bdb_state->dbenv);
}

int bdb_recovery_start_lsn(bdb_env_type *bdb_state, char *lsnout, int lsnlen)
{
    DB_LSN lsn;
    int rc;
    rc = bdb_state->dbenv->get_recovery_lsn(bdb_state->dbenv, &lsn);
    if (rc)
        return rc;
    snprintf(lsnout, lsnlen, "%u:%u", lsn.file, lsn.offset);
    return 0;
}

int bdb_panic(bdb_env_type *bdb_state)
{
    __db_panic(bdb_state->dbenv, EINVAL);
    /* this shouldn't return!!! */
    return 0;
}
