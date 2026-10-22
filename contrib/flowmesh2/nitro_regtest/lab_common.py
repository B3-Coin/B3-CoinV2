#!/usr/bin/env python3
"""Separate, disabled-mainnet OWNED Nitro lab helpers. No public provider path."""
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
sys.path[:0] = [str(HERE.parent / 'regtest'), str(REPO / 'contrib/b3bridge/arbitrum_history'),
               str(REPO / 'contrib/b3bridge/nitro_regtest')]
from custody import Cast, CustodyError, atomic_json, expect, hex_value, address
from local_process import JsonProcess, raw_hex
from local_rpc import LocalNitroRpc, unique_object

PROFILE = 'fm2-owned-nitro-usdt0-lab/1'
CHAIN = 412346
IMPL_SLOT = '0x360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc'
ADMIN_SLOT = '0xb53127684a568b3173ae13b9f8a6016e243e63b6e8ee1178d6a717850b5d6103'
# PUBLIC, valueless Nitro --dev fixture, published by OffchainLabs. It is never
# a user's/private operator identity. https://github.com/OffchainLabs/nitro-devnode/blob/main/run-dev-node.sh
LAB_PUBLIC_DEV_KEY = '0xb6b15c8cb491557369f3c7d2c287b053eb229daa9c22138887752191c9520659'
WITHDRAWAL_ABI = '(bytes32,bytes32,bytes32,uint256,address,uint64,bytes32,uint64)'


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'))


def private_new(path, value):
    path = Path(path)
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    data = (canonical(value) + '\n').encode()
    if path.exists():
        expect(path.read_bytes() == data, 'immutable lab record changed')
        return
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)


def raw_file(path, maximum):
    path = Path(path)
    import stat
    metadata = path.lstat()
    expect(stat.S_ISREG(metadata.st_mode) and metadata.st_nlink == 1 and metadata.st_size <= maximum,
           'unsafe or oversized lab input')
    with path.open('rb') as stream:
        value = stream.read(maximum + 1)
    expect(len(value) == metadata.st_size and len(value) <= maximum, 'lab input changed or exceeded bound')
    return value


def owned_b3_control(cli, node, operation, block_hash):
    """Two test controls, only against this fixture's already running B3 node."""
    expect(operation in ('invalidateblock', 'reconsiderblock') and
           isinstance(block_hash, str) and re.fullmatch('[0-9a-f]{64}', block_hash) is not None,
           'unsupported owned B3 test control')
    expect(node['process'].process.poll() is None, 'owned B3 process is not running')
    node['rpc'].assert_regtest()
    command = [str(cli), '-regtest', '-datadir=' + str(node['directory']),
        '-rpcconnect=127.0.0.1', '-rpccookiefile=' + str(node['rpc'].cookie_file),
        '-rpcport=' + node['rpc'].url.rsplit(':', 1)[1], operation, block_hash]
    checked = subprocess.run(command, capture_output=True, timeout=20)
    expect(checked.returncode == 0 and len(checked.stdout) <= 65536 and len(checked.stderr) <= 65536,
           'owned B3 test control failed')


