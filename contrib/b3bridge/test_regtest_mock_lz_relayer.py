#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Offline unit tests for the REGTEST ONLY LayerZero test relayer (fake RPC objects, no network)."""
import contextlib
import io
import json
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "regtest"))
import b3_bridge_forwarders_v2 as fw  # noqa: E402
import b3_bridge_observe_v2 as observer  # noqa: E402
import mock_lz_relayer as lz  # noqa: E402

EP_ETH, EP_ARB = bytes(19) + b"\xe1", bytes(19) + b"\xa1"
ADAPTER, USDT0 = bytes(19) + b"\xad", bytes(19) + b"\x0d"
RELAYER = bytes(19) + b"\x5e"
ETH_EID, ARB_EID = 30101, 30110


def word(n):
    return n.to_bytes(32, "big")


def message(to, amount):
    return fw.address_word(to) + amount.to_bytes(8, "big")


class FakeChain:
    """One anvil with one MockLzEndpoint at `endpoint`."""

    def __init__(self, chain_id, endpoint, eid):
        self.chain_id, self.endpoint, self.eid = chain_id, endpoint, eid
        self.block_number = 20
        self.logs = []
        self.delivered = set()
        self.sent = []
        self.receipts = {}
        self.calls = []
        self.revert = None  # custom error a deliver simulation reverts with
        self.mined_revert = False  # the simulation passes but the mined deliver reverts

    def add_packet(self, nonce, dst_eid, sender, receiver_word, payload, block=10):
        data = fw.encode_abi(["bytes32", "bytes"], [receiver_word, payload])
        self.logs.append({"address": fw.to_0x(self.endpoint),
                          "topics": [fw.to_0x(fw.event_topic(lz.EV_PACKET_SENT)), fw.to_0x(word(nonce)),
                                     fw.to_0x(word(dst_eid)), fw.to_0x(fw.address_word(sender))],
                          "data": fw.to_0x(data), "blockNumber": hex(block), "logIndex": hex(len(self.logs)),
                          "transactionHash": fw.to_0x(word(0x7700 + nonce))})

    def call(self, method, params=()):
        self.calls.append((method, list(params)))
        if method == "eth_chainId":
            return hex(self.chain_id)
        if method == "eth_blockNumber":
            return hex(self.block_number)
        if method == "eth_getCode":
            return "0x6000" if bytes.fromhex(params[0][2:]) == self.endpoint else "0x"
        if method == "eth_getLogs":
            query = params[0]
            assert query["toBlock"] == hex(self.block_number)
            out = []
            for log in self.logs:
                if log["address"] != query["address"]:
                    continue
                if all(want is None or log["topics"][i] == want for i, want in enumerate(query["topics"])):
                    if int(query["fromBlock"], 16) <= int(log["blockNumber"], 16):
                        out.append(log)
            return out
        if method == "eth_call":
            data = bytes.fromhex(params[0]["data"][2:])
            if data[:4] == fw.selector(lz.SIG_EID):
                return fw.to_0x(word(self.eid))
            if data[:4] == fw.selector(lz.SIG_IS_DELIVERED):
                src_eid, sender, nonce = fw.decode_abi(["uint32", "bytes32", "uint64"], data[4:])
                return fw.to_0x(word(int((src_eid, sender, nonce) in self.delivered)))
            if data[:4] == fw.selector(lz.SIG_DELIVER):
                if self.revert:
                    raise observer.RpcError("eth_call", {"code": 3, "message": "execution reverted",
                                                         "data": fw.to_0x(fw.selector(self.revert))})
                return "0x"
            raise AssertionError("unexpected eth_call")
        if method == "eth_sendTransaction":
            data = bytes.fromhex(params[0]["data"][2:])
            assert data[:4] == fw.selector(lz.SIG_DELIVER)
            args = fw.decode_abi(["uint32", "bytes32", "uint64", "address", "bytes"], data[4:])
            self.sent.append(args)
            self.block_number += 1
            tx_hash = fw.to_0x(word(len(self.sent)))
            if not self.mined_revert:
                self.delivered.add(tuple(args[:3]))
            self.receipts[tx_hash] = {"status": "0x0" if self.mined_revert else "0x1",
                                      "transactionHash": tx_hash, "blockNumber": hex(self.block_number)}
            return tx_hash
        if method == "eth_getTransactionReceipt":
            return self.receipts.get(params[0])
        raise AssertionError(f"unexpected RPC {method}")


