/*
   Copyright 2017 Bloomberg Finance L.P.

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

#ifndef _DB_TUNABLES_H
#define _DB_TUNABLES_H
/*
  We need this guard to avoid accidental inclusion of this file
  at multiple places.
*/

REGISTER_TUNABLE_WITH_INT_DEFAULT("abort_on_in_use_rqid", NULL, TUNABLE_BOOLEAN,
                 &gbl_abort_on_clear_inuse_rqid, gbl_abort_on_clear_inuse_rqid, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("accept_osql_mismatch", NULL, TUNABLE_BOOLEAN,
                 &gbl_reject_osql_mismatch, gbl_reject_osql_mismatch, READONLY | INVERSE_VALUE | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("allow_lua_print", "Enable to allow stored "
                                    "procedures to print trace on "
                                    "DB's stdout. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_allow_lua_print, gbl_allow_lua_print, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("allow_lua_dynamic_libs",
                 "Enable to allow use of dynamic "
                 "libraries (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_allow_lua_dynamic_libs, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("allow_portmux_route", NULL, TUNABLE_BOOLEAN,
                 &gbl_pmux_route_enabled, gbl_pmux_route_enabled, READONLY | NOARG | READEARLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("allow_user_schema",
                 "Enable to allow per-user schemas. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_allow_user_schema, gbl_allow_user_schema, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("analyze_comp_threads",
                 "Number of thread to use when generating samples for "
                 "computing index statistics. (Default: 10)",
                 TUNABLE_INTEGER, &analyze_max_comp_threads, analyze_max_comp_threads, READONLY, NULL,
                 NULL, analyze_set_max_sampling_threads, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("analyze_comp_threshold",
                 "Index file size above which we'll do sampling, rather than "
                 "scan the entire index. (Default: 104857600)",
                 TUNABLE_INTEGER, &sampling_threshold, sampling_threshold, READONLY, NULL, NULL,
                 analyze_set_sampling_threshold, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("analyze_tbl_threads",
                 "Number of threads to go through generated samples when "
                 "generating index statistics. (Default: 5)",
                 TUNABLE_INTEGER, &analyze_max_table_threads, analyze_max_table_threads, READONLY, NULL,
                 NULL, analyze_set_max_table_threads, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("badwrite_intvl", NULL, TUNABLE_INTEGER,
                 &gbl_test_badwrite_intvl, gbl_test_badwrite_intvl, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("bbenv", NULL, TUNABLE_BOOLEAN, &gbl_bbenv, gbl_bbenv,
                 DEPRECATED | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("blob_mem_mb", "Blob allocator: Sets the max "
                                "memory limit to allow for blob "
                                "values (in MB). (Default: 0)",
                 TUNABLE_INTEGER, &gbl_blobmem_cap, gbl_blobmem_cap, READONLY, NULL, NULL,
                 blob_mem_mb_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("blobmem_sz_thresh_kb",
                 "Sets the threshold (in KB) above which blobs are allocated "
                 "by the blob allocator. (Default: 0)",
                 TUNABLE_INTEGER, &gbl_blob_sz_thresh_bytes, gbl_blob_sz_thresh_bytes, READONLY, NULL,
                 NULL, blobmem_sz_thresh_kb_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("blobstripe", NULL, TUNABLE_BOOLEAN, &gbl_blobstripe, gbl_blobstripe,
                 READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("blocksql_grace",
                 "Let block transactions run this long if db is exiting before "
                 "being killed (and returning an error). (Default: 10sec)",
                 TUNABLE_INTEGER, &gbl_blocksql_grace, gbl_blocksql_grace, 0, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("blocksql_over_sockets", NULL, TUNABLE_BOOLEAN,
                 &gbl_upgrade_blocksql_to_socksql, gbl_upgrade_blocksql_to_socksql, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("blocksql_throttle", NULL, TUNABLE_INTEGER,
                 &g_osql_blocksql_parallel_max, g_osql_blocksql_parallel_max, READONLY, NULL, NULL, NULL, 
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("broken_max_rec_sz", NULL, TUNABLE_INTEGER,
                 &gbl_broken_max_rec_sz, gbl_broken_max_rec_sz, READONLY, NULL, NULL,
                 broken_max_rec_sz_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("broken_num_parser", NULL, TUNABLE_BOOLEAN,
                 &gbl_broken_num_parser, gbl_broken_num_parser, READONLY | NOARG | READEARLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("buffers_per_context", NULL, TUNABLE_INTEGER,
                 &gbl_buffers_per_context, gbl_buffers_per_context, READONLY | NOZERO, NULL, NULL, NULL,
                 NULL);
/*
REGISTER_TUNABLE("cache",
                 "Database cache size (in kb) . (Default: 64mb)",
                 TUNABLE_INTEGER, &db->cacheszkb, READONLY, NULL, NULL, NULL,
                 NULL);
*/
REGISTER_TUNABLE_WITH_INT_DEFAULT("cachekb", NULL, TUNABLE_INTEGER, &db->cacheszkb, 65536, READONLY,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("cachekbmax", NULL, TUNABLE_INTEGER, &db->cacheszkbmax, 0,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("cachekbmin", NULL, TUNABLE_INTEGER, &db->cacheszkbmin, 65536,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_DEFAULT("checkctags", NULL, TUNABLE_ENUM, &gbl_check_client_tags, "FULL",
                 READONLY, checkctags_value, NULL, checkctags_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("chkpoint_alarm_time",
                 "Warn if checkpoints are taking more than this many seconds. "
                 "(Default: 60 secs)",
                 TUNABLE_INTEGER, &gbl_chkpoint_alarm_time, gbl_chkpoint_alarm_time, READONLY, NULL,
                 NULL, NULL, NULL);
/* Generate the value of 'cluster' on fly (define value()). */
/*
REGISTER_TUNABLE_WITH_INT_DEFAULT("cluster",
                 "List of nodes that comprise the cluster for this database.",
                 TUNABLE_STRING, &placeholder, placeholder, READONLY, NULL, NULL, NULL,
                 NULL);
*/
REGISTER_TUNABLE_WITH_INT_DEFAULT("compress_page_compact_log", NULL, TUNABLE_BOOLEAN,
                 &gbl_compress_page_compact_log, gbl_compress_page_compact_log, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
/* NOTE: THIS OPTION IS CURRENTLY IGNORED */
REGISTER_TUNABLE_WITH_INT_DEFAULT("convflush", NULL, TUNABLE_INTEGER, &gbl_conv_flush_freq, gbl_conv_flush_freq,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("crc32c",
                 "Use crc32c (alternate faster implementation of CRC32, "
                 "different checksums) for page checksums. (Default: on)",
                 TUNABLE_BOOLEAN, &gbl_crc32c, gbl_crc32c, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("crypto", NULL, TUNABLE_STRING, &gbl_crypto, gbl_crypto, READONLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("ctrace_dbdir",
                 "If set, debug trace files will go to the data directory "
                 "instead of `$COMDB2_ROOT/var/log/cdb2/). (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_ctrace_dbdir, gbl_ctrace_dbdir, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("ctrace_gzip", NULL, TUNABLE_INTEGER, &ctrace_gzip, ctrace_gzip,
                 DEPRECATED | READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE(
    "ddl_cascade_drop",
    "On DROP, also drop the dependent keys/constraints. (Default: 1)",
    TUNABLE_BOOLEAN, &gbl_ddl_cascade_drop, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("deadlock_policy_override", NULL, TUNABLE_INTEGER,
                 &gbl_deadlock_policy_override, gbl_deadlock_policy_override, READONLY, NULL, NULL,
                 deadlock_policy_override_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("debug_rowlocks", NULL, TUNABLE_BOOLEAN, &gbl_debug_rowlocks, gbl_debug_rowlocks,
                 NOARG, NULL, NULL, NULL, NULL);
/*
REGISTER_TUNABLE("decimal_rounding", NULL, TUNABLE_INTEGER,
                 &gbl_decimal_rounding, READONLY, NULL, NULL, NULL, NULL);
                 */
REGISTER_TUNABLE_WITH_INT_DEFAULT("decom_time", "Decomission time. (Default: 0)",
                 TUNABLE_INTEGER, &gbl_decom, gbl_decom, READONLY | NOZERO, NULL, NULL,
                 NULL, NULL);
/*
REGISTER_TUNABLE("default_datetime_precision", NULL,
                 TUNABLE_INTEGER, &gbl_datetime_precision, READONLY, NULL, NULL,
                 NULL, NULL);
*/
REGISTER_TUNABLE("dir",
                 "Database directory. (Default: $COMDB2_ROOT/var/cdb2/$DBNAME)",
                 TUNABLE_STRING, &db->basedir, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_bbipc", NULL, TUNABLE_BOOLEAN, &gbl_use_bbipc, gbl_use_bbipc,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_cache_internal_nodes",
                 "Disables 'enable_cache_internal_nodes'. B-tree leaf nodes "
                 "are treated same as internal nodes.",
                 TUNABLE_BOOLEAN, &gbl_enable_cache_internal_nodes, gbl_enable_cache_internal_nodes,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_datetime_ms_us_sc",
                 "Disables 'enable_datetime_ms_us_sc'", TUNABLE_BOOLEAN,
                 &gbl_forbid_datetime_ms_us_s2s, gbl_forbid_datetime_ms_us_s2s, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_datetime_promotion",
                 "Disables 'enable_datetime_promotion'", TUNABLE_BOOLEAN,
                 &gbl_forbid_datetime_promotion, gbl_forbid_datetime_promotion, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_datetime_truncation",
                 "Disables 'enable_datetime_truncation'", TUNABLE_BOOLEAN,
                 &gbl_forbid_datetime_truncation, gbl_forbid_datetime_truncation, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_inplace_blob_optimization",
                 "Disables 'enable_inplace_blob_optimization'", TUNABLE_BOOLEAN,
                 &gbl_inplace_blob_optimization, gbl_inplace_blob_optimization,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_inplace_blobs", "Disables 'enable_inplace_blobs'",
                 TUNABLE_BOOLEAN, &gbl_inplace_blobs, gbl_inplace_blobs,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_lowpri_snapisol", "Disables 'enable_lowpri_snapisol'",
                 TUNABLE_BOOLEAN, &gbl_lowpri_snapisol_sessions, gbl_lowpri_snapisol_sessions,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
/*
REGISTER_TUNABLE("disable_new_snapshot",
                 "Disables 'enable_new_snapshot'", TUNABLE_BOOLEAN,
                 &gbl_new_snapisol, INVERSE_VALUE | READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
*/
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_osql_blob_optimization",
                 "Disables 'enable_osql_blob_optimization'", TUNABLE_BOOLEAN,
                 &gbl_osql_blob_optimization, gbl_osql_blob_optimization, INVERSE_VALUE | READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_overflow_page_trace",
                 "Disables 'enable_overflow_page_trace'", TUNABLE_BOOLEAN,
                 &gbl_disable_overflow_page_trace, gbl_disable_overflow_page_trace, NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_DOUBLE_DEFAULT("disable_page_compact", "Sets 'page_compact_thresh_ff' to 0.",
                 TUNABLE_BOOLEAN, &gbl_pg_compact_thresh, gbl_pg_compact_thresh,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_page_compact_backward_scan",
                 "Disables 'enable_page_compact_backward_scan'",
                 TUNABLE_BOOLEAN, &gbl_disable_backward_scan, gbl_disable_backward_scan, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_page_latches", "Disables 'page_latches'",
                 TUNABLE_BOOLEAN, &gbl_page_latches, gbl_page_latches,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_partial_indexes", "Disables 'enable_partial_indexes'",
                 TUNABLE_BOOLEAN, &gbl_partial_indexes, gbl_partial_indexes,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_prefault_udp", "Disables 'enable_prefault_udp'",
                 TUNABLE_BOOLEAN, &gbl_prefault_udp, gbl_prefault_udp, INVERSE_VALUE | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_replicant_latches", "Disables 'replicant_latches'",
                 TUNABLE_BOOLEAN, &gbl_replicant_latches, gbl_replicant_latches,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_rowlock_locking", NULL, TUNABLE_BOOLEAN,
                 &gbl_disable_rowlocks, gbl_disable_rowlocks, READONLY | NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_skip_rows", NULL, TUNABLE_BOOLEAN,
                 &gbl_disable_skip_rows, gbl_disable_skip_rows, READONLY | NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_sock_fstsnd", "Disables 'enable_sock_fstsnd'",
                 TUNABLE_BOOLEAN, &gbl_enable_sock_fstsnd, gbl_enable_sock_fstsnd,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_sparse_lockerid_map",
                 "Disables 'enable_sparse_lockerid_map'", TUNABLE_BOOLEAN,
                 &gbl_sparse_lockerid_map, gbl_sparse_lockerid_map, INVERSE_VALUE | READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_sql_dlmalloc",
                 "If set, will use default system malloc for SQL state "
                 "machines. By default, each thread running SQL gets a "
                 "dedicated memory pool. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_disable_sql_dlmalloc, gbl_disable_sql_dlmalloc, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_tagged_api", "Disables 'enable_tagged_api'",
                 TUNABLE_BOOLEAN, &gbl_disable_tagged_api, gbl_disable_tagged_api, NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_temptable_pool", "Sets 'temptable_limit' to 0.",
                 TUNABLE_BOOLEAN, &gbl_temptable_pool_capacity, gbl_temptable_pool_capacity,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_upgrade_ahead", "Sets 'enable_upgrade_ahead' to 0.",
                 TUNABLE_BOOLEAN, &gbl_num_record_upgrades, gbl_num_record_upgrades,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disallow_portmux_route", "Disables 'allow_portmux_route'",
                 TUNABLE_BOOLEAN, &gbl_pmux_route_enabled, gbl_pmux_route_enabled,
                 INVERSE_VALUE | READONLY | NOARG | READEARLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("dont_abort_on_in_use_rqid", "Disable 'abort_on_in_use_rqid'",
                 TUNABLE_BOOLEAN, &gbl_abort_on_clear_inuse_rqid, gbl_abort_on_clear_inuse_rqid,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("dont_forbid_ulonglong", "Disables 'forbid_ulonglong'",
                 TUNABLE_BOOLEAN, &gbl_forbid_ulonglong, gbl_forbid_ulonglong,
                 INVERSE_VALUE | READONLY | NOARG | READEARLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("dont_init_with_inplace_updates",
                 "Disables 'init_with_inplace_updates'", TUNABLE_BOOLEAN,
                 &gbl_init_with_ipu, gbl_init_with_ipu, INVERSE_VALUE | READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("dont_init_with_instant_schema_change",
                 "Disables 'instant_schema_change'", TUNABLE_BOOLEAN,
                 &gbl_init_with_instant_sc, gbl_init_with_instant_sc, INVERSE_VALUE | READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("dont_init_with_ondisk_header",
                 "Disables 'dont_init_with_ondisk_header'", TUNABLE_BOOLEAN,
                 &gbl_init_with_odh, gbl_init_with_odh, INVERSE_VALUE | READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("dont_optimize_repdb_truncate",
                 "Disable 'optimize_repdb_truncate'", TUNABLE_BOOLEAN,
                 &gbl_optimize_truncate_repdb, gbl_optimize_truncate_repdb,
                 INVERSE_VALUE | READONLY | NOARG | READEARLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("dont_prefix_foreign_keys", "Disables 'prefix_foreign_keys'",
                 TUNABLE_BOOLEAN, &gbl_fk_allow_prefix_keys, gbl_fk_allow_prefix_keys,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("dont_superset_foreign_keys",
                 "Disables 'superset_foreign_keys'", TUNABLE_BOOLEAN,
                 &gbl_fk_allow_superset_keys, gbl_fk_allow_superset_keys, INVERSE_VALUE | READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("dont_sort_nulls_with_header",
                 "Disables 'sort_nulls_with_header'", TUNABLE_BOOLEAN,
                 &gbl_sort_nulls_correctly, gbl_sort_nulls_correctly, INVERSE_VALUE | READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("dont_use_bbipc_fastseed", "Disable 'use_bbipc_fastseed'",
                 TUNABLE_BOOLEAN, &gbl_use_bbipc_global_fastseed, gbl_use_bbipc_global_fastseed,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("dtastripe", NULL, TUNABLE_INTEGER, &gbl_dtastripe, gbl_dtastripe,
                 READONLY | NOZERO, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("early",
                 "When set, replicants will ack a transaction as soon as they "
                 "acquire locks - not that replication must succeed at that "
                 "point, and reads on that node will either see the records or "
                 "block. (Default: on)",
                 TUNABLE_BOOLEAN, &gbl_early, gbl_early, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_berkdb_retry_deadlock_bias", NULL, TUNABLE_BOOLEAN,
                 &gbl_enable_berkdb_retry_deadlock_bias, gbl_enable_berkdb_retry_deadlock_bias, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_blockoffload", NULL, TUNABLE_INTEGER,
                 &gbl_enable_block_offload, gbl_enable_block_offload, READONLY | NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "enable_cache_internal_nodes",
    "B-tree internal nodes have a higher cache priority. (Default: on)",
    TUNABLE_BOOLEAN, &gbl_enable_cache_internal_nodes, gbl_enable_cache_internal_nodes, READONLY | NOARG, NULL,
    NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_datetime_ms_us_sc", NULL, TUNABLE_BOOLEAN,
                 &gbl_forbid_datetime_ms_us_s2s, gbl_forbid_datetime_ms_us_s2s,
                 READONLY | INVERSE_VALUE | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_datetime_promotion", NULL, TUNABLE_BOOLEAN,
                 &gbl_forbid_datetime_promotion, gbl_forbid_datetime_promotion,
                 READONLY | INVERSE_VALUE | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_datetime_truncation", NULL, TUNABLE_BOOLEAN,
                 &gbl_forbid_datetime_truncation, gbl_forbid_datetime_truncation,
                 READONLY | INVERSE_VALUE | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_inplace_blob_optimization",
                 "Enables inplace blob updates (blobs are updated in place in "
                 "their b-tree when possible, not deleted/added) Note: This "
                 "changes the data-format. (Default: on)",
                 TUNABLE_BOOLEAN, &gbl_inplace_blob_optimization, gbl_inplace_blob_optimization,
                 READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "enable_inplace_blobs",
    "Do not update the rowid of a blob entry on an update. (Default: on)",
    TUNABLE_BOOLEAN, &gbl_inplace_blobs, gbl_inplace_blobs, READONLY | NOARG, NULL, NULL, NULL,
    NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_lowpri_snapisol",
                 "Give lower priority to locks acquired when updating snapshot "
                 "state. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_lowpri_snapisol_sessions, gbl_lowpri_snapisol_sessions,
                 READONLY | NOARG, NULL, NULL, NULL, NULL);
/*
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_new_snapshot",
                 "Enable new SNAPSHOT implementation. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_new_snapisol, gbl_new_snapisol, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "enable_new_snapshot_asof",
    "Enable new BEGIN TRANSACTION AS OF implementation. (Default: off)",
    TUNABLE_BOOLEAN, &gbl_new_snapisol_asof, gbl_new_snapisol_asof, READONLY | NOARG, NULL, NULL, NULL,
    NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_new_snapshot_logging",
                 "Enable alternate logging scheme. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_new_snapisol_logging, gbl_new_snapisol_logging, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
*/
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_osql_blob_optimization",
                 "Replicant tracks which columns are modified in a transaction "
                 "to allow blob updates to be ommitted if possible. (Default: "
                 "on)",
                 TUNABLE_BOOLEAN, &gbl_osql_blob_optimization, gbl_osql_blob_optimization, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_overflow_page_trace",
                 "If set, warn when a page order table scan encounters an "
                 "overflow page. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_disable_overflow_page_trace, gbl_disable_overflow_page_trace,
                 INVERSE_VALUE | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_page_compact_backward_scan", NULL, TUNABLE_INTEGER,
                 &gbl_disable_backward_scan, gbl_disable_backward_scan, INVERSE_VALUE | READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "enable_partial_indexes",
    "If set, allows partial index definitions in table schema. (Default: off)",
    TUNABLE_BOOLEAN, &gbl_partial_indexes, gbl_partial_indexes, READONLY | NOARG, NULL, NULL, NULL,
    NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_position_apis",
                 "Enables support for position APIs. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_enable_position_apis, gbl_enable_position_apis, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_prefault_udp",
                 "Send lossy prefault requests to replicants. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_prefault_udp, gbl_prefault_udp, NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_selectv_range_check",
                 "If set, SELECTV will send ranges for verification, not every "
                 "touched record. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_selectv_rangechk, gbl_selectv_rangechk, NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("disable_selectv_range_check",
                 "Disables 'enable_selectv_range_check'", TUNABLE_BOOLEAN,
                 &gbl_selectv_rangechk, gbl_selectv_rangechk, INVERSE_VALUE | NOARG, NULL, NULL, NULL,
                 NULL);
/*
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_snapshot_isolation",
                 "Enable to allow SNAPSHOT level transactions to run against "
                 "the database. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_snapisol, gbl_snapisol, READONLY, NULL, NULL, NULL,
                 NULL);
*/
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_sock_fstsnd", NULL, TUNABLE_BOOLEAN,
                 &gbl_enable_sock_fstsnd, gbl_enable_sock_fstsnd, READONLY | NOARG | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_sparse_lockerid_map",
                 "If set, allocates a sparse map of lockers for deadlock "
                 "resolution. (Default: on)",
                 TUNABLE_BOOLEAN, &gbl_sparse_lockerid_map, gbl_sparse_lockerid_map, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_sp_strict_assignments", NULL, TUNABLE_INTEGER,
                 &gbl_spstrictassignments, gbl_spstrictassignments, READONLY | NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_DEFAULT("enable_sql_stmt_caching",
                 "Enable caching of query plans. If followed by \"all\" will "
                 "cache all queries, including those without parameters. "
                 TUNABLE_ENUM, &gbl_enable_sql_stmt_caching, "NONE", READONLY | NOARG,
                 enable_sql_stmt_caching_value, NULL,
                 enable_sql_stmt_caching_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_tagged_api",
                 "Enables tagged api requests. (Default: on)", TUNABLE_BOOLEAN,
                 &gbl_disable_tagged_api, gbl_disable_tagged_api, INVERSE_VALUE | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enable_upgrade_ahead",
                 "Occasionally update read records to the newest schema "
                 "version (saves some processing when reading them later). "
                 "(Default: off)",
                 TUNABLE_INTEGER, &gbl_num_record_upgrades, gbl_num_record_upgrades, READONLY | NOARG,
                 NULL, NULL, enable_upgrade_ahead_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enque_flush_interval", NULL, TUNABLE_INTEGER,
                 &gbl_enque_flush_interval, gbl_enque_flush_interval, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enque_flush_interval_signal", NULL, TUNABLE_INTEGER,
                 &gbl_enque_flush_interval_signal, gbl_enque_flush_interval_signal, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("enque_reorder_lookahead", NULL, TUNABLE_INTEGER,
                 &gbl_enque_reorder_lookahead, gbl_enque_reorder_lookahead, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("env_messages", NULL, TUNABLE_BOOLEAN, &gbl_noenv_messages, gbl_noenv_messages,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("epochms_repts", NULL, TUNABLE_BOOLEAN,
                 &gbl_berkdb_epochms_repts, gbl_berkdb_epochms_repts, READONLY | NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("erroff", "Disables 'erron'", TUNABLE_BOOLEAN, &db->errstaton, db,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("erron", NULL, TUNABLE_BOOLEAN, &db->errstaton, db,
                 READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "exclusive_blockop_qconsume",
    "Enables serialization of blockops and queue consumes. (Default: off)",
    TUNABLE_BOOLEAN, &gbl_exclusive_blockop_qconsume, gbl_exclusive_blockop_qconsume, READONLY | NOARG, NULL,
    NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("exitalarmsec", NULL, TUNABLE_INTEGER, &gbl_exit_alarm_sec, gbl_exit_alarm_sec,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("exit_on_internal_failure", NULL, TUNABLE_BOOLEAN,
                 &gbl_exit_on_internal_error, gbl_exit_on_internal_error, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("fdbdebg", NULL, TUNABLE_INTEGER, &gbl_fdb_track, gbl_fdb_track, READONLY,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("fdbtrackhints", NULL, TUNABLE_INTEGER, &gbl_fdb_track_hints, gbl_fdb_track_hints,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("fkrcode", "Enable foreign-key violation return code.",
                 TUNABLE_BOOLEAN, &gbl_fkrcode, gbl_fkrcode, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("forbid_ulonglong", "Disallow u_longlong. (Default: on)",
                 TUNABLE_BOOLEAN, &gbl_forbid_ulonglong, gbl_forbid_ulonglong,
                 READONLY | NOARG | READEARLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("force_highslot", NULL, TUNABLE_BOOLEAN, &gbl_force_highslot, gbl_force_highslot,
                 READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("foreign_db_allow_cross_class", NULL, TUNABLE_BOOLEAN,
                 &gbl_fdb_allow_cross_classes, gbl_fdb_allow_cross_classes, READONLY | NOARG | READEARLY,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("foreign_db_resolve_local", NULL, TUNABLE_BOOLEAN,
                 &gbl_fdb_resolve_local, gbl_fdb_resolve_local, READONLY | NOARG | READEARLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("fullrecovery", "Attempt to run database "
                                 "recovery from the beginning of "
                                 "available logs. (Default : off)",
                 TUNABLE_BOOLEAN, &gbl_fullrecovery, gbl_fullrecovery, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("gbl_exit_on_pthread_create_fail",
                 "If set, database will exit if thread pools aren't able to "
                 "create threads. (Default: 0)",
                 TUNABLE_INTEGER, &gbl_exit_on_pthread_create_fail, gbl_exit_on_pthread_create_fail, READONLY,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("goslow", NULL, TUNABLE_BOOLEAN, &gbl_goslow, gbl_goslow, NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("gofast", NULL, TUNABLE_BOOLEAN, &gbl_goslow, gbl_goslow,
                 INVERSE_VALUE | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("heartbeat_check_time",
                 "Raise an error if no heartbeat for this amount of time (in "
                 "secs). (Default: 10 secs)",
                 TUNABLE_INTEGER, &gbl_heartbeat_check, gbl_heartbeat_check, READONLY | NOZERO, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("heartbeat_send_time",
                 "Send heartbeats this often. (Default: 5secs)",
                 TUNABLE_INTEGER, &gbl_heartbeat_send, gbl_heartbeat_send, READONLY | NOZERO, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE("hostname", NULL, TUNABLE_STRING, &gbl_mynode,
                 READONLY | READEARLY, NULL, NULL, hostname_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("incoherent_alarm_time", NULL, TUNABLE_INTEGER,
                 &gbl_incoherent_alarm_time, gbl_incoherent_alarm_time, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("incoherent_msg_freq", NULL, TUNABLE_INTEGER,
                 &gbl_incoherent_msg_freq, gbl_incoherent_msg_freq, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("inflatelog", NULL, TUNABLE_INTEGER, &gbl_inflate_log, gbl_inflate_log,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("init_with_bthash", NULL, TUNABLE_INTEGER,
                 &gbl_init_with_bthash, gbl_init_with_bthash, READONLY | NOZERO, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_DEFAULT("init_with_compr", NULL, TUNABLE_ENUM, &gbl_init_with_compr, "crle",
                 READONLY, init_with_compr_value, NULL, init_with_compr_update,
                 NULL);
REGISTER_TUNABLE_WITH_DEFAULT("init_with_compr_blobs", NULL, TUNABLE_ENUM,
                 &gbl_init_with_compr_blobs, "lz4", READONLY, init_with_compr_value,
                 NULL, init_with_compr_blobs_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("init_with_genid48",
                 "Enables Genid48 for the database. (Default: off)",
                 TUNABLE_INTEGER, &gbl_init_with_genid48, gbl_init_with_genid48, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("init_with_inplace_updates",
                 "Initialize tables with inplace-update support. (Default: on)",
                 TUNABLE_BOOLEAN, &gbl_init_with_ipu, gbl_init_with_ipu, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("init_with_instant_schema_change",
                 "Same as 'instant_schema_change'", TUNABLE_BOOLEAN,
                 &gbl_init_with_instant_sc, gbl_init_with_instant_sc, READONLY | NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("init_with_ondisk_header",
                 "Initialize tables with on-disk header. (Default: on)",
                 TUNABLE_BOOLEAN, &gbl_init_with_odh, gbl_init_with_odh, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("init_with_rowlocks",
                 "Enables row-locks for the database. (Default: 0)",
                 TUNABLE_INTEGER, &gbl_init_with_rowlocks, gbl_init_with_rowlocks, READONLY | NOARG,
                 NULL, NULL, init_with_rowlocks_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "init_with_rowlocks_master_only",
    "Enables row-locks for the database (master-only). (Default: 0)",
    TUNABLE_INTEGER, &gbl_init_with_rowlocks, gbl_init_with_rowlocks, READONLY | NOARG, NULL, NULL,
    init_with_rowlocks_master_only_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("init_with_time_based_genids", "Enables time-based GENIDs",
                 TUNABLE_BOOLEAN, &gbl_init_with_genid48, gbl_init_with_genid48,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("instant_schema_change",
                 "When possible (eg: when just adding fields) schema change "
                 "will not rebuild the underlying tables. (Default: on)",
                 TUNABLE_BOOLEAN, &gbl_init_with_instant_sc, gbl_init_with_instant_sc, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("ioqueue",
                 "Maximum depth of the I/O prefaulting queue. (Default: 0)",
                 TUNABLE_INTEGER, &gbl_ioqueue, gbl_ioqueue, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("iothreads",
                 "Number of threads to use for I/O prefaulting. (Default: 0)",
                 TUNABLE_INTEGER, &gbl_iothreads, gbl_iothreads, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("keycompr",
                 "Enable index compression (applies to newly allocated index "
                 "pages, rebuild table to force for all pages.",
                 TUNABLE_BOOLEAN, &gbl_keycompr, gbl_keycompr, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("largepages", "Enables large pages. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_largepages, gbl_largepages, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("lclpooledbufs", NULL, TUNABLE_INTEGER, &gbl_lclpooled_buffers, gbl_lclpooled_buffers,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("lk_hash", NULL, TUNABLE_INTEGER, &gbl_lk_hash, gbl_lk_hash,
                 READONLY | READEARLY, NULL, lk_verify, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("lk_part", NULL, TUNABLE_INTEGER, &gbl_lk_parts, gbl_lk_parts,
                 READONLY | READEARLY, NULL, lk_verify, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("lkr_hash", NULL, TUNABLE_INTEGER, &gbl_lkr_hash, gbl_lkr_hash,
                 READONLY | READEARLY, NULL, lk_verify, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("lkr_part", NULL, TUNABLE_INTEGER, &gbl_lkr_parts, gbl_lkr_parts,
                 READONLY | READEARLY, NULL, lk_verify, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("lock_conflict_trace",
                 "Dump count of lock conflicts every second. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_lock_conflict_trace, gbl_lock_conflict_trace, NOARG, NULL, NULL,
                 NULL, NULL);
/* TODO(Nirbhay): Merge the following 3 into a single (enum?) tunable. */
REGISTER_TUNABLE_WITH_INT_DEFAULT("log_delete_after_backup",
                 "Set log deletion policy to disable log deletion (can be set "
                 "by backups). (Default: off)",
                 TUNABLE_INTEGER, &db->log_delete_age, 0, READONLY, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("log_delete_before_startup",
                 "Set log deletion policy to disable logs older than database "
                 "startup time. (Default: off)",
                 TUNABLE_INTEGER, &db->log_delete_age, 0, READONLY | NOARG, NULL,
                 NULL, log_delete_before_startup_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "log_delete_now",
    "Set log deletion policy to delete logs as soon as possible. (Default: 0)",
    TUNABLE_INTEGER, &db->log_delete_age, 0, READONLY | NOARG | INVERSE_VALUE,
    NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("loghist", NULL, TUNABLE_INTEGER, &gbl_loghist, gbl_loghist,
                 READONLY | NOARG, NULL, NULL, loghist_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("loghist_verbose", NULL, TUNABLE_BOOLEAN, &gbl_loghist_verbose, gbl_loghist_verbose,
                 READONLY | NOARG, NULL, NULL, NULL, NULL);
/*
REGISTER_TUNABLE_WITH_INT_DEFAULT("mallocregions", NULL, TUNABLE_INTEGER,
                 &gbl_malloc_regions, gbl_malloc_regions, READONLY, NULL, NULL, NULL, NULL);
*/
/*
  Note: master_retry_poll_ms' value < 0 was previously ignored without
  any error.
*/
REGISTER_TUNABLE_WITH_INT_DEFAULT("master_retry_poll_ms",
                 "Have a node wait this long after a master swing before "
                 "retrying a transaction. (Default: 100ms)",
                 TUNABLE_INTEGER, &gbl_master_retry_poll_ms, gbl_master_retry_poll_ms, READONLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("master_swing_osql_verbose",
                 "Produce verbose trace for SQL handlers detecting a master "
                 "change. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_master_swing_osql_verbose, gbl_master_swing_osql_verbose,
                 READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("master_swing_osql_verbose_off",
                 "Disables 'master_swing_osql_verbose'", TUNABLE_BOOLEAN,
                 &gbl_master_swing_osql_verbose, gbl_master_swing_osql_verbose, INVERSE_VALUE | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE("master_swing_sock_restart_sleep",
                 "For testing: sleep in osql_sock_restart when master swings",
                 TUNABLE_INTEGER, &gbl_master_swing_sock_restart_sleep,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("maxblobretries", NULL, TUNABLE_INTEGER, &gbl_maxblobretries, gbl_maxblobretries,
                 READONLY, NULL, maxretries_verify, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("maxblockops", NULL, TUNABLE_INTEGER, &gbl_maxblockops, gbl_maxblockops,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("maxcolumns",
                 "Raise the maximum permitted number of columns per table. "
                 "There is a hard limit of 1024. (Default: 255)",
                 TUNABLE_INTEGER, &gbl_max_columns_soft_limit, gbl_max_columns_soft_limit, READONLY, NULL,
                 maxcolumns_verify, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("maxcontextskips", NULL, TUNABLE_INTEGER, &gbl_maxcontextskips, gbl_maxcontextskips,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("maxosqltransfer",
                 "Maximum number of record modifications allowed per "
                 "transaction. (Default: 50000)",
                 TUNABLE_INTEGER, &g_osql_max_trans, g_osql_max_trans, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("maxthrottletime", NULL, TUNABLE_INTEGER,
                 &gbl_osql_max_throttle_sec, gbl_osql_max_throttle_sec, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("max_incoherent_nodes", NULL, TUNABLE_INTEGER,
                 &gbl_max_incoherent_nodes, gbl_max_incoherent_nodes, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("max_lua_instructions",
                 "Maximum lua opcodes to execute before we assume the stored "
                 "procedure is looping and kill it. (Default: 10000)",
                 TUNABLE_INTEGER, &gbl_max_lua_instructions, gbl_max_lua_instructions, READONLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("max_num_compact_pages_per_txn", NULL, TUNABLE_INTEGER,
                 &gbl_max_num_compact_pages_per_txn, gbl_max_num_compact_pages_per_txn, 0, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("maxq",
                 "Maximum queue depth for write requests. (Default: 192)",
                 TUNABLE_INTEGER, &gbl_maxqueue, gbl_maxqueue, READONLY, NULL, NULL,
                 maxq_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("maxretries", "Maximum number of times a "
                               "transactions will be retried on a "
                               "deadlock. (Default: 500)",
                 TUNABLE_INTEGER, &gbl_maxretries, gbl_maxretries, READONLY, NULL,
                 maxretries_verify, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "max_sqlcache_hints",
    "Maximum number of \"hinted\" query plans to keep (global). (Default: 100)",
    TUNABLE_INTEGER, &gbl_max_sql_hint_cache, gbl_max_sql_hint_cache, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("max_sqlcache_per_thread",
                 "Maximum number of plans to cache per sql thread (statement "
                 "cache is per-thread). (Default: 10)",
                 TUNABLE_INTEGER, &gbl_max_sqlcache, gbl_max_sqlcache, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("maxt", NULL, TUNABLE_INTEGER, &gbl_maxthreads, gbl_maxthreads,
                 READONLY | NOZERO, NULL, NULL, maxt_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "maxwt",
    "Maximum number of threads processing write requests. (Default: 8)",
    TUNABLE_INTEGER, &gbl_maxwthreads, gbl_maxwthreads, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("memnice", NULL, TUNABLE_INTEGER, &gbl_mem_nice, gbl_mem_nice,
                 READONLY | NOARG, NULL, NULL, memnice_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("mempget_timeout", NULL, TUNABLE_INTEGER,
                 &__gbl_max_mpalloc_sleeptime, __gbl_max_mpalloc_sleeptime, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("memstat_autoreport_freq",
                 "Dump memory usage to trace files at this frequency (in "
                 "secs). (Default: 180 secs)",
                 TUNABLE_INTEGER, &gbl_memstat_freq, gbl_memstat_freq, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("morecolumns", NULL, TUNABLE_BOOLEAN, &gbl_morecolumns, gbl_morecolumns,
                 READONLY | NOARG | READEARLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("move_deadlock_max_attempt", NULL, TUNABLE_INTEGER,
                 &gbl_move_deadlk_max_attempt, gbl_move_deadlk_max_attempt, 0, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("name", NULL, TUNABLE_STRING, &name, DEPRECATED | READONLY,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("natural_types", "Same as 'nosurprise'", TUNABLE_BOOLEAN,
                 &gbl_surprise, gbl_surprise, INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("netbufsz", "Size of the network buffer (per "
                             "node) for the replication network. "
                             "(Default: 1MB)",
                 TUNABLE_INTEGER, &gbl_netbufsz, gbl_netbufsz, READONLY | NOZERO, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("netbufsz_signal", "Size of the network buffer "
                                    "(per node) for the signal "
                                    "network. (Default: 65536)",
                 TUNABLE_INTEGER, &gbl_netbufsz_signal, gbl_netbufsz_signal, READONLY | NOZERO, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "net_explicit_flush_trace",
    "Produce a stack dump for long network flushes. (Default: off)",
    TUNABLE_BOOLEAN, &explicit_flush_trace, explicit_flush_trace, READONLY | NOARG, NULL, NULL, NULL,
    NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("net_lmt_upd_incoherent_nodes", NULL, TUNABLE_INTEGER,
                 &gbl_net_lmt_upd_incoherent_nodes, gbl_net_lmt_upd_incoherent_nodes, 0, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("net_max_mem",
                 "Maximum size (in MB) of items keep on replication network "
                 "queue before dropping (per replicant). (Default: 0)",
                 TUNABLE_INTEGER, &gbl_net_max_mem, gbl_net_max_mem, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("net_max_queue",
                 "Maximum number of items to keep on replication network queue "
                 "before dropping (per replicant). (Default: 25000)",
                 TUNABLE_INTEGER, &gbl_net_max_queue, gbl_net_max_queue, READONLY, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("net_max_queue_signal",
                 "Maximum number of items to keep on the signal network queue "
                 "before dropping (per replicant). (Default: 100)",
                 TUNABLE_INTEGER, &gbl_net_max_queue_signal, gbl_net_max_queue_signal, READONLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("net_poll",
                 "Allow a connection to linger for this many milliseconds "
                 "before identifying itself. Connections that take longer are "
                 "shut down. (Default: 100ms)",
                 TUNABLE_INTEGER, &gbl_net_poll, gbl_net_poll, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("net_portmux_register_interval",
                 "Check on this interval if our port is correctly registered "
                 "with pmux for the replication net. (Default: 600ms)",
                 TUNABLE_INTEGER, &gbl_net_portmux_register_interval, gbl_net_portmux_register_interval, READONLY,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("net_throttle_percent", NULL, TUNABLE_INTEGER,
                 &gbl_net_throttle_percent, gbl_net_throttle_percent, READONLY, NULL, percent_verify,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("nice", "If set, nice() will be called with this "
                         "value to set the database nice level.",
                 TUNABLE_INTEGER, &gbl_nice, gbl_nice, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("noblobstripe", "Disables 'blobstripe'", TUNABLE_BOOLEAN,
                 &gbl_blobstripe, gbl_blobstripe, INVERSE_VALUE | READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("noblocksql_over_sockets", "Disables 'blocksql_over_sockets'",
                 TUNABLE_BOOLEAN, &gbl_upgrade_blocksql_to_socksql, gbl_upgrade_blocksql_to_socksql,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("no_compress_page_compact_log",
                 "Disables 'compress_page_compact_log'", TUNABLE_BOOLEAN,
                 &gbl_compress_page_compact_log, gbl_compress_page_compact_log,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("nocrc32c", "Disables 'crc32c'", TUNABLE_BOOLEAN, &gbl_crc32c, gbl_crc32c,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("nodeid", NULL, TUNABLE_INTEGER, &gbl_mynodeid, gbl_mynodeid, READONLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("noearly", "Disables 'early'", TUNABLE_BOOLEAN, &gbl_early, gbl_early,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("noenv_messages", NULL, TUNABLE_BOOLEAN, &gbl_noenv_messages, gbl_noenv_messages,
                 READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("no_epochms_repts", "Disables 'epochms_repts'",
                 TUNABLE_BOOLEAN, &gbl_berkdb_epochms_repts, gbl_berkdb_epochms_repts,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("no_exit_on_internal_failure",
                 "Disables 'exit_on_internal_failure'", TUNABLE_BOOLEAN,
                 &gbl_exit_on_internal_error, gbl_exit_on_internal_error, INVERSE_VALUE | READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("nokeycompr", "Disables 'keycompr'", TUNABLE_BOOLEAN,
                 &gbl_keycompr, gbl_keycompr, INVERSE_VALUE | READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("no_lock_conflict_trace", "Disables 'lock_conflict_trace'",
                 TUNABLE_BOOLEAN, &gbl_lock_conflict_trace, gbl_lock_conflict_trace,
                 INVERSE_VALUE | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("nonames", NULL, TUNABLE_BOOLEAN, &gbl_nonames, gbl_nonames,
                 READONLY | NOARG | READEARLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("no_net_explicit_flush_trace",
                 "Disables 'net_explicit_flush_trace'", TUNABLE_BOOLEAN,
                 &explicit_flush_trace, explicit_flush_trace, INVERSE_VALUE | READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("no_null_blob_fix", "Disables 'null_blob_fix'",
                 TUNABLE_BOOLEAN, &gbl_disallow_null_blobs, gbl_disallow_null_blobs,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("norcache", "Disables 'rcache'", TUNABLE_BOOLEAN, &gbl_rcache, gbl_rcache,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("noreallearly", "Disables 'reallearly'", TUNABLE_BOOLEAN,
                 &gbl_reallyearly, gbl_reallyearly, INVERSE_VALUE | READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("norepdebug", "Disables 'repdebug'", TUNABLE_BOOLEAN,
                 &gbl_repdebug, gbl_repdebug, INVERSE_VALUE | READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("no_rep_process_txn_trace", "Disables 'rep_process_txn_trace'",
                 TUNABLE_BOOLEAN, &gbl_rep_process_txn_time, gbl_rep_process_txn_time,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("no_round_robin_stripes", "Disables 'round_robin_stripes'",
                 TUNABLE_BOOLEAN, &gbl_round_robin_stripes, gbl_round_robin_stripes,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("no_sc_inco_chk", NULL, TUNABLE_BOOLEAN, &gbl_sc_inco_chk, gbl_sc_inco_chk,
                 READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("no_static_tag_blob_fix", NULL, TUNABLE_BOOLEAN,
                 &gbl_force_notnull_static_tag_blobs, gbl_force_notnull_static_tag_blobs,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("nosurprise", NULL, TUNABLE_BOOLEAN, &gbl_surprise, gbl_surprise,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("notimeout", "Turns off SQL timeouts. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_notimeouts, gbl_notimeouts, NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("no_toblock_net_throttle", "Disables 'toblock_net_throttle'",
                 TUNABLE_BOOLEAN, &gbl_toblock_net_throttle, gbl_toblock_net_throttle,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("noudp", NULL, TUNABLE_BOOLEAN, &gbl_udp, gbl_udp,
                 INVERSE_VALUE | READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("no_update_delete_limit", NULL, TUNABLE_BOOLEAN,
                 &gbl_update_delete_limit, gbl_update_delete_limit, INVERSE_VALUE | READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("nowatch", "Disable watchdog. Watchdog aborts "
                            "the database if basic things like "
                            "creating threads, allocating memory, "
                            "etc. doesn't work. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_watchdog_disable_at_start, gbl_watchdog_disable_at_start,
                 READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("null_blob_fix", NULL, TUNABLE_BOOLEAN,
                 &gbl_disallow_null_blobs, gbl_disallow_null_blobs, READONLY | NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "nullfkey",
    "Do not enforce foreign key constraints for null keys. (Default: off)",
    TUNABLE_BOOLEAN, &gbl_nullfkey, gbl_nullfkey, READONLY | NOARG | READEARLY, NULL, NULL,
    NULL, NULL);
/*
REGISTER_TUNABLE_WITH_INT_DEFAULT("nullsort", NULL, TUNABLE_ENUM,
                 &placeholder, placeholder, READONLY, NULL, NULL, NULL, NULL);
*/
REGISTER_TUNABLE_WITH_INT_DEFAULT("num_contexts", NULL, TUNABLE_INTEGER, &gbl_num_contexts, gbl_num_contexts,
                 READONLY | NOZERO, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("num_qdbs", NULL, TUNABLE_INTEGER, &db->num_qdbs, NULL, READONLY,
                 NULL, NULL, num_qdbs_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("num_record_converts",
                 "During schema changes, pack this many records into a "
                 "transaction. (Default: 100)",
                 TUNABLE_INTEGER, &gbl_num_record_converts, gbl_num_record_converts, READONLY | NOZERO,
                 NULL, NULL, NULL, NULL);
/* Backwards compatibility: This tunable DOES expect an argument. */
REGISTER_TUNABLE_WITH_INT_DEFAULT("oldrangexlim", NULL, TUNABLE_BOOLEAN,
                 &gbl_honor_rangextunit_for_old_apis, gbl_honor_rangextunit_for_old_apis, READONLY, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("optimize_repdb_truncate",
                 "Enables use of optimized repdb truncate code. (Default: on)",
                 TUNABLE_BOOLEAN, &gbl_optimize_truncate_repdb, gbl_optimize_truncate_repdb,
                 READONLY | NOARG | READEARLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("osql_bkoff_netsend", NULL, TUNABLE_INTEGER,
                 &gbl_osql_bkoff_netsend, gbl_osql_bkoff_netsend, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("osql_bkoff_netsend_lmt", NULL, TUNABLE_INTEGER,
                 &gbl_osql_bkoff_netsend_lmt, gbl_osql_bkoff_netsend_lmt, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("osql_blockproc_timeout_sec", NULL, TUNABLE_INTEGER,
                 &gbl_osql_blockproc_timeout_sec, gbl_osql_blockproc_timeout_sec, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("osql_heartbeat_alert_time", NULL, TUNABLE_INTEGER,
                 &gbl_osql_heartbeat_alert, gbl_osql_heartbeat_alert, READONLY | NOZERO, NULL,
                 osql_heartbeat_alert_time_verify, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("osql_heartbeat_send_time", NULL, TUNABLE_INTEGER,
                 &gbl_osql_heartbeat_send, gbl_osql_heartbeat_send, READONLY | NOZERO, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("osql_max_queue", NULL, TUNABLE_INTEGER, &gbl_osql_max_queue, gbl_osql_max_queue,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("osql_net_poll",
                 "Like net_sql, but for the offload network (used by write "
                 "transactions on replicants to send work to the master) "
                 "(Default: 100ms)",
                 TUNABLE_INTEGER, &gbl_osql_net_poll, gbl_osql_net_poll, READONLY, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("osql_net_portmux_register_interval", NULL, TUNABLE_INTEGER,
                 &gbl_osql_net_portmux_register_interval, gbl_osql_net_portmux_register_interval, READONLY, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("osqlprefaultthreads",
                 "If set, send prefaulting hints to nodes. (Default: 0)",
                 TUNABLE_INTEGER, &gbl_osqlpfault_threads, gbl_osqlpfault_threads, READONLY, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("osql_verify_ext_chk",
                 "For block transaction mode only - after this many verify "
                 "errors, check if transaction is non-commitable (see default "
                 "isolation level). (Default: on)",
                 TUNABLE_INTEGER, &gbl_osql_verify_ext_chk, gbl_osql_verify_ext_chk, READONLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("osql_verify_retry_max",
                 "Retry a transaction on a verify error this many times (see "
                 "optimistic concurrency control). (Default: 499)",
                 TUNABLE_INTEGER, &gbl_osql_verify_retries_max, gbl_osql_verify_retries_max, READONLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("override_cachekb", NULL, TUNABLE_INTEGER,
                 &db->override_cacheszkb, 0, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("page_compact_latency_ms", NULL, TUNABLE_INTEGER,
                 &gbl_pg_compact_latency_ms, gbl_pg_compact_latency_ms, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_DOUBLE_DEFAULT("page_compact_target_ff", NULL, TUNABLE_DOUBLE,
                 &gbl_pg_compact_target_ff, gbl_pg_compact_target_ff, NOARG, NULL, NULL,
                 page_compact_target_ff_update, NULL);
REGISTER_TUNABLE_WITH_DOUBLE_DEFAULT("page_compact_thresh_ff", NULL, TUNABLE_DOUBLE,
                 &gbl_pg_compact_thresh, gbl_pg_compact_thresh, READONLY | NOARG, NULL, NULL,
                 page_compact_thresh_ff_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("page_latches",
                 "If set, in rowlocks mode, will acquire fast latches on pages "
                 "instead of full locks. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_page_latches, gbl_page_latches, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
/*
REGISTER_TUNABLE_WITH_INT_DEFAULT("pagesize", NULL, TUNABLE_INTEGER,
                 &placeholder, placeholder, DEPRECATED|READONLY, NULL, NULL, NULL,
                 NULL);
*/
REGISTER_TUNABLE_WITH_INT_DEFAULT("parallel_recovery", NULL, TUNABLE_INTEGER,
                 &gbl_parallel_recovery_threads, gbl_parallel_recovery_threads, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("penaltyincpercent", NULL, TUNABLE_INTEGER,
                 &gbl_penaltyincpercent, gbl_penaltyincpercent, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("perfect_ckp", NULL, TUNABLE_INTEGER, &gbl_use_perfect_ckp, gbl_use_perfect_ckp,
                 READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_DEFAULT("portmux_bind_path", NULL, TUNABLE_STRING,
                 &gbl_portmux_unix_socket, gbl_portmux_unix_socket, READONLY | READEARLY, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("portmux_port", NULL, TUNABLE_INTEGER, &portmux_port, portmux_port,
                 READONLY | READEARLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("prefaulthelper_blockops", NULL, TUNABLE_INTEGER,
                 &gbl_prefaulthelper_blockops, gbl_prefaulthelper_blockops, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("prefaulthelper_sqlreadahead", NULL, TUNABLE_INTEGER,
                 &gbl_prefaulthelper_sqlreadahead, gbl_prefaulthelper_sqlreadahead, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("prefaulthelperthreads",
                 "Max number of prefault helper threads. (Default: 0)",
                 TUNABLE_INTEGER, &gbl_prefaulthelperthreads, gbl_prefaulthelperthreads, READONLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("print_syntax_err",
                 "Trace all SQL with syntax errors. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_print_syntax_err, gbl_print_syntax_err, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("queuepoll", "Occasionally wake up and poll "
                              "consumer queues even when no "
                              "events require it. (Default: 5secs)",
                 TUNABLE_INTEGER, &gbl_queue_sleeptime, gbl_queue_sleeptime, READONLY, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("random_lock_release_interval", NULL, TUNABLE_INTEGER,
                 &gbl_sql_random_release_interval, gbl_sql_random_release_interval, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("rangextlim", NULL, TUNABLE_INTEGER, &gbl_rangextunit, gbl_rangextunit,
                 READONLY | NOZERO, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "rcache", "Keep a lookaside cache of root pages for B-trees. (Default: on)",
    TUNABLE_BOOLEAN, &gbl_rcache, gbl_rcache, READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("reallearly",
                 "Acknowledge as soon as a commit record is seen by the "
                 "replicant (before it's applied). This effectively makes "
                 "replication asynchronous, so reads may not see the effects "
                 "of a committed transaction yet. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_reallyearly, gbl_reallyearly, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("reject_osql_mismatch", "(Default: on)", TUNABLE_BOOLEAN,
                 &gbl_reject_osql_mismatch, gbl_reject_osql_mismatch, READONLY | NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("repchecksum",
                 "Enable to perform additional checksumming of replication "
                 "stream. Note: Log records in replication stream already have "
                 "checksums. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_repchecksum, gbl_repchecksum, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("repdebug", "Enables replication debug messages.",
                 TUNABLE_BOOLEAN, &gbl_repdebug, gbl_repdebug, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("replicant_latches",
                 "Also acquire latches on replicants. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_replicant_latches, gbl_replicant_latches, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("replicate_local",
                 "When enabled, record all database events to a comdb2_oplog "
                 "table. This can be used to set clusters/instances that are "
                 "fed data from a database cluster. Alternate ways of doing "
                 "this are being planned, so enabling this option should not "
                 "be needed in the near future. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_replicate_local, gbl_replicate_local, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("replicate_local_concurrent", NULL, TUNABLE_BOOLEAN,
                 &gbl_replicate_local_concurrent, gbl_replicate_local_concurrent, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("report_deadlock_verbose",
                 "If set, dump the current thread's stack for every deadlock. "
                 "(Default: off)",
                 TUNABLE_BOOLEAN, &gbl_disable_deadlock_trace, gbl_disable_deadlock_trace, NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("deadlkon", "Same as 'report_deadlock_verbose'",
                 TUNABLE_BOOLEAN, &gbl_disable_deadlock_trace, gbl_disable_deadlock_trace, NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("deadlkoff", "Disables 'report_deadlock_verbose'",
                 TUNABLE_BOOLEAN, &gbl_disable_deadlock_trace, gbl_disable_deadlock_trace,
                 INVERSE_VALUE | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("rep_process_txn_trace",
                 "If set, report processing time on replicant for all "
                 "transactions. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_rep_process_txn_time, gbl_rep_process_txn_time, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("reqldiffstat", NULL, TUNABLE_INTEGER, &diffstat_thresh, diffstat_thresh,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("reqltruncate", NULL, TUNABLE_INTEGER, &reqltruncate, reqltruncate, READONLY,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("retry", NULL, TUNABLE_INTEGER, &db->retry, 10, READONLY, NULL,
                 NULL, retry_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("round_robin_stripes",
                 "Alternate to which table stripe new records are written. The "
                 "default is to keep stripe affinity by writer. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_round_robin_stripes, gbl_round_robin_stripes, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("rr_enable_count_changes", NULL, TUNABLE_BOOLEAN,
                 &gbl_rrenablecountchanges, gbl_rrenablecountchanges, READONLY | NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("sbuftimeout", NULL, TUNABLE_INTEGER, &gbl_sbuftimeout, gbl_sbuftimeout,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("sc_del_unused_files_threshold", NULL, TUNABLE_INTEGER,
                 &gbl_sc_del_unused_files_threshold_ms, gbl_sc_del_unused_files_threshold_ms, READONLY | NOZERO, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("signal_net_portmux_register_interval", NULL, TUNABLE_INTEGER,
                 &gbl_signal_net_portmux_register_interval, gbl_signal_net_portmux_register_interval, READONLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("simulate_rowlock_deadlock", NULL, TUNABLE_INTEGER,
                 &gbl_simulate_rowlock_deadlock_interval, gbl_simulate_rowlock_deadlock_interval, 0, NULL, NULL,
                 simulate_rowlock_deadlock_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("singlemeta", NULL, TUNABLE_INTEGER, &gbl_init_single_meta, gbl_init_single_meta,
                 READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("skip_clear_queue_extents", NULL, TUNABLE_BOOLEAN,
                 &skip_clear_queue_extents, skip_clear_queue_extents, READONLY | NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("slowfget", NULL, TUNABLE_INTEGER, &__slow_memp_fget_ns, __slow_memp_fget_ns,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("slowread", NULL, TUNABLE_INTEGER, &__slow_read_ns, __slow_read_ns, READONLY,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("slow_rep_process_txn_freq", NULL, TUNABLE_INTEGER,
                 &gbl_slow_rep_process_txn_freq, gbl_slow_rep_process_txn_freq, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("slow_rep_process_txn_maxms", NULL, TUNABLE_INTEGER,
                 &gbl_slow_rep_process_txn_maxms, gbl_slow_rep_process_txn_maxms, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("slowwrite", NULL, TUNABLE_INTEGER, &__slow_write_ns, __slow_write_ns, READONLY,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("sort_nulls_with_header",
                 "Using record headers in key sorting. (Default: on)",
                 TUNABLE_BOOLEAN, &gbl_sort_nulls_correctly, gbl_sort_nulls_correctly, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("spfile", NULL, TUNABLE_STRING, &gbl_spfile_name, gbl_spfile_name, READONLY,
                 NULL, NULL, spfile_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("sqlflush", "Force flushing the current record "
                             "stream to client every specified "
                             "number of records. (Default: 0)",
                 TUNABLE_INTEGER, &gbl_sqlflush_freq, gbl_sqlflush_freq, READONLY, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "sqlrdtimeout",
    "Set timeout for reading from an SQL connection. (Default: 100000ms)",
    TUNABLE_INTEGER, &gbl_sqlrdtimeoutms, gbl_sqlrdtimeoutms, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("sqlreadahead", NULL, TUNABLE_INTEGER, &gbl_sqlreadahead, gbl_sqlreadahead,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("sqlreadaheadthresh", NULL, TUNABLE_INTEGER,
                 &gbl_sqlreadaheadthresh, gbl_sqlreadaheadthresh, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("sqlsortermem", "Maximum amount of memory to be "
                                 "allocated to the sqlite sorter. "
                                 "(Default: 314572800)",
                 TUNABLE_INTEGER, &gbl_sqlite_sorter_mem, gbl_sqlite_sorter_mem, READONLY, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("sqlsortermult", NULL, TUNABLE_INTEGER, &gbl_sqlite_sortermult, gbl_sqlite_sortermult,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("sql_time_threshold",
                 "Sets the threshold time in ms after which queries are "
                 "reported as running a long time. (Default: 5000 ms)",
                 TUNABLE_INTEGER, &gbl_sql_time_threshold, gbl_sql_time_threshold, READONLY, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE("sql_tranlevel_default",
                 "Sets the default SQL transaction level for the database.",
                 TUNABLE_ENUM, &gbl_sql_tranlevel_default, READONLY,
                 sql_tranlevel_default_value, NULL,
                 sql_tranlevel_default_update, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "sqlwrtimeout",
    "Set timeout for writing to an SQL connection. (Default: 10000ms)",
    TUNABLE_INTEGER, &gbl_sqlwrtimeoutms, gbl_sqlwrtimeoutms, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("static_tag_blob_fix", NULL, TUNABLE_BOOLEAN,
                 &gbl_force_notnull_static_tag_blobs, gbl_force_notnull_static_tag_blobs, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("surprise", NULL, TUNABLE_BOOLEAN, &gbl_surprise, gbl_surprise,
                 READONLY | NOARG, NULL, NULL, NULL, NULL);
/*
  Note: survive_n_master_swings' value < 0 was previously ignored without
  any error.
*/
REGISTER_TUNABLE_WITH_INT_DEFAULT("survive_n_master_swings",
                 "Have a node retry applying a transaction against a new "
                 "master this many times before giving up. (Default: 600)",
                 TUNABLE_INTEGER, &gbl_survive_n_master_swings, gbl_survive_n_master_swings, READONLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("temptable_limit",
                 "Set the maximum number of temporary tables the database can "
                 "create. (Default: 8192)",
                 TUNABLE_INTEGER, &gbl_temptable_pool_capacity, gbl_temptable_pool_capacity, READONLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("test_blob_race", NULL, TUNABLE_INTEGER, &gbl_test_blob_race, gbl_test_blob_race,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("test_scindex_deadlock",
                 "Test index on expressions schema change deadlock",
                 TUNABLE_BOOLEAN, &gbl_test_scindex_deadlock, READONLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE("test_sc_resume_race",
                 "Test race between schemachange resume and blockprocessor",
                 TUNABLE_BOOLEAN, &gbl_test_sc_resume_race, READONLY, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("throttlesqloverlog",
                 "On a full queue of SQL requests, dump the current thread "
                 "pool this often (in secs). (Default: 5sec)",
                 TUNABLE_INTEGER, &gbl_throttle_sql_overload_dump_sec, gbl_throttle_sql_overload_dump_sec, READONLY,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("toblock_net_throttle",
                 "Throttle writes in apply_changes. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_toblock_net_throttle, gbl_toblock_net_throttle, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("track_berk_locks", NULL, TUNABLE_INTEGER,
                 &gbl_berkdb_track_locks, gbl_berkdb_track_locks, READONLY | NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("udp", NULL, TUNABLE_BOOLEAN, &gbl_udp, gbl_udp, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("unnatural_types", "Same as 'surprise'", TUNABLE_BOOLEAN,
                 &gbl_surprise, gbl_surprise, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("update_delete_limit", NULL, TUNABLE_BOOLEAN,
                 &gbl_update_delete_limit, gbl_update_delete_limit, READONLY | NOARG, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("updategenids",
                 "Enable use of update genid scheme. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_updategenids, gbl_updategenids, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("update_shadows_interval",
                 "Set to higher than 0 to update snaphots on every Nth "
                 "operation. (Default: 0, update on for every operation)",
                 TUNABLE_INTEGER, &gbl_update_shadows_interval, gbl_update_shadows_interval, 0, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("upd_null_cstr_return_conv_err", NULL, TUNABLE_INTEGER,
                 &gbl_upd_null_cstr_return_conv_err, gbl_upd_null_cstr_return_conv_err, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("use_appsock_as_sqlthread", NULL, TUNABLE_INTEGER,
                 &gbl_use_appsock_as_sqlthread, gbl_use_appsock_as_sqlthread, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("use_bbipc_fastseed", NULL, TUNABLE_BOOLEAN,
                 &gbl_use_bbipc_global_fastseed, gbl_use_bbipc_global_fastseed, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("use_live_schema_change", NULL, TUNABLE_INTEGER,
                 &gbl_default_livesc, gbl_default_livesc, READONLY | NOARG, NULL, NULL, NULL, NULL);
/*
REGISTER_TUNABLE_WITH_INT_DEFAULT("use_llmeta", NULL, TUNABLE_INTEGER,
                 &gbl_use_llmeta, gbl_use_llmeta, READONLY, NULL, NULL, NULL, NULL);
*/
REGISTER_TUNABLE_WITH_INT_DEFAULT("usenames", NULL, TUNABLE_BOOLEAN, &gbl_nonames, gbl_nonames,
                 INVERSE_VALUE | READONLY | NOARG | READEARLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("use_node_priority",
                 "Sets node priority for the db. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_use_node_pri, gbl_use_node_pri, READONLY | NOARG, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("use_nondedicated_network", NULL, TUNABLE_BOOLEAN | NOARG,
                 &_non_dedicated_subnet, _non_dedicated_subnet, READONLY, NULL, NULL,
                 net_add_nondedicated_subnet, NULL);
/*
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "use_parallel_schema_change",
    "Scan stripes for a table in parallel during schema change. (Default: on)",
    TUNABLE_BOOLEAN, &gbl_default_sc_scanmode, gbl_default_sc_scanmode, READONLY, NULL, NULL, NULL,
    NULL);
*/

REGISTER_TUNABLE_WITH_INT_DEFAULT("use_planned_schema_change",
                 "Only change entities that need to change on a schema change. "
                 "Disable to always rebuild all data files and indices for the "
                 "changing table. (Default: 1)",
                 TUNABLE_INTEGER, &gbl_default_plannedsc, gbl_default_plannedsc, READONLY | NOARG,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("watchthreshold", NULL, TUNABLE_INTEGER,
                 &gbl_watchdog_watch_threshold, gbl_watchdog_watch_threshold, READONLY, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("ctrace_nlogs",
                 "When rolling trace files, keep this many. The older files "
                 "will have incrementing number suffixes (.1, .2, etc.). "
                 "(Default: 7)",
                 TUNABLE_INTEGER, &nlogs, nlogs, READONLY | NOZERO, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("ctrace_rollat",
                 "Roll database debug trace file "
                 "($COMDB2_ROOT/var/log/cdb2/$dbname.trc.c) at specified size. "
                 "Set to 0 (default) to never roll.",
                 TUNABLE_INTEGER, &rollat, rollat, READONLY | NOZERO, NULL, NULL,
                 ctrace_set_rollat, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "debugthreads",
    "If set to 'on' enables trace on thread events. (Default: off)",
    TUNABLE_BOOLEAN, &thread_debug, thread_debug, READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT(
    "dumpthreadonexit",
    "If set to 'on' dump resources held by a thread on exit. (Default: off)",
    TUNABLE_BOOLEAN, &dump_resources_on_thread_exit, dump_resources_on_thread_exit, READONLY, NULL, NULL, NULL,
    NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("stack_disable", NULL, TUNABLE_BOOLEAN, &gbl_walkback_enabled, gbl_walkback_enabled,
                 INVERSE_VALUE | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("stack_enable", NULL, TUNABLE_BOOLEAN, &gbl_walkback_enabled, gbl_walkback_enabled,
                 NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("stack_warn_threshold", NULL, TUNABLE_INTEGER, &gbl_warnthresh, gbl_warnthresh,
                 READONLY | NOZERO, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("ack_trace",
                 "Every second, produce trace for ack messages. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_ack_trace, gbl_ack_trace, READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("no_ack_trace", "Disables 'ack_trace'", TUNABLE_BOOLEAN,
                 &gbl_ack_trace, gbl_ack_trace, INVERSE_VALUE | READONLY | NOARG, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("bdblock_debug", NULL, TUNABLE_BOOLEAN, &gbl_bdblock_debug, gbl_bdblock_debug,
                 READONLY | NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("debug.autoanalyze", "debug autoanalyze operations",
                 TUNABLE_BOOLEAN, &gbl_debug_aa, gbl_debug_aa, NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("bdboslog", NULL, TUNABLE_INTEGER, &gbl_namemangle_loglevel, gbl_namemangle_loglevel,
                 READONLY, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("deadlock_rep_retry_max", NULL, TUNABLE_INTEGER,
                 &max_replication_trans_retries, max_replication_trans_retries, READONLY | NOZERO, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE("logmsg", NULL, TUNABLE_COMPOSITE, NULL, INTERNAL | READEARLY,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("logmsg.level",
                 "All messages below this level will not be logged.",
                 TUNABLE_ENUM, NULL, READEARLY, logmsg_level_value, NULL,
                 logmsg_level_update, NULL);
REGISTER_TUNABLE("logmsg.syslog", "Log messages to syslog.", TUNABLE_BOOLEAN,
                 NULL, NOARG | READEARLY, logmsg_syslog_value, NULL,
                 logmsg_syslog_update, NULL);
REGISTER_TUNABLE("logmsg.timestamp", "Stamp all messages with timestamp.",
                 TUNABLE_BOOLEAN, NULL, NOARG | READEARLY,
                 logmsg_timestamp_value, NULL, logmsg_timestamp_update, NULL);
REGISTER_TUNABLE("logmsg.notimestamp", "Disables 'syslog.timestamp'.",
                 TUNABLE_BOOLEAN, NULL, INVERSE_VALUE | NOARG | READEARLY,
                 logmsg_timestamp_value, NULL, logmsg_timestamp_update, NULL);
REGISTER_TUNABLE("block_set_commit_genid_trace",
                 "Print trace when blocking set commit_genid. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_block_set_commit_genid_trace, INTERNAL,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("abort_on_unset_ha_flag",
                 "Abort in snap_uid_retry if ha is unset. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_abort_on_unset_ha_flag, INTERNAL, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE("write_dummy_trace",
                 "Print trace when doing a dummy write. (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_write_dummy_trace, INTERNAL, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE("seed_genid", "Set genid-seed in hex for genid48 test.",
                 TUNABLE_STRING, NULL, EXPERIMENTAL | INTERNAL,
                 next_genid_value, NULL, genid_seed_update, NULL);
REGISTER_TUNABLE("abort_on_bad_upgrade",
                 "Abort in upgrade current-generation exceeds ctrl-gen.",
                 TUNABLE_BOOLEAN, &gbl_abort_on_incorrect_upgrade,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE_WITH_INT_DEFAULT("poll_in_pgfree_recover", "Poll pgfree recovery handler.",
                 TUNABLE_BOOLEAN, &gbl_poll_in_pg_free_recover, gbl_poll_in_pg_free_recover,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("rep_badgen_trace", "Trace on rep mismatched generations.",
                 TUNABLE_BOOLEAN, &gbl_rep_badgen_trace,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("dump_zero_coherency_ts", "Enable zero-coherency-ts trace.",
                 TUNABLE_BOOLEAN, &gbl_dump_zero_coherency_timestamp,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("allow_incoherent_sql", "Enable sql against incoherent nodes.",
                 TUNABLE_BOOLEAN, &gbl_allow_incoherent_sql,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("rep_process_msg_print_rc", "Print rc from rep_process_msg.",
                 TUNABLE_BOOLEAN, &gbl_rep_process_msg_print_rc,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("verbose_master_req",
                 "Print trace showing master-req protocol.", TUNABLE_BOOLEAN,
                 &gbl_verbose_master_req, EXPERIMENTAL | INTERNAL, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE("verbose_send_cohlease",
                 "Print trace from lease-issue thread.", TUNABLE_BOOLEAN,
                 &gbl_verbose_send_coherency_lease, EXPERIMENTAL | INTERNAL,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("reset_on_unelectable_cluster", "Reset master if unelectable.",
                 TUNABLE_BOOLEAN, &gbl_reset_on_unelectable_cluster,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("decoupled_logputs",
                 "Perform logputs out-of-band. (Default: on)", TUNABLE_BOOLEAN,
                 &gbl_decoupled_logputs, EXPERIMENTAL | INTERNAL, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE("apply_pollms",
                 "Apply-thread poll time before checking queue. "
                 "(Default: 100ms)",
                 TUNABLE_INTEGER, &gbl_apply_thread_pollms,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("rep_verify_always_grab_writelock",
                 "Force every rep_verify to grab writelock.", TUNABLE_BOOLEAN,
                 &gbl_rep_verify_always_grab_writelock, EXPERIMENTAL | INTERNAL,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("rep_verify_will_recover_trace",
                 "Trace rep_verify_will_recover.", TUNABLE_BOOLEAN,
                 &gbl_rep_verify_will_recover_trace, EXPERIMENTAL | INTERNAL,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("max_wr_rows_per_txn",
                 "Set the max written rows per transaction.", TUNABLE_INTEGER,
                 &gbl_max_wr_rows_per_txn, 0, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("print_deadlock_cycles",
                 "Print all deadlock cycles. (Default: off)", TUNABLE_BOOLEAN,
                 &gbl_print_deadlock_cycles, NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("always_send_cnonce",
                 "Always send cnonce to master. (Default: on)", TUNABLE_BOOLEAN,
                 &gbl_always_send_cnonce, NOARG, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("force_serial_on_writelock", "Disable parallel rep on "
                                              "upgrade.  (Default: on)",
                 TUNABLE_BOOLEAN, &gbl_force_serial_on_writelock,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("processor_thd_poll", "Poll before dispatching worker thds. "
                                       "(Default: 0ms)",
                 TUNABLE_INTEGER, &gbl_processor_thd_poll,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("time_rep_apply", "Display rep-apply times periodically. "
                                   "(Default: off)",
                 TUNABLE_BOOLEAN, &gbl_time_rep_apply, EXPERIMENTAL | INTERNAL,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("logput_window",
                 "Drop log-broadcasts for incoherent nodes "
                 "more than this many bytes behind.  (Default: 0)",
                 TUNABLE_INTEGER, &gbl_incoherent_logput_window,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("dump_full_netqueue", "Dump net-queue on full rcode. "
                                       "(Default: off)",
                 TUNABLE_BOOLEAN, &gbl_dump_full_net_queue,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE(
    "max_clientstats",
    "Max number of client stats stored in comdb2_clientstats. (Default: 10000)",
    TUNABLE_INTEGER, &gbl_max_clientstats_cache, DYNAMIC, NULL, NULL, NULL,
    NULL);
REGISTER_TUNABLE("max_logput_queue",
                 "Maximum queued log-records.  (Default: 100000)",
                 TUNABLE_INTEGER, &gbl_max_logput_queue,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("master_req_waitms",
                 "Request master once per this interval.  (Default: 200ms)",
                 TUNABLE_INTEGER, &gbl_master_req_waitms,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("req_all_threshold",
                 "Use req_all if a replicant is behind by "
                 "this amount or more.  (Default: 10000000)",
                 TUNABLE_INTEGER, &gbl_req_all_threshold,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("fill_throttle",
                 "Throttle fill-reqs to once per fill-throttle ms.  "
                 "(Default: 500ms)",
                 TUNABLE_INTEGER, &gbl_fills_waitms, EXPERIMENTAL | INTERNAL,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("verbose_fills", "Print fill trace.  (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_verbose_fills, EXPERIMENTAL | INTERNAL,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("verbose_repdups",
                 "Print trace on duplicate replication.  (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_verbose_repdups, EXPERIMENTAL | INTERNAL,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("warn_queue_latency",
                 "Trace for log queues processed that are older than this.  "
                 "(Default: 500ms)",
                 TUNABLE_INTEGER, &gbl_warn_queue_latency_threshold,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("print_net_queue_size",
                 "Trace for net queue size.  (Default: off)", TUNABLE_BOOLEAN,
                 &gbl_print_net_queue_size, EXPERIMENTAL | INTERNAL, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE("verbose_repmore_trace",
                 "Verbose trace for rep-more requests.  (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_trace_repmore_reqs,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("throttle_logput_trace",
                 "Print trace when stopping logputs "
                 "to incoherent nodes.  (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_throttle_logput_trace,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("catchup_window_trace",
                 "Print master catchup window trace.  (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_catchup_window_trace,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("early_ack_trace",
                 "Print trace when sending an early ack.  (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_early_ack_trace, EXPERIMENTAL | INTERNAL,
                 NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("commit_delay_trace", "Verbose commit-delays.  (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_commit_delay_trace,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("set_coherent_state_trace",
                 "Verbose coherency trace.  (Default: off)", TUNABLE_BOOLEAN,
                 &gbl_set_coherent_state_trace, EXPERIMENTAL | INTERNAL, NULL,
                 NULL, NULL, NULL);
REGISTER_TUNABLE("finish_fill_threshold",
                 "Fill to end if end is less than this.  (Default: 60000000)",
                 TUNABLE_INTEGER, &gbl_finish_fill_threshold,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("req_delay_count_threshold",
                 "Request commit-delay if falling "
                 "behind this many times.  (Default: 5)",
                 TUNABLE_INTEGER, &gbl_req_delay_count_threshold,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("max_apply_dequeue",
                 "Limit apply-processing to this many per "
                 "loop.  this many times.  (Default: 100000)",
                 TUNABLE_INTEGER, &gbl_max_apply_dequeue,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("last_locked_seqnum",
                 "Broadcast last-locked variable as seqnum.  (Default: on)",
                 TUNABLE_BOOLEAN, &gbl_last_locked_seqnum,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("rep_getlock_latency",
                 "Sleep on replicant before getting locks.  (Default: 0)",
                 TUNABLE_INTEGER, &gbl_getlock_latencyms,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("net_writer_poll_ms",
                 "Poll time for net writer thread.  (Default: 1000)",
                 TUNABLE_INTEGER, &gbl_net_writer_thread_poll_ms,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("inmem_repdb_maxlog",
                 "Maximum records for in-memory replist.  "
                 "(Default: 10000)",
                 TUNABLE_INTEGER, &gbl_inmem_repdb_maxlog,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("durable_set_trace",
                 "Trace setting durable lsn.  (Default: off)", TUNABLE_BOOLEAN,
                 &gbl_durable_set_trace, EXPERIMENTAL | INTERNAL, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE("set_seqnum_trace",
                 "Trace setting setting seqnum.  (Default: off)",
                 TUNABLE_BOOLEAN, &gbl_set_seqnum_trace,
                 EXPERIMENTAL | INTERNAL, NULL, NULL, NULL, NULL);
REGISTER_TUNABLE("elect_priority_bias",
                 "Bias this node's election priority by this amount.  "
                 "(Default: 0)",
                 TUNABLE_INTEGER, &gbl_elect_priority_bias, 0, NULL, NULL, NULL,
                 NULL);
REGISTER_TUNABLE("apply_queue_memory",
                 "Current memory usage of apply-queue.  (Default: 0)",
                 TUNABLE_INTEGER, &gbl_apply_queue_memory, READONLY, NULL, NULL,
                 NULL, NULL);
REGISTER_TUNABLE("inmem_repdb_memory",
                 "Current memory usage of in-memory repdb.  (Default: 0)",
                 TUNABLE_INTEGER, &gbl_inmem_repdb_memory, READONLY, NULL, NULL,
                 NULL, NULL);

#endif /* _DB_TUNABLES_H */
