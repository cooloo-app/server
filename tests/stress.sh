#!/bin/sh
# stress.sh - handoff §6: 100 concurrent FOLLOW + high-frequency SEND for
# one minute against loopback coolood. Checks liveness, delivery, and RSS
# growth (crude leak indicator; ASan/UBSan nettest covers correctness).
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cooloo-stress.XXXXXX")
PORT=$((23000 + RANDOM % 20000))
export COOLOO_HOME=$TMP

NFOLLOW=100
DURATION=60

mkdir -p "$TMP/rooms"
"$ROOT/coolood" -b 127.0.0.1 -p "$PORT" >"$TMP/daemon.log" 2>&1 &
DPID=$!
trap 'kill $DPID 2>/dev/null; wait $DPID 2>/dev/null;
      kill $(jobs -p) 2>/dev/null; rm -rf "$TMP"' EXIT

NC="$ROOT/tests/net_client $PORT"
KEY1=0101010101010101010101010101010101010101010101010101010101010101
KEY2=0202020202020202020202020202020202020202020202020202020202020202

sleep 0.5
$NC "$KEY1" HELLO alice >/dev/null 2>&1
$NC "$KEY2" HELLO bob >/dev/null 2>&1

echo "stress: starting $NFOLLOW followers"
i=0
while [ $i -lt $NFOLLOW ]; do
    $NC "$KEY1" FOLLOW stress 70 >"$TMP/f$i.out" 2>&1 &
    i=$((i + 1))
done
sleep 2

RSS0=$(ps -o rss= -p $DPID | tr -d ' ')
echo "stress: sender loop for ${DURATION}s (daemon rss start ${RSS0}KB)"
END=$(($(date +%s) + DURATION))
i=0
while [ "$(date +%s)" -lt $END ]; do
    i=$((i + 1))
    $NC "$KEY2" SEND stress "stress msg $i" >/dev/null 2>&1 || {
        echo "stress: FAIL - send $i failed"; exit 1; }
done
$NC "$KEY2" SEND stress "stress-final-marker" >/dev/null 2>&1
sleep 3

kill -0 $DPID 2>/dev/null || { echo "stress: FAIL - daemon died"; exit 1; }
RSS1=$(ps -o rss= -p $DPID | tr -d ' ')
echo "stress: sent $i messages in ${DURATION}s; daemon rss ${RSS0}KB -> ${RSS1}KB"

# every follower must have seen the final marker
missing=0
n=0
while [ $n -lt $NFOLLOW ]; do
    grep -q 'stress-final-marker' "$TMP/f$n.out" || missing=$((missing + 1))
    n=$((n + 1))
done
echo "stress: followers missing final marker: $missing/$NFOLLOW"

# daemon log must not show crash-level errors
grep -v 'closed: idle timeout' "$TMP/daemon.log" | \
    grep -i 'error\|abort\|segv' && { echo "stress: FAIL - daemon log errors"; exit 1; }

if [ $missing -gt 0 ]; then
    echo "stress: FAIL"
    exit 1
fi
echo "stress: PASS ($i msgs, $NFOLLOW followers, rss delta $((RSS1 - RSS0))KB)"
