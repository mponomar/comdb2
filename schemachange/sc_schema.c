/*
   Copyright 2015 Bloomberg Finance L.P.

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
#include <sys/statvfs.h>

#include <memory_sync.h>
#include "schemachange.h"
#include "sc_util.h"
#include "sc_global.h"
#include "sc_schema.h"
#include "sc_callbacks.h"
#include "intern_strings.h"
#include "views.h"
#include "logmsg.h"
#include "comdb2_atomic.h"

extern int gbl_partial_indexes;
extern uint64_t gbl_sc_headroom;

int verify_record_constraint(const struct ireq *iq, const struct dbtable *db, void *trans,
                             const void *old_dta, unsigned long long ins_keys,
                             blob_buffer_t *blobs, int maxblobs,
                             const char *from, int rebuild, int convert)
{
    int rc = 0;
    void *new_dta = NULL;
    struct convert_failure reason;
    struct ireq ruleiq;

    if (db->n_constraints == 0) {
        return 0;
    }

    const void *od_dta;
    if (rebuild && convert) {
        /* .ONDISK -> .NEW..ONDISK */
        /* if record added while schema change is on */
        new_dta = malloc(db->lrl);
        if (new_dta == NULL) {
            logmsg(LOGMSG_ERROR, "%s() malloc failed\n", __func__);
            rc = ERR_CONSTR;
            goto done;
        }
        rc = stag_to_stag_buf(db, from, old_dta, ".NEW..ONDISK",
                              new_dta, &reason);
        if (rc) {
            rc = ERR_CONSTR;
            goto done;
        }
        od_dta = new_dta;
        from = ".NEW..ONDISK";
    }
    else {
        od_dta = old_dta;
    }

    init_fake_ireq(thedb, &ruleiq);
    ruleiq.opcode = OP_REBUILD;
    ruleiq.debug = gbl_who;

    for (int ci = 0; ci < db->n_constraints; ci++) {
        const constraint_t *ct = &(db->constraints[ci]);

        char lcl_tag[MAXTAGLEN];
        char lcl_key[MAXKEYLEN];
        int lcl_idx;
        int lcl_len;

        /* Name: .NEW.COLUMNNAME -> .NEW..ONDISK_IX_nn */
        snprintf(lcl_tag, sizeof lcl_tag, ".NEW.%s", ct->lclkeyname);
        rc = getidxnumbyname(db, lcl_tag, &lcl_idx);
        if (rc) {
            logmsg(LOGMSG_ERROR, "could not get index for %s\n", lcl_tag);
            rc = ERR_CONSTR;
            break;
        }
        if (gbl_partial_indexes && db->ix_partial && !(ins_keys & (1ULL << lcl_idx))) {
            continue;
        }
        int len = snprintf(lcl_tag, sizeof lcl_tag, ".NEW..ONDISK_IX_%d", lcl_idx);
        if (len >= sizeof lcl_tag) {
            rc = ERR_BUF_TOO_SMALL;
            break;
        }

        /* Data -> Key : ONDISK -> .ONDISK_IX_nn */
        if (iq->idxInsert && !convert)
            memcpy(lcl_key, iq->idxInsert[lcl_idx], db->ix_keylen[lcl_idx]);
        else
            rc = stag_to_stag_buf_blobs(db, from, od_dta, lcl_tag,
                                        lcl_key, NULL, blobs, maxblobs, 0);
        if (rc) {
            rc = ERR_CONSTR;
            break;
        }

        lcl_len = getkeysize(db, lcl_idx);
        if (lcl_len < 0) {
            rc = ERR_CONSTR;
            break;
        }

        int ri;
        rc = check_single_key_constraint(&ruleiq, ct, lcl_tag, lcl_key,
                                         db, trans, &ri);
        if (rc == RC_INTERNAL_RETRY) {
            break;
        } else if (rc && rc != ERR_CONSTR) {
            logmsg(LOGMSG_ERROR, "fk violation: %s @ %s -> %s @ %s, rc=%d\n",
                   ct->lclkeyname, db->tablename, ct->keynm[ri], ct->table[ri],
                   rc);
            fsnapf(stderr, lcl_key, lcl_len > 32 ? 32 : lcl_len);
            logmsg(LOGMSG_ERROR, "\n");
            rc = ERR_CONSTR;
            break;
        }
    }

done:
    if (new_dta) free(new_dta);
    return rc;
}

