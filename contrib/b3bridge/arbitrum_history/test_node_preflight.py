#!/usr/bin/env python3
"""Adversarial metadata parsing and conservative sizing boundaries."""
import copy
import json
import unittest
from unittest.mock import patch

import node_preflight as preflight


class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.pointer = 'arb1/2026-10-07-4601923c/'
        self.metadata = {'chain_name': 'arb1', 'snapshot_kind': 'archive-path',
            'state_scheme': 'path', 'database_type': 'pebble', 'compression': 'none',
            'snapshot_identifier': '2026-10-07-4601923c', 'snapshot_date': '2026-10-07',
            'nitro_version': 'v3.12.1-rc.2-70fa99a',
            'parts': [{'filename': 'archive-path.tar.part0000', 'size_bytes': 100,
                       'sha256': '01' * 32}], 'size': {'total_bytes': 100}}

    def test_sizing_is_read_only_and_never_source_authority_even_with_resources(self):
        report = preflight.assess(self.pointer, self.metadata, 2 ** 50, 2 ** 40)
        self.assertTrue(report['resource_screens_passed'])
        for field in ('mainnet_enabled', 'rpc_independent_source_qualified', 'activation_supported',
                      'archive_downloaded', 'source_state_initialized'):
            self.assertIs(report[field], False)
        self.assertIs(report['snapshot']['metadata_is_consensus_authority'], False)
        self.assertIs(report['snapshot']['pathdb_is_block_validator'], False)
        self.assertIs(report['snapshot']['reported_build_is_prerelease'], True)

    def test_disk_and_ram_screens_are_independent_exact_boundaries(self):
        required_memory = preflight.MIN_MEMORY_BYTES
        report = preflight.assess(self.pointer, self.metadata, 120, required_memory)
        self.assertTrue(report['resource_screens_passed'])
        self.assertFalse(preflight.assess(self.pointer, self.metadata, 119, required_memory)['disk_screen_passed'])
        self.assertFalse(preflight.assess(self.pointer, self.metadata, 120, required_memory - 1)['memory_screen_passed'])
        for free, memory in ((True, 1), (-1, 1), (1, 1.0), (1, -1)):
            with self.assertRaises(preflight.PreflightError):
                preflight.assess(self.pointer, self.metadata, free, memory)

    def test_pointer_never_supplies_an_arbitrary_host_path_or_filename(self):
        values = [None, '', '../arb1/2026-10-07-4601923c/', 'https://example.com/',
                  'arb1/2026-13-01-4601923c/', 'arb1/2026-10-07-4601923C/',
                  'arb1/2026-10-07-4601923c/../', 'arb1/2026-10-07-4601923c/\n',
                  'arb1/2026-10-07-4601923c/?q=x', 'nova/2026-10-07-4601923c/']
        for value in values:
            with self.subTest(pointer=value), self.assertRaises(preflight.PreflightError):
                preflight.validate_pointer(value)

    def test_chain_scheme_kind_and_identifier_must_all_bind(self):
        for field, value in (('chain_name', 'nova'), ('state_scheme', 'hash'), ('snapshot_kind', 'pruned'),
                             ('database_type', 'other'), ('compression', 'gzip'),
                             ('snapshot_identifier', '2026-10-08-4601923c'), ('snapshot_date', '2026-10-08')):
            changed = copy.deepcopy(self.metadata)
            changed[field] = value
            with self.subTest(field=field), self.assertRaises(preflight.PreflightError):
                preflight.summarize_metadata(self.pointer, changed)

    def test_parts_reject_gaps_duplicates_traversal_uppercase_hash_and_bool_size(self):
        changes = [('filename', '../archive-path.tar.part0000'), ('filename', 'archive-path.tar.part0001'),
                   ('sha256', 'AA' * 32), ('sha256', '0' * 63), ('size_bytes', True),
                   ('size_bytes', 0), ('size_bytes', 1024 ** 4 + 1)]
        for field, value in changes:
            changed = copy.deepcopy(self.metadata)
            changed['parts'][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(preflight.PreflightError):
                preflight.summarize_metadata(self.pointer, changed)
        for parts in ([], [self.metadata['parts'][0]] * 2, [self.metadata['parts'][0]] * 1025):
            changed = copy.deepcopy(self.metadata)
            changed['parts'] = parts
            with self.assertRaises(preflight.PreflightError):
                preflight.summarize_metadata(self.pointer, changed)

    def test_total_bytes_are_exact_not_human_labels(self):
        changed = copy.deepcopy(self.metadata)
        changed['size']['total_bytes'] = 101
        with self.assertRaises(preflight.PreflightError):
            preflight.summarize_metadata(self.pointer, changed)
        changed['size']['total_bytes'] = True
        with self.assertRaises(preflight.PreflightError):
            preflight.summarize_metadata(self.pointer, changed)
        changed['size']['total_bytes'] = 100
        changed['size']['total_human'] = '0 MB'
        self.assertEqual(preflight.summarize_metadata(self.pointer, changed)['archive_bytes'], 100)

    def test_json_refuses_duplicate_fields_nonfinite_and_malformed_values(self):
        for value in ('{"size":1,"size":2}', '{"size":NaN}', '{"size":Infinity}',
                      b'\xff', '{', '[' * 2000):
            with self.subTest(value=str(value)[:50]), self.assertRaises(preflight.PreflightError):
                preflight.strict_json(value)

    def test_metadata_url_fields_are_never_followed(self):
        self.metadata['base_url'] = 'http://127.0.0.1/secret'
        self.metadata['download_url'] = 'file:///private'
        with patch('node_preflight.http.client.HTTPSConnection', side_effect=AssertionError('no network')):
            report = preflight.assess(self.pointer, self.metadata, 1, 1)
        self.assertEqual(report['official_metadata_url'],
            'https://snapshot.arbitrum.foundation/arb1/2026-10-07-4601923c/metadata.json')

    def test_network_path_allowlist_is_enforced_before_connect(self):
        with patch('node_preflight.http.client.HTTPSConnection', side_effect=AssertionError('no connection')):
            for path in ('https://example.com', '/arb1/../secret', '/arb1/archive-path.tar.part0000',
                         '/arb1/2026-10-07-4601923c/metadata.json?secret=1'):
                with self.assertRaises(preflight.PreflightError):
                    preflight.read_official(path)

    def test_release_label_does_not_autoapprove_binary_compatibility(self):
        changed = copy.deepcopy(self.metadata)
        changed['nitro_version'] = 'v3.12.1-70fa99a'
        report = preflight.assess(self.pointer, changed, 1 << 50, 1 << 40)
        self.assertFalse(report['snapshot']['reported_build_is_prerelease'])
        self.assertFalse(report['rpc_independent_source_qualified'])
        for version in (None, 'latest', '$(command)', 'v3.12.1 --flag'):
            changed['nitro_version'] = version
            with self.assertRaises(preflight.PreflightError):
                preflight.summarize_metadata(self.pointer, changed)


if __name__ == '__main__':
    unittest.main()
