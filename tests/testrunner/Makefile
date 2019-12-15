OBJS=testrunner.o #plhash.o pool.o
BINARY=testrunner

LIBS=/opt/bb/lib/libcdb2api.a -lprotobuf-c -lssl -lcrypto -pthread -lunwind
CFLAGS=-pthread -Wall -g -Wextra -Wno-unused-parameter -I/opt/bb/include

$(BINARY): $(OBJS)
	gcc -o $(BINARY) $(OBJS) $(LDFLAGS) $(LIBS)

%.o: %.c
	gcc -c $(CFLAGS) $< -o $@

clean:  
	rm -f $(BINARY) $(OBJS)
