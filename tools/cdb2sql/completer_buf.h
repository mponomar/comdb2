#ifndef BUF_H
#define BUF_H

typedef int (*completer_buf_feedfunc)(void *usrptr, char **buf);
typedef void(*completer_buf_freefunc)(void *);

struct completer_buf;
int completerbuf_init(struct completer_buf *b, completer_buf_feedfunc feedfunc, completer_buf_freefunc freefunc, void *usrptr);
int completerbuf_get(struct completer_buf *b, char **out, int incomplete);
void completerbuf_advance(struct completer_buf *b, int bytes);

#endif

