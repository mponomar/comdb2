#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>

#include "csc2parser.h"
#include "parse.h"

#include <poll.h>

void* csc2ParserAlloc(void*(*)(size_t));
void csc2ParserFree(void*, void(*)(void*));

void cscErrorMsg(CSC2Parser *pParse, const char *fmt, ...) {
    va_list args;
    char *err;
    va_start(args, fmt);
    vasprintf(&err, fmt, args);
    va_end(args);
    fprintf(stderr, "%s\n", err);
    free(err);
    pParse->rc = 1;
}

void cscAddField(CSC2Parser *pParse, struct schema *s, struct field *field) {
    struct field *ef;
    
    LISTC_FOR_EACH(&s->fields, ef, lnk) {
        if (strcasecmp(field->name, ef->name) == 0)
            cscErrorMsg(pParse, "line %d, duplicate column \"%s\"", pParse->line, field->name);
    }

    listc_abl(&s->fields, field);
}

void* cscAlloc(CSC2Parser *pParse, size_t sz) {
    struct alloc *a;
    a = malloc(sizeof(struct alloc));
    a->p = malloc(sz);
    a->size = sz;
    listc_abl(&pParse->allocations, a);
    return a->p;
}

void cscFree(CSC2Parser *pParse, void *p) {
    /* do nothing, we free all memory in cscParserDestroy */
}

static char* tokstr(CSC2Parser *pParse, Token *t) {
    char *s;
    s = cscAlloc(pParse, t->n + 1);
    memcpy(s, t->z, t->n);
    s[t->n] = 0;
    return s;
}

int cscFindConstant(CSC2Parser *pParse, char *name, int *value) {
    struct constant *c;
    for (c = pParse->constants.top; c; c = c->lnk.next) {
        if (strcasecmp(c->name, name) == 0) {
            if (value)
                *value = c->value;
            return 1;
        }
    }
    return 0;
}

void cscAddConstant(CSC2Parser *pParse, Token *id, Token *n) {
    char *name = tokstr(pParse, id);

    if (cscFindConstant(pParse, name, NULL))
        cscErrorMsg(pParse, "line %d, duplicate definition for constant %s", pParse->line, name);

    char *numstr = tokstr(pParse, n);
    int num = atoi(numstr);
    struct constant *c;
    c = cscAlloc(pParse, sizeof(struct constant));
    c->name = name;
    c->value = num;
    listc_abl(&pParse->constants, c);
}

int cscTokNum(CSC2Parser *pParse, Token *t) {
    char *s;
    int n;
    s = tokstr(pParse, t);
    n = atoi(s);
    cscFree(pParse, s);
    return n;
}

char* cscTokenStr(CSC2Parser *pParse, Token *t) {
    return tokstr(pParse, t);
}

extern void csc2Parser(void *parser, int tok, Token tokdata, CSC2Parser *p);
void csc2ParserTrace(FILE *TraceFILE, char *zTracePrompt);

struct field_option_list* cscNewFieldOptions(CSC2Parser *pParse) {
    struct field_option_list *l;
    l = cscAlloc(pParse, sizeof(struct field_option_list));
    listc_init(&l->options, offsetof(struct field_option, lnk));
    return l;
}

void cscAddFieldOption(CSC2Parser *pParse, struct field_option_list *l, struct field_option *o) {
    listc_abl(&l->options, o);
}

