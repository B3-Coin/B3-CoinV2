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

    def run_test(self):
        market, asset = self.bootstrap_latency_market()
        self.fund_latency_client(market, asset)
        self.fund_seller(market, asset)
        self.assert_engine_off()
        relay0, relay1 = self.tls_relays
        n0 = self.nodes[0]
        self.check_options(market)

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
        self.liquidity_report["checks_complete"] = True
        self.log.info("FLOWMESH_MARKET_VIEWS rationed=%s what_if=%s", json.dumps(rationed), json.dumps(what_if))


if __name__ == "__main__":
    FlowMeshMarketViewsTest(__file__).main()
