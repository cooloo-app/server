#!/usr/bin/env python3
"""Offline one-time reset, NOT a compatibility layer. Stop all writers and back up first.

Preserves account IDs, credentials, nicknames, messages and room ACLs. Only user
owner/slot display fields and obsolete authz are cleared. Refuses new relations.
Without --apply this is a dry run. Root must be explicitly specified.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import shutil
import tempfile


def reset(root, apply=False):
    root = Path(root)
    if not root.is_dir() or root.is_symlink():
        raise ValueError('root must be an existing real directory')
    fd = os.open(root / '.registry.lock', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        relations = root / 'relations'
        if relations.is_symlink() or (relations.exists() and any(relations.iterdir())):
            raise ValueError('new relationship records exist; refusing legacy reset')
        pending = []
        for directory in sorted((root / 'rooms').iterdir()):
            if len(directory.name) != 64 or any(c not in '0123456789abcdef' for c in directory.name):
                continue
            path = directory / '.meta'
            if directory.is_symlink() or path.is_symlink():
                raise ValueError('symlink metadata is not supported')
            obj = json.loads(path.read_text(encoding='utf-8'))
            if obj['id'] != directory.name:
                raise ValueError('metadata ID mismatch')
            if obj['type'] == 'user' and (obj.get('owner') or obj.get('slot') or obj.get('mirror')):
                obj['owner'], obj['slot'] = '', 0
                obj.pop('mirror', None)
                pending.append((path, json.dumps(obj, ensure_ascii=False, separators=(',', ':')) + '\n'))
        authz = root / 'authz'
        if authz.is_symlink():
            raise ValueError('symlink authz is not supported')
        grants = sum(1 for p in authz.rglob('*') if p.is_file()) if authz.exists() else 0
        if apply:
            for path, text in pending:
                stat = path.stat()
                tmpfd, tmp = tempfile.mkstemp(prefix='.reset-', dir=path.parent)
                try:
                    with os.fdopen(tmpfd, 'w', encoding='utf-8') as output:
                        if os.geteuid() == 0:
                            os.fchown(output.fileno(), stat.st_uid, stat.st_gid)
                        output.write(text)
                        output.flush()
                        os.fsync(output.fileno())
                    os.replace(tmp, path)
                    dfd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
                    try:
                        os.fsync(dfd)
                    finally:
                        os.close(dfd)
                finally:
                    if os.path.exists(tmp):
                        os.unlink(tmp)
            if authz.exists():
                shutil.rmtree(authz)
        return {'applied': apply, 'detached_users': len(pending), 'removed_legacy_grants': grants}
    finally:
        os.close(fd)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root')
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    print(json.dumps(reset(args.root, args.apply)))
