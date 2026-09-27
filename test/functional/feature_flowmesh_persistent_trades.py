#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Small direct-HTTPS matched-fill qualification, using fresh local regtest data.

Run the identical fixture separately against the original close-per-request
daemon (--transport-arm=baseline) and the persistent daemon (=warm). No TLS
relay, existing wallet, remote node or Qt is used. Both maker and buyer are
wallets in the fifth, engine-off process. The four operators retain the
existing preagreement/FMN2 fixture and all economic/proof rules.

Six one-unit matches ascend and descend through a fixed price schedule while
B3 advances. Timing is an observed RPC-to-proof/account upper bound, not a
consensus-only measurement or a claim that inclusion alone proves a fill.

--samples=N runs N matches, cycling the same price schedule; funding grows
with N. --status-wait-ms=MS passes MS as getflowmeshactionstatus wait_ms
when the client binary's help lists that argument (checked once); an older
binary reads immediately and the report records that. While B3 advances, a
waited read is cut to end when the next mock-time tick is due, so it returns
to the pump on the same 0.5 s cadence as the 5 ms poll loop and both arms
see the same B3 workload. Every order records the validators' B3 heights
just before its RPC and just after certification, outside the timed span,
and the pump ticks inside its window, so results can be stratified by
whether B3 advanced during the measurement. Either option, or
--record-reconnects, records every sample's HTTPS reuse/handshake flags
instead of asserting the warm arm's reuse per sample, so a connection
rollover or reset stays in the distribution rather than aborting the run.
The baseline arm keeps its per-sample checks in every mode: a
close-per-request daemon has no rollover to excuse. It also
records, rather than fails on, a validator trace stream reaching its
process-lifetime cap (32,768 rows, about eight samples on node0): that
stream is then absent from later captures, while the measured timings come
from the harness clock. With none of them the fixture and its assertions
are unchanged.
"""

import argparse
import hashlib
import json
import math
import re
import sys
import time
from collections import Counter, deque
from decimal import Decimal
from pathlib import Path

from feature_flowmesh_latency import FlowMeshLatencyTest, PRE_ADMISSION_REJECTIONS, distribution, host_us
from feature_flowmesh_release import B3_ARGS, TRADE_PRICE
from test_framework.authproxy import JSONRPCException
from test_framework.test_node import TestNode
from test_framework.util import assert_equal, get_datadir_path, initialize_datadir, p2p_port


PRICES = (TRADE_PRICE // 2, TRADE_PRICE, TRADE_PRICE * 3 // 2,
          TRADE_PRICE, TRADE_PRICE // 2, TRADE_PRICE // 4)
BALANCES = ("base_available", "base_reserved", "b3_available_atoms", "b3_reserved_atoms")
IMMUTABLE_ACTION = ("market_id", "domain", "execution_config_id", "account_id", "action_id",
                    "sequence", "canonical_side", "canonical_points", "signed_bytes_sha256",
                    "signed_bytes_size", "initial_submission_ms")
B3_ATOMS = 1_000_000_000  # Native atomic units per B3.
# The client retains at most 512 actions: two deposits plus an ask and a bid
# per match. More matches would evict certified orders the final checks read.
MAX_SAMPLES = (512 - 2) // 2
STATUS_WAIT_MAX_MS = 2500  # getflowmeshactionstatus wait_ms bound.
# A waited status read ends when the next B3 pump tick is due; returning
# later than this after the due time means the pump cadence was not kept.
PUMP_OVERDUE_BOUND_MS = 100


def bounded_int(name, low, high):
    def parse(value):
        number = int(value)
        if not low <= number <= high:
            raise argparse.ArgumentTypeError(f"{name} must be between {low} and {high}")
        return number
    return parse


class FlowMeshPersistentTradesTest(FlowMeshLatencyTest):
    def add_options(self, parser):
        super().add_options(parser)
        parser.add_argument("--transport-arm", choices=("baseline", "warm"), required=True)
        size = parser.add_mutually_exclusive_group()
        size.add_argument("--harness-smoke", action="store_true",
                          help="One match to validate the fixture; never a reported performance arm")
        size.add_argument("--samples", type=bounded_int("--samples", 1, MAX_SAMPLES), default=len(PRICES),
                          help=f"Measured matches, cycling the fixed price schedule (1-{MAX_SAMPLES}, default {len(PRICES)})")
        parser.add_argument("--status-wait-ms", type=bounded_int("--status-wait-ms", 0, STATUS_WAIT_MAX_MS), default=0,
                            help=f"getflowmeshactionstatus wait_ms, used only if the client binary lists it (0-{STATUS_WAIT_MAX_MS}, default 0: immediate reads)")
        parser.add_argument("--record-reconnects", action="store_true",
                            help=f"Record per-sample HTTPS reuse/handshake flags and validator trace-cap markers instead of asserting them (implied by --samples other than {len(PRICES)} or --status-wait-ms)")

    def set_test_params(self):
        self.options.latency_production_logging = True
        super().set_test_params()
        self.prices = (PRICES[:1] if self.options.harness_smoke else
                       tuple(PRICES[number % len(PRICES)] for number in range(self.options.samples)))
        # Without the new options every sample keeps its original assertions.
        self.strict_samples = not (self.options.record_reconnects or self.options.status_wait_ms or
                                   self.options.samples != len(PRICES))
        self.status_wait_ms = 0  # Effective wait_ms, set once the client binary is known.
        self.pump_ticks = deque(maxlen=256)  # Host time at the start of recent B3 pump ticks.
        self.capture_clock = None
        self.trade_report = {"format_version": 1, "arm": self.options.transport_arm,
            "harness_smoke": self.options.harness_smoke, "performance_measurement_qualified": False,
            "correctness_pass": False, "prices": list(self.prices), "samples": [], "child_exits": [],
            "sample_assertions": "per_sample" if self.strict_samples else "recorded_only",
            "status_wait": {"requested_ms": self.options.status_wait_ms, "effective_ms": 0},
            "trace_limits_reached": [],
            "scope": {"operators": 4, "engine_off_clients": 1, "client_wallets": 2,
                "HTTPS": "direct native TLS listeners; no relay", "B3_advancing": True,
                "price_model": "one standing ask and equal-price bid per one-unit match",
                "measurement": "original buyer order RPC start to locally verified inclusion, then authenticated buyer/maker state and four native histories; offered wall-clock timestamps are host observations, not certified time",
                "not_qualified": ["Qt", "WAN", "high concurrency", "crash durability", "200ms guarantee"]}}
        self.capture_nodes = []

    def start_ordinary_client(self):
        initialize_datadir(self.options.tmpdir, 4, self.chain, self.disable_autoconnect)
        self.client_args = [*B3_ARGS, "-enableflowmeshvalidator=0", "-debug=0",
            f"-port={p2p_port(12)}", f"-bind=127.0.0.1:{p2p_port(12)}",
            f"-flowmeshendpointca={self.pki['ca']}",
            *[f"-flowmeshendpoint=https://127.0.0.1:{port}" for port in self.api_ports]]
        self.client = TestNode(4, get_datadir_path(self.options.tmpdir, 4), chain=self.chain,
            rpchost=None, timewait=self.rpc_timeout, timeout_factor=self.options.timeout_factor,
            binaries=self.get_binaries(), coverage_dir=self.options.coveragedir,
            cwd=self.options.tmpdir, extra_args=self.client_args, uses_wallet=True)
        self.client.start()
        self.client.wait_for_rpc_connection()
        self.client.createwallet(wallet_name=self.default_wallet_name, load_on_startup=True)
        self.client.createwallet(wallet_name="persistent_maker", load_on_startup=True)
        self.buyer = self.client.get_wallet_rpc(self.default_wallet_name)
        self.maker = self.client.get_wallet_rpc("persistent_maker")
        self.sync_client_time()
        self.client.addnode(f"127.0.0.1:{p2p_port(0)}", "onetry")
        self.wait_client_b3_sync()
        self.assert_engine_off()
        if self.options.status_wait_ms:
            # Asked once. A binary without the argument (c10c952) gets the
            # unchanged two-argument read; the report records which applied.
            supported = re.search(r"^3\. wait_ms\b", self.client.help("getflowmeshactionstatus"), re.M) is not None
            self.status_wait_ms = self.options.status_wait_ms if supported else 0
            self.trade_report["status_wait"].update(client_supports_wait_ms=supported, effective_ms=self.status_wait_ms)

    def assert_engine_off(self):
        # Client/validator status are wallet RPCs too; the inherited helper
        # assumes one loaded wallet, whereas this fixture deliberately has two.
        for rpc in (self.buyer, self.maker):
            info = rpc.getflowmeshclientinfo()
            assert_equal(info["backend"], "remote")
            assert_equal(info["engine_enabled"], False)
            assert_equal(len(info["endpoints"]), 2)
            validator = rpc.getflowmeshvalidatorinfo()
            assert_equal(validator["service_available"], False)
            assert_equal(validator["armed"], False)
            assert_equal(validator["wallet_key_count"], 0)
        assert not (self.client.chain_path / "flowmesh" / "network").exists()
        if self.last_market:
            assert not (self.client.chain_path / "flowmesh" / self.last_market).exists()

    def pump_b3(self):
        # Log when each mock-time tick starts, so an order window can report
        # the B3 progress it overlapped.
        due, started = self.next_clock_tick, host_us()
        super().pump_b3()
        if self.next_clock_tick != due:
            self.pump_ticks.append(started)

    def b3_heights(self):
        return [node.getblockcount() for node in self.nodes]

    def status_read(self, read):
        """One status read, and how long it kept the harness past a due B3 tick (ms)."""
        due, begun = self.next_clock_tick, time.monotonic()
        status = read()
        if self.clock_indices is None:
            return status, 0.0
        return status, max(0.0, time.monotonic() - max(due, begun)) * 1000

    @staticmethod
    def saved_actions(rpc, market):
        result = rpc.listflowmeshactions(market)
        assert_equal(result["source"], "local-retained-outbox")
        actions = result["actions"]
        assert_equal(len(actions), len({row["action_id"] for row in actions}))
        return {row["action_id"]: row for row in actions}

    def authenticated_account(self, rpc, market, predicate=lambda row: True):
        result = {}
        def ready():
            self.pump_b3()
            data = rpc.getflowmeshmarketdata(market, {"limit": 100, "curve_limit": 8})
            verification = data["verification"]
            for field in ("certificate_verified", "account_state_verified", "history_endpoint_reported"):
                assert_equal(verification[field], True)
            for field in ("execution_result_verified", "freshest_network_head_proven"):
                assert_equal(verification[field], False)
            assert_equal(verification["source"], "remote_endpoint")
            if "account" in data and predicate(data["account"]):
                result.update(data)
                return True
            return False
        self.wait_until(ready, timeout=60, check_interval=.01)
        return result

    def certify(self, rpc, market, response, sample):
        action_id = response["action_id"]
        saved = self.saved_actions(rpc, market)[action_id]
        original = {key: saved[key] for key in IMMUTABLE_ACTION if key in saved}
        sample.update(action_id=action_id, original_action=original)
        status, deadline, retry_at = response, time.monotonic() + 60, time.monotonic() + 1
        retries = []
        next_read = time.monotonic()
        overdue_ms = 0.0
        while status["receipt_state"] != "certified_inclusion":
            assert time.monotonic() < deadline, "bounded action certification deadline exceeded"
            self.pump_b3()
            state = status["receipt_state"]
            retryable = state == "unknown" or (state == "rejected" and status.get("reason") in PRE_ADMISSION_REJECTIONS)
            assert state != "rejected" or retryable, status
            if retryable and time.monotonic() >= retry_at and len(retries) < 3:
                status = rpc.retryflowmeshaction(market, action_id)
                retries.append({"host_us": host_us(), "state": status["receipt_state"]})
                retry_at = time.monotonic() + 1
            elif self.status_wait_ms:
                # A waited read paces itself. Reads still start at least 5 ms
                # apart, so one that returns at once (for example from an
                # endpoint without the capability) cannot spin.
                time.sleep(max(0, next_read - time.monotonic()))
                next_read = time.monotonic() + .005
                wait_ms = self.status_wait_ms
                if self.clock_indices is not None:
                    # End the wait when the next B3 tick is due, so the loop
                    # pumps on the poll loop's cadence and both arms run the
                    # same B3 workload during the measured window.
                    wait_ms = min(wait_ms, max(1, math.ceil((self.next_clock_tick - time.monotonic()) * 1000)))
                status, late_ms = self.status_read(lambda: rpc.getflowmeshactionstatus(market, action_id, wait_ms))
                overdue_ms = max(overdue_ms, late_ms)
            else:
                time.sleep(.005)
                status, late_ms = self.status_read(lambda: rpc.getflowmeshactionstatus(market, action_id))
                overdue_ms = max(overdue_ms, late_ms)
            assert_equal(status["action_id"], action_id)
        sample.update(certified_status=status, certified_host_us=host_us(), exact_action_retries=retries)
        if "b3" in sample:
            # Read after the timed span closed. Ticks are counted from the
            # order RPC start to certification.
            after = self.b3_heights()
            start, end = sample["started_host_us"], sample["certified_host_us"]
            inside = [tick for tick in self.pump_ticks if start <= tick <= end]
            earlier = [tick for tick in self.pump_ticks if tick < start]
            sample["b3"].update(heights_after=after,
                advanced_in_window=any(new > old for old, new in zip(sample["b3"]["heights_before"], after)),
                pump_ticks_in_window=len(inside), pump_tick_offsets_ms=[(tick - start) / 1000 for tick in inside],
                last_pump_tick_before_start_ms=(start - earlier[-1]) / 1000 if earlier else None,
                status_read_max_overdue_ms=overdue_ms)
        assert_equal(status["certificate_verified"], True)
        assert_equal(status["outcome_verified"], False)
        after = self.saved_actions(rpc, market)[action_id]
        assert_equal({key: after[key] for key in original}, original)
        assert_equal(after["previously_certified"], True)
        assert_equal(after["may_have_been_sent"], True)
        return status

    def submit_order(self, rpc, market, side, price, sequence):
        before_ids = set(self.saved_actions(rpc, market))
        sample = {"side": side, "price": price, "account_sequence": sequence,
                  "offered_host_us": host_us(), "offered_wall_clock_ms": time.time_ns() // 1_000_000,
                  "pre_admission_refusals": []}
        for attempt in range(10):
            self.pump_b3()
            try:
                if "started_host_us" not in sample:
                    if self.clock_indices is not None:
                        # Read before the timed span opens.
                        sample["b3"] = {"heights_before": self.b3_heights()}
                    sample["started_host_us"] = host_us()
                response = rpc.submitflowmeshorder(market, side, price, 1, sequence)
                break
            except JSONRPCException as error:
                if error.error.get("code") != -1 or error.error.get("message") not in PRE_ADMISSION_REJECTIONS:
                    raise
                # A new signing attempt is permitted only when no instruction
                # was retained. Any ambiguous result fails without resigning.
                assert_equal(set(self.saved_actions(rpc, market)), before_ids)
                sample["pre_admission_refusals"].append({"reason": error.error["message"], "host_us": host_us()})
                time.sleep(.025)
        else:
            raise AssertionError("bounded pre-admission attempts exhausted")
        sample.update(initial_response=response, initial_response_host_us=host_us())
        assert_equal(response["sequence"], sequence)
        self.certify(rpc, market, response, sample)
        saved = self.saved_actions(rpc, market)
        assert_equal(set(saved) - before_ids, {sample["action_id"]})
        points = ([{"price": price, "quantity": 1}, {"price": price + 1, "quantity": 0}] if side == "bid"
                  else [{"price": price - 1, "quantity": 0}, {"price": price, "quantity": 1}])
        assert_equal(saved[sample["action_id"]]["canonical_points"], points)
        sample["initial_response_ms"] = (sample["initial_response_host_us"] - sample["started_host_us"]) / 1000
        sample["client_certified_ms"] = (sample["certified_host_us"] - sample["started_host_us"]) / 1000
        return sample

    def fund_wallets(self, market, asset):
        self.start_ordinary_client()
        # Six matches keep the original 1 B3 / 20 unit deposits; longer runs
        # deposit every price with a 20% margin and one unit per ask.
        buyer_b3 = max(1, math.ceil(sum(self.prices) * 6 / 5 / B3_ATOMS))
        maker_units = max(20, len(self.prices))
        self.trade_report["funding"] = {"buyer_b3": buyer_b3, "maker_base_units": maker_units}
        for rpc in (self.buyer, self.maker):
            address = rpc.getnewaddress()
            self.nodes[0].sendtoaddress(address, Decimal(buyer_b3 + 2) if rpc is self.buyer else Decimal("3"))
            if rpc is self.maker:
                self.nodes[0].sendasset(asset, maker_units, address)
        self.synchronize_mempools()
        self.mine_pos_blocks(1, allow_overshoot=True)
        self.wait_client_b3_sync()
        deposits = []
        for rpc, currency, amount in ((self.buyer, "B3", Decimal(buyer_b3)), (self.maker, asset, maker_units)):
            deposit = rpc.flowmeshdeposit(asset, currency, amount)
            self.publish_client_transaction(deposit)
            deposits.append((rpc, deposit))
        self.mine_pos_blocks(30, allow_overshoot=True)
        self.wait_client_b3_sync()
        for rpc, deposit in deposits:
            response = rpc.submitflowmeshdeposit(market, deposit["deposit_txid"], deposit["deposit_vout"])
            self.certify(rpc, market, response, {})
        operations = self.publish_until_vault_operations(market, 2)
        assert_equal(len(operations), 2)
        for operation in operations:
            assert_equal(operation["kind"], "deposit-sweep")
            self.publish_client_transaction(self.maker.createflowmeshvaulttx(operation["effect_id"]))
        self.wait_for_market_convergence(market)
        self.authenticated_account(self.buyer, market, lambda row: row["b3_available_atoms"] == buyer_b3 * B3_ATOMS)
        self.authenticated_account(self.maker, market, lambda row: row["base_available"] == maker_units)

    def start_capture(self):
        assert not self.capture_nodes
        self.capture_clock = None
        for node in [*self.nodes, self.client]:
            assert not any(node.logging().values()), "measured run must have debug categories disabled"
            before = host_us()
            started = node.flowmeshtiming("start")
            after = host_us()
            if node is self.client:
                # Host time minus client span time, bracketed by this call.
                self.capture_clock = (before - started["start_us"], after - started["start_us"])
            self.capture_nodes.append(node)

    def stop_capture(self, number):
        result = {}
        while self.capture_nodes:
            node = self.capture_nodes.pop(0)
            capture = node.flowmeshtiming("stop")
            path = Path(self.options.tmpdir, f"persistent-trade-{number}-node{node.index}-timing.json")
            path.write_text(json.dumps(capture, indent=2) + "\n", encoding="utf-8")
            assert_equal(capture["dropped"], 0)
            limits = [row for row in capture["events"] if row["event"].get("stage") == "trace_limit_reached"]
            assert not (self.strict_samples and limits)
            # A capped stream is absent from every later capture of this node.
            self.trade_report["trace_limits_reached"] += [{"sample": number, "node": node.index,
                "stream": row["stream"], "substream": row["event"].get("stream")} for row in limits]
            result[str(node.index)] = capture
        return result

    def measured_window(self, events, requests, bid):
        """Client HTTPS requests of the measured bid window.

        The window runs from the original order RPC start to the status reply
        that returned certified inclusion. A request counts when its outermost
        client span is one of the order/status entry points (for the bid's
        action, where the span names one) and can overlap the window, with
        client span times mapped to host time by this capture's bracketed
        offset. The neighbouring client calls with HTTPS are account reads
        (Data), which never count.
        """
        low, high = self.capture_clock
        spans = {row["span_id"]: row for row in events if "span_id" in row}
        def outermost(row):
            while row["parent_span_id"] in spans:
                row = spans[row["parent_span_id"]]
            return row
        def measured(row):
            root = outermost(row)
            return (root["stage"] in ("Market", "Submit", "ActionStatus", "client_action_wait") and
                    root.get("action_id", bid["action_id"]) == bid["action_id"] and
                    root["started_us"] + low <= bid["certified_host_us"] and
                    root["monotonic_us"] + high >= bid["started_host_us"])
        def caller(row):
            parent = spans.get(row["parent_span_id"], {})
            return parent.get("method", parent.get("stage", "unattributed"))
        inside = [row for row in requests if measured(row)]
        handshakes = sorted(caller(row) for row in inside if "tls_handshake_done_us" in row)
        return {"clock_offset_us_bounds": [low, high], "requests": len(inside),
                "reused": sum(row.get("connection_reused", 0) for row in inside),
                "handshakes": len(handshakes), "handshake_callers": handshakes,
                "callers": dict(Counter(caller(row) for row in inside))}

    def qualify_match(self, market, price, number):
        buyer_before = self.authenticated_account(self.buyer, market)["account"]
        maker_before = self.authenticated_account(self.maker, market)["account"]
        sample = {"number": number, "price": price, "quantity": 1,
                  "buyer_before": buyer_before, "maker_before": maker_before, "complete": False}
        self.trade_report["samples"].append(sample)
        self.start_capture()
        try:
            ask = self.submit_order(self.maker, market, "ask", price, maker_before["next_sequence"])
            sample["ask"] = ask
            self.authenticated_account(self.maker, market, lambda row:
                row["next_sequence"] == maker_before["next_sequence"] + 1 and row["base_reserved"] == 1)
            # Deliberately keep B3 progressing in each pair, before the measured
            # buyer call. The price schedule and ordering are identical per arm.
            self.wait_for_new_b3_block(self.nodes[0].getblockcount())
            bid = self.submit_order(self.buyer, market, "bid", price, buyer_before["next_sequence"])
            sample["bid"] = bid
            buyer_after = self.authenticated_account(self.buyer, market, lambda row:
                row["next_sequence"] == buyer_before["next_sequence"] + 1)["account"]
            maker_after = self.authenticated_account(self.maker, market, lambda row: row["base_reserved"] == 0)["account"]
            observed = host_us()
            fee = price // 10_000
            assert_equal([buyer_after[k] for k in BALANCES], [buyer_before["base_available"] + 1,
                0, buyer_before["b3_available_atoms"] - price, 0])
            assert_equal([maker_after[k] for k in BALANCES], [maker_before["base_available"] - 1,
                0, maker_before["b3_available_atoms"] + price - fee, 0])
            sample.update(buyer_after=buyer_after, maker_after=maker_after, fee_atoms=fee,
                account_state_verified_ms=(observed - bid["started_host_us"]) / 1000)
            status = bid["certified_status"]
            target = {"sequence": status["microblock_sequence"], "hash": status["microblock_hash"]}
            native = {}
            def all_replicas_match():
                self.pump_b3()
                for index, node in enumerate(self.nodes):
                    if index in native:
                        continue
                    data = node.getflowmeshmarketdata(market, {"limit": 100, "curve_limit": 1})
                    matches = [row for row in data["history"]["entries"] if row["sequence"] == target["sequence"]]
                    if not matches:
                        continue
                    assert_equal(len(matches), 1)
                    row = matches[0]
                    assert_equal(row["microblock_hash"], target["hash"])
                    assert_equal((row["cleared"], row["price"], row["quantity"], row["notional_atoms"], row["fee_atoms"]),
                                 (True, price, 1, price, fee))
                    native[index] = row
                return len(native) == 4
            self.wait_until(all_replicas_match, timeout=60, check_interval=.01)
            sample.update(native_history=[native[index] for index in range(4)],
                          all_replicas_observed_ms=(host_us() - bid["started_host_us"]) / 1000)
            for rpc, action in ((self.maker, ask), (self.buyer, bid)):
                before = self.saved_actions(rpc, market)
                receipt = rpc.retryflowmeshaction(market, action["action_id"])
                assert_equal(receipt["receipt_state"], "certified_inclusion")
                after = self.saved_actions(rpc, market)
                assert_equal(set(before), set(after))
                for key in IMMUTABLE_ACTION:
                    assert_equal(after[action["action_id"]][key], before[action["action_id"]][key])
            sample["complete"] = True
        finally:
            captures = self.stop_capture(number)
            events = [row["event"] for row in captures.get("4", {}).get("events", [])]
            requests = [row for row in events if row.get("stage") == "https_request"]
            submissions = Counter(row["action_id"] for row in events
                if row.get("stage") == "client_call" and row.get("method") == "submit")
            sample["HTTPS"] = {"requests": len(requests),
                "handshakes": sum("tls_handshake_done_us" in row for row in requests),
                "reused": sum(row.get("connection_reused", 0) for row in requests),
                "submit_calls_by_action": dict(submissions)}
            if "bid" in sample:
                submit_spans = {row["span_id"] for row in events if row.get("stage") == "client_call" and
                    row.get("method") == "submit" and row.get("action_id") == sample["bid"]["action_id"]}
                bid_requests = sorted((row for row in requests if row["parent_span_id"] in submit_spans),
                                      key=lambda row: row["started_us"])
                sample["HTTPS"]["measured_bid_submit"] = [{"span_id": row["span_id"],
                    "parent_span_id": row["parent_span_id"], "reused": bool(row.get("connection_reused", 0)),
                    "handshake_performed": "tls_handshake_done_us" in row} for row in bid_requests]
                sample["HTTPS"]["measured_bid_submit_warm"] = bool(bid_requests) and bid_requests[0].get("connection_reused") == 1
                sample["HTTPS"]["measured_window"] = self.measured_window(events, requests, sample["bid"])
            for key in ("ask", "bid"):
                if key in sample:
                    action = sample[key]
                    assert_equal(submissions[action["action_id"]], 1 + len(action["exact_action_retries"]))
            if sample["complete"]:
                assert requests
            if sample["complete"] and self.options.transport_arm == "baseline":
                # A close-per-request daemon handshakes on every request, so
                # no rollover or reset can excuse reuse: these hold in every
                # mode, and a reusing daemon cannot pass under this label.
                assert_equal(sample["HTTPS"]["handshakes"], len(requests))
                assert_equal(sample["HTTPS"]["reused"], 0)
                assert bid_requests and "tls_handshake_done_us" in bid_requests[0]
            # In record mode the flags above are the warm arm's record, and a
            # sample that paid a handshake stays in every distribution instead
            # of ending the run.
            if sample["complete"] and self.options.transport_arm == "warm" and self.strict_samples:
                assert sample["HTTPS"]["reused"] > 0, "persistent arm did not reuse HTTPS"
                assert bid_requests and bid_requests[0].get("connection_reused") == 1, "measured original bid submit was not warm"

    def write_report(self):
        path = Path(self.options.tmpdir, "persistent-trades.json")
        path.write_text(json.dumps(self.trade_report, indent=2, sort_keys=True, default=str) + "\n", encoding="utf-8")

    def record_provenance(self):
        # Complete this before capturing any trade timing. Retained local-only
        # arguments and hashes make the unchanged/patched arms reproducible.
        commands = [{"node": node.index, "argv": list(node.process.args)}
                    for node in [*self.nodes, self.client]]
        hashes = {}
        for path in {str(Path(__file__).resolve()), *(row["argv"][0] for row in commands)}:
            digest = hashlib.sha256()
            with Path(path).open("rb") as source:
                for chunk in iter(lambda: source.read(1024 * 1024), b""):
                    digest.update(chunk)
            hashes[path] = digest.hexdigest()
        self.trade_report["provenance"] = {"invocation": list(sys.argv),
            "randomseed": self.options.randomseed, "processes": commands, "sha256": hashes}

    def run_test(self):
        try:
            market, asset = self.bootstrap_latency_market()
            self.trade_report.update(market_id=market, asset_id=asset)
            self.fund_wallets(market, asset)
            self.record_provenance()
            before = [node.getblockcount() for node in self.nodes]
            self.trade_report["B3_heights_before"] = before
            self.begin_b3_workload(range(4))
            try:
                for number, price in enumerate(self.prices):
                    self.qualify_match(market, price, number)
                    self.write_report()
            finally:
                self.end_b3_workload()
            after = [node.getblockcount() for node in self.nodes]
            self.trade_report["B3_heights_after"] = after
            assert all(end > start for start, end in zip(before, after))
            samples = self.trade_report["samples"]
            assert_equal(len(samples), len(self.prices))
            assert all(row["complete"] for row in samples)
            assert_equal(len({row["bid"]["action_id"] for row in samples}), len(self.prices))
            for rpc in (self.buyer, self.maker):
                saved = self.saved_actions(rpc, market)
                orders = [row for row in saved.values() if "sequence" in row]
                assert_equal(len(orders), len(self.prices))
                assert_equal(len({row["sequence"] for row in orders}), len(self.prices))
            self.assert_engine_off()
            self.assert_no_b3_flowmesh_traffic()
            if self.options.transport_arm == "warm":
                assert any(row["HTTPS"]["reused"] for row in samples), "persistent arm did not reuse HTTPS"
            reconnected = [row["HTTPS"]["measured_window"]["handshakes"] > 0 for row in samples]
            self.trade_report["summary"] = {
                "client_certified": distribution([row["bid"]["client_certified_ms"] for row in samples], 200),
                "account_state_verified": distribution([row["account_state_verified_ms"] for row in samples], 200),
                "all_replicas_observed": distribution([row["all_replicas_observed_ms"] for row in samples], 200),
                "matched_units": len(samples), "total_fees_atoms": sum(row["fee_atoms"] for row in samples),
                "transport": {"measured_bid_submit_warm": sum(row["HTTPS"]["measured_bid_submit_warm"] for row in samples),
                    "measured_window_reconnected": sum(reconnected),
                    "measured_window_handshakes": sum(row["HTTPS"]["measured_window"]["handshakes"] for row in samples),
                    "client_certified_without_handshake": distribution([row["bid"]["client_certified_ms"]
                        for row, paid in zip(samples, reconnected) if not paid], 200),
                    "client_certified_with_handshake": distribution([row["bid"]["client_certified_ms"]
                        for row, paid in zip(samples, reconnected) if paid], 200)}}
            advanced = [row["bid"]["b3"]["advanced_in_window"] for row in samples]
            overdue = max(row[key]["b3"]["status_read_max_overdue_ms"] for row in samples for key in ("ask", "bid"))
            self.trade_report["summary"]["b3_during_bid"] = {
                "windows_with_pump_tick": sum(row["bid"]["b3"]["pump_ticks_in_window"] > 0 for row in samples),
                "pump_ticks": sum(row["bid"]["b3"]["pump_ticks_in_window"] for row in samples),
                "windows_with_b3_advance": sum(advanced),
                "status_read_max_overdue_ms": overdue,
                "client_certified_with_b3_advance": distribution([row["bid"]["client_certified_ms"]
                    for row, moved in zip(samples, advanced) if moved], 200),
                "client_certified_without_b3_advance": distribution([row["bid"]["client_certified_ms"]
                    for row, moved in zip(samples, advanced) if not moved], 200)}
            if self.status_wait_ms:
                # The waited arm must keep the poll arm's B3 cadence.
                assert overdue < PUMP_OVERDUE_BOUND_MS, f"a waited status read held the B3 pump {overdue:.1f} ms past its tick"
            self.trade_report["correctness_pass"] = True
            self.trade_report["performance_measurement_qualified"] = not self.options.harness_smoke
        except Exception as error:
            self.trade_report["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            self.write_report()

    def shutdown(self):
        processes = [(node.index, node.process) for node in [*self.nodes, *([self.client] if self.client else [])]
                     if node.process is not None]
        try:
            return super().shutdown()
        finally:
            if Path(self.options.tmpdir).exists():
                self.trade_report["child_exits"] = [{"node": index, "pid": process.pid, "returncode": process.poll()}
                                                   for index, process in processes]
                self.write_report()


if __name__ == "__main__":
    FlowMeshPersistentTradesTest(__file__).main()
