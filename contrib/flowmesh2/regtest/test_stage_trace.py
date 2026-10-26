#!/usr/bin/env python3
"""stage_trace.py is inert unless enabled, and stage_report.py splits a trace into stages."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import stage_report  # noqa: E402

PROBE = """
import sys
sys.path.insert(0, sys.argv[1])
import custody, https_api, stage_trace
stage_trace.install('client')
print(int(hasattr(custody.LocalRpc._exchange, '__wrapped__')), int(hasattr(https_api.HttpsClient.request, '__wrapped__')))
stage_trace.event('probe', value=7)
stage_trace.total('worker', 'info', 1500)
stage_trace.total('worker', 'info', 500)
"""


def probe(environment, cwd):
    env = {key: value for key, value in os.environ.items() if key != 'FM2_STAGE_TRACE_DIR'}
    env.update(environment)
    result = subprocess.run([sys.executable, '-B', '-c', PROBE, str(HERE)], capture_output=True, text=True,
                            env=env, cwd=cwd, timeout=60)
    if result.returncode:
        raise AssertionError(result.stderr)
    return result.stdout.split()


class StageTraceTests(unittest.TestCase):
    def test_disabled_tracing_changes_nothing_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertEqual(probe({}, directory), ['0', '0'])
            self.assertEqual(list(Path(directory).iterdir()), [])

    def test_enabled_tracing_writes_events_and_totals(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = Path(directory) / 'trace'
            self.assertEqual(probe({'FM2_STAGE_TRACE_DIR': str(trace)}, directory), ['1', '1'])
            files = list(trace.glob('*.jsonl'))
            self.assertEqual(len(files), 1)
            events = [json.loads(line) for line in files[0].read_text().splitlines()]
            self.assertEqual([item['ev'] for item in events], ['process', 'probe', 'totals'])
            self.assertEqual(events[0]['role'], 'client')
            self.assertEqual(events[1]['value'], 7)
            self.assertEqual(events[2]['calls'], {'worker/info': {'count': 2, 'ns': 2000}})


def write(directory, name, events):
    (directory / name).write_text(''.join(json.dumps(item) + '\n' for item in events))


MS = 1_000_000


class StageReportTests(unittest.TestCase):
    def test_one_sample_is_split_into_its_stages(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            base = 10_000 * MS
            write(root, 'benchmark-1.jsonl', [
                {'t': 0, 'ev': 'process', 'role': 'client'},
                {'t': base, 'ev': 'sample', 'index': 0, 'phase': 'start'},
                {'t': base + 1 * MS, 'ev': 'https', 'dur': 4 * MS, 'call': 'POST submit', 'code': 'ADMITTED', 'included': False},
                {'t': base + 20 * MS, 'ev': 'https', 'dur': 4 * MS, 'call': 'POST submit', 'code': 'DUPLICATE', 'included': False},
                {'t': base + 50 * MS, 'ev': 'https', 'dur': 6 * MS, 'call': 'POST submit', 'code': 'DUPLICATE', 'included': True},
                {'t': base + 57 * MS, 'ev': 'https', 'dur': 5 * MS, 'call': 'GET totals', 'code': None, 'included': False},
                {'t': base + 63 * MS, 'ev': 'sample', 'index': 0, 'phase': 'end', 'latency_ms': 62.0, 'client_loop_ms': 70.0},
                {'t': base + 99 * MS, 'ev': 'totals', 'calls': {'https_client/POST submit': {'count': 3, 'ns': 14 * MS}}},
            ])
            for seat, applied in enumerate((40, 42, 45, 48)):
                events = [{'t': 0, 'ev': 'process', 'role': 'operator'},
                          {'t': base + applied * MS, 'ev': 'applied'}]
                events += [{'t': base + 30 * MS, 'ev': 'validate_pending', 'dur': (seat + 1) * MS},
                           {'t': base + 30 * MS, 'ev': 'verify_retained', 'dur': MS // 2},
                           {'t': base + 32 * MS, 'ev': 'worker', 'dur': 2 * MS, 'cmd': 'network_dispatch'}]
                if seat == 0:
                    events += [{'t': base + 3 * MS, 'ev': 'enqueue', 'dur': MS},
                               {'t': base + 8 * MS, 'ev': 'seal', 'dur': 12 * MS},
                               {'t': base + 9 * MS, 'ev': 'observe', 'dur': 7 * MS}]
                write(root, f'operator_wrapper-{seat}.jsonl', events)
            report = stage_report.analyse(stage_report.load(root), warmup=0)
            stages = {name: value and value['p50'] for name, value in report['stages'].items()}
            self.assertEqual(stages['latency_ms'], 62.0)
            self.assertEqual(stages['submit_ms'], 4.0)
            self.assertEqual(stages['staged_ms'], 3.0)
            self.assertEqual(stages['seal_start_ms'], 8.0)
            self.assertEqual(stages['seal_ms'], 12.0)
            self.assertEqual(stages['observe_ms'], 7.0)
            self.assertEqual(stages['applied_first_ms'], 40.0)
            self.assertEqual(stages['applied_last_ms'], 48.0)
            self.assertEqual(stages['detect_ms'], 16.0)
            self.assertEqual(stages['polls'], 4)
            self.assertEqual(stages['https_ms_mean'], 4.75)
            self.assertEqual(report['stages']['guard_ms']['n'], 4)
            self.assertEqual(report['stages']['guard_ms']['mean'], 2.5)
            self.assertEqual(stages['guard_calls'], 1)
            self.assertEqual(stage_report.covered([(0, 4), (1, 2), (3, 6), (8, 9)]), 7)
            self.assertEqual(stages['dispatches'], 1)
            self.assertEqual(stages['dispatch_ms'], 2.0)
            self.assertEqual(report['totals']['benchmark-1.jsonl']['role'], 'client')

    def test_warmup_samples_are_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            events = [{'t': 0, 'ev': 'process', 'role': 'client'}]
            for index, latency in enumerate((500.0, 10.0, 20.0)):
                events += [{'t': (index * 100 + 1) * MS, 'ev': 'sample', 'index': index, 'phase': 'start'},
                           {'t': (index * 100 + 50) * MS, 'ev': 'sample', 'index': index, 'phase': 'end',
                            'latency_ms': latency, 'client_loop_ms': latency}]
            write(root, 'benchmark-1.jsonl', events)
            stages = stage_report.analyse(stage_report.load(root), warmup=1)['stages']
            self.assertEqual(stages['latency_ms']['n'], 2)
            self.assertEqual(stages['latency_ms']['max'], 20.0)
            self.assertIsNone(stages['seal_ms'])


if __name__ == '__main__':
    unittest.main()
