#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Live-update reads (getflowmeshmarketdata watch_ms) of the engine-off client.

Reuse only the latency fixture's generated four-validator bootstrap, funding
and engine-off client, whose two TLS fault relays forward to validator 0
(-flowmeshapiwatch=2000) and validator 1 (watches off, the previous
contract). Checked, through the relays' recorded public requests: the
client shows each endpoint's advertised live_updates_ms; a watch at the
watching endpoint returns verified data and a supported, waited watch; a
concurrent order is not delayed by a sleeping watch; a lost watch reply
changes no retained instruction and sends nothing; an endpoint without the
advertisement never receives watch_ms and is read at once; a stale
advertisement is withdrawn with an ordinary read in the same call; and with
300 ms of added reply delay a paced watch loop keeps replies inside the 3 s
display freshness gate on a quiet market for 60 s. This is not a latency
benchmark.
"""

import json
import threading
import time
from pathlib import Path

from feature_flowmesh_independent import RECONCILING_REFUSAL
from feature_flowmesh_latency import FlowMeshLatencyTest, PRE_ADMISSION_REJECTIONS
from feature_flowmesh_release import TRADE_PRICE
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, assert_raises_rpc_error, get_rpc_proxy

WATCH_MAX_MS = 2000
CLIENT_WATCH_MS = 1500
WATCH_KEYS = {"status", "supported", "waited", "gap", "endpoint", "endpoint_live_ms"}
FRESHNESS_GATE_MS = 3000


def watch_bodies(relay):
    """watch_ms of every recorded 'updates' request that carried one."""
    out = []
    for row in relay.snapshot()["requests"]:
        if row["method"] == "updates" and "body_hex" in row:
            params = json.loads(bytes.fromhex(row["body_hex"]))["params"]
            if "watch_ms" in params:
                out.append(params["watch_ms"])
    return out


def inject_advert(method, body):
    """A stale capability: every status object advertises watches."""
    reply = json.loads(body)
    result = reply.get("result") if reply.get("ok") else None
    rows = []
    if method == "markets" and isinstance(result, list):
        rows = result
    elif method in {"snapshot", "updates"} and isinstance(result, dict) and isinstance(result.get("status"), dict):
        rows = [result["status"]]
    for row in rows:
        row["updates_watch_ms_max"] = WATCH_MAX_MS
    return json.dumps(reply, separators=(",", ":")).encode()


class FlowMeshClientWatchTest(FlowMeshLatencyTest):
    def set_test_params(self):
        self.options.latency_production_logging = True
        super().set_test_params()
        self.extra_args[0].append(f"-flowmeshapiwatch={WATCH_MAX_MS}")
        self.watch_report = {"fixture": "generated regtest engine-off client live updates",
                             "scope": "correctness only; no latency claim", "correctness_pass": False}

    def watch(self, market, rpc=None, **options):
        rpc = rpc or self.client
        started = time.monotonic()
        data = rpc.getflowmeshmarketdata(market, {"limit": 100, "curve_limit": 128, "watch_ms": CLIENT_WATCH_MS, **options})
        elapsed = (time.monotonic() - started) * 1000
        assert_equal(set(data["watch"]), WATCH_KEYS)
        assert_equal(data["verification"]["certificate_verified"], True)
        assert_equal(data["verification"]["account_state_verified"], True)
        return data, elapsed

    def new_instruction(self, method, *params):
        deadline = time.monotonic() + 30
        while True:
            self.pump_b3()
            try:
                response = getattr(self.client, method)(*params)
                break
            except JSONRPCException as error:
                if error.error.get("code") != -1 or error.error.get("message") not in PRE_ADMISSION_REJECTIONS:
                    raise
                assert time.monotonic() < deadline, "pre-admission gate did not reopen"
                time.sleep(.05)
        action_id = response["action_id"]
        while response["receipt_state"] not in {"queued", "admitted", "certified_inclusion"}:
            assert response["receipt_state"] in {"rejected", "unknown"}, response
            assert_equal(response["reason"], RECONCILING_REFUSAL)
            assert time.monotonic() < deadline, "reconciliation gate did not reopen"
            time.sleep(.05)
            self.pump_b3()
            response = self.client.retryflowmeshaction(params[0], action_id)
        return response

    def endpoints(self):
        return {row["url"]: row for row in self.client.getflowmeshclientinfo()["endpoints"]}

    def read_until_endpoint(self, market, relay):
        """Ordinary reads until the cached state was served through `relay`."""
        def served():
            self.pump_b3()
            data = self.client.getflowmeshmarketdata(market, {"limit": 1, "curve_limit": 1})
            return data["verification"]["endpoint"] == relay.url
        self.wait_until(served, timeout=60, check_interval=.1)

    def check_options(self, market):
        for bad, code, message in ((-1, -3, "watch_ms must be a nonnegative integer"), (1.5, -3, "watch_ms must be a nonnegative integer"),
                                   ("500", -3, "watch_ms must be a nonnegative integer"),
                                   (2001, -8, "watch_ms is outside its supported range")):
            assert_raises_rpc_error(code, message, self.client.getflowmeshmarketdata, market, {"watch_ms": bad})
        assert_raises_rpc_error(-8, "watch_ms cannot be combined with before_sequence, curve_cursor or expected_head",
                                self.client.getflowmeshmarketdata, market, {"watch_ms": 500, "before_sequence": 1})
        # Without watch_ms the reply has no watch object.
        assert "watch" not in self.client.getflowmeshmarketdata(market, {"limit": 1, "curve_limit": 1})

    def run_test(self):
        try:
            market, asset = self.bootstrap_latency_market()
            account = self.fund_latency_client(market, asset)
            relay0, relay1 = self.tls_relays
            self.watch_report["market_id"] = market
            self.begin_b3_workload(range(4))
            self.check_options(market)

            # The watching endpoint is advertised in getflowmeshclientinfo.
            self.read_until_endpoint(market, relay0)
            assert_equal(self.endpoints()[relay0.url]["live_updates_ms"], WATCH_MAX_MS)

            # A watch there: verified data, and a supported watch that waited.
            data, elapsed = self.watch(market, known_head=self.client.getflowmeshmarketdata(market, {"limit": 1})["snapshot"]["last_microblock_hash"])
            assert_equal((data["watch"]["supported"], data["watch"]["waited"]), (True, True))
            assert data["watch"]["status"] in {"timeout", "ready", "head_changed"}, data["watch"]
            assert_equal((data["watch"]["endpoint"], data["watch"]["endpoint_live_ms"]), (relay0.url, WATCH_MAX_MS))
            assert elapsed < CLIENT_WATCH_MS + 1500, elapsed
            assert watch_bodies(relay0) and max(watch_bodies(relay0)) <= CLIENT_WATCH_MS
            self.watch_report["first_watch"] = {"status": data["watch"]["status"], "elapsed_ms": round(elapsed, 3)}

            # A sleeping watch does not delay an order: it holds its own lane,
            # not the client's work gate.
            watcher_rpc = get_rpc_proxy(self.client.url, 4, timeout=30, coveragedir=self.options.coveragedir)
            result = {}
            def sleeping_watch():
                result["data"], result["elapsed"] = self.watch(market, rpc=watcher_rpc)
            thread = threading.Thread(target=sleeping_watch)
            thread.start()
            time.sleep(.3)
            started = time.monotonic()
            order = self.new_instruction("submitflowmeshorder", market, "bid", TRADE_PRICE // 2, 1, account["next_sequence"])
            submit_ms = (time.monotonic() - started) * 1000
            thread.join(timeout=30)
            assert not thread.is_alive()
            assert submit_ms < 1000, submit_ms
            self.watch_report["order_during_watch"] = {"submit_ms": round(submit_ms, 3), "watch": result["data"]["watch"],
                                                       "watch_elapsed_ms": round(result["elapsed"], 3)}
            self.wait_until(lambda: self.client.getflowmeshactionstatus(market, order["action_id"])["certificate_verified"] or
                            (self.pump_b3() and False), timeout=60, check_interval=.1)

            # A lost watch reply: the call fails, nothing is sent or resent,
            # and no retained instruction changes.
            self.read_until_endpoint(market, relay0)
            saved = self.client.listflowmeshactions()
            submits = sum(row["method"] == "submit" for relay in self.tls_relays for row in relay.snapshot()["requests"])
            relay0.configure(response_hold_ms={"updates": 6000})
            try:
                started = time.monotonic()
                assert_raises_rpc_error(-1, None, self.client.getflowmeshmarketdata, market,
                                        {"limit": 100, "curve_limit": 128, "watch_ms": 500})
                lost_ms = (time.monotonic() - started) * 1000
            finally:
                relay0.configure()
            assert_equal(self.client.listflowmeshactions(), saved)
            assert_equal(sum(row["method"] == "submit" for relay in self.tls_relays for row in relay.snapshot()["requests"]), submits)
            # No transport cooldown: the endpoint is still read at once.
            assert_equal(self.endpoints()[relay0.url]["retry_after_ms"], 0)
            self.watch_report["lost_watch_reply"] = {"elapsed_ms": round(lost_ms, 3)}

            # An endpoint without the advertisement never receives watch_ms and
            # is read at once.
            relay0.configure(unavailable=True)
            try:
                self.read_until_endpoint(market, relay1)
                before = len(watch_bodies(relay1))
                data, elapsed = self.watch(market)
                assert_equal((data["watch"]["status"], data["watch"]["supported"], data["watch"]["waited"]),
                             ("not_eligible", False, False))
                assert_equal(data["watch"]["endpoint"], relay1.url)
                assert elapsed < 1000, elapsed
                assert_equal(len(watch_bodies(relay1)), before)
                assert_equal(self.endpoints()[relay1.url]["live_updates_ms"], 0)

                # A stale advertisement relayed from it is withdrawn, and the
                # same call reads ordinarily.
                relay1.configure(reply_mutation=inject_advert)
                self.read_until_endpoint(market, relay1)
                assert_equal(self.endpoints()[relay1.url]["live_updates_ms"], WATCH_MAX_MS)
                data, elapsed = self.watch(market)
                assert_equal((data["watch"]["status"], data["watch"]["supported"]), ("unsupported", False))
                assert_equal(len(watch_bodies(relay1)), before + 1)
                self.watch_report["stale_advertisement"] = {"elapsed_ms": round(elapsed, 3)}
            finally:
                relay1.configure()
                relay0.configure()

            # With 300 ms added to every reply, a paced watch loop (watch again
            # at once only after a waited reply) keeps a quiet market's
            # replies inside the display freshness gate for 60 s.
            self.read_until_endpoint(market, relay0)
            relay0.configure(response_hold_ms={"updates": 300, "snapshot": 300})
            try:
                last, gaps, statuses = time.monotonic(), [], {}
                head = self.client.getflowmeshmarketdata(market, {"limit": 1})["snapshot"]["last_microblock_hash"]
                deadline = last + 60
                while time.monotonic() < deadline:
                    self.pump_b3()
                    try:
                        data, _ = self.watch(market, known_head=head)
                    except JSONRPCException:
                        statuses["error"] = statuses.get("error", 0) + 1
                        time.sleep(.5)
                        continue
                    now = time.monotonic()
                    gaps.append((now - last) * 1000)
                    last = now
                    head = data["snapshot"]["last_microblock_hash"]
                    statuses[data["watch"]["status"]] = statuses.get(data["watch"]["status"], 0) + 1
                    if not data["watch"]["waited"]:
                        time.sleep(.5)
            finally:
                relay0.configure()
            self.watch_report["freshness_300ms"] = {"replies": len(gaps), "max_gap_ms": round(max(gaps), 3),
                                                    "statuses": statuses}
            assert len(gaps) > 20, self.watch_report["freshness_300ms"]
            assert max(gaps) < FRESHNESS_GATE_MS, self.watch_report["freshness_300ms"]
            self.watch_report["correctness_pass"] = True
        except Exception as error:
            self.watch_report["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            try:
                self.end_b3_workload()
            finally:
                path = Path(self.options.tmpdir, "flowmesh-client-watch.json")
                path.write_text(json.dumps(self.watch_report, sort_keys=True, indent=2, default=str) + "\n", encoding="utf-8")
                self.log.info("FLOWMESH_CLIENT_WATCH_REPORT %s", path)
                self.log.info("FLOWMESH_CLIENT_WATCH_SUMMARY %s", json.dumps({key: value for key, value in self.watch_report.items()
                                                                              if key != "market_id"}, default=str))


if __name__ == "__main__":
    FlowMeshClientWatchTest(__file__).main()
