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

#ifndef INCLUDED_BDB_SYSTABLE_H
#define INCLUDED_BDB_SYSTABLE_H

#include "db.h"
#include "llog_auto.h"

int bdb_handle_systable_op(DB_ENV *dbenv, u_int32_t rectype, llog_systable_op_args *args, DB_LSN *lsn, db_recops op);
int bdb_log_systable_op(bdb_state_type *bdb_state, void *trans, uint16_t op, const char *tablename,
                        void *payload, uint32_t payload_size);

#endif