class NitroLabChain:
    """Owned local full-node trust. This is NOT an L1/Arbitrum source capability."""
    def __init__(self, runtime, tools, evidence, source_root=REPO):
        self.runtime, self.tools, self.evidence = Path(runtime).resolve(strict=True), Path(tools), Path(evidence)
        self.source = Path(source_root)
        self.cast = Cast(self.tools / 'cast')
        self.evidence.mkdir(mode=0o700, parents=True, exist_ok=True)
        status_command = [sys.executable, str(self.source / 'contrib/b3bridge/nitro_regtest/devnet.py'),
                          'status', '--runtime', str(self.runtime)]
        if os.geteuid() != 0:
            status_command[:0] = ['sudo', '-n']
        checked = subprocess.run(status_command, capture_output=True, timeout=30)
        expect(checked.returncode == 0 and len(checked.stdout) <= 65536, 'owned Nitro Docker provenance unavailable')
        self.provenance = json.loads(checked.stdout, object_pairs_hook=unique_object)
        expect(self.provenance.get('running') is True and self.provenance.get('mainnet_enabled') is False and
               self.provenance.get('l1_settlement_verified') is False, 'owned Nitro provenance refused')
        self.port = int(self.provenance['port'])
        self.url = 'http://127.0.0.1:' + str(self.port)
        self.read = LocalNitroRpc(self.port)
        ready = json.loads((self.runtime / 'ready.json').read_text(), object_pairs_hook=unique_object)
        self.genesis = hex_value(ready['genesis_hash'], 32)
        expect(ready['chain_id'] == CHAIN, 'lab supports only the separate owned Nitro chain412346')
        self.counter, self.fenced = 0, False
        self.identity()
        self.dev_address = address(self.cast.run('wallet', 'address', '--private-key', LAB_PUBLIC_DEV_KEY))

    def identity(self):
        expect(not self.fenced, 'ambiguous local transaction fences this runner; retain evidence')
        chain = int(self.read.call('eth_chainId'), 16)
        genesis = self.read.call('eth_getBlockByNumber', ['0x0', False])
        expect(chain == CHAIN and hex_value(genesis['hash'], 32) == self.genesis, 'Nitro lab chain/genesis changed')
        return chain

    def extra_read(self, method, params):
        expect(method in ('eth_call', 'eth_getStorageAt'), 'unsupported separate lab read method')
        self.counter += 1
        payload = canonical({'jsonrpc': '2.0', 'id': self.counter, 'method': method, 'params': params}).encode()
        expect(len(payload) <= 65536, 'lab read exceeds bound')
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=15)
        try:
            connection.request('POST', '/', body=payload, headers={'Content-Type': 'application/json', 'Connection': 'close'})
            reply = connection.getresponse()
            expect(reply.status == 200, 'owned lab read HTTP refusal')
            value = reply.read(4_194_305)
            expect(len(value) <= 4_194_304, 'owned lab read exceeds bound')
            decoded = json.loads(value, object_pairs_hook=unique_object)
            expect(set(decoded) == {'jsonrpc', 'id', 'result'} and decoded['jsonrpc'] == '2.0' and
                   type(decoded['id']) is int and decoded['id'] == self.counter, 'owned lab read identity/refusal')
            return decoded['result']
        finally:
            connection.close()

    def call(self, target, signature, *arguments):
        data = self.cast.calldata(signature, *arguments)
        return hex_value(self.extra_read('eth_call', [{'to': address(target), 'data': data}, 'latest']))

    def number(self, target, signature, *arguments):
        result = self.call(target, signature, *arguments)
        expect(len(result) == 66, 'lab uint ABI length differs')
        return int(result, 16)

    def code_hash(self, target):
        code = hex_value(self.read.call('eth_getCode', [address(target), 'latest']))
        expect(code != '0x', 'expected deployed code missing')
        return self.cast.keccak(code)

    def point(self):
        self.identity()
        head = self.read.call('eth_getBlockByNumber', ['latest', False])
        height = int(head['number'], 16) - 1
        expect(height >= 0, 'local confirmation successor unavailable')
        block = self.read.call('eth_getBlockByNumber', [hex(height), False])
        return {'height': int(block['number'], 16), 'hash': hex_value(block['hash'], 32), 'minimum_confirmations': 1}

    def check_point(self, value):
        self.identity()
        expect(isinstance(value, dict) and set(value) == {'height', 'hash', 'minimum_confirmations'} and
               type(value['height']) is int and value['height'] >= 0 and
               type(value['minimum_confirmations']) is int and value['minimum_confirmations'] == 1,
               'exact local checkpoint policy required')
        head = self.read.call('eth_getBlockByNumber', ['latest', False])
        expect(int(head['number'], 16) >= value['height'] + 1, 'local checkpoint lacks one canonical successor')
        block = self.read.call('eth_getBlockByNumber', [hex(int(value['height'])), False])
        expect(isinstance(block, dict) and hex_value(block['hash'], 32) == value['hash'], 'owned source checkpoint changed')
        return block

    def send(self, target, data='0x', *, value=0, create=False):
        self.identity()
        data = hex_value(data)
        intent = {'chain_id': CHAIN, 'genesis': self.genesis, 'from': self.dev_address,
                  'to': None if create else address(target), 'data': data, 'value': int(value)}
        identity = uuid.uuid4().hex
        private_new(self.evidence / (identity + '.intent.json'), intent)
        command = [str(self.tools / 'cast'), 'send', '--rpc-url', self.url, '--chain', str(CHAIN), '--private-key', LAB_PUBLIC_DEV_KEY,
                   '--json', '--value', str(int(value))]
        command += ['--create', data] if create else [address(target), '--data', data]
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=90)
            if result.returncode:
                private_new(self.evidence / (identity + '.failure.json'), {'returncode': result.returncode,
                    'stderr': result.stderr[-4000:], 'stdout': result.stdout[-4000:]})
            expect(result.returncode == 0 and len(result.stdout) <= 4_194_304, 'owned Nitro fixture transaction failed')
            receipt = json.loads(result.stdout, object_pairs_hook=unique_object)
            if receipt.get('schema_version') == 1:
                expect(receipt.get('success') is True and isinstance(receipt.get('data'), dict), 'cast transaction envelope refused')
                receipt = receipt['data']
            status = receipt['status']
            expect(int(status, 16) == 1 if isinstance(status, str) else status == 1, 'owned Nitro transaction reverted')
            tx = hex_value(receipt['transactionHash'], 32)
            canonical_receipt = self.read.call('eth_getTransactionReceipt', [tx])
            expect(isinstance(canonical_receipt, dict) and canonical_receipt['blockHash'].lower() == receipt['blockHash'].lower(),
                   'local transaction receipt is not canonical')
            block = self.read.call('eth_getBlockByNumber', [canonical_receipt['blockNumber'], False])
            expect(block['hash'].lower() == canonical_receipt['blockHash'].lower(), 'local transaction block replaced')
            private_new(self.evidence / (identity + '.receipt.json'), canonical_receipt)
            return canonical_receipt
        except Exception:
            self.fenced = True  # Never retry an ambiguous transaction with a fresh nonce.
            raise

    def invoke(self, target, signature, *arguments):
        return self.send(target, self.cast.calldata(signature, *arguments))

    def deploy(self, bytecode, constructor='0x'):
        receipt = self.send(None, hex_value(bytecode) + hex_value(constructor)[2:], create=True)
        return address(receipt['contractAddress'])

    def bind_usdt0(self, token, implementation, artifact):
        expect(self.code_hash(token) == artifact['proxy_runtime_hash'] and
               self.code_hash(implementation) == artifact['implementation_runtime_hash'], 'exact USDT0 runtime mismatch')
        word = self.extra_read('eth_getStorageAt', [token, IMPL_SLOT, 'latest'])
        expect(hex_value(word, 32) == '0x' + '00' * 12 + raw_hex(implementation, 20), 'fresh USDT0 proxy implementation changed')
        expect(self.number(token, 'decimals()') == 6, 'USDT0 decimals changed')


