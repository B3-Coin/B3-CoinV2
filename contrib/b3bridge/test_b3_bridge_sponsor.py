#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Offline unit tests for b3_bridge_sponsor.py (fake RPC objects, no network)."""
import contextlib
from decimal import Decimal
import email.message
import io
import json
from pathlib import Path
import sys
import unittest
from unittest import mock
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
import b3_bridge_observe_v2 as observer  # noqa: E402
import b3_bridge_sponsor as sponsor  # noqa: E402

POOL_VALUE = Decimal("0.000400000")
LABEL = sponsor.DEFAULT_LABEL
ASSET_ID = "ab" * 32
PARTIAL = "02000000" + "00" * 40
MIN_TIP = 50_000
URL = "http://127.0.0.1:18443"


def txid(n):
    return f"{n:064x}"


def coin(n, amount=POOL_VALUE, label=LABEL, confirmations=1, **overrides):
    entry = {"txid": txid(n), "vout": 0, "address": f"maddr{n}", "amount": Decimal(amount),
             "confirmations": confirmations, "spendable": True, "solvable": True, "safe": True}
    if label is not None:
        entry["label"] = label
    entry.update(overrides)
    return entry


def rpc_error(method, message, code=-4):
    return observer.RpcError(method, {"code": code, "message": message})


class FakeRpc:
    """Records every call; replies from a dict of callables, values or exceptions."""

    def __init__(self, replies, log=None, name="node"):
        self.replies = dict(replies)
        self.calls = [] if log is None else log
        self.name = name
        self.wallets = {}

    def wallet(self, name):
        if name not in self.wallets:
            self.wallets[name] = FakeRpc({}, self.calls, name=f"wallet:{name}")
        return self.wallets[name]

    def call(self, method, params=()):
        params = dict(params) if isinstance(params, dict) else list(params)
        self.calls.append((self.name, method, params))
        if method not in self.replies:
            raise AssertionError(f"unexpected RPC {method} on {self.name}")
        reply = self.replies[method]
        if isinstance(reply, Exception):
            raise reply
        return reply(params) if callable(reply) else reply

    def methods(self, endpoint=None):
        return [method for name, method, _ in self.calls if endpoint is None or name == endpoint]

    def params_of(self, method):
        return [params for _, called, params in self.calls if called == method]


def make_world(coins=(), chain="regtest"):
    node = FakeRpc({
        "getblockchaininfo": {"chain": chain},
        "getbridgev2info": {"configured": True, "active": True, "asset_id": ASSET_ID, "next_height": 420},
    })
    wallet = node.wallet("relayer")
    addresses = iter(f"mpool{i}" for i in range(1, 1000))
    wallet.replies.update({
        "listunspent": list(coins),
        "getnewaddress": lambda params: next(addresses),
        "lockunspent": True,
        "sendmany": txid(0xfeed),
        "listlockunspent": [],
        "getbalances": {"mine": {"trusted": Decimal("1.000000000"), "untrusted_pending": Decimal("0E-9"),
                                 "immature": Decimal("0E-9")}},
        "getwalletassets": {"assets": [{"asset_id": ASSET_ID, "confirmed": 100_000, "unconfirmed": 50_000,
                                        "spendable": 100_000, "utxos": [{}, {}, {}]}]},
    })
    return node, wallet


def completion(**overrides):
    result = {"txid": txid(0xc0), "ptxid": txid(0xc1), "hex": PARTIAL + "ff", "broadcast": True,
              "network_fee": Decimal("0.000400000"), "vsize": 420,
              "fee_input": {"txid": txid(7), "vout": 0, "amount": Decimal("0.000400000")},
              "tip": {"vout": 2, "amount": MIN_TIP},
              "burn": {"amount": 8_000_000, "route": "ethereum", "ethereum_recipient": "11" * 20,
                       "burn_vout": 0, "binding_vout": 1},
              "proof_rpc_after_confirmation": "getbridgewithdrawalproofv2 ..."}
    result.update(overrides)
    return result


