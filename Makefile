CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
PREFIX ?= /usr/local

.PHONY: all clean install uninstall

all: drivemap

drivemap: drivemap.c
	$(CC) $(CFLAGS) -o $@ $<

install: drivemap
	install -d "$(PREFIX)/bin"
	install -m 0755 drivemap "$(PREFIX)/bin/drivemap"

uninstall:
	rm -f "$(PREFIX)/bin/drivemap"

clean:
	rm -f drivemap
