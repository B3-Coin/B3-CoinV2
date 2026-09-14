#!/usr/bin/env python3
"""Pure generated-file tests for source manifest/package helpers; no services."""
import hashlib
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


if __name__ == '__main__':
    unittest.main()