# =============================================================================
# refill
# =============================================================================
class RefillTests(unittest.TestCase):
    def test_creates_the_deficit_with_labelled_legacy_addresses(self):
        pool = [coin(1), coin(2, confirmations=0)]
        node, wallet = make_world(pool + [coin(3, amount="0.5", label=None)])
        result = sponsor.refill(node, wallet, 5, POOL_VALUE)
        self.assertEqual((result["pool_before"], result["created"], result["remaining_deficit"]), (2, 3, 0))
        self.assertEqual(result["addresses"], ["mpool1", "mpool2", "mpool3"])
        self.assertEqual(result["txid"], txid(0xfeed))
        # Every new address has the pool label and the legacy (P2PKH) type.
        self.assertEqual(wallet.params_of("getnewaddress"), [[LABEL, "legacy"]] * 3)
        # One sendmany pays each new address exactly the pool value.
        self.assertEqual(wallet.params_of("sendmany"),
                         [{"dummy": "", "amounts": {"mpool1": POOL_VALUE, "mpool2": POOL_VALUE,
                                                    "mpool3": POOL_VALUE}}])
        # The chain check comes first; the labelled pool is locked around the
        # sendmany and unlocked afterwards (never the unlabelled coin).
        self.assertEqual(node.methods(), ["getblockchaininfo", "listunspent", "getnewaddress", "getnewaddress",
                                          "getnewaddress", "lockunspent", "sendmany", "lockunspent"])
        outpoints = [{"txid": txid(1), "vout": 0}, {"txid": txid(2), "vout": 0}]
        self.assertEqual(wallet.params_of("lockunspent"), [[False, outpoints], [True, outpoints]])
        self.assertEqual(wallet.params_of("listunspent"), [[0]])

    def test_noop_when_the_pool_is_full(self):
        node, wallet = make_world([coin(1), coin(2), coin(3)])
        for count in (2, 3):
            node.calls.clear()
            result = sponsor.refill(node, wallet, count, POOL_VALUE)
            self.assertEqual((result["pool_before"], result["created"], result["txid"], result["addresses"]),
                             (3, 0, None, []))
            self.assertEqual(node.methods(), ["getblockchaininfo", "listunspent"])

    def test_counts_only_matching_coins(self):
        coins = [
            coin(1),                                       # counted
            coin(2, label="other"),                        # another label
            coin(3, amount="0.000400001"),                 # another value
            coin(4, safe=False),                           # unsafe
            coin(5, solvable=False),                       # not solvable
            coin(6, confirmations=0),                      # counted at minconf 0 only
            coin(7, amount="0.0004"),                      # same value, other spelling: counted
        ]
        node, wallet = make_world(coins)
        result = sponsor.refill(node, wallet, 4, POOL_VALUE)
        self.assertEqual((result["pool_before"], result["created"]), (3, 1))
        # Every safe labelled coin is protected, whatever its value or depth.
        locked = wallet.params_of("lockunspent")[0][1]
        self.assertEqual([entry["txid"] for entry in locked], [txid(1), txid(3), txid(6), txid(7)])
        node, wallet = make_world(coins)
        result = sponsor.refill(node, wallet, 4, POOL_VALUE, minconf=1)
        self.assertEqual((result["pool_before"], result["created"]), (2, 2))

    def test_no_lock_without_labelled_coins_and_fee_rate_is_named(self):
        node, wallet = make_world([coin(1, label=None)])
        result = sponsor.refill(node, wallet, 1, POOL_VALUE, fee_rate=Decimal("2.5"))
        self.assertEqual(result["created"], 1)
        self.assertNotIn("lockunspent", node.methods())
        self.assertEqual(wallet.params_of("sendmany"),
                         [{"dummy": "", "amounts": {"mpool1": POOL_VALUE}, "fee_rate": Decimal("2.5")}])

    def test_unlocks_the_pool_when_sendmany_fails(self):
        node, wallet = make_world([coin(1)])
        wallet.replies["sendmany"] = rpc_error("sendmany", "Insufficient funds", code=-6)
        with self.assertRaisesRegex(sponsor.SponsorError, "^sendmany refused the refill: Insufficient funds$"):
            sponsor.refill(node, wallet, 3, POOL_VALUE)
        self.assertEqual([params[0] for params in wallet.params_of("lockunspent")], [False, True])
        # A transport failure may or may not have sent the refill.
        node, wallet = make_world([coin(1)])
        wallet.replies["sendmany"] = sponsor.SponsorError("sendmany: transport failure (TimeoutError)")
        with self.assertRaisesRegex(sponsor.SponsorError, "may have been sent"):
            sponsor.refill(node, wallet, 3, POOL_VALUE)
        self.assertEqual([params[0] for params in wallet.params_of("lockunspent")], [False, True])

    def test_lock_and_unlock_failures(self):
        node, wallet = make_world([coin(1)])
        wallet.replies["lockunspent"] = rpc_error("lockunspent", "Invalid parameter, output already locked", -8)
        with self.assertRaisesRegex(sponsor.SponsorError, "could not lock the 1 pool coins .*nothing was sent"):
            sponsor.refill(node, wallet, 3, POOL_VALUE)
        self.assertNotIn("sendmany", node.methods())

        def unlock_fails(params):
            if params[0]:
                raise rpc_error("lockunspent", "boom")
            return True
        node, wallet = make_world([coin(1)])
        wallet.replies["lockunspent"] = unlock_fails
        with self.assertRaisesRegex(sponsor.SponsorError, f"the refill '{txid(0xfeed)}' was sent; unlocking the "
                                                          "1 pool coins then failed \\(boom\\)"):
            sponsor.refill(node, wallet, 3, POOL_VALUE)

    def test_per_run_cap_and_bad_replies(self):
        node, wallet = make_world()
        result = sponsor.refill(node, wallet, sponsor.MAX_REFILL_OUTPUTS + 7, POOL_VALUE)
        self.assertEqual((result["created"], result["remaining_deficit"]), (sponsor.MAX_REFILL_OUTPUTS, 7))
        self.assertEqual(len(wallet.params_of("sendmany")[0]["amounts"]), sponsor.MAX_REFILL_OUTPUTS)
        node, wallet = make_world()
        wallet.replies["getnewaddress"] = "same"
        with self.assertRaisesRegex(sponsor.SponsorError, "unusable pool address 'same'; nothing was sent"):
            sponsor.refill(node, wallet, 2, POOL_VALUE)
        node, wallet = make_world()
        wallet.replies["sendmany"] = {"txid": txid(1)}
        with self.assertRaisesRegex(sponsor.SponsorError, "sendmany returned no txid"):
            sponsor.refill(node, wallet, 1, POOL_VALUE)
        node, wallet = make_world([coin(1, amount="0.0000000001")])
        with self.assertRaisesRegex(sponsor.SponsorError, "more than 9 decimal places"):
            sponsor.refill(node, wallet, 1, POOL_VALUE)

    def test_argument_values_refused(self):
        node, wallet = make_world()
        for count in (0, -1, True, "3"):
            with self.assertRaisesRegex(sponsor.SponsorError, "--count"):
                sponsor.refill(node, wallet, count, POOL_VALUE)
        for value in (Decimal(0), Decimal("-1"), Decimal("1E-10"), Decimal("NaN"), "x", Decimal("1E+40")):
            with self.assertRaises(sponsor.SponsorError):
                sponsor.refill(node, wallet, 1, value)
        self.assertNotIn("sendmany", node.methods())


