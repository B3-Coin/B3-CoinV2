#!/usr/bin/env python3
"""Fresh owned L1 + posted Nitro rollup + actual WASM validation.

This is a local development trust root, never Ethereum PoS or public Arbitrum
authority. No existing container, volume, directory or journal is reset.
"""
import argparse
import copy
import datetime
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import socket
import stat
import subprocess
import time
import uuid

FORMAT = 'b3-owned-nitro-settlement/1'
LABEL = 'org.b3.nitro-settlement.instance'
NITRO = 'offchainlabs/nitro-node@sha256:948cf8c42a6ed85bc055f13866fb9a029c8234dc16c12d7ffa4ed669270aaea8'
GETH = 'ethereum/client-go@sha256:16c5a976cd8b6985300e80df4043a40e31e964793f937e8244bceda22163874d'
NODE = 'library/node@sha256:1694ccde5ea9efb3060bb8612b1f287256061ff2a04d75dc1b71f57cb7239520'
MODULE_ROOT = '0xc10cd7ec6acaf1c441a3f6bd0900ad20f15855ba775a96f1939118cbc629dc97'
TESTNODE_COMMIT = 'd46fe1bfe6dd0dbc2e27dcc6d0dc559a248433f5'
CONTRACTS_COMMIT = '4341b132cfbdcc980ead03765ca5224ff6cb5d97'
DEV = '0x3f1eae7d46d88f08fc2f8ed27fcb2ab183eb2d0e'
SEQUENCER = '0xe2148ee53c0755215df69b2616e552154edc584f'
VALIDATOR = '0x6a568afe0f82d34759347bb36f14a6bb171d2cbe'
OWNER = '0x5e1497dd1f08c87b2d8fe23e9aab6c1de833d927'
GIB = 1024 ** 3
DISK_LIMIT = 64 * GIB
# Combined steady-state allocation is exactly 12 GiB and eight CPUs.
ROLES = {'geth': (1, 1), 'sequencer': (2, 2), 'poster': (2, 1), 'validator': (3, 2), 'validation_node': (4, 2)}
ENV = {'PATH': '/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin',
       'HOME': '/root', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TZ': 'UTC'}
READ_METHODS = frozenset({'web3_clientVersion', 'eth_chainId', 'eth_blockNumber', 'eth_getBlockByNumber',
    'eth_getBlockByHash', 'eth_getCode', 'eth_getBalance', 'eth_getTransactionReceipt', 'eth_getProof',
    'eth_getStorageAt', 'eth_getLogs', 'eth_call', 'debug_getRawHeader', 'debug_getRawBlock',
    'debug_getRawReceipts', 'eth_getRawTransactionByBlockHashAndIndex', 'arb_latestValidated',
    'arb_findBatchContainingBlock', 'arb_getL1Confirmations'})


def expect(value, message):
    if not value:
        raise ValueError(message)


def unique(pairs):
    result = {}
    for key, value in pairs:
        expect(key not in result, 'duplicate JSON member')
        result[key] = value
    return result


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def safe(path, exists=True):
    path = Path(path).absolute()
    expect('..' not in path.parts, 'parent traversal refused')
    for part in [*reversed(path.parents), path]:
        try:
            meta = part.lstat()
        except FileNotFoundError:
            expect(part == path and not exists, 'absent ancestor')
            continue
        expect(not stat.S_ISLNK(meta.st_mode), 'symbolic path refused')
        if part != path:
            expect(stat.S_ISDIR(meta.st_mode), 'nondirectory ancestor')
    return path


def read_json(path, maximum=1024 * 1024):
    path = safe(path)
    meta = path.lstat()
    expect(stat.S_ISREG(meta.st_mode) and meta.st_nlink == 1 and meta.st_size <= maximum, 'unsafe JSON input')
    data = path.read_bytes()
    expect(len(data) <= maximum, 'JSON input exceeds bound')
    return json.loads(data, object_pairs_hook=unique)


def write_json(path, value, *, fresh=True):
    data = (json.dumps(value, sort_keys=True, indent=2) + '\n').encode()
    expect(len(data) <= 4 * 1024 * 1024, 'JSON output exceeds bound')
    path = Path(path)
    target = path if fresh else path.with_name(path.name + '.new-' + uuid.uuid4().hex)
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data); stream.flush(); os.fsync(stream.fileno())
    if not fresh:
        os.replace(target, path)
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def docker(*args, timeout=60):
    result = subprocess.run(['/usr/bin/docker', '--host', 'unix:///var/run/docker.sock', *args],
        capture_output=True, text=True, env=ENV, timeout=timeout)
    if result.returncode:
        raise RuntimeError('local Docker refused ' + ' '.join(args[:2]) + ': ' + result.stderr[-1200:].strip())
    expect(len(result.stdout) <= 32 * 1024 * 1024, 'Docker output exceeds bound')
    return result.stdout.strip()


