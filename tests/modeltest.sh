#!/bin/sh
# Isolated file-model tests; never use the user's live data directory.
set -eu
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
"${MODEL_TEST_BIN:-./tests/model_test}" "$TMP/data"
python3 - "$TMP/data" <<'PY'
import json
import pathlib
import sys
root = pathlib.Path(sys.argv[1]) / 'rooms'
metas = [json.loads(p.read_text()) for p in root.glob('*/.meta')]
assert len(metas) == 5
assert all(len(v['id']) == 64 for v in metas)
assert len({v['id'] for v in metas}) == 5
child = root / ('c' * 64) / 'messages.jsonl'
record = json.loads(child.read_text())
assert record['from'] == 'b' * 64
assert record['to'] == 'c' * 64  # resolved destination, not alias
assert record['content'] == 'hello\n"世界"\t\\'
assert list(record) == ['ts', 'from', 'to', 'content']
assert isinstance(record['ts'], int)
assert child.read_bytes().count(b'\n') == 1
assert not (root / ('a' * 64) / 'messages.jsonl').exists()  # no broadcast
room = next(v['id'] for v in metas if v['type'] == 'room')
lines = (root / room / 'messages.jsonl').read_bytes().splitlines()
assert lines[-1] == b'{incomplete'  # subsequent SEND refused, no appended debris
messages = [json.loads(line) for line in lines[:-1]]
assert len(messages) == 80
assert sum(v['content'] == 'child' for v in messages) == 40
assert sum(v['content'] == 'parent' for v in messages) == 40
print('model: independent JSON parser verified metadata, exact content and concurrent records')
PY
