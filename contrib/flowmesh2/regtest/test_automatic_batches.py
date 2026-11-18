#!/usr/bin/env python3
"""Durable public-client queue recovery and fair autonomous scheduling checks."""
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest

from automatic_batches import AutomaticBatches
from custody import CustodyError


class Worker:
    def __init__(self):
        self.actions, self.codes, self.calls = {}, {}, []

    def action(self, number, account=0):
        frame = number.to_bytes(4, 'big').hex()
        value = {'id': hashlib.sha256(bytes.fromhex(frame)).hexdigest(), 'account': account.to_bytes(32, 'big').hex(), 'frame': frame}
        self.actions[frame] = value
        return value

    def request(self, command, **fields):
        self.calls.append((command, fields))
        if command == 'info':
            return {'domain': '12' * 32, 'seat': 1}
        action = self.actions[fields['frame']]
        code = self.codes.get(action['frame'], 'ADMITTED')
        return {'code': code, 'id': action['id'], 'account': action['account'],
                'pending': code in ('ADMITTED', 'DUPLICATE'), 'included': code == 'INCLUDED'} if code != 'INCLUDED' else {
                    'code': 'DUPLICATE', 'id': action['id'], 'account': action['account'], 'pending': False, 'included': True}


class Runtime:
    MAX_APP_BYTES = 0xfff000

    def __init__(self, directory, worker, enabled=True):
        def immutable(path, value):
            encoded = (json.dumps(value, sort_keys=True, separators=(',', ':')) + '\n').encode()
            if path.exists() and path.read_bytes() != encoded:
                raise CustodyError('immutable identity changed')
            path.write_bytes(encoded)
        self.operator = SimpleNamespace(directory=directory, worker=worker, config={'automatic_batches': enabled},
                handovers=SimpleNamespace(write_once=immutable), pending=None, pending_frozen=False,
                source=SimpleNamespace(scan=lambda *args: ([], [], {'to': {'height': 1, 'hash': '00' * 32}})))
        self.seals, self.messages = [], []

    def _broadcast(self, value):
        self.messages.append(value)

    def seal(self, actions, **fields):
        self.seals.append((actions, fields))


def info(**fields):
    return {'pending_actions': 1, 'observation_frozen': False, 'changing_view': False,
            'head_hash': '00' * 32, **fields}


class AutomaticBatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.directory = Path(self.temp.name)
        self.worker = Worker()
        self.runtime = Runtime(self.directory, self.worker)
        self.queue = AutomaticBatches(self.runtime)

    def tearDown(self):
        self.temp.cleanup()

    def add(self, number, account=0):
        value = self.worker.action(number, account)
        self.queue.enqueue(value['frame'], self.worker.request('submit', frame=value['frame']), public=True)
        return value

    def test_public_admission_is_durable_and_exact_retry_is_idempotent(self):
        value = self.add(1)
        saved = json.loads(self.queue.path.read_text())
        self.assertEqual(saved['actions'], [value])
        self.assertTrue(self.queue.identity_path.exists())
        self.queue.enqueue(value['frame'], self.worker.request('submit', frame=value['frame']), public=True)
        self.assertEqual(len(self.queue.pending), 1)
        self.assertEqual(self.queue.stats['public_staged'], 1)

    def test_native_refusal_never_enters_durable_queue(self):
        value = self.worker.action(2)
        self.queue.enqueue(value['frame'], {'code': 'BAD_SIG'})
        self.assertFalse(self.queue.path.exists())
        self.assertFalse(self.queue.pending)

    def test_restart_retains_transient_admission_and_retries_it(self):
        value = self.add(3)
        self.worker.codes[value['frame']] = 'BUDGET'
        reopened = AutomaticBatches(self.runtime)
        self.assertEqual(list(reopened.pending), [value['id']])
        self.worker.codes[value['frame']] = 'ADMITTED'
        reopened.step(100, info())
        reopened.step(101, info())
        self.assertTrue(self.runtime.seals)
        self.assertTrue(self.runtime.messages)

    def test_certified_inclusion_and_expiry_retire_without_replaying(self):
        first, second = self.add(4), self.add(5)
        self.worker.codes[first['frame']] = 'INCLUDED'
        self.worker.codes[second['frame']] = 'EXPIRED'
        self.queue.on_applied({'applied': [1]})
        self.assertFalse(self.queue.pending)
        self.assertEqual(json.loads(self.queue.path.read_text())['actions'], [])

    def test_recovery_refuses_bad_signature_or_identity_substitution(self):
        value = self.add(6)
        self.worker.codes[value['frame']] = 'BAD_SIG'
        with self.assertRaisesRegex(CustodyError, 'no longer authenticates'):
            AutomaticBatches(self.runtime)
        self.worker.codes[value['frame']] = 'ADMITTED'
        self.worker.actions[value['frame']] = {**value, 'account': 'aa' * 32}
        with self.assertRaisesRegex(CustodyError, 'native authenticated identity'):
            AutomaticBatches(self.runtime)

    def test_missing_acknowledged_queue_refuses_fresh_start(self):
        self.add(7)
        self.queue.path.unlink()
        with self.assertRaisesRegex(CustodyError, 'pointer is missing'):
            AutomaticBatches(self.runtime)

    def test_bounded_queue_refuses_without_overwriting_prior_actions(self):
        first = self.add(8)
        self.queue.CAPACITY = 1
        second = self.worker.action(9)
        with self.assertRaisesRegex(CustodyError, 'queue is full'):
            self.queue.enqueue(second['frame'], self.worker.request('submit', frame=second['frame']))
        self.assertEqual(json.loads(self.queue.path.read_text())['actions'], [first])

    def test_gossip_has_per_account_and_total_fair_bounds(self):
        for account in range(10):
            for sequence in range(10):
                self.add(100 + account * 10 + sequence, account)
        self.queue._gossip()
        frames = self.runtime.messages[-1]['actions']
        counts = {}
        for frame in frames:
            account = self.worker.actions[frame]['account']
            counts[account] = counts.get(account, 0) + 1
        self.assertEqual(len(frames), 64)
        self.assertEqual(max(counts.values()), 8)
        self.assertEqual(len(counts), 8)

    def test_view_change_and_frozen_source_do_not_start_a_competing_batch(self):
        self.add(10)
        self.queue.step(100, info(changing_view=True))
        self.assertFalse(self.runtime.seals)
        self.runtime.operator.pending = {'parent_head_hash': '00' * 32}
        self.runtime.operator.pending_frozen = True
        self.queue.step(101, info())
        self.assertFalse(self.runtime.seals)

    def test_confirmed_source_event_schedules_without_admin_seal(self):
        self.runtime.operator.source.scan = lambda *args: ([{'deposit': True}], [], {'to': {'height': 4, 'hash': 'ab' * 32}})
        self.queue.step(100, info(pending_actions=0))
        self.assertEqual(self.runtime.seals, [(None, {'evm_end': {'height': 4, 'hash': 'ab' * 32}})])

    def test_disabled_fault_fixture_does_not_automatically_schedule(self):
        self.queue.enabled = False
        self.add(11)
        self.queue.step(100, info())
        self.assertFalse(self.runtime.seals)
        self.assertFalse(self.runtime.messages)


if __name__ == '__main__':
    unittest.main()
