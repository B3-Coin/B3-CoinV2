#!/usr/bin/env python3
"""Regression for atomic transport-identity updates broadening file access."""
import json
import os
from pathlib import Path
import tempfile
import unittest

from custody import atomic_json


@unittest.skipUnless(os.name == 'posix', 'the supported operator runtime is Linux')
class PrivateAtomicJson(unittest.TestCase):
    def test_initial_and_transport_identity_update_stay_private(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'private-identity.json'
            atomic_json(path, {'identity': 'test-public-marker'})
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            atomic_json(path, {'identity': 'test-public-marker', 'network_identity': 'test-marker'})
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(json.loads(path.read_text())['network_identity'], 'test-marker')

    def test_retained_temporary_file_is_private_before_replacement(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'private-identity.json'
            temporary = path.with_name(path.name+'.tmp')
            temporary.write_text('old-public-marker')
            temporary.chmod(0o644)
            atomic_json(path, {'identity': 'replacement-test-marker'})
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertFalse(temporary.exists())

    def test_temporary_symlink_refused_without_touching_target(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)/'private-identity.json'
            target = Path(directory)/'untouched-test-marker'
            target.write_text('unchanged')
            path.with_name(path.name+'.tmp').symlink_to(target)
            with self.assertRaises(OSError):
                atomic_json(path, {'identity': 'test-marker'})
            self.assertEqual(target.read_text(), 'unchanged')
            self.assertFalse(path.exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
