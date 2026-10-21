#!/usr/bin/env python3
"""Offline post-restore epoch rotation. Stop Native and gate writers first.
Never run automatically at startup: restarts do not replace message streams.
"""
import argparse
import fcntl
import os
from pathlib import Path
import secrets

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('root', type=Path)
parser.add_argument('--apply', action='store_true')
args = parser.parse_args()
with (args.root / '.registry.lock').open('rb') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    rooms = sorted(p for p in (args.root / 'rooms').iterdir()
                   if len(p.name) == 64 and all(c in '0123456789abcdef' for c in p.name)
                   and p.is_dir() and not p.is_symlink())
    for room in rooms:
        if args.apply:
            target = room / '.generation'
            temporary = room / '.generation.rotate'
            fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'w') as out:
                out.write(secrets.token_hex(32) + '\n')
                out.flush()
                os.fsync(out.fileno())
            os.replace(temporary, target)
            directory = os.open(room, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
    print(f'{"Rotated" if args.apply else "Would rotate"} {len(rooms)} stream generations; messages and private workspace files unchanged.')
