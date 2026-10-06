#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Offline unit tests for b3_bridge_deposit_guard_v2.py (fake RPC objects, no network)."""
import contextlib
import io
import json
from pathlib import Path
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import b3_bridge_deposit_guard_v2 as guard  # noqa: E402
import b3_bridge_observe_v2 as observer  # noqa: E402

CLOSED_REASON = "sticky kind 13 conflict at height 412"
REFUSAL = "; this relayer refuses deposits and flushes"


def bridge_v2_info(deposits=None, **overrides):
    info = {
        "configured": True, "active": True, "state_available": True, "next_height": 420,
        "activation_height": 281, "origin_chain_id": 31337,
        "deposits": {"status": "OPEN"} if deposits is None else deposits,
    }
    info.update(overrides)
    return info


class FakeRpc:
    """Records every call; replies from a dict of callables, values or exceptions."""

    def __init__(self, replies):
        self.replies = dict(replies)
        self.calls = []

    def call(self, method, params=()):
        self.calls.append((method, list(params)))
        if method not in self.replies:
            raise AssertionError(f"unexpected RPC {method}")
        reply = self.replies[method]
        if isinstance(reply, Exception):
            raise reply
        return reply(params) if callable(reply) else reply

    def methods(self):
        return [method for method, _ in self.calls]