int verify_partial_rev_constraint(struct dbtable *to_db, struct dbtable *newdb,
                                  void *trans, void *od_dta,
                                  unsigned long long ins_keys, const char *from)
{
    int i = 0, rc = 0;
    struct ireq ruleiq;
    if (!gbl_partial_indexes || !newdb->ix_partial) return 0;

    init_fake_ireq(thedb, &ruleiq);
    ruleiq.opcode = OP_REBUILD;
    ruleiq.debug = gbl_who;

    for (i = 0; i < to_db->n_rev_constraints; i++) {
        int j = 0;
        constraint_t *cnstrt = to_db->rev_constraints[i];
        char rkey[MAXKEYLEN];
        int rixnum = 0, rixlen = 0;
        char rondisk_tag[MAXTAGLEN];
        for (j = 0; j < cnstrt->nrules; j++) {
            char ondisk_tag[MAXTAGLEN];
            int ixnum = 0;
            struct dbtable *ldb;
            char lkey[MAXKEYLEN];
            int fndrrn;
            unsigned long long genid;
            if (strcasecmp(cnstrt->table[j], to_db->tablename)) {
                continue;
            }
            ldb = get_dbtable_by_name(cnstrt->table[j]);
            if (strcasecmp(ldb->tablename, newdb->tablename)) {
                logmsg(LOGMSG_FATAL, "%s: failed to find table\n", __func__);
                abort();
                return ERR_INTERNAL;
            }
            ldb = newdb;
            snprintf(ondisk_tag, sizeof(ondisk_tag), ".NEW.%s",
                     cnstrt->keynm[j]);
            rc = getidxnumbyname(ldb, ondisk_tag, &ixnum);
            if (rc) {
                logmsg(LOGMSG_ERROR, "%s: unknown keytag '%s'\n", __func__,
                       ondisk_tag);
                return ERR_CONVERT_IX;
            }

            /* This key will be part of the record, no need to check */
            if (ins_keys & (1ULL << ixnum)) continue;

            /* From now on, it means this record doesn't touch the partial index
             * (ixnum). We need to check if someone else had constraints on this
             * before. */
            snprintf(ondisk_tag, sizeof(ondisk_tag), ".NEW..ONDISK_IX_%d",
                     ixnum);
            /* Data -> Key : ONDISK -> .ONDISK_IX_nn */
            rc = stag_to_stag_buf(newdb, from, od_dta, ondisk_tag,
                                  lkey, NULL);
            if (rc) {
                logmsg(LOGMSG_ERROR, "%s: failed to convert to '%s'\n",
                       __func__, ondisk_tag);
                return ERR_CONVERT_IX;
            }
            /* here we convert the key into return db format */
            rc = getidxnumbyname(get_dbtable_by_name(cnstrt->lcltable->tablename),
                                 cnstrt->lclkeyname, &rixnum);
            if (rc) {
                logmsg(LOGMSG_ERROR, "%s: unknown keytag '%s'\n", __func__,
                       cnstrt->lclkeyname);
                return ERR_CONVERT_IX;
            }

            snprintf(rondisk_tag, sizeof(rondisk_tag), ".ONDISK_IX_%d", rixnum);

            int nulls = 0;

            rixlen = rc = stag_to_stag_buf_ckey(
                ldb, ondisk_tag, lkey, cnstrt->lcltable->tablename,
                rondisk_tag, rkey, &nulls, PK2FK);
            if (rc == -1) {
                /* I followed the logic in check_update_constraints */
                logmsg(LOGMSG_ERROR,
                       "%s: cant form key for source table, continue\n",
                       __func__);
                continue;
            }
            if (skip_lookup_for_nullfkey(cnstrt->lcltable, rixnum, nulls)) continue;

            if (cnstrt->lcltable->ix_collattr[rixnum]) {
                rc = extract_decimal_quantum(cnstrt->lcltable, rixnum, rkey,
                                             NULL, 0, NULL);
                if (rc) {
                    abort(); /* called doesn't return error for these arguments,
                                at least not now */
                }
            }

            ruleiq.usedb = cnstrt->lcltable;
            rc = ix_find_by_key_tran(&ruleiq, rkey, rixlen, rixnum, NULL,
                                     &fndrrn, &genid, NULL, NULL, 0, trans);
            if (rc == IX_FND || rc == IX_FNDMORE)
                /* A key in a child table index is referencing this key */
                return ERR_CONSTR;
            else if (rc == IX_PASTEOF || rc == IX_NOTFND || rc == IX_EMPTY)
                /* No such key in the child table index */
                continue;
            else if (rc == RC_INTERNAL_RETRY)
                return rc;
            else {
                logmsg(LOGMSG_ERROR, "%s:%d got unexpected error rc = %d\n",
                       __func__, __LINE__, rc);
                return rc;
            }
        }
    }
    return 0;
}

static int verify_constraints_forward_changes(struct dbtable *db, struct dbtable *newdb)
{
    int i = 0, rc = 0, verify = 0;
    /* verify forward constraints first */
    for (i = 0; i < newdb->n_constraints; i++) {
        constraint_t *ct = &newdb->constraints[i];

        rc = find_constraint(db, ct);
        if (rc == 0) {
            /* constraint not found!  will need to re-verify */
            verify = 1;
            break;
        } else {
            /* constraint found.  lets check if any indexes in the rules have
             * been changed */
            rc = has_index_changed(db, ct->lclkeyname, 1 /*ct_check*/,
                                   1 /*new*/, NULL, 0);
            if (rc < 0) /* error */
            {
                logmsg(LOGMSG_ERROR, "error in checking constraint key %s\n",
                       ct->lclkeyname);
                return -2;
            } else if (rc == 0) {
                int j = 0;
                /* we need this loop for any constraint rules that may
                   point back to our table.  If that's the case, we
                   need to make sure none of those point to the keys
                   that changed...otherwise, need to re-verify
                */
                for (j = 0; j < ct->nrules; j++) {
                    if (!strcasecmp(db->tablename, ct->table[j])) {
                        rc = has_index_changed(db, ct->keynm[j], 1, 1 /*new*/,
                                               NULL, 0);
                        if (rc == 0)
                            continue;
                        else if (rc < 0) {
                            logmsg(LOGMSG_ERROR,
                                   "error in checking constraint key "
                                   "%s table %s\n",
                                   ct->keynm[j], ct->table[j]);
                            return -2;
                        } else {
                            /* key changed. reverify */
                            verify = 1;
                            break;
                        }
                    }
                }
                /* found a changed key..no point looping further */
                if (verify) break;
                /* not changed..since we found constraint, no need to check each
                   rule,this means
                   that the data's ok */
                continue;
            } else /* rc==1..changed key */
            {
                /*
                   our key has changed. need to re-verify:
                   1) nothing, if new key is a field subset of old key.
                   anything else will be caught when trying to regenerate
                   new index.
                   2) every data record otherwise.

                   FOR NOW, ILL ALWAYS DO STEP 2.
                */
                verify = 1;
                break;
            }

        } /* else .. find_constraint() */
    }     /* for each new consraint */

    /* see if we removed constraints */
    for (i = 0; i < db->n_constraints; i++) {
        rc = find_constraint(newdb, &db->constraints[i]);
        /* as a kludge - for now verify.  technically, we don't need to */
        if (rc == 0) verify = 1;
    }

    if (!verify) return 0;

    return 1;
}

int set_header_and_properties(void *tran, struct dbtable *newdb,
                              struct schema_change_type *s, int inplace_upd,
                              int bthash)
{
    int rc;

    /* set the meta and set the ODH/compression flags */
    if ((rc = set_meta_odh_flags_tran(newdb, tran, s->headers, s->compress,
                                      s->compress_blobs, s->ip_updates))) {
        sc_errf(s, "Failed to set on disk headers\n");
        return SC_TRANSACTION_FAILED;
    }

    if (inplace_upd) {
        if (put_db_inplace_updates(newdb, tran, s->ip_updates)) {
            sc_errf(s, "Failed to set inplace updates in meta\n");
            return SC_TRANSACTION_FAILED;
        }
    }

    if (put_db_instant_schema_change(newdb, tran,
                                     newdb->instant_schema_change)) {
        sc_errf(s, "Failed to set instant schema change in meta\n");
        return SC_TRANSACTION_FAILED;
    }

    if (IS_FASTINIT(s) || s->force_rebuild || newdb->instant_schema_change) {
        if (put_db_datacopy_odh(newdb, tran, 1)) {
            sc_errf(s, "Failed to set datacopy odh in meta\n");
            return SC_TRANSACTION_FAILED;
        }
    }

    if (put_db_bthash(newdb, tran, bthash)) {
        sc_errf(s, "Failed to set bthash size in meta\n");
        return SC_TRANSACTION_FAILED;
    }
    return SC_OK;
}

/* mark in llmeta that schemachange is finished
 * we mark schemachange start in mark_sc_in_llmeta()
 */
