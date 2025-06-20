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

/****************************************************************************/
/* NOTE to all who may venture here:  DO NOT EVER USE NON TRANSACTIONAL FIND*/
/* calls inside of the block processor.  doing so will result in deadlocks. */
/* when genid is available, it is always preferable to do transactional     */
/* finds based on rrn+genid rather than a transactional find based on       */
/* "primary key" (index 0).  we dont enforce the existence of a primary key */
/* in a pure tagged database.                                               */
/*                                                                          */
/* also, all transactional ops can result in deadlock.  they must all be    */
/* handled "properly" - ie, tran aborts, makes it back up to sltdbt and gets*/
/* retried there.                                                           */
/****************************************************************************/

/* db block request */
#include <alloca.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/types.h>
#include <inttypes.h>

#include <epochlib.h>
#include <util.h>
#include <f2cstr.h>
#include <list.h>
#include <plhash_glue.h>
#include <memory_sync.h>
#include <rtcpu.h>
#include <unistd.h>

#include "comdb2.h"
#include "tag.h"
#include "types.h"
#include "translistener.h"
#include "block_internal.h"
#include "prefault.h"
#include <cdb2api.h>

#include "sql.h"
#include "sqloffload.h"
#include "errstat.h"
#include "timer.h"
#include "dbglog.h"
#include "osqlblockproc.h"
#include <gettimeofday_ms.h>
#include <endian_core.h>
#include "bdb_access.h"
#include "bdb_int.h"
#include "osqlblkseq.h"
#include "localrep.h"
#include "util.h"
#include "tohex.h"
#include "osqlcomm.h"
#include <bdb_schemachange.h>
#include "bpfunc.h"
#include "debug_switches.h"
#include "logmsg.h"
#include "reqlog.h"
#include "comdb2_atomic.h"
#include "str0.h"
#include "schemachange.h"
#include "views.h"
#include <disttxn.h>

#if 0
#define TEST_OSQL
#endif

int (*comdb2_ipc_swapnpasdb_sinfo)(struct ireq *) = 0;
void (*comdb2_ipc_setrmtdbmc)(int dbnum, char *host, int len, void *inptr) = 0;

extern int is_buffer_from_remote(const void *buf);
extern pthread_t gbl_invalid_tid;
int gbl_coordinator_wait_propagate = 1;
extern int gbl_replicant_retry_on_not_durable;
extern int gbl_enable_berkdb_retry_deadlock_bias;
extern int gbl_debug_disttxn_trace;

/* For testing 2pc: disable verify but we still want to wait as if verify were enabled.
 * This is because Verify errors are retried by definition, so it will be masked */
int gbl_debug_wait_on_verify_off;
extern int gbl_osql_verify_retries_max;
extern int verbose_deadlocks;
extern int gbl_goslow;
extern int n_commits;
extern int n_commit_time;
extern pthread_mutex_t osqlpf_mutex;
extern int gbl_prefault_udp;
extern int gbl_reorder_socksql_no_deadlock;
extern int gbl_print_blockp_stats;
extern int gbl_dump_blkseq;
extern char gbl_dbname[MAX_DBNAME_LENGTH];
extern __thread int send_prefault_udp;
extern unsigned int gbl_delayed_skip;
int gbl_debug_blkseq_race = 0;

extern void delay_if_sc_resuming(struct ireq *iq);
extern void clear_pfk(struct dbenv *dbenv, int i, int numops);

/*
   When no osql, NONE
   When osql is detected, transactions are aborted and NOTRANS
   When osql finished the sql part and starting record.c calls and
   RECREATEDTRANS
*/
enum {
    OSQL_BPLOG_NONE = 0,
    OSQL_BPLOG_NOTRANS = 1,
    OSQL_BPLOG_RECREATEDTRANS = 2
};


int gbl_sc_close_txn = 1;

#if 0
#define GOTOBACKOUT                                                            \
    do {                                                                       \
        printf("***BACKOUT*** from %d rc %d\n", __LINE__, rc);                 \
        if (1)                                                                 \
            goto backout;                                                      \
    } while (0)
#else
#define GOTOBACKOUT                                                            \
    do {                                                                       \
        fromline = __LINE__;                                                   \
        goto backout;                                                          \
    } while (0);
#endif

static int toblock_outer(struct ireq *iq, block_state_t *blkstate);
static int toblock_main(struct javasp_trans_state *javasp_trans_handle,
                        struct ireq *iq, block_state_t *blkstate);
static int block_state_offset_from_ptr(block_state_t *p_blkstate,
                                       const uint8_t *p_buf);

int gbl_blockop_count_xrefs[BLOCK_MAXOPCODE];
const char *gbl_blockop_name_xrefs[NUM_BLOCKOP_OPCODES];

static int block2_qadd(struct ireq *iq, block_state_t *p_blkstate, void *trans,
                       struct packedreq_qadd *buf, blob_buffer_t *blobs)
{
    int cblob;
    int rc;
    struct dbtable *qdb;
    struct dbtable *olddb;
    char qname[MAXTABLELEN];

    if (buf->qnamelen > sizeof(qname) - 1) {
        if (iq->debug)
            reqprintf(iq, "NAME TOO LONG %u>%zu", buf->qnamelen,
                      sizeof(qname) - 1);
        reqerrstr(iq, COMDB2_CUST_RC_NAME_SZ, "queue name to long %u>%zu",
                  buf->qnamelen, sizeof(qname) - 1);
        return ERR_BADREQ;
    }

    iq->p_buf_in = buf_no_net_get(qname, buf->qnamelen, iq->p_buf_in,
                                  p_blkstate->p_buf_next_start);

    qname[buf->qnamelen] = '\0';

    /* Must have exactly one blob, which will be the queue data. */
    for (cblob = 0; cblob < MAXBLOBS; cblob++) {
        if (blobs[cblob].exists && cblob >= 1) {
            if (iq->debug)
                reqprintf(iq, "TOO MANY BLOBS");
            reqerrstr(iq, COMDB2_QADD_RC_NB_BLOBS, "too many blobs");
            return ERR_BADREQ;
        }
        if (blobs[cblob].exists &&
            blobs[cblob].length != blobs[cblob].collected) {
            if (iq->debug)
                reqprintf(iq, "GOT BAD BLOB BUFFERS FOR BLOB %d", cblob);
            reqerrstr(iq, COMDB2_QADD_RC_BAD_BLOB_BUFF,
                      "got bad blob buffers for blob %d", cblob);
            return ERR_BADREQ;
        }
    }

    if (!blobs[0].exists) {
        if (iq->debug)
            reqprintf(iq, "EXPECTED AT LEAST ONE BLOB");
        reqerrstr(iq, COMDB2_QADD_RC_BAD_BLOB_BUFF,
                  "expected one blob (internal api error)");
        return ERR_BADREQ;
    }

    qdb = getqueuebyname(qname);
    if (!qdb) {
        if (iq->debug)
            reqprintf(iq, "UNKNOWN QUEUE '%s'", qname);
        reqerrstr(iq, COMDB2_QADD_RC_NO_QUEUE, "unknown queue '%s'", qname);
        return ERR_BADREQ;
    }

    olddb = iq->usedb;
    iq->usedb = qdb;
    rc = dbq_add(iq, trans, blobs[0].data, blobs[0].length);
    iq->usedb = olddb;
    if (iq->debug) {
        reqprintf(iq, "dbq_add:%s:rcode %d length %u dat ", qname, rc,
                  (unsigned)blobs[0].length);
        reqdumphex(iq, blobs[0].data, blobs[0].length);
    }
    return rc;
}

static int block2_custom(struct ireq *iq, struct packedreq_custom *buf,
                         const uint8_t *p_opname, blob_buffer_t *blobs)
{
    char opname[MAXCUSTOPNAME + 1];
    int rc;
    const void *data;
    size_t datalen;
    int cblob;

    if (buf->opnamelen > MAXCUSTOPNAME) {
        if (iq->debug)
            reqprintf(iq, "NAME TOO LONG %d>%d", buf->opnamelen, MAXCUSTOPNAME);
        reqerrstr(iq, COMDB2_CUST_RC_NAME_SZ, "name to long %d>%d",
                  buf->opnamelen, MAXCUSTOPNAME);
        return ERR_BADREQ;
    }
    memcpy(opname, p_opname, buf->opnamelen);
    opname[buf->opnamelen] = '\0';

    /* We can have zero blobs or one blob.  Any more is a bad request. */
    for (cblob = 0; cblob < MAXBLOBS; cblob++) {
        if (blobs[cblob].exists &&
            blobs[cblob].length != blobs[cblob].collected) {
            if (iq->debug)
                reqprintf(iq, "GOT BAD BLOB BUFFERS FOR BLOB %d", cblob);
            reqerrstr(iq, COMDB2_CUST_RC_BAD_BLOB_BUFF,
                      "got bad blob buffers for blob %d", cblob);
            return ERR_BADREQ;
        }

        if (blobs[cblob].exists && cblob >= 1) {
            if (iq->debug)
                reqprintf(iq, "TOO MANY BLOBS");
            reqerrstr(iq, COMDB2_CUST_RC_NB_BLOBS, "too many blobs");
            return ERR_BADREQ;
        }
    }

    if (blobs[0].exists) {
        data = blobs[0].data;
        datalen = blobs[0].length;
    } else {
        data = NULL;
        datalen = 0;
    }

    if (!iq->jsph) {
        if (iq->debug)
            reqprintf(iq, "JVM NOT LOADED OR NO CUSTOM OPS REGISTERED");
        reqerrstr(iq, COMDB2_CUST_RC_ENV,
                  "jvm not loaded or no custom ops registered");
        return ERR_BADREQ;
    }

    /* This will excute the custom write and fire off any listeners for it. */
    rc = javasp_custom_write(iq->jsph, opname, data, datalen);
    if (rc != 0)
        return rc;

    return 0;
}

/* this is primarily so that blockops can record how many of each opcode they
 * have without having to use a massive sparse array to hold the counters. */
void toblock_init(void)
{
    int index = 1;
    int ii;
    for (ii = 0; ii < NUM_BLOCKOP_OPCODES; ii++)
        gbl_blockop_name_xrefs[ii] = "???";
#define add_blockop(x)                                                         \
    gbl_blockop_name_xrefs[index] = breq2a((x));                               \
    gbl_blockop_count_xrefs[(x)] = index++;

    add_blockop(BLOCK_ADDSL);
    add_blockop(BLOCK_ADDSEC);
    add_blockop(BLOCK_SECAFPRI);
    add_blockop(BLOCK_ADNOD);
    add_blockop(BLOCK_DELSC);
    add_blockop(BLOCK_DELSEC);
    add_blockop(BLOCK_DELNOD);
    add_blockop(BLOCK_UPVRRN);
    add_blockop(BLOCK2_ADDDTA);
    add_blockop(BLOCK2_ADDKEY);
    add_blockop(BLOCK2_DELDTA);
    add_blockop(BLOCK2_DELKEY);
    add_blockop(BLOCK2_UPDATE);
    add_blockop(BLOCK2_ADDKL);
    add_blockop(BLOCK2_DELKL);
    add_blockop(BLOCK2_UPDKL);
    add_blockop(BLOCK2_ADDKL_POS);
    add_blockop(BLOCK2_UPDKL_POS);
    add_blockop(BLOCK_DEBUG);
    add_blockop(BLOCK_SEQ);
    add_blockop(BLOCK_USE);
    add_blockop(BLOCK2_USE);
    add_blockop(BLOCK2_SEQ);
    add_blockop(BLOCK2_QBLOB);
    add_blockop(BLOCK2_RNGDELKL);
    add_blockop(BLOCK_SETFLAGS);
    add_blockop(BLOCK2_CUSTOM);
    add_blockop(BLOCK2_QADD);
    add_blockop(BLOCK2_QCONSUME);
    add_blockop(BLOCK2_TZ);
    add_blockop(BLOCK2_TRAN);
    add_blockop(BLOCK2_DELOLDER);
    add_blockop(BLOCK2_DELOLDER);
    add_blockop(BLOCK2_SOCK_SQL);
    add_blockop(BLOCK2_SCSMSK);
    add_blockop(BLOCK2_RECOM);
    add_blockop(BLOCK2_UPDBYKEY);
    add_blockop(BLOCK2_SNAPISOL);
    add_blockop(BLOCK2_SERIAL);
    add_blockop(BLOCK2_DBGLOG_COOKIE);
    add_blockop(BLOCK2_PRAGMA);
    add_blockop(BLOCK2_SEQV2);
    add_blockop(BLOCK2_UPTBL);
#undef add_blockop
    /* a runtime assert to make sure we have the right size of blockop count
     * array */
    if (index >= NUM_BLOCKOP_OPCODES) {
        logmsg(LOGMSG_FATAL, "%s: too many blockops defined!\n", __func__);
        logmsg(LOGMSG_FATAL, "%s: you need to increase NUM_BLOCKOP_OPCODES to %d\n",
                __func__, index);
        exit(1);
    }
}

const char *breq2a(int req)
{
    switch (req) {
    case BLOCK_ADDSL:
        return "BLOCK_ADDSL";
    case BLOCK_ADDSEC:
        return "BLOCK_ADDSEC";
    case BLOCK_SECAFPRI:
        return "BLOCK_SECAFPRI";
    case BLOCK_ADNOD:
        return "BLOCK_ADNOD";
    case BLOCK_DELSC:
        return "BLOCK_DELSC";
    case BLOCK_DELSEC:
        return "BLOCK_DELSEC";
    case BLOCK_DELNOD:
        return "BLOCK_DELNOD";
    case BLOCK_UPVRRN:
        return "BLOCK_UPVRRN";
    case BLOCK2_ADDDTA:
        return "BLOCK2_ADDDTA";
    case BLOCK2_ADDKEY:
        return "BLOCK2_ADDKEY";
    case BLOCK2_DELDTA:
        return "BLOCK2_DELDTA";
    case BLOCK2_DELKEY:
        return "BLOCK2_DELKEY";
    case BLOCK2_UPDATE:
        return "BLOCK2_UPDATE";
    case BLOCK2_ADDKL:
        return "BLOCK2_ADDKL";
    case BLOCK2_ADDKL_POS:
        return "BLOCK2_ADDKL_POS";
    case BLOCK2_DELKL:
        return "BLOCK2_DELKL";
    case BLOCK2_UPDKL:
        return "BLOCK2_UPDKL";
    case BLOCK2_UPDKL_POS:
        return "BLOCK2_UPDKL_POS";
    case BLOCK_DEBUG:
        return "BLOCK_DEBUG";
    case BLOCK_SEQ:
        return "BLOCK_SEQ";
    case BLOCK_USE:
        return "BLOCK_USE";
    case BLOCK2_USE:
        return "BLOCK2_USE";
    case BLOCK2_TZ:
        return "BLOCK2_TZ";
    case BLOCK2_SEQ:
        return "BLOCK2_SEQ";
    case BLOCK2_QBLOB:
        return "BLOCK2_QBLOB";
    case BLOCK2_RNGDELKL:
        return "BLOCK2_RNGDELKL";
    case BLOCK_SETFLAGS:
        return "BLOCK_SETFLAGS";
    case BLOCK2_CUSTOM:
        return "BLOCK2_CUSTOM";
    case BLOCK2_QADD:
        return "BLOCK2_QADD";
    case BLOCK2_QCONSUME:
        return "BLOCK2_QCONSUME";
    case BLOCK2_TRAN:
        return "BLOCK2_TRAN";
    case BLOCK2_DELOLDER:
        return "BLOCK2_DELOLDER";
    case BLOCK2_MODNUM:
        return "BLOCK2_MODNUM";
    case BLOCK2_SOCK_SQL:
        return "BLOCK2_SOCK_SQL";
    case BLOCK2_SCSMSK:
        return "BLOCK2_SCSMSK";
    case BLOCK2_RECOM:
        return "BLOCK2_RECOM";
    case BLOCK2_UPDBYKEY:
        return "BLOCK2_UPDBYKEY";
    case BLOCK2_SNAPISOL:
        return "BLOCK2_SNAPISOL";
    case BLOCK2_SERIAL:
        return "BLOCK2_SERIAL";
    case BLOCK2_DBGLOG_COOKIE:
        return "BLOCK2_DBGLOG_COOKIE";
    case BLOCK2_PRAGMA:
        return "BLOCK2_PRAGMA";
    case BLOCK2_SEQV2:
        return "BLOCK2_SEQV2";
    case BLOCK2_UPTBL:
        return "BLOCK2_UPTBL";
    default:
        return "??";
    }
}

/* this forwards the block operation to master machine.  then it adds
   response to table of outstanding requests to await reply and log catch-up */
static int forward_longblock_to_master(struct ireq *iq,
                                       block_state_t *p_blkstate, char *mstr)
{
    int rc = 0;
    struct longblock_fwd_pre_hdr fwd;
    struct req_hdr req_hdr;
    size_t req_len;

    if (mstr == bdb_master_dupe || mstr == db_eid_invalid) {
        if (iq->debug)
            logmsg(LOGMSG_ERROR, "%s:no master! (%s) req from %s\n", __func__,
                   mstr, getorigin(iq));
        return ERR_NOMASTER;
    }

    /*modify request to indicate forwarded and send off to remote */
    if (req_hdr_get(&req_hdr, iq->p_buf_out_start,
                    p_blkstate->p_buf_req_start, 0) != p_blkstate->p_buf_req_start)
        return ERR_INTERNAL;
    req_hdr.opcode = OP_FWD_LBLOCK;
    if (req_hdr_put(&req_hdr, iq->p_buf_out_start,
                    p_blkstate->p_buf_req_start) != p_blkstate->p_buf_req_start)
        return ERR_INTERNAL;

    fwd.flags = 0;
    if (iq->errstrused)
        fwd.flags |= BLKF_ERRSTAT;
    /* write it and make sure we wrote the same length */
    if (p_blkstate->p_buf_req_start != iq->p_buf_out ||
        !(iq->p_buf_out = longblock_fwd_pre_hdr_put(&fwd, iq->p_buf_out,
                                                    iq->p_buf_out_end)))
        return ERR_INTERNAL;

    req_len = p_blkstate->p_buf_req_end - iq->p_buf_out_start;

    /*have a valid master to pass this off to. */
    if (iq->debug)
        reqprintf(iq, "forwarded req from %s to master node %s db %d rqlen "
                "%zu\n", getorigin(iq), mstr, iq->origdb->dbnum, req_len);
    if (iq->is_socketrequest || iq->ipc_sndbak) {
        if (iq->sb == NULL && iq->is_socketrequest) {
            // what case is this?
            return ERR_INCOHERENT;
        } else {
            rc = offload_comm_send_blockreq(mstr, iq->request_data,
                                            iq->p_buf_out_start, req_len);
            free_bigbuf_nosignal(iq->p_buf_out_start);
        }
    } else if (comdb2_ipc_swapnpasdb_sinfo) {
        /* Don't change anything in request for socket-fstsnd. */
        if (comdb2_ipc_setrmtdbmc) {
            if (iq->origdb->dbnum)
                comdb2_ipc_setrmtdbmc(iq->origdb->dbnum, mstr, req_len,
                                      iq->p_buf_out_start);
            else
                comdb2_ipc_setrmtdbmc(thedb->dbnum, mstr, req_len,
                                      iq->p_buf_out_start);
        }
        rc = comdb2_ipc_swapnpasdb_sinfo(iq);
    }

    if (rc != 0) {
        logmsg(LOGMSG_ERROR, "%s:failed to send to master, rc %d\n", __func__, rc);
        return ERR_REJECTED;
    }
    return RC_INTERNAL_FORWARD;
}

static int forward_block_to_master(struct ireq *iq, block_state_t *p_blkstate,
                                   char *mstr)
{
    int rc = 0;
    struct block_fwd fwd;
    struct req_hdr req_hdr;
    size_t req_len;

    if (mstr == bdb_master_dupe || mstr == db_eid_invalid) {
        if (iq->debug)
            logmsg(LOGMSG_ERROR, "%s:no master! (%s) req from %s\n", __func__,
                   mstr, getorigin(iq));
        return ERR_NOMASTER;
    }


    /*modify request to indicate forwarded and send off to remote */
    if (req_hdr_get(&req_hdr, iq->p_buf_out_start,
                    p_blkstate->p_buf_req_start,iq->comdbg_flags) != p_blkstate->p_buf_req_start)
        return ERR_INTERNAL;
    req_hdr.opcode = OP_FWD_BLOCK;
    if (iq->comdbg_flags & COMDBG_FLAG_FROM_LE) {
        req_hdr.opcode = OP_FWD_BLOCK_LE;
    }
    req_hdr.luxref = iq->luxref;
    if (req_hdr_put(&req_hdr, iq->p_buf_out_start,
                    p_blkstate->p_buf_req_start) != p_blkstate->p_buf_req_start)
        return ERR_INTERNAL;

    // source node isn't used - so reuse it for flags, which we otherwse don't have
    // in a forwarded request
    fwd.flags = 0;
    fwd.offset =
        block_state_offset_from_ptr(p_blkstate, p_blkstate->p_buf_req_end);
    fwd.num_reqs = p_blkstate->numreq;

    // if the original request had BLKF_ERRSTAT set, set it in the forwarded
    // request as well.
    if (iq->errstrused)
        fwd.flags |= BLKF_ERRSTAT;

    /* write it and make sure we wrote the same length */
    if (p_blkstate->p_buf_req_start != iq->p_buf_out ||
        (iq->p_buf_out = block_fwd_put(&fwd, iq->p_buf_out,
                                       iq->p_buf_out_end, iq->comdbg_flags)) != iq->p_buf_in)
        return ERR_INTERNAL;

    req_len = p_blkstate->p_buf_req_end - iq->p_buf_out_start;

    if (iq->is_socketrequest || iq->ipc_sndbak) {
        if (iq->is_socketrequest && iq->sb == NULL) {
            return ERR_INCOHERENT;
        } else {
            rc = offload_comm_send_blockreq(mstr, iq->request_data,
                                            iq->p_buf_out_start, req_len);
            free_bigbuf_nosignal(iq->p_buf_out_start);
        }
    } else if (comdb2_ipc_swapnpasdb_sinfo) {
        if (comdb2_ipc_setrmtdbmc) {
            if (iq->origdb->dbnum)
                comdb2_ipc_setrmtdbmc(iq->origdb->dbnum, mstr, req_len,
                                      iq->p_buf_out_start);
            else
                comdb2_ipc_setrmtdbmc(thedb->dbnum, mstr, req_len,
                                      iq->p_buf_out_start);
        }
        rc = comdb2_ipc_swapnpasdb_sinfo(iq);
    }

    if (rc != 0) {
        logmsg(LOGMSG_ERROR, "%s:failed to forward to master, rc %d\n",
               __func__, rc);
        return ERR_REJECTED;
    }
    return RC_INTERNAL_FORWARD;
}

/**
 * Turn an offset into a ptr into the req buf.  This routine sucks, but what can
 * we do, all the offsets in block_reqs are absolute 1 based word offsets into
 * the fstsnd buffer.
 * @param p_blkstate    pointer to the block_state containing the buffer
 * @param offset    1 based from from the begining of fstsnd buffer in words
 * @return pointer that offset referenced on success; NULL otherwise
 */
static const uint8_t *block_state_ptr_from_offset(block_state_t *p_blkstate,
                                                  int offset)
{
    /*  remove the length prccom hdr which isn't in p_buf_req_start */
    return ptr_from_one_based_word_offset(
        p_blkstate->p_buf_req_start - REQ_HDR_LEN, offset);
}

/**
 * Turn a ptr in the req buf into an offset.  This routine sucks, but what can
 * we do, all the offsets in block_reqs are absolute 1 based word offsets into
 * the fstsnd buffer.
 * Note: no sanity checks are performed.
 * Note: offset may not be exact if ptr is not on a word boundry.
 * @param p_blkstate    pointer to the block_state containing the buffer
 * @param p_buf     pointer to calculate offset for
 * @return 1 based from from the begining of fstsnd buffer in words
 */
static int block_state_offset_from_ptr(block_state_t *p_blkstate,
                                       const uint8_t *p_buf)
{
    /*  remove the length prccom hdr which isn't in p_buf_req_start */
    return one_based_word_offset_from_ptr(
        p_blkstate->p_buf_req_start - REQ_HDR_LEN, p_buf);
}

/**
 * Validate and save offset as the end of the whole block_req.
 * @param iq    pointer to ireq to read block_req from
 * @param p_blkstate    pointer to the block_state containing the buffer
 * @param offset    1 based from from the begining of fstsnd buffer in words
 * @see block_state_next()
 */
static int block_state_set_end(struct ireq *iq, block_state_t *p_blkstate,
                               int offset)
{
    const uint8_t *p_buf;

    if (!(p_buf = block_state_ptr_from_offset(p_blkstate, offset))) {
        if (iq->debug)
            reqprintf(iq, "BAD OFFSET %d", offset);
        reqerrstr(iq, COMDB2_BLK_RC_NB_REQS, "bad offset %d", offset);
        return ERR_BADREQ;
    }

    if (p_buf < iq->p_buf_in || p_buf > iq->p_buf_in_end) {
        if (iq->debug)
            reqprintf(iq, "%s OFFSET OUT OF RANGE %d", __func__, offset);
        reqerrstr(iq, COMDB2_BLK_RC_NB_REQS, "OFFSET OUT OF RANGE %d", offset);
        return ERR_BADREQ;
    }

    p_blkstate->p_buf_req_end = p_buf;
    return RC_OK;
}

/**
 * Validate and save offset as the end of the whole longblock_req.
 * @param iq    pointer to ireq to read block_req from
 * @param p_blkstate    pointer to the block_state containing the buffer
 * @param offset    0 based from from the begining of fstsnd buffer in words;
 *                  yes, that's right, the offset in the header for long blocks
 *                  is 0 based, not 1 based like every other offset we get in
 *                  the whole block processor, grrrrrrrrrrrrrrrrrrrrrrrrr.
 * @see block_state_next()
 */
static int block_state_set_end_long(struct ireq *iq, block_state_t *p_blkstate,
                                    int offset)
{
    return block_state_set_end(iq, p_blkstate, offset + 1 /*make it 1 based*/);
}

/**
 * Validate and save offset as the beginning of the next block op.
 * @param iq    pointer to ireq to read block_req from
 * @param p_blkstate    pointer to the block_state containing the buffer
 * @param offset    1 based from from the begining of fstsnd buffer in words
 * @see block_state_next()
 */
int block_state_set_next(struct ireq *iq, block_state_t *p_blkstate, int offset)
{
    const uint8_t *p_buf;

    if (!(p_buf = block_state_ptr_from_offset(p_blkstate, offset))) {
        if (iq->debug)
            reqprintf(iq, "BAD OFFSET %d", offset);
        reqerrstr(iq, COMDB2_BLK_RC_NB_REQS, "bad offset %d", offset);
        return ERR_BADREQ;
    }

    if (p_buf < iq->p_buf_in || p_buf > p_blkstate->p_buf_req_end) {
        if (iq->debug)
            reqprintf(iq, "%s OFFSET OUT OF RANGE %d", __func__, offset);
        reqerrstr(iq, COMDB2_BLK_RC_NB_REQS, "OFFSET OUT OF RANGE %d", offset);
        return ERR_BADREQ;
    }

    p_blkstate->p_buf_next_start = p_buf;
    return RC_OK;
}

/**
 * Jump the iq to the begining of the next block op as saved by
 * block_state_set_next().
 * @param iq    pointer to ireq to read block_req from
 * @param p_blkstate    pointer to the block_state containing the buffer
 * @see block_state_set_next()
 */
int block_state_next(struct ireq *iq, block_state_t *p_blkstate)
{
    iq->p_buf_in = p_blkstate->p_buf_next_start;
    p_blkstate->p_buf_next_start = NULL;
    return RC_OK;
}

/**
 * Backup the req through a point.  If we've already backed up to or past the
 * given point, do nothing.
 * @param p_blkstate    pointer to the block_state containing the buffer to save
 * @param p_buf_save_thru   pointer to just past the end of the data to save,
 *                          must point inside p_blkstate's buffer
 * @return 0 on success; !0 otherwise
 * @see block_state_restore()
 */
static int block_state_backup(block_state_t *p_blkstate,
                              const uint8_t *p_buf_save_thru, int useblobmem)
{
    size_t already_saved_len;
    size_t to_save_len;
    const uint8_t *p_buf_to_save;

    /* if we havn't yet, create a space to backup the req */
    if (!p_blkstate->p_buf_saved_start) {
        size_t buf_req_len;

        buf_req_len = p_blkstate->p_buf_req_end - p_blkstate->p_buf_req_start;

        if (!useblobmem || blobmem == NULL)
            p_blkstate->p_buf_saved_start = malloc(buf_req_len);
        else
            p_blkstate->p_buf_saved_start =
                comdb2_bmalloc(blobmem, buf_req_len);

        if (!p_blkstate->p_buf_saved_start)
            return ERR_INTERNAL;

        p_blkstate->p_buf_saved = p_blkstate->p_buf_saved_start;
        p_blkstate->p_buf_saved_end =
            p_blkstate->p_buf_saved_start + buf_req_len;
    }

    already_saved_len = p_blkstate->p_buf_saved - p_blkstate->p_buf_saved_start;
    p_buf_to_save = p_blkstate->p_buf_req_start + already_saved_len;

    /* if we've already backed up enough */
    if (p_buf_to_save > p_buf_save_thru)
        return RC_OK;

    to_save_len = p_buf_save_thru - p_buf_to_save;

    /* backup the part we've read already */
    if (!(p_blkstate->p_buf_saved = buf_no_net_put(
              p_buf_to_save, to_save_len, p_blkstate->p_buf_saved,
              p_blkstate->p_buf_saved_end)))
        return ERR_INTERNAL;

    return RC_OK;
}

