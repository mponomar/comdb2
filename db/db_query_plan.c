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

#include <math.h>

#include "sql.h"

hash_t *gbl_query_plan_hash;
pthread_mutex_t gbl_query_plan_hash_mu = PTHREAD_MUTEX_INITIALIZER;
int gbl_query_plan_max_queries = 1000;

static char *form_query_plan(struct client_query_stats *query_stats) {
    struct strbuf *query_plan_buf;
    struct client_query_path_component *c;
    char *query_plan;

    if (query_stats->n_components == 0) {
        return NULL;
    }

    query_plan_buf = strbuf_new();
    for (int i = 0; i < query_stats->n_components; i++) {
        if (i > 0) {
            strbuf_append(query_plan_buf, ", ");
        }
        c = &query_stats->path_stats[i];
        strbuf_appendf(query_plan_buf, "table %s index %d", c->table, c->ix);
    }

    query_plan = strdup((char *)strbuf_buf(query_plan_buf));
    strbuf_free(query_plan_buf);
    return query_plan;
}

void add_query_plan(struct client_query_stats *query_stats) {
    char *query_plan = form_query_plan(query_stats);
    if (!query_plan) {
        return;
    }

    double current_cost = query_stats->cost;
    double average_cost;
    Pthread_mutex_lock(&gbl_query_plan_hash_mu);
    if (gbl_query_plan_hash == NULL) {
        gbl_query_plan_hash = hash_init_strptr(0);
    }
    struct query_plan_item *q = hash_find(gbl_query_plan_hash, &query_plan);
    if (q == NULL) {
        /* make sure we haven't generated an unreasonable number of these */
        int nents = hash_get_num_entries(gbl_query_plan_hash);
        if (nents >= gbl_query_plan_max_queries) {
            static int complain_once = 1;
            if (complain_once) {
                logmsg(LOGMSG_WARN,
                       "Stopped tracking query plan, hit max #queries %d.\n",
                       gbl_query_plan_max_queries);
                complain_once = 0;
            }
        } else {
            q = calloc(1, sizeof(struct query_plan_item));
            q->plan = strdup(query_plan);
            q->total_cost = current_cost;
            q->nexecutions = 1;
            hash_add(gbl_query_plan_hash, q);
        }
    } else {
        average_cost = q->total_cost / q->nexecutions;
        if (fabs(current_cost - average_cost) > 100) {
            logmsg(LOGMSG_WARN, "Average cost for this query was %f, now is %f\n", average_cost, current_cost);
        }
        q->total_cost += current_cost;
        q->nexecutions++;
    }

    void *ent;
    unsigned int bkt;
    logmsg(LOGMSG_WARN, "START\n");
    for (q = (struct query_plan_item*)hash_first(gbl_query_plan_hash, &ent, &bkt); q; q = (struct query_plan_item*)hash_next(gbl_query_plan_hash, &ent, &bkt)) {
        logmsg(LOGMSG_WARN, "plan: %s, total cost: %f, num executions: %d, average: %f\n", q->plan, q->total_cost, q->nexecutions, q->total_cost / q->nexecutions);
    }
    logmsg(LOGMSG_WARN, "END\n");

    Pthread_mutex_unlock(&gbl_query_plan_hash_mu);
    free(query_plan);
}
