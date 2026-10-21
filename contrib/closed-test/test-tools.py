#!/usr/bin/env python3
"""Pure generated-file tests for source manifest/package helpers; no services."""
import hashlib
import ast
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('package_mac', Path(__file__).with_name('package-mac.py'))
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)
notice_spec = importlib.util.spec_from_file_location('collect_notices', Path(__file__).with_name('collect-notices.py'))
notices = importlib.util.module_from_spec(notice_spec)
notice_spec.loader.exec_module(notices)
capture_path = Path(__file__).with_name('capture-qt.py')
capture_spec = importlib.util.spec_from_file_location('capture_qt', capture_path)
capture = importlib.util.module_from_spec(capture_spec)
capture_spec.loader.exec_module(capture)


class PackagingHelpers(unittest.TestCase):
    def manifest(self, root):
        rows = [{'path': str(path.relative_to(root)), 'sha256': package.sha(path),
                 'executable': bool(path.stat().st_mode & 0o111)}
                for path in sorted(root.rglob('*')) if path.is_file()]
        canonical = ''.join(row['sha256'] + ' ' + ('x' if row['executable'] else '-') + ' ' + row['path'] + '\n'
                            for row in rows)
        return {'files': rows, 'source_tree_sha256': hashlib.sha256(canonical.encode()).hexdigest()}

    def test_export_exact_bytes_and_modes(self):
        with tempfile.TemporaryDirectory(prefix='b3-closed-source-') as temporary:
            root = Path(temporary)
            path = root / 'fixture.txt'
            path.write_text('generated source\n')
            document = self.manifest(root)
            package.verify_export(root, document)
            path.write_text('changed\n')
            with self.assertRaises(RuntimeError):
                package.verify_export(root, document)
            path.write_text('generated source\n')
            path.chmod(path.stat().st_mode | 0o100)
            with self.assertRaises(RuntimeError):
                package.verify_export(root, document)

    def test_export_rejects_extra_missing_and_links(self):
        with tempfile.TemporaryDirectory(prefix='b3-closed-source-') as temporary:
            root = Path(temporary)
            original = root / 'fixture.txt'
            original.write_text('generated\n')
            document = self.manifest(root)
            extra = root / 'extra.txt'
            extra.write_text('extra\n')
            with self.assertRaises(RuntimeError):
                package.verify_export(root, document)
            extra.unlink()
            original.rename(root / 'moved.txt')
            with self.assertRaises(RuntimeError):
                package.verify_export(root, document)
            (root / 'moved.txt').rename(original)
            extra.symlink_to(original)
            with self.assertRaises(RuntimeError):
                package.verify_export(root, document)

    def test_export_digest_is_verified(self):
        with tempfile.TemporaryDirectory(prefix='b3-closed-source-') as temporary:
            root = Path(temporary)
            (root / 'fixture').write_text('test')
            document = self.manifest(root)
            document['source_tree_sha256'] = '0' * 64
            with self.assertRaises(RuntimeError):
                package.verify_export(root, document)

    def test_macos_minimum_is_numeric(self):
        self.assertEqual(package.version('26.0'), package.version('26.0.0'))
        self.assertGreater(package.version('26.1'), package.version('26.0'))
        self.assertGreater(package.version('14.10'), package.version('14.9'))

    def test_framework_patch_version_not_short_display_version(self):
        value = {'CFBundleShortVersionString':'6.11', 'CFBundleVersion':'6.11.1'}
        self.assertNotEqual(value['CFBundleShortVersionString'], '6.11.1')
        self.assertEqual(package.framework_version(value), '6.11.1')
        with self.assertRaises(RuntimeError):
            package.framework_version({'CFBundleShortVersionString':'6.11'})

    def test_public_qt_attribution_literal_copyright_newline(self):
        original = b'{"Copyright":"First copyright line\nSecond copyright line","LicenseFile":"LICENSE"}'
        with self.assertRaises(json.JSONDecodeError):
            json.loads(original) # The exact old collector parsing failure.
        parsed = notices.read_attribution(original)
        self.assertEqual(parsed[0]['Copyright'], 'First copyright line\nSecond copyright line')
        self.assertEqual(parsed[0]['LicenseFile'], 'LICENSE')
        with self.assertRaises(json.JSONDecodeError):
            notices.read_attribution(b'{broken')
        with self.assertRaises(ValueError):
            notices.read_attribution(b'[1]')

    def test_loopback_origin_forms_match_client_endpoint_contract(self):
        for host in ('127.0.0.1', 'localhost'):
            for suffix in ('', '/', '/flowmesh/v1'):
                endpoint = f'https://{host}:20952{suffix}'
                self.assertTrue(package.loopback_https_endpoint(endpoint))
                self.assertEqual(capture.loopback_https_endpoint(endpoint).port, 20952)
        for endpoint in ('http://127.0.0.1:20952', 'https://example.org:20952',
                         'https://127.0.0.1:0', 'https://localhost:65536',
                         'https://localhost', 'https://user@localhost:20952',
                         'https://localhost:20952/other', 'https://localhost:20952?',
                         'https://localhost:20952#', 'https://localhost:20952\n'):
            self.assertFalse(package.loopback_https_endpoint(endpoint), endpoint)
            with self.assertRaises(RuntimeError, msg=endpoint):
                capture.loopback_https_endpoint(endpoint)

    def test_capture_posts_fixed_api_path_for_origin_endpoints(self):
        # Inspect the actual preflight request expression, not only a constant
        # which might be unused. No connection or service is started.
        tree = ast.parse(capture_path.read_text())
        requests = [node for node in ast.walk(tree) if isinstance(node, ast.Call)
                    and isinstance(node.func, ast.Attribute) and node.func.attr == 'request']
        self.assertEqual(len(requests), 1)
        for suffix in ('', '/', '/flowmesh/v1'):
            expression = ast.Expression(requests[0].args[1])
            value = eval(compile(expression, str(capture_path), 'eval'),
                         {'API_PATH': capture.API_PATH,
                          'parsed': capture.loopback_https_endpoint('https://127.0.0.1:20952' + suffix)})
            self.assertEqual(value, '/flowmesh/v1')


if __name__ == '__main__':
    unittest.main()
