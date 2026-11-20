#!/usr/bin/env python3
"""Bounded untrusted Arbitrum Sepolia L2 fixture availability.

Public HTTPS requires explicit opt-in. This collector supplies consensus bytes
to the separate native typed settlement verifier; no endpoint, JSON flag,
helper output or successful acquisition creates a source capability.
"""
import argparse
import hashlib
import http.client
import ipaddress
import json
import os
from pathlib import Path
import re
import socket
import ssl
import stat
import subprocess
import sys
import time
import urllib.parse

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT.parent / 'arbitrum_history'))
from proof_bundle import Assertion, Proof, BlockWitness, IndexedRecord, AccountWitness, StorageWitness, encode_proof

CHAIN_ID = 421614
MAX_REPLY = 32 * 1024 * 1024
MAX_AGGREGATE = 128 * 1024 * 1024
MAX_CALLS = 20000
MAX_PAGES = 64
PUBLIC_L2_HOSTS = frozenset(('sepolia-rollup.arbitrum.io', 'arbitrum-sepolia.drpc.org',
                             'arbitrum-sepolia-rpc.publicnode.com'))


def require(ok, message):
    if not ok:
        raise ValueError(message)


def unique(pairs):
    out = {}
    for key, value in pairs:
        require(key not in out, 'duplicate JSON field')
        out[key] = value
    return out


def strict_json(data):
    def bad(_value):
        raise ValueError('nonfinite JSON field')
    return json.loads(data, object_pairs_hook=unique, parse_constant=bad)


def raw(value, size=None, maximum=MAX_REPLY):
    require(type(value) is str and re.fullmatch(r'0x(?:[0-9a-fA-F]{2})*', value) is not None, 'canonical hex bytes required')
    out = bytes.fromhex(value[2:])
    require(len(out) <= maximum and (size is None or len(out) == size), 'raw field bound or size refused')
    return out


def integer(value, maximum=2 ** 64 - 1):
    require(type(value) is int and 0 <= value <= maximum, 'bounded unsigned integer required')
    return value


def safe(path, existing=True):
    path = Path(path).absolute()
    require('..' not in path.parts, 'path traversal refused')
    for part in [*reversed(path.parents), path]:
        try:
            info = part.lstat()
        except FileNotFoundError:
            require(part == path and not existing, 'absent ancestor')
            continue
        require(not stat.S_ISLNK(info.st_mode), 'symbolic path refused')
        if part != path:
            require(stat.S_ISDIR(info.st_mode), 'nondirectory ancestor')
    return path


def read(path, maximum):
    path = safe(path); info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1 and info.st_size <= maximum, 'unsafe bounded file')
    value = path.read_bytes(); require(len(value) == info.st_size, 'input changed during read')
    return value


def write(path, data):
    require(type(data) is bytes and len(data) <= MAX_REPLY, 'bounded output bytes required')
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, 'wb') as stream:
        stream.write(data); stream.flush(); os.fsync(stream.fileno())
    return hashlib.sha256(data).hexdigest()


def write_json(path, value):
    return write(path, (json.dumps(value, sort_keys=True, indent=2) + '\n').encode())


class Objects:
    def __init__(self, path):
        self.path = path; path.mkdir(mode=0o700)
        self.bytes = self.count = 0

    def put(self, data):
        require(type(data) is bytes and len(data) <= 16 * 1024 * 1024, 'object size bound exceeded')
        digest = hashlib.sha256(data).hexdigest(); target = self.path / digest
        if target.exists():
            require(read(target, 16 * 1024 * 1024) == data, 'raw object conflict')
            return digest
        require(self.bytes + len(data) <= MAX_REPLY and self.count < 32768, 'aggregate object budget exceeded')
        write(target, data); self.bytes += len(data); self.count += 1
        return digest


def endpoint(value, allow_public_https):
    parsed = urllib.parse.urlsplit(value)
    require(parsed.scheme in ('http', 'https') and parsed.hostname and parsed.username is None and parsed.password is None
            and not parsed.query and not parsed.fragment, 'credential-free endpoint URL required')
    try:
        address = ipaddress.ip_address(parsed.hostname)
    except ValueError:
        address = None
    loopback = address is not None and address.is_loopback
    if not loopback:
        require(allow_public_https is True and parsed.scheme == 'https' and address is None,
                'public L2 HTTPS requires explicit opt-in; literal external addresses refused')
        require(parsed.hostname in PUBLIC_L2_HOSTS and parsed.port in (None,443)
                and parsed.path in ('','/','/rpc'), 'only reviewed no-key Arbitrum Sepolia L2 availability hosts allowed')
    else:
        require(parsed.scheme == 'http' and address.version == 4, 'owned availability uses literal IPv4 loopback HTTP')
    require(parsed.port is None or 1 <= parsed.port <= 65535, 'endpoint port refused')
    return parsed, not loopback