def chains():
    """(arb, eth): route (b) packets run 31338 -> 31337."""
    return FakeChain(31338, EP_ARB, ARB_EID), FakeChain(31337, EP_ETH, ETH_EID)


class RelayTests(unittest.TestCase):
    def test_delivers_once_and_is_idempotent(self):
        arb, eth = chains()
        f_d = bytes(19) + b"\xfd"
        arb.add_packet(1, ETH_EID, USDT0, fw.address_word(ADAPTER), message(f_d, 25_000_000_000))
        arb.add_packet(2, 30184, USDT0, fw.address_word(ADAPTER), message(f_d, 1))  # another chain's packet
        arb.add_packet(3, ETH_EID, USDT0, fw.address_word(ADAPTER), message(f_d, 7_000_000))
        out = lz.relay_packets(arb, EP_ARB, eth, EP_ETH, RELAYER)
        self.assertEqual((out["delivered"], out["already_delivered"], out["failed"]), (2, 0, 0))
        self.assertEqual([p["nonce"] for p in out["packets"]], [1, 3])
        self.assertEqual((out["src_chain_id"], out["dst_chain_id"], out["src_eid"], out["dst_eid"]),
                         (31338, 31337, ARB_EID, ETH_EID))
        self.assertEqual(eth.sent[0], [ARB_EID, fw.address_word(USDT0), 1, fw.to_0x(ADAPTER),
                                       message(f_d, 25_000_000_000)])
        self.assertTrue(all(p["status"] == lz.STATUS_DELIVERED and p["tx"] for p in out["packets"]))
        # Every deliver went out from --from on the destination only.
        self.assertTrue(all(params[0]["from"] == fw.to_0x(RELAYER)
                            for method, params in eth.calls if method == "eth_sendTransaction"))
        self.assertNotIn("eth_sendTransaction", [method for method, _ in arb.calls])
        again = lz.relay_packets(arb, EP_ARB, eth, EP_ETH, RELAYER)
        self.assertEqual((again["delivered"], again["already_delivered"], again["failed"]), (0, 2, 0))
        self.assertEqual(len(eth.sent), 2)

    def test_from_block_bounds_the_scan(self):
        arb, eth = chains()
        arb.add_packet(1, ETH_EID, USDT0, fw.address_word(ADAPTER), message(ADAPTER, 5), block=3)
        arb.add_packet(2, ETH_EID, USDT0, fw.address_word(ADAPTER), message(ADAPTER, 6), block=12)
        out = lz.relay_packets(arb, EP_ARB, eth, EP_ETH, RELAYER, from_block=10)
        self.assertEqual([p["nonce"] for p in out["packets"]], [2])
        self.assertEqual(lz.relay_packets(arb, EP_ARB, eth, EP_ETH, RELAYER, from_block=21)["packets"], [])
        with self.assertRaisesRegex(fw.ForwarderError, "--from-block"):
            lz.relay_packets(arb, EP_ARB, eth, EP_ETH, RELAYER, from_block=-1)

    def test_failure_is_reported_retryable_and_retried(self):
        eth, arb = FakeChain(31337, EP_ETH, ETH_EID), FakeChain(31338, EP_ARB, ARB_EID)
        user = bytes(19) + b"\x71"
        eth.add_packet(1, ARB_EID, ADAPTER, fw.address_word(USDT0), message(user, 4_999_000_000))
        arb.revert = "EndpointPaused()"
        out = lz.relay_packets(eth, EP_ETH, arb, EP_ARB, RELAYER)
        self.assertEqual(out["failed"], 1)
        self.assertEqual(out["packets"][0]["status"], lz.STATUS_FAILED)
        self.assertIs(out["packets"][0]["retryable"], True)
        self.assertEqual(out["packets"][0]["error"], "EndpointPaused()")
        self.assertEqual(arb.sent, [])  # the simulation stopped it
        arb.revert, arb.mined_revert = None, True
        out = lz.relay_packets(eth, EP_ETH, arb, EP_ARB, RELAYER)
        self.assertEqual(out["packets"][0]["status"], lz.STATUS_FAILED)
        self.assertIn("reverted on chain", out["packets"][0]["error"])
        arb.mined_revert = False
        out = lz.relay_packets(eth, EP_ETH, arb, EP_ARB, RELAYER)
        self.assertEqual((out["delivered"], out["failed"]), (1, 0))
        self.assertEqual(lz.relay_packets(eth, EP_ETH, arb, EP_ARB, RELAYER)["already_delivered"], 1)

    def test_receiver_that_is_not_an_address_is_undeliverable(self):
        arb, eth = chains()
        arb.add_packet(1, ETH_EID, USDT0, (1 << 160 | 0xAD).to_bytes(32, "big"), message(ADAPTER, 5))
        out = lz.relay_packets(arb, EP_ARB, eth, EP_ETH, RELAYER)
        self.assertEqual(out["packets"][0]["status"], lz.STATUS_UNDELIVERABLE)
        self.assertIs(out["packets"][0]["retryable"], False)
        self.assertEqual(out["failed"], 1)
        self.assertEqual(eth.sent, [])

    def test_chain_and_endpoint_refusals(self):
        for src_id, dst_id, message_text in ((1, 31337, "refusing source chain id 1"),
                                             (31338, 42161, "refusing destination chain id 42161"),
                                             (31337, 31337, "both chain id 31337")):
            arb, eth = chains()
            arb.chain_id, eth.chain_id = src_id, dst_id
            with self.assertRaisesRegex(fw.ForwarderError, message_text):
                lz.relay_packets(arb, EP_ARB, eth, EP_ETH, RELAYER)
            self.assertEqual(eth.sent, [])
        arb, eth = chains()
        eth.eid = ARB_EID
        with self.assertRaisesRegex(fw.ForwarderError, "both report eid 30110"):
            lz.relay_packets(arb, EP_ARB, eth, EP_ETH, RELAYER)
        arb, eth = chains()
        with self.assertRaisesRegex(fw.ForwarderError, "destination endpoint .* has no code"):
            lz.relay_packets(arb, EP_ARB, eth, ADAPTER, RELAYER)

    def test_decode_packet_refuses_malformed_logs(self):
        arb, _ = chains()
        arb.add_packet(1, ETH_EID, USDT0, fw.address_word(ADAPTER), message(ADAPTER, 5))
        log = dict(arb.logs[0])
        self.assertEqual(lz.decode_packet(log)[:3], (1, ETH_EID, fw.address_word(USDT0)))
        log["topics"] = log["topics"][:3]
        with self.assertRaisesRegex(fw.ForwarderError, "not a PacketSent log"):
            lz.decode_packet(log)
        log = dict(arb.logs[0])
        log["topics"] = list(log["topics"])
        log["topics"][1] = fw.to_0x(word(1 << 64))
        with self.assertRaisesRegex(fw.ForwarderError, "malformed PacketSent topics"):
            lz.decode_packet(log)


