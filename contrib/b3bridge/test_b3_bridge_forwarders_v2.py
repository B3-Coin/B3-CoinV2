#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Offline unit tests for b3_bridge_forwarders_v2.py (fake RPC objects, no network)."""
import contextlib
import io
import json
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import b3_bridge_deposit_guard_v2 as guard  # noqa: E402
import b3_bridge_forwarders_v2 as fw  # noqa: E402
import b3_bridge_observe_v2 as observer  # noqa: E402

VECTORS = Path(__file__).resolve().parents[2] / "contracts" / "test" / "vectors" / "bridge_v2_vectors.json"


def addr(n):
    return bytes(19) + bytes([n])


def word(n):
    return n.to_bytes(32, "big")


# =============================================================================
# Fake Ethereum and B3 endpoints.
# =============================================================================
class Revert(Exception):
    def __init__(self, data=b""):
        self.data = data


def reverts_with(signature):
    """A FakeEth view handler that reverts with the custom error `signature`."""
    def handler(args, tx):
        raise Revert(fw.selector(signature))
    return handler


class FakeEth:
    """Answers eth_* from registered contract handlers; records every call.

    view(address, signature, returns, fn): eth_call of `signature` at
    `address` returns encode(returns, fn(args, tx)); fn may raise Revert.
    tx(address, signature, fn): eth_sendTransaction runs fn(args, tx), which
    returns the receipt's logs as (address, [topics], data) triples.
    """

    def __init__(self, chain_id=31337, timestamp=1_800_000_000):
        self.chain_id = chain_id
        self.timestamp = timestamp
        self.block_number = 100
        self.code = {}
        self.views = {}
        self.txs = {}
        self.logs = []
        self.sent = []
        self.receipts = {}
        self.calls = []

    def view(self, address, signature, returns, fn):
        self.views[(address, fw.selector(signature))] = (signature, returns, fn)

    def tx(self, address, signature, fn):
        self.txs[(address, fw.selector(signature))] = (signature, fn)

    def methods(self):
        return [method for method, _ in self.calls]

    @staticmethod
    def _decode(signature, data):
        _, types = fw.split_signature(signature)
        return fw.decode_abi(types, data[4:])

    def call(self, method, params=()):
        self.calls.append((method, list(params)))
        if method == "eth_chainId":
            return hex(self.chain_id)
        if method == "eth_blockNumber":
            return hex(self.block_number)
        if method == "eth_getCode":
            return fw.to_0x(self.code.get(bytes.fromhex(params[0][2:]), b""))
        if method == "eth_getBlockByNumber":
            return {"number": hex(self.block_number), "timestamp": hex(self.timestamp)}
        if method == "eth_getLogs":
            return self._get_logs(params[0])
        if method == "eth_call":
            tx = params[0]
            to, data = bytes.fromhex(tx["to"][2:]), bytes.fromhex(tx["data"][2:])
            if (to, data[:4]) not in self.views:
                raise AssertionError(f"unexpected eth_call 0x{data[:4].hex()} at 0x{to.hex()}")
            signature, returns, fn = self.views[(to, data[:4])]
            try:
                value = fn(self._decode(signature, data), tx)
            except Revert as e:
                raise observer.RpcError("eth_call", {"code": 3, "message": "execution reverted",
                                                     "data": fw.to_0x(e.data)}) from None
            if isinstance(returns, (list, tuple)):
                return fw.to_0x(fw.encode_abi(list(returns), list(value)))
            return fw.to_0x(fw.encode_abi([returns], [value]))
        if method == "eth_sendTransaction":
            tx = params[0]
            to, data = bytes.fromhex(tx["to"][2:]), bytes.fromhex(tx["data"][2:])
            self.sent.append(tx)
            signature, fn = self.txs[(to, data[:4])]
            logs = fn(self._decode(signature, data), tx)
            self.block_number += 1
            tx_hash = "0x" + word(len(self.sent)).hex()
            self.receipts[tx_hash] = {
                "status": "0x1", "transactionHash": tx_hash, "blockNumber": hex(self.block_number),
                "transactionIndex": "0x0", "cumulativeGasUsed": hex(90_000), "type": "0x2",
                "logsBloom": "0x" + "00" * 256,
                "logs": [{"address": fw.to_0x(a), "topics": [fw.to_0x(t) for t in topics], "data": fw.to_0x(d),
                          "blockNumber": hex(self.block_number), "logIndex": hex(i)}
                         for i, (a, topics, d) in enumerate(logs)]}
            return tx_hash
        if method == "eth_getTransactionReceipt":
            return self.receipts.get(params[0])
        raise AssertionError(f"unexpected Ethereum RPC {method}")

    def _get_logs(self, query):
        address = bytes.fromhex(query["address"][2:])
        out = []
        for log in self.logs:
            if bytes.fromhex(log["address"][2:]) != address:
                continue
            ok = True
            for i, wanted in enumerate(query["topics"]):
                if wanted is None:
                    continue
                choices = wanted if isinstance(wanted, list) else [wanted]
                if i >= len(log["topics"]) or log["topics"][i] not in choices:
                    ok = False
            if ok:
                out.append(log)
        return out

    def add_log(self, address, topics, data, block=50, index=0, tx="0x" + "11" * 32):
        self.logs.append({"address": fw.to_0x(address), "topics": [fw.to_0x(t) for t in topics],
                          "data": fw.to_0x(data), "blockNumber": hex(block), "logIndex": hex(index),
                          "transactionHash": tx})


class FakeNode:
    def __init__(self, replies):
        self.replies = dict(replies)
        self.calls = []

    def call(self, method, params=()):
        self.calls.append((method, list(params)))
        if method not in self.replies:
            raise AssertionError(f"unexpected B3 RPC {method}")
        reply = self.replies[method]
        if isinstance(reply, Exception):
            raise reply
        return reply(params) if callable(reply) else reply

    def methods(self):
        return [method for method, _ in self.calls]


# =============================================================================
# A route (b) world: one Vault V2, its deposit factory and one F_d.
# =============================================================================
VAULT, TOKEN, FD_FACTORY, VERIFIER = addr(0x0A), addr(0x0B), addr(0x0F), addr(0x0C)
SENDER, REFUND_EOA, OTHER = addr(0x51), addr(0x52), addr(0x53)
INIT_CODE_HASH = fw.keccak256(b"B3-TEST/forwarder-init-code")
RECIPIENT = (1 << 160 | 0x1234).to_bytes(32, "big")
MAX_DEPOSIT = 10_000_000_000
FEE = 250_000
NOW = 1_800_000_000


def bridge_v2_info(**overrides):
    info = {
        "configured": True, "active": True, "state_available": True, "next_height": 420,
        "activation_height": 281, "origin_chain_id": 31337, "origin_deployment_block": 3,
        "vault": fw.to_0x(VAULT), "token": fw.to_0x(TOKEN), "verifier": fw.to_0x(VERIFIER),
        "deposits": {"status": "OPEN"}, "light_client": {"bootstrapped": False},
        "max_per_block": MAX_DEPOSIT, "max_per_epoch": 3 * MAX_DEPOSIT,
        "forwarders": {"deposit": {"factory": fw.to_0x(FD_FACTORY), "init_code_hash": fw.to_0x(INIT_CODE_HASH)},
                       "exit": {"factory": fw.to_0x(addr(0x0E)), "init_code_hash": fw.to_0x(INIT_CODE_HASH),
                                "dst_eid": 30110}},
    }
    info.update(overrides)
    return info


