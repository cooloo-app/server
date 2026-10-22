"""Disposable gate/model tests: persisted room controls, limits and legacy activation."""
import json
import uuid
import os
from pathlib import Path
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cooloo-private-') as tmp:
    home = Path(tmp)
    env = dict(os.environ, COOLOO_HOME=tmp)
    def uid(n):
        return f'{n:064x}'
    def call(actor, *args, text=None, ok=True):
        if args[0]=='send': args=(*args[:2],uuid.uuid4().hex,*args[2:])
        p = subprocess.run([str(root / 'cooloo'), '--as', actor, *args],
                           env=env, input=text, text=True, capture_output=True, timeout=10)
        assert (p.returncode == 0) == ok, (args, p.returncode, p.stdout, p.stderr)
        return p.stdout
    a, b, c, d = [uid(i) for i in range(1, 5)]
    call(a, 'register', 'owner')
    call(b, 'register', 'member')
    room = call(a, 'create', 'private-test').split()[1]
    call(b, 'send', room, text='historical participant')
    call(b, 'room-mode', room, 'private')  # nonowner may switch
    call(b, 'send', room, text='existing member')
    call(c, 'register', 'newcomer')
    call(d, 'register', 'other')
    for cmd in [('read', room, '0'), ('meta', room)]:
        call(c, *cmd, ok=False)
    assert room not in call(c, 'rooms')
    call(c, 'send', room, text='ordinary', ok=False)
    call(c, 'send', room, text=f'@{room} join-request\nextra', ok=False)
    call(c, 'send', room, text=f'@{room} join-approve {d}', ok=False)
    call(b, 'send', room, text=f'@{room} join-approve {c}', ok=False)
    call(c, 'join', room)
    log = home / 'rooms' / room / 'messages.jsonl'
    before = log.read_bytes()
    call(c, 'join', room, ok=False)  # a new process cannot evade the cooldown
    assert log.read_bytes() == before
    # Controlled offline fixtures test expiry without sleeping for days.
    records = [json.loads(line) for line in log.read_text().splitlines()]
    import time
    records[-1]['ts'] = int(time.time()) - 86402
    log.write_text(''.join(json.dumps(r, separators=(',', ':')) + '\n' for r in records))
    call(c, 'join', room)  # greater than 24 hours is accepted
    call(c, 'join', room, ok=False)
    records = [json.loads(line) for line in log.read_text().splitlines()]
    for r in records:
        if r['content'] == f'@{room} join-request':
            r['ts'] = int(time.time()) - 8 * 86400
    log.write_text(''.join(json.dumps(r, separators=(',', ':')) + '\n' for r in records))
    call(b, 'send', room, text=f'@{room} join-approve {c}', ok=False)
    call(c, 'join', room)
    call(b, 'send', room, text=f'@{room}  join-approve {c}')
    approved = log.read_bytes()
    call(b, 'send', room, text=f'@{room} join-approve {c}', ok=False)
    assert log.read_bytes() == approved  # idempotent approval
    call(c, 'read', room, '0')
    call(c, 'meta', room)
    call(c, 'send', room, text='member after restart')
    call(c, 'join', room, ok=False)
    call(d, 'join', room)
    call(c, 'send', room, text=f'@{room}\tjoin-approve\t{d}')
    call(d, 'read', room, '0')  # canonical escaped tabs replay correctly
    e = uid(5)
    call(e, 'register', 'outsider')
    call(e, 'room-mode', room, 'public')  # explicitly allowed temporary policy
    call(e, 'read', room, '0')
    call(e, 'send', room, text=f'@{room} join-approve {a}', ok=False)
    call(e, 'room-mode', room, 'private')
    call(e, 'send', room, text='not a historical sender', ok=False)
    call(e, 'read', room, '0', ok=False)  # reading and switching do not grant membership
    assert room not in call(e, 'rooms')
    call(e, 'room-mode', room, 'public')
    call(e, 'send', room, text='now a historical sender')
    call(e, 'send', room, text='duplicate sender counts once')
    call(e, 'room-mode', room, 'private')
    call(e, 'send', room, text='snapshotted')
    access = home / 'rooms' / room / '.access'
    assert access.read_text().splitlines()[0].endswith(' 5')
    # Raw extension bytes, 256-byte maximum, survive mode changes.
    lines = access.read_text().splitlines()
    for i, line in enumerate(lines):
        if line.startswith(a + ' '):
            lines[i] = a + ' 256 ' + bytes(range(256)).hex()
    access.write_text('\n'.join(lines) + '\n')
    call(b, 'room-mode', room, 'public')
    assert a + ' 256 ' + bytes(range(256)).hex() in access.read_text()
    call(b, 'room-mode', room, 'private')
    saved = access.read_bytes()
    access.unlink()
    call(a, 'read', room, '0', ok=False)  # schema marker: never fall back to public
    call(a, 'room-mode', room, 'public', ok=False)
    access.write_bytes(b'broken\n')
    call(a, 'read', room, '0', ok=False)
    access.write_bytes(saved)
    # Legacy logs must not turn old literal control text into newly granted membership.
    legacy = call(a, 'create', 'legacy').split()[1]
    directory = home / 'rooms' / legacy
    meta = json.loads((directory / '.meta').read_text())
    meta.pop('access')
    (directory / '.meta').write_text(json.dumps(meta, separators=(',', ':')) + '\n')
    (directory / '.access').unlink()
    (directory / 'messages.jsonl').write_text(json.dumps(dict(ts=1, **{'from': a, 'to': legacy},
        content=f'@{legacy} join-approve {e}', id=uuid.uuid4().hex), separators=(',', ':')) + '\n')
    call(b, 'join', legacy)
    call(e, 'send', legacy, text=f'@{legacy} join-approve {b}', ok=False)
    call(a, 'send', legacy, text=f'@{legacy} join-approve {b}')
    # Boundary: 199 existing members, two concurrent approvals for the final seat.
    for i in range(6, 200):
        call(uid(i), 'register', 'user')
    full = call(a, 'create', 'capacity').split()[1]
    for i in range(2, 200):
        call(uid(i), 'send', full, text='participant')
    call(b, 'room-mode', full, 'private')
    for i in (200, 201):
        call(uid(i), 'register', 'candidate')
        call(uid(i), 'join', full)
    def approve(i):
        return subprocess.run([str(root / 'cooloo'), '--as', b, 'send', full, uuid.uuid4().hex,
            f'@{full} join-approve {uid(i)}'], env=env, capture_output=True).returncode
    with ThreadPoolExecutor(max_workers=2) as pool:
        assert sorted(pool.map(approve, (200, 201))) == [0, 1]
    call(a, 'room-mode', full, 'public')
    snapshot = (home / 'rooms' / full / '.access').read_bytes()
    call(a, 'room-mode', full, 'private', ok=False)  # owner + 200 distinct historical senders
    assert (home / 'rooms' / full / '.access').read_bytes() == snapshot
    call(uid(201), 'read', full, '0')
    call(uid(202), 'room-mode', full, 'public', ok=False)  # unregistered
    # Over 200 server identities does not prevent converting an empty room.
    empty = call(a, 'create', 'empty').split()[1]
    call(b, 'room-mode', empty, 'private')
    call(a, 'read', empty, '0')
    call(b, 'read', empty, '0', ok=False)
    # Accepted requests count as sent messages; other rooms/inbox traffic does not.
    call(b, 'join', empty)
    call(c, 'send', a, text='inbox is not this room')
    call(c, 'room-mode', empty, 'public')
    call(c, 'room-mode', empty, 'private')
    call(b, 'read', empty, '0')
    call(c, 'read', empty, '0', ok=False)
    print('private room: controls, cooldown, replay, metadata ACL, mode snapshots, blocks, legacy activation and concurrent capacity passed')