def docker_logs(container):
    result = subprocess.run(['/usr/bin/docker', '--host', 'unix:///var/run/docker.sock', 'logs', container],
                            capture_output=True, env=ENV, timeout=30)
    expect(result.returncode == 0 and len(result.stdout) + len(result.stderr) <= 32 * 1024 * 1024,
           'owned log export refused')
    return (result.stdout + result.stderr).decode(errors='replace')


class Rpc:
    def __init__(self, port, timeout=10):
        expect(type(port) is int and 1024 <= port <= 65535, 'invalid loopback port')
        expect(type(timeout) in (int, float) and 0 < timeout <= 30, 'invalid deadline')
        self.port, self.timeout, self.sequence = port, timeout, 0

    def _request(self, method, params):
        self.sequence += 1
        body = json.dumps({'jsonrpc': '2.0', 'id': self.sequence, 'method': method, 'params': params}).encode()
        expect(len(body) <= 65536, 'request exceeds bound')
        conn = http.client.HTTPConnection('127.0.0.1', self.port, timeout=self.timeout)
        try:
            conn.request('POST', '/', body=body, headers={'Content-Type': 'application/json', 'Connection': 'close'})
            reply = conn.getresponse()
            expect(reply.status == 200, 'owned RPC HTTP refusal')
            raw = reply.read(32 * 1024 * 1024 + 1)
            expect(len(raw) <= 32 * 1024 * 1024, 'RPC output exceeds bound')
            value = json.loads(raw, object_pairs_hook=unique)
            expect(set(value) == {'jsonrpc', 'id', 'result'} and value['jsonrpc'] == '2.0'
                   and type(value['id']) is int and value['id'] == self.sequence, 'owned RPC result refused')
            return value['result']
        finally:
            conn.close()

    def call(self, method, params=()):
        expect(method in READ_METHODS, 'read-only owned availability method required')
        return self._request(method, list(params))

def assets_info(path):
    path = safe(path)
    value = read_json(path / 'assets.json', 4 * 1024 * 1024)
    expect(value['format'] == 'b3-owned-nitro-settlement-assets/1' and value['mainnet_enabled'] is False
           and value['directory'] == str(path) and value['upstream']['testnode']['commit'] == TESTNODE_COMMIT
           and value['upstream']['contracts']['commit'] == CONTRACTS_COMMIT, 'unrecognized pinned helper assets')
    expect(value.get('build', {}).get('contracts') is True and value['build'].get('scripts') is True, 'build the pinned helper assets first')
    for name, digest in value['source_files'].items():
        item = path / name
        expect(not Path(name).is_absolute() and '..' not in Path(name).parts and not item.is_symlink() and sha(item) == digest,
               'pinned helper source or tool changed')
    return path, value


def used_bytes(path):
    size = int(subprocess.check_output(['/usr/bin/du', '-sb', str(path)], text=True, timeout=30).split()[0])
    expect(size <= DISK_LIMIT, 'owned runtime disk budget exceeded')
    return size


def base_args(name, instance, image, memory, cpus, network=None, user='1000:1000'):
    expect(type(memory) is int and 1 <= memory <= 4 and type(cpus) is int and 1 <= cpus <= 2, 'role budget refused')
    return ['create', '--name', name, '--label', LABEL + '=' + instance, '--memory', str(memory) + 'g',
        '--memory-swap', str(memory) + 'g', '--cpus', str(cpus), '--pids-limit', '512', '--cap-drop', 'ALL',
        '--security-opt', 'no-new-privileges', '--read-only', '--user', user,
        '--tmpfs', '/tmp:rw,nosuid,nodev,size=536870912', '--log-opt', 'max-size=10m', '--log-opt', 'max-file=2',
        *(['--network', network] if network else [])]


