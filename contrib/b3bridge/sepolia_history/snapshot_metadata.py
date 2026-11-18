#!/usr/bin/env python3
"""Small official Sepolia snapshot sizing only; no archive download or sync."""
import argparse
import datetime
import hashlib
import http.client
import json
from pathlib import Path
import re
import shutil
import ssl
import time

from acquire import require, strict_json, write_json

HOST = 'snapshot.arbitrum.foundation'
KINDS = {'pruned': 'hash', 'archive': 'hash', 'full-path': 'path', 'archive-path': 'path'}
MAXIMUM = 262144


def pointer(value):
    require(type(value) is str and re.fullmatch(r'sepolia-rollup/\d{4}-\d{2}-\d{2}-[0-9a-f]{8}/', value), 'only official Sepolia pointer shape allowed')
    datetime.date.fromisoformat(value.split('/')[1][:10])
    return value


def fetch(path, maximum=MAXIMUM):
    require(path in ['/sepolia-rollup/latest-' + kind + '.txt' for kind in KINDS]
            or re.fullmatch(r'/sepolia-rollup/\d{4}-\d{2}-\d{2}-[0-9a-f]{8}/metadata\.json', path), 'only pointer/metadata paths allowed')
    connection = http.client.HTTPSConnection(HOST, timeout=20, context=ssl.create_default_context())
    deadline = time.monotonic() + 20
    try:
        # Maintained Nitro/Go client identity is accepted by the official edge;
        # previous Python-specific HTTP403 was not a storage-size measurement.
        connection.request('GET', path, headers={'User-Agent': 'Go-http-client/1.1', 'Accept-Encoding': 'identity'})
        response = connection.getresponse()
        require(response.status == 200 and response.getheader('Content-Encoding', 'identity') == 'identity',
                'official small metadata unavailable: HTTP ' + str(response.status))
        size = response.getheader('Content-Length')
        if size is not None:
            require(size.isdecimal() and int(size) <= maximum, 'official metadata length exceeds bound')
        data = response.read(maximum + 1)
        require(len(data) <= maximum and time.monotonic() < deadline, 'official metadata byte/time bound exceeded')
        return data
    finally:
        connection.close()


def summarize(kind, selected, data, available):
    require(kind in KINDS, 'snapshot kind refused'); pointer(selected)
    require(type(data) is dict and data['chain_name'] == 'sepolia-rollup' and data['snapshot_kind'] == kind
            and data['state_scheme'] == KINDS[kind] and data['database_type'] == 'pebble'
            and data['compression'] == 'none', 'metadata source/kind/scheme/compression refused')
    require(data['snapshot_identifier'] == selected.split('/')[1] and data['snapshot_date'] == selected.split('/')[1][:10],
            'snapshot pointer and metadata identity differ')
    parts = data['parts']; require(type(parts) is list and 0 < len(parts) <= 64, 'bounded snapshot parts required')
    total, names = 0, set()
    for part in parts:
        name = part['filename']; count = part['size_bytes']
        require(type(name) is str and re.fullmatch(re.escape(kind) + r'\.tar\.part[0-9]{4}', name)
                and name not in names, 'unique bounded part filename required')
        require(type(count) is int and 0 < count < 2 ** 63 and type(part['sha256']) is str
                and re.fullmatch(r'[0-9a-f]{64}', part['sha256']), 'part size/checksum metadata refused')
        names.add(name); total += count
    require(type(data['size']['total_bytes']) is int and data['size']['total_bytes'] == total,
            'snapshot part sum differs from total')
    require(type(available) is int and available >= 0, 'actual available disk bytes required')
    return {'format': 'b3-sepolia-snapshot-sizing/1', 'chain_id': 421614, 'kind': kind,
            'pointer': selected, 'metadata_url': 'https://' + HOST + '/' + selected + 'metadata.json',
            'state_scheme': data['state_scheme'], 'nitro_version': data['nitro_version'], 'compression': 'none',
            'snapshot_archive_bytes': total, 'minimum_snapshot_floor_bytes': total,
            'standard_download_extract_peak_floor_bytes': 2 * total,
            'peak_excludes_catchup_growth_and_database_overhead': True,
            'available_disk_bytes': available, 'fits_even_minimum_floor': available >= total,
            'part_count': len(parts), 'metadata_sha256': hashlib.sha256(json.dumps(data, sort_keys=True).encode()).hexdigest(),
            'published_base_url_used': False, 'archive_bytes_downloaded': 0, 'node_started': False,
            'source_authority': False, 'mainnet_enabled': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kind', choices=KINDS, default='pruned'); parser.add_argument('--disk-path', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    selected = pointer(fetch('/sepolia-rollup/latest-' + args.kind + '.txt', 4096).decode().strip())
    data = strict_json(fetch('/' + selected + 'metadata.json'))
    result = summarize(args.kind, selected, data, shutil.disk_usage(args.disk_path).free)
    if args.output:
        write_json(args.output, result)
    print(json.dumps(result, sort_keys=True))


if __name__ == '__main__':
    main()