static char* toktext(int tok) {
    switch (tok) {
        case TK_SCHEMA: return "TK_SCHEMA";
        case TK_OPEN_BRACE: return "TK_OPEN_BRACE";
        case TK_CLOSE_BRACE: return "TK_CLOSE_BRACE";
        case TK_TAG: return "TK_TAG";
        case TK_DEFAULT: return "TK_DEFAULT";
        case TK_STRING: return "TK_STRING";
        case TK_SHORT: return "TK_SHORT";
        case TK_U_SHORT: return "TK_U_SHORT";
        case TK_INT: return "TK_INT";
        case TK_U_INT: return "TK_U_INT";
        case TK_LONGLONG: return "TK_LONGLONG";
        case TK_U_LONGLONG: return "TK_U_LONGLONG";
        case TK_FLOAT: return "TK_FLOAT";
        case TK_DOUBLE: return "TK_DOUBLE";
        case TK_BYTE: return "TK_BYTE";
        case TK_CSTRING: return "TK_CSTRING";
        case TK_PSTRING: return "TK_PSTRING";
        case TK_BLOB: return "TK_BLOB";
        case TK_DATETIME: return "TK_DATETIME";
        case TK_DATETIMEUS: return "TK_DATETIMEUS";
        case TK_INTERVALYM: return "TK_INTERVALYM";
        case TK_INTERVALDS: return "TK_INTERVALDS";
        case TK_INTERVALDSUS: return "TK_INTERVALDSUS";
        case TK_VUTF8: return "TK_VUTF8";
        case TK_DECIMAL32: return "TK_DECIMAL32";
        case TK_DECIMAL64: return "TK_DECIMAL64";
        case TK_DECIMAL128: return "TK_DECIMAL128";
        case TK_YES: return "TK_YES";
        case TK_NO: return "TK_NO";
        case TK_OPEN_BRACKET: return "TK_OPEN_BRACKET";
        case TK_NUMBER: return "TK_NUMBER";
        case TK_CLOSE_BRACKET: return "TK_CLOSE_BRACKET";
        case TK_ID: return "TK_ID";
        case TK_HEXSTRING: return "TK_HEXSTRING";
        case TK_DBSTORE: return "TK_DBSTORE";
        case TK_EQUALS: return "TK_EQUALS";
        case TK_DBLOAD: return "TK_DBLOAD";
        case TK_NULL: return "TK_NULL";
        case TK_DBPAD: return "TK_DBPAD";
        case TK_KEYS: return "TK_KEYS";
        case TK_PLUS: return "TK_PLUS";
        case TK_CONSTANTS: return "TK_CONSTANTS";
        case TK_CONSTRAINTS: return "TK_CONSTRAINTS";
        case TK_MINUS: return "TK_MINUS";
        case TK_GREATER: return "TK_GREATER";
        case TK_LESS: return "TK_LESS";
        case TK_COLON: return "TK_COLON";
        case TK_ON: return "TK_ON";
        case TK_DELETE: return "TK_DELETE";
        case TK_CASCADE: return "TK_CASCADE";
        case TK_UPDATE: return "TK_UPDATE";
        case TK_RESTRICT: return "TK_RESTRICT";
        case TK_ASCEND: return "TK_ASCEND";
        case TK_DESCEND: return "TK_ASCEND";
        case TK_RECNUMS: return "TK_RECNUMS";
        case TK_DATACOPY: return "TK_DATACOPY";
        case TK_DUP: return "TK_DUP";
        case TK_ONDISK: return "TK_ONDISK";
        case TK_COMMA: return "TK_COMMA";

        default:
            return "???";
    }
}

int nextToken(CSC2Parser *pParse, char **s, Token *t);

char *summarize(char *s) {
    static char t[100] = {0};
    for (int i = 0, j = 0; i < 20; i++, j++) {
        if (s[i] == 0)
            break;
        if (s[i] == '\n') {
            t[j] = '\\';
            t[++j] = 'n';
        }
        else {
            t[j] = s[i];
        }
    }
    return t;
}

struct literal_value* cscLiteralInteger(CSC2Parser *pParse, Token *t) {
    struct literal_value *d = cscAlloc(pParse, sizeof(struct literal_value));
    d->type = CLIENT_INT;
    d->u.number = cscTokNum(pParse, t);
    return d;
}

struct literal_value* cscLiteralString(CSC2Parser *pParse, Token *t) {
    struct literal_value *d = cscAlloc(pParse, sizeof(struct literal_value));
    d->type = CLIENT_CSTR;
    d->u.string = cscTokenStr(pParse, t);
    return d;
}

/* We're assuming the lexer is working properly and we end up with hex chars here. */
static int hexdigit(char t) {
    if (t >= '0' && t <= '9')
        return t - '0';
    if (t >= 'a' && t <= 'f')
        return t - 'a';
    if (t >= 'A' && t <= 'F')
        return t - 'A';
    return -1;
}

struct literal_value* cscLiteralBlob(CSC2Parser *pParse, Token *t) {
    uint8_t *v;
    struct literal_value *d = cscAlloc(pParse, sizeof(struct literal_value));

    d->type = CLIENT_BYTEARRAY;
    v = cscAlloc(pParse, t->n / 2);
    for (int i = 0; i < t->n / 2; i+= 2) {
        v[i/2] = hexdigit(t->z[i]) << 4 | hexdigit(t->z[i+1]);
    }
    d->u.blob.len = t->n / 2;
    d->u.blob.p = v;
    return d;
}

