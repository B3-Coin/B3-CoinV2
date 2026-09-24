#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Offline parser/coherence tests, not a server-race or certificate test."""

import hashlib
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from flowmesh_public_consistency_analyze import (
    Reader, analyze, correlated_rejections, decode_entry_prefix, inspect_context, response_context,
)


# Public, zero-action production entry from retained matched-baseline-01,
# endpoint0 request1. Certificate/state bytes deliberately not embedded.
# Expected identity independently observed in that server's status/history.
ENTRY = (
    "000001b00100011a3405424a805d6b95600c5b7598af38b1f8f3ba25bdb2636500c76ad3f43879"
    "61733818e183c66d5c53d6336e1e0aed819792cd9de16cbc5633f7c89fe5582a0000000000000000"
    "9b5aa3942e1ec41f902328735192dc99028b2333509ef60d9019dcdff684c5a70000000000000000"
    "000000000000000000000000000000000000000000000000000000000000000085000000"
    "4454a1fac89fe713bb4efaf46fec95ef1312d3c676d6609f413a9d27b3a73b7d"
    "9bec1d5f24b7b1beddb43aaa55803eb694b6f35f50b3497218b473035da3b44500"
    "5aef782c2890f4cd9208080da84b633b43f801e03c355e4ba8fb3425ab390877c"
    "f57e6f73c33254c075cd9c72386403ab2681b75cc707241b25f2863036109dee"
    "cb083870219eb6f8187f5568c0207d7faf650adbacfc5fb9e45905d5607fefb"
    "000000000000000000000000cd887bad6fad7506c7fa0e156ab78329a430605d3d2588b07d717c46bc881f65"
    "0000000000000000ffffffff0000000000000000000000000000000000000000000000000000000000000000"
    "0000000000000000000000000000000000000000000000000000000000000000")
EXPECTED_HASH = "57f7c30b88517d5fbb81ce47dd6e27e568fabf69f7d4f89a015148011122bdb7"


def snapshot():
    entry = decode_entry_prefix(ENTRY)
    identity = {"market_id": entry["market_id"], "domain": entry["domain"], "execution_config_id": "config"}
    head = {"next_microblock_sequence": 1, "last_microblock_hash": EXPECTED_HASH, "state_root": entry["state_root"]}
    request = {"method": "snapshot", "params": {"market_id": entry["market_id"], "account_id": "account"}}
    response = {"ok": True, "result": {"status": {**identity, **head}, "certified_payload": ENTRY,
        "cursor": {"instance_id": "instance", "event_id": 1}, "reported_data": {
            **identity, "snapshot": dict(head), "account": {"account_id": "account", "next_sequence": 1},
            "history": {"entries": [{"sequence": 0, "microblock_hash": EXPECTED_HASH}]}}}}
    return request, response


class PrefixTests(unittest.TestCase):
    def test_real_public_identity_vector_matches_reported_head(self):
        entry = decode_entry_prefix(ENTRY)
        self.assertEqual(entry["entry_hash"], EXPECTED_HASH)
        self.assertEqual(entry["sequence"], 0)
        self.assertEqual(entry["entry_bytes"], 432)
        self.assertEqual(entry["anchor_height"], 133)
        self.assertFalse(entry["certificate_authenticated_by_analyzer"])
        self.assertEqual(entry["certificate_suffix_bytes"], 0)

    def test_truncated_unknown_version_and_noncanonical_count_are_rejected(self):
        raw = bytearray.fromhex(ENTRY)
        bad_version = raw.copy()
        bad_version[4] = 2
        bad_count = raw.copy()
        bad_count[4 + 215] = 253
        for value in ("", ENTRY[:-2], bad_version.hex(), bad_count.hex()):
            with self.subTest(value=value[:12]), self.assertRaises(ValueError):
                decode_entry_prefix(value)


