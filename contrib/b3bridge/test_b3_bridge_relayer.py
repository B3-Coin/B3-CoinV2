#!/usr/bin/env python3
"""Offline unit tests for b3_bridge_relayer.py."""
import json
import io
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import traceback
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import b3_bridge_relayer as relayer


H20 = "0x" + "11" * 20
H32 = "0x" + "22" * 32
ROOT_A = "0x" + "33" * 32
ROOT_B = "0x" + "44" * 32
TOKEN = "0x" + "12" * 20
VERIFIER = "0x" + "13" * 20
VAULT_CODE = "0x6001"
VERIFIER_CODE = "0x6002"
TOKEN_CODE = "0x6003"


def code_hash(code):
    return "0x" + relayer.receipts.keccak256(bytes.fromhex(code[2:])).hex()


VAULT_CODE_HASH = code_hash(VAULT_CODE)
VERIFIER_CODE_HASH = code_hash(VERIFIER_CODE)
TOKEN_CODE_HASH = code_hash(TOKEN_CODE)


def bridge_info():
    return {"configured": True, "ready": True, "vault": H20,
            "vault_runtime_code_hash": VAULT_CODE_HASH,
            "token": TOKEN, "registry_id": H32,
            "implementation_or_adapter": TOKEN_CODE_HASH,
            "adapter_version": 1, "recipient_encoding_version": 1,
            "asset_id": ROOT_A, "ethereum_chain_id": 1,
            "origin_deployment_block": 50, "activation_height": 100,
            "origin_decimals": 6,
            "asset_decimals": 6, "trusted_checkpoint_root": ROOT_B,
            "trusted_checkpoint_slot": 8192,
            "genesis_validators_root": ROOT_A,
            "ethereum_fork_schedule": [{"activation_epoch": 0,
                                          "fork_version": "0x00000000"}],
            "fork_schedule_valid_through_epoch": 999999,
            "electra_epoch": 10, "min_sync_committee_participants": 342,
            "max_sync_lag_slots": 128,
            "withdrawal_mode": "decentralized-verifier-v1",
            "decentralized_verifier": VERIFIER,
            "decentralized_verifier_code_hash": VERIFIER_CODE_HASH,
            "bootstrap_validator_set_hash": H32,
            "withdrawal_rules_version": 1,
            "withdrawal_rules_commitment": ROOT_B,
            "min_bridge_validators": 4, "max_bridge_validators": 64,
            "min_bridge_total_weight": 1332, "max_epoch_lag": 3600,
            "max_per_block": 1_000_000, "max_per_epoch": 10_000_000,
            "mint_epoch_length_blocks": 30}


def pinned_code(params):
    address, block = params
    if block not in ("0x32", "latest"):
        raise AssertionError(f"unexpected code block {block}")
    return {H20: VAULT_CODE, VERIFIER: VERIFIER_CODE,
            TOKEN: TOKEN_CODE}[address]


def deposit_log(deposit_id, tx_byte, transaction_index, log_index):
    amount = deposit_id + 1
    recipient = bytes.fromhex("00" * 11 + "01" + "34" * 20)
    return {
        "address": H20,
        "topics": [relayer.DEPOSIT_TOPIC,
                   "0x" + deposit_id.to_bytes(32, "big").hex(),
                   "0x" + "00" * 12 + TOKEN[2:]],
        "data": "0x" + amount.to_bytes(32, "big").hex() + recipient.hex(),
        "blockNumber": "0x32",
        "blockHash": H32,
        "transactionHash": "0x" + f"{tx_byte:02x}" * 32,
        "transactionIndex": hex(transaction_index),
        "logIndex": hex(log_index),
    }


def receipt_for(log):
    return {"status": "0x1", "cumulativeGasUsed": "0x5208",
            "logsBloom": "0x" + "00" * 256,
            "blockNumber": log["blockNumber"],
            "blockHash": log["blockHash"],
            "transactionHash": log["transactionHash"],
            "transactionIndex": log["transactionIndex"], "logs": [log]}


