/*
   Copyright 2015 Bloomberg Finance L.P.

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

#include <poll.h>
#include <list.h>
#include "comdb2.h"
#include "sql.h"
#include "osql_srs.h"
#include "sqloffload.h"
#include "comdb2uuid.h"

#include "debug_switches.h"

extern int gbl_osql_verify_retries_max;

typedef struct srs_tran_query {
    int iscommit;
    void *stmt;
    LINKC_T(struct srs_tran_query) lnk; /* next query */
} srs_tran_query_t;

static void *save_stmt(struct sqlclntstate *clnt)
{
    return clnt->plugin.save_stmt(clnt, NULL);
}

static void restore_stmt(struct sqlclntstate *clnt, srs_tran_query_t *item)
{
    clnt->plugin.restore_stmt(clnt, item->stmt); /* newsql_restore_stmt */
}

static void destroy_stmt(struct sqlclntstate *clnt, srs_tran_query_t *item)
{
    clnt->plugin.destroy_stmt(clnt, item->stmt); /* newsql_destroy_stmt_evbuffer */
}

static char *print_stmt(struct sqlclntstate *clnt, srs_tran_query_t *item)
{
    return clnt->plugin.print_stmt(clnt, item->stmt);
}

typedef struct srs_tran {
    LISTC_T(struct srs_tran_query) lst; /* list of queries up to this point */
} srs_tran_t;

/**
 * Free a statement entry.
 */
static void srs_free_tran_entry(struct sqlclntstate *clnt,
                                srs_tran_query_t *item)
{
    if (item == NULL)
        return;
    destroy_stmt(clnt, item);
    free(item);
}

void srs_tran_print_history(struct sqlclntstate *clnt, int indent)
{
    srs_tran_query_t *item = NULL;
    srs_tran_t *history = clnt->osql.history;
    if (history == NULL)
        return;
    int num = 0;
    LISTC_FOR_EACH(&history->lst, item, lnk) {
        logmsg(LOGMSG_WARN, "%*c %3d) %s\n", indent, ' ', num++, print_stmt(clnt, item));
    }
}

/**
 * Create a history of sql for this transaction
 * that will allow replay in the case of verify errors
 */
int srs_tran_create(struct sqlclntstate *clnt)
{
    osqlstate_t *osql = &clnt->osql;
    int rc = 0;

    if (osql->history) {
        logmsg(LOGMSG_ERROR, "%s: the history should be empty!\n", __func__);
        cheap_stack_trace();
        rc = srs_tran_destroy(clnt);
        if (rc)
            return rc;
    }

    osql->history = (srs_tran_t *)calloc(1, sizeof(srs_tran_t));
    if (!osql->history) {
        logmsg(LOGMSG_ERROR, "malloc %zu\n", sizeof(srs_tran_t));
        return -1;
    }
#if 0
   printf ("%d Creating history %p\n", pthread_self(), osql->history);
#endif

    listc_init(&osql->history->lst, offsetof(struct srs_tran_query, lnk));

    return 0;
}

/* set replay status
 * at the end of it all, we may replay the entire list
 * of statements in a transaction from srs_tran_replay,
 * depending on the replay status
 */
void osql_set_replay(const char *file, int line, struct sqlclntstate *clnt,
                     int replay)
{
    osqlstate_t *osql = &clnt->osql;

    osql->replay_file = (char *)file;
    osql->replay_line = line;
    osql->last_replay = osql->replay;
    /*
    logmsg(LOGMSG_ERROR, "Replay set from %s:%d clnt %p from %d to %d\n", file,
    line, clnt, osql->replay, replay);
            */
    osql->replay = replay;
}

/**
 * Destroy the sql transaction history
 *
 */
