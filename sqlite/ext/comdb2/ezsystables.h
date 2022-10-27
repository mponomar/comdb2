#ifndef INCLUDED_SYSTABLE_H
#define INCLUDED_SYSTABLE_H

#include <sqliteInt.h>

enum {
    SYSTABLE_END_OF_FIELDS = -1
};

enum {
    SYSTABLE_FIELD_NULLABLE  = 0x1000,
};

/* All other types have enough information to determine size.  Blobs need a little help. */
typedef struct {
    void *value;
    size_t size;
} systable_blobtype;

int create_system_table(sqlite3 *db, char *name, sqlite3_module *module,
        int(*init_callback)(void **data, int *npoints),
        void(*release_callback)(void *data, int npoints),
        size_t struct_size,
        // type, name, offset,  type2, name2, offset2, ..., SYSTABLE_END_OF_FIELDS
        ...);

// Note: these ONLY work for cursors created with create_system_table()
BtCursor *get_systable_cursor_from_table(sqlite3_vtab *vtab);
BtCursor *get_systable_cursor_from_cursor(sqlite3_vtab_cursor *vtab);
void put_systable_cursor_from_table(sqlite3_vtab *vtab);

#endif