class FlushWorld:
    def __init__(self, balance=25_000_000_000, deployed=False, fee_cap=4 * FEE):
        self.p = fw.DepositParams(VAULT, RECIPIENT, REFUND_EOA, fee_cap, NOW + 30 * 86400)
        self.account = fw.deposit_forwarder_address(FD_FACTORY, INIT_CODE_HASH, self.p)
        self.balance, self.reserved, self.fees_charged = balance, 0, 0
        self.deployed, self.gate_open, self.next_id = deployed, True, 7
        self.node = FakeNode({"getblockchaininfo": {"chain": "regtest"}, "getbridgev2info": bridge_v2_info()})
        eth = self.eth = FakeEth(timestamp=NOW)
        for contract in (VAULT, TOKEN, FD_FACTORY):
            eth.code[contract] = b"\x60\x00"
        if deployed:
            eth.code[self.account] = b"\x60\x00"
        eth.view(FD_FACTORY, fw.SIG_D_ACCOUNT_OF, "address", lambda a, tx: fw.deposit_forwarder_address(
            FD_FACTORY, INIT_CODE_HASH, fw.DepositParams(*a[0])))
        eth.view(FD_FACTORY, fw.SIG_D_VAULT, "address", lambda a, tx: VAULT)
        eth.view(FD_FACTORY, fw.SIG_TOKEN, "address", lambda a, tx: TOKEN)
        eth.view(FD_FACTORY, fw.SIG_D_MIN_FLUSH_RAW, "uint256", lambda a, tx: fw.MIN_FLUSH_RAW)
        eth.view(FD_FACTORY, fw.SIG_D_MAX_DEPOSIT_RAW, "uint256", lambda a, tx: MAX_DEPOSIT)
        eth.view(FD_FACTORY, fw.SIG_D_FEES_CHARGED, "uint256", lambda a, tx: self.fees_charged)
        eth.view(FD_FACTORY, fw.SIG_D_FREE_BALANCE, "uint256", lambda a, tx: max(self.balance - self.reserved, 0))
        eth.view(FD_FACTORY, fw.SIG_D_IS_ACCOUNT, "bool", lambda a, tx: self.deployed)
        eth.view(VAULT, fw.SIG_VAULT_DEPOSIT_GATE_OPEN, "bool", lambda a, tx: self.gate_open)
        eth.view(FD_FACTORY, fw.SIG_D_DEPLOY, "address", self._deploy_check)
        eth.view(FD_FACTORY, fw.SIG_D_FLUSH, "uint256", lambda a, tx: self._flush_amount(a[1], tx))
        eth.tx(FD_FACTORY, fw.SIG_D_DEPLOY, self._deploy)
        eth.tx(FD_FACTORY, fw.SIG_D_FLUSH, self._flush)
        self.extra_flush_logs = []

    def _deploy_check(self, args, tx):
        if self.deployed:
            raise Revert(fw.selector("AlreadyDeployed()"))
        return self.account

    def _deploy(self, args, tx):
        self._deploy_check(args, tx)
        self.deployed = True
        self.eth.code[self.account] = b"\x60\x00"
        return [(FD_FACTORY, [fw.event_topic(fw.EV_PUBLISHED_DEPOSIT), fw.address_word(self.account)],
                 fw.encode_abi([fw.DEPOSIT_PARAMS_TYPE], [self.p.abi()]))]

    def _flush_amount(self, fee, tx):
        if not self.deployed:
            raise Revert(fw.selector("NotDeployed()"))
        if not self.gate_open:
            raise Revert(fw.selector("DepositClosed()"))
        if self.fees_charged + fee > self.p.fee_cap:
            raise Revert(fw.selector("FeeCapExceeded()"))
        amount = min(self.balance - self.reserved - fee, MAX_DEPOSIT)
        if amount < fw.MIN_FLUSH_RAW:
            raise Revert(fw.selector("BelowMinimum()"))
        return amount

    def _flush(self, args, tx):
        fee = args[1]
        amount = self._flush_amount(fee, tx)
        self.fees_charged += fee
        self.reserved += fee
        self.balance -= amount
        deposit_id = self.next_id
        self.next_id += 1
        return [(TOKEN, [fw.event_topic(fw.EV_TRANSFER), fw.address_word(self.account), fw.address_word(VAULT)],
                 word(amount)),
                (VAULT, [fw.event_topic(fw.EV_DEPOSIT), word(deposit_id), fw.address_word(TOKEN)],
                 word(amount) + RECIPIENT)] + self.extra_flush_logs

    def flush(self, fee=FEE, **kwargs):
        if "account" not in kwargs:
            kwargs.setdefault("params", self.p)
        return fw.flush_forwarder(self.node, self.eth, SENDER, FD_FACTORY, fee, **kwargs)

    def sends(self):
        return [bytes.fromhex(tx["data"][2:10]) for tx in self.eth.sent]


