CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

all: krgtool

krgtool: krgtool.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f krgtool