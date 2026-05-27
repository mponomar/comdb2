#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include <cdb2api.h>

#define OP_BLOCK             100
#define BLOCK2_USE           802
#define BLOCK2_QBLOB         804
#define BLOCK2_SEQV2         824
#define BLOCK2_DEBUG         826

#define DEBUG_QADD_RECNO     1
#define DEBUG_QCONSUME       2

#define BLKF_ERRSTAT         1
#define ERR_BLOCK_FAILED     220

#define REQ_HDR_LEN          8
#define BLOCK_REQ_LEN        12
#define PACKEDREQ_HDR_LEN    8
#define PACKEDREQ_USEKL_LEN  (4+4+(4*8))
#define PACKEDREQ_SEQ2_LEN   16
#define PACKEDREQ_QBLOB_LEN  (4+4+4+(4*5))

#define BLOCK_RSPKL_LEN      8
#define BLOCK_ERR_LEN        12
#define ERRSTAT_LEN           256

static void put_u32(uint8_t *dst, uint32_t val)
{
    uint32_t nval = htonl(val);
    memcpy(dst, &nval, 4);
}

static void put_u16(uint8_t *dst, uint16_t val)
{
    uint16_t nval = htons(val);
    memcpy(dst, &nval, 2);
}

static void put_bytes(uint8_t *dst, const void *src, size_t len)
{
    memcpy(dst, src, len);
}

static int one_based_word_offset(const uint8_t *start, const uint8_t *ptr)
{
    return ((int)(ptr - start) + 3) / 4 + 1;
}

static uint32_t get_u32(const uint8_t *src)
{
    uint32_t val;
    memcpy(&val, src, 4);
    return ntohl(val);
}

static cdb2_hndl_tp *db_hndl;

/*
 * Send a block request with sub-ops: USE, SEQV2, optional QBLOB, main op.
 * Returns the block-level rc (0 = success, ERR_BLOCK_FAILED = 220, etc).
 * Sets *errcode to the per-op error code on failure.
 */
