#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Server-side bounded 'action' wait on the restricted HTTPS trading API.

Reuse only the latency fixture's generated four-validator bootstrap, funding
and engine-off client. Raw public-API reads go straight to validator 0 (waits
enabled by default) and validator 1 (-flowmeshapiactionwait=0) with the
generated CA. Two seats are disarmed so that one admitted action provably
stays uncertified: waits end at their deadline, a third concurrent waiter from
one address is refused at once, and ordinary reads keep their workers.
Re-arming certifies it, and a single waited read returns the certified payload.
Stopping validator 0 ends a waiting read promptly. This is not a latency
benchmark; the client never sends wait_ms here.
"""

import http.client
import json
import ssl
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

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

    def new_instruction(self, method, *params):
        deadline = time.monotonic() + 30
        while True:
            self.pump_b3()
            try:
                return getattr(self.client, method)(*params)
            except JSONRPCException as error:
                # Only an explicit refusal before submission permits signing again.
                if error.error.get("code") != -1 or error.error.get("message") not in PRE_ADMISSION_REJECTIONS:
                    raise
                assert time.monotonic() < deadline, "pre-admission gate did not reopen"
                time.sleep(.05)

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
        # The disabled server keeps rejecting the field; the enabled one never
        # waits for an action it has not recorded.
        status, reply, _ = self.raw("action", {"market_id": market, "action_id": "11" * 32, "wait_ms": 500}, 1)
        assert_equal((status, reply["ok"], reply["error"]), (400, False, "Unknown or duplicate client API field"))
        result, elapsed = self.action(market, "11" * 32, 2500)
        assert_equal((result["receipt_state"], result["wait_status"]), ("unknown", "untracked"))
        assert elapsed < 1000, elapsed
        self.wait_report["capability"] = {"validator0": WAIT_MAX_MS, "validator1": None}

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
            other, _ = self.action(market, action_id, index=1)
            assert_equal((other["microblock_sequence"], other["microblock_hash"]),
                         (result["microblock_sequence"], result["microblock_hash"]))

            # The unchanged client resolves the same action by its ordinary path.
            def client_certified():
                self.pump_b3()
                return self.client.getflowmeshactionstatus(market, action_id)["certificate_verified"]
            self.wait_until(client_certified, timeout=60, check_interval=.1)

            # Shutdown ends a waiting read at once rather than after its wait.
            for node in self.nodes[2:]:
                assert_equal(node.stopflowmeshvalidator()["running"], False)
            bid_account = self.wait_client_balance(
                market, lambda row: row["next_sequence"] == account["next_sequence"] + 1)
            cancel_id = self.new_instruction("cancelflowmeshorder", market, "bid", bid_account["next_sequence"])["action_id"]
            self.wait_until(lambda: self.action(market, cancel_id)[0]["receipt_state"] in {"queued", "admitted"},
                            timeout=30, check_interval=.1)
            with ThreadPoolExecutor(max_workers=1) as pool:
                def waiting_read():
                    try:
                        return self.raw("action", {"market_id": market, "action_id": cancel_id, "wait_ms": WAIT_MAX_MS})
                    except (OSError, http.client.HTTPException) as error:
                        return type(error).__name__
                    finally:
                        ended.append(time.monotonic())
                ended = []
                pending = pool.submit(waiting_read)
                time.sleep(.3)
                stop_started = time.monotonic()
                self.stop_node(0)
                outcome = pending.result()
            interrupted_ms = (ended[0] - stop_started) * 1000
            self.wait_report["shutdown"] = {"outcome": outcome if isinstance(outcome, str) else outcome[1]["result"].get("wait_status"),
                                            "read_ended_after_stop_ms": round(interrupted_ms, 3)}
            # The wait still had about 1.7 s left when the stop began.
            assert interrupted_ms < 1200, self.wait_report["shutdown"]
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