int srs_tran_destroy(struct sqlclntstate *clnt)
{
    osqlstate_t *osql = &clnt->osql;

#if 0
   printf ("%d Destroying history %p \n", pthread_self(), osql->history);
#endif
    if (clnt->dist_txnid) {
        free(clnt->dist_txnid);
        clnt->dist_txnid = NULL;
        clnt->dist_timestamp = 0;
    }

    if (!osql->history)
        goto done;

    srs_tran_empty(clnt);
    free(osql->history);
    osql->history = NULL;

done:
    if (osql->replay != OSQL_RETRY_NONE) {
        logmsg(LOGMSG_ERROR,
               "%s: cleaned history but state is wrong %d, fixing\n", __func__,
               osql->replay);
        cheap_stack_trace();
        osql_set_replay(__FILE__, __LINE__, clnt, OSQL_RETRY_NONE);
    }

    return 0;
}

int srs_tran_del_last_query(struct sqlclntstate *clnt)
{
    if (clnt->osql.history == NULL)
        return 0;
    srs_tran_query_t *item = NULL;
    item = listc_rbl(&clnt->osql.history->lst);
    srs_free_tran_entry(clnt, item);
    return 0;
}

static inline int srs_tran_do_not_retry(struct sqlclntstate *clnt)
{
    return (clnt->verifyretry_off || clnt->isselect || clnt->dbtran.trans_has_sp || clnt->has_recording);
}

/**
 * Add a new query to the transaction
 *
 */
int srs_tran_add_query(struct sqlclntstate *clnt)
{
    osqlstate_t *osql = &clnt->osql;
    srs_tran_query_t *item = NULL;

    if (srs_tran_do_not_retry(clnt))
        return 0;

    /* don't grow session when the transaction is simply repeated */
    if (osql->replay != OSQL_RETRY_NONE) {
        if (!osql->history) {
            logmsg(LOGMSG_ERROR, "%s: state is %d but no history???\n",
                   __func__, osql->replay);
            cheap_stack_trace();

            return -1;
        }
        return 0;
    }

    if (!osql->history) {
        if (srs_tran_create(clnt))
            return -1;
    }

    item = malloc(sizeof(srs_tran_query_t));
    item->stmt = save_stmt(clnt);
    item->iscommit = strcasecmp("commit", clnt->sql) == 0;
    listc_abl(&osql->history->lst, item);
    clnt->added_to_hist = 1;

    /* if previous item is a 'commit', abort for debugging */
    item = item->lnk.prev;
    if (item && item->iscommit) {
        abort();
    }

    return 0;
}

/**
 * Empty the context of the transaction
 *
 */
int srs_tran_empty(struct sqlclntstate *clnt)
{
    osqlstate_t *osql = &clnt->osql;
    srs_tran_query_t *item = NULL, *tmp = NULL;

    if (!srs_tran_do_not_retry(clnt)) {
        Pthread_mutex_lock(&clnt->sql_lk);
        /* clnt->sql points into a buffer in clnt->appdata->query.
         * ensure that it's set to NULL before we free the memory */
        clnt->sql = NULL;
        Pthread_mutex_unlock(&clnt->sql_lk);
    }

    LISTC_FOR_EACH_SAFE(&osql->history->lst, item, tmp, lnk)
    {
        listc_rfl(&osql->history->lst, item);
        srs_free_tran_entry(clnt, item);
    }
    return 0;
}

long long gbl_verify_tran_replays = 0;
int gbl_disttxn_random_retry_poll = 500;

/**
 * Replay transaction using the current history
 *
 */
