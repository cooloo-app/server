# cooloo server - v0 SSH gate (cooloo) + v1 native daemon (coolood)
CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99
PREFIX  ?= /usr/local

# v0 gate sources, listed explicitly so v1 files never get linked into it.
GATE_SRC := src/main.c src/model.c src/util.c

# v1 noise stack: single-file Noise_XX + vendored Monocypher. Shared with
# the client repo byte-identically (tests/vendor_sync.sh enforces).
NOISE_SRC := src/noise_xx.c src/vendor/monocypher.c

# coolood: line protocol + poll loop; write path reuses store.o (same code
# as the v0 gate), reads stream through store's emit hook.
DAEMON_SRC := src/daemon.c src/proto.c src/model.c src/util.c \
              $(NOISE_SRC)

all: cooloo coolood

cooloo: $(GATE_SRC) $(wildcard src/*.h) $(wildcard src/*.inc)
	$(CC) $(CFLAGS) -o $@ $(GATE_SRC)

coolood: $(DAEMON_SRC) $(wildcard src/*.h) $(wildcard src/*.inc)
	$(CC) $(CFLAGS) -o $@ $(DAEMON_SRC)

tests/noise_kat: tests/noise_kat.c $(NOISE_SRC) src/noise_xx.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/noise_kat.c $(NOISE_SRC)

tests/net_client: tests/net_client.c $(NOISE_SRC) src/noise_xx.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/net_client.c $(NOISE_SRC)

tests/model_test: tests/model_test.c src/model.c src/model.h $(wildcard src/*.inc)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/model_test.c src/model.c

modeltest: tests/model_test
	sh tests/modeltest.sh

# cacophony known-answer vectors - the crypto gate for v1 (must pass
# before coolood or the CLI ship anything).
vectors: tests/noise_kat
	python3 tests/noise_vectors.py

vendor-sync:
	sh tests/vendor_sync.sh

# loopback integration: coolood on 127.0.0.1 + net_client + v0 interop
nettest: coolood cooloo tests/net_client
	python3 tests/protocol2_test.py

test: cooloo coolood vectors nettest modeltest
	python3 tests/private_room_test.py
	python3 tests/reset_relations_test.py

install: cooloo coolood
	install -m 0755 cooloo $(PREFIX)/bin/cooloo
	install -m 0755 coolood $(PREFIX)/bin/coolood

clean:
	rm -f cooloo coolood tests/noise_kat tests/net_client tests/model_test

.PHONY: all vectors vendor-sync nettest modeltest test install clean
