#!/usr/bin/env python3
"""Exact /3 source-range fact codec. Source authentication stays in SourceStream."""
import struct

from custody import CHAIN_ID, CustodyError, uint
from local_process import raw_hex

SOURCE_RANGE_KIND = 7  # 6 is the existing EXIT_NULLIFIER registry entry.
SOURCE_RANGE_BYTES = 133


def encode_source_range(cursor_height, cursor_index, domain_hex, source):
    if not isinstance(source, dict):
        raise CustodyError('source range must be an independently checked complete scan')
    previous, end = source['previous'], source['to']
    if (source.get('complete') is not True or
            any(not isinstance(point, dict) or set(point) != {'height', 'hash', 'minimum_confirmations'}
                or type(point['minimum_confirmations']) is not int or point['minimum_confirmations'] != 2
                for point in (previous, end))):
        raise CustodyError('source range differs from the pinned local finality policy')
    start, finish = uint(previous['height'], 64), uint(end['height'], 64)
    domain = bytes.fromhex(raw_hex(domain_hex, 32))
    start_hash, end_hash = (bytes.fromhex(raw_hex(point['hash'], 32)) for point in (previous, end))
    if (not any(domain) or not any(start_hash) or not any(end_hash) or
            finish < start or finish - start > 4096 or
            source.get('from_height') != start + 1 or
            type(source.get('empty_range')) is not bool or source['empty_range'] != (start == finish) or
            (start == finish and previous != end)):
        raise CustodyError('source range is noncanonical or incomplete')
    return (struct.pack('<BII', SOURCE_RANGE_KIND, uint(cursor_height, 32), uint(cursor_index, 32)) +
            domain + struct.pack('<QQ', CHAIN_ID, start) + start_hash +
            struct.pack('<Q', finish) + end_hash + struct.pack('<I', 2))


def verify_source_range(fact, cursor_height, cursor_index, domain_hex, source):
    if not isinstance(fact, bytes) or fact != encode_source_range(cursor_height, cursor_index, domain_hex, source):
        raise CustodyError('certified source range differs from this independently checked scan')