# =============================================================================
# Tests
# =============================================================================
class VectorTests(unittest.TestCase):
    """The derivations equal contracts/test/vectors/bridge_v2_vectors.json forwarder_v2."""

    @classmethod
    def setUpClass(cls):
        if not VECTORS.is_file():
            raise unittest.SkipTest(f"{VECTORS} is missing")
        cls.vectors = json.loads(VECTORS.read_text())["forwarder_v2"]

    def test_tags(self):
        self.assertEqual(fw.to_0x(fw.DEPOSIT_TAG), self.vectors["tags"]["deposit"])
        self.assertEqual(fw.to_0x(fw.EXIT_TAG), self.vectors["tags"]["exit"])
        # B3ForwarderTypesV2's hard-coded values (cast keccak).
        self.assertEqual(fw.DEPOSIT_TAG.hex(), "cc2b88b5788e6a3d2f51c52fcb908e55a5386859799df6e4878912fdb1b01d2a")
        self.assertEqual(fw.EXIT_TAG.hex(), "0fdc29b9af6b10d38ef4781cb890c8a6d762464db73cc2f5800457cdf07b4c87")
        self.assertEqual(self.vectors["init_code_hash"],
                         fw.to_0x(fw.keccak256(b"B3-VECTORS/forwarder-init-code")))

    def test_deposit_forwarders(self):
        ich = self.vectors["init_code_hash"]
        self.assertGreaterEqual(len(self.vectors["deposit"]), 3)
        for case in self.vectors["deposit"]:
            with self.subTest(case["name"]):
                p = fw.DepositParams.from_json(case)
                preimage = fw.deposit_salt_preimage(p)
                self.assertEqual(len(preimage), fw.SALT_PREIMAGE_LENGTH)
                self.assertEqual(fw.to_0x(preimage), case["salt_preimage"])
                self.assertEqual(preimage, fw.encode_abi(
                    ["bytes32", "address", "bytes32", "address", "uint96", "uint64"],
                    [fw.DEPOSIT_TAG, p.vault, p.b3_recipient, p.refund_eoa, p.fee_cap, p.not_after]))
                self.assertEqual(fw.to_0x(fw.deposit_salt(p)), case["salt"])
                self.assertEqual(len(fw.create2_preimage(case["factory"], case["salt"], ich)),
                                 fw.CREATE2_PREIMAGE_LENGTH)
                self.assertEqual(fw.to_0x(fw.deposit_forwarder_address(case["factory"], ich, p)), case["account"])
                self.assertEqual(fw.to_0x(fw.create2_address(case["factory"], case["salt"], ich)), case["account"])

    def test_exit_forwarders(self):
        ich = self.vectors["init_code_hash"]
        pinned = self.vectors["pinned_dst_eid"]
        self.assertGreaterEqual(len(self.vectors["exit"]), 3)
        for case in self.vectors["exit"]:
            with self.subTest(case["name"]):
                p = fw.ExitParams.from_json(case)
                preimage = fw.exit_salt_preimage(p)
                self.assertEqual(fw.to_0x(preimage), case["salt_preimage"])
                self.assertEqual(preimage, fw.encode_abi(
                    ["bytes32", "uint32", "bytes32", "uint16", "uint96", "address"],
                    [fw.EXIT_TAG, p.dst_eid, p.to, p.max_bridge_fee_bps, p.fee_cap, p.refund_eoa]))
                self.assertEqual(fw.to_0x(fw.exit_salt(p)), case["salt"])
                self.assertEqual(fw.to_0x(fw.exit_forwarder_address(case["factory"], ich, p)), case["account"])
                self.assertEqual(fw.exit_params_valid(p, pinned), case["is_valid"])

    def test_deposit_fee_cap(self):
        for case in self.vectors["deposit_fee_cap"]:
            with self.subTest(case):
                self.assertEqual(fw.deposit_fee_cap(case["fee_per_tx"]), case["fee_cap"])
        self.assertEqual(fw.deposit_fee_cap(FEE), 4 * FEE)
        self.assertIsNone(fw.deposit_fee_cap(0))
        self.assertIsNone(fw.deposit_fee_cap(fw.UINT64_MAX // 4 + 1))


class ValidityTests(unittest.TestCase):
    def test_recipient_and_address_words(self):
        self.assertTrue(fw.recipient_v1_valid(RECIPIENT))
        for bad in (bytes(32), (1 << 160).to_bytes(32, "big"), (2 << 160 | 1).to_bytes(32, "big"),
                    (1 << 200 | 1 << 160 | 1).to_bytes(32, "big")):
            self.assertFalse(fw.recipient_v1_valid(bad))
        self.assertTrue(fw.address_word_valid(fw.address_word(OTHER)))
        self.assertFalse(fw.address_word_valid(bytes(32)))
        self.assertFalse(fw.address_word_valid((1 << 160 | 5).to_bytes(32, "big")))

    def test_deposit_params_valid(self):
        p = fw.DepositParams(VAULT, RECIPIENT, REFUND_EOA, 1, 2)
        self.assertTrue(fw.deposit_params_valid(p, VAULT, TOKEN, FD_FACTORY))
        self.assertFalse(fw.deposit_params_valid(p, OTHER, TOKEN, FD_FACTORY))
        for refund in (bytes(20), VAULT, TOKEN, FD_FACTORY):
            self.assertFalse(fw.deposit_params_valid(fw.DepositParams(VAULT, RECIPIENT, refund, 1, 2),
                                                     VAULT, TOKEN, FD_FACTORY))
        self.assertFalse(fw.deposit_params_valid(fw.DepositParams(VAULT, bytes(32), REFUND_EOA, 1, 2),
                                                 VAULT, TOKEN, FD_FACTORY))

    def test_exit_params_valid(self):
        good = fw.ExitParams(30110, fw.address_word(OTHER), 50, 0, REFUND_EOA)
        self.assertTrue(fw.exit_params_valid(good, 30110))
        self.assertFalse(fw.exit_params_valid(good, 30101))
        self.assertFalse(fw.exit_params_valid(good, 0))
        self.assertFalse(fw.exit_params_valid(fw.ExitParams(30110, fw.address_word(OTHER), 51, 0, REFUND_EOA), 30110))
        self.assertFalse(fw.exit_params_valid(fw.ExitParams(30110, bytes(32), 0, 0, REFUND_EOA), 30110))
        self.assertFalse(fw.exit_params_valid(
            fw.ExitParams(30110, (1 << 160 | 1).to_bytes(32, "big"), 0, 0, REFUND_EOA), 30110))
        self.assertFalse(fw.exit_params_valid(fw.ExitParams(30110, fw.address_word(OTHER), 0, 0, bytes(20)), 30110))

    def test_params_ranges_and_json(self):
        with self.assertRaisesRegex(observer.ObserverError, "p.fee_cap does not fit uint96"):
            fw.DepositParams(VAULT, RECIPIENT, REFUND_EOA, 1 << 96, 1)
        with self.assertRaisesRegex(observer.ObserverError, "p.not_after does not fit uint64"):
            fw.DepositParams(VAULT, RECIPIENT, REFUND_EOA, 1, 1 << 64)
        with self.assertRaisesRegex(observer.ObserverError, "p.max_bridge_fee_bps does not fit uint16"):
            fw.ExitParams(1, fw.address_word(OTHER), 1 << 16, 0, REFUND_EOA)
        p = fw.DepositParams(VAULT, RECIPIENT, REFUND_EOA, 5, 6)
        # bridgeforwarderaddress's whole result or its params object.
        self.assertEqual(fw.DepositParams.from_json({"params": p.to_json(), "forwarder": "0x"}), p)
        self.assertEqual(fw.DepositParams.from_json(p.to_json()), p)
        with self.assertRaisesRegex(fw.ForwarderError, "lack not_after"):
            fw.DepositParams.from_json({k: v for k, v in p.to_json().items() if k != "not_after"})


class AbiTests(unittest.TestCase):
    # forge inspect method identifiers of the contracts this relayer calls.
    SELECTORS = {
        fw.SIG_RELAY_WITH_FORWARDS: "770bdba5", fw.SIG_D_FLUSH: "9c032127", fw.SIG_D_DEPLOY: "e8195283",
        fw.SIG_D_ACCOUNT_OF: "b4ffc84f", fw.SIG_D_IS_VALID: "dd3d5fc2", fw.SIG_D_FREE_BALANCE: "95b881de",
        fw.SIG_D_FEES_CHARGED: "11e11895", fw.SIG_D_IS_ACCOUNT: "25ca4c9c", fw.SIG_D_VAULT: "411557d1",
        fw.SIG_D_MAX_DEPOSIT_RAW: "bb326fef", fw.SIG_D_MIN_FLUSH_RAW: "f6f07edf", fw.SIG_TOKEN: "82bfefc8",
        fw.SIG_INIT_CODE_HASH: "928cfec6", fw.SIG_X_ACCOUNT_OF: "b99fd154", fw.SIG_X_IS_VALID: "02aa55d0",
        fw.SIG_X_QUOTE_FORWARD: "a0a5d397", fw.SIG_X_FORWARD: "e2321e3d", fw.SIG_X_DST_EID: "176e619e",
        fw.SIG_VAULT_DEPOSIT_GATE_OPEN: "4d8ef4d0", fw.SIG_VAULT_RELEASED: "e2672708",
        fw.SIG_VAULT_RELEASE: "ca514def", fw.SIG_BALANCE_OF: "70a08231",
    }
    # cast keccak of each event signature.
    TOPICS = {
        fw.EV_PUBLISHED_DEPOSIT: "fa15fad523904866973f01217953dbd55156e04b477621afc38fbb8cc7849f43",
        fw.EV_PUBLISHED_EXIT: "3cd7c0bcf419c28073622fb8abfe46393b06cc2fe502942a3dd48f2c37041662",
        fw.EV_TRANSFER: "ddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef",
        fw.EV_DEPOSIT: "daf0af297d25c0e96a0b209d35692b4e07c503634eeca57fc5c35c006acf527f",
        fw.EV_WITHDRAWN: "fbdd71ee17bf8a90c045ef67aac39ce54b0cbac598e6d332e2543046225b9099",
        fw.EV_FORWARDED: "871e2cef2de9601ba40f60434470f052a72ad5fe40409a55a3128f590ff9dd03",
        fw.EV_FORWARD_FAILED: "f94916ff3cbeae39122b151afee40dba8eb5ab786137ad954ec62ee9ab0f004a",
        fw.EV_FORWARD_SKIPPED: "0d70c5c5256ed4a14b1d87d224afec1e0d862becb9b9a3d957bdaadf4acedb9f",
        fw.EV_RELEASE_FAILED: "3e8a4268a1d17fe3b418081f3df8ab2e4dbdc7fb92a93c92d251190f5089f430",
        fw.EV_RELEASE_SKIPPED: "9193ed809ea4e4affc100a4369d345644745afb2d352c57bafddafc4b8511e2a",
        fw.EV_CERTIFICATE_FAILED: "ddef6cbc91ae598d6dfea6d61f5e630e2f6460aaa9d4306e73c47d240e2e4d2d",
    }

    def test_selectors_and_topics(self):
        for signature, expected in self.SELECTORS.items():
            self.assertEqual(fw.selector(signature).hex(), expected, signature)
        for signature, expected in self.TOPICS.items():
            self.assertEqual(fw.event_topic(signature).hex(), expected, signature)

    def test_flush_calldata_layout(self):
        p = fw.DepositParams(VAULT, RECIPIENT, REFUND_EOA, 4 * FEE, NOW)
        data = fw.encode_call(fw.SIG_D_FLUSH, p.abi(), FEE)
        self.assertEqual(len(data), 4 + 6 * 32)
        self.assertEqual(data[:4].hex(), "9c032127")
        words = [data[4 + 32 * i:36 + 32 * i] for i in range(6)]
        self.assertEqual(words, [fw.address_word(VAULT), RECIPIENT, fw.address_word(REFUND_EOA), word(4 * FEE),
                                 word(NOW), word(FEE)])

    def test_relay_with_forwards_calldata_layout(self):
        withdrawal = (9, OTHER, 5_000_000, 350)
        path = [word(i + 1) for i in range(32)]
        xp = fw.ExitParams(30110, fw.address_word(OTHER), 50, 3_000_000, REFUND_EOA)
        data = fw.encode_call(fw.SIG_RELAY_WITH_FORWARDS, VERIFIER, [], 0, VAULT, [(withdrawal, path)], 500_000,
                              addr(0x0E), [(xp.abi(), 1_000_000, SENDER, 123)], 1_500_000, SENDER)
        self.assertEqual(data[:4].hex(), "770bdba5")
        head = [int.from_bytes(data[4 + 32 * i:36 + 32 * i], "big") for i in range(10)]
        # Ten head slots; the three arrays are dynamic.
        self.assertEqual(head[1], 320)
        self.assertEqual(head[4], 352)
        self.assertEqual(head[7], 352 + 32 + 36 * 32)
        self.assertEqual((head[2], head[5], head[8]), (0, 500_000, 1_500_000))
        body = data[4:]
        self.assertEqual(body[320:352], word(0))  # no certificates
        self.assertEqual(body[352:384], word(1))
        self.assertEqual(body[384:384 + 4 * 32], b"".join([word(9), fw.address_word(OTHER), word(5_000_000),
                                                         word(350)]))
        self.assertEqual(body[384 + 4 * 32:384 + 36 * 32], b"".join(path))
        forwards = body[head[7]:]
        self.assertEqual(len(forwards), 32 + 8 * 32)
        self.assertEqual(forwards[:32], word(1))
        self.assertEqual(forwards[32:32 + 5 * 32], b"".join([word(30110), xp.to, word(50), word(3_000_000),
                                                            fw.address_word(REFUND_EOA)]))
        self.assertEqual(forwards[192:], word(1_000_000) + fw.address_word(SENDER) + word(123))
        _, types = fw.split_signature(fw.SIG_RELAY_WITH_FORWARDS)
        decoded = fw.decode_abi(types, body)
        self.assertEqual(decoded[4][0][0], (9, fw.to_0x(OTHER), 5_000_000, 350))
        self.assertEqual(decoded[7][0][3], 123)

    def test_dynamic_round_trip_and_refusals(self):
        packed = fw.encode_abi(["bytes32", "bytes"], [word(7), b"\x01" * 40])
        self.assertEqual(len(packed), 32 + 32 + 32 + 64)
        self.assertEqual(fw.decode_abi(["bytes32", "bytes"], packed), [word(7), b"\x01" * 40])
        self.assertEqual(fw.decode_abi(["(uint8,bytes)[]"], fw.encode_abi(["(uint8,bytes)[]"],
                                                                         [[(1, b"ab"), (2, b"")]]))[0],
                         [(1, b"ab"), (2, b"")])
        for bad_type in ("uint7", "bytes33", "(uint256", "uint256[0]", "foo"):
            with self.assertRaises(fw.ForwarderError):
                fw.parse_type(bad_type)
        with self.assertRaisesRegex(fw.ForwarderError, "does not fit uint8"):
            fw.encode_abi(["uint8"], [256])
        with self.assertRaisesRegex(fw.ForwarderError, "non-zero upper bytes"):
            fw.decode_abi(["address"], (1 << 160).to_bytes(32, "big"))
        with self.assertRaisesRegex(fw.ForwarderError, "too short"):
            fw.decode_abi(["uint256", "uint256"], word(1))

    def test_describe_revert(self):
        self.assertEqual(fw.describe_revert(fw.selector("Expired()")), "Expired()")
        self.assertEqual(fw.describe_revert(fw.selector("NativeFeeShort(uint256,uint256)") + word(5) + word(3)),
                         "NativeFeeShort(5, 3)")
        self.assertEqual(fw.describe_revert(fw.selector("Error(string)") + fw.encode_abi(["string"], ["no"])),
                         "'no'")
        self.assertEqual(fw.describe_revert(b""), "reverted without data")
        self.assertEqual(fw.describe_revert(b"\xde\xad\xbe\xef"), "0xdeadbeef")


class ReceiptSizeTests(unittest.TestCase):
    """The model equals contracts/test/v2/utils/ReceiptSizeV2.sol's hand-computed sizes."""

    def test_model(self):
        self.assertEqual(fw.WORST_RECEIPT_KEY, b"\x82\xff\xff")
        self.assertEqual(fw.receipt_leaf_size([]), 285)
        self.assertEqual(fw.receipt_leaf_size([(addr(0x0E), [], b"")]), 309)
        # test_PR3_FlushReceiptLeafWithinBudget: 634, within 1,133 and 7,168.
        self.assertEqual(fw.predicted_flush_leaf_size(), 634)
        self.assertLessEqual(fw.predicted_flush_leaf_size(), fw.PR3_LEAF_LIMIT)
        # Junk logs (what PR-2 makes impossible) would trip PR-3.
        self.assertGreater(fw.predicted_flush_leaf_size(fw.FLUSH_LOG_SHAPE + ((4, 1_100),) * 6),
                           fw.PR3_LEAF_LIMIT)

    def test_actual_receipt_leaf(self):
        world = FlushWorld(deployed=True)
        world.flush()
        receipt = next(iter(world.eth.receipts.values()))
        # Key rlp(0) = 0x80 instead of 0x82ffff (path 3 bytes, not 5) and a
        # 3-byte cumulativeGasUsed instead of 8: 634 - 2 - 5 bytes.
        self.assertEqual(fw.actual_receipt_leaf_size(receipt), 627)
        self.assertEqual(fw.receipt_leaf_size(fw.receipt_logs(receipt)), 634)


class FlushTests(unittest.TestCase):
    def test_codeless_forwarder_is_deployed_then_flushed(self):
        world = FlushWorld()
        out = world.flush()
        self.assertEqual(world.sends(), [fw.selector(fw.SIG_D_DEPLOY), fw.selector(fw.SIG_D_FLUSH)])
        self.assertEqual(out["status"], "flushed")
        self.assertEqual(out["account"], fw.to_0x(world.account))
        self.assertEqual((out["amount"], out["fee"], out["deposit_id"]), (MAX_DEPOSIT, FEE, 7))
        self.assertEqual(out["deploy_tx"], "0x" + word(1).hex())
        self.assertEqual(out["flush_tx"], "0x" + word(2).hex())
        self.assertEqual(out["remaining_free"], 25_000_000_000 - MAX_DEPOSIT - FEE)
        self.assertEqual(out["leaf_size_worst_case"], 634)
        # B3's guard ran first; the vault's gate was read; the sender is the --from EOA.
        self.assertEqual(world.node.methods()[:2], ["getblockchaininfo", "getbridgev2info"])
        self.assertEqual({tx["from"] for tx in world.eth.sent}, {fw.to_0x(SENDER)})
        # Every send was simulated by eth_call first.
        methods = world.eth.methods()
        self.assertLess(methods.index("eth_call", methods.index("eth_getBlockByNumber")),
                        methods.index("eth_sendTransaction"))

    def test_deployed_forwarder_is_only_flushed_and_parts_follow(self):
        world = FlushWorld(deployed=True)
        self.assertEqual(world.flush()["amount"], MAX_DEPOSIT)
        self.assertEqual(world.flush()["amount"], MAX_DEPOSIT)
        out = world.flush()
        self.assertEqual(out["amount"], 25_000_000_000 - 2 * MAX_DEPOSIT - 3 * FEE)
        self.assertEqual(out["remaining_free"], 0)
        self.assertEqual(world.sends(), [fw.selector(fw.SIG_D_FLUSH)] * 3)
        # D5: a fourth fee fits the 4-fee cap but nothing is left.
        self.assertEqual(world.flush()["status"], "nothing-to-flush")
        with self.assertRaisesRegex(fw.ForwarderError, "exceeds what is left of the lifetime fee cap"):
            world.flush(fee=FEE + 1)
        self.assertEqual(len(world.eth.sent), 3)

    def test_guard_refusal_sends_nothing(self):
        world = FlushWorld()
        world.node.replies["getbridgev2info"] = bridge_v2_info(
            deposits={"status": "CLOSED_FOR_GOOD", "reason": "sticky kind 13 conflict"})
        with self.assertRaisesRegex(guard.DepositsClosedError,
                                    "^bridge V2 deposits are closed for good: sticky kind 13 conflict; this "
                                    "relayer refuses deposits and flushes$"):
            world.flush()
        self.assertEqual(world.eth.sent, [])
        self.assertEqual(world.eth.calls, [])
        world.node.replies["getbridgev2info"] = bridge_v2_info(deposits={"status": "UNAVAILABLE", "reason": "x"})
        with self.assertRaisesRegex(observer.ObserverError, "deposit status is unavailable"):
            world.flush()
        world.node.replies["getblockchaininfo"] = {"chain": "main"}
        with self.assertRaisesRegex(observer.ObserverError, "regtest only"):
            world.flush()
        self.assertEqual(world.eth.calls, [])

    def test_retired_vault_refused(self):
        world = FlushWorld()
        world.node.replies["getbridgev2info"] = bridge_v2_info(retired=True)
        with self.assertRaisesRegex(fw.ForwarderError, "launch vault entry is retired"):
            world.flush()
        self.assertEqual(world.eth.sent, [])
        world.node.replies["getbridgev2info"] = bridge_v2_info(retired=False)
        self.assertEqual(world.flush()["status"], "flushed")

    def test_mint_readiness_refusals(self):
        """Node changes section 11.3: nothing is sent unless B3 can mint the flush promptly."""
        day = 24 * 60 * 60
        fresh = {"bootstrapped": True, "fresh": True, "stale_after": NOW + 3600, "horizon_time": NOW + 30 * day}
        cases = [
            ({"light_client": dict(fresh, fresh=False)}, "light client is stale at B3"),
            ({"light_client": dict(fresh, horizon_time=NOW + 7 * day - 1)}, "horizon .* is less than 7 days"),
            ({"max_per_block": MAX_DEPOSIT - 1}, "exceeds B3's max_per_block"),
            ({"mint_epoch": 3, "epoch_minted": 1, "epoch_mint_headroom": MAX_DEPOSIT - 1},
             "exceeds the mint epoch's headroom"),
            ({"light_client": None}, "no light_client status"),
            ({"light_client": {}}, "no light_client status"),
        ]
        for overrides, message in cases:
            with self.subTest(message):
                world = FlushWorld()
                world.node.replies["getbridgev2info"] = bridge_v2_info(**overrides)
                with self.assertRaisesRegex(fw.ForwarderError, message):
                    world.flush()
                self.assertEqual(world.eth.sent, [])
        # At the bounds, and before the bootstrap without freshness or horizon.
        for overrides in ({"light_client": dict(fresh, horizon_time=NOW + 7 * day)},
                          {"epoch_mint_headroom": MAX_DEPOSIT, "max_per_block": MAX_DEPOSIT},
                          {"light_client": {"bootstrapped": False, "fresh": False}}):
            with self.subTest(overrides):
                world = FlushWorld()
                world.node.replies["getbridgev2info"] = bridge_v2_info(**overrides)
                self.assertEqual(world.flush()["status"], "flushed")
        # The amount checked is the one the flush deposits: min(free - fee, MAX_DEPOSIT_RAW).
        world = FlushWorld(balance=5_000_000)
        world.node.replies["getbridgev2info"] = bridge_v2_info(epoch_mint_headroom=5_000_000 - FEE)
        self.assertEqual(world.flush()["amount"], 5_000_000 - FEE)
        world = FlushWorld(balance=5_000_000)
        world.node.replies["getbridgev2info"] = bridge_v2_info(epoch_mint_headroom=5_000_000 - FEE - 1)
        with self.assertRaisesRegex(fw.ForwarderError, "headroom"):
            world.flush()

    def test_no_receipt_size_skip(self):
        """PR-3: the flush log shape is fixed, so there is no pre-send skip;
        every flushed receipt reports its leaf sizes within the bound."""
        out = FlushWorld().flush()
        self.assertNotIn("predicted_leaf_size", out)
        self.assertLessEqual(out["leaf_size_worst_case"], fw.PR3_LEAF_LIMIT)
        self.assertLessEqual(out["leaf_size"], out["leaf_size_worst_case"])

    def test_nothing_to_flush(self):
        for balance in (0, fw.MIN_FLUSH_RAW + FEE - 1):
            world = FlushWorld(balance=balance)
            out = world.flush()
            self.assertEqual(out["status"], "nothing-to-flush")
            self.assertEqual(out["free"], balance)
            self.assertEqual(world.eth.sent, [])
        world = FlushWorld(balance=fw.MIN_FLUSH_RAW + FEE)
        self.assertEqual(world.flush()["amount"], fw.MIN_FLUSH_RAW)

    def test_gate_closed_refused(self):
        world = FlushWorld()
        world.gate_open = False
        with self.assertRaisesRegex(fw.ForwarderError, "depositGateOpen\\(\\) is false"):
            world.flush()
        self.assertEqual(world.eth.sent, [])

    def test_identity_refusals(self):
        world = FlushWorld()
        with self.assertRaisesRegex(fw.ForwarderError, "is not the pinned deposit forwarder factory"):
            fw.flush_forwarder(world.node, world.eth, SENDER, OTHER, FEE, params=world.p)
        other_vault = fw.DepositParams(OTHER, RECIPIENT, REFUND_EOA, 1, NOW + 1)
        with self.assertRaisesRegex(fw.ForwarderError, "is not the bridge V2 vault"):
            world.flush(params=other_vault)
        with self.assertRaisesRegex(fw.ForwarderError, "params are invalid"):
            world.flush(params=fw.DepositParams(VAULT, RECIPIENT, TOKEN, 1, NOW + 1))
        # factory.accountOf disagrees with the pinned init-code hash.
        world.eth.view(FD_FACTORY, fw.SIG_D_ACCOUNT_OF, "address", lambda a, tx: OTHER)
        with self.assertRaisesRegex(fw.ForwarderError, "accountOf\\(p\\) is"):
            world.flush()
        world = FlushWorld()
        world.eth.code[SENDER] = b"\xef\x01\x00" + bytes(20)  # an EIP-7702 delegation
        with self.assertRaisesRegex(fw.ForwarderError, "PR-2"):
            world.flush()
        world = FlushWorld()
        world.node.replies["getbridgev2info"] = bridge_v2_info(forwarders={})
        with self.assertRaisesRegex(fw.ForwarderError, "no deposit forwarder factory is pinned"):
            world.flush()
        self.assertEqual(world.eth.sent, [])

    def test_ethereum_chain_refusals(self):
        for chain_id in (1, 11155111, 31338):
            world = FlushWorld()
            world.eth.chain_id = chain_id
            with self.assertRaisesRegex(fw.ForwarderError, f"refusing Ethereum chain id {chain_id}"):
                world.flush()
            self.assertEqual(world.eth.methods(), ["eth_chainId"])
        world = FlushWorld()
        world.node.replies["getbridgev2info"] = bridge_v2_info(origin_chain_id=1)
        with self.assertRaisesRegex(fw.ForwarderError, "vault entry is on chain 1"):
            world.flush()

    def test_expired_and_fee_cap_refused(self):
        world = FlushWorld()
        world.eth.timestamp = world.p.not_after
        with self.assertRaisesRegex(fw.ForwarderError, "expired at"):
            world.flush()
        world = FlushWorld(fee_cap=FEE - 1)
        with self.assertRaisesRegex(fw.ForwarderError, "lifetime fee cap"):
            world.flush()
        self.assertEqual(world.eth.sent, [])

    def test_account_from_published(self):
        world = FlushWorld()
        world.eth.add_log(FD_FACTORY, [fw.event_topic(fw.EV_PUBLISHED_DEPOSIT), fw.address_word(world.account)],
                          fw.encode_abi([fw.DEPOSIT_PARAMS_TYPE], [world.p.abi()]))
        out = world.flush(account=world.account)
        self.assertEqual(out["status"], "flushed")
        self.assertEqual(fw.DepositParams.from_json(out["params"]), world.p)
        query = [params[0] for method, params in world.eth.calls if method == "eth_getLogs"][0]
        self.assertEqual(query["fromBlock"], hex(3))  # origin_deployment_block
        unknown = fw.deposit_forwarder_address(FD_FACTORY, INIT_CODE_HASH,
                                               fw.DepositParams(VAULT, RECIPIENT, REFUND_EOA, 1, 1))
        with self.assertRaisesRegex(fw.ForwarderError, "no Published event names forwarder"):
            world.flush(account=unknown)

    def test_post_check_refuses_extra_logs(self):
        world = FlushWorld(deployed=True)
        world.extra_flush_logs = [(OTHER, [word(1)], b"")]
        with self.assertRaisesRegex(fw.ForwarderError, "PR-1 post-check: flush .* emitted 3 logs"):
            world.flush()

    def test_published_logs_decode(self):
        world = FlushWorld()
        xp = fw.ExitParams(30110, fw.address_word(OTHER), 10, 7, REFUND_EOA)
        world.eth.add_log(FD_FACTORY, [fw.event_topic(fw.EV_PUBLISHED_DEPOSIT), fw.address_word(world.account)],
                          fw.encode_abi([fw.DEPOSIT_PARAMS_TYPE], [world.p.abi()]), block=5)
        world.eth.add_log(FD_FACTORY, [fw.event_topic(fw.EV_PUBLISHED_EXIT), fw.address_word(OTHER)],
                          fw.encode_abi([fw.EXIT_PARAMS_TYPE], [xp.abi()]), block=6)
        world.eth.view(FD_FACTORY, fw.SIG_INIT_CODE_HASH, "bytes32", lambda a, tx: INIT_CODE_HASH)
        out = fw.list_published(world.eth, FD_FACTORY)
        self.assertEqual([(e["kind"], e["derivation_ok"]) for e in out["published"]],
                         [("deposit", True), ("exit", False)])
        self.assertEqual(fw.ExitParams.from_json(out["published"][1]["params"]), xp)


# =============================================================================
# route (d3)
# =============================================================================
FX_FACTORY, BATCH, USER_TO = addr(0x0E), addr(0x0D), addr(0x71)
W_AMOUNT = 5_000_000_000
QUOTE = 10**14
TXID = "ab" * 32


class ForwardWorld:
    def __init__(self, released=False, balance=0):
        self.p = fw.ExitParams(30110, fw.address_word(USER_TO), 50, 3_000_000, REFUND_EOA)
        self.account = fw.exit_forwarder_address(FX_FACTORY, INIT_CODE_HASH, self.p)
        self.released, self.balance = released, balance
        self.batch_logs = []
        self.proof = {
            "withdrawal_id": 4, "source_txid": TXID, "burn_output_index": 0, "b3_height": 350,
            "destination_chain_id": 31337, "vault": fw.to_0x(VAULT), "token": fw.to_0x(TOKEN),
            "recipient": fw.to_0x(self.account), "amount_raw": W_AMOUNT, "path": [fw.to_0x(word(i)) for i in range(32)],
            "exit": dict(self.p.to_json(), version=1, factory=fw.to_0x(FX_FACTORY), account=fw.to_0x(self.account),
                         salt=fw.to_0x(fw.exit_salt(self.p))),
        }
        self.node = FakeNode({"getblockchaininfo": {"chain": "regtest"},
                              "getbridgev2info": bridge_v2_info(deposits={"status": "CLOSED_FOR_GOOD",
                                                                          "reason": "C-1: releases go on"}),
                              "getbridgewithdrawalproofv2": lambda params: self.proof})
        eth = self.eth = FakeEth(timestamp=NOW)
        for contract in (VAULT, TOKEN, FX_FACTORY, BATCH):
            eth.code[contract] = b"\x60\x00"
        eth.view(FX_FACTORY, fw.SIG_X_ACCOUNT_OF, "address", lambda a, tx: fw.exit_forwarder_address(
            FX_FACTORY, INIT_CODE_HASH, fw.ExitParams(*a[0])))
        eth.view(FX_FACTORY, fw.SIG_X_DST_EID, "uint32", lambda a, tx: 30110)
        eth.view(FX_FACTORY, fw.SIG_TOKEN, "address", lambda a, tx: TOKEN)
        eth.view(FX_FACTORY, fw.SIG_X_QUOTE_FORWARD, "uint256", lambda a, tx: QUOTE)
        eth.view(VAULT, fw.SIG_VAULT_RELEASED, "bool", lambda a, tx: self.released)
        eth.view(TOKEN, fw.SIG_BALANCE_OF, "uint256", lambda a, tx: self.balance)
        eth.view(BATCH, fw.SIG_RELAY_WITH_FORWARDS, ("uint256", "uint256", "uint256"), self._simulate)
        eth.tx(BATCH, fw.SIG_RELAY_WITH_FORWARDS, self._relay)
        self.forward_ok = True

    def _simulate(self, args, tx):
        releases, forwards = args[4], args[7]
        paid = 0 if self.released else len(releases)
        sent = 1 if self.forward_ok and int(tx.get("value", "0x0"), 16) >= forwards[0][3] else 0
        return (0, paid, sent)

    def _relay(self, args, tx):
        releases, forwards = args[4], args[7]
        self.last_args, self.last_tx = args, tx
        logs = []
        if releases:
            w = releases[0][0]
            logs.append((VAULT, [fw.event_topic(fw.EV_WITHDRAWN), word(w[0]), fw.address_word(TOKEN),
                                 fw.address_word(self.account)], word(w[2]) + word(w[3])))
            self.released = True
            self.balance += w[2]
        p, fee, fee_recipient, _ = forwards[0]
        amount = self.balance - fee
        logs.append((FX_FACTORY, [fw.event_topic(fw.EV_FORWARDED), fw.address_word(self.account), p[1], word(0x99)],
                     fw.encode_abi(["uint256", "uint256", "uint256", "address", "uint256"],
                                   [amount, amount, fee, fee_recipient, QUOTE])))
        self.balance = 0
        return logs + self.batch_logs

    def forward(self, fee=1_000_000, **kwargs):
        return fw.release_and_forward(self.node, self.eth, SENDER, BATCH, TXID, 0, 360, fee, **kwargs)


class ForwardTests(unittest.TestCase):
    def test_release_and_forward_in_one_batch(self):
        world = ForwardWorld()
        out = world.forward(native_margin=5)
        self.assertEqual(out["status"], "forwarded")
        self.assertTrue(out["release_included"] and out["released_by_this_tx"])
        self.assertEqual(out["account"], fw.to_0x(world.account))
        self.assertEqual((out["amount_sent"], out["fee"], out["native_fee"]), (W_AMOUNT - 1_000_000, 1_000_000, QUOTE))
        self.assertEqual(out["native_value"], QUOTE + 5)
        self.assertEqual(len(world.eth.sent), 1)
        tx = world.eth.sent[0]
        self.assertEqual((tx["to"], tx["value"]), (fw.to_0x(BATCH), hex(QUOTE + 5)))
        self.assertEqual(int(tx["gas"], 16), out["gas_limit"])
        args = world.last_args
        self.assertEqual(args[1], [])  # no certificates
        self.assertEqual(args[3], fw.to_0x(VAULT))
        self.assertEqual(args[4][0][0], (4, fw.to_0x(world.account), W_AMOUNT, 350))
        self.assertEqual(args[4][0][1], [word(i) for i in range(32)])
        self.assertEqual((args[5], args[6], args[8], args[9]),
                         (fw.DEFAULT_RELEASE_GAS, fw.to_0x(FX_FACTORY), fw.DEFAULT_FORWARD_GAS, fw.to_0x(SENDER)))
        self.assertEqual(fw.ExitParams(*args[7][0][0]), world.p)
        self.assertEqual(args[7][0][1:], (1_000_000, fw.to_0x(SENDER), QUOTE + 5))
        # The gas limit covers both item caps (estimation would under-size a try/catch batch).
        self.assertGreater(out["gas_limit"], fw.DEFAULT_RELEASE_GAS + fw.DEFAULT_FORWARD_GAS)
        # C-1: ForwardWorld's B3 reports deposits CLOSED_FOR_GOOD; the forward
        # went ahead because the deposit guard is not applied to releases.

    def test_already_released_forwards_only(self):
        world = ForwardWorld(released=True, balance=7_000_000)
        out = world.forward()
        self.assertFalse(out["release_included"])
        self.assertEqual(world.last_args[4], [])
        self.assertEqual(out["amount_sent"], 6_000_000)

    def test_already_released_and_empty_refused(self):
        world = ForwardWorld(released=True, balance=0)
        with self.assertRaisesRegex(fw.ForwarderError, "already released and its exit forwarder .* holds nothing"):
            world.forward()
        self.assertEqual(world.eth.sent, [])

    def test_refusals_before_sending(self):
        world = ForwardWorld()
        del world.proof["exit"]
        with self.assertRaisesRegex(fw.ForwarderError, "carries no route \\(d3\\) exit block"):
            world.forward()
        world = ForwardWorld()
        world.proof["recipient"] = fw.to_0x(OTHER)
        with self.assertRaisesRegex(fw.ForwarderError, "is not the exit forwarder"):
            world.forward()
        world = ForwardWorld()
        world.proof["exit"]["max_bridge_fee_bps"] = 51
        with self.assertRaisesRegex(fw.ForwarderError, "exit parameters are invalid"):
            world.forward()
        world = ForwardWorld()
        world.proof["exit"]["factory"] = fw.to_0x(OTHER)
        with self.assertRaisesRegex(fw.ForwarderError, "not the pinned exit forwarder factory"):
            world.forward()
        world = ForwardWorld()
        with self.assertRaisesRegex(fw.ForwarderError, "per-forward fee cap"):
            world.forward(fee=3_000_001)
        world = ForwardWorld()
        world.eth.view(FX_FACTORY, fw.SIG_X_ACCOUNT_OF, "address", lambda a, tx: OTHER)
        with self.assertRaisesRegex(fw.ForwarderError, "accountOf\\(p\\) differs"):
            world.forward()
        world = ForwardWorld()
        world.node.replies["getblockchaininfo"] = {"chain": "test"}
        with self.assertRaisesRegex(fw.ForwarderError, "refusing B3 chain 'test'"):
            world.forward()
        world = ForwardWorld()
        world.eth.chain_id = 1
        with self.assertRaisesRegex(fw.ForwarderError, "refusing Ethereum chain id 1"):
            world.forward()
        world = ForwardWorld()
        world.node.replies["getbridgewithdrawalproofv2"] = observer.RpcError(
            "getbridgewithdrawalproofv2", {"code": -5, "message": "Burn source is not a confirmed V2 withdrawal"})
        with self.assertRaisesRegex(fw.ForwarderError, "refused: Burn source is not a confirmed"):
            world.forward()
        self.assertEqual(world.eth.sent, [])

    def test_simulation_failure_sends_nothing(self):
        world = ForwardWorld(released=True, balance=7_000_000)
        world.forward_ok = False
        world.eth.view(FX_FACTORY, fw.SIG_X_FORWARD, "uint256", reverts_with("EndpointPaused()"))
        with self.assertRaisesRegex(fw.ForwarderError, "sends 0 of 1 forwards: the forward would fail "
                                                       "\\(EndpointPaused\\(\\)\\); nothing sent"):
            world.forward()
        self.assertEqual(world.eth.sent, [])

    def test_post_check_refuses_failed_items(self):
        world = ForwardWorld()
        world.batch_logs = [(BATCH, [fw.event_topic(fw.EV_FORWARD_FAILED), word(0), fw.address_word(world.account)],
                             fw.encode_abi(["bytes"], [fw.selector("NothingToForward()")]))]
        with self.assertRaisesRegex(fw.ForwarderError, "reports ForwardFailed: NothingToForward\\(\\)"):
            world.forward()


class CliTests(unittest.TestCase):
    def run_main(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = fw.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_non_local_endpoints_refused_before_any_request(self):
        for url in ("https://127.0.0.1:8545", "http://10.0.0.1:8545", "http://user:pw@127.0.0.1:8545"):
            code, out, err = self.run_main(["published", "--ethereum-rpc", url, "--factory", fw.to_0x(FD_FACTORY)])
            self.assertEqual((code, out), (1, ""))
            self.assertTrue(err.startswith("error: refusing"), err)
        code, _, err = self.run_main(["flush", "--ethereum-rpc", "http://127.0.0.1:9", "--b3-rpc-url",
                                      "http://192.168.1.1:8332", "--from", fw.to_0x(SENDER), "--factory",
                                      fw.to_0x(FD_FACTORY), "--account", fw.to_0x(OTHER), "--fee", "1"])
        self.assertEqual(code, 1)
        self.assertIn("refusing non-local endpoint", err)

    def test_argument_errors(self):
        with contextlib.redirect_stderr(io.StringIO()):
            for argv in (["flush", "--ethereum-rpc", "http://127.0.0.1:9", "--b3-rpc-url", "http://127.0.0.1:9",
                          "--from", "0x01", "--factory", "0x02", "--fee", "1"],
                         ["flush", "--ethereum-rpc", "http://127.0.0.1:9", "--b3-rpc-url", "http://127.0.0.1:9",
                          "--from", "0x01", "--factory", "0x02", "--fee", "1", "--account", "0x03",
                          "--params", "{}"],
                         ["forward", "--ethereum-rpc", "http://127.0.0.1:9", "--b3-rpc-url", "http://127.0.0.1:9",
                          "--from", "0x01", "--batch", "0x02", "--txid", TXID, "--vout", "0",
                          "--certificate-block", "5", "--fee", "1", "--b3-rpc-user", "u"]):
                with self.assertRaises(SystemExit):
                    fw.parse_args(argv)
        args = fw.parse_args(["forward", "--ethereum-rpc", "http://127.0.0.1:9", "--b3-rpc-url",
                              "http://127.0.0.1:9", "--from", "0x01", "--batch", "0x02", "--txid", TXID,
                              "--vout", "1", "--certificate-block", "360", "--fee", "7"])
        self.assertEqual((args.certificate_block, args.vout, args.native_margin), (360, 1, 0))
        args = fw.parse_args(["forward", "--ethereum-rpc", "http://127.0.0.1:9", "--b3-rpc-url",
                              "http://127.0.0.1:9", "--from", "0x01", "--batch", "0x02", "--txid", TXID,
                              "--vout", "1", "--certificate-block", "00ff" * 16, "--fee", "7"])
        self.assertEqual(args.certificate_block, "00ff" * 16)

    def test_params_argument(self):
        p = fw.DepositParams(VAULT, RECIPIENT, REFUND_EOA, 5, 6)
        self.assertEqual(fw._params_argument(json.dumps({"params": p.to_json()})), p)
        with self.assertRaisesRegex(fw.ForwarderError, "--params is not JSON"):
            fw._params_argument("{oops")
        with self.assertRaisesRegex(fw.ForwarderError, "cannot read --params file"):
            fw._params_argument("/nonexistent/b3-forwarder-params.json")

    def test_main_prints_refusal(self):
        world = FlushWorld()
        world.node.replies["getbridgev2info"] = bridge_v2_info(retired=True)
        with mock.patch.object(fw, "JsonRpc", side_effect=[world.eth, world.node]):
            code, out, err = self.run_main(["flush", "--ethereum-rpc", "http://127.0.0.1:9", "--b3-rpc-url",
                                            "http://127.0.0.1:9", "--from", fw.to_0x(SENDER), "--factory",
                                            fw.to_0x(FD_FACTORY), "--params", json.dumps(world.p.to_json()),
                                            "--fee", str(FEE)])
        self.assertEqual((code, out), (1, ""))
        self.assertTrue(err.startswith("error: the launch vault entry is retired"), err)
        world = FlushWorld()
        with mock.patch.object(fw, "JsonRpc", side_effect=[world.eth, world.node]):
            code, out, err = self.run_main(["flush", "--ethereum-rpc", "http://127.0.0.1:9", "--b3-rpc-url",
                                            "http://127.0.0.1:9", "--from", fw.to_0x(SENDER), "--factory",
                                            fw.to_0x(FD_FACTORY), "--params", json.dumps(world.p.to_json()),
                                            "--fee", str(FEE)])
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(json.loads(out)["status"], "flushed")


if __name__ == "__main__":
    unittest.main()
