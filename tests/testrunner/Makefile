OBJS=testrunner.o #plhash.o pool.o
BINARY=testrunner

LIBS=-L/opt/bb/lib -Wl,-rpath=/opt/bb/lib -lcdb2api -lprotobuf-c -lssl
CFLAGS=-Wall -g -Wextra -Wno-unused-parameter -I/opt/bb/include
$(BINARY): $(OBJS)
	gcc -o $(BINARY) $(OBJS) $(LDFLAGS) $(LIBS)

%.o: %.c
	gcc -c $(CFLAGS) $< -o $@

clean:  
	rm -f $(BINARY) $(OBJS)