int mark_schemachange_over_tran(const char *table, tran_type *tran)
{
    /* mark the schema change over */
    int bdberr;

    if (tran && bdb_increment_num_sc_done(thedb->bdb_env, tran, &bdberr)) {
        logmsg(LOGMSG_WARN, "could not increment num_sc_done\n");
        return SC_BDB_ERROR;
    }

    bdb_delete_sc_seed(thedb->bdb_env, tran, table, &bdberr);
    bdb_delete_sc_start_lsn(tran, table, &bdberr);

    if (bdb_set_in_schema_change(tran, table, NULL /*schema_change_data*/,
                                 0 /*schema_change_data_len*/, &bdberr) ||
        bdberr != BDBERR_NOERROR) {
        logmsg(LOGMSG_WARN,
               "POSSIBLY RESUMABLE: bdberr %d Could not mark schema change "
               "done in the low level meta table.  This usually means "
               "that the schema change failed in a potentially "
               "resumable way (ie there is a new master) if this is "
               "the case, the new master will try to resume\n", bdberr);

        return SC_BDB_ERROR;
    }

    return SC_OK;
}

int mark_schemachange_over(const char *table)
{
    tran_type *tran = NULL;
    int rc, rc2;

    struct ireq iq;
    init_fake_ireq(thedb, &iq);
    iq.usedb = get_dbtable_by_name(table);

    rc = trans_start_sc(&iq, NULL, &tran);
    if (rc) {
        logmsg(LOGMSG_ERROR,
               "%s: failed to create a txn rc %d, running non transactional!\n",
               __func__, rc);
        tran = NULL;
    }
    rc = mark_schemachange_over_tran(table, tran);

    if (rc) {
        rc2 = tran ? trans_abort(&iq, tran) : 0;
        if (rc2) {
            logmsg(LOGMSG_ERROR, "%s Failed to run and abort rc %d rc2 %d\n",
                   __func__, rc, rc2);
        } else {
            logmsg(LOGMSG_ERROR, "%s Failed to run rc %d\n", __func__, rc);
        }
    } else if (tran) {
        rc = trans_commit(&iq, tran, gbl_myhostname);
        if (rc) {
            logmsg(LOGMSG_ERROR, "%s Failed to commit rc %d\n", __func__, rc);
        }
    }

    return rc;
}

int prepare_table_version_one(tran_type *tran, struct dbtable *db,
                              struct schema **version)
{
    int rc, bdberr;
    char *ondisk_text;
    struct schema *ondisk_schema;
    struct schema *ver_one;
    char tag[MAXTAGLEN];

    /* For init with instant_sc, add ONDISK as version 1.  */
    rc = get_csc2_file_tran(db->tablename, -1, &ondisk_text, NULL, tran);
    if (rc) {
        logmsg(LOGMSG_FATAL,
               "Couldn't get latest csc2 from llmeta for %s rc %d\n",
               db->tablename, rc);
        return -1;
    }

    /* db's version has been reset */
    bdberr = bdb_reset_csc2_version(tran, db->tablename, db->schema_version, 1);
    if (bdberr != BDBERR_NOERROR) return SC_BDB_ERROR;

    /* Add latest csc2 as version 1 */
    rc = bdb_new_csc2(tran, db->tablename, 1, ondisk_text, &bdberr);
    free(ondisk_text);
    if (rc != 0) {
        logmsg(LOGMSG_FATAL, "Couldn't save in llmeta rc %d\n", rc);
        return -1;
    }

    ondisk_schema = find_tag_schema(db, ".ONDISK");
    if (NULL == ondisk_schema) {
        logmsg(LOGMSG_FATAL, ".ONDISK not found in %s! PANIC!!\n",
               db->tablename);
        abort();
    }
    ver_one = clone_schema(ondisk_schema);
    if (ver_one == NULL) {
        logmsg(LOGMSG_FATAL, "clone schema failed %s @ %d\n", __func__,
               __LINE__);
        abort();
    }
    sprintf(tag, "%s1", gbl_ondisk_ver);
    free(ver_one->tag);
    ver_one->tag = strdup(tag);
    if (ver_one->tag == NULL) {
        logmsg(LOGMSG_FATAL, "strdup failed %s @ %d\n", __func__, __LINE__);
        abort();
    }
    *version = ver_one;

    return SC_OK;
}

int fetch_sc_seed(const char *tablename, struct dbenv *thedb,
                             unsigned long long *stored_sc_genid,
                             unsigned int *stored_sc_host)
{
    int bdberr;
    int rc = bdb_get_sc_seed(thedb->bdb_env, NULL, tablename,
                             stored_sc_genid, stored_sc_host, &bdberr);
    if (rc == -1 && bdberr == BDBERR_FETCH_DTA) {
        /* No seed exists, proceed. */
    } else if (rc) {
        logmsg(LOGMSG_ERROR,
               "Can't retrieve schema change seed, rc %d bdberr %d\n",
               rc, bdberr);
        return SC_INTERNAL_ERROR;
    } else {
        /* found some seed */
    }

    return SC_OK;
}

inline int check_option_coherency(struct schema_change_type *s, struct dbtable *db,
                                  struct scinfo *scinfo)
{
    if (!s->headers && (s->compress || s->compress_blobs)) {
        sc_errf(s, "compression requires ondisk header to be added or "
                   "already present\n");
        return SC_INVALID_OPTIONS;
    }

    if (!s->headers && (s->ip_updates)) {
        sc_errf(s, "inplace updates requires ondisk header to be added or "
                   "already present\n");
        return SC_INVALID_OPTIONS;
    }

    if (!s->headers && (s->instant_sc)) {
        sc_errf(s,
                "instant schema-change requires ondisk header to be added or "
                "already present\n");
        return SC_INVALID_OPTIONS;
    }

    if (db && s->headers != db->odh && !s->force_rebuild) {
        sc_errf(s, "on-disk headers require a rebuild to enable or disable.\n");
        return SC_INVALID_OPTIONS;
    }

    if (db && s->headers != db->odh) {
        s->header_change = s->force_dta_rebuild = s->force_blob_rebuild = 1;
    }

    if (scinfo && scinfo->olddb_inplace_updates && !s->ip_updates &&
        !s->force_rebuild) {
        sc_errf(s, "inplace-updates requires a rebuild to disable.\n");
        return SC_INVALID_OPTIONS;
    }

    if (scinfo && scinfo->olddb_instant_sc && !s->instant_sc &&
        !s->force_rebuild) {
        sc_errf(s, "instant schema-change needs a rebuild to disable.\n");
        return SC_INVALID_OPTIONS;
    }
    return SC_OK;
}

inline int check_option_queue_coherency(struct schema_change_type *s,
                                        struct dbtable *db)
{
    if (s->ip_updates || s->instant_sc || s->compress_blobs) {
        sc_errf(s, "unsupported option for queues.\n");
        return SC_INVALID_OPTIONS;
    }

