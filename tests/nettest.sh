#!/bin/sh
# nettest.sh - T2/T4 loopback integration for coolood (handoff §6).
# Starts coolood on 127.0.0.1 with a throwaway COOLOO_HOME, drives it with
# tests/net_client, and checks the v0<->native interop both directions.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cooloo-nettest.XXXXXX")
PORT=$((20000 + RANDOM % 20000))
export COOLOO_HOME=$TMP

mkdir -p "$TMP/rooms"
"$ROOT/coolood" -b 127.0.0.1 -p "$PORT" >"$TMP/daemon.log" 2>&1 &
DPID=$!
trap 'kill $DPID 2>/dev/null; wait $DPID 2>/dev/null; rm -rf "$TMP"' EXIT

NC="$ROOT/tests/net_client $PORT"
KEY1=0101010101010101010101010101010101010101010101010101010101010101
KEY2=0202020202020202020202020202020202020202020202020202020202020202

pass=0
fail=0
check() { # check <name> <file> <pattern>
    if grep -q "$3" "$2" 2>/dev/null; then
        echo "ok   - $1"
        pass=$((pass + 1))
    else
        echo "FAIL - $1 (want: $3)"
        sed 's/^/       got: /' "$2" 2>/dev/null | head -5
        fail=$((fail + 1))
    fi
}
run() { # run <outfile> <cmd...>
    out=$1
    shift
    "$@" >"$out" 2>&1
}

sleep 0.5

echo "== greeting / D25 registration gate"
run "$TMP/o1" "$ROOT/tests/net_client" "$PORT" "$KEY1" WHOAMI
check "greeting nick=- for new key" "$TMP/o1" '^COOLOO 1 -$'
check "WHOAMI -> NICK -"           "$TMP/o1" '^NICK -$'
run "$TMP/o2" "$ROOT/tests/net_client" "$PORT" "$KEY1" SEND general hello
check "unregistered SEND -> ERR not-registered" "$TMP/o2" '^ERR not-registered'
run "$TMP/o3" "$ROOT/tests/net_client" "$PORT" "$KEY1" READ general 0
check "unregistered READ -> ERR not-registered" "$TMP/o3" '^ERR not-registered'

echo "== HELLO registration"
run "$TMP/o4" "$ROOT/tests/net_client" "$PORT" "$KEY1" HELLO alice
check "HELLO alice -> NICK alice" "$TMP/o4" '^NICK alice$'
run "$TMP/o5" "$ROOT/tests/net_client" "$PORT" "$KEY1" WHOAMI
check "reconnect auto-bound greeting" "$TMP/o5" '^COOLOO 1 alice$'
run "$TMP/o6" "$ROOT/tests/net_client" "$PORT" "$KEY2" HELLO alice
check "second key same nick -> ERR nick-taken" "$TMP/o6" '^ERR nick-taken'
run "$TMP/o7" "$ROOT/tests/net_client" "$PORT" "$KEY1" RAWLINE "HELLO bob"
check "HELLO on bound conn -> ERR already-bound" "$TMP/o7" '^ERR already-bound'
run "$TMP/o8" "$ROOT/tests/net_client" "$PORT" "$KEY2" HELLO BadNick
check "bad nick -> ERR nick-invalid" "$TMP/o8" '^ERR nick-invalid'
run "$TMP/o9" "$ROOT/tests/net_client" "$PORT" "$KEY2" HELLO bob
check "bob registers" "$TMP/o9" '^NICK bob$'

echo "== send / read / rooms"
run "$TMP/o10" "$ROOT/tests/net_client" "$PORT" "$KEY1" SEND general "hello native"
check "SEND -> OK <offset>" "$TMP/o10" '^OK [0-9]'
run "$TMP/o11" "$ROOT/tests/net_client" "$PORT" "$KEY1" SEND general 'back\slash'
check "SEND backslash -> OK" "$TMP/o11" '^OK [0-9]'
run "$TMP/o12" "$ROOT/tests/net_client" "$PORT" "$KEY1" READ general 0
check "READ shows MSG" "$TMP/o12" '^MSG [0-9]* [0-9]* alice '
check "READ message text" "$TMP/o12" 'hello native'
check "READ escapes backslash" "$TMP/o12" 'back\\\\slash'
check "READ ends with END" "$TMP/o12" '^END$'
run "$TMP/o13" "$ROOT/tests/net_client" "$PORT" "$KEY1" READ general -1
check "READ -1 tail works" "$TMP/o13" 'back\\\\slash'
run "$TMP/o14" "$ROOT/tests/net_client" "$PORT" "$KEY1" ROOMS
check "ROOMS lists general" "$TMP/o14" '^ROOM general$'

echo "== ping / errors"
run "$TMP/o15" "$ROOT/tests/net_client" "$PORT" "$KEY1" PING
check "PING -> PONG" "$TMP/o15" '^PONG$'
run "$TMP/o16" "$ROOT/tests/net_client" "$PORT" "$KEY1" RAWLINE FOOBAR
check "unknown command" "$TMP/o16" '^ERR unknown-command'
run "$TMP/o17" "$ROOT/tests/net_client" "$PORT" "$KEY1" READ BadRoom 0
check "bad room" "$TMP/o17" '^ERR room-invalid'
run "$TMP/o18" "$ROOT/tests/net_client" "$PORT" "$KEY1" READ general abc
check "bad offset" "$TMP/o18" '^ERR bad-request'
run "$TMP/o19" "$ROOT/tests/net_client" "$PORT" "$KEY1" SENDBIG general 9000
check "oversize -> ERR message-too-large" "$TMP/o19" '^ERR message-too-large'

echo "== v0 -> native interop (D17)"
USER=v0tester "$ROOT/cooloo" send general "from v0 gate" >"$TMP/o20" 2>&1
check "v0 send OK" "$TMP/o20" '^OK [0-9]'
run "$TMP/o21" "$ROOT/tests/net_client" "$PORT" "$KEY1" READ general -1
check "native READ sees v0 message" "$TMP/o21" 'from v0 gate'

echo "== native follow <- v0 write"
"$ROOT/tests/net_client" "$PORT" "$KEY2" FOLLOW general 3 >"$TMP/follow.out" 2>&1 &
FPID=$!
sleep 1
USER=v0tester "$ROOT/cooloo" send general "v0 to native push" >/dev/null 2>&1
wait $FPID
check "FOLLOW received v0 write within window" "$TMP/follow.out" 'v0 to native push'

echo "== fingerprint (D26)"
"$ROOT/cooloo" fingerprint >"$TMP/o22" 2>&1
check "gate fingerprint prints 64 hex" "$TMP/o22" '^[0-9a-f]\{64\}$'
if [ -f "$TMP/server_fingerprint" ] && \
   cmp -s "$TMP/o22" "$TMP/server_fingerprint"; then
    echo "ok   - gate fingerprint matches coolood file"
    pass=$((pass + 1))
else
    echo "FAIL - gate fingerprint matches coolood file"
    fail=$((fail + 1))
fi

echo
echo "nettest: passed $pass, failed $fail"
[ "$fail" -eq 0 ]