def helper(runtime, value, title, directory, command, extra_env=None):
    assets = Path(value['assets'])
    name = value['name'] + '-helper-' + title
    args = base_args(name, value['instance'], NODE, 4, 2, value['network'])
    args += ['--mount', 'type=bind,src=' + str(assets) + ',dst=/assets',
             '--mount', 'type=bind,src=' + str(runtime / 'config') + ',dst=/config',
             '--mount', 'type=bind,src=' + str(runtime / 'keystore') + ',dst=/home/user/l1keystore', '--workdir', directory]
    variables = {'HOME': '/assets/home', 'XDG_CACHE_HOME': '/assets/home/cache',
                 'PATH': '/assets/tools:/usr/local/bin:/usr/bin:/bin', 'NODE_OPTIONS': '--max-old-space-size=3072'}
    variables.update(extra_env or {})
    for key, item in variables.items():
        args += ['--env', key + '=' + item]
    args += [NODE, *command]
    cid = docker(*args)
    try:
        docker('start', cid)
        deadline = time.monotonic() + 900
        while True:
            state = json.loads(docker('inspect', cid))[0]['State']
            if not state['Running']:
                logs = docker_logs(cid)
                (runtime / (title + '.log')).write_text(logs + '\n')
                expect(state['ExitCode'] == 0, 'owned helper failed: ' + title)
                return logs
            expect(time.monotonic() < deadline, 'owned helper deadline: ' + title)
            used_bytes(runtime)
            time.sleep(1)
    finally:
        if json.loads(docker('inspect', cid))[0]['State']['Running']:
            docker('stop', '--time', '20', cid)


def validate_config(role, config):
    expect(config['chain']['id'] == 412346 and config['parent-chain']['id'] == 1337,
           'owned chain identities required')
    node = config['node']
    expect(node['staker']['dangerous']['without-block-validator'] is False
           and node['bold']['start-validation-from-staked'] is False
           and node['staker']['start-validation-from-staked'] is False,
           'assumed validation or validator bypass refused')
    expect(node['batch-poster']['post-4844-blobs'] is False and node['dangerous']['disable-blob-reader'] is True,
           'owned calldata-only DA profile required')
    if role == 'validator':
        expect(node['block-validator']['enable'] is True and node['staker']['enable'] is True
               and node['staker']['strategy'] == 'MakeNodes', 'actual validating staker required')
        expect(node['feed']['input']['url'] == [], 'independent L1-only validator input required')
    else:
        expect(node['staker']['enable'] is False, 'separate validating staker required')


def write_role_configs(runtime):
    config_dir = runtime / 'config'
    for role in ('sequencer', 'poster', 'validator'):
        path = config_dir / (role + '_config.json')
        config = read_json(path)
        config['parent-chain']['connection']['url'] = 'http://geth:8545'
        config['parent-chain']['id'] = 1337
        config['node']['seq-coordinator']['enable'] = False
        config['node']['dangerous']['no-sequencer-coordinator'] = True
        config['node']['dangerous']['disable-blob-reader'] = True
        config['node']['batch-poster']['redis-url'] = ''
        config['node']['batch-poster']['max-delay'] = '2s'
        config['node']['batch-poster']['post-4844-blobs'] = False
        config['node']['block-validator']['enable'] = role == 'validator'
        config['node']['block-validator']['validation-server']['url'] = 'ws://validation_node:8549'
        config['node']['bold']['start-validation-from-staked'] = False
        config['node']['staker']['start-validation-from-staked'] = False
        config['node']['staker']['dangerous']['without-block-validator'] = False
        config['node']['feed'] = {'input': {'url': ['ws://sequencer:9642'] if role == 'poster' else []},
                                  'output': {'enable': role == 'sequencer', 'port': 9642}}
        config['node']['delayed-sequencer']['use-merge-finality'] = False
        config['node']['delayed-sequencer']['finalize-distance'] = 1
        config['execution']['caching'] = {'archive': True, 'state-scheme': 'hash'}
        config['http'].update({'port': 8547, 'api': ['net', 'web3', 'eth', 'arb', 'debug', 'arbdebug'],
                               'vhosts': ['localhost', '127.0.0.1', role]})
        config['ws'].update({'addr': '0.0.0.0', 'port': 8548, 'api': ['net', 'web3', 'eth', 'arb']})
        validate_config(role, config)
        write_json(path, config, fresh=False)
        os.chown(path, 1000, 1000)
    for name in ('jwt.hex', 'val_jwt.hex'):
        path = config_dir / name
        path.write_text(os.urandom(32).hex())
        path.chmod(0o600); os.chown(path, 1000, 1000)


