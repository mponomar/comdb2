#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "comdb2.h"
#include "bdb_int.h"
#include <build/db.h>
#include "comdb2systblInt.h"
#include "sql.h"
#include "ezsystables.h"
#include "types.h"

static sqlite3_module systblDblistModule = {
    .access_flag = CDB2_ALLOW_USER,
};

struct dblist_entry {
    char* fname;
    int64_t dbreg;
    char* inhash;
    char* deleted;
    systable_blobtype ufid;
};
typedef struct dblist_entry dblist_entry_t;

struct dblist_entry_collection {
    dblist_entry_t *entries;
    int allocated;
    int nent;
};
typedef struct dblist_entry_collection dblist_entry_collection_t;

static void collect_dblist_entry(void *usrptr, char *name, int dbreg, int inhash, int deleted, void *ufid) {
    dblist_entry_collection_t *entries;
    entries = (dblist_entry_collection_t*) usrptr;
    if (entries->nent == entries->allocated) {
        int nallocated = entries->allocated * 2 + 16;
        dblist_entry_t *tmp = realloc(entries->entries, nallocated * sizeof(dblist_entry_t));
        if (tmp == NULL)
            return;
        entries->allocated = nallocated;
        entries->entries = tmp;
    }
    if (strncmp(name, "XXX.", 4) == 0)
        name += 4;
    entries->entries[entries->nent].fname = strdup(name);
    entries->entries[entries->nent].dbreg = dbreg;
    entries->entries[entries->nent].inhash = inhash ? "Y" : "N";
    entries->entries[entries->nent].deleted = deleted ? "Y" : "N";
    entries->entries[entries->nent].ufid.value = malloc(DB_FILE_ID_LEN);
    entries->entries[entries->nent].ufid.size = DB_FILE_ID_LEN;
    memcpy(entries->entries[entries->nent].ufid.value, ufid, DB_FILE_ID_LEN); 
    entries->nent++;
}



static int get_dblist_entries(void **data, int *records)
{
    dblist_entry_collection_t entries = { .entries = NULL, .allocated = 0, .nent = 0};
    __bb_dbreg_collect(thedb->bdb_env->dbenv, collect_dblist_entry, &entries);
    *data = entries.entries;
    *records = entries.nent;
    return 0;
}

static void free_dblist_entries(void *p, int n)
{
    dblist_entry_t *e = (dblist_entry_t*) p;
    for (int i = 0; i < n; i++) {
        free(e[i].fname);
        free(e[i].ufid.value);
    }
    free(e);
}

int systblDblist(sqlite3 *db)
{
    return create_system_table(db, "comdb2_dblist", &systblDblistModule,
        get_dblist_entries, free_dblist_entries, sizeof(dblist_entry_t),
        CDB2_CSTRING, "fname", -1, offsetof(dblist_entry_t, fname),
        CDB2_INTEGER, "dbreg", -1, offsetof(dblist_entry_t, dbreg),
        CDB2_BLOB, "ufid", -1, offsetof(dblist_entry_t, ufid),
        CDB2_CSTRING, "inhash", -1, offsetof(dblist_entry_t, inhash),
        CDB2_CSTRING, "deleted", -1, offsetof(dblist_entry_t, deleted),
        SYSTABLE_END_OF_FIELDS);
}