/**
 * Restore the req buf from backup.  This will restore as much of the req buf as
 * has been previously restored with block_state_backup()
 * @param iq    pointer to the ireq whose buffer will be restored
 * @param p_blkstate    pointer to the block_state containing the backup to use
 * @return 0 on success; !0 otherwise
 * @see block_state_backup()
 */
static int block_state_restore(struct ireq *iq, block_state_t *p_blkstate)
{
    /* restore from backup */
    if (!buf_no_net_put(p_blkstate->p_buf_saved_start,
                        p_blkstate->p_buf_saved - p_blkstate->p_buf_saved_start,
                        (uint8_t *)p_blkstate->p_buf_req_start,
                        p_blkstate->p_buf_req_end))
        return ERR_INTERNAL;

    iq->p_buf_in = p_blkstate->p_buf_req_start;
    iq->p_buf_in_end = p_blkstate->p_buf_req_end;
    iq->p_buf_out = (uint8_t *)iq->p_buf_out_start + REQ_HDR_LEN;
    iq->p_buf_out_end = (uint8_t *)iq->p_buf_out_end;

    return RC_OK;
}

/**
 * Release all the resources held by a block_state.
 */
static void block_state_free(block_state_t *p_blkstate)
{
    free(p_blkstate->p_buf_saved_start);
    p_blkstate->p_buf_saved_start = NULL;
}

unsigned long long blkseq_replay_count = 0;
unsigned long long blkseq_replay_error_count = 0;

void replay_stat(void)
{
    logmsg(LOGMSG_USER, "Blkseq-replay-count: %llu\n", blkseq_replay_count);
    logmsg(LOGMSG_USER, "Blkseq-replay-error-count: %llu\n",
           blkseq_replay_error_count);
}

void flush_db(void);

/* buf_fstblk is sized FSTBLK_MAX_BUF_LEN */
int prepare_dist_abort_blkseq(uint8_t *buf_fstblk, int *outlen)
{
    uint8_t *p_buf_fstblk = buf_fstblk;
    const uint8_t *p_buf_fstblk_end = buf_fstblk + FSTBLK_MAX_BUF_LEN;

    struct fstblk_header fstblk_header;
    struct fstblk_pre_rspkl fstblk_pre_rspkl;

    /* Prepares require cnonce fstblk keys */
    fstblk_header.type = FSTBLK_SNAP_INFO;
    fstblk_pre_rspkl.fluff = (short)0;

    if (!(p_buf_fstblk = fstblk_header_put(&fstblk_header, p_buf_fstblk, p_buf_fstblk_end))) {
        return ERR_INTERNAL;
    }

    if (!(p_buf_fstblk = fstblk_pre_rspkl_put(&fstblk_pre_rspkl, p_buf_fstblk, p_buf_fstblk_end))) {
        return ERR_INTERNAL;
    }

    int outrc = ERR_DIST_ABORT;

    if (!(p_buf_fstblk = buf_put(&(outrc), sizeof(outrc), p_buf_fstblk, p_buf_fstblk_end))) {
        return ERR_INTERNAL;
    }

    errstat_t errstat = {0};
    errstat.errval = ERR_DIST_ABORT;
    if (gbl_debug_disttxn_trace) {
        logmsg(LOGMSG_USER, "DISTTXN %s line %d aborting\n", __func__, __LINE__);
    }
    snprintf(errstat.errstr, sizeof(errstat.errstr), "Transaction aborted by coordinator");

    if (!(p_buf_fstblk = osqlcomm_errstat_type_put(&errstat, p_buf_fstblk, p_buf_fstblk_end))) {
        return ERR_INTERNAL;
    }

    struct query_effects effects = {0};

    if (!(p_buf_fstblk = osqlcomm_query_effects_put(&effects, p_buf_fstblk, p_buf_fstblk_end))) {
        return ERR_INTERNAL;
    }

    struct fstblk_rspkl fstblk_rspkl = {0};
    if (!(p_buf_fstblk = fstblk_rspkl_put(&fstblk_rspkl, p_buf_fstblk, p_buf_fstblk_end))) {
        return ERR_INTERNAL;
    }

    struct block_err block_err = {0};
    if (!(p_buf_fstblk = block_err_put(&block_err, p_buf_fstblk, p_buf_fstblk_end))) {
        return ERR_INTERNAL;
    }

    int t = comdb2_time_epoch();

    if (!(p_buf_fstblk = buf_no_net_put(&(t), sizeof(t), p_buf_fstblk, p_buf_fstblk_end))) {
        return ERR_INTERNAL;
    }
    (*outlen) = (p_buf_fstblk - buf_fstblk);

    return 0;
}

int dist_txn_abort_write_blkseq(void *in_bdb_state, void *bskey, int bskeylen)
{
    bdb_state_type *bdb_state = (in_bdb_state ? in_bdb_state : thedb->bdb_env);
    uint8_t buf_fstblk[FSTBLK_MAX_BUF_LEN];
    int outlen = 0, rc;
    if ((rc = prepare_dist_abort_blkseq(buf_fstblk, &outlen)) != 0) {
        logmsg(LOGMSG_ERROR, "%s: error %d preparing blkseq payload\n", __func__, rc);
        return rc;
    }
    assert(outlen <= FSTBLK_MAX_BUF_LEN);
    return bdb_blkseq_insert(bdb_state, NULL, bskey, bskeylen, buf_fstblk, outlen, NULL, NULL, 1);
}

static int do_replay_case(struct ireq *iq, void *fstseqnum, int seqlen,
                          int num_reqs, int check_long_trn, void *replay_data,
                          int replay_data_len, unsigned int line)
{
    int rc = 0;
    int outrc = 0, snapinfo_outrc = 0, snapinfo = 0;
    uint8_t buf_fstblk[FSTBLK_MAX_BUF_LEN];
    uint8_t *p_fstblk_buf = buf_fstblk,
            *p_fstblk_buf_end = buf_fstblk + sizeof(buf_fstblk);

    struct fstblk_pre_rspkl fstblk_pre_rspkl;
    struct fstblk_rspkl fstblk_rspkl;
    struct block_rspkl rspkl;
    struct fstblk_rspok fstblk_rspok;
    struct fstblk_header fstblk_header;
    struct fstblk_rsperr fstblk_rsperr;
    struct block_rsp rsp;
    struct block_err err;

    int blkseq_line = 0;
    int datalen = replay_data_len - 4;

    if (!replay_data) {

        if (!IQ_HAS_SNAPINFO(iq)) {
            assert( (seqlen == (sizeof(fstblkseq_t))) || 
                    (seqlen == (sizeof(uuid_t))));
        }

        rc = bdb_blkseq_find(thedb->bdb_env, NULL, fstseqnum, seqlen,
                             &replay_data, &replay_data_len);
        if (rc == IX_FND) {
            memcpy(buf_fstblk, replay_data, replay_data_len - 4);
            datalen = replay_data_len - 4;
        }
    }

    if (rc == IX_NOTFND)
    /*
       XXX
       we failed to add cause it was already there, but then
       we failed to find cause it got deleted out from under us
       by the timer trap.  we know its a replay, but we dont know
       what response to give back...
       this should be impossible, as we guarantee to hold blkseqs for
       a much longer time than the client is allowed to retry.
    */
    {
        int *seq = (int *)fstseqnum;
        if (!check_long_trn)
            logmsg(LOGMSG_ERROR, 
                    "%s: %08x:%08x:%08x fstblk replay deleted under us\n",
                    __func__, seq[0], seq[1], seq[2]);
        blkseq_line = __LINE__;
        goto replay_error;
    } else if (rc != 0) {
        int *seq = (int *)fstseqnum;
        logmsg(LOGMSG_ERROR, "%s: %08x:%08x:%08x unexpected fstblk find error %d\n",
                __func__, seq[0], seq[1], seq[2], rc);
        blkseq_line = __LINE__;
        goto replay_error;
    } else if (datalen < sizeof(struct fstblk_header)) {
        int *seq = (int *)fstseqnum;
        logmsg(LOGMSG_ERROR,
               "%s: %08x:%08x:%08x fstblk replay too small %d < %zu\n",
               __func__, seq[0], seq[1], seq[2], datalen,
               sizeof(struct fstblk_header));
        blkseq_line = __LINE__;
        goto replay_error;
    } else {
        p_fstblk_buf_end = buf_fstblk + datalen;

        if (replay_data_len > sizeof(buf_fstblk))
            rc = IX_ACCESS;
        else {
            /* we add a timestamp to the end of the payload - but we don't
             * want it as part of the response - skip those extra 4 bytes. */
            memcpy(buf_fstblk, replay_data, replay_data_len - 4);
            datalen = replay_data_len - 4;
            rc = 0;
        }

        if (!(p_fstblk_buf = (uint8_t *)fstblk_header_get(
                  &fstblk_header, p_fstblk_buf, p_fstblk_buf_end))) {
            logmsg(LOGMSG_ERROR, "%s: error on fstblk_header_get\n", __func__);
            blkseq_line = __LINE__;
            goto replay_error;
        }

        switch (fstblk_header.type) {
        case FSTBLK_RSPOK: {
            rsp.num_completed = num_reqs;
            if (!(iq->p_buf_out =
                      block_rsp_put(&rsp, iq->p_buf_out, iq->p_buf_out_end, iq->comdbg_flags))) {
                blkseq_line = __LINE__;
                goto replay_error;
            }

            if (!(iq->p_buf_out =
                      buf_zero_put(sizeof(int) * num_reqs, iq->p_buf_out,
                                   iq->p_buf_out_end))) {
                blkseq_line = __LINE__;
                goto replay_error;
            }

            /* consume fluff */
            if (!(p_fstblk_buf = (uint8_t *)fstblk_rspok_get(
                      &fstblk_rspok, p_fstblk_buf, p_fstblk_buf_end))) {
                blkseq_line = __LINE__;
                goto replay_error;
            }

            if (!(iq->p_buf_out = buf_no_net_put(
                      p_fstblk_buf, p_fstblk_buf_end - p_fstblk_buf,
                      iq->p_buf_out, iq->p_buf_out_end))) {
                blkseq_line = __LINE__;
                goto replay_error;
            }

            outrc = RC_OK;

            break;
        }

        case FSTBLK_RSPERR: {
            if (!(p_fstblk_buf = (uint8_t *)fstblk_rsperr_get(
                      &fstblk_rsperr, p_fstblk_buf, p_fstblk_buf_end))) {
                blkseq_line = __LINE__;
                goto replay_error;
            }

            rsp.num_completed = fstblk_rsperr.num_completed;
            if (!(iq->p_buf_out =
                      block_rsp_put(&rsp, iq->p_buf_out, iq->p_buf_out_end, iq->comdbg_flags))) {
                blkseq_line = __LINE__;
                goto replay_error;
            }

            if (!(iq->p_buf_out =
                      buf_zero_put(sizeof(int) * num_reqs, iq->p_buf_out,
                                   iq->p_buf_out_end))) {
                blkseq_line = __LINE__;
                goto replay_error;
            }

            if (!(iq->p_buf_out = buf_no_net_put(
                      p_fstblk_buf, fstblk_rsperr.num_completed * sizeof(int),
                      iq->p_buf_out, iq->p_buf_out_end))) {
                blkseq_line = __LINE__;
                goto replay_error;
            }

            if (!(iq->p_buf_out =
                      buf_put(&fstblk_rsperr.rcode, sizeof(fstblk_rsperr.rcode),
                              iq->p_buf_out, iq->p_buf_out_end))) {
                blkseq_line = __LINE__;
                goto replay_error;
            }
            outrc = ERR_BLOCK_FAILED;
            break;
        }

        case FSTBLK_SNAP_INFO:
            snapinfo = 1; /* fallthrough */

        case FSTBLK_RSPKL: 
        {
            /* fluff */
            if (!(p_fstblk_buf = (uint8_t *)fstblk_pre_rspkl_get(
                      &fstblk_pre_rspkl, p_fstblk_buf, p_fstblk_buf_end))) {
                blkseq_line = __LINE__;
                goto replay_error;
            }

            if (snapinfo) {
                if (!(p_fstblk_buf = (uint8_t *)buf_get(&(snapinfo_outrc), sizeof(snapinfo_outrc), 
                        p_fstblk_buf, p_fstblk_buf_end)))  {
                    flush_db();
                    abort();
                }
                if (snapinfo_outrc != 0) {
                    if (!(p_fstblk_buf = (uint8_t *)osqlcomm_errstat_type_get(
                              &(iq->errstat), p_fstblk_buf,
                              p_fstblk_buf_end))) {
                        flush_db();
                        abort();
                    }
                }
                // retrieve the effects
                if (IQ_HAS_SNAPINFO(iq) &&
                    (!(p_fstblk_buf = (uint8_t *)osqlcomm_query_effects_get(
                           &(IQ_SNAPINFO(iq)->effects), p_fstblk_buf,
                           p_fstblk_buf_end)))) {
                    flush_db();
                    abort();
                }
            }

            if (!(p_fstblk_buf = (uint8_t *)fstblk_rspkl_get(
                      &fstblk_rspkl, p_fstblk_buf, p_fstblk_buf_end))) {
                blkseq_line = __LINE__;
                goto replay_error;
            }

            rspkl.num_completed = fstblk_rspkl.num_completed;
            rspkl.numerrs = fstblk_rspkl.numerrs;

            if (!(iq->p_buf_out = block_rspkl_put(&rspkl, iq->p_buf_out,
                                                  iq->p_buf_out_end))) {
                blkseq_line = __LINE__;
                goto replay_error;
            }

            if (fstblk_rspkl.numerrs > 0) {

                if (!(p_fstblk_buf = (uint8_t *)block_err_get(
                          &err, p_fstblk_buf, p_fstblk_buf_end))) {
                    blkseq_line = __LINE__;
                    goto replay_error;
                }

                if (!(iq->p_buf_out = block_err_put(&err, iq->p_buf_out,
                                                    iq->p_buf_out_end))) {
                    blkseq_line = __LINE__;
                    goto replay_error;
                }

                if (snapinfo) {
                    outrc = snapinfo_outrc;
                } else {
                    switch (err.errcode) {
                    case ERR_NO_RECORDS_FOUND:
                    case ERR_CONVERT_DTA:
                    case ERR_NULL_CONSTRAINT:
                    case ERR_SQL_PREP:
                    case ERR_CONSTR:
                    case ERR_UNCOMMITTABLE_TXN:
                    case ERR_NOMASTER:
                    case ERR_DIST_ABORT:
                    case ERR_NOTSERIAL:
                        outrc = err.errcode;
                        break;
                    default:
                        outrc = ERR_BLOCK_FAILED;
                        break;
                    }
                }

                if (iq->sorese) {
                    /* reconstruct sorese rcout similar to toblock_main */
                    if (outrc) {
                        switch (outrc) {
                        case ERR_NO_RECORDS_FOUND:
                        case ERR_CONVERT_DTA:
                        case ERR_NULL_CONSTRAINT:
                        case ERR_SQL_PREP:
                        case ERR_CONSTR:
                        case ERR_UNCOMMITTABLE_TXN:
                        case ERR_NOMASTER:
                        case ERR_NOTSERIAL:
                        case ERR_DIST_ABORT:
                        case ERR_SC:
                        case ERR_TRAN_TOO_BIG:
                            iq->sorese->rcout = outrc;
                            break;
                        default:
                            iq->sorese->rcout = outrc + err.errcode;
                            break;
                        }
                    }
                }
            } else {
                /* we need to clear sorese->rcout, it might store some
                   2nd run error */
                if (iq->sorese)
                    iq->sorese->rcout = 0;

                outrc = RC_OK;
            }

            break;
        }

        default:
            logmsg(LOGMSG_ERROR, "%s: bad fstblk replay type %d\n", __func__,
                    fstblk_header.type);
            blkseq_line = __LINE__;
            goto replay_error;
        }
    }

    char *printkey = NULL;

    /* Snapinfo is ascii text */
    if (snapinfo) {
        assert(IQ_HAS_SNAPINFO(iq));
        printkey = (char *)malloc(seqlen + 1);
        memcpy(printkey, fstseqnum, seqlen);
        printkey[seqlen]='\0';
    }
    else {
        printkey = (char *)malloc((seqlen * 2) + 1);
        printkey[0] = '\0';
        util_tohex(printkey, fstseqnum, seqlen);
    }

    char *cnonce = NULL;
    if (IQ_HAS_SNAPINFO_KEY(iq)) {
        cnonce = alloca(IQ_SNAPINFO(iq)->keylen + 1);
        memcpy(cnonce, IQ_SNAPINFO(iq)->key, IQ_SNAPINFO(iq)->keylen);
        cnonce[IQ_SNAPINFO(iq)->keylen] = '\0';
    }

    logmsg(LOGMSG_WARN,
           "%s from line %d replay returns %d for fstblk %s, cnonce %s\n",
           __func__, line, outrc, printkey, cnonce);
    free(printkey);

    /* If the latest commit is durable, then the blkseq commit must be durable.
     * This can incorrectly report NOT_DURABLE but that's sane given that half
     * the cluster is incoherent */
    if ((bdb_attr_get(thedb->bdb_attr, BDB_ATTR_DURABLE_LSNS) || gbl_replicant_retry_on_not_durable) &&
        !bdb_latest_commit_is_durable(thedb->bdb_env)) {
        if (IQ_HAS_SNAPINFO_KEY(iq)) {
            logmsg(LOGMSG_ERROR,
                   "%u replay rc changed from %d to NOT_DURABLE for blkseq '%s'\n",
                   line, outrc, cnonce);
        }
        outrc = ERR_NOT_DURABLE;
    }

    if (gbl_dump_blkseq && IQ_HAS_SNAPINFO_KEY(iq)) {
        logmsg(LOGMSG_USER,
               "Replay case for '%s' rc=%d, errval=%d errstr='%s' rcout=%d\n",
               cnonce, outrc, iq->errstat.errval, iq->errstat.errstr,
               iq->sorese ? iq->sorese->rcout : 0);
    }
    blkseq_replay_count++;
    return outrc;

replay_error:
    logmsg(LOGMSG_ERROR, "%s:%d in REPLAY ERROR\n", __func__, blkseq_line);
    if (check_long_trn) {
        outrc = RC_TRAN_CLIENT_RETRY;
        return outrc;
    }
    struct block_rsp errrsp;
    errrsp.num_completed = 0;
    iq->p_buf_out = block_rsp_put(&errrsp, iq->p_buf_out, iq->p_buf_out_end, iq->comdbg_flags);

    outrc = ERR_BLOCK_FAILED;
    blkseq_replay_error_count++;
    return outrc;
}

/**
 * Unpack a longblock_req_pre_hdr from iq's in buffer, and put it's data into
 * p_blkstate.
 * Meant to be called by toblock().
 * @param iq    pointer to ireq to read longblock_req_pre_hdr from
 * @param p_blkstate    block_state to put longblock_req_pre_hdr data into
 * @return 0 on success; !0 otherwise
 * @see toblock()
 */
static int tolongblock_req_pre_hdr_int(struct ireq *iq,
                                       block_state_t *p_blkstate)
{
    struct longblock_req_pre_hdr req;

    if (!(iq->p_buf_in = longblock_req_pre_hdr_get(&req, iq->p_buf_in,
                                                   iq->p_buf_in_end))) {
        if (iq->debug)
            reqprintf(iq, "BAD LONG BLOCK REQ");
        return ERR_BADREQ;
    }

    iq->errstrused = req.flags & BLKF_ERRSTAT;

    p_blkstate->source_host = NULL;

    return RC_OK;
}

pthread_mutex_t delay_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * Unpack a longblock_fwd_pre_hdr from iq's in buffer, and put it's data into
 * p_blkstate.
 * Meant to be called by toblock().
 * @param iq    ireq to read longblock_fwd_pre_hdr from
 * @param p_blkstate    block_state to put longblock_fwd_pre_hdr data into
 * @return 0 on success; !0 otherwise
 * @see toblock()
 */
static int tolongblock_fwd_pre_hdr_int(struct ireq *iq,
                                       block_state_t *p_blkstate)
{
    struct longblock_fwd_pre_hdr fwd;

    if (!(iq->p_buf_in = longblock_fwd_pre_hdr_get(&fwd, iq->p_buf_in,
                                                   iq->p_buf_in_end))) {
        if (iq->debug)
            reqprintf(iq, "BAD LONG BLOCK FWD");
        return ERR_BADREQ;
    }

    iq->errstrused = fwd.flags & BLKF_ERRSTAT;
    p_blkstate->source_host = NULL;

    return RC_OK;
}

static int process_long_trn(struct ireq *iq, longblk_trans_type *blklong_trans, struct longblock_req_hdr *hdr,
                            block_state_t *blkstate)
{
    int in_dataszw = 0;
    int ii = 0;
    int rc = 0;
    int check_auxdb = 0;
    int gotsequence = 0;
    int gotsequence2 = 0;
    fstblkseq_t sequence;
    uuid_t blkseq_uuid;

    if (blklong_trans == NULL) {
        if (!hdr->docommit) {
            return RC_TRAN_CLIENT_RETRY;
        }
        check_auxdb = 1;
    }
    /* if expected piece lower than sent one, something arrived out of
     * sequence, so we need to replay the transaction */
    else {
        /* allocator memory from blobmem first before Pthread_mutex_lock
           otherwise there might be deadlocks */
        in_dataszw = hdr->offset - (((REQ_HDR_LEN + LONGBLOCK_REQ_PRE_HDR_LEN + LONGBLOCK_REQ_HDR_LEN) + 3) / 4);

        if (blobmem == NULL)
            blklong_trans->trn_data =
                realloc(blklong_trans->trn_data, (blklong_trans->datasz + in_dataszw) * sizeof(int));
        else {
            blklong_trans->trn_data =
                comdb2_brealloc(blobmem, blklong_trans->trn_data, (blklong_trans->datasz + in_dataszw) * sizeof(int));
        }

        Pthread_mutex_lock(&iq->dbenv->long_trn_mtx);
        if (blklong_trans->expseg < hdr->curpiece) {
            hash_del(iq->dbenv->long_trn_table, blklong_trans);
            Pthread_mutex_unlock(&iq->dbenv->long_trn_mtx);
            free(blklong_trans->trn_data);
            free(blklong_trans);
            blklong_trans = NULL;
            if (!hdr->docommit) {
                return RC_TRAN_CLIENT_RETRY; /* replay the transaction */
            }
            check_auxdb = 1;
        } else if (blklong_trans->expseg > hdr->curpiece) {
            struct longblock_rsp rsp;

            /* starting writes, no more reads */
            iq->p_buf_in = NULL;
            iq->p_buf_in_end = NULL;

            /* we already received that piece...just drop the packet */
            memcpy(rsp.trnid, &blklong_trans->tranid, sizeof(int) * 2);
            Pthread_mutex_unlock(&iq->dbenv->long_trn_mtx);
            rsp.rc = 0;
            memset(rsp.reserved, 0, sizeof(rsp.reserved));

            if (!(iq->p_buf_out = longblock_rsp_put(&rsp, iq->p_buf_out, iq->p_buf_out_end)))
                return ERR_INTERNAL;

            return 0;
        }
    }

    /* loop to adjust all new offsets to account for already existing
     * ones, this is a pre-loop, we want to jump back to the begining after
     * it's done */
    for (ii = 0; ii < hdr->num_reqs; ++ii, block_state_next(iq, blkstate)) {
        struct packedreq_hdr op_hdr;
        uint8_t *p_buf_op_start;

        p_buf_op_start = (uint8_t *)iq->p_buf_in;
        if (!(iq->p_buf_in = packedreq_hdr_get(&op_hdr, iq->p_buf_in, blkstate->p_buf_req_end, 0)) ||
            block_state_set_next(iq, blkstate, op_hdr.nxt))
            break;

        /* IF BLKLONG_TRANS IS NULL, IT MEANS WE'RE SEARCHING FOR FASTBLOCK
           AND SIMPLY NEED TO GET SEQUENCE NUMBER FROM THIS BUFFER..DON'T
           CARE IF THE POINTERS GET UPDATED IN THIS CASE */
        if (blklong_trans != NULL) {
            op_hdr.nxt +=
                (blklong_trans->datasz - (((REQ_HDR_LEN + LONGBLOCK_REQ_PRE_HDR_LEN + LONGBLOCK_REQ_HDR_LEN) + 3) / 4));
            if (packedreq_hdr_put(&op_hdr, p_buf_op_start, iq->p_buf_in) != iq->p_buf_in) {
                /* we are still locked here UNLESS
                 * hdr.docommit&&check_auxdb */
                if (!hdr->docommit || !check_auxdb)
                    Pthread_mutex_unlock(&iq->dbenv->long_trn_mtx);
                return ERR_INTERNAL;
            }
        }
        switch (op_hdr.opcode) {
        case BLOCK_SEQ: {
            struct packedreq_seq seq;

            if (gotsequence || !hdr->docommit) {
                logmsg(LOGMSG_ERROR,
                       "%s: longblock err gotsequence=%d "
                       "docommit=%d\n",
                       __func__, gotsequence, hdr->docommit);
                /* we are still locked here UNLESS
                 * hdr.docommit&&check_auxdb */
                if (!hdr->docommit || !check_auxdb)
                    Pthread_mutex_unlock(&iq->dbenv->long_trn_mtx);
                return ERR_INTERNAL;
            }
            gotsequence = 1;
            if (!(iq->p_buf_in = packedreq_seq_get(&seq, iq->p_buf_in, blkstate->p_buf_next_start))) {
                /* we are still locked here UNLESS
                 * hdr.docommit&&check_auxdb */
                if (!hdr->docommit || !check_auxdb)
                    Pthread_mutex_unlock(&iq->dbenv->long_trn_mtx);
                return ERR_INTERNAL;
            }

            /* reorder the sequence number */
            sequence.int3[0] = seq.seq3;
            sequence.int3[1] = seq.seq1;
            sequence.int3[2] = seq.seq2;
            break;
        }
        case BLOCK2_SEQV2: {
            struct packedreq_seq2 seq;
            gotsequence2 = 1;

            if (!(iq->p_buf_in = packedreq_seq2_get(&seq, iq->p_buf_in, blkstate->p_buf_next_start))) {
                if (!hdr->docommit || !check_auxdb)
                    Pthread_mutex_unlock(&iq->dbenv->long_trn_mtx);
                return ERR_INTERNAL;
            }
            comdb2uuidcpy(blkseq_uuid, seq.seq);
            break;
        }

        default:
            break;
        }
        if (blklong_trans != NULL)
            blklong_trans->numreqs++;
    }
    /* unknown transaction..check auxilary db for incoming SEQ # */
    if (check_auxdb && hdr->docommit) {
        /* long_trn_mutex lock must already be released at this point */
        if (gotsequence)
            return do_replay_case(iq, &sequence, sizeof(sequence), hdr->tot_reqs, 1, NULL, 0, __LINE__);
        else if (gotsequence2)
            return do_replay_case(iq, blkseq_uuid, sizeof(uuid_t), hdr->tot_reqs, 1, NULL, 0, __LINE__);
        else
            return RC_TRAN_CLIENT_RETRY;
    }
    /* we should still be locked down here */
    /* just in case...this is a check that should never happen */
    if (blklong_trans == NULL) {
        logmsg(LOGMSG_ERROR, "%s: blklong_trans==NULL unexpectedly!!\n", __func__);
        Pthread_mutex_unlock(&iq->dbenv->long_trn_mtx);
        return ERR_INTERNAL;
    }

    /* allocation is done before grabbing long_trn_mtx */
    if (blklong_trans->trn_data == NULL) {
        hash_del(iq->dbenv->long_trn_table, blklong_trans);
        Pthread_mutex_unlock(&iq->dbenv->long_trn_mtx);
        free(blklong_trans->trn_data);
        free(blklong_trans);
        return ERR_INTERNAL;
    }
    memcpy((char *)blklong_trans->trn_data + (blklong_trans->datasz) * sizeof(int),
           blkstate->p_buf_req_start + (LONGBLOCK_REQ_PRE_HDR_LEN + LONGBLOCK_REQ_HDR_LEN), in_dataszw * sizeof(int));
    blklong_trans->datasz += in_dataszw;
    blklong_trans->numsegs++;
    blklong_trans->expseg++;

    if (!hdr->docommit) {
        struct longblock_rsp rsp;

        /* starting writes, no more reads */
        iq->p_buf_in = NULL;
        iq->p_buf_in_end = NULL;

        /* we already received that piece...just drop the packet */
        memcpy(rsp.trnid, &blklong_trans->tranid, sizeof(int) * 2);
        Pthread_mutex_unlock(&iq->dbenv->long_trn_mtx);
        rsp.rc = 0;
        memset(rsp.reserved, 0, sizeof(rsp.reserved));

        if (!(iq->p_buf_out = longblock_rsp_put(&rsp, iq->p_buf_out, iq->p_buf_out_end)))
            return ERR_INTERNAL;
    } else {
        /* do not free data until execution is done */
        uint8_t *p_rawdata = (uint8_t *)blklong_trans->trn_data;
        int trnsegs = blklong_trans->numsegs;
        int retries = 0;

        int totpen = 0;
        int penaltyinc;
        double gbl_penaltyincpercent_d;

        blkstate->p_buf_req_start = p_rawdata + REQ_HDR_LEN;
        blkstate->p_buf_req_end = p_rawdata + (blklong_trans->datasz * sizeof(int));
        blkstate->numreq = blklong_trans->numreqs;

        MEMORY_SYNC;

        hash_del(iq->dbenv->long_trn_table, blklong_trans);
        Pthread_mutex_unlock(&iq->dbenv->long_trn_mtx);
        free(blklong_trans);

        totpen = 0;

        gbl_penaltyincpercent_d = (double)gbl_penaltyincpercent * .01;

    retrylong:
        rc = toblock_outer(iq, blkstate);
        if (rc == RC_INTERNAL_RETRY) {
            iq->retries++;
            if (++retries < gbl_maxretries) {
                if (!bdb_attr_get(thedb->bdb_attr, BDB_ATTR_DISABLE_WRITER_PENALTY_DEADLOCK)) {
                    Pthread_mutex_lock(&delay_lock);

                    penaltyinc =
                        (double)(gbl_maxwthreads - gbl_maxwthreadpenalty) * (gbl_penaltyincpercent_d / iq->retries);

                    if (penaltyinc <= 0) {
                        /* at least one less writer */
                        penaltyinc = 1;
                    }

                    if (penaltyinc + gbl_maxwthreadpenalty > gbl_maxthreads)
                        penaltyinc = gbl_maxthreads - gbl_maxwthreadpenalty;

                    gbl_maxwthreadpenalty += penaltyinc;
                    totpen += penaltyinc;

                    Pthread_mutex_unlock(&delay_lock);
                }

                n_retries++;
                poll(0, 0, (rand() % 25 + 1));
                goto retrylong;
            }

            logmsg(LOGMSG_ERROR, "*ERROR* [%d] tolongblock too much contention %d\n", __LINE__, retries);
            thd_dump();
        }

        /* we need this in rare case when the request is retried
           500 times; this is happening due to other bugs usually
           this ensures no requests replays will be left stuck
           papers around other short returns in toblock jic
         */
        osql_blkseq_unregister(iq);

        Pthread_mutex_lock(&delay_lock);

        gbl_maxwthreadpenalty -= totpen;

        Pthread_mutex_unlock(&delay_lock);

        block_state_free(blkstate);

        free(p_rawdata);
        iq->dbenv->long_trn_stats.ltrn_fulltrans++;
        if (iq->dbenv->long_trn_stats.ltrn_maxnseg < trnsegs)
            iq->dbenv->long_trn_stats.ltrn_maxnseg = trnsegs;
        if ((iq->dbenv->long_trn_stats.ltrn_minnseg > trnsegs) || iq->dbenv->long_trn_stats.ltrn_minnseg == 0)
            iq->dbenv->long_trn_stats.ltrn_minnseg = trnsegs;
        if (iq->dbenv->long_trn_stats.ltrn_fulltrans == 1)
            iq->dbenv->long_trn_stats.ltrn_avgnseg = trnsegs;
        else {
            iq->dbenv->long_trn_stats.ltrn_avgnseg =
                ((iq->dbenv->long_trn_stats.ltrn_avgnseg * (iq->dbenv->long_trn_stats.ltrn_fulltrans - 1) + trnsegs) /
                 iq->dbenv->long_trn_stats.ltrn_fulltrans);
        }

        if (iq->dbenv->long_trn_stats.ltrn_maxsize < blkstate->p_buf_req_end - blkstate->p_buf_req_end)
            iq->dbenv->long_trn_stats.ltrn_maxsize = blkstate->p_buf_req_end - blkstate->p_buf_req_end;
        if ((iq->dbenv->long_trn_stats.ltrn_minsize > blkstate->p_buf_req_end - blkstate->p_buf_req_end) ||
            iq->dbenv->long_trn_stats.ltrn_minsize == 0)
            iq->dbenv->long_trn_stats.ltrn_minsize = blkstate->p_buf_req_end - blkstate->p_buf_req_end;
        if (iq->dbenv->long_trn_stats.ltrn_fulltrans == 1)
            iq->dbenv->long_trn_stats.ltrn_avgsize = blkstate->p_buf_req_end - blkstate->p_buf_req_end;
        else {
            iq->dbenv->long_trn_stats.ltrn_avgsize =
                (((long)iq->dbenv->long_trn_stats.ltrn_avgsize * (iq->dbenv->long_trn_stats.ltrn_fulltrans - 1) +
                  (blkstate->p_buf_req_end - blkstate->p_buf_req_end)) /
                 iq->dbenv->long_trn_stats.ltrn_fulltrans);
        }

        if (iq->dbenv->long_trn_stats.ltrn_maxnreq < blkstate->numreq)
            iq->dbenv->long_trn_stats.ltrn_maxnreq = blkstate->numreq;
        if ((iq->dbenv->long_trn_stats.ltrn_minnreq > blkstate->numreq) || iq->dbenv->long_trn_stats.ltrn_minnreq == 0)
            iq->dbenv->long_trn_stats.ltrn_minnreq = blkstate->numreq;
        if (iq->dbenv->long_trn_stats.ltrn_fulltrans == 1)
            iq->dbenv->long_trn_stats.ltrn_avgnreq = blkstate->numreq;
        else {
            iq->dbenv->long_trn_stats.ltrn_avgnreq =
                (((long)iq->dbenv->long_trn_stats.ltrn_avgnreq * (iq->dbenv->long_trn_stats.ltrn_fulltrans - 1) +
                  blkstate->numreq) /
                 iq->dbenv->long_trn_stats.ltrn_fulltrans);
        }

        return rc;
    }
    return 0;
}