# =============================================================================
# chain and endpoint guards
# =============================================================================
class GuardTests(unittest.TestCase):
    def test_refuses_non_regtest_chain_before_any_wallet_call(self):
        actions = (lambda node, wallet: sponsor.refill(node, wallet, 1, POOL_VALUE),
                   lambda node, wallet: sponsor.status(node, wallet),
                   lambda node, wallet: sponsor.complete(node, wallet, PARTIAL, MIN_TIP, max_fee=POOL_VALUE),
                   lambda node, wallet: sponsor.release(node, wallet))
        for chain in ("main", "test", "testnet4", "signet", None):
            for action in actions:
                node, wallet = make_world([coin(1)], chain=chain)
                with self.assertRaisesRegex(sponsor.SponsorError, "regtest only"):
                    action(node, wallet)
                self.assertEqual(node.methods(), ["getblockchaininfo"])
        node, wallet = make_world()
        node.replies["getblockchaininfo"] = ["regtest"]
        with self.assertRaisesRegex(sponsor.SponsorError, "regtest only"):
            sponsor.status(node, wallet)

    def test_local_endpoint_guard(self):
        for url in ("http://127.0.0.1:18443", "http://localhost:18443"):
            self.assertEqual(sponsor.LocalRpc(url).url, url)
        for url in ("https://127.0.0.1:18443", "http://10.0.0.1:18443", "http://user:pass@127.0.0.1:1",
                    "http://127.0.0.1.example.invalid:18443", "ws://localhost:18443", "127.0.0.1:18443",
                    "http://localhost:notaport", "", None):
            with self.assertRaises(observer.ObserverError, msg=url):
                sponsor.LocalRpc(url)
        # The wallet endpoint stays on the same local origin.
        wallet = sponsor.LocalRpc(URL).wallet("relay er/1")
        self.assertIsInstance(wallet, sponsor.LocalRpc)
        self.assertEqual(wallet.url, URL + "/wallet/relay%20er%2F1")

    def test_opener_never_uses_a_proxy(self):
        # build_opener(ProxyHandler({})) replaces the environment's default
        # ProxyHandler with one that has no proxy, so no handler proxies.
        handlers = sponsor._DIRECT_OPENER.handlers
        self.assertTrue(handlers)
        self.assertFalse([handler for handler in handlers
                          if isinstance(handler, urllib.request.ProxyHandler) and handler.proxies])
        self.assertFalse([handler for handler in handlers if hasattr(handler, "proxy_open")])
        # Through the real handler chain, with a proxy in the environment, the
        # request reaches the HTTP handler unproxied.
        http = [handler for handler in handlers if isinstance(handler, urllib.request.HTTPHandler)]
        self.assertEqual(len(http), 1)
        sent = []

        def fake_http_open(request):
            sent.append(request)
            return FakeResponse(b'{"result": {"chain": "regtest"}}')
        with mock.patch.dict("os.environ", {"http_proxy": "http://proxy.invalid:3128",
                                            "HTTP_PROXY": "http://proxy.invalid:3128"}), \
                mock.patch.object(http[0], "http_open", side_effect=fake_http_open):
            self.assertEqual(sponsor.LocalRpc(URL).call("getblockchaininfo"), {"chain": "regtest"})
        self.assertEqual(sent[0].full_url, URL)
        self.assertFalse(sent[0].has_proxy())
        self.assertEqual(sent[0].host, "127.0.0.1:18443")