def create_role(runtime, value, role, args):
    memory, cpus = ROLES[role]
    image = GETH if role == 'geth' else NITRO
    command = base_args(value['name'] + '-' + role, value['instance'], image, memory, cpus, value['network'])
    command += ['--network-alias', role, '--mount', 'type=bind,src=' + str(runtime / 'config') + ',dst=/config,readonly',
                '--mount', 'type=bind,src=' + str(runtime / 'keystore') + ',dst=/keystore,readonly',
                '--mount', 'type=bind,src=' + str(runtime / 'keystore') + ',dst=/home/user/l1keystore,readonly',
                '--mount', 'type=bind,src=' + str(runtime / role) + ',dst=/data']
    if role != 'validation_node':
        internal = 8545 if role == 'geth' else 8547
        command += ['--publish', '127.0.0.1:' + str(value['ports'][role]) + ':' + str(internal)]
    else:
        command += ['--entrypoint', '/usr/local/bin/nitro-val']
    if role != 'geth':
        args = [*args, '--file-logging.file=/data/nitro.log', '--file-logging.max-size=5', '--file-logging.max-backups=2']
    command += [image, *args]
    container = docker(*command)
    value['containers'][role] = container
    write_json(runtime / 'runtime.json', value, fresh=False)
    docker('start', container)
    return container


def wait_rpc(runtime, value, role, chain_id, timeout=180):
    rpc = Rpc(value['ports'][role], 2)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            expect(int(rpc.call('eth_chainId'), 16) == chain_id, 'owned RPC chain changed')
            block = rpc.call('eth_getBlockByNumber', ['0x0', False])
            return block['hash']
        except (OSError, ValueError, RuntimeError, TimeoutError):
            state = json.loads(docker('inspect', value['containers'][role]))[0]['State']
            if not state['Running']:
                (runtime / (role + '-startup.log')).write_text(docker_logs(value['containers'][role]) + '\n')
                raise RuntimeError('owned role exited: ' + role)
            time.sleep(1)
    raise TimeoutError('owned RPC readiness deadline: ' + role)