/* this is used in bb-plugins */
int tolongblock(struct ireq *iq)
{
    unsigned long long tranid = 0LL;
    int rc = 0;
    longblk_trans_type *blklong_trans = NULL;
    block_state_t blkstate;
    struct longblock_req_hdr hdr;

    /* fill blkstate's common fields */
    blkstate.p_buf_req_start = iq->p_buf_in;
    blkstate.p_buf_req_end = NULL;
    blkstate.p_buf_next_start = NULL;

    blkstate.p_buf_saved = NULL;
    blkstate.p_buf_saved_start = NULL;
    blkstate.p_buf_saved_end = NULL;

    blkstate.ct_id_key = 0LL;
    blkstate.pfk_bitmap = NULL;
    blkstate.coordinated = 0;
    blkstate.modnum = 0;

    blkstate.longblock_single = 0;

    iq->errstrused = 0;

    /* fill blkstate's type specific fields */
    switch (iq->opcode) {
    case OP_FWD_LBLOCK: /*forwarded block op*/
        rc = tolongblock_fwd_pre_hdr_int(iq, &blkstate);
        break;

    case OP_LONGBLOCK:
        rc = tolongblock_req_pre_hdr_int(iq, &blkstate);
        break;

    default:
        return ERR_BADREQ;
    }
    if (rc)
        return rc;

    if (!(iq->p_buf_in =
              longblock_req_hdr_get(&hdr, iq->p_buf_in, iq->p_buf_in_end))) {
        if (iq->debug)
            reqprintf(iq, "BAD LONG BLOCK HDR");
        return ERR_BADREQ;
    }

    if ((rc = block_state_set_end_long(iq, &blkstate, hdr.offset)))
        return rc;

    blkstate.numreq = hdr.num_reqs;

    /* TODO why is this here? */
    MEMORY_SYNC;

    /* validate number of reqs */
    if (blkstate.numreq < 1 || blkstate.numreq > gbl_maxblockops) {
        if (iq->debug)
            reqprintf(iq, "BAD NUM REQS %d", blkstate.numreq);
        reqerrstr(iq, COMDB2_BLK_RC_NB_REQS, "bad number of requests %d",
                  blkstate.numreq);
        return ERR_BADREQ;
    }

    /* I MUST BE MASTER. FORWARD IF NOT */
    if (!gbl_local_mode) {
        char *mstr = iq->dbenv->master;

        if (mstr != gbl_myhostname) {
            if (iq->is_socketrequest) {
                return ERR_REJECTED;
            }
            /* need to be master for this, so send it to the master */
            return forward_longblock_to_master(iq, &blkstate, mstr);
        }
    }

    memcpy(&tranid, hdr.trnid, sizeof(int) * 2);
    if (!iq->is_socketrequest && iq->is_fromsocket && tranid != 0) {
        /* if coming from socket, remove transaction entry from table, if it
         * exists.  we dont need any pieces since we have full buffer coming
         * in anyway */
        Pthread_mutex_lock(&iq->dbenv->long_trn_mtx);
        blklong_trans = hash_find(iq->dbenv->long_trn_table, &tranid);

        if (blklong_trans == NULL) {
            Pthread_mutex_unlock(&iq->dbenv->long_trn_mtx);
        } else {
            hash_del(iq->dbenv->long_trn_table, blklong_trans);
            Pthread_mutex_unlock(&iq->dbenv->long_trn_mtx);
            free(blklong_trans->trn_data);
            free(blklong_trans);
            blklong_trans = NULL;
        }
        tranid = 0;
    }
    if (tranid == 0LL && !hdr.docommit) {
        struct longblock_rsp rsp;

        get_context(iq, &tranid);
        if (hdr.curpiece != 1) {
            return ERR_BADREQ;
        }
        blklong_trans = malloc(sizeof(longblk_trans_type));
        if (blklong_trans == NULL) {
            return ERR_INTERNAL;
        }
        bzero(blklong_trans, sizeof(longblk_trans_type));
        blklong_trans->tranid = tranid;
        blklong_trans->timestamp = time(NULL);
        blklong_trans->datasz = hdr.offset;
        blklong_trans->numsegs = hdr.curpiece;
        blklong_trans->expseg = hdr.curpiece + 1;
        blklong_trans->numreqs = hdr.num_reqs;
        if (blobmem != NULL)
            blklong_trans->trn_data =
                comdb2_bmalloc(blobmem, (blklong_trans->datasz) * sizeof(int));
        else
            blklong_trans->trn_data =
                malloc((blklong_trans->datasz) * sizeof(int));

        blklong_trans->first_scsmsk_offset = -1;

        if (blklong_trans->trn_data == NULL) {
            free(blklong_trans);
            return ERR_INTERNAL;
        }
        memcpy(blklong_trans->trn_data, iq->p_buf_out_start,
               (blklong_trans->datasz) * sizeof(int));

        rc = add_new_transaction_entry(iq->dbenv, blklong_trans);
        if (rc != 0) {
            free(blklong_trans->trn_data);
            free(blklong_trans);
            return rc;
        }

        /* starting writes, no more reads */
        iq->p_buf_in = NULL;
        iq->p_buf_in_end = NULL;

        /*fprintf(stderr, "calling tolongbloc add trn %llu %d %d %d %d %d %d\n",
         * blklong_trans->tranid,blklong_trans->timestamp,blklong_trans->datasz,
         * blklong_trans->numreqs,blklong_trans->numsegs,blklong_trans->expseg,
         * sizeof(bf->lrsp));*/
        memcpy(rsp.trnid, &tranid, sizeof(int) * 2);

        rsp.rc = 0;
        memset(rsp.reserved, 0, sizeof(rsp.reserved));
        if (!(iq->p_buf_out =
                  longblock_rsp_put(&rsp, iq->p_buf_out, iq->p_buf_out_end)))
            return ERR_INTERNAL;

        iq->dbenv->long_trn_stats.ltrn_fulltrans++;

/* run through block, dump it */
#if 0
        {
            int in_dataszw=0, ii=0, curoff=0, lastoff=0, maxoff=0;
            union packedreq * packedreq=NULL;
            curoff=(sizeof(hdr)+3)/4+1;
            lastoff=curoff-1;
            maxoff=hdr.offset;
            for (ii = 0; ii < hdr.num_reqs; ii++)
            {
                if (curoff <= lastoff || curoff + sizeof(packedreq->hdr) / 4
                        > maxoff)
                    break;
                packedreq=(union packedreq*)&(((int*)(iq->rq))[curoff-1]);
                lastoff=curoff;
                curoff=packedreq->hdr.nxt;
                printf("(first) op %d nxt %d op %d %s\n", ii, 
                        packedreq->hdr.nxt, packedreq->hdr.opcode, 
                        breq2a(packedreq->hdr.opcode)); 
            }
        }
#endif
    } else if (tranid != 0LL) {
        memcpy(&tranid, hdr.trnid, sizeof(int) * 2);

        Pthread_mutex_lock(&iq->dbenv->long_trn_mtx);
        blklong_trans = hash_find(iq->dbenv->long_trn_table, &tranid);
        /* mark the in-use flag to prevent the transaction
           from being purged by purge_expired_long_transactions() */
        if (blklong_trans != NULL)
            blklong_trans->inuse = 1;
        Pthread_mutex_unlock(&iq->dbenv->long_trn_mtx);

        int ltrnrc = process_long_trn(iq, blklong_trans, &hdr, &blkstate);

        Pthread_mutex_lock(&iq->dbenv->long_trn_mtx);
        /* If we still find the transaction, mark back its in-use flag so
           this can be safely timed out by purge_expired_long_transactions(). */
        blklong_trans = hash_find(iq->dbenv->long_trn_table, &tranid);
        if (blklong_trans != NULL)
            blklong_trans->inuse = 0;
        Pthread_mutex_unlock(&iq->dbenv->long_trn_mtx);
        return ltrnrc;
    } else {
        /*fprintf(stderr, "calling tolongbloc one trn\n");*/
        int retries = 0;
        int totpen = 0;
        int penaltyinc;
        double gbl_penaltyincpercent_d;

        totpen = 0;

        gbl_penaltyincpercent_d = (double)gbl_penaltyincpercent * .01;

        penaltyinc = (double)gbl_maxwthreads * gbl_penaltyincpercent_d;

    /* run request */
    retrysingle:
        blkstate.longblock_single = 1;
        rc = toblock_outer(iq, &blkstate);
        if (rc == RC_INTERNAL_RETRY) {
            iq->retries++;
            if (++retries < gbl_maxretries) {
                /*fprintf(stderr, "setting gbl_maxwthreadpenalty\n");*/
                if (!bdb_attr_get(thedb->bdb_attr,
                                  BDB_ATTR_DISABLE_WRITER_PENALTY_DEADLOCK)) {
                    Pthread_mutex_lock(&delay_lock);

                    gbl_maxwthreadpenalty += penaltyinc;
                    totpen += penaltyinc;

                    Pthread_mutex_unlock(&delay_lock);
                }

                n_retries++;
                poll(0, 0, (rand() % 25 + 1));
                goto retrysingle;
            } else {
                logmsg(LOGMSG_ERROR, 
                        "*ERROR* [%d] tolongblock too much contention %d\n",
                        __LINE__, retries);
                thd_dump();
            }
        }

        Pthread_mutex_lock(&delay_lock);

        gbl_maxwthreadpenalty -= totpen;

        Pthread_mutex_unlock(&delay_lock);

        block_state_free(&blkstate);

        /* some stats */
        iq->dbenv->long_trn_stats.ltrn_fulltrans++;
        if (iq->dbenv->long_trn_stats.ltrn_maxnseg < 1)
            iq->dbenv->long_trn_stats.ltrn_maxnseg = 1;
        if (iq->dbenv->long_trn_stats.ltrn_minnseg > 1 ||
            iq->dbenv->long_trn_stats.ltrn_minnseg == 0)
            iq->dbenv->long_trn_stats.ltrn_minnseg = 1;
        if (iq->dbenv->long_trn_stats.ltrn_fulltrans == 1)
            iq->dbenv->long_trn_stats.ltrn_avgnseg = 1;
        else {
            iq->dbenv->long_trn_stats.ltrn_avgnseg =
                ((iq->dbenv->long_trn_stats.ltrn_avgnseg *
                      (iq->dbenv->long_trn_stats.ltrn_fulltrans - 1) +
                  1) /
                 iq->dbenv->long_trn_stats.ltrn_fulltrans);
        }

        if (iq->dbenv->long_trn_stats.ltrn_maxsize <
            blkstate.p_buf_req_end - blkstate.p_buf_req_end)
            iq->dbenv->long_trn_stats.ltrn_maxnseg =
                blkstate.p_buf_req_end - blkstate.p_buf_req_end;
        if ((iq->dbenv->long_trn_stats.ltrn_minsize >
             blkstate.p_buf_req_end - blkstate.p_buf_req_end) ||
            iq->dbenv->long_trn_stats.ltrn_minsize == 0)
            iq->dbenv->long_trn_stats.ltrn_minsize =
                blkstate.p_buf_req_end - blkstate.p_buf_req_end;
        if (iq->dbenv->long_trn_stats.ltrn_fulltrans == 1)
            iq->dbenv->long_trn_stats.ltrn_avgsize =
                blkstate.p_buf_req_end - blkstate.p_buf_req_end;
        else {
            iq->dbenv->long_trn_stats.ltrn_avgsize =
                ((iq->dbenv->long_trn_stats.ltrn_avgsize *
                      (iq->dbenv->long_trn_stats.ltrn_fulltrans - 1) +
                  (blkstate.p_buf_req_end - blkstate.p_buf_req_end)) /
                 iq->dbenv->long_trn_stats.ltrn_fulltrans);
        }

        if (iq->dbenv->long_trn_stats.ltrn_maxnreq < blkstate.numreq)
            iq->dbenv->long_trn_stats.ltrn_maxnreq = blkstate.numreq;
        if ((iq->dbenv->long_trn_stats.ltrn_minnreq > blkstate.numreq) ||
            iq->dbenv->long_trn_stats.ltrn_minnreq == 0)
            iq->dbenv->long_trn_stats.ltrn_minnreq = blkstate.numreq;
        if (iq->dbenv->long_trn_stats.ltrn_fulltrans == 1)
            iq->dbenv->long_trn_stats.ltrn_avgnreq = blkstate.numreq;
        else {
            iq->dbenv->long_trn_stats.ltrn_avgnreq =
                ((iq->dbenv->long_trn_stats.ltrn_avgnreq *
                      (iq->dbenv->long_trn_stats.ltrn_fulltrans - 1) +
                  blkstate.numreq) /
                 iq->dbenv->long_trn_stats.ltrn_fulltrans);
        }

        return rc;
    }
    return 0;
}

/**
 * Unpack a block_req from iq's in buffer, and put it's data into p_blkstate.
 * Meant to be called by toblock().
 * @param iq    pointer to ireq to read block_req from
 * @param p_blkstate    block_state to put block_req data into
 * @return 0 on success; !0 otherwise
 * @see toblock()
 */
static int toblock_req_int(struct ireq *iq, block_state_t *p_blkstate)
{
    int rc;
    struct block_req req;

    if (!(iq->p_buf_in = block_req_get(&req, iq->p_buf_in, iq->p_buf_in_end, iq->comdbg_flags))) {
        if (iq->debug)
            reqprintf(iq, "BAD BLOCK REQ");
        return ERR_BADREQ;
    }

    iq->errstrused = req.flags & BLKF_ERRSTAT;

    if ((rc = block_state_set_end(iq, p_blkstate, req.offset)))
        return rc;

    p_blkstate->numreq = req.num_reqs;
    p_blkstate->source_host = NULL;

    return RC_OK;
}

/**
 * Unpack a block_fwd from iq's in buffer, and put it's data into p_blkstate.
 * Meant to be called by toblock().
 * @param iq    ireq to read block_fwd from
 * @param p_blkstate    block_state to put block_fwd data into
 * @return 0 on success; !0 otherwise
 * @see toblock()
 */
static int toblock_fwd_int(struct ireq *iq, block_state_t *p_blkstate, int comdbg_flags)
{
    int rc;
    struct block_fwd fwd;

    if (!(iq->p_buf_in = block_fwd_get(&fwd, iq->p_buf_in, iq->p_buf_in_end, comdbg_flags))) {
        if (iq->debug)
            reqprintf(iq, "BAD BLOCK FWD");
        return ERR_BADREQ;
    }

    if (iq->opcode == OP_FWD_BLOCK_LE) {
        /* everything else is identical except the block inside the envelope
         * came from a little endian client */
        iq->comdbg_flags = COMDBG_FLAG_FROM_LE | OP_FWD_BLOCK;
    }

    iq->errstrused = fwd.flags & BLKF_ERRSTAT;
    if ((rc = block_state_set_end(iq, p_blkstate, fwd.offset)))
        return rc;

    p_blkstate->numreq = fwd.num_reqs;

    p_blkstate->source_host = iq->frommach;

    return RC_OK;
}

int to_sorese_init(struct ireq *iq)
{
    return -1;
}

int to_sorese(struct ireq *iq)
{
    int rc = 0;
    int gotlk = 0;

    /* check that I am the master */
    if (!gbl_local_mode) {
        char *mstr = iq->dbenv->master;

        if (mstr != gbl_myhostname) {
            /* Ask the replicant to retry against the new master. */
            if (iq->sorese) {
                iq->sorese->rcout = ERR_NOMASTER;
            }
            return ERR_REJECTED;
        }
    }

    iq->jsph = javasp_trans_start(iq->debug);

    if (gbl_exclusive_blockop_qconsume) {
        Pthread_rwlock_rdlock(&gbl_block_qconsume_lock);
        gotlk = 1;
    }

    bdb_stripe_get(iq->dbenv->bdb_env);

    /* TODO
    rc = to_sorese_main(iq->jsph, iq); */
    rc = -1;

    bdb_stripe_done(iq->dbenv->bdb_env);

    if (gotlk)
        Pthread_rwlock_unlock(&gbl_block_qconsume_lock);

    javasp_trans_end(iq->jsph);

    return rc;
}

int toblock(struct ireq *iq)
{
    int rc = 0;
    block_state_t blkstate;

    /* fill blkstate's common fields */
    blkstate.p_buf_req_start = iq->p_buf_in;
    blkstate.p_buf_req_end = NULL;
    blkstate.p_buf_next_start = NULL;

    blkstate.p_buf_saved = NULL;
    blkstate.p_buf_saved_start = NULL;
    blkstate.p_buf_saved_end = NULL;

    blkstate.ct_id_key = 0LL;
    blkstate.pfk_bitmap = NULL;
    blkstate.coordinated = 0;
    blkstate.modnum = 0;

    /* fill blkstate's type specific fields */
    switch (iq->opcode) {
    case OP_BLOCK:
        rc = toblock_req_int(iq, &blkstate);
        break;

    case OP_FWD_BLOCK: /*forwarded block op*/
    case OP_FWD_BLOCK_LE:
        rc = toblock_fwd_int(iq, &blkstate, iq->comdbg_flags);
        break;

    default:
        return ERR_BADREQ;
    }
    if (rc)
        return rc;

    /* TODO why is this here? */
    MEMORY_SYNC;

    if (iq->debug)
        reqprintf(iq, "BLOCK NUM REQS %d", blkstate.numreq);

    /* validate number of reqs */
    if (blkstate.numreq < 1 || blkstate.numreq > MAXBLOCKOPS) {
        if (iq->debug)
            reqprintf(iq, "BAD NUM REQS %d", blkstate.numreq);
        reqerrstr(iq, COMDB2_BLK_RC_NB_REQS, "bad number of requests %d",
                  blkstate.numreq);
        return ERR_BADREQ;
    }

    /* I MUST BE MASTER. FORWARD IF NOT */
    if (!gbl_local_mode) {
        char *mstr = iq->dbenv->master;

        if (mstr != gbl_myhostname) {
            if (iq->sorese) {
                /* Ask the replicant to retry against the new master. */
                iq->sorese->rcout = ERR_NOMASTER;
                return ERR_REJECTED;
            }
            if (iq->is_socketrequest &&
                (!bdb_am_i_coherent(iq->dbenv->bdb_env) || !iq->request_data)) {
                return ERR_REJECTED;
            }

            /* need to be master for this, so send it to the master */
            return forward_block_to_master(iq, &blkstate, mstr);
        }
    }

    rc = toblock_outer(iq, &blkstate);

    block_state_free(&blkstate);

    return rc;
}

static int create_child_transaction(struct ireq *iq, tran_type *parent_trans,
                                    tran_type **trans)
{
    int irc = 0;
    if (gbl_enable_berkdb_retry_deadlock_bias) {
        irc = trans_start_set_retries(iq, parent_trans, trans, iq->retries, iq->retries);
    } else if (bdb_attr_get(thedb->bdb_attr, BDB_ATTR_DEADLOCK_YOUNGEST_EVER) ||
               bdb_attr_get(thedb->bdb_attr,
                            BDB_ATTR_DEADLOCK_LEAST_WRITES_EVER)) {
        if (iq->priority == 0) {
            if (bdb_attr_get(thedb->bdb_attr, BDB_ATTR_DEADLOCK_YOUNGEST_EVER))
                iq->priority = comdb2_time_epochms();
        }

        if (verbose_deadlocks)
            logmsg(LOGMSG_DEBUG, "%" PRIxPTR "%s:%d Using iq %p priority %d\n",
                   (intptr_t)pthread_self(), __FILE__, __LINE__, iq, iq->priority);
        irc = trans_start_set_retries(iq, parent_trans, trans, iq->retries, iq->priority);
    } else {
        irc = trans_start_set_retries(iq, parent_trans, trans, iq->retries, 0);
    }
    return irc;
}


static int
osql_create_transaction(struct javasp_trans_state *javasp_trans_handle,
                        struct ireq *iq, tran_type **trans,
                        tran_type **parent_trans, int *osql_needtransaction,
                        int line)
{
    int rc = 0;
    int irc = 0;

    if (iq->tranddl) {
        irc = trans_start_logical_sc(iq, &(iq->sc_logical_tran));
        if (gbl_rowlocks && irc == 0) { // rowlock
            tran_type *sc_parent = NULL;
            if (parent_trans)
                *parent_trans = NULL;
            *trans = iq->sc_logical_tran;
            sc_parent = bdb_get_sc_parent_tran(iq->sc_logical_tran);
            iq->sc_logical_tran = NULL; // use trans in rowlocks
            if (sc_parent == NULL) {
                irc = -1;
                logmsg(LOGMSG_ERROR, "%s:%d/%d td %p failed to get physical tran\n", __func__, __LINE__, line,
                       (void *)pthread_self());
            } else {
                irc = trans_start_sc(iq, sc_parent, &(iq->sc_tran));
                if (irc == 0 && gbl_sc_close_txn)
                    irc = trans_start_sc(iq, sc_parent, &(iq->sc_close_tran));
            }
        } else if (irc == 0) { // pagelock
            if (parent_trans) {
                *parent_trans = bdb_get_physical_tran(iq->sc_logical_tran);
                /* start child tran */
                irc = create_child_transaction(iq, *parent_trans, trans);
                if (irc == 0) {
                    /* start another child tran for schema changes */
                    irc = create_child_transaction(iq, *parent_trans,
                                                   &(iq->sc_tran));
                    /* Another for close-old files */
                    if (irc == 0 && gbl_sc_close_txn)
                        irc = create_child_transaction(iq, *parent_trans,
                                                       &(iq->sc_close_tran));
                }
            } else {
                *trans = bdb_get_physical_tran(iq->sc_logical_tran);
                irc = create_child_transaction(iq, *trans, &(iq->sc_tran));
                if (irc == 0 && gbl_sc_close_txn)
                    irc = create_child_transaction(iq, *trans,
                                                   &(iq->sc_close_tran));
            }
        }
    } else if (!gbl_rowlocks) {
        /* start parent transaction */
        if (parent_trans) {
            rc = trans_start(iq, NULL, parent_trans);
            if (rc != 0) {
                return rc;
            }
        }

        /* START child TRANSACTION */
        irc = create_child_transaction(iq, parent_trans ? *parent_trans : NULL,
                                       trans);
    } else {
        irc = trans_start_logical(iq, trans);
        if (parent_trans)
            *parent_trans = NULL;
    }
    if (irc != 0) {
        logmsg(LOGMSG_ERROR, "%s:%d trans_start failed rc %d\n", __func__,
               __LINE__, irc);
        return irc;
    }

    if (iq->debug && iq->usedb) {
        /* TODO print trans twice? No parent_trans? */
        reqprintf(iq, "%p:START TRANSACTION OSQL ID %p DB %d '%s'", *trans,
                  *trans, iq->usedb->dbnum, iq->usedb->tablename);
    }

    if (parent_trans)
        javasp_trans_set_trans(javasp_trans_handle, iq, *parent_trans, *trans);
    else
        javasp_trans_set_trans(javasp_trans_handle, iq, NULL, *trans);

    if (osql_needtransaction)
        *osql_needtransaction = OSQL_BPLOG_RECREATEDTRANS;

    return rc;
}

static int osql_destroy_transaction(struct ireq *iq, tran_type **parent_trans,
                                    tran_type **trans,
                                    int *osql_needtransaction)
{
    int error = 0;
    int rc = 0;

    rc = trans_abort(iq, *trans);
    *trans = NULL;
    if (rc) {
        logmsg(LOGMSG_ERROR, "aborting transaction failed\n");
        error = 1;
    }
    if (parent_trans && *parent_trans) {
        rc = trans_abort(iq, *parent_trans);
        *parent_trans = NULL;
        if (rc) {
            logmsg(LOGMSG_ERROR, "aborting parent transaction failed\n");
            error = 1;
        }
    }

    *osql_needtransaction = OSQL_BPLOG_NOTRANS;

    return error;
}

/* This wraps toblock_main() making it easier for somethings to be created
 * and cleaned up.  Note that this is inside of the retry loop. */
