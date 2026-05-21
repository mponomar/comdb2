#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>
#include <cdb2api.h>

void hexdump(const char *label, const void *data, size_t size)
{
    printf("%s (size %zu):\n", label, size);
    const unsigned char *bytes = (const unsigned char *)data;
    for (size_t i = 0; i < size; i++) {
        printf("%02x ", bytes[i]);
        if ((i + 1) % 16 == 0)
            printf("\n");
    }
    if (size % 16 != 0)
        printf("\n");
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <dbname> <host> <payload_size>\n", argv[0]);
        return 1;
    }

    const char *dbname = argv[1];
    const char *host = argv[2];
    size_t payload_size = (size_t)atol(argv[3]);
    cdb2_hndl_tp *hndl;
    int rc;

    int lux = 0;
    int flags = 0;
    struct req_hdr {
        int ver1;
        short ver2;
        unsigned char luxref;
        unsigned char opcode;
    };
    struct block_req {
        int flags;
        int offset;
        int num_reqs;
        /*more packed reqs... */
    };
    struct packedreq_hdr {
        int opcode;
        int nxt;
    };
    struct req_hdr hdr = {0};
    struct block_req breq = {0};
    struct packedreq_hdr preq = {0};
    preq.opcode = 800;
    preq.nxt = 0;
    const size_t minsize = sizeof(struct req_hdr) + sizeof(struct block_req) + sizeof(struct packedreq_hdr) + 12;
    if (payload_size < minsize) {
        fprintf(stderr, "payload_size must be at least %zd\n", minsize);
        return 1;
    }
    void *buf = calloc(1, payload_size);
    // bad blockop - no requests - the response should be a meaningful error
    hdr.opcode = 100;
    breq.num_reqs = htonl(1);
    memcpy(buf, &hdr, sizeof(hdr));
    breq.offset = htonl(sizeof(struct req_hdr)+sizeof(struct block_req));
    memcpy((char*)buf+sizeof(struct req_hdr), &breq, sizeof(breq));
    memcpy((char*)buf+sizeof(struct req_hdr)+sizeof(struct block_req), &preq, sizeof(preq));

    rc = cdb2_open(&hndl, dbname, host, CDB2_SET_TAGGED | CDB2_DIRECT_CPU);
    if (rc) {
        fprintf(stderr, "cdb2_open(%s, %s) rc=%d: %s\n",
                dbname, host, rc, cdb2_errstr(hndl));
        return 1;
    }

    cdb2_bind_param(hndl, "buffer", CDB2_BLOB, buf, (int)payload_size);
    cdb2_bind_param(hndl, "lux", CDB2_INTEGER, &lux, sizeof(lux));
    cdb2_bind_param(hndl, "flags", CDB2_INTEGER, &flags, sizeof(flags));

    rc = cdb2_run_statement(hndl,
             "exec procedure comdb2_legacy(@buffer, @lux, @flags)");
    if (rc) {
        printf("run rc %d\n", rc);
        free(buf);
        cdb2_close(hndl);
        return 0;
    }

    int db_rc = *(int32_t *)cdb2_column_value(hndl, 0);
    // 220 is a "good" rcode - the request got dispatched to the block processor
    // 199 is a "bad" rcode - the request was rejected as invalid before being dispatched
    printf("db_rc %d\n", db_rc);

    // hexdump("response", cdb2_column_value(hndl, 1), cdb2_column_size(hndl, 1));

    free(buf);
    cdb2_close(hndl);
    return 0;
}
