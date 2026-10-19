"""Isolated real Noise connections; keys below are disposable test fixtures."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cooloo-proto2-') as tmp:
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    keys = ['11' * 32, '22' * 32, '33' * 32, '44' * 32]
    def call(key, *args):
        return subprocess.check_output([str(root / 'tests/net_client'), str(port), key, *args], text=True, timeout=12).splitlines()
    ids = [call(k, 'PUBLIC')[0] for k in keys]
    env = dict(os.environ, COOLOO_HOME=tmp)
    with open(Path(tmp) / 'daemon.log', 'w') as log:
        daemon = subprocess.Popen([str(root / 'coolood'), '-b', '127.0.0.1', '-p', str(port)], env=env, stdout=log, stderr=log)
        try:
            for _ in range(100):
                if daemon.poll() is not None:
                    raise RuntimeError('daemon failed')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.1):
                        break
                except OSError:
                    time.sleep(.02)
            else:
                raise RuntimeError('daemon not ready')
            for k in keys[:2]:
                assert call(k, 'HELLO', 'same')[-1] == 'NICK same'
            room_line = call(keys[0], 'RAWLINE', 'CREATE shared')[-1]
            room = room_line.split()[1]
            assert len(room) == 64
            assert call(keys[0], 'RAWLINE', f'AVATAR 001 {ids[2]} child')[-1].startswith('USER ')
            alias = ids[0] + ':001'
            text = 'hello\n"世界"\\'
            assert call(keys[1], 'SEND', alias, text)[-1].startswith('OK ')
            assert call(keys[0], 'READ', alias, '0')[-1] == 'END'
            received = call(keys[2], 'READ', ids[2], '0')
            message = json.loads(received[2])
            assert message['content'] == text and message['from'] == ids[1] and message['to'] == ids[2]
            assert call(keys[2], 'RAWLINE', f'RENAME {ids[0]} stolen')[-1].startswith('ERR forbidden')
            assert call(keys[0], 'SEND', room, 'public')[-1].startswith('OK ')
            received = call(keys[1], 'READ', room, '0')
            end = received[1].split()[1]
            assert json.loads(received[2])['content'] == 'public'
            assert call(keys[1], 'READ', room, end)[-1] == 'END'
            assert call(keys[1], 'READ', room, '1')[-1].startswith('ERR bad-request')
            assert call(keys[0], 'RAWLINE', f'AVATAR 002 {ids[1]} claim')[-1].startswith('ERR exists')
            rooms = call(keys[0], 'ROOMS')
            assert any(line.startswith('ROOM ' + ids[0]) for line in rooms)
            assert any(line.startswith('ROOM ' + ids[2]) for line in rooms)
            assert call(keys[0], 'RAWLINE', f'MIRROR 002 {ids[3]} mirror')[-1].startswith('USER ')
            assert call(keys[3], 'RAWLINE', 'IDENTITY')[-1] == f'IDENTITY {ids[3]} {ids[0]}'
            assert call(keys[3], 'ROOMS')[-1].startswith('ERR identity-required')
            assert call(keys[1], 'SEND', ids[0]+':002', 'no')[-1].startswith('ERR forbidden')
            assert call(keys[0], 'RAWLINE', f'DETACH {ids[3]}')[-1]=='OK'
            assert call(keys[3], 'RAWLINE', 'IDENTITY')[-1] == f'IDENTITY {ids[3]} {ids[3]}'
            print('protocol2: real Noise, avatars, pure mirror context/alias rejection/detach, private delivery and incremental read passed')
        finally:
            daemon.terminate()
            daemon.wait(timeout=5)
