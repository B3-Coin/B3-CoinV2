"""Network identity, acquisition scope and metadata bounds are not authority."""
import json
from pathlib import Path
import tempfile
import unittest

from acquire import Availability, account_request, collect, endpoint, raw, strict_json
from snapshot_metadata import pointer, summarize


class FixtureBoundaryTests(unittest.TestCase):
    def test_public_l2_needs_explicit_opt_in(self):
        with self.assertRaises(ValueError):
            endpoint('https://arbitrum-sepolia.drpc.org', False)
        self.assertTrue(endpoint('https://arbitrum-sepolia.drpc.org', True)[1])

    def test_only_reviewed_no_key_sepolia_endpoints(self):
        for value in ('http://arbitrum-sepolia.drpc.org', 'https://mainnet.infura.io', 'https://127.0.0.2',
                      'https://10.0.0.1', 'https://example.com', 'https://user:secret@arbitrum-sepolia.drpc.org',
                      'https://arbitrum-sepolia.drpc.org?key=secret', 'https://arbitrum-sepolia.drpc.org/#fragment'):
            with self.assertRaises(ValueError, msg=value):
                endpoint(value, True)
        self.assertFalse(endpoint('http://127.0.0.1:18551', False)[1])

    def test_reader_cannot_send_transactions_or_control_node(self):
        source = Availability('http://127.0.0.1:18551')
        for method in ('eth_sendRawTransaction', 'eth_sendTransaction', 'admin_addPeer', 'engine_forkchoiceUpdatedV3', 'evm_increaseTime'):
            with self.assertRaises(ValueError):
                source.call(method)
        self.assertEqual(source.sequence, 0)

    def test_owned_and_mainnet_cannot_be_relabelled_public_sepolia(self):
        class Source:
            def __init__(self, chain): self.chain = chain
            def call(self, method, params=()):
                if method != 'eth_chainId': raise AssertionError('identity rejected before history reads')
                return hex(self.chain)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'never-created'
            for chain in (1, 42161, 412346, 1337):
                with self.assertRaisesRegex(ValueError, 'chain421614'):
                    collect(Source(chain), None, output, settled_hash='0x'+'11'*32, previous_height=1,
                            previous_hash='0x'+'22'*32, to_height=2, to_hash='0x'+'11'*32)
                self.assertFalse(output.exists())

    def test_duplicate_and_nonfinite_json_refused(self):
        for value in ('{"id":1,"id":2}', '{"id":NaN}', '{"id":Infinity}'):
            with self.assertRaises(ValueError): strict_json(value)

    def test_normalized_storage_key_duplicates_refused(self):
        with self.assertRaises(ValueError):
            account_request({'address':'0x'+'11'*20, 'storage_keys':['0x'+'Aa'*32, '0x'+'aa'*32]})

    def test_snapshot_cannot_select_mainnet_or_traversal(self):
        for value in ('arb1/2026-10-24-f9ac2a55/', 'sepolia-rollup/../metadata.json',
                      'https://example.com/snapshot/', 'sepolia-rollup/2026-13-24-f9ac2a55/'):
            with self.assertRaises(ValueError): pointer(value)

    def test_measured_snapshot_size_controls_capacity_not_http_failure(self):
        selected='sepolia-rollup/2026-10-24-f9ac2a55/'
        value={'chain_name':'sepolia-rollup','snapshot_kind':'pruned','state_scheme':'hash','database_type':'pebble',
               'compression':'none','snapshot_identifier':'2026-10-24-f9ac2a55','snapshot_date':'2026-10-24',
               'nitro_version':'pinned-version','size':{'total_bytes':1941836697600},
               'parts':[{'filename':'pruned.tar.part0000','size_bytes':1941836697600,'sha256':'a'*64}]}
        out=summarize('pruned',selected,value,800*1024**3)
        self.assertFalse(out['fits_even_minimum_floor']); self.assertEqual(out['archive_bytes_downloaded'],0)
        self.assertFalse(out['source_authority']); self.assertFalse(out['node_started'])
        value['parts'][0]['size_bytes']-=1
        with self.assertRaises(ValueError): summarize('pruned',selected,value,800*1024**3)


if __name__ == '__main__': unittest.main()
