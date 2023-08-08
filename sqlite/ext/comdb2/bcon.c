/*
   Copyright 2023, Bloomberg Finance L.P.

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


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>
#include <stdint.h>
#include <assert.h>
#include <stdarg.h>
#include <inttypes.h>
#include <math.h>

#include "sqliteInt.h"

// Format is simple:
// A 32bit dictionary size, followed by a sequence of varint length and value pairs for strings.
// Following the dictionary is the value.  The first byte is the type (JSON_TYPE*) for all types.
// Integers are an sqlite varint value (variable length, from 1-9 bytes).  Strings are also a varint.
// The value is the offset of the string in the dictionary. Array and object values are followed
// with 2 32-bit sizes.  The first is the total size of the array.  Adding that to the offset of the
// type skips the object.  The second is the number of entries.  For objects, it's
// a list of key value pairs.  The key is always a string.  The two integers are fixed size so that we
// can append to the array/object without moving data around if the size needs to grow.  bcon_explain
// will dump a text description of the value with all offsets for debugging.

enum json_type {
    JSON_TYPE_INVALID = 0,

    JSON_TYPE_STRING  = 1,
    JSON_TYPE_ARRAY   = 2,
    JSON_TYPE_INTEGER  = 3,
    JSON_TYPE_OBJECT  = 4,
    JSON_TYPE_NUMBER  = 5,
    JSON_TYPE_TRUE  = 6,
    JSON_TYPE_FALSE = 7,
    JSON_TYPE_NULL = 8
};
typedef enum json_type json_type;
typedef uint32_t json_off;

struct buf {
    uint8_t *data;
    size_t len;
    size_t cap;
};
typedef struct buf buf;

#define BUF_STATIC_INIT { .data = NULL, .len = 0, .cap = 0 }

struct json_parser {
    buf dict;
    buf value;
    char *error;
    const char *start;
    const char *pos;
};
typedef struct json_parser json_parser;

static json_type parse_json_next(json_parser *p);

static void resize(buf *b, size_t sz) {
    if (b->len + sz <= b->cap)
        return;
    b->cap = (b->len + sz) * 2 + 16;
    // b->cap = b->len + sz;
    b->data = realloc(b->data, b->cap);
}

static uint32_t bufadd(buf *b, const void* data, size_t sz) {
    uint32_t ret;
    resize(b, sz);
    memcpy(b->data + b->len, data, sz);
    ret = b->len;
    b->len += sz;
    return ret;
}

static uint32_t bufaddint(buf *b, u64 val) {
    // worst case fits in 9 bytes
    resize(b, 9);
    int sz = putVarint(b->data+b->len, val);
    b->len += sz;
    return sz;
}

static void bufreset(buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static uint32_t bufgetint(buf *b, uint32_t off, u64 *val) {
    int sz = getVarint(b->data + off, val);
    return sz;
}

static int frombuf(buf *b, uint32_t off, void *data, size_t sz) {
    memcpy(data, b->data + off, sz);
    return 0;
}

static void dict_init(buf *dict) {
    uint32_t sz = sizeof(uint32_t);
    bufadd(dict, &sz, sizeof(uint32_t));
}

static uint32_t add_to_dict(buf *dict, const char *key, u64 klen) {
    uint32_t dictsize;
    frombuf(dict, 0, &dictsize, sizeof(uint32_t));
    uint32_t off = (uint32_t) sizeof(uint32_t);
    while(off < dictsize) {
        u64 sz;
        uint32_t szsz;
        szsz = bufgetint(dict, off, &sz);
        if (sz == klen && memcmp(dict->data + off + szsz, key, klen) == 0) {
            return off;
        }
        off += (sz + szsz);
    }
    off = dict->len;
    uint32_t szsz;
    szsz = bufaddint(dict, klen);
    bufadd(dict, key, klen);
    dictsize += (szsz + klen);
    memcpy(dict->data, &dictsize, sizeof(uint32_t));
    return off;
}

static char *get_from_dict(buf *dict, uint32_t off, u64 *klen) {
    uint32_t dictsize;
    if (frombuf(dict, 0, &dictsize, sizeof(uint32_t)))
        return 0;
    if (off > dictsize)
        return NULL;
    uint32_t keyszsz = bufgetint(dict, off, klen);
    if (off + *klen > (dictsize + sizeof(uint32_t)))
        return NULL;
    return (char*) (dict->data + off + keyszsz);
}

static json_type parse_num(json_parser *p) {
    int len = 0;
    const char *start;
    uint8_t type = JSON_TYPE_INTEGER;

    printf("%s %s\n", __func__, p->pos);

    start = p->pos;
    if (*p->pos == '-')
        p->pos++;
    while (isdigit(*p->pos)) {
        len++;
        p->pos++;
    }
    // TODO: obviously not complete code for a reading doubles, just for a quick prototype
    if (*p->pos == '.') {
        len++;
        type = JSON_TYPE_NUMBER;
        p->pos++;
        if (!isdigit(*p->pos)) {
            return JSON_TYPE_INVALID;
        }
        while (isdigit(*p->pos)) {
            len++;
            p->pos++;
        }
        if (*p->pos == 'e' || *p->pos == 'E') {
            p->pos++;
            if (*p->pos == '-')
                p->pos++;
            while (isdigit(*p->pos)) {
                len++;
                p->pos++;
            }
        }
    }

    if (len > 1100) {  /* TODO: figure out from float.h */
        return JSON_TYPE_INVALID;
    }

    char *n = alloca(len+1);
    memcpy(n, start, len);
    n[len] = 0;

    bufadd(&p->value, &type, sizeof(uint8_t));
    if (type == JSON_TYPE_INTEGER) {
        bufaddint(&p->value, strtoll(n, NULL, 10));
    }
    else if (type == JSON_TYPE_NUMBER) {
        // TODO: error check
        double d = strtod(n, NULL);
        bufadd(&p->value, &d, sizeof(double));
    }

    return JSON_TYPE_INTEGER;
}

