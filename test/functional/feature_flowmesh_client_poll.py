#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Focused generated-regtest receipt polling/coalescing regression.

Reuse only bootstrap/funding from the four-validator engine-off client fixture.
One resting bid and its cancellation exercise bounded automatic HTTP traffic,
exact unknown-action retry, eight-endpoint read failover and cached certificates.
This is not a latency campaign, fill benchmark or production endpoint test.
Run directly; no benchmark thresholds or API abuse limits are changed.
"""

import hashlib
import json
import time
from decimal import Decimal
from pathlib import Path

from feature_flowmesh_latency import FlowMeshLatencyTest, PRE_ADMISSION_REJECTIONS, host_us
from feature_flowmesh_release import B3_ARGS, TRADE_PRICE
from test_framework.authproxy import JSONRPCException
from test_framework.flowmesh_client_tls import FlowMeshTLSFaultRelay
from test_framework.test_node import TestNode
from test_framework.util import assert_equal, get_datadir_path, initialize_datadir, p2p_port


def unknown_outcome(method, body):
    """Hide proof/ack only; upstream still receives the original signed action."""
    if method not in {"submit", "action"}:
        return body
    reply = json.loads(body)
    if reply.get("ok") and isinstance(reply.get("result"), dict):
        receipt = reply["result"]
        receipt.update(receipt_state="unknown", accepted=False, certificate_verified=False,
                       outcome_verified=False, reason="Synthetic unresolved receipt for polling regression")
        for field in ("certified_payload", "microblock_hash", "microblock_sequence"):
            receipt.pop(field, None)
    return json.dumps(reply, separators=(",", ":")).encode()


def invalid_action_reply(method, body):
    if method == "action":
        # Transport is available but the receipt cannot validate. This matters:
        # transport cooldown alone must not hide a broken continuation cursor.
        return b'{"ok":true,"result":{"action_id":"invalid"}}'
    return body


def max_contained_second_attempts(spans):
    """Count only requests whose entire owning RPC is inside a one-second span.

    HTTP dispatch is between RPC entry/return. Relay receipt is after TLS and
    can shift over a boundary, so it is not an exact client dispatch timestamp.
    The deterministic C++ test checks the exact rolling-window boundary.
    """
    return max((sum(row["http_attempts"] for row in spans
                    if row["started_host_us"] >= start["started_host_us"]
                    and row["completed_host_us"] <= start["started_host_us"] + 1_000_000)
                for start in spans), default=0)


class FlowMeshClientPollTest(FlowMeshLatencyTest):
    def set_test_params(self):
        self.options.latency_production_logging = True
        super().set_test_params()
        self.poll_report = {"fixture": "focused_engine_off_client_polling", "correctness_pass": False,
                            "automatic_rpc_spans": [], "scope": "generated regtest only; no latency/fill claim"}

    def start_ordinary_client(self):
        # Six OS-assigned loopback ports avoid taking another test's fixed RPC
        # slots. All eight independently addressed relays use the generated CA.
        for index in range(8):
            port = self.client_api_ports[index] if index < 2 else 0
            self.tls_relays.append(FlowMeshTLSFaultRelay(port, self.api_ports[index % 2], self.pki))
        initialize_datadir(self.options.tmpdir, 4, self.chain, self.disable_autoconnect)
        self.client_args = [*B3_ARGS, "-enableflowmeshvalidator=0", "-debug=0",
                            f"-port={p2p_port(12)}", f"-bind=127.0.0.1:{p2p_port(12)}",
                            f"-flowmeshendpointca={self.pki['ca']}",
                            *[f"-flowmeshendpoint={relay.url}" for relay in self.tls_relays]]
        self.client = TestNode(4, get_datadir_path(self.options.tmpdir, 4), chain=self.chain,
                               rpchost=None, timewait=self.rpc_timeout, timeout_factor=self.options.timeout_factor,
                               binaries=self.get_binaries(), coverage_dir=self.options.coveragedir,
                               cwd=self.options.tmpdir, extra_args=self.client_args, uses_wallet=True)
        self.client.start()
        self.client.wait_for_rpc_connection()
        self.client.createwallet(wallet_name=self.default_wallet_name, load_on_startup=True)
        self.sync_client_time()
        self.client.addnode(f"127.0.0.1:{p2p_port(0)}", "onetry")
        self.wait_client_b3_sync()
        self.assert_engine_off()

    def assert_engine_off(self):
        info = self.client.getflowmeshclientinfo()
        assert_equal(info["backend"], "remote")
        assert_equal(info["engine_enabled"], False)
        assert_equal(len(info["endpoints"]), 8)
        validator = self.client.getflowmeshvalidatorinfo()
        assert_equal(validator["service_available"], False)
        assert_equal(validator["armed"], False)
        assert_equal(validator["wallet_key_count"], 0)
        assert not (self.client.chain_path / "flowmesh" / "network").exists()
        if self.last_market:
            assert not (self.client.chain_path / "flowmesh" / self.last_market).exists()

    def marks(self):
        return [len(relay.snapshot()["requests"]) for relay in self.tls_relays]

    def requests_since(self, marks):
        rows = []
        for index, (relay, mark) in enumerate(zip(self.tls_relays, marks)):
            snapshot = relay.snapshot()
            assert_equal(snapshot["records_dropped"], 0)
            rows.extend({"endpoint_index": index, **row} for row in snapshot["requests"][mark:])
        return sorted(rows, key=lambda row: row["host_monotonic_us"])

    def retained(self, market, action_id):
        rows = [row for row in self.client.listflowmeshactions(market)["actions"]
                if row["action_id"] == action_id]
        assert_equal(len(rows), 1)
        return rows[0]

    def new_instruction(self, method, *params):
        deadline = time.monotonic() + 30
        while True:
            self.pump_b3()
            try:
                return getattr(self.client, method)(*params)
            except JSONRPCException as error:
                # Only explicit refusal before wallet submission permits this
                # path. An ambiguous RPC failure is never signed again here.
                if error.error.get("code") != -1 or error.error.get("message") not in PRE_ADMISSION_REJECTIONS:
                    raise
                assert time.monotonic() < deadline, "pre-admission gate did not reopen"
                time.sleep(.05)

    def wait_certified(self, market, action_id, *, allow_exact_retry=False):
        deadline = time.monotonic() + 60
        next_retry = time.monotonic() + 1
        while True:
            self.pump_b3()
            status = self.client.getflowmeshactionstatus(market, action_id)
            assert_equal(status["action_id"], action_id)
            assert_equal(status["outcome_verified"], False)
            if status["certificate_verified"]:
                assert_equal(status["receipt_state"], "certified_inclusion")
                return status
            retryable = status["receipt_state"] == "unknown" or (
                status["receipt_state"] == "rejected" and status.get("reason") in PRE_ADMISSION_REJECTIONS)
            assert status["receipt_state"] in {"unknown", "queued", "admitted"} or retryable, status
            assert time.monotonic() < deadline, "read-only polling did not obtain certificate"
            if allow_exact_retry and retryable and time.monotonic() >= next_retry:
                retried = self.client.retryflowmeshaction(market, action_id)
                assert_equal(retried["action_id"], action_id)
                next_retry = time.monotonic() + 1
            time.sleep(.005)

    def run_test(self):
        assert_equal(self.options.qt_review_hold_seconds, 0)
        try:
            market, asset = self.bootstrap_latency_market()
            account = self.fund_latency_client(market, asset)
            self.poll_report.update(market_id=market, account_id=account["account_id"])
            self.begin_b3_workload(range(4))
            for relay in self.tls_relays:
                relay.configure(reply_mutation=unknown_outcome)
            marks = self.marks()
            response = self.new_instruction("submitflowmeshorder", market, "bid", TRADE_PRICE // 2,
                                            1, account["next_sequence"])
            action_id = response["action_id"]
            assert_equal(response["receipt_state"], "unknown")
            original = self.retained(market, action_id)
            original_requests = [row for row in self.requests_since(marks) if row["method"] == "submit"]
            assert_equal(len(original_requests), 1)
            exact = original_requests[0]["action_hex"]
            assert_equal(original["signed_bytes_sha256"], hashlib.sha256(bytes.fromhex(exact)).hexdigest())

            # The caller still polls every5ms. Only the reusable client backend
            # may coalesce HTTP work; no external limit or harness counter reset.
            marks = self.marks()
            deadline = time.monotonic() + 2.25
            while time.monotonic() < deadline:
                self.pump_b3()
                before = self.marks()
                started = host_us()
                status = self.client.getflowmeshactionstatus(market, action_id)
                completed = host_us()
                requests = self.requests_since(before)
                assert all(row["method"] == "action" and row["action_id"] == action_id for row in requests)
                assert_equal(status["receipt_state"], "unknown")
                assert_equal(status["certificate_verified"], False)
                assert_equal(status["outcome_verified"], False)
                self.poll_report["automatic_rpc_spans"].append({"started_host_us": started,
                    "completed_host_us": completed, "http_attempts": len(requests)})
                time.sleep(.005)
            automatic = self.requests_since(marks)
            spans = self.poll_report["automatic_rpc_spans"]
            maximum = max_contained_second_attempts(spans)
            assert maximum <= 16, (maximum, spans)
            assert len(automatic) > 0
            assert any(row["http_attempts"] == 0 for row in spans), "no automatic status call was coalesced"
            assert all(row["method"] == "action" for row in automatic)
            assert all(row.get("upstream_http_status") == 200 for row in automatic)
            self.poll_report["automatic_polling"] = {"rpc_calls": len(spans), "http_requests": len(automatic),
                "maximum_requests_in_fully_contained_one_second_rpc_spans": maximum,
                "boundary": "client dispatch lies inside the RPC span; exact gate timing also unit-tested"}

            # An explicit retry ignores the automatic allowance but performs
            # only ONE fresh status query before its ONE exact-byte submission.
            marks = self.marks()
            retried = self.client.retryflowmeshaction(market, action_id)
            rows = self.requests_since(marks)
            assert_equal([row["method"] for row in rows], ["action", "submit"])
            assert all(row["action_id"] == action_id for row in rows)
            assert_equal(rows[1]["action_hex"], exact)
            assert_equal(retried["receipt_state"], "unknown")
            assert_equal(retried["certificate_verified"], False)
            after_retry = self.retained(market, action_id)
            for field in ("sequence", "signed_bytes_sha256", "signed_bytes_size", "initial_submission_ms", "account_id"):
                assert_equal(after_retry[field], original[field])
            assert_equal(after_retry["previously_certified"], False)
            self.poll_report["exact_retry"] = {"methods": [row["method"] for row in rows],
                "action_id": action_id, "signed_bytes_sha256": original["signed_bytes_sha256"]}

            for relay in self.tls_relays[:-1]:
                relay.configure(reply_mutation=invalid_action_reply)
            self.tls_relays[-1].configure()
            # Selecting an already configured endpoint does not reset the poll
            # allowance or signed object; its discovery request remains healthy.
            self.client.flowmeshclientconnect(self.tls_relays[0].url)
            marks = self.marks()
            certified = self.wait_certified(market, action_id)
            failover = self.requests_since(marks)
            assert failover and all(row["method"] == "action" for row in failover)
            assert_equal({row["endpoint_index"] for row in failover}, set(range(8)))
            assert_equal(certified["endpoint"], self.tls_relays[7].url)
            self.poll_report["eight_endpoint_failover"] = {"attempts": len(failover),
                "endpoint_order": [row["endpoint_index"] for row in failover], "certificate": certified}

            marks = self.marks()
            for _ in range(20):
                assert_equal(self.client.getflowmeshactionstatus(market, action_id)["certificate_verified"], True)
            for _ in range(3):
                assert_equal(self.client.retryflowmeshaction(market, action_id)["certificate_verified"], True)
            assert_equal(self.requests_since(marks), [])
            assert_equal(self.retained(market, action_id)["previously_certified"], True)
            self.poll_report["cached_certificate_http_requests"] = 0

            for relay in self.tls_relays:
                relay.configure()
            bid_account = self.wait_client_balance(market, lambda row: row["next_sequence"] == account["next_sequence"] + 1)
            assert_equal(bid_account["b3_reserved"], Decimal("0.05"))
            cancelled = self.new_instruction("cancelflowmeshorder", market, "bid", bid_account["next_sequence"])
            cancel_id = cancelled["action_id"]
            self.wait_certified(market, cancel_id, allow_exact_retry=True)
            final_account = self.wait_client_balance(market, lambda row: row["next_sequence"] == bid_account["next_sequence"] + 1)
            assert_equal(final_account["b3_reserved"], Decimal("0"))
            assert_equal(final_account["b3_available"], Decimal("1"))
            assert_equal(final_account["base_available"], 0)
            self.poll_report["cancel_action_id"] = cancel_id
            self.assert_engine_off()
            self.assert_no_b3_flowmesh_traffic()
            self.poll_report["correctness_pass"] = True
        except Exception as error:
            self.poll_report["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            for relay in self.tls_relays:
                relay.configure()
            try:
                self.end_b3_workload()
            finally:
                path = Path(self.options.tmpdir, "flowmesh-client-poll.json")
                path.write_text(json.dumps(self.poll_report, sort_keys=True, indent=2, default=str) + "\n", encoding="utf-8")
                self.log.info("FLOWMESH_CLIENT_POLL_REPORT %s", path)


if __name__ == "__main__":
    FlowMeshClientPollTest(__file__).main()
