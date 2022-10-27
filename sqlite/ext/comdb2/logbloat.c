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

#include "comdb2.h"
#include "sqlite3.h"
#include "comdb2systblInt.h"
#include "sql.h"
#include "ezsystables.h"

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
    char table[MAXTABLELEN];
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
// to the master to actually generate the bloat.  It probably should be a new opcode, but is instead
// handled as a write to a special comdb2_sysdummy table.  This call isn't to be confused with process_bloat
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

    // TODO: a new cursor class?  No point for this table to actually exist
    // This is a cursor to a dummy table.  The table itself must exist, and gets saved opcodes on the SQL side
    // like any other table, but osql_process_packet treats writes to this table differently - it'll cause a
    // custom log record to be written and a custom action (in this case writing a dummy log record of a given
    // length, but really anything) to be carried out.  Then the same handler code is called on the replicant
    // when the custom log record is processed.

    // The get/put cursor logic is regrettable but necessary - SQLite doesn't call open on a virtual table
    // for an insert, so we don't create a cursor, but need some way to have a temporary cursor on which to
    // call write.
    BtCursor *pCur = get_systable_cursor_from_table(pVTab);
    struct sql_thread *sqlthd = pthread_getspecific(query_info_key);

    struct bloatrec rec = {
            .table = "comdb2_log_bloat",
            .size = flibc_htonll(argv[2]->u.i),
            .sleep = flibc_htonll(argv[3]->u.i)
    };

    blob_buffer_t blobs[MAXBLOBS] = {0};
    int rc = osql_insrec(pCur, sqlthd, (char*) &rec, sizeof(struct bloatrec), blobs, MAXBLOBS, 0);
    if (rc) {
        pVTab->zErrMsg = sqlite3_mprintf("osql_save_insrec rc %d for table comdb2_log_bloat");
    }
    put_systable_cursor_from_table(pVTab);

    return rc;
}

int process_bloat(void *payload, uint32_t len) {
    struct bloatrec rec;
    int sz = (int) (sizeof(struct bloatrec) - offsetof(struct bloatrec, size));
    if (len != sizeof(struct bloatrec) - offsetof(struct bloatrec, size)) {
        logmsg(LOGMSG_ERROR, "Unexpected bloat size? len %"PRIu32" expected %d", len, sz);
        return -1;
    }
    memcpy(&rec.size, payload, len);
    rec.size = flibc_htonll(rec.size);
    rec.sleep = flibc_htonll(rec.sleep);
    printf("sz %d sleep %d\n", (int) rec.size, (int) rec.sleep);
    return 0;
}

#endif /* (!defined(SQLITE_CORE) || defined(SQLITE_BUILDING_FOR_COMDB2))       \
          && !defined(SQLITE_OMIT_VIRTUALTABLE) */
