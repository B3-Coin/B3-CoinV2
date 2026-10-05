#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Offline unit tests for b3_bridge_observe_v2.py (fake RPC objects, no network)."""
import contextlib
import io
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import b3_bridge_observe_v2 as observer  # noqa: E402


VERIFIER = "0x" + "a1" * 20
VAULT = "0x" + "b2" * 20
TOKEN = "0x" + "c3" * 20
ANCHOR_HASH = "0x" + "d4" * 32
ANCHOR_NUMBER = 1234


def word(n):
    return "0x" + n.to_bytes(32, "big").hex()


BALANCE_SLOT = "0x" + "e5" * 32
BLACKLIST_SLOT = "0x" + "f6" * 32
PLAN_ACCOUNTS = {
    1: [{"role": "verifier", "address": VERIFIER, "slots": [word(0), word(1), word(2), word(4), word(6)]}],
    2: [{"role": "vault", "address": VAULT, "slots": [word(0), word(1), word(3)]},
        {"role": "token", "address": TOKEN, "slots": [BALANCE_SLOT]}],
    3: [{"role": "token", "address": TOKEN, "slots": [word(0), word(10), BLACKLIST_SLOT]}],
}


def bridge_v2_info(**overrides):
    subjects = []
    for subject, name in observer.SUBJECTS.items():
        subjects.append({"subject": subject, "name": name, "anchor_is_newer": True,
                         "accounts": [dict(a, slots=list(a["slots"])) for a in PLAN_ACCOUNTS[subject]]})
    info = {
        "configured": True, "active": True, "state_available": True, "next_height": 300,
        "activation_height": 281, "origin_chain_id": 31337,
        "observation_plan": {
            "anchor": {"hash_evm": ANCHOR_HASH, "number": ANCHOR_NUMBER, "timestamp": 1_700_000_000,
                       "connected_height": 290, "connected_block": "11" * 32},
            "subjects": subjects,
        },
    }
    info.update(overrides)
    return info


class FakeRpc:
    """Records every call; replies from a dict of callables or values."""

    def __init__(self, replies):
        self.replies = dict(replies)
        self.calls = []

    def call(self, method, params=()):
        params = list(params)
        self.calls.append((method, params))
        if method not in self.replies:
            raise AssertionError(f"unexpected RPC {method}")
        reply = self.replies[method]
        if isinstance(reply, Exception):
            raise reply
        return reply(params) if callable(reply) else reply

    def methods(self):
        return [method for method, _ in self.calls]


def proof_reply(params):
    address, keys, block = params
    return {"address": address.upper().replace("0X", "0x"), "accountProof": ["0xf8"],
            "balance": "0x0", "nonce": "0x1", "codeHash": "0x" + "00" * 32, "storageHash": "0x" + "00" * 32,
            # anvil answers keys as quantities; the observer accepts both forms.
            "storageProof": [{"key": hex(int(key, 16)), "value": "0x0", "proof": []} for key in keys]}


def build_reply(params):
    subject, anchor_hash, proofs = params
    return {"payload_hex": "09%02x" % subject + "00" * 40, "subject": subject,
            "name": observer.SUBJECTS[subject], "verdict": 0, "anchor_block": ANCHOR_NUMBER,
            "anchor_hash_evm": anchor_hash, "t_obs": 1_700_000_000, "next_height": 300,
            "values": {"subject": subject}}


