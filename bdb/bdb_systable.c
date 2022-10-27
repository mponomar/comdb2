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
#include "bdb_systable.h"

// Handle a custom "systable" event.  This is a quick and easy way for us to get "writable" systables - where writing
// has some side effect.  This is the bdb side of things - lots of the usual boilerplate, some bounds checks, and
// the call to sql code that takes the payload and hopefully knows what to do with it.
int handle_systable_op(DB_ENV *dbenv, u_int32_t rectype, llog_systable_op_args *args, DB_LSN *lsn, db_recops op) {
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
        // TODO: implement, register so cdb2_printlog also prints, the usual
        break;

        default:
            return EINVAL;
    }
    if (isundo)
        *lsn = args->prev_lsn;

    if (bdb_state->callback->systableop_rtn) {
        rc = bdb_state->callback->systableop_rtn(tablename, args->payload.data, args->payload.size, args->op, isundo);
        if (rc)
            return EINVAL;
    }

    return rc;
}

