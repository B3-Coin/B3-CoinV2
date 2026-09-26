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
"""

import hashlib
import json
import sys
import time
from collections import Counter
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


class FlowMeshPersistentTradesTest(FlowMeshLatencyTest):
    def add_options(self, parser):
        super().add_options(parser)
        parser.add_argument("--transport-arm", choices=("baseline", "warm"), required=True)
        parser.add_argument("--harness-smoke", action="store_true",
                            help="One match to validate the fixture; never a reported performance arm")

    def set_test_params(self):
        self.options.latency_production_logging = True
        super().set_test_params()
        self.prices = PRICES[:1] if self.options.harness_smoke else PRICES
        self.trade_report = {"format_version": 1, "arm": self.options.transport_arm,
            "harness_smoke": self.options.harness_smoke, "performance_measurement_qualified": False,
            "correctness_pass": False, "prices": list(self.prices), "samples": [], "child_exits": [],
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
            else:
                time.sleep(.005)
                status = rpc.getflowmeshactionstatus(market, action_id)
            assert_equal(status["action_id"], action_id)
        sample.update(certified_status=status, certified_host_us=host_us(), exact_action_retries=retries)
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
                sample.setdefault("started_host_us", host_us())
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
        for rpc in (self.buyer, self.maker):
            address = rpc.getnewaddress()
            self.nodes[0].sendtoaddress(address, Decimal("3"))
            if rpc is self.maker:
                self.nodes[0].sendasset(asset, 20, address)
        self.synchronize_mempools()
        self.mine_pos_blocks(1, allow_overshoot=True)
        self.wait_client_b3_sync()
        deposits = []
        for rpc, currency, amount in ((self.buyer, "B3", Decimal("1")), (self.maker, asset, 20)):
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
        self.authenticated_account(self.buyer, market, lambda row: row["b3_available_atoms"] == 1_000_000_000)
        self.authenticated_account(self.maker, market, lambda row: row["base_available"] == 20)

    def start_capture(self):
        assert not self.capture_nodes
        for node in [*self.nodes, self.client]:
            assert not any(node.logging().values()), "measured run must have debug categories disabled"
            node.flowmeshtiming("start")
            self.capture_nodes.append(node)

    def stop_capture(self, number):
        result = {}
        while self.capture_nodes:
            node = self.capture_nodes.pop(0)
            capture = node.flowmeshtiming("stop")
            path = Path(self.options.tmpdir, f"persistent-trade-{number}-node{node.index}-timing.json")
            path.write_text(json.dumps(capture, indent=2) + "\n", encoding="utf-8")
            assert_equal(capture["dropped"], 0)
            assert not any(row["event"].get("stage") == "trace_limit_reached" for row in capture["events"])
            result[str(node.index)] = capture
        return result

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
            for key in ("ask", "bid"):
                if key in sample:
                    action = sample[key]
                    assert_equal(submissions[action["action_id"]], 1 + len(action["exact_action_retries"]))
            if sample["complete"]:
                assert requests
                if self.options.transport_arm == "warm":
                    assert sample["HTTPS"]["reused"] > 0, "persistent arm did not reuse HTTPS"
                    assert bid_requests and bid_requests[0].get("connection_reused") == 1, "measured original bid submit was not warm"
                else:
                    assert_equal(sample["HTTPS"]["handshakes"], len(requests))
                    assert_equal(sample["HTTPS"]["reused"], 0)
                    assert bid_requests and "tls_handshake_done_us" in bid_requests[0]

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
            self.trade_report["summary"] = {
                "client_certified": distribution([row["bid"]["client_certified_ms"] for row in samples], 200),
                "account_state_verified": distribution([row["account_state_verified_ms"] for row in samples], 200),
                "all_replicas_observed": distribution([row["all_replicas_observed_ms"] for row in samples], 200),
                "matched_units": len(samples), "total_fees_atoms": sum(row["fee_atoms"] for row in samples)}
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
