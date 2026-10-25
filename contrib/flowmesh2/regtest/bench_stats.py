#!/usr/bin/env python3
"""Latency statistics for benchmark.py's extra reports (task 1.5, PERF-3).

The gate's own distribution and pass/fail rule stay in benchmark.py. These
helpers only add reports: a running percentile record, and a distribution in
which every refused or timed-out submission counts as one sample at the
timeout value, so failures stay in the denominator.
"""
import bisect
import math


def nearest_rank(ordered, fraction):
    """The nearest-rank percentile benchmark.py's gate uses, over a sorted list."""
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


class RunningPercentiles:
    """Exact running p50/p95/p99, with a checkpoint every `every` samples."""

    def __init__(self, every=100):
        if type(every) is not int or every < 1:
            raise ValueError('checkpoint interval must be a positive integer')
        self.every = every
        self.ordered = []
        self.checkpoints = []

    def add(self, value):
        bisect.insort(self.ordered, value)
        if len(self.ordered) % self.every == 0:
            self.checkpoints.append(self.snapshot())

    def snapshot(self):
        if not self.ordered:
            return None
        return {'samples': len(self.ordered), 'p50_ms': nearest_rank(self.ordered, .5),
                'p95_ms': nearest_rank(self.ordered, .95), 'p99_ms': nearest_rank(self.ordered, .99)}


def with_failures(measured, failures, timeout_ms):
    """The distribution over successes and failures, each failure at timeout_ms."""
    if failures < 0 or timeout_ms <= 0:
        raise ValueError('failures must be non-negative and the timeout positive')
    values = sorted(list(measured) + [timeout_ms] * failures)
    if not values:
        return None
    return {'samples': len(values), 'successes': len(measured), 'failures': failures, 'failure_value_ms': timeout_ms,
            'p50_ms': nearest_rank(values, .5), 'p95_ms': nearest_rank(values, .95), 'p99_ms': nearest_rank(values, .99),
            'min_ms': values[0], 'max_ms': values[-1]}