def start(parent, assets, port):
    expect(os.geteuid() == 0, 'use explicit local root Docker without changing group membership')
    parent = safe(parent)
    expect(parent.is_dir() and parent.name == 'B3-peer-work' and parent.stat().st_uid == 0
           and not parent.stat().st_mode & 0o077, 'dedicated root-owned private B3-peer-work parent required')
    assets, _ = assets_info(assets)
    expect(type(port) is int and 1024 <= port <= 65530, 'invalid loopback base port')
    for number in range(port, port + 4):
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', number))
    expect(int(subprocess.check_output(['/usr/bin/df', '--output=avail', '-B1', str(parent)], text=True).splitlines()[-1]) >= DISK_LIMIT,
           '64 GiB free allocation required')
    suffix = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d-%H%M%S') + '-' + uuid.uuid4().hex[:8]
    runtime = parent / ('settlement-' + suffix)
    runtime.mkdir(mode=0o700)
    for name in ('config', 'keystore', *ROLES):
        path = runtime / name; path.mkdir(mode=0o700); os.chown(path, 1000, 1000)
    name = 'b3-nitro-settlement-' + suffix
    network = name + '-network'
    docker('network', 'create', '--label', LABEL + '=' + suffix,
           '--opt', 'com.docker.network.bridge.enable_ip_masquerade=false', network)
    value = {'format': FORMAT, 'instance': suffix, 'runtime': str(runtime), 'name': name, 'network': network,
        'assets': str(assets), 'assets_manifest_sha256': sha(assets / 'assets.json'), 'containers': {},
        'ports': dict(zip(('geth', 'sequencer', 'validator', 'poster'), range(port, port + 4))),
        'images': {'geth': GETH, 'nitro': NITRO, 'node': NODE}, 'module_root': MODULE_ROOT,
        'mainnet_enabled': False, 'ethereum_pos_verified': False, 'arbitrum_authority': False,
        'scope': 'owned-geth-development-parent-and-real-posted-Nitro-with-WASM-validator'}
    write_json(runtime / 'runtime.json', value)
    print(json.dumps({'stage': 'fresh runtime allocated', 'runtime': str(runtime), 'mainnet_enabled': False}), flush=True)
    try:
        helper(runtime, value, 'write-genesis', '/assets/testnode/scripts', ['node', 'index.js', 'write-geth-genesis-config'])
        helper(runtime, value, 'write-accounts', '/assets/testnode/scripts', ['node', 'index.js', 'write-accounts'])
        (runtime / 'geth/passphrase').write_text('passphrase\n')
        os.chown(runtime / 'geth/passphrase', 1000, 1000)
        # Fresh Geth-only initialization, scoped to this new private bind.
        args = base_args(name + '-helper-geth-init', suffix, GETH, 1, 1, network)
        args += ['--mount', 'type=bind,src=' + str(runtime / 'geth') + ',dst=/data',
                 '--mount', 'type=bind,src=' + str(runtime / 'config') + ',dst=/config,readonly', GETH,
                 'init', '--state.scheme', 'hash', '--datadir', '/data', '/config/geth_genesis.json']
        cid = docker(*args); docker('start', '--attach', cid, timeout=90)
        expect(json.loads(docker('inspect', cid))[0]['State']['ExitCode'] == 0, 'owned Geth init failed')
        create_role(runtime, value, 'geth', ['--keystore=/keystore', '--http', '--http.addr=0.0.0.0', '--http.port=8545',
            '--http.vhosts=*', '--http.api=engine,personal,eth,net,web3,debug', '--ws', '--ws.addr=0.0.0.0', '--ws.port=8546',
            '--ws.api=personal,eth,net,web3,debug,txpool', '--datadir=/data', '--allow-insecure-unlock', '--unlock=' + DEV,
            '--password=/data/passphrase', '--nodiscover', '--syncmode=full', '--state.scheme=hash', '--dev', '--dev.period=1',
            '--gcmode=archive', '--rpc.allow-unprotected-txs'])
        value['l1_genesis'] = wait_rpc(runtime, value, 'geth', 1337)
        write_json(runtime / 'runtime.json', value, fresh=False)
        for role in ('validator', 'sequencer', 'l2owner'):
            helper(runtime, value, 'fund-' + role, '/assets/testnode/scripts',
                   ['node', 'index.js', '--l1url', 'ws://geth:8546', 'send-l1', '--ethamount', '1000', '--to', role, '--wait'])
        helper(runtime, value, 'chain-config', '/assets/testnode/scripts',
               ['node', 'index.js', '--l2owner', OWNER, 'write-l2-chain-config'])
        helper(runtime, value, 'deploy-rollup', '/assets/contracts',
               ['node_modules/.bin/hardhat', '--config', 'b3-hardhat.config.ts', 'run', '--no-compile',
                'scripts/local-deployment/deployCreatorAndCreateRollup.ts'],
               {'PARENT_CHAIN_RPC': 'http://geth:8545', 'PARENT_CHAIN_ID': '1337', 'CHILD_CHAIN_NAME': 'b3-owned-settlement',
                'MAX_DATA_SIZE': '117964', 'OWNER_ADDRESS': OWNER, 'WASM_MODULE_ROOT': MODULE_ROOT,
                'SEQUENCER_ADDRESS': SEQUENCER, 'AUTHORIZE_VALIDATORS': '10',
                'CHILD_CHAIN_CONFIG_PATH': '/config/l2_chain_config.json', 'CHAIN_DEPLOYMENT_INFO': '/config/deployment.json',
                'CHILD_CHAIN_INFO': '/config/l2_chain_info.json',
                # Public, valueless upstream fixture key; never user credentials.
                'DEPLOYER_PRIVKEY': '0xdc04c5399f82306ec4b4d654a342f40e2e0620fe39950d967e1e574b32d4dd36'})
        helper(runtime, value, 'node-config', '/assets/testnode/scripts',
               ['node', 'index.js', '--l1url', 'http://geth:8545', '--validationNodeUrl', 'ws://validation_node:8549', 'write-config'])
        write_role_configs(runtime)
        create_role(runtime, value, 'sequencer', ['--conf.file=/config/sequencer_config.json', '--persistent.chain=/data', '--persistent.global-config=/data'])
        value['l2_genesis'] = wait_rpc(runtime, value, 'sequencer', 412346)
        # Real delayed Inbox ETH deposit, using only the upstream public funnel
        # fixture. This helper runs before the remaining 9 GiB of steady roles.
        helper(runtime, value, 'bridge-gas', '/assets/testnode/scripts',
               ['node', 'index.js', '--l1url', 'ws://geth:8546', '--l2url', 'ws://sequencer:8548',
                'bridge-funds', '--ethamount', '100', '--from', 'funnel', '--times', '1', '--threads', '1', '--wait'])
        expect(int(Rpc(value['ports']['sequencer']).call('eth_getBalance', [DEV, 'latest']), 16) >= 100 * 10 ** 18,
               'real Inbox gas fixture deposit did not arrive')
        create_role(runtime, value, 'validation_node', ['--conf.file=/config/validation_node_config.json', '--persistent.chain=/data',
            '--persistent.global-config=/data', '--validation.jit.workers=1', '--validation.arbitrator.workers=1',
            '--validation.arbitrator.output-path=/data/validation-output'])
        create_role(runtime, value, 'poster', ['--conf.file=/config/poster_config.json', '--persistent.chain=/data', '--persistent.global-config=/data'])
        create_role(runtime, value, 'validator', ['--conf.file=/config/validator_config.json', '--persistent.chain=/data', '--persistent.global-config=/data'])
        expect(wait_rpc(runtime, value, 'poster', 412346) == value['l2_genesis'], 'poster genesis differs')
        expect(wait_rpc(runtime, value, 'validator', 412346) == value['l2_genesis'], 'validator genesis differs')
        value['ready'] = True
        write_json(runtime / 'runtime.json', value, fresh=False)
        write_json(runtime / 'ready.json', value)
        return value
    except BaseException:
        for cid in value['containers'].values():
            if json.loads(docker('inspect', cid))[0]['State']['Running']:
                docker('stop', '--time', '30', cid)
        raise


