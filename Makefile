# cooloo v0 - single small binary, no third-party deps
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99
PREFIX  ?= /usr/local

SRC     := $(wildcard src/*.c)
HDR     := $(wildcard src/*.h)
BIN     := cooloo

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: $(BIN)
	sh tests/selftest.sh

install: $(BIN)
	install -m 0755 $(BIN) $(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all test install clean
