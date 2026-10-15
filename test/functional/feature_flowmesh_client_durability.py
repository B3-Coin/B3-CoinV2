#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""One bounded new-client save-failure / delayed-lane qualification.

Uses the existing separately compiled client crash-hook daemon and generated
regtest fixture. No fault hooks exist in ordinary binaries. This tests an
injected failure BEFORE a journal write, not an uncertain write or power loss.
There is one original signed order; recovery never submits or signs another.
"""

import hashlib
import http.client
import json
import ssl
import threading
import time
from pathlib import Path

from feature_flowmesh_client_crash import FlowMeshClientCrashTest
from feature_flowmesh_latency import FlowMeshLatencyTest
from test_framework.authproxy import JSONRPCException
from test_framework.test_framework import TestStatus
from test_framework.util import assert_equal, get_rpc_proxy


class FlowMeshClientDurabilityTest(FlowMeshClientCrashTest):
    def run_test(self):
        # Reuse only the generated PoS/market/genesis-checkpoint bootstrap,
        # not the unrelated full release, withdrawal and reindex campaign.
        # The helper's one configuration seam below explicitly keeps this
        # fixture's original legacy agreement profile unchanged.
        market_id, _ = FlowMeshLatencyTest.bootstrap_latency_market(self)
        self.wait_for_independent_mesh()
        self.wait_for_market_convergence(market_id)
        self.qualification_workload(market_id)
        self.assert_no_b3_flowmesh_traffic()

    def configure_fresh_market(self, market_id):
        assert self.last_market is None and not self.pos_running
        assert len(market_id) == 64
        assert all(not arg.startswith("-flowmeshpreagreementmarket=")
                   for args in self.extra_args for arg in args)
        assert all(not arg.startswith("-flowmeshpreagreementmarket=")
                   for node in self.nodes for arg in node.extra_args)
        # Deliberate no-op: no protocol flag, store, journal or signer change.
        # bootstrap_latency_market pins this fresh market before mining A2.

    def set_test_params(self):
        super().set_test_params()
        self.crash_cases = ["D"]
        self.durability_children = []

    def remember_durability_children(self):
        known = {id(row["process"]) for row in self.durability_children}
        for node in [*self.nodes, *([self.client] if self.client else [])]:
            process = node.process
            if process is None or id(process) in known:
                continue
            assert self.chain == "regtest" and 0 <= node.index <= 4
            assert node.datadir_path.resolve() == Path(self.options.tmpdir, f"node{node.index}").resolve()
            assert f"-datadir={node.datadir_path}" in process.args
            self.durability_children.append({"node": node.index, "process": process, "expected_exit": 0})
            known.add(id(process))

    def start_nodes(self, *args, **kwargs):
        try:
            return super().start_nodes(*args, **kwargs)
        finally:
            self.remember_durability_children()

    def start_ordinary_client(self):
        try:
            return super().start_ordinary_client()
        finally:
            self.remember_durability_children()

    def start_crash_client(self):
        try:
            return super().start_crash_client()
        finally:
            self.remember_durability_children()

    def kill_owned_client(self, *args, **kwargs):
        process = self.client.process
        result = super().kill_owned_client(*args, **kwargs)
        # Only the existing checked, intentional crash permits a nonzero
        # expected exit. Cleanup termination can never earn this exception.
        record = next(row for row in self.durability_children if row["process"] is process)
        record["expected_exit"] = result["actual_child_exit"]
        return result

    def begin_lane_order(self, market, sequence):
        assert self.crash_thread is None or not self.crash_thread.is_alive()
        self.crash_rpc_result = {}
        rpc = get_rpc_proxy(self.client.url, 4, timeout=45)

        def submit_once():
            try:
                self.crash_rpc_result["response"] = rpc.submitflowmeshorder(
                    market, "bid", self.price_atoms, 1, sequence, 2000)
            except Exception as error:
                # Never serialize local RPC authentication from exception URLs.
                self.crash_rpc_result["error_type"] = type(error).__name__

        self.crash_thread = threading.Thread(target=submit_once, name="client-durability-delayed-lane")
        self.crash_thread.start()

    def save_events(self):
        return [row for row in self.hook_events()
                if row["pid"] == self.client.process.pid and row["stage"] == "synchronous_journal_save_returned"]

    def operator_action_status(self, market, action_id):
        # Public, read-only operator API: the operator's wallet does not own
        # this client's action. Do not query the client here either: that would
        # verify and persist the inclusion before the targeted save fault.
        connection = http.client.HTTPSConnection("127.0.0.1", self.api_ports[0], timeout=5,
            context=ssl.create_default_context(cafile=str(self.pki["ca"])))
        try:
            connection.request("POST", "/flowmesh/v1", body=json.dumps({"method": "action",
                "params": {"market_id": market, "action_id": action_id}}),
                headers={"Content-Type": "application/json"})
            reply = connection.getresponse()
            body = reply.read(5 * 1024 * 1024 + 1)
            assert_equal(reply.status, 200)
            assert len(body) <= 5 * 1024 * 1024, "Operator action response exceeded bound"
            decoded = json.loads(body)
            assert_equal(decoded["ok"], True)
            assert_equal(decoded["result"]["action_id"], action_id)
            return decoded["result"]
        finally:
            connection.close()

    def qualification_workload(self, market_id):
        self.last_market = market_id
        self.crash_evidence = Path(self.options.tmpdir, "client-verified-return-durability")
        self.crash_evidence.mkdir(exist_ok=False)
        success = False
        result = {"attempts": 1, "fault": "injected_before_journal_write",
                  "bootstrap_profile": "generated-legacy-minimal-genesis-v1",
                  "full_release_reindex_qualification": False,
                  "power_loss_qualified": False, "unknown_write_qualified": False}
        try:
            self.initialize_crash_client(market_id)
            self.end_b3_workload()
            self.sync_blocks(timeout=120)
            self.wait_client_b3_sync()
            before_account = self.client_balance(market_id)
            sequence = before_account["next_sequence"]
            start_submits = len(self.submissions())
            self.configure_hook("D-delayed-lane", hold_lane=True)
            self.begin_lane_order(market_id, sequence)
            holder = {}

            def lane_held():
                rows = [row for row in self.hook_events()
                        if row["pid"] == self.client.process.pid and row["stage"] == "lane_reply_before_apply_held"]
                if not rows:
                    return False
                holder["event"] = rows[-1]
                return True

            self.wait_until(lane_held, timeout=15, check_interval=.01)
            action_id = holder["event"]["data"]["action_id"]
            assert_equal(len(self.submissions()) - start_submits, 1)
            saved_before = self.latest_saved_journal(action_id)
            original = next(row for row in saved_before["data"]["actions"] if row["action_id"] == action_id)
            assert original["may_have_been_sent"] and not original["previously_certified"]
            # Require the operator's certified receipt and durable payload
            # before asking the client for a snapshot. This precondition is
            # not independent client verification; that actual verification
            # and its failing journal save are exercised immediately below.
            # No retry or new economic action obtains this state.
            def operator_has_certificate():
                receipt = self.operator_action_status(market_id, action_id)
                return (receipt["certificate_verified"] and receipt["receipt_state"] == "certified_inclusion"
                        and bool(receipt.get("certified_payload")) and "evidence_error" not in receipt)

            self.wait_until(operator_has_certificate, timeout=10, check_interval=.1)
            self.configure_hook("D-save-failure", fail_inclusion=action_id, hold_lane=True)
            snapshot_error = None
            try:
                self.client.getflowmeshmarketdata(market_id, {"limit": 1, "curve_limit": 2})
            except JSONRPCException as error:
                snapshot_error = error.error
            assert snapshot_error is not None, "New inclusion unexpectedly survived the injected save failure"
            failures = [row for row in self.hook_events()
                        if row["pid"] == self.client.process.pid and row["stage"] == "new_inclusion_save_failed_before_write"]
            assert failures, "The actual new-inclusion journal save path was not exercised"
            assert not next(row for row in self.latest_saved_journal(action_id)["data"]["actions"]
                            if row["action_id"] == action_id)["previously_certified"]
            self.write_evidence("D-before-lane-release.json", {
                "held_lane": holder["event"], "snapshot_error": snapshot_error,
                "failed_saves": failures, "last_durable": self.latest_saved_journal(action_id),
                "saved_memory": self.local_saved(market_id), "requests": self.relay_capture()})
            # The original implementation returns its unsaved in-memory
            # verified inclusion through ApplyLaneResult's early return here.
            self.configure_hook("D-save-failure", fail_inclusion=action_id, hold_lane=True, release_lane=True)
            self.crash_thread.join(timeout=15)
            assert not self.crash_thread.is_alive(), "The held lane failed to finish within its bound"
            result["delayed_lane_response"] = dict(self.crash_rpc_result)
            assert "response" in self.crash_rpc_result, self.crash_rpc_result
            response = self.crash_rpc_result["response"]
            assert_equal(response["action_id"], action_id)
            assert_equal(response["certificate_verified"], False)
            assert_equal(response["outcome_verified"], False)
            assert_equal(len(self.submissions()) - start_submits, 1)
            # A second read still cannot promote an unsaved marker. Its
            # previous certification remains binding in memory and exact
            # original bytes remain retained, even though labels withdraw.
            failed_status = self.client.getflowmeshactionstatus(market_id, action_id)
            assert_equal(failed_status["certificate_verified"], False)
            retained = self.local_saved(market_id)
            row = next(row for row in retained["actions"] if row["action_id"] == action_id)
            assert row["previously_certified"] and row["may_have_been_sent"]
            assert_equal(row["signed_bytes_sha256"], original_sha(original))
            assert_equal(len(self.submissions()) - start_submits, 1)
            self.configure_hook("D-save-recovered")
            recovered = {}

            def verified_after_save():
                status = self.client.getflowmeshactionstatus(market_id, action_id)
                recovered["status"] = status
                return status["certificate_verified"]

            self.wait_until(verified_after_save, timeout=10, check_interval=.1)
            saved = self.latest_saved_journal(action_id)
            durable_original = next(row for row in saved["data"]["actions"] if row["action_id"] == action_id)
            assert durable_original["previously_certified"]
            assert_equal(durable_original["action_hex"], original["action_hex"])
            assert_equal(len(self.submissions()) - start_submits, 1)
            after_account = self.assert_one_order_effect(market_id, before_account, 1)
            # A real owned process exit/reopen verifies the durable marker,
            # while labels remain unknown until fresh independent evidence.
            exit_result = self.kill_owned_client("D", saved)
            for relay in self.tls_relays:
                relay.configure(receipt_expired_action_id=action_id)
            self.reopen_and_compare("D", market_id, action_id, saved, sequence)
            submit_count = len(self.submissions())
            retry = self.client.retryflowmeshaction(market_id, action_id)
            assert_equal(retry["certificate_verified"], False)
            assert "not be resubmitted" in retry["reason"]
            assert_equal(len(self.submissions()), submit_count)

            # Pure outcome upgrades change no restart-relevant marker and
            # must not add a synchronous write merely to persist receipt text.
            def without_outcome(method, body):
                parsed = json.loads(body)
                if method == "action" and parsed.get("ok"):
                    parsed["result"].pop("outcome_evidence", None)
                    parsed["result"]["outcome_evidence_error"] = "BUDGET"
                return json.dumps(parsed, separators=(",", ":")).encode()

            for relay in self.tls_relays:
                relay.configure(reply_mutation=without_outcome)
            self.client.listflowmeshmarkets() # Learn outcome capability, without a snapshot.
            self.wait_until(verified_after_save, timeout=10, check_interval=.1)
            assert_equal(recovered["status"]["outcome_verified"], False)
            saves_before_upgrade = len(self.save_events())
            time.sleep(1.1) # Leave the bounded automatic status-attempt window.
            upgraded = self.client.getflowmeshactionstatus(market_id, action_id)
            assert upgraded["certificate_verified"] and upgraded["outcome_verified"]
            assert_equal(len(self.save_events()), saves_before_upgrade)
            assert_equal(len(self.submissions()), submit_count)
            result.update(action_id=action_id, original=original, snapshot_error=snapshot_error,
                          failed_status=failed_status, recovered=recovered["status"], saved=saved,
                          actual_child_exit=exit_result, exact_retry_no_resubmit=retry,
                          outcome_only_upgrade=upgraded, outcome_upgrade_sync_write_delta=0,
                          before_account=before_account, after_account=after_account)
            success = True
        finally:
            if self.crash_token:
                # Release only this generated client's test barrier. Never
                # retry an economic operation merely to finish a failed test.
                self.configure_hook("D-cleanup", hold_lane=True, release_lane=True)
            if self.crash_thread is not None:
                self.crash_thread.join(timeout=35)
                result["submission_thread_stopped"] = not self.crash_thread.is_alive()
            for relay in self.tls_relays:
                relay.configure()
            self.write_evidence("RESULTS.json", {"success": success, "result": result,
                "requests": self.relay_capture(), "hook_events": self.hook_events() if self.crash_token else [],
                "scope": "one generated client, actual backend save failure and delayed lane, no live data"})
            assert self.crash_thread is None or not self.crash_thread.is_alive(), "Owned submission thread did not stop"

    def shutdown(self):
        self.options.nocleanup = True
        errors = []
        code = 1

        def attempt(node, phase, operation):
            try:
                operation()
                return True
            except Exception as error:
                # Do not serialize RPC URLs or authentication in exceptions.
                errors.append({"node": node.index, "phase": phase, "error_type": type(error).__name__})
                return False

        try:
            self.remember_durability_children()
            nodes = [*self.nodes, *([self.client] if self.client else [])]
            # Unlike the ordinary failed-test policy, this bounded campaign
            # stops ALL its generated children even on its expected failure.
            # Request every graceful stop before waiting for any one child.
            for node in nodes:
                if node.running and node.rpc_connected:
                    attempt(node, "stop", lambda: node.stop_node(wait_until_stopped=False))
            for node in nodes:
                process = node.process
                if process is None:
                    continue
                if attempt(node, "wait", lambda: node.wait_until_stopped(timeout=30)):
                    continue
                # Direct retained Popen identity only: never find a process by
                # name, PID search, open port or a reused test directory.
                if process.poll() is None:
                    attempt(node, "terminate", process.terminate)
                    if not attempt(node, "terminate_wait", lambda: process.wait(timeout=5)):
                        attempt(node, "kill", process.kill)
                        attempt(node, "kill_wait", lambda: process.wait(timeout=5))
                for name in ("stdout", "stderr"):
                    stream = getattr(node, name)
                    if stream is not None:
                        attempt(node, "close_" + name, stream.close)
                node.running = False
                node.rpc_connected = False
                node.process = None
            if errors:
                self.success = TestStatus.FAILED
            code = super().shutdown()
        except BaseException as error:
            self.success = TestStatus.FAILED
            errors.append({"node": None, "phase": "shutdown", "error_type": type(error).__name__})
            raise
        finally:
            rows = []
            for record in self.durability_children:
                process = record["process"]
                exit_code = process.poll()
                rows.append({"node": record["node"], "pid": process.pid,
                             "expected_exit": record["expected_exit"], "actual_exit": exit_code})
            clean = bool(rows) and not errors and all(
                row["actual_exit"] == row["expected_exit"] for row in rows)
            if not clean:
                code = 1
            Path(self.options.tmpdir, "client-durability-child-exits.json").write_text(
                json.dumps({"format_version": 1, "children": rows, "cleanup_errors": errors,
                            "all_owned_children_exited_as_expected": clean,
                            "qualification_pass": self.success == TestStatus.PASSED,
                            "harness_exit_code": code}, sort_keys=True, indent=2) + "\n", encoding="utf-8")
        return code


def original_sha(row):
    return hashlib.sha256(bytes.fromhex(row["action_hex"])).hexdigest()


if __name__ == "__main__":
    FlowMeshClientDurabilityTest(__file__).main()
