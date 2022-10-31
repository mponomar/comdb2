/*
   Copyright 2022 Bloomberg Finance L.P.

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

#if (!defined(SQLITE_CORE) || defined(SQLITE_BUILDING_FOR_COMDB2)) &&          \
    !defined(SQLITE_OMIT_VIRTUALTABLE)

#if defined(SQLITE_BUILDING_FOR_COMDB2) && !defined(SQLITE_CORE)
#define SQLITE_CORE 1
#endif

#include <unistd.h>
#include "comdb2.h"
#include "sqlite3.h"
#include "comdb2systblInt.h"
#include "sql.h"
#include "ezsystables.h"
#include "bdb_api.h"
#include "bdb_systable.h"

static int addSomeBloat(
        sqlite3_vtab *pVTab,
        int argc,
        sqlite3_value **argv,
        sqlite_int64 *pRowid
);

static sqlite3_module systblLogBloatModule = {
    .access_flag = CDB2_ALLOW_USER,
    .xUpdate = addSomeBloat
};

static int no_op_alloc(void **data, int *npoints) {
    *npoints = 0;
    *data = NULL;
    return 0;
}

static void no_op_free(void *data, int npoints) {
}

struct bloatrec {
    int64_t size;
    int64_t sleep;
};

int systblLogBloat(sqlite3 *db) {
    int rc = create_system_table(db, "comdb2_log_bloat", &systblLogBloatModule,
            no_op_alloc, no_op_free, sizeof(struct bloatrec),
                                 CDB2_INTEGER, "size", -1, offsetof(struct bloatrec, size),
                                 CDB2_INTEGER, "sleep", -1, offsetof(struct bloatrec, sleep),
            SYSTABLE_END_OF_FIELDS);
    return rc;
}

// This is the xUpdate handler.  It gets called on the replicant side.  Its job is to generate an opcode
// to the master to actually generate the bloat.  This call isn't to be confused with process_bloat
// which is what's called on the replicant when the bloat is logged and processed.
static int addSomeBloat(
        sqlite3_vtab *pVTab,
        int argc,
        sqlite3_value **argv,
        sqlite_int64 *pRowid
) {
    /* https://www.sqlite.org/vtab.html#the_xupdate_method
     * The interface to xUpdate requires it to handle insert/update/delete - combination of argc and argv[0]
     * determines what the operation is.  We only care about INSERT - everything else is ignored and reported
     * as success.
     * */

    if (argc != 4 || !(argv[0]->flags & MEM_Null) || !(argv[1]->flags & MEM_Null))
        return SQLITE_OK;

    if (argv[2]->flags != MEM_Int || argv[3]->flags != MEM_Int) {
        pVTab->zErrMsg = sqlite3_mprintf("Unexpected types");
        return SQLITE_CONSTRAINT;
    }

    struct bloatrec rec = {
            .size = flibc_htonll(argv[2]->u.i),
            .sleep = flibc_htonll(argv[3]->u.i)
    };

    struct sql_thread *thd = sql_current_thread();
    int rc = osql_systable_op(thd, 0, "comdb2_log_bloat", &rec, sizeof(rec));
    if (rc) {
        pVTab->zErrMsg = sqlite3_mprintf("osql_systable_op returned %d", rc);
        return SQLITE_INTERNAL;
    }
    return 0;
}

int process_bloat(void *trans, void *payload, uint32_t len, sql_systable_recops recop) {
    struct bloatrec rec;
    int sz = (int) (sizeof(struct bloatrec) - offsetof(struct bloatrec, size));
    int rc;

    if (recop != SQL_SYSTABLE_OP_DO && recop != SQL_SYSTABLE_OP_APPLY) {
        logmsg(LOGMSG_ERROR, "%s: unexpected recop %d?", __func__, (int) recop);
        return -1;
    }
    if (len != sizeof(struct bloatrec)) {
        logmsg(LOGMSG_ERROR, "%s: unexpected bloat size? len %"PRIu32" expected %d", __func__, len, sz);
        return -1;
    }

    memcpy(&rec.size, payload, len);
    rec.size = flibc_htonll(rec.size);
    rec.sleep = flibc_htonll(rec.sleep);
    printf("%s as %s sz %d sleep %d\n", __func__, recop == SQL_SYSTABLE_OP_DO ? "master" : "replicant", (int) rec.size, (int) rec.sleep);

    // Who are we?  If we're on the master, we need to log the event so the replicants see it.  If we're on a
    // replicant we need to process it.  recop tells us which it is.
    if (recop == SQL_SYSTABLE_OP_DO) {
        // we're the master and need to log something for the replicant to do
        uint32_t bytes_left = rec.size;
        // bloat the log by writing (ignored) debug records
        while (bytes_left > 0) {
            static const int maxbloat_chunk = (1026*1024);
            uint32_t bloatsize = bytes_left > maxbloat_chunk ? maxbloat_chunk : bytes_left;
            rc = bdb_debug_log(thedb->bdb_env, trans, 1, bloatsize);
            if (rc)
                goto done;
            bytes_left -= bloatsize;
        }
        rc = bdb_log_systable_op(thedb->bdb_env, trans, 0, "comdb2_log_bloat", payload, len);
        if (rc)
            goto done;
    }
    else {
        // we're a replicant and are applying the thing logged by the master
        if (rec.sleep) {
            sleep(rec.sleep);
        }
        rc = 0;
    }

done:
    return rc;
}

#endif /* (!defined(SQLITE_CORE) || defined(SQLITE_BUILDING_FOR_COMDB2))       \
          && !defined(SQLITE_OMIT_VIRTUALTABLE) */
