#!/usr/bin/env python3
"""bench_stats.py: running percentiles and failures counted in the denominator."""

from pathlib import Path
import random
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from bench_stats import RunningPercentiles, nearest_rank, with_failures  # noqa: E402
import benchmark  # noqa: E402


class BenchStatsTests(unittest.TestCase):
    def test_nearest_rank_matches_the_gate_distribution(self):
        values = [random.Random(7).uniform(100, 400) for _ in range(137)]
        gate = benchmark.distribution(values)
        ordered = sorted(values)
        self.assertEqual(nearest_rank(ordered, .5), gate['p50_ms'])
        self.assertEqual(nearest_rank(ordered, .95), gate['p95_ms'])
        self.assertEqual(nearest_rank(ordered, .99), gate['p99_ms'])

    def test_running_percentiles_checkpoint_and_stay_exact(self):
        running = RunningPercentiles(every=100)
        values = [float(value) for value in range(1000, 0, -1)]
        for value in values:
            running.add(value)
        self.assertEqual([item['samples'] for item in running.checkpoints], list(range(100, 1001, 100)))
        self.assertEqual(running.checkpoints[0], {'samples': 100, 'p50_ms': 950.0, 'p95_ms': 995.0, 'p99_ms': 999.0})
        self.assertEqual(running.snapshot(), {'samples': 1000, 'p50_ms': 500.0, 'p95_ms': 950.0, 'p99_ms': 990.0})
        self.assertIsNone(RunningPercentiles().snapshot())
        with self.assertRaises(ValueError):
            RunningPercentiles(every=0)

    def test_failures_count_in_the_denominator_at_the_timeout(self):
        measured = [100.0] * 94
        report = with_failures(measured, 6, 45_000)
        self.assertEqual((report['samples'], report['successes'], report['failures']), (100, 94, 6))
        self.assertEqual(report['p50_ms'], 100.0)
        self.assertEqual(report['p95_ms'], 45_000)
        self.assertEqual(report['max_ms'], 45_000)
        self.assertEqual(with_failures(measured, 0, 45_000)['p95_ms'], 100.0)
        self.assertEqual(with_failures([], 2, 45_000)['p50_ms'], 45_000)
        self.assertIsNone(with_failures([], 0, 45_000))
        with self.assertRaises(ValueError):
            with_failures(measured, -1, 45_000)


class BenchmarkArgumentTests(unittest.TestCase):
    def parse(self, *extra):
        return benchmark.parse_arguments(['--source-root', '/src', '--evidence-dir', '/ev', '--worker', '/w',
                                          '--b3coind', '/b', '--b3coin-cli', '/c', '--tools', '/t', '--solc', '/s',
                                          *extra])

    def test_default_invocation_keeps_the_gate_semantics(self):
        args = self.parse()
        self.assertEqual((args.samples, args.warmup, args.concurrency, args.failures), (100, 20, 1, 'abort'))

    def test_extra_modes_need_the_authenticated_path(self):
        with self.assertRaises(SystemExit):
            self.parse('--concurrency', '4')
        with self.assertRaises(SystemExit):
            self.parse('--failures', 'count')
        args = self.parse('--transport', 'authenticated-tcp', '--concurrency', '4', '--failures', 'count',
                          '--samples', '1000')
        self.assertEqual((args.concurrency, args.failures, args.samples), (4, 'count', 1000))

    def test_bounds(self):
        for extra in (('--samples', '99'), ('--samples', '1001'), ('--warmup', '4'),
                      ('--transport', 'authenticated-tcp', '--concurrency', '0'),
                      ('--transport', 'authenticated-tcp', '--concurrency', '9')):
            with self.subTest(extra=extra), self.assertRaises(SystemExit):
                self.parse(*extra)


if __name__ == '__main__':
    unittest.main()
