# cooloo server - v0 SSH gate (cooloo) + v1 native daemon (coolood, T2)
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99
PREFIX  ?= /usr/local

# v0 gate sources, listed explicitly so v1 files (noise_xx.c, daemon.c)
# never get linked into the gate binary.
GATE_SRC := src/main.c src/cmd_gate.c src/cmd_send.c src/cmd_read.c \
            src/cmd_follow.c src/cmd_misc.c src/store.c src/escape.c src/util.c

# v1 noise stack: single-file Noise_XX + vendored Monocypher. Shared with
# the client repo byte-identically (tests/vendor_sync.sh enforces).
NOISE_SRC := src/noise_xx.c src/vendor/monocypher.c

BIN     := cooloo

all: $(BIN)

$(BIN): $(GATE_SRC) $(wildcard src/*.h)
	$(CC) $(CFLAGS) -o $@ $(GATE_SRC)

tests/noise_kat: tests/noise_kat.c $(NOISE_SRC) src/noise_xx.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/noise_kat.c $(NOISE_SRC)

# cacophony known-answer vectors - the crypto gate for v1 (must pass
# before coolood or the CLI ship anything).
vectors: tests/noise_kat
	python3 tests/noise_vectors.py

vendor-sync:
	sh tests/vendor_sync.sh

test: $(BIN) vectors
	sh tests/selftest.sh

install: $(BIN)
	install -m 0755 $(BIN) $(PREFIX)/bin/$(BIN)

clean:
	rm -f $(BIN) tests/noise_kat

.PHONY: all vectors vendor-sync test install clean