class FakeResponse(io.BytesIO):
    code = 200
    msg = "OK"

    def info(self):
        return email.message.Message()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class WireTests(unittest.TestCase):
    def exchange(self, reply, method="sendmany", params=(), **rpc):
        sent = []

        def fake_open(request, timeout):
            sent.append(request)
            return FakeResponse(reply.encode())
        rpc_client = sponsor.LocalRpc(URL, **rpc)
        with mock.patch.object(sponsor._DIRECT_OPENER, "open", side_effect=fake_open):
            result = rpc_client.call(method, params)
        return result, sent[0]

    def test_amounts_are_exact_both_ways(self):
        result, request = self.exchange(
            '{"jsonrpc":"2.0","id":1,"result":{"amount":0.000400000,"vout":3,"big":12345678.123456789}}',
            params={"dummy": "", "amounts": {"a": Decimal("1E-7"), "b": POOL_VALUE}, "fee_rate": Decimal("2.5")},
            user="u", password="p")
        self.assertEqual(result, {"amount": Decimal("0.000400000"), "vout": 3, "big": Decimal("12345678.123456789")})
        self.assertIsInstance(result["amount"], Decimal)
        body = json.loads(request.data)
        self.assertEqual(body["method"], "sendmany")
        # Named parameters stay an object; Decimals go out as exact decimal
        # strings, never in exponent notation or as floats.
        self.assertEqual(body["params"], {"dummy": "", "amounts": {"a": "0.0000001", "b": "0.000400000"},
                                          "fee_rate": "2.5"})
        self.assertEqual(request.get_header("Authorization"), "Basic dTpw")
        self.assertEqual(request.full_url, URL)

    def test_rpc_error_and_bad_reply(self):
        with self.assertRaises(observer.RpcError) as caught:
            self.exchange('{"jsonrpc":"2.0","id":1,"error":{"code":-26,"message":"sponsor-tip-missing"}}',
                          params=[1])
        self.assertEqual((caught.exception.code, caught.exception.message), (-26, "sponsor-tip-missing"))
        with self.assertRaisesRegex(sponsor.SponsorError, "malformed JSON-RPC reply"):
            self.exchange("[1]")
        with self.assertRaisesRegex(sponsor.SponsorError, "transport failure"):
            self.exchange("not json")

    def test_cookie_auth(self):
        with mock.patch.object(sponsor.Path, "read_text", return_value="__cookie__:secret\n"):
            _, request = self.exchange('{"result": null}', cookie="/regtest/.cookie")
        self.assertEqual(request.get_header("Authorization"), "Basic X19jb29raWVfXzpzZWNyZXQ=")
        with self.assertRaisesRegex(sponsor.SponsorError, "cannot read the B3 RPC cookie"):
            sponsor.LocalRpc(URL, cookie="/nonexistent/b3-sponsor/.cookie").call("getblockchaininfo")


