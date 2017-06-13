%token_prefix TK_
%token_type {Token}

%extra_argument {CSC2Parser *pParse}

%include {
#include <stdint.h>
#include <assert.h>
#include "csc2parser.h"
#include "csctypes.h"
#include "list.h"

typedef uint64_t u64;

#define NEVER(X) 0
#define ALWAYS(X) 1

}

%parse_accept {
    cscFinalChecks(pParse);
}

%syntax_error {
    cscErrorMsg(pParse, "line %d, near \"%.*s\": syntax error", pParse->line, TOKEN.n, TOKEN.z);
    pParse->rc = 1;
}

%name csc2Parser


start ::= csc.

csc ::= section.
csc ::= csc section.

section ::= schema_section.
section ::= keys_section.
section ::= constants_section.
section ::= constraints_section.

%type record {struct schema*}
schema_section ::= SCHEMA OPEN_BRACE record(R) CLOSE_BRACE.        { cscSchemaSetName(pParse, R, ".ONDISK");  cscAddRecord(pParse, R); }
schema_section ::= TAG DEFAULT OPEN_BRACE record(R) CLOSE_BRACE.   { cscSchemaSetName(pParse, R, ".DEFAULT"); cscAddRecord(pParse, R); }
schema_section ::= TAG ONDISK OPEN_BRACE record(R) CLOSE_BRACE.    { cscSchemaSetName(pParse, R, ".ONDISK"); cscAddRecord(pParse, R); }
schema_section ::= TAG STRING(S) OPEN_BRACE record(R) CLOSE_BRACE. { cscSchemaSetName(pParse, R, cscTokenStr(pParse, &S)); cscAddRecord(pParse, R); }

%type field {struct field*}
record(OUT) ::= record(R) field(F).  { cscAddField(pParse, R, F); OUT = R; }
record(R) ::= field(F).         { R = cscNewRecord(pParse); cscAddField(pParse, R, F); }

%type type {struct type}
type(T) ::= SHORT.           { T.basetype = CLIENT_INT; T.size = 2; }
type(T) ::= U_SHORT.         { T.basetype = CLIENT_UINT; T.size = 2; }
type(T) ::= INT.             { T.basetype = CLIENT_INT; T.size = 4; }
type(T) ::= U_INT.             { T.basetype = CLIENT_UINT; T.size = 4; }
type(T) ::= LONGLONG.             { T.basetype = CLIENT_INT; T.size = 8; }
type(T) ::= U_LONGLONG.             { T.basetype = CLIENT_UINT; T.size = 8; }
type(T) ::= FLOAT.             { T.basetype = CLIENT_REAL; T.size = 4; }
type(T) ::= DOUBLE.             { T.basetype = CLIENT_REAL; T.size = 8; }
type(T) ::= BYTE.             { T.basetype = CLIENT_BYTEARRAY; T.size = -1; }
type(T) ::= CSTRING.             { T.basetype = CLIENT_CSTR; T.size = -1; }
type(T) ::= PSTRING.             { T.basetype = CLIENT_PSTR; T.size = -1; }
type(T) ::= BLOB.             { T.basetype = CLIENT_BLOB; T.size = -1; }
type(T) ::= DATETIME.             { T.basetype = CLIENT_DATETIME; T.size = -1; }
type(T) ::= DATETIMEUS.             { T.basetype = CLIENT_DATETIMEUS; T.size = -1; }
type(T) ::= INTERVALYM.             { T.basetype = CLIENT_INTVYM; T.size = -1; }
type(T) ::= INTERVALDS.             { T.basetype = CLIENT_INTVDS; T.size = -1; }
type(T) ::= INTERVALDSUS.             { T.basetype = CLIENT_INTVDSUS; T.size = -1; }
type(T) ::= VUTF8.             { T.basetype = CLIENT_VUTF8; T.size = -1; }
/* This is a bit of a kludge - there's no corresponding client types for decimal */
type(T) ::= DECIMAL32.             { T.basetype = SERVER_DECIMAL; T.size = 5; }
type(T) ::= DECIMAL64.             { T.basetype = SERVER_DECIMAL; T.size = 9; }
type(T) ::= DECIMAL128.             { T.basetype = SERVER_DECIMAL; T.size = 17; }

%type yesno {int}
yesno(YN) ::= YES.     { YN = 1; }
yesno(YN) ::= NO.      { YN = 0; }

%type size {int}
size(SZ) ::= OPEN_BRACKET NUMBER(N) CLOSE_BRACKET.  { int n = cscTokNum(pParse, &N); SZ = n; }
size(SZ) ::= OPEN_BRACKET ID(I) CLOSE_BRACKET.      {
    int n = -1; 
    char *name = cscTokenStr(pParse, &I);
    if (cscFindConstant(pParse, name, &n) == 0) {
        cscErrorMsg(pParse, "line %d, undefined constant %s", pParse->line, name);
        SZ = -1;
    }
    SZ = n;
}
size(SZ) ::= .  { SZ = -1; }  /* not specified, type default */

