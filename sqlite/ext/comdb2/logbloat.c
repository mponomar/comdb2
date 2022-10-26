/*
   Copyright 2020 Bloomberg Finance L.P.

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
#include "comdb2systblInt.h"
#include "sql.h"
#include "ezsystables.h"

static sqlite3_module systblLogBloat = {
    .access_flag = CDB2_ALLOW_USER,
};


int systblLogBloat(sqlite3 *db) {
    int rc = create_system_table(db, "comdb2_log_bloat", &systblLogBloat,
            get_index_usage, free_index_usage, sizeof(index_usage),
            CDB2_CSTRING, "table_name", -1, offsetof(index_usage, tablename),
            CDB2_INTEGER, "ix_num", -1, offsetof(index_usage, ixnum),
            CDB2_CSTRING, "csc_name", -1, offsetof(index_usage, cscname),
            CDB2_CSTRING, "sql_name", -1, offsetof(index_usage, sqlname),
            CDB2_INTEGER, "steps", -1, offsetof(index_usage, steps),
            CDB2_INTEGER, "non_sql_steps", -1, offsetof(index_usage, non_sql_steps),
            SYSTABLE_END_OF_FIELDS);
    return rc;
}


#endif /* (!defined(SQLITE_CORE) || defined(SQLITE_BUILDING_FOR_COMDB2))       \
          && !defined(SQLITE_OMIT_VIRTUALTABLE) */