static json_off parse_string(json_parser *p) {
    const char *start = p->pos;
    int len = 0;

    printf("%s %s\n", __func__, p->pos);

    while (*p->pos != '"') {
        len++;
        if (*p->pos == 0) {
            printf("huh? %d\n", __LINE__);
            return 0;
        }
        p->pos++;
    }
    p->pos++;
    uint8_t type = JSON_TYPE_STRING;
    bufadd(&p->value, &type, sizeof(uint8_t));
    json_off key = add_to_dict(&p->dict, start, len);
    bufaddint(&p->value, key);
    return JSON_TYPE_STRING;
}

static json_type parse_array(json_parser *p) {
    json_off start = p->value.len;
    uint32_t totalsize = 0;
    uint32_t nent = 0;
    uint8_t type = JSON_TYPE_ARRAY;
    bufadd(&p->value, &type, sizeof(uint8_t));
    json_off totalsize_off = p->value.len;
    bufadd(&p->value, &totalsize, sizeof(uint32_t));
    json_off nent_off = p->value.len;
    bufadd(&p->value, &nent, sizeof(uint32_t));
    json_off next;
    printf("%s %s\n", __func__, p->pos);

    for (;;) {
        while (isspace(*p->pos))
            p->pos++;
        // handle [] case
        if (*p->pos == ']') {
            p->pos++;
            break;
        }
        next = parse_json_next(p);
        if (next == JSON_TYPE_INVALID) {
            printf("huh? %d\n", __LINE__);
            return JSON_TYPE_INVALID;
        }
        nent++;
        while (isspace(*p->pos))
            p->pos++;
        if (*p->pos == ']') {
            p->pos++;
            break;
        }
        else if (*p->pos != ',') {
            printf("huh? %d\n", __LINE__);
            return JSON_TYPE_INVALID;
        }
        p->pos++;
    }
    totalsize = p->value.len - start;
    memcpy(p->value.data + totalsize_off, &totalsize, sizeof(uint32_t));
    memcpy(p->value.data + nent_off, &nent, sizeof(uint32_t));

    return JSON_TYPE_ARRAY;
}

static json_off parse_object(json_parser *p) {
    json_off start = p->value.len;
    uint8_t type = JSON_TYPE_OBJECT;
    bufadd(&p->value, &type, sizeof(uint8_t));
    uint32_t totalsize = 0;
    json_off totalsize_off = bufadd(&p->value, &totalsize, sizeof(uint32_t));
    uint32_t nent = 0;
    json_off nent_off = bufadd(&p->value, &nent, sizeof(uint32_t));

    printf("%s %s\n", __func__, p->pos);

    while (isspace(*p->pos))
        p->pos++;
    if (*p->pos == '}')
        return JSON_TYPE_OBJECT;
    json_type next = parse_json_next(p);
    while (next != JSON_TYPE_INVALID) {
        nent++;
        if (next != JSON_TYPE_STRING) {
            printf("huh? %d\n", __LINE__);
            return JSON_TYPE_INVALID;
        }
        while (isspace(*p->pos))
            p->pos++;
        if (*p->pos != ':') {
            printf("huh? %d\n", __LINE__);
            return JSON_TYPE_INVALID;
        }
        p->pos++;
        while (isspace(*p->pos))
            p->pos++;
        const char *last = p->pos;
        next = parse_json_next(p);
        if (next == JSON_TYPE_INVALID) {
            printf("huh? %d at %s\n", __LINE__, last);
            return JSON_TYPE_INVALID;
        }
        while (isspace(*p->pos))
            p->pos++;
        if (*p->pos == '}') {
            p->pos++;
            break;
        }
        if (*p->pos != ',') {
            printf("huh? %d\n", __LINE__);
            return JSON_TYPE_INVALID;
        }
        p->pos++;
        next = parse_json_next(p);
    }
    totalsize = p->value.len - start;
    memcpy(p->value.data + totalsize_off , &totalsize, sizeof(uint32_t));
    memcpy(p->value.data + nent_off , &nent, sizeof(uint32_t));

    return JSON_TYPE_OBJECT;
}

