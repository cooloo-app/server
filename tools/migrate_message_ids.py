#!/usr/bin/env python3
"""Offline Protocol 3 migration. Stop ALL writers and back up the entire root first.
Stages an immutable migration plan before replacing any live files. Re-running
--apply completes that same plan; UUIDs and generations never change on retry.
Only record encoding/IDs, ACL journal byte offsets and stream generations change.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import secrets
import uuid


def digest(data):
    return hashlib.sha256(data).hexdigest()


def atomic(path, data):
    tmp = path.with_name(path.name + '.tmp')
    with tmp.open('wb') as f:
        os.chmod(tmp, 0o600)
        f.write(data)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def canonical(m):
    # Match model.c quote(): all ASCII controls use lowercase \\u00xx.
    content = '"' + ''.join('\\"' if c == '"' else '\\\\' if c == '\\' else
                           '\\u%04x' % ord(c) if ord(c) < 32 else c for c in m['content']) + '"'
    return ('{"ts":%d,"from":"%s","to":"%s","content":%s,"id":"%s"}\n' %
            (m['ts'], m['from'], m['to'], content, m['id'])).encode('utf-8')


def valid_id(s, length=64):
    return isinstance(s, str) and len(s) == length and all(c in '0123456789abcdef' for c in s)


def migrate(root, apply=False):
    root = root.resolve()
    stage = root / '.message-id-migration-v1'
    with (root / '.registry.lock').open('rb') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        manifest = stage / 'manifest.json'
        if manifest.exists():
            plan = json.loads(manifest.read_text())
        else:
            replacements = []
            seen = set()
            count = added = 0
            def replace(path, data):
                old = path.read_bytes() if path.exists() else None
                replacements.append((str(path.relative_to(root)), digest(old) if old is not None else None, data))
            for room in sorted((root / 'rooms').iterdir()):
                if not valid_id(room.name):
                    continue
                if room.is_symlink() or not room.is_dir():
                    raise ValueError('unsafe room directory')
                log = room / 'messages.jsonl'
                mapping = {0: 0}
                source = dest = 0
                records = []
                if log.exists():
                    if log.is_symlink():
                        raise ValueError('symlink journal')
                    for line in log.read_bytes().splitlines(keepends=True):
                        if not line.endswith(b'\n'):
                            raise ValueError('incomplete journal')
                        m = json.loads(line)
                        if set(m) not in ({'ts', 'from', 'to', 'content'}, {'ts', 'from', 'to', 'content', 'id'}):
                            raise ValueError('unexpected record fields')
                        if (type(m['ts']) is not int or m['ts'] < 0 or not valid_id(m['from']) or
                                m['to'] != room.name or not isinstance(m['content'], str) or
                                '\0' in m['content'] or not 1 <= len(m['content'].encode()) <= 4096):
                            raise ValueError('invalid record')
                        if 'id' not in m:
                            m['id'] = uuid.uuid4().hex
                            added += 1
                        mid = m['id']
                        if not valid_id(mid, 32) or uuid.UUID(hex=mid).version != 4 or mid in seen:
                            raise ValueError('invalid/duplicate message UUID')
                        seen.add(mid)
                        encoded = canonical(m)
                        records.append(encoded)
                        source += len(line)
                        dest += len(encoded)
                        mapping[source] = dest
                        count += 1
                    replace(log, b''.join(records))
                acl = room / '.access'
                if acl.exists():
                    if acl.is_symlink():
                        raise ValueError('symlink ACL')
                    lines = acl.read_bytes().splitlines(keepends=True)
                    header = lines[0].decode().split()
                    if len(header) != 4 or header[0] != 'COOLOO-ACCESS-1' or int(header[2]) not in mapping:
                        raise ValueError('invalid ACL journal boundary')
                    header[2] = str(mapping[int(header[2])])
                    replace(acl, (' '.join(header) + '\n').encode() + b''.join(lines[1:]))
                generation = room / '.generation'
                if generation.is_symlink():
                    raise ValueError('symlink generation')
                replace(generation, (secrets.token_hex(32) + '\n').encode())
            plan = {'records': count, 'added': added, 'files': []}
            if not apply:
                return {'dry_run': True, 'records': count, 'added': added, 'files': len(replacements)}
            stage.mkdir(mode=0o700, exist_ok=True)
            for i, (path, before, data) in enumerate(replacements):
                name = str(i)
                atomic(stage / name, data)
                plan['files'].append({'path': path, 'before': before, 'after': digest(data), 'stage': name})
            atomic(manifest, json.dumps(plan).encode())
        # Validate all inputs before touching any live file. Permit an interrupted prefix.
        for entry in plan['files']:
            path = root / entry['path']
            if path.is_symlink():
                raise ValueError('unsafe migration target')
            actual = digest(path.read_bytes()) if path.exists() else None
            if actual not in (entry['before'], entry['after']):
                raise ValueError('history changed since staging; keep writers stopped')
            if digest((stage / entry['stage']).read_bytes()) != entry['after']:
                raise ValueError('corrupt migration stage')
        if apply:
            for entry in plan['files']:
                path = root / entry['path']
                if not path.exists() or digest(path.read_bytes()) != entry['after']:
                    atomic(path, (stage / entry['stage']).read_bytes())
            atomic(stage / 'complete', b'complete\n')
        return {'dry_run': not apply, 'records': plan['records'], 'added': plan['added'], 'files': len(plan['files'])}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    print(json.dumps(migrate(args.root, args.apply)))
