#!/usr/bin/env python3
"""Terminal certified-history handover durability; native verifies all crypto."""
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import Mock, patch

from custody import CustodyError, atomic_json
from network_runtime import NetworkRuntime
from operator_wrapper import Operator
from source_handover import SourceHandover
from source_stream import encode_source_facts


def immutable(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    encoded = (json.dumps(value, sort_keys=True, separators=(',', ':')) + '\n').encode()
    if path.exists() and path.read_bytes() != encoded:
        raise CustodyError('immutable source evidence changed')
    path.write_bytes(encoded)


class CertifiedHistoryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.parent = 'aa' * 32
        self.source = SimpleNamespace(validate_observation_file=Mock(), verify_binding=Mock())
        self.operator = SimpleNamespace(directory=self.directory, source=self.source, config={})
        self.old = {'parent_head_hash': self.parent, 'source': {'to': 'B'}}
        self.new = {'parent_head_hash': self.parent, 'source': {'to': 'A'}}
        self.binding = {'parent': self.parent, 'hash': 'bb' * 32}
        self.header_patch = patch('source_handover.header_binding', return_value=self.binding)
        self.header_patch.start()
        self.handover = SourceHandover(self.operator, immutable)

    def tearDown(self):
        self.header_patch.stop()
        self.temp.cleanup()

    def authority(self, view=0, kind='certified-history'):
        return {'view': view, 'authority_kind': kind, 'message': 'cafe', 'header': 'beef'}

    def test_view_zero_final_certificate_persists_distinct_non_signing_authority(self):
        self.handover.record(self.old, self.new, self.authority())
        reopened = SourceHandover(self.operator, immutable)
        self.assertEqual(reopened.latest['format'], 'fm2-source-handover/2')
        self.assertEqual(reopened.open_authority(), {'message': 'cafe', 'authority_kind': 'certified-history'})
        self.handover.record(self.old, self.new, self.authority())
        self.assertEqual(len(list(self.handover.root.glob('*.json'))), 1)

    def test_final_certificate_can_supersede_higher_local_view_but_not_be_replaced(self):
        self.handover.record(self.old, self.new, self.authority(5, 'signed-new-view'))
        self.handover.record(self.new, self.old, self.authority())
        self.assertEqual(SourceHandover(self.operator, immutable).latest['authority_kind'], 'certified-history')
        with self.assertRaisesRegex(CustodyError, 'committed or new-view'):
            self.handover.record(self.old, self.new, self.authority(6, 'signed-new-view'))

    def test_recovery_completes_only_exact_old_to_selected_pointer(self):
        self.handover.record(self.old, self.new, self.authority())
        pointer = self.directory / 'pending-observation.json'
        atomic_json(pointer, self.old)
        self.handover.reconcile({'head_hash': self.parent}, self.source)
        self.assertEqual(json.loads(pointer.read_text()), self.new)
        atomic_json(pointer, {'parent_head_hash': self.parent, 'source': {'to': 'forged'}})
        with self.assertRaisesRegex(CustodyError, 'either side'):
            self.handover.reconcile({'head_hash': self.parent}, self.source)

    def test_mutated_terminal_evidence_cannot_reopen(self):
        self.handover.record(self.old, self.new, self.authority())
        path = next(self.handover.root.glob('*.json'))
        value = json.loads(path.read_text())
        value['message'] = 'bad0'
        path.write_text(json.dumps(value))
        with self.assertRaisesRegex(CustodyError, 'history changed'):
            SourceHandover(self.operator, immutable)

    def test_forged_history_descriptor_is_refused_before_any_native_authority(self):
        runtime = NetworkRuntime.__new__(NetworkRuntime)
        runtime.operator = SimpleNamespace(b3=SimpleNamespace(verify_anchor=Mock()),
            source=SimpleNamespace(scan=Mock(return_value=([], [], {'complete': True})),
                                   range_commitment=False, verify_binding=Mock()))
        runtime.worker = SimpleNamespace(request=Mock())
        binding = {'parent': self.parent, 'facts_from': {'height': 1, 'index': 0}, 'hash': 'bb' * 32}
        descriptor = {'parent': self.parent, 'anchor': {'height': 1, 'hash': 'cc' * 32}, 'events': [], 'evm_end': {}}
        with patch('network_runtime.header_binding', return_value=binding):
            with self.assertRaisesRegex(CustodyError, 'native certified facts'):
                runtime._preflight_history_observation(descriptor, {'header': 'beef', 'facts': ['ff']},
                                                       {'head_hash': self.parent, 'domain': 'dd' * 32})
        runtime.worker.request.assert_not_called()
        runtime.operator.source.verify_binding.assert_not_called()

    def test_initial_new_view_preserves_null_predecessor_and_recovers_pre_pointer_crash(self):
        authority = self.authority(1, 'signed-new-view')
        self.handover.record(None, self.new, authority, initial=True)
        self.assertFalse((self.directory / 'pending-observation.json').exists())
        reopened = SourceHandover(self.operator, immutable)
        self.assertEqual(reopened.latest['format'], 'fm2-source-handover/3')
        self.assertIsNone(reopened.latest['old'])
        self.assertIs(reopened.latest['initial'], True)
        self.assertEqual(reopened.open_authority(), {'message': authority['message']})
        reopened.reconcile({'head_hash': self.parent}, self.source)
        self.assertEqual(json.loads((self.directory / 'pending-observation.json').read_text()), self.new)
        self.assertEqual(len(list((self.directory / 'source-observations').glob('*.json'))), 1)

    def test_initial_new_view_preserves_actual_consumed_prior_pointer(self):
        prior = {'parent_head_hash': 'ee' * 32, 'source': {'to': 'consumed'}}
        self.source.committed = prior
        atomic_json(self.directory / 'pending-observation.json', prior)
        self.handover.record(prior, self.new, self.authority(1, 'own-quorum'), initial=True)
        reopened = SourceHandover(self.operator, immutable)
        self.assertEqual(reopened.latest['old'], prior)
        reopened.reconcile({'head_hash': self.parent}, self.source)
        self.assertEqual(json.loads((self.directory / 'pending-observation.json').read_text()), self.new)

    def test_initial_authority_cannot_invent_or_replace_an_unrecorded_predecessor(self):
        atomic_json(self.directory / 'pending-observation.json', self.old)
        with self.assertRaisesRegex(CustodyError, 'actual prior consumed pointer'):
            self.handover.record(None, self.new, self.authority(1, 'signed-new-view'), initial=True)
        with self.assertRaisesRegex(CustodyError, 'actual prior consumed pointer'):
            self.handover.record(self.old, self.new, self.authority(1, 'signed-new-view'), initial=True)

    def test_unfrozen_observation_still_uses_the_exact_verified_native_capability(self):
        runtime = NetworkRuntime.__new__(NetworkRuntime)
        authority = self.authority(1, 'signed-new-view')
        authority['token'] = 7
        descriptor = {'parent': self.parent, 'anchor': {'height': 2, 'hash': 'bb' * 32}, 'events': [], 'evm_end': {}}
        runtime.worker = SimpleNamespace(request=Mock())
        runtime.operator = SimpleNamespace(pending={**self.old, 'anchor': {}, 'events': []}, pending_frozen=False,
            _check_proposal_binding=Mock(side_effect=[CustodyError('different old facts'), None]), _observe=Mock())
        runtime.stats = {'source_handovers': 0}
        runtime._authorize(descriptor, 'beef', authority, info={'head_hash': self.parent, 'observation_frozen': False})
        self.assertIs(runtime.operator._observe.call_args.kwargs['handover'], authority)
        self.assertEqual(runtime.stats['source_handovers'], 1)

    def test_wrapper_observe_allows_verified_initial_and_unfrozen_capabilities(self):
        for existing in (None, self.old):
            with self.subTest(initial=existing is None):
                operator = Operator.__new__(Operator)
                operator.directory, operator.pending, operator.pending_frozen = self.directory, existing, False
                pointer = self.directory / 'pending-observation.json'
                if pointer.exists():
                    pointer.unlink()
                if existing is not None:
                    atomic_json(pointer, existing)
                state = {'head_hash': self.parent, 'header': 'beef', 'domain': 'dd' * 32, 'observation_frozen': False}
                requests = []
                def request(command, **fields):
                    requests.append((command, fields))
                    if command == 'info': return state
                    if command == 'check_observation': return {'acceptable': True, 'observation_frozen': False}
                    return {}
                operator.worker = SimpleNamespace(request=request)
                operator.b3 = SimpleNamespace(verify_anchor=Mock())
                operator.history_tip = '00' * 32
                operator._verify_retained = Mock()
                operator.handovers = SimpleNamespace(record=Mock())
                operator.source = SimpleNamespace(head_hash=self.parent, range_commitment=False,
                    scan=Mock(return_value=([], [], {'to': 'B'})))
                def save(saved, before_publish):
                    self.assertEqual(operator.handovers.record.called, existing is None)
                    if before_publish is not None:
                        before_publish()
                    atomic_json(pointer, saved)
                operator.source.save_pending = save
                authority = {**self.authority(1, 'signed-new-view'), 'token': 9}
                with patch('operator_wrapper.header_fields', return_value={
                        'anchor': {'height': 1, 'hash': 'cc' * 32}, 'facts_to': {'height': 1, 'index': 0}}):
                    operator._observe({'height': 1, 'hash': 'cc' * 32}, [], evm_end={}, handover=authority)
                token_requests = [fields['observation_token'] for command, fields in requests
                                  if command in ('check_observation', 'observe')]
                self.assertEqual(token_requests, [9, 9])
                self.assertEqual(operator.handovers.record.call_args.kwargs.get('initial', False), existing is None)
                self.assertEqual(operator.handovers.record.call_args.args[0], existing)

    def test_source_fact_encoder_retains_exact_receipt_identity_and_advances_cursor(self):
        events = [{'kind': 'deposit'}]
        records = [{'transaction_hash': '11' * 32, 'log_index': 7, 'amount_raw': 300,
                    'account': '22' * 32, 'asset': '33' * 32}]
        facts, end = encode_source_facts(events, records, {'height': 4, 'index': 9}, '44' * 32, {}, False)
        encoded = bytes.fromhex(facts[0])
        self.assertEqual(encoded[0], 1)
        self.assertEqual(encoded[9:41].hex(), records[0]['transaction_hash'])
        self.assertEqual(encoded[45:77].hex(), records[0]['account'])
        self.assertEqual(encoded[77:109].hex(), records[0]['asset'])
        self.assertEqual(end, {'height': 4, 'index': 10})


class TypedNewViewLossTests(unittest.TestCase):
    def runtime(self, frame, enabled):
        import test_runtime_guards as guards
        runtime, calls = guards.RuntimeGuardTests().runtime(frame)
        runtime.regtest_drop_new_views = enabled
        runtime.stats.update(new_view_frames=0, regtest_dropped_new_views=0)
        runtime._try_own_observation.return_value = False
        return runtime, calls

    def test_typed_loss_releases_new_view_before_authority_or_agreement_dispatch(self):
        runtime, calls = self.runtime({'token': 3, 'lane': 1, 'slot': 6, 'is_new_view': True}, True)
        runtime.step()
        self.assertIn('network_drop', calls)
        self.assertNotIn('network_observation_authority', calls)
        self.assertNotIn('network_dispatch', calls)
        runtime._authorize.assert_not_called()
        self.assertEqual(runtime.stats['new_view_frames'], 1)
        self.assertEqual(runtime.stats['regtest_dropped_new_views'], 1)
        self.assertFalse(runtime.frames)

    def test_normal_new_view_authority_path_remains_armed_when_loss_is_disabled(self):
        runtime, calls = self.runtime({'token': 3, 'lane': 1, 'slot': 6, 'is_new_view': True}, False)
        request = runtime.worker.request.side_effect
        def native(command, **fields):
            result = request(command, **fields)
            if command == 'network_observation_authority':
                return {'token': 3, 'header': 'beef', 'actions': []}
            return result
        runtime.worker.request.side_effect = native
        runtime._descriptor_for_header = Mock(return_value={'parent': 'aa' * 32})
        runtime.step()
        self.assertIn('network_observation_authority', calls)
        self.assertIn('network_dispatch', calls)
        self.assertNotIn('network_drop', calls)
        self.assertEqual(runtime.stats['new_view_frames'], 1)
        self.assertEqual(runtime.stats['regtest_dropped_new_views'], 0)

    def test_critical_type_filter_never_drops_certified_history_bulk(self):
        frame = {'token': 4, 'lane': 4, 'slot': 6, 'is_new_view': True}
        runtime, calls = self.runtime(frame, True)
        runtime._bulk = Mock()
        runtime.step()
        runtime._bulk.assert_called_once_with({'received': True, **frame})
        self.assertNotIn('network_drop', calls)
        self.assertEqual(runtime.stats['new_view_frames'], 0)
        self.assertEqual(runtime.stats['regtest_dropped_new_views'], 0)

    def test_fault_counters_saturate_at_fixed_width(self):
        runtime, calls = self.runtime({'token': 3, 'lane': 1, 'slot': 6, 'is_new_view': True}, True)
        runtime.stats.update(new_view_frames=0xffffffff, regtest_dropped_new_views=0xffffffff)
        runtime.step()
        self.assertEqual(runtime.stats['new_view_frames'], 0xffffffff)
        self.assertEqual(runtime.stats['regtest_dropped_new_views'], 0xffffffff)


if __name__ == '__main__':
    unittest.main()
