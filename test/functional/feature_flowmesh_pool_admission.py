#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Public trading API admission budgets on a generated regtest validator.

Reuses the outcome test's fixture (four fresh validators, one engine-off
remote client). The client signs one far-future bid and one far-future cancel;
validator 0's local admission refuses both (never queued, never certified),
so their exact bytes can be resubmitted freely. With the client idle, raw
HTTPS bursts from this harness (one loopback identity) then show:

- reads: 28 per identity per second are admitted, the rest get the exact
  previous 429 body, and a trade write in the same second is still admitted;
- orders: 12 order submissions per identity per second pass the order
  sub-budget, the 13th gets its error, and a cancel in the same second is
  still admitted.

The budgets use fixed one-second windows; a burst that does not fit in one
window is repeated rather than judged. Regtest only, no outside peers.
"""

import http.client
import json
import ssl
import time
from pathlib import Path

from feature_flowmesh_action_outcome import FlowMeshActionOutcomeTest
from feature_flowmesh_release import TRADE_PRICE
from test_framework.util import assert_equal

BUDGET_BODY = b'{"ok":false,"error":"Public trading request budget exhausted before admission"}'
ORDER_BUDGET = "Public trading order budget exhausted; cancels and withdrawals are still accepted"
FAR_FUTURE = "more than 64 ahead"


class FlowMeshPoolAdmissionTest(FlowMeshActionOutcomeTest):
    def set_test_params(self):
        super().set_test_params()
        self.admission_report = {"fixture": "generated regtest public API admission",
                                 "scope": "correctness only; no latency claim", "correctness_pass": False}

    def burst(self, bodies, index=0):
        """Sends `bodies` back to back from one identity, starting just after
        a fresh one-second window can begin. Returns [(status, body)] and the
        seconds from the first request to the last reply."""
        context = ssl.create_default_context(cafile=str(self.pki["ca"]))
        connection = http.client.HTTPSConnection("127.0.0.1", self.api_ports[index], timeout=15, context=context)
        connection.connect()
        replies = []
        try:
            started = time.monotonic()
            for body in bodies:
                connection.request("POST", "/flowmesh/v1", body=body, headers={"Content-Type": "application/json"})
                reply = connection.getresponse()
                replies.append((reply.status, reply.read(32 * 1024 * 1024 + 1)))
            return replies, time.monotonic() - started
        finally:
            connection.close()

    def fitted_burst(self, bodies):
        for _ in range(5):
            time.sleep(1.2)  # every earlier window of this identity has ended
            replies, elapsed = self.burst(bodies)
            if elapsed < .8:
                return replies, elapsed
            self.admission_report.setdefault("repeated_slow_bursts", []).append(round(elapsed, 3))
        raise AssertionError("no burst fitted in one budget window")

    @staticmethod
    def body(method, params):
        # Compact, "method" first: exactly how in-tree clients write it.
        return json.dumps({"method": method, "params": params}, separators=(",", ":")).encode()

    def run_test(self):
        try:
            market, asset = self.bootstrap_latency_market()
            account = self.fund_latency_client(market, asset)
            self.admission_report["market_id"] = market
            self.begin_b3_workload(range(4))
            self.tls_relays[1].configure(unavailable=True)
            first = account["next_sequence"]
            bid = self.refused_instruction(FAR_FUTURE, "submitflowmeshorder", market, "bid", TRADE_PRICE // 2, 1, first + 65)
            cancel = self.refused_instruction(FAR_FUTURE, "cancelflowmeshorder", market, "bid", first + 66)
            bid_hex = self.submits_for(bid["action_id"])[0]["action_hex"]
            cancel_hex = self.submits_for(cancel["action_id"])[0]["action_hex"]
            submit_bid = self.body("submit", {"market_id": market, "action_id": bid["action_id"], "action_hex": bid_hex})
            submit_cancel = self.body("submit", {"market_id": market, "action_id": cancel["action_id"],
                                                 "action_hex": cancel_hex})
            # From here the client is idle: only this harness uses the identity.

            # 10. Read budget: 28 per identity; a refusal is the previous 429
            # body and charges nothing, so a write in the same second passes.
            replies, elapsed = self.fitted_burst([self.body("markets", {})] * 30 + [submit_cancel])
            statuses = [status for status, _ in replies]
            assert_equal(statuses[:28], [200] * 28)
            assert_equal(statuses[28:30], [429, 429])
            for _, data in replies[28:30]:
                assert_equal(data, BUDGET_BODY)
            status, data = replies[30]
            assert_equal(status, 200)
            write = json.loads(data)["result"]
            assert_equal(write["action_id"], cancel["action_id"])
            assert FAR_FUTURE in write["reason"], write
            self.admission_report["read_budget"] = {"admitted_reads": 28, "refused_reads": 2,
                                                    "same_second_write": "admitted", "burst_seconds": round(elapsed, 3)}

            # 11. Order sub-budget: 12 orders per identity; the 13th gets the
            # order-budget error while a cancel in the same second passes.
            replies, elapsed = self.fitted_burst([submit_bid] * 13 + [submit_cancel])
            for status, data in replies[:12]:
                assert_equal(status, 200)
                assert FAR_FUTURE in json.loads(data)["result"]["reason"], data
            status, data = replies[12]
            assert_equal(status, 400)
            assert_equal(json.loads(data), {"ok": False, "error": ORDER_BUDGET})
            status, data = replies[13]
            assert_equal(status, 200)
            assert FAR_FUTURE in json.loads(data)["result"]["reason"], data
            self.admission_report["order_budget"] = {"admitted_orders": 12, "refused_orders": 1,
                                                     "same_second_cancel": "admitted", "burst_seconds": round(elapsed, 3)}

            # Nothing above was ever queued: a refusal from certified state
            # appends no v1 event, only a local notice naming its reason.
            for action_id in (bid["action_id"], cancel["action_id"]):
                result = self.ok("action", {"market_id": market, "action_id": action_id, "outcome": True})
                assert_equal(result["receipt_state"], "unknown")
                assert_equal(result["pool_notice"]["reason_code"], "FAR_FUTURE_SEQUENCE")
            self.admission_report["correctness_pass"] = True
        except Exception as error:
            self.admission_report["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            try:
                for relay in self.tls_relays:
                    relay.configure()
                self.end_b3_workload()
            finally:
                path = Path(self.options.tmpdir, "flowmesh-pool-admission.json")
                path.write_text(json.dumps(self.admission_report, sort_keys=True, indent=2, default=str) + "\n",
                                encoding="utf-8")
                self.log.info("FLOWMESH_POOL_ADMISSION_REPORT %s", path)


if __name__ == "__main__":
    FlowMeshPoolAdmissionTest(__file__).main()
