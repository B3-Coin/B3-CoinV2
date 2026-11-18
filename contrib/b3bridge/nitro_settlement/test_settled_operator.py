"""Trusted-boundary regressions for the separate full-settlement adapter."""
import json
from pathlib import Path
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

from settled_common import SettledLabChain, retain_bundle, policy_binding
from settled_operator import SettledLabOperator
from lab_operator import LabOperator
from custody import CustodyError


class AdapterBoundaryTests(unittest.TestCase):
    def test_committed_cursor_comes_from_native_range_fact(self):
        point = {'height': 18, 'hash': '0x' + '11' * 32, 'minimum_confirmations': 1}
        operator = SettledLabOperator.__new__(SettledLabOperator)
        operator.current = {'point': {**point, 'height': 10}}
        operator.worker = types.SimpleNamespace(request=Mock(return_value={'height': 1, 'source_checkpoint': point}))
        with patch.object(LabOperator, 'guard') as old_guard:
            with self.assertRaisesRegex(CustodyError, 'native committed SourceRangeFact'):
                operator.guard()
            old_guard.assert_not_called()
        operator.current = {'point': point}
        with patch.object(LabOperator, 'guard', return_value='checked'):
            self.assertEqual(operator.guard(), 'checked')

    def test_missing_native_cursor_cannot_fall_back_to_local_json(self):
        operator = SettledLabOperator.__new__(SettledLabOperator)
        operator.current = {'point': {'height': 10}}
        operator.worker = types.SimpleNamespace(request=Mock(return_value={'height': 1}))
        with self.assertRaises(CustodyError):
            operator.guard()

    def test_genesis_cursor_is_domain_bound_to_original_manifest(self):
        operator = SettledLabOperator.__new__(SettledLabOperator)
        operator.current = {'point': {'height': 11}}
        operator.manifest = {'source_initial': {'height': 10}}
        operator.worker = types.SimpleNamespace(request=Mock(return_value={'height': 0}))
        with self.assertRaisesRegex(CustodyError, 'domain-bound deployment'):
            operator.guard()

    def test_observation_does_not_accept_facts_or_verified_flags(self):
        operator = SettledLabOperator.__new__(SettledLabOperator)
        for fields in ({'facts': []}, {'verified': True}, {'source_authority': True}):
            with self.assertRaisesRegex(CustodyError, 'unknown full settlement'):
                operator.observe(fields)

    def test_policy_binding_detects_overwrite_at_same_path(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'policy.json'; path.write_text('{"policy":1}')
            chain = types.SimpleNamespace(genesis='0x' + '11' * 32, l1_genesis='0x' + '22' * 32)
            original = policy_binding(path, chain)
            path.write_text('{"policy":2}')
            self.assertNotEqual(policy_binding(path, chain), original)

    def test_full_bundle_retention_refuses_object_substitution(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source = root / 'source'; source.mkdir()
            (source / 'objects').mkdir(); (source / 'objects' / ('a' * 64)).write_bytes(b'substituted')
            with self.assertRaisesRegex(CustodyError, 'SHA address changed'):
                retain_bundle(source, root / 'retained')

    def test_coverage_requires_nonempty_actual_wasm_roots(self):
        chain = SettledLabChain.__new__(SettledLabChain)
        chain.settled = Mock(return_value={'height': 20})
        chain.validator = types.SimpleNamespace(call=Mock(return_value={'verified': True, 'WasmRoots': []}))
        self.assertFalse(chain.covered({'height': 10}))


if __name__ == '__main__':
    unittest.main()