%type literal {struct literal_value*}
literal(L) ::= NUMBER(T).            { L = cscLiteralInteger(pParse, &T);  }
literal(L) ::= STRING(T).            { L = cscLiteralString(pParse, &T);   }
literal(L) ::= HEXSTRING(T).         { L = cscLiteralBlob(pParse, &T);     }

%type field_option {struct field_option*}
field_option(O) ::= DBSTORE EQUALS literal(L).   { O = cscAlloc(pParse, sizeof(struct field_option)); O->type = FIELD_OPTION_DBSTORE; O->u.l = L; }
field_option(O) ::= DBLOAD EQUALS literal(L).    { O = cscAlloc(pParse, sizeof(struct field_option)); O->type = FIELD_OPTION_DBLOAD; O->u.l = L; }
field_option(O) ::= NULL EQUALS yesno(YN).       { O = cscAlloc(pParse, sizeof(struct field_option)); O->type = FIELD_OPTION_NULLABLE; O->u.nullable = YN; }
field_option(O) ::= DBPAD EQUALS NUMBER(N).      { O = cscAlloc(pParse, sizeof(struct field_option)); O->type = FIELD_OPTION_DBPAD; O->u.dbpad = cscTokNum(pParse, &N); }

%type field_options {struct field_option_list*}
field_options(OPTS) ::= field_option(OPT).              { OPTS = cscNewFieldOptions(pParse); cscAddFieldOption(pParse, OPTS, OPT); }
field_options(OPTS) ::= field_options(RESTOPT) field_option(OPT).   { cscAddFieldOption(pParse, RESTOPT, OPT); OPTS = RESTOPT; }

field(F) ::= type(T) ID(ID) size(SZ) field_options(OPTS).  { F = cscNewField(pParse, &T, &ID, SZ, OPTS); }
field(F) ::= type(T) ID(ID) size(SZ).                      { F = cscNewField(pParse, &T, &ID, SZ, NULL); }

keys_section ::= KEYS OPEN_BRACE keydefs CLOSE_BRACE.

keydefs ::= keydef keydefs.
keydefs ::= .

%type key_field_option {int}
key_field_option ::= LESS ASCEND GREATER.
key_field_option(O) ::= LESS DESCEND GREATER.  { O = 1; }
key_field_option ::= .


%type field_list {struct field_list*}
field_list(L) ::= field_list(EL) PLUS key_field_option(DESC) ID(I).  { cscFieldListAddField(pParse, EL, &I, DESC); L = EL; }
field_list(L) ::= key_field_option(DESC) ID(I).     { L = cscNewFieldList(pParse);  cscFieldListAddField(pParse, L, &I, DESC); }

%type key_option {int}
key_option(O) ::= DUP.      { O |= 1; }
key_option(O) ::= RECNUMS.  { O |= 2; }
key_option(O) ::= DATACOPY. { O |= 4; }

%type key_options {int}
key_options(OPTS) ::= key_options(EOPTS) key_option(O). { OPTS = EOPTS | O; }
key_options(OPTS) ::= key_option(O). { OPTS = O; }

%type keydef {struct key*}
keydef(K) ::= key_options(O) STRING(N) EQUALS field_list(L). { K = cscAddKey(pParse, &N, O, L); }
keydef(K) ::= STRING(N) EQUALS field_list(L). { K = cscAddKey(pParse, &N, 0, L); }

constants_section ::= CONSTANTS OPEN_BRACE constants CLOSE_BRACE.
constants_section ::= CONSTANTS OPEN_BRACE CLOSE_BRACE.

constant ::= ID(I) EQUALS NUMBER(N).     { cscAddConstant(pParse, &I, &N); }

constants ::= constants COMMA constant.
constants ::= constant.

constraints_section ::= CONSTRAINTS OPEN_BRACE constraints CLOSE_BRACE.
constraints_section ::= CONSTRAINTS OPEN_BRACE CLOSE_BRACE.

constraints ::= constraint.
constraints ::= constraints constraint.

%type constraint_option {int}
%type constraint_options {int}

// Yes, - > parses.  Almost certainly not intentional.
%type constraint {struct constraint*}
constraint(C) ::= STRING(L) MINUS GREATER LESS STRING(T) COLON STRING(K) GREATER constraint_options(O). { C = cscAddConstraint(pParse, &L, &T, &K, O); }
constraint(C) ::= STRING(L) MINUS GREATER LESS STRING(T) COLON STRING(K) GREATER. { C = cscAddConstraint(pParse, &L, &T, &K, 0); }

constraint_options(OS) ::= constraint_options(OSR) constraint_option(O). { OS |= (OSR | O); }
constraint_options(OS) ::= constraint_option(O).  { OS |= O; }

constraint_option(O) ::= ON DELETE CASCADE. { O |= CASCADE_DELETE; }
constraint_option(O) ::= ON UPDATE CASCADE. { O |= CASCADE_UPDATE; }
constraint_option(O) ::= ON DELETE RESTRICT. { O &= ~CASCADE_DELETE; }
constraint_option(O) ::= ON UPDATE RESTRICT. { O &= ~CASCADE_UPDATE; }
