#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Runtime-free public trace/account-read regressions; no daemon or TLS run.

Run: python3 -B -m unittest -v flowmesh_public_trace_test
These tests qualify recording, joining and bounded retry logic only. Mocked
responses do not reproduce the server's snapshot assembly race, verify real
certificates, or establish real HTTP request-limit recovery/performance.
"""

import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import threading
import unittest
from unittest.mock import Mock, patch

import feature_flowmesh_performance as performance
from test_framework.authproxy import JSONRPCException
from test_framework.flowmesh_client_tls import FlowMeshTLSFaultRelay
from test_framework.flowmesh_public_trace import (
    PublicBodyCapture, attribute_requests, public_request_context, public_response_context, reply_rejected,
)


class PublicContextTests(unittest.TestCase):
    def test_snapshot_identity_history_range_and_cursor(self):
        value = {"ok": True, "result": {
            "status": {"market_id": "m", "execution_config_id": "c", "domain": "d"},
            "cursor": {"instance_id": "instance", "event_id": 7},
            "certified_payload": "aabb", "state_bytes": "ccdd",
            "reported_data": {"market_id": "m", "execution_config_id": "c",
                "snapshot": {"next_microblock_sequence": 4, "state_root": "r"},
                "account": {"account_id": "a", "next_sequence": 2},
                "history": {"available": True, "entries": [
                    {"sequence": 3, "microblock_hash": "h3", "state_root": "r3"},
                    {"sequence": 2, "microblock_hash": "h2", "state_root": "r2"}]}}}}
        result = public_response_context(json.dumps(value).encode())
        self.assertEqual(result["cursor"]["instance_id"], "instance")
        self.assertEqual(result["status"]["execution_config_id"], "c")
        history = result["reported_data"]["history"]
        self.assertEqual(history["sequence_range"], [2, 3])
        self.assertTrue(history["strictly_descending"])
        self.assertEqual(history["heads"][0]["microblock_hash"], "h3")
        self.assertEqual(result["certified_payload"]["sha256"], hashlib.sha256(bytes.fromhex("aabb")).hexdigest())

    def test_request_scope_and_signed_action_prefix(self):
        account = bytes(range(32))
        payload = account + (12).to_bytes(8, "little") + b"\x03"
        result = public_request_context({"method": "submit", "params": {
            "market_id": "m", "action_id": "id", "action_hex": payload.hex()}})
        self.assertEqual(result["account_id"], account[::-1].hex())
        self.assertEqual(result["account_sequence"], 12)
        result = public_request_context({"method": "updates", "params": {
            "market_id": "m", "account_id": "a", "known_head": "h",
            "cursor": {"instance_id": "i", "event_id": 9}, "before_sequence": 8}})
        self.assertEqual(result["known_head"], "h")
        self.assertEqual(result["cursor"]["event_id"], 9)

    def test_rejected_http_and_successful_http_runtime_errors_are_distinct(self):
        context = public_response_context(b'{"ok":false,"error":"Public trading API request budget exceeded"}')
        self.assertIn("budget", context["error"])
        self.assertTrue(reply_rejected({"upstream_http_status": 429, "upstream_context": context}))
        self.assertTrue(reply_rejected({"upstream_http_status": 200, "upstream_context": {
            "receipt_state": "rejected", "reason": "paused"}}))
        self.assertFalse(reply_rejected({"upstream_http_status": 200, "upstream_context": {
            "receipt_state": "admitted"}}))

    def test_bad_json_and_bounded_history_projection_are_explicit(self):
        self.assertFalse(public_response_context(b"not JSON")["json_valid"])
        result = public_response_context(json.dumps({"result": {"history": {"entries": [
            {"sequence": sequence} for sequence in range(300)]}}}).encode())
        history = result["reported_data"]["history"]
        self.assertTrue(history["context_truncated"])
        self.assertEqual(history["entry_count"], 300)
        self.assertEqual(len(history["heads"]), 256)
        self.assertFalse(history["strictly_descending"])


class CaptureTests(unittest.TestCase):
    def test_full_bytes_and_budget_failure(self):
        with TemporaryDirectory() as temporary:
            capture = PublicBodyCapture(Path(temporary, "capture"), max_bytes=6, max_files=2)
            first = capture.capture(1, "request", b"123456")
            self.assertEqual(Path(first["path"]).read_bytes(), b"123456")
            failure = capture.capture(1, "upstream", b"7")
            self.assertFalse(failure["complete"])
            self.assertEqual(failure["sha256"], hashlib.sha256(b"7").hexdigest())
            self.assertFalse(capture.snapshot()["complete_capture"])
            self.assertEqual(capture.snapshot()["failure_count"], 1)

    def test_duplicate_write_and_unrecordable_request_are_explicit(self):
        with TemporaryDirectory() as temporary:
            capture = PublicBodyCapture(Path(temporary, "capture"))
            capture.capture(1, "request", b"first")
            duplicate = capture.capture(1, "request", b"second")
            self.assertEqual(duplicate["error"], "FileExistsError")
            capture.note_failure("incomplete request body", stage="request")
            self.assertEqual(capture.snapshot()["failure_count"], 2)

    def test_identical_delivered_reply_reuses_full_upstream_artifact(self):
        with TemporaryDirectory() as temporary:
            relay = object.__new__(FlowMeshTLSFaultRelay)
            relay.lock = threading.Lock()
            relay.public_capture = PublicBodyCapture(Path(temporary, "capture"))
            row = {"request_id": 5}
            body = b'{"ok":false,"error":"budget"}'
            relay.record_public_response(row, "upstream", 429, body)
            relay.record_public_response(row, "delivered", 429, body)
            self.assertTrue(row["delivered_body"]["identical_to_upstream"])
            self.assertEqual(row["upstream_body"]["path"], row["delivered_body"]["path"])
            self.assertEqual(row["client_http_status"], 429)
            self.assertEqual(relay.public_capture.snapshot()["files_reserved"], 1)


class AttributionTests(unittest.TestCase):
    def setUp(self):
        self.samples = [
            {"sample_id": 1, "market_id": "m1", "action_id": "same", "account_id": "a1"},
            {"sample_id": 2, "market_id": "m2", "action_id": "same", "account_id": "a2"}]
        self.calls = [{"sample_id": index, "start_host_us": 10, "end_host_us": 30} for index in (1, 2)]

    def test_scoped_action_exact_join_never_merges_market_credentials(self):
        rows = [{"method": "action", "host_monotonic_us": 20,
                 "request_context": {"market_id": "m2", "action_id": "same"}, "upstream_http_status": 429}]
        result = attribute_requests(rows, self.samples, self.calls)
        self.assertEqual(rows[0]["attributed_sample_id"], 2)
        self.assertEqual(result["per_sample"]["2"]["http_rejections"], {"action": 1})
        self.assertEqual(result["per_sample"]["1"]["total"], 0)

    def test_account_read_interval_and_shared_reads(self):
        rows = [{"method": "snapshot", "host_monotonic_us": 20,
                 "request_context": {"market_id": "m1", "account_id": "a1"}},
                {"method": "markets", "host_monotonic_us": 20, "request_context": {}}]
        result = attribute_requests(rows, self.samples, self.calls)
        self.assertEqual(rows[0]["attribution"], "unique_matching_active_rpc_interval")
        self.assertEqual(rows[1]["attribution"], "shared_or_ambiguous")
        self.assertNotIn("attributed_sample_id", rows[1])
        self.assertEqual(result["shared_or_unattributed"], {"markets": 1})

    def test_explicit_unknown_action_does_not_fallback_to_incidental_interval(self):
        rows = [{"method": "action", "host_monotonic_us": 20,
                 "request_context": {"market_id": "m1", "action_id": "other"}}]
        result = attribute_requests(rows, self.samples, self.calls)
        self.assertEqual(result["shared_or_unattributed"], {"action": 1})


class RpcObservationTests(unittest.TestCase):
    def subject(self):
        local = threading.local()
        local.sample = {"sample_id": 3, "market_id": "m", "account_id": "a"}
        return SimpleNamespace(trace_local=local, report_lock=threading.Lock(), rpc_calls=[],
                               rpc_trace_dropped=0, rpc_trace_bytes=0, drain_relay_observations=Mock())

    def test_rpc_end_precedes_capture_work(self):
        owner = self.subject()
        rpc = Mock()
        rpc.getflowmeshactionstatus.return_value = {"receipt_state": "certified_inclusion"}
        owner.drain_relay_observations.side_effect = lambda: performance.host_us()
        with patch.object(performance, "host_us", side_effect=[100, 200, 900]):
            response = performance.ObservedClientRPC(owner, rpc, "generated").getflowmeshactionstatus("m", "a")
        self.assertEqual(response["receipt_state"], "certified_inclusion")
        self.assertEqual(owner.rpc_calls[0]["end_host_us"], 200)
        self.assertEqual(owner.trace_local.last_rpc_return_us, 200)

    def test_capture_failure_never_masks_rpc_failure_or_changes_success(self):
        for fail_rpc in (False, True):
            with self.subTest(fail_rpc=fail_rpc):
                owner, rpc = self.subject(), Mock()
                original = JSONRPCException({"code": -1, "message": "public error"})
                rpc.submitflowmeshorder.side_effect = original if fail_rpc else None
                rpc.submitflowmeshorder.return_value = {"action_id": "saved"}
                owner.drain_relay_observations.side_effect = AssertionError("capture failed")
                wrapped = performance.ObservedClientRPC(owner, rpc, "generated")
                if fail_rpc:
                    with self.assertRaises(JSONRPCException) as raised:
                        wrapped.submitflowmeshorder()
                    self.assertIs(raised.exception, original)
                else:
                    self.assertEqual(wrapped.submitflowmeshorder(), {"action_id": "saved"})
                self.assertIn("capture_error", owner.rpc_calls[0])
                self.assertEqual(rpc.submitflowmeshorder.call_count, 1)


class ReadRecoveryTests(unittest.TestCase):
    def subject(self, outcomes, enabled=True):
        return SimpleNamespace(options=SimpleNamespace(performance_read_recovery=enabled),
            authenticated_account=Mock(side_effect=outcomes), stop_requested=Mock(),
            worker_stop=SimpleNamespace(wait=Mock(return_value=False)), observed_rpc_return_us=Mock(return_value=200))

    def read(self, subject, sample):
        with patch.object(performance.time, "monotonic", return_value=0):
            return performance.FlowMeshPerformanceTest.followup_account_read(subject, object(), "m", sample, 60)

    @staticmethod
    def error():
        return JSONRPCException({"code": -1, "message": "Reported history is stale/out of order"})

    def test_recovered_read_preserves_first_failure_and_original_certificate_clock(self):
        subject = self.subject([self.error(), {"account": {"next_sequence": 4}}])
        sample = {"initial_submission_host_us": 10, "client_certified_host_us": 100}
        self.assertEqual(self.read(subject, sample)["account"]["next_sequence"], 4)
        self.assertFalse(sample["read_consistency_pass"])
        self.assertTrue(sample["account_read_rpc_recovered"])
        # Expected balance/sequence checks in execute_action, not a successful
        # read alone, are the authority for account_read_recovered.
        self.assertNotIn("account_read_recovered", sample)
        self.assertEqual(sample["initial_submission_host_us"], 10)
        self.assertEqual(sample["client_certified_host_us"], 100)
        self.assertIs(sample["first_balance_read_error"], sample["read_consistency_failures"][0])
        subject.worker_stop.wait.assert_called_once_with(.5)

    def test_default_failfast_never_retries(self):
        subject = self.subject([self.error(), {}], enabled=False)
        sample = {}
        with self.assertRaises(JSONRPCException):
            self.read(subject, sample)
        self.assertEqual(subject.authenticated_account.call_count, 1)
        subject.worker_stop.wait.assert_not_called()
        self.assertFalse(sample["read_consistency_pass"])

    def test_invalid_proof_assertion_is_never_retried(self):
        subject = self.subject([AssertionError("certificate invalid"), {}])
        with self.assertRaises(AssertionError):
            self.read(subject, {})
        subject.worker_stop.wait.assert_not_called()
        self.assertEqual(subject.authenticated_account.call_count, 1)

    def test_retry_budget_is_per_action_not_per_read_call(self):
        subject = self.subject([self.error(), {}, self.error(), {}, self.error(), {}, self.error(), {}, self.error()])
        sample = {}
        for _ in range(4):
            self.read(subject, sample)
        with self.assertRaises(JSONRPCException):
            self.read(subject, sample)
        self.assertEqual([call.args[0] for call in subject.worker_stop.wait.call_args_list], [.5, 1, 2, 4])
        self.assertEqual(len(sample["read_consistency_failures"]), 5)

    def test_certification_and_completion_do_not_erase_read_failure(self):
        sample = {"status": "complete", "kind": "fill_bid", "client_certified_host_us": 100,
            "client_certified_ms": 100, "client_certified_from_offer_ms": 110,
            "account_read_recovered": True, "read_consistency_pass": False,
            "read_consistency_failures": [{"error": "first"}]}
        summary = performance.summarize_window([sample], [], 20, 21)
        self.assertEqual(summary["certified_inclusion_count"], 1)
        self.assertTrue(summary["workload_completion_pass"])
        self.assertFalse(summary["correctness_pass"])
        self.assertFalse(summary["performance_pass"])
        self.assertEqual(summary["read_recovered_action_count"], 1)


if __name__ == "__main__":
    unittest.main()