class DepositGuardTests(unittest.TestCase):
    def setUp(self):
        self.node = FakeRpc({
            "getblockchaininfo": {"chain": "regtest"},
            "getbridgev2info": bridge_v2_info(),
        })

    def assert_refused(self, message, error=observer.ObserverError):
        with self.assertRaisesRegex(error, message) as caught:
            guard.deposit_guard(self.node)
        return caught.exception

    # -- status ---------------------------------------------------------------
    def test_open(self):
        self.assertEqual(guard.deposit_guard(self.node), {"deposits": "OPEN", "next_height": 420})
        # Read-only: the chain check comes first and nothing is submitted.
        self.assertEqual(self.node.methods(), ["getblockchaininfo", "getbridgev2info"])

    def test_closed_for_good_with_reason(self):
        self.node.replies["getbridgev2info"] = bridge_v2_info({"status": "CLOSED_FOR_GOOD",
                                                               "reason": CLOSED_REASON})
        error = self.assert_refused("^bridge V2 deposits are closed for good: " + CLOSED_REASON + REFUSAL + "$",
                                    guard.DepositsClosedError)
        self.assertEqual(error.reason, CLOSED_REASON)
        # A flush path can tell the permanent close from any other refusal,
        # and a generic ObserverError handler still catches it.
        self.assertIsInstance(error, observer.ObserverError)
        self.node.replies["getbridgev2info"] = bridge_v2_info({"status": "CLOSED_FOR_GOOD"})
        self.assertEqual(self.assert_refused("no reason reported", guard.DepositsClosedError).reason,
                         "no reason reported")

    def test_unavailable(self):
        self.node.replies["getbridgev2info"] = bridge_v2_info(
            {"status": "UNAVAILABLE", "reason": "the bridge state is unavailable at the active tip"})
        error = self.assert_refused("deposit status is unavailable \\(the bridge state is unavailable at the "
                                    "active tip\\)" + REFUSAL)
        self.assertNotIsInstance(error, guard.DepositsClosedError)

    def test_missing_or_unknown_status_fails_closed(self):
        for deposits, message in (({}, "unknown bridge V2 deposit status None"),
                                  ({"status": "open"}, "unknown bridge V2 deposit status 'open'"),
                                  ({"status": "CLOSED"}, "unknown bridge V2 deposit status 'CLOSED'")):
            self.node.replies["getbridgev2info"] = bridge_v2_info(deposits)
            self.assert_refused(message)
        info = bridge_v2_info()
        del info["deposits"]
        self.node.replies["getbridgev2info"] = info
        self.assert_refused("reports no deposit status")

    # -- refusals ---------------------------------------------------------------
    def test_refuses_non_regtest_chain(self):
        for chain in ("main", "test", "testnet4", "signet", None):
            self.node.calls.clear()
            self.node.replies["getblockchaininfo"] = {"chain": chain}
            self.assert_refused("regtest only")
            self.assertEqual(self.node.methods(), ["getblockchaininfo"])
        self.node.replies["getblockchaininfo"] = ["regtest"]
        self.assert_refused("regtest only")

    def test_refuses_bridge_v2_not_configured(self):
        self.node.replies["getbridgev2info"] = observer.RpcError(
            "getbridgev2info", {"code": -1, "message": "Bridge V2 is not configured on this chain "
                                                       "(regtest-only -b3bridgepins v2 section)"})
        self.assert_refused("^getbridgev2info refused: Bridge V2 is not configured on this chain "
                            "\\(regtest-only -b3bridgepins v2 section\\)" + REFUSAL + "$")
        self.node.replies["getbridgev2info"] = observer.RpcError("getbridgev2info",
                                                                 {"code": -28, "message": "Loading block index"})
        self.assert_refused("^getbridgev2info refused: Loading block index" + REFUSAL + "$")
        self.node.replies["getbridgev2info"] = bridge_v2_info(configured=False)
        self.assert_refused("configuration is incomplete")
        self.node.replies["getbridgev2info"] = None
        self.assert_refused("configuration is incomplete")

    def test_refuses_inactive_bridge(self):
        self.node.replies["getbridgev2info"] = bridge_v2_info(
            {"status": "UNAVAILABLE", "reason": "bridge V2 rules are not active for the next block"},
            active=False, next_height=200)
        self.assert_refused("not active for the next block \\(height 200, V 281\\)")

    def test_remote_url_refused(self):
        for url in ("http://127.0.0.1:18443", "http://localhost:18443"):
            self.assertEqual(guard.check_local_url(url), url)
        for url in ("https://127.0.0.1:18443", "http://10.0.0.1:18443", "http://user:pass@127.0.0.1:1",
                    "http://127.0.0.1.example.invalid:18443", "ws://localhost:18443", "127.0.0.1:18443", "", None):
            with self.assertRaises(observer.ObserverError, msg=url):
                guard.JsonRpc(url)
            stderr = io.StringIO()
            with mock.patch.object(guard, "JsonRpc", side_effect=AssertionError("connected")), \
                    contextlib.redirect_stderr(stderr):
                code = guard.main(["--b3-rpc-url", str(url)])
            self.assertEqual(code, 1, url)
            self.assertTrue(stderr.getvalue().startswith("error: refusing "), stderr.getvalue())

    # -- command line -----------------------------------------------------------
    def run_main(self, info):
        node = FakeRpc({"getblockchaininfo": {"chain": "regtest"}, "getbridgev2info": info})
        stdout, stderr = io.StringIO(), io.StringIO()
        with mock.patch.object(guard, "JsonRpc", return_value=node) as rpc, \
                contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = guard.main(["--b3-rpc-url", "http://127.0.0.1:18443", "--b3-cookie", "/nonexistent/.cookie"])
        rpc.assert_called_once_with("http://127.0.0.1:18443", None, None, "/nonexistent/.cookie", timeout=120)
        return code, stdout.getvalue(), stderr.getvalue()

    def test_cli_exit_codes_and_output(self):
        code, out, err = self.run_main(bridge_v2_info())
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(out, '{"deposits":"OPEN","next_height":420}\n')
        self.assertEqual(json.loads(out), {"deposits": "OPEN", "next_height": 420})

        code, out, err = self.run_main(bridge_v2_info({"status": "CLOSED_FOR_GOOD", "reason": CLOSED_REASON}))
        self.assertEqual((code, out), (1, ""))
        self.assertEqual(err, f"error: bridge V2 deposits are closed for good: {CLOSED_REASON}{REFUSAL}\n")

        code, out, err = self.run_main(bridge_v2_info({"status": "UNAVAILABLE", "reason": "no lineage"}))
        self.assertEqual((code, out), (1, ""))
        self.assertTrue(err.startswith("error: the bridge V2 deposit status is unavailable (no lineage)"), err)

    def test_cli_argument_refusals(self):
        for argv, message in ((["--b3-rpc-url", "http://127.0.0.1:1", "--b3-rpc-user", "u"], "supplied together"),
                              (["--b3-rpc-url", "http://127.0.0.1:1", "--b3-rpc-user", "u", "--b3-rpc-password", "p",
                                "--b3-cookie", "c"], "choose cookie or user/password"),
                              ([], "--b3-rpc-url")):
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit):
                guard.parse_args(argv)
            self.assertIn(message, stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
