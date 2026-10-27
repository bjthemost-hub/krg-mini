CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

all: krgtool dsmread

krgtool: krgtool.c
	$(CC) $(CFLAGS) -o $@ $<

dsmread: dsmread.c
	$(CC) $(CFLAGS) -pthread -o $@ $<

clean:
	rm -f krgtool dsmread
