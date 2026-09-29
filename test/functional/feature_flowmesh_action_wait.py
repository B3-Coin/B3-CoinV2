#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Server-side bounded 'action' and 'submit' waits on the restricted HTTPS trading API.

Reuse only the latency fixture's generated four-validator bootstrap, funding
and engine-off client. Raw public-API requests go straight to validator 0
(waits, including waited submissions, enabled by default) and validator 1
(-flowmeshapiactionwait=0) with the generated CA. Two seats are disarmed so
that one admitted action provably stays uncertified: waits end at their
deadline, a third concurrent waiter from one address (a read or a waited
resubmission of the exact signed bytes) is refused at once, and ordinary reads
keep their workers. Re-arming certifies it, and a single waited read (or a
waited resubmission) returns the certified payload. The unchanged wallet RPC
with wait_ms then places an order in one waited submission whose certified
inclusion the client verifies in the same call. Stopping validator 0 ends a
waiting read and a waiting submission promptly. This is not a latency
benchmark.
"""

import http.client
import json
import ssl
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from feature_flowmesh_independent import RECONCILING_REFUSAL
from feature_flowmesh_latency import FlowMeshLatencyTest, PRE_ADMISSION_REJECTIONS
from feature_flowmesh_release import TRADE_PRICE
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal

WAIT_MAX_MS = 2000  # -flowmeshapiactionwait default


class FlowMeshActionWaitTest(FlowMeshLatencyTest):
    def set_test_params(self):
        self.options.latency_production_logging = True
        super().set_test_params()
        # Validator 1 keeps the previous request contract exactly.
        self.extra_args[1].append("-flowmeshapiactionwait=0")
        self.wait_report = {"fixture": "generated regtest restricted-API action wait",
                            "scope": "correctness only; no latency claim", "correctness_pass": False,
                            "reads": []}

    def raw(self, method, params, index=0):
        context = ssl.create_default_context(cafile=str(self.pki["ca"]))
        connection = http.client.HTTPSConnection("127.0.0.1", self.api_ports[index], timeout=15, context=context)
        started = time.monotonic()
        try:
            connection.request("POST", "/flowmesh/v1", body=json.dumps({"method": method, "params": params}),
                               headers={"Content-Type": "application/json"})
            reply = connection.getresponse()
            body = reply.read(32 * 1024 * 1024 + 1)
            return reply.status, json.loads(body), (time.monotonic() - started) * 1000
        finally:
            connection.close()

    def action(self, market, action_id, wait_ms=None, index=0):
        params = {"market_id": market, "action_id": action_id}
        if wait_ms is not None:
            params["wait_ms"] = wait_ms
        status, reply, elapsed_ms = self.raw("action", params, index)
        assert_equal((status, reply["ok"]), (200, True))
        result = reply["result"]
        assert_equal(result["action_id"], action_id)
        # A latency hint only, and present only when the request asked.
        assert_equal("wait_status" in result, wait_ms is not None)
        self.wait_report["reads"].append({"validator": index, "wait_ms": wait_ms, "elapsed_ms": round(elapsed_ms, 3),
                                          "receipt_state": result["receipt_state"],
                                          "wait_status": result.get("wait_status")})
        return result, elapsed_ms

    def exact_signed_hex(self, action_id):
        """The exact signed bytes the client delivered, as the relays recorded them."""
        rows = [row for relay in self.tls_relays for row in relay.snapshot()["requests"]] + self.submit_records
        for row in rows:
            if row["method"] == "submit" and row["action_id"] == action_id and row.get("action_hex"):
                return row["action_hex"]
        raise AssertionError(f"no recorded submission of {action_id}")

    def submit(self, market, action_id, action_hex, wait_ms=None, index=0):
        """A raw resubmission of exact signed bytes; with wait_ms, a waited submission."""
        params = {"market_id": market, "action_id": action_id, "action_hex": action_hex}
        if wait_ms is not None:
            params["wait_ms"] = wait_ms
        status, reply, elapsed_ms = self.raw("submit", params, index)
        assert_equal((status, reply["ok"]), (200, True))
        result = reply["result"]
        assert_equal(result["action_id"], action_id)
        # A latency hint only, and present only when the request asked.
        assert_equal("wait_status" in result, wait_ms is not None)
        if wait_ms is None:
            assert "action" not in result, result
        elif "action" in result:
            # Exactly the 'action' reply for the same action after the wait.
            assert_equal(result["action"]["action_id"], action_id)
            assert_equal(result["action"]["wait_status"], result["wait_status"])
        self.wait_report["reads"].append({"validator": index, "submit_wait_ms": wait_ms, "elapsed_ms": round(elapsed_ms, 3),
                                          "receipt_state": result["receipt_state"],
                                          "wait_status": result.get("wait_status")})
        return result, elapsed_ms

    def new_instruction(self, method, *params):
        deadline = time.monotonic() + 30
        while True:
            self.pump_b3()
            try:
                response = getattr(self.client, method)(*params)
                break
            except JSONRPCException as error:
                # Only an explicit refusal before submission permits signing again.
                if error.error.get("code") != -1 or error.error.get("message") not in PRE_ADMISSION_REJECTIONS:
                    raise
                assert time.monotonic() < deadline, "pre-admission gate did not reopen"
                time.sleep(.05)
        # A B3 block can connect after the wallet's status check passed and
        # before the signed action reaches the endpoint, whose reconciliation
        # gate then refuses it before runtime admission. That is returned as a
        # receipt (rejected; unknown on a later attempt, which the client
        # cannot tell from an earlier possible delivery), not as the error
        # above, and nothing resends it: without this, the reads below would
        # wait for an action no validator ever admitted. Resend the exact
        # retained action, never a new signature, until an endpoint queues it.
        action_id = response["action_id"]
        while response["receipt_state"] not in {"queued", "admitted"}:
            assert response["receipt_state"] in {"rejected", "unknown"}, response
            assert_equal(response["reason"], RECONCILING_REFUSAL)
            self.wait_report.setdefault("reconciling_refusals", []).append(
                {"method": method, "action_id": action_id, "receipt_state": response["receipt_state"]})
            assert time.monotonic() < deadline, "reconciliation gate did not reopen"
            time.sleep(.05)
            self.pump_b3()
            response = self.client.retryflowmeshaction(params[0], action_id)
            assert_equal(response["action_id"], action_id)
        return response

    def check_capability(self, market):
        for index, advertised in ((0, WAIT_MAX_MS), (1, None)):
            status, reply, _ = self.raw("markets", {}, index)
            assert_equal(status, 200)
            rows = [row for row in reply["result"] if row["market_id"] == market]
            assert_equal(len(rows), 1)
            objects = [rows[0]]
            for method in ("snapshot", "updates"):
                status, reply, _ = self.raw(method, {"market_id": market}, index)
                assert_equal(status, 200)
                objects.append(reply["result"]["status"])
            for row in objects:
                assert_equal(row.get("action_wait_ms_max"), advertised)
                # Waited submissions share the action wait, and its maximum.
                assert_equal(row.get("submit_wait_ms_max"), advertised)
        # The disabled server keeps rejecting the field; the enabled one never
        # waits for an action it has not recorded.
        status, reply, _ = self.raw("action", {"market_id": market, "action_id": "11" * 32, "wait_ms": 500}, 1)
        assert_equal((status, reply["ok"], reply["error"]), (400, False, "Unknown or duplicate client API field"))
        junk = {"market_id": market, "action_id": "11" * 32, "action_hex": "00"}
        status, reply, _ = self.raw("submit", {**junk, "wait_ms": 500}, 1)
        assert_equal((status, reply["ok"], reply["error"]), (400, False, "Unknown or duplicate client API field"))
        # A malformed wait is refused before the action is even decoded.
        status, reply, _ = self.raw("submit", {**junk, "wait_ms": "500"}, 0)
        assert_equal((status, reply["ok"], reply["error"]), (400, False, "Expected integer: wait_ms"))
        result, elapsed = self.action(market, "11" * 32, 2500)
        assert_equal((result["receipt_state"], result["wait_status"]), ("unknown", "untracked"))
        assert elapsed < 1000, elapsed
        self.wait_report["capability"] = {"validator0": WAIT_MAX_MS, "validator1": None,
                                          "submit_wait": {"validator0": WAIT_MAX_MS, "validator1": None}}

    def run_test(self):
        try:
            market, asset = self.bootstrap_latency_market()
            account = self.fund_latency_client(market, asset)
            self.wait_report["market_id"] = market
            self.begin_b3_workload(range(4))
            self.check_capability(market)

            # Two of four seats stop signing: the next action is admitted but
            # cannot reach the three-signature certificate threshold.
            for node in self.nodes[2:]:
                assert_equal(node.stopflowmeshvalidator()["running"], False)
            response = self.new_instruction("submitflowmeshorder", market, "bid", TRADE_PRICE // 2,
                                            1, account["next_sequence"])
            action_id = response["action_id"]
            self.wait_report["action_id"] = action_id
            self.wait_until(lambda: self.action(market, action_id)[0]["receipt_state"] in {"queued", "admitted"},
                            timeout=30, check_interval=.1)

            result, elapsed = self.action(market, action_id, 500)
            assert_equal(result["wait_status"], "timeout")
            assert result["receipt_state"] in {"queued", "admitted"}, result
            assert 450 <= elapsed < 2000, elapsed
            # A waited resubmission of the exact signed bytes waits the same
            # way; its acknowledgement and the embedded read are unchanged.
            action_hex = self.exact_signed_hex(action_id)
            resubmitted, elapsed = self.submit(market, action_id, action_hex, 500)
            assert_equal(resubmitted["wait_status"], "timeout")
            assert resubmitted["receipt_state"] in {"queued", "admitted"}, resubmitted
            assert resubmitted["action"]["receipt_state"] in {"queued", "admitted"}, resubmitted
            assert 450 <= elapsed < 2000, elapsed
            plain_resubmission, _ = self.submit(market, action_id, action_hex)
            assert plain_resubmission["receipt_state"] in {"queued", "admitted"}, plain_resubmission
            # Clamped to the advertised maximum, never refused.
            result, elapsed = self.action(market, action_id, 60000)
            assert_equal(result["wait_status"], "timeout")
            assert WAIT_MAX_MS - 50 <= elapsed < WAIT_MAX_MS + 1500, elapsed

            # Two slots per client address. A third waiter gets the immediate
            # reply, and ordinary reads keep their workers meanwhile.
            with ThreadPoolExecutor(max_workers=2) as pool:
                waiters = [pool.submit(self.action, market, action_id, 1500) for _ in range(2)]
                time.sleep(.3)
                third, elapsed = self.action(market, action_id, 1500)
                assert_equal(third["wait_status"], "busy")
                assert elapsed < 1000, elapsed
                # A waited submission takes the same slots: busy at once, and
                # still answered with its acknowledgement and the read.
                third, elapsed = self.submit(market, action_id, action_hex, 1500)
                assert_equal((third["wait_status"], third["action"]["wait_status"]), ("busy", "busy"))
                assert third["receipt_state"] in {"queued", "admitted"}, third
                assert elapsed < 1000, elapsed
                status, _, elapsed = self.raw("markets", {})
                assert_equal(status, 200)
                assert elapsed < 1000, elapsed
                for waiter in waiters:
                    result, elapsed = waiter.result()
                    assert_equal(result["wait_status"], "timeout")
                    assert elapsed >= 1400, elapsed

            # Re-arm. One waited read returns certified inclusion and the
            # durable certified payload, never a payload built from the event.
            for node in self.nodes[2:]:
                assert_equal(node.startflowmeshvalidator()["armed_keys"], 1)
            deadline = time.monotonic() + 90
            while True:
                self.pump_b3()
                result, elapsed = self.action(market, action_id, WAIT_MAX_MS)
                if result["receipt_state"] == "certified_inclusion":
                    break
                assert_equal(result["wait_status"], "timeout")
                assert time.monotonic() < deadline, "re-armed seats did not certify the action"
            assert result["wait_status"] in {"terminal", "none"}, result
            assert elapsed < WAIT_MAX_MS + 1000, elapsed
            assert "certified_payload" in result and "evidence_error" not in result, result
            assert_equal(result["certificate_verified"], True)
            self.wait_report["certified_read"] = {"wait_status": result["wait_status"], "elapsed_ms": round(elapsed, 3),
                                                  "microblock_sequence": result["microblock_sequence"]}
            plain, _ = self.action(market, action_id)
            assert_equal(plain["certified_payload"], result["certified_payload"])
            again, elapsed = self.action(market, action_id, WAIT_MAX_MS)
            assert_equal(again["wait_status"], "none")
            assert elapsed < 1000, elapsed
            # Resubmitted with a wait once certified: answered at once from
            # certified data, with the same durable payload, never refused.
            resubmitted, elapsed = self.submit(market, action_id, action_hex, WAIT_MAX_MS)
            assert_equal((resubmitted["receipt_state"], resubmitted["wait_status"]), ("certified_inclusion", "none"))
            assert_equal(resubmitted["action"]["certified_payload"], result["certified_payload"])
            assert elapsed < 1000, elapsed
            # Validator 1 applies the same certificate on its own schedule and
            # cannot wait server-side (its waits are disabled): poll it until
            # it reports the inclusion, then compare.
            def validator1_included():
                self.pump_b3()
                return self.action(market, action_id, index=1)[0]["receipt_state"] == "certified_inclusion"
            self.wait_until(validator1_included, timeout=30, check_interval=.1)
            other, _ = self.action(market, action_id, index=1)
            assert_equal((other["microblock_sequence"], other["microblock_hash"]),
                         (result["microblock_sequence"], result["microblock_hash"]))

            # The unchanged client resolves the same action by its ordinary path.
            def client_certified():
                self.pump_b3()
                return self.client.getflowmeshactionstatus(market, action_id)["certificate_verified"]
            self.wait_until(client_certified, timeout=60, check_interval=.1)

            # The unchanged wallet RPC with wait_ms: one waited submission
            # through the relay to validator 0, no separate status read, and
            # the certified inclusion verified by the client in the same call.
            order_account = self.wait_client_balance(
                market, lambda row: row["next_sequence"] == account["next_sequence"] + 1)
            self.drain_relay_observations()
            started = time.monotonic()
            placed = self.client.submitflowmeshorder(market, "bid", TRADE_PRICE // 2, 1,
                                                     order_account["next_sequence"], WAIT_MAX_MS)
            placed_ms = (time.monotonic() - started) * 1000
            rows = [row for relay in self.tls_relays for row in relay.snapshot()["requests"]
                    if row.get("action_id") == placed["action_id"]]
            submissions = [row for row in rows if row["method"] == "submit"]
            assert_equal(len(submissions), 1)
            assert_equal(json.loads(bytes.fromhex(submissions[0]["body_hex"]))["params"]["wait_ms"], WAIT_MAX_MS)
            assert_equal([row for row in rows if row["method"] == "action"], [])
            assert_equal((placed["receipt_state"], placed["certificate_verified"]), ("certified_inclusion", True))
            self.wait_report["waited_submission"] = {"action_id": placed["action_id"], "elapsed_ms": round(placed_ms, 3),
                                                     "https_requests_for_action": len(rows),
                                                     "microblock_sequence": placed["microblock_sequence"]}

            # Shutdown ends a waiting read at once rather than after its wait.
            for node in self.nodes[2:]:
                assert_equal(node.stopflowmeshvalidator()["running"], False)
            bid_account = self.wait_client_balance(
                market, lambda row: row["next_sequence"] == account["next_sequence"] + 2)
            cancel_id = self.new_instruction("cancelflowmeshorder", market, "bid", bid_account["next_sequence"])["action_id"]
            self.wait_until(lambda: self.action(market, cancel_id)[0]["receipt_state"] in {"queued", "admitted"},
                            timeout=30, check_interval=.1)
            cancel_hex = self.exact_signed_hex(cancel_id)
            with ThreadPoolExecutor(max_workers=2) as pool:
                def waiting(method, params):
                    try:
                        return self.raw(method, params)
                    except (OSError, http.client.HTTPException) as error:
                        return type(error).__name__
                    finally:
                        ended.append(time.monotonic())
                ended = []
                pending = [pool.submit(waiting, "action", {"market_id": market, "action_id": cancel_id, "wait_ms": WAIT_MAX_MS}),
                           pool.submit(waiting, "submit", {"market_id": market, "action_id": cancel_id,
                                                           "action_hex": cancel_hex, "wait_ms": WAIT_MAX_MS})]
                time.sleep(.3)
                stop_started = time.monotonic()
                self.stop_node(0)
                outcomes = [request.result() for request in pending]
            interrupted_ms = [(end - stop_started) * 1000 for end in ended]
            self.wait_report["shutdown"] = {"outcomes": [outcome if isinstance(outcome, str) else outcome[1]["result"].get("wait_status")
                                                         for outcome in outcomes],
                                            "ended_after_stop_ms": [round(value, 3) for value in interrupted_ms]}
            # Each wait still had about 1.7 s left when the stop began.
            assert_equal(len(interrupted_ms), 2)
            assert max(interrupted_ms) < 1200, self.wait_report["shutdown"]
            self.wait_report["correctness_pass"] = True
        except Exception as error:
            self.wait_report["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            try:
                self.end_b3_workload()
            finally:
                path = Path(self.options.tmpdir, "flowmesh-action-wait.json")
                path.write_text(json.dumps(self.wait_report, sort_keys=True, indent=2, default=str) + "\n", encoding="utf-8")
                self.log.info("FLOWMESH_ACTION_WAIT_REPORT %s", path)


if __name__ == "__main__":
    FlowMeshActionWaitTest(__file__).main()