static int send_debug_request(const char *name, int debug_op,
                              const uint8_t *payload, size_t payload_len,
                              const uint8_t *blob, size_t blob_len,
                              int *errcode)
{
    static const char USE_TABLE[] = "t";
    int use_taglen = (int)sizeof(USE_TABLE) - 1;
    size_t use_payload_len = PACKEDREQ_USEKL_LEN + use_taglen;
    size_t use_op_raw = PACKEDREQ_HDR_LEN + use_payload_len;
    size_t use_op_padded = (use_op_raw + 3) & ~(size_t)3;

    size_t seq_op_raw = PACKEDREQ_HDR_LEN + PACKEDREQ_SEQ2_LEN;
    size_t seq_op_padded = (seq_op_raw + 3) & ~(size_t)3;

    /* QBLOB sub-op (only if blob provided) */
    size_t qblob_payload_len = blob ? (PACKEDREQ_QBLOB_LEN + blob_len) : 0;
    size_t qblob_op_raw = blob ? (PACKEDREQ_HDR_LEN + qblob_payload_len) : 0;
    size_t qblob_op_padded = (qblob_op_raw + 3) & ~(size_t)3;

    /* main op: debug_op (4 bytes) + sub-op payload */
    size_t main_payload_len = 4 + payload_len;
    size_t main_op_raw = PACKEDREQ_HDR_LEN + main_payload_len;
    size_t main_op_padded = (main_op_raw + 3) & ~(size_t)3;

    int num_subops = blob ? 4 : 3;

    size_t total_len = REQ_HDR_LEN + BLOCK_REQ_LEN +
                       use_op_padded + seq_op_padded +
                       qblob_op_padded + main_op_padded;

    uint8_t *buf = calloc(1, total_len);
    if (!buf) {
        fprintf(stderr, "malloc failed for %s\n", name);
        return -1;
    }
    uint8_t *p = buf;

    /* req_hdr (8 bytes) */
    put_u32(p, 0);       p += 4;
    put_u16(p, 0);       p += 2;
    *p++ = 0;
    *p++ = (uint8_t)OP_BLOCK;

    /* block_req (12 bytes) - fill offsets later */
    uint8_t *block_req_pos = p;
    p += BLOCK_REQ_LEN;

    /* --- Sub-op: BLOCK2_USE --- */
    uint8_t *use_hdr_pos = p;
    p += PACKEDREQ_HDR_LEN;
    put_u32(p, 0); p += 4;                        /* dbnum */
    put_u32(p, (uint32_t)use_taglen); p += 4;     /* taglen */
    p += 4 * 8;                                    /* reserve[8] */
    put_bytes(p, USE_TABLE, use_taglen); p += use_taglen;
    p = buf + REQ_HDR_LEN + BLOCK_REQ_LEN + use_op_padded;

    /* --- Sub-op: BLOCK2_SEQV2 --- */
    uint8_t *seq_hdr_pos = p;
    p += PACKEDREQ_HDR_LEN;
    for (int i = 0; i < 16; i++)
        *p++ = (uint8_t)(rand() & 0xff);
    p = buf + REQ_HDR_LEN + BLOCK_REQ_LEN + use_op_padded + seq_op_padded;

    /* --- Sub-op: BLOCK2_QBLOB (optional) --- */
    uint8_t *qblob_hdr_pos = NULL;
    if (blob) {
        qblob_hdr_pos = p;
        p += PACKEDREQ_HDR_LEN;

        put_u32(p, 0); p += 4;                           /* blobno = 0 */
        put_u32(p, (uint32_t)blob_len); p += 4;          /* length */
        put_u32(p, (uint32_t)blob_len); p += 4;          /* frag_len */
        p += 4 * 5;                                       /* reserve[5] */
        put_bytes(p, blob, blob_len); p += blob_len;
        p = buf + REQ_HDR_LEN + BLOCK_REQ_LEN +
            use_op_padded + seq_op_padded + qblob_op_padded;
    }

    /* --- Sub-op: BLOCK2_DEBUG --- */
    uint8_t *op_hdr_pos = p;
    p += PACKEDREQ_HDR_LEN;
    put_u32(p, (uint32_t)debug_op); p += 4;
    if (payload_len > 0)
        memcpy(p, payload, payload_len);

    /* compute offsets */
    size_t off_after_use = REQ_HDR_LEN + BLOCK_REQ_LEN + use_op_padded;
    size_t off_after_seq = off_after_use + seq_op_padded;
    size_t off_after_qblob = off_after_seq + qblob_op_padded;
    int use_nxt = one_based_word_offset(buf, buf + off_after_use);
    int seq_nxt = one_based_word_offset(buf, buf + off_after_seq);
    int qblob_nxt = one_based_word_offset(buf, buf + off_after_qblob);
    int end_off = one_based_word_offset(buf, buf + total_len);

    /* fill block_req */
    put_u32(block_req_pos + 0, BLKF_ERRSTAT);
    put_u32(block_req_pos + 4, (uint32_t)end_off);
    put_u32(block_req_pos + 8, (uint32_t)num_subops);

    /* fill sub-op headers */
    put_u32(use_hdr_pos + 0, (uint32_t)BLOCK2_USE);
    put_u32(use_hdr_pos + 4, (uint32_t)use_nxt);

    put_u32(seq_hdr_pos + 0, (uint32_t)BLOCK2_SEQV2);
    put_u32(seq_hdr_pos + 4, blob ? (uint32_t)seq_nxt : (uint32_t)qblob_nxt);

    if (blob) {
        put_u32(qblob_hdr_pos + 0, (uint32_t)BLOCK2_QBLOB);
        put_u32(qblob_hdr_pos + 4, (uint32_t)qblob_nxt);
    }

    put_u32(op_hdr_pos + 0, (uint32_t)BLOCK2_DEBUG);
    put_u32(op_hdr_pos + 4, (uint32_t)end_off);

    /* Send via cdb2api */
    int rc;
    int32_t lux = 0;
    int32_t flags = 0;

    cdb2_clearbindings(db_hndl);

    rc = cdb2_bind_param(db_hndl, "buffer", CDB2_BLOB, buf, (int)total_len);
    if (rc) {
        fprintf(stderr, "%s: cdb2_bind_param(buffer) rc=%d: %s\n",
                name, rc, cdb2_errstr(db_hndl));
        free(buf);
        return -1;
    }

    rc = cdb2_bind_param(db_hndl, "lux", CDB2_INTEGER, &lux, sizeof(lux));
    if (rc) {
        fprintf(stderr, "%s: cdb2_bind_param(lux) rc=%d: %s\n",
                name, rc, cdb2_errstr(db_hndl));
        free(buf);
        return -1;
    }

    rc = cdb2_bind_param(db_hndl, "flags", CDB2_INTEGER, &flags, sizeof(flags));
    if (rc) {
        fprintf(stderr, "%s: cdb2_bind_param(flags) rc=%d: %s\n",
                name, rc, cdb2_errstr(db_hndl));
        free(buf);
        return -1;
    }

    rc = cdb2_run_statement(db_hndl,
             "exec procedure comdb2_legacy(@buffer, @lux, @flags)");
    if (rc) {
        fprintf(stderr, "%s: cdb2_run_statement rc=%d: %s\n",
                name, rc, cdb2_errstr(db_hndl));
        free(buf);
        return -1;
    }

    int db_rc = (int)*(int32_t *)cdb2_column_value(db_hndl, 0);

    *errcode = 0;
    if (db_rc == ERR_BLOCK_FAILED) {
        const uint8_t *rsp = cdb2_column_value(db_hndl, 1);
        int rsp_len = cdb2_column_size(db_hndl, 1);
        const uint8_t *body = rsp + REQ_HDR_LEN;
        int body_len = rsp_len - REQ_HDR_LEN;

        if (rsp && body_len >= BLOCK_RSPKL_LEN + BLOCK_ERR_LEN) {
            *errcode = (int)get_u32(body + BLOCK_RSPKL_LEN + 4);
        }
    }

    /* drain remaining rows */
    while (cdb2_next_record(db_hndl) == CDB2_OK)
        ;

    free(buf);
    return db_rc;
}

