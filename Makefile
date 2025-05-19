CC = gcc
CFLAGS = -Wall -pthread
LDFLAGS = -lssl -lcrypto

all: p2p_advanced

p2p_advanced: p2p_advanced.c
	$(CC) $(CFLAGS) -o p2p_advanced p2p_advanced.c $(LDFLAGS)

clean:
	rm -f p2p_advanced

