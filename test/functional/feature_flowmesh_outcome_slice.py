#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""PRIVATE opt-in client-only diagnostic; original f344 workload is unchanged.

Requires --native-outcome-decision-slice-v1 --performance-profile=native-queue
--performance-require-outcome. No global memory/RPC/public capture flags. Root
must select fixed ports/seed and approve one experiment after source review.
This is not a performance, durable-outcome, or reliability qualification.
"""

import hashlib
import json
from pathlib import Path
import sys
import threading

from feature_flowmesh_performance import FlowMeshPerformanceTest, host_us
from test_framework.flowmesh_outcome_slice import OutcomeSlice, PROFILE, analyze_capture


BASE_SHA256 = "f344f81341aa4d4fd568ea491ff7e7f0695ff84b25440fe4baffda5e77412121"
CLIENT_SOURCE_REFERENCE = "bff8dac180fae191bf256c41c061b6a82a5ba97b412f7b57501c65e3790f5f1e"


class SliceClientRPC:
    """Forward each original call exactly once; observe only the fixed target.

    No new RPC methods/arguments, error recovery, logging, or body retention.
    Observer errors never replace the original response/error or sign again.
    """
    def __init__(self, owner, rpc):
        self.owner, self.rpc = owner, rpc

    def __getattr__(self, method):
        original = getattr(self.rpc, method)
        if method not in ("submitflowmeshorder", "getflowmeshactionstatus", "retryflowmeshaction"):
            return original
        def observed(*args, **kwargs):
            token = None
            try:
                token = self.owner.outcome_slice.begin_call(
                    getattr(self.owner.trace_local, "sample", None), method, args, kwargs, host_us())
            except Exception:
                self.owner.outcome_slice.error("observer_begin_exception")
            try:
                result = original(*args, **kwargs)
            except Exception:
                try:
                    self.owner.outcome_slice.end_call(token, None, host_us(), failed=True)
                except Exception:
                    self.owner.outcome_slice.error("observer_error_callback_exception")
                raise
            try:
                self.owner.outcome_slice.end_call(token, result, host_us())
            except Exception:
                self.owner.outcome_slice.error("observer_return_exception")
            return result
        return observed


class FlowMeshOutcomeSliceTest(FlowMeshPerformanceTest):
    def add_options(self, parser):
        super().add_options(parser)
        parser.add_argument("--native-outcome-decision-slice-v1", action="store_true",
                            help="Explicit client-only two-observation diagnostic; no workload/poll changes")

    def set_test_params(self):
        options = self.options
        if (not options.native_outcome_decision_slice_v1 or options.performance_profile != "native-queue" or
                not options.performance_require_outcome or options.performance_submit_wait_ms != 0 or
                options.performance_action_timeout != 60 or options.performance_memory_trace or
                options.performance_rpc_trace or options.performance_public_trace or options.performance_diagnostic or
                options.performance_read_recovery or options.latency_local_reader_hz or options.latency_lock_capture):
            raise ValueError("slice requires explicit native-queue/outcome/zero-wait/60s, no other capture/read-recovery profile")
        base = Path(__file__).with_name("feature_flowmesh_performance.py")
        if hashlib.sha256(base.read_bytes()).hexdigest() != BASE_SHA256:
            raise ValueError("original performance harness is not frozen f344")
        super().set_test_params()
        self.outcome_slice = OutcomeSlice()
        self.slice_main_thread = threading.get_ident()
        self.slice_start = None
        self.slice_capture_owned = False
        self.slice_stop_attempted = False
        self.slice_report = {"profile": PROFILE, "base_harness_sha256": BASE_SHA256,
                             "client_source_review_reference_not_runtime_binary_hash": CLIENT_SOURCE_REFERENCE,
                             "diagnostic_complete": False}

    def independent_rpc(self, node, wallet=None):
        rpc = super().independent_rpc(node, wallet)
        return SliceClientRPC(self, rpc) if node is self.client and wallet is not None else rpc

    def _main_thread(self):
        if threading.get_ident() != self.slice_main_thread:
            raise AssertionError("capture control must run on original main-thread pump/finally")

    def _bench_off(self):
        enabled = sorted(name for name, enabled in self.client.logging().items() if enabled)
        if enabled:
            raise AssertionError("client debug categories must remain disabled for the whole diagnostic profile")
        return {"enabled_debug_categories": enabled, "bench_enabled": False}

    def _start_slice(self):
        self._main_thread()
        if self.slice_capture_owned or self.slice_stop_attempted:
            raise AssertionError("slice may start exactly once")
        self.slice_report["logging_before_start"] = self._bench_off()
        # This fresh generated fixture is the sole capture controller. Refuse
        # an already-active capture before claiming any ownership.
        prior = self.client.flowmeshtiming("read")
        if prior.get("enabled") is not False or prior.get("generation") != 0 or prior.get("events") != []:
            raise AssertionError("preexisting capture or retained generation is not owned by this diagnostic")
        before = host_us()
        self.outcome_slice.start(before, before)
        # Own even an ambiguous start reply: finally makes one stop attempt,
        # retains the failure, and never starts/restarts another generation.
        self.slice_capture_owned = True
        try:
            result = self.client.flowmeshtiming("start")
            after = host_us()
            self.outcome_slice.start_bounds = [before, after]
            self.slice_start = result
            if result.get("enabled") is not True or result.get("dropped") != 0 or result.get("events") != []:
                raise AssertionError("unexpected capture start metadata")
        except Exception:
            self.outcome_slice.error("capture_start_failed_or_ambiguous")
            raise

    def _stop_slice(self, reason):
        self._main_thread()
        if not self.slice_capture_owned or self.slice_stop_attempted:
            return
        self.slice_stop_attempted = True
        state = self.outcome_slice.freeze(reason, host_us())
        before = host_us()
        try:
            capture = self.client.flowmeshtiming("stop")
            after = host_us()
            # File I/O and JSON projection are main-thread-only, after stop.
            Path(self.options.tmpdir, "native-outcome-slice-node4.json").write_text(
                json.dumps(capture, indent=2) + "\n", encoding="utf-8")
            self.slice_report.update(analyze_capture(state, capture, [before, after]))
            if self.slice_start is None or capture.get("generation") != self.slice_start.get("generation"):
                self.slice_report["diagnostic_complete"] = False
                self.slice_report.setdefault("observer_errors", []).append("capture_generation_not_proven")
        except Exception as error:
            self.slice_report.update(state, diagnostic_complete=False,
                                     capture_control_error={"type": type(error).__name__})
        finally:
            self.slice_capture_owned = False
            try:
                self.slice_report["logging_after_stop"] = self._bench_off()
            except Exception as error:
                self.slice_report["diagnostic_complete"] = False
                self.slice_report["logging_control_error"] = {"type": type(error).__name__}

    def run_window(self, label, **kwargs):
        if label == "burst_8":
            self._start_slice()
        return super().run_window(label, **kwargs)

    def pump_b3(self):
        super().pump_b3()
        if self.slice_capture_owned:
            reason = self.outcome_slice.stop_reason(host_us())
            if reason:
                self._stop_slice(reason)

    def end_memory_capture(self):
        # The global flag remains OFF. This owns only the private node4 slice.
        self._stop_slice(self.outcome_slice.stop_reason(host_us()) or "finally")

    def verify_production_logging(self, market_id, phase):
        # Explicit compatible profile: client capture bytes are expected, not
        # evidence of disk logging. Uncaptured validators still require zero.
        observations = {}
        for node in [*self.nodes, self.client]:
            enabled = sorted(name for name, enabled in node.logging().items() if enabled)
            assert not enabled, "diagnostic profile enabled debug logging"
            row = {"enabled_debug_categories": enabled, "bench_enabled": False}
            if node is not self.client:
                markets = node.getflowmeshdeliveryinfo(market_id)["markets"]
                assert len(markets) == 1 and markets[0]["market_id"] == market_id
                row.update(runtime_trace_bytes=markets[0]["trace_bytes"],
                           runtime_trace_events_dropped=markets[0]["trace_events_dropped"])
                assert row["runtime_trace_bytes"] == 0 and row["runtime_trace_events_dropped"] == 0
            observations[str(node.index)] = row
        self.performance_report["logging_checks"].append(
            {"phase": phase, "client_only_slice_bytes_permitted": True, "processes": observations})

    def finish_report(self, primary_exception_in_flight, filename, log_label):
        self._stop_slice(self.outcome_slice.stop_reason(host_us()) or "finally")
        self.performance_report["native_outcome_decision_slice"] = self.slice_report
        return super().finish_report(primary_exception_in_flight, filename, log_label)

    def run_test(self):
        try:
            super().run_test()
        finally:
            primary = sys.exc_info()[0] is not None
            self._stop_slice("finally")
            self.slice_report["all_harness_threads_stopped"] = self.performance_report.get("all_harness_threads_stopped", False)
            self.slice_report["setup_restart_exit_artifact"] = "native-child-exits.jsonl"
            self.slice_report["native_child_exits_jsonl_scope"] = "setup restarts plus final client only; not final four validators"
            self.slice_report["workload_correctness_pass"] = self.performance_report.get("correctness_pass", False)
            self.slice_report["child_exit_collection"] = (
                "inherited Preagreement final-shutdown stdout PREAGREEMENT_TEST_CHILD_EXIT index/pid/status for all5 indexes0..4 required; "
                "missing/nonzero final cleanup evidence disqualifies; future actual exits unclaimed")
            try:
                Path(self.options.tmpdir, "native-outcome-decision-slice.json").write_text(
                    json.dumps(self.slice_report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
            except Exception:
                if not primary:
                    raise
            if not primary and (not self.slice_report.get("diagnostic_complete") or
                                not self.slice_report["all_harness_threads_stopped"]):
                raise AssertionError("Outcome slice incomplete; preserve the original workload and partial diagnostic")


if __name__ == "__main__":
    FlowMeshOutcomeSliceTest(__file__).main()
