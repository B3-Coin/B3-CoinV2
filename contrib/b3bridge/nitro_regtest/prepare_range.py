#!/usr/bin/env python3
"""Assemble genuine local Nitro exports without inventing an L1 assertion."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

from devnet import safe_path, write_json
from local_rpc import raw_hex, unique_object


def read_json(path):
    path = safe_path(path)
    meta = path.stat()
    if not path.is_file() or meta.st_nlink != 1 or meta.st_size > 4 * 1024 * 1024:
        raise ValueError('bounded plain JSON file required')
    return json.loads(path.read_text(), object_pairs_hook=unique_object)


def prepare(exports, request_path, output):
    # Sibling codec handles only untrusted bytes and never issues a capability.
    sys.path.insert(0, str(Path(__file__).absolute().parent.parent / 'arbitrum_history'))
    from proof_bundle import DirectoryObjects, assemble_proof, encode_proof
    if not 1 <= len(exports) <= 128:
        raise ValueError('bounded newest-to-oldest export list required')
    request = read_json(request_path)
    if (set(request) != {'previous_height', 'previous_hash', 'to_height', 'to_hash'}
            or any(type(request[k]) is not int or request[k] < 0
                   for k in ('previous_height', 'to_height'))
            or request['to_height'] - request['previous_height'] != len(exports)):
        raise ValueError('exact independent range request required')
    for key in ('previous_hash', 'to_hash'):
        raw_hex(request[key], size=32)
        if request[key] != request[key].lower():
            raise ValueError('lowercase requested hash required')
    values = [read_json(Path(path) / 'manifest.json') for path in exports]
    source = values[0]['source']
    if source['kind'] != 'owned-regtest' or source['chain_id'] in (1, 42161):
        raise ValueError('this assembler supports only an owned local Nitro chain')
    for index, value in enumerate(values):
        if (value['format'] != 'b3-owned-nitro-raw-export/1' or value['source'] != source
                or value['mainnet_enabled'] is not False or value['native_verified'] is not False
                or value['l1_settlement_verified'] is not False
                or value['header']['number'] != request['to_height'] - index):
            raise ValueError('export context or declared range differs')
        if index and values[index - 1]['header']['parent_hash'] != value['header']['hash']:
            raise ValueError('declared export ancestry differs')
    if (values[0]['header']['hash'] != request['to_hash']
            or values[-1]['header']['parent_hash'] != request['previous_hash']):
        raise ValueError('exports differ from requested endpoints')
    output = safe_path(output, existing=False)
    output.mkdir(mode=0o700)
    objects = output / 'objects'
    objects.mkdir(mode=0o700)
    total, visited = 0, 0
    for path in exports:
        for item in (safe_path(path) / 'objects').iterdir():
            visited += 1
            if visited > 32768:
                raise ValueError('raw object count exceeds bound')
            # DirectoryObjects performs nofollow/plain-file/size/digest checks.
            raw = DirectoryObjects(Path(path) / 'objects', source).read('availability', item.name, 16 * 1024 * 1024)
            target = objects / item.name
            if target.exists():
                if target.read_bytes() != raw:
                    raise ValueError('content-addressed object conflict')
                continue
            total += len(raw)
            if total > 32 * 1024 * 1024:
                raise ValueError('aggregate raw export exceeds bound')
            fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'wb') as stream:
                stream.write(raw)
                stream.flush()
                os.fsync(stream.fileno())
    zero = '0x' + '00' * 32
    manifest = {'format': 'b3-arbitrum-proof-manifest/1', 'mainnet_enabled': False,
                'writes_enabled': False, 'source': source,
                'assertion': {'parent_assertion': zero, 'block_hash': zero, 'send_root': zero,
                              'inbox_position': 0, 'position_in_message': 0, 'machine_status': 1,
                              'end_history_root': zero, 'inbox_accumulator': zero},
                'l1_accounts': [],
                'blocks': [{key: value['block'][key] for key in
                            ('header_sha256', 'receipt_sha256', 'transaction_sha256')}
                           for value in values],
                'confirmed_accounts': values[0]['accounts'],
                'historical_accounts': values[-1]['accounts']}
    proof = encode_proof(assemble_proof(DirectoryObjects(objects, source), manifest))
    fd = os.open(output / 'proof.bin', os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(proof)
        stream.flush()
        os.fsync(stream.fileno())
    write_json(output / 'proof-manifest.json', manifest)
    write_json(output / 'source-request.json', request)
    report = {'format': 'b3-owned-nitro-range-export/1', 'source': source,
              'image': values[0]['image'], 'availability_transport': 'owned-loopback-nitro-rpc',
              'proof_sha256': hashlib.sha256(proof).hexdigest(), 'proof_bytes': len(proof),
              'l1_settlement_verified': False, 'live_arbitrum_authority': False,
              'actual_erc20_custody': False, 'mainnet_enabled': False,
              'exporter_is_untrusted': True}
    write_json(output / 'source-context.json', report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--export', dest='exports', action='append', type=Path, required=True,
                        help='Repeat in newest-to-oldest order')
    parser.add_argument('--source-request', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(prepare(args.exports, args.source_request, args.output), sort_keys=True))


if __name__ == '__main__':
    main()
