#include <stdlib.h>
#include <stdio.h> 
#include <stddef.h>
#include <inttypes.h>
#include <string.h>
#include <assert.h>

#include "completer_buf.h"

static const int minbuf = 10;
static const int initbuf = 1024;

struct completer_buf {
    char *buf;
    int buflen;                        /* max read into buffer */
    int allocated;                     /* max allocated size   */
    int offset;                        /* offset of first unused byte */
    completer_buf_feedfunc feed;       /* callback to get more data */
    completer_buf_freefunc freefunc;   /* call to free data returned by feed() */
    void *usrptr;                      /* passed to feed when it needs to be called */
};

/*                        advance
 *                  +----------------+
 *                  |                |
 *        feed      V          get   |
 *  source -> [completer_buf]   ->  dest -+
 *
 *        ------------------------------------ offset 
 *        V
 *  [xxxxxxyyyyyyyyyyyy                ] xxx - consumed data,   yyy - unconsumed data
 *  ^----------------------------------^------ allocated
 *  ^-----------------^----------------------- buflen
 *
 *  completerbuf__get will call feed if needed and returned a pointer to unconsumed data.  If "incomplete" is true
 *  in a call, it'll feed some more.
 */

int completerbuf_init(struct completer_buf *b, completer_buf_feedfunc feedfunc, completer_buf_freefunc freefunc, void *usrptr) {
    b->buf = malloc(initbuf);
    if (b->buf == NULL)
        return -1;
    b->allocated = initbuf;
    b->buflen = 0;
    b->offset = 0;
    b->feed = feedfunc;
    b->freefunc = freefunc;
    b->usrptr = usrptr;
    return 0;
}

int completerbuf_get(struct completer_buf *b, char **out, int incomplete) {
    int available = b->buflen - b->offset;

    // if we have something, return it
    if (available && !incomplete) {
        // reset if there's just a few bytes left
        if ((b->buflen - b->offset) < minbuf) {
            memmove(b->buf, b->buf + b->offset, b->buflen - b->offset);
            b->buflen = b->buflen - b->offset;
            b->offset = 0;
        }
        *out = b->buf + b->offset;
        return available;
    }
    // otherwise, try to get some more
    else {
        char *more;
        // call the callback we were given to get more data
        int bytes = b->feed(b->usrptr, &more);
        // error? pass it back - we assume nothing is allocated on error
        if (bytes <= 0)
            return bytes;
        if (bytes < (b->allocated - b->buflen)) {
            memcpy(b->buf + b->buflen, more, bytes);
            b->buflen += bytes;
        }
        else {
            int needmore = bytes - (b->allocated - b->buflen);
            b->buf = realloc(b->buf, b->allocated + needmore);
            memcpy(b->buf + b->buflen, more, bytes);
            b->allocated = b->allocated + needmore;
            b->buflen = b->allocated;
        }
        *out = b->buf + b->offset;
        if (b->freefunc)
            b->freefunc(more);
        return bytes;
    }
}

void completerbuf_advance(struct completer_buf *b, int bytes) {
    assert(b->offset + bytes <= b->buflen);
    b->offset += bytes;
    // reset if we consumed everything
    if (b->offset == b->buflen) {
        b->buflen = 0;
        b->offset =0;
    }
}

void completerbuf_free(struct completer_buf *b) {
    free(b->buf);
}
