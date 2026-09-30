#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Clearing ticks and the indicative next clearing (getflowmeshmarketdata).

Reuses the liquidity fixture: four generated regtest validators and the
engine-off client, whose TLS relays forward to validator 0 (clearing ticks
served, the default) and validator 1 (-flowmeshapiclearingticks=0, the
previous public contract). Checked: a rationed clearing is one tick with
its price, volume, imbalance and the side left unfilled, on the local engine
(also for a light known_head poll and after_sequence paging) and, bound to
the client's verified head, through the advertising endpoint; an endpoint
without the advertisement never receives "ticks"; the indicative next
clearing is labelled not certified, agrees between the local engine and the
client's verified state, and a draft's what-if equals the tick the same
order then produces. Nothing here is a latency claim.
"""

import json

from feature_flowmesh_latency import PRE_ADMISSION_REJECTIONS
from feature_flowmesh_liquidity import FlowMeshLiquidityTest
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, assert_raises_rpc_error

PRICE = 40_000_000
TICK_KEYS = {"sequence", "microblock_hash", "kind", "cleared", "price", "volume", "imbalance", "imbalance_side"}
MAX_MONEY = 662_200_000_000 * 1_000_000
LEVEL_KEYS = {"bucket", "zero_price", "low", "high", "size", "cumulative", "notional", "curves", "own"}


def evaluate(points, price):
    """ClearingEngine::EvaluateCurve: flat outside, floor-interpolated inside."""
    if price <= points[0]["price"]:
        return points[0]["quantity"]
    if price >= points[-1]["price"]:
        return points[-1]["quantity"]
    for a, b in zip(points, points[1:]):
        if price < b["price"]:
            return a["quantity"] + (b["quantity"] - a["quantity"]) * (price - a["price"]) // (b["price"] - a["price"])
    return points[-1]["quantity"]


def expected_levels(curves, depth, account, ticks=None):
    """Exact levels of step-shaped curves (every drop one tick wide), from
    their breakpoints; `ticks` groups canonical prices (bids floor, asks ceil)."""
    def remaining(curve, price):
        return 0 if price < 0 else max(0, evaluate(curve["points"], price) - curve["filled_quantity"])
    out = {}
    for side in ("bid", "ask"):
        own_side = [curve for curve in curves if curve["side"] == side]
        for curve in own_side:
            for a, b in zip(curve["points"], curve["points"][1:]):
                assert a["quantity"] == b["quantity"] or b["price"] == a["price"] + 1, curve
        prices = sorted({point["price"] + d for curve in own_side for point in curve["points"] for d in (-1, 0, 1)
                         if point["price"] + d >= 0})
        buckets = {}
        for price in prices:
            for curve in own_side:
                lots = remaining(curve, price) - remaining(curve, price + 1 if side == "bid" else price - 1)
                if lots <= 0:
                    continue
                index = price if ticks is None else (price // ticks if side == "bid" else -(-price // ticks))
                bucket = buckets.setdefault(index, {"size": 0, "notional": 0, "curves": set(), "own": 0})
                bucket["size"] += lots
                bucket["notional"] += lots * price
                bucket["curves"].add(curve["account_id"])
                if curve["account_id"] == account:
                    bucket["own"] += lots
        rows, cumulative = [], 0
        for index in sorted(buckets, reverse=side == "bid")[:depth]:
            bucket = buckets[index]
            cumulative += bucket["size"]
            if ticks is None:
                low = high = index
            elif side == "bid":
                low, high = index * ticks, min(index * ticks + ticks - 1, MAX_MONEY)
            else:
                low, high = (index * ticks - ticks + 1 if index else 0), min(index * ticks, MAX_MONEY)
            rows.append({"bucket": str(index), "zero_price": False, "low": low, "high": high, "size": bucket["size"],
                         "cumulative": cumulative, "notional": str(bucket["notional"]), "curves": len(bucket["curves"]),
                         "own": bucket["own"]})
        out[side] = (rows, len(buckets) > depth)
    return out


def tick_bodies(relay):
    """The "ticks" parameter of every recorded request that carried one."""
    out = []
    for row in relay.snapshot()["requests"]:
        if "body_hex" in row:
            params = json.loads(bytes.fromhex(row["body_hex"]))["params"]
            if "ticks" in params:
                out.append((row["method"], params["ticks"]))
    return out


class FlowMeshMarketViewsTest(FlowMeshLiquidityTest):
    def set_test_params(self):
        super().set_test_params()
        self.extra_args[1].append("-flowmeshapiclearingticks=0")

    def read_until_endpoint(self, market, relay):
        """Reads until the client's cached state was served through `relay`."""
        def served():
            self.pump_b3()
            data = self.client.getflowmeshmarketdata(market, {"limit": 1, "curve_limit": 1})
            return data["verification"]["endpoint"] == relay.url
        self.wait_until(served, timeout=60, check_interval=.1)

    def check_ticks(self, page, head):
        assert_equal(page["available"], True)
        rows = page["ticks"]
        assert rows, page
        for row in rows:
            assert_equal(set(row), TICK_KEYS)
        sequences = [row["sequence"] for row in rows]
        assert_equal(sequences, list(range(sequences[0], sequences[0] + len(sequences))))
        assert_equal(rows[-1]["microblock_hash"], head)
        return rows

    def check_options(self, market):
        for options, code, message in (
            ({"ticks": []}, -3, "ticks must be an object"),
            ({"ticks": {"after": 1}}, -8, "Unknown ticks option 'after'"),
            ({"ticks": {"limit": 0}}, -8, "limit is outside its supported range"),
            ({"ticks": {"limit": 257}}, -8, "limit is outside its supported range"),
            ({"indicative": {"draft": {"side": "buy", "price": 1, "quantity": 1}}}, -8, "indicative draft side must be bid or ask"),
            ({"indicative": {"draft": {"side": "bid", "price": 0, "quantity": 1}}}, -8, "price must be in the range [1, MAX_MONEY]"),
            ({"indicative": {"draft": {"side": "bid", "price": 1}}}, -3, "quantity must be an integer"),
            ({"indicative": {"now": True}}, -8, "Unknown indicative option 'now'"),
        ):
            assert_raises_rpc_error(code, message, self.nodes[0].getflowmeshmarketdata, market, options)
        # Without the options every reply is as before.
        plain = self.nodes[0].getflowmeshmarketdata(market, {"limit": 1})
        assert "clearing_ticks" not in plain and "indicative" not in plain

    def all_curves(self, node, market):
        """Every standing curve of one head, paged through curve_cursor."""
        first = node.getflowmeshmarketdata(market, {"limit": 1, "curve_limit": 128})
        head = first["snapshot"]["last_microblock_hash"]
        curves, page = list(first["liquidity"]["curves"]), first["liquidity"]
        while "next_cursor" in page:
            page = node.getflowmeshmarketdata(market, {"limit": 1, "curve_limit": 128, "curve_cursor": page["next_cursor"],
                                                       "expected_head": head})["liquidity"]
            curves += page["curves"]
        assert_equal(len(curves), first["liquidity"]["total_curves"])
        return head, curves

    def check_level_options(self, market):
        n0 = self.nodes[0]
        group = {"orientation": "canonical", "mantissa": 1, "exponent": 0, "decimals": 6}
        for options, code, message in (
            ({"levels": []}, -3, "levels must be an object"),
            ({"levels": {"deep": 1}}, -8, "Unknown levels option 'deep'"),
            ({"levels": {"depth": 0}}, -8, "depth is outside its supported range"),
            ({"levels": {"depth": 51}}, -8, "depth is outside its supported range"),
            ({"levels": {"include_unchanged": 1}}, -3, "include_unchanged must be a boolean"),
            ({"levels": {"group": {**group, "mantissa": 3}}}, -8, "levels group: mantissa must be 1, 2 or 5"),
            # 0.0001 B3 per token of a 6-decimal token is a tenth of a tick.
            ({"levels": {"group": {**group, "exponent": -4}}}, -8, "levels group: a canonical price step must be a whole number"),
            ({"levels": {"group": {**group, "orientation": "inverse", "exponent": 4}}}, -8, "levels group: an inverse price step"),
            ({"levels": {"group": {**group, "orientation": "sideways"}}}, -8, "levels group orientation must be canonical or inverse"),
            ({"levels": {"group": {**group, "step": 1}}}, -8, "Unknown levels group option 'step'"),
            ({"levels": {"group": {"orientation": "canonical", "mantissa": 1, "decimals": 6}}}, -3, "exponent must be an integer"),
        ):
            assert_raises_rpc_error(code, message, n0.getflowmeshmarketdata, market, options)
        # Without the option every reply is as before.
        assert "book_levels" not in n0.getflowmeshmarketdata(market, {"limit": 1})

    def ladder(self, node, market, side, steps, label):
        sequence = self.market_data(node, market)["account"]["next_sequence"]
        status = self.certify_action(node, market, "submitflowmeshladder", (market, side, steps, sequence), label, sequence)
        self.wait_until(lambda: self.market_data(node, market)["account"]["next_sequence"] == sequence + 1,
                        timeout=90, check_interval=.1)
        # The signing reply itself describes the signed ladder.
        records = self.client_results if node is self.client else self.liquidity_report["native_action_receipts"]
        response = records[-1]["initial_response"]
        assert_equal(response["action_id"], status["action_id"])
        assert_equal((response["side"], response["sequence"]), (side, sequence))
        assert_equal(response["quantity"], sum(step["quantity"] for step in steps))
        return response

    def check_ladder_options(self, market):
        n0 = self.nodes[0]
        step = {"price": PRICE, "quantity": 1}
        for side, steps, code, message in (
            ("buy", [step], -8, "side must be 'bid' or 'ask'"),
            ("bid", [], -8, "steps must hold 1 to 4 prices"),
            ("bid", [{"price": PRICE + i, "quantity": 1} for i in range(5)], -8, "steps must hold 1 to 4 prices"),
            ("bid", {"price": PRICE}, -3, "JSON value of type object is not of expected type array"),
            ("bid", [step, step], -8, "each ladder price must be distinct"),
            ("bid", [{"price": PRICE, "quantity": 0}], -8, "quantity must be in the range [1, MAX_MONEY]"),
            ("bid", [{"price": PRICE, "quantity": 1, "tif": "ioc"}], -8, "Unknown step option 'tif'"),
            ("bid", [{"price": MAX_MONEY, "quantity": 1}], -8, "bid price must be below MAX_MONEY"),
            ("bid", [{"price": MAX_MONEY - 1, "quantity": 2}], -8, "the ladder's total quantity or reservation exceeds MAX_MONEY"),
        ):
            assert_raises_rpc_error(code, message, n0.submitflowmeshladder, market, side, steps)

    def check_levels(self, market, groupings):
        """The local engine's levels equal the ones computed here from every
        paged curve; the client's, from its verified state, equal them at the
        same head (own shares are each wallet's)."""
        n0 = self.nodes[0]
        for group, ticks in groupings:
            levels = {"depth": 50, **({"group": group} if group else {})}

            def same_head():
                self.pump_b3()
                local = n0.getflowmeshmarketdata(market, {"limit": 1, "curve_limit": 1, "levels": levels})
                remote = self.client.getflowmeshmarketdata(market, {"limit": 1, "curve_limit": 1, "levels": levels})
                if local["snapshot"]["last_microblock_hash"] != remote["snapshot"]["last_microblock_hash"]:
                    return None
                return local, remote
            self.wait_until(lambda: same_head() is not None, timeout=60, check_interval=.2)
            local, remote = same_head()
            head, curves = self.all_curves(n0, market)
            if head != local["snapshot"]["last_microblock_hash"]:
                continue
            for data in (local, remote):
                view = data["book_levels"]
                assert_equal((view["available"], view["certified"], view["excludes_pending_actions"], view["basis"], view["head"]),
                             (True, False, True, "verified_standing_curves", head))
                assert_equal(view["grouping"], {"exact": True} if not group else {"exact": False, **group})
                assert_equal(view["total_curves"], len(curves))
                expected = expected_levels(curves, 50, data["account"]["account_id"], ticks)
                for side, key in (("bid", "bids"), ("ask", "asks")):
                    rows, more = expected[side]
                    for row in view[key]["levels"]:
                        assert_equal(set(row), LEVEL_KEYS)
                    assert_equal(view[key]["levels"], rows)
                    assert_equal(view[key]["more"], more)
                if expected["bid"][0] and not ticks:
                    assert_equal(view["best_bid"], expected["bid"][0][0]["low"])
                assert_equal(view["standing"]["certified"], False)
                assert "draft" not in view["standing"]
            strip = lambda view: {**view, "bids": {**view["bids"], "levels": [{**row, "own": 0} for row in view["bids"]["levels"]]},
                                  "asks": {**view["asks"], "levels": [{**row, "own": 0} for row in view["asks"]["levels"]]}}
            assert_equal(strip(local["book_levels"]), strip(remote["book_levels"]))
            # A light poll carries levels only when asked.
            light = n0.getflowmeshmarketdata(market, {"known_head": head, "levels": levels})
            if light["unchanged"]:
                assert "book_levels" not in light
                asked = n0.getflowmeshmarketdata(market, {"known_head": head, "levels": {**levels, "include_unchanged": True}})
                if asked["unchanged"]:
                    assert_equal(asked["book_levels"], local["book_levels"])
        return local["book_levels"]

    def run_test(self):
        market, asset = self.bootstrap_latency_market()
        self.fund_latency_client(market, asset)
        self.fund_seller(market, asset)
        self.assert_engine_off()
        relay0, relay1 = self.tls_relays
        n0 = self.nodes[0]
        self.check_options(market)
        self.check_level_options(market)

        # A rationed clearing: one unit offered, two bid at the same price.
        self.order(n0, market, "ask", PRICE, 1, "rationed_ask")
        self.order(self.client, market, "bid", PRICE, 2, "rationed_bid")
        operators, _ = self.capture(market, "rationed")
        head = operators[0]["snapshot"]["last_microblock_hash"]
        local = n0.getflowmeshmarketdata(market, {"ticks": {}, "indicative": {}})
        assert_equal(local["snapshot"]["last_microblock_hash"], head)
        assert_equal(local["clearing_ticks"]["source"], "local_engine")
        rows = self.check_ticks(local["clearing_ticks"], head)
        rationed = [row for row in rows if row["cleared"]][-1]
        assert_equal((rationed["kind"], rationed["price"], rationed["volume"], rationed["imbalance"], rationed["imbalance_side"]),
                     ("execution", PRICE, 1, 1, "bid"))
        # Every replica observed the same tick on its own replay.
        for node in self.nodes[1:]:
            replica = node.getflowmeshmarketdata(market, {"ticks": {"after_sequence": rationed["sequence"] - 1, "limit": 1}})
            assert_equal(replica["clearing_ticks"]["ticks"], [rationed])

        # A light poll: no heavy data, still the ticks after its position.
        light = n0.getflowmeshmarketdata(market, {"known_head": head, "ticks": {"after_sequence": rationed["sequence"] - 1},
                                                  "indicative": {}})
        assert_equal(light["unchanged"], True)
        assert "liquidity" not in light and "history" not in light
        assert_equal(light["clearing_ticks"]["ticks"][0], rationed)
        assert_equal(light["indicative"], local["indicative"])

        # The standing indicative: the rationed bid's rest, nothing to cross.
        indicative = local["indicative"]
        assert_equal((indicative["available"], indicative["certified"], indicative["excludes_pending_actions"]), (True, False, True))
        assert_equal((indicative["head"], indicative["basis"], indicative["cleared"]), (head, "verified_standing_curves", False))
        assert_equal(indicative["best_bid"], PRICE)
        assert "best_ask" not in indicative and "draft" not in indicative

        # The client: its verified state gives the same indicative, and the
        # advertising endpoint's ticks, bound to the verified head.
        self.read_until_endpoint(market, relay0)

        def same_indicative():
            remote_view = self.client.getflowmeshmarketdata(market, {"limit": 1, "indicative": {}})["indicative"]
            local_view = n0.getflowmeshmarketdata(market, {"limit": 1, "indicative": {}})["indicative"]
            if remote_view["head"] != local_view["head"]:
                return False
            assert_equal(remote_view, local_view)
            return True
        self.wait_until(same_indicative, timeout=60, check_interval=.2)

        def remote_ticks():
            self.pump_b3()
            data = self.client.getflowmeshmarketdata(market, {"limit": 1, "ticks": {}})
            return data if data["clearing_ticks"]["available"] else None
        self.wait_until(lambda: remote_ticks() is not None, timeout=60, check_interval=.2)
        remote = remote_ticks()
        assert_equal(remote["clearing_ticks"]["source"], "endpoint_reported")
        remote_rows = self.check_ticks(remote["clearing_ticks"], remote["snapshot"]["last_microblock_hash"])
        by_sequence = {row["sequence"]: row for row in rows}
        common = [row for row in remote_rows if row["sequence"] in by_sequence]
        assert common
        for row in common:
            assert_equal(row, by_sequence[row["sequence"]])
        assert all(params.get("limit") == 256 for _, params in tick_bodies(relay0)), tick_bodies(relay0)

        # The seller's what-if for one unit at the price, then that order.
        what_if = n0.getflowmeshmarketdata(market, {"indicative": {"draft": {"side": "ask", "price": PRICE, "quantity": 1}}})["indicative"]
        assert_equal((what_if["basis"], what_if["draft"]["account_known"], what_if["draft"]["replaces_standing_curve"],
                      what_if["draft"]["assumes_admission"]),
                     ("verified_standing_curves_with_draft", True, False, True))
        assert_equal((what_if["cleared"], what_if["price"], what_if["volume"], what_if["imbalance"], what_if["imbalance_side"]),
                     (True, PRICE, 1, 0, "none"))
        assert_equal((what_if["draft_fill_min"], what_if["draft_fill_max"]), (1, 1))
        self.order(n0, market, "ask", PRICE, 1, "what_if_ask")
        self.capture(market, "what_if_executed")
        after = n0.getflowmeshmarketdata(market, {"ticks": {"after_sequence": rationed["sequence"]}})
        executed = [row for row in after["clearing_ticks"]["ticks"] if row["cleared"]]
        assert_equal(len(executed), 1)
        assert_equal((executed[0]["price"], executed[0]["volume"], executed[0]["imbalance"], executed[0]["imbalance_side"]),
                     (what_if["price"], what_if["volume"], what_if["imbalance"], what_if["imbalance_side"]))

        # Through the endpoint that serves no ticks: never asked for, and a
        # head it verifies leaves the client's ticks unavailable, with why.
        relay0.configure(unavailable=True)
        try:
            self.read_until_endpoint(market, relay1)
            assert_equal(tick_bodies(relay1), [])
            # The operator's own order: no relay is involved.
            sequence = self.market_data(n0, market)["account"]["next_sequence"]

            def submitted():
                try:
                    n0.submitflowmeshorder(market, "ask", 2 * PRICE, 1, sequence)
                    return True
                except JSONRPCException as error:
                    if error.error.get("code") != -1 or error.error.get("message") not in PRE_ADMISSION_REJECTIONS:
                        raise
                    return False
            self.wait_until(submitted, timeout=30, check_interval=.1)
            self.wait_until(lambda: self.market_data(n0, market)["account"]["next_sequence"] == sequence + 1,
                            timeout=90, check_interval=.1)
            moved = n0.getflowmeshmarketdata(market, {"limit": 1})["snapshot"]["last_microblock_hash"]

            def through_relay1():
                self.pump_b3()
                data = self.client.getflowmeshmarketdata(market, {"limit": 1, "ticks": {}})
                return data if data["snapshot"]["last_microblock_hash"] == moved else None
            self.wait_until(lambda: through_relay1() is not None, timeout=60, check_interval=.2)
            data = through_relay1()
            assert_equal(data["verification"]["endpoint"], relay1.url)
            assert_equal((data["clearing_ticks"]["available"], data["clearing_ticks"]["unavailable_reason"]),
                         (False, "endpoint_does_not_advertise_clearing_ticks"))
            assert_equal(tick_bodies(relay1), [])
        finally:
            relay0.configure()
        # Back at the advertising endpoint the missing ticks arrive.
        self.read_until_endpoint(market, relay0)
        self.wait_until(lambda: remote_ticks() is not None, timeout=60, check_interval=.2)
        assert_equal(remote_ticks()["clearing_ticks"]["ticks"][-1]["microblock_hash"], moved)

        # Book levels: the operator's standing ask and a bid of the client,
        # exact and in canonical groups of 10^6 ticks (d = 9, h = 10^6).
        self.order(self.client, market, "bid", PRICE // 2, 3, "levels_bid")
        grouped = ({"orientation": "canonical", "mantissa": 1, "exponent": 6, "decimals": 9}, 10**6)
        self.check_levels(market, ((None, None), grouped))

        # Ladder orders: several prices on one side as one curve, through the
        # existing signed submission. Each replaces its side's order.
        self.check_ladder_options(market)
        bid_steps = [{"price": PRICE // 2, "quantity": 3}, {"price": PRICE // 2 - 1_000_000, "quantity": 2},
                     {"price": PRICE // 2 - 3_000_000, "quantity": 4}, {"price": PRICE // 2 - 3_000_001, "quantity": 1}]
        ask_steps = [{"price": 3 * PRICE, "quantity": 1}, {"price": 2 * PRICE, "quantity": 1}, {"price": 2 * PRICE + 5_000_000, "quantity": 2}]
        bid = self.ladder(self.client, market, "bid", bid_steps, "ladder_bid")
        ask = self.ladder(self.nodes[0], market, "ask", ask_steps, "ladder_ask")
        assert_equal(bid["reservation"], sum(step["price"] * step["quantity"] for step in bid_steps))
        assert_equal(ask["reservation"], 4)
        assert_equal([step["price"] for step in bid["steps"]], sorted(step["price"] for step in bid_steps))
        # Two adjacent prices share a breakpoint: seven, not eight.
        assert_equal(len(bid["points"]), 7)
        levels = self.check_levels(market, ((None, None), grouped))
        # Each step is its own exact level, the account's own share.
        head, curves = self.all_curves(self.nodes[0], market)
        own_bid = [curve for curve in curves if curve["side"] == "bid"]
        assert_equal(len(own_bid), 1)
        assert_equal(own_bid[0]["points"], [{"price": point["price"], "quantity": point["quantity"]} for point in bid["points"]])
        exact = self.client.getflowmeshmarketdata(market, {"limit": 1, "levels": {}})
        if exact["snapshot"]["last_microblock_hash"] == head:
            rows = exact["book_levels"]["bids"]["levels"]
            assert_equal([(row["low"], row["size"], row["own"]) for row in rows],
                         sorted(((step["price"], step["quantity"], step["quantity"]) for step in bid_steps), reverse=True))
            reserved = exact["account"]["b3_reserved_atoms"]
            assert_equal(reserved, bid["reservation"])
        self.log.info("FLOWMESH_MARKET_VIEWS levels=%s", json.dumps(levels))
        self.liquidity_report["checks_complete"] = True
        self.log.info("FLOWMESH_MARKET_VIEWS rationed=%s what_if=%s", json.dumps(rationed), json.dumps(what_if))


if __name__ == "__main__":
    FlowMeshMarketViewsTest(__file__).main()
