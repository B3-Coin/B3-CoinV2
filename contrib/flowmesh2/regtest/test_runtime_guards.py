#!/usr/bin/env python3
"""Fresh source guards stay at signing boundaries even between idle heartbeats."""
from collections import OrderedDict, deque
from types import SimpleNamespace
import time
import unittest
from unittest.mock import Mock

from custody import CustodyError
from network_runtime import NetworkRuntime


class RuntimeGuardTests(unittest.TestCase):
    def runtime(self, frame=None):
        runtime = NetworkRuntime.__new__(NetworkRuntime)
        now = time.monotonic()
        head = 'aa' * 32
        state = {'head_hash': head, 'height': 5, 'observation_handover_pending': False}
        calls = []
        frames = deque([frame] if frame else [])
        def request(command, **fields):
            calls.append(command)
            if command == 'info':
                return dict(state)
            if command == 'network_poll':
                return {'received': True, **frames.popleft()} if frames else {'received': False}
            if command in ('network_dispatch', 'pbft_timeout'):
                return {'applied': []}
            return {}
        runtime.worker = SimpleNamespace(request=Mock(side_effect=request), process=SimpleNamespace(poll=lambda: None))
        def pending_guard():
            calls.append('fresh-source-and-B3-guard')
        runtime.operator = SimpleNamespace(pending={'parent_head_hash': head}, pending_frozen=True, opened=True,
            _verify_retained=Mock(side_effect=lambda: calls.append('heartbeat-source-guard')),
            _validate_pending=Mock(side_effect=pending_guard),
            source=SimpleNamespace(records=[]))
        runtime.active, runtime.fenced, runtime.paused = True, None, False
        runtime.last_guard = runtime.last_timer = runtime.last_hello = now
        runtime.stats = {'authenticated_frames': 0, 'agreement_frames': 0, 'peer_refusals': 0, 'timer_ticks': 0}
        runtime.frames, runtime.started_batches, runtime.sent_history = deque(), set(), {}
        runtime.genesis_head = 'bb' * 32
        runtime.last_request, runtime.batches = {}, {head: {'observation': {}, 'actions': []}}
        runtime._authorize = Mock()
        runtime._try_own_observation = Mock()
        runtime._finish = Mock()
        runtime._flush = Mock()
        runtime._start_batch = Mock()
        runtime.automatic = SimpleNamespace(step=Mock(), enabled=False, pending={})
        runtime.custody = SimpleNamespace(step=Mock())
        return runtime, calls

    def test_recent_idle_heartbeat_never_replaces_fresh_peer_signing_guard(self):
        runtime, calls = self.runtime({'token': 3, 'lane': 1, 'slot': 6})
        runtime.step()
        self.assertIn('network_dispatch', calls)
        self.assertLess(calls.index('fresh-source-and-B3-guard'), calls.index('network_dispatch'))
        self.assertEqual(runtime.operator._validate_pending.call_count, 1)
        runtime.operator._verify_retained.assert_not_called()

    def test_fresh_source_failure_blocks_peer_dispatch_despite_recent_heartbeat(self):
        runtime, calls = self.runtime({'token': 3, 'lane': 1, 'slot': 6})
        runtime.operator._validate_pending.side_effect = CustodyError('retained canonical checkpoint removed')
        runtime.step()
        self.assertNotIn('network_dispatch', calls)
        self.assertIn('network_drop', calls)
        self.assertEqual(runtime.stats['peer_refusals'], 1)

    def test_due_monotone_timer_has_its_own_fresh_signing_guard(self):
        runtime, calls = self.runtime()
        runtime.last_timer = 0
        runtime.step()
        self.assertIn('pbft_timeout', calls)
        self.assertLess(calls.index('fresh-source-and-B3-guard'), calls.index('pbft_timeout'))
        self.assertEqual(runtime.stats['timer_ticks'], 1)

    def test_idle_period_uses_bounded_heartbeat_without_repeated_frame_guard(self):
        runtime, calls = self.runtime()
        runtime.step()
        self.assertNotIn('fresh-source-and-B3-guard', calls)
        runtime.last_guard = time.monotonic() - .501
        runtime.step()
        self.assertEqual(runtime.operator._validate_pending.call_count, 1)
        self.assertNotIn('network_dispatch', calls)
        self.assertNotIn('pbft_timeout', calls)


    def test_cached_local_candidate_keeps_admission_and_fresh_guard_without_reexecution(self):
        runtime, calls = self.runtime()
        runtime._finish = lambda reply: reply
        runtime.operator._validate_proposal = Mock(side_effect=lambda value: calls.append('fresh-candidate-guard'))
        candidate = {'header': 'aa', 'body': 'bb', 'actions': ['cc']}
        NetworkRuntime._start_batch(runtime, {'parent': 'aa' * 32}, ['cc'], candidate=candidate)
        self.assertIn('submit', calls)
        self.assertNotIn('candidate', calls)
        self.assertLess(calls.index('fresh-candidate-guard'), calls.index('pbft_timeout'))
        self.assertLess(calls.index('pbft_timeout'), calls.index('pbft_start'))

    def test_startup_tick_that_applies_an_existing_certificate_never_starts_stale_candidate(self):
        runtime, calls = self.runtime()
        runtime._finish = lambda reply: {'applied': [1]}
        runtime.operator._validate_proposal = Mock()
        candidate = {'header': 'aa', 'body': 'bb', 'actions': []}
        NetworkRuntime._start_batch(runtime, {'parent': 'aa' * 32}, [], candidate=candidate)
        self.assertIn('pbft_timeout', calls)
        self.assertNotIn('pbft_start', calls)

    def test_cached_candidate_cannot_substitute_another_action_list(self):
        runtime, calls = self.runtime()
        candidate = {'header': 'aa', 'body': 'bb', 'actions': ['wrong']}
        with self.assertRaisesRegex(CustodyError, 'changed its exact native admitted actions'):
            NetworkRuntime._start_batch(runtime, {'parent': 'aa' * 32}, ['cc'], candidate=candidate)
        self.assertNotIn('pbft_start', calls)


if __name__ == '__main__':
    unittest.main()