static json_type parse_constant(json_parser *p) {
    uint8_t type = JSON_TYPE_INVALID;
    printf("%s %s\n", __func__, p->pos);
    if (strncmp(p->pos, "NaN", 3) == 0 && !isalnum(p->pos[3])) {
        type = JSON_TYPE_NUMBER;
        bufadd(&p->value, &type, sizeof(uint8_t));
        double d = NAN;
        bufadd(&p->value, &d, sizeof(double));
        p->pos += 3;
    }
    else if (strncmp(p->pos, "true", 4) == 0 && !isalnum(p->pos[4])) {
        type = JSON_TYPE_TRUE;
        bufadd(&p->value, &type, sizeof(uint8_t));
        p->pos += 4;
    }
    else if (strncmp(p->pos, "false", 5) == 0 && !isalnum(p->pos[5])) {
        type = JSON_TYPE_FALSE;
        bufadd(&p->value, &type, sizeof(uint8_t));
        p->pos += 5;
    }
    else if (strncmp(p->pos, "null", 4) == 0 && !isalnum(p->pos[4])) {
        type = JSON_TYPE_NULL;
        bufadd(&p->value, &type, sizeof(uint8_t));
        p->pos += 4;
    }

    return type;
}

static json_type parse_json_next(json_parser *p) {
    json_type type = JSON_TYPE_INVALID;
    while (*p->pos && isspace(*p->pos))
        p->pos++;

    if (isdigit(*p->pos) || *p->pos == '-') {
        type = parse_num(p);
    }
    else if (isalpha(*p->pos)) {
        type = parse_constant(p);
    }
    else if (*p->pos == '[') {
        p->pos++;
        type = parse_array(p);
    }
    else if (*p->pos == '{') {
        p->pos++;
        type = parse_object(p);
    }
    else if (*p->pos == '"') {
        p->pos++;
        type = parse_string(p);
    }
    else {
        printf("huh? %d\n", __LINE__);
        return JSON_TYPE_INVALID;
    }

    return type;
}

static void hexdump(uint8_t *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        size_t lim = len - off;
        if (lim > 16)
            lim = 16;
        printf("%08x |", (unsigned) off);
        for (int i = 0; i < lim; i++) {
            if (i > 0 && i % 4 == 0)
                printf(" ");
            printf("%02x", data[off + i]);
        }
        for (size_t i = lim; i < 16; i++) {
            printf("  ");
            if (i > 0 && i % 4 == 0)
                printf(" ");
        }
        printf("|");
        for (int i = 0; i < lim; i++) {
            if (i > 0 && i % 4 == 0)
                printf(" ");
            printf("%c", isprint(data[off+i]) ? data[off+i] : '.');
        }
        for (size_t i = lim; i < 16; i++) {
            if (i > 0 && i % 4 == 0)
                printf(" ");
            printf(" ");
        }
        printf("\n");
        off += lim;
    }
}

static void sprintf_to_buf(buf *out, const char *fmt, ...) {
    va_list args;
    int sz;

    if (out->data == NULL)
        resize(out, 1);

    va_start(args, fmt);
    sz = vsnprintf((char*) (out->data + out->len), out->cap - out->len, fmt, args);
    va_end(args);
    if (sz >= (out->cap - out->len)) {
        resize(out, out->cap - out->len + sz + 1);
        va_start(args, fmt);
        vsnprintf((char*) (out->data + out->len), out->cap - out->len + sz + 1, fmt, args);
        va_end(args);
    }
    out->len += (sz);
}

static void hexdumpbytes(buf *out, const uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        sprintf_to_buf(out, "%02x", bytes[i]);
    }
}

static json_type parse_json(const char *text, buf *out) {
    struct json_parser p = {
            .dict = {0},
            .value = {0},
            .error = NULL,
            .start = text,
            .pos = text
    };
    dict_init(&p.dict);
    json_type type = parse_json_next(&p);
    if (type == JSON_TYPE_INVALID) {
        bufreset(&p.dict);
        bufreset(&p.value);
        return JSON_TYPE_INVALID;
    }

    bufadd(&p.dict, p.value.data, p.value.len);
    bufreset(&p.value);
    *out = p.dict;
    return type;
}


