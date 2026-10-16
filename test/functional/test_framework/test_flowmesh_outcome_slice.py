#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Bounded stdlib-only controls: no framework startup, daemon, network or build.

Run this file directly. The entry script is loaded with an inert fake parent;
these controls cannot establish real native emission, TLS/API availability,
clock uncertainty, zero native drops, or actual child/thread exits.
"""

import ast
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import threading
import types
import unittest
from unittest.mock import patch

import flowmesh_outcome_slice as logic


MARKET, OTHER_MARKET, ACCOUNT, ACTION, HEAD = (letter * 64 for letter in "abcde")
ENTRY_PATH = Path(__file__).parent.parent / "feature_flowmesh_outcome_slice.py"


class FakeBase:
    def independent_rpc(self, node, wallet=None):
        return node.rpc

    def run_window(self, label, **kwargs):
        self.forwarded_window = (label, kwargs)
        return "unchanged-window"

    def pump_b3(self):
        self.pumps = getattr(self, "pumps", 0) + 1

    def run_test(self):
        self.performance_report = {"all_harness_threads_stopped": True, "correctness_pass": True}


fake_parent = types.ModuleType("feature_flowmesh_performance")
fake_parent.FlowMeshPerformanceTest = FakeBase
fake_parent.host_us = lambda: 3200
fake_package = types.ModuleType("test_framework")
fake_package.__path__ = []
with patch.dict(sys.modules, {"feature_flowmesh_performance": fake_parent, "test_framework": fake_package,
                             "test_framework.flowmesh_outcome_slice": logic}):
    spec = importlib.util.spec_from_file_location("private_outcome_slice_entry", ENTRY_PATH)
    entry = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(entry)


def sample(market=MARKET, sample_id=3):
    return {"sample_id": sample_id, "phase": "burst_8", "market_id": market, "account_id": ACCOUNT}


def reply(*, inclusion=True, verified=False):
    return {"action_id": ACTION, "account_id": ACCOUNT,
            "receipt_state": "certified_inclusion" if inclusion else "queued",
            "certificate_verified": inclusion, "outcome_verified": verified,
            "microblock_sequence": 5, "microblock_hash": HEAD, "reason_code": "OUTCOME_EVIDENCE_EXPIRED",
            "action_hex": "MUST_NOT_BE_COPIED", "private_key": "MUST_NOT_BE_COPIED"}


def call(state, method, args, result, start, end, target=None):
    token = state.begin_call(sample() if target is None else target, method, args, {}, start)
    state.end_call(token, result, end)


def prepared(*, verified=False):
    state = logic.OutcomeSlice()
    state.start(1000, 1000)
    call(state, "submitflowmeshorder", (MARKET, "bid", 1, 1, 0), reply(), 1100, 1500)
    call(state, "getflowmeshactionstatus", (MARKET, ACTION), reply(verified=verified), 2000, 3000)
    return state


def native(decision=15, *, https=False):
    parent = {"stage": "ActionStatus", "market_id": MARKET, "action_id": ACTION,
              "span_id": 7, "parent_span_id": 0, "started_us": 2100, "monotonic_us": 2900,
              "outcome_upgrade_endpoint": 0, "outcome_upgrade_advertised": 1,
              "outcome_upgrade_proof_bytes": 100}
    if decision is not None:
        parent["outcome_upgrade_decision"] = decision
    events = [{"stream": "FlowMeshNativeSpan", "event": parent}]
    if https:
        events.append({"stream": "FlowMeshNativeSpan", "event": {
            "stage": "https_request", "span_id": 8, "parent_span_id": 7,
            "started_us": 2200, "monotonic_us": 2800}})
    return {"generation": 1, "enabled": False, "start_us": 1000, "stop_us": 4000,
            "dropped": 0, "bytes": 1000, "max_bytes": 16 * 1024 * 1024,
            "max_events": 32768, "events": events}


def analyzed(state=None, capture=None, reason=None):
    state = prepared(verified=True) if state is None else state
    frozen = state.freeze(reason or state.stop_reason(3200) or "finally", 3200)
    return logic.analyze_capture(frozen, native() if capture is None else capture, [4100, 4200])


class ObservationControls(unittest.TestCase):
    def test_success_two_compact_observations_exact_pair(self):
        state = prepared(verified=True)
        out = analyzed(state)
        self.assertTrue(out["diagnostic_complete"])
        self.assertEqual(out["decision"], 15)
        self.assertEqual(out["target_identity"], {"market_id": MARKET, "account_id": ACCOUNT, "action_id": ACTION})
        self.assertEqual(len(out["target_observations"]), 2)
        self.assertNotIn("MUST_NOT_BE_COPIED", json.dumps(out))
        call(state, "getflowmeshactionstatus", (MARKET, ACTION), reply(), 3300, 3400)
        self.assertEqual(len(state.observations), 2)

    def test_already_verified_initial_reply_needs_no_extra_status(self):
        state = logic.OutcomeSlice()
        state.start(1000, 1000)
        call(state, "submitflowmeshorder", (MARKET, "bid", 1, 1, 0), reply(verified=True), 1100, 1500)
        out = analyzed(state, native(None))
        self.assertTrue(out["diagnostic_complete"])
        self.assertEqual(len(out["target_observations"]), 1)
        self.assertTrue(out["native_decision_not_needed_for_already_verified_original_rpc"])

    def test_preexisting_retry_can_observe_first_inclusion_but_adds_no_call(self):
        state = logic.OutcomeSlice()
        state.start(1000, 1000)
        call(state, "submitflowmeshorder", (MARKET, "bid", 1, 1, 0), reply(inclusion=False), 1100, 1500)
        call(state, "retryflowmeshaction", (MARKET, ACTION), reply(), 1600, 1900)
        call(state, "getflowmeshactionstatus", (MARKET, ACTION), reply(verified=True), 2000, 3000)
        out = analyzed(state)
        self.assertTrue(out["diagnostic_complete"])
        self.assertEqual([row["method"] for row in out["target_observations"]],
                         ["retryflowmeshaction", "getflowmeshactionstatus"])

    def test_pending_submission_then_two_original_status_returns(self):
        state = logic.OutcomeSlice()
        state.start(1000, 1000)
        call(state, "submitflowmeshorder", (MARKET, "bid", 1, 1, 0), reply(inclusion=False), 1100, 1500)
        self.assertEqual(state.observations, [])
        call(state, "getflowmeshactionstatus", (MARKET, ACTION), reply(), 1600, 1900)
        call(state, "getflowmeshactionstatus", (MARKET, ACTION), reply(verified=True), 2000, 3000)
        self.assertTrue(analyzed(state)["diagnostic_complete"])

    def test_first_status_inclusion_with_verified_outcome_needs_no_upgrade_marker(self):
        state = logic.OutcomeSlice()
        state.start(1000, 1000)
        call(state, "submitflowmeshorder", (MARKET, "bid", 1, 1, 0), reply(inclusion=False), 1100, 1500)
        call(state, "getflowmeshactionstatus", (MARKET, ACTION), reply(verified=True), 2000, 3000)
        out = analyzed(state, native(None))
        self.assertTrue(out["diagnostic_complete"])
        self.assertTrue(out["client_verified_outcome_observed"])
        self.assertTrue(out["native_upgrade_decision_not_required_for_first_verified_inclusion"])
        self.assertEqual(out["conservative_selected_rpc_native_matches"], 1)
        self.assertEqual(len(out["target_observations"]), 1)
        self.assertEqual(out["target_observations"][0]["role"], "first_verified_inclusion")
        self.assertNotIn("decision", out)
        self.assertNotIn("outcome_upgrade_decision", out["selected_native_status"])
        self.assertFalse(analyzed(state, native(999))["diagnostic_complete"])
        # The success exception does not bypass exact-pair or lifetime/clock
        # correlation and never waits for an extra status merely for a marker.
        wrong_pair = native(None)
        wrong_pair["events"][0]["event"]["market_id"] = OTHER_MARKET
        self.assertFalse(analyzed(state, wrong_pair)["diagnostic_complete"])
        uncontained = native(None)
        uncontained["events"][0]["event"]["started_us"] = 999
        self.assertFalse(analyzed(state, uncontained)["diagnostic_complete"])
        uncertain = logic.OutcomeSlice()
        uncertain.start(1000, 1200)
        call(uncertain, "submitflowmeshorder", (MARKET, "bid", 1, 1, 0), reply(inclusion=False), 1100, 1500)
        call(uncertain, "getflowmeshactionstatus", (MARKET, ACTION), reply(verified=True), 2000, 3000)
        self.assertFalse(analyzed(uncertain, native(None))["diagnostic_complete"])
        self.assertEqual(state.call_number, 2)

    def test_other_sample_and_same_action_in_other_market_do_not_join(self):
        state = logic.OutcomeSlice()
        state.start(1000, 1000)
        call(state, "submitflowmeshorder", (MARKET, "bid", 1, 1, 0), reply(), 1100, 1500, sample(sample_id=7))
        self.assertIsNone(state.identity)
        call(state, "submitflowmeshorder", (MARKET, "bid", 1, 1, 0), reply(), 1100, 1500)
        self.assertIsNone(state.begin_call(sample(), "getflowmeshactionstatus", (OTHER_MARKET, ACTION), {}, 2000))
        self.assertEqual(len(state.observations), 1)
        self.assertIn("target_call_context_missing_or_changed", state.errors)

    def test_cutoff_censors_inflight_and_late_return(self):
        state = logic.OutcomeSlice()
        state.start(1000, 1000)
        call(state, "submitflowmeshorder", (MARKET, "bid", 1, 1, 0), reply(), 1100, 1500)
        token = state.begin_call(sample(), "getflowmeshactionstatus", (MARKET, ACTION), {}, 2000)
        self.assertEqual(state.stop_reason(1000 + logic.CUTOFF_US), "fixed_cutoff")
        frozen = state.freeze("fixed_cutoff", 1000 + logic.CUTOFF_US + 7)
        self.assertEqual(frozen["target_call_censored_at_stop"]["call_number"], token)
        self.assertEqual(frozen["cutoff_overshoot_us"], 7)
        state.end_call(token, reply(verified=True), 1000 + logic.CUTOFF_US + 10)
        self.assertEqual(len(state.observations), 1)
        self.assertFalse(logic.analyze_capture(frozen, native(), [4100, 4200])["diagnostic_complete"])

    def test_return_after_fixed_cutoff_is_not_selected(self):
        state = logic.OutcomeSlice()
        state.start(1000, 1000)
        call(state, "submitflowmeshorder", (MARKET, "bid", 1, 1, 0), reply(verified=True), 1100, 1000 + logic.CUTOFF_US + 1)
        self.assertEqual(state.observations, [])
        self.assertTrue(state.late_return)
        self.assertEqual(state.stop_reason(1000 + logic.CUTOFF_US + 2), "fixed_cutoff")

    def test_terminal_11_is_no_http_response_not_wire_delivery_proof(self):
        out = analyzed(prepared(), native(11, https=True))
        self.assertTrue(out["diagnostic_complete"])
        self.assertFalse(out["client_verified_outcome_observed"])
        self.assertFalse(out["https_dispatch_attempt_observed"])
        self.assertIn("not_wire_delivery_proof", out["decision_meaning"])

    def test_global_budget_9_has_no_added_https_call(self):
        out = analyzed(prepared(), native(9))
        self.assertTrue(out["diagnostic_complete"])
        self.assertEqual(out["selected_status_https_child_count"], 0)
        bad = analyzed(prepared(), native(9, https=True))
        self.assertFalse(bad["diagnostic_complete"])

    def test_missing_native_decision_is_incomplete_not_budget(self):
        out = analyzed(prepared(), native(None))
        self.assertFalse(out["diagnostic_complete"])
        self.assertNotIn("decision", out)
        self.assertIn("native_decision_missing_or_unknown", out["observer_errors"])

    def test_selected_parent_span_identity_required_before_no_https_claim(self):
        for key, value in (("span_id", None), ("span_id", "7"), ("span_id", 0),
                           ("span_id", True), ("span_id", -1), ("parent_span_id", None),
                           ("parent_span_id", "0"), ("parent_span_id", False)):
            with self.subTest(key=key, value=value):
                capture = native(9)
                if value is None:
                    del capture["events"][0]["event"][key]
                else:
                    capture["events"][0]["event"][key] = value
                out = analyzed(prepared(), capture)
                self.assertFalse(out["diagnostic_complete"])
                self.assertIn("selected_native_status_span_identity_missing_or_invalid", out["observer_errors"])
                self.assertNotIn("selected_status_https_child_count", out)
                self.assertNotIn("https_dispatch_attempt_observed", out)
                self.assertNotIn("decision", out)
        self.assertTrue(analyzed(prepared(), native(9))["diagnostic_complete"])

    def test_code13_stale_budget_receipt_is_observed_not_a_budget_cause(self):
        state = logic.OutcomeSlice()
        state.start(1000, 1000)
        call(state, "submitflowmeshorder", (MARKET, "bid", 1, 1, 0), reply(), 1100, 1500)
        retained_budget = {**reply(), "reason_code": "OUTCOME_EVIDENCE_BUDGET"}
        call(state, "getflowmeshactionstatus", (MARKET, ACTION), retained_budget, 2000, 3000)
        out = analyzed(state, native(13, https=True))
        self.assertTrue(out["diagnostic_complete"])
        self.assertEqual(out["decision"], 13)
        self.assertEqual(out["target_observations"][-1]["reason_code"], "OUTCOME_EVIDENCE_BUDGET")
        self.assertTrue(out["exact_terminal_cause_unproven_from_receipt_alone"])
        self.assertIn("unproven_from_receipt_reason_code_alone", out["decision_meaning"])
        self.assertNotEqual(out["decision_meaning"], logic.DECISIONS[14])
        self.assertNotIn("budget_cause", out)

    def test_native_10_never_means_network_dispatch(self):
        out = analyzed(prepared(), native(10))
        self.assertIn("not_dispatch", out["decision_meaning"])
        self.assertFalse(out["https_dispatch_attempt_observed"])

    def test_ambiguous_match_and_clock_uncertainty_are_incomplete(self):
        capture = native()
        capture["events"].append({"stream": "FlowMeshNativeSpan", "event": dict(capture["events"][0]["event"], span_id=9)})
        self.assertFalse(analyzed(capture=capture)["diagnostic_complete"])
        state = prepared(verified=True)
        state.start_bounds = [1000, 1200]
        self.assertFalse(analyzed(state)["diagnostic_complete"])

    def test_other_market_and_uncontained_lifetimes_are_not_attributed(self):
        capture = native()
        capture["events"].extend([
            {"stream": "FlowMeshNativeSpan", "event": dict(capture["events"][0]["event"], market_id=OTHER_MARKET, span_id=9)},
            {"stream": "FlowMeshNativeSpan", "event": {"stage": "other", "span_id": 10, "started_us": 999, "monotonic_us": 2000}}])
        out = analyzed(capture=capture)
        self.assertTrue(out["diagnostic_complete"])
        self.assertEqual(out["conservative_selected_rpc_native_matches"], 1)
        self.assertEqual(out["uncontained_native_spans_censored"], 1)

    def test_drops_wrong_bounds_and_limit_marker_fail_closed(self):
        for changes in ({"dropped": 1}, {"max_events": 65536}, {"enabled": True}):
            with self.subTest(changes=changes):
                capture = native()
                capture.update(changes)
                self.assertFalse(analyzed(capture=capture)["diagnostic_complete"])
        capture = native()
        capture["events"].append({"stream": "other", "event": {"stage": "trace_limit_reached"}})
        self.assertFalse(analyzed(capture=capture)["diagnostic_complete"])


class FakeRPC:
    def __init__(self):
        self.calls = []
        self.result = reply(verified=True)
        self.error = None
        self.stop_error = None

    def __getattr__(self, method):
        def invoke(*args, **kwargs):
            self.calls.append((method, args, kwargs))
            if self.error:
                raise self.error
            return self.result
        return invoke

    def logging(self):
        self.calls.append(("logging", (), {}))
        return {"bench": False}

    def flowmeshtiming(self, command):
        self.calls.append(("flowmeshtiming", (command,), {}))
        if command == "stop":
            if self.stop_error:
                raise self.stop_error
            return native()
        if command == "read":
            return {"enabled": False, "generation": 0, "events": []}
        return {"enabled": True, "generation": 1, "dropped": 0, "events": []}


class HarnessControls(unittest.TestCase):
    def owner(self, tmpdir):
        owner = object.__new__(entry.FlowMeshOutcomeSliceTest)
        owner.outcome_slice = prepared(verified=True)
        owner.slice_main_thread = threading.get_ident()
        owner.slice_start = {"generation": 1}
        owner.slice_capture_owned = True
        owner.slice_stop_attempted = False
        owner.slice_report = {"diagnostic_complete": False}
        owner.trace_local = threading.local()
        owner.trace_local.sample = sample()
        owner.options = types.SimpleNamespace(tmpdir=tmpdir)
        owner.client = FakeRPC()
        owner.performance_report = {"all_harness_threads_stopped": True, "correctness_pass": True}
        return owner

    def test_proxy_forwards_exact_original_calls_once_no_economic_retry(self):
        owner = types.SimpleNamespace(outcome_slice=logic.OutcomeSlice(), trace_local=threading.local())
        owner.outcome_slice.start(0, 0)
        owner.trace_local.sample = sample()
        rpc = FakeRPC()
        proxy = entry.SliceClientRPC(owner, rpc)
        params = (MARKET, "bid", 4, 1, 0)
        self.assertIs(proxy.submitflowmeshorder(*params), rpc.result)
        self.assertEqual(rpc.calls, [("submitflowmeshorder", params, {})])
        self.assertEqual(len(owner.outcome_slice.observations), 1)
        proxy.listflowmeshactions(MARKET)
        self.assertEqual(len(rpc.calls), 2)

    def test_proxy_preserves_original_exception_and_does_not_retry(self):
        owner = types.SimpleNamespace(outcome_slice=logic.OutcomeSlice(), trace_local=threading.local())
        owner.outcome_slice.start(0, 0)
        owner.trace_local.sample = sample()
        rpc = FakeRPC()
        original = rpc.error = OSError("synthetic fixture-only error")
        with self.assertRaises(OSError) as caught:
            entry.SliceClientRPC(owner, rpc).submitflowmeshorder(MARKET, "bid", 4, 1, 0)
        self.assertIs(caught.exception, original)
        self.assertEqual(len(rpc.calls), 1)
        self.assertEqual(owner.outcome_slice.errors, ["target_original_rpc_exception"])

    def test_main_pump_stops_once_and_finally_never_restarts(self):
        with tempfile.TemporaryDirectory(prefix="outcome-slice-control-") as directory:
            owner = self.owner(directory)
            owner.pump_b3()
            owner.end_memory_capture()
            owner._stop_slice("finally")
            self.assertEqual(owner.pumps, 1)
            self.assertEqual([call for call in owner.client.calls if call[0] == "flowmeshtiming"],
                             [("flowmeshtiming", ("stop",), {})])
            self.assertTrue(owner.slice_report["diagnostic_complete"])
            self.assertTrue(Path(directory, "native-outcome-slice-node4.json").exists())

    def test_stop_exception_retained_no_hidden_control_retry(self):
        with tempfile.TemporaryDirectory(prefix="outcome-slice-control-") as directory:
            owner = self.owner(directory)
            owner.client.stop_error = OSError("synthetic stop error")
            owner._stop_slice("finally")
            owner._stop_slice("finally")
            self.assertFalse(owner.slice_report["diagnostic_complete"])
            self.assertEqual(owner.slice_report["capture_control_error"], {"type": "OSError"})
            self.assertEqual(sum(call[0] == "flowmeshtiming" for call in owner.client.calls), 1)

    def test_retained_generation_is_not_cleared_or_claimed(self):
        with tempfile.TemporaryDirectory(prefix="outcome-slice-control-") as directory:
            owner = self.owner(directory)
            owner.slice_capture_owned = False
            with patch.object(owner.client, "flowmeshtiming", return_value={"enabled": False, "generation": 1, "events": []}) as control:
                with self.assertRaisesRegex(AssertionError, "retained generation"):
                    owner._start_slice()
                owner._stop_slice("finally")
            self.assertEqual(control.call_args_list, [unittest.mock.call("read")])
            self.assertFalse(owner.slice_capture_owned)

    def test_ambiguous_start_owned_finally_stopped_once_not_restarted(self):
        with tempfile.TemporaryDirectory(prefix="outcome-slice-control-") as directory:
            owner = self.owner(directory)
            owner.outcome_slice = logic.OutcomeSlice()
            owner.slice_capture_owned = False
            owner.slice_start = None
            def control(command):
                if command == "read":
                    return {"enabled": False, "generation": 0, "events": []}
                if command == "start":
                    raise OSError("synthetic ambiguous start reply")
                return native()
            with patch.object(owner.client, "flowmeshtiming", side_effect=control) as calls:
                with self.assertRaises(OSError):
                    owner._start_slice()
                owner._stop_slice("finally")
                owner._stop_slice("finally")
            self.assertEqual([args.args[0] for args in calls.call_args_list], ["read", "start", "stop"])
            self.assertFalse(owner.slice_report["diagnostic_complete"])
            self.assertIn("capture_start_failed_or_ambiguous", owner.slice_report["observer_errors"])

    def test_capture_control_refuses_non_main_thread(self):
        with tempfile.TemporaryDirectory(prefix="outcome-slice-control-") as directory:
            owner = self.owner(directory)
            owner.slice_main_thread = -1
            with self.assertRaises(AssertionError):
                owner._stop_slice("finally")
            self.assertEqual(owner.client.calls, [])

    def test_cutoff_cleanup_preserves_censored_record_and_incomplete_exit(self):
        with tempfile.TemporaryDirectory(prefix="outcome-slice-control-") as directory:
            owner = self.owner(directory)
            owner.outcome_slice = logic.OutcomeSlice()
            owner.outcome_slice.start(0, 0)
            call(owner.outcome_slice, "submitflowmeshorder", (MARKET, "bid", 1, 1, 0), reply(), 100, 200)
            owner.outcome_slice.begin_call(sample(), "getflowmeshactionstatus", (MARKET, ACTION), {}, 300)
            with patch.object(entry, "host_us", return_value=logic.CUTOFF_US):
                with self.assertRaisesRegex(AssertionError, "slice incomplete"):
                    owner.run_test()
            saved = json.loads(Path(directory, "native-outcome-decision-slice.json").read_text())
            self.assertTrue(saved["all_harness_threads_stopped"])
            self.assertEqual(saved["target_call_censored_at_stop"]["method"], "getflowmeshactionstatus")
            self.assertFalse(saved["diagnostic_complete"])

    def test_native_workload_methods_not_copied_or_overridden(self):
        tree = ast.parse(ENTRY_PATH.read_text())
        klass = next(node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == "FlowMeshOutcomeSliceTest")
        methods = {node.name for node in klass.body if isinstance(node, ast.FunctionDef)}
        self.assertFalse(methods & {"execute_action", "worker_loop", "schedule_window", "setup_wallets", "stop_nodes", "shutdown"})
        self.assertEqual(entry.BASE_SHA256, "f344f81341aa4d4fd568ea491ff7e7f0695ff84b25440fe4baffda5e77412121")
        calls = [node for node in ast.walk(tree) if isinstance(node, ast.Call) and isinstance(node.func, ast.Attribute)]
        self.assertFalse({node.func.attr for node in calls} &
                         {"submitflowmeshorder", "retryflowmeshaction", "getflowmeshactionstatus", "cancelflowmeshorder"})

    def test_invalid_compatibility_flags_refused_before_framework_setup(self):
        values = dict(native_outcome_decision_slice_v1=True, performance_profile="native-queue",
                      performance_require_outcome=True, performance_submit_wait_ms=0,
                      performance_action_timeout=60, performance_memory_trace=False,
                      performance_rpc_trace=False, performance_public_trace=False,
                      performance_diagnostic=False, performance_read_recovery=False,
                      latency_local_reader_hz=0, latency_lock_capture=False)
        for key, bad in (("native_outcome_decision_slice_v1", False), ("performance_memory_trace", True),
                         ("performance_rpc_trace", True), ("performance_public_trace", True),
                         ("performance_action_timeout", 59), ("performance_submit_wait_ms", 1),
                         ("performance_diagnostic", True), ("performance_profile", "ladder")):
            with self.subTest(key=key):
                owner = object.__new__(entry.FlowMeshOutcomeSliceTest)
                owner.options = types.SimpleNamespace(**{**values, key: bad})
                with self.assertRaises(ValueError):
                    owner.set_test_params()


if __name__ == "__main__":
    unittest.main()
