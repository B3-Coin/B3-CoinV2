#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Bounded process-crash checks for one NEW generated engine-off client.

Four operators use the normal binary and are never crash targets. Client4
alone uses a separately compiled FLOWMESH_CLIENT_CRASH_TEST_HOOKS daemon.
Exactly one A/B/C attempt is selected before execution. Failures are retained;
this is not a loop-until-green test and is not power-loss qualification.
"""

import hashlib
import json
import os
import secrets
import signal
import threading
import time
from decimal import Decimal
from pathlib import Path

from feature_flowmesh_remote_client import FlowMeshRemoteClientTest
from test_framework.util import assert_equal, get_rpc_proxy, p2p_port


class FlowMeshClientCrashTest(FlowMeshRemoteClientTest):
    def add_options(self, parser):
        super().add_options(parser)
        parser.add_argument("--crash-client-binary", required=True,
                            help="Absolute separately-built test-hook daemon; used only for new client4")

    def set_test_params(self):
        super().set_test_params()
        self.crash_results = []
        self.crash_env = None
        self.crash_token = None
        self.crash_thread = None
        self.crash_rpc_result = None
        self.crash_evidence = None
        self.price_atoms = 1

    def run_test(self):
        super().run_test()

    def write_evidence(self, name, value):
        path = self.crash_evidence / name
        path.write_text(json.dumps(value, sort_keys=True, indent=2, default=str) + "\n", encoding="utf-8")

    def hook_events(self):
        path = self.client.datadir_path / "flowmesh-client-crash-events.jsonl"
        if not path.exists():
            return []
        # Only complete newline-terminated events count. The child may be in
        # the middle of appending its final public record when we inspect it.
        data = path.read_bytes()
        assert len(data) <= 16 * 1024 * 1024
        rows = [json.loads(line) for line in data.split(b"\n")[:-1]]
        assert all(row["token"] == self.crash_token for row in rows)
        return rows

    def configure_hook(self, case, *, pause=False):
        marker = {"token": self.crash_token, "case": case,
                  "chain_dir": str(self.client.chain_path.resolve()),
                  "fresh_generated_client": True, "pause_before_first_send": pause}
        # Parent-owned fixture marker, not a wallet or signing journal.
        temporary = self.client.datadir_path / "flowmesh-client-crash-guard.next"
        temporary.write_text(json.dumps(marker), encoding="utf-8")
        temporary.replace(self.client.datadir_path / "flowmesh-client-crash-guard.json")

    def relay_capture(self):
        snapshots = [relay.snapshot() for relay in self.tls_relays]
        assert all(row["records_dropped"] == 0 for row in snapshots), "Incomplete bounded request capture"
        return snapshots

    def submissions(self):
        return sorted([row for relay in self.relay_capture() for row in relay["requests"] if row["method"] == "submit"],
                      key=lambda row: row["host_monotonic_us"])

    def local_saved(self, market):
        before = [len(row["requests"]) for row in self.relay_capture()]
        value = self.client.listflowmeshactions(market)
        assert_equal([len(row["requests"]) for row in self.relay_capture()], before)
        assert_equal(value["source"], "local-retained-outbox")
        return value

    def latest_saved_journal(self, action_id):
        rows = [event for event in self.hook_events()
                if event["pid"] == self.client.process.pid and event["stage"] == "synchronous_journal_save_returned"
                and any(row["action_id"] == action_id for row in event["data"]["actions"])]
        assert rows, f"No synchronous save evidence for {action_id}"
        return rows[-1]

    def start_crash_client(self):
        self.client.start(extra_args=list(self.client_args), env=self.crash_env)
        self.client.wait_for_rpc_connection()
        self.client_clock = None
        self.sync_client_time()
        self.client.addnode(f"127.0.0.1:{p2p_port(0)}", "onetry")
        self.wait_client_b3_sync()

    def initialize_crash_client(self, market):
        crash_binary = Path(self.options.crash_client_binary)
        assert crash_binary.is_absolute() and crash_binary.is_file() and os.access(crash_binary, os.X_OK)
        # This setup creates client4 itself and imports no operator wallet/key.
        assert not Path(self.options.tmpdir, "node4").exists(), "Refusing any pre-existing client datadir"
        self.start_ordinary_client()
        market_status = self.market_status(self.nodes[0], market)
        self.funding_address = self.client.getnewaddress()
        funding = self.nodes[0].sendtoaddress(self.funding_address, Decimal("3"))
        self.synchronize_mempools()
        self.mine_pos_blocks(1, allow_overshoot=True)
        self.wait_client_b3_sync()
        assert self.client.gettransaction(funding)["confirmations"] >= 1
        deposit = self.prepare_and_publish_deposit(self.client, market, market_status["base_asset_id"], "B3", Decimal("1"))
        self.publish_client_transaction(deposit)
        self.mine_pos_blocks(30, allow_overshoot=True)
        self.wait_client_b3_sync()
        self.observe_action(market, "submitflowmeshdeposit", (market, deposit["deposit_txid"], deposit["deposit_vout"]),
                            "new_crash_client_funding_deposit")
        self.wait_client_balance(market, lambda row: row["b3_available"] == Decimal("1"))
        self.client.stop_node()
        assert not self.client.running and self.client.process is None
        self.client.args[0] = str(crash_binary)
        self.crash_token = secrets.token_hex(32)
        self.crash_env = {"B3_TEST_CLIENT_CRASH_CHAIN_DIR": str(self.client.chain_path.resolve()),
                          "B3_TEST_CLIENT_CRASH_TOKEN": self.crash_token}
        self.configure_hook("setup")
        self.start_crash_client()
        assert any(event["stage"] == "journal_restore_completed" and event["pid"] == self.client.process.pid
                   for event in self.hook_events()), "Dedicated client binary did not emit the compiled test hook"
        self.assert_engine_off()
        self.operator_pids = [node.process.pid for node in self.nodes]
        self.bootstrap_submit_count = len(self.submissions())
        self.write_evidence("PLAN-AND-INPUTS.json", {
            "cases_preselected": ["A", "B", "C"], "attempts_per_case": 1,
            "ordinary_binary": self.nodes[0].args[0],
            "ordinary_binary_sha256": hashlib.sha256(Path(self.nodes[0].args[0]).read_bytes()).hexdigest(),
            "crash_client_binary": str(crash_binary), "crash_client_binary_sha256": hashlib.sha256(crash_binary.read_bytes()).hexdigest(),
            "operator_pids_never_crashed": self.operator_pids,
            "new_client_datadir": str(self.client.datadir_path.resolve()),
            "market_id": market, "wallet_name": self.default_wallet_name,
            "initial_saved": self.local_saved(market), "hook_events": self.hook_events(),
            "process_crash_only_not_power_loss": True, "no_preserved_fixture_inputs": True})

    def begin_order(self, market, sequence, quantity):
        assert self.crash_thread is None or not self.crash_thread.is_alive()
        self.crash_rpc_result = {}
        rpc = get_rpc_proxy(self.client.url, 4, timeout=90)

        def submit_once():
            try:
                self.crash_rpc_result["response"] = rpc.submitflowmeshorder(market, "bid", self.price_atoms, quantity, sequence)
            except Exception as error:
                # Do not print local RPC authentication from exception URLs.
                self.crash_rpc_result["error_type"] = type(error).__name__

        self.crash_thread = threading.Thread(target=submit_once, name="new-client-crash-submission")
        self.crash_thread.start()

    def kill_owned_client(self, case, stage):
        process = self.client.process
        assert self.client.running and process is not None and process.poll() is None
        assert self.client.index == 4
        assert self.client.datadir_path.resolve() == Path(self.options.tmpdir, "node4").resolve()
        assert self.client.args[0] == self.options.crash_client_binary
        assert process.pid not in self.operator_pids
        assert all(node.process.pid == pid and node.process.poll() is None for node, pid in zip(self.nodes, self.operator_pids))
        assert stage["pid"] == process.pid and stage["token"] == self.crash_token
        self.write_evidence(f"{case}-before-signal.json", {"stage": stage, "relay_capture": self.relay_capture(),
                                                        "hook_events": self.hook_events(), "client_pid": process.pid})
        sent_us = time.monotonic_ns() // 1000
        process.send_signal(signal.SIGKILL)
        actual_exit = process.wait(timeout=15)
        assert_equal(actual_exit, -signal.SIGKILL)
        self.client.wait_until_stopped(expected_ret_code=-signal.SIGKILL)
        if self.crash_thread is not None:
            self.crash_thread.join(timeout=15)
            assert not self.crash_thread.is_alive(), "Submission RPC thread did not finish after owned child exit"
        assert all(node.process.pid == pid and node.process.poll() is None for node, pid in zip(self.nodes, self.operator_pids))
        result = {"case": case, "pid": process.pid, "signal": "SIGKILL", "signal_sent_monotonic_us": sent_us,
                  "actual_child_exit": actual_exit, "submission_rpc": self.crash_rpc_result,
                  "operators_still_same_running_processes": True}
        self.write_evidence(f"{case}-actual-exit.json", result)
        return result

    def reopen_and_compare(self, case, market, action_id, before, sequence):
        self.configure_hook(case + "-restart")
        self.start_crash_client()
        restored = [event for event in self.hook_events() if event["stage"] == "journal_restore_completed"
                    and event["pid"] == self.client.process.pid]
        assert_equal(len(restored), 1)
        # Full retained public journal equality includes exact bytes, all prior
        # instructions, owner/domain/config, uncertainty and head high-water.
        assert_equal(restored[0]["data"], before["data"])
        saved = self.local_saved(market)
        rows = [row for row in saved["actions"] if row["action_id"] == action_id]
        assert_equal(len(rows), 1)
        row = rows[0]
        original = next(row for row in before["data"]["actions"] if row["action_id"] == action_id)
        assert_equal(row["sequence"], sequence)
        for public, durable in (("market_id", "market_id"), ("domain", "domain"),
                                ("execution_config_id", "config"), ("account_id", "owner_account")):
            assert_equal(row[public], original[durable])
        assert_equal(row["signed_bytes_sha256"], hashlib.sha256(bytes.fromhex(original["action_hex"])).hexdigest())
        assert_equal(row["signed_bytes_size"], len(bytes.fromhex(original["action_hex"])))
        for flag in ("may_have_been_sent", "previously_certified", "initial_submission_ms"):
            assert_equal(row[flag], original[flag])
        assert_equal(row["receipt"]["receipt_state"], "unknown")
        assert_equal(row["receipt"]["certificate_verified"], False)
        assert self.client.getaddressinfo(self.funding_address)["ismine"]
        assert_equal(self.client.getwalletinfo()["walletname"], self.default_wallet_name)
        self.write_evidence(f"{case}-reopened.json", {"restored": restored[0], "saved": saved,
                                                     "wallet_accessible": True, "exact_public_journal_equal": True})
        return original

    def certify_original(self, market, action_id, original):
        attempts = []
        retry_at = 0

        def ready():
            nonlocal retry_at
            self.pump_b3()
            status = self.client.getflowmeshactionstatus(market, action_id)
            attempts.append({"kind": "status", "response": status})
            if status["certificate_verified"]:
                assert_equal(status["receipt_state"], "certified_inclusion")
                return True
            if time.monotonic() >= retry_at:
                retry = self.client.retryflowmeshaction(market, action_id)
                assert_equal(retry["action_id"], action_id)
                attempts.append({"kind": "identical_saved_instruction_retry", "response": retry})
                retry_at = time.monotonic() + 1
            return False

        self.wait_until(ready, timeout=60, check_interval=.1)
        submitted = [row for row in self.submissions() if row["action_id"] == action_id]
        assert submitted
        assert all(row["action_hex"] == original["action_hex"] for row in submitted)
        assert len(attempts) <= 700
        return attempts

    def assert_one_order_effect(self, market, before, quantity):
        after = self.wait_client_balance(market, lambda row: row["next_sequence"] == before["next_sequence"] + 1)
        reserve_atoms = self.price_atoms * quantity
        reserve = Decimal(reserve_atoms) / Decimal(1_000_000_000)
        assert_equal(after["b3_available"], before["b3_available"] + before["b3_reserved"] - reserve)
        assert_equal(after["b3_reserved"], reserve)
        assert_equal(after["base_available"], before["base_available"])
        assert_equal(after["base_reserved"], before["base_reserved"])
        assert_equal(len(after["curves"]), 1)
        curve = after["curves"][0]
        assert_equal(curve["account_id"], after["account_id"])
        assert_equal(curve["side"], "bid")
        assert_equal(curve["filled_quantity"], 0)
        assert_equal(curve["remaining_quantity"], quantity)
        assert_equal(curve["reserved_amount"], reserve_atoms)
        assert_equal(curve["points"], [{"price": self.price_atoms, "quantity": quantity},
                                      {"price": self.price_atoms + 1, "quantity": 0}])
        return after

    def exercise_case(self, case, market):
        before_account = self.client_balance(market)
        sequence = before_account["next_sequence"]
        start_submits = len(self.submissions())
        self.configure_hook(case, pause=case == "A")
        if case == "B":
            # Both endpoints hold replies, including any failover. Forwarding
            # and response-attempt timestamps are recorded independently.
            for relay in self.tls_relays:
                relay.configure(response_hold_ms={"submit": 6000})
        self.write_evidence(f"{case}-inputs.json", {"sequence": sequence, "account": before_account,
                                                   "submissions_before": self.submissions(), "hook_events": self.hook_events()})
        # Existing curve semantics replace this account's bid, rather than
        # appending a separate order. Grow from one to two base units so the
        # exact expected reservation delta remains one native atom.
        self.begin_order(market, sequence, {"A": 1, "B": 2}[case])
        holder = {}

        def reached():
            if case == "A":
                rows = [row for row in self.hook_events() if row["stage"] == "durable_new_instruction_before_first_send"
                        and row["pid"] == self.client.process.pid and row["case"] == case]
                if not rows:
                    return False
                holder["stage"] = rows[-1]
                holder["action_id"] = rows[-1]["data"]["action_id"]
                assert_equal(len(self.submissions()), start_submits)
            else:
                rows = self.submissions()[start_submits:]
                if not rows or not any(row["forwarded"] and "response_hold_started_us" in row for row in rows):
                    return False
                assert all("client_response_attempted_us" not in row for row in rows), rows
                holder["action_id"] = rows[0]["action_id"]
                assert all(row["action_id"] == holder["action_id"] for row in rows)
                holder["stage"] = {"pid": self.client.process.pid, "token": self.crash_token,
                                   "stage": "upstream_forwarded_no_client_response", "requests": rows}
            return True

        self.wait_until(reached, timeout=30, check_interval=.01)
        action_id = holder["action_id"]
        saved = self.latest_saved_journal(action_id)
        original = next(row for row in saved["data"]["actions"] if row["action_id"] == action_id)
        assert_equal(original["may_have_been_sent"], case == "B")
        assert_equal(original["previously_certified"], False)
        result = self.kill_owned_client(case, holder["stage"])
        # The kill must precede every response attempt, not merely our earlier
        # observation of a hold. Keep holds active until this comparison.
        if case == "B":
            for row in self.submissions()[start_submits:]:
                assert "client_response_attempted_us" not in row or row["client_response_attempted_us"] > result["signal_sent_monotonic_us"]
        for relay in self.tls_relays:
            relay.configure(receipt_expired_action_id=action_id if case == "B" else None)
        original = self.reopen_and_compare(case, market, action_id, saved, sequence)
        if case == "B":
            # A genuine lost submit response is followed by a deliberately
            # absent status: prove the permitted retry is the original bytes.
            unknown = self.client.getflowmeshactionstatus(market, action_id)
            assert_equal(unknown["receipt_state"], "unknown")
            exact_retry = self.client.retryflowmeshaction(market, action_id)
            assert_equal(exact_retry["action_id"], action_id)
            result["unknown_status"] = unknown
            result["identical_retry_response"] = exact_retry
        for relay in self.tls_relays:
            relay.configure()
        result.update(action_id=action_id, sequence=sequence, exact_signed_payload=original["action_hex"],
                      recovery=self.certify_original(market, action_id, original),
                      account_before=before_account, account_after=self.assert_one_order_effect(market, before_account, {"A": 1, "B": 2}[case]))
        now_saved = self.local_saved(market)
        assert_equal(len([row for row in now_saved["actions"] if row.get("sequence") == sequence]), 1)
        result["saved_after_recovery"] = now_saved
        self.crash_results.append(result)
        self.write_evidence(f"{case}-result.json", result)

    def exercise_certified_case(self, market):
        case = "C"
        before_account = self.client_balance(market)
        sequence = before_account["next_sequence"]
        self.configure_hook(case)
        self.write_evidence("C-inputs.json", {"sequence": sequence, "account": before_account,
                                              "submissions_before": self.submissions(), "hook_events": self.hook_events()})
        sample = self.observe_action(market, "submitflowmeshorder", (market, "bid", self.price_atoms, 3, sequence),
                                     "C_certified_before_process_crash", expect_sequence=sequence)
        action_id = sample["action_id"]
        account_after = self.assert_one_order_effect(market, before_account, 3)
        status = self.client.getflowmeshactionstatus(market, action_id)
        assert_equal(status["certificate_verified"], True)
        saved = self.latest_saved_journal(action_id)
        assert next(row for row in saved["data"]["actions"] if row["action_id"] == action_id)["previously_certified"]
        self.crash_rpc_result = {"completed_status": status}
        result = self.kill_owned_client(case, saved)
        for relay in self.tls_relays:
            relay.configure(receipt_expired_action_id=action_id)
        original = self.reopen_and_compare(case, market, action_id, saved, sequence)
        submit_count = len(self.submissions())
        unknown = self.client.getflowmeshactionstatus(market, action_id)
        assert_equal(unknown["receipt_state"], "unknown")
        retry = self.client.retryflowmeshaction(market, action_id)
        assert_equal(retry["action_id"], action_id)
        assert_equal(retry["certificate_verified"], False)
        assert "not be resubmitted" in retry["reason"]
        assert_equal(len(self.submissions()), submit_count)
        for relay in self.tls_relays:
            relay.configure()
        fresh = self.client.getflowmeshactionstatus(market, action_id)
        assert_equal(fresh["certificate_verified"], True)
        assert_equal(len(self.submissions()), submit_count)
        reopened_account = self.client_balance(market)
        for field in ("next_sequence", "b3_available", "b3_reserved", "base_available", "base_reserved"):
            assert_equal(reopened_account[field], account_after[field])
        result.update(action_id=action_id, sequence=sequence, exact_signed_payload=original["action_hex"],
                      unknown_after_restart=unknown, explicit_retry_without_resubmit=retry,
                      fresh_proof=fresh, submit_delta_after_restart=0,
                      account_before=before_account, account_after=reopened_account)
        self.crash_results.append(result)
        self.write_evidence("C-result.json", result)

    def qualification_workload(self, market_id):
        self.last_market = market_id
        self.crash_evidence = Path(self.options.tmpdir, "client-process-crash")
        self.crash_evidence.mkdir(exist_ok=False)
        success = False
        try:
            self.initialize_crash_client(market_id)
            for case in ("A", "B"):
                self.exercise_case(case, market_id)
            self.exercise_certified_case(market_id)
            assert_equal([row["case"] for row in self.crash_results], ["A", "B", "C"])
            expected = {row["action_id"]: row["exact_signed_payload"] for row in self.crash_results}
            for row in self.submissions()[self.bootstrap_submit_count:]:
                assert row["action_id"] in expected, "A replacement or unexpected instruction was submitted"
                assert_equal(row["action_hex"], expected[row["action_id"]])
            retained = self.local_saved(market_id)
            sequenced = [row for row in retained["actions"] if "sequence" in row]
            assert_equal({row["action_id"] for row in sequenced}, set(expected))
            assert_equal(len(sequenced), 3)
            assert len({row["sequence"] for row in sequenced}) == 3
            self.assert_engine_off()
            success = True
        finally:
            # Normal inherited cleanup shuts down this disposable fixture.
            # Preserve every captured failure, with no automatic second run.
            for relay in self.tls_relays:
                relay.configure()
            self.write_evidence("RESULTS.json", {"success": success, "cases": self.crash_results,
                "requests": self.relay_capture(), "hook_events": self.hook_events() if self.crash_token else [],
                "chosen_attempts_per_case": 1, "Qt_exercised": False, "power_loss_qualified": False,
                "scope": "new generated client process crashes only; operators not crash targets"})


if __name__ == "__main__":
    FlowMeshClientCrashTest(__file__).main()