def load(runtime):
    runtime = safe(runtime)
    value = read_json(runtime / 'runtime.json')
    expect(value['format'] == FORMAT and value['runtime'] == str(runtime) and value['mainnet_enabled'] is False
           and value['ethereum_pos_verified'] is False and value['arbitrum_authority'] is False
           and value['images'] == {'geth': GETH, 'nitro': NITRO, 'node': NODE}, 'owned runtime binding refused')
    expect(re.fullmatch(r'b3-nitro-settlement-[0-9]{8}-[0-9]{6}-[0-9a-f]{8}', value['name']), 'owned runtime name refused')
    for role, cid in value['containers'].items():
        expect(role in ROLES, 'unknown owned role')
        info = json.loads(docker('inspect', cid))[0]
        memory, cpus = ROLES[role]
        host = info['HostConfig']
        expected_ports = {} if role == 'validation_node' else {
            ('8545/tcp' if role == 'geth' else '8547/tcp'):
                [{'HostIp': '127.0.0.1', 'HostPort': str(value['ports'][role])}]}
        expect(info['Name'] == '/' + value['name'] + '-' + role and info['Config']['Labels'][LABEL] == value['instance']
               and info['Config']['Image'] == (GETH if role == 'geth' else NITRO)
               and host['Memory'] == memory * GIB and host['MemorySwap'] == memory * GIB
               and host['NanoCpus'] == cpus * 10 ** 9 and host['CapDrop'] == ['ALL'] and not host['Privileged']
               and host['ReadonlyRootfs'] and host['NetworkMode'] == value['network'] and host['PidMode'] == ''
               and (host.get('PortBindings') or {}) == expected_ports,
               'owned role ownership/resource boundary changed')
        mounts = {item['Destination']: item for item in info['Mounts'] if item['Type'] == 'bind'}
        expect(set(mounts) == {'/config', '/keystore', '/home/user/l1keystore', '/data'}
               and mounts['/config']['Source'] == str(runtime / 'config') and not mounts['/config']['RW']
               and mounts['/keystore']['Source'] == str(runtime / 'keystore') and not mounts['/keystore']['RW']
               and mounts['/home/user/l1keystore']['Source'] == str(runtime / 'keystore') and not mounts['/home/user/l1keystore']['RW']
               and mounts['/data']['Source'] == str(runtime / role) and mounts['/data']['RW'],
               'owned private bind layout changed')
    network = json.loads(docker('network', 'inspect', value['network']))[0]
    expect(network['Labels'][LABEL] == value['instance']
           and network['Options']['com.docker.network.bridge.enable_ip_masquerade'] == 'false', 'owned network boundary changed')
    return runtime, value


