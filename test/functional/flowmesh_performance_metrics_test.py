#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Offline checks for benchmark bookkeeping; never starts a node."""

import json
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest

from feature_flowmesh_performance import FlowMeshPerformanceTest, arrival_offsets, expected_balance, submit_context, summarize_window
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

    def test_same_semantic_id_is_not_a_cross_market_retry(self):
        def record(market, payload):
            body = {"method": "submit", "params": {
                "market_id": market, "action_id": "same-semantic-id", "action_hex": payload}}
            return {"action_id": "same-semantic-id", "action_hex": payload,
                    "body_hex": json.dumps(body).encode().hex()}
        rows = [record("market-A", "01"), record("market-B", "02")]
        # Former join merges the two authorized, domain-separated signatures.
        self.assertEqual(len({row["action_hex"] for row in rows}), 2)
        selected = [row for row in rows if submit_context(row) == ("market-A", "same-semantic-id")]
        self.assertEqual([row["action_hex"] for row in selected], ["01"])
        rows.append(record("market-A", "03"))
        # Do not hide changed bytes within the SAME market by selecting a hash.
        selected = [row for row in rows if submit_context(row) == ("market-A", "same-semantic-id")]
        self.assertEqual(len({row["action_hex"] for row in selected}), 2)


class CaptureCleanupTest(unittest.TestCase):
    def collect(self, results):
        temporary = TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        calls = []
        def node(index):
            def stop(command):
                calls.append((index, command))
                result = results[index]
                if isinstance(result, Exception):
                    raise result
                return result
            return SimpleNamespace(index=index, flowmeshtiming=stop)
        client_index = len(results) - 1
        invalidations = []
        owner = SimpleNamespace(
            nodes=[node(index) for index in range(client_index)], client=node(client_index),
            memory_capture_nodes=list(range(len(results))),
            options=SimpleNamespace(tmpdir=temporary.name),
            performance_report={"memory_capture": {str(index): {} for index in range(len(results))}},
            invalidate_final_result=lambda: invalidations.append(True))
        return owner, calls, invalidations, Path(temporary.name)

    def test_overflow_preserves_every_child_capture(self):
        owner, calls, invalidations, path = self.collect([
            {"events": [], "dropped": 1}, {"events": [], "dropped": 0},
            {"events": [], "dropped": 0}])
        with self.assertRaisesRegex(AssertionError, "Incomplete bounded timing capture"):
            FlowMeshPerformanceTest.end_memory_capture(owner)
        self.assertEqual(calls, [(0, "stop"), (1, "stop"), (2, "stop")])
        self.assertEqual(owner.memory_capture_nodes, [])
        self.assertEqual(invalidations, [True])
        for index in range(3):
            self.assertTrue((path / f"memory-timing-node{index}.json").exists())
        FlowMeshPerformanceTest.end_memory_capture(owner)
        self.assertEqual(len(calls), 3)

    def test_stop_failure_does_not_hide_other_nodes(self):
        owner, calls, invalidations, path = self.collect([
            RuntimeError("synthetic stop failure"), {"events": [], "dropped": 0}])
        with self.assertRaisesRegex(AssertionError, "Incomplete bounded timing capture"):
            FlowMeshPerformanceTest.end_memory_capture(owner)
        self.assertEqual(calls, [(0, "stop"), (1, "stop")])
        self.assertEqual(invalidations, [True])
        self.assertEqual(owner.performance_report["memory_capture"]["0"]["capture_error"],
                         {"type": "RuntimeError"})
        self.assertTrue((path / "memory-timing-node1.json").exists())

    def test_stream_limit_invalidates_complete_attribution(self):
        marker = {"events": [{"event": {"stage": "trace_limit_reached"}}], "dropped": 0}
        owner, calls, invalidations, _ = self.collect([marker])
        with self.assertRaisesRegex(AssertionError, "Incomplete bounded timing capture"):
            FlowMeshPerformanceTest.end_memory_capture(owner)
        self.assertEqual(calls, [(0, "stop")])
        self.assertEqual(invalidations, [True])
        self.assertEqual(owner.performance_report["memory_capture"]["0"]["limit_markers"], 1)

    def test_complete_captures_are_not_invalidated(self):
        owner, calls, invalidations, _ = self.collect([
            {"events": [], "dropped": 0}, {"events": [], "dropped": 0}])
        FlowMeshPerformanceTest.end_memory_capture(owner)
        self.assertEqual(calls, [(0, "stop"), (1, "stop")])
        self.assertEqual(invalidations, [])

    def test_interrupted_start_metadata_does_not_skip_child_stop(self):
        owner, calls, invalidations, path = self.collect([
            {"events": [], "dropped": 0}, {"events": [], "dropped": 0}])
        del owner.performance_report["memory_capture"]["0"]
        with self.assertRaisesRegex(AssertionError, "Incomplete bounded timing capture"):
            FlowMeshPerformanceTest.end_memory_capture(owner)
        self.assertEqual(calls, [(0, "stop"), (1, "stop")])
        self.assertEqual(owner.memory_capture_nodes, [])
        self.assertEqual(invalidations, [True])
        self.assertTrue(owner.performance_report["memory_capture"]["0"]["start_metadata_missing"])
        self.assertTrue((path / "memory-timing-node1.json").exists())


if __name__ == "__main__":
    unittest.main()