    return check_option_coherency(s, db, NULL);
}

int sc_request_disallowed(SBUF2 *sb)
{
    char *from = get_origin_mach_by_buf(sb);
    /* Allow if we can't figure out where it came from - don't want this
       to break in production. */
    if (from == NULL) return 0;
    if (strcmp(from, "localhost") == 0) return 0;
    if (allow_write_from_remote(from)) return 0;
    return 1;
}

int sc_cmp_fileids(unsigned long long a, unsigned long long b)
{
    return bdb_cmp_genids(a, b);
}

void verify_schema_change_constraint(struct ireq *iq, void *trans,
                                     unsigned long long newgenid, void *od_dta,
                                     unsigned long long ins_keys)
{
    struct dbtable *usedb = iq->usedb;
    blob_buffer_t *add_idx_blobs = NULL;
    void *new_dta = NULL;
    int rc = 0;

    if (!usedb)
        return;

    Pthread_rwlock_rdlock(&usedb->sc_live_lk);

    /* if there's no schema change in progress, nothing to verify */
    if (!usedb->sc_to)
        goto unlock;

    if (usedb->sc_live_logical)
        goto unlock;

    if (gbl_sc_abort || usedb->sc_abort || iq->sc_should_abort)
        goto unlock;

    if (usedb->sc_to->n_constraints == 0)
        goto unlock;

    if (is_genid_right_of_stripe_pointer(usedb->handle, newgenid,
                                         usedb->sc_to->sc_genids)) {
        goto unlock;
    }

    blob_status_t oldblobs[MAXBLOBS] = {{0}};
    blob_buffer_t add_blobs_buf[MAXBLOBS] = {{0}};

    if (usedb->sc_to->ix_blob) {
        rc =
            save_old_blobs(iq, trans, ".ONDISK", od_dta, 2, newgenid, oldblobs);
        if (rc) {
            logmsg(LOGMSG_ERROR, "%s() save old blobs failed rc %d\n", __func__,
                   rc);
            goto done;
        }
        blob_status_to_blob_buffer(oldblobs, add_blobs_buf);
        add_idx_blobs = add_blobs_buf;
    }

    new_dta = malloc(usedb->sc_to->lrl);
    if (new_dta == NULL) {
        logmsg(LOGMSG_ERROR, "%s() malloc failed\n", __func__);
        goto done;
    }

    struct convert_failure reason;
    rc = stag_to_stag_buf_blobs(usedb->sc_to, ".ONDISK", od_dta,
                                ".NEW..ONDISK", new_dta, &reason, add_idx_blobs,
                                add_idx_blobs ? MAXBLOBS : 0, 1);
    if (rc) {
        logmsg(LOGMSG_ERROR, "%s() convert record failed\n", __func__);
        goto done;
    }

    ins_keys =
        revalidate_new_indexes(iq, usedb->sc_to, new_dta, ins_keys,
                               add_idx_blobs, add_idx_blobs ? MAXBLOBS : 0);

    if (iq->debug) {
        reqpushprefixf(iq, "%s: ", __func__);
        reqprintf(iq, "verify constraints for genid 0x%llx in new table",
                  newgenid);
    }

    int rebuild = usedb->sc_to->plan && usedb->sc_to->plan->dta_plan;
    if (verify_record_constraint(iq, usedb->sc_to, trans, new_dta, ins_keys,
                                 add_idx_blobs, add_idx_blobs ? MAXBLOBS : 0,
                                 ".NEW..ONDISK", rebuild, 0) != 0) {
        logmsg(LOGMSG_ERROR, "%s: verify constraints for genid %llx failed.\n",
               __func__, newgenid);
        usedb->sc_abort = 1;
        MEMORY_SYNC;
    }

done:
    if (rc) {
        logmsg(LOGMSG_ERROR,
               "%s: verify constraints for genid %llx failed, rc=%d.\n",
               __func__, newgenid, rc);
        usedb->sc_abort = 1;
        MEMORY_SYNC;
    }
    if (new_dta)
        free(new_dta);
    free_blob_status_data(oldblobs);
unlock:
    Pthread_rwlock_unlock(&usedb->sc_live_lk);
}

/* After loading new schema file, should call this routine to see if ondisk
 * operations are required to carry out a schema change. */
int ondisk_schema_changed(const char *table, struct dbtable *newdb, FILE *out,
                          struct schema_change_type *s)
{
    int tag_rc, index_rc, constraint_rc;

    tag_rc = compare_tag(table, ".ONDISK", out);
    if (tag_rc < 0) {
        return tag_rc;
    }

    index_rc = compare_indexes(table, out);
    if (index_rc < 0) {
        return index_rc;
    }

    /* no table should point to the index which changed */
    if (index_rc && fk_source_change(newdb, out, s)) {
        return SC_BAD_INDEX_CHANGE;
    }

    constraint_rc = compare_constraints(table, newdb);
    if (constraint_rc < 0) {
        return constraint_rc;
    }

    /*check name len */
    if (!sc_via_ddl_only()) {
        int rc = validate_ix_names(newdb);
        if (rc) return rc;
    }

    if (tag_rc == SC_COLUMN_ADDED) {
        if (!newdb->instant_schema_change) tag_rc = SC_TAG_CHANGE;
    }

    if (tag_rc == SC_TAG_CHANGE) {
        return tag_rc;
    }

    if (index_rc) {
        return SC_KEY_CHANGE;
    }

    if (constraint_rc && (newdb->n_constraints || newdb->n_check_constraints)) {
        return SC_CONSTRAINT_CHANGE;
    }

    /* Possible return codes at this point:
     * SC_NO_CHANGE
     * SC_COLUMN_ADDED
     * SC_DBSTORE_CHANGE */
    return tag_rc;
}