static const char QNAME[] = "testq";

static int test_qadd_recno(uint32_t recno)
{
    int qnamelen = (int)sizeof(QNAME) - 1;

    /* payload: reserved(4) + recno(4) + qnamelen(4) + qname */
    size_t payload_len = 4 + 4 + 4 + qnamelen;
    uint8_t *payload = calloc(1, payload_len);
    uint8_t *p = payload;

    put_u32(p, 0); p += 4;                       /* reserved */
    put_u32(p, recno); p += 4;                    /* recno */
    put_u32(p, (uint32_t)qnamelen); p += 4;       /* qnamelen */
    put_bytes(p, QNAME, qnamelen);

    /* queue message blob */
    uint8_t blob_data[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};

    int errcode = 0;
    int rc = send_debug_request("DEBUG_QADD_RECNO", DEBUG_QADD_RECNO,
                                payload, payload_len,
                                blob_data, sizeof(blob_data),
                                &errcode);
    free(payload);

    printf("DEBUG_QADD_RECNO recno=%u rc=%d errcode=%d\n", recno, rc, errcode);
    return rc;
}

static int test_qconsume(void)
{
    int qnamelen = (int)sizeof(QNAME) - 1;

    /* payload: qnamelen(4) + qname */
    size_t payload_len = 4 + qnamelen;
    uint8_t *payload = calloc(1, payload_len);
    uint8_t *p = payload;

    put_u32(p, (uint32_t)qnamelen); p += 4;       /* qnamelen */
    put_bytes(p, QNAME, qnamelen);

    int errcode = 0;
    int rc = send_debug_request("DEBUG_QCONSUME", DEBUG_QCONSUME,
                                payload, payload_len,
                                NULL, 0,
                                &errcode);
    free(payload);

    printf("DEBUG_QCONSUME rc=%d errcode=%d\n", rc, errcode);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <dbname> <master>\n", argv[0]);
        return 1;
    }

    const char *dbname = argv[1];
    char *master = argv[2];
    int rc;

    srand((unsigned)(time(NULL) ^ (getpid() << 16)));

    rc = cdb2_open(&db_hndl, dbname, master,
                   CDB2_SET_TAGGED | CDB2_DIRECT_CPU);
    if (rc) {
        fprintf(stderr, "cdb2_open(%s, %s) rc=%d: %s\n",
                dbname, master, rc, cdb2_errstr(db_hndl));
        return 1;
    }

    int failed = 0;
    // Try to sneak up on it?
    /* Add at UINT_MAX- and consume it */
    rc = test_qadd_recno(UINT_MAX-1);
    if (rc != 0) {
        fprintf(stderr, "FAIL: DEBUG_QADD_RECNO UINT_MAX expected rc=0 got rc=%d\n", rc);
        failed = 1;
    }
    if (!failed) {
        rc = test_qconsume();
        if (rc != 0) {
            fprintf(stderr, "FAIL: DEBUG_QCONSUME after UINT_MAX expected rc=0 got rc=%d\n", rc);
            failed = 1;
        }
    }

    /* append UINT_MAX and consume it, and then the next few subsequent records */
    for (int i = 0; i < 5 && !failed; i++) {
        rc = test_qadd_recno(0);
        if (rc != 0) {
            fprintf(stderr, "FAIL: DEBUG_QADD_RECNO UINT_MAX expected rc=0 got rc=%d\n", rc);
            failed = 1;
        }
        if (!failed) {
            rc = test_qconsume();
            if (rc != 0) {
                fprintf(stderr, "FAIL: DEBUG_QCONSUME after UINT_MAX expected rc=0 got rc=%d\n", rc);
                failed = 1;
            }
        }
    }

    cdb2_close(db_hndl);

    if (!failed)
        printf("SUCCESS\n");
    return failed;
}