class Availability:
    METHODS = frozenset(('eth_chainId', 'eth_getBlockByNumber', 'eth_getBlockByHash', 'eth_getBlockReceipts',
                         'eth_getTransactionReceipt', 'eth_getProof'))

    def __init__(self, url, allow_public_https=False, seconds=300):
        self.parsed, self.public = endpoint(url, allow_public_https)
        self.url = url; self.sequence = self.received = 0
        self.deadline = time.monotonic() + integer(seconds, 900)

    def call(self, method, params=()):
        require(method in self.METHODS and self.sequence < MAX_CALLS, 'read-only bounded L2 availability method required')
        require(time.monotonic() < self.deadline, 'availability deadline exceeded')
        self.sequence += 1
        request = json.dumps({'jsonrpc': '2.0', 'id': self.sequence, 'method': method, 'params': list(params)}).encode()
        require(len(request) <= 65536, 'availability request exceeds bound')
        port = self.parsed.port or (443 if self.parsed.scheme == 'https' else 80)
        timeout = min(20, self.deadline - time.monotonic())
        connection = (http.client.HTTPSConnection(self.parsed.hostname, port, timeout=timeout, context=ssl.create_default_context())
                      if self.parsed.scheme == 'https' else http.client.HTTPConnection(self.parsed.hostname, port, timeout=timeout))
        try:
            connection.request('POST', self.parsed.path or '/', request,
                               headers={'Content-Type': 'application/json', 'Accept-Encoding': 'identity', 'Connection': 'close'})
            response = connection.getresponse()
            require(response.status == 200 and response.getheader('Content-Encoding', 'identity') == 'identity',
                    'L2 availability HTTP response refused; redirects are never followed')
            length = response.getheader('Content-Length')
            if length is not None:
                require(length.isdecimal() and int(length) <= MAX_REPLY, 'availability response exceeds bound')
            data = response.read(min(MAX_REPLY, MAX_AGGREGATE - self.received) + 1)
            self.received += len(data)
            require(len(data) <= MAX_REPLY and self.received <= MAX_AGGREGATE and time.monotonic() < self.deadline,
                    'availability byte/time budget exceeded')
            value = strict_json(data)
            require(type(value) is dict and value.get('jsonrpc') == '2.0' and type(value.get('id')) is int
                    and value['id'] == self.sequence, 'availability response identity refused')
            if 'error' in value:
                code = value['error'].get('code') if type(value['error']) is dict else None
                raise ValueError('L2 availability method refused: ' + method + ' code=' + str(code))
            require(set(value) == {'jsonrpc', 'id', 'result'}, 'availability result shape refused')
            return value['result']
        finally:
            connection.close()


class Codec:
    def __init__(self, binary):
        self.binary = safe(binary)
        manifest = strict_json(read(self.binary.parent / 'build-manifest.json', 1024 * 1024))
        require(manifest['format'] == 'b3-maintained-nitro-history-codec-build/1'
                and manifest['geth_commit'] == '5eee8f546e7602b7deeef7909f291bbebc77e233'
                and manifest['geth_module_h1'] == 'h1:KkMlxmymHsJXSUXubbnfgLVrScmG6vIBce9JKAfjMhU='
                and manifest['binary_sha256'] == hashlib.sha256(read(self.binary, 128 * 1024 * 1024)).hexdigest()
                and manifest['mainnet_enabled'] is False and manifest['source_authority'] is False, 'pinned maintained codec provenance refused')
        self.manifest = manifest

    def encode(self, block, receipts=None):
        data = json.dumps({'mode': 'header' if receipts is None else 'block', 'expected_hash': block['hash'],
                           'block': block, 'receipts': [] if receipts is None else receipts}, separators=(',', ':')).encode()
        require(len(data) <= MAX_REPLY, 'codec input exceeds bound')
        checked = subprocess.run([str(self.binary)], input=data, capture_output=True, timeout=30,
                                  env={'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8',
                                       'GOMAXPROCS': '2', 'GOMEMLIMIT': '512MiB'})
        require(checked.returncode == 0 and len(checked.stdout) <= 64 * 1024 * 1024,
                'maintained codec refused block: ' + checked.stderr.decode(errors='replace')[:500])
        result = strict_json(checked.stdout)
        require(result['format'] == 'b3-maintained-nitro-json-codec/1' and result['hash'].lower() == block['hash'].lower()
                and result['source_authority'] is False and result['mainnet_enabled'] is False,
                'codec result scope/hash refused')
        if receipts is not None:
            require(result['commitments_match'] is True, 'complete codec commitments differ')
        return result


