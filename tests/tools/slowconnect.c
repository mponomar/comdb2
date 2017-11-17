#include <pthread.h>

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>

#include <unistd.h>
#include <sys/time.h>

#include <cdb2api.h>

/* We link against a patched cdb2api that inserts a pause of 50ms after connecting, half the time, 
 * before sending a connect string.  Comdb2 will reject connections that take longer than 100ms to 
 * say hi.  Older versions of Comdb2 process all connections serially, so a slow connector can cause 
 * other connections to wait behind it.  Current version wait for all accepted connections to 
 * become ready at the same time, so at worst we should wait for the maximum pause time. 
 *
 * */

int64_t hrtime(void) {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (t.tv_sec * 1000000 + t.tv_usec) / 1000;
}

int maxtime = 200;

int have_errors = 0;
pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;

struct ret {
    int64_t runtime;
};

void* thd(void *dbnamep) {
    char *dbname = (char*) dbnamep;
    cdb2_hndl_tp *db;
    int rc;
    int64_t start, end;

    struct ret *ret;

    rc = cdb2_open(&db, dbname, "default", 0);
    if (rc) {
        fprintf(stderr, "open rc %d %s\n", rc, cdb2_errstr(db));
    }

    start = hrtime();
    rc = cdb2_run_statement(db, "select 1");
    if (rc) {
        fprintf(stderr, "run rc %d %s\n", rc, cdb2_errstr(db));
        pthread_mutex_lock(&lk);
        have_errors++;
        pthread_mutex_unlock(&lk);
        return NULL;
    }
    end = hrtime();
    ret = malloc(sizeof(struct ret));
    ret->runtime = end - start;
    cdb2_close(db);

    return ret;
}

int main(int argc, char *argv[]) {
    char *dbname;
    int numqueries, numruns;
    int rc;

    if (argc != 4) {
        fprintf(stderr, "Usage: dbname numqueries numruns\n");
        return 1;
    }
    numqueries = atoi(argv[2]);
    numruns = atoi(argv[3]);

    char *conf = getenv("CDB2_CONFIG");
    if (conf)
        cdb2_set_comdb2db_config(conf);

    cdb2_disable_sockpool();

    dbname = argv[1];
    pthread_t *tids;
    tids = calloc(numqueries, sizeof(pthread_t));

    for (int nruns = 0; nruns < 100; nruns++) {
        for (int i = 0; i < numqueries; i++) {
            rc = pthread_create(&tids[i], NULL, thd, (void*) dbname);
            if (rc) {
                fprintf(stderr, "can't create thread: %d %s\n", rc, strerror(rc));
                return 1;
            }
        }

        for (int i = 0; i < numqueries; i++) {
            void *p;
            struct ret *ret;
            rc = pthread_join(tids[i], &p);
            if (rc) {
                fprintf(stderr, "can't join thread: %d %s\n", rc, strerror(rc));
                return 1;
            }
            ret = (struct ret*) p;
            if (ret == NULL)
                have_errors++;
            else {
                printf("%d\n", ret->runtime);
                free(ret);
            }
        }
    }
    return have_errors;
}