static int toblock_outer(struct ireq *iq, block_state_t *blkstate)
{
    int rc;
    int gaveaway;
    int i = 0;
    block_state_t blkstate_copy;
    pthread_t my_tid;
    pthread_t working_for;
    int prefaulton;
    struct ireq newiq;

    prefaulton = (gbl_prefault_toblock_local || gbl_prefault_toblock_bcast) && prefault_check_enabled();

    if (!gbl_prefaulthelper_blockops)
        prefaulton = 0;

    gaveaway = 0;

    my_tid = pthread_self();

    if (!iq->tranddl) {
        iq->jsph = javasp_trans_start(iq->debug);
    }
    iq->blkstate = blkstate;

    /* paranoia - make sure this thing starts out initialized unless we
       get a helper thread */
    iq->blkstate->pfk_bitmap = NULL;
    iq->blkstate->opnum = 0;
    iq->helper_thread = -1;

    /* fastpath this nonsense if we arent running prefault threads */
    if (prefaulton) {
        /*
            Yes, we're copying iq to a STACK-variable to be operated on by a
            signaled thread.  This looks broken, but there's code later which
            prevents this function from continuing (and prevents the stack
           variable
            from disappearing) until the thread has completed.
        */
        memcpy(&newiq, iq, sizeof(struct ireq));
        memcpy(&blkstate_copy, blkstate, sizeof(block_state_t));
        newiq.thdinfo = NULL;
        newiq.reqlogger = NULL;

        /* try to give away this buffer to a toblock_prefault_thread*/
        Pthread_mutex_lock(&(iq->dbenv->prefault_helper.mutex));

        for (i = 0; i < iq->dbenv->prefault_helper.numthreads; i++) {
            if (iq->dbenv->prefault_helper.threads[i].working_for ==
                gbl_invalid_tid) {

                /*fprintf(stderr, "found helper %d set to invalid\n", i);*/

                /* found an idle thread to give this to! */
                gaveaway = 1;

                clear_pfk(iq->dbenv, i, blkstate->numreq);

                iq->dbenv->prefault_helper.threads[i].seqnum++;
                if (iq->dbenv->prefault_helper.threads[i].seqnum == 0)
                    iq->dbenv->prefault_helper.threads[i].seqnum = 1;

                /* he's working for me! */
                iq->dbenv->prefault_helper.threads[i].working_for = my_tid;

                /* give him some stuff to work on */
                iq->dbenv->prefault_helper.threads[i].type = PREFAULT_TOBLOCK;
                iq->dbenv->prefault_helper.threads[i].blkstate = &blkstate_copy;
                iq->dbenv->prefault_helper.threads[i].iq = &newiq;
                iq->dbenv->prefault_helper.threads[i].abort = 0;

                /* chain a pointer to our pfk_bitmap */
                iq->blkstate->pfk_bitmap =
                    iq->dbenv->prefault_helper.threads[i].pfk_bitmap;

                /* record what helper thread is helping us */
                iq->helper_thread = i;

                MEMORY_SYNC;

                /*fprintf(stderr, "waking up prefault_helper %d\n", i);*/

                Pthread_cond_signal(
                    &(iq->dbenv->prefault_helper.threads[i].cond));

                break;
            }
        }

        Pthread_mutex_unlock(&(iq->dbenv->prefault_helper.mutex));
    }

    /*sleep(1);*/
    {
        int gotlk = 0;

        if (gbl_exclusive_blockop_qconsume) {
            Pthread_rwlock_rdlock(&gbl_block_qconsume_lock);
            gotlk = 1;
        }

        bdb_stripe_get(iq->dbenv->bdb_env);

        rc = toblock_main(iq->jsph, iq, blkstate);

        bdb_stripe_done(iq->dbenv->bdb_env);

        if (gotlk)
            Pthread_rwlock_unlock(&gbl_block_qconsume_lock);
    }

    if (!gaveaway)
        iq->dbenv->prefault_stats.num_nohelpers++;

    if (gaveaway) {
        /* we have another thread working on our memory - we cant
           proceed till he is done with it.  in theory he should be
           much faster than us as it's just memory operations */

        /*fprintf(stderr, "waiting for helper %d\n", i);*/

        /* check once first with no sync */
        if (iq->dbenv->prefault_helper.threads[i].working_for == my_tid) {
            int retry_count;

            retry_count = 0;

            /*
               we have another thread working on our memory - we cant
               proceed till he is done with it.  in theory he should be
               much faster than us as it's just memory operations.
               set the abort flag and force it out.

               did he just manage to finish after we checked and then we
               aborted the next request?  possibly.  is it the end of the
               world?  no.
               */
            iq->dbenv->prefault_helper.threads[i].abort = 1;
            MEMORY_SYNC;

        again:

            working_for = iq->dbenv->prefault_helper.threads[i].working_for;

            /* if he's still working for me, try again */
            if (working_for == my_tid) {
                if (retry_count > 100) {
                    poll(0, 0, (rand() % 25 + 1));
                }
                retry_count++;

                goto again;
            }
        }

        Pthread_mutex_lock(&(iq->dbenv->prefault_helper.mutex));

        /*fprintf(stderr, "done waiting for helper %d\n", i);*/

        iq->dbenv->prefault_helper.threads[i].seqnum++;
        if (iq->dbenv->prefault_helper.threads[i].seqnum == 0)
            iq->dbenv->prefault_helper.threads[i].seqnum = 1;

        Pthread_mutex_unlock(&(iq->dbenv->prefault_helper.mutex));
    }

    javasp_trans_end(iq->jsph);
    return rc;
}

static int extract_blkseq(struct ireq *iq, block_state_t *p_blkstate,
                          int *found_blkseq, int *have_blkseq)
{
    struct packedreq_seq seq;

    if (!*found_blkseq) {
        *found_blkseq = 1;

        if (!(iq->p_buf_in = packedreq_seq_get(&seq, iq->p_buf_in,
                                               p_blkstate->p_buf_next_start))) {
            if (iq->debug)
                reqprintf(iq, "FAILED TO UNPACK");
            return ERR_BADREQ;
        }

        /* The old proxy only sent an 8 byte sequence number.
         * The new proxy sends 12 bytes, but for compatibility
         * with older comdb2 builds the first 4 bytes of what
         * is logically the sequence number are sent last.
         * Thus we have to reorder the data. */
        memcpy(iq->seq, &seq, 3 * sizeof(int));
        iq->seqlen = 3 * sizeof(int);

        if (seq.seq1 == 0 && seq.seq1 == 0 && seq.seq1 == 0) {
            /* for long transactions, this means we did not go
               through proxy */
            *have_blkseq = 0;
        } else {
            *have_blkseq = 1;

            /* backup everything we havn't already */
            if (block_state_backup(p_blkstate, p_blkstate->p_buf_req_end,
                                   iq->opcode == OP_FWD_LBLOCK ||
                                       iq->opcode == OP_LONGBLOCK))
                return ERR_INTERNAL;
        }
    }
    return 0;
}

