#!/usr/bin/env python3
"""Fresh, capped official Nitro --dev lab; no mainnet or external RPC source.

Run through root Docker CLI without adding users to the Docker group.  This
launches actual Nitro execution, but has NO L1 rollup assertion/finality proof.
It never deletes existing runtimes, Docker images, containers, or volumes.
"""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import stat
import subprocess
import time

from local_rpc import LocalNitroRpc, raw_hex, unique_object

IMAGE = 'offchainlabs/nitro-node@sha256:948cf8c42a6ed85bc055f13866fb9a029c8234dc16c12d7ffa4ed669270aaea8'
IMAGE_SOURCE = '70fa99a6e711f09eedd96a907290c3df0f3bcf7d'
LABEL = 'org.b3.nitro-regtest.instance'
FORMAT = 'b3-owned-nitro-devnet/1'
MAX_STORAGE = 25 * 1024 ** 3
MAX_RUNTIME_STORAGE = 8 * 1024 ** 3
MAX_EXPORT = 32 * 1024 ** 2


def docker(*args, timeout=60):
    environment = {'PATH': '/usr/bin:/bin', 'HOME': '/nonexistent',
                   'LANG': 'C', 'LC_ALL': 'C', 'TZ': 'UTC'}
    result = subprocess.run(['/usr/bin/docker', '--host', 'unix:///var/run/docker.sock', *args],
                            capture_output=True, text=True, timeout=timeout, check=False,
                            env=environment)
    if result.returncode:
        raise RuntimeError('Docker command failed: ' + ' '.join(args[:2])
                           + ': ' + result.stderr[-2000:].strip())
    return result.stdout.strip()


def safe_path(path, *, existing=True):
    path = Path(path).absolute()
    if '..' in path.parts:
        raise ValueError('parent traversal refused')
    for part in [*reversed(path.parents), path]:
        try:
            meta = part.lstat()
        except FileNotFoundError:
            if part == path and not existing:
                continue
            raise
        if stat.S_ISLNK(meta.st_mode):
            raise ValueError('symbolic path refused')
        if part != path and not stat.S_ISDIR(meta.st_mode):
            raise ValueError('non-directory ancestor refused')
    return path


