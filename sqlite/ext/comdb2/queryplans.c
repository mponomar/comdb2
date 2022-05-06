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

/* This implements the comdb2_queryplans table.  This table tracks plans by fingerprint (see comment in fingerprint.c)
 * for a brief summary of what a fingerprint is.  There's a few reasons we may have multiple plans per fingerprint.
 * Most commonly, a schema changed, or statistics got updated, and we are now running a different plan for the same
 * query (because a better index was made available, or a better plan emerged because the data is now differently
 * distributed).  Another less common but important case is when a different path is preferred for different key
 * values. */

#include <stdlib.h>

#include "comdb2.h"
#include "comdb2systbl.h"
#include "comdb2systblInt.h"
#include "sql.h"
#include "ezsystables.h"

static sqlite3_module systblQueryPlansModule = {
    .access_flag = CDB2_ALLOW_USER,
};

int systblQueryPlansInit(sqlite3 *db) {
    return create_system_table(db, "comdb2_query_plans", &systblQueryPlansModule,
                               get_all_query_plans, free_all_query_plans,
                               sizeof(struct plan_example),
                               CDB2_CSTRING, "fingerprint", -1, offsetof(struct plan_example, fingerprint),
                               CDB2_CSTRING, "plan", -1, offsetof(struct plan_example, plan),
                               CDB2_INTEGER, "nrows", -1, offsetof(struct plan_example, nrows),
                               CDB2_INTEGER, "cost", -1, offsetof(struct plan_example, cost),

                               SYSTABLE_END_OF_FIELDS);
}