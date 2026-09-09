#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Bounded real-P2P four-validator workload, not an engine-only benchmark.

Reuses the full deposit/withdrawal qualification flow. Extra actions must
certify with no new B3 block. Latencies are observations on this local test
machine, not mainnet guarantees. No timing threshold weakens quorum tests.
"""

import json
import math
import time

from feature_flowmesh_release import FlowMeshReleaseTest, B3_DEPOSIT, TRADE_PRICE, TRADE_QUANTITY
from test_framework.util import assert_equal


class FlowMeshSpeedTest(FlowMeshReleaseTest):
    def set_test_params(self):
        super().set_test_params()

    def run_test(self):
        super().run_test()

    def wait_for_account_sequence(self, buyer, market_id, sequence):
        try:
            self.wait_until(lambda: buyer.getflowmeshbalance(market_id)["account"]["next_sequence"] == sequence,
                            timeout=60, check_interval=0.05)
        except AssertionError:
            # Preserve transient candidate/evidence/vote observations before
            # test cleanup. A restart retains locks, but not these counters.
            for index, node in enumerate(self.nodes):
                try:
                    peers = [{"id": p["id"],
                              "received": {k: v for k, v in p.get("bytesrecv_per_msg", {}).items() if k.startswith("fm")},
                              "sent": {k: v for k, v in p.get("bytessent_per_msg", {}).items() if k.startswith("fm")}}
                             for p in node.getpeerinfo()]
                    diagnostic = {"node": index, "expected_account_sequence": sequence,
                                  "balance": node.getflowmeshbalance(market_id),
                                  "validator": node.getflowmeshvalidatorinfo(), "peers": peers}
                    self.log.error("FLOWMESH_STALL_DIAGNOSTIC %s", json.dumps(diagnostic, sort_keys=True, default=str))
                except Exception as error:
                    self.log.error("FlowMesh diagnostic unavailable for test node %s: %s", index, error)
            raise

    def exercise_extra_trading(self, market_id):
        buyer = self.nodes[1]
        height = buyer.getblockcount()
        first = buyer.getflowmeshbalance(market_id)
        sequence = first["account"]["next_sequence"]
        latencies = []
        rpc_times = []
        self.log.info("40 sequential real-P2P order/cancel actions without B3 block production")
        for index in range(40):
            started = time.monotonic()
            if index % 2 == 0:
                response = buyer.submitflowmeshorder(market_id, "bid", TRADE_PRICE // 2, TRADE_QUANTITY, sequence)
            else:
                response = buyer.cancelflowmeshorder(market_id, "bid", sequence)
            rpc_times.append((time.monotonic() - started) * 1000)
            assert_equal(response["accepted"], True)
            sequence += 1
            self.wait_for_account_sequence(buyer, market_id, sequence)
            latencies.append((time.monotonic() - started) * 1000)
            account = buyer.getflowmeshbalance(market_id)["account"]
            if index % 2:
                assert_equal(account["b3_reserved"], 0)
                assert_equal(account["b3_available"], B3_DEPOSIT)
        self.wait_for_market_convergence(market_id)
        assert_equal(buyer.getblockcount(), height)

        self.log.info("64 explicitly sequenced actions in one bounded burst, then verify every account sequence")
        started = time.monotonic()
        burst_start = sequence
        for index in range(64):
            if index % 2 == 0:
                reply = buyer.submitflowmeshorder(market_id, "bid", TRADE_PRICE // 2, TRADE_QUANTITY, sequence)
            else:
                reply = buyer.cancelflowmeshorder(market_id, "bid", sequence)
            assert_equal(reply["accepted"], True)
            assert_equal(reply["sequence"], sequence)
            sequence += 1
        submitted_ms = (time.monotonic() - started) * 1000
        self.wait_for_account_sequence(buyer, market_id, sequence)
        certified_ms = (time.monotonic() - started) * 1000
        self.wait_for_market_convergence(market_id)
        account = buyer.getflowmeshbalance(market_id)["account"]
        assert_equal(account["b3_available"], B3_DEPOSIT)
        assert_equal(account["b3_reserved"], 0)
        assert_equal(account["base_available"], 0)
        assert_equal(sequence - burst_start, 64)
        assert_equal(buyer.getblockcount(), height)

        ordered = sorted(latencies)
        percentile = lambda p: ordered[max(0, math.ceil(len(ordered) * p) - 1)]
        report = {"network": "four-node local regtest P2P", "sequential_actions": 40,
                  "confirmation_observation_interval_ms": 50,
                  "rpc_mean_ms": sum(rpc_times) / len(rpc_times),
                  "certified_p50_ms": percentile(.5), "certified_p95_ms": percentile(.95),
                  "certified_p99_ms": percentile(.99), "burst_actions": 64,
                  "burst_rpc_submitted_ms": submitted_ms, "burst_all_certified_ms": certified_ms,
                  "new_B3_blocks": 0}
        self.log.info("FLOWMESH_SPEED_RESULT %s", json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    FlowMeshSpeedTest(__file__).main()
