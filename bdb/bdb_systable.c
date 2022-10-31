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

#include <string.h>
#include <bdb_int.h>
#include <db.h>

#include "llog_auto.h"
#include "llog_ext.h"
#include "llog_handlers.h"

// Handle a custom "systable" event.  This is a quick and easy way for us to get "writable" systables - where writing
// has some side effect.  This is the bdb side of things - lots of the usual boilerplate, some bounds checks, and
// the call to sql code that takes the payload and hopefully knows what to do with it.
int bdb_handle_systable_op(DB_ENV *dbenv, u_int32_t rectype, llog_systable_op_args *args, DB_LSN *lsn, db_recops op) {
    bdb_state_type *bdb_state;
    bdb_state = dbenv->app_private;
    int isundo = 0;
    int rc = 0;

    if (strnlen((char*) args->tablename.data, MAXTABLELEN) == MAXTABLELEN)
        return EINVAL;
    char *tablename = (char*) args->tablename.data;

    switch (op) {
    case DB_TXN_BACKWARD_ROLL:
    case DB_TXN_ABORT:
        isundo = 1;
        break;

    case DB_TXN_APPLY:
    case DB_TXN_FORWARD_ROLL:
        break;

    case DB_TXN_SNAPISOL:
        break;

    case DB_TXN_PRINT:
        printf("[%lu][%lu]scdone: rec: %lu txnid %lx prevlsn[%lu][%lu]\n",
               (u_long)lsn->file, (u_long)lsn->offset, (u_long)rectype,
               (u_long)args->txnid->txnid, (u_long)args->prev_lsn.file,
               (u_long)args->prev_lsn.offset);
            printf("\top: %d\n", args->op);
            printf("\ttable: %.*s\n", (int) args->tablename.size, (char*) args->tablename.data);
            printf("\tpayload_size: %u\n", args->payload.size);
        printf("\n");
        break;

        default:
            return EINVAL;
    }

    if (isundo)
        *lsn = args->prev_lsn;

    if (op == DB_TXN_FORWARD_ROLL || op == DB_TXN_BACKWARD_ROLL) {
        rc = 0;
        goto done;
    }

    if (bdb_state && bdb_state->callback && bdb_state->callback->systableop_rtn) {
        rc = bdb_state->callback->systableop_rtn(tablename, args->payload.data, args->payload.size, args->op, isundo);
        if (rc)
            return EINVAL;
    }

done:
    return rc;
}

int bdb_log_systable_op(bdb_state_type *bdb_state, void *trans, uint16_t op, const char *tablename, void *payload, uint32_t payload_size) {
    tran_type *t = (tran_type*) trans;
    DB_LSN lsn;
    DBT dbt_tablename = { .data = (void*) tablename, .size = strlen(tablename)+1 };
    DBT dbt_payload = { .data = (void*) payload, .size = payload_size };
    return llog_systable_op_log(bdb_state->dbenv, t->tid, &lsn, 0, op, &dbt_tablename, &dbt_payload);
}