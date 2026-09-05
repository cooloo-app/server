#!/bin/sh
# tests/selftest.sh - cooloo v0 local self-test (no sshd needed)
#
# Simulates two gate identities (alice/bob) via SSH_ORIGINAL_COMMAND.
# Covers handoff section 6 checkpoints:
#   [x] send argv + stdin modes, OK <offset>
#   [x] read from 0, offsets increase; mid-offset incremental read
#   [x] read -1 returns only the last line
#   [x] multi-line message stored as one escaped line (grep-able)
#   [x] invalid rooms (../etc, ABC, overlong) all ERR
#   [x] message > 4096 escaped bytes ERR; exactly 4096 OK
#   [x] follow: background, new message visible within 1s
#   [x] whoami prints the gate nick
#   [x] rooms lists general
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/cooloo"
[ -x "$BIN" ] || { echo "binary not found: $BIN (run make first)"; exit 1; }

export COOLOO_HOME="$(mktemp -d /tmp/cooloo-test.XXXXXX)"
trap 'rm -rf "$COOLOO_HOME"' EXIT
mkdir -p "$COOLOO_HOME/rooms"
FOLLOW_OUT="$COOLOO_HOME/follow.out"

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "ok   - $1"; }
bad() { FAIL=$((FAIL+1)); echo "FAIL - $1"; }

# gate <SSH_ORIGINAL_COMMAND> <nick>  -- run the gate like sshd would
gate() { SSH_ORIGINAL_COMMAND="$1" "$BIN" __gate "$2"; }

echo "== send: argv mode and stdin mode"
OUT1="$(gate 'send general hello world' alice)"
case "$OUT1" in OK\ *) ok "send argv mode: $OUT1" ;; *) bad "send argv mode: $OUT1" ;; esac
OFF1="${OUT1#OK }"

OUT2="$(printf 'multi\nline\n' | gate 'send general' bob)"
case "$OUT2" in OK\ *) ok "send stdin mode: $OUT2" ;; *) bad "send stdin mode: $OUT2" ;; esac
OFF2="${OUT2#OK }"

[ "$OFF2" -gt "$OFF1" ] && ok "offsets increase ($OFF1 -> $OFF2)" || bad "offsets not increasing"

echo "== read: from 0, incremental, tail"
R0="$(gate 'read general 0' alice)"
[ "$(printf '%s\n' "$R0" | wc -l)" -eq 2 ] && ok "read 0 returns 2 lines" || bad "read 0: $R0"
printf '%s\n' "$R0" | grep -q 'alice hello world$' || bad "read 0 missing msg1"
printf '%s\n' "$R0" | grep -q 'bob multi\\nline$'   || bad "read 0 missing msg2 (escaped)"

RM="$(gate "read general $OFF1" alice)"
[ "$(printf '%s\n' "$RM" | wc -l)" -eq 1 ] && ok "mid-offset read = 1 line" || bad "mid-offset: $RM"
printf '%s\n' "$RM" | grep -q "^$OFF2 " && ok "mid-offset end_offset = $OFF2" || bad "mid-offset offset: $RM"

RT="$(gate 'read general -1' alice)"
[ "$(printf '%s\n' "$RT" | wc -l)" -eq 1 ] && printf '%s\n' "$RT" | grep -q 'bob ' \
  && ok "read -1 returns last line only" || bad "read -1: $RT"

echo "== storage: one line per message, escaped, grep-able"
LINES="$(wc -l < "$COOLOO_HOME/rooms/general.log")"
[ "$LINES" -eq 2 ] && ok "log has 2 lines" || bad "log lines: $LINES"
grep -qF 'multi\nline' "$COOLOO_HOME/rooms/general.log" \
  && ok "multi-line stored escaped on one line" || bad "escape not found in log"

echo "== validation: bad rooms, oversize message"
for ROOM in '../etc' 'ABC' 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'; do
  ERR="$(gate "send $ROOM x" alice 2>&1)"; RC=$?
  [ $RC -ne 0 ] && case "$ERR" in ERR*) ok "room '$ROOM' rejected" ;; *) bad "room '$ROOM': no ERR prefix" ;; esac \
  || bad "room '$ROOM' accepted"
done

BIG_OK="$(head -c 4096 /dev/zero | tr '\0' x | gate 'send big' alice)"; RC=$?
[ $RC -eq 0 ] && case "$BIG_OK" in OK\ *) ok "4096-byte message accepted" ;; *) bad "4096 msg: $BIG_OK" ;; esac \
|| bad "4096-byte message rejected: $BIG_OK"
BIG_NO="$(head -c 4097 /dev/zero | tr '\0' x | gate 'send big' alice 2>&1)"; RC=$?
[ $RC -ne 0 ] && case "$BIG_NO" in ERR*) ok "4097-byte message rejected" ;; *) bad "4097 msg: no ERR" ;; esac \
|| bad "4097-byte message accepted"

echo "== follow: new message visible within 1s"
: > "$FOLLOW_OUT"
SSH_ORIGINAL_COMMAND='follow general' "$BIN" __gate fbot > "$FOLLOW_OUT" 2>&1 &
FPID=$!
sleep 0.3
gate 'send general follow me' alice > /dev/null
I=0; SEEN=""
while [ $I -lt 10 ]; do
  grep -q 'follow me' "$FOLLOW_OUT" && { SEEN=1; break; }
  I=$((I+1)); sleep 0.1
done
kill "$FPID" 2>/dev/null; wait "$FPID" 2>/dev/null
[ -n "$SEEN" ] && ok "follow saw new message (<1s)" || bad "follow output: $(cat "$FOLLOW_OUT")"

echo "== whoami / rooms / welcome / unknown command"
[ "$(gate 'whoami' alice)" = "alice" ] && ok "whoami = gate nick" || bad "whoami"
[ "$(gate 'rooms' alice)" = "$(printf 'big\ngeneral')" ] \
  && ok "rooms lists rooms sorted" || bad "rooms: $(gate 'rooms' alice)"
"$BIN" __gate alice | grep -q '^Welcome to cooloo, alice!' \
  && ok "empty command prints welcome" || bad "welcome"
ERR="$(SSH_ORIGINAL_COMMAND='cat /etc/passwd' "$BIN" __gate alice 2>&1)"; RC=$?
[ $RC -ne 0 ] && case "$ERR" in ERR*) ok "unknown command rejected" ;; *) bad "unknown cmd: $ERR" ;; esac \
|| bad "unknown command accepted"

echo
echo "passed: $PASS  failed: $FAIL"
[ "$FAIL" -eq 0 ]
