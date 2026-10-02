#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Operator-observed time on the FlowMesh market-data paths (display only).

Reuses the liquidity fixture: four generated regtest validators and the
engine-off client, whose TLS relays forward to validator 0 (observed time and
candles served, the defaults) and validator 1 (-flowmeshapiobservedtime=0
-flowmeshapicandles=0, the previous public contract). Checked:

- each validator records when it applied every certified entry; its own RPC
  returns the stamps of the head, history and ticks (never certified), and
  getflowmeshcandles aggregates exactly what the rollup rules place;
- the RPC options are strict, and a window past 10000-01-01 is clamped;
- through validator 0's endpoint the client labels the times with that
  endpoint and reads its candles, labelled endpoint-reported, also when an
  auction is certified between its market read and its candles read (the
  endpoint's forming candle then ends past the client's verified head, and
  the page is held back to that head until a market-data read moves it);
- validator 1's endpoint never receives observed_time or 'candles', and the
  client says why there are none;
- validator 3 stopped while the others trade and started again: the times it
  recorded before persist unchanged, and the entries it then catches up are
  recorded late, never live;
- the share of late entries in steady trading (the gap rate) is reported.

Nothing here is a latency claim.
"""

import json

from feature_flowmesh_latency import PRE_ADMISSION_REJECTIONS
from feature_flowmesh_liquidity import FlowMeshLiquidityTest
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, assert_raises_rpc_error

PRICE = 40_000_000
HOUR_MS = 3_600_000
# 10000-01-01T00:00:00Z: every observed time is before it.
OBSERVED_MS_LIMIT = 253_402_300_800_000


def observed_bodies(relay):
    """(method, params) of every recorded request naming observed time or candles."""
    out = []
    for row in relay.snapshot()["requests"]:
        if "body_hex" in row:
            request = json.loads(bytes.fromhex(row["body_hex"]))
            if request.get("method") == "candles" or "observed_time" in request.get("params", {}):
                out.append((request.get("method"), request.get("params")))
    return out


def expected_candles(rows, ticks, interval_ms):
    """The node's rollup rules applied to complete rows and ticks: a live
    entry goes into its bucket; a delayed one only when its bound (the last
    live time of the contiguous run before it, to when it was applied) lies
    inside one bucket (flag "late"); nothing else is placed."""
    by_sequence = {tick["sequence"]: tick for tick in ticks}
    candles = {}
    last_sequence = None
    last_live = None
    for row in rows:
        if last_sequence is None or row["sequence"] != last_sequence + 1:
            last_live = None
        last_sequence = row["sequence"]
        place, late = None, False
        if row["mode"] == "live":
            place = row["observed_ms"] - row["observed_ms"] % interval_ms
        elif row["mode"] == "delayed" and last_live is not None and \
                last_live - last_live % interval_ms == row["applied_here_ms"] - row["applied_here_ms"] % interval_ms:
            place, late = row["applied_here_ms"] - row["applied_here_ms"] % interval_ms, True
        if row["mode"] == "live":
            last_live = row["observed_ms"]
        if place is None:
            continue
        candle = candles.setdefault(place, {"open_ms": place, "first_sequence": row["sequence"], "entries": 0,
                                            "clearings": 0, "base_volume": 0, "prices": [], "late": False})
        candle["last_sequence"] = row["sequence"]
        candle["entries"] += 1
        candle["late"] |= late
        tick = by_sequence[row["sequence"]]
        if tick["cleared"]:
            candle["clearings"] += 1
            candle["base_volume"] += tick["volume"]
            candle["prices"].append(tick["price"])
    return [candles[key] for key in sorted(candles)]


class FlowMeshObservedTimeTest(FlowMeshLiquidityTest):
    def set_test_params(self):
        super().set_test_params()
        self.extra_args[1] += ["-flowmeshapiobservedtime=0", "-flowmeshapicandles=0"]

    def read_until_endpoint(self, market, relay):
        def served():
            self.pump_b3()
            data = self.client.getflowmeshmarketdata(market, {"limit": 1, "curve_limit": 1})
            return data["verification"]["endpoint"] == relay.url
        self.wait_until(served, timeout=60, check_interval=.1)

    def operator_order(self, market, side, price, quantity):
        """Validator 0's own order, waiting only for validator 0 (another
        validator may be offline)."""
        n0 = self.nodes[0]
        sequence = self.market_data(n0, market)["account"]["next_sequence"]

        def submitted():
            try:
                n0.submitflowmeshorder(market, side, price, quantity, sequence)
                return True
            except JSONRPCException as error:
                if error.error.get("code") != -1 or error.error.get("message") not in PRE_ADMISSION_REJECTIONS:
                    raise
                return False
        self.wait_until(submitted, timeout=30, check_interval=.1)
        self.wait_until(lambda: self.market_data(n0, market)["account"]["next_sequence"] == sequence + 1,
                        timeout=90, check_interval=.1)

    def timed(self, node, market):
        return node.getflowmeshmarketdata(market, {"limit": 100, "ticks": {"limit": 256}, "observed_time": True})

    def check_options(self, market):
        n0 = self.nodes[0]
        assert_raises_rpc_error(-3, "observed_time must be a boolean", n0.getflowmeshmarketdata, market, {"observed_time": 1})
        for options, code, message in (
            ({"interval": "2m"}, -8, "interval must be 1m, 5m, 15m, 1h, 4h or 1d"),
            ({"interval": 60}, -3, "interval must be a string"),
            ({"limit": 0}, -8, "limit is outside its supported range"),
            ({"limit": 501}, -8, "limit is outside its supported range"),
            ({"before_ms": -1}, -3, "before_ms must be a nonnegative integer"),
            ({"before_ms": 2**63}, -8, "before_ms is outside its supported range"),
            ({"from_ms": 1}, -8, "Unknown option 'from_ms'"),
        ):
            assert_raises_rpc_error(code, message, n0.getflowmeshcandles, market, options)
        # A window past 10000-01-01 (the display bound), up to INT64_MAX, is
        # clamped: no observed time is at or past it. Above INT64_MAX it is
        # refused as out of range (above).
        page = n0.getflowmeshcandles(market, {"interval": "1m", "before_ms": 2**63 - 1})
        assert_equal((page["available"], page["before_ms"], page["from_ms"], page["candles"]),
                     (True, OBSERVED_MS_LIMIT, OBSERVED_MS_LIMIT - 200 * 60_000, []))
        # Without the option every reply is as before.
        assert "observed_time" not in n0.getflowmeshmarketdata(market, {"limit": 1, "ticks": {}})

    def check_local(self, market, label):
        """Validator 0's own times: structure, and candles that aggregate exactly."""
        data = self.timed(self.nodes[0], market)
        section = data["observed_time"]
        assert_equal((section["available"], section["source"], section["source_kind"], section["certified"]),
                     (True, "this_node", "operator_observed", False))
        # This node's storage detail (operator only): nothing was lost.
        assert_equal(section["storage"], {"persisted": True, "rows_not_persisted": 0, "candles_not_persisted": 0})
        ticks = data["clearing_ticks"]["ticks"]
        rows = section["rows"]
        by_sequence = {tick["sequence"]: tick for tick in ticks}
        assert rows, section
        head = data["snapshot"]["next_microblock_sequence"] - 1
        assert_equal(rows[-1]["sequence"], head)
        assert_equal(rows[-1]["microblock_hash"], data["snapshot"]["last_microblock_hash"])
        for row in rows:
            assert_equal(row["microblock_hash"], by_sequence[row["sequence"]]["microblock_hash"])
            assert row["mode"] in {"live", "delayed", "untimed"}, row
            if row["mode"] == "live":
                assert "applied_here_ms" not in row and "delay_reason" not in row, row
            if row["mode"] == "delayed":
                assert "observed_ms" not in row and row["delay_reason"], row
        live = [row for row in rows if row["mode"] == "live"]
        assert live, rows
        for earlier, later in zip(live, live[1:]):
            assert earlier["observed_ms"] <= later["observed_ms"], (earlier, later)
        report = {"label": label, "entries": len(rows),
                  "delayed": sum(row["mode"] == "delayed" for row in rows),
                  "untimed": sum(row["mode"] == "untimed" for row in rows),
                  "reasons": sorted({row.get("delay_reason", "") for row in rows} - {""})}
        self.log.info("FLOWMESH_OBSERVED_GAP_RATE %s", json.dumps(report, sort_keys=True))
        # The node's hour candles, against the rules applied to its rows, when
        # the reply holds every entry from the first one recorded.
        if rows[0]["sequence"] == section["recorded_since_sequence"] and \
                [row["sequence"] for row in rows] == list(range(rows[0]["sequence"], head + 1)):
            page = self.nodes[0].getflowmeshcandles(market, {"interval": "1h", "limit": 500})
            assert_equal((page["available"], page["source"], page["certified"], page["endpoint_reported"], page["interval_ms"]),
                         (True, "this_node", False, False, HOUR_MS))
            expected = expected_candles(rows, ticks, HOUR_MS)
            got = page["candles"]
            assert_equal(len(got), len(expected))
            self.log.info("FLOWMESH_OBSERVED_CANDLES_EXACT %s", json.dumps([[c["open_ms"], c["entries"], c["base_volume"]] for c in got]))
            for candle, want in zip(got, expected):
                assert_equal((candle["open_ms"], candle["first_sequence"], candle["last_sequence"], candle["entries"],
                              candle["clearings"], int(candle["base_volume"])),
                             (want["open_ms"], want["first_sequence"], want["last_sequence"], want["entries"],
                              want["clearings"], want["base_volume"]))
                assert_equal("late" in candle["flags"], want["late"])
                if want["prices"]:
                    assert_equal((candle["open"], candle["close"], candle["high"], candle["low"]),
                                 (want["prices"][0], want["prices"][-1], max(want["prices"]), min(want["prices"])))
                    assert_equal(int(candle["quote_volume"]),
                                 sum(by_sequence[s]["price"] * by_sequence[s]["volume"]
                                     for s in range(candle["first_sequence"], candle["last_sequence"] + 1)
                                     if by_sequence[s]["cleared"] and any(r["sequence"] == s and (r["mode"] == "live" or "late" in candle["flags"]) for r in rows)))
                else:
                    assert "open" not in candle
        return data

    def run_test(self):
        market, asset = self.bootstrap_latency_market()
        self.fund_latency_client(market, asset)
        self.fund_seller(market, asset)
        self.assert_engine_off()
        relay0, relay1 = self.tls_relays
        n0 = self.nodes[0]
        self.check_options(market)

        # Steady trading: a clearing, then a second at another price.
        self.order(n0, market, "ask", PRICE, 1, "first_ask")
        self.order(self.client, market, "bid", PRICE, 1, "first_bid")
        self.order(n0, market, "ask", PRICE + 1_000_000, 2, "second_ask")
        self.order(self.client, market, "bid", PRICE + 1_000_000, 2, "second_bid")
        operators, _ = self.capture(market, "traded")
        local = self.check_local(market, "steady")

        # Every validator records its own series; entries it applied while
        # caught up agree on sequence and hash with validator 0's.
        for node in self.nodes[1:]:
            rows = self.timed(node, market)["observed_time"]["rows"]
            by_hash = {row["microblock_hash"]: row["sequence"] for row in local["observed_time"]["rows"]}
            for row in rows:
                if row["microblock_hash"] in by_hash:
                    assert_equal(row["sequence"], by_hash[row["microblock_hash"]])

        # The client through validator 0: the endpoint's times, labelled.
        self.read_until_endpoint(market, relay0)

        def remote_times():
            self.pump_b3()
            data = self.client.getflowmeshmarketdata(market, {"limit": 1, "ticks": {}, "observed_time": True})
            section = data["observed_time"]
            return data if section["available"] and section["rows"] else None
        self.wait_until(lambda: remote_times() is not None, timeout=60, check_interval=.2)
        remote = remote_times()
        assert_equal((remote["observed_time"]["source"], remote["observed_time"]["certified"]), (relay0.url, False))
        # Only a node's own section carries its storage detail.
        assert "storage" not in remote["observed_time"], remote["observed_time"]
        local_rows = {row["sequence"]: row for row in self.timed(n0, market)["observed_time"]["rows"]}
        for row in remote["observed_time"]["rows"]:
            mine = local_rows[row["sequence"]]
            assert_equal((row["microblock_hash"], row["mode"], row.get("observed_ms"), row.get("applied_here_ms")),
                         (mine["microblock_hash"], mine["mode"], mine.get("observed_ms"), mine.get("applied_here_ms")))
            # Public rows carry only clock flags, never a delay reason.
            assert "delay_reason" not in row
            assert set(row["flags"]) <= {"clock_clamped", "clock_step", "clock_suspect", "restart"}, row
        page = self.client.getflowmeshcandles(market, {"interval": "1m"})
        assert_equal((page["available"], page["source"], page["endpoint_reported"], page["certified"]), (True, relay0.url, True, False))
        own = n0.getflowmeshcandles(market, {"interval": "1m", "before_ms": page["before_ms"]})
        assert_equal(page["candles"], own["candles"])

        # An auction certified between the client's market read and its
        # candles read, as on any active market: the endpoint's forming
        # candle ends past the head the client verified. The page is bounded
        # to that head (what goes past it is held back; the client refreshes
        # nothing for a candles page) and reaches the auction once a
        # market-data read has verified it.
        client_next = self.client.getflowmeshmarketdata(market, {"limit": 1})["snapshot"]["next_microblock_sequence"]
        self.operator_order(market, "ask", 4 * PRICE, 1)
        assert n0.getflowmeshmarketdata(market, {"limit": 1})["snapshot"]["next_microblock_sequence"] > client_next
        raced = self.client.getflowmeshcandles(market, {"interval": "1m"})
        assert_equal((raced["available"], raced["source"]), (True, relay0.url))
        assert raced["candles"], raced
        assert all(candle["last_sequence"] < client_next for candle in raced["candles"]), (raced, client_next)
        after_next = self.client.getflowmeshmarketdata(market, {"limit": 1})["snapshot"]["next_microblock_sequence"]
        assert after_next > client_next, (after_next, client_next)
        caught_up = {}

        def candles_caught_up():
            try:
                page = self.client.getflowmeshcandles(market, {"interval": "1m"})
            except JSONRPCException as error:
                # Two pages a second per client at the endpoint.
                if "Public candles request budget exhausted" not in error.error.get("message", ""):
                    raise
                return False
            caught_up["last"] = page["candles"][-1]["last_sequence"] if page["candles"] else None
            return (bool(page["candles"]) and page["candles"][-1]["last_sequence"] >= client_next and
                    all(candle["last_sequence"] < after_next for candle in page["candles"]))
        self.wait_until(candles_caught_up, timeout=30, check_interval=.6)
        self.log.info("FLOWMESH_OBSERVED_CANDLES_HELD client_next=%d raced_last=%d caught_up_last=%d",
                      client_next, raced["candles"][-1]["last_sequence"], caught_up["last"])

        # Through validator 1 (both flags off): never asked, and why.
        relay0.configure(unavailable=True)
        try:
            self.read_until_endpoint(market, relay1)
            data = self.client.getflowmeshmarketdata(market, {"limit": 1, "ticks": {}, "observed_time": True})
            assert_equal(data["verification"]["endpoint"], relay1.url)
            assert_equal((data["observed_time"]["available"], data["observed_time"]["unavailable_reason"]),
                         (False, "endpoint_does_not_advertise_observed_time"))
            candles = self.client.getflowmeshcandles(market, {"interval": "5m"})
            assert_equal((candles["available"], candles["unavailable_reason"]), (False, "endpoint_does_not_advertise_candles"))
            assert_equal(observed_bodies(relay1), [])
        finally:
            relay0.configure()

        # Validator 3 stops while the others trade, then comes back.
        before = {row["sequence"]: row for row in self.timed(self.nodes[3], market)["observed_time"]["rows"]}
        head_before = self.nodes[3].getflowmeshmarketdata(market, {"limit": 1})["snapshot"]["next_microblock_sequence"]
        self.stop_node(3)
        self.operator_order(market, "ask", 3 * PRICE, 1)
        self.operator_order(market, "bid", PRICE // 2, 1)
        target = n0.getflowmeshmarketdata(market, {"limit": 1})["snapshot"]
        assert target["next_microblock_sequence"] >= head_before + 2
        self.start_node(3)
        self.nodes[3].setmocktime(self.mock_time)
        self.connect_nodes(3, 2)
        self.wait_for_independent_mesh()

        def caught_up():
            self.pump_b3()
            snapshot = self.nodes[3].getflowmeshmarketdata(market, {"limit": 1})["snapshot"]
            return snapshot["next_microblock_sequence"] >= target["next_microblock_sequence"]
        self.wait_until(caught_up, timeout=120, check_interval=.2)
        after = {row["sequence"]: row for row in self.timed(self.nodes[3], market)["observed_time"]["rows"]}
        for sequence, row in before.items():
            assert_equal(after[sequence], row)
        missed = [after[sequence] for sequence in range(head_before, target["next_microblock_sequence"]) if sequence in after]
        assert missed, after
        for row in missed:
            assert_equal(row["mode"], "delayed")
            assert row["delay_reason"] in {"catchup", "warmup"}, row
        self.log.info("FLOWMESH_OBSERVED_RESTART kept=%d caught_up=%s", len(before),
                      json.dumps([(row["sequence"], row["delay_reason"]) for row in missed]))
        self.liquidity_report["checks_complete"] = True


if __name__ == "__main__":
    FlowMeshObservedTimeTest(__file__).main()