static int extract_blkseq2(struct ireq *iq, block_state_t *p_blkstate,
                           int *found_blkseq, int *have_blkseq)
{
    struct packedreq_seq2 seq;

    if (!*found_blkseq) {
        *found_blkseq = 1;

        if (!(iq->p_buf_in = packedreq_seq2_get(
                  &seq, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
            if (iq->debug)
                reqprintf(iq, "FAILED TO UNPACK");
            return ERR_BADREQ;
        }

        memcpy(iq->seq, seq.seq, sizeof(uuid_t));
        iq->seqlen = sizeof(uuid_t);

        *have_blkseq = 1;

        /* backup everything we havn't already */
        if (block_state_backup(p_blkstate, p_blkstate->p_buf_req_end,
                               iq->opcode == OP_FWD_LBLOCK ||
                                   iq->opcode == OP_LONGBLOCK))
            return ERR_INTERNAL;
    }
    return 0;
}

static pthread_rwlock_t commit_lock = PTHREAD_RWLOCK_INITIALIZER;


void handle_postcommit_bpfunc(struct ireq *iq)
{
    bpfunc_lstnode_t *cur_bpfunc = NULL;

    while((cur_bpfunc = listc_rtl(&iq->bpfunc_lst)))
    {
        assert(cur_bpfunc->func->success != NULL);
        cur_bpfunc->func->success(NULL /*not used*/, cur_bpfunc->func,
                                  &iq->errstat);
        free_bpfunc(cur_bpfunc->func);
    }
}

void handle_postabort_bpfunc(struct ireq *iq)
{
    bpfunc_lstnode_t *cur_bpfunc = NULL;
    while((cur_bpfunc = listc_rtl(&iq->bpfunc_lst)))
    {
        assert(cur_bpfunc->func->fail != NULL);
        cur_bpfunc->func->fail(NULL /*not used*/, cur_bpfunc->func,
                               &iq->errstat);
        free_bpfunc(cur_bpfunc->func);
    }
}

static void backout_and_abort_tranddl(struct ireq *iq, tran_type *parent,
                                      int rowlocks)
{
    int rc = 0;
    if (rowlocks) {
        assert(parent);
        /* Check if we started a physical transaction. */
        if (bdb_get_physical_tran(parent) != NULL) {
            rc = trans_abort(iq, bdb_get_physical_tran(parent));
            bdb_reset_physical_tran(parent);
            if (rc != 0) {
                logmsg(LOGMSG_FATAL, "%s:%d TRANS_ABORT FAILED RC %d", __func__, __LINE__, rc);
                comdb2_die(1);
            }
        }
        parent = bdb_get_sc_parent_tran(parent);
    }
    if (iq->sc_close_tran) {
        if (iq->sc_closed_files)
            rc = trans_commit(iq, iq->sc_close_tran, gbl_myhostname);
        else
            rc = trans_abort(iq, iq->sc_close_tran);
        if (rc != 0) {
            logmsg(LOGMSG_FATAL, "%s:%d TRANS_%s FAILED RC %d\n", __func__,
                   __LINE__, iq->sc_closed_files ? "COMMIT" : "ABORT", rc);
            comdb2_die(0);
        }
        iq->sc_close_tran = NULL;
    }
    if (iq->sc_tran) {
        assert(parent);
        rc = trans_abort(iq, iq->sc_tran);
        if (rc != 0) {
            logmsg(LOGMSG_FATAL, "%s:%d TRANS_ABORT FAILED RC %d", __func__,
                   __LINE__, rc);
            comdb2_die(1);
        }
        iq->sc_tran = NULL;
        backout_schema_changes(iq, parent);
    }
    if (iq->sc_locked) {
        unlock_schema_lk();
        iq->sc_locked = 0;
    }
    if (iq->sc_logical_tran) {
        rc = trans_commit_logical(iq, iq->sc_logical_tran, gbl_myhostname, 0, 1,
                                  NULL, 0, NULL, 0);
        if (rc != 0) {
            logmsg(LOGMSG_ERROR, "%s:%d TD %p TRANS_ABORT FAILED RC %d\n", __func__, __LINE__, (void *)pthread_self(),
                   rc);
        }
    } else if (parent && !rowlocks) {
        /*
        ** We didn't start an sc logical transaction.
        ** Just abort the parent transcation here.
        */
        trans_abort(iq, parent);
    }
    iq->sc_logical_tran = NULL;
}

static int pack_up_response(struct ireq *iq, int have_keyless_requests,
                            int num_reqs, int numerrs, struct block_err *err,
                            int rc, int opnum)
{
    if (!have_keyless_requests) {
        struct block_rsp rsp;

        rsp.num_completed = opnum;

        if (!(iq->p_buf_out =
                  block_rsp_put(&rsp, iq->p_buf_out, iq->p_buf_out_end, iq->comdbg_flags)))
            return ERR_INTERNAL;

        /* rcodes */
        for (int jj = 0; jj < num_reqs; jj++) {
            int rcode;
            int comdbg_flags = iq->comdbg_flags;

            PUTFUNC

            rcode = (jj == opnum) ? rc : 0;
            if (!(iq->p_buf_out = putfunc(&rcode, sizeof(rcode), iq->p_buf_out,
                                          iq->p_buf_out_end)))
                return ERR_INTERNAL;
        }

        /* rrns */
        for (int jj = 0; jj < num_reqs; jj++) {
            int rrn;
            int comdbg_flags = iq->comdbg_flags;

            PUTFUNC

            rrn = (jj < opnum) ? 2 : 0;
            if (!(iq->p_buf_out = putfunc(&rrn, sizeof(rrn), iq->p_buf_out,
                                          iq->p_buf_out_end)))
                return ERR_INTERNAL;

#if 0
            reqmoref(iq, " a%d:%d", jj, p_blkstate->rsp.packed_rc_rrn_brc[jj]);
            reqmoref(iq, " b%d:%d", jj,
                    p_blkstate->rsp.packed_rc_rrn_brc[jj+num_reqs]);
            reqmoref(iq, " c%d:%d", jj,
                    p_blkstate->rsp.packed_rc_rrn_brc[jj + num_reqs * 2]);
#endif
        }

        /* borcodes */
        if (!(iq->p_buf_out = buf_zero_put(sizeof(int) * num_reqs,
                                           iq->p_buf_out, iq->p_buf_out_end)))
            return ERR_INTERNAL;
    } else {
        if (iq->is_block2positionmode) {
            struct block_rspkl_pos rspkl_pos;

            rspkl_pos.num_completed = opnum;

            rspkl_pos.position = iq->last_genid;

            if (numerrs)
                rspkl_pos.numerrs = 1;

            if (!(iq->p_buf_out = block_rspkl_pos_put(&rspkl_pos, iq->p_buf_out,
                                                      iq->p_buf_out_end)))
                return ERR_INTERNAL;

            if (numerrs) {
                if (!(iq->p_buf_out =
                          block_err_put(err, iq->p_buf_out, iq->p_buf_out_end)))
                    return ERR_INTERNAL;
            }

        } else {
            struct block_rspkl rspkl;

            rspkl.num_completed = opnum;

            if (numerrs)
                rspkl.numerrs = 1;

            if (!(iq->p_buf_out = block_rspkl_put(&rspkl, iq->p_buf_out,
                                                  iq->p_buf_out_end)))
                return ERR_INTERNAL;

            if (numerrs) {
                if (!(iq->p_buf_out =
                          block_err_put(err, iq->p_buf_out, iq->p_buf_out_end)))
                    return ERR_INTERNAL;
            }
        }
    }
    return 0;
}

static inline int check_for_node_up(struct ireq *iq, block_state_t *p_blkstate)
{
    /* If we get here and we are rtcpu-ed AND this is a cluster,
     * return RC_TRAN_CLIENT_RETRY. */
    if (debug_switch_reject_writes_on_rtcpu() &&
        is_node_up(gbl_myhostname) != 1) {
        const char *nodes[REPMAX];
        int nsiblings;
        nsiblings = net_get_all_nodes_connected(thedb->handle_sibling, nodes);
        if (nsiblings >= 1) {
            reqprintf(iq, "Master is swinging, repeat the request");
            /* The client code doesn't retry correctly if the request
               is (1) a non-long transaction or, (2) a long transaction of a
               single packet.  So if the block request was a single packet,
               send back a return code telling the proxy to retry.  Otherwise,
               the client code needs to retry (and handles it correctly).  The
               reason the client doesn't handle single-buffer retries correctly
               is that it doesn't make copies of these requests since the proxy
               already has it.  Thus the proxy, not the client should retry. */
            if (iq->is_socketrequest) {
                /* We don't have proxy in this case so the client which has to
                 * retry. */
                return RC_TRAN_CLIENT_RETRY;
            }
            if (iq->opcode == OP_BLOCK || p_blkstate->numreq == 1) {
                return 999; /* no dedicated code for proxy retry... */
            } else if (p_blkstate->longblock_single == 1) {
                return 999; /* no dedicated code for proxy retry... */
            } else {
                return RC_TRAN_CLIENT_RETRY;
            }
        }
    }
    return 0;
}

extern int gbl_is_physical_replicant;

static int localrep_seqno(tran_type *trans, block_state_t *p_blkstate)
{
    int rc = 0;
    /* Transactionally read the last sequence number.
       This effectively serializes all updates.
       There are probably riskier more clever schemes where we don't
       need to do this. */
    if (gbl_replicate_local_concurrent) {
        unsigned long long useqno;
        useqno = bdb_get_timestamp(thedb->bdb_env);
        memcpy(&p_blkstate->seqno, &useqno, sizeof(unsigned long long));
    } else {
        long long seqno;
        rc = get_next_seqno(trans, NULL, &seqno);
        if (rc == 0)
            p_blkstate->seqno = seqno;
    }

    return rc;
}

#define MIXED_SQL_DYNTAGS(trans, parent_trans) \
    { \
        if (!trans) { \
            if (osql_needtransaction == OSQL_BPLOG_NOTRANS) { \
                assert(is_block2sqlmode != 0); \
                logmsg(LOGMSG_ERROR, "%s:%d INCORRECT TRANSACTION MIX, SQL AND DYNTAG\n", \
                        __FILE__, __LINE__); \
                rc = osql_create_transaction( \
                        javasp_trans_handle, iq, &trans, \
                        have_blkseq ? &parent_trans : NULL, \
                        &osql_needtransaction, __LINE__); \
                if (rc) { \
                    numerrs = 1; \
                    GOTOBACKOUT; \
                } \
            } else { \
                logmsg(LOGMSG_FATAL, "%s:%d NULL TRANSACTION!\n", __FILE__, \
                            __LINE__); \
                abort(); \
            } \
        } \
    }

int gbl_random_prepare_commit = 0;
int gbl_all_prepare_commit = 0;
int gbl_all_prepare_abort = 0;
int gbl_all_prepare_leak = 0;

static inline int debug_prepare_testcase()
{
    return gbl_all_prepare_commit || gbl_all_prepare_abort || gbl_all_prepare_leak || gbl_random_prepare_commit;
}

static inline int debug_should_prepare()
{
    return gbl_all_prepare_commit || gbl_all_prepare_abort || gbl_all_prepare_leak ||
           (gbl_random_prepare_commit && rand() % 2);
}

static inline int debug_prepared_should_commit()
{
    return (!gbl_all_prepare_leak && !gbl_all_prepare_abort);
}

static inline int debug_prepared_should_abort()
{
    return gbl_all_prepare_abort;
}

static inline void debug_prepare_tests(struct ireq *iq, tran_type *parent_trans, char *source_host, void *bskey,
                                       int bskeylen)
{
    int prepared = 0, bdberr;
    char dist_txnid[64] = {0};

    if (debug_should_prepare()) {
        uint64_t g = get_genid(thedb->bdb_env, 0);
        snprintf(dist_txnid, sizeof(dist_txnid), "test-%" PRIu64, g);
        /* TODO: only allow prepares blkseq-key is a cnonce.  We can check
         * early & error out. */
        int prepare_rc = bdb_tran_prepare(thedb->bdb_env, parent_trans, dist_txnid, "test-coordinator", "test-tier",
                                          rand() % 10000, bskey, bskeylen, &bdberr);
        /* We test that prepare fails if the logs don't have utxnids */
        if (prepare_rc) {
            logmsg(LOGMSG_FATAL, "%s got weird error from prepare, %d??\n", __func__, prepare_rc);
            exit(1);
        }
        prepared = 1;
    }

    if (!prepared || debug_prepared_should_commit()) {
        trans_commit_adaptive(iq, parent_trans, source_host);
    } else if (prepared && debug_prepared_should_abort()) {
        dist_txn_abort_write_blkseq(thedb->bdb_env, bskey, bskeylen);
        trans_abort(iq, parent_trans);
    } else {
        assert(gbl_all_prepare_leak);
        logmsg(LOGMSG_USER, "%s leaking a prepared txn %s\n", __func__, dist_txnid);
        bdb_flush(thedb->bdb_env, &bdberr);
        sleep(2);
        exit(1);
    }
}

static int should_rewrite_rcode(int rcode)
{
    switch (rcode) {
    case ERR_NO_RECORDS_FOUND:
    case ERR_CONVERT_DTA:
    case ERR_NULL_CONSTRAINT:
    case ERR_SQL_PREP:
    case ERR_CONSTR:
    case ERR_UNCOMMITTABLE_TXN:
    case ERR_NOMASTER:
    case ERR_NOTSERIAL:
    case ERR_DIST_ABORT:
    case ERR_SC:
    case ERR_TRAN_TOO_BIG:
        return 0;
        break;
    }
    return 1;
}

static pthread_mutex_t blklk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t blkcd = PTHREAD_COND_INITIALIZER;
static int prepared_count = 0;
static int blkcnt = 0;
__thread int waitdie_deadlock;

void abort_disttxn(struct ireq *iq, int rc, int outrc)
{
    if (iq->sorese->is_coordinator) {
        if (gbl_debug_disttxn_trace) {
            logmsg(LOGMSG_USER, "%s DISTTXN %s coord failing disttxn rc=%d outrc=%d\n", __func__,
                   iq->sorese->dist_txnid, rc, outrc);
        }
        coordinator_failed(iq->sorese->dist_txnid);
    } else {
        assert(iq->sorese->is_participant);
        participant_has_failed(iq->sorese->dist_txnid, iq->sorese->coordinator_dbname, iq->sorese->coordinator_master,
                               rc, outrc, iq->errstat.errstr);
        if (gbl_debug_disttxn_trace) {
            logmsg(LOGMSG_USER, "%s DISTTXN %s participant failing disttxn rc=%d outrc=%d\n", __func__,
                   iq->sorese->dist_txnid, rc, outrc);
        }
    }
}

#define LOG_LEGACY_REQUEST() \
    do { \
        if (!have_legacy_requests) { \
            log_legacy_request(iq, NULL); \
            have_legacy_requests = 1; \
        } \
    } while(0)

__thread int64_t *txn_logbytes = NULL;

static int toblock_main_int(struct javasp_trans_state *javasp_trans_handle, struct ireq *iq, block_state_t *p_blkstate)
{
    int rowlocks = gbl_rowlocks;
    int fromline = -1;
    int opnum, jj, num_reqs;
    int rc = 0, ixkeylen, rrn;
    int irc = 0;
    char *source_host;
    tran_type *trans = NULL; /*transaction handle */
    tran_type *parent_trans = NULL;
    /* for updates */
    char saved_fndkey[MAXKEYLEN];
    int saved_rrn = 0;
    int addrrn;
    int outrc=-1;
    unsigned char nulls[MAXNULLBITS] = {0};
    unsigned long long genid = 0;
    int have_blkseq = 0;
    int have_keyless_requests = 0;
    int numerrs = 0;
    int check_serializability = 0;
    int force_serial_error = 0;
    struct block_err err;
    int opcode_counts[NUM_BLOCKOP_OPCODES];
    int nops = 0;
    int is_block2sqlmode = 0; /* set this for all osql modes */
    int osql_needtransaction = OSQL_BPLOG_NONE;
    int blkpos = -1, ixout = -1, errout = 0;
    int backed_out = 0;
    struct thr_handle *thr_self = thrman_self();
    int found_blkseq = 0;
    uint8_t *p_buf_rsp_start;
    struct longblock_req_hdr lhdr;
    struct block_req hdr;
    int is_mixed_sqldyn = 0;
    int have_legacy_requests = 0;
    bdb_state_type *bdb_state = thedb->bdb_env;

#if DEBUG_DID_REPLAY
    int did_replay = 0;
#endif
    /* The blob buffer.  If the tag includes blobs then we are sent the blob
     * data in separate opcodes before we are sent the add/update command.
     * After each keyless write op we clear this buffer ready for the next one.
     */
    blob_buffer_t blobs[MAXBLOBS];
    int delayed = 0;
    int hascommitlock = 0;

    int convert_flags = (iq->comdbg_flags & COMDBG_FLAG_FROM_LE) ? CONVERT_LITTLE_ENDIAN_CLIENT : 0;

    /* zero this out very high up or we can crash if we get to backout: without
     * having initialised this. */
    memset(blobs, 0, sizeof(blobs));
    memset(&err, 0, sizeof(struct block_err));

    /* default tzname per client transaction */
    bzero(iq->tzname, sizeof(iq->tzname));

    /* same for oplog counter */
    iq->oplog_numops = 0;

    num_reqs = p_blkstate->numreq;

    if (gbl_is_physical_replicant) {
        logmsg(LOGMSG_ERROR, "%s client attempting write against physical replicant\n", __func__);
        rc = ERR_NOMASTER;
        return rc;
    }

    /* reset queue hits stats so we don't accumulate them over several
     * retries */
    iq->num_queues_hit = 0;

    iq->p_buf_in = p_blkstate->p_buf_req_start;
    iq->p_buf_in_end = p_blkstate->p_buf_req_end;

    if (iq->opcode == OP_LONGBLOCK || iq->opcode == OP_FWD_LBLOCK)
        iq->p_buf_in =
            longblock_req_hdr_get(&lhdr, iq->p_buf_in, iq->p_buf_in_end) + 4;
    else if (iq->opcode == OP_BLOCK || iq->opcode == OP_FWD_BLOCK || iq->opcode == OP_FWD_BLOCK_LE)
        iq->p_buf_in = block_req_get(&hdr, iq->p_buf_in, iq->p_buf_in_end, iq->comdbg_flags);

    /* backup everything we've read already (ie the header) */
    if (block_state_backup(p_blkstate, iq->p_buf_in,
                           iq->opcode == OP_LONGBLOCK ||
                               iq->opcode == OP_FWD_LBLOCK))
        return ERR_INTERNAL;

    if (iq->opcode == OP_FWD_BLOCK || iq->opcode == OP_FWD_LBLOCK || iq->opcode == OP_FWD_BLOCK_LE) {
        /* this was forwarded from a replicant */
        source_host = p_blkstate->source_host;
    } else {
        /* this is considered local block op */
        source_host = gbl_myhostname;
    }

    addrrn = -1; /*for secafpri, remember last rrn. */

    if (iq->debug) {
        reqprintf(iq, "BLOCK OPCODES: %d reqs ", num_reqs);
    }
    if (iq->retries == 0) {
        bzero(opcode_counts, sizeof(opcode_counts));
    }

    /* If this is a sorese transaction, don't recreate transaction objects */
    if (iq->sorese)
        osql_needtransaction = OSQL_BPLOG_RECREATEDTRANS;

    int lrc = check_for_node_up(iq, p_blkstate);
    if (lrc)
        return lrc;

    uint64_t strt = comdb2_time_epochus();
    reqlog_set_queue_time(iq->reqlogger, strt - iq->startus);
    reqlog_set_startprcs(iq->reqlogger, strt);

    /* adding this back temporarily so I can get blockop counts again. this
       really just belongs in the main loop (should get its own logger event
       flag and logger events for this flag should be reset in the case of a
       retry) */
    if (iq->retries == 0) {
        const uint8_t *p_buf_in_saved;
        int got_blockseq = 0;
        int got_osql = 0;
        int is_tagged = 1;

        /* this is a pre-loop, we want to jump back to the begining after it's
         * done */
        p_buf_in_saved = iq->p_buf_in;
        for (opnum = 0; opnum < num_reqs;
             ++opnum, block_state_next(iq, p_blkstate)) {
            struct packedreq_hdr hdr;

            iq->p_buf_in = packedreq_hdr_get(&hdr, iq->p_buf_in,
                                             p_blkstate->p_buf_req_end, iq->comdbg_flags);
            if (iq->p_buf_in == NULL)
                break;
            if (block_state_set_next(iq, p_blkstate, hdr.nxt))
                break;

            if (iq->debug)
                reqprintf(iq, "PRE LOOP REQ %d: %d", opnum, hdr.opcode);

            if (hdr.opcode > 0 && hdr.opcode < BLOCK_MAXOPCODE)
                opcode_counts[gbl_blockop_count_xrefs[hdr.opcode]]++;

            /* Since I am peeking into this, I adding my hacks here */
            switch (hdr.opcode) {
            case BLOCK_SEQ:
                if (gbl_use_blkseq) {
                    rc = extract_blkseq(iq, p_blkstate, &found_blkseq,
                                        &have_blkseq);
                    if (rc) {
                        logmsg(LOGMSG_ERROR, "failed to extract blockseq\n");
                        GOTOBACKOUT;
                    }
                    if (have_blkseq) {
                        got_blockseq = 1;
                        /* We don't do the pre-scan again if it's a retry, so
                         * save whether we had a blkseq */
                        iq->have_blkseq =
                            1; /* if we don't go through proxy we don't have
                                  blkseq */
                    }
                }
                break;
            case BLOCK2_SEQV2:
                if (gbl_use_blkseq) {
                    rc = extract_blkseq2(iq, p_blkstate, &found_blkseq,
                                         &have_blkseq);
                    if (rc) {
                        logmsg(LOGMSG_ERROR, "failed to extract blockseq\n");
                        GOTOBACKOUT;
                    }
                    if (have_blkseq) {
                        /* We don't do the pre-scan again if it's a retry, so
                         * save whether we had a blkseq */
                        iq->have_blkseq =
                            1; /* if we don't go through proxy we don't have
                                  blkseq */
                    }
                }
            case BLOCK2_SOCK_SQL:
            case BLOCK2_RECOM:
                got_osql = 1;
                /* fall-through */
            case BLOCK2_SNAPISOL:
            case BLOCK2_SERIAL:
                is_tagged = 0;
                if (gbl_use_blkseq) {
                    have_blkseq = 1;
                    osql_bplog_set_blkseq(iq->sorese, iq);
                }
                break;
            }
        }

        if (got_osql && IQ_HAS_SNAPINFO_KEY(iq)) {
            // the goal here is to stall until the original transaction
            // has finished that way we can retrieve the outcome from blkseq
            osql_blkseq_register_ireq(iq);

            void *replay_data = NULL;
            int replay_len = 0;
            BDB_READLOCK("early_replay_cnonce");
            if (thedb->master != gbl_myhostname) {
                bdb_rellock(thedb->bdb_env, __func__, __LINE__);
                outrc = ERR_NOMASTER;
                fromline = __LINE__;
                goto cleanup;
            }
            int found;
            found = bdb_blkseq_find(
                thedb->bdb_env, parent_trans, IQ_SNAPINFO(iq)->key,
                IQ_SNAPINFO(iq)->keylen, &replay_data, &replay_len);
            if (!found) {
                logmsg(LOGMSG_WARN,
                       "early snapinfo blocksql replay detected\n");
                outrc = do_replay_case(iq, IQ_SNAPINFO(iq)->key,
                                       IQ_SNAPINFO(iq)->keylen, num_reqs, 0,
                                       replay_data, replay_len, __LINE__);
                bdb_rellock(thedb->bdb_env, __func__, __LINE__);
#if DEBUG_DID_REPLAY
                did_replay = 1;
#endif
                fromline = __LINE__;
                goto cleanup;
            }
            bdb_rellock(thedb->bdb_env, __func__, __LINE__);
        }

        if (got_blockseq && got_osql && !IQ_HAS_SNAPINFO(iq)) {
            /* register this blockseq early to detect expensive replays
               of the same blocksql transactions */
            BDB_READLOCK("early_replay");
            if (thedb->master != gbl_myhostname) {
                bdb_rellock(thedb->bdb_env, __func__, __LINE__);
                outrc = ERR_NOMASTER;
                fromline = __LINE__;
                goto cleanup;
            }
            rc = osql_blkseq_register(iq);
            switch (rc) {
            case OSQL_BLOCKSEQ_INV:
            case OSQL_BLOCKSEQ_FIRST:
                /* silence */
                break;
            default:
                assert(rc == OSQL_BLOCKSEQ_REPLAY);
                logmsg(LOGMSG_WARN, "early blocksql replay detection\n");
                outrc = do_replay_case(iq, iq->seq, iq->seqlen, num_reqs, 0,
                                       NULL, 0, __LINE__);
                bdb_rellock(thedb->bdb_env, __func__, __LINE__);
#if DEBUG_DID_REPLAY
                did_replay = 1;
#endif
                fromline = __LINE__;
                goto cleanup;
            }
            bdb_rellock(thedb->bdb_env, __func__, __LINE__);
        }
        iq->p_buf_in = p_buf_in_saved;

        for (opnum = 0; opnum < NUM_BLOCKOP_OPCODES; opnum++) {
            if (opcode_counts[opnum] > 0) {
                reqlog_logf(iq->reqlogger, REQL_INFO, "%dx%s",
                            opcode_counts[opnum],
                            gbl_blockop_name_xrefs[opnum]);
            }
        }
        if (is_tagged && gbl_disable_tagged_api_writes) {
            logmsg(LOGMSG_ERROR, "Rejecting tagged api request\n");
            outrc = ERR_BADREQ;
            fromline = __LINE__;
            goto cleanup;
        }
    } else {
        have_blkseq = iq->have_blkseq;
    }

    iq->blkstate->pos = 0;
    txn_logbytes = &iq->written_logbytes_count;
    iq->written_logbytes_count = 0;

    if (!rowlocks) {
        /* start parent transaction */
        if (have_blkseq) {
            rc = trans_start(iq, NULL, &parent_trans);
            if (rc != 0) {
                return rc;
            }
        }

        irc = create_child_transaction(iq, parent_trans, &trans);
    } else {
        /* we dont have nested transaction support here.  we play games with
           writing the blkseq record differently in rowlocks mode */
        irc = trans_start_logical(iq, &trans);
        parent_trans = NULL;

        if (irc != 0) {
            /* At this point if we have a transaction, it would prevent a
             * downgrade
             * because it holds a bdb readlock.  Make sure I am still the master
             */
            if (thedb->master != gbl_myhostname || irc == ERR_NOMASTER) {
                numerrs = 1;
                rc = ERR_NOMASTER; /*this is what bdb readonly error gets us */
                GOTOBACKOUT;
            }
        }
    }

    if (debug_switch_test_ddl_backout_nomaster()) {
        rc = ERR_NOMASTER;
        GOTOBACKOUT;
    }

    if (debug_switch_test_ddl_backout_deadlock()) {
        rc = RC_INTERNAL_RETRY;
        GOTOBACKOUT;
    }

    if (irc != 0) {
        logmsg(LOGMSG_ERROR, "%s:%d trans_start failed rc %d\n", __func__,
               __LINE__, irc);
        numerrs = 1;
        rc = irc;
        GOTOBACKOUT;
    }

    if (iq->debug) {
        /* TODO print trans twice? No parent_trans? */
        reqprintf(iq, "%" PRIxPTR ":START TRANSACTION ID %p DB %d '%s'",
                  (intptr_t)pthread_self(), trans, iq->usedb->dbnum,
                  iq->usedb->tablename);
    }

    delay_if_sc_resuming(
        iq); /* tiny sleep if resuming sc has not marked sc pointers */

    javasp_trans_set_trans(javasp_trans_handle, iq, parent_trans, trans);

    if (gbl_replicate_local && get_dbtable_by_name("comdb2_oplog") && !iq->tranddl) {
        rc = localrep_seqno(trans, p_blkstate);
        if (rc) {
            if (rc != RC_INTERNAL_RETRY)
                logmsg(LOGMSG_ERROR, "get_next_seqno unexpected rc %d\n", rc);
            GOTOBACKOUT;
        }
    }

    /*which db currently operating on. */
    iq->usedb = iq->origdb; /*start with original db. */

    clear_constraints_tables();

    // Reset query effects
    if (IQ_HAS_SNAPINFO(iq))
        memset(&iq->sorese->snap_info->effects, 0, sizeof(struct query_effects));

    /* THE BLOCK PROCESSOR FOR LOOP */
    for (opnum = 0; opnum < num_reqs;
         ++opnum, block_state_next(iq, p_blkstate)) {
        struct packedreq_hdr hdr;
        if (!(iq->p_buf_in = packedreq_hdr_get(&hdr, iq->p_buf_in,
                                               p_blkstate->p_buf_req_end, iq->comdbg_flags)) ||
            block_state_set_next(iq, p_blkstate, hdr.nxt)) {
            if (iq->debug)
                reqprintf(iq, "%p:BAD BUFFER OFFSET OP %d", trans, opnum);

            rc = ERR_BADREQ;
            GOTOBACKOUT;
        }

        if (iq->debug) {
            /* remove all prefixes tothe debug trace and put in the transaction
             * and operation name prefix. */
            reqpopprefixes(iq, -1);
            reqpushprefixf(iq, "%" PRIxPTR ":tran %p:%s ", (intptr_t)pthread_self(), trans,
                           breq2a(hdr.opcode));

            reqprintflush(iq);
        }

        /* keep track of which operation is currently done */
        reqerrstrhdrclr(iq);

        thrman_wheref(thr_self, "%s [%s]", req2a(iq->opcode),
                      breq2a(hdr.opcode));

        iq->blkstate->opnum = opnum;

        if (iq->debug) {
            reqmoref(iq, " %d %s(%d) offset after hdr %p", opnum,
                     breq2a(hdr.opcode), hdr.opcode, iq->p_buf_in);
        }

        /* if this is the first request then keep stats */
        if (iq->retries == 0) {
            if ((unsigned)hdr.opcode < BLOCK_MAXOPCODE) {
                opcode_counts[gbl_blockop_count_xrefs[hdr.opcode]]++;
                iq->usedb->blocktypcnt[hdr.opcode]++;
                if (iq->rawnodestats)
                    iq->rawnodestats
                        ->blockop_counts[gbl_blockop_count_xrefs[hdr.opcode]]++;
            } else {
                opcode_counts[0]++; /* unknown opcode */
                iq->usedb->blocktypcnt[0]++;
                if (iq->rawnodestats)
                    iq->rawnodestats->blockop_counts[0]++;
            }
        }

        /* this is the list of blockops that are part of a osql transaction;
           anything else
           should be flagged and questioned */
        if (!is_mixed_sqldyn && hdr.opcode != BLOCK2_QBLOB &&
            hdr.opcode != BLOCK2_SOCK_SQL && hdr.opcode != BLOCK2_RECOM &&
            hdr.opcode != BLOCK2_SNAPISOL && hdr.opcode != BLOCK2_SERIAL &&
            hdr.opcode != BLOCK2_TZ && hdr.opcode != BLOCK_SEQ &&
            hdr.opcode != BLOCK2_SEQV2 && hdr.opcode != BLOCK2_USE &&
            hdr.opcode != BLOCK2_DBGLOG_COOKIE && hdr.opcode != BLOCK2_PRAGMA) {
            is_mixed_sqldyn = 1;
        }

        switch (hdr.opcode) {
        case BLOCK2_RNGDELKL:
            // need to mark this as keyless to have the right response length
            have_keyless_requests++;
            goto unknown_request;
            break;

        case BLOCK2_UPDBYKEY: {
            struct packedreq_updbykey updbykey;
            const uint8_t *p_buf_tag_name;
            const uint8_t *p_buf_tag_name_end;
            uint8_t *p_buf_data;
            const uint8_t *p_buf_data_end;
            LOG_LEGACY_REQUEST();

            ++delayed;
            have_keyless_requests = 1;

            if (!(iq->p_buf_in = packedreq_updbykey_get(
                      &updbykey, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            loadnullbmp(nulls, sizeof(nulls), updbykey.fldnullmap,
                        sizeof(updbykey.fldnullmap));

            if (updbykey.taglen >
                (p_blkstate->p_buf_next_start - iq->p_buf_in)) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK TAG NAME");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            p_buf_tag_name = iq->p_buf_in;
            iq->p_buf_in += updbykey.taglen;
            p_buf_tag_name_end = iq->p_buf_in;

            if (updbykey.reclen >
                (p_blkstate->p_buf_next_start - iq->p_buf_in)) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK DATA");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            /*
            iq->p_buf_in = (const uint8_t *)(iq->p_buf_in + updbykey.reclen);
            */

            p_buf_data = (uint8_t *)iq->p_buf_in;
            iq->p_buf_in += updbykey.reclen;
            p_buf_data_end = iq->p_buf_in;

            MIXED_SQL_DYNTAGS(trans, parent_trans);

            rc = updbykey_record(
                iq, trans, (const uint8_t *)p_buf_tag_name,
                (const uint8_t *)p_buf_tag_name_end, p_buf_data, p_buf_data_end,
                updbykey.keyname, nulls, blobs, MAXBLOBS, &err.errcode,
                &err.ixnum, &addrrn, &genid, hdr.opcode, opnum, /*blkpos*/
                RECFLAGS_DYNSCHEMA_NULLS_ONLY);
            free_blob_buffers(blobs, MAXBLOBS);
            if (rc != 0) {
                numerrs = 1;
                GOTOBACKOUT;
            }
            break;
        }

        case BLOCK2_ADDKL_POS:
            iq->is_block2positionmode = 1;
        case BLOCK2_ADDKL: {
            struct packedreq_addkl addkl;
            const uint8_t *p_buf_tag_name;
            const uint8_t *p_buf_tag_name_end;
            const uint8_t *p_buf_data;
            const uint8_t *p_buf_data_end;
            LOG_LEGACY_REQUEST();

            have_keyless_requests = 1;

            if (!(iq->p_buf_in = packedreq_addkl_get(
                      &addkl, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            loadnullbmp(nulls, sizeof(nulls), addkl.fldnullmap,
                        sizeof(addkl.fldnullmap));

            /* get the start/end of the tag name */
            if (addkl.taglen > (p_blkstate->p_buf_next_start - iq->p_buf_in)) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK TAG NAME");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }
            p_buf_tag_name = iq->p_buf_in;
            iq->p_buf_in += addkl.taglen;
            p_buf_tag_name_end = iq->p_buf_in;

            /* get the start/end of the data */
            if (addkl.reclen > (p_blkstate->p_buf_next_start - iq->p_buf_in)) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK DATA");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }
            p_buf_data = iq->p_buf_in;
            iq->p_buf_in += addkl.reclen;
            p_buf_data_end = iq->p_buf_in;

            MIXED_SQL_DYNTAGS(trans, parent_trans);

            {
                int bdberr = 0;
                rc = access_control_check_write(iq, trans, &bdberr);
                if (rc) {
                    numerrs = 1;
                    err.errcode = ERR_ACCESS;
                    GOTOBACKOUT;
                }
            }

            /* if any of the following have been executed then we
             * need to perform delayed key adds.
             *    BLOCK_ADDSL
             *    BLOCK_ADDSEC
             *    BLOCK_ADNOD
             *    BLOCK_SECAFPRI
             *    BLOCK_DELSC
             *    BLOCK_DELSEC
             *    BLOCK_DELNOD
             *    BLOCK_UPVRRN
             *    BLOCK2_ADDDTA
             *    BLOCK2_ADDKEY
             *    BLOCK2_DELDTA
             *    BLOCK2_DELKEY
             *    BLOCK2_UPDATE
             *    BLOCK2_DELKL
             *    BLOCK2_UPDKL
             *    BLOCK2_RNGDELKL
             *    BLOCK2_CUSTOM
             *    BLOCK2_QADD
             *    BLOCK2_DELOLDER
             *    BLOCK2_SOCK_SQL
             *    BLOCK2_UPDBYKEY
             *    BLOCK2_RECOM
             *    BLOCK2_SNAPISOL
             *    BLOCK2_SERIAL */
            int addflags = RECFLAGS_DYNSCHEMA_NULLS_ONLY;
            if (delayed == 0 && opnum == num_reqs - 1 &&
                iq->usedb->n_constraints == 0 && gbl_goslow == 0) {
                addflags |= RECFLAGS_NO_CONSTRAINTS;
            } else {
                ++delayed;
            }

            /* we drop the const from p_buf_data, but it shouldn't be
             * changed, can't change add_record's def to take a const
             * ptr because deep inside it puts the buf in dpb->put()
             * which doesn't take a const ptr */
            rc = add_record(iq, trans, p_buf_tag_name, p_buf_tag_name_end,
                            (uint8_t *)p_buf_data, p_buf_data_end, nulls, blobs,
                            MAXBLOBS, &err.errcode, &err.ixnum, &addrrn, &genid,
                            -1ULL, hdr.opcode, opnum, /*blkpos*/
                            addflags, 0);
            free_blob_buffers(blobs, MAXBLOBS);
            if (rc != 0) {
                numerrs = 1;
                GOTOBACKOUT;
            }
            /* TODO should this be set to rrn? where did that value come
             * from? */
            break;
        }

        case BLOCK2_ADDDTA: {
            struct packedreq_adddta adddta;

            const uint8_t *p_buf_data;
            const uint8_t *p_buf_data_end;

            const uint8_t *p_buf_tag_name;
            const uint8_t *p_buf_tag_name_end;
            LOG_LEGACY_REQUEST();

            ++delayed;
            if (!(iq->p_buf_in = packedreq_adddta_get(
                      &adddta, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            /* get the start/end of the data */
            if (adddta.reclen > (p_blkstate->p_buf_next_start - iq->p_buf_in)) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK DATA");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }
            p_buf_data = iq->p_buf_in;
            iq->p_buf_in += adddta.reclen;
            p_buf_data_end = iq->p_buf_in;

            p_buf_tag_name = (const uint8_t *)".DEFAULT";
            p_buf_tag_name_end = p_buf_tag_name + 8 /*strlen(.DEFAULT)*/;

            bzero(nulls, sizeof(nulls));

            MIXED_SQL_DYNTAGS(trans, parent_trans);

            {
                int bdberr = 0;
                rc = access_control_check_write(iq, trans, &bdberr);
                if (rc) {
                    numerrs = 1;
                    err.errcode = ERR_ACCESS;
                    GOTOBACKOUT;
                }
            }

            rc = add_record(iq, trans, p_buf_tag_name, p_buf_tag_name_end,
                            (uint8_t *)p_buf_data, p_buf_data_end, nulls,
                            NULL, /*blobs*/
                            0,    /*maxblobs*/
                            &err.errcode, &err.ixnum, &addrrn, &genid, -1ULL,
                            hdr.opcode, opnum, /*blkpos*/
                            RECFLAGS_DYNSCHEMA_NULLS_ONLY, 0);
            if (rc != 0) {
                numerrs = 1;
                GOTOBACKOUT;
            }
            break;
        }

        case BLOCK2_ADDKEY: {
            struct packedreq_addkey addkey;
            LOG_LEGACY_REQUEST();
            ++delayed;

            if (!(iq->p_buf_in = packedreq_addkey_get(
                      &addkey, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            /* if we got this far, the BLOCK2_ADDDTA should have
               added the keys.  save rrn and proceed. */
            if (iq->debug)
                reqprintf(iq, "IGNORED IX %d", addkey.ixnum);
            rc = 0;
            break;
        }

        case BLOCK2_DELKL: {
            struct packedreq_delkl delkl;
            LOG_LEGACY_REQUEST();

            ++delayed;
            if (!(iq->p_buf_in = packedreq_delkl_get(
                      &delkl, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            have_keyless_requests = 1;

            MIXED_SQL_DYNTAGS(trans, parent_trans);

            {
                int bdberr = 0;
                rc = access_control_check_write(iq, trans, &bdberr);
                if (rc) {
                    numerrs = 1;
                    err.errcode = ERR_ACCESS;
                    GOTOBACKOUT;
                }
            }

            /* for some reason we get a tag and data record with this
             * request which we then don't use.  let's not waste time
             * creating dynamic schemas that we won't use. */

            rc = del_record(iq, trans, NULL, /*primkey*/
                            delkl.rrn, delkl.genid, -1ULL, &err.errcode,
                            &err.ixnum, hdr.opcode, 0);
            if (rc != 0) {
                numerrs = 1;
                GOTOBACKOUT;
            }
            break;
        }

        case BLOCK2_DELDTA: {
            struct packedreq_delete delete;
            LOG_LEGACY_REQUEST();

            ++delayed;
            if (!(iq->p_buf_in = packedreq_delete_get(
                      &delete, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            rrn = delete.rrn;

            MIXED_SQL_DYNTAGS(trans, parent_trans);

            {
                int bdberr = 0;
                rc = access_control_check_write(iq, trans, &bdberr);
                if (rc) {
                    numerrs = 1;
                    err.errcode = ERR_ACCESS;
                    GOTOBACKOUT;
                }
            }

            rc = del_record(iq, trans, saved_fndkey, saved_rrn, 0, /*genid*/
                            -1ULL, &err.errcode, &err.ixnum, hdr.opcode, 0);
            if (rc != 0) {
                numerrs = 1;
                GOTOBACKOUT;
            }
            break;
        }

        /* Note: there is similar code earlier in this function in the
         * prescan loop, if you change it here please change it there */
        case BLOCK2_DELKEY: {
            struct packedreq_delkey delkey;
            LOG_LEGACY_REQUEST();

            ++delayed;
            if (!(iq->p_buf_in = packedreq_delkey_get(
                      &delkey, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            ixkeylen = getdefaultkeysize(iq->usedb, delkey.ixnum);

            /* ix 0 is unique: use it to find record,
               use record to form and delete other keys */
            if (delkey.ixnum != 0) {
                if (iq->debug)
                    reqprintf(iq, "IGNORED IX %d", delkey.ixnum);
                rc = 0;
                break;
            }

            if (ixkeylen < 0 || delkey.keylen != ixkeylen) {
                if (iq->debug)
                    reqprintf(iq, "INVALID INDEX %d OR BAD KEY LENGTH "
                                  "%d!=%d\n",
                              delkey.ixnum, delkey.keylen, ixkeylen);
                reqerrstr(iq, COMDB2_DEL_RC_INVL_KEY,
                          "invalid index %d or bad key length %d!=%d\n",
                          delkey.ixnum, delkey.keylen, ixkeylen);
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }
            rrn = delkey.rrn;

            /* TODO is this the right length to check? */
            if (ixkeylen > (iq->p_buf_in_end - iq->p_buf_in)) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK KEY");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            char ondisk_tag[MAXTAGLEN], client_tag[MAXTAGLEN];
            char key[MAXKEYLEN];
            /* ix 0 is unique: use it to find record,
               use record to form and delete other keys */
            snprintf(client_tag, MAXTAGLEN, ".DEFAULT_IX_0");
            snprintf(ondisk_tag, MAXTAGLEN, ".ONDISK_IX_0");
            rc = ctag_to_stag_buf(iq->usedb, client_tag,
                                  (const char *)iq->p_buf_in, WHOLE_BUFFER,
                                  nulls, ondisk_tag, key, 0, NULL);
            if (rc == -1) {
                if (iq->debug)
                    reqprintf(iq, "ERR CONVERT IX 0");
                rc = ERR_CONVERT_IX;
                GOTOBACKOUT;
            }
            memcpy(saved_fndkey, key, getkeysize(iq->usedb, delkey.ixnum));
            saved_rrn = rrn;
            if (iq->debug)
                reqprintf(iq, "SAVED IX %d RRN %d", delkey.ixnum, saved_rrn);
            break;
        }

        case BLOCK2_UPDKL_POS:
            iq->is_block2positionmode = 1;
        case BLOCK2_UPDKL: {
            struct packedreq_updrrnkl updrrnkl;
            const uint8_t *p_buf_tag_name;
            const uint8_t *p_buf_tag_name_end;
            const uint8_t *p_buf_data;
            const uint8_t *p_buf_data_end;
            LOG_LEGACY_REQUEST();

            ++delayed;
            if (!(iq->p_buf_in = packedreq_updrrnkl_get(
                      &updrrnkl, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            loadnullbmp(nulls, sizeof(nulls), updrrnkl.fldnullmap,
                        sizeof(updrrnkl.fldnullmap));

            /* TODO verify we didn't go past
             * p_blkstate->p_buf_next_start */
            p_buf_tag_name = iq->p_buf_in;
            p_buf_tag_name_end = p_buf_tag_name + updrrnkl.taglen;
            p_buf_data = p_buf_tag_name_end;
            p_buf_data_end = p_buf_data + updrrnkl.rlen;

            have_keyless_requests = 1;

            MIXED_SQL_DYNTAGS(trans, parent_trans);

            {
                int bdberr = 0;
                rc = access_control_check_write(iq, trans, &bdberr);
                if (rc) {
                    numerrs = 1;
                    err.errcode = ERR_ACCESS;
                    GOTOBACKOUT;
                }
            }

            rc = upd_record(iq, trans, NULL, /*primary key*/
                            updrrnkl.rrn, updrrnkl.genid, p_buf_tag_name,
                            p_buf_tag_name_end, (uint8_t *)p_buf_data,
                            p_buf_data_end, NULL /*p_buf_vrec*/,
                            NULL /*p_buf_vrec_end*/, nulls, NULL, /*updcols*/
                            blobs, MAXBLOBS, &genid, -1ULL, -1ULL, &err.errcode,
                            &err.ixnum, hdr.opcode, opnum, /*blkpos*/
                            RECFLAGS_DYNSCHEMA_NULLS_ONLY);
            free_blob_buffers(blobs, MAXBLOBS);
            if (rc != 0) {
                numerrs = 1;
                GOTOBACKOUT;
            }
            break;
        }

        /* Note: there is similar code earlier in this function in the
         * prescan loop, if you change it here please change it there */
        case BLOCK2_UPDATE: {
            struct packedreq_updrrn updrrn;

            const uint8_t *p_buf_data_new;
            const uint8_t *p_buf_data_new_end;
            const uint8_t *p_buf_data_v;
            const uint8_t *p_buf_data_v_end;
            const uint8_t *p_buf_tag_name;
            const uint8_t *p_buf_tag_name_end;
            LOG_LEGACY_REQUEST();

            ++delayed;
            if (!(iq->p_buf_in = packedreq_updrrn_get(
                      &updrrn, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            /* just like BLOCK_UPVRRN */

            if (updrrn.rlen > (p_blkstate->p_buf_next_start - iq->p_buf_in)) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK NEW DATA");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            p_buf_data_new = iq->p_buf_in;
            iq->p_buf_in += updrrn.rlen;
            p_buf_data_new_end = iq->p_buf_in;

            if (updrrn.vlen > (p_blkstate->p_buf_next_start - iq->p_buf_in)) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK VDATA");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            p_buf_data_v = iq->p_buf_in;
            iq->p_buf_in += updrrn.vlen;
            p_buf_data_v_end = iq->p_buf_in;

            if (updrrn.rlen != updrrn.vlen) {
                if (iq->debug)
                    reqprintf(iq, "MISMATCH newlen %d != vlen %d", updrrn.rlen,
                              updrrn.vlen);
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            p_buf_tag_name = (const uint8_t *)".DEFAULT";
            p_buf_tag_name_end = p_buf_tag_name + 8 /*strlen(.DEFAULT)*/;
            bzero(nulls, sizeof(nulls));

            MIXED_SQL_DYNTAGS(trans, parent_trans);

            {
                int bdberr = 0;
                rc = access_control_check_write(iq, trans, &bdberr);
                if (rc) {
                    numerrs = 1;
                    err.errcode = ERR_ACCESS;
                    GOTOBACKOUT;
                }
            }

            rc = upd_record(iq, trans, NULL, /*primkey - will be formed from
                                               verification data*/
                            updrrn.rrn, 0,   /*vgenid*/
                            p_buf_tag_name, p_buf_tag_name_end,
                            (uint8_t *)p_buf_data_new, p_buf_data_new_end,
                            (uint8_t *)p_buf_data_v, p_buf_data_v_end, nulls,
                            NULL, /*updcols*/
                            NULL, /*blobs*/
                            0,    /*maxblobs*/
                            &genid, -1ULL, -1ULL, &err.errcode, &err.ixnum,
                            hdr.opcode, opnum, /*blkpos*/
                            RECFLAGS_DYNSCHEMA_NULLS_ONLY);

            if (rc != 0) {
                numerrs = 1;
                GOTOBACKOUT;
            }

            break;
        }

        case BLOCK_ADDSL: {
            struct packedreq_add packedreq_add;
            int datoff, datlen;
            const uint8_t *p_buf_tag_name;
            const uint8_t *p_buf_tag_name_end;
            const uint8_t *p_buf_data;
            const uint8_t *p_buf_data_end;
            LOG_LEGACY_REQUEST();

            ++delayed;
            if (!(iq->p_buf_in =
                      packedreq_add_get(&packedreq_add, iq->p_buf_in,
                                        p_blkstate->p_buf_next_start, iq->comdbg_flags))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            /*ADD RRN AND IX 0 KEY */
            ixkeylen = getdefaultkeysize(iq->usedb, 0);
            datoff = (ixkeylen + 3) & -4; /*round up to 4, this is
                                            data */
            datlen = (packedreq_add.lrl) * 4;

            /* increment input pointer to the offset */
            iq->p_buf_in += datoff;
            p_buf_data = iq->p_buf_in;

            /* verify that there's enough space */
            if (datlen > (p_blkstate->p_buf_next_start - iq->p_buf_in)) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK DATA");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            /* increment the input pointer past the record */
            iq->p_buf_in += datlen;
            p_buf_data_end = iq->p_buf_in;

            /* set tag name */
            p_buf_tag_name = (const uint8_t *)".DEFAULT";
            p_buf_tag_name_end = p_buf_tag_name + 8;

            bzero(nulls, sizeof(nulls));

            MIXED_SQL_DYNTAGS(trans, parent_trans);

            {
                int bdberr = 0;
                rc = access_control_check_write(iq, trans, &bdberr);
                if (rc) {
                    numerrs = 1;
                    err.errcode = ERR_ACCESS;
                    GOTOBACKOUT;
                }
            }

            /* add */
            int addflags = RECFLAGS_DYNSCHEMA_NULLS_ONLY;
            if (iq->comdbg_flags)
                addflags |= RECFLAGS_COMDBG_FROM_LE;

            rc = add_record(iq, trans, p_buf_tag_name, p_buf_tag_name_end,
                            (uint8_t *)p_buf_data, p_buf_data_end, nulls,
                            NULL, /*blobs*/
                            0,    /*maxblobs*/
                            &err.errcode, &err.ixnum, &addrrn, &genid, -1ULL,
                            hdr.opcode, opnum, /*blkpos*/
                            addflags, 0);

            if (rc != 0) {
                numerrs = 1;
                GOTOBACKOUT;
            }

            break;
        }
        case BLOCK_SEQ: {
            extract_blkseq(iq, p_blkstate, &found_blkseq, &have_blkseq);
            break;
        }
        case BLOCK2_SEQV2: {
            extract_blkseq2(iq, p_blkstate, &found_blkseq, &have_blkseq);
            break;
        }

        case BLOCK_ADDSEC:
        case BLOCK_ADNOD:
        case BLOCK_SECAFPRI: {
            struct packedreq_addsec addsec;
            int ixnum;
            LOG_LEGACY_REQUEST();

            ++delayed;
            if (!(iq->p_buf_in = packedreq_addsec_get(
                      &addsec, iq->p_buf_in, p_blkstate->p_buf_req_end, iq->comdbg_flags))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK DATA");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            ixnum = addsec.ixnum;

            rc = 0;
            if (iq->debug)
                reqprintf(iq, "IGNORED IX %d", ixnum);
            break;
        }

        /* Note: there is similar code earlier in this function in the
         * prescan loop, if you change it here please change it there */
        case BLOCK_DELSC: {
            struct packedreq_del delsc;
            const char *p_keydat;
            char key[MAXKEYLEN];
            LOG_LEGACY_REQUEST();

            ++delayed;
            if (!(iq->p_buf_in = packedreq_del_get(
                      &delsc, iq->p_buf_in, p_blkstate->p_buf_req_end, iq->comdbg_flags))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK DATA");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            ixkeylen = getkeysize(iq->usedb, 0);
            rrn = delsc.rrn;

            if (ixkeylen > p_blkstate->p_buf_req_end - iq->p_buf_in) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK DATA");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }
            p_keydat = (const char *)iq->p_buf_in;
            iq->p_buf_in += ixkeylen;

            MIXED_SQL_DYNTAGS(trans, parent_trans);

            /* convert key */
            bzero(nulls, sizeof(nulls));
            rc = ctag_to_stag_buf(iq->usedb, ".DEFAULT_IX_0",
                                  (const char *)p_keydat, WHOLE_BUFFER, nulls,
                                  ".ONDISK_IX_0", key, convert_flags, NULL);
            if (rc == -1) {
                if (iq->debug)
                    reqprintf(iq, "ERR CLIENT CONVERT IX 0 RRN %d", rrn);
                rc = ERR_CONVERT_IX;
                GOTOBACKOUT;
            }
            rc = del_record(iq, trans, key, delsc.rrn, 0, /*genid*/
                            -1ULL, &err.errcode, &err.ixnum, hdr.opcode, 0);
            if (rc != 0) {
                numerrs = 1;
                GOTOBACKOUT;
            }
            break;
        }

        case BLOCK_DELSEC:
        case BLOCK_DELNOD: {
            struct packedreq_delsec delsec;
            int ixnum;
            int ixkeylen;
            LOG_LEGACY_REQUEST();

            ++delayed;
            if (!(iq->p_buf_in = packedreq_delsec_get(
                      &delsec, iq->p_buf_in, p_blkstate->p_buf_req_end, iq->comdbg_flags))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK DATA");
                rc = ERR_BADREQ;

                GOTOBACKOUT;
            }

            ixnum = delsec.ixnum;
            ixkeylen = getdefaultkeysize(iq->usedb, ixnum);
            if (ixkeylen < 0) {
                if (iq->debug)
                    reqprintf(iq, "BAD IX %d", ixnum);
                reqerrstr(iq, COMDB2_DEL_RC_INVL_IDX, "bad index %d", ixnum);
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            /* handled in BLOCK_DELSC */
            if (iq->debug)
                reqprintf(iq, "IGNORED IX %d", ixnum);
            rc = 0;
            break;
        }

        /* Note: there is similar code earlier in this function in the
         * prescan loop, if you change it here please change it there */
        case BLOCK_UPVRRN: {
            struct packedreq_upvrrn upvrrn;
            int vlen, newlen;
            const uint8_t *p_buf_tag_name;
            const uint8_t *p_buf_tag_name_end;
            const uint8_t *p_newdta;
            const uint8_t *p_newdta_end;
            const uint8_t *p_buf_vdta;
            const uint8_t *p_buf_vdta_end;
            int comdbg_flags = iq->comdbg_flags;
            LOG_LEGACY_REQUEST();
            GETFUNC

            ++delayed;
            if (!(iq->p_buf_in = packedreq_upvrrn_get(
                      &upvrrn, iq->p_buf_in, p_blkstate->p_buf_req_end, iq->comdbg_flags))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK DATA");
                rc = ERR_BADREQ;

                GOTOBACKOUT;
            }

            rrn = upvrrn.rrn;
            vlen = upvrrn.vlen4 * 4;

            p_buf_vdta = iq->p_buf_in;
            if (vlen > p_blkstate->p_buf_req_end - iq->p_buf_in) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK DATA");
                rc = ERR_BADREQ;

                GOTOBACKOUT;
            }

            iq->p_buf_in += vlen;
            p_buf_vdta_end = iq->p_buf_in;

            if (!(iq->p_buf_in = getfunc(&newlen, sizeof(newlen), iq->p_buf_in,
                                         p_blkstate->p_buf_req_end))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK DATA");
                rc = ERR_BADREQ;

                GOTOBACKOUT;
            }

            newlen *= 4;

            p_newdta = iq->p_buf_in;

            if (newlen > p_blkstate->p_buf_req_end - iq->p_buf_in) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK DATA");
                rc = ERR_BADREQ;

                GOTOBACKOUT;
            }

            iq->p_buf_in += newlen;
            p_newdta_end = iq->p_buf_in;
            p_buf_tag_name = (const uint8_t *)".DEFAULT";
            p_buf_tag_name_end = p_buf_tag_name + 8;

            /*
                vlen            -> length of the verify record
                p_buf_vdta      -> start of verify record
                p_buf_vdta_end  -> end of verify record
                newlen          -> length of the new record
                p_newdta        -> start of the new record
                p_newdta_end    -> end of the new record
                p_buf_tag_name  -> .DEFAULT
            */

            {
                /*
                   capture comdb clients that are trying to verify
                   only a prefix of the row (size < .default rowsize)
                 */
                int rowsz = get_size_of_schema_by_name(iq->usedb, ".DEFAULT");
                if (rowsz != vlen) {
                    logmsg(
                        LOGMSG_ERROR,
                        "%s: %s prefix bug, client sz=%d, default-tag sz=%d\n",
                        getorigin(iq), iq->usedb->tablename, newlen, rowsz);
                }
            }

            bzero(nulls, sizeof(nulls));

            MIXED_SQL_DYNTAGS(trans, parent_trans);

            {
                int bdberr = 0;
                rc = access_control_check_write(iq, trans, &bdberr);
                if (rc) {
                    numerrs = 1;
                    err.errcode = ERR_ACCESS;
                    GOTOBACKOUT;
                }
            }

            rc = upd_record(iq, trans, NULL, /*primkey - will be formed from
                                               verification data*/
                            rrn, 0,          /*vgenid*/
                            p_buf_tag_name, p_buf_tag_name_end,
                            (uint8_t *)p_newdta, p_newdta_end,
                            (uint8_t *)p_buf_vdta, p_buf_vdta_end, nulls,
                            NULL, /*updcols*/
                            NULL, /*blobs*/
                            0,    /*maxblobs*/
                            &genid, -1ULL, -1ULL, &err.errcode, &err.ixnum,
                            hdr.opcode, opnum, /*blkpos*/
                            RECFLAGS_DYNSCHEMA_NULLS_ONLY);

            if (rc != 0) {
                numerrs = 1;
                GOTOBACKOUT;
            }
            break;
        }

        case BLOCK_DEBUG: {
            if (iq->debug)
                reqprintf(iq, "OP OFFSET %d", -1);
            break;
        }

        /* Note: there is similar code earlier in this function in the
         * prescan loop, if you change it here please change it there */
        case BLOCK_USE: {
            struct packedreq_use use;

            if (!(iq->p_buf_in = packedreq_use_get(
                      &use, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            iq->usedb = getdbbynum(use.dbnum);
            logmsg(LOGMSG_DEBUG, "%s %d use.dbnum = %d\n", __FILE__, __LINE__, use.dbnum);
            if (iq->usedb == NULL) {
                if (iq->debug)
                    reqprintf(iq, "ERROR DB NUM %d NOT IN "
                                  "TRANSACTION GROUP",
                              use.dbnum);
                reqerrstr(iq, COMDB2_BLK_RC_INVL_DBNUM,
                          "error db num %d not in transaction group",
                          use.dbnum);
                iq->usedb = iq->origdb;
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }
            if (iq->debug)
                reqprintf(iq, "DB NUM %d '%s'", use.dbnum,
                          iq->usedb->tablename);
            break;
        }

        /* Note: there is similar code earlier in this function in the
         * prescan loop, if you change it here please change it there */
        case BLOCK2_USE: {
            struct packedreq_usekl usekl;

            if (!(iq->p_buf_in = packedreq_usekl_get(
                      &usekl, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            if (usekl.taglen) {
                char tbltag[64];

                if (usekl.taglen < 0 || usekl.taglen >= sizeof(tbltag)) {
                    if (iq->debug)
                        reqprintf(iq, "INVALID TAGLEN %d", usekl.taglen);
                    rc = ERR_BADREQ;
                    GOTOBACKOUT;
                }

                bzero(tbltag, sizeof(tbltag));
                /* we make sure tbltag is NUL terminated by ensuring
                 * taglen < sizeof(tbltag) above */
                if (!(iq->p_buf_in =
                          buf_no_net_get(tbltag, usekl.taglen, iq->p_buf_in,
                                         p_blkstate->p_buf_next_start))) {
                    if (iq->debug)
                        reqprintf(iq, "FAILED TO UNPACK TBLTAG");
                    rc = ERR_BADREQ;
                    GOTOBACKOUT;
                }

                iq->usedb = get_dbtable_by_name(tbltag);
                if (iq->usedb == NULL) {
                    iq->usedb = iq->origdb;
                    if (iq->debug)
                        reqprintf(iq, "ERROR UNKNOWN TABLE '%s'", tbltag);
                    reqerrstr(iq, COMDB2_BLK_RC_UNKN_TAG,
                              "error unknown table '%s'", tbltag);
                    GOTOBACKOUT;
                }

                if (iq->debug)
                    reqprintf(iq, "DB '%s'", iq->usedb->tablename);
            } else {
                iq->usedb = getdbbynum(usekl.dbnum);
                if (iq->usedb == 0) {
                    if (iq->debug)
                        reqprintf(iq, "ERROR UNKNOWN DB NUM %d", usekl.dbnum);
                    reqerrstr(iq, COMDB2_BLK_RC_INVL_DBNUM,
                              "error unknown db num %d", usekl.dbnum);
                    iq->usedb = iq->origdb;
                    rc = ERR_BADREQ;
                    GOTOBACKOUT;
                }
                if (iq->debug)
                    reqprintf(iq, "DB NUM %d '%s'", usekl.dbnum,
                              iq->usedb->tablename);
            }
            break;
        }

        case BLOCK2_TZ: {
            struct packedreq_tzset tzset;

            if (!(iq->p_buf_in = packedreq_tzset_get(
                      &tzset, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            if (!(iq->p_buf_in =
                      buf_no_net_get(iq->tzname, tzset.tznamelen, iq->p_buf_in,
                                     p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK TZ");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            break;
        }

        case BLOCK2_QBLOB: {
            struct packedreq_qblob qblob;

            have_keyless_requests = 1;

            if (!(iq->p_buf_in = packedreq_qblob_get(
                      &qblob, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            if (qblob.blobno >= MAXBLOBS) {
                reqerrstr(iq, COMDB2_BLOB_RC_RCV_TOO_MANY,
                          "blob %d out of range (MAXBLOBS is %d)", qblob.blobno,
                          MAXBLOBS);
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            } else {
                blob_buffer_t *blob = &blobs[qblob.blobno];
                if (!blob->exists) {
                    if (qblob.length > MAXBLOBLENGTH) {
                        reqerrstr(iq, COMDB2_BLOB_RC_RCV_TOO_LARGE,
                                  "blob %d too large (%u > max size %u)",
                                  qblob.blobno, qblob.length, MAXBLOBLENGTH);
                        rc = ERR_BLOB_TOO_LARGE;
                        GOTOBACKOUT;
                    }
                    blob->length = qblob.length;
                    blob->collected = 0;
                    if (blob->length > 0) {
                        if (blob->length > gbl_blob_sz_thresh_bytes)
                            blob->data = comdb2_bmalloc(blobmem, blob->length);
                        else
                            blob->data = malloc(blob->length);

                        if (!blob->data) {
                            logmsg(LOGMSG_ERROR, "BLOCK2_QBLOB: malloc failed %d\n", blob->length);
                            reqerrstr(iq, COMDB2_BLOB_RC_ALLOC, "malloc failed");
                            rc = ERR_INTERNAL;
                            GOTOBACKOUT;
                        }
                    }
                    blob->exists = 1;
                    reqlog_logf(iq->reqlogger, REQL_INFO, "%d byte blob", blob->length);
                } else if (qblob.length != blob->length) {
                    reqerrstr(iq, COMDB2_BLOB_RC_RCV_BAD_LENGTH,
                              "bad fragment for blob %d gives length %u "
                              "expected %zu",
                              qblob.blobno, qblob.length, blob->length);
                    rc = ERR_BADREQ;
                    GOTOBACKOUT;
                }
                /* collect more data */
                if (qblob.frag_len > 0) {
                    if (qblob.frag_len + blob->collected > blob->length) {
                        reqerrstr(iq, COMDB2_BLOB_RC_RCV_TOO_MUCH,
                                  "received too much data for blob %d "
                                  "(I had %zu/%zu bytes, received another "
                                  "%u)",
                                  qblob.blobno, blob->collected, blob->length,
                                  qblob.frag_len);
                        rc = ERR_BADREQ;
                        GOTOBACKOUT;
                    }
                    iq->p_buf_in = buf_no_net_get(blob->data + blob->collected,
                                                  qblob.frag_len, iq->p_buf_in,
                                                  p_blkstate->p_buf_next_start);

                    blob->collected += qblob.frag_len;
                }
            }
            break;
        }

        case BLOCK2_QADD: {
            struct packedreq_qadd qadd;

            ++delayed;
            if (!(iq->p_buf_in = packedreq_qadd_get(
                      &(qadd), iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            have_keyless_requests = 1;

            MIXED_SQL_DYNTAGS(trans, parent_trans);

            rc = block2_qadd(iq, p_blkstate, trans, &qadd, blobs);
            free_blob_buffers(blobs, MAXBLOBS);
            if (rc != 0)
                GOTOBACKOUT;
            break;
        }
        case BLOCK_SETFLAGS: {
            struct packedreq_setflags p_setflags;
            if (!(iq->p_buf_in =
                      packedreq_setflags_get(&p_setflags, iq->p_buf_in,
                                             p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }
            break;
        }

        case BLOCK2_CUSTOM: {
            struct packedreq_custom p_custom;

            ++delayed;
            if (!(iq->p_buf_in = packedreq_custom_get(
                      &p_custom, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            /* A Java stored procedure custom operation. */
            rc = block2_custom(iq, &p_custom, iq->p_buf_in, blobs);
            free_blob_buffers(blobs, MAXBLOBS);
            if (rc != 0)
                GOTOBACKOUT;
            break;
        }

        case BLOCK2_TRAN: {
            LOG_LEGACY_REQUEST();
            /* handled in loop/switch above */
            break;
        }

        case BLOCK2_DELOLDER: {
            unsigned long long genid;
            void *rec;
            int dtalen;
            int failop, failnum;
            struct packedreq_delolder delolder;
            LOG_LEGACY_REQUEST();

            if (iq && iq->usedb && iq->usedb->odh) {
                reqprintf(iq, "ERROR - OPCODE BLOCK2_DELOLDER FOR ODH TABLE");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            ++delayed;
            if (!(iq->p_buf_in = packedreq_delolder_get(
                      &delolder, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            if (!gbl_dtastripe) {
                rc = ERR_BADREQ;
                break;
            }
            dtalen =
                get_size_of_schema_by_name(iq->usedb, ".ONDISK");

            MIXED_SQL_DYNTAGS(trans, parent_trans);

            rec = malloc(dtalen);
            rc = find_record_older_than(iq, trans, delolder.timestamp, rec,
                                        &dtalen, dtalen, &genid);
            if (rc == 0) {
                {
                    int bdberr = 0;
                    rc = access_control_check_write(iq, trans, &bdberr);
                    if (rc) {
                        numerrs = 1;
                        err.errcode = ERR_ACCESS;
                        GOTOBACKOUT;
                    }
                }

                rc = del_record(iq, trans, NULL, 2, genid, -1ULL, &failop,
                                &failnum, BLOCK2_DELOLDER, 0);
                if (iq->debug)
                    reqprintf(iq, "DELETE_OLDER %d %s genid %016llx rc "
                                  "%d\n",
                              delolder.timestamp, iq->usedb->tablename, genid,
                              rc);
                if (rc) {
                    fromline = __LINE__;
                    goto backout;
                }
            } else {
                if (iq->debug)
                    reqprintf(iq, "DELETE_OLDER %s none found\n",
                              iq->usedb->tablename);
                rc = ERR_NO_RECORDS_FOUND;
            }
            free(rec);
            if (rc) {
                goto backout;
            }
            break;
        }

        case BLOCK2_SOCK_SQL:
        case BLOCK2_RECOM:
        case BLOCK2_SNAPISOL:
        case BLOCK2_SERIAL: {
            struct packedreq_sql sql;
            const uint8_t *p_buf_sqlq;

            /* synthetic block2_use */
            iq->usedb = get_dbtable_by_name(thedb->static_table.tablename);

            /* synthetic block2_tz */
            strncpy0(iq->tzname, iq->sorese->tzname, sizeof(iq->tzname));

            ++delayed;
            is_block2sqlmode = 1;
            have_keyless_requests = 1;

            int d_ms = BDB_ATTR_GET(thedb->bdb_attr, DELAY_AFTER_SAVEOP_DONE);
            if (d_ms) {
                logmsg(LOGMSG_DEBUG,
                       "Sleeping for DELAY_AFTER_SAVEOP_DONE (%dms)\n", d_ms);
                usleep(1000 * d_ms);
            }

            if (!(iq->p_buf_in = packedreq_sql_get(
                      &sql, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            if (sql.sqlqlen > (p_blkstate->p_buf_next_start - iq->p_buf_in)) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK SQLQ");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            p_buf_sqlq = iq->p_buf_in;
            iq->p_buf_in += sql.sqlqlen;

            if (iq->retries > 0) {
                if (iq->debug)
                    reqprintf(iq, "query retries '%s'", (char *)p_buf_sqlq);
                break;
            }

            if (iq->debug)
                reqprintf(
                    iq, "%s received query '%s'",
                    (hdr.opcode == BLOCK2_SOCK_SQL)
                        ? "socksql"
                        : ((hdr.opcode == BLOCK2_RECOM)
                               ? "recom"
                               : ((hdr.opcode == BLOCK2_SNAPISOL) ? "snapisol"
                                                                  : "serial")),
                    (char *)p_buf_sqlq);

            if (is_mixed_sqldyn) {
                logmsg(LOGMSG_ERROR, "%s:%d INCORRECT TRANSACTION MIX, SQL AND DYNTAG %d\n",
                        __FILE__, __LINE__, hdr.opcode);
            } else {
                if (osql_needtransaction == OSQL_BPLOG_NONE) {
                    rc = osql_destroy_transaction(
                        iq, have_blkseq ? &parent_trans : NULL, &trans,
                        &osql_needtransaction);
                    if (rc) {
                        numerrs = 1;
                        GOTOBACKOUT;
                    }
                }
            }

            block2_sorese(iq, (char *)p_buf_sqlq, sql.sqlqlen, hdr.opcode);
            break;
        }

        case BLOCK2_MODNUM: {
            struct packedreq_set_modnum modnum;
            LOG_LEGACY_REQUEST();

            if (!(iq->p_buf_in = packedreq_set_modnum_get(
                      &modnum, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }
            iq->blkstate->modnum = modnum.modnum;
            break;
        }

        case BLOCK2_SCSMSK: {
            /* there should only ever be more then one BLOCK2_SCSMSK if
             * a long block comes in and more then one piece has a
             * BLOCK2_SCSMSK in this case tolongblock should have
             * updated the fisrt mask to contain all the stripes
             * needed by the whole request but here we check to make
             * sure that was the case */

            struct packedreq_scsmsk scsmsk;
            LOG_LEGACY_REQUEST();

            if (!(iq->p_buf_in = packedreq_scsmsk_get(
                      &scsmsk, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            break;
        }

        case BLOCK2_DBGLOG_COOKIE: {
            struct packedreq_dbglog_cookie cookie;
            LOG_LEGACY_REQUEST();

            if (!(iq->p_buf_in = packedreq_dbglog_cookie_get(
                      &cookie, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK DBGLOG COOKIE");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            switch (cookie.op) {
            case DEBUG_COOKIE_DUMP_PLAN:
                iq->dbglog_file = open_dbglog_file(cookie.cookie);
                iq->queryid = cookie.queryid;
                dbglog_init_write_counters(iq);
                break;
            default:
                break;
            }
            break;
        }

        case BLOCK2_PRAGMA: {
            struct packed_pragma pragma;

            if (!(iq->p_buf_in = packedreq_pragma_get(
                      &pragma, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK PRAGMA BLOCKOP");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            if (pragma.type == SQL_PRAGMA_MAXCOST) {

                struct query_limits_req req;

                if (pragma.len < sizeof(struct query_limits_req))
                    break;

                if (!(iq->p_buf_in = query_limits_req_get(
                          &req, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                    if (iq->debug)
                        reqprintf(iq,
                                  "FAILED TO UNPACK SQL_PRAGMA_MAXCOST PRAGMA");
                    rc = ERR_BADREQ;
                    GOTOBACKOUT;
                }

                if (req.have_max_cost) {
                    iq->__limits.maxcost = req.max_cost;
                }
                if (req.have_allow_tablescans) {
                    iq->__limits.tablescans_ok = req.allow_tablescans;
                }
                if (req.have_allow_temptables) {
                    iq->__limits.temptables_ok = req.allow_temptables;
                }

                if (req.have_max_cost_warning) {
                    iq->__limits.maxcost_warn = req.max_cost_warning;
                }
                if (req.have_tablescans_warning) {
                    iq->__limits.tablescans_warn = req.tablescans_warning;
                }
                if (req.have_allow_temptables) {
                    iq->__limits.temptables_warn = req.temptables_warning;
                }

            } else if (pragma.type == TAGGED_PRAGMA_CLIENT_ENDIAN) {
                struct client_endian_pragma_req req;
                if (pragma.len < sizeof(struct client_endian_pragma_req))
                    break;

                if (!(iq->p_buf_in = client_endian_pragma_req_get(
                          &req, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                    if (iq->debug)
                        reqprintf(
                            iq, "FAILED TO UNPACK TAGGED_CLIENT_ENDIAN PRAGMA");
                    rc = ERR_BADREQ;
                    GOTOBACKOUT;
                }
                /* Set ireq for big-endian. */
                if (TAGGED_API_BIG_ENDIAN == req.endian) {
                    iq->have_client_endian = 1;
                    iq->client_endian = TAGGED_API_BIG_ENDIAN;
                }
                /* Set ireq for little-endian. */
                else if (TAGGED_API_LITTLE_ENDIAN == req.endian) {
                    iq->have_client_endian = 1;
                    iq->client_endian = TAGGED_API_LITTLE_ENDIAN;
                }
                /* Bad request. */
                else {
                    if (iq->debug)
                        reqprintf(iq, "INVALID TAGGED_CLIENT_ENDIAN PRAGMA");
                    rc = ERR_BADREQ;
                    GOTOBACKOUT;
                }
            }

            break;
        }

        case BLOCK2_UPTBL: {
            struct packedreq_uptbl uptbl;
            int dtasz;
            LOG_LEGACY_REQUEST();

            if (!(iq->p_buf_in = packedreq_uptbl_get(
                      &uptbl, iq->p_buf_in, p_blkstate->p_buf_next_start))) {
                if (iq->debug)
                    reqprintf(iq, "FAILED TO UNPACK");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            if (iq->usedb == NULL) {
                if (iq->debug)
                    reqprintf(iq, "NO USEDB PROVIDED");
                rc = ERR_BADREQ;
                GOTOBACKOUT;
            }

            // if upgrade-ahead more than 1 record, start a table upgrade
            // thread.
            // otherwise use upgrade_record shortcut.
            if (uptbl.nrecs > 1) {
                rc = start_table_upgrade(iq->dbenv, iq->usedb->tablename,
                                         uptbl.genid, 0, uptbl.nrecs, 1);
                if (rc != 0)
                    GOTOBACKOUT;
            } else {
                dtasz = getdatsize(iq->usedb);
                rc = upgrade_record(iq, trans, uptbl.genid, NULL,
                                    (const uint8_t *)(uintptr_t)dtasz,
                                    &err.errcode, &err.ixnum, BLOCK2_UPTBL,
                                    hdr.opcode);
            }

            break;
        }

        default:
unknown_request:
            /*unknown operation */
            if (iq->debug)
                reqprintf(iq, "BAD OPCODE %d", hdr.opcode);
            reqerrstr(iq, COMDB2_BLK_RC_UNKN_OP, "bad opcode %d", hdr.opcode);
            rc = ERR_BADREQ;
            GOTOBACKOUT;
        } /*switch opcode */
    }     /* BLOCK PROCESSOR FOR LOOP */

    /* if this a blocksql transaction, we need to actually execute the ops */
    if (is_block2sqlmode) {
        int tmpnops = 0;
        int needbackout = 0;

        if (iq->tranddl) {
            int iirc;
            if (trans) {
                iirc = osql_destroy_transaction(iq, have_blkseq ? &parent_trans
                                                                : NULL,
                                                &trans, &osql_needtransaction);
                if (iirc) {
                    numerrs = 1;
                    GOTOBACKOUT;
                }
            }
            /* Don't get the bdb-lock here: you'll end up holding it for the
             * duration of schema-change */
            iirc = bplog_schemachange(iq);
            if (iirc) {
                rc = iirc;
                needbackout = 1;
            } else if (gbl_replicate_local && get_dbtable_by_name("comdb2_oplog")) {
                rc = localrep_seqno(trans, p_blkstate);
                if (rc) {
                    if (rc != RC_INTERNAL_RETRY)
                        logmsg(LOGMSG_ERROR, "get_next_seqno unexpected rc %d for ddl\n", rc);
                    needbackout = 1;
                }
            }
        }

        /* recreate a transaction here */
        if (osql_needtransaction == OSQL_BPLOG_NOTRANS) {
            int iirc =
                osql_create_transaction(javasp_trans_handle, iq, &trans,
                                        have_blkseq ? &parent_trans : NULL,
                                        &osql_needtransaction, __LINE__);
            if (iirc) {
                if (!rc)
                    rc = iirc;
                numerrs = 1;
                GOTOBACKOUT;
            }

            /* at this point we have a transaction, which would prevent a
            downgrade;
            make sure I am still the master */
            if (thedb->master != gbl_myhostname) {
                numerrs = 1;
                rc = ERR_NOMASTER; /*this is what bdb readonly error gets us */
                GOTOBACKOUT;
            }
        }

        if (iq->tranddl) {
            if (gbl_replicate_local && get_dbtable_by_name("comdb2_oplog") &&
                !gbl_replicate_local_concurrent) {
                long long seqno;
                rc = get_next_seqno(trans, iq->sc_tran, &seqno);
                if (rc) {
                    if (rc != RC_INTERNAL_RETRY)
                        logmsg(LOGMSG_ERROR,
                               "get_next_seqno unexpected rc %d\n", rc);
                    GOTOBACKOUT;
                }
                p_blkstate->seqno = seqno;
            }
        }

        if (needbackout) {
            GOTOBACKOUT;
        }

        /*numerrs should be 0 for osql mode, since no
          updates are actually performed up to this point */
        if (gbl_prefault_udp)
            send_prefault_udp = 1;
        rc = osql_bplog_commit(iq, trans, &tmpnops, &err);
        if (rc == ERR_VERIFY) {
            check_serializability = 1;
            force_serial_error = 1;
        }
        send_prefault_udp = 0;

        if (iq->osql_step_ix) {
            gbl_osqlpf_step[*(iq->osql_step_ix)].rqid = 0;
            gbl_osqlpf_step[*(iq->osql_step_ix)].step = 0;
            Pthread_mutex_lock(&osqlpf_mutex);
            queue_add(gbl_osqlpf_stepq, iq->osql_step_ix);
            Pthread_mutex_unlock(&osqlpf_mutex);
            iq->osql_step_ix = NULL;
        }

        delayed = iq->sorese->is_delayed ? 1 : 0;

        if (rc) {
            numerrs = 1;
            GOTOBACKOUT;
        } else {
            nops += tmpnops;
            iq->sorese->nops = nops;
        }
    }

    /* clear prefixes */
    if (iq->debug)
        reqpopprefixes(iq, -1);

    /* do all previously not done ADD key ops here--they were delayed due
     * to necessity of constraint checks */
    thrman_wheref(thr_self, "%s [constraints]", req2a(iq->opcode));
    blkpos = -1;
    ixout = -1;
    errout = 0;

    if (delayed || gbl_goslow || osql_is_index_reorder_on(iq->osql_flags)) {

        if (osql_is_index_reorder_on(iq->osql_flags)) {
            if (iq->debug)
                reqpushprefixf(iq, "%p process_defered_table: ", trans);
            rc = process_defered_table(iq, trans, &blkpos, &ixout, &errout);
            if (iq->debug)
                reqpopprefixes(iq, 1);

            if (rc != 0) {
                opnum = blkpos; /* so we report the failed blockop accurately */
                err.blockop_num = blkpos;
                err.errcode = errout;
                err.ixnum = ixout;
                numerrs = 1;
                reqlog_set_error(iq->reqlogger, "Process Defered Table", rc);
                GOTOBACKOUT;
            }
        }

        if (iq->debug)
            reqpushprefixf(iq, "%p delayed_key_adds: ", trans);

        int verror = 0;
        rc = delayed_key_adds(iq, trans, &blkpos, &ixout, &errout);

        if (iq->debug)
            reqpopprefixes(iq, 1);

        if (rc != 0) {
            if (iq->vfy_idx_track == 1 && iq->dup_key_insert == 1) {
                rc = ERR_UNCOMMITTABLE_TXN;
                errout = ERR_UNCOMMITTABLE_TXN;
                reqerrstr(iq, COMDB2_CSTRT_RC_DUP, "Transaction is uncommittable: "
                                                   "Duplicate insert on key '%s' "
                                                   "in table '%s' index %d",
                      get_keynm_from_db_idx(iq->usedb, ixout),
                      iq->usedb->tablename, ixout);
            } else {
                check_serializability = 1;
            }
            opnum = blkpos; /* so we report the failed blockop accurately */
            err.blockop_num = blkpos;
            err.errcode = errout;
            err.ixnum = ixout;
            numerrs = 1;
            reqlog_set_error(iq->reqlogger, "Delayed Key Adds", rc);
            GOTOBACKOUT;
        }

        /* check foreign key constraints */

        if (iq->debug)
            reqpushprefixf(iq, "%p verify_del_constraints: ", trans);
        rc = verify_del_constraints(iq, trans, &verror);
        if (iq->debug)
            reqpopprefixes(iq, 1);

        if (rc != 0) {
            check_serializability = 1;
            err.blockop_num = 0;
            err.errcode = verror;
            err.ixnum = -1;
            numerrs = 1;
            reqlog_set_error(iq->reqlogger, "Verify Del Constraints", rc);
            GOTOBACKOUT;
        }

        if (iq->debug)
            reqpushprefixf(iq, "%p verify_add_constraints: ", trans);

        rc = verify_add_constraints(iq, trans, &verror);
        if (iq->debug)
            reqpopprefixes(iq, 1);

        if (rc != 0) {
            check_serializability = 1;
            err.blockop_num = 0;
            err.errcode = verror;
            err.ixnum = -1;
            numerrs = 1;
            reqlog_set_error(iq->reqlogger, "Verify Add Constraints", rc);
            GOTOBACKOUT;
        }

        /* Fake a verify error */
        extern int gbl_toblock_random_verify_error;
        if (!rc && gbl_toblock_random_verify_error && (rand() % 100) == 0) {
            logmsg(LOGMSG_USER, "%s throwing random verify error\n", __func__);
            outrc = ERR_BLOCK_FAILED;
            rc = ERR_VERIFY;
            check_serializability = 1;
            opnum = blkpos; /* so we report the failed blockop accurately */
            err.blockop_num = blkpos;
            err.errcode = ERR_VERIFY;
            err.ixnum = ixout;
            numerrs = 1;
            reqlog_set_error(iq->reqlogger, "Debug random verify error", rc);
            GOTOBACKOUT;
        }
    } /* end delayed */
    else {
        ++gbl_delayed_skip;
    }

    if (gbl_replicate_local && iq->oplog_numops > 0) {
        /* write the commit record.  this is "soft" commit - things
           can still go wrong that'll cause it to abort and that's ok.
           only write the commit if we have logged other operations as part
           of this txn. */
        if (gbl_replicate_local_concurrent) {
            /* this is the transaction sequence number.  Note that
             * get_next_seqno is
             * a serialization point - no other transactions can proceed until
             * the one that called get_next_seqno commits.  */
            long long seqno;
            struct ireq aiq;

            rc = get_next_seqno(trans, NULL, &seqno);
            if (rc)
                GOTOBACKOUT;

            init_fake_ireq(thedb, &aiq);
            aiq.jsph = iq->jsph;

            rc = add_local_commit_entry(&aiq, trans, seqno, p_blkstate->seqno,
                                        iq->oplog_numops);
        } else {
            rc = add_oplog_entry(iq, trans, LCL_OP_COMMIT, NULL, 0);
        }
        if (rc)
            GOTOBACKOUT;
    }

    Pthread_rwlock_rdlock(&commit_lock);
    hascommitlock = 1;
    if (iq->arr || iq->selectv_arr) {
        // serializable read-set validation
        Pthread_rwlock_unlock(&commit_lock);
        Pthread_rwlock_wrlock(&commit_lock);
        hascommitlock = 1;

        /* This is looking for a commit record - one dive is fine */
        while ((iq->arr &&
                bdb_osql_serial_check(thedb->bdb_env, iq->arr, &(iq->arr->file),
                                      &(iq->arr->offset), 1)) ||
               (iq->selectv_arr &&
                bdb_osql_serial_check(thedb->bdb_env, iq->selectv_arr,
                                      &(iq->selectv_arr->file),
                                      &(iq->selectv_arr->offset), 1))) {
            Pthread_rwlock_unlock(&commit_lock);
            hascommitlock = 0;

            if (iq->selectv_arr &&
                bdb_osql_serial_check(thedb->bdb_env, iq->selectv_arr,
                                      &(iq->selectv_arr->file),
                                      &(iq->selectv_arr->offset), 0)) {
                currangearr_free(iq->selectv_arr);
                iq->selectv_arr = NULL;
                numerrs = 1;

                /* verify error */
                err.ixnum = -1; /* data */
                err.errcode = ERR_CONSTR;

                rc = ERR_CONSTR;
                reqerrstr(iq, COMDB2_CSTRT_RC_INVL_REC, "selectv constraints");
                GOTOBACKOUT;
            } else if (iq->arr && bdb_osql_serial_check(
                                      thedb->bdb_env, iq->arr, &(iq->arr->file),
                                      &(iq->arr->offset), 0)) {
                currangearr_free(iq->arr);
                iq->arr = NULL;
                numerrs = 1;
                rc = ERR_NOTSERIAL;

                /* Check selectv_arr in backout code */
                if (iq->selectv_arr)
                    check_serializability = 1;
                reqerrstr(iq, ERR_NOTSERIAL, "transaction is not serializable");
                GOTOBACKOUT;
            } else {
                Pthread_rwlock_wrlock(&commit_lock);
                hascommitlock = 1;
            }
        }

        if (iq->arr) {
            currangearr_free(iq->arr);
            iq->arr = NULL;
        }
        if (iq->selectv_arr) {
            currangearr_free(iq->selectv_arr);
            iq->selectv_arr = NULL;
        }
    }

    /* no errors yet - clean up blob buffers */
    free_blob_buffers(blobs, MAXBLOBS);

    /* From this point onwards Java triggers should not be allowed to write
     * the db using our transaction.  However we still need iq to be valid
     * so that javasp_reqprint() can be called successfully. */
    javasp_trans_set_trans(javasp_trans_handle, iq, trans, NULL);

    /* COMMIT CHILD TRANSACTION */
    thrman_wheref(thr_self, "%s [child commit]", req2a(iq->opcode));

    /* if this is a logical transaction, we DON'T commit the child it as we dont
       have nested logical transactions.  we need to delay the commit until the
       point where we would write the blkseq using the parent transaction,
       and call tran_commit_logical_with_blkseq instead */
    if (!rowlocks && parent_trans && !iq->tranddl) {
        /*fprintf(stderr, "commit child\n");*/
        irc = trans_commit(iq, trans, source_host);
        if (irc != 0) { /* this shouldnt happen */
            logmsg(LOGMSG_FATAL, "%s:%d TRANS_COMMIT FAILED RC %d", __func__,
                   __LINE__, irc);
            comdb2_die(0);
        }
        trans = NULL;
    }

    if (iq->debug) {
        if (is_block2sqlmode)
            reqprintf(iq, "TRANSACTION COMMITTED, NUM_ROWS WRITTEN %d", nops);
        else
            reqprintf(iq, "TRANSACTION COMMITTED, NUM_REQS %d", num_reqs);
    }

    /* starting writes, no more reads */
    iq->p_buf_in = NULL;
    iq->p_buf_in_end = NULL;

    p_buf_rsp_start = iq->p_buf_out;

    if (!have_keyless_requests) {
        struct block_rsp rsp;

        /* pack up response. */

        rsp.num_completed = opnum;

        if (!(iq->p_buf_out =
                  block_rsp_put(&rsp, iq->p_buf_out, iq->p_buf_out_end, iq->comdbg_flags)))
            /* TODO can I just return here? should prob go to cleanup ? */
            return ERR_INTERNAL;

        /* rcodes */
        if (!(iq->p_buf_out = buf_zero_put(sizeof(int) * num_reqs,
                                           iq->p_buf_out, iq->p_buf_out_end)))
            /* TODO can I just return here? should prob go to cleanup ? */
            return ERR_INTERNAL;

        /* rrns */
        for (jj = 0; jj < num_reqs; jj++) {
            int rrn;

            rrn = (jj < opnum) ? 2 : 0;
            if (!(iq->p_buf_out = buf_put(&rrn, sizeof(rrn), iq->p_buf_out,
                                          iq->p_buf_out_end)))
                /* TODO can I just return here? should prob go to cleanup ? */
                return ERR_INTERNAL;
        }

        /* borcodes */
        if (!(iq->p_buf_out = buf_zero_put(sizeof(int) * num_reqs,
                                           iq->p_buf_out, iq->p_buf_out_end)))
            /* TODO can I just return here? should prob go to cleanup ? */
            return ERR_INTERNAL;
    } else {
        if (iq->is_block2positionmode) {
            struct block_rspkl_pos rspkl_pos;

            if (is_block2sqlmode)
                rspkl_pos.num_completed = nops;
            else
                rspkl_pos.num_completed = opnum;

            rspkl_pos.position = iq->last_genid;

            if (numerrs) {
                rspkl_pos.numerrs = 1;
            } else {
                rspkl_pos.numerrs = 0;
                bzero(&err, sizeof(err));
            }

            if (!(iq->p_buf_out = block_rspkl_pos_put(&rspkl_pos, iq->p_buf_out,
                                                      iq->p_buf_out_end)))
                /* TODO can I just return here? should prob go to cleanup ? */
                return ERR_INTERNAL;

            if (!(iq->p_buf_out =
                      block_err_put(&err, iq->p_buf_out, iq->p_buf_out_end)))
                /* TODO can I just return here? should prob go to cleanup ? */
                return ERR_INTERNAL;

        } else {
            struct block_rspkl rspkl;

            if (is_block2sqlmode)
                rspkl.num_completed = nops;
            else
                rspkl.num_completed = opnum;

            if (numerrs) {
                rspkl.numerrs = 1;
            } else {
                rspkl.numerrs = 0;
                bzero(&err, sizeof(err));
            }

            if (!(iq->p_buf_out = block_rspkl_put(&rspkl, iq->p_buf_out,
                                                  iq->p_buf_out_end)))
                /* TODO can I just return here? should prob go to cleanup ? */
                return ERR_INTERNAL;

            if (!(iq->p_buf_out =
                      block_err_put(&err, iq->p_buf_out, iq->p_buf_out_end)))
                /* TODO can I just return here? should prob go to cleanup ? */
                return ERR_INTERNAL;
        }
    }

    outrc = RC_OK;

    goto add_blkseq;

/*------ERROR CONDITION------*/

backout:
    /* wait-die deadlock on distributed txn -> verify-error */
    if (rc == RC_INTERNAL_RETRY && iq->sorese && iq->sorese->dist_txnid && waitdie_deadlock) {
        extern long long gbl_distributed_deadlock_count;
        gbl_distributed_deadlock_count++;
        rc = ERR_VERIFY;
        outrc = ERR_BLOCK_FAILED;
        err.errcode = ERR_VERIFY;
        reqlog_set_error(iq->reqlogger, "Distributed deadlock", rc);
    }

    if (gbl_verbose_toblock_backouts) {
        unsigned long long rqid = iq->sorese ? iq->sorese->rqid : -1;
        uuidstr_t us = "(none)";
        if (iq->sorese) {
            comdb2uuidstr(iq->sorese->uuid, us);
        }
        logmsg(LOGMSG_ERROR, "Backing out, rc=%d outrc=%d rqid=%lld uuid=%s from line %d\n", rc, outrc, rqid, us,
               fromline);
    }

    if (!reqlog_get_error_code(iq->reqlogger))
        reqlog_set_error(iq->reqlogger, "Error Processing", rc);
    backed_out = 1;

    /* We don't have to prove serializability here, but if we have both a 
     * serializable error and a dup-key constraint we should return the 
     * serializable error as the dup-key may have been caused by the 
     * conflicting write. */
    if (check_serializability) {
        /* Check serial and selectv readsets independently, serial first to
         * eliminate the race: selectv-error should override serial error */
        if (iq->arr && (force_serial_error ||
                               bdb_osql_serial_check(thedb->bdb_env, iq->arr,
                                                     &(iq->arr->file),
                                                     &(iq->arr->offset), 0))) {
            numerrs = 1;
            rc = ERR_NOTSERIAL;
            reqerrstr(iq, ERR_NOTSERIAL, "transaction is not serializable");
        }

        if (iq->selectv_arr &&
            bdb_osql_serial_check(thedb->bdb_env, iq->selectv_arr,
                                  &(iq->selectv_arr->file),
                                  &(iq->selectv_arr->offset), 0)) {
            numerrs = 1;
            /* verify error */
            err.ixnum = -1; /* data */
            err.errcode = ERR_CONSTR;

            rc = ERR_CONSTR;
            reqerrstr(iq, COMDB2_CSTRT_RC_INVL_REC, "selectv constraints");
        } 
    }

    /* Cursor ranges need freeing on error, too. */
    currangearr_free(iq->arr);
    iq->arr = NULL;
    currangearr_free(iq->selectv_arr);
    iq->selectv_arr = NULL;

    /* starting writes, no more reads */
    iq->p_buf_in = NULL;
    iq->p_buf_in_end = NULL;

    p_buf_rsp_start = iq->p_buf_out;

    thrman_wheref(thr_self, "%s [backout]", req2a(iq->opcode));

    if (iq->debug) {
        reqpopprefixes(iq, -1);
        reqpushprefixf(iq, "%p:", trans);
    }

    /* free blob buffers, they won't be needed now */
    free_blob_buffers(blobs, MAXBLOBS);

    /* ABORT TRANSACTION - EXPECTS rc TO BE SET */

    /* If it's a logical transaction we can't abort here either - still
       need to write the blkseq.  The exception is if it's a retry, in
       which case we have to abort. */
    if (!rowlocks || rc == RC_INTERNAL_RETRY) {
        if (osql_needtransaction != OSQL_BPLOG_NOTRANS) {
            int priority = 0;

            if (iq->tranddl) {
                if (iq->sc_close_tran) {
                    if (iq->sc_closed_files)
                        irc =
                            trans_commit(iq, iq->sc_close_tran, gbl_myhostname);
                    else
                        irc = trans_abort(iq, iq->sc_close_tran);
                    if (irc != 0) {
                        logmsg(LOGMSG_FATAL, "%s:%d TRANS_%s FAILED RC %d\n",
                               __func__, __LINE__,
                               iq->sc_closed_files ? "COMMIT" : "ABORT", irc);
                        comdb2_die(0);
                    }
                    iq->sc_close_tran = NULL;
                }
                if (iq->sc_tran) {
                    irc = trans_abort(iq, iq->sc_tran);
                    if (irc != 0) {
                        logmsg(LOGMSG_FATAL, "%s:%d TRANS_ABORT FAILED RC %d",
                               __func__, __LINE__, irc);
                        comdb2_die(1);
                    }
                    iq->sc_tran = NULL;
                }

                /* Backout Schema Change */
                if (!parent_trans)
                    backout_schema_changes(iq, trans);
                else {
                    irc = trans_abort_priority(iq, trans, &priority);
                    if (irc != 0) {
                        logmsg(LOGMSG_FATAL, "%s:%d TRANS_ABORT FAILED RC %d",
                               __func__, __LINE__, irc);
                        comdb2_die(1);
                    }
                    trans = NULL;
                    backout_schema_changes(iq, parent_trans);
                }
            }
            if (trans) {
                irc = trans_abort_priority(iq, trans, &priority);
                if (irc != 0) {
                    logmsg(LOGMSG_FATAL, "%s:%d TRANS_ABORT FAILED RC %d",
                           __func__, __LINE__, irc);
                    comdb2_die(1);
                }
                trans = NULL;
            }

            if (bdb_attr_get(thedb->bdb_attr,
                             BDB_ATTR_DEADLOCK_LEAST_WRITES_EVER)) {
                if (verbose_deadlocks)
                    logmsg(LOGMSG_ERROR,
                           "%" PRIxPTR "%s:%d Setting iq %p priority from %d to %d\n",
                           (intptr_t)pthread_self(), __FILE__, __LINE__, iq,
                           iq->priority, priority);

                if (((unsigned)priority) == UINT_MAX) {
                    /*
                       the deadlock victim was a transaction with no write
                       locks;
                       since we were evicted once, set priority to old priority
                       + 1
                       to give a chance for progress
                     */
                    priority = iq->priority + 1;
                }

                // TODO: Dorin, this still throws here:
                // assert (iq->priority <= priority);

                iq->priority = priority;
            }
        } else {
            assert(trans == NULL);
            if (iq->tranddl) {
                BDB_READLOCK("sc_downgrade");
                if (thedb->master != gbl_myhostname) {
                    backout_schema_changes(iq, NULL);
                }
                bdb_rellock(thedb->bdb_env, __func__, __LINE__);
            }
        }

        if (rc == ERR_UNCOMMITTABLE_TXN) {
            logmsg(
                LOGMSG_ERROR,
                "Forced VERIFY-FAIL for uncommittable blocksql transaction\n");
            err.errcode = ERR_UNCOMMITTABLE_TXN;
            rc = ERR_UNCOMMITTABLE_TXN;
        }

        if (rc == RC_INTERNAL_RETRY) {
            /* abandon our parent if we have one, we're gonna redo it all */
            if (osql_needtransaction != OSQL_BPLOG_NOTRANS && parent_trans) {
                if (iq->tranddl)
                    backout_and_abort_tranddl(iq, parent_trans, 0);
                else
                    trans_abort(iq, parent_trans);
                parent_trans = NULL;
            }

            /* restore any part of the req we have saved */
            if (block_state_restore(iq, p_blkstate))
                /* TODO can I just return here? should prob go to cleanup ? */
                return ERR_INTERNAL;

            outrc = RC_INTERNAL_RETRY;
            fromline = __LINE__;
            goto cleanup;
        }
    }

    if (iq->debug)
        reqprintf(iq, "%p:TRANSACTION ABORTED, RC %d ERRCODE %d", trans, rc,
                  err.errcode);

    lrc = pack_up_response(iq, have_keyless_requests, num_reqs, numerrs, &err,
                           rc, opnum);
    if (lrc) /* TODO can I just return here? should prob go to cleanup ? */
        return lrc;

#if 0
    printf("**toblock error reply_len %d rc %d\n", iq->reply_len,
            ERR_BLOCK_FAILED);
#endif

    /* I really don't understand how we return ERR_DUP to the client.. -- SJ */
    /* wouldn't it be nice not to jump through these hoops? */
    switch (rc) {
    case ERR_NO_RECORDS_FOUND:
    case ERR_CONVERT_DTA:
    case ERR_NULL_CONSTRAINT:
    case ERR_SQL_PREP:
    case ERR_CONSTR:
    case ERR_UNCOMMITTABLE_TXN:
    case ERR_NOMASTER:
    case ERR_NOTSERIAL:
    case ERR_DIST_ABORT:
    case ERR_SC:
    case ERR_TRAN_TOO_BIG:
        outrc = rc;
        if (iq->sorese)
            iq->sorese->rcout = outrc;
        break;
    default:
        outrc = ERR_BLOCK_FAILED;

        if (have_keyless_requests) {
/* hack alert: block proc sends back a 2 error codes and the
 * client convert that to a error number and a string we
 * provide here the similar trick for computing the number */
#if 0
                iq->errstat.errval = outrc+err.errcode;
#endif
            if (iq->sorese)
                iq->sorese->rcout = outrc + err.errcode;
        }

        break;
    }

    /* if this was canning a block processor, don't save so that the retry is
       gonna execute fine */
    if (rc == ERR_NOMASTER && have_blkseq) {
        if (gbl_master_swing_osql_verbose)
            logmsg(LOGMSG_USER, "%" PRIxPTR "%s:%d Skipping add blkseq due to early "
                                "bplog termination\n",
                   (intptr_t)pthread_self(), __FILE__, __LINE__);

        /* Abort disttxn on master swing */
        if (iq->sorese && iq->sorese->dist_txnid) {
            abort_disttxn(iq, rc, outrc);
        }

        /* we need to abort the logical/parent transaction
           we'll skip the rest of statistics */
        if (rowlocks) {
            if (iq->tranddl)
                backout_and_abort_tranddl(iq, trans, 1);
            irc = trans ? trans_abort_logical(iq, trans, NULL, 0, NULL, 0) : 0;
        } else {
            if (iq->tranddl) {
                if (trans) {
                    trans_abort(iq, trans);
                    trans = NULL;
                }
                backout_and_abort_tranddl(iq, parent_trans, 0);
            } else
                trans_abort(iq, parent_trans);
            parent_trans = NULL;
        }
        fromline = __LINE__;
        goto cleanup;
    }

/*
   now add to the blkseq db with either the success or failure of the child
   */
add_blkseq:
    thrman_wheref(thr_self, "%s [blkseq]", req2a(iq->opcode));

    iq->timings.replication_start = osql_log_time();
    if (have_blkseq) {
        uint8_t buf_fstblk[FSTBLK_MAX_BUF_LEN];
        uint8_t *p_buf_fstblk = buf_fstblk;
        const uint8_t *p_buf_fstblk_end = buf_fstblk + sizeof(buf_fstblk);

        /* add the response array as a dta record */
        if (!have_keyless_requests) {
            struct block_rsp rsp;
            size_t num_rrns;
            struct fstblk_header fstblk_header;
            const uint8_t *p_buf_rsp_unpack = p_buf_rsp_start;

            /* unpack the rsp that we just packed above (there is almost
             * certainly a more efficient way to do this) */
            if (!(p_buf_rsp_unpack =
                      block_rsp_get(&rsp, p_buf_rsp_unpack, iq->p_buf_out))) {
                /* TODO can I just return here? should prob go to cleanup ? */
                return ERR_INTERNAL;
            }

            if (outrc == ERR_BLOCK_FAILED) {
                struct fstblk_rsperr fstblk_rsperr;

                fstblk_header.type = (short)FSTBLK_RSPERR;
                fstblk_rsperr.num_completed = (short)rsp.num_completed;
                /*bzero(&fstblk_rsperr.pad0, sizeof(fstblk_rsperr.pad0));*/

                /* unpack the rcode of the op we err'd on */
                if (!buf_get(&fstblk_rsperr.rcode, sizeof(fstblk_rsperr.rcode),
                             p_buf_rsp_unpack +
                                 (sizeof(int) * rsp.num_completed),
                             iq->p_buf_out)) {
                    /* TODO can I just return here? should prob go to
                     * cleanup ? */
                    return ERR_INTERNAL;
                }

                if (!(p_buf_fstblk = fstblk_header_put(
                          &fstblk_header, p_buf_fstblk, p_buf_fstblk_end))) {
                    /* TODO can I just return here? should prob go to
                     * cleanup ? */
                    return ERR_INTERNAL;
                }

                if (!(p_buf_fstblk = fstblk_rsperr_put(
                          &fstblk_rsperr, p_buf_fstblk, p_buf_fstblk_end))) {
                    /* TODO can I just return here? should prob go to
                     * cleanup ? */
                    return ERR_INTERNAL;
                }

                num_rrns = fstblk_rsperr.num_completed;
            } else {
                struct fstblk_rspok fstblk_rspok;

                fstblk_header.type = (short)FSTBLK_RSPOK;
                fstblk_rspok.fluff = (short)0;
                /*bzero(&fstblk_rspok.pad0, sizeof(fstblk_rspok.pad0));*/

                if (!(p_buf_fstblk = fstblk_header_put(
                          &fstblk_header, p_buf_fstblk, p_buf_fstblk_end))) {
                    /* TODO can I just return here? should prob go to
                     * cleanup ? */
                    return ERR_INTERNAL;
                }

                if (!(p_buf_fstblk = fstblk_rspok_put(
                          &fstblk_rspok, p_buf_fstblk, p_buf_fstblk_end))) {
                    /* TODO can I just return here? should prob go to
                     * cleanup ? */
                    return ERR_INTERNAL;
                }

                num_rrns = num_reqs;
            }

            /* make sure the number of return codes and rrns we expect
             * were actually packed */
            if ((sizeof(int) * (num_reqs + num_rrns)) >
                (iq->p_buf_out - p_buf_rsp_unpack)) {
                /* TODO can I just return here? should prob go to cleanup ? */
                return ERR_INTERNAL;
            }

            /* skip over the packed return codes */
            p_buf_rsp_unpack += sizeof(int) * num_reqs;

            /* copy over packed rrns */
            if (!(p_buf_fstblk =
                      buf_no_net_put(p_buf_rsp_unpack, sizeof(int) * num_rrns,
                                     p_buf_fstblk, p_buf_fstblk_end))) {
                /* TODO can I just return here? should prob go to cleanup ? */
                return ERR_INTERNAL;
            }
        } else {
            struct fstblk_header fstblk_header;
            struct fstblk_pre_rspkl fstblk_pre_rspkl;

            fstblk_header.type =
                (short)(IQ_HAS_SNAPINFO(iq) ? FSTBLK_SNAP_INFO : FSTBLK_RSPKL);
            fstblk_pre_rspkl.fluff = (short)0;

            if (!(p_buf_fstblk = fstblk_header_put(&fstblk_header, p_buf_fstblk,
                                                   p_buf_fstblk_end))) {
                /* TODO can I just return here? should prob go to cleanup ? */
                return ERR_INTERNAL;
            }

            if (!(p_buf_fstblk = fstblk_pre_rspkl_put(
                      &fstblk_pre_rspkl, p_buf_fstblk, p_buf_fstblk_end))) {
                /* TODO can I just return here? should prob go to cleanup ? */
                return ERR_INTERNAL;
            }

            if (IQ_HAS_SNAPINFO(iq)) {
                if (!(p_buf_fstblk = buf_put(&(outrc), sizeof(outrc), p_buf_fstblk, 
                                p_buf_fstblk_end))) {
                            return ERR_INTERNAL;
                }
                if (outrc != 0) {
                    if (!(p_buf_fstblk = osqlcomm_errstat_type_put(
                              &(iq->errstat), p_buf_fstblk,
                              p_buf_fstblk_end))) {
                        return ERR_INTERNAL;
                    }
                }
                if (!(p_buf_fstblk = osqlcomm_query_effects_put(
                          &(IQ_SNAPINFO(iq)->effects), p_buf_fstblk,
                          p_buf_fstblk_end))) {
                    return ERR_INTERNAL;
                }
            }

            if (!(p_buf_fstblk = buf_no_net_put(
                      p_buf_rsp_start, iq->p_buf_out - p_buf_rsp_start,
                      p_buf_fstblk, p_buf_fstblk_end))) {
                /* TODO can I just return here? should prob go to cleanup ? */
                return ERR_INTERNAL;
            }
        }

        void *replay_data = NULL;
        int replay_len = 0;

        void *bskey;
        int bskeylen;
        /* Snap_info is our blkseq key */
        if (IQ_HAS_SNAPINFO_KEY(iq)) {
            bskey = IQ_SNAPINFO(iq)->key;
            bskeylen = IQ_SNAPINFO(iq)->keylen;
        } else {
            bskey = iq->seq;
            bskeylen = iq->seqlen;
        }
        int t = comdb2_time_epoch();
        int can_retry = (IQ_HAS_SNAPINFO(iq) && IQ_SNAPINFO(iq)->replicant_is_able_to_retry);
        memcpy(p_buf_fstblk, &t, sizeof(int));

        if (!rowlocks) {
            // if VERIFY-ERROR && replicant_is_able_to_retry don't add to blkseq
            if ((outrc == ERR_NOTSERIAL || (outrc == ERR_BLOCK_FAILED && err.errcode == ERR_VERIFY)) && can_retry) {
            } else {
                rc = bdb_blkseq_insert(thedb->bdb_env, parent_trans, bskey, bskeylen, buf_fstblk,
                                       p_buf_fstblk - buf_fstblk + sizeof(int), &replay_data, &replay_len, 0);
                if (debug_switch_test_ddl_backout_blkseq())
                    rc = -1;
                if (!rc && gbl_debug_blkseq_race && !(rand() % 5)) {
                    logmsg(LOGMSG_INFO, "Sleep 10 to debug blkseq\n");
                    sleep(10);
                }
            }

            if (rc == 0 && have_blkseq) {
                if (iq->tranddl) {
                    if (backed_out) {
                        assert(trans == NULL);
                        if (iq->sc_logical_tran)
                            bdb_ltran_put_schema_lock(iq->sc_logical_tran);
                    } else {
                        assert(iq->sc_tran);
                        assert(trans != NULL);
                        trans_commit(iq, trans, source_host);
                        trans = NULL;

                        if (iq->sc_close_tran) {
                            if (iq->sc_closed_files)
                                irc = trans_commit(iq, iq->sc_close_tran,
                                                   source_host);
                            else
                                irc = trans_abort(iq, iq->sc_close_tran);
                            if (irc != 0) {
                                logmsg(LOGMSG_FATAL,
                                       "%s:%d TRANS_%s FAILED RC %d\n",
                                       __func__, __LINE__,
                                       iq->sc_closed_files ? "COMMIT" : "ABORT",
                                       irc);
                                comdb2_die(0);
                            }
                            iq->sc_close_tran = NULL;
                        }

                        irc = trans_commit(iq, iq->sc_tran, source_host);
                        if (irc != 0) { /* this shouldnt happen */
                            logmsg(LOGMSG_FATAL,
                                   "%s:%d TRANS_COMMIT FAILED RC %d", __func__,
                                   __LINE__, irc);
                            comdb2_die(0);
                        }
                        iq->sc_tran = NULL;
                    }
                    if (iq->sc_locked) {
                        unlock_schema_lk();
                        iq->sc_locked = 0;
                    }
                    if (iq->sc_logical_tran) {
                        irc = trans_commit_logical(iq, iq->sc_logical_tran,
                                                   gbl_myhostname, 0, 1, NULL,
                                                   0, NULL, 0);
                    }
                    assert(outrc || iq->sc_running == 0);
                    iq->sc_logical_tran = NULL;
                } else {
                    /* TODO: Prevent 2pc txns from running in serializable isolation, as that
                     * would require holding the commit-lock in write mode long-term.  */
                    if (iq->sorese && iq->sorese->dist_txnid) {
                        int bdberr, prepare_rc;
                        char *cname = iq->sorese->is_coordinator ? gbl_dbname : iq->sorese->coordinator_dbname;
                        char *ctier = iq->sorese->is_coordinator ? "_coordinator_local" : iq->sorese->coordinator_tier;
                        if (outrc == 0) {
                            /* Write prepare record & wait for it to replicate */
                            prepare_rc = bdb_tran_prepare(thedb->bdb_env, parent_trans, iq->sorese->dist_txnid, cname,
                                                          ctier, 0, bskey, bskeylen, &bdberr);
                            /* Failed to propagate prepare record to quorum */
                            if (prepare_rc != 0) {
                                logmsg(LOGMSG_ERROR, "Failed to prepare %s from %s:%s: %d\n", iq->sorese->dist_txnid,
                                       iq->sorese->coordinator_dbname, iq->sorese->coordinator_tier, prepare_rc);
                                if (iq->sorese->is_participant) {
                                    participant_has_failed(iq->sorese->dist_txnid, iq->sorese->coordinator_dbname,
                                                           iq->sorese->coordinator_master, ERR_NOT_DURABLE,
                                                           ERR_BLOCK_FAILED, "Prepare was not durable");
                                } else {
                                    coordinator_failed(iq->sorese->dist_txnid);
                                    rc = BDBERR_NOT_DURABLE;
                                    outrc = ERR_BLOCK_FAILED;
                                }

                                /* Prepare failed to reach a majority of the cluster: fail the txn */
                                dist_txn_abort_write_blkseq(thedb->bdb_env, bskey, bskeylen);
                                trans_abort(iq, parent_trans);

                                /* Wrote & replicated prepare record */
                            } else {

                                Pthread_mutex_lock(&blklk);
                                prepared_count++;
                                Pthread_mutex_unlock(&blklk);

                                /* Block on coordinator/participants */
                                int waitrc = -1, should_wait = (can_retry || gbl_debug_wait_on_verify_off);
                                if (iq->sorese->is_coordinator) {
                                    waitrc = coordinator_wait(iq->sorese->dist_txnid, should_wait, &err.errcode, &outrc,
                                                              iq->errstat.errstr, ERRSTAT_STR_SZ, 0);
                                } else {
                                    assert(iq->sorese->is_participant);
                                    waitrc =
                                        participant_wait(iq->sorese->dist_txnid, iq->sorese->coordinator_dbname,
                                                         iq->sorese->coordinator_tier, iq->sorese->coordinator_master);
                                }

                                /* We have committed dist-txn durably: commit & replicate */
                                if (waitrc == HAS_COMMITTED) {
                                    Pthread_mutex_lock(&blklk);
                                    prepared_count--;
                                    Pthread_mutex_unlock(&blklk);
                                    irc = trans_commit_adaptive(iq, parent_trans, source_host);
                                    if (iq->sorese->is_coordinator) {
                                        if (gbl_coordinator_wait_propagate) {
                                            coordinator_wait_propagate(iq->sorese->dist_txnid);
                                        } else {
                                            coordinator_resolve(iq->sorese->dist_txnid);
                                        }
                                    }
                                    if (iq->sorese->is_participant) {
                                        participant_has_propagated(iq->sorese->dist_txnid,
                                                                   iq->sorese->coordinator_dbname,
                                                                   iq->sorese->coordinator_master);
                                    }
                                    /* Disttxn has failed- abort & update rcode */
                                } else if (waitrc == HAS_ABORTED) {
                                    Pthread_mutex_lock(&blklk);
                                    prepared_count--;
                                    Pthread_mutex_unlock(&blklk);
                                    if (iq->sorese->is_coordinator) {
                                        assert(outrc != 0);
                                        assert(err.errcode != 0);
                                        if (!should_rewrite_rcode(outrc)) {
                                            iq->sorese->rcout = outrc;
                                        } else {
                                            outrc = ERR_BLOCK_FAILED;
                                            iq->sorese->rcout = outrc + err.errcode;
                                        }
                                    }
                                    if (gbl_debug_disttxn_trace) {
                                        logmsg(LOGMSG_USER,
                                               "DISTTXN %s line %d aborting %s coord=%d part=%d rc=%d outrc=%d "
                                               "errmsg=%s\n",
                                               __func__, __LINE__, iq->sorese->dist_txnid, iq->sorese->is_coordinator,
                                               iq->sorese->is_participant, err.errcode, outrc, iq->errstat.errstr);
                                    }
                                    if ((outrc == ERR_NOTSERIAL ||
                                         (outrc == ERR_BLOCK_FAILED && err.errcode == ERR_VERIFY)) &&
                                        can_retry) {
                                    } else {
                                        dist_txn_abort_write_blkseq(thedb->bdb_env, bskey, bskeylen);
                                    }
                                    trans_abort(iq, parent_trans);

                                    /* Downgrade, but disttxn commit record hasn't replicated */
                                } else {

                                    /* Have been told to release locks because 'lock-is-desired'
                                     * but releasing locks means something else could overwrite our
                                     * changes.  This is not allowed because this is prepared!
                                     *
                                     * Upgrade-downgrade-reopen-wrap invokes 'abort_prepared_waiters',
                                     * which aborts all transactions which are blocked on prepared
                                     * transactions.  So block here, waiting for all non-prepared
                                     * transactions to resolve. */

                                    assert(waitrc == LOCK_DESIRED);
                                    logmsg(LOGMSG_INFO, "%s disttxn %s failed to commit durably\n", __func__,
                                           iq->sorese->dist_txnid);
                                    Pthread_mutex_lock(&blklk);
                                    while (prepared_count < blkcnt) {
                                        struct timespec waittime;
                                        bdb_abort_waiters(thedb->bdb_env, parent_trans);
                                        setup_waittime(&waittime, 1000);
                                        pthread_cond_timedwait(&blkcd, &blklk, &waittime);
                                        logmsg(LOGMSG_INFO,
                                               "Blocking non-prepared threads to resolve, prepared=%d total=%d\n",
                                               prepared_count, blkcnt);
                                    }
                                    prepared_count--;
                                    Pthread_mutex_unlock(&blklk);
                                    logmsg(LOGMSG_INFO, "%s disttxn discarding prepared txn %s\n", __func__,
                                           iq->sorese->dist_txnid);

                                    /* Assert that nothing wants my locks in write-mode */
                                    int wrwaiters = bdb_tran_count_write_waiters(thedb->bdb_env, parent_trans);
                                    if (wrwaiters > 0) {
                                        logmsg(LOGMSG_FATAL, "Dist-txn %s has %u write-waiters\n",
                                               iq->sorese->dist_txnid, wrwaiters);
                                        abort();
                                    }
                                    trans_discard_prepared(iq, parent_trans);
                                }
                            }
                        } else {
                            if (iq->sorese->is_coordinator) {
                                if ((outrc == ERR_NOTSERIAL ||
                                     (outrc == ERR_BLOCK_FAILED && err.errcode == ERR_VERIFY)) &&
                                    (can_retry || gbl_debug_wait_on_verify_off)) {
                                    char errstr[ERRSTAT_STR_SZ] = {0};
                                    int prc = 0, poutrc = 0, waitrc,
                                        should_wait = (can_retry || gbl_debug_wait_on_verify_off);
                                    waitrc = coordinator_wait(iq->sorese->dist_txnid, should_wait, &prc, &poutrc,
                                                              errstr, ERRSTAT_STR_SZ, 1);
                                    /* Coordinator-wait wants us to change the errorcode */
                                    if (waitrc != KEEP_RCODE) {
                                        if (!should_rewrite_rcode(poutrc)) {
                                            iq->sorese->rcout = poutrc;
                                        } else {
                                            iq->sorese->rcout = poutrc + prc;
                                        }
                                    }
                                } else if (gbl_debug_disttxn_trace) {
                                    logmsg(LOGMSG_USER,
                                           "%s DISTTXN %s not waiting, outrc=%d rc=%d err.errcode=%d canretry=%d\n",
                                           __func__, iq->sorese->dist_txnid, outrc, rc, err.errcode, can_retry);
                                }

                                if (gbl_debug_disttxn_trace) {
                                    logmsg(LOGMSG_USER, "%s DISTTXN %s coord failing disttxn outrc=%d\n", __func__,
                                           iq->sorese->dist_txnid, outrc);
                                }
                                coordinator_failed(iq->sorese->dist_txnid);
                            }
                            if (iq->sorese->is_participant) {
                                participant_has_failed(iq->sorese->dist_txnid, iq->sorese->coordinator_dbname,
                                                       iq->sorese->coordinator_master, err.errcode, outrc,
                                                       iq->errstat.errstr);
                            }
                            irc = trans_commit_adaptive(iq, parent_trans, source_host);
                        }
                    } else if (!debug_prepare_testcase()) {
                        irc = trans_commit_adaptive(iq, parent_trans, source_host);
                        /* 2pc testcases */
                    } else {
                        debug_prepare_tests(iq, parent_trans, source_host, bskey, bskeylen);
                    }
                    parent_trans = NULL;
                }
                if (irc) {
                    /* We've committed to the btree, but we are not replicated:
                     * ask the the client to retry */
                    if (irc == BDBERR_NOT_DURABLE &&
                        (bdb_attr_get(thedb->bdb_attr, BDB_ATTR_DURABLE_LSNS) || gbl_replicant_retry_on_not_durable)) {
                        rc = ERR_NOT_DURABLE;
                    }
                    logmsg(LOGMSG_DEBUG, "trans_commit_adaptive irc=%d, "
                            "rc=%d\n", irc, rc);
                }

                if (hascommitlock) {
                    Pthread_rwlock_unlock(&commit_lock);
                    hascommitlock = 0;
                }
                if (gbl_dump_blkseq && IQ_HAS_SNAPINFO_KEY(iq)) {
                    char *bskey = alloca(IQ_SNAPINFO(iq)->keylen + 1);
                    memcpy(bskey, IQ_SNAPINFO(iq)->key,
                           IQ_SNAPINFO(iq)->keylen);
                    bskey[IQ_SNAPINFO(iq)->keylen] = '\0';
                    logmsg(LOGMSG_USER,
                           "blkseq add '%s', outrc=%d errval=%d "
                           "errstr='%s', rcout=%d commit-rc=%d\n",
                           bskey, outrc, iq->errstat.errval, iq->errstat.errstr,
                           iq->sorese ? iq->sorese->rcout : 0, irc);
                }
                /* else case for (rc == 0 && have_blkseq) */
            } else {
                if (hascommitlock) {
                    Pthread_rwlock_unlock(&commit_lock);
                    hascommitlock = 0;
                }
                if (iq->tranddl) {
                    if (trans) {
                        trans_abort(iq, trans);
                        trans = NULL;
                    }
                    backout_and_abort_tranddl(iq, parent_trans, 0);
                } else {
                    if (iq->sorese && iq->sorese->dist_txnid && rc != RC_INTERNAL_RETRY) {
                        if (iq->sorese->is_coordinator) {
                            if ((outrc == ERR_NOTSERIAL || (outrc == ERR_BLOCK_FAILED && err.errcode == ERR_VERIFY)) &&
                                can_retry) {
                                char errstr[ERRSTAT_STR_SZ] = {0};
                                int prc = 0, poutrc = 0, waitrc;
                                waitrc = coordinator_wait(iq->sorese->dist_txnid, can_retry, &prc, &poutrc, errstr,
                                                          ERRSTAT_STR_SZ, 1);
                                if (waitrc) {
                                    if (!should_rewrite_rcode(poutrc)) {
                                        iq->sorese->rcout = poutrc;
                                    } else {
                                        iq->sorese->rcout = poutrc + prc;
                                    }
                                }
                            }

                            if (gbl_debug_disttxn_trace) {
                                logmsg(LOGMSG_USER, "%s DISTTXN %s coord failing disttxn outrc=%d\n", __func__,
                                       iq->sorese->dist_txnid, outrc);
                            }
                            coordinator_failed(iq->sorese->dist_txnid);
                        }
                        if (iq->sorese->is_participant) {
                            participant_has_failed(iq->sorese->dist_txnid, iq->sorese->coordinator_dbname,
                                                   iq->sorese->coordinator_master, err.errcode, outrc,
                                                   iq->errstat.errstr);
                        }
                    }

                    trans_abort(iq, parent_trans);
                }
                parent_trans = NULL;
                if (rc == IX_DUP) {
                    logmsg(LOGMSG_WARN, "%" PRIxPTR "%s:%d replay detected!\n",
                           (intptr_t)pthread_self(), __FILE__, __LINE__);
                    outrc = do_replay_case(iq, bskey, bskeylen, num_reqs, 0,
                                           replay_data, replay_len, __LINE__);
#if DEBUG_DID_REPLAY
                    did_replay = 1;
#endif
                    logmsg(LOGMSG_DEBUG, "%" PRIxPTR "%s:%d replay returned %d!\n",
                           (intptr_t)pthread_self(), __FILE__, __LINE__, outrc);
                    fromline = __LINE__;

                    goto cleanup;
                }

                if (rc == RC_INTERNAL_RETRY) {
                    /* restore any part of the req we have saved */
                    if (block_state_restore(iq, p_blkstate))
                        /* TODO can I just return here? should prob go to
                         * cleanup ? */
                        return ERR_INTERNAL;

                    outrc = RC_INTERNAL_RETRY;
                    fromline = __LINE__;

                    /* lets bump the priority if we got killed here */
                    if (bdb_attr_get(thedb->bdb_attr,
                                     BDB_ATTR_DEADLOCK_LEAST_WRITES_EVER)) {
                        iq->priority += bdb_attr_get(
                            thedb->bdb_attr,
                            BDB_ATTR_DEADLK_PRIORITY_BUMP_ON_FSTBLK);
                    }
                    goto cleanup;
                }

                outrc = rc;
                fromline = __LINE__;
                goto cleanup;
            }
        } else /* rowlocks */
        {
            /* commit or abort the trasaction as appropriate,
               and write the blkseq */
            if (!backed_out) {
                /*fprintf(stderr, "trans_commit_logical\n");*/
                if (iq->tranddl) {
                    if (iq->sc_close_tran) {
                        if (iq->sc_closed_files)
                            irc = trans_commit(iq, iq->sc_close_tran,
                                               source_host);
                        else
                            irc = trans_abort(iq, iq->sc_close_tran);
                        if (irc != 0) {
                            logmsg(LOGMSG_FATAL, "%s:%d TRANS_%s FAILED RC %d",
                                   __func__, __LINE__,
                                   iq->sc_closed_files ? "COMMIT" : "ABORT",
                                   irc);
                            comdb2_die(0);
                        }
                        iq->sc_close_tran = NULL;
                    }

                    irc = trans_commit(iq, iq->sc_tran, source_host);
                    if (irc != 0) { /* this shouldnt happen */
                        logmsg(LOGMSG_FATAL, "%s:%d TRANS_COMMIT FAILED RC %d",
                               __func__, __LINE__, irc);
                        comdb2_die(0);
                    }
                    iq->sc_tran = NULL;
                }
                if (iq->sc_locked) {
                    unlock_schema_lk();
                    iq->sc_locked = 0;
                }
                /* TODO: private blkseq with rowlocks? */
                rc = trans_commit_logical(
                    iq, trans, gbl_myhostname, 0, 1, buf_fstblk,
                    p_buf_fstblk - buf_fstblk + sizeof(int), bskey, bskeylen);

                if (hascommitlock) {
                    Pthread_rwlock_unlock(&commit_lock);
                    hascommitlock = 0;
                }

                if (rc == BDBERR_NOT_DURABLE)
                    rc = ERR_NOT_DURABLE;
            } else {
                if (hascommitlock) {
                    Pthread_rwlock_unlock(&commit_lock);
                    hascommitlock = 0;
                }
                if (iq->tranddl)
                    backout_and_abort_tranddl(iq, trans, 1);
                rc = trans_abort_logical(
                    iq, trans, buf_fstblk,
                    p_buf_fstblk - buf_fstblk + sizeof(int), bskey, bskeylen);

                if (rc == BDBERR_NOT_DURABLE)
                    rc = ERR_NOT_DURABLE;
            }
        }

        if (rc != 0) {
            /* if it's a logical transaction and the commit fails we abort
             * inside the commit call */
            if (rc == IX_DUP) {
                logmsg(LOGMSG_WARN, "%" PRIxPTR "%s:%d replay detected!\n",
                       (intptr_t)pthread_self(), __FILE__, __LINE__);
                outrc = do_replay_case(iq, bskey, bskeylen, num_reqs, 0,
                                       replay_data, replay_len, __LINE__);
#if DEBUG_DID_REPLAY
                did_replay = 1;
#endif
                logmsg(LOGMSG_DEBUG, "%" PRIxPTR "%s:%d replay returned %d!\n",
                       (intptr_t)pthread_self(), __FILE__, __LINE__, outrc);
                fromline = __LINE__;

                goto cleanup;
            }

            if (rc == RC_INTERNAL_RETRY) {
                /* restore any part of the req we have saved */
                if (block_state_restore(iq, p_blkstate))
                    /* TODO can I just return here? should prob go to cleanup ?
                     */
                    return ERR_INTERNAL;

                outrc = RC_INTERNAL_RETRY;
                fromline = __LINE__;
                goto cleanup;
            }

            outrc = rc;
            fromline = __LINE__;
            goto cleanup;
        }
    } else /* no blkseq */
    {
        thrman_wheref(thr_self, "%s [commit and replicate]", req2a(iq->opcode));

        /*
        if (iq->sorese)
        {
            fprintf(stderr, "i don't have a blkseq & sorese is set? from %s\n",
        iq->frommach);
        }
        */

        if (rowlocks) {
            if (rc) {
                if (hascommitlock) {
                    Pthread_rwlock_unlock(&commit_lock);
                    hascommitlock = 0;
                }
                irc = trans_abort_logical(iq, trans, NULL, 0, NULL, 0);
                if (irc == BDBERR_NOT_DURABLE)
                    irc = ERR_NOT_DURABLE;
            } else {

                irc = trans_commit_logical(iq, trans, gbl_myhostname, 0, 1,
                                           NULL, 0, NULL, 0);
                if (irc == BDBERR_NOT_DURABLE)
                    irc = ERR_NOT_DURABLE;

                if (hascommitlock) {
                    Pthread_rwlock_unlock(&commit_lock);
                    hascommitlock = 0;
                }
            }
        } else {
            /*fprintf(stderr, "commiting parent\n");*/
            irc = 0;
            if (trans) {

                irc = trans_commit_adaptive(iq, trans, source_host);
                if (irc == BDBERR_NOT_DURABLE)
                    irc = rc = ERR_NOT_DURABLE;
            }
            if (hascommitlock) {
                Pthread_rwlock_unlock(&commit_lock);
                hascommitlock = 0;
            }
        }

        if (irc != 0) {
            if (iq->debug)
                reqprintf(iq, "%p:PARENT TRANSACTION COMMIT FAILED RC %d",
                          parent_trans, rc);

            if (irc == RC_INTERNAL_RETRY) {
                /* restore any part of the req we have saved */
                if (block_state_restore(iq, p_blkstate))
                    /* TODO can I just return here? should prob go to
                     * cleanup ? */
                    return ERR_INTERNAL;

                outrc = RC_INTERNAL_RETRY;
                fromline = __LINE__;
                goto cleanup;
            }
            outrc = irc;
            fromline = __LINE__;
            goto cleanup;
        }
    }

    /* At this stage it's not a replay so we either committed a transaction
     * or we had to abort. */
    if (outrc == 0) {
        /* Committed new sqlite_stat1 statistics from analyze - reload sqlite
         * engines */
        iq->dbenv->txns_committed++;
        if (iq->dbglog_file) {
            dbglog_dump_write_stats(iq);
            sbuf2close(iq->dbglog_file);
            iq->dbglog_file = NULL;
        }
    } else {
        iq->dbenv->txns_aborted++;
    }
    reqlog_set_nwrites(iq->reqlogger, iq->written_row_count, iq->cascaded_row_count);

    /* update stats (locklessly so we may get gibberish - I know this
     * and don't care) */
    if (iq->total_txnsize > iq->dbenv->biggest_txn)
        iq->dbenv->biggest_txn = iq->total_txnsize;
    iq->dbenv->total_txn_sz = iq->total_txnsize;
    iq->dbenv->num_txns++;
    if (iq->timeoutms > iq->dbenv->max_timeout_ms)
        iq->dbenv->max_timeout_ms = iq->timeoutms;
    iq->dbenv->total_timeouts_ms += iq->timeoutms;
    if (iq->reptimems > iq->dbenv->max_reptime_ms)
        iq->dbenv->max_reptime_ms = iq->reptimems;
    iq->dbenv->total_reptime_ms += iq->reptimems;

    if (iq->debug) {
        uint64_t rate;
        if (iq->reptimems)
            rate = iq->total_txnsize / iq->reptimems;
        else
            rate = 0;
        reqprintf(iq,
                  "%p:TRANSACTION SIZE %llu TIMEOUT %d REPTIME %d RATE "
                  "%llu",
                  trans, iq->total_txnsize, iq->timeoutms, iq->reptimems, rate);
    }

    int diff_time_micros = (int)reqlog_current_us(iq->reqlogger);

    ATOMIC_ADD64(n_commit_time, diff_time_micros);
    ATOMIC_ADD32(n_commits, 1);

    if (outrc == 0) {
        if (iq->__limits.maxcost_warn &&
            (iq->cost > iq->__limits.maxcost_warn)) {
            logmsg(LOGMSG_WARN, "[%s] warning: transaction exceeded cost threshold "
                            "(%f >= %f)\n",
                    iq->corigin, iq->cost, iq->__limits.maxcost_warn);
        }
    }

    fromline = __LINE__;
cleanup:
#if DEBUG_DID_REPLAY
    logmsg(LOGMSG_DEBUG, "%s cleanup rc %d did_replay:%d fromline:%d\n",
           __func__, outrc, did_replay, fromline);
#endif
    // legacy_sndbak is being done from an SQL thread, which will hold a curtran lock, so skip this check
    if (!iq->ipc_sndbak)
        bdb_checklock(thedb->bdb_env);

    iq->timings.req_finished = osql_log_time();
    /*printf("Set req_finished=%llu\n", iq->timings.req_finished);*/
    iq->timings.retries++;

    /* clear in memory blkseq entry
       if this is not a osql request, this is fine
       the iq will not be hashed and this is a nop
     */
    if (outrc != RC_INTERNAL_RETRY)
        osql_blkseq_unregister(iq);

    /* XXX
       This can't happen .. we wait-for-seqnum on the commit for failed transactions

      wait for last committed seqnum on abort, in case we are racing.
         thread 1:  select -> not found ; insert
         thread 2:  insert -> got dupe? -> select -> NOT FOUND?!
      thread 2 can race with thread 1, this lets the abort wait
    if (backed_out)
        trans_wait_for_last_seqnum(iq, source_host);
    */

    return outrc;
}

static int blkmax = 0;

int reset_blkmax(void)
{
    blkmax = 0;
    return 0;
}

int get_blkmax(void) { return blkmax; }

static uint64_t block_processor_ms = 0;

static int toblock_main(struct javasp_trans_state *javasp_trans_handle, struct ireq *iq, block_state_t *p_blkstate)
{
    int rc, prcnt = 0, prmax = 0;
    static uint64_t lastpr = 0;

    uint64_t start = gettimeofday_ms();
    uint64_t end;

    Pthread_mutex_lock(&blklk);
    if (bdb_lock_desired(thedb->bdb_env)) {
        Pthread_mutex_unlock(&blklk);
        return ERR_REJECTED;
    }

    blkcnt++;
    if (start - lastpr > 1000) {
        prcnt = blkcnt;
        lastpr = start;
    }

    if (blkcnt > blkmax)
        blkmax = blkcnt;

    prmax = blkmax;

    Pthread_mutex_unlock(&blklk);

    if (prcnt && gbl_print_blockp_stats) {
        logmsg(LOGMSG_USER,
               "%d threads are in the block processor, max is %d\n", prcnt,
               prmax);
    }

    if (iq->tptlock) {
        if (iq->tranddl) {
            /* avoid a lock inversion here; simply forbit mixing bpfunc that
             * requires tpt lock with other schema changes
             */
            reqlog_set_error(
                iq->reqlogger,
                "Error cannot mix tpt retention with other schema changes",
                rc = -1);
            logmsg(
                LOGMSG_ERROR,
                "Error cannot mix tpt retention with other schema changes\n");
            end = gettimeofday_ms();
            goto done;
        }
        views_lock();
    }
    rc = toblock_main_int(javasp_trans_handle, iq, p_blkstate);
    end = gettimeofday_ms();

    extern int gbl_all_prepare_leak;
    if (!gbl_all_prepare_leak)
        bdb_assert_notran(thedb->bdb_env);

    if (rc == 0) {
        osql_postcommit_handle(iq);
        handle_postcommit_bpfunc(iq);
    } else {
        osql_postabort_handle(iq);
        handle_postabort_bpfunc(iq);
    }

    if (iq->tptlock)
        views_unlock();

done:

    assert(iq->sc_running == 0);
    txn_logbytes = NULL;

    Pthread_mutex_lock(&blklk);
    blkcnt--;
    Pthread_cond_broadcast(&blkcd);
    block_processor_ms += (end - start);
    Pthread_mutex_unlock(&blklk);

    if (prcnt && gbl_print_blockp_stats) {
        logmsg(LOGMSG_USER,
               "%" PRIu64 " total time spent in the block processor\n",
               block_processor_ms);
    }

    return rc;
}

/* Callback function for range delete.  Forms the given key on demand.
 *
 * The callback implementation should return:
 *  0   - key formed successfully, go ahead and delete this record.
 *  -1  - do not delete this record, but continue the range delete operation.
 *  -2  - halt the range delete operation with an error.
 */
static int keyless_range_delete_formkey(void *record, size_t record_len,
                                        void *index, size_t index_len,
                                        int index_num, void *userptr)
{
    rngdel_info_t *rngdel_info = userptr;
    struct ireq *iq = rngdel_info->iq;
    int ixkeylen;
    int rc;

    ixkeylen = getkeysize(iq->usedb, index_num);
    if (ixkeylen < 0 || ixkeylen != index_len) {
        if (iq->debug)
            reqprintf(iq, "%p:RNGDELKL CALLBACK BAD INDEX %d OR KEYLENGTH %d",
                      rngdel_info->parent_trans, index_num, ixkeylen);
        reqerrstr(iq, COMDB2_DEL_RC_INVL_KEY,
                  "rngdelkl callback bad index %d or keylength %d", index_num,
                  ixkeylen);
        return -2;
    }

    rc = stag_ondisk_to_ix(iq->usedb, index_num, record, index);
    if (rc == -1) {
        if (iq->debug)
            reqprintf(iq, "%p:RNGDELKL CALLBACK CANT FORM INDEX %d",
                      rngdel_info->parent_trans, index_num);
        rngdel_info->rc = ERR_CONVERT_DTA;
        rngdel_info->err = OP_FAILED_CONVERSION;
        return -1;
    }

    return 0;
}

/* The pre delete callback currently gets called so that we can find the
 * blobs for the record about to be deleted.  Then we can pass them to
 * java post delete listeners. */
static int keyless_range_delete_pre_delete(void *record, size_t record_len,
                                           int rrn, unsigned long long genid,
                                           void *userptr)
{
    rngdel_info_t *rngdel_info = userptr;

    if (rngdel_info->saveblobs) {
        int rc;

        free_blob_status_data(rngdel_info->oldblobs);

        rc = save_old_blobs(rngdel_info->iq, rngdel_info->trans, ".ONDISK",
                            record, rrn, genid, rngdel_info->oldblobs);

        /* should not fail - if there is any error then cancel the operation. */
        if (rc != 0) {
            rngdel_info->rc = rc;
            rngdel_info->err = OP_FAILED_CONVERSION;
            return -1;
        }
    }

    return 0;
}

/* The post delete callback gets called after record deletion; here we
 * notify Java of the operation. */
static int keyless_range_delete_post_delete(void *record, size_t record_len,
                                            int rrn, unsigned long long genid,
                                            void *userptr)
{
    rngdel_info_t *rngdel_info = userptr;

    /* notify our Java stored procedures of this deletion. */
    if (rngdel_info->notifyjava) {
        int rc;
        struct javasp_rec *jrec;
        jrec = javasp_alloc_rec(record, record_len,
                                rngdel_info->iq->usedb);
        if (rngdel_info->saveblobs)
            javasp_rec_set_blobs(jrec, rngdel_info->oldblobs);
        rc = javasp_trans_tagged_trigger(
            rngdel_info->javasp_trans_handle, JAVASP_TRANS_LISTEN_AFTER_DEL,
            jrec, NULL, rngdel_info->iq->usedb->tablename);
        javasp_dealloc_rec(jrec);
        if (rngdel_info->iq->debug)
            reqprintf(rngdel_info->iq,
                      "%p:RNGDELKL JAVASP_TRANS_LISTEN_AFTER_DEL %d",
                      rngdel_info->parent_trans, rc);
        if (rc != 0) {
            rngdel_info->err = OP_FAILED_INTERNAL + ERR_JAVASP_ABORT_OP;
            rngdel_info->rc = rc;
            return -1;
        }
    }

    return 0;
}

int get_next_seqno(void *tran, void *sc_tran, long long *seqno)
{
    /* key is a long long + int + descriptor bytes for each */
    char fndkey[14];
    int rc;
    int fndlen;
    int outnull, outsz;
    struct ireq iq;
    int64_t stored_seqno = 0;

    init_fake_ireq(thedb, &iq);

    if (gbl_replicate_local_concurrent)
        iq.usedb = get_dbtable_by_name("comdb2_commit_log");
    else
        iq.usedb = get_dbtable_by_name("comdb2_oplog");

    /* HUH? */
    if (iq.usedb == NULL) {
        logmsg(LOGMSG_USER, "get_next_seqno: no comdb2_oplog table\n");
        return -1;
    }

    /* find it.  set the write hint so we lock the record once found */
    rc = ix_fetch_last_key_tran(&iq, tran, 1, 0, sizeof(fndkey), fndkey,
                                &fndlen);
    if (rc)
        return rc;
    if (fndlen == 0) {
        bdb_get_seqno(sc_tran ? sc_tran : tran, &stored_seqno);
        *seqno = stored_seqno;
    } else {
        long long next_seqno = 0;

        /* convert to client format value so we can use it. Call the Routine
         * Of Many Arguments to do so. */
        rc = SERVER_BINT_to_CLIENT_INT(fndkey, 9, NULL, NULL, seqno,
                                       sizeof(long long), &outnull, &outsz,
                                       NULL, NULL);
        if (rc)
            return rc;
        /* This gives us a big-endian value, which is great, if we aren't on a
         * little-endian
         * machine. */
        buf_get(&next_seqno, sizeof(long long), (uint8_t *)seqno,
                (uint8_t *)seqno + sizeof(long long));
        next_seqno++;

        *seqno = next_seqno;
    }
    return 0;
}