# =============================================================================
# status
# =============================================================================
class StatusTests(unittest.TestCase):
    def test_pool_tips_and_market_note(self):
        coins = [coin(1), coin(2, confirmations=0), coin(3, amount="0.001"), coin(4, label="other")]
        node, wallet = make_world(coins)
        result = sponsor.status(node, wallet, value=POOL_VALUE)
        self.assertEqual(result["note"], sponsor.MARKET_NOTE)
        self.assertIn("B3/bUSD FlowMesh market, which does not exist yet", result["note"])
        self.assertIn("tips accrue as bUSD V2", result["note"])
        self.assertEqual((result["pool"]["coins"], result["pool"]["confirmed"], result["pool"]["unconfirmed"]),
                         (2, 1, 1))
        self.assertEqual(result["pool"]["total"], Decimal("0.000800000"))
        self.assertEqual(result["labelled_by_value"], {"0.000400000": 2, "0.001": 1})
        self.assertEqual(result["tips"], {"asset_id": ASSET_ID, "confirmed": 100_000, "unconfirmed": 50_000,
                                          "spendable": 100_000, "utxos": 3})
        self.assertEqual(result["complete_max_fee"], POOL_VALUE)
        self.assertEqual(wallet.params_of("getwalletassets"), [[ASSET_ID, 0, True]])
        # Read-only.
        self.assertFalse({"sendmany", "lockunspent", "bridgesponsorburn"} & set(node.methods()))
        # Without --value every labelled coin is reported.
        node, wallet = make_world(coins)
        self.assertEqual(sponsor.status(node, wallet)["pool"]["coins"], 3)
        # Exact amounts on stdout: decimal strings.
        printed = json.loads(sponsor.dumps(result))
        self.assertEqual(printed["pool"]["total"], "0.000800000")
        self.assertEqual(printed["value"], "0.000400000")

    def test_no_tips_yet_and_bridge_not_configured(self):
        node, wallet = make_world()
        wallet.replies["getwalletassets"] = {"assets": []}
        result = sponsor.status(node, wallet)
        self.assertEqual(result["tips"], {"asset_id": ASSET_ID, "confirmed": 0, "unconfirmed": 0,
                                          "spendable": 0, "utxos": 0})
        node, wallet = make_world()
        node.replies["getbridgev2info"] = rpc_error("getbridgev2info", "Bridge V2 is not configured", -1)
        result = sponsor.status(node, wallet)
        self.assertEqual(result["bridge_v2"], {"configured": False, "reason": "Bridge V2 is not configured"})
        self.assertIsNone(result["tips"])
        self.assertEqual(result["note"], sponsor.MARKET_NOTE)
        self.assertNotIn("getwalletassets", node.methods())


# =============================================================================
# complete
# =============================================================================
class CompleteTests(unittest.TestCase):
    def world(self, reply=None):
        node, wallet = make_world()
        wallet.replies["bridgesponsorburn"] = completion() if reply is None else reply
        return node, wallet

    def test_maps_options(self):
        node, wallet = self.world()
        result = sponsor.complete(node, wallet, PARTIAL, MIN_TIP, max_fee=POOL_VALUE)
        self.assertEqual(result["txid"], txid(0xc0))
        # max_fee is positional; the pool label restricts the fee coin.
        self.assertEqual(wallet.params_of("bridgesponsorburn"),
                         [[PARTIAL, MIN_TIP, POOL_VALUE, {"broadcast": True, "fee_label": LABEL}]])
        self.assertEqual(node.methods(), ["getblockchaininfo", "bridgesponsorburn"])
        node, wallet = self.world(completion(broadcast=False))
        sponsor.complete(node, wallet, " 0x" + PARTIAL.upper() + "\n", MIN_TIP, fee_rate=Decimal("2.5"),
                         max_fee=POOL_VALUE, minconf=2, broadcast=False, label="fees")
        self.assertEqual(wallet.params_of("bridgesponsorburn"),
                         [[PARTIAL, MIN_TIP, POOL_VALUE, {"broadcast": False, "fee_rate": Decimal("2.5"),
                                                          "minconf": 2, "fee_label": "fees"}]])
        # label None: any coin of the wallet up to max_fee.
        node, wallet = self.world()
        sponsor.complete(node, wallet, PARTIAL, MIN_TIP, max_fee=POOL_VALUE, label=None)
        self.assertEqual(wallet.params_of("bridgesponsorburn"), [[PARTIAL, MIN_TIP, POOL_VALUE, {"broadcast": True}]])

    def test_node_refusal_is_reported(self):
        for message in ("sponsor-tip-below-minimum: tip 50000 < 50001",
                        "sponsor-signature-invalid: input 0 (Signature must be zero for failed CHECK(MULTI)SIG "
                        "operation): an output changed after the user signed",
                        "No native fee coin labelled 'b3-sponsor-fee' between 0.000100000 and 0.000400000; "
                        "refill the fee pool",
                        "The sponsored sighash rule (owner ruling R10) is not active for the next block"):
            node, wallet = self.world(rpc_error("bridgesponsorburn", message, -26))
            with self.assertRaisesRegex(sponsor.SponsorError, "^bridgesponsorburn refused the partial: "):
                sponsor.complete(node, wallet, PARTIAL, MIN_TIP, max_fee=POOL_VALUE)
            try:
                sponsor.complete(node, wallet, PARTIAL, MIN_TIP, max_fee=POOL_VALUE)
            except sponsor.SponsorError as e:
                self.assertTrue(str(e).endswith(message))

    def test_local_refusals_send_nothing(self):
        for hex_text in ("", "0x", "abc", "zz" * 4, None):
            node, wallet = self.world()
            with self.assertRaisesRegex(sponsor.SponsorError, "--hex"):
                sponsor.complete(node, wallet, hex_text, MIN_TIP, max_fee=POOL_VALUE)
            self.assertNotIn("bridgesponsorburn", node.methods())
        for min_tip in (0, -1, True, "50000"):
            node, wallet = self.world()
            with self.assertRaisesRegex(sponsor.SponsorError, "--min-tip"):
                sponsor.complete(node, wallet, PARTIAL, min_tip, max_fee=POOL_VALUE)
            self.assertNotIn("bridgesponsorburn", node.methods())
        # max_fee is required: no default that could spend a large coin.
        for max_fee, message in ((None, "--max-fee is required"), (Decimal(0), "--max-fee must be positive"),
                                 (Decimal("0.0000000001"), "--max-fee has more than 9 decimal places")):
            node, wallet = self.world()
            with self.assertRaisesRegex(sponsor.SponsorError, message):
                sponsor.complete(node, wallet, PARTIAL, MIN_TIP, max_fee=max_fee)
            self.assertNotIn("bridgesponsorburn", node.methods())
        with self.assertRaises(TypeError):
            sponsor.complete(*self.world(), PARTIAL, MIN_TIP)
        for label in ("", "*"):
            node, wallet = self.world()
            with self.assertRaisesRegex(sponsor.SponsorError, "--label"):
                sponsor.complete(node, wallet, PARTIAL, MIN_TIP, max_fee=POOL_VALUE, label=label)
            self.assertNotIn("bridgesponsorburn", node.methods())
        node, wallet = self.world()
        with self.assertRaisesRegex(sponsor.SponsorError, "--fee-rate has more than 3 decimal places"):
            sponsor.complete(node, wallet, PARTIAL, MIN_TIP, max_fee=POOL_VALUE, fee_rate=Decimal("1.0001"))
        self.assertNotIn("bridgesponsorburn", node.methods())

    def test_result_contradicting_the_request_is_refused(self):
        cases = (
            (completion(tip={"vout": 2, "amount": MIN_TIP - 1}), f"tip {MIN_TIP - 1}, below --min-tip"),
            (completion(tip=None), "tip None, below --min-tip"),
            (completion(broadcast=False), "reports broadcast=False"),
            (completion(network_fee=Decimal("0.000400001")), "paying 0.000400001, above --max-fee 0.000400000"),
            (completion(txid="zz"), "no completed transaction"),
            ([], "no completed transaction"),
        )
        for reply, message in cases:
            node, wallet = self.world(reply)
            with self.assertRaisesRegex(sponsor.SponsorError, message):
                sponsor.complete(node, wallet, PARTIAL, MIN_TIP, max_fee=POOL_VALUE)