void dumpLiteral(CSC2Parser *pParse, struct literal_value *v) {
    switch (v->type) {
        case CLIENT_INT:
            printf("%d", v->u.number);
            break;
        case CLIENT_CSTR:
            printf("\"%s\"", v->u.string);
            break;
        case CLIENT_BYTEARRAY:
            printf("x'");
            for (int i = 0; i < v->u.blob.len; i++)
                printf("%02x", ((uint8_t*) v->u.blob.p)[i]);
            printf("'");
            break;
        default:
            printf("???");
            break;
    }
}

void dumpFieldOptions(CSC2Parser *pParse, struct field_option_list *l) {
    for (struct field_option *o = l->options.top; o; o = o->lnk.next) {
        switch (o->type) {
            case FIELD_OPTION_DBSTORE:
                printf(" dbstore="); dumpLiteral(pParse, o->u.l);
                break;
            case FIELD_OPTION_DBLOAD:
                printf(" dbload="); dumpLiteral(pParse, o->u.l);
                break;
            case FIELD_OPTION_DBPAD:
                printf(" dbpad=%d", (int) o->u.dbpad);
                break;
            case FIELD_OPTION_NULLABLE:
                printf(" null=%s", o->u.nullable ? "yes" : "no");
                break;
        }
    }
}

void initCSC2Parser(CSC2Parser *pParse) {
    memset(pParse, 0, sizeof(CSC2Parser));
    pParse->line = 1;
    listc_init(&pParse->constants, offsetof(struct constant, lnk));
    listc_init(&pParse->allocations, offsetof(struct alloc, lnk));
    listc_init(&pParse->keys, offsetof(struct key, lnk));
    listc_init(&pParse->schemas, offsetof(struct schema, lnk));
    listc_init(&pParse->constraints, offsetof(struct constraint, lnk));
}

void cscParserDestroy(CSC2Parser *pParse) {
    struct alloc *a, *next;
    for (a = pParse->allocations.top; a; a = next) {
        next = a->lnk.next;
        free(a->p);
        free(a);
    }
}

int isVariableSizeType(int type) {
    switch (type) {
        case CLIENT_VUTF8:
        case CLIENT_CSTR:
        case CLIENT_BYTEARRAY:
        case CLIENT_BLOB:
        case SERVER_BLOB2:
        case SERVER_VUTF8:
        case SERVER_BYTEARRAY:
        case SERVER_BLOB:
            return 1;
        default:
            return 0;
    }
}

static const char* typeToStr(int type, int size) {
    switch (type) {
        case SERVER_UINT:
        case CLIENT_UINT:
            switch (size) {
                case 3:
                case 2:
                    return "u_short";
                    break;
                case 5:
                case 4:
                    return "u_int";
                    break;
                case 9:
                case 8:
                    return "u_longlong";
                    break;
                default:
                    return "???";
            }
            break;
        case SERVER_BINT:
        case CLIENT_INT:
            switch (size) {
                case 3:
                case 2:
                    return "short";
                    break;
                case 5:
                case 4:
                    return "int";
                    break;
                case 9:
                case 8:
                    return "longlong";
                    break;
                default:
                    return "???";
            }
            break;
        case SERVER_BREAL:
        case CLIENT_REAL:
            switch (size) {
                case 5:
                case 4:
                    return "float";
                    break;
                case 9:
                case 8:
                    return "double";
                    break;
                default:
                    return "???";
            }
            break;
        case SERVER_BCSTR:
        case CLIENT_CSTR:
            return "cstring";
            break;
        case CLIENT_PSTR:
        case CLIENT_PSTR2:
            return "pstring";
            break;
        case SERVER_BYTEARRAY:
        case CLIENT_BYTEARRAY:
            return "byte";
            break;
        case SERVER_BLOB:
        case SERVER_BLOB2:
        case CLIENT_BLOB:
            return "blob";
            break;
        case SERVER_DATETIME:
        case CLIENT_DATETIME:
            return "datetime";
            break;
        case SERVER_INTVYM:
        case CLIENT_INTVYM:
            return "intervalym";
            break;
        case SERVER_INTVDS:
        case CLIENT_INTVDS:
            return "intervalds";
            break;
        case SERVER_VUTF8:
        case CLIENT_VUTF8:
            return "vutf8";
            break;
        case SERVER_DECIMAL:
            switch (size) {
                case 5:
                    return "decimal32";
                    break;
                case 9:
                    return "decimal64";
                    break;
                case 17:
                    return "decimal128";
                    break;
                default:
                    return "???";
            }
            break;
        case SERVER_DATETIMEUS:
        case CLIENT_DATETIMEUS:
            return "datetimeus";
            break;
        case SERVER_INTVDSUS:
        case CLIENT_INTVDSUS:
            return "intervaldsus";
            break;
        default:
            return "???";
            break;
    }
}

