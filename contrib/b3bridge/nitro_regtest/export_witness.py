#!/usr/bin/env python3
"""Export genuine owned Nitro bytes; this exporter grants no source authority."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat

from devnet import load, safe_path, storage_bytes, write_json
from local_rpc import LocalNitroRpc, raw_hex, unique_object

MAX_BYTES = 32 * 1024 * 1024
MAX_ITEMS = 4096


def account_request(value):
    if not isinstance(value, dict) or set(value) != {'address', 'storage_keys'}:
        raise ValueError('account request shape refused')
    raw_hex(value['address'], size=20)
    keys = value['storage_keys']
    if not isinstance(keys, list) or len(keys) > 64 or len(set(keys)) != len(keys):
        raise ValueError('storage key count or duplication refused')
    for key in keys:
        raw_hex(key, size=32)
    return {'address': value['address'].lower(), 'storage_keys': [key.lower() for key in keys]}


class Objects:
    def __init__(self, path):
        self.path = path
        path.mkdir(mode=0o700)
        self.total, self.count = 0, 0

    def put(self, raw):
        if not isinstance(raw, bytes) or len(raw) > 16 * 1024 * 1024:
            raise ValueError('raw witness object exceeds bound')
        digest = hashlib.sha256(raw).hexdigest()
        target = self.path / digest
        if target.exists():
            meta = target.lstat()
            if (not stat.S_ISREG(meta.st_mode) or meta.st_nlink != 1
                    or meta.st_size != len(raw) or target.read_bytes() != raw):
                raise ValueError('CAS object conflict or unsafe path')
            return digest
        if self.total + len(raw) > MAX_BYTES or self.count + 1 > 32768:
            raise ValueError('raw witness export exceeds aggregate bound')
        fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'wb') as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        self.total += len(raw)
        self.count += 1
        return digest


def export(runtime, output, *, block_hash=None, accounts=()):
    runtime, config, info = load(runtime)
    if not info['State']['Running']:
        raise ValueError('owned Nitro node is stopped')
    # Docker inspection needs root; acquisition and all exported data do not.
    owner = runtime.stat()
    if os.geteuid() == 0 and owner.st_uid != 0:
        os.setgroups([])
        os.setgid(owner.st_gid)
        os.setuid(owner.st_uid)
    ready_path = runtime / 'ready.json'
    ready = json.loads(ready_path.read_text(), object_pairs_hook=unique_object)
    if ready.get('mainnet_enabled') is not False or ready.get('l1_settlement_verified') is not False:
        raise ValueError('unsafe owned Nitro provenance')
    if len(accounts) > 16:
        raise ValueError('too many account requests')
    accounts = [account_request(value) for value in accounts]
    if len({value['address'] for value in accounts}) != len(accounts):
        raise ValueError('duplicate account request')
    rpc = LocalNitroRpc(config['port'])
    chain = int(rpc.call('eth_chainId'), 16)
    genesis = rpc.call('eth_getBlockByNumber', ['0x0', False])
    if chain != ready['chain_id'] or genesis['hash'].lower() != ready['genesis_hash'].lower():
        raise ValueError('owned Nitro genesis or chain identity changed')
    if chain in (1, 42161):
        raise ValueError('live chain identity refused')
    if block_hash is None:
        block = rpc.call('eth_getBlockByNumber', ['latest', False])
        block_hash = block['hash']
    else:
        raw_hex(block_hash, size=32)
        block = rpc.call('eth_getBlockByHash', [block_hash, False])
    if not isinstance(block, dict) or block.get('hash', '').lower() != block_hash.lower():
        raise ValueError('selected block unavailable')
    transactions = block.get('transactions')
    if not isinstance(transactions, list) or len(transactions) > MAX_ITEMS:
        raise ValueError('transaction count exceeds bounded export')
    output = safe_path(output, existing=False)
    output.mkdir(mode=0o700)
    objects = Objects(output / 'objects')
    raw_header = raw_hex(rpc.call('debug_getRawHeader', [block_hash]))
    raw_block = raw_hex(rpc.call('debug_getRawBlock', [block_hash]))
    receipts = rpc.call('debug_getRawReceipts', [block_hash])
    if not isinstance(receipts, list) or len(receipts) != len(transactions):
        raise ValueError('complete transaction/receipt count mismatch')
    receipt_digests = [objects.put(raw_hex(value)) for value in receipts]
    tx_digests = []
    for index in range(len(transactions)):
        value = rpc.call('eth_getRawTransactionByBlockHashAndIndex', [block_hash, hex(index)])
        encoded = raw_hex(value)
        if not encoded:
            raise ValueError('missing indexed transaction bytes')
        tx_digests.append(objects.put(encoded))
    proofs = []
    for request in accounts:
        result = rpc.call('eth_getProof', [request['address'], request['storage_keys'],
                                         {'blockHash': block_hash, 'requireCanonical': True}])
        if (not isinstance(result, dict) or result.get('address', '').lower() != request['address']
                or not isinstance(result.get('accountProof'), list)
                or not isinstance(result.get('storageProof'), list)
                or len(result['storageProof']) != len(request['storage_keys'])):
            raise ValueError('account proof response shape refused')
        expected_keys = {int(key, 16) for key in request['storage_keys']}
        actual_keys = [int(value.get('key', '-1'), 16) for value in result['storageProof']]
        if len(set(actual_keys)) != len(actual_keys) or set(actual_keys) != expected_keys:
            raise ValueError('storage proof keys differ from exact request')
        proofs.append({'address': request['address'],
                       'nodes': [objects.put(raw_hex(node)) for node in result['accountProof']],
                       'storage': [{'slot': '0x' + format(int(value['key'], 16), '064x'),
                                    'nodes': [objects.put(raw_hex(node)) for node in value['proof']]}
                                   for value in result['storageProof']]})
    canonical = rpc.call('eth_getBlockByNumber', [block['number'], False])
    final_genesis = rpc.call('eth_getBlockByNumber', ['0x0', False])
    if canonical['hash'].lower() != block_hash.lower() or final_genesis['hash'].lower() != ready['genesis_hash'].lower():
        raise ValueError('owned Nitro canonical source changed during export')
    manifest = {'format': 'b3-owned-nitro-raw-export/1',
                'source': {'kind': 'owned-regtest', 'chain_id': chain,
                           'genesis_hash': ready['genesis_hash']},
                'image': config['image'], 'nitro_source_commit': config['nitro_source_commit'],
                'header': {'number': int(block['number'], 16), 'hash': block_hash,
                           'parent_hash': block['parentHash'], 'state_root': block['stateRoot'],
                           'transactions_root': block['transactionsRoot'],
                           'receipts_root': block['receiptsRoot']},
                'block': {'header_sha256': objects.put(raw_header),
                          'raw_block_sha256': objects.put(raw_block),
                          'receipt_sha256': receipt_digests, 'transaction_sha256': tx_digests},
                'accounts': proofs, 'raw_object_bytes': objects.total,
                'raw_object_count': objects.count,
                'availability_transport': 'owned-loopback-nitro-rpc',
                'external_rpc_used': False, 'native_verified': False,
                'l1_settlement_verified': False, 'source_authority': False,
                'mainnet_enabled': False}
    write_json(output / 'manifest.json', manifest)
    directory = os.open(objects.path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(directory)
    finally:
        os.close(directory)
    storage_bytes(runtime)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--block-hash')
    parser.add_argument('--accounts', type=Path,
                        help='JSON array of exact address/storage_keys requests')
    args = parser.parse_args()
    accounts = json.loads(safe_path(args.accounts).read_text(), object_pairs_hook=unique_object) if args.accounts else []
    value = export(args.runtime, args.output, block_hash=args.block_hash, accounts=accounts)
    print(json.dumps(value, sort_keys=True))


if __name__ == '__main__':
    main()