# =============================================================================
# release
# =============================================================================
class ReleaseTests(unittest.TestCase):
    def world(self, locked, txouts, labels):
        node, wallet = make_world()
        wallet.replies["listlockunspent"] = locked
        node.replies["gettxout"] = lambda params: txouts.get((params[0], params[1]))
        wallet.replies["getaddressinfo"] = lambda params: {"address": params[0], "labels": labels.get(params[0], [])}
        return node, wallet

    def test_unlocks_only_unspent_pool_coins(self):
        def out(address):
            return {"value": POOL_VALUE, "scriptPubKey": {"address": address}}
        locked = [{"txid": txid(1), "vout": 0}, {"txid": txid(2), "vout": 1}, {"txid": txid(3), "vout": 0},
                  {"txid": txid(4), "vout": 2}]
        txouts = {
            (txid(1), 0): out("mpool1"),    # stale: completion lost or never sent
            # txid(2):1 is spent in the chain or the mempool: the completion stands
            (txid(3), 0): out("mother"),    # not a pool coin (another lock of this wallet)
            (txid(4), 2): out("mpool4"),    # stale
        }
        node, wallet = self.world(locked, txouts, {"mpool1": [LABEL], "mother": ["savings"], "mpool4": [LABEL]})
        result = sponsor.release(node, wallet)
        self.assertEqual(result["released"], [{"txid": txid(1), "vout": 0}, {"txid": txid(4), "vout": 2}])
        self.assertEqual([(entry["txid"], entry["reason"]) for entry in result["kept"]],
                         [(txid(2), "spent in the chain or the mempool"), (txid(3), f"not labelled {LABEL}")])
        self.assertEqual(wallet.params_of("lockunspent"), [[True, result["released"]]])
        # Every lookup includes the mempool.
        self.assertEqual([params[2] for params in node.params_of("gettxout")], [True] * 4)

    def test_nothing_stale_sends_no_unlock(self):
        node, wallet = self.world([{"txid": txid(2), "vout": 1}], {}, {})
        result = sponsor.release(node, wallet)
        self.assertEqual((result["released"], len(result["kept"])), ([], 1))
        self.assertNotIn("lockunspent", wallet.methods())

    def test_refusals(self):
        node, wallet = self.world([{"txid": "zz", "vout": 0}], {}, {})
        with self.assertRaisesRegex(sponsor.SponsorError, "malformed outpoint"):
            sponsor.release(node, wallet)
        node, wallet = make_world(chain="main")
        with self.assertRaisesRegex(sponsor.SponsorError, "regtest only"):
            sponsor.release(node, wallet)
        self.assertEqual(node.methods(), ["getblockchaininfo"])
        node, wallet = self.world([{"txid": txid(1), "vout": 0}],
                                  {(txid(1), 0): {"scriptPubKey": {"address": "mpool1"}}}, {"mpool1": [LABEL]})
        wallet.replies["lockunspent"] = rpc_error("lockunspent", "Invalid parameter")
        with self.assertRaisesRegex(sponsor.SponsorError, "could not unlock the 1 stale fee coins"):
            sponsor.release(node, wallet)


