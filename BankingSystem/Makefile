CC      = gcc
CFLAGS  = -Wall -Wextra -O2

SERVER_OBJS = server.o db.o net.o customer.o employee.o manager.o admin.o

all: server client

server: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_OBJS) -lm

client: client.o
	$(CC) $(CFLAGS) -o $@ client.o

%.o: %.c common.h db.h net.h roles.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f *.o server client

# Wipes all data (a fresh admin is created on next server start)
reset:
	rm -rf data

# Concurrency and crash-recovery demonstration (uses a temporary copy of the data)
test: all
	tests/run_tests.sh

.PHONY: all clean reset test zip
