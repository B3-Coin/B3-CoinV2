#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Canonical execution controls for the reverse B3 client presentation.

All assets, wallets and operators are newly generated regtest fixtures. The
inherited release market/history remains intact. A second, generated six-place
cUSD market uses ordinary issuance/bootstrap; no existing market is inverted.
RPC orders below are canonical control instructions, NOT a Qt/adapter test.
The opt-in bounded Qt hold exposes this same wallet for an attended screen test.
"""

from decimal import Decimal
from fractions import Fraction
import argparse
import hashlib
from io import BytesIO
import json
import os
from pathlib import Path
import struct
import time

from feature_flowmesh_remote_client import FlowMeshRemoteClientTest
from test_framework.key import verify_schnorr
from test_framework.messages import deser_compact_size, hash256, ser_string
from test_framework.flowmesh_qt_review import FlowMeshQtReviewController
from test_framework.util import assert_equal, p2p_port, rpc_port


B3_ATOMS = 1_000_000_000
ASSET_ATOMS = 1_000_000
CANONICAL_PRICE = 2_000  # 2 B3 / cUSD == displayed 0.5 cUSD / B3.


def decode_order(payload_hex, domain, config):
    """Independently verify the unchanged bounded V1 limit-curve instruction."""
    raw = bytes.fromhex(payload_hex)
    source = BytesIO(raw)
    signer = source.read(32)
    sequence, kind = struct.unpack("<QB", source.read(9))
    count = deser_compact_size(source)
    assert count <= 64
    points = [struct.unpack("<qq", source.read(16)) for _ in range(count)]
    suffix = source.read(32 + 8 + 32 + 36)
    credential = source.read(deser_compact_size(source))
    assert source.read() == b""
    assert len(signer) == 32 and len(suffix) == 108 and len(credential) == 96
    assert kind in (0, 1, 2, 3)
    assert count == (2 if kind in (0, 1) else 0)
    assert suffix == b"\0" * 104 + b"\xff" * 4
    identity = hash256(ser_string(b"b3/flowmesh/action/v2") + signer +
                       struct.pack("<QBQ", sequence, kind, count) +
                       b"".join(struct.pack("<qq", *point) for point in points) + suffix)
    signature_digest = hash256(ser_string(b"b3/flowmesh/action-sig/v2") +
                               bytes.fromhex(domain)[::-1] + bytes.fromhex(config)[::-1] + identity)
    assert hash256(ser_string(b"b3/flowmesh/account/v1") + credential[:32]) == signer
    assert verify_schnorr(credential[:32], credential[32:], signature_digest)
    return {"account_id": signer[::-1].hex(), "sequence": sequence, "action_type": kind,
            "points": [{"price": price, "quantity": quantity} for price, quantity in points],
            "action_id": identity[::-1].hex(), "signature_digest": signature_digest[::-1].hex(),
            "signed_bytes_sha256": hashlib.sha256(raw).hexdigest(), "signed_bytes_size": len(raw),
            "signature_verified": True, "canonical_zero_fields_verified": True}


class FlowMeshReverseClientTest(FlowMeshRemoteClientTest):
    def add_options(self, parser):
        super().add_options(parser)

        def bounded_hold(value):
            seconds = int(value)
            if not 0 <= seconds <= 1800:
                raise argparse.ArgumentTypeError("Reverse-view attended hold must be between 0 and 1800 seconds")
            return seconds

        # The existing fixture remains unchanged. This separately authorized
        # screen checklist allows at most thirty minutes, never an open-ended
        # operator lifetime, and still defaults to no attended hold.
        hold = next(action for action in parser._actions if action.dest == "qt_review_hold_seconds")
        hold.type = bounded_hold
        hold.help = "Retain only these generated operators/endpoints for bounded attended reverse-view Qt checks (0-1800, default 0)"

    def set_test_params(self):
        super().set_test_params()
        self.reverse_results = []
        self.reverse_market = None
        self.reverse_issuance = None
        self.reverse_sweeps = []

    def run_test(self):
        super().run_test()

    def write_artifact(self, name, value):
        Path(self.options.tmpdir, name).write_text(json.dumps(value, sort_keys=True, indent=2, default=str) + "\n")

    def create_reverse_test_market(self, inherited_market):
        n0, n1 = self.nodes[:2]
        inherited = self.market_status(n0, inherited_market)
        issued = n0.issueasset(100 * ASSET_ATOMS, 6)
        self.reverse_issuance = issued
        asset = issued["asset_id"]
        assert asset != inherited["base_asset_id"]
        bootstrap = n0.flowmeshdeposit(asset, asset, 20 * ASSET_ATOMS,
                                      {"market_bootstrap": True, "minconf": 0, "include_unsafe": True})
        market_id = bootstrap["market_id"]
        self.reverse_market = market_id
        assert market_id != inherited_market
        self.synchronize_mempools()
        self.mine_pos_blocks(31, allow_overshoot=True)
        self.wait_until(lambda: bool(self.market_status(n0, market_id)), timeout=120)
        self.wait_until(lambda: self.market_status(n0, market_id)["checkpoint_pending"], timeout=120)
        genesis = self.publish_checkpoint(market_id, prepare=True)
        assert_equal(genesis["sequence"], 0)
        self.wait_for_market_convergence(market_id)
        self.last_market = market_id
        self.start_ordinary_client()
        for node in [*self.nodes, self.client]:
            metadata = node.setassetmetadata(asset, "cUSD Regtest Unbacked", "cUSD", issued["hex"])
            assert_equal(metadata["asset_id"], asset)
            assert_equal(metadata["decimals"], 6)
            assert_equal(metadata["precision_known"], True)
        funding_address = self.client.getnewaddress()
        n0.sendtoaddress(funding_address, Decimal("30"))
        n0.sendasset(asset, 30 * ASSET_ATOMS, funding_address)
        self.synchronize_mempools()
        self.mine_pos_blocks(1, allow_overshoot=True)
        self.wait_client_b3_sync()
        native_operator = n1.flowmeshdeposit(asset, "B3", Decimal("20"))
        self.synchronize_mempools()
        self.mine_pos_blocks(1, allow_overshoot=True)
        self.wait_client_b3_sync()
        client_asset = self.prepare_and_publish_deposit(self.client, market_id, asset, asset, 20 * ASSET_ATOMS)
        self.publish_client_transaction(client_asset)
        client_native = self.prepare_and_publish_deposit(self.client, market_id, asset, "B3", Decimal("20"))
        self.publish_client_transaction(client_native)
        self.mine_pos_blocks(30, allow_overshoot=True)
        self.wait_client_b3_sync()
        n0.submitflowmeshdeposit(market_id, bootstrap["deposit_txid"], bootstrap["deposit_vout"])
        n1.submitflowmeshdeposit(market_id, native_operator["deposit_txid"], native_operator["deposit_vout"])
        self.observe_action(market_id, "submitflowmeshdeposit",
                            (market_id, client_asset["deposit_txid"], client_asset["deposit_vout"]), "reverse_client_asset_deposit")
        self.observe_action(market_id, "submitflowmeshdeposit",
                            (market_id, client_native["deposit_txid"], client_native["deposit_vout"]), "reverse_client_B3_deposit")
        balance = self.wait_client_balance(market_id, lambda row: row["base_available"] == 20 * ASSET_ATOMS and row["b3_available"] == 20)
        self.wait_until(lambda: n0.getflowmeshbalance(market_id)["account"]["base_available"] == 20 * ASSET_ATOMS and
                        n1.getflowmeshbalance(market_id)["account"]["b3_available"] == 20, timeout=120)
        # Existing checkpoint/sweep rules move all four connected deposits into
        # vault custody. This is explicit mock-time transaction injection by the
        # inherited fixture, not an ordinary B3 propagation benchmark.
        operations = self.publish_until_vault_operations(market_id, 4)
        assert_equal(len(operations), 4)
        for operation in operations:
            assert_equal(operation["kind"], "deposit-sweep")
            sweep = self.prepare_and_publish(n0, n0.createflowmeshvaulttx, operation["effect_id"], None)
            assert_equal(sweep["operation"], "deposit-sweep")
            self.reverse_sweeps.append(sweep)
        self.synchronize_mempools()
        sweep_first_height = n0.getblockcount() + 1
        self.mine_pos_blocks(1, allow_overshoot=True)
        self.wait_client_b3_sync()
        for sweep in self.reverse_sweeps:
            height = self.transaction_height(n0, sweep["txid"], sweep_first_height)
            sweep.update(connected_height=height, connected_block_hash=n0.getblockhash(height))
        self.wait_until(lambda: not n0.listflowmeshvaultoperations(market_id), timeout=120)
        metadata = self.client.getflowmeshmarketdata(market_id, {"limit": 1})["base_metadata"]
        assert_equal(metadata["decimals"], 6)
        self.write_artifact("reverse-market-fixture.json", {
            "inherited_market_id_preserved": inherited_market,
            "market_id": market_id, "canonical_base_asset_id": asset,
            "canonical_quote_asset_id": "00" * 32, "display_pair": "B3/cUSD",
            "generated_regtest_only": True, "asset_backing": "unbacked generated test money",
            "base_metadata": metadata, "issuance_txid": issued["txid"],
            "genesis_checkpoint": genesis, "initial_client_balance": balance,
            "operator_accounts": [node.getflowmeshbalance(market_id).get("account") for node in self.nodes],
            "inherited_identity_after": self.market_status(n0, inherited_market)["base_asset_id"],
            "manual_B3_transaction_injection": True,
        })
        return market_id, asset

    def checked_order(self, market_id, side, price, quantity, label, *, cancel=False):
        before = self.client_balance(market_id)
        method = "cancelflowmeshorder" if cancel else "submitflowmeshorder"
        params = ((market_id, side, before["next_sequence"]) if cancel else
                  (market_id, side, price, quantity, before["next_sequence"]))
        sample = self.observe_action(market_id, method, params, label, expect_sequence=before["next_sequence"])
        self.wait_client_balance(market_id, lambda row: row["next_sequence"] == before["next_sequence"] + 1)
        market = self.market_status(self.nodes[0], market_id)
        transmitted = [row for relay in self.tls_relays for row in relay.snapshot()["requests"]
                       if row["method"] == "submit" and row.get("action_id") == sample["action_id"]]
        assert transmitted
        assert len({row["action_hex"] for row in transmitted}) == 1
        decoded = decode_order(transmitted[0]["action_hex"], market["domain"], market["execution_config_id"])
        assert_equal(decoded["action_id"], sample["action_id"])
        assert_equal(decoded["sequence"], before["next_sequence"])
        assert_equal(decoded["action_type"], (2 if side == "bid" else 3) if cancel else (0 if side == "bid" else 1))
        expected_points = ([] if cancel else
                           [{"price": price, "quantity": quantity}, {"price": price + 1, "quantity": 0}] if side == "bid" else
                           [{"price": price - 1, "quantity": 0}, {"price": price, "quantity": quantity}])
        assert_equal(decoded["points"], expected_points)
        saved = next(row for row in self.client.listflowmeshactions(market_id)["actions"] if row["action_id"] == sample["action_id"])
        for field in ("action_id", "action_type", "sequence", "signed_bytes_sha256", "signed_bytes_size"):
            assert_equal(saved[field], decoded[field])
        assert_equal(saved["canonical_side"], side)
        assert_equal(saved["canonical_points"], decoded["points"])
        row = {"label": label, "canonical_side": side, "user_side": "Buy B3" if side == "ask" else "Sell B3",
               "cancel": cancel, "canonical_signed_instruction": decoded, "saved_public_summary": saved,
               "balance_before": before, "balance_after_admission": self.client_balance(market_id),
               "certified_inclusion": sample["status"], "Qt_exercised": False}
        if not cancel:
            row.update(display_limit_quote_per_B3=str(Fraction(B3_ATOMS, price * ASSET_ATOMS)),
                       quote_quantity=str(Fraction(quantity, ASSET_ATOMS)),
                       B3_notional_at_limit=str(Fraction(price * quantity, B3_ATOMS)))
        self.reverse_results.append(row)
        return row

    def exercise_reverse_controls(self, market_id, asset):
        n0, n1 = self.nodes[:2]
        market_before = self.market_status(n0, market_id)
        immutable = {field: market_before[field] for field in
                     ("domain", "market_id", "vault_id", "base_asset_id", "quote_asset", "execution_config_id")}
        self.begin_b3_workload(range(4))
        height_before = n0.getblockcount()
        buy = self.checked_order(market_id, "ask", CANONICAL_PRICE, 2 * ASSET_ATOMS, "Buy_B3_canonical_ask")
        reserved = self.client_balance(market_id)
        assert_equal(reserved["base_available"], 18 * ASSET_ATOMS)
        assert_equal(reserved["base_reserved"], 2 * ASSET_ATOMS)
        assert_equal(reserved["b3_available"], 20)
        self.submit_observed(market_id, 1, "bid", price=CANONICAL_PRICE, quantity=ASSET_ATOMS, label="Buy_B3_partial_counterparty")
        partial = self.wait_client_balance(market_id, lambda row: row["base_reserved"] == ASSET_ATOMS)
        assert_equal(partial["base_available"], 18 * ASSET_ATOMS)
        assert_equal(partial["b3_available"], Decimal("21.9998"))
        buy["partial_fill"] = {"balance": partial, "spent_quote_atoms": ASSET_ATOMS,
                               "received_B3_atoms_gross": 2 * B3_ATOMS, "actual_fee_asset": "B3",
                               "actual_fee_atoms": 200_000, "received_B3_atoms_net": 1_999_800_000}
        self.checked_order(market_id, "ask", None, None, "cancel_Buy_B3_releases_cUSD", cancel=True)
        cancelled = self.wait_client_balance(market_id, lambda row: row["base_reserved"] == 0)
        assert_equal(cancelled["base_available"], 19 * ASSET_ATOMS)
        assert_equal(cancelled["b3_available"], Decimal("21.9998"))
        buy["after_cancel"] = cancelled

        seller_before = n0.getflowmeshbalance(market_id)["account"]
        sell = self.checked_order(market_id, "bid", CANONICAL_PRICE, 2 * ASSET_ATOMS, "Sell_B3_canonical_bid")
        assert_equal(self.client_balance(market_id)["b3_reserved"], 4)
        self.submit_observed(market_id, 0, "ask", price=CANONICAL_PRICE, quantity=ASSET_ATOMS, label="Sell_B3_partial_counterparty")
        partial = self.wait_client_balance(market_id, lambda row: row["base_available"] == 20 * ASSET_ATOMS)
        assert_equal(partial["b3_reserved"], 2)
        assert_equal(partial["b3_available"], Decimal("17.9998"))
        seller_after = n0.getflowmeshbalance(market_id)["account"]
        assert_equal(seller_after["b3_available"] - seller_before["b3_available"], Decimal("1.9998"))
        sell["partial_fill"] = {"balance": partial, "spent_B3_atoms": 2 * B3_ATOMS,
                                "received_quote_atoms": ASSET_ATOMS, "client_trading_fee_atoms": 0,
                                "canonical_seller_fee_asset": "B3", "canonical_seller_fee_atoms": 200_000}
        self.checked_order(market_id, "bid", None, None, "cancel_Sell_B3_releases_B3", cancel=True)
        cancelled = self.wait_client_balance(market_id, lambda row: row["b3_reserved"] == 0)
        assert_equal(cancelled["b3_available"], Decimal("19.9998"))
        sell["after_cancel"] = cancelled

        # Canonical buyer gets a better (lower) B3/token price. Current V1
        # preserves the original reservation until cancellation/exhaustion;
        # reverse Sell B3 promises a maximum spend, not an exact B3 quantity.
        better = self.checked_order(market_id, "bid", CANONICAL_PRICE, 2 * ASSET_ATOMS, "Sell_B3_better_clearing")
        self.submit_observed(market_id, 0, "ask", price=CANONICAL_PRICE // 2, quantity=ASSET_ATOMS, label="better_clearing_counterparty")
        partial = self.wait_client_balance(market_id, lambda row: row["base_available"] == 21 * ASSET_ATOMS)
        assert_equal(partial["b3_reserved"], 3)
        assert_equal(partial["b3_available"], Decimal("15.9998"))
        better["better_clearing"] = {"balance": partial, "limit_price_raw": CANONICAL_PRICE,
                                     "clearing_price_raw": CANONICAL_PRICE // 2,
                                     "display_limit_quote_per_B3": "1/2", "display_clearing_quote_per_B3": "1",
                                     "received_quote_atoms": ASSET_ATOMS, "spent_B3_atoms": B3_ATOMS}
        self.checked_order(market_id, "bid", None, None, "cancel_better_price_releases_savings", cancel=True)
        cancelled = self.wait_client_balance(market_id, lambda row: row["b3_reserved"] == 0)
        assert_equal(cancelled["b3_available"], Decimal("18.9998"))
        better["after_cancel"] = cancelled
        self.wait_for_new_b3_block(height_before)
        self.end_b3_workload()
        self.wait_for_market_convergence(market_id)
        market_after = self.market_status(n0, market_id)
        assert_equal({field: market_after[field] for field in immutable}, immutable)
        self.write_artifact("reverse-market-identity.json", {"before": immutable,
                           "after": {field: market_after[field] for field in immutable},
                           "B3_height_before": height_before, "B3_height_after": n0.getblockcount(),
                           "consensus_config_changed": False})
        return buy["canonical_signed_instruction"]["action_id"]

    def verify_clean_restart(self, market_id):
        def local_saved_view():
            request_counts = [len(relay.snapshot()["requests"]) for relay in self.tls_relays]
            result = self.client.listflowmeshactions(market_id)
            assert_equal([len(relay.snapshot()["requests"]) for relay in self.tls_relays], request_counts)
            return result

        before = local_saved_view()
        account_before = self.client_balance(market_id)
        submissions_before = sum(row["method"] == "submit" for relay in self.tls_relays for row in relay.snapshot()["requests"])
        self.client.stop_node()
        self.client.start(extra_args=self.client_args)
        self.client.wait_for_rpc_connection()
        self.client_clock = None
        self.sync_client_time()
        self.client.addnode(f"127.0.0.1:{p2p_port(0)}", "onetry")
        self.wait_client_b3_sync()
        restored = local_saved_view()
        fields = ("market_id", "domain", "execution_config_id", "account_id", "action_id", "action_type", "sequence",
                  "signed_bytes_sha256", "signed_bytes_size", "initial_submission_ms", "may_have_been_sent", "previously_certified",
                  "canonical_side", "canonical_points")
        assert_equal([{key: row.get(key) for key in fields} for row in before["actions"]],
                     [{key: row.get(key) for key in fields} for row in restored["actions"]])
        self.assert_engine_off()
        account_after = self.client_balance(market_id)
        assert_equal(account_after, account_before)
        assert_equal(sum(row["method"] == "submit" for relay in self.tls_relays for row in relay.snapshot()["requests"]), submissions_before)
        self.write_artifact("reverse-client-reopen.json", {"same_wallet": True, "engine_off": True,
                            "before": before, "restored_before_remote_read": restored,
                            "account_before": account_before, "account_after": account_after,
                            "submit_delta": 0, "Qt_exercised": False})

    def settlement_control(self, market_id, asset):
        # Run01 demonstrated that account credit and completed trades do not
        # establish anchored POOL_CHANGE capacity: its sweeps were at329 and
        # its request at tip336 could only use anchor306. Match the inherited
        # release fixture's existing maturity preparation BEFORE signing a
        # first withdrawal in this new run. No rejected instruction is retried
        # or replaced and no idle certified head is forced to advance.
        n0 = self.nodes[0]
        assert len(self.reverse_sweeps) == 4
        mature_tip = max(sweep["connected_height"] for sweep in self.reverse_sweeps) + 30
        if n0.getblockcount() < mature_tip:
            self.mine_pos_blocks(mature_tip - n0.getblockcount(), allow_overshoot=True)
        self.wait_client_b3_sync()
        tip = n0.getblockcount()
        next_anchor_height = tip - 30
        sweep_evidence = []
        for sweep in self.reverse_sweeps:
            transaction = n0.gettransaction(sweep["txid"])
            assert transaction["confirmations"] >= 31
            assert_equal(transaction["blockhash"], sweep["connected_block_hash"])
            assert_equal(n0.getblockhash(sweep["connected_height"]), sweep["connected_block_hash"])
            assert sweep["connected_height"] <= next_anchor_height
            sweep_evidence.append({"txid": sweep["txid"], "connected_height": sweep["connected_height"],
                                   "connected_block_hash": sweep["connected_block_hash"],
                                   "confirmations": transaction["confirmations"], "eligible_at_next_anchor": True})
        market = self.market_status(n0, market_id)
        custody_atoms = self.custody(self.reverse_sweeps, asset, market["vault_id"])
        before = self.client_balance(market_id)
        wallet_before = self.wallet_asset(self.client, asset)["confirmed"]
        destination = self.client.getnewaddress()
        amount = ASSET_ATOMS // 10
        assert custody_atoms >= amount
        evidence = {"market_id": market_id, "asset_id": asset, "destination": destination,
                    "amount_atoms": amount, "before_request_B3_tip": tip,
                    "before_request_next_anchor_height": next_anchor_height,
                    "before_request_next_anchor_hash": n0.getblockhash(next_anchor_height),
                    "sweeps": sweep_evidence, "known_unspent_mature_asset_pool_atoms": custody_atoms,
                    "account_before_request": before,
                    "maturity_checked_before_signing": True,
                    "idle_certified_head_advance_required": False}
        self.write_artifact("reverse-withdrawal-evidence.json", evidence)
        try:
            action = self.observe_action(market_id, "requestflowmeshwithdrawal",
                                         (market_id, asset, amount, destination, before["next_sequence"]),
                                         "reverse_unchanged_withdrawal", expect_sequence=before["next_sequence"])
            evidence["original_action"] = {"action_id": action["action_id"], "receipt": action["status"],
                                           "sequence": before["next_sequence"]}
            evidence["account_at_inclusion_observation"] = self.client_balance(market_id)
            self.write_artifact("reverse-withdrawal-evidence.json", evidence)
            evidence["account_after_request"] = self.wait_client_balance(
                market_id, lambda row: row["base_available"] == before["base_available"] - amount)
        except Exception as error:
            evidence["failure"] = {"type": type(error).__name__, "message": str(error)}
            evidence["operator_failure_observations"] = []
            for index, node in enumerate(self.nodes):
                try:
                    evidence["operator_failure_observations"].append({"node": index,
                        "B3_tip": node.getblockcount(), "market": self.market_status(node, market_id),
                        "account": node.getflowmeshmarketdata(market_id, {"limit": 1, "curve_limit": 4}).get("account")})
                except Exception as observation_error:
                    evidence["operator_failure_observations"].append({"node": index, "error": str(observation_error)})
            self.write_artifact("reverse-withdrawal-evidence.json", evidence)
            raise
        self.write_artifact("reverse-withdrawal-evidence.json", evidence)
        operation = self.wait_vault_operation(market_id, before["account_id"], "withdrawal", asset)
        payout = self.client.createflowmeshvaulttx(operation["effect_id"], destination, {"broadcast": False})
        row = self.publish_parity_transaction(payout, 9)
        row.update(withdrawal_destination=destination, withdrawal_asset_id=asset, withdrawal_amount_atoms=amount)
        self.wait_until(lambda: self.wallet_asset(self.client, asset)["confirmed"] == wallet_before + amount, timeout=60)

    def qt_review_hold(self, market_id):
        self.write_artifact("reverse-qt-order-plan.json", {
            "market_id": market_id, "base_asset_id": self.reverse_issuance["asset_id"],
            "canonical_pair": "cUSD/B3", "display_pair": "B3/cUSD", "asset_decimals": 6,
            "canonical_limit_raw": CANONICAL_PRICE, "display_limit": "0.5",
            "client_balance": self.client_balance(market_id),
            "operator_RPC_ports": [rpc_port(index) for index in range(4)],
            "counterparty_for_Buy_B3": {"node": 1, "canonical_side": "bid", "price": CANONICAL_PRICE, "quantity": ASSET_ATOMS},
            "counterparty_for_Sell_B3": {"node": 0, "canonical_side": "ask", "price": CANONICAL_PRICE, "quantity": ASSET_ATOMS},
            "instructions": "Actual Qt must choose market, inspect review, confirm, observe receipt/account and cancel. Canonical RPC controls above are not screen qualification.",
        })
        seconds = self.options.qt_review_hold_seconds
        if not seconds:
            return
        self.options.nocleanup = True
        self.client.stop_node()
        complete = Path(self.options.tmpdir, "qt-review-complete")
        assert not complete.exists()
        settings = json.loads((self.client.chain_path / "settings.json").read_text())
        assert_equal(settings.get("wallet"), [self.default_wallet_name])
        startup_args = [f"-datadir={self.client.datadir_path}", "-regtest", "-server=1",
                        f"-mocktime={self.mock_time}", f"-rpcport={rpc_port(4)}",
                        f"-connect=127.0.0.1:{p2p_port(0)}", *self.client_args]

        def mine_review_blocks(count):
            before = self.nodes[0].getblockcount()
            self.mine_pos_blocks(count)
            return [self.nodes[0].getblockhash(height) for height in range(before + 1, self.nodes[0].getblockcount() + 1)]

        controller = FlowMeshQtReviewController(self.options.tmpdir, self.tls_relays, mine_blocks=mine_review_blocks)
        try:
            market = self.market_status(self.nodes[0], market_id)
            self.write_artifact("qt-review.json", {
                "automated_checks_succeeded": True, "Qt_path_exercised": False,
                "fixture_parent_pid": os.getpid(), "operator_pids": [node.process.pid for node in self.nodes],
                "client_stopped": True, "client_datadir": str(self.client.datadir_path),
                "client_startup_args": startup_args, "gui_launch_args": startup_args,
                "market_id": market_id, "base_asset_id": market["base_asset_id"],
                "trade_price_atoms": CANONICAL_PRICE,
                "operator_api_endpoints": [f"https://127.0.0.1:{port}" for port in self.api_ports],
                "client_proxy_endpoints": [relay.url for relay in self.tls_relays], "tls_ca": str(self.pki["ca"]),
                "mocktime": self.mock_time, "client_B3_port": p2p_port(12), "client_RPC_port": rpc_port(4),
                "hold_seconds": seconds, "completion_marker": str(complete),
                "Qt_preferences_isolation_required": True, "local_controls": controller.manifest(),
                "operator_wallet_names": [self.default_wallet_name] * len(self.nodes),
                "operator_account_ids": [node.getflowmeshbalance(market_id).get("account", {}).get("account_id") for node in self.nodes],
                "warning": "No Qt process is launched by this fixture. Use exact guarded build and fresh preferences. Stop Qt cleanly before completion marker. Canonical controls do not qualify Qt.",
            })
            self.log.info("FLOWMESH_QT_REVIEW_READY %s", Path(self.options.tmpdir, "qt-review.json"))
            deadline = time.monotonic() + seconds
            while not complete.exists() and time.monotonic() < deadline:
                controller.poll_once()
                time.sleep(min(.5, max(0, deadline - time.monotonic())))
            self.log.info("FLOWMESH_QT_REVIEW_HOLD_ENDED marker=%s", complete.exists())
        finally:
            controller.close()

    def qualification_workload(self, inherited_market):
        started = time.monotonic()
        success = False
        try:
            market_id, asset = self.create_reverse_test_market(inherited_market)
            self.exercise_reverse_controls(market_id, asset)
            self.settlement_control(market_id, asset)
            self.verify_clean_restart(market_id)
            self.wait_for_independent_mesh()
            success = True
        finally:
            self.end_b3_workload()
            self.write_artifact("flowmesh-reverse-client-qualification.json", {
                "success": success, "elapsed_ms": (time.monotonic() - started) * 1000,
                "canonical_control_actions": self.reverse_results, "actions": self.client_results,
                "compatibility": self.compatibility, "tls_relays": [relay.snapshot() for relay in self.tls_relays],
                "market_id": self.reverse_market, "inherited_market_id": inherited_market,
                "Qt_exercised": False, "order_entry_adapter_exercised_by_RPC": False,
                "ordinary_client_datadir": str(self.client.datadir_path) if self.client else None,
                "node_debug_logs": [str(node.debug_log_path) for node in self.nodes] + ([str(self.client.debug_log_path)] if self.client else []),
                "manual_B3_transaction_injection": True, "WAN_qualified": False,
                "actual_block_parity_qualified": success and {row["block_validation"]["record_type"] for row in self.compatibility
                    if row.get("block_validation", {}).get("block_parity_qualified")} == {8, 9},
                "actual_fee_asset": "B3", "stable_fee_qualified": False,
            })
        if success:
            self.qt_review_hold(self.reverse_market)


if __name__ == "__main__":
    FlowMeshReverseClientTest(__file__).main()
