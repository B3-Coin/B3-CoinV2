#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Focused generated-regtest receipt polling/coalescing regression.

Reuse only bootstrap/funding from the four-validator engine-off client fixture.
One resting bid and its cancellation exercise bounded automatic HTTP traffic,
exact unknown-action retry, eight-endpoint read failover and cached certificates.
Further bids/cancels exercise the bounded waited status read (wait_ms): one
lane request per waited call that leaves the client work gate free, no
wait_ms toward an endpoint that does not advertise it, and a same-call
fallback that withdraws a stale capability (validator 1 disables waits).
This is not a latency campaign, fill benchmark or production endpoint test.
Run directly; no benchmark thresholds or API abuse limits are changed.
"""

import hashlib
import json
import time
from concurrent.futures import ThreadPoolExecutor
from decimal import Decimal
from pathlib import Path

from feature_flowmesh_latency import FlowMeshLatencyTest, PRE_ADMISSION_REJECTIONS, host_us
from feature_flowmesh_release import B3_ARGS, TRADE_PRICE
from test_framework.authproxy import JSONRPCException
from test_framework.flowmesh_client_tls import FlowMeshTLSFaultRelay
from test_framework.test_node import TestNode
from test_framework.util import assert_equal, get_datadir_path, get_rpc_proxy, initialize_datadir, p2p_port

WAIT_MAX_MS = 2000  # -flowmeshapiactionwait default on validator 0


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


def status_objects(method, result):
    if method == "markets" and isinstance(result, list):
        return [row for row in result if isinstance(row, dict)]
    if method in {"snapshot", "updates"} and isinstance(result, dict) and isinstance(result.get("status"), dict):
        return [result["status"]]
    return []


def rewrite_status(method, body, change):
    reply = json.loads(body)
    rows = status_objects(method, reply.get("result")) if reply.get("ok") else []
    if not rows:
        return body
    for row in rows:
        change(row)
    return json.dumps(reply, separators=(",", ":")).encode()


def strip_action_wait(method, body):
    """Present a c10c952 endpoint: status objects carry no wait capability."""
    return rewrite_status(method, body, lambda row: row.pop("action_wait_ms_max", None))


def advertise_action_wait(method, body):
    """A stale capability: advertise waits for a server that has them disabled."""
    return rewrite_status(method, body, lambda row: row.update(action_wait_ms_max=WAIT_MAX_MS))


def request_wait_ms(row):
    """wait_ms of a recorded request; 'action' bodies are always recorded."""
    return json.loads(bytes.fromhex(row["body_hex"]))["params"].get("wait_ms")


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
        # Validator 1 keeps the previous 'action' contract (wait_ms is an
        # unknown field); relays with an odd index forward to it.
        self.extra_args[1].append("-flowmeshapiactionwait=0")
        # Appended to the client's startup arguments (kept for its restarts).
        self.client_extra_args = []
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
                            *[f"-flowmeshendpoint={relay.url}" for relay in self.tls_relays],
                            *self.client_extra_args]
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

    def select_endpoint(self, index):
        # Selects the endpoint for submits and preferred reads, and re-reads
        # its advertised capability from its discovery rows.
        self.client.flowmeshclientconnect(self.tls_relays[index].url)

    def submitted_via(self, marks, action_id):
        rows = [row for row in self.requests_since(marks) if row["method"] == "submit"]
        assert_equal([row["action_id"] for row in rows], [action_id])
        return rows[0]["endpoint_index"]

    def drain_status_budget(self):
        # A waited read is charged to the client's automatic status budget
        # (16 attempts per rolling second) like any automatic read. Let an
        # earlier phase's 5 ms polling leave that window, so a phase that
        # counts requests observes its own reads rather than coalescing.
        time.sleep(1.1)

    def waited_status(self, market, action_id):
        """One waited RPC; returns its receipt and the requests it caused."""
        marks = self.marks()
        started = time.monotonic()
        status = self.client.getflowmeshactionstatus(market, action_id, WAIT_MAX_MS)
        elapsed_ms = (time.monotonic() - started) * 1000
        assert_equal(status["action_id"], action_id)
        assert_equal(status["outcome_verified"], False)
        return status, self.requests_since(marks), elapsed_ms

    def waited_until_certified(self, market, action_id, endpoint):
        """Waited reads until certified. Each RPC starts with one 'action'
        request carrying wait_ms to the delivering endpoint. An ordinary read
        follows only if that reply could not be applied (for example a
        certificate whose B3 anchor this client has not connected yet)."""
        deadline = time.monotonic() + 90
        calls = []
        while True:
            self.pump_b3()
            status, rows, elapsed_ms = self.waited_status(market, action_id)
            assert rows and all(row["method"] == "action" for row in rows), rows
            assert_equal((rows[0]["endpoint_index"], request_wait_ms(rows[0])), (endpoint, WAIT_MAX_MS))
            assert_equal(rows[0]["upstream_http_status"], 200)
            assert all(request_wait_ms(row) is None for row in rows[1:]), rows
            calls.append({"elapsed_ms": round(elapsed_ms, 3), "receipt_state": status["receipt_state"],
                          "http_requests": len(rows)})
            if status["certificate_verified"]:
                assert_equal(status["receipt_state"], "certified_inclusion")
                return status, calls
            assert status["receipt_state"] in {"queued", "admitted"}, status
            assert time.monotonic() < deadline, "waited reads did not obtain the certificate"

    def run_waited_status(self, market, account):
        report = self.poll_report["waited_status"] = {}
        for relay in self.tls_relays:
            relay.configure()
        self.select_endpoint(0)

        # A. Two of four seats stop signing, so the admitted bid provably
        # stays uncertified. The waited read is one lane request that the
        # endpoint holds for its advertised maximum; meanwhile the client
        # work gate is free, so a verified market read completes first.
        for node in self.nodes[2:]:
            assert_equal(node.stopflowmeshvalidator()["running"], False)
        marks = self.marks()
        bid_id = self.new_instruction("submitflowmeshorder", market, "bid", TRADE_PRICE // 2, 1,
                                      account["next_sequence"])["action_id"]
        assert_equal(self.submitted_via(marks, bid_id), 0)
        marks = self.marks()
        rpc = get_rpc_proxy(self.client.url, 4, timeout=90)
        with ThreadPoolExecutor(max_workers=1) as pool:
            started = time.monotonic()
            def waited():
                result = rpc.getflowmeshactionstatus(market, bid_id, WAIT_MAX_MS)
                return result, time.monotonic()
            pending = pool.submit(waited)
            self.wait_until(lambda: any(row["method"] == "action" for row in self.requests_since(marks)),
                            timeout=10, check_interval=.01)
            self.client.getflowmeshmarketdata(market)
            read_ended = time.monotonic()
            status, waited_ended = pending.result()
        actions = [row for row in self.requests_since(marks) if row["method"] == "action"]
        assert_equal(len(actions), 1)
        assert_equal((actions[0]["endpoint_index"], request_wait_ms(actions[0])), (0, WAIT_MAX_MS))
        assert_equal(actions[0]["upstream_http_status"], 200)
        assert not any(row["method"] == "submit" for row in self.requests_since(marks))
        assert status["receipt_state"] in {"queued", "admitted"}, status
        assert_equal(status["certificate_verified"], False)
        waited_ms = (waited_ended - started) * 1000
        assert WAIT_MAX_MS - 500 <= waited_ms < WAIT_MAX_MS + 3000, waited_ms
        assert read_ended < waited_ended - .2, (read_ended, waited_ended)
        report["uncertified_wait"] = {"waited_ms": round(waited_ms, 3),
                                      "market_read_finished_before_wait_ms": round((waited_ended - read_ended) * 1000, 3)}

        # B. Re-armed, waited reads (one request each, no fallback poll)
        # obtain the certified inclusion; nothing is resent, and the verified
        # receipt is then served without any request.
        for node in self.nodes[2:]:
            assert_equal(node.startflowmeshvalidator()["armed_keys"], 1)
        self.drain_status_budget()
        marks = self.marks()
        certified, calls = self.waited_until_certified(market, bid_id, 0)
        assert not any(row["method"] == "submit" for row in self.requests_since(marks))
        assert_equal(self.retained(market, bid_id)["previously_certified"], True)
        marks = self.marks()
        again, rows, _ = self.waited_status(market, bid_id)
        assert_equal((again["certificate_verified"], rows), (True, []))
        report["certified_after_rearm"] = {"rpc_calls": calls, "endpoint": certified["endpoint"]}

        # C. An endpoint without the capability (c10c952 shape) never sees
        # wait_ms; the waited call is the ordinary immediate read.
        for relay in self.tls_relays:
            relay.configure(reply_mutation=strip_action_wait)
        self.select_endpoint(0)
        bid_account = self.wait_client_balance(market, lambda row: row["next_sequence"] == account["next_sequence"] + 1)
        marks = self.marks()
        cancel_id = self.new_instruction("cancelflowmeshorder", market, "bid", bid_account["next_sequence"])["action_id"]
        assert_equal(self.submitted_via(marks, cancel_id), 0)
        status, rows, elapsed_ms = self.waited_status(market, cancel_id)
        assert all(request_wait_ms(row) is None for row in rows if row["method"] == "action")
        assert all(row["method"] == "action" for row in rows), rows
        assert elapsed_ms < 1500, elapsed_ms
        self.wait_certified(market, cancel_id)
        assert not any(request_wait_ms(row) is not None for row in self.requests_since(marks) if row["method"] == "action")
        report["old_endpoint_shape"] = {"elapsed_ms": round(elapsed_ms, 3), "requests": len(rows)}

        # D. A stale capability for validator 1 (waits disabled): the waited
        # request is refused as an unknown field, the ordinary read follows
        # in the same call, nothing is resent, and the capability is
        # withdrawn until the endpoint advertises it again.
        for relay in self.tls_relays:
            relay.configure()
        self.tls_relays[1].configure(reply_mutation=advertise_action_wait)
        self.select_endpoint(1)
        cancel_account = self.wait_client_balance(market, lambda row: row["next_sequence"] == bid_account["next_sequence"] + 1)
        marks = self.marks()
        stale_id = self.new_instruction("submitflowmeshorder", market, "bid", TRADE_PRICE // 2, 1,
                                        cancel_account["next_sequence"])["action_id"]
        assert_equal(self.submitted_via(marks, stale_id), 1)
        self.drain_status_budget()
        marks = self.marks()
        status, rows, _ = self.waited_status(market, stale_id)
        assert_equal([row["method"] for row in rows], ["action", "action"])
        assert_equal((rows[0]["endpoint_index"], request_wait_ms(rows[0])), (1, WAIT_MAX_MS))
        assert_equal(rows[0]["upstream_http_status"], 400)
        assert_equal((request_wait_ms(rows[1]), rows[1]["upstream_http_status"]), (None, 200))
        assert status["receipt_state"] in {"queued", "admitted", "certified_inclusion"}, status
        self.drain_status_budget()
        status, rows, _ = self.waited_status(market, stale_id)
        if not status["certificate_verified"]:
            assert_equal([(row["method"], request_wait_ms(row)) for row in rows], [("action", None)])
        assert not any(row["method"] == "submit" for row in self.requests_since(marks))
        self.tls_relays[1].configure()
        self.wait_certified(market, stale_id)
        report["stale_capability"] = {"fallback_same_call": True}

        # E. With every seat signing, a fresh cancel usually certifies within
        # its first waited read (one request, no polling interval).
        self.select_endpoint(0)
        stale_account = self.wait_client_balance(market, lambda row: row["next_sequence"] == cancel_account["next_sequence"] + 1)
        marks = self.marks()
        final_id = self.new_instruction("cancelflowmeshorder", market, "bid", stale_account["next_sequence"])["action_id"]
        assert_equal(self.submitted_via(marks, final_id), 0)
        self.drain_status_budget()
        marks = self.marks()
        certified, calls = self.waited_until_certified(market, final_id, 0)
        assert not any(row["method"] == "submit" for row in self.requests_since(marks))
        report["armed_certification"] = {"rpc_calls": calls}
        final = self.wait_client_balance(market, lambda row: row["next_sequence"] == stale_account["next_sequence"] + 1)
        assert_equal(final["b3_reserved"], Decimal("0"))

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
            self.run_waited_status(market, final_account)
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