class ObserverTests(unittest.TestCase):
    def setUp(self):
        self.node = FakeRpc({
            "getblockchaininfo": {"chain": "regtest"},
            "getbridgev2info": lambda _: bridge_v2_info(),
            "buildbridgeobservationv2": build_reply,
        })
        self.eth = FakeRpc({
            "eth_chainId": "0x7a69",
            "eth_getBlockByNumber": lambda params: {"number": params[0], "hash": ANCHOR_HASH},
            "eth_getProof": proof_reply,
        })
        self.txids = iter(f"{i:064x}" for i in range(1, 10))
        self.wallet = FakeRpc({
            "submitbridgecarrier": lambda _: {"txid": next(self.txids), "record_kind": "eth-state-observation"},
        })

    def observe(self, subjects=(1, 2, 3), dry_run=False):
        return observer.observe(self.node, self.wallet, self.eth, list(subjects), dry_run=dry_run)

    def assert_refused(self, message, subjects=(1, 2, 3), dry_run=False):
        with self.assertRaisesRegex(observer.ObserverError, message):
            self.observe(subjects, dry_run)
        self.assertEqual(self.wallet.calls, [], "a refusal must submit nothing")

    # -- request shapes and ordering ------------------------------------------
    def test_request_shapes_and_order(self):
        result = self.observe()
        self.assertEqual(self.node.methods()[:2], ["getblockchaininfo", "getbridgev2info"])
        self.assertEqual(self.eth.calls[0], ("eth_chainId", []))
        self.assertEqual(self.eth.calls[1], ("eth_getBlockByNumber", [hex(ANCHOR_NUMBER), False]))
        proofs = [params for method, params in self.eth.calls if method == "eth_getProof"]
        # One call per plan account, subjects ascending, accounts and keys in plan order.
        self.assertEqual(proofs, [
            [VERIFIER, PLAN_ACCOUNTS[1][0]["slots"], hex(ANCHOR_NUMBER)],
            [VAULT, PLAN_ACCOUNTS[2][0]["slots"], hex(ANCHOR_NUMBER)],
            [TOKEN, [BALANCE_SLOT], hex(ANCHOR_NUMBER)],
            [TOKEN, PLAN_ACCOUNTS[3][0]["slots"], hex(ANCHOR_NUMBER)],
        ])
        builds = [params for method, params in self.node.calls if method == "buildbridgeobservationv2"]
        self.assertEqual([(b[0], b[1], len(b[2])) for b in builds],
                         [(1, ANCHOR_HASH, 1), (2, ANCHOR_HASH, 2), (3, ANCHOR_HASH, 1)])
        # The eth_getProof results go to the node unchanged, in plan order.
        self.assertEqual([p["address"].lower() for p in builds[1][2]], [VAULT, TOKEN])
        # Every subject is built before the first submission.
        order = [m for m, _ in self.node.calls if m == "buildbridgeobservationv2"]
        self.assertEqual(len(order), 3)
        self.assertEqual([params for _, params in self.wallet.calls],
                         [[o["payload_hex"]] for o in result["observations"]])
        self.assertEqual([o["subject"] for o in result["observations"]], [1, 2, 3])
        self.assertEqual([o["txid"] for o in result["observations"]], [f"{i:064x}" for i in (1, 2, 3)])
        self.assertEqual(result["anchor"], {"number": ANCHOR_NUMBER, "hash_evm": ANCHOR_HASH,
                                            "timestamp": 1_700_000_000})
        self.assertFalse(result["dry_run"])
        json.dumps(result)  # The stdout object is plain JSON.

    def test_subset_and_dry_run(self):
        result = self.observe(subjects=(3,), dry_run=True)
        self.assertEqual([o["name"] for o in result["observations"]], ["TOKEN_FLAGS"])
        self.assertNotIn("txid", result["observations"][0])
        self.assertEqual(self.wallet.calls, [])
        self.assertEqual([params[0] for method, params in self.eth.calls if method == "eth_getProof"], [TOKEN])

    def test_parse_subjects(self):
        self.assertEqual(observer.parse_subjects("3,1"), [1, 3])
        self.assertEqual(observer.parse_subjects("verifier_e, RESERVE_E,2"), [1, 2])
        for bad in ("0", "4", "", "kind9", "1,,2"):
            with self.assertRaises(observer.ObserverError, msg=bad):
                observer.parse_subjects(bad)

    # -- refusals ---------------------------------------------------------------
    def test_refuses_non_regtest_chain(self):
        for chain in ("main", "test", "signet", None):
            self.node.replies["getblockchaininfo"] = {"chain": chain}
            self.assert_refused("regtest only")
        self.assertEqual(self.eth.calls, [])

    def test_refuses_other_ethereum_chain(self):
        for chain_id in ("0x1", "0xaa36a7", "0x7a6a"):
            self.eth.calls.clear()
            self.eth.replies["eth_chainId"] = chain_id
            self.assert_refused("bridge V2 vault entry is on chain 31337")
            self.assertEqual([m for m in self.eth.methods() if m != "eth_chainId"], [])

    def test_refuses_inactive_or_incomplete_bridge(self):
        for overrides, message in (({"configured": False}, "configuration is incomplete"),
                                   ({"active": False}, "not active for the next block"),
                                   ({"state_available": False}, "bridge state is unavailable")):
            self.node.replies["getbridgev2info"] = lambda _, o=overrides: bridge_v2_info(**o)
            self.assert_refused(message)

    def test_refuses_missing_or_foreign_anchor(self):
        info = bridge_v2_info()
        del info["observation_plan"]["anchor"]
        self.node.replies["getbridgev2info"] = lambda _: info
        self.assert_refused("relay a light-client update first")
        self.node.replies["getbridgev2info"] = lambda _: bridge_v2_info()
        self.eth.replies["eth_getBlockByNumber"] = lambda params: {"hash": "0x" + "00" * 32}
        self.assert_refused("is not the B3 anchor")

    def test_refuses_anchor_not_newer(self):
        info = bridge_v2_info()
        info["observation_plan"]["subjects"][1]["anchor_is_newer"] = False
        info["observation_plan"]["subjects"][1]["latest_anchor_block"] = ANCHOR_NUMBER
        self.node.replies["getbridgev2info"] = lambda _: info
        self.assert_refused("RESERVE_E: the plan's anchor is not newer")
        # The other subjects alone still work.
        self.observe(subjects=(1, 3))

    def test_refuses_mismatched_proofs(self):
        def wrong_address(params):
            reply = proof_reply(params)
            reply["address"] = "0x" + "99" * 20
            return reply

        def wrong_key_order(params):
            reply = proof_reply(params)
            reply["storageProof"].reverse()
            return reply

        def missing_slot(params):
            reply = proof_reply(params)
            reply["storageProof"].pop()
            return reply

        def no_account_proof(params):
            return dict(proof_reply(params), accountProof=[])

        for reply, message in ((wrong_address, "answered for another address"),
                               (wrong_key_order, "is not for the plan's slot"),
                               (missing_slot, "the plan needs 5"),
                               (no_account_proof, "no account proof")):
            self.eth.replies["eth_getProof"] = reply
            self.node.calls.clear()
            self.assert_refused(message)
            self.assertNotIn("buildbridgeobservationv2", self.node.methods())

    def test_refuses_mismatched_build(self):
        def other_subject(params):
            return dict(build_reply(params), subject=2)

        def other_anchor(params):
            return dict(build_reply(params), anchor_hash_evm="0x" + "77" * 32)

        for reply, message in ((other_subject, "built another subject"), (other_anchor, "named another anchor")):
            self.node.replies["buildbridgeobservationv2"] = reply
            self.assert_refused(message)

    def test_node_refusal_submits_nothing(self):
        def refuse_reserve(params):
            if params[0] == 2:
                raise observer.RpcError("buildbridgeobservationv2", {"code": -26, "message": "next block would refuse"})
            return build_reply(params)

        self.node.replies["buildbridgeobservationv2"] = refuse_reserve
        self.assert_refused("next block would refuse")

    def test_submission_refusal_names_earlier_carriers(self):
        replies = iter([{"txid": "aa" * 32, "record_kind": "eth-state-observation"},
                        observer.RpcError("submitbridgecarrier", {"code": -26, "message": "verdict does not match"})])

        def submit(_):
            reply = next(replies)
            if isinstance(reply, Exception):
                raise reply
            return reply

        self.wallet.replies["submitbridgecarrier"] = submit
        with self.assertRaisesRegex(observer.ObserverError, "RESERVE_E: .*verdict does not match.*" + "aa" * 32):
            self.observe()
        self.wallet.replies["submitbridgecarrier"] = lambda _: {"txid": "bb" * 32, "record_kind": "update"}
        with self.assertRaisesRegex(observer.ObserverError, "did not create an eth-state-observation"):
            self.observe()

    def test_local_endpoint_guard(self):
        for url in ("http://127.0.0.1:8545", "http://localhost:18443"):
            self.assertEqual(observer.check_local_url(url), url)
        for url in ("https://127.0.0.1:8545", "http://10.0.0.1:8545", "http://user:pass@127.0.0.1:1",
                    "http://127.0.0.1.example.invalid:8545", "ws://localhost:8545", "127.0.0.1:8545",
                    "http://localhost:notaport", "", None):
            with self.assertRaises(observer.ObserverError, msg=url):
                observer.check_local_url(url)
            with self.assertRaises(observer.ObserverError, msg=url):
                observer.JsonRpc(url)

    def test_cli_refusals(self):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
            observer.parse_args(["--ethereum-rpc", "http://127.0.0.1:1", "--b3-rpc-url", "http://127.0.0.1:2"])
        self.assertIn("--wallet is required", stderr.getvalue())
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            observer.parse_args(["--ethereum-rpc", "http://127.0.0.1:1", "--b3-rpc-url", "http://127.0.0.1:2",
                                 "--dry-run", "--subjects", "4"])
        args = observer.parse_args(["--ethereum-rpc", "http://127.0.0.1:1", "--b3-rpc-url", "http://127.0.0.1:2",
                                    "--dry-run", "--subjects", "token_flags,1"])
        self.assertEqual(args.subject_list, [1, 3])
        # A non-local endpoint is refused before any request (exit 1, "error: ...").
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            code = observer.main(["--ethereum-rpc", "https://ethereum-rpc.example.invalid",
                                  "--b3-rpc-url", "http://127.0.0.1:2", "--dry-run"])
        self.assertEqual(code, 1)
        self.assertTrue(stderr.getvalue().startswith("error: refusing non-local endpoint"))


if __name__ == "__main__":
    unittest.main()
