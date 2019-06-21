OBJS=testrunner.o #plhash.o pool.o
BINARY=testrunner

LIBS=
CFLAGS=-Wall -g -Wextra -Wno-unused-parameter
$(BINARY): $(OBJS)
	gcc -o $(BINARY) $(OBJS) $(LDFLAGS) $(LIBS)

%.o: %.c
	gcc -c $(CFLAGS) $< -o $@

clean:  
	rm -f $(BINARY) $(OBJS)
