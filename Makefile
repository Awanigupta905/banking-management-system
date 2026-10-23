CC     = gcc
CFLAGS = -Wall -Wextra -Wno-unused-parameter -Wno-unused-result

all: server client stress_test

# server.c includes database.c, so it must be rebuilt when that changes
server: server.c database.c
	$(CC) $(CFLAGS) server.c -o server

client: client.c
	$(CC) $(CFLAGS) client.c -o client

stress_test: stress_test.c
	$(CC) $(CFLAGS) stress_test.c -o stress_test -lm

# delete all data files (fresh start with the sample users)
reset:
	rm -rf data bank.log

clean: reset
	rm -f server client stress_test

.PHONY: all reset clean