#define scprint(s, i, args...)                                                 \
    do {                                                                       \
        if (s->dryrun)                                                         \
            sbuf2printf(s->sb, i, ##args);                                     \
        else                                                                   \
            sc_printf(s, i + 1, ##args);                                       \
    } while (0)

int create_schema_change_plan(struct schema_change_type *s, struct dbtable *olddb,
                              struct dbtable *newdb, struct scplan *plan)
{
    int rc;
    int ixn;
    int blobn = newdb->numblobs;
    int ii;
    struct schema *oldsc;
    struct schema *newsc;
    char *info;
    sc_tag_change_subtype subtype;

    info = ">Schema change plan:-\n";
    scprint(s, info);

    int force_dta_rebuild = s->force_dta_rebuild;
    int force_blob_rebuild = s->force_blob_rebuild;

    /* Patch for now to go over blob corruption issue:
       if I am forcing blob rebuilds, I am forcing rec rebuild */
    if (force_blob_rebuild) force_dta_rebuild = 1;

    memset(plan, 0, sizeof(struct scplan));

    oldsc = find_tag_schema(olddb, ".ONDISK");
    newsc = find_tag_schema(olddb, ".NEW..ONDISK");
    if (!oldsc || !newsc) {
        sc_errf(s, "%s: can't find both schemas! oldsc=%p newsc=%p\n", __func__,
                oldsc, newsc);
        return -1;
    }

    rc = compare_tag_int(oldsc, newsc, NULL, 0 /*non-strict compliance*/, &subtype);
    if (rc < 0) {
        return rc;
    }

    if (force_dta_rebuild) {
        rc = SC_TAG_CHANGE;
        subtype = SC_TAG_CHANGE_REBUILD;
    }

    if (rc != SC_TAG_CHANGE && (s->flg & SC_CHK_PGSZ)) {
        int sz1 = getpgsize(olddb->handle);
        int sz2 = calc_pagesize(4096, newdb->lrl);
        if (sz1 != sz2) {
            rc = SC_TAG_CHANGE;
            info = ">    Rebuilding dta to optimal pagesize %d -> %d\n";
            scprint(s, info, sz1, sz2);
        }
    }

    if (rc == SC_COLUMN_ADDED) {
        if (newdb->odh && newdb->instant_schema_change) {
            info = ">    Will perform instant schema change\n";
            scprint(s, info);
        } else if (newdb->odh) {
            rc = SC_TAG_CHANGE;
            subtype = SC_TAG_CHANGE_ADDITION_ISC;
            info = ">    Instant schema change possible (but disabled)\n";
            scprint(s, info);
        } else {
            rc = SC_TAG_CHANGE;
            subtype = SC_TAG_CHANGE_ADDITION_ODH;
        }
    }

    if (rc == SC_TAG_CHANGE) {
        /* Rebuild data */

        info = ">    Rebuild main data file: %s\n";
        scprint(s, info, sc_tag_change_subtype_text(subtype));

        plan->dta_plan = -1;
        plan->plan_convert = 1;

        /* Converting VUTF8 to CSTRING or BLOB to BYTEARRAY */
        if ((newsc->nmembers == oldsc->nmembers) &&
            (newsc->numblobs < oldsc->numblobs))
            s->use_old_blobs_on_rebuild = 1;

    } else {
        /* Rename old data file */
        info = ">    No changes to main data file\n";
        scprint(s, info);
        plan->dta_plan = 0;
    }

    for (ii = 0; ii < blobn; ii++) {
        plan->blob_plan[ii] = -1;
    }

    for (blobn = 0; blobn < newdb->numblobs; blobn++) {
        int map;
        map = tbl_blob_no_to_tbl_blob_no(newdb, ".NEW..ONDISK",
                                         blobn, olddb, ".ONDISK");
        /* Sanity check, although I don't see how this can possibly
         * happen - make sure we haven't already decided to use this
         * blob file for anything. */
        if (map >= 0 && map < olddb->numblobs) {
            for (ii = 0; ii < blobn; ii++) {
                if (plan->blob_plan[ii] == map) {
                    sc_errf(s, "SURPRISING BLOB MAP BLOBNO %d MAP %d\n", blobn,
                            map);
                    map = -1;
                    break;
                }
            }
        }
        if (force_blob_rebuild) {
            info = ">    Blob file %d rebuild forced\n";
            scprint(s, info, blobn);
            plan->blob_plan[blobn] = -1;
        } else if (map >= 0 && map < olddb->numblobs) {
            int oldidx =
                get_schema_blob_field_idx(olddb, ".ONDISK", map);
            int newidx = get_schema_blob_field_idx(newdb, ".NEW..ONDISK", blobn);

            /* rebuild if the blob length changed */
            if (oldsc->member[oldidx].len != newsc->member[newidx].len) {
                info = ">    Blob %d changed in record length\n";
                scprint(s, info, blobn);
                s->use_old_blobs_on_rebuild = 1;
            } else {
                if (map == blobn) {
                    info = ">    No action for blob %d.\n";
                    scprint(s, info, blobn);
                } else {
                    info = ">    Rename old blob file %d -> new blob file %d.\n";
                    scprint(s, info, map, blobn);
                }
                plan->blob_plan[blobn] = map;
                plan->old_blob_plan[map] = 1;
            }
        } else {
            info = ">    Blob file %d is new\n";
            scprint(s, info, blobn);
            plan->blob_plan[blobn] = -1;
        }
    }

    if (force_blob_rebuild)
        plan->plan_blobs = 0;
    else
        plan->plan_blobs = 1;

    int datacopy_odh;
    get_db_datacopy_odh(olddb, &datacopy_odh);

    for (ixn = 0; ixn < newdb->nix; ixn++) {
        int oldixn;
        struct schema *newixs = newdb->ixschema[ixn];

        /* Assume a rebuild. */
        plan->ix_plan[ixn] = -1;

        /* If the new index has datacopy and there are ondisk changes then
         * the index must be rebuilt. */
        if ((newixs->flags & (SCHEMA_DATACOPY | SCHEMA_PARTIALDATACOPY)) && plan->dta_plan == -1) {
            plan->ix_plan[ixn] = -1;
        } else {
            /* Try to find an unused index in the old file which exactly matches
             * this index ondisk. */
            for (oldixn = 0; oldixn < olddb->nix; oldixn++) {
                if (plan->old_ix_plan[oldixn] == 0 &&
                    cmp_index_int(olddb->ixschema[oldixn], newixs, NULL, 0) ==
                        0) {
                    /* Excellent; we can just use an existing index file. */
                    plan->ix_plan[ixn] = oldixn;
                    plan->old_ix_plan[oldixn] = 1;
                    break;
                }
            }
        }

        if (newdb->odh &&                        /* table has odh */
            (newixs->flags & (SCHEMA_DATACOPY | SCHEMA_PARTIALDATACOPY)) && /* index had datacopy */
            !datacopy_odh) /* index did not have odh in datacopy */
        {
            plan->ix_plan[ixn] = -1;
        }

        if (s->rebuild_index && s->index_to_rebuild == ixn)
            plan->ix_plan[ixn] = -1;

        /* If we have to build an index, we have to run convert_all_records */
        if (plan->ix_plan[ixn] == -1) plan->plan_convert = 1;

        char *str_datacopy;
        if (newixs->flags & (SCHEMA_DATACOPY | SCHEMA_PARTIALDATACOPY)) {
            if (olddb->odh) {
                if (newdb->odh) {
                    if (datacopy_odh) {
                        str_datacopy = " [datacopy contains odh]";
                    } else {
                        str_datacopy = " [datacopy will contain odh]";
                    }
                } else {
                    str_datacopy = " [odh will be removed from datacopy]";
                }
            } else {
                if (newdb->odh) {
                    str_datacopy = " [datacopy will contain odh]";
                } else {
                    str_datacopy = " [no odh in datacopy]";
                }
            }
        } else {
            str_datacopy = " [no datacopy]";
        }

        if (plan->ix_plan[ixn] >= 0) {
            char extra[256] = {0};
            int offset = get_offset_of_keyname(newixs->csctag);
            if (strcmp(newixs->csctag + offset,
                       olddb->ixschema[plan->ix_plan[ixn]]->csctag) != 0)
                snprintf(extra, sizeof(extra),
                         " (IDX name changed from %s to %s)",
                         olddb->ixschema[plan->ix_plan[ixn]]->csctag,
                         newixs->csctag + offset);

            if (plan->ix_plan[ixn] == ixn) {
                info = ">    No action for index %d (%s)%s%s\n";
                scprint(s, info, ixn, newixs->csctag, str_datacopy, extra);
            } else {
                info = ">    Rename .ix%d -> .ix%d (%s)%s%s\n";
                scprint(s, info, plan->ix_plan[ixn], ixn, newixs->csctag, str_datacopy, extra);
            }
        } else {
            info = ">    Rebuild index %d (%s)%s\n";
            scprint(s, info, ixn, newixs->csctag, str_datacopy);
        }
    }

    int nixadds = 0, nixdels = 0;
    /*
     * for every index in the new schema, if a matching index in the old schema
     * can't be found, we're adding an index.
     */
    for (int newix = 0; newix < newdb->nix; ++newix) {
        if (plan->ix_plan[newix] < 0) {
            ++nixadds;
        }
    }
    if (nixadds > 0) {
        info = ">    %d index%s to be created\n";
        scprint(s, info, nixadds, (nixadds > 1 ? "es" : ""));
    }

    /*
     * for every index in the old schema, if a matching index in the new schema
     * can't be found, we're dropping an index.
     */
    for (int oldix = 0; oldix < olddb->nix; ++oldix) {
        if (plan->old_ix_plan[oldix] == 0) {
            ++nixdels;
        }
    }
    if (nixdels > 0) {
        info = ">    %d index%s to be dropped\n";
        scprint(s, info, nixdels, (nixdels > 1 ? "es" : ""));
    }

    char *str_constraints = "";
    rc = ondisk_schema_changed(s->tablename, newdb, NULL, s);
    if (rc == SC_CONSTRAINT_CHANGE && !plan->plan_convert &&
        newdb->n_constraints) {
        plan->plan_convert = 1;
        str_constraints = " (to verify constraints)";
    }

    if (plan->plan_convert)
        info = ">    Schema change requires a table scan %s\n";
    else
        info = ">    Schema change does not require a table scan %s\n";

    scprint(s, info, str_constraints);

    return 0;
}

/* Transfer settings such as dbnum, blobstrip_genid etc from olddb to newdb */
void transfer_db_settings(struct dbtable *olddb, struct dbtable *newdb)
{
    newdb->dbnum = olddb->dbnum;
    if (gbl_blobstripe) {
        newdb->blobstripe_genid = olddb->blobstripe_genid;
        bdb_set_blobstripe_genid(newdb->handle, newdb->blobstripe_genid);
    }
    memcpy(newdb->typcnt, olddb->typcnt, sizeof(olddb->typcnt));
    memcpy(newdb->blocktypcnt, olddb->blocktypcnt, sizeof(olddb->blocktypcnt));
    memcpy(newdb->prev_blocktypcnt, olddb->prev_blocktypcnt, sizeof(olddb->prev_blocktypcnt));
    memcpy(newdb->blockosqltypcnt, olddb->blockosqltypcnt,
           sizeof(olddb->blockosqltypcnt));
    memcpy(newdb->prev_blockosqltypcnt, olddb->prev_blockosqltypcnt, sizeof(olddb->prev_blockosqltypcnt));
    memcpy(newdb->write_count, olddb->write_count, sizeof(olddb->write_count));
    memcpy(newdb->saved_write_count, olddb->saved_write_count,
           sizeof(olddb->saved_write_count));
    XCHANGE64(newdb->aa_lastepoch, olddb->aa_lastepoch);
}

/* use callers transaction if any, need to do I/O */
void set_odh_options_tran(struct dbtable *db, tran_type *tran)
{
    int compr = 0;
    int blob_compr = 0;
    int datacopy_odh = 0;

    get_db_odh_tran(db, &db->odh, tran);
    get_db_instant_schema_change_tran(db, &db->instant_schema_change, tran);
    get_db_datacopy_odh_tran(db, &datacopy_odh, tran);
    get_db_inplace_updates_tran(db, &db->inplace_updates, tran);
    get_db_compress_tran(db, &compr, tran);
    get_db_compress_blobs_tran(db, &blob_compr, tran);
    db->schema_version = get_csc2_version_tran(db->tablename, tran);

    set_bdb_option_flags(db, db->odh, db->inplace_updates,
                         db->instant_schema_change, db->schema_version, compr,
                         blob_compr, datacopy_odh);

    /*
    if (db->schema_version < 0)
        return -1;

    return 0;
    */
}

/* Get flags from llmeta and set db, bdb_state */
void set_odh_options(struct dbtable *db)
{
    set_odh_options_tran(db, NULL);
}

int compare_constraints(const char *table, struct dbtable *newdb)
{
    int i = 0, rc = 0, nvlist = 0;
    struct dbtable **verifylist = NULL;
    struct dbtable *db = get_dbtable_by_name(table);
    if (db == NULL) return -2;

    /* check reverse constraints for old 'db*' here. there maybe other tables
       referencing us */
    verifylist = (struct dbtable **)malloc(thedb->num_dbs * sizeof(struct dbtable *));
    if (verifylist == NULL) {
        logmsg(LOGMSG_ERROR, "error in malloc during verify constraint!\n");
        return -2;
    }

    rc = verify_constraints_forward_changes(db, newdb);
    if (rc < 0) {
        free(verifylist);
        return -2;
    } else if (rc == 1) {
        verifylist[nvlist++] = newdb;
    }

    for (i = 0; i < db->n_rev_constraints; i++) {
        constraint_t *ct = (constraint_t *)db->rev_constraints[i];
        int j = 0;
        /* skip constraints pointing to themselves (same table)..their changes
         * would've been picked up in forward check */

        /*fprintf(stderr, "%s\n", ct->lcltable->tablename);*/
        if (!strcasecmp(ct->lcltable->tablename, db->tablename))
            continue;

        for (j = 0; j < ct->nrules; j++) {
            /* skip references to other tables*/
            if (strcasecmp(ct->table[j], db->tablename))
                continue;
            /* fprintf(stderr, "  rule %s:%s\n", ct->table[j], ct->keynm[j]);*/
            rc = has_index_changed(db, ct->keynm[j], 1, 0 /*old table key */,
                                   NULL, 0);
            if (rc < 0) {
                logmsg(LOGMSG_ERROR,
                       "error in checking reverse constraint table %s "
                       "key %s\n",
                       ct->lcltable->tablename, ct->lclkeyname);
                free(verifylist);
                return -2;
            } else if (rc == 0) {
                continue; /* key has not changed...nothing to do here */
            } else {
                /* key has changed...must reverify rule */
                int k = 0, fnd = 0;
                for (k = 0; k < nvlist; k++) {
                    if (verifylist[k] == ct->lcltable) {
                        fnd = 1;
                        break;
                    }
                }
                if (nvlist >= thedb->num_dbs) {
                    logmsg(LOGMSG_ERROR,
                           "error! constraints reference more tables "
                           "than available %d! last tbl '%s' key '%s'\n",
                           nvlist, ct->lcltable->tablename, ct->lclkeyname);
                    free(verifylist);
                    return -2;
                }
                if (!fnd) {
                    verifylist[nvlist++] = ct->lcltable;
                }
                break;
            }
        }
    }

    free(verifylist);

    if (nvlist > 0) return 1;

    /* Verify CHECK constraints on all records. */
    if (newdb->n_check_constraints > 0) {
        return 1;
    }

    return 0;
}

int restore_constraint_pointers_main(struct dbtable *db, struct dbtable *newdb,
                                     int copyof)
{
    int i = 0;
    /* lets deal with pointers...all tables pointing to me must be entered into
     * my 'reverse' */
    if (copyof) {
        for (int i = 0; i < db->n_rev_constraints; i++) {
            add_reverse_constraint(newdb, db->rev_constraints[i]);
        }
    }
    /* additionally, for each table i'm pointing to in old db, must get its
     * reverse constraint array updated to get all reverse ct *'s removed */
    for (i = 0; i < thedb->num_dbs; i++) {
        struct dbtable *rdb = thedb->dbs[i];
        if (!strcasecmp(rdb->tablename, newdb->tablename)) {
            rdb = newdb;
        }
        Pthread_mutex_lock(&rdb->rev_constraints_lk);
        for (int j = 0; j < rdb->n_rev_constraints; j++) {
            constraint_t *ct = NULL;
            ct = rdb->rev_constraints[j];
            if (!strcasecmp(ct->lcltable->tablename, db->tablename)) {
                delete_reverse_constraint(rdb, j);
                j--;
            }
        }
        Pthread_mutex_unlock(&rdb->rev_constraints_lk);
        for (int j = 0; j < newdb->n_constraints; j++) {
            for (int k = 0; k < newdb->constraints[j].nrules; k++) {
                int ridx = 0;
                int dupadd = 0;

                if (strcasecmp(newdb->constraints[j].table[k], rdb->tablename))
                    continue;
                for (ridx = 0; ridx < rdb->n_rev_constraints; ridx++) {
                    if (rdb->rev_constraints[ridx] == &newdb->constraints[j]) {
                        dupadd = 1;
                        break;
                    }
                }
                if (dupadd) continue;
                add_reverse_constraint(rdb, &newdb->constraints[j]);
            }
        }
    }

    return 0;
}

int restore_constraint_pointers(struct dbtable *db, struct dbtable *newdb)
{
    return restore_constraint_pointers_main(db, newdb, 1);
}

int backout_constraint_pointers(struct dbtable *db, struct dbtable *newdb)
{
    return restore_constraint_pointers_main(db, newdb, 0);
}

/* did keys change which are also constraint sources? */
int fk_source_change(struct dbtable *newdb, FILE *out, struct schema_change_type *s)
{
    int i;
    struct dbtable *olddb = get_dbtable_by_name(newdb->tablename);
    for (i = 0; i < newdb->nix; ++i) {
        struct schema *index = newdb->ixschema[i];
        int offset = get_offset_of_keyname(index->csctag);
        char *key = index->csctag + offset;
        if (has_index_changed(olddb, key, 1, 0, NULL, 1))
            if (compatible_constraint_source(olddb, newdb, index, key, out,
                                             s) != 0)
                return 1;
    }
    return 0;
}

int check_sc_headroom(struct schema_change_type *s, struct dbtable *olddb,
                      struct dbtable *newdb)
{
    uint64_t avail, wanted;
    struct statvfs st;
    int rc;
    uint64_t headroom = gbl_sc_headroom; /* percent */
    uint64_t oldsize, newsize, diff;
    char b1[32], b2[32], b3[32], b4[32];

    /* 1 if using the entire table size to estimate disk space needed.
       0 if using the following formula:
            disk_space_needed = index_width / table_width * table_size / In(2)
       This assumes that the table is 100% filled and the index is randomly
       inserted into and thus has a fill factor of In(2) (~0.693). So in reality
       the actual disk space used should be lower than the estimate. */
    int conservative = 0;
    double pct = 0;
    struct scplan *theplan = newdb->plan;

    if (theplan == NULL /* if not using a plan */ || (theplan->dta_plan == -1) /* if rebuilding data */ ||
        !theplan->plan_blobs /* if rebuilding all blobs */)
        conservative = 1;
    else {
        /* if rebuilding any blobs (could be expanding inline portion in which case
           data size increases and blob size decreases, or shrinking inline portion
           in which case data size decreases and blob size increases), just estimate
           disk requirement the old way. */
        for (int iblb = 0, nblb = newdb->numblobs; iblb != nblb; ++iblb) {
            if (theplan->blob_plan[iblb] == -1) {
                conservative = 1;
                break;
            }
        }

        if (!conservative) {
            int iix, nix, lrl;
            struct schema *ix;
            for (iix = 0, nix = newdb->nix, lrl = newdb->lrl; iix != nix; ++iix) {
                if (theplan->ix_plan[iix] == -1) {
                    ix = newdb->ixschema[iix];
                    /* If an index has a blob field or is datacopy, the size
                       will rise based on the blob size or data size. So still
                       use the old way here. */
                    if (!ix->ix_blob && !(ix->flags & (SCHEMA_DATACOPY | SCHEMA_PARTIALDATACOPY)))
                        pct += get_size_of_schema(ix) / (double)lrl;
                }
            }

            /* Neither an instant schema change, nor rebuilding data, indexes and blobs.
               How can this happen? */
            if (pct == 0)
                conservative = 1;
        }
    }

    oldsize = calc_table_size(olddb, 0);
    newsize = calc_table_size(newdb, 0);

    if (conservative) {
        if (newsize > oldsize)
            diff = oldsize / 3; /* newdb already larger; assume 33% growth */
        else
            diff = oldsize - newsize;
    } else {
        /* In case there's a miscalculation. */
        if (newsize > oldsize)
            diff = oldsize / 3;
        else if ((pct * oldsize) / 0.693 > newsize)
            diff = (pct * oldsize) / 0.693 - newsize;
        else
            diff = oldsize - newsize;
    }

    wanted = (diff * (uint64_t)(100 + headroom)) / 100ULL;

    rc = statvfs(olddb->dbenv->basedir, &st);
    if (rc == -1) {
        sc_errf(s, "cannot get file system data for %s: %d %s\n",
                olddb->dbenv->basedir, errno, strerror(errno));
        return -1;
    }

    avail = (uint64_t)st.f_bavail * (uint64_t)st.f_frsize;

    sc_printf(
        s, "Table %s, old %s, new %s, reqd. %s, avail %s\n", olddb->tablename,
        fmt_size(b1, sizeof(b1), oldsize), fmt_size(b2, sizeof(b2), newsize),
        fmt_size(b3, sizeof(b3), wanted), fmt_size(b4, sizeof(b4), avail));

    if (wanted > avail) {
        sc_errf(s, "DANGER low headroom for schema change\n");
        sc_client_error(s, "server is running low on disk space");
        return -1;
    }

    return 0;
}

/* compatible change if type unchanged but get larger in size */
int compat_chg(struct dbtable *olddb, struct schema *s2, const char *ixname)
{
    struct schema *s1 = find_tag_schema(olddb, ixname);
    if (s1->nmembers != s2->nmembers) return 1;
    int i;
    for (i = 0; i < s1->nmembers; ++i) {
        struct field *f1 = &s1->member[i];
        struct field *f2 = &s2->member[i];
        if (f1->type != f2->type) return 1;
        if (strcmp(f1->name, f2->name) != 0) return 1;
        if (f1->flags != f2->flags) return 1;
        if (f1->len > f2->len) return 1;
    }
    return 0;
}

int compatible_constraint_source(struct dbtable *olddb, struct dbtable *newdb,
                                 struct schema *newsc, const char *key,
                                 FILE *out, struct schema_change_type *s)
{
    const char *dbname = newdb->tablename;
    int i, j, k;
    for (i = 0; i < thedb->num_dbs; ++i) {
        struct dbtable *db = thedb->dbs[i];
        if (strcmp(db->tablename, dbname) == 0)
            continue;
        for (j = 0; j < db->n_constraints; ++j) {
            constraint_t *ct = &db->constraints[j];
            for (k = 0; k < ct->nrules; ++k) {
                if (strcmp(dbname, ct->table[k]) == 0 &&
                    strcasecmp(key, ct->keynm[k]) == 0) {
                    if (compat_chg(olddb, newsc, key) == 0) continue;
                    char *info = ">%s:%s -> %s:%s\n";
                    if (s && s->dryrun) {
                        sbuf2printf(s->sb, info, db->tablename, ct->lclkeyname,
                                    dbname, ct->keynm[k]);
                    } else if (out) {
                        logmsgf(LOGMSG_USER, out, info + 1, db->tablename,
                                ct->lclkeyname, dbname, ct->keynm[k]);
                    }
                    return 1;
                }
            } /* each rule of constraint */
        }     /* each constraint */
    }         /* each table in db */
    return 0;
}

int remove_constraint_pointers(struct dbtable *db)
{
    for (int i = 0; i < thedb->num_dbs; i++) {
        struct dbtable *rdb = thedb->dbs[i];
        int j = 0;
        Pthread_mutex_lock(&rdb->rev_constraints_lk);
        for (j = 0; j < rdb->n_rev_constraints; j++) {
            constraint_t *ct = NULL;
            ct = rdb->rev_constraints[j];
            if (!strcasecmp(ct->lcltable->tablename, db->tablename)) {
                delete_reverse_constraint(rdb, j);
                j--;
            }
        }
        Pthread_mutex_unlock(&rdb->rev_constraints_lk);
    }
    return 0;
}

int rename_constraint_pointers(struct dbtable *db, const char *newname)
{
    for (int i = 0; i < thedb->num_dbs; i++) {
        struct dbtable *rdb = thedb->dbs[i];
        int j = 0;
        Pthread_mutex_lock(&rdb->rev_constraints_lk);
        for (j = 0; j < rdb->n_rev_constraints; j++) {
            constraint_t *ct = NULL;
            ct = rdb->rev_constraints[j];
            if (!strcasecmp(ct->lcltable->tablename, db->tablename)) {
                strcpy(ct->lcltable->tablename, newname);
            }
        }
        Pthread_mutex_unlock(&rdb->rev_constraints_lk);
    }
    return 0;
}

void fix_constraint_pointers(struct dbtable *db, struct dbtable *newdb)
{
    /* This is a kludge.  Newdb is going away.  Go through all
     * tables.  Any constraints that point to newdb should be
     * changed to point to the same constraint in db. */
    int i, j, k;
    struct dbtable *rdb;
    constraint_t *ct;

    for (i = 0; i < thedb->num_dbs; i++) {
        rdb = thedb->dbs[i];
        /* fix reverse references */
        if (rdb->n_rev_constraints > 0) {
            Pthread_mutex_lock(&rdb->rev_constraints_lk);
            for (j = 0; j < rdb->n_rev_constraints; j++) {
                ct = rdb->rev_constraints[j];
                for (k = 0; k < newdb->n_constraints; k++) {
                    if (ct == &newdb->constraints[k]) {
                        rdb->rev_constraints[j] = &db->constraints[k];
                    }
                }
            }
            Pthread_mutex_unlock(&rdb->rev_constraints_lk);
        }
        /* fix forward references */
        if (rdb->n_constraints) {
            for (j = 0; j < rdb->n_constraints; j++) {
                ct = &rdb->constraints[j];
                if (ct->lcltable == newdb)
                    ct->lcltable = db;
            }
        }
    }
}

/* return 1 if the table is only referenced by foreign key in the same table or
 * it is not referenced at all, 0 otherwise
 */
int self_referenced_only(struct dbtable *db)
{
    int i, rc;
    if (db->n_rev_constraints == 0)
        return 1;

    rc = 1;
    for (i = 0; i < db->n_rev_constraints; i++) {
        constraint_t *ct = db->rev_constraints[i];
        if (strcasecmp(ct->lcltable->tablename, db->tablename)) {
            rc = 0;
            break;
        }
    }
    return rc;
}

void change_schemas_recover(char *table)
{
    struct dbtable *db = get_dbtable_by_name(table);
    if (db == NULL) {
        /* shouldn't happen */
        logmsg(LOGMSG_ERROR, "change_schemas_recover: invalid table %s\n",
               table);
        return;
    }
    backout_schemas(table);
    live_sc_off(db);
}
