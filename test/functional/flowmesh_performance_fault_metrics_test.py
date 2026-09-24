#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Runtime-free regressions for fault-harness observation and cleanup logic.

Run from this directory:
    python3 -B -m unittest -v flowmesh_performance_fault_metrics_test

RPC replies, clocks, and threads are mocked. These tests start no daemons,
write no fixture reports, and do not qualify real catch-up, signing, network
faults, rate-limit recovery, or thread termination. The cleanup tests execute
the actual harness finally block, extracted with AST, rather than a copy.
"""

import ast
import copy
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import feature_flowmesh_performance_faults as faults
from feature_flowmesh_latency import FlowMeshLatencyTest


TARGET = {"sequence": 5, "hash": "a" * 64, "state_root": "b" * 64}


def ready_snapshot(**changes):
    return {
        "running": True, "halt": "none", "certified": True,
        "next_microblock_sequence": 6, "last_microblock_hash": TARGET["hash"],
        "state_root": TARGET["state_root"], "observer_only": False,
        "paused": False, "chain_reconciling": False, "error": "", **changes,
    }


def data(snapshot, entries=()):
    return {"snapshot": snapshot, "history": {"entries": list(entries)}}


def readiness_subject(responses):
    node = Mock()
    node.getflowmeshmarketdata.side_effect = responses
    return SimpleNamespace(
        nodes=[None, None, None, node], market_specs=[{"market_id": "generated-market"}],
        pump_b3=Mock(), contains_target=FlowMeshLatencyTest.contains_target, recovery={})


class EligibilityTests(unittest.TestCase):
    def test_each_closed_or_unknown_gate_is_rejected(self):
        for field, value in (
            ("paused", True), ("chain_reconciling", True), ("observer_only", True),
            ("error", "reconciliation failed"), ("running", False), ("halt", "stalled"),
            ("paused", None), ("chain_reconciling", None), ("error", None),
        ):
            with self.subTest(field=field, value=value):
                subject = readiness_subject([
                    data(ready_snapshot(**{field: value})), data(ready_snapshot())])
                with patch.object(faults.time, "sleep"), patch.object(faults, "host_us", side_effect=[200, 300]):
                    observed = faults.FlowMeshPerformanceFaultsTest.wait_recovery_target(
                        subject, TARGET, require_signing=True)
                self.assertEqual(observed, 300)
                self.assertEqual(subject.nodes[3].getflowmeshmarketdata.call_count, 2)
                self.assertFalse(subject.recovery["last_readiness_observation"]["signing_share_proven"])

    def test_catchup_can_precede_signing_eligibility(self):
        subject = readiness_subject([data(ready_snapshot(observer_only=True, paused=True))])
        with patch.object(faults, "host_us", return_value=200):
            observed = faults.FlowMeshPerformanceFaultsTest.wait_recovery_target(subject, TARGET)
        self.assertEqual(observed, 200)
        observation = subject.recovery["last_readiness_observation"]
        self.assertTrue(observation["exact_target_applied"])
        self.assertTrue(observation["status"]["observer_only"])
        self.assertFalse(observation["signing_share_proven"])

    def test_matching_sequence_without_matching_hash_is_not_catchup(self):
        subject = readiness_subject([
            data(ready_snapshot(last_microblock_hash="c" * 64)), data(ready_snapshot())])
        with patch.object(faults.time, "sleep"):
            faults.FlowMeshPerformanceFaultsTest.wait_recovery_target(subject, TARGET)
        self.assertEqual(subject.nodes[3].getflowmeshmarketdata.call_count, 2)

    def test_exact_tip_target_also_requires_matching_state_root(self):
        subject = readiness_subject([data(ready_snapshot(state_root="c" * 64))])
        with self.assertRaises(AssertionError):
            faults.FlowMeshPerformanceFaultsTest.wait_recovery_target(subject, TARGET)


class ReturningTargetTests(unittest.TestCase):
    def exercise(self, response, phase, *, released):
        matching = {
            "required_replicas": [0, 1, 2], "initial_submission_host_us": 10,
            "certified_status": {"microblock_sequence": 5, "microblock_hash": TARGET["hash"]}}
        different = copy.deepcopy(matching)
        different["certified_status"]["microblock_hash"] = "d" * 64
        rpc = Mock()
        rpc.getflowmeshmarketdata.return_value = response
        recovery = {"restart_host_us": 100, "phase": phase}
        if released:
            recovery["bulk_release_host_us"] = 150
        subject = SimpleNamespace(
            performance_report={"samples": [matching, different]},
            market_specs=[{"market_id": "generated-market"}], returning_rpc=rpc,
            recovery=recovery, contains_target=FlowMeshLatencyTest.contains_target)
        with patch.object(faults, "host_us", return_value=200):
            faults.FlowMeshPerformanceFaultsTest.observe_returning_replica(subject)
        self.assertNotIn("returning_replica_observation", different)
        self.assertNotIn("every_replica_observed_ms", matching)
        observation = matching["returning_replica_observation"]
        self.assertEqual(observation["from_original_submission_ms"], .19)
        self.assertEqual(observation["from_restart_ms"], .1)
        self.assertEqual(observation["during_phase"], phase)
        return observation

    def test_exact_target_arrival_is_recorded_even_during_held_phase(self):
        # A phase label is not proof that every possible catch-up path is held.
        observation = self.exercise(data(ready_snapshot()), "node3_bulk_held", released=False)
        self.assertNotIn("from_bulk_release_ms", observation)

    def test_historical_target_uses_exact_hash_and_distinct_release_clock(self):
        response = data(ready_snapshot(next_microblock_sequence=7, last_microblock_hash="c" * 64),
                        [{"sequence": 5, "microblock_hash": TARGET["hash"]}])
        observation = self.exercise(response, "node3_bulk_throttled", released=True)
        self.assertEqual(observation["from_bulk_release_ms"], .05)


class CleanupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = ast.parse(Path(faults.__file__).read_text(encoding="utf-8"))
        harness = next(node for node in source.body if isinstance(node, ast.ClassDef) and
                       node.name == "FlowMeshPerformanceFaultsTest")
        run = next(node for node in harness.body if isinstance(node, ast.FunctionDef) and node.name == "run_test")
        actual_finally = next(node for node in run.body if isinstance(node, ast.Try)).finalbody
        scaffold = ast.parse(
            "def exercise_tail(self, incoming=None):\n"
            "    try:\n"
            "        if incoming is not None:\n"
            "            raise incoming\n"
            "    finally:\n"
            "        pass\n")
        scaffold.body[0].body[0].finalbody = copy.deepcopy(actual_finally)
        namespace = vars(faults).copy()
        exec(compile(ast.fix_missing_locations(scaffold), "<actual fault cleanup regression>", "exec"), namespace)
        cls.cleanup = staticmethod(namespace["exercise_tail"])

    def subject(self, alive):
        reports = []
        thread = Mock()
        thread.is_alive.return_value = alive
        fault = Mock()
        fault.snapshot.return_value = {"bulk_held": False}
        subject = SimpleNamespace(
            fault=fault, returning_rpc=None, scheduler_stop=Mock(), worker_stop=Mock(),
            workers=[], threads=[thread], end_b3_workload=Mock(), log=Mock(),
            options=SimpleNamespace(tmpdir="unused-mocked-report-directory"),
            performance_report={"correctness_pass": True, "fault_scenario_pass": True})
        subject.write_report = lambda: reports.append(copy.deepcopy(subject.performance_report))
        return subject, reports

    def test_lingering_thread_saves_failed_report_before_raising(self):
        subject, reports = self.subject(alive=True)
        with self.assertRaisesRegex(AssertionError, "failure report preserved"):
            self.cleanup(subject)
        self.assertEqual(len(reports), 1)
        self.assertFalse(reports[0]["correctness_pass"])
        self.assertFalse(reports[0]["fault_scenario_pass"])
        self.assertIn("cleanup_error", reports[0])

    def test_cleanup_preserves_original_exception_identity(self):
        for original in (ValueError("primary failure"), KeyboardInterrupt()):
            with self.subTest(exception=type(original).__name__):
                subject, reports = self.subject(alive=True)
                with self.assertRaises(type(original)) as caught:
                    self.cleanup(subject, original)
                self.assertIs(caught.exception, original)
                self.assertEqual(len(reports), 1)
                self.assertFalse(reports[0]["all_harness_threads_stopped"])

    def test_clean_shutdown_retains_success_without_cleanup_error(self):
        subject, reports = self.subject(alive=False)
        self.cleanup(subject)
        self.assertEqual(len(reports), 1)
        self.assertTrue(reports[0]["correctness_pass"])
        self.assertNotIn("cleanup_error", reports[0])


if __name__ == "__main__":
    unittest.main()