static int srs_tran_replay_int(struct sqlclntstate *clnt, int(dispatch_fn)(struct sqlclntstate *))
{
    osqlstate_t *osql = &clnt->osql;
    srs_tran_query_t *item = 0;
    int rc = 0;
    int nq = 0;
    int tnq = 0;

    clnt->verify_retries = 0;

    if (!osql->history) {
        logmsg(LOGMSG_ERROR, "Trying to replay, but no history?\n");
        cheap_stack_trace();
        return -1;
    }

    do {
        reset_query_effects(clnt); /* Reset it for each retry*/
        if (!osql->history) {
            logmsg(LOGMSG_ERROR, "Trying to replay, but no history?\n");
            abort();
        }

        clnt->verify_retries++;
        gbl_verify_tran_replays++;
        if (clnt->dist_timestamp > 0) {
            int pval = gbl_disttxn_random_retry_poll;
            if (pval > 1) {
                poll(0, 0, rand() % pval);
            }
        }

        /* Replays for SERIAL or SNAPISOL will never have select or selectv */
        if (clnt->dbtran.mode == TRANLEVEL_RECOM /* not for modsnap */) {
            /* we need to free all the shadows but selectv table (recgenid) */
            rc = osql_shadtbl_reset_for_selectv(clnt);
            if (rc) {
                logmsg(LOGMSG_ERROR,
                       "Failed to reset selectv in read committed\n");
                abort();
                cheap_stack_trace();
                return -1;
            }
        } else {
            osql_shadtbl_close(clnt); 
        }

        if (clnt->verify_retries == gbl_osql_verify_retries_max + 1) {
            osql_set_replay(__FILE__, __LINE__, clnt, OSQL_RETRY_LAST);
        }

        if (0 /*!bdb_am_i_coherent(thedb->bdb_env)*/) {
            logmsg(LOGMSG_ERROR,
                   "Cannot replay, I am incoherent id=%d retries=%d\n",
                   clnt->queryid, clnt->verify_retries);
            rc = CDB2ERR_VERIFY_ERROR;
            break;
        }
        nq = 0;
        clnt->start_gen = bdb_get_rep_gen(thedb->bdb_env);
        LISTC_FOR_EACH(&osql->history->lst, item, lnk)
        {
            restore_stmt(clnt, item);
            if ((rc = dispatch_fn(clnt)) != 0)
                break;
            if (!osql->history)
                break;
            nq++;
        }
        if (rc == 0)
            tnq = nq;

        /* don't repeat if we fail with unexplicable error, i.e. not a logical
         * error */
        if (rc < 0) {
            if (osql->replay != OSQL_RETRY_NONE) {
                logmsg(LOGMSG_ERROR,
                       "%p Replaying failed abnormally, calling abort, nq=%d tnq=%d\n",
                       clnt, nq, tnq);
                if (debug_switch_osql_verbose_history_replay()) {
                    if (osql->history) {
                        LISTC_FOR_EACH(&osql->history->lst, item, lnk)
                        {
                            logmsg(LOGMSG_DEBUG, "\"%s\"\n", print_stmt(clnt, item));
                        }
                    }
                }

                int type = tran2req(clnt->dbtran.mode);
                osql_sock_abort(clnt, type);
            }
            break;
        }
    } while (osql->replay == OSQL_RETRY_DO && clnt->verify_retries <= gbl_osql_verify_retries_max);

    if (clnt->verify_retries >= gbl_osql_verify_retries_max && osql->xerr.errval) {
        uuidstr_t us;
        logmsg(LOGMSG_ERROR, "transaction %llx %s failed %d times with verify errors\n", osql->rqid,
               comdb2uuidstr(osql->uuid, us), clnt->verify_retries);
        /* Set to NONE to suppress the error from srs_tran_destroy(). */
        osql_set_replay(__FILE__, __LINE__, clnt, OSQL_RETRY_NONE);
    }

    /* replayed, free the session */
    if (srs_tran_destroy(clnt)) {
        logmsg(LOGMSG_ERROR, "%s Fail to destroy transaction replay session\n",
               __func__);
    }
    if (rc && clnt->verify_retries < gbl_osql_verify_retries_max) {
        logmsg(LOGMSG_ERROR, "Uncommittable transaction %d retried %d times,  "
               "rc=%d [global retr=%lld] nq=%d tnq=%d\n", clnt->queryid,
               clnt->verify_retries, rc, gbl_verify_tran_replays, nq, tnq);
    }

    osql_set_replay(__FILE__, __LINE__, clnt, OSQL_RETRY_NONE);

    return rc;
}

static int run_sql_query(struct sqlclntstate *clnt)
{
    sqlengine_work_appsock(clnt->thd, clnt);
    return 0;
}

int srs_tran_replay_inline(struct sqlclntstate *clnt)
{
    return srs_tran_replay_int(clnt, run_sql_query);
}
