"""Boundaries of untrusted owned history acquisition and source preparation."""
import hashlib
import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

import export_witness as exporter
import prepare


class Boundaries(unittest.TestCase):
    def test_proxy_slots_are_derived_from_the_standard_names(self):
        for actual, name in ((exporter.PRIMARY, b'eip1967.proxy.implementation'),
                             (exporter.SECONDARY, b'eip1967.proxy.implementation.secondary'),
                             (exporter.ADMIN, b'eip1967.proxy.admin')):
            self.assertEqual(actual, exporter.hex32(int.from_bytes(exporter.keccak256(name), 'big') - 1))

    def test_refuses_duplicate_or_substituted_proof_slots(self):
        address = '0x' + '11' * 20
        slots = [exporter.hex32(1), exporter.hex32(2)]
        class Rpc:
            def call(self, method, params):
                return {'address': address, 'accountProof': [], 'storageProof': [{'key': '0x1', 'proof': []}, {'key': '0x1', 'proof': []}]}
        with self.assertRaisesRegex(ValueError, 'proof keys changed'):
            exporter.witness(Rpc(), address, slots, '0x' + '22' * 32, None)

    def test_refuses_substituted_proof_account(self):
        class Rpc:
            def call(self, method, params):
                return {'address': '0x' + '33' * 20}
        with self.assertRaisesRegex(ValueError, 'proof account changed'):
            exporter.witness(Rpc(), '0x' + '11' * 20, (), '0x' + '22' * 32, None)

    def test_raw_header_must_open_the_requested_hash(self):
        class Rpc:
            def call(self, method, params):
                if method == 'eth_getBlockByHash':
                    return {'hash': params[0]}
                if method == 'debug_getRawHeader':
                    return '0x01'
                raise AssertionError('unexpected acquisition request')
        with self.assertRaisesRegex(ValueError, 'header preimage changed'):
            exporter.block(Rpc(), '0x' + '22' * 32, None)

    def test_source_archives_refuse_traversal_and_links(self):
        for name, kind in (('root/../outside', tarfile.REGTYPE), ('root/link', tarfile.SYMTYPE), ('/root/outside', tarfile.REGTYPE)):
            stream = io.BytesIO()
            with tarfile.open(fileobj=stream, mode='w:gz') as tar:
                info = tarfile.TarInfo(name); info.type = kind
                if kind == tarfile.SYMTYPE:
                    info.linkname = '../../outside'
                tar.addfile(info, io.BytesIO())
            with tempfile.TemporaryDirectory() as directory:
                with self.assertRaises(ValueError):
                    prepare.extract(stream.getvalue(), Path(directory) / 'assets')
                self.assertFalse((Path(directory) / 'outside').exists())

    def test_pinned_source_digest_is_required_before_extraction(self):
        response = unittest.mock.MagicMock()
        response.__enter__.return_value.status = 200
        response.__enter__.return_value.read.return_value = b'substituted archive'
        with patch.object(prepare.urllib.request, 'urlopen', return_value=response):
            with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
                prepare.download('https://codeload.github.com/OffchainLabs/nitro-testnode/tar.gz/pinned', hashlib.sha256(b'original').hexdigest(), 64)

    def test_fixed_commit_archive_pins_have_exact_hash_width(self):
        for _, _, commit, digest in prepare.ARCHIVES:
            self.assertEqual(len(bytes.fromhex(commit)), 20)
            self.assertEqual(len(bytes.fromhex(digest)), 32)


if __name__ == '__main__':
    unittest.main()
