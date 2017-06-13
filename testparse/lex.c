#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>

#include "csc2parser.h"
#include "parse.h"

struct reservedWord {
    char *lex;
    int tok;
};

static struct reservedWord reservedWords[] = {
    { "schema", TK_SCHEMA },
    { "tag", TK_TAG },
    { "default", TK_DEFAULT },
    { "short", TK_SHORT },
    { "u_short", TK_U_SHORT },
    { "int", TK_INT },
    { "u_int", TK_U_INT },
    { "longlong", TK_LONGLONG },
    { "u_longlong", TK_U_LONGLONG },
    { "float", TK_FLOAT },
    { "double", TK_DOUBLE },
    { "byte", TK_BYTE },
    { "cstring", TK_CSTRING },
    { "pstring", TK_PSTRING },
    { "blob", TK_BLOB },
    { "datetime", TK_DATETIME },
    { "datetimeus", TK_DATETIMEUS },
    { "intervalym", TK_INTERVALYM },
    { "intervalds", TK_INTERVALDS },
    { "intervaldsus", TK_INTERVALDSUS },
    { "vutf8", TK_VUTF8 },
    { "decimal32", TK_DECIMAL32 },
    { "decimal64", TK_DECIMAL64 },
    { "decimal128", TK_DECIMAL128 },
    { "yes", TK_YES },
    { "no", TK_NO },
    { "dbload", TK_DBLOAD },
    { "dbstore", TK_DBSTORE },
    { "null", TK_NULL },
    { "dbpad", TK_DBPAD },
    { "keys", TK_KEYS },
    { "constants", TK_CONSTANTS },
    { "constraints", TK_CONSTRAINTS },
    { "on", TK_ON },
    { "delete", TK_DELETE },
    { "cascade", TK_CASCADE },
    { "update", TK_UPDATE },
    { "restrict", TK_RESTRICT },
    { "ascend", TK_ASCEND },
    { "descend", TK_DESCEND },
    { "datacopy", TK_DATACOPY },
    { "dup", TK_DUP },
    { "recnums", TK_RECNUMS },
    { "ondisk", TK_ONDISK },
    { "{", TK_OPEN_BRACE },
    { "}", TK_CLOSE_BRACE },
    { "[", TK_OPEN_BRACKET },
    { "]", TK_CLOSE_BRACKET },
    { "=", TK_EQUALS },
    { "+", TK_PLUS },
    { "-", TK_MINUS },
    { ">", TK_GREATER },
    { "<", TK_LESS },
    { ":", TK_COLON },
    { ",", TK_COMMA }
};

int nextToken(CSC2Parser *pParse, char **s, Token *tokout) {
    char *t = *s;
    int tokcode = 0;

    tokout->n = 0;

    while (*t) {
        if (*t == '\n') {
            pParse->line++;
            t++;
            *s = t;
            continue;
        }
        if (isspace(*t)) {
            t++;
            *s = t;
            continue;
        }
        if (*t == '/' && *(t+1) == '/') {
            while (*t && *t != '\n') t++;
            if (*t == 0) {
                return 0;  /* eof */
            }
            t++;
            *s = t;
            continue;
        }
        if (*t == 'x' && *(t+1) == '\'') {
            int numdigits = 0;
            t+=2;
            tokout->z = t;
            tokcode = TK_HEXSTRING;
            while (*t) {
                if (*t == 0 || *t == '\n') {
                    cscErrorMsg(pParse, "Unterminated blob literal");
                    return -1;
                }
                else if (*t == '\'') {
                    if (numdigits % 2 != 0) {
                        cscErrorMsg(pParse, "Invalid blob literal");
                        return -1;
                    }
                    t++;
                    break;
                }
                else if (!isxdigit(*t)) {
                    cscErrorMsg(pParse, "Invalid character in blob literal");
                    return -1;
                }
                numdigits++;
                t++;
            }
            goto done;
        }
        if (isalpha(*t)) {
            tokout->z = t;
            while (*t && (*t == '_' || isdigit(*t) || isalpha(*t))) {
                t++;
            }
            for (int i = 0; i < sizeof(reservedWords)/sizeof(reservedWords[0]); i++) {
                if (strlen(reservedWords[i].lex) == (t - *s) && strncasecmp(reservedWords[i].lex, tokout->z, (t - *s)) == 0) {
                    tokcode = reservedWords[i].tok;
                    goto done;
                }
            }
            tokcode = TK_ID;
            goto done;
        }
        else if (isdigit(*t)) {
            tokout->z = t;
            while (isdigit(*t) || *t == '.')
                t++;
            tokcode = TK_NUMBER;
            goto done;
        }
        else if (*t == '\"') {
            t++;
            tokout->z = t;
            while (*t) {
                if (*t == '\"' && *(t+1) == '\"')
                    t++;
                if (*t == '\"') {
                    t++;
                    break;
                }
                if (*t == 0 || *t == '\n') {
                    cscErrorMsg(pParse, "unterminated string");
                    return -1;
                }
                t++;
            }
            tokcode = TK_STRING;
            goto done;
        }
        else {
            for (int i = 0; i < sizeof(reservedWords)/sizeof(reservedWords[0]); i++) {
                if (reservedWords[i].lex[0] == *t) {
                    tokcode = reservedWords[i].tok;
                    t++;
                    goto done;
                }
            }

            cscErrorMsg(pParse, "unknown token");
            return -1;
        }
    }
    return 0;

done:
    tokout->n = t - *s;
    *s = t;
    if (tokcode == TK_STRING || tokcode == TK_HEXSTRING)
        tokout->n-=2;
    return tokcode;
}
