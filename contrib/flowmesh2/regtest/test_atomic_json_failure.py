"""Permission errors stay fatal and close the private temporary descriptor."""
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from custody import atomic_json


class AtomicPermissionFailureTests(unittest.TestCase):
    def assert_closed_on_permission_failure(self, failure):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'identity.json'
            original = b'{"retained":true}\n'
            target.write_bytes(original)
            opened = []
            real_open = os.open
            def capture(*args, **kwargs):
                descriptor = real_open(*args, **kwargs)
                opened.append(descriptor)
                return descriptor
            with mock.patch('custody.os.open', side_effect=capture), \
                    mock.patch('custody.os.fchmod', side_effect=failure, create=True) as permissions, \
                    mock.patch('custody.os.fdopen') as write_stream, \
                    self.assertRaises(type(failure)):
                atomic_json(target, {'secret': 'TEST-ONLY-MUST-NOT-BE-WRITTEN'})
            self.assertEqual(len(opened), 1)
            permissions.assert_called_once_with(opened[0], 0o600)
            write_stream.assert_not_called()
            with self.assertRaises(OSError):
                os.fstat(opened[0])
            self.assertEqual(target.read_bytes(), original)
            self.assertEqual(target.with_name(target.name + '.tmp').read_bytes(), b'')

    def test_permission_failure_closes_descriptor_and_preserves_identity(self):
        self.assert_closed_on_permission_failure(PermissionError('TEST private mode refused'))

    def test_missing_permission_primitive_still_refuses_and_closes_descriptor(self):
        self.assert_closed_on_permission_failure(AttributeError('TEST fchmod unavailable'))


if __name__ == '__main__':
    unittest.main()