def account_request(value):
    require(type(value) is dict and set(value) == {'address', 'storage_keys'}, 'exact account request required')
    raw(value['address'], 20); keys = value['storage_keys']
    require(type(keys) is list and len(keys) <= 64, 'bounded storage keys required')
    for key in keys:
        raw(key, 32)
    keys = [x.lower() for x in keys]
    require(len(set(keys)) == len(keys), 'bounded unique storage keys required')
    return {'address': value['address'].lower(), 'storage_keys': keys}


def state_proof(availability, request, block_hash, objects):
    result = availability.call('eth_getProof', [request['address'], request['storage_keys'],
                                               {'blockHash': block_hash, 'requireCanonical': True}])
    require(type(result) is dict and result['address'].lower() == request['address'] and len(result['accountProof']) <= 64
            and len(result['storageProof']) == len(request['storage_keys']), 'exact bounded account proof required')
    actual = [int(x['key'], 16) for x in result['storageProof']]
    require(len(set(actual)) == len(actual) and set(actual) == {int(x, 16) for x in request['storage_keys']}, 'account proof storage keys differ')
    witness = AccountWitness(raw(request['address'], 20), tuple(raw(n, maximum=8192) for n in result['accountProof']),
        tuple(StorageWitness(int(x['key'], 16).to_bytes(32, 'big'), tuple(raw(n, maximum=8192) for n in x['proof'])) for x in result['storageProof']))
    record = {'address': request['address'], 'nodes': [objects.put(n) for n in witness.nodes],
              'storage': [{'slot': '0x' + s.slot.hex(), 'nodes': [objects.put(n) for n in s.nodes]} for s in witness.storage]}
    return witness, record


