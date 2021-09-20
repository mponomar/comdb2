OBJS=testrunner.o #plhash.o pool.o
BINARY=testrunner

CFLAGS=-pthread -std=c99 -pthread -Wall -g -U__STRICT_ANSI__ -Wextra -Wno-unused-parameter -I/opt/bb/include
LDFLAGS=-pthread

$(BINARY): $(OBJS)
	gcc -o $(BINARY) $(OBJS) $(LDFLAGS) /opt/bb/lib/libcdb2api.a -lprotobuf-c -lssl -lcrypto -lunwind -luuid

%.o: %.c
	gcc -c $(CFLAGS) $< -o $@

clean:  
	rm -f $(BINARY) $(OBJS)