struct field* cscNewField(CSC2Parser *pParse, struct type *type, Token *namet, int arrsz, struct field_option_list *opts) {
    struct field *f;

    if (arrsz != -1 && !isVariableSizeType(type->basetype))
        cscErrorMsg(pParse, "line %d, arrays of type %s aren't supported", pParse->line, typeToStr(type->basetype, type->size));

    f = cscAlloc(pParse, sizeof(struct field));
    f->type = type->basetype;
    f->size = type->size;
    f->arrsize = arrsz;
    f->name = cscTokenStr(pParse, namet);
    f->options = opts;
    return f;
}

struct schema* cscNewRecord(CSC2Parser *pParse) {
    struct schema *s = cscAlloc(pParse, sizeof(struct schema));
    listc_init(&s->fields, offsetof(struct field, lnk));
    return s;
}

void cscAddRecord(CSC2Parser *pParse, struct schema *s) {
    struct schema *es;
    LISTC_FOR_EACH(&pParse->schemas, es, lnk) {
        if (strcasecmp(es->tagname, s->tagname) == 0) {
            if (strcasecmp(s->tagname, ".ONDISK") == 0) {
                cscErrorMsg(pParse, "line %d, multiple record definitions", pParse->line);
                return;
            }
            else if (strcasecmp(s->tagname, ".DEFAULT") == 0) {
                cscErrorMsg(pParse, "line %d, multiple default tag definitions", pParse->line);
                return;
            }
            else {
                cscErrorMsg(pParse, "line %d, multiple definitions of tag \"%s\"", pParse->line, s->tagname);
                return;
            }
        }
    }
    listc_abl(&pParse->schemas, s);
}

void cscSchemaSetName(CSC2Parser *pParse, struct schema *s, char *name) {
    s->tagname = name;
}

struct field_list* cscNewFieldList(CSC2Parser *pParse) {
    struct field_list *fl;
    fl = cscAlloc(pParse, sizeof(struct field_list));
    listc_init(&fl->fields, offsetof(struct field_name, lnk));
    return fl;
}

void cscFieldListAddField(CSC2Parser *pParse, struct field_list *fl, Token *namet, int desc) {
    struct field_name *fn;

    fn = cscAlloc(pParse, sizeof(struct field_name));
    fn->name = cscTokenStr(pParse, namet);
    fn->descend = desc;
    listc_abl(&fl->fields, fn);
}

struct key* cscAddKey(CSC2Parser *pParse, Token *keyname, int flags, struct field_list *fields) {
    struct key *k, *ek;

    k = cscAlloc(pParse, sizeof(struct key));
    k->name = cscTokenStr(pParse, keyname);
    k->flags = flags;
    k->fields.top = (void*) fields->fields.top;
    k->fields.bot = (void*) fields->fields.bot;
    k->fields.diff = fields->fields.diff;
    k->fields.count = fields->fields.count;

    LISTC_FOR_EACH(&pParse->keys, ek, lnk) {
        if (strcasecmp(k->name, ek->name) == 0) {
            cscErrorMsg(pParse, "line %d, duplicate key definition", pParse->line);
            return k;
        }
    }

    listc_abl(&pParse->keys, k);
    return k;
}

static void dumpSchema(CSC2Parser *p, struct schema *s) {
    if (strcasecmp(s->tagname, ".ONDISK") == 0) {
        printf("schema {\n");
    }
    else if (strcasecmp(s->tagname, ".DEFAULT") == 0) {
        printf("tag default {\n");
    }
    else {
        printf("tag \"%s\" {\n", s->tagname);
    }
    struct field *f;
    LISTC_FOR_EACH(&s->fields, f, lnk) {
        printf("    ");
        printf("%s %s", typeToStr(f->type, f->size), f->name);
        if (f->arrsize != -1)
            printf("[%d]", f->arrsize);
        if (f->options)
            dumpFieldOptions(p, f->options);

        printf("\n");
    }
    printf("}\n");
    printf("\n");
}