class ContextTests(unittest.TestCase):
    def analyze_response(self, request, response, known=None):
        return inspect_context(response_context(request, response), known or {})

    def test_consistent_snapshot_has_no_finding(self):
        context = self.analyze_response(*snapshot())
        self.assertEqual(context["findings"], [])
        self.assertEqual(context["history"]["sequence_range"], [0, 0])

    def test_snapshot_history_ahead_of_certified_prefix_is_exact_finding(self):
        request, response = snapshot()
        response["result"]["reported_data"]["history"]["entries"].insert(0, {"sequence": 1, "microblock_hash": "new"})
        context = self.analyze_response(request, response)
        findings = {row["code"] for row in context["findings"]}
        self.assertIn("snapshot_history_at_or_ahead_of_included_entry", findings)
        self.assertIn("history_at_or_ahead_of_reported_head", findings)

    def test_identity_account_root_and_order_mismatches_remain_distinct(self):
        request, response = snapshot()
        reported = response["result"]["reported_data"]
        reported["execution_config_id"] = "wrong"
        reported["account"]["account_id"] = "wrong"
        reported["snapshot"]["state_root"] = "wrong"
        reported["history"]["entries"] *= 2
        findings = {row["code"] for row in self.analyze_response(request, response)["findings"]}
        self.assertEqual(findings, {"response_identity_mismatch", "reported_account_mismatch",
                                   "snapshot_projection_differs_from_entry", "history_not_strictly_descending"})

    def test_equal_height_history_hash_is_bound_to_included_entry(self):
        request, response = snapshot()
        response["result"]["reported_data"]["history"]["entries"][0]["microblock_hash"] = "wrong"
        findings = self.analyze_response(request, response)["findings"]
        self.assertEqual([row["code"] for row in findings], ["snapshot_equal_height_history_commitment_conflict"])

    def test_updates_newer_head_requires_refresh_not_automatic_stale_finding(self):
        request, response = snapshot()
        entry = decode_entry_prefix(ENTRY)
        request["method"] = "updates"
        request["params"].update(known_head=EXPECTED_HASH, cursor={"instance_id": "instance", "event_id": 0})
        result = response["result"]
        result.pop("certified_payload")
        result.update(gap=False, more=False)
        result["status"].update(next_microblock_sequence=2, last_microblock_hash="new")
        result["reported_data"]["snapshot"].update(next_microblock_sequence=2, last_microblock_hash="new")
        result["reported_data"]["history"]["entries"].insert(0, {"sequence": 1, "microblock_hash": "new"})
        context = self.analyze_response(request, response, {(entry["market_id"], EXPECTED_HASH): entry})
        self.assertEqual(context["findings"], [])
        self.assertTrue(context["updates_refresh_required_observation"])

    def test_cursor_instance_requires_gap_and_event_position_cannot_regress(self):
        request, response = snapshot()
        request["method"] = "updates"
        request["params"]["cursor"] = {"instance_id": "old", "event_id": 2}
        response["result"]["gap"] = False
        self.assertIn("cursor_instance_changed_without_gap", {row["code"] for row in self.analyze_response(request, response)["findings"]})
        request["params"]["cursor"]["instance_id"] = "instance"
        self.assertIn("cursor_event_id_regressed", {row["code"] for row in self.analyze_response(request, response)["findings"]})

    def test_rpc_rejection_join_is_interval_and_scope_only_not_causality(self):
        context = response_context(*snapshot())
        market = context["request"]["market_id"]
        calls = [{"start_host_us": 10, "end_host_us": 30, "market_id": market, "account_id": "account",
                  "error": {"rpc_message": "Reported history is stale/out of order"}},
                 {"start_host_us": 10, "end_host_us": 30, "market_id": "other", "error": {}},
                 {"start_host_us": 31, "end_host_us": 40, "market_id": market, "error": {}}]
        result = correlated_rejections({"host_monotonic_us": 20}, context, calls)
        self.assertEqual([row["rpc_index"] for row in result], [0])
        self.assertIn("not causal proof", result[0]["relationship"])


class FileTests(unittest.TestCase):
    def fixture(self, root, response=True):
        directory = root / "public-http-trace" / "endpoint0"
        directory.mkdir(parents=True)
        request, reply = snapshot()
        request_path = directory / "00000001-request.json"
        request_path.write_text(json.dumps(request))
        if response:
            (directory / "00000001-upstream.json").write_text(json.dumps(reply))
        return request_path

    def test_read_only_raw_scan_no_report_no_failure_reproduction_claim(self):
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.fixture(root)
            before = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in root.rglob("*.json")}
            result = analyze(root)
            after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in root.rglob("*.json")}
            self.assertEqual(before, after)
            self.assertTrue(result["analysis_complete"])
            self.assertFalse(result["report_present"])
            self.assertIsNone(result["capture_complete_as_reported"])
            self.assertEqual(result["finding_counts"], {})
            self.assertIn("not inferred", result["old_failure_reproduced"])
            self.assertFalse(result["rpc_correlation_metadata_complete"])
            self.assertEqual(result["requests_without_report_metadata_count"], 1)

    def test_missing_response_and_symlink_are_explicit_incomplete_inputs(self):
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = self.fixture(root, response=False)
            result = analyze(root)
            self.assertFalse(result["analysis_complete"])
            self.assertEqual(result["analysis_failure_count"], 1)
            linked = root / "linked.json"
            linked.symlink_to(path)
            with self.assertRaises(ValueError):
                Reader().read_json(linked)

    def test_manifest_hash_mismatch_is_retained(self):
        with TemporaryDirectory() as temporary:
            root = Path(temporary)
            path = self.fixture(root)
            report = {"http_requests": [{"request_id": 1, "request_body": {
                "path": str(path), "bytes": 0, "sha256": "wrong"}}]}
            (root / "flowmesh-performance.json").write_text(json.dumps(report))
            result = analyze(root)
            self.assertFalse(result["analysis_complete"])
            self.assertIn("size/hash", result["analysis_errors"][0]["error"])

    def test_file_size_bound_is_enforced_before_parsing(self):
        with TemporaryDirectory() as temporary:
            path = Path(temporary, "body.json")
            path.write_bytes(b"{}")
            with self.assertRaises(ValueError):
                Reader().read_json(path, limit=1)


if __name__ == "__main__":
    unittest.main()