static uint32_t dump_node_from_buf(buf *in, uint32_t off, buf *out, int unwrap) {
    uint8_t type;
    frombuf(in, off, &type, sizeof(uint8_t));
    off += sizeof(uint8_t);
    switch (type) {
        case JSON_TYPE_STRING: {
            u64 key;
            u64 klen;
            off += bufgetint(in, off, &key);
            char *s = get_from_dict(in, key, &klen);
            if (unwrap)
                sprintf_to_buf(out, "%.*s", (int) klen, s);
            else
                sprintf_to_buf(out, "\"%.*s\"", (int) klen, s);
            break;
        }
        case JSON_TYPE_OBJECT: {
            uint32_t totalsz;
            uint32_t nent;
            frombuf(in, off, &totalsz, sizeof(uint32_t));
            off += sizeof(uint32_t);
            frombuf(in, off, &nent, sizeof(uint32_t));
            off += sizeof(uint32_t);
            sprintf_to_buf(out, "{");
            for (uint32_t i = 0; i < nent; i++) {
                uint8_t keytype;
                frombuf(in, off, &keytype, sizeof(uint8_t));
                assert(keytype == JSON_TYPE_STRING);
                off += sizeof(uint8_t);
                u64 dictoff;
                off += bufgetint(in, off, &dictoff);
                char *key;
                u64 keylen;
                key = get_from_dict(in, dictoff, &keylen);
                if (key == NULL)
                    return 0;
                sprintf_to_buf(out, "\"%.*s\": ", (int) keylen, key);
                uint32_t nextoff = dump_node_from_buf(in, off, out, 0);
                if (nextoff == 0)
                    return 0;
                if (i != nent-1)
                    sprintf_to_buf(out, ", ");
                off = nextoff;
            }
            sprintf_to_buf(out, "}");
            break;
        }
        case JSON_TYPE_ARRAY: {
            json_off totalsz;
            json_off nent;
            frombuf(in, off, &totalsz, sizeof(uint32_t));
            off += sizeof(uint32_t);
            frombuf(in, off, &nent, sizeof(uint32_t));
            off += sizeof(uint32_t);
            sprintf_to_buf(out, "[");
            for (uint32_t i = 0; i < nent; i++) {
                uint32_t nextoff = dump_node_from_buf(in, off, out, 0);
                if (nextoff == 0)
                    return 0;
                off = nextoff;
                if (i != nent-1)
                    sprintf_to_buf(out, ", ");
            }
            sprintf_to_buf(out, "]");
            break;
        }

        case JSON_TYPE_NUMBER: {
            double d;
            frombuf(in, off, &d, sizeof(double));
            off += sizeof(double);
            sprintf_to_buf(out, "%f", d);
            break;
        }

        case JSON_TYPE_INTEGER: {
            u64 n;
            off += bufgetint(in, off, &n);
            sprintf_to_buf(out, "%"PRId64, n);
            break;
        }

        case JSON_TYPE_TRUE: {
            sprintf_to_buf(out, "true");
            break;
        } 

        case JSON_TYPE_FALSE: {
            sprintf_to_buf(out, "false");
            break;
        }

        case JSON_TYPE_NULL: {
            sprintf_to_buf(out, "null");
            break;
        }

        default:
            fprintf(stderr, "unknown type %d\n", (int) type);
            return 0;
    }
    return off;
}

static void dump_from_buf(buf *in, buf *out, int unwrap) {
    uint32_t off;
    frombuf(in, 0, &off, sizeof(uint32_t));
    dump_node_from_buf(in, off, out, unwrap);
}

static json_off find_property(buf *bin, json_off objoff, const char *key, size_t klen) {
    json_off off = objoff;
    uint32_t totalsz;
    uint32_t nent;
    char *nextkey;
    u64 nextklen;
    uint8_t type;
    frombuf(bin, objoff, &type, sizeof(uint8_t));
    if (type != JSON_TYPE_OBJECT)
        return 0;
    off += sizeof(uint8_t);
    frombuf(bin, off, &totalsz, sizeof(uint32_t));
    off += sizeof(uint32_t);
    frombuf(bin, off, &nent, sizeof(uint32_t));
    off += sizeof(uint32_t);
    for (uint32_t i = 0; i < nent; i++) {
        u64 keyoff;
        frombuf(bin, off, &type, sizeof(uint8_t));
        off += sizeof(uint8_t);
        if (type != JSON_TYPE_STRING)
            return 0;
        off += bufgetint(bin, off, &keyoff);
        nextkey = get_from_dict(bin, keyoff, &nextklen);
        if (nextkey == NULL)
            return 0;
        if (nextklen == klen && memcmp(nextkey, key, klen) == 0) {
            return off;
        }
        // skip the data and keep searching
        frombuf(bin, off, &type, sizeof(uint8_t));
        off += sizeof(uint8_t);
        switch (type) {
            case JSON_TYPE_NUMBER: {
                double d; 
                frombuf(bin, off, &d, sizeof(double));
                off += sizeof(double);
                break;
            }

            case JSON_TYPE_STRING:
            case JSON_TYPE_INTEGER:
                off += bufgetint(bin, off, &keyoff);
                break;
            case JSON_TYPE_OBJECT:
            case JSON_TYPE_ARRAY:
                frombuf(bin, off, &totalsz, sizeof(uint32_t));
                off = (off-1) + totalsz;
                break;
            default:
                return 0;
        }
    }
    return 0;
}