static void dumpCSC(CSC2Parser *p) {
    struct schema *s;
    struct constant *c;

    if (p->constants.count > 0) {
        printf("constants {\n");
        LISTC_FOR_EACH(&p->constants, c, lnk) {
            printf("   %s=%d%s\n", c->name, c->value, c->lnk.next == NULL ? "" : ",");
        }
        printf("}\n\n");
    }
    LISTC_FOR_EACH(&p->schemas, s, lnk) {
        dumpSchema(p, s);
    }
    if (p->keys.count > 0) {
        printf("keys {\n");
        struct key *k;
        LISTC_FOR_EACH(&p->keys, k, lnk) {
            printf("    ");
            if (k->flags & 1)
                printf("dup ");
            if (k->flags & 2)
                printf("recnums ");
            if (k->flags & 4)
                printf("datacopy ");
            printf("\"%s\" = ", k->name);
            struct field_name *f;
            LISTC_FOR_EACH(&k->fields, f, lnk) {
                if (f->descend)
                    printf("<DESCEND>");
                printf("%s", f->name);
                if (f->lnk.next)
                    printf(" + ");
            }
            printf("\n");
        }
        printf("}\n\n");
    }
    if (p->constraints.count > 0) {
        struct constraint *c;
        LISTC_FOR_EACH(&p->constraints, c, lnk) {
            printf("  \"%s\" -> <\"%s\":\"%s\"> ", c->key, c->table, c->fkey);
            if (c->options & CASCADE_UPDATE)
                printf(" on update cascade ");
            if (c->options & CASCADE_DELETE)
                printf(" on delete cascade ");
        }
    }
}

struct constraint* cscAddConstraint(CSC2Parser *pParse, Token *keyt, Token *tablet, Token *foreignt, int options) {
    struct constraint *c;
    c = cscAlloc(pParse, sizeof(struct constraint));
    c->key = tokstr(pParse, keyt);
    c->table = tokstr(pParse, tablet);
    c->fkey = tokstr(pParse, foreignt);
    c->options = options;
    listc_abl(&pParse->constraints, c);
    return c;
}

static int keyExists(CSC2Parser *pParse, char *key) {
    struct key *k;
    LISTC_FOR_EACH(&pParse->keys, k, lnk) {
        if (strcmp(k->name, key) == 0)
            return 1;
    }
    return 0;
}

void cscFinalChecks(CSC2Parser *pParse) {
    struct constraint *c;
    LISTC_FOR_EACH(&pParse->constraints, c, lnk) {
        if (!keyExists(pParse, c->key)) {
            cscErrorMsg(pParse, "line %d, constraint references unknown key \"%s\"", pParse->line, c->key);
        }
    }
}

int main(int argc, char *argv[]) {
    char *fname;
    FILE *f;
    off_t sz;
    char *csc, *csc_orig;
    int rc;

    int tok;
    char *csc_copy = csc;
    Token t;
    CSC2Parser p = {0};

    initCSC2Parser(&p);

    if (argc != 2) {
        fprintf(stderr, "Usage: file\n");
        return 1;
    }
    fname = argv[1];
    f = fopen(fname, "r");
    if (f == NULL) {
        fprintf(stderr, "can't open %s\n", fname);
        return 1;
    }
    if (fseek(f, 0, SEEK_END)) {
        fprintf(stderr, "can't discover size of %s\n", fname);
        return 1;
    }
    sz = ftell(f);
    rewind(f);
    csc = malloc(sz+1);
    csc_orig = csc;
    rc = fread(csc, sz, 1, f);
    if (rc != 1) {
        fprintf(stderr, "can't read %s\n", fname);
        return 1;
    }
    csc[sz] = 0;

    // csc2ParserTrace(stdout, ">>>");

    void *parser;
    parser = csc2ParserAlloc(malloc);
    while ((tok = nextToken(&p, &csc, &t)) != 0) {
        if (tok == -1)
            break;

        csc2Parser(parser, tok, t, &p);
        rc |= p.rc;
    }
    /* end of file and no error? finalize parser */
    csc2Parser(parser, 0, t, &p);
    if (p.rc) {
        fprintf(stderr, "parser error\n");
        return -1;
    }

    dumpCSC(&p);

    cscParserDestroy(&p);
    csc2ParserFree(parser, free);
    free(csc_orig);

    return 0;
}