def write_json(path, value):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8') as stream:
        json.dump(value, stream, sort_keys=True, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def load(runtime):
    runtime = safe_path(runtime)
    path = runtime / 'runtime.json'
    meta = path.lstat()
    if not stat.S_ISREG(meta.st_mode) or meta.st_nlink != 1 or meta.st_mode & 0o077:
        raise ValueError('unsafe runtime record')
    value = json.loads(path.read_text(), object_pairs_hook=unique_object)
    if (value.get('format') != FORMAT or value.get('mainnet_enabled') is not False
            or value.get('image') != IMAGE or value.get('runtime') != str(runtime)
            or not re.fullmatch(r'b3-nitro-regtest-[a-z0-9-]{1,70}', value.get('name', ''))):
        raise ValueError('unrecognized owned runtime')
    info = json.loads(docker('inspect', value['container_id']))[0]
    if (info['Name'] != '/' + value['name']
            or info['Config']['Labels'].get(LABEL) != value['instance']
            or info['Config']['Image'] != IMAGE):
        raise ValueError('Docker ownership binding changed')
    host = info['HostConfig']
    bindings = host.get('PortBindings') or {}
    expected_bindings = {'8547/tcp': [{'HostIp': '127.0.0.1', 'HostPort': str(value['port'])}]}
    if (host['Memory'] != 8 * 1024 ** 3 or host['NanoCpus'] != 4 * 10 ** 9
            or host['MemorySwap'] != host['Memory'] or host['Privileged']
            or not host['ReadonlyRootfs'] or host['CapDrop'] != ['ALL']
            or host['NetworkMode'] != value['network'] or bindings != expected_bindings):
        raise ValueError('Docker resource or loopback boundary changed')
    network = json.loads(docker('network', 'inspect', value['network']))[0]
    if (network['Labels'].get(LABEL) != value['instance']
            or network['Options'].get('com.docker.network.bridge.enable_ip_masquerade') != 'false'):
        raise ValueError('Docker network isolation changed')
    return runtime, value, info


def storage_bytes(root):
    total, count = 0, 0
    for base, dirs, files in os.walk(root, followlinks=False):
        for name in [*dirs, *files]:
            path = Path(base) / name
            meta = path.lstat()
            if stat.S_ISLNK(meta.st_mode):
                raise ValueError('runtime contains a symbolic link')
            if stat.S_ISREG(meta.st_mode):
                total += meta.st_size
                count += 1
                if count > 250000 or total > MAX_RUNTIME_STORAGE:
                    raise ValueError('runtime storage budget exceeded')
    return total


def start(parent, port):
    parent = safe_path(parent)
    if not parent.is_dir() or parent.name != 'B3-peer-work':
        raise ValueError('runtime parent must be the dedicated B3-peer-work directory')
    if shutil.disk_usage(parent).free < MAX_STORAGE:
        raise ValueError('less than 25 GiB free')
    # Refuse collisions before allocating anything; no existing process is stopped.
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', port))
    image = json.loads(docker('image', 'inspect', IMAGE))[0]
    if image['Architecture'] != 'amd64' or image['Os'] != 'linux' or image['Size'] > MAX_STORAGE:
        raise ValueError('unexpected or oversized pinned image')
    suffix = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S') + '-' + os.urandom(4).hex()
    name = 'b3-nitro-regtest-' + suffix
    runtime = parent / ('nitro-regtest-' + suffix)
    runtime.mkdir(mode=0o700)
    data = runtime / 'data'
    data.mkdir(mode=0o700)
    owner = parent.stat()
    os.chown(runtime, owner.st_uid, owner.st_gid)
    os.chown(data, 1000, 1000)
    # A no-masquerade bridge permits Docker's loopback publication without NAT
    # access to external providers. Nitro --dev has no parent or feed source.
    network = name + '-network'
    docker('network', 'create', '--opt', 'com.docker.network.bridge.enable_ip_masquerade=false',
           '--label', LABEL + '=' + suffix, network)
    container = docker('create', '--name', name, '--label', LABEL + '=' + suffix,
                       '--network', network, '--publish', '127.0.0.1:%d:8547' % port,
                       '--memory', '8g', '--memory-swap', '8g', '--cpus', '4',
                       '--pids-limit', '512', '--cap-drop', 'ALL',
                       '--security-opt', 'no-new-privileges', '--read-only',
                       '--tmpfs', '/tmp:rw,nosuid,nodev,size=67108864',
                       '--log-opt', 'max-size=10m', '--log-opt', 'max-file=2',
                       '--mount', 'type=bind,src=' + str(data) + ',dst=/home/user/.arbitrum',
                       IMAGE, '--dev', '--persistent.chain', '/home/user/.arbitrum/nitro',
                       '--http.addr', '0.0.0.0', '--http.port', '8547',
                       '--http.api', 'net,web3,eth,debug', '--http.vhosts', 'localhost,127.0.0.1')
    value = {'format': FORMAT, 'instance': suffix, 'runtime': str(runtime),
             'name': name, 'network': network, 'container_id': container,
             'image': IMAGE, 'nitro_source_commit': IMAGE_SOURCE, 'port': port,
             'mainnet_enabled': False, 'scope': 'owned-nitro-dev-execution-only',
             'l1_settlement_verified': False}
    write_json(runtime / 'runtime.json', value)
    os.chown(runtime / 'runtime.json', owner.st_uid, owner.st_gid)
    docker('start', container)
    rpc = LocalNitroRpc(port, timeout=2)
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        try:
            chain = int(rpc.call('eth_chainId'), 16)
            genesis = rpc.call('eth_getBlockByNumber', ['0x0', False])
            raw_hex(genesis['hash'], size=32)
            if chain in (1, 42161):
                raise ValueError('live chain identity refused')
            result = {**value, 'chain_id': chain, 'genesis_hash': genesis['hash'],
                      'client_version': rpc.call('web3_clientVersion'),
                      'image_id': image['Id'], 'runtime_bytes': storage_bytes(runtime)}
            write_json(runtime / 'ready.json', result)
            os.chown(runtime / 'ready.json', owner.st_uid, owner.st_gid)
            return result
        except (ConnectionError, OSError, TimeoutError, RuntimeError):
            current = json.loads(docker('inspect', container))[0]
            if not current['State']['Running']:
                raise RuntimeError('owned Nitro exited; retained runtime: ' + str(runtime))
            time.sleep(0.1)
    docker('stop', '--time', '10', container)
    raise RuntimeError('owned Nitro startup deadline exceeded; retained runtime: ' + str(runtime))


def status(runtime):
    runtime, value, info = load(runtime)
    return {**value, 'running': info['State']['Running'],
            'runtime_bytes': storage_bytes(runtime),
            'mainnet_enabled': False, 'l1_settlement_verified': False}


def stop(runtime):
    runtime, value, info = load(runtime)
    if info['State']['Running']:
        docker('stop', '--time', '30', value['container_id'])
    return status(runtime)


def serve(runtime, seconds):
    """Keep an owned WSL session alive; stop only this container on exit."""
    if not 1 <= seconds <= 12 * 3600:
        raise ValueError('invalid bounded session duration')
    runtime, value, info = load(runtime)
    if not info['State']['Running']:
        docker('start', value['container_id'])
    print(json.dumps({'serving': value['name'], 'runtime': str(runtime),
                      'seconds': seconds, 'mainnet_enabled': False}), flush=True)
    deadline = time.monotonic() + seconds
    try:
        while time.monotonic() < deadline:
            time.sleep(2)
            current = status(runtime)
            if not current['running']:
                return current
        return status(runtime)
    finally:
        stop(runtime)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    begin = commands.add_parser('start')
    begin.add_argument('--runtime-parent', type=Path, required=True)
    begin.add_argument('--port', type=int, default=18547)
    for name in ('status', 'stop'):
        item = commands.add_parser(name)
        item.add_argument('--runtime', type=Path, required=True)
    keep = commands.add_parser('serve')
    keep.add_argument('--runtime', type=Path, required=True)
    keep.add_argument('--seconds', type=int, default=8 * 3600)
    args = parser.parse_args()
    if args.command == 'start':
        if type(args.port) is not int or not 1024 <= args.port <= 65535:
            parser.error('invalid loopback port')
        value = start(args.runtime_parent, args.port)
    elif args.command == 'serve':
        value = serve(args.runtime, args.seconds)
    else:
        value = globals()[args.command](args.runtime)
    print(json.dumps(value, sort_keys=True))


if __name__ == '__main__':
    main()