static json_off extract(buf *bin, json_off off, const char *path, buf *bout) {
    while (isspace(*path))
        path++;
    if (*path != '$')
        return 0;
    path++;

    if (off == 0) {
        frombuf(bin, 0, &off, sizeof(uint32_t));
    }

    for (;;) {
        uint8_t lasttype;
        frombuf(bin, off, &lasttype, sizeof(uint8_t));

        while (isspace(*path))
            path++;
        if (*path == '.') {
            path++;
            if (lasttype != JSON_TYPE_OBJECT)
                return 0;

            while (isspace(*path))
                path++;
            const char *key = path;

            // todo: quoted string names
            while (isalnum(*path))
                path++;

            off = find_property(bin, off, key, path - key);
            if (off == 0)
                return 0;

            while (isspace(*path))
                path++;

        }
        if (*path == 0) {
            if (bout)
                dump_node_from_buf(bin, off, bout, 1);
            return off;
        }
    }
}

static const char *typename(uint8_t type) {
    switch (type) {
        case JSON_TYPE_OBJECT:
            return "JSON_TYPE_OBJECT";
        case JSON_TYPE_ARRAY:
            return "JSON_TYPE_ARRAY";
        case JSON_TYPE_NUMBER:
            return "JSON_TYPE_NUMBER";
        case JSON_TYPE_INTEGER:
            return "JSON_TYPE_INTEGER";
        case JSON_TYPE_STRING:
            return "JSON_TYPE_STRING";
        case JSON_TYPE_TRUE:
            return "JSON_TYPE_TRUE";
        case JSON_TYPE_FALSE:
            return "JSON_TYPE_FALSE";
        case JSON_TYPE_NULL:
            return "JSON_TYPE_NULL";
        default:
            return "???";
    }
}

static json_off explainbuf_at_off(buf *b, json_off off, buf *out) {
    sprintf_to_buf(out, "%03d ", (int) off);
    uint8_t type;
    frombuf(b, off, &type, sizeof(uint8_t));
    hexdumpbytes(out, b->data + off, sizeof(uint8_t));
    sprintf_to_buf(out, " type %s\n", typename(type));
    off += sizeof(uint8_t);
    switch (type) {
        case JSON_TYPE_NUMBER: {
            double d;
            sprintf_to_buf(out, "%03d ", (int) off);
            frombuf(b, off, &d, sizeof(double));
            hexdumpbytes(out, (uint8_t*) &d, sizeof(double));
            sprintf_to_buf(out, " %f double\n", d);
            off += sizeof(double);
            break;
        };

        case JSON_TYPE_INTEGER:
        case JSON_TYPE_STRING: {
            size_t sz;
            u64 val;
            u64 klen;
            sprintf_to_buf(out, "%03d ", (int) off);
            sz = bufgetint(b, off, &val);
            if (type == JSON_TYPE_INTEGER) {
                hexdumpbytes(out, b->data + off, sz);
                sprintf_to_buf(out, " %"PRId64 " varint\n", val);
            }
            else {
                hexdumpbytes(out, b->data + off, sz);
                char *k = get_from_dict(b, val, &klen);
                sprintf_to_buf(out, " keyoffset %d (\"%.*s\")\n", (int) val, (int) klen, k);
            }
            off += sz;
            break;
        }
        case JSON_TYPE_OBJECT:
        case JSON_TYPE_ARRAY: {
            uint32_t totalsize;
            uint32_t nent;
            sprintf_to_buf(out, "%03d ", (int) off);
            frombuf(b, off, &totalsize, sizeof(uint32_t));
            hexdumpbytes(out, b->data + off, sizeof(uint32_t));
            sprintf_to_buf(out, " %d total size (ends at %d)\n", (int) totalsize, off + totalsize - 1);
            off += sizeof(uint32_t);
            frombuf(b, off, &nent, sizeof(uint32_t));
            sprintf_to_buf(out, "%03d ", (int) off);
            hexdumpbytes(out, b->data + off, sizeof(uint32_t));
            sprintf_to_buf(out, " %d number of entries\n",  (int) nent);
            off += sizeof(uint32_t);
            for (uint32_t i = 0; i < nent; i++) {
                off = explainbuf_at_off(b, off, out);
                if (off == 0)
                    return 0;
            }
        }
        case JSON_TYPE_TRUE:
        case JSON_TYPE_FALSE:
        case JSON_TYPE_NULL:
           break;

        default: {
            sprintf_to_buf(out, " type %d ???", type);
            return 0;
        }
    }
    return off;
}

