#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Bounded real-P2P four-validator workload, not an engine-only benchmark.

Reuses the full deposit/withdrawal qualification flow. Extra actions must
certify with no new B3 block. Latencies are observations on this local test
machine, not mainnet guarantees. No timing threshold weakens quorum tests.
"""

import json
import math
import time
import unittest
from unittest.mock import Mock

from feature_flowmesh_release import FlowMeshReleaseTest, B3_DEPOSIT, TRADE_PRICE, TRADE_QUANTITY
from test_framework.util import assert_equal


def collect_stall_diagnostic(node, index, market_id, sequence):
    """Best-effort independent reads; never lose other fields to a missing account."""
    diagnostic = {"node": index, "market_id": market_id,
                  "expected_account_sequence": sequence, "errors": {}}

    def capture(field, read):
        try:
            diagnostic[field] = read()
        except Exception as error:
            # Do not echo exception text, which may include an RPC URL or other
            # transport details. The exception type and RPC code are sufficient.
            failure = {"type": type(error).__name__}
            rpc_error = getattr(error, "error", None)
            if isinstance(rpc_error, dict) and type(rpc_error.get("code")) is int:
                failure["code"] = rpc_error["code"]
            diagnostic["errors"][field] = failure

    def validator_status():
        value = node.getflowmeshvalidatorinfo()
        return {key: value[key] for key in (
            "scope", "service_available", "service_enabled", "service_running",
            "armed", "armed_keys_fingerprint", "armed_key_count", "wallet_key_count",
            "wallet_armed_key_count", "wallet_all_keys_armed", "armed_is_signing_proof",
        ) if key in value}

    def peer_counters():
        peers = node.getpeerinfo()
        kinds = ("fmhello", "fmaction", "fmprop", "fmattest", "fmcert", "fmget", "fmentries")
        counters = lambda peer, field: {key: peer.get(field, {})[key]
                                        for key in kinds if key in peer.get(field, {})}
        diagnostic["peer_count"] = len(peers)
        diagnostic["peers_truncated"] = len(peers) > 32
        return [{"id": peer["id"], "received": counters(peer, "bytesrecv_per_msg"),
                 "sent": counters(peer, "bytessent_per_msg")} for peer in peers[:32]]

    capture("balance", lambda: node.getflowmeshbalance(market_id))
    capture("validator", validator_status)
    # The second argument is the RPC options object, not a positional count.
    # One history entry/curve retains the full runtime diagnostic snapshot.
    capture("market_data", lambda: node.getflowmeshmarketdata(market_id, {"limit": 1, "curve_limit": 1}))
    capture("peers", peer_counters)
    return diagnostic


class FlowMeshSpeedTest(FlowMeshReleaseTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def wait_for_account_sequence(self, buyer, market_id, sequence):
        try:
            self.wait_until(lambda: buyer.getflowmeshbalance(market_id)["account"]["next_sequence"] == sequence,
                            timeout=60, check_interval=0.05)
        except AssertionError:
            # Preserve transient candidate/evidence/vote observations before
            # test cleanup. A restart retains locks, but not these counters.
            for index, node in enumerate(self.nodes):
                diagnostic = collect_stall_diagnostic(node, index, market_id, sequence)
                self.log.error("FLOWMESH_STALL_DIAGNOSTIC %s", json.dumps(diagnostic, sort_keys=True, default=str))
            raise

    def exercise_extra_trading(self, market_id):
        buyer = self.nodes[1]
        height = buyer.getblockcount()
        first = buyer.getflowmeshbalance(market_id)
        sequence = first["account"]["next_sequence"]
        latencies = []
        rpc_times = []
        self.log.info("40 sequential real-P2P order/cancel actions without B3 block production")
        for index in range(40):
            started = time.monotonic()
            if index % 2 == 0:
                response = buyer.submitflowmeshorder(market_id, "bid", TRADE_PRICE // 2, TRADE_QUANTITY, sequence)
            else:
                response = buyer.cancelflowmeshorder(market_id, "bid", sequence)
            rpc_times.append((time.monotonic() - started) * 1000)
            assert_equal(response["accepted"], True)
            sequence += 1
            self.wait_for_account_sequence(buyer, market_id, sequence)
            latencies.append((time.monotonic() - started) * 1000)
            account = buyer.getflowmeshbalance(market_id)["account"]
            if index % 2:
                assert_equal(account["b3_reserved"], 0)
                assert_equal(account["b3_available"], B3_DEPOSIT)
        self.wait_for_market_convergence(market_id)
        assert_equal(buyer.getblockcount(), height)

        self.log.info("64 explicitly sequenced actions in one bounded burst, then verify every account sequence")
        started = time.monotonic()
        burst_start = sequence
        for index in range(64):
            if index % 2 == 0:
                reply = buyer.submitflowmeshorder(market_id, "bid", TRADE_PRICE // 2, TRADE_QUANTITY, sequence)
            else:
                reply = buyer.cancelflowmeshorder(market_id, "bid", sequence)
            assert_equal(reply["accepted"], True)
            assert_equal(reply["sequence"], sequence)
            sequence += 1
        submitted_ms = (time.monotonic() - started) * 1000
        self.wait_for_account_sequence(buyer, market_id, sequence)
        certified_ms = (time.monotonic() - started) * 1000
        self.wait_for_market_convergence(market_id)
        account = buyer.getflowmeshbalance(market_id)["account"]
        assert_equal(account["b3_available"], B3_DEPOSIT)
        assert_equal(account["b3_reserved"], 0)
        assert_equal(account["base_available"], 0)
        assert_equal(sequence - burst_start, 64)
        assert_equal(buyer.getblockcount(), height)

        ordered = sorted(latencies)
        percentile = lambda p: ordered[max(0, math.ceil(len(ordered) * p) - 1)]
        report = {"network": "four-node local regtest P2P", "sequential_actions": 40,
                  "confirmation_observation_interval_ms": 50,
                  "rpc_mean_ms": sum(rpc_times) / len(rpc_times),
                  "certified_p50_ms": percentile(.5), "certified_p95_ms": percentile(.95),
                  "certified_p99_ms": percentile(.99), "burst_actions": 64,
                  "burst_rpc_submitted_ms": submitted_ms, "burst_all_certified_ms": certified_ms,
                  "new_B3_blocks": 0}
        self.log.info("FLOWMESH_SPEED_RESULT %s", json.dumps(report, sort_keys=True))


class StallDiagnosticTests(unittest.TestCase):
    """Offline coverage: python3 -B -m unittest feature_flowmesh_speed.StallDiagnosticTests"""

    def node(self):
        node = Mock()
        node.getflowmeshbalance.return_value = {"account": {"next_sequence": 7}}
        node.getflowmeshvalidatorinfo.return_value = {"service_running": True, "armed_key_count": 1}
        node.getflowmeshmarketdata.return_value = {"snapshot": {"runtime": {"proposals_missing_evidence": 3}}}
        node.getpeerinfo.return_value = [{"id": 8, "bytesrecv_per_msg": {"fmaction": 42, "ping": 99},
                                        "bytessent_per_msg": {"fmprop": 100}, "addr": "not logged"}]
        return node

    def test_missing_account_preserves_other_rpc_results(self):
        node = self.node()
        failure = RuntimeError("private transport detail")
        failure.error = {"code": -4, "message": "No FlowMesh account"}
        node.getflowmeshbalance.side_effect = failure
        report = collect_stall_diagnostic(node, 2, "ab" * 32, 8)
        self.assertEqual(report["errors"], {"balance": {"type": "RuntimeError", "code": -4}})
        self.assertEqual(report["validator"]["armed_key_count"], 1)
        self.assertEqual(report["market_data"]["snapshot"]["runtime"]["proposals_missing_evidence"], 3)
        self.assertEqual(report["peers"], [{"id": 8, "received": {"fmaction": 42}, "sent": {"fmprop": 100}}])
        node.getflowmeshmarketdata.assert_called_once_with("ab" * 32, {"limit": 1, "curve_limit": 1})
        self.assertNotIn("private transport detail", json.dumps(report))

    def test_each_rpc_failure_is_independent(self):
        for method, field in (("getflowmeshbalance", "balance"), ("getflowmeshvalidatorinfo", "validator"),
                              ("getflowmeshmarketdata", "market_data"), ("getpeerinfo", "peers")):
            with self.subTest(method=method):
                node = self.node()
                getattr(node, method).side_effect = RuntimeError("unavailable")
                report = collect_stall_diagnostic(node, 0, "ab" * 32, 8)
                self.assertEqual(set(report["errors"]), {field})
                for other in {"balance", "validator", "market_data", "peers"} - {field}:
                    self.assertIn(other, report)
                self.assertEqual(len(node.method_calls), 4)

    def test_peer_output_is_bounded_and_public(self):
        node = self.node()
        node.getpeerinfo.return_value *= 40
        node.getflowmeshvalidatorinfo.return_value["wallet_bls_pubkeys"] = ["omitted"] * 100
        report = collect_stall_diagnostic(node, 0, "ab" * 32, 8)
        self.assertEqual(report["peer_count"], 40)
        self.assertTrue(report["peers_truncated"])
        self.assertEqual(len(report["peers"]), 32)
        self.assertNotIn("addr", json.dumps(report))
        self.assertNotIn("wallet_bls_pubkeys", report["validator"])

    def test_wait_failure_reports_all_nodes_then_reraises(self):
        harness = Mock()
        failure = AssertionError("original wait failure")
        harness.wait_until.side_effect = failure
        harness.nodes = [self.node() for _ in range(4)]
        for node in harness.nodes[2:]:
            node.getflowmeshbalance.side_effect = RuntimeError("No FlowMesh account")
        with self.assertRaises(AssertionError) as caught:
            FlowMeshSpeedTest.wait_for_account_sequence(harness, harness.nodes[1], "ab" * 32, 8)
        self.assertIs(caught.exception, failure)
        self.assertEqual(harness.log.error.call_count, 4)
        for index, call in enumerate(harness.log.error.call_args_list):
            report = json.loads(call.args[1])
            self.assertEqual(report["node"], index)
            self.assertIn("market_data", report)
            self.assertIn("validator", report)
            self.assertIn("peers", report)


if __name__ == "__main__":
    FlowMeshSpeedTest(__file__).main()
