#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Wallet signing preflight join, end to end on the engine-off client fixture.

The client runs with the regtest-only -flowmeshtestjoinwindowms, so the join
windows are wide and every case below is decided by the join's own-action
rules rather than by timing. Each wallet call is observed through the TLS
relays (which requests it sent) and a client timing capture (the Market
span's 'preflight' field):

A. After a verified balance read, an order with no explicit sequence joins
   that read: no 'updates'/'snapshot' request, and it signs the certified
   next sequence.
B. Once that order's certified inclusion is verified, the next action (no
   sequence) refreshes and signs previous+1: the verified inclusion is newer
   than the stamped cache.
C. While an own order is unresolved (two seats stopped signing), a freshly
   stamped cache is not joined: the preflight refreshes, and the sequence it
   still returns is refused locally without any submission.
D. After a client restart, restored certified actions whose sequences the
   cached state has consumed do not block a join.

Finally the option is refused out of range and with the validator engine.
Generated regtest and loopback only; no latency or fill claim. Run directly.
"""

import json
import time
from pathlib import Path

from feature_flowmesh_client_poll import FlowMeshClientPollTest
from feature_flowmesh_latency import PRE_ADMISSION_REJECTIONS
from feature_flowmesh_release import TRADE_PRICE
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, p2p_port

# Wide enough that no case below depends on how long a step takes.
JOIN_WINDOW_MS = 300_000
JOIN_WINDOW_MAX_MS = 600_000
REFRESH_METHODS = {"updates", "snapshot"}


class FlowMeshClientJoinTest(FlowMeshClientPollTest):
    def set_test_params(self):
        super().set_test_params()
        self.client_extra_args = [f"-flowmeshtestjoinwindowms={JOIN_WINDOW_MS}"]
        self.join_report = {"fixture": "signing_preflight_join", "correctness_pass": False,
                            "join_window_ms": JOIN_WINDOW_MS, "cases": {},
                            "scope": "generated regtest only; no latency/fill claim"}

    def ready_balance(self, market):
        """Balance reads until one reports a running, unpaused market. The
        last read is the verified refresh that a following preflight may join."""
        observed = {}

        def ready():
            self.pump_b3()
            try:
                status = self.client.getflowmeshbalance(market)
            except JSONRPCException as error:
                # For example right after a restart, before the market is known.
                observed["error"] = error.error
                return False
            observed["status"] = status
            return (status["available"] and status["running"] and not status["paused"] and status["halt"] == "none"
                    and not status["error"] and "account" in status)

        try:
            self.wait_until(ready, timeout=60, check_interval=.05)
        except AssertionError:
            raise AssertionError(f"market not ready for a signing preflight: {observed}")
        verification = observed["status"]["verification"]
        assert_equal((verification["certificate_verified"], verification["account_state_verified"]), (True, True))
        return observed["status"]

    def captured_call(self, method, *params):
        """One wallet RPC: its result or error, the preflight outcome of every
        Market span it made, and the methods of the HTTPS requests it sent."""
        marks = self.marks()
        self.client.flowmeshtiming("start")
        result = error = None
        try:
            result = getattr(self.client, method)(*params)
        except JSONRPCException as exception:
            error = exception
        finally:
            capture = self.client.flowmeshtiming("stop")
        assert_equal(capture["dropped"], 0)
        preflights = [row["event"]["preflight"] for row in capture["events"]
                      if row["event"].get("stage") == "Market" and "preflight" in row["event"]]
        requests = [row["method"] for row in self.requests_since(marks)]
        return result, error, preflights, requests

    def signed_call(self, method, market, *params, retry_pre_admission=True):
        """Retries only an explicit pre-admission refusal (nothing was signed).
        Returns the result and every attempt's preflights and requests. The
        first attempt carries the join decision under test. A refused attempt
        may itself have stamped the not-ready status it refused on, which the
        widened windows would let every retry join; so each retry follows a
        ready balance read, and may legitimately join that read."""
        deadline = time.monotonic() + 60
        attempts = []
        while True:
            result, error, preflights, requests = self.captured_call(method, market, *params)
            attempts.append({"preflights": preflights, "requests": requests,
                             "error": None if error is None else error.error.get("message")})
            if error is None:
                return {"result": result, "attempts": attempts}
            if (not retry_pre_admission or error.error.get("code") != -1 or
                    error.error.get("message") not in PRE_ADMISSION_REJECTIONS):
                raise error
            assert time.monotonic() < deadline, f"pre-admission gate did not reopen: {attempts}"
            self.ready_balance(market)

    def assert_joined(self, call, sequence):
        assert_equal(len(call["attempts"]), 1)
        attempt = call["attempts"][0]
        assert_equal(attempt["preflights"], ["joined"])
        assert not REFRESH_METHODS.intersection(attempt["requests"]), call
        assert_equal(attempt["requests"].count("submit"), 1)
        assert_equal(call["result"]["sequence"], sequence)
        assert self.delivered_or_refused(call["result"]), call

    @staticmethod
    def delivered_or_refused(receipt):
        """The endpoint admitted the signed action, or refused it before
        admission while reconciling (definite; resent exactly below)."""
        return (receipt["receipt_state"] in {"queued", "admitted", "certified_inclusion"} or
                (receipt["receipt_state"] == "rejected" and receipt["reason"] in PRE_ADMISSION_REJECTIONS))

    def until_admitted(self, market, action_id, receipt):
        """Resend the exact signed bytes (never a new signature) while the
        endpoint refuses them before admission."""
        deadline = time.monotonic() + 60
        while receipt["receipt_state"] == "rejected":
            assert receipt["reason"] in PRE_ADMISSION_REJECTIONS, receipt
            assert time.monotonic() < deadline, receipt
            self.pump_b3()
            time.sleep(.1)
            receipt = self.client.retryflowmeshaction(market, action_id)
            assert_equal(receipt["action_id"], action_id)
        return receipt

    def assert_refreshed(self, call):
        first = call["attempts"][0]
        assert_equal(first["preflights"], ["refreshed"])
        assert REFRESH_METHODS.intersection(first["requests"]), call

    def certified(self, market, action_id):
        marks = self.marks()
        try:
            return self.wait_certified(market, action_id, allow_exact_retry=True)
        except AssertionError:
            diagnostics = {"status": self.client.getflowmeshactionstatus(market, action_id),
                           "retained": self.own_actions(market).get(action_id),
                           "requests": [(row["endpoint_index"], row["method"], row.get("upstream_http_status"))
                                        for row in self.requests_since(marks)][-8:]}
            for index in (0, 1):
                try:
                    status = self.nodes[index].getflowmeshbalance(market)
                    diagnostics[f"validator{index}"] = {key: status.get(key) for key in (
                        "running", "paused", "halt", "error", "next_microblock_sequence", "pending_actions", "round")}
                except JSONRPCException as error:
                    diagnostics[f"validator{index}"] = error.error
            raise AssertionError(f"action {action_id} did not certify: {diagnostics}")

    def own_actions(self, market):
        return {row["action_id"]: row for row in self.client.listflowmeshactions(market)["actions"]}

    def restart_client(self):
        self.client.stop_node()
        self.client.start()
        self.client.wait_for_rpc_connection()
        self.client_clock = None
        self.sync_client_time()
        self.client.addnode(f"127.0.0.1:{p2p_port(0)}", "onetry")
        self.wait_client_b3_sync()

    def run_join_cases(self, market):
        cases = self.join_report["cases"]
        for relay in self.tls_relays:
            relay.configure()
        self.select_endpoint(0)

        # A. The order joins the balance read made just before it.
        balance = self.ready_balance(market)
        first_sequence = balance["account"]["next_sequence"]
        joined = self.signed_call("submitflowmeshorder", market, "bid", TRADE_PRICE // 2, 1, retry_pre_admission=False)
        self.assert_joined(joined, first_sequence)
        first = joined["result"]["action_id"]
        self.certified(market, first)
        cases["A_joined_certified_next_sequence"] = {"sequence": first_sequence, "attempts": joined["attempts"]}

        # B. No market read since A: the only fresh stamp predates the verified
        # inclusion of A's order, so this preflight must not reuse it.
        after_inclusion = self.signed_call("cancelflowmeshorder", market, "bid")
        self.assert_refreshed(after_inclusion)
        assert_equal(after_inclusion["result"]["sequence"], first_sequence + 1)
        cancel = after_inclusion["result"]["action_id"]
        self.certified(market, cancel)
        cases["B_refreshed_after_inclusion"] = {"sequence": first_sequence + 1,
                                                "attempts": after_inclusion["attempts"]}

        # C. Two of four seats stop signing, so the next order stays
        # unresolved. A balance read stamps a cache that cannot reflect it.
        for node in self.nodes[2:]:
            assert_equal(node.stopflowmeshvalidator()["running"], False)
        try:
            pending = self.signed_call("submitflowmeshorder", market, "bid", TRADE_PRICE // 2, 1)
            self.assert_refreshed(pending)  # A verified inclusion again reset the stamp.
            assert_equal(pending["result"]["sequence"], first_sequence + 2)
            assert self.delivered_or_refused(pending["result"]), pending
            third = pending["result"]["action_id"]
            # It must be admitted to stay unresolved: a definite refusal
            # would let the next preflight join.
            receipt = self.until_admitted(market, third, pending["result"])
            assert receipt["receipt_state"] in {"queued", "admitted", "unknown"}, receipt
            assert_equal(self.own_actions(market)[third]["receipt"]["certificate_verified"], False)
            self.client.getflowmeshbalance(market)
            unresolved = self.signed_call("submitflowmeshorder", market, "bid", TRADE_PRICE // 2 - 1, 1)
            # Every attempt refreshes while the own order is unresolved.
            for attempt in unresolved["attempts"]:
                assert_equal(attempt["preflights"], ["refreshed"])
                assert REFRESH_METHODS.intersection(attempt["requests"]), unresolved
                assert "submit" not in attempt["requests"], unresolved
            # The refreshed state cannot include the unresolved order, so the
            # wallet signs its sequence again; the client refuses to retain
            # or send a different action for it.
            refused = unresolved["result"]
            assert_equal((refused["sequence"], refused["receipt_state"]), (first_sequence + 2, "rejected"))
            assert "already uses this account sequence" in refused["reason"], refused
            assert refused["action_id"] not in self.own_actions(market)
            cases["C_refreshed_while_unresolved"] = {"attempts": unresolved["attempts"], "reason": refused["reason"]}
        finally:
            for node in self.nodes[2:]:
                assert_equal(node.startflowmeshvalidator()["armed_keys"], 1)
        self.certified(market, third)

        # D. Restored certified actions carry no microblock, but each sequence
        # is below the cached next sequence, so the join is still taken.
        self.restart_client()
        restored = self.own_actions(market)
        for action_id in (first, cancel, third):
            assert_equal(restored[action_id]["previously_certified"], True)
            assert_equal(restored[action_id]["receipt"]["certificate_verified"], False)
        balance = self.ready_balance(market)
        assert_equal(balance["account"]["next_sequence"], first_sequence + 3)
        restart_join = self.signed_call("cancelflowmeshorder", market, "bid", retry_pre_admission=False)
        self.assert_joined(restart_join, first_sequence + 3)
        self.certified(market, restart_join["result"]["action_id"])
        cases["D_joined_past_restored_certified"] = {"sequence": first_sequence + 3,
                                                     "attempts": restart_join["attempts"]}

    def check_option_refusals(self):
        self.client.stop_node()
        base = [arg for arg in self.client_args if not arg.startswith("-flowmeshtestjoinwindowms=")]
        self.client.assert_start_raises_init_error(
            extra_args=[*base, f"-flowmeshtestjoinwindowms={JOIN_WINDOW_MAX_MS + 1}"],
            expected_msg=f"Error: -flowmeshtestjoinwindowms must be between 0 and {JOIN_WINDOW_MAX_MS}")
        engine = [arg for arg in base if not arg.startswith("-enableflowmeshvalidator=")]
        self.client.assert_start_raises_init_error(
            extra_args=[*engine, "-enableflowmeshvalidator=1", f"-flowmeshtestjoinwindowms={JOIN_WINDOW_MS}"],
            expected_msg="Error: -flowmeshtestjoinwindowms applies only to the HTTPS trading client (-enableflowmeshvalidator=0)")

    def run_test(self):
        assert_equal(self.options.qt_review_hold_seconds, 0)
        try:
            market, asset = self.bootstrap_latency_market()
            account = self.fund_latency_client(market, asset)
            self.join_report.update(market_id=market, account_id=account["account_id"])
            self.begin_b3_workload(range(4))
            self.run_join_cases(market)
            self.assert_engine_off()
            self.assert_no_b3_flowmesh_traffic()
            self.join_report["correctness_pass"] = True
        except Exception as error:
            self.join_report["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            try:
                self.end_b3_workload()
            finally:
                path = Path(self.options.tmpdir, "flowmesh-client-join.json")
                path.write_text(json.dumps(self.join_report, sort_keys=True, indent=2, default=str) + "\n",
                                encoding="utf-8")
                self.log.info("FLOWMESH_CLIENT_JOIN_REPORT %s", path)
        self.check_option_refusals()


if __name__ == "__main__":
    FlowMeshClientJoinTest(__file__).main()