# =============================================================================
# CLI
# =============================================================================
COMMON = ["--b3-rpc-url", URL, "--wallet", "relayer"]


class CliTests(unittest.TestCase):
    def run_main(self, argv, world=None):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            if world is None:
                code = sponsor.main(argv)
            else:
                with mock.patch.object(sponsor, "LocalRpc", return_value=world) as factory:
                    code = sponsor.main(argv)
                self.factory_args = factory.call_args
        return code, out.getvalue(), err.getvalue()

    def test_help(self):
        for argv in (["--help"], ["refill", "--help"], ["status", "--help"], ["complete", "--help"],
                     ["release", "--help"]):
            with self.assertRaises(SystemExit) as caught, contextlib.redirect_stdout(io.StringIO()) as out:
                sponsor.parse_args(argv)
            self.assertEqual(caught.exception.code, 0)
            self.assertIn("usage:", out.getvalue())

    def test_complete_maps_options_and_no_broadcast(self):
        node, wallet = make_world()
        wallet.replies["bridgesponsorburn"] = completion(broadcast=False)
        code, out, err = self.run_main(["complete", *COMMON, "--b3-cookie", "/regtest/.cookie", "--hex", PARTIAL,
                                        "--min-tip", str(MIN_TIP), "--fee-rate", "2.5", "--max-fee",
                                        "0.0004", "--minconf", "0", "--no-broadcast"], node)
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(self.factory_args, mock.call(URL, None, None, "/regtest/.cookie", timeout=120))
        self.assertEqual(wallet.params_of("bridgesponsorburn"),
                         [[PARTIAL, MIN_TIP, Decimal("0.0004"), {"broadcast": False, "fee_rate": Decimal("2.5"),
                                                                 "minconf": 0, "fee_label": LABEL}]])
        printed = json.loads(out)
        self.assertEqual((printed["wallet"], printed["txid"], printed["network_fee"]),
                         ("relayer", txid(0xc0), "0.000400000"))
        # Without --no-broadcast the completion is broadcast.
        node, wallet = make_world()
        wallet.replies["bridgesponsorburn"] = completion()
        code, _, _ = self.run_main(["complete", *COMMON, "--hex", PARTIAL, "--min-tip", "1", "--max-fee",
                                    "0.0004", "--label", "fees"], node)
        self.assertEqual(code, 0)
        self.assertEqual(wallet.params_of("bridgesponsorburn"),
                         [[PARTIAL, 1, Decimal("0.0004"), {"broadcast": True, "fee_label": "fees"}]])

    def test_refill_and_status_cli(self):
        node, wallet = make_world([coin(1)])
        code, out, err = self.run_main(["refill", *COMMON, "--count", "2", "--value", "0.0004",
                                        "--label", "fees"], node)
        self.assertEqual((code, err), (0, ""))
        printed = json.loads(out)
        self.assertEqual((printed["created"], printed["label"], printed["value"]), (2, "fees", "0.0004"))
        self.assertEqual(wallet.params_of("getnewaddress"), [["fees", "legacy"]] * 2)
        node, wallet = make_world([coin(1)])
        code, out, _ = self.run_main(["status", *COMMON, "--value", "0.0004"], node)
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out)["note"], sponsor.MARKET_NOTE)
        node, wallet = make_world()
        wallet.replies["listlockunspent"] = [{"txid": txid(5), "vout": 0}]
        node.replies["gettxout"] = {"scriptPubKey": {"address": "mpool5"}}
        wallet.replies["getaddressinfo"] = {"labels": ["fees"]}
        code, out, err = self.run_main(["release", *COMMON, "--label", "fees"], node)
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(json.loads(out)["released"], [{"txid": txid(5), "vout": 0}])
        self.assertEqual(wallet.params_of("lockunspent"), [[True, [{"txid": txid(5), "vout": 0}]]])

    def test_refusal_exits_1_with_error(self):
        node, wallet = make_world(chain="main")
        code, out, err = self.run_main(["status", *COMMON], node)
        self.assertEqual((code, out), (1, ""))
        self.assertTrue(err.startswith("error: refusing B3 chain 'main'"), err)
        node, wallet = make_world()
        wallet.replies["bridgesponsorburn"] = rpc_error("bridgesponsorburn", "sponsor-tip-missing", -26)
        code, out, err = self.run_main(["complete", *COMMON, "--hex", PARTIAL, "--min-tip", "5", "--max-fee",
                                        "0.0004"], node)
        self.assertEqual((code, out), (1, ""))
        self.assertEqual(err, "error: bridgesponsorburn refused the partial: sponsor-tip-missing\n")

    def test_non_local_endpoint_refused_before_any_request(self):
        with mock.patch.object(sponsor._DIRECT_OPENER, "open", side_effect=AssertionError("network used")):
            for url in ("https://127.0.0.1:18443", "http://10.0.0.1:18443", "http://u:p@localhost:1"):
                code, out, err = self.run_main(["status", "--b3-rpc-url", url, "--wallet", "relayer"])
                self.assertEqual((code, out), (1, ""))
                self.assertTrue(err.startswith("error: refusing non-local endpoint"), err)

    def test_argument_errors(self):
        bad = (
            ["refill", *COMMON, "--count", "0", "--value", "0.0004"],
            ["refill", *COMMON, "--count", "01", "--value", "0.0004"],
            ["refill", *COMMON, "--count", "2", "--value", "0.0000000001"],
            ["refill", *COMMON, "--count", "2", "--value", "0"],
            ["refill", *COMMON, "--count", "2", "--value", "inf"],
            ["refill", *COMMON, "--count", "2", "--value", "0.0004", "--label", ""],
            ["refill", *COMMON, "--count", "2", "--value", "0.0004", "--label", "*"],
            ["refill", *COMMON, "--count", "2", "--value", "0.0004", "--fee-rate", "-1"],
            ["refill", "--b3-rpc-url", URL, "--count", "2", "--value", "0.0004"],
            ["status", *COMMON, "--b3-rpc-user", "u"],
            ["status", *COMMON, "--b3-rpc-user", "u", "--b3-rpc-password", "p", "--b3-cookie", "c"],
            ["status", "--b3-rpc-url", URL, "--wallet", ""],
            ["complete", *COMMON, "--hex", PARTIAL, "--max-fee", "0.0004"],
            ["complete", *COMMON, "--hex", PARTIAL, "--min-tip", "1"],
            ["complete", *COMMON, "--hex", PARTIAL, "--min-tip", "0", "--max-fee", "0.0004"],
            ["complete", *COMMON, "--hex", PARTIAL, "--min-tip", "1", "--max-fee", "-0.1"],
            ["complete", *COMMON, "--hex", PARTIAL, "--min-tip", "1", "--max-fee", "0.0004", "--fee-rate",
             "0.0001"],
            ["complete", *COMMON, "--hex", PARTIAL, "--min-tip", "1", "--max-fee", "0.0004", "--no-broadcast",
             "true"],
            ["complete", *COMMON, "--hex", PARTIAL, "--min-tip", "1", "--max-fee", "0.0004", "--label", "*"],
            ["release", *COMMON, "--label", ""],
        )
        with contextlib.redirect_stderr(io.StringIO()):
            for argv in bad:
                with self.assertRaises(SystemExit, msg=argv):
                    sponsor.parse_args(argv)
        args = sponsor.parse_args(["refill", *COMMON, "--count", "4", "--value", "4E-4"])
        self.assertEqual((args.count, args.value, args.label, args.minconf, args.fee_rate),
                         (4, Decimal("0.0004"), LABEL, 0, None))
        args = sponsor.parse_args(["complete", *COMMON, "--hex", PARTIAL, "--min-tip", "50000", "--max-fee",
                                   "0.0004"])
        self.assertEqual((args.min_tip, args.max_fee, args.fee_rate, args.minconf, args.no_broadcast, args.label),
                         (50000, Decimal("0.0004"), None, None, False, LABEL))
        args = sponsor.parse_args(["release", *COMMON])
        self.assertEqual(args.label, LABEL)


if __name__ == "__main__":
    unittest.main()
