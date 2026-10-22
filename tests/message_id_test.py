"""Isolated UUID, cross-process idempotency, conflict and migration regression."""
import concurrent.futures
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import uuid

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('migration', root / 'tools/migrate_message_ids.py')
migration = importlib.util.module_from_spec(spec)
spec.loader.exec_module(migration)
with tempfile.TemporaryDirectory() as tmp:
    home = Path(tmp)
    env = dict(os.environ, COOLOO_HOME=tmp)
    a, b = 'a' * 64, 'b' * 64
    def call(actor, *args, text=None, ok=True):
        p = subprocess.run([str(root / 'cooloo'), '--as', actor, *args], env=env,
                           input=text, text=True, capture_output=True, timeout=20)
        assert (p.returncode == 0) == ok, (args, p.stdout, p.stderr)
        return p.stdout if ok else p.stderr
    call(a, 'register', 'a')
    call(b, 'register', 'b')
    room = call(a, 'create', 'room').split()[1]
    mid = uuid.uuid4().hex
    def send(_):
        return call(a, 'send', room, mid, text='exact\n"世界"\t\\')
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        answers = list(pool.map(send, range(24)))
    assert len(set(answers)) == 1
    log = home / 'rooms' / room / 'messages.jsonl'
    assert len(log.read_bytes().splitlines()) == 1
    assert 'id-conflict' in call(a, 'send', room, mid, text='other', ok=False)
    assert 'id-conflict' in call(b, 'send', room, mid, text='exact\n"世界"\t\\', ok=False)
    assert 'id-conflict' in call(a, 'send', b, mid, text='exact\n"世界"\t\\', ok=False)
    for bad in ['0'*32, mid.upper(), str(uuid.uuid4()), 'x'*32]:
        call(a, 'send', room, bad, text='bad', ok=False)
    # Independent IDs, identical content in a single integer second must reject.
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        def duplicate(_):
            return subprocess.run([str(root/'cooloo'),'--as',a,'send',room,uuid.uuid4().hex],
                                  env=env,input='same second',text=True,capture_output=True)
        attempts=list(pool.map(duplicate,range(24)))
    assert any(p.returncode and 'duplicate-content' in p.stderr for p in attempts)
    # Migration preserves duplicate legacy records, adjusts ACL journal boundary,
    # has stable staged UUIDs/epochs on repeat and preserves unrelated files.
    old = dict(ts=1, **{'from':a,'to':room}, content='legacy\n"世界"\t\\')
    legacy = (json.dumps(old,ensure_ascii=False)+'\n').encode()
    log.write_bytes(legacy * 2)
    acl = log.parent / '.access'
    acl.write_text(f'COOLOO-ACCESS-1 0 {len(legacy)} 1\n{a} 0 \n')
    unrelated=home/'unrelated';unrelated.write_bytes(b'untouched')
    assert migration.migrate(home)['added'] == 2
    result=migration.migrate(home,True)
    assert result['records']==2 and result['added']==2
    first=log.read_bytes();epoch=(log.parent/'.generation').read_bytes()
    records=[json.loads(line) for line in first.splitlines()]
    assert records[0]['id']!=records[1]['id']
    for record in records:
        assert {k:v for k,v in record.items() if k!='id'}==old
    assert int(acl.read_text().splitlines()[0].split()[2])==len(first.splitlines(keepends=True)[0])
    migration.migrate(home,True)
    assert log.read_bytes()==first and (log.parent/'.generation').read_bytes()==epoch
    assert unrelated.read_bytes()==b'untouched'
    # Re-importing the same UUID after migration/restart returns the original ACK.
    reply=call(a,'send',room,records[0]['id'],text=old['content'])
    assert int(reply.split()[1])==len(first.splitlines(keepends=True)[0])
    assert log.read_bytes()==first
    # Corrupt tail in any stream is fail-closed for global UUID conflict lookup.
    log.write_bytes(first+b'{broken')
    call(a,'send',b,uuid.uuid4().hex,text='fail closed',ok=False)
print('UUID: concurrency, restart, conflict, D1, invalid IDs, migration/ACL/epoch replay passed')
