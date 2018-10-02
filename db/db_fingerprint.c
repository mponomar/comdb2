#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>

#include "sql.h"
#include "sqliteInt.h"
#include "util.h"
#include "tohex.h"

#include <openssl/sha.h>

hash_t *gbl_fingerprint_hash = NULL;
pthread_mutex_t gbl_fingerprint_hash_mu = PTHREAD_MUTEX_INITIALIZER;

int gbl_fingerprint_max_queries = 1000;

int sqlite3IsId(sqlite3_stmt *p, const char *id);
int sqlite3IsLiteral(int op);


void add_fingerprint(unsigned char fingerprint[FINGERPRINTSZ], int64_t cost,
                     int64_t time, int64_t nrows, const char *normalized_sql) {
    struct fingerprint_track *t;
    static int complain_once = 1;

    /* Some SQL (eg: DDL) isn't fingerprinted.  Check early if that's the case and return. */
    if (fingerprint[0] == 0) {
        /* do a cheap check of fingerprints[0] first so we only check the entire fingerprint
         * a small portion of the time, since non-fingerprinted SQL should be relatively rare. */
        static const unsigned char zerofp[FINGERPRINTSZ] = {0};
        if (memcmp(zerofp, fingerprint, FINGERPRINTSZ) == 0)
            return;
    }

    int rc;
    pthread_mutex_lock(&gbl_fingerprint_hash_mu);
    if (gbl_fingerprint_hash == NULL)
       gbl_fingerprint_hash = hash_init(FINGERPRINTSZ);
    t = hash_find(gbl_fingerprint_hash, fingerprint);
    if (t == NULL) {
       /* make sure we haven't generated an unreasonable number of these */
       int nents;
       hash_info(gbl_fingerprint_hash, NULL, NULL, NULL, NULL, &nents, NULL, NULL);
       if (nents >= gbl_fingerprint_max_queries) {
           if (complain_once) {
               fprintf(stderr, "Stopped tracking fingerprints, hit max #queries %d.\n", gbl_fingerprint_max_queries);
               complain_once = 0;
           }
           goto err;
       }

       t = malloc(sizeof(struct fingerprint_track));
       memcpy(t->fingerprint, fingerprint, FINGERPRINTSZ);
       t->count = 1;
       t->cost = cost;
       t->time = time;
       t->rows = nrows;
       /* overestimate, but guaranteed to be no bigger */
       t->normalized_query = strdup(normalized_sql);
       hash_add(gbl_fingerprint_hash, t);

       if (gbl_verbose_normalized_queries) {
          char fp[FINGERPRINTSZ*2+1];
          util_tohex(fp, t->fingerprint, FINGERPRINTSZ);
          printf("[%s] -> %s\n", fp, t->normalized_query);
       }
    } else {
       t->count++;
       t->cost += cost;
       t->time += time;
       t->rows += nrows;
    }
err:
    pthread_mutex_unlock(&gbl_fingerprint_hash_mu);
}

void normalized_sql_to_fingerprint(const char *sql, char fingerprint[FINGERPRINTSZ]) {
    SHA_CTX c;
    SHA1_Init(&c);
    SHA1_Update(&c, sql, strlen(sql));
    SHA1_Final(fingerprint, &c);
}