static void explainbuf(buf *b, buf *out) {
    uint32_t dictsize;
    json_off off = 0;
    frombuf(b, 0, &dictsize, sizeof(uint32_t));
    sprintf_to_buf(out, "%03d ", (int) off);
    hexdumpbytes(out, (uint8_t*) &dictsize, sizeof(uint32_t));
    sprintf_to_buf(out, " dictionary size %d\n", (int) dictsize);
    off += sizeof(uint32_t);
    while (off < dictsize) {
        u64 val;
        size_t sz = bufgetint(b, off, &val);
        sprintf_to_buf(out, "%03d ", (int) off);
        hexdumpbytes(out, b->data + off, sz);
        sprintf_to_buf(out, " %"PRId64" key len, varint\n", val);
        off += sz;
        sprintf_to_buf(out, "%03d ", (int) off);
        hexdumpbytes(out, b->data + off, val);
        sprintf_to_buf(out, " \"%.*s\" key\n", val, b->data + off);
        off += val;
    }
    while (off < b->len) {
        off = explainbuf_at_off(b, off, out);
        if (off == 0)
            break;
    }
}

static void json_to_bcon(sqlite3_context* context, int argc, sqlite3_value **argv) {
    assert(argc == 1);
    if (sqlite3_value_type(argv[0]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "Expected text parameter for json_to_bcon", -1);
        return;
    }
    buf b = BUF_STATIC_INIT;
    json_off off = parse_json((char*) sqlite3_value_text(argv[0]), &b);
    if (off == 0) {
        sqlite3_result_error(context, "Can't parse json?", -1);
        return;
    }
    sqlite3_result_blob(context, b.data, (int) b.len, free);
}

static void bcon_to_json(sqlite3_context* context, int argc, sqlite3_value **argv) {
    if (sqlite3_value_type(argv[0]) != SQLITE_BLOB) {
        sqlite3_result_error(context, "Expected blob parameter for bcon_to_json", -1);
        return;
    }
    buf b = { .data = (uint8_t*) sqlite3_value_blob(argv[0]), .len = sqlite3_value_bytes(argv[0])};
    buf out = BUF_STATIC_INIT;
    dump_from_buf(&b, &out, 0);
    return sqlite3_result_text(context, (char*) out.data, (int) out.len, free);
}

static void bcon_extract(sqlite3_context* context, int argc, sqlite3_value **argv) {
    assert(argc == 2);
    if (sqlite3_value_type(argv[0]) != SQLITE_BLOB && sqlite3_value_type(argv[1]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "Expected blob, text parameters for bcon_extract", -1);
        return;
    }
    buf in;
    buf out = BUF_STATIC_INIT;
    in.len = in.cap = (int) sqlite3_value_bytes(argv[0]);
    in.data = (uint8_t*) sqlite3_value_blob(argv[0]);
    json_off off = extract(&in, 0, (char*) sqlite3_value_text(argv[1]), &out);
    if (off == 0) {
        sqlite3_result_error(context, "Extract error", -1);
        return;
    }

    sqlite3_result_text(context, (char*) out.data, (int) out.len, free);
}

static void bcon_explain(sqlite3_context* context, int argc, sqlite3_value **argv) {
    if (sqlite3_value_type(argv[0]) != SQLITE_BLOB && sqlite3_value_type(argv[1]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "Expected blob, parameter for bcon_extract", -1);
        return;
    }
    buf b = { .data = (uint8_t*) sqlite3_value_blob(argv[0]), .len = sqlite3_value_bytes(argv[0])};
    buf out = BUF_STATIC_INIT;
    explainbuf(&b, &out);
    sqlite3_result_text(context, (char*) out.data, (int) out.len, free);
}

static void bcon_get(sqlite3_context* context, int argc, sqlite3_value **argv) {
    if (sqlite3_value_type(argv[0]) != SQLITE_BLOB && sqlite3_value_type(argv[1]) != SQLITE_INTEGER) {
        sqlite3_result_error(context, "Expected blob, int parameter for bcon_get", -1);
        return;
    }
    buf b = { .data = (uint8_t*) sqlite3_value_blob(argv[0]), .len = sqlite3_value_bytes(argv[0])};
    buf out = BUF_STATIC_INIT;
    dump_node_from_buf(&b, sqlite3_value_int(argv[1]), &out, 1);
    // todo: numbers as numbers, not strings
    sqlite3_result_text(context, (char*) out.data, (int) out.len, free);
}

static __thread buf *hi;