def status(runtime):
    runtime, value = load(runtime)
    return {**value, 'running': {role: json.loads(docker('inspect', cid))[0]['State']['Running']
                                for role, cid in value['containers'].items()}, 'runtime_bytes': used_bytes(runtime)}


def stop(runtime):
    runtime, value = load(runtime)
    for role in ('validator', 'poster', 'sequencer', 'validation_node', 'geth'):
        if role in value['containers']:
            cid = value['containers'][role]
            if json.loads(docker('inspect', cid))[0]['State']['Running']:
                docker('stop', '--time', '60', cid, timeout=90)
    return status(runtime)


def resume(runtime):
    runtime, value = load(runtime)
    expect(value.get('ready') is True and set(value['containers']) == set(ROLES), 'only the original complete runtime can resume')
    for role in ('geth', 'validation_node', 'sequencer', 'poster', 'validator'):
        cid = value['containers'][role]
        if not json.loads(docker('inspect', cid))[0]['State']['Running']:
            docker('start', cid)
        if role == 'geth':
            expect(wait_rpc(runtime, value, role, 1337) == value['l1_genesis'], 'original L1 genesis changed')
        elif role != 'validation_node':
            expect(wait_rpc(runtime, value, role, 412346) == value['l2_genesis'], 'original L2 genesis changed')
    return status(runtime)


def serve(runtime, seconds):
    expect(type(seconds) is int and 1 <= seconds <= 12 * 3600, 'bounded runtime duration required')
    runtime, value = load(runtime)
    expect(value.get('ready') is True, 'runtime did not complete initialization')
    rpc = Rpc(value['ports']['geth'])
    deadline = time.monotonic() + seconds
    print(json.dumps({'serving': str(runtime), 'seconds': seconds, 'mainnet_enabled': False}), flush=True)
    try:
        while time.monotonic() < deadline:
            expect(all(status(runtime)['running'].values()), 'owned settlement role exited')
            expect(int(rpc.call('eth_chainId'), 16) == 1337, 'owned L1 chain changed')
            expect(rpc.call('eth_getBlockByNumber', ['0x0', False])['hash'] == value['l1_genesis'], 'owned L1 genesis changed')
            time.sleep(2)
    finally:
        stop(runtime)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    begin = commands.add_parser('start')
    begin.add_argument('--runtime-parent', type=Path, required=True)
    begin.add_argument('--assets', type=Path, required=True)
    begin.add_argument('--port', type=int, default=18550)
    begin.add_argument('--serve-seconds', type=int, default=0)
    for name in ('status', 'stop', 'serve', 'resume'):
        item = commands.add_parser(name); item.add_argument('--runtime', type=Path, required=True)
        if name == 'serve':
            item.add_argument('--seconds', type=int, default=28800)
        if name == 'resume':
            item.add_argument('--serve-seconds', type=int, default=0)
    args = parser.parse_args()
    if args.command == 'start':
        result = start(args.runtime_parent, args.assets, args.port)
    elif args.command == 'serve':
        result = serve(args.runtime, args.seconds)
    else:
        result = globals()[args.command](args.runtime)
    if result is not None:
        print(json.dumps(result, sort_keys=True), flush=True)
        if args.command in ('start', 'resume') and args.serve_seconds:
            serve(Path(result['runtime']), args.serve_seconds)


if __name__ == '__main__':
    main()