def deploy_usdt0(chain):
    artifact = json.loads((HERE / 'usdt0_bytecode.json').read_text())
    expect(artifact['format'] == 'b3-owned-nitro-usdt0-bytecode/1' and artifact['mainnet_enabled'] is False,
           'unrecognized USDT0 code artifact')
    implementation = chain.deploy(artifact['implementation_creation'])
    expect(chain.code_hash(implementation) == artifact['implementation_runtime_hash'], 'USDT0 implementation code hash differs')
    # A separate public test address is proxy admin. The real constructor's
    # delegatecall initializes the fresh proxy with the development sender as owner.
    admin = chain.cast.test_address(20001)
    init = chain.cast.calldata('initialize(string,string,uint8)', 'USD₮0', 'USDT0', 6)
    arguments = chain.cast.abi_encode('f(address,address,bytes)', implementation, admin, init)
    token = chain.deploy(artifact['proxy_creation'], arguments)
    chain.bind_usdt0(token, implementation, artifact)
    expect(chain.call(token, 'owner()')[-40:] == raw_hex(chain.dev_address, 20), 'fresh token owner differs')
    chain.invoke(token, 'setOFTContract(address)', chain.dev_address)
    expect(chain.call(token, 'oftContract()')[-40:] == raw_hex(chain.dev_address, 20), 'local issuer role was not set')
    binding = {'source_chain_id': CHAIN, 'source_genesis_hash': raw_hex(chain.genesis), 'quote_token': raw_hex(token, 20),
               'quote_token_code_hash': raw_hex(artifact['proxy_runtime_hash']),
               'quote_implementation': raw_hex(implementation, 20),
               'quote_implementation_code_hash': raw_hex(artifact['implementation_runtime_hash'])}
    return token, implementation, binding


def withdrawal_tuple(value):
    return '(' + ','.join(str(value[key]) for key in
        ('id', 'account', 'asset', 'amount', 'recipient', 'slot', 'stateRoot', 'validUntil')) + ')'


def export_block(source_root, runtime, output, block_hash, accounts):
    request = Path(output).with_suffix('.accounts.json')
    private_new(request, accounts)
    command = [sys.executable, str(Path(source_root) / 'contrib/b3bridge/nitro_regtest/export_witness.py'),
               '--runtime', str(runtime), '--output', str(output), '--block-hash', block_hash,
               '--accounts', str(request)]
    if os.geteuid() != 0:
        command[:0] = ['sudo', '-n']
    result = subprocess.run(command, capture_output=True, timeout=60)
    expect(result.returncode == 0, 'owned raw witness export failed')
    return json.loads((Path(output) / 'manifest.json').read_text())
