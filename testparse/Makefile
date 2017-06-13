SRCHOME=..
include ../main.mk
include ../libs.mk

OBJS=parse.o test.o lex.o

CFLAGS+=-I$(SRCHOME)/util

LIBS+=-L$(SRCHOME)/util -lutil -L$(SRCHOME)/bb -lbb -L$(SRCHOME)/dlmalloc -ldlmalloc -lpthread

parse: $(OBJS)
	cc -o parse $(OBJS) $(LIBS)

parse.c: lemon

parse.c: parse.y
	./lemon parse.y

lemon: ../sqlite/lemon.o
	cc -o lemon $<

clean:
	rm -f parse $(OBJS) lemon lemon.o parse.c parse.h parse.out