def collect(availability, codec, output, *, settled_hash, previous_height, previous_hash, to_height, to_hash, accounts=()):
    raw(settled_hash, 32); raw(previous_hash, 32); raw(to_hash, 32)
    integer(previous_height); integer(to_height)
    require(0 < to_height - previous_height <= 128 and len(accounts) <= 16, 'bounded nonempty history range required')
    accounts = [account_request(x) for x in accounts]
    require(len({x['address'] for x in accounts}) == len(accounts), 'duplicate account request')
    require(int(availability.call('eth_chainId'), 16) == CHAIN_ID, 'Arbitrum Sepolia chain421614 required; owned/live identities refused')
    genesis = availability.call('eth_getBlockByNumber', ['0x0', False])
    require(type(genesis) is dict, 'genesis availability missing')
    raw(genesis['hash'], 32)
    output = safe(output, existing=False); output.mkdir(mode=0o700)
    objects = Objects(output / 'objects')

    def acquire(block_hash, complete=False):
        block = availability.call('eth_getBlockByHash', [block_hash, complete])
        require(type(block) is dict and block['hash'].lower() == block_hash.lower(), 'requested block unavailable')
        receipts = availability.call('eth_getBlockReceipts', [block_hash]) if complete else None
        if complete:
            require(type(receipts) is list and len(receipts) <= 8192, 'complete bounded receipts required')
        encoded = codec.encode(block, receipts)
        txs = tuple(IndexedRecord(i, raw(x, maximum=1024*1024)) for i, x in enumerate(encoded['transactions']))
        recs = tuple(IndexedRecord(i, raw(x, maximum=1024*1024)) for i, x in enumerate(encoded['receipts']))
        require(len(txs) == len(recs), 'codec record sets differ')
        witness = BlockWitness(raw(encoded['header'], maximum=4096), recs, txs)
        record = {'number': encoded['number'], 'hash': encoded['hash'], 'parent_hash': encoded['parent_hash'],
                  'header_sha256': objects.put(witness.header), 'receipts': [objects.put(x.encoded) for x in recs],
                  'transactions': [objects.put(x.encoded) for x in txs]}
        return witness, record

    settled, head = acquire(settled_hash)
    require(head['number'] >= to_height, 'requested endpoint newer than requested settled hash')
    write(output / 'settled-header.rlp', settled.header)
    pages = []; cursor = head
    while cursor['number'] > to_height:
        require(len(pages) < MAX_PAGES, 'ancestor page bound exceeded')
        page = []
        for _ in range(min(128, cursor['number'] - to_height)):
            _, older = acquire(cursor['parent_hash'])
            require(older['number'] + 1 == cursor['number'], 'ancestor heights differ')
            page.append(older); cursor = older
        pages.append(page)
    require(cursor['hash'].lower() == to_hash.lower(), 'requested endpoint not linked to requested settled hash')
    blocks, records = [], []; cursor_hash = to_hash
    for number in range(to_height, previous_height, -1):
        block, record = acquire(cursor_hash, complete=True)
        require(record['number'] == number, 'complete range heights differ')
        blocks.append(block); records.append(record); cursor_hash = record['parent_hash']
    require(cursor_hash.lower() == previous_hash.lower(), 'range predecessor differs')
    state, state_records = [], []
    for request in accounts:
        witness, record = state_proof(availability, request, to_hash, objects)
        state.append(witness); state_records.append(record)
    proof = encode_proof(Proof(Assertion(), (), tuple(blocks), tuple(state), ()))
    proof_sha = write(output / 'history-proof.bin', proof)
    write_json(output / 'ancestor-pages.json', {'format': 'b3-arbitrum-sepolia-ancestor-pages/1', 'pages': pages})
    require(int(availability.call('eth_chainId'), 16) == CHAIN_ID and
            availability.call('eth_getBlockByNumber', ['0x0', False])['hash'] == genesis['hash'], 'availability network changed')
    manifest = {'format': 'b3-arbitrum-sepolia-history-export/1',
        'source': {'kind': 'arbitrum-sepolia', 'chain_id': CHAIN_ID, 'reported_genesis_hash': genesis['hash']},
        'request': {'previous_height': previous_height, 'previous_hash': previous_hash, 'to_height': to_height, 'to_hash': to_hash},
        'requested_settled_hash': settled_hash, 'settled_header': head, 'ancestor_page_count': len(pages),
        'history_blocks': records, 'endpoint_accounts': state_records, 'history_proof_sha256': proof_sha,
        'codec_binary_sha256': codec.manifest['binary_sha256'], 'codec_geth_commit': codec.manifest['geth_commit'],
        'availability_endpoint': availability.url, 'availability_kind': 'public-L2-HTTPS-RPC-fixture' if availability.public else 'owned-loopback-L2-API-fixture',
        'availability_received_bytes': availability.received, 'availability_calls': availability.sequence,
        'raw_object_bytes': objects.bytes, 'raw_object_count': objects.count,
        'external_l2_rpc_used': availability.public, 'ethereum_rpc_used': False,
        'native_verified': False, 'rollup_settlement_verified': False, 'source_authority': False,
        'owned_canonical_derivation_qualified': False, 'public_rpc_free_history_qualified': False, 'mainnet_enabled': False}
    write_json(output / 'manifest.json', manifest)
    for directory in (objects.path, output):
        descriptor = os.open(directory, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--endpoint', default='http://127.0.0.1:18551')
    parser.add_argument('--allow-public-l2-https', action='store_true')
    parser.add_argument('--codec', type=Path, required=True); parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--settled-hash', required=True)
    parser.add_argument('--previous-height', type=int, required=True); parser.add_argument('--previous-hash', required=True)
    parser.add_argument('--to-height', type=int, required=True); parser.add_argument('--to-hash', required=True)
    parser.add_argument('--accounts', type=Path)
    args = parser.parse_args(); accounts = strict_json(read(args.accounts, 65536)) if args.accounts else []
    result = collect(Availability(args.endpoint, args.allow_public_l2_https), Codec(args.codec), args.output,
        settled_hash=args.settled_hash, previous_height=args.previous_height, previous_hash=args.previous_hash,
        to_height=args.to_height, to_hash=args.to_hash, accounts=accounts)
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
