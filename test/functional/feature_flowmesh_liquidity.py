#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Bounded generated-regtest liquidity and genuine-fill chart fixture.

Reuse only the latency fixture's fresh bootstrap/funding helpers. Six actual
matched auctions and two final non-crossing curves use the unchanged native V1
pre-agreement runtime. This is seeded test liquidity, not real market makers,
P2FV node integration, execution-proof verification by the client, or a latency
qualification. History has sequence coordinates, not execution timestamps.

No daemon is retained by default. --qt-review-hold-seconds explicitly requests
the existing bounded manual-review window; the harness never launches Qt.
"""

import hashlib
import http.client
import json
import ssl
import struct
import time
from pathlib import Path

from feature_flowmesh_latency import FlowMeshLatencyTest, PRE_ADMISSION_REJECTIONS
from feature_flowmesh_release import TEST_ASSET_DEPOSIT
from flowmesh_public_consistency_analyze import ENTRY_HEADER, ENTRY_TAIL, compact_size, decode_entry_prefix
from test_framework.authproxy import JSONRPCException
from test_framework.messages import hash256, ser_string
from test_framework.test_framework import TestStatus
from test_framework.util import assert_equal


# B3 atoms per raw base unit, and raw base quantity. Total cost is 0.475 B3.
FILLS = ((40_000_000, 1), (50_000_000, 2), (60_000_000, 1),
         (45_000_000, 2), (55_000_000, 1), (65_000_000, 2))
RESTING_BID = (30_000_000, 3)
RESTING_ASK = (80_000_000, 5)
TRADE_FIELDS = ("sequence", "microblock_hash", "epoch", "anchor_height", "price",
                "quantity", "notional_atoms", "fee_atoms")
BALANCE_FIELDS = ("account_id", "next_sequence", "base_available", "base_reserved",
                  "b3_available_atoms", "b3_reserved_atoms")


def cleared_rows(data):
    return sorted(({key: row[key] for key in TRADE_FIELDS}
                   for row in data["history"]["entries"] if row["cleared"]),
                  key=lambda row: row["sequence"])


def native_action_entry(receipt, market, domain):
    """Bind exact semantic membership to the native runtime's certified hash.

    This bounded decoder does not verify BLS; all four native replicas must
    independently expose the resulting hash as locally verified execution.
    Reuse the existing public commitment decoder, then check its action root.
    """
    prefix = decode_entry_prefix(receipt["certified_payload"])
    assert_equal((prefix["market_id"], prefix["domain"], prefix["kind"]), (market, domain, 1))
    assert_equal((prefix["sequence"], prefix["entry_hash"]),
                 (receipt["microblock_sequence"], receipt["microblock_hash"]))
    payload = bytes.fromhex(receipt["certified_payload"])
    entry = payload[4:4 + prefix["entry_bytes"]]
    count, position = compact_size(entry, ENTRY_HEADER)
    actions_start, actions_end = position, len(entry) - ENTRY_TAIL

    def take(size):
        nonlocal position
        assert position + size <= actions_end, "Truncated native action"
        value = entry[position:position + size]
        position += size
        return value

    ids = []
    for _ in range(count):
        identity_prefix = take(41)  # signer, sequence, type
        points, position = compact_size(entry, position)
        assert points <= 64
        curve = take(16 * points)
        suffix = take(108)  # asset, amount, destination, deposit outpoint
        credential_size, position = compact_size(entry, position)
        assert_equal(credential_size, 0)  # Certified bodies exclude credentials.
        identity = ser_string(b"b3/flowmesh/action/v2") + identity_prefix + struct.pack("<Q", points) + curve + suffix
        ids.append(hash256(identity)[::-1].hex())
    assert_equal(position, actions_end)
    assert_equal(len(set(ids)), len(ids))
    assert_equal(ids.count(receipt["action_id"]), 1)
    tag = hashlib.sha256(b"B3/FLOWMESH/ACTIONS/V1").digest()
    root = hashlib.sha256(tag + tag + struct.pack("<Q", count) + entry[actions_start:actions_end]).digest()
    assert_equal(root, entry[actions_end:actions_end + 32])
    # This fixture has exactly four seats; check framing/context, not BLS.
    certificate = payload[4 + len(entry):]
    assert_equal(len(certificate), 145)
    assert_equal(struct.unpack_from(">QQ", certificate), (prefix["epoch"], prefix["sequence"]))
    assert_equal(certificate[16:48][::-1].hex(), prefix["entry_hash"])
    assert certificate[48] & 0xf0 == 0 and bin(certificate[48]).count("1") >= 3
    return {**prefix, "exact_action_membership_checked": True, "actions_root_checked": True,
            "certificate_authentication": "native replicas, not this Python decoder"}


class FlowMeshLiquidityTest(FlowMeshLatencyTest):
    def set_test_params(self):
        # This campaign does not join benchmark timestamps or sample latency.
        self.options.latency_production_logging = True
        super().set_test_params()
        self.child_records = []
        self.bootstrap_deposit = None
        self.liquidity_report = {
            "format_version": 1, "profile": "generated_regtest_v1_liquidity",
            "checks_complete": False, "complete": False,
            "generated_test_liquidity": True, "real_market_maker_liquidity": False,
            "p2fv_node_integration_qualified": False, "latency_qualified": False,
            "trade_b3_settlement_qualified": False,
            "qt_exercised_by_harness": False, "actions": [], "transactions": [],
            "snapshots": [], "matched_fills": [],
            "units": {"quote": "B3", "quote_decimals": 9,
                      "prices": "B3 atoms per raw base unit", "quantities": "raw base units",
                      "history_axis": "certified microblock sequence; no execution timestamps"},
            "side_meanings": {
                "bid": {"canonical": "Buy base / Sell B3", "reverse_view": "Sell B3"},
                "ask": {"canonical": "Sell base / Buy B3", "reverse_view": "Buy B3"}},
            "evidence_scope": "Operator execution plus all-replica certified state/root and history agreement; engine-off client authenticates state/curves but history remains endpoint-reported",
            "client_action_samples": self.client_results, "native_action_receipts": [],
        }

    def observe_action(self, *args, **kwargs):
        # Keep the inherited exact-byte retry/verification path and raw report,
        # but do not print its large timing/trace JSON in this non-latency run.
        def concise(record):
            return record.msg != "FLOWMESH_REMOTE_ACTION %s"

        self.log.addFilter(concise)
        try:
            sample = super().observe_action(*args, **kwargs)
        finally:
            self.log.removeFilter(concise)
        self.log.info("FLOWMESH_LIQUIDITY_ACTION label=%s action_id=%s state=%s",
                      sample["label"], sample["action_id"], sample["status"]["receipt_state"])
        return sample

    def native_action_status(self, market, action_id):
        # Existing public read-only API, against only this generated node0 and
        # its generated CA. A deposit has no action signer, so the wallet-owned
        # status RPC cannot associate its runtime event with a wallet account.
        connection = http.client.HTTPSConnection("127.0.0.1", self.api_ports[0], timeout=10,
            context=ssl.create_default_context(cafile=str(self.pki["ca"])))
        try:
            connection.request("POST", "/flowmesh/v1", body=json.dumps({"method": "action",
                "params": {"market_id": market, "action_id": action_id}}),
                headers={"Content-Type": "application/json"})
            reply = connection.getresponse()
            body = reply.read(5 * 1024 * 1024 + 1)
            assert_equal(reply.status, 200)
            assert len(body) <= 5 * 1024 * 1024, "Native action response exceeded bound"
            decoded = json.loads(body)
            assert_equal(decoded["ok"], True)
            return decoded["result"]
        finally:
            connection.close()

    def remember_children(self):
        known = {id(row["process"]) for row in self.child_records}
        for node in [*self.nodes, *([self.client] if self.client else [])]:
            if node.process is not None and id(node.process) not in known:
                self.child_records.append({"node": node.index, "process": node.process,
                                           "log": node.debug_log_path})

    def start_nodes(self, *args, **kwargs):
        try:
            return super().start_nodes(*args, **kwargs)
        finally:
            self.remember_children()

    def start_node(self, *args, **kwargs):
        try:
            return super().start_node(*args, **kwargs)
        finally:
            self.remember_children()

    def start_ordinary_client(self):
        try:
            return super().start_ordinary_client()
        finally:
            self.remember_children()

    def configure_fresh_market(self, market_id):
        # Capture the one original bootstrap deposit before the shared helper
        # mines it. Do not fabricate another deposit or credit an arbitrary tx.
        matches = []
        node = self.nodes[0]
        for txid in node.getrawmempool():
            raw = node.getrawtransaction(txid)
            decoded = node.decoderawtransaction(raw)
            assert_equal(decoded["txid"], txid)
            for output in decoded["vout"]:
                parsed = self.parse_vault_script(output["scriptPubKey"]["hex"])
                if parsed and parsed["kind"] == 1:
                    matches.append({"txid": txid, "hex": raw, "deposit_txid": txid,
                                    "deposit_vout": output["n"], "parsed": parsed})
        assert_equal(len(matches), 1)
        self.bootstrap_deposit = matches[0]
        assert_equal(self.bootstrap_deposit["parsed"]["amount"], TEST_ASSET_DEPOSIT)
        super().configure_fresh_market(market_id)

    def publish_client_transaction(self, transaction):
        # Also used for the operator's exact already-created sweep. Only the
        # submitted transaction's own identity can satisfy this confirmation.
        assert_equal(self.nodes[0].decoderawtransaction(transaction["hex"])["txid"], transaction["txid"])
        first_height = self.nodes[0].getblockcount() + 1
        super().publish_client_transaction(transaction)
        height = self.transaction_height(self.nodes[0], transaction["txid"], first_height)
        block = self.nodes[0].getblockhash(height)
        for node in [*self.nodes, self.client]:
            assert_equal(node.getblockhash(height), block)
            assert transaction["txid"] in node.getblock(block)["tx"]
        self.liquidity_report["transactions"].append({"txid": transaction["txid"],
            "signed_bytes_sha256": hashlib.sha256(bytes.fromhex(transaction["hex"])).hexdigest(),
            "confirmed_height": height, "confirmed_block": block, "confirmed_on_processes": 5})

    def market_data(self, node, market):
        return node.getflowmeshmarketdata(market, {"limit": 100, "curve_limit": 128})

    def certify_action(self, node, market, method, params, label, sequence=None):
        if node is self.client:
            sample = self.observe_action(market, method, params, label, expect_sequence=sequence)
            status = sample["status"]
        else:
            response = None

            def submit_once_admitted():
                nonlocal response
                try:
                    response = getattr(node, method)(*params)
                    return True
                except JSONRPCException as error:
                    # Only an explicit pre-admission failure permits calling
                    # the signing RPC again. Never regenerate an unknown vote.
                    if error.error.get("code") != -1 or error.error.get("message") not in PRE_ADMISSION_REJECTIONS:
                        raise
                    return False

            self.wait_until(submit_once_admitted, timeout=30, check_interval=.1)
            assert response["receipt_state"] in {"queued", "admitted", "unknown", "certified_inclusion"}, response
            if sequence is not None:
                assert_equal(response["sequence"], sequence)
            action_id = response["action_id"]
            status = None
            native = {"label": label, "action_id": action_id, "initial_response": response,
                      "source": "generated_operator_native_runtime", "status": None}
            self.liquidity_report["native_action_receipts"].append(native)

            def included():
                nonlocal status
                status = self.native_action_status(market, action_id)
                native["status"] = status
                assert_equal(status["action_id"], action_id)
                assert status["receipt_state"] in {"queued", "admitted", "unknown", "certified_inclusion"}, status
                # Poll this exact ActionId. Unknown/timeout cannot authorize a
                # replacement submission or another deposit.
                return status["receipt_state"] == "certified_inclusion"

            self.wait_until(included, timeout=90, check_interval=.1)
            native["entry_check"] = native_action_entry(status, market, self.market_data(node, market)["domain"])
            sample = {"label": label, "action_id": action_id, "initial_response": response, "status": status}
        assert_equal(status["certificate_verified"], True)
        assert_equal(status["outcome_verified"], False)
        target = (status["microblock_sequence"], status["microblock_hash"])
        def applied_everywhere():
            for replica in self.nodes:
                data = self.market_data(replica, market)
                assert_equal(data["verification"]["source"], "local_engine")
                assert_equal(data["verification"]["certificate_verified"], True)
                assert_equal(data["verification"]["execution_result_verified"], True)
                if not self.contains_target(data, *target):
                    return False
            return True

        self.wait_until(applied_everywhere, timeout=90, check_interval=.1)
        self.liquidity_report["actions"].append({"actor": "engine_off_buyer" if node is self.client else "operator_seller",
            "label": label, "action_id": sample["action_id"], "account_sequence": sequence,
            "certified_sequence": target[0], "certified_hash": target[1], "applied_replicas": 4})
        self.drain_relay_observations()
        return status

    def capture(self, market, label):
        last = None

        def agreed():
            nonlocal last
            operators = [self.market_data(node, market) for node in self.nodes]
            remote = self.market_data(self.client, market)
            all_data = [*operators, remote]
            for data in all_data:
                assert_equal(data["verification"]["certificate_verified"], True)
                assert_equal(data["verification"]["account_state_verified"], True)
                assert_equal(data["snapshot"]["halt"], "none")
                assert_equal(data["liquidity"]["complete"], True)
                assert_equal(data["liquidity"]["total_curves"], len(data["liquidity"]["curves"]))
            assert_equal(remote["verification"]["source"], "remote_endpoint")
            assert_equal(remote["verification"]["execution_result_verified"], False)
            assert_equal(remote["verification"]["history_endpoint_reported"], True)
            assert all(data["verification"]["execution_result_verified"] for data in operators)
            identities = {(data["snapshot"]["last_microblock_hash"], data["snapshot"]["state_root"],
                           data["snapshot"]["next_microblock_sequence"]) for data in all_data}
            if len(identities) != 1:
                return False
            for data in all_data[1:]:
                assert_equal(data["liquidity"]["curves"], operators[0]["liquidity"]["curves"])
                assert_equal(cleared_rows(data), cleared_rows(operators[0]))
            last = (operators, remote)
            return True

        self.wait_until(agreed, timeout=90, check_interval=.1)
        operators, remote = last
        seller, buyer = operators[0]["account"], remote["account"]
        self.liquidity_report["snapshots"].append({"label": label,
            "head": remote["snapshot"]["last_microblock_hash"], "state_root": remote["snapshot"]["state_root"],
            "certified_replicas": 4, "engine_off_state_verified": True,
            "seller": {key: seller[key] for key in BALANCE_FIELDS},
            "buyer": {key: buyer[key] for key in BALANCE_FIELDS},
            "curves": remote["liquidity"]["curves"], "cleared_entries": cleared_rows(remote)})
        return operators, remote

    def fund_seller(self, market, asset):
        deposit = self.bootstrap_deposit
        status = self.market_status(self.nodes[0], market)
        assert_equal(deposit["parsed"]["asset"], bytes.fromhex(asset)[::-1])
        assert_equal(deposit["parsed"]["vault"], bytes.fromhex(status["vault_id"])[::-1])
        assert_equal(self.nodes[0].gettransaction(deposit["txid"])["hex"], deposit["hex"])
        assert self.nodes[0].gettransaction(deposit["txid"])["confirmations"] >= 31
        self.certify_action(self.nodes[0], market, "submitflowmeshdeposit",
                            (market, deposit["deposit_txid"], deposit["deposit_vout"]), "seller_bootstrap_deposit")
        self.wait_until(lambda: self.market_data(self.nodes[0], market)["account"]["base_available"] == TEST_ASSET_DEPOSIT,
                        timeout=90, check_interval=.1)
        operations = self.publish_until_vault_operations(market, 1)
        assert_equal(len(operations), 1)
        sweep = operations[0]
        assert_equal(sweep["kind"], "deposit-sweep")
        assert_equal(sweep["deposit_txid"], deposit["txid"])
        assert_equal(sweep["deposit_vout"], deposit["deposit_vout"])
        assert_equal(sweep["account_id"], self.market_data(self.nodes[0], market)["account"]["account_id"])
        self.publish_client_transaction(self.nodes[0].createflowmeshvaulttx(sweep["effect_id"]))
        self.wait_until(lambda: not self.nodes[0].listflowmeshvaultoperations(market), timeout=60)
        assert_equal(self.nodes[0].gettxout(deposit["txid"], deposit["deposit_vout"]), None)

    def order(self, node, market, side, price, quantity, label):
        sequence = self.market_data(node, market)["account"]["next_sequence"]
        self.certify_action(node, market, "submitflowmeshorder",
                            (market, side, price, quantity, sequence), label, sequence)
        self.wait_until(lambda: self.market_data(node, market)["account"]["next_sequence"] == sequence + 1,
                        timeout=90, check_interval=.1)

    def run_test(self):
        # Retain generated evidence/wallets for inspection; this does not keep
        # their processes alive or authorize a later transaction/replay.
        self.options.nocleanup = True
        started = time.monotonic()
        market, asset = self.bootstrap_latency_market()
        self.liquidity_report.update(market_id=market, base_asset_id=asset,
            operator_subversions=[node.getnetworkinfo()["subversion"] for node in self.nodes])
        self.fund_latency_client(market, asset)
        self.fund_seller(market, asset)
        self.assert_engine_off()
        operators, remote = self.capture(market, "funded")
        assert_equal(operators[0]["account"]["base_available"], TEST_ASSET_DEPOSIT)
        assert_equal(operators[0]["account"]["b3_available_atoms"], 0)
        assert_equal(remote["account"]["base_available"], 0)
        assert_equal(remote["account"]["b3_available_atoms"], 1_000_000_000)
        assert_equal(remote["liquidity"]["curves"], [])
        assert_equal(cleared_rows(remote), [])
        before_height = self.nodes[0].getblockcount()
        total_quantity = total_notional = total_fee = 0
        for index, (price, quantity) in enumerate(FILLS):
            self.order(self.nodes[0], market, "ask", price, quantity, f"maker_ask_{index}")
            asks, resting = self.capture(market, f"resting_before_fill_{index}")
            assert_equal(len(resting["liquidity"]["curves"]), 1)
            curve = resting["liquidity"]["curves"][0]
            assert_equal(curve["side"], "ask")
            assert_equal(curve["remaining_quantity"], quantity)
            assert_equal(asks[0]["account"]["base_reserved"], quantity)
            self.order(self.client, market, "bid", price, quantity, f"matched_bid_{index}")
            operators, remote = self.capture(market, f"matched_{index}")
            total_quantity += quantity
            notional, fee = price * quantity, price * quantity // 10_000
            total_notional += notional
            total_fee += fee
            seller, buyer = operators[0]["account"], remote["account"]
            assert_equal(seller["base_available"], TEST_ASSET_DEPOSIT - total_quantity)
            assert_equal(seller["base_reserved"], 0)
            assert_equal(seller["b3_available_atoms"], total_notional - total_fee)
            assert_equal(buyer["base_available"], total_quantity)
            assert_equal(buyer["b3_available_atoms"], 1_000_000_000 - total_notional)
            assert_equal(buyer["b3_reserved_atoms"], 0)
            assert_equal(remote["liquidity"]["curves"], [])
            clears = cleared_rows(remote)
            assert_equal(len(clears), index + 1)
            fill = clears[-1]
            assert_equal((fill["price"], fill["quantity"], fill["notional_atoms"], fill["fee_atoms"]),
                         (price, quantity, notional, fee))
            buyer_fill = next(row for row in remote["history"]["entries"] if row["sequence"] == fill["sequence"])
            seller_fill = next(row for row in operators[0]["history"]["entries"] if row["sequence"] == fill["sequence"])
            assert_equal((buyer_fill["account_fills_known"], buyer_fill["account_bid_fill"], buyer_fill["account_ask_fill"]), (True, quantity, 0))
            assert_equal((seller_fill["account_fills_known"], seller_fill["account_bid_fill"], seller_fill["account_ask_fill"]), (True, 0, quantity))
            assert "timestamp" not in buyer_fill and "timestamp" not in seller_fill
            self.liquidity_report["matched_fills"].append(fill)
            # Advance real B3 between fills, not a fabricated trade clock.
            # This is not a concurrent-load or latency measurement.
            self.mine_pos_blocks(1, allow_overshoot=True)
            self.wait_client_b3_sync()

        self.order(self.client, market, "bid", *RESTING_BID, "final_resting_bid")
        self.order(self.nodes[0], market, "ask", *RESTING_ASK, "final_resting_ask")
        operators, remote = self.capture(market, "final_two_sided_book")
        assert_equal(len(remote["liquidity"]["curves"]), 2)
        curves = {row["side"]: row for row in remote["liquidity"]["curves"]}
        assert_equal(set(curves), {"bid", "ask"})
        for side, (price, quantity), account in (("bid", RESTING_BID, remote["account"]),
                                                ("ask", RESTING_ASK, operators[0]["account"])):
            curve = curves[side]
            assert_equal(curve["account_id"], account["account_id"])
            assert_equal((curve["status"], curve["filled_quantity"], curve["remaining_quantity"]), ("open", 0, quantity))
            assert {"price": price, "quantity": quantity} in curve["points"]
        assert RESTING_BID[0] < RESTING_ASK[0]
        assert_equal(remote["account"]["b3_reserved_atoms"], RESTING_BID[0] * RESTING_BID[1])
        assert_equal(remote["account"]["b3_available_atoms"], 1_000_000_000 - total_notional - RESTING_BID[0] * RESTING_BID[1])
        assert_equal(remote["account"]["base_available"], total_quantity)
        assert_equal(operators[0]["account"]["base_reserved"], RESTING_ASK[1])
        assert_equal(operators[0]["account"]["base_available"], TEST_ASSET_DEPOSIT - total_quantity - RESTING_ASK[1])
        assert_equal(operators[0]["account"]["b3_available_atoms"], total_notional - total_fee)
        assert_equal(cleared_rows(remote), self.liquidity_report["matched_fills"])
        assert all(data["snapshot"]["pending_actions"] == 0 for data in operators)
        assert all(data["snapshot"]["running"] and not data["snapshot"]["paused"] and
                   not data["snapshot"]["pending_handoff"] for data in operators)
        orders = [row for row in self.liquidity_report["actions"] if row["account_sequence"] is not None]
        assert_equal(len(orders), 2 * len(FILLS) + 2)
        assert_equal(len({row["action_id"] for row in self.liquidity_report["actions"]}),
                     len(self.liquidity_report["actions"]))
        assert self.nodes[0].getblockcount() > before_height
        self.assert_engine_off()
        self.assert_no_b3_flowmesh_traffic()
        self.drain_relay_observations()
        saved = {row["action_id"]: row for row in self.client.listflowmeshactions(market)["actions"]}
        client_actions = [row for row in self.liquidity_report["actions"] if row["actor"] == "engine_off_buyer"]
        assert_equal(len(client_actions), len(FILLS) + 1)
        assert_equal(len(self.client_results), len(FILLS) + 2)
        assert_equal(self.client_results[0]["label"], "latency_setup_deposit")
        assert_equal(set(saved), {row["action_id"] for row in self.client_results})
        for action in client_actions:
            original = saved[action["action_id"]]
            copies = [row for row in self.submit_records if row["action_id"] == action["action_id"]]
            assert copies and all(row["forwarded"] and not row["response_dropped"] for row in copies)
            exact = {row["action_hex"] for row in copies}
            assert_equal(len(exact), 1)
            assert_equal(hashlib.sha256(bytes.fromhex(exact.pop())).hexdigest(), original["signed_bytes_sha256"])
            action["exact_signed_bytes_preserved"] = True
        # Exact decoded PUBLIC RPC response, including honest remote-history
        # labels. It contains no wallet/TLS/RPC credentials or private keys.
        chart_path = Path(self.options.tmpdir, "flowmesh-liquidity-engine-off-market-data.json")
        chart_path.write_text(json.dumps(remote, sort_keys=True, indent=2, default=str) + "\n", encoding="utf-8")
        self.liquidity_report.update(checks_complete=True, matched_fill_count=len(cleared_rows(remote)),
            resting_bid_count=1, resting_ask_count=1, total_base_traded=total_quantity,
            total_notional_atoms=total_notional, total_fee_atoms=total_fee,
            certified_order_actions=len(orders), client_retained_actions=len(saved),
            final_orderbook=remote["liquidity"], final_buyer=remote["account"], final_seller=operators[0]["account"],
            B3_height_before_trades=before_height, B3_height_after_trades=self.nodes[0].getblockcount(),
            chart_input=str(chart_path), chart_input_sha256=hashlib.sha256(chart_path.read_bytes()).hexdigest(),
            client_deposit_actions=self.client_results[:1], elapsed_seconds=time.monotonic() - started)
        self.qt_review_hold(market)

    def shutdown(self):
        self.options.nocleanup = True
        cleanup_errors = []
        code = 1
        shutdown_error = None

        def attempt(node, phase, operation):
            try:
                operation()
                return True
            except Exception as error:
                cleanup_errors.append({"node": node.index, "phase": phase, "error": str(error)})
                return False

        try:
            self.remember_children()
            nodes = [*self.nodes, *([self.client] if self.client else [])]
            # Request all graceful stops before waiting, including after failures.
            for node in nodes:
                if node.running and node.rpc_connected:
                    attempt(node, "stop", lambda: node.stop_node(wait_until_stopped=False))
            for node in nodes:
                process = node.process
                if process is None:
                    continue
                if attempt(node, "wait", lambda: node.wait_until_stopped(timeout=30)):
                    continue
                # Only exact children of this fresh fixture. Each failed
                # operation must still permit later attempts and other children.
                # The initial wait failure prevents a forced exit passing.
                try:
                    if process.poll() is None:
                        attempt(node, "terminate", process.terminate)
                        if not attempt(node, "terminate_wait", lambda: process.wait(timeout=5)):
                            attempt(node, "kill", process.kill)
                            attempt(node, "kill_wait", lambda: process.wait(timeout=5))
                except Exception as error:
                    cleanup_errors.append({"node": node.index, "phase": "poll", "error": str(error)})
                finally:
                    for name in ("stdout", "stderr"):
                        stream = getattr(node, name)
                        if stream is not None:
                            attempt(node, "close_" + name, stream.close)
                    node.running = False
                    node.rpc_connected = False
                    node.process = None
            if cleanup_errors:
                self.success = TestStatus.FAILED
            code = super().shutdown()
        except BaseException as error:
            shutdown_error = error
            self.success = TestStatus.FAILED
            cleanup_errors.append({"node": None, "phase": "framework_shutdown", "error": str(error)})
            raise
        finally:
            rows = []
            for record in self.child_records:
                exit_code = None
                try:
                    exit_code = record["process"].poll()
                except Exception as error:
                    cleanup_errors.append({"node": record["node"], "phase": "final_poll", "error": str(error)})
                rows.append({"node": record["node"], "pid": record["process"].pid, "exit_code": exit_code})
            clean = bool(rows) and not cleanup_errors and all(row["exit_code"] == 0 for row in rows)
            complete = self.liquidity_report["checks_complete"] and clean and code == 0
            code = code if complete or code != 0 else 1
            self.liquidity_report.update(children=rows, cleanup_errors=cleanup_errors,
                all_daemon_children_clean=clean, complete=complete, harness_exit_code=code)
            try:
                Path(self.options.tmpdir, "flowmesh-liquidity.json").write_text(
                    json.dumps(self.liquidity_report, sort_keys=True, indent=2, default=str) + "\n", encoding="utf-8")
            except Exception as error:
                if shutdown_error is None:
                    raise
                # Failure to save evidence must not replace the original error.
                if hasattr(shutdown_error, "add_note"):
                    shutdown_error.add_note(f"Could not write child-exit evidence: {error}")
        return code


if __name__ == "__main__":
    FlowMeshLiquidityTest(__file__).main()