static void bcon_get_path(sqlite3_context* context, int argc, sqlite3_value **argv) {
    if (sqlite3_value_type(argv[0]) != SQLITE_BLOB && sqlite3_value_type(argv[1]) != SQLITE_INTEGER && sqlite3_value_type(argv[1]) != SQLITE_TEXT) {
        sqlite3_result_error(context, "Expected blob, int parameter for bcon_get", -1);
        return;
    }
    buf b = { .data = (uint8_t*) sqlite3_value_blob(argv[0]), .len = sqlite3_value_bytes(argv[0])};
    // buf bufout = BUF_STATIC_INIT;
    // buf *out = &bufout;
    buf *out = hi;


    out->len = 0;
    json_off off = sqlite3_value_int(argv[1]);
    char *path = (char*) sqlite3_value_text(argv[2]);
    extract(&b, off, path, out);
    // printf("buf %d off %d path %s\n", (int) b.len, (int) off, path);
    // todo: numbers as numbers, not strings
    sqlite3_result_text(context, (char*) out->data, (int) out->len, NULL);
}

struct json_each_cursor {
    sqlite3_vtab_cursor base;  /* Base class - must be first */
    int argsError;
    int typeError;
    int i;
    buf data;
    buf out;
    json_type type;
    uint32_t nEnt;
    json_off off;
    json_off keyoff;
    json_off dataoff;
};

enum {
    BCON_COLUMN_KEY=0,
    BCON_COLUMN_VALUE=1,
    BCON_COLUMN_BCON=2,
    BCON_COLUMN_PATH=3
};

static int bcon_each_connect(
        sqlite3 *db,
        void *pAux,
        int argc, const char *const*argv,
        sqlite3_vtab **ppVtab,
        char **pzErr
) {
    sqlite3_vtab *vt = malloc(sizeof(sqlite3_vtab));
    int ret = sqlite3_declare_vtab(db, "CREATE TABLE bcon_each(key , value , bcon blob HIDDEN, path text HIDDEN)");
    *ppVtab = vt;
    return ret;
}

static int bcon_each_disconnect(sqlite3_vtab *pVtab){
    sqlite3_free(pVtab);
    return SQLITE_OK;
}

static int bcon_each_open(sqlite3_vtab *p, sqlite3_vtab_cursor **ppCursor) {
    struct json_each_cursor *pCur = calloc(1, sizeof(struct json_each_cursor));
    pCur->i = 0;
    pCur->nEnt = 0;
    pCur->out = (buf){ .len = 0, .cap = 1024, .data = malloc(1024) };
    hi = &pCur->out;
    *ppCursor = (sqlite3_vtab_cursor*) pCur;
    return SQLITE_OK;
}

static int bcon_each_close(sqlite3_vtab_cursor *cur) {
    // printf("close\n");
    free(cur);
    return SQLITE_OK;
}

void loadnext(struct json_each_cursor *pCur) {
    json_off start = pCur->off;


    // save key/data offsets for the current value
    if (pCur->type == JSON_TYPE_ARRAY) {
        pCur->keyoff = 0;
        pCur->dataoff = pCur->off;
    }
    else {
        pCur->keyoff = pCur->off;
        uint8_t type;
        frombuf(&pCur->data, pCur->off, &type, sizeof(uint8_t));
        pCur->off += sizeof(uint8_t);
        assert(type == JSON_TYPE_STRING);
        u64 val;
        pCur->off += bufgetint(&pCur->data, pCur->off, &val);
        pCur->dataoff = pCur->off;
    }
    // move to next value. we're positioned on the data, skip it to get to the next key (or data for array)
    // need a nextval function - we do this a bunch
    uint8_t type;
    frombuf(&pCur->data, pCur->off, &type, sizeof(uint8_t));
    pCur->off += sizeof(uint8_t);
    u64 val;
    switch (type) {
        case JSON_TYPE_NUMBER: {
            pCur->off += sizeof(double);
            break;
        }
        case JSON_TYPE_NULL:
        case JSON_TYPE_TRUE:
        case JSON_TYPE_FALSE:
            break;
        case JSON_TYPE_STRING:
        case JSON_TYPE_INTEGER:
            pCur->off += bufgetint(&pCur->data, pCur->off, &val);
            break;
        default: {
            uint32_t off;
            frombuf(&pCur->data, pCur->off, &off, sizeof(uint32_t));
            pCur->off = start + off;
        }
    }
    // fflush(NULL);
}

static int bcon_each_next(sqlite3_vtab_cursor *cur) {
    struct json_each_cursor *pCur = (struct json_each_cursor*) cur;
    pCur->i++;
    loadnext(pCur);
    return SQLITE_OK;
}

static int bcon_each_eof(sqlite3_vtab_cursor *cur){
    struct json_each_cursor *pCur = (struct json_each_cursor*) cur;
    return pCur->i >= pCur->nEnt;
}

int json_to_context_result(struct json_each_cursor *pCur, sqlite3_context *ctx, json_off off) {
    buf b = BUF_STATIC_INIT;
    dump_node_from_buf(&pCur->data, off, &b, 1);
    sqlite3_result_text(ctx, (char*) b.data, (int) b.len, free);
    return SQLITE_OK;
}

