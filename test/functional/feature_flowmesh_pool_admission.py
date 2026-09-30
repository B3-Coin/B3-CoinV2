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

Then the read-rate gate (design I2): the client is driven like the Qt
trading panel for 30 s in each of its two modes (the 500 ms market poll,
and the live-update watch loop with its 5 s auxiliary refresh), with its
own order and cancel every few seconds (waited submissions and waited
status reads). Every request it sends and every extra admission token its
opt-in replies cost are counted at validator 0's identity; the peak over
any one second must stay at or below 24, under the 28 read cap, and no
request may meet a 429.

The budgets use fixed one-second windows; a burst that does not fit in one
window is repeated rather than judged. Regtest only, no outside peers.
"""

import http.client
import json
import ssl
import threading
import time
from pathlib import Path

from feature_flowmesh_action_outcome import FlowMeshActionOutcomeTest
from feature_flowmesh_independent import RECONCILING_REFUSAL
from feature_flowmesh_latency import PRE_ADMISSION_REJECTIONS
from feature_flowmesh_release import TRADE_PRICE
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, get_rpc_proxy

BUDGET_BODY = b'{"ok":false,"error":"Public trading request budget exhausted before admission"}'
ORDER_BUDGET = "Public trading order budget exhausted; cancels and withdrawals are still accepted"
FAR_FUTURE = "more than 64 ahead"
WATCH_MAX_MS = 2000
QT_WATCH_MS = 1500           # B3FlowMeshWatch::Policy::WATCH_MS
QT_TICK_S = .5               # the panel's 500 ms timer
QT_AUX_REFRESH_S = 5         # catalog/status/effects/receipt cadence while live
QT_RECEIPT_WAIT_MS = 2000    # POST_SUBMIT_RECEIPT_WAIT_MS
READ_RATE_GATE = 24          # design I2: peak per identity, below the 28 read cap
GATE_SECONDS = 30


def extra_tokens(method, body):
    """Admission tokens a reply cost beyond its own request (TradingApi
    charges one per opt-in evidence part it added or tried to add, one for a
    waited submission's embedded read; a refused charge says BUDGET), and
    the soft budget refusals it shows."""
    try:
        reply = json.loads(body)
    except ValueError:
        return 0, 0
    if method == "outcome_evidence":
        refused = reply.get("error") == "Public trading request budget exhausted before outcome evidence"
        return (0, 1) if refused else (1, 0)
    result = reply.get("result") if reply.get("ok") else None
    if not isinstance(result, dict):
        return 0, 0
    charged = refused = 0

    def parts(value):
        nonlocal charged, refused
        for present, error in (("outcome_evidence", "outcome_evidence_error"),
                               ("consumer_certified_payload", "consumer_evidence_error")):
            if value.get(error) == "BUDGET":
                refused += 1
            elif present in value or error in value:
                charged += 1
    if method == "action":
        parts(result)
    elif method == "submit" and "wait_status" in result:
        if result["wait_status"] == "budget":
            refused += 1
        elif isinstance(result.get("action"), dict):
            charged += 1
            parts(result["action"])
    return charged, refused


class FlowMeshPoolAdmissionTest(FlowMeshActionOutcomeTest):
    def set_test_params(self):
        super().set_test_params()
        # The live-update watch the read-rate gate's live mode uses.
        self.extra_args[0].append(f"-flowmeshapiwatch={WATCH_MAX_MS}")
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
            # 12. Read-rate gate, in both of the panel's modes.
            self.admission_report["read_rate_gate"] = {}
            for live in (False, True):
                self.read_rate_gate(market, live)
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


    def qt_like_client(self, market, live, stop, errors, counts):
        """Threads that use the client's RPC like the Qt trading panel: its
        passive refresh (client info, saved actions, one auxiliary phase per
        tick, the market read every 500 ms unless live), its live-update loop
        (at most two watch reads per second) and a trader placing and
        cancelling one resting bid with waited submissions and waited
        receipt reads, as the panel's order ticket does."""
        def rpc():
            return get_rpc_proxy(self.client.url, 4, timeout=60, coveragedir=self.options.coveragedir)

        def call(proxy, name, method, *params, **named):
            try:
                result = getattr(proxy, method)(*params, **named)
                counts[name] = counts.get(name, 0) + 1
                return result
            except JSONRPCException as error:
                errors.append(f"{name} {method}: {error.error.get('message')}")
                stop.wait(QT_TICK_S)  # the panel retries on its next tick
                return None

        tracked = {"id": None}
        submissions = counts.setdefault("submissions", [])

        def refresh():
            proxy, phases, tick, head, next_aux = rpc(), ("catalog", "status", "effects", "receipt"), 0, None, 0
            while not stop.is_set():
                started = time.monotonic()
                call(proxy, "refresh", "getflowmeshclientinfo")
                call(proxy, "refresh", "listflowmeshactions")
                if not live or started >= next_aux:
                    phase = phases[tick % len(phases)]
                    tick += 1
                    next_aux = started + QT_AUX_REFRESH_S
                    if phase == "catalog":
                        call(proxy, "refresh", "listflowmeshmarkets")
                    elif phase == "status":
                        call(proxy, "refresh", "getflowmeshbalance", market)
                    elif phase == "effects":
                        call(proxy, "refresh", "listflowmeshvaultoperations", market)
                    elif tracked["id"] is not None:
                        call(proxy, "refresh", "getflowmeshactionstatus", market, tracked["id"])
                if not live:
                    options = {"limit": 100, "curve_limit": 128}
                    if head is not None:
                        options["known_head"] = head
                    data = call(proxy, "refresh", "getflowmeshmarketdata", market, options)
                    if data is not None:
                        head = data["snapshot"]["last_microblock_hash"]
                stop.wait(max(0, QT_TICK_S - (time.monotonic() - started)))

        def watch():
            proxy, head = rpc(), None
            while not stop.is_set():
                started = time.monotonic()
                options = {"limit": 100, "curve_limit": 128, "watch_ms": QT_WATCH_MS}
                if head is not None:
                    options["known_head"] = head
                data = call(proxy, "watch", "getflowmeshmarketdata", market, options)
                if data is not None:
                    head = data["snapshot"]["last_microblock_hash"]
                stop.wait(max(0, QT_TICK_S - (time.monotonic() - started)))

        def signed(proxy, method, *params):
            """One new instruction; only its exact bytes are resent while the
            endpoint's reconciliation gate refuses it before admission."""
            deadline = time.monotonic() + 20
            while not stop.is_set():
                try:
                    receipt = getattr(proxy, method)(*params, wait_ms=QT_RECEIPT_WAIT_MS)
                    break
                except JSONRPCException as error:
                    if error.error.get("message") not in PRE_ADMISSION_REJECTIONS or time.monotonic() > deadline:
                        errors.append(f"trade {method}: {error.error.get('message')}")
                        return None
                    stop.wait(.2)
            else:
                return None
            counts["trade"] = counts.get("trade", 0) + 1
            first = {key: receipt.get(key) for key in ("receipt_state", "reason", "certificate_verified", "stage")}
            while receipt["reason"] == RECONCILING_REFUSAL and time.monotonic() < deadline and not stop.is_set():
                stop.wait(.2)
                receipt = call(proxy, "trade", "retryflowmeshaction", market, receipt["action_id"]) or receipt
            tracked["id"] = receipt["action_id"]
            while not receipt.get("certificate_verified") and time.monotonic() < deadline and not stop.is_set():
                read_started = time.monotonic()
                status = call(proxy, "trade", "getflowmeshactionstatus", market, receipt["action_id"], QT_RECEIPT_WAIT_MS)
                if status is None:
                    break
                receipt = status
                # A read that returned at once is paced like the panel's tick.
                stop.wait(max(0, QT_TICK_S - (time.monotonic() - read_started)))
            submissions.append({"method": method, "first": first, "final_state": receipt.get("receipt_state"),
                                "certificate_verified": receipt.get("certificate_verified"),
                                "final_reason": receipt.get("reason")})
            return receipt if receipt.get("certificate_verified") else None

        def trade():
            proxy = rpc()
            while not stop.is_set():
                started = time.monotonic()
                if signed(proxy, "submitflowmeshorder", market, "bid", TRADE_PRICE // 2, 1) is not None:
                    signed(proxy, "cancelflowmeshorder", market, "bid")
                stop.wait(max(0, 3 - (time.monotonic() - started)))

        threads = [threading.Thread(target=refresh), threading.Thread(target=trade)]
        if live:
            threads.append(threading.Thread(target=watch))
        return threads

    def read_rate_gate(self, market, live):
        relay = self.tls_relays[0]
        observed = []  # (host_us, extra tokens charged, budget refusals)
        lock = threading.Lock()

        def observe(method, body):
            charged, refused = extra_tokens(method, body)
            if charged or refused:
                with lock:
                    observed.append((time.monotonic_ns() // 1000, charged, refused))
            return body
        # Every earlier window of this identity (the harness's bursts) ends.
        time.sleep(1.5)
        relay.configure(reply_mutation=observe)
        first_row = len(relay.snapshot()["requests"])
        stop, errors, counts = threading.Event(), [], {}
        threads = self.qt_like_client(market, live, stop, errors, counts)
        started_us = time.monotonic_ns() // 1000
        for thread in threads:
            thread.start()
        try:
            deadline = time.monotonic() + GATE_SECONDS
            while time.monotonic() < deadline:
                self.pump_b3()  # mock time stays on the harness thread
                time.sleep(.05)
        finally:
            stop.set()
            for thread in threads:
                thread.join(timeout=30)
            relay.configure()
        assert not any(thread.is_alive() for thread in threads)
        ended_us = time.monotonic_ns() // 1000
        rows = [row for row in relay.snapshot()["requests"][first_row:] if row.get("forwarded")]
        tokens = sorted([(row["upstream_started_us"], 1) for row in rows] +
                        [(at, charged) for at, charged, _ in observed if charged])
        peak, start = 0, 0
        total = 0
        for at, cost in tokens:
            total += cost
            while tokens[start][0] <= at - 1_000_000:
                total -= tokens[start][1]
                start += 1
            peak = max(peak, total)
        by_method = {}
        for row in rows:
            by_method[row["method"]] = by_method.get(row["method"], 0) + 1
        refused_429 = sum(row.get("upstream_http_status") == 429 for row in rows)
        refused_by_method = {}
        for row in rows:
            if row.get("upstream_http_status") == 429:
                refused_by_method[row["method"]] = refused_by_method.get(row["method"], 0) + 1
        soft_refusals = sum(refused for _, _, refused in observed)
        seconds = (ended_us - started_us) / 1e6
        report = {"mode": "live watch" if live else "500 ms poll", "seconds": round(seconds, 3),
                  "requests": len(rows), "extra_tokens": sum(charged for _, charged, _ in observed),
                  "mean_tokens_per_second": round((len(rows) + sum(c for _, c, _ in observed)) / seconds, 3),
                  "peak_tokens_in_one_second": peak, "gate": READ_RATE_GATE, "requests_by_method": by_method,
                  "http_429": refused_429, "http_429_by_method": refused_by_method,
                  "budget_refused_parts": soft_refusals, "client_rpc_calls": counts,
                  "client_rpc_errors": errors[:20], "client_rpc_error_count": len(errors)}
        self.admission_report["read_rate_gate"]["live" if live else "poll"] = report
        self.log.info("read-rate gate (%s): %s", report["mode"], json.dumps(report, sort_keys=True))
        assert any(row["certificate_verified"] for row in counts["submissions"]), report
        assert_equal(refused_429, 0)
        assert_equal(soft_refusals, 0)
        assert peak <= READ_RATE_GATE, report


if __name__ == "__main__":
    FlowMeshPoolAdmissionTest(__file__).main()
