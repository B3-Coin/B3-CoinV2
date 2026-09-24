#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Offline checks for benchmark bookkeeping; never starts a node."""

import unittest

from feature_flowmesh_performance import arrival_offsets, expected_balance, summarize_window
from feature_flowmesh_release import TRADE_PRICE
from flowmesh_performance_analyze import runtime_spans, stats


class PerformanceMetricsTest(unittest.TestCase):
    def sample(self, elapsed=150, status="complete", in_window=True):
        return {"status": status, "kind": "fill_bid", "client_certified_ms": elapsed,
                "client_certified_from_offer_ms": elapsed + 20,
                "account_state_verified_ms": elapsed + 30,
                "certified_during_offer_window": in_window,
                "account_verified_during_offer_window": in_window,
                "completed_during_offer_window": in_window}

    def test_arrivals_are_not_completion_relative(self):
        self.assertEqual(arrival_offsets(4, .5), [0, 2, 4, 6])
        self.assertEqual(arrival_offsets(8, 0), [0] * 8)
        with self.assertRaises(ValueError):
            arrival_offsets(-1, 1)

    def test_drain_is_not_in_window_throughput(self):
        result = summarize_window([self.sample(), self.sample(in_window=False)], [], 2, 4)
        self.assertEqual(result["offered"], 2)
        self.assertEqual(result["completed"], 2)
        self.assertEqual(result["completed_actions_per_offer_second"], .5)
        self.assertEqual(result["completed_actions_per_elapsed_second"], .5)

    def test_failures_remain_in_denominator(self):
        failed = {"status": "failed", "kind": "fill_bid"}
        result = summarize_window([self.sample(), failed], [], 2, 2)
        self.assertEqual(result["offered"], 2)
        self.assertEqual(result["failed"], 1)
        self.assertFalse(result["performance_pass"])

    def test_gate_is_p50_200_and_p95_600(self):
        result = summarize_window([self.sample(180), self.sample(550)], [], 2, 2)
        self.assertTrue(result["performance_pass"])
        result = summarize_window([self.sample(201), self.sample(550)], [], 2, 2)
        self.assertFalse(result["performance_pass"])
        result = summarize_window([self.sample(180), self.sample(601)], [], 2, 2)
        self.assertFalse(result["performance_pass"])

    def test_reservation_release_and_fill_are_distinct(self):
        initial = {"base_available": 0, "base_reserved": 0,
                   "b3_available_atoms": 1_000_000_000, "b3_reserved_atoms": 0}
        reserved = expected_balance(initial, "resting_bid")
        self.assertEqual(expected_balance(reserved, "cancel"), initial)
        filled = expected_balance(initial, "fill_bid")
        self.assertEqual(filled["base_available"], 1)
        self.assertEqual(filled["b3_available_atoms"], initial["b3_available_atoms"] - TRADE_PRICE)

    def test_empty_samples_are_unknown_not_fast(self):
        self.assertEqual(stats([])["availability"], "unknown")
        self.assertIsNone(stats([])["p50_ms"])

    def test_trace_spans_never_cross_nodes_or_restarts(self):
        base = {"node": "0", "segment": 0, "market_id": "market", "epoch": 0,
                "sequence": 1, "object_id": "candidate", "monotonic_us": 1000,
                "stage": "publication_started"}
        end = {**base, "monotonic_us": 4000, "stage": "durably_applied"}
        self.assertEqual(runtime_spans([base, end])[0]["elapsed_ms"], 3)
        self.assertEqual(runtime_spans([base, {**end, "node": "1"}]), [])
        self.assertEqual(runtime_spans([base, {**end, "segment": 1}]), [])

    def test_conflicting_candidate_is_not_paired(self):
        begin = {"node": "0", "segment": 0, "market_id": "market", "epoch": 0,
                 "sequence": 1, "object_id": "candidate-A", "monotonic_us": 1000,
                 "stage": "execution_started"}
        end = {**begin, "object_id": "candidate-B", "monotonic_us": 2000,
               "stage": "execution_completed"}
        self.assertEqual(runtime_spans([begin, end]), [])


if __name__ == "__main__":
    unittest.main()
