#!/usr/bin/env python3
"""Read-only sizing for an owned Arbitrum history node; never installs or syncs.

The official manifest is untrusted availability metadata, not a verified state
checkpoint. Its checksums detect changed downloads, not invalid chain state.
PathDB is useful for archive queries but is not Nitro block-validator mode.
Ethereum execution/history/blob storage and verification are additional work.
No endpoint, credentials, transaction method, activation or downloader exists
in this tool. Even sufficient local resources cannot qualify the source.
"""
import argparse
import datetime
import hashlib
import http.client
import json
import os
from pathlib import Path
import re
import shutil
import ssl
import time

HOST = 'snapshot.arbitrum.foundation'
POINTER_PATH = '/arb1/latest-archive-path.txt'
MAX_METADATA = 262144
MIN_MEMORY_BYTES = 64 * 1024 ** 3


class PreflightError(ValueError):
    pass


def strict_json(raw):
    def fields(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise PreflightError('duplicate metadata field')
            value[key] = item
        return value
    def bad_constant(_value):
        raise PreflightError('nonfinite metadata number')
    try:
        return json.loads(raw, object_pairs_hook=fields, parse_constant=bad_constant)
    except (UnicodeError, json.JSONDecodeError, RecursionError) as error:
        raise PreflightError('malformed bounded metadata') from error


def validate_pointer(pointer):
    if not isinstance(pointer, str) or re.fullmatch(r'arb1/(\d{4}-\d{2}-\d{2})-[0-9a-f]{8}/', pointer) is None:
        raise PreflightError('unknown official snapshot pointer')
    try:
        datetime.date.fromisoformat(pointer.split('/')[1][:10])
    except ValueError as error:
        raise PreflightError('invalid snapshot date') from error
    return pointer


def read_official(path, maximum=MAX_METADATA, timeout=20):
    if path != POINTER_PATH and re.fullmatch(r'/arb1/\d{4}-\d{2}-\d{2}-[0-9a-f]{8}/metadata\.json', path) is None:
        raise PreflightError('only the fixed official pointer and metadata may be read')
    if type(maximum) is not int or not 1 <= maximum <= MAX_METADATA:
        raise PreflightError('invalid metadata size bound')
    deadline = time.monotonic() + timeout
    connection = http.client.HTTPSConnection(HOST, timeout=timeout, context=ssl.create_default_context())
    try:
        connection.request('GET', path, headers={'Accept-Encoding': 'identity', 'User-Agent': 'B3-history-preflight/1'})
        response = connection.getresponse()
        if response.status != 200 or response.getheader('Content-Encoding', 'identity') != 'identity':
            raise PreflightError('official metadata unavailable, redirected or compressed')
        length = response.getheader('Content-Length')
        if length is not None and (re.fullmatch(r'0|[1-9][0-9]*', length) is None or int(length) > maximum):
            raise PreflightError('official metadata length exceeds bound')
        data = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise PreflightError('official metadata deadline exceeded')
            if connection.sock is not None:
                connection.sock.settimeout(remaining)
            chunk = response.read1(min(8192, maximum + 1 - len(data)))
            if not chunk:
                break
            data.extend(chunk)
            if len(data) > maximum:
                raise PreflightError('official metadata exceeds bound')
        if length is not None and len(data) != int(length):
            raise PreflightError('official metadata response was truncated')
        return bytes(data)
    except (OSError, http.client.HTTPException) as error:
        raise PreflightError('official metadata read failed') from error
    finally:
        connection.close()


def summarize_metadata(pointer, value):
    validate_pointer(pointer)
    if (not isinstance(value, dict) or value.get('chain_name') != 'arb1' or
            value.get('snapshot_kind') != 'archive-path' or value.get('state_scheme') != 'path' or
            value.get('database_type') != 'pebble' or value.get('compression') != 'none' or
            value.get('snapshot_identifier') != pointer.split('/')[1] or
            value.get('snapshot_date') != pointer.split('/')[1][:10]):
        raise PreflightError('metadata does not describe the exact Arbitrum One PathDB archive pointer')
    parts = value.get('parts')
    if not isinstance(parts, list) or not 1 <= len(parts) <= 1024:
        raise PreflightError('invalid bounded snapshot part count')
    total = 0
    for index, part in enumerate(parts):
        if (not isinstance(part, dict) or part.get('filename') != 'archive-path.tar.part' + str(index).zfill(4) or
                type(part.get('size_bytes')) is not int or not 0 < part['size_bytes'] <= 1024 ** 4 or
                not isinstance(part.get('sha256'), str) or re.fullmatch(r'[0-9a-f]{64}', part['sha256']) is None):
            raise PreflightError('invalid snapshot part name, size, order or digest')
        total += part['size_bytes']
    size = value.get('size')
    if not isinstance(size, dict) or type(size.get('total_bytes')) is not int or size['total_bytes'] != total:
        raise PreflightError('snapshot total differs from its exact parts')
    version = value.get('nitro_version')
    if not isinstance(version, str) or re.fullmatch(r'v[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.]+)*', version) is None:
        raise PreflightError('invalid snapshot build label')
    return {'chain_name': 'arb1', 'state_scheme': 'path', 'snapshot_kind': 'archive-path',
            'snapshot_identifier': value['snapshot_identifier'], 'part_count': len(parts),
            'archive_bytes': total, 'reported_nitro_version': version,
            'reported_build_is_prerelease': bool(re.search(r'(?:^|[-.])(rc|beta|alpha)(?:[-.0-9]|$)', version)),
            'metadata_is_consensus_authority': False, 'pathdb_is_block_validator': False}


def assess(pointer, metadata, free_bytes, memory_bytes):
    if type(free_bytes) is not int or free_bytes < 0 or type(memory_bytes) is not int or memory_bytes < 0:
        raise PreflightError('invalid observed resource sizes')
    snapshot = summarize_metadata(pointer, metadata)
    # This is an explicitly conservative sizing screen, not a Nitro resource
    # requirement/security proof. Extraction/workspace, growth and the parent
    # Ethereum node require additional capacity; no automatic setup follows.
    disk_screen = (snapshot['archive_bytes'] * 6 + 4) // 5
    return {'format': 'b3-arbitrum-owned-node-preflight/1', 'mainnet_enabled': False,
            'rpc_independent_source_qualified': False, 'activation_supported': False,
            'archive_downloaded': False, 'source_state_initialized': False,
            'official_metadata_url': 'https://' + HOST + '/' + pointer + 'metadata.json',
            'snapshot': snapshot, 'free_bytes': free_bytes, 'memory_bytes': memory_bytes,
            'conservative_disk_screen_bytes': disk_screen, 'minimum_documented_memory_bytes': MIN_MEMORY_BYTES,
            'disk_screen_passed': free_bytes >= disk_screen, 'memory_screen_passed': memory_bytes >= MIN_MEMORY_BYTES,
            'resource_screens_passed': free_bytes >= disk_screen and memory_bytes >= MIN_MEMORY_BYTES,
            'remaining_requirements': [
                'Maintained Ethereum execution and beacon peers, historical calldata and authenticated blob availability.',
                'Authenticated Nitro starting state and correct archive state scheme; manifest hashes alone are insufficient.',
                'Reviewed Nitro STF/code and verified Arbitrum source-policy integration; PathDB archive queries are not block validation.',
                'Durable complete source ranges, rollback handling, source catchup and operator custody integration.',
                'Additional Ethereum storage, extraction space, ongoing growth, load and recovery qualification.'],
            'primary_sources': [
                'https://docs.arbitrum.io/run-a-node/more-types/run-archive-node',
                'https://docs.arbitrum.io/run-a-node/start-here',
                'https://docs.arbitrum.io/run-a-node/faq']}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data-directory', required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--read-official-metadata', action='store_true')
    mode.add_argument('--metadata-json')
    parser.add_argument('--pointer')
    args = parser.parse_args(argv)
    directory = Path(args.data_directory).resolve(strict=True)
    if not directory.is_dir():
        parser.error('data-directory must be an existing directory; no files are created')
    if args.read_official_metadata:
        if args.pointer is not None:
            parser.error('the official mode obtains its own exact bounded pointer')
        pointer = read_official(POINTER_PATH, 256).decode('ascii').strip()
        validate_pointer(pointer)
        raw = read_official('/' + pointer + 'metadata.json')
    else:
        pointer = validate_pointer(args.pointer)
        with Path(args.metadata_json).open('rb') as stream:
            raw = stream.read(MAX_METADATA + 1)
        if len(raw) > MAX_METADATA:
            raise PreflightError('offline metadata exceeds bound')
    memory = os.sysconf('SC_PHYS_PAGES') * os.sysconf('SC_PAGE_SIZE')
    report = assess(pointer, strict_json(raw), shutil.disk_usage(directory).free, memory)
    report['metadata_sha256'] = hashlib.sha256(raw).hexdigest()
    report['metadata_origin'] = 'official-https-availability-only' if args.read_official_metadata else 'offline-untrusted-file'
    print(json.dumps(report, sort_keys=True, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