def bloom_for(*values):
    bloom = bytearray(256)
    for value in values:
        digest = relayer.receipts.keccak256(bytes.fromhex(value[2:]))
        for offset in (0, 2, 4):
            bit = ((digest[offset] << 8) | digest[offset + 1]) & 2047
            bloom[255 - bit // 8] |= 1 << (bit % 8)
    return "0x" + bloom.hex()


def authenticated_block(*logs):
    block_receipts = []
    for index, item in enumerate(logs):
        item["transactionIndex"] = hex(index)
        item["logIndex"] = hex(index)
        receipt = receipt_for(item)
        receipt["transactionIndex"] = hex(index)
        block_receipts.append(receipt)
    items = [(relayer.receipts.rlp(i), relayer.receipts.encode_receipt(receipt))
             for i, receipt in enumerate(block_receipts)]
    receipt_root = relayer.receipts.Trie(items).root()
    header = {
        "parentHash": H32, "sha3Uncles": ROOT_A, "miner": H20,
        "stateRoot": ROOT_A, "transactionsRoot": ROOT_B,
        "receiptsRoot": "0x" + receipt_root.hex(),
        "logsBloom": bloom_for(H20, relayer.DEPOSIT_TOPIC,
                               "0x" + "00" * 12 + TOKEN[2:]),
        "difficulty": "0x1", "number": "0x32", "gasLimit": "0x100000",
        "gasUsed": "0x5208", "timestamp": "0x1", "extraData": "0x",
        "mixHash": ROOT_A, "nonce": "0x" + "00" * 8,
        "transactions": [receipt["transactionHash"] for receipt in block_receipts],
    }
    header["hash"] = "0x" + relayer.receipts.keccak256(
        relayer.encode_exec_header(header)).hex()
    for receipt in block_receipts:
        receipt["blockHash"] = header["hash"]
        for item in receipt["logs"]:
            item["blockHash"] = header["hash"]
    return header, block_receipts


def store(kind, payload, slot, current=ROOT_A, next_root=ROOT_B):
    return {"kind": kind, "payload_hex": payload, "store_period": slot // 8192,
            "finalized_beacon_slot": slot, "anchor_block_number": 100,
            "finalized_beacon_root": ROOT_A, "anchor_hash": H32,
            "current_sync_committee_root": current,
            "next_sync_committee_root": next_root}


def retained_anchor(block_number=100, block_hash=H32):
    return {"block_number": block_number, "block_hash": block_hash,
            "receipts_root": ROOT_B,
            "source_finalized_beacon_slot": 8192,
            "source_finalized_execution_block": block_number,
            "execution_timestamp": 12345, "connected_height": 50,
            "connected_block": ROOT_A, "b3_finalized_height": 60,
            "b3_finalized_block": ROOT_B}


class FakeRpc:
    def __init__(self, replies=None):
        self.replies = replies or {}
        self.calls = []

    def call(self, method, params=()):
        self.calls.append((method, list(params)))
        reply = self.replies.get(method)
        if callable(reply):
            return reply(list(params))
        if isinstance(reply, Exception):
            raise reply
        return reply


class RelayerTests(unittest.TestCase):
    def test_endpoint_errors_never_expose_url_credentials_or_api_keys(self):
        secret = "super-secret-api-key"
        rpc_url = f"https://user:password@rpc.example/v3/{secret}"
        self.assertEqual(relayer.endpoint_label(rpc_url),
                         "https://rpc.example")

        http_error = relayer.urllib.error.HTTPError(
            rpc_url, 503, f"upstream failed at {rpc_url}", {},
            io.BytesIO(b"not-json"))
        with patch.object(relayer.urllib.request, "urlopen",
                          side_effect=http_error):
            with self.assertRaises(relayer.RelayerError) as caught:
                relayer.JsonRpc(rpc_url).call("getbridgeinfo")
        rendered = "".join(traceback.format_exception(caught.exception))
        self.assertNotIn(secret, rendered)
        self.assertNotIn("password", rendered)
        self.assertIn("https://rpc.example", rendered)
        http_error.close()

        beacon_url = f"https://token:{secret}@beacon.example/api/{secret}"
        with patch.object(relayer.urllib.request, "urlopen",
                          side_effect=OSError(beacon_url)):
            with self.assertRaises(relayer.RelayerError) as caught:
                relayer.fetch(beacon_url)
        rendered = "".join(traceback.format_exception(caught.exception))
        self.assertNotIn(secret, rendered)
        self.assertIn("https://beacon.example", rendered)

        malformed = f"https://host.invalid:bad/{secret}"
        with self.assertRaises(relayer.RelayerError) as caught:
            relayer.provider_origin(malformed)
        rendered = "".join(traceback.format_exception(caught.exception))
        self.assertNotIn(secret, rendered)

    def test_bridge_identity_binds_origin_deployment_block(self):
        info = bridge_info() | {"origin_deployment_block": 12345}
        identity = relayer.bridge_identity(info, 1, ROOT_B)
        self.assertEqual(identity["origin_deployment_block"], 12345)
        self.assertEqual(identity["vault_runtime_code_hash"], VAULT_CODE_HASH)
        self.assertEqual(identity["origin_token_runtime_code_hash"],
                         TOKEN_CODE_HASH)
        self.assertEqual(identity["decentralized_verifier"], VERIFIER)
        with self.assertRaisesRegex(relayer.RelayerError, "chain id"):
            relayer.bridge_identity(info, 2, ROOT_B)

    def test_bridge_identity_requires_every_contract_and_codec_pin(self):
        for field in ("vault_runtime_code_hash", "implementation_or_adapter",
                      "adapter_version", "recipient_encoding_version",
                      "decentralized_verifier",
                      "decentralized_verifier_code_hash"):
            info = bridge_info()
            del info[field]
            with self.subTest(field=field):
                with self.assertRaisesRegex(relayer.RelayerError,
                                            "unconfigured or incomplete"):
                    relayer.bridge_identity(info, 1, ROOT_B)

    def test_runtime_code_pins_check_deployment_and_latest_on_every_provider(self):
        identity = relayer.bridge_identity(bridge_info(), 1, ROOT_B)
        primary = FakeRpc({"eth_getCode": pinned_code})
        witness = FakeRpc({"eth_getCode": pinned_code})
        relayer.verify_runtime_code_pins([primary, witness], identity)
        expected = [
            ("eth_getCode", [address, block])
            for block in ("0x32", "latest")
            for address in (H20, VERIFIER, TOKEN)
        ]
        self.assertEqual(primary.calls, expected)
        self.assertEqual(witness.calls, expected)

    def test_runtime_code_pin_mismatch_and_empty_code_fail_closed(self):
        identity = relayer.bridge_identity(bridge_info(), 1, ROOT_B)

        def changed_latest(params):
            if params == [H20, "latest"]:
                return "0x6004"
            return pinned_code(params)

        with self.assertRaisesRegex(relayer.RelayerError,
                                    "vault runtime code hash.*latest"):
            relayer.verify_runtime_code_pins(
                [FakeRpc({"eth_getCode": changed_latest})], identity)

        def missing_at_deployment(params):
            if params == [VERIFIER, "0x32"]:
                return "0x"
            return pinned_code(params)

        with self.assertRaisesRegex(relayer.RelayerError,
                                    "verifier has no valid runtime bytecode"):
            relayer.verify_runtime_code_pins(
                [FakeRpc({"eth_getCode": missing_at_deployment})], identity)

    def test_saved_state_binds_contract_and_verifier_code_pins(self):
        identity = relayer.bridge_identity(bridge_info(), 1, ROOT_B)
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind(identity, identity["origin_deployment_block"])

            changed = bridge_info()
            changed["decentralized_verifier_code_hash"] = ROOT_A
            changed_identity = relayer.bridge_identity(changed, 1, ROOT_B)
            with self.assertRaisesRegex(relayer.RelayerError,
                                        "different bridge pins"):
                state.bind(changed_identity, None)

    def test_duplicate_provider_origins_are_rejected(self):
        argv = ["--ethereum-rpc", "https://api.example/v1/key-a",
                "--ethereum-rpc-secondary", "https://user:secret@api.example/v2/key-b",
                "--beacon-url", "https://beacon.example", "--wallet", "relay",
                "--trusted-root", ROOT_B, "--max-fee-b3", "0.01",
                "--daily-fee-budget-b3", "1"]
        with patch("sys.stderr"):
            with self.assertRaises(SystemExit):
                relayer.parse_args(argv)

    def test_finalized_store_snapshot_is_bound_and_written_without_bootstrap(self):
        info = bridge_info() | {
            "light_client_connected_height": 80,
            "light_client_connected_block": H32,
            "light_client_period": 1,
            "finalized_beacon_slot": 8192,
            "finalized_execution_block": 100,
            "finalized_execution_hash": ROOT_A,
        }
        snapshot = {
            "version": 1,
            "connection": {"height": 80, "block_hash": H32},
            "b3_finalized": {"height": 90, "block_hash": ROOT_B},
            "store": {
                "period": 1,
                "finalized_header": {
                    "beacon": {"slot": 8192},
                    "execution": {"block_number": 100,
                                  "block_hash": ROOT_A},
                },
            },
        }
        self.assertIs(relayer.validate_store_snapshot(snapshot, info), snapshot)
        changed = json.loads(json.dumps(snapshot))
        changed["connection"]["height"] = 81
        with self.assertRaisesRegex(relayer.RelayerError, "disagrees"):
            relayer.validate_store_snapshot(changed, info)
        common = ({"cfg": 1}, None, [], {"finality": 1}, 100, ROOT_A,
                  snapshot)
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "bootstrap.json").write_text("stale")
            relayer.write_common(work, common)
            self.assertFalse((work / "bootstrap.json").exists())
            self.assertEqual(json.loads((work / "store.json").read_text()),
                             snapshot)

    def test_capture_from_store_requests_current_period_not_bootstrap(self):
        info = bridge_info() | {
            "light_client_connected_height": 80,
            "light_client_connected_block": H32,
            "light_client_period": 1,
            "finalized_beacon_slot": 8192,
            "finalized_execution_block": 100,
            "finalized_execution_hash": ROOT_A,
        }
        snapshot = {
            "version": 1,
            "connection": {"height": 80, "block_hash": H32},
            "b3_finalized": {"height": 90, "block_hash": ROOT_B},
            "store": {"period": 1, "finalized_header": {
                "beacon": {"slot": 8192},
                "execution": {"block_number": 100,
                              "block_hash": ROOT_A}}},
        }
        finality = {"data": {
            "signature_slot": "8193",
            "attested_header": {"beacon": {"slot": "8192"}},
            "finalized_header": {
                "beacon": {"slot": "8192"},
                "execution": {"block_number": "100",
                              "block_hash": ROOT_A}},
        }}
        seen = []

        def fake_fetch(url):
            seen.append(url)
            if url.endswith("/eth/v1/beacon/genesis"):
                return {"data": {"genesis_validators_root": ROOT_A}}
            if url.endswith("/eth/v1/config/fork_schedule"):
                return {"data": [{"epoch": "0",
                                  "current_version": "0x00000000"}]}
            if url.endswith("/eth/v1/beacon/light_client/finality_update"):
                return finality
            if url.endswith("/eth/v1/beacon/headers/head"):
                return {"data": {"header": {"message": {"slot": "8192"}}}}
            if "light_client/updates?start_period=1&count=1" in url:
                return []
            raise AssertionError(url)

        with patch.object(relayer, "fetch", side_effect=fake_fetch):
            common = relayer.capture_common(
                "https://beacon.example", ROOT_B, info, snapshot)
        self.assertIs(common[6], snapshot)
        self.assertTrue(any("start_period=1" in url for url in seen))
        self.assertFalse(any("/bootstrap/" in url for url in seen))

    def test_b3_finalized_historical_anchor_requires_provider_agreement(self):
        item = deposit_log(7, 0x55, 0, 0)
        header, _block_receipts = authenticated_block(item)
        anchor = retained_anchor(50, header["hash"])
        anchor["receipts_root"] = header["receiptsRoot"]
        anchor["execution_timestamp"] = 1
        response = {"target_block": 40, "found": True, **anchor}
        normalized = relayer.validate_execution_anchor(response, 40)
        primary = FakeRpc({"eth_getBlockByNumber": header})
        witness = FakeRpc({"eth_getBlockByNumber": dict(header)})
        relayer.corroborate_execution_anchor(normalized, [primary, witness])
        bad_header = dict(header, receiptsRoot=ROOT_A)
        with self.assertRaisesRegex(relayer.RelayerError,
                                    "disagrees with.*anchor"):
            relayer.corroborate_execution_anchor(
                normalized, [primary, FakeRpc({"eth_getBlockByNumber": bad_header})])
        with self.assertRaisesRegex(relayer.RelayerError, "retains no"):
            relayer.validate_execution_anchor(
                {"target_block": 40, "found": False}, 40)

        def exact_recheck(params):
            self.assertEqual(params, [50])
            return {"target_block": 50, "found": True, **anchor}

        relayer.recheck_execution_anchor(
            FakeRpc({"getbridgeanchorforblock": exact_recheck}), normalized)
        changed = dict(anchor, connected_block=H32)
        with self.assertRaisesRegex(relayer.RelayerError,
                                    "changed while assembling"):
            relayer.recheck_execution_anchor(FakeRpc({
                "getbridgeanchorforblock": {
                    "target_block": 50, "found": True, **changed}}),
                normalized)

    def test_authenticated_receipt_scan_finds_every_deposit(self):
        first = deposit_log(7, 0x55, 1, 5)
        second = deposit_log(8, 0x66, 2, 6)
        header, block_receipts = authenticated_block(first, second)
        primary = FakeRpc({
            "eth_getBlockByNumber": header,
            "eth_getBlockReceipts": block_receipts,
        })

        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 50)
            relayer.scan_finalized(
                state, primary, {"vault": H20, "token": TOKEN},
                50, header["hash"], 100, 100, lambda _deposit: [])
            self.assertEqual(state.cursor(), 51)
            deposits = state.db.execute(
                "SELECT deposit_id FROM deposits ORDER BY id").fetchall()
            self.assertEqual([row[0] for row in deposits], ["7", "8"])

    def test_receipt_root_mismatch_rejects_without_advancing(self):
        item = deposit_log(7, 0x55, 1, 5)
        header, block_receipts = authenticated_block(item)
        block_receipts[0]["logs"][0]["data"] = "0x" + "01" * 64
        primary = FakeRpc({"eth_getBlockByNumber": header,
                           "eth_getBlockReceipts": block_receipts})

        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 50)
            with self.assertRaisesRegex(relayer.RelayerError,
                                        "receipt array.*receipts root"):
                relayer.scan_finalized(
                    state, primary, {"vault": H20, "token": TOKEN},
                    50, header["hash"], 100, 100, lambda _deposit: [])
            self.assertEqual(state.cursor(), 50)
            self.assertEqual(state.unplanned(), [])

    def test_authenticated_negative_bloom_never_fetches_receipts(self):
        header = {"number": "0x32", "hash": H32,
                  "logsBloom": "0x" + "00" * 256}
        rpc = FakeRpc()
        self.assertEqual(relayer.authenticated_block_deposits(
            rpc, header, {"vault": H20, "token": TOKEN}), [])
        self.assertEqual(rpc.calls, [])

    def test_bad_deposit_proof_plan_cannot_advance_or_persist_page(self):
        item = deposit_log(7, 0x55, 1, 5)
        header, block_receipts = authenticated_block(item)
        primary = FakeRpc({"eth_getBlockByNumber": header,
                           "eth_getBlockReceipts": block_receipts})

        def reject(_deposit):
            raise relayer.RelayerError("bad receipt proof")

        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 50)
            with self.assertRaisesRegex(relayer.RelayerError,
                                        "bad receipt proof"):
                relayer.scan_finalized(
                    state, primary, {"vault": H20, "token": TOKEN},
                    50, header["hash"], 100, 100, reject)
            self.assertEqual(state.cursor(), 50)
            self.assertEqual(
                state.db.execute("SELECT COUNT(*) FROM deposits").fetchone()[0],
                0)

    def test_partial_block_replay_is_idempotent_before_cursor_commit(self):
        first = deposit_log(7, 0x55, 1, 5)
        second = deposit_log(8, 0x66, 2, 6)
        header, block_receipts = authenticated_block(first, second)
        primary = FakeRpc({"eth_getBlockByNumber": header,
                           "eth_getBlockReceipts": block_receipts})
        failed = []

        def fail_second(deposit):
            if deposit["deposit_id"] == 8 and not failed:
                failed.append(True)
                raise relayer.RelayerError("simulated crash before cursor")
            return [{"kind": "mint",
                     "payload_hex": f"{deposit['deposit_id']:02x}",
                     "amount": "1", "address": "B3address"}]

        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 50)
            with self.assertRaisesRegex(relayer.RelayerError,
                                        "simulated crash"):
                relayer.scan_finalized(
                    state, primary, {"vault": H20, "token": TOKEN},
                    50, header["hash"], 100, 100, fail_second)
            self.assertEqual(state.cursor(), 50)
            self.assertEqual(state.db.execute(
                "SELECT COUNT(*) FROM deposits").fetchone()[0], 1)
            relayer.scan_finalized(
                state, primary, {"vault": H20, "token": TOKEN},
                50, header["hash"], 100, 100, fail_second)
            self.assertEqual(state.cursor(), 51)
            self.assertEqual(state.db.execute(
                "SELECT COUNT(*) FROM deposits").fetchone()[0], 2)
            self.assertEqual(state.db.execute(
                "SELECT COUNT(*) FROM jobs").fetchone()[0], 2)

    def test_single_provider_dry_scan_never_persists_omission_cursor(self):
        item = deposit_log(7, 0x55, 1, 5)
        header, block_receipts = authenticated_block(item)
        primary = FakeRpc({"eth_getBlockByNumber": header,
                           "eth_getBlockReceipts": block_receipts})
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 50)
            relayer.scan_finalized(
                state, primary, {"vault": H20, "token": TOKEN},
                50, header["hash"], 100, 100, lambda _deposit: [],
                persist_cursor=False)
            self.assertEqual(state.cursor(), 50)
            self.assertEqual(
                state.db.execute("SELECT COUNT(*) FROM deposits").fetchone()[0],
                1)

    def test_witness_chain_id_mismatch_is_rejected_before_state_binding(self):
        args = SimpleNamespace(ethereum_chain_id=1)
        state = FakeRpc()
        node = FakeRpc({"getbridgeinfo": bridge_info()})
        primary = FakeRpc({"eth_chainId": "0x1"})
        witness = FakeRpc({"eth_chainId": "0x2"})

        with self.assertRaisesRegex(relayer.RelayerError,
                                    "providers disagree on chain id"):
            relayer.run_once(args, state, primary, [witness], node,
                             FakeRpc(), 63)
        self.assertEqual(primary.calls, [("eth_chainId", [])])
        self.assertEqual(witness.calls, [("eth_chainId", [])])

    def test_witness_finalized_block_hash_mismatch_is_rejected(self):
        args = SimpleNamespace(
            ethereum_chain_id=1, trusted_root=ROOT_B, start_block=None,
            b3_confirmations=1, beacon_url="https://beacon.invalid",
            payload_tool="ethcheck")
        primary = FakeRpc({
            "eth_chainId": "0x1",
            "eth_getCode": pinned_code,
            "eth_getBlockByNumber": {"hash": H32},
        })
        witness = FakeRpc({
            "eth_chainId": "0x1",
            "eth_getCode": pinned_code,
            "eth_getBlockByNumber": {"hash": ROOT_A},
        })
        node = FakeRpc({"getbridgeinfo": bridge_info()})
        common = ({}, {}, [], {}, 100, H32)

        with tempfile.TemporaryDirectory() as directory:
            args.work_root = str(Path(directory) / "work")
            state = relayer.State(Path(directory) / "state.sqlite3")
            plan = {"bootstrap": store("bootstrap", "01", 8192),
                    "updates": [], "backfills": []}
            with (patch.object(relayer, "capture_common", return_value=common),
                  patch.object(relayer, "emit_plan", return_value=plan)):
                with self.assertRaisesRegex(
                        relayer.RelayerError,
                        "secondary Ethereum RPC disagrees.*finalized execution hash"):
                    relayer.run_once(args, state, primary, [witness], node,
                                     FakeRpc(), 63)
            self.assertEqual(state.cursor(), 50)
        self.assertIn(("eth_getBlockByNumber", ["0x64", False]),
                      witness.calls)

    def test_unverified_beacon_plan_cannot_advance_scan_cursor(self):
        args = SimpleNamespace(
            ethereum_chain_id=1, trusted_root=ROOT_B, start_block=None,
            b3_confirmations=1, beacon_url="https://beacon.invalid",
            work_root=None, payload_tool="ethcheck", scan_chunk=100)
        primary = FakeRpc({
            "eth_chainId": "0x1",
            "eth_getCode": pinned_code,
            "eth_getBlockByNumber": {"hash": H32},
        })
        witness = FakeRpc({
            "eth_chainId": "0x1",
            "eth_getCode": pinned_code,
            "eth_getBlockByNumber": {"hash": H32},
        })
        node = FakeRpc({"getbridgeinfo": bridge_info()})
        common = ({}, {}, [], {}, 100, H32)

        with tempfile.TemporaryDirectory() as directory:
            args.work_root = str(Path(directory) / "work")
            state = relayer.State(Path(directory) / "state.sqlite3")
            with (patch.object(relayer, "capture_common", return_value=common),
                  patch.object(relayer, "emit_plan",
                               side_effect=relayer.RelayerError("bad beacon proof")),
                  patch.object(relayer, "scan_finalized") as scan):
                with self.assertRaisesRegex(relayer.RelayerError,
                                            "bad beacon proof"):
                    relayer.run_once(args, state, primary, [witness], node,
                                     FakeRpc(), 63)
                scan.assert_not_called()
            self.assertEqual(state.cursor(), 50)

    def test_update_slots_respect_pinned_fork_horizon(self):
        update = {"data": {"signature_slot": "65",
                           "attested_header": {"beacon": {"slot": "64"}},
                           "finalized_header": {"beacon": {"slot": "63"}}}}
        relayer.validate_update_horizon(update, 2)
        update["data"]["attested_header"]["beacon"]["slot"] = "96"
        with self.assertRaisesRegex(relayer.RelayerError, "fork schedule horizon"):
            relayer.validate_update_horizon(update, 2)
        update["data"]["attested_header"]["beacon"]["slot"] = "64"
        update["data"]["signature_slot"] = "0"
        with self.assertRaises(relayer.RelayerError):
            relayer.validate_update_horizon(update, 2)

    def test_recipient_v1_base58_and_rejection(self):
        raw = "0x" + "00" * 11 + "01" + "12" * 20
        address = relayer.recipient_address(raw, 63)
        self.assertTrue(address)
        self.assertNotEqual(address, relayer.recipient_address(raw, 111))
        with self.assertRaisesRegex(relayer.RelayerError, "RECIPIENT_V1"):
            relayer.recipient_address("0x" + "00" * 32, 63)
        with self.assertRaisesRegex(relayer.RelayerError, "zero P2PKH"):
            relayer.recipient_address("0x" + "00" * 11 + "01" + "00" * 20,
                                      63)

    def test_exact_store_reconciliation_and_mint_metadata(self):
        boot = store("bootstrap", "01", 8192)
        update = store("update", "02", 16384, ROOT_B, None)
        recipient = "0x" + "00" * 11 + "01" + "12" * 20
        mint = {"kind": "mint", "payload_hex": "04", "registry_id": H32,
                "origin_token": TOKEN,
                "origin_amount": 123, "amount": 123, "b3_recipient": recipient, "deposit_id": 7,
                "tx_index": 3, "receipt_log_index": 2,
                "target_block_number": 90, "target_block_hash": H32,
                "source_anchor_hash": H32}
        source = retained_anchor()
        plan = {"bootstrap": boot, "updates": [update], "backfills": [],
                "mint": mint, "source_anchor": source}
        info = {"light_client_bootstrapped": True, "light_client_period": 1,
                "finalized_beacon_slot": 8192, "current_sync_committee_root": ROOT_A,
                "next_sync_committee_root": ROOT_B, "registry_id": H32,
                "token": TOKEN,
                "finalized_beacon_root": ROOT_A,
                "finalized_execution_block": 100, "finalized_execution_hash": H32}
        dep = {"deposit_id": "7", "tx_index": 3, "receipt_log_index": 2,
               "block_number": 90, "block_hash": H32, "amount": "123",
               "recipient": recipient, "token": TOKEN,
               "source_anchor": source}
        records = relayer.select_records(plan, info, 63, dep)
        self.assertEqual([r["kind"] for r in records], ["update", "mint"])
        self.assertEqual(records[-1]["amount"], "123")
        bad = dict(info, current_sync_committee_root="0x" + "99" * 32)
        with self.assertRaisesRegex(relayer.RelayerError, "cannot reconcile"):
            relayer.select_records(plan, bad, 63, dep)
        bad_token_plan = dict(plan, mint=dict(mint, origin_token=H20))
        with self.assertRaisesRegex(relayer.RelayerError, "mint token"):
            relayer.select_records(bad_token_plan, info, 63, dep)
        wrong_source = dict(plan, source_anchor=dict(
            source, receipts_root=ROOT_A))
        with self.assertRaisesRegex(relayer.RelayerError,
                                    "source anchor does not match"):
            relayer.select_records(wrong_source, info, 63, dep)
        forged_b3_finality = dict(plan, source_anchor=dict(
            source, b3_finalized_block=ROOT_A))
        with self.assertRaisesRegex(relayer.RelayerError,
                                    "source anchor does not match"):
            relayer.select_records(forged_b3_finality, info, 63, dep)
        distant = dict(update, anchor_block_number=20101)
        distant_plan = dict(plan, updates=[distant])
        with self.assertRaisesRegex(relayer.RelayerError,
                                    "missing intermediate period updates"):
            relayer.select_records(distant_plan, info, 63, dep)

    def test_same_slot_next_committee_progress_requires_same_finality(self):
        record = store("update", "02", 8192, ROOT_A, None)
        info = {"light_client_bootstrapped": True, "light_client_period": 1,
                "finalized_beacon_slot": 8192,
                "finalized_beacon_root": "0x" + "99" * 32,
                "current_sync_committee_root": ROOT_A,
                "next_sync_committee_root": ROOT_B,
                "finalized_execution_block": 100,
                "finalized_execution_hash": H32}
        self.assertFalse(relayer.store_ahead(record, info))
        info["finalized_beacon_root"] = ROOT_A
        self.assertTrue(relayer.store_ahead(record, info))

    def test_sqlite_cursor_dedup_and_retained_anchors(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 50)
            deposit = {"deposit_id": 7, "block_number": 50, "block_hash": H32,
                       "tx_hash": "0x" + "55" * 32, "tx_index": 1,
                       "receipt_log_index": 0, "amount": 6, "recipient": H32}
            record = {"kind": "execution-backfill", "payload_hex": "aabb",
                      "target_block_hash": ROOT_A}
            state.add_verified_deposit(deposit, [record])
            state.advance_cursor(50, 51)
            self.assertEqual(state.cursor(), 51)
            state.bind({"bridge": 1}, 50)  # immutable start, not advancing cursor
            state.add_plan([record])
            self.assertEqual(len(state.pending()), 1)
            state.set_state(state.first()["id"], "confirmed")
            self.assertEqual(state.retained_anchors(), [ROOT_A])
            with self.assertRaisesRegex(relayer.RelayerError, "different bridge pins"):
                state.bind({"bridge": 2}, None)

    def test_preview_cursor_is_replayed_once_under_authenticated_scan(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 50)
            state.advance_cursor(50, 80)
            state.db.execute(
                "DELETE FROM meta WHERE key='authenticated_scan_version'")
            state.bind({"bridge": 1}, 50)
            self.assertEqual(state.cursor(), 50)
            state.advance_cursor(50, 60)
            state.bind({"bridge": 1}, 50)
            self.assertEqual(state.cursor(), 60)

    def test_light_client_jobs_have_priority_over_earlier_mints(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 1)
            state.add_plan([
                {"kind": "mint", "payload_hex": "01", "amount": "1",
                 "address": "B3address"},
                {"kind": "update", "payload_hex": "02"},
            ])
            self.assertEqual(state.first()["kind"], "update")

    def test_daily_fee_reservations_are_durable_and_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 1)
            state.add_plan([{"kind": "bootstrap", "payload_hex": "01"},
                            {"kind": "update", "payload_hex": "02"}])
            first, second = state.db.execute(
                "SELECT * FROM jobs ORDER BY id").fetchall()
            state.prepared(first["id"], "aa" * 32, "00", 600,
                           "2026-09-02", 1000, 1000)
            with self.assertRaisesRegex(relayer.RelayerError, "daily.*exhausted"):
                state.prepared(second["id"], "bb" * 32, "01", 500,
                               "2026-09-02", 1000, 1000)
            rows = state.db.execute(
                "SELECT state,fee_atoms FROM jobs ORDER BY id").fetchall()
            self.assertEqual([tuple(row) for row in rows],
                             [("prepared", 600), ("planned", None)])

    def test_workdir_cleanup_is_scoped_and_bounded(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            old = root / "deposit-old"
            new = root / "deposit-new"
            keep = root / "operator-notes"
            for path in (old, new, keep):
                path.mkdir()
            (old / "proof.json").write_text("old")
            (new / "proof.json").write_text("new")
            os.utime(old, (1, 1))
            os.utime(new, (2, 2))
            relayer.prune_deposit_workdirs(root, 1)
            self.assertFalse(old.exists())
            self.assertTrue(new.exists())
            self.assertTrue(keep.exists())

    def test_emit_tool_contract(self):
        seen = []

        class Result:
            returncode = 0
            stderr = "verified\n"
            stdout = json.dumps({"bootstrap": {}, "updates": [], "backfills": []})

        def runner(argv, **kwargs):
            seen.append((argv, kwargs))
            return Result()

        plan = relayer.emit_plan("tool", Path("work"), runner)
        self.assertEqual(plan["updates"], [])
        self.assertEqual(seen[0][0], ["tool", "work", "--emit-payloads"])
        self.assertTrue(seen[0][1]["capture_output"])
        self.assertTrue(seen[0][1]["text"])

    def test_dry_run_never_calls_b3(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 1)
            state.add_plan([{"kind": "bootstrap", "payload_hex": "01"}])
            node, wallet = FakeRpc(), FakeRpc()
            relayer.process_jobs(state, node, wallet, 1, True)
            self.assertEqual(node.calls, [])
            self.assertEqual(wallet.calls, [])
            self.assertEqual(state.first()["state"], "planned")

    def test_state_has_single_process_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            first = relayer.ProcessLock(Path(directory) / "state.sqlite3")
            try:
                with self.assertRaisesRegex(relayer.RelayerError, "another relayer"):
                    relayer.ProcessLock(Path(directory) / "state.sqlite3")
            finally:
                first.close()

    def test_prepare_persist_broadcast_wait_and_resume(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 1)
            state.add_plan([{"kind": "bootstrap", "payload_hex": "01"},
                            {"kind": "update", "payload_hex": "02"}])
            txid = "aa" * 32
            known = {txid: None}

            def gettransaction(params):
                value = known.get(params[0])
                if value is None:
                    raise relayer.RpcError({"code": -5, "message": "not found"})
                return {"confirmations": value, "blockheight": 10,
                        "blockhash": "bb" * 32}

            wallet = FakeRpc({"submitbridgecarrier": {
                "txid": txid, "hex": "00", "broadcast": False,
                "network_fee": "0.000001000"},
                "gettransaction": gettransaction})
            node = FakeRpc({"getbridgeinfo": {"active": True, "state_available": True,
                                               "mint_approval_open": True},
                            "sendrawtransaction": txid,
                            "getmempoolentry": relayer.RpcError({
                                "code": -5, "message": "not found"}),
                            "getfinalitystatus": {"finalized": {"height": 10}},
                            "getblockhash": "bb" * 32})
            relayer.process_jobs(state, node, wallet, 1, False, 2000, 10000)
            self.assertEqual(state.first()["state"], "broadcast")
            self.assertEqual(sum(m == "submitbridgecarrier" for m, _ in wallet.calls), 1)
            known[txid] = 1
            node.replies["getfinalitystatus"] = {"finalized": {"height": 9}}
            relayer.process_jobs(state, node, wallet, 1, False, 2000, 10000)
            self.assertEqual(state.first()["state"], "broadcast")
            node.replies["getfinalitystatus"] = {"finalized": {"height": 10}}
            # Stop after confirming the first job by making the second prepare fail;
            # this proves it was not attempted before its dependency confirmed.
            wallet.replies["submitbridgecarrier"] = relayer.RelayerError("stop")
            with self.assertRaisesRegex(relayer.RelayerError, "stop"):
                relayer.process_jobs(state, node, wallet, 1, False, 2000, 10000)
            first = state.db.execute("SELECT state FROM jobs ORDER BY id LIMIT 1").fetchone()[0]
            self.assertEqual(first, "confirmed")

    def test_per_transaction_fee_budget_blocks_broadcast(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 1)
            state.add_plan([{"kind": "bootstrap", "payload_hex": "01"}])
            wallet = FakeRpc({"submitbridgecarrier": {
                "txid": "aa" * 32, "hex": "00", "broadcast": False,
                "network_fee": "0.000003000"}})
            node = FakeRpc({"getbridgeinfo": {
                "active": True, "state_available": True,
                "light_client_bootstrapped": False},
                "getfinalitystatus": {"finalized": {"height": 10}}})
            with self.assertRaisesRegex(relayer.RelayerError,
                                        "exceeds per-transaction budget"):
                relayer.process_jobs(state, node, wallet, 1, False,
                                     2000, 10000)
            row = state.first()
            self.assertEqual(row["state"], "planned")
            self.assertIsNone(row["txid"])
            self.assertFalse(any(method == "sendrawtransaction"
                                 for method, _ in node.calls))

    def test_conflicted_wallet_transaction_is_reprepared(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 1)
            state.add_plan([{"kind": "bootstrap", "payload_hex": "01"}])
            prepared = []

            def prepare(_params):
                txid = ("aa" if not prepared else "bb") * 32
                prepared.append(txid)
                return {"txid": txid, "hex": "00", "broadcast": False,
                        "network_fee": "0.000001000"}

            def gettransaction(params):
                if params[0] == "aa" * 32:
                    return {"confirmations": -1}
                raise relayer.RpcError({"code": -5, "message": "not found"})

            wallet = FakeRpc({"submitbridgecarrier": prepare,
                              "gettransaction": gettransaction})
            node = FakeRpc({"getbridgeinfo": {
                "active": True, "state_available": True,
                "light_client_bootstrapped": False},
                "getfinalitystatus": {"finalized": {"height": 10}},
                "getmempoolentry": relayer.RpcError({
                    "code": -5, "message": "not found"}),
                "sendrawtransaction": "bb" * 32})
            relayer.process_jobs(state, node, wallet, 1, False, 2000, 10000)
            self.assertEqual(state.first()["state"], "planned")
            relayer.process_jobs(state, node, wallet, 1, False, 2000, 10000)
            row = state.first()
            self.assertEqual(prepared, ["aa" * 32, "bb" * 32])
            self.assertEqual(row["txid"], "bb" * 32)
            self.assertEqual(row["state"], "broadcast")

    def test_node_reported_input_conflict_is_reprepared_next_cycle(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 1)
            state.add_plan([{"kind": "bootstrap", "payload_hex": "01"}])
            prepared = []

            def prepare(_params):
                txid = ("aa" if not prepared else "bb") * 32
                prepared.append(txid)
                return {"txid": txid, "hex": "00", "broadcast": False,
                        "network_fee": "0.000001000"}

            send_count = []

            def send(_params):
                send_count.append(1)
                if len(send_count) == 1:
                    raise relayer.RpcError({
                        "code": -26, "message": "txn-mempool-conflict"})
                return "bb" * 32

            not_found = relayer.RpcError({"code": -5,
                                          "message": "not found"})
            wallet = FakeRpc({"submitbridgecarrier": prepare,
                              "gettransaction": not_found})
            node = FakeRpc({"getbridgeinfo": {
                "active": True, "state_available": True,
                "light_client_bootstrapped": False},
                "getfinalitystatus": {"finalized": {"height": 10}},
                "getmempoolentry": not_found,
                "sendrawtransaction": send})
            relayer.process_jobs(state, node, wallet, 1, False, 2000, 10000)
            self.assertEqual(state.first()["state"], "planned")
            relayer.process_jobs(state, node, wallet, 1, False, 2000, 10000)
            self.assertEqual(state.first()["state"], "broadcast")
            self.assertEqual(state.first()["txid"], "bb" * 32)

    def test_wallet_conflict_discovered_after_send_is_bounded_to_next_cycle(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 1)
            state.add_plan([{"kind": "bootstrap", "payload_hex": "01"}])
            txid = "aa" * 32
            lookups = []

            def gettransaction(_params):
                lookups.append(1)
                if len(lookups) == 1:
                    raise relayer.RpcError({"code": -5,
                                            "message": "not found"})
                return {"confirmations": -1}

            wallet = FakeRpc({"submitbridgecarrier": {
                "txid": txid, "hex": "00", "broadcast": False,
                "network_fee": "0.000001000"},
                "gettransaction": gettransaction})
            node = FakeRpc({"getbridgeinfo": {
                "active": True, "state_available": True,
                "light_client_bootstrapped": False},
                "getfinalitystatus": {"finalized": {"height": 10}},
                "getmempoolentry": relayer.RpcError({
                    "code": -5, "message": "not found"}),
                "sendrawtransaction": relayer.RpcError({
                    "code": -26, "message": "txn-mempool-conflict"})})
            relayer.process_jobs(state, node, wallet, 1, False, 2000, 10000)
            self.assertEqual(state.first()["state"], "planned")
            self.assertIsNone(state.first()["txid"])
            self.assertEqual(sum(method == "submitbridgecarrier"
                                 for method, _ in wallet.calls), 1)

    def test_ambiguous_send_recovers_from_exact_mempool_txid(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 1)
            state.add_plan([{"kind": "bootstrap", "payload_hex": "01"}])
            txid = "aa" * 32
            not_found = relayer.RpcError({"code": -5, "message": "not found"})
            wallet = FakeRpc({"submitbridgecarrier": {
                "txid": txid, "hex": "00", "broadcast": False,
                "network_fee": "0.000001000"},
                "gettransaction": not_found})
            node = FakeRpc({"getbridgeinfo": {"active": True,
                                               "state_available": True,
                                               "light_client_bootstrapped": False},
                            "getfinalitystatus": {"finalized": {"height": 10}},
                            "sendrawtransaction": relayer.RpcError({
                                "code": -27, "message": "already known"}),
                            "getmempoolentry": {"vsize": 100}})
            relayer.process_jobs(state, node, wallet, 1, False, 2000, 10000)
            self.assertEqual(state.first()["state"], "broadcast")
            sends = sum(method == "sendrawtransaction" for method, _ in node.calls)
            relayer.process_jobs(state, node, wallet, 1, False, 2000, 10000)
            self.assertEqual(sum(method == "sendrawtransaction" for method, _ in node.calls), sends)

    def test_external_lc_exact_store_is_finalized_without_carrier(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 1)
            update = store("update", "02", 8192)
            state.add_plan([update])
            info = {"active": True, "state_available": True,
                    "mint_approval_open": True, "light_client_bootstrapped": True,
                    "light_client_period": 1, "finalized_beacon_slot": 8192,
                    "finalized_beacon_root": ROOT_A,
                    "current_sync_committee_root": ROOT_A,
                    "next_sync_committee_root": ROOT_B,
                    "finalized_execution_block": 100,
                    "finalized_execution_hash": H32,
                    "light_client_connected_height": 50,
                    "light_client_connected_block": "bb" * 32}
            node = FakeRpc({"getbridgeinfo": info,
                            "getfinalitystatus": {"finalized": {"height": 50}},
                            "getblockhash": "bb" * 32})
            wallet = FakeRpc()
            relayer.process_jobs(state, node, wallet, 1, False, 2000, 10000)
            self.assertIsNone(state.first())
            self.assertEqual(wallet.calls, [])
            state.add_plan([update])
            self.assertEqual(state.db.execute("SELECT COUNT(*) FROM jobs").fetchone()[0], 1)

    def test_external_mint_reconciles_before_closed_window(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 1)
            state.add_plan([{"kind": "mint", "payload_hex": "04", "amount": "5",
                             "address": "B3address", "deposit_id": 7,
                             "target_block_hash": H32}])
            node = FakeRpc({
                "getbridgeinfo": {"active": True, "state_available": True,
                                  "mint_approval_open": False},
                "getfinalitystatus": {"finalized": {"height": 60}},
                "getblockhash": "bb" * 32,
                "getbridgeproofstatus": {"state_available": True,
                                         "deposit": {"claimed": True,
                                                     "claimed_height": 59,
                                                     "claimed_block": "bb" * 32}}})
            wallet = FakeRpc()
            relayer.process_jobs(state, node, wallet, 1, False, 2000, 10000)
            self.assertIsNone(state.first())
            self.assertEqual(wallet.calls, [])

    def test_superseded_lc_keeps_later_dependency(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 1)
            state.add_plan([store("update", "02", 8192),
                            {"kind": "execution-backfill", "payload_hex": "03",
                             "target_block_hash": H32}])
            info = {"active": True, "state_available": True,
                    "mint_approval_open": True, "light_client_bootstrapped": True,
                    "light_client_period": 2, "finalized_beacon_slot": 16384,
                    "finalized_beacon_root": "0x" + "99" * 32,
                    "current_sync_committee_root": ROOT_B,
                    "next_sync_committee_root": None,
                    "finalized_execution_block": 200,
                    "finalized_execution_hash": "0x" + "88" * 32,
                    "light_client_connected_height": 50,
                    "light_client_connected_block": "bb" * 32}
            node = FakeRpc({
                "getbridgeinfo": info,
                "getfinalitystatus": {"finalized": {"height": 60}},
                "getblockhash": "bb" * 32,
                "getbridgeproofstatus": {"state_available": True,
                                         "anchor": {"known": True,
                                                    "connected_height": 61,
                                                    "connected_block": "bb" * 32}}})
            wallet = FakeRpc()
            relayer.process_jobs(state, node, wallet, 1, False, 2000, 10000)
            rows = state.db.execute("SELECT state FROM jobs ORDER BY id").fetchall()
            self.assertEqual([row[0] for row in rows], ["superseded", "planned"])
            self.assertEqual(wallet.calls, [])

    def test_confirmed_effect_disappearance_reopens_dependencies(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 1)
            state.add_plan([{"kind": "execution-backfill", "payload_hex": "03",
                             "target_block_hash": H32},
                            {"kind": "mint", "payload_hex": "04", "amount": "5",
                             "address": "B3address", "deposit_id": 7,
                             "target_block_hash": H32}])
            for row in state.pending():
                state.set_state(row["id"], "confirmed")
            node = FakeRpc({
                "getfinalitystatus": {"finalized": {"height": 60}},
                "getbridgeinfo": {"state_available": True},
                "getbridgeproofstatus": {"state_available": True,
                                         "anchor": {"known": False}}})
            relayer.audit_confirmed(state, node, FakeRpc(), 1)
            self.assertEqual([r["state"] for r in state.pending()], ["planned", "planned"])
            self.assertTrue(all(r["txid"] is None for r in state.pending()))

    def test_confirmed_effect_block_reorg_reopens(self):
        with tempfile.TemporaryDirectory() as directory:
            state = relayer.State(Path(directory) / "state.sqlite3")
            state.bind({"bridge": 1}, 1)
            state.add_plan([{"kind": "execution-backfill", "payload_hex": "03",
                             "target_block_hash": H32}])
            state.confirm_effect(state.first()["id"], 50, "aa" * 32)
            node = FakeRpc({"getfinalitystatus": {"finalized": {"height": 60}},
                            "getbridgeinfo": {"state_available": True},
                            "getblockhash": "bb" * 32})
            relayer.audit_confirmed(state, node, FakeRpc(), 1)
            self.assertEqual(state.first()["state"], "planned")
            self.assertIsNone(state.first()["effect_height"])


if __name__ == "__main__":
    unittest.main()