class CliTests(unittest.TestCase):
    ARGS = ["deliver", "--src-endpoint", fw.to_0x(EP_ARB), "--dst-endpoint", fw.to_0x(EP_ETH),
            "--from", fw.to_0x(RELAYER)]

    def run_main(self, argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = lz.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_url_refusals(self):
        for src, dst in (("https://127.0.0.1:8545", "http://127.0.0.1:8546"),
                         ("http://127.0.0.1:8545", "http://arbitrum.example:8546"),
                         ("http://10.1.1.1:8545", "http://localhost:8546")):
            code, out, err = self.run_main(self.ARGS + ["--src-url", src, "--dst-url", dst])
            self.assertEqual((code, out), (1, ""))
            self.assertIn("refusing non-local endpoint", err)

    def test_exit_codes(self):
        arb, eth = chains()
        arb.add_packet(1, ETH_EID, USDT0, fw.address_word(ADAPTER), message(ADAPTER, 5))
        local = ["--src-url", "http://127.0.0.1:9", "--dst-url", "http://localhost:9"]
        eth.revert = "OnlyPeer(uint32,bytes32)"
        with mock.patch.object(lz, "JsonRpc", side_effect=[arb, eth]):
            code, out, _ = self.run_main(self.ARGS + local)
        self.assertEqual(code, lz.EXIT_RETRYABLE)
        self.assertEqual(json.loads(out)["failed"], 1)
        eth.revert = None
        with mock.patch.object(lz, "JsonRpc", side_effect=[arb, eth]):
            code, out, _ = self.run_main(self.ARGS + local)
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out)["delivered"], 1)
        arb.chain_id = 1
        with mock.patch.object(lz, "JsonRpc", side_effect=[arb, eth]):
            code, out, err = self.run_main(self.ARGS + local)
        self.assertEqual((code, out), (1, ""))
        self.assertTrue(err.startswith("error: refusing source chain id 1"), err)


if __name__ == "__main__":
    unittest.main()