static int bcon_each_column(
        sqlite3_vtab_cursor *cur,   /* The cursor */
        sqlite3_context *ctx,       /* First argument to sqlite3_result_...() */
        int i                       /* Which column to return */
){
    struct json_each_cursor *pCur = (struct json_each_cursor*) cur;
    switch (i) {
        case BCON_COLUMN_KEY:
            if (pCur->type == JSON_TYPE_ARRAY) {
                sqlite3_result_int(ctx, 0);
                return SQLITE_OK;
            }
            else {
                sqlite3_result_int(ctx, (int) pCur->keyoff);
                // return json_to_context_result(pCur, ctx, pCur->keyoff);
            }
            break;

        case BCON_COLUMN_VALUE:
            sqlite3_result_int(ctx, (int) pCur->dataoff);
            break;

        case BCON_COLUMN_BCON:
            sqlite3_result_blob(ctx, pCur->data.data, (int) pCur->data.len, NULL);
            break;

        case BCON_COLUMN_PATH:
            sqlite3_result_text(ctx, "$.values", -1, NULL);
            break;

        default:
            abort();
    };
    return SQLITE_OK;
}

static int bcon_each_rowid(sqlite3_vtab_cursor *cur, sqlite_int64 *pRowid){
    struct json_each_cursor *pCur = (struct json_each_cursor*) cur;
    *pRowid = pCur->i;
    return SQLITE_OK;
}

static int bcon_best_index(
        sqlite3_vtab *tab,
        sqlite3_index_info *pIdxInfo) {
    int usable = 0;
    for (int i = 0; i < pIdxInfo->nConstraint; i++) {
        if (pIdxInfo->aConstraint[i].iColumn == BCON_COLUMN_BCON && pIdxInfo->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_EQ && pIdxInfo->aConstraint[i].usable) {
            pIdxInfo->aConstraintUsage[i].argvIndex = 1;
            usable++;
        } else if (pIdxInfo->aConstraint[i].iColumn == BCON_COLUMN_PATH && pIdxInfo->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_EQ && pIdxInfo->aConstraint[i].usable) {
            pIdxInfo->aConstraintUsage[i].argvIndex = 2;
            usable++;
        }
    }
    if (usable != 2)
        return SQLITE_CONSTRAINT;
    return SQLITE_OK;
}

static int bcon_filter(
        sqlite3_vtab_cursor *cur,
        int idxNum, const char *idxStr,
        int argc, sqlite3_value **argv) {
    struct json_each_cursor *pCur = (struct json_each_cursor*) cur;
    pCur->data = (buf){ .data = (uint8_t*) sqlite3_value_blob(argv[0]), .len = sqlite3_value_bytes(argv[0])};
    json_off off = extract(&pCur->data, 0, (char*) sqlite3_value_text(argv[1]), NULL);
    pCur->off = off;
    pCur->i = 0;

    uint8_t type;
    frombuf(&pCur->data, off, &type, sizeof(uint8_t));
    uint32_t totalsize;
    switch (type) {
        case JSON_TYPE_OBJECT:
        case JSON_TYPE_ARRAY:
            off += sizeof(uint8_t);
            frombuf(&pCur->data, off, &totalsize, sizeof(uint32_t));
            off += sizeof(uint32_t);
            frombuf(&pCur->data, off, &pCur->nEnt, sizeof(uint32_t));
            off += sizeof(uint32_t);
            pCur->off = off;
            pCur->type = type;
            loadnext(pCur);
            break;
        default:
            printf("typeerror?\n");
            pCur->typeError = 1;
    }
    return SQLITE_OK;
}

static sqlite3_module bconeach_module = {
        .xConnect = bcon_each_connect,
        .xDisconnect = bcon_each_disconnect,
        .xFilter = bcon_filter,
        .xBestIndex = bcon_best_index,
        .xOpen = bcon_each_open,
        .xClose = bcon_each_close,
        .xNext = bcon_each_next,
        .xEof = bcon_each_eof,
        .xColumn = bcon_each_column,
        .xRowid = bcon_each_rowid
};

void register_bcon_functions(sqlite3 *db) {
    sqlite3_create_function(db, "json_to_bcon", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, json_to_bcon, NULL, NULL);
    sqlite3_create_function(db, "bcon_extract", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, bcon_extract, NULL, NULL);
    sqlite3_create_function(db, "bcon_explain", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, bcon_explain, NULL, NULL);
    sqlite3_create_function(db, "bcon_get", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, bcon_get, NULL, NULL);
    sqlite3_create_function(db, "bcon_get", 3, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, bcon_get_path, NULL, NULL);
    sqlite3_create_function(db, "bcon_to_json", 1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, bcon_to_json, NULL, NULL);
    sqlite3_create_module(db, "bcon_each", &bconeach_module, NULL);
}
