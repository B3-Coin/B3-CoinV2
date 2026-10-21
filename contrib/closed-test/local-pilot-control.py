#!/usr/bin/env python3
"""Write one bounded local pilot command; never invoke wallet RPC directly."""
import argparse
import fcntl
import json
import os
from pathlib import Path
import tempfile
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--session-dir', required=True, type=Path)
    parser.add_argument('command', choices=['snapshot', 'fund', 'mine', 'checkpoint', 'hold_read', 'clear_holds', 'requests', 'stop'])
    parser.add_argument('--address')
    parser.add_argument('--blocks', type=int)
    parser.add_argument('--milliseconds', type=int)
    args = parser.parse_args()
    HERE = args.session_dir.resolve(strict=True)
    info = HERE.stat()
    if not HERE.is_dir() or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise RuntimeError('Session must be an existing same-user private directory')
    session = json.loads((HERE / 'session.json').read_text())
    os.kill(session['controller_pid'], 0) # Existence only; ready/response separately establish work.
    if not (HERE / 'ready.json').exists() or (HERE / 'exit.json').exists(): raise RuntimeError('Pilot is not active/ready')
    descriptor = os.open(HERE / 'control-writer.lock', os.O_WRONLY | os.O_CREAT | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        existing = [int(p.stem) for p in (HERE / 'commands').glob('*.json')]
        identifier = max(existing, default=0) + 1
        if identifier > 128: raise RuntimeError('Command bound exhausted')
        value = {'id': identifier, 'command': args.command}
        optional = {'fund': ('address', args.address), 'mine': ('blocks', args.blocks), 'hold_read': ('milliseconds', args.milliseconds)}
        if args.command in optional:
            name, val = optional[args.command]
            if val is None: parser.error('Required --' + name)
            value[name] = val
        path = HERE / 'commands' / f'{identifier:03d}.json'
        # The reader does not take this writer lock. Publish only a complete,
        # flushed object, without overwriting any earlier command identity.
        fd, temporary_name = tempfile.mkstemp(prefix=f'.{identifier:03d}-', suffix='.pending', dir=path.parent)
        temporary = Path(temporary_name)
        try:
            with os.fdopen(fd, 'w') as output:
                json.dump(value, output); output.write('\n'); output.flush(); os.fsync(output.fileno())
            # Cooperating writers hold control-writer.lock throughout ID
            # allocation/publication in this same-user private directory.
            if path.exists() or path.is_symlink():
                raise FileExistsError('Command identity already exists')
            os.rename(temporary, path)
        finally:
            # Only this invocation's newly created, unpublished pathname.
            # No existing command, response, lock or wallet data is removed.
            if temporary.exists(): temporary.unlink()
        directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    print(json.dumps({'queued': True, 'id': identifier, 'response': str(HERE / 'responses' / f'{identifier:03d}.json')}))

if __name__ == '__main__': main()
