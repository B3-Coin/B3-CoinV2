#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Offline checks for benchmark bookkeeping; never starts a node."""

import argparse
import copy
import io
import json
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import feature_flowmesh_performance as performance
from feature_flowmesh_performance import FlowMeshPerformanceTest, arrival_offsets, checked_execution_outcome, expected_balance, submit_context, summarize_window
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


class SubmitWaitProbeTest(unittest.TestCase):
    def parser(self):
        parser = argparse.ArgumentParser()
        owner = object.__new__(FlowMeshPerformanceTest)
        owner.add_options(parser)
        return parser

    def test_submit_wait_option_defaults_to_zero_and_rejects_outside_client_bound(self):
        parser = self.parser()
        self.assertEqual(parser.parse_args([]).performance_submit_wait_ms, 0)
        for value in ("0", "1", "2000", "2500"):
            with self.subTest(value=value):
                parsed = parser.parse_args(["--performance-submit-wait-ms", value])
                self.assertEqual(parsed.performance_submit_wait_ms, int(value))
        for value in ("-1", "2501", "1.5", "nan", "inf", "true"):
            with self.subTest(value=value), patch("sys.stderr", new=io.StringIO()), self.assertRaises(SystemExit):
                parser.parse_args(["--performance-submit-wait-ms", value])

    def test_nonzero_wait_requires_explicit_native_result_profile_before_setup(self):
        for profile, outcome in (("ladder", False), ("ladder", True), ("matched-repair", True), ("native-queue", False)):
            with self.subTest(profile=profile, outcome=outcome):
                owner = object.__new__(FlowMeshPerformanceTest)
                owner.options = SimpleNamespace(performance_profile=profile,
                    performance_submit_wait_ms=2000, performance_require_outcome=outcome)
                with patch.object(performance.FlowMeshLatencyTest, "set_test_params") as setup:
                    with self.assertRaisesRegex(ValueError, "native-queue.*require-outcome"):
                        owner.set_test_params()
                    setup.assert_not_called()

    def execute_probe(self, kind, wait=None):
        class ProbeStopped(RuntimeError):
            pass
        clock = [1_000_000]
        observed_delay_us = 275_000
        submitted = []
        identity = "a" * 64
        status = {**ExecutionOutcomeMeasurementTest().status(), "action_id": identity}
        if kind == "cancel":
            del status["outcome"]["fill"]
        elif kind == "resting_bid":
            status["outcome"]["fill"].update(quantity=0, quote_atoms=0, fee_atoms=0, net_quote_atoms=0)
        class RPC:
            def submitflowmeshorder(self, *args):
                submitted.append(("submitflowmeshorder", args))
                clock[0] += observed_delay_us
                return copy.deepcopy(status)
            def cancelflowmeshorder(self, *args):
                submitted.append(("cancelflowmeshorder", args))
                clock[0] += observed_delay_us
                return copy.deepcopy(status)
            def retryflowmeshaction(self, *args):
                raise AssertionError("A completed probe must not resend")
            def getflowmeshactionstatus(self, *args):
                raise AssertionError("A verified result needs no status poll")
        options = SimpleNamespace(performance_action_timeout=60, performance_require_outcome=True)
        if wait is not None:
            options.performance_submit_wait_ms = wait
        def followup(*args):
            raise ProbeStopped()
        owner = SimpleNamespace(options=options, stop_requested=lambda deadline: None,
            observed_rpc_return_us=lambda: clock[0], followup_account_read=followup)
        worker = {"market_id": "market", "account": {"next_sequence": 7, "account_id": "account",
            "base_available": 0, "base_reserved": 0, "b3_available_atoms": 2_000_000_000, "b3_reserved_atoms": 0}}
        sample = {"kind": kind, "enqueued_host_us": clock[0], "scheduled_host_us": clock[0],
            "attempts": [], "receipt_observations": []}
        with patch.object(performance, "host_us", side_effect=lambda: clock[0]), \
             patch.object(performance.time, "monotonic", side_effect=lambda: clock[0] / 1_000_000):
            with self.assertRaises(ProbeStopped):
                FlowMeshPerformanceTest.execute_action(owner, worker, RPC(), [], sample)
        return sample, submitted

    def test_default_and_explicit_zero_preserve_exact_order_and_cancel_arguments(self):
        for wait in (None, 0):
            for kind in ("fill_bid", "resting_bid", "cancel"):
                with self.subTest(kind=kind, wait=wait):
                    sample, submitted = self.execute_probe(kind, wait)
                    expected = ("market", "bid", 7) if kind == "cancel" else (
                        "market", "bid", TRADE_PRICE if kind == "fill_bid" else TRADE_PRICE // 2, 1, 7)
                    self.assertEqual(submitted[0][1], expected)
                    self.assertEqual(len(submitted), 1)

    def test_wait_appends_exact_trailing_argument_without_changing_original_identity(self):
        for kind in ("fill_bid", "cancel"):
            with self.subTest(kind=kind):
                sample, submitted = self.execute_probe(kind, 2000)
                expected = ("market", "bid", 7, 2000) if kind == "cancel" else (
                    "market", "bid", TRADE_PRICE, 1, 7, 2000)
                self.assertEqual(submitted[0][1], expected)
                self.assertEqual(len(submitted), 1)
                self.assertEqual(sample["action_id"], "a" * 64)
                self.assertEqual(sample["attempts"], [])
                self.assertEqual(sample["submit_wait_ms"], 2000)

    def test_result_clock_includes_the_original_submission_wait(self):
        sample, _ = self.execute_probe("fill_bid", 2000)
        self.assertEqual(sample["initial_submission_host_us"], 1_000_000)
        self.assertEqual(sample["initial_response_ms"], 275)
        self.assertEqual(sample["client_certified_ms"], 275)
        self.assertEqual(sample["execution_outcome_verified_ms"], 275)
        self.assertEqual(sample["execution_outcome_verified_from_offer_ms"], 275)


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


class ExecutionOutcomeMeasurementTest(unittest.TestCase):
    def status(self):
        return {"receipt_state": "certified_inclusion", "certificate_verified": True,
            "microblock_hash": "entry", "microblock_sequence": 12, "outcome_verified": True,
            "outcome": {"status": "applied", "final": True, "evidence": "certified_result_preimage",
                "microblock_hash": "entry", "microblock_sequence": 12,
                "fill": {"side": "bid", "superseded_in_entry": False, "quantity": 1,
                    "cleared": True, "price": TRADE_PRICE, "quote_atoms": TRADE_PRICE,
                    "fee_atoms": 0, "net_quote_atoms": TRADE_PRICE}}}

    def test_inclusion_is_not_an_execution_result(self):
        status = self.status()
        status["outcome_verified"] = False
        del status["outcome"]
        self.assertIsNone(checked_execution_outcome(status, "fill_bid"))

    def test_matched_fill_has_exact_certified_quantity_price_and_fee(self):
        status = self.status()
        self.assertEqual(checked_execution_outcome(status, "fill_bid"), status["outcome"])
        for field, value in (("side", "ask"), ("quantity", 0), ("cleared", False),
                             ("price", TRADE_PRICE - 1), ("fee_atoms", 1),
                             ("quote_atoms", TRADE_PRICE + 1), ("superseded_in_entry", True)):
            with self.subTest(field=field):
                bad = copy.deepcopy(status)
                bad["outcome"]["fill"][field] = value
                with self.assertRaises(AssertionError):
                    checked_execution_outcome(bad, "fill_bid")

    def test_rejected_unverified_and_different_entry_claims_fail(self):
        status = self.status()
        for field, value in (("status", "rejected"), ("final", False),
                             ("evidence", "endpoint_history"), ("microblock_hash", "other"),
                             ("microblock_sequence", 13)):
            with self.subTest(field=field):
                bad = copy.deepcopy(status)
                bad["outcome"][field] = value
                with self.assertRaises(AssertionError):
                    checked_execution_outcome(bad, "fill_bid")
        status["certificate_verified"] = False
        with self.assertRaises(AssertionError):
            checked_execution_outcome(status, "fill_bid")

    def test_resting_order_and_cancel_do_not_claim_a_fill(self):
        status = self.status()
        status["outcome"]["fill"].update(quantity=0, quote_atoms=0, fee_atoms=0, net_quote_atoms=0)
        self.assertIsNotNone(checked_execution_outcome(status, "resting_bid"))
        with self.assertRaises(AssertionError):
            checked_execution_outcome(status, "cancel")
        del status["outcome"]["fill"]
        self.assertIsNotNone(checked_execution_outcome(status, "cancel"))

    def test_missing_result_never_passes_stronger_gate(self):
        sample = PerformanceMetricsTest().sample(150)
        result = summarize_window([sample], [], 1, 1, require_outcomes=True)
        self.assertFalse(result["correctness_pass"])
        self.assertFalse(result["performance_pass"])
        self.assertFalse(result["offer_inclusive_target_pass"])

    def test_outcome_clock_not_faster_inclusion_clock_controls_stronger_gate(self):
        sample = PerformanceMetricsTest().sample(150)
        sample.update(execution_outcome_verified_host_us=1000,
                      execution_outcome_verified_ms=250, execution_outcome_verified_from_offer_ms=270)
        result = summarize_window([sample], [], 1, 1, require_outcomes=True)
        self.assertTrue(result["correctness_pass"])
        self.assertFalse(result["performance_pass"])
        self.assertEqual(result["execution_outcome_verified_count"], 1)
        sample.update(execution_outcome_verified_ms=180, execution_outcome_verified_from_offer_ms=190)
        self.assertTrue(summarize_window([sample], [], 1, 1, require_outcomes=True)["performance_pass"])

    def probe(self, *, require_outcome, included):
        class ProbeStopped(RuntimeError):
            pass
        identity = "a" * 64
        included = {**included, "action_id": identity}
        unknown = {"action_id": identity, "receipt_state": "unknown",
                   "certificate_verified": False, "outcome_verified": False}
        calls, polls = [], []
        class RPC:
            def submitflowmeshorder(self, *args):
                return copy.deepcopy(included)
            def getflowmeshactionstatus(self, *args):
                polls.append(True)
                return dict(unknown)
            def retryflowmeshaction(self, *args):
                calls.append("retry after verified inclusion")
                return dict(unknown)
        def stop(deadline):
            if len(polls) + len(calls) >= 3:
                raise ProbeStopped()
        def followup(*args):
            raise ProbeStopped()
        owner = SimpleNamespace(
            options=SimpleNamespace(performance_action_timeout=100, performance_require_outcome=require_outcome),
            stop_requested=stop, observed_rpc_return_us=lambda: 1_000_000, followup_account_read=followup)
        worker = {"market_id": "market", "account": {"next_sequence": 0, "account_id": "account",
            "base_available": 0, "base_reserved": 0, "b3_available_atoms": 2_000_000_000, "b3_reserved_atoms": 0}}
        sample = {"kind": "fill_bid", "enqueued_host_us": 1_000_000, "scheduled_host_us": 1_000_000,
                  "attempts": [], "receipt_observations": []}
        clock = iter(range(10, 1000, 10))
        with patch.object(performance, "host_us", return_value=1_000_000), \
             patch.object(performance.time, "monotonic", side_effect=lambda: next(clock)), \
             patch.object(performance.time, "sleep"):
            with self.assertRaises(ProbeStopped):
                FlowMeshPerformanceTest.execute_action(owner, worker, RPC(), [], sample)
        return sample, calls, polls

    def test_observed_inclusion_permanently_disables_harness_retry(self):
        included = self.status()
        included["outcome_verified"] = False
        del included["outcome"]
        sample, calls, polls = self.probe(require_outcome=True, included=included)
        self.assertEqual(calls, [])
        self.assertEqual(len(polls), 3)
        self.assertEqual(sample["client_certified_host_us"], 1_000_000)
        self.assertNotIn("execution_outcome", sample)

    def test_legacy_profile_does_not_gain_stronger_result_assertions(self):
        included = self.status()
        included["outcome"]["fill"]["quantity"] = 123
        sample, calls, polls = self.probe(require_outcome=False, included=included)
        self.assertEqual(calls, [])
        self.assertEqual(polls, [])
        self.assertIn("client_certified_host_us", sample)
        self.assertNotIn("execution_outcome", sample)


if __name__ == "__main__":
    unittest.main()
