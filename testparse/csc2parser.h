#ifndef INCLUDED_CSC2PARSER_H
#define INCLUDED_CSC2PARSER_H

#include <stdint.h>
#include <assert.h>
#include "csctypes.h"
#include "list.h"

#define MAX_CONSTRAINTS 32

/* Use sqlite's token, we don't need much else. */
typedef struct Token {
  const char *z;
  unsigned int n;
} Token;

struct key {
    char *name;
    int flags;
    LISTC_T(struct field_name) fields;
    LINKC_T(struct key) lnk;
};

struct type {
    int basetype;
    int size;
};

struct field_option {
    int type;
    union {
        struct literal_value *l;
        uint8_t dbpad;
        int nullable;
    } u;
    LINKC_T(struct field_option) lnk;
};

struct field_option_list {
    LISTC_T(struct field_option) options;
};

struct field {
    int type;
    int size;
    int arrsize;
    char *name;
    struct field_option_list *options;
    LINKC_T(struct field) lnk;
};

struct constant {
    char *name;
    int value;
    LINKC_T(struct constant) lnk;
};

struct schema {
    char *tagname;  /* booo! hissss! */
    LISTC_T(struct field) fields;
    LINKC_T(struct schema) lnk;
};

struct literal_value {
    int type;
    union {
        int number;
        char *string;
        struct {
            int len;
            void *p;
        } blob;
    } u;
};

struct field_name {
    char *name;
    int descend;
    LINKC_T(struct field_name) lnk;
};

struct field_list {
    LISTC_T(struct field_name) fields;
};

enum { FIELD_OPTION_DBSTORE, FIELD_OPTION_DBLOAD, FIELD_OPTION_DBPAD, FIELD_OPTION_NULLABLE };

struct alloc {
    void *p;
    size_t size;
    LINKC_T(struct alloc) lnk;
};

struct constraint {
    char *key;
    char *table;
    char *fkey;
    int options;
    LINKC_T(struct constraint) lnk;
};

typedef struct {
    LISTC_T(struct alloc) allocations;
    char *errstr;
    int line;
    int rc;
    LISTC_T(struct constant) constants;
    LISTC_T(struct key) keys;
    LISTC_T(struct schema) schemas;
    LISTC_T(struct constraint) constraints;
} CSC2Parser;

enum {
    CASCADE_UPDATE = 1,
    CASCADE_DELETE = 2
};

#define NEVER(X) 0
#define ALWAYS(X) 1

void cscErrorMsg(CSC2Parser *, const char *fmt, ...);
void cscAddField(CSC2Parser *p, struct schema *s, struct field *field);
void cscAddConstant(CSC2Parser *p, Token *id, Token *n);
int cscTokNum(CSC2Parser *pParse, Token *t);
int cscFindConstant(CSC2Parser *pParse, char *name, int *value);
char* cscTokenStr(CSC2Parser *pParse, Token *t);

struct literal_value* cscLiteralInteger(CSC2Parser *pParse, Token *t);
struct literal_value* cscLiteralString(CSC2Parser *pParse, Token *t);
struct literal_value* cscLiteralBlob(CSC2Parser *pParse, Token *t);

void dumpFieldOptions(CSC2Parser *pParse, struct field_option_list *opt);
void* cscAlloc(CSC2Parser *pParse, size_t sz);
void cscFree(CSC2Parser *pParse, void *p);
struct field_option_list* cscNewFieldOptions(CSC2Parser *pParse);
void cscAddFieldOption(CSC2Parser *pParse, struct field_option_list *l, struct field_option *o);
struct field* cscNewField(CSC2Parser *pParse, struct type *type, Token *namet, int arrsz, struct field_option_list *opts);
struct schema* cscNewRecord(CSC2Parser *pParse);
void cscSchemaSetName(CSC2Parser *pParse, struct schema *s, char *name);
struct field_list* cscNewFieldList(CSC2Parser *pParse);
void cscFieldListAddField(CSC2Parser *pParse, struct field_list *fl, Token *namet, int desc);
struct key* cscAddKey(CSC2Parser *, Token *keyname, int flags, struct field_list *fields);
void cscAddRecord(CSC2Parser *pParse, struct schema *s);
struct constraint* cscAddConstraint(CSC2Parser *pParse, Token *keyt, Token *tablet, Token *foreignt, int options);
void cscFinalChecks(CSC2Parser *pParse);

#endif
