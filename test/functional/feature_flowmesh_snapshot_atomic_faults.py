#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""One preselected aggregate-snapshot fault phase on NEW generated regtest.

Use a fresh harness directory for EACH required --snapshot-atomic-phase.
This preserves the unchanged r5 bootstrap/owned-child cleanup and the frozen
v1 two-to-one count fixture. No ordinary daemon contains these test hooks.
The committed throw/hold are AFTER a successful actual SyncWrite, not a
simulation of an unknown physical write, corrupt storage or power loss.
"""

import copy
import json
import threading
import time
from pathlib import Path

from feature_flowmesh_client_durability import FlowMeshClientDurabilityTest, original_sha
from feature_flowmesh_snapshot_atomic import action_row, market_head, restart_fields, restart_sha
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, get_rpc_proxy


class FlowMeshSnapshotAtomicFaultTest(FlowMeshClientDurabilityTest):
    def add_options(self, parser):
        super().add_options(parser)
        parser.add_argument("--snapshot-atomic-phase", required=True,
                            choices=("prewrite", "committed-throw", "committed-kill", "outcome-only"),
                            help="One bounded phase, selected before launch; use a NEW harness directory per phase")

    def set_test_params(self):
        super().set_test_params()
        self.snapshot_thread = None
        self.snapshot_rpc_result = {}

    def run_test(self):
        super().run_test()

    def start_crash_client(self):
        if self.options.snapshot_atomic_phase != "committed-kill":
            return super().start_crash_client()
        # Two intentionally held RPCs consume the inherited two dispatcher
        # workers. The independent leaf-view observer needs its own worker;
        # this is an explicit TEST profile, not a production capacity repair.
        previous_args = self.client_args
        self.client_args = [*previous_args, "-rpcthreads=3"]
        try:
            return super().start_crash_client()
        finally:
            self.client_args = previous_args

    def probe_saved_view(self, market):
        # An observer timeout must not poison the primary cleanup connection.
        rpc = get_rpc_proxy(self.client.url, 4, timeout=2)
        counts = [len(row["requests"]) for row in self.relay_capture()]
        result = rpc.listflowmeshactions(market)
        assert_equal(result["source"], "local-retained-outbox")
        assert_equal([len(row["requests"]) for row in self.relay_capture()], counts)
        return result

    def configure_atomic_hook(self, case, market, action_id, mode, *, release_lane=False):
        assert all(len(value) == 64 and all(c in "0123456789abcdef" for c in value)
                   for value in (market, action_id))
        marker = {"token": self.crash_token, "case": case,
                  "chain_dir": str(self.client.chain_path.resolve()), "fresh_generated_client": True,
                  "pause_before_first_send": False, "hold_lane_before_apply": True,
                  "release_lane_before_apply": release_lane}
        if mode == "prewrite":
            marker["fail_new_inclusion_save_for"] = action_id
        elif mode in ("committed-throw", "committed-kill"):
            marker.update(after_sync_fault_action_id=action_id, after_sync_fault_market_id=market,
                          after_sync_fault_mode="throw" if mode == "committed-throw" else "hold")
        # Fault and release fields MUST become visible in one replacement:
        # never briefly release the old lane with its fault disarmed.
        marker_path = self.client.datadir_path / "flowmesh-client-crash-guard.json"
        temporary = marker_path.with_suffix(".next")
        temporary.write_text(json.dumps(marker), encoding="utf-8")
        temporary.replace(marker_path)

    def phase_events(self, stage, case=None):
        return [row for row in self.hook_events() if row["pid"] == self.client.process.pid
                and row["stage"] == stage and (case is None or row["case"] == case)]

    def begin_snapshot(self, market):
        assert self.snapshot_thread is None
        rpc = get_rpc_proxy(self.client.url, 4, timeout=45)

        def read_once():
            try:
                self.snapshot_rpc_result["response"] = rpc.getflowmeshmarketdata(market, {"limit": 1, "curve_limit": 2})
            except Exception as error:
                self.snapshot_rpc_result["error_type"] = type(error).__name__

        self.snapshot_thread = threading.Thread(target=read_once, name="snapshot-atomic-post-sync-hold")
        self.snapshot_thread.start()

    def assert_unknown(self, receipt):
        assert_equal(receipt["certificate_verified"], False)
        assert_equal(receipt["outcome_verified"], False)

    def assert_complete_transition(self, before, after, market, action_id):
        old_head = market_head(before["data"], market)
        new_head = market_head(after["data"], market)
        assert new_head["sequence"] > old_head["sequence"], "Target snapshot did not advance the retained head"
        original = action_row(before["data"], action_id)
        retained = action_row(after["data"], action_id)
        assert retained["previously_certified"] and retained["may_have_been_sent"]
        assert original["action_hex"] == retained["action_hex"], "Exact original bytes changed"
        expected = copy.deepcopy(before["data"])
        action_row(expected, action_id)["previously_certified"] = True
        market_head(expected, market).update(new_head)
        assert restart_fields(after["data"]) == restart_fields(expected), "Unexpected restart-relevant journal change"
        return {"old_head": old_head, "new_head": new_head, "exact_original_sha256": original_sha(original),
                "final_restart_fields_sha256": restart_sha(after["data"]),
                "all_restart_fields_equal_expected": True, "exact_original_bytes_unchanged": True}

    def assert_no_replay_with_expired_receipt(self, market, action_id, count, *, require_unknown=True):
        for relay in self.tls_relays:
            relay.configure(receipt_expired_action_id=action_id)
        retry = self.client.retryflowmeshaction(market, action_id)
        assert_equal(retry["action_id"], action_id)
        if require_unknown:
            self.assert_unknown(retry)
            assert "not be resubmitted" in retry["reason"]
        elif retry["certificate_verified"]:
            # Endpoint expiry does not revoke this process's independently
            # verified, still-current cached inclusion.
            assert_equal(retry["receipt_state"], "certified_inclusion")
        else:
            self.assert_unknown(retry)
            assert "not be resubmitted" in retry["reason"]
        retained = action_row(self.local_saved(market), action_id)
        assert_equal(retained["market_id"], market)
        assert retained["previously_certified"] and retained["may_have_been_sent"]
        assert_equal(len(self.submissions()), count)
        return retry

    def fresh_verified_status(self, market, action_id):
        holder = {}

        def verified():
            holder["status"] = self.client.getflowmeshactionstatus(market, action_id)
            return holder["status"]["certificate_verified"]

        self.wait_until(verified, timeout=10, check_interval=.1)
        return holder["status"]

    def qualification_workload(self, market_id):
        phase = self.options.snapshot_atomic_phase
        self.crash_cases = ["snapshot-atomic-v2-" + phase]
        self.last_market = market_id
        self.crash_evidence = Path(self.options.tmpdir, "client-snapshot-atomic-v2-" + phase)
        self.crash_evidence.mkdir(exist_ok=False)
        success = False
        result = {"format_version": 2, "profile": "generated-regtest-snapshot-atomic-faults-v2",
                  "phase": phase, "attempts": 1, "bootstrap_profile": "generated-legacy-minimal-genesis-v1",
                  "full_release_reindex_qualification": False, "power_loss_qualified": False,
                  "unknown_physical_write_qualified": False, "corrupt_store_recovery_qualified": False,
                  "join_failure_fence_evidence": "source review; no extra instruction signed to probe a join"}
        try:
            self.initialize_crash_client(market_id)
            self.end_b3_workload()
            self.sync_blocks(timeout=120)
            self.wait_client_b3_sync()
            before_account = self.client_balance(market_id)
            sequence = before_account["next_sequence"]
            start_submits = len(self.submissions())
            self.configure_hook("atomic-v2-original-held", hold_lane=True)
            self.begin_lane_order(market_id, sequence)
            holder = {}

            def lane_held():
                rows = self.phase_events("lane_reply_before_apply_held", "atomic-v2-original-held")
                if not rows:
                    return False
                holder["event"] = rows[-1]
                return True

            self.wait_until(lane_held, timeout=15, check_interval=.01)
            action_id = holder["event"]["data"]["action_id"]
            saved_before = self.latest_saved_journal(action_id)
            original = action_row(saved_before["data"], action_id)
            assert original["may_have_been_sent"] and not original["previously_certified"]
            assert_equal(original["market_id"], market_id)
            assert_equal(len(self.submissions()) - start_submits, 1)
            result.update(market_id=market_id, action_id=action_id)

            def operator_has_certificate():
                receipt = self.operator_action_status(market_id, action_id)
                return (receipt["certificate_verified"] and receipt["receipt_state"] == "certified_inclusion"
                        and bool(receipt.get("certified_payload")) and "evidence_error" not in receipt)

            self.wait_until(operator_has_certificate, timeout=10, check_interval=.1)
            case = "atomic-v2-target-" + phase
            self.configure_atomic_hook(case, market_id, action_id, phase)
            request_counts = [len(row["requests"]) for row in self.relay_capture()]
            target = None
            snapshot_error = None
            if phase == "committed-kill":
                # A prior completed retention step may already expose this
                # original as unknown. The held transition must preserve the
                # exact prior leaf view, not assume the original was absent.
                saved_view_before_snapshot = self.probe_saved_view(market_id)
                for row in saved_view_before_snapshot["actions"]:
                    if row["action_id"] == action_id:
                        assert_equal(row["previously_certified"], False)
                        self.assert_unknown(row["receipt"])
                self.begin_snapshot(market_id)

                def sync_held():
                    rows = self.phase_events("committed_journal_before_digest_held", case)
                    if not rows:
                        return False
                    holder["committed"] = rows[-1]
                    return True

                self.wait_until(sync_held, timeout=15, check_interval=.01)
                target = holder["committed"]
                assert self.snapshot_thread.is_alive(), "Post-sync hold unexpectedly published/returned"
                assert_equal(self.snapshot_rpc_result, {})
                # Leaf reads must not expose ANY part of this held verified
                # transition, nor wait for its work gate to be released.
                assert_equal(self.probe_saved_view(market_id), saved_view_before_snapshot)
                assert self.snapshot_thread.is_alive(), "Post-sync hold ended during the leaf-view probe"
                assert_equal(self.snapshot_rpc_result, {})
                result["held_saved_view_equals_pre_snapshot"] = True
                result["test_only_rpc_workers_for_two_holds_and_leaf_observer"] = 3
            elif phase in ("prewrite", "committed-throw"):
                try:
                    self.client.getflowmeshmarketdata(market_id, {"limit": 1, "curve_limit": 2})
                except JSONRPCException as error:
                    snapshot_error = error.error
                assert snapshot_error is not None, "Injected snapshot fault unexpectedly returned verified state"
                stage = ("new_inclusion_save_failed_before_write" if phase == "prewrite"
                         else "committed_journal_test_throw_before_digest")
                rows = self.phase_events(stage, case)
                assert_equal(len(rows), 1)
                target = rows[0]
            else:
                snapshot = self.client.getflowmeshmarketdata(market_id, {"limit": 1, "curve_limit": 2})
                assert_equal(snapshot["verification"]["certificate_verified"], True)
                rows = self.phase_events("synchronous_journal_save_returned", case)
                assert_equal(len(rows), 1)
                target = rows[0]
            requests = [request for relay, count in zip(self.relay_capture(), request_counts)
                        for request in relay["requests"][count:]]
            assert_equal(sum(request["method"] == "snapshot" for request in requests), 1)
            result.update(self.assert_complete_transition(saved_before, target, market_id, action_id))
            result.update(snapshot_error=snapshot_error, targeted_snapshot_requests=1,
                          actual_target_sync_returns=0 if phase == "prewrite" else 1,
                          target_successful_digest_records=len(self.phase_events("synchronous_journal_save_returned", case)))
            assert_equal(result["target_successful_digest_records"], 1 if phase == "outcome-only" else 0)
            if phase == "prewrite":
                # Removing the old head-only crash prefix is intentional:
                # nothing new was written, so death may restore the OLD blob.
                assert self.latest_saved_journal(action_id)["data"] == saved_before["data"], "Prewrite failure persisted a partial head"
                restart_expected = saved_before
            else:
                restart_expected = target
            self.write_evidence("TARGET-BOUNDARY.json", {"result": result, "target": target,
                "before": saved_before, "requests": requests, "held_lane": holder["event"]})

            if phase != "committed-kill":
                # Keep the fault active while the OLD lane returns, so it
                # cannot launder an unsaved/uncertain verified label.
                lane_before = len(self.save_events())
                self.configure_atomic_hook("atomic-v2-delayed-" + phase, market_id, action_id, phase, release_lane=True)
                self.crash_thread.join(timeout=15)
                assert not self.crash_thread.is_alive(), "Held original lane did not stop"
                assert "response" in self.crash_rpc_result, self.crash_rpc_result.get("error_type", "No lane response")
                response = self.crash_rpc_result["response"]
                assert_equal(response["action_id"], action_id)
                if phase == "outcome-only":
                    assert_equal(response["certificate_verified"], True)
                    assert_equal(len(self.save_events()) - lane_before, 0)
                else:
                    self.assert_unknown(response)
                    self.assert_unknown(self.client.getflowmeshactionstatus(market_id, action_id))
                memory = action_row(self.local_saved(market_id), action_id)
                assert memory["previously_certified"] and memory["may_have_been_sent"]
                assert_equal(memory["signed_bytes_sha256"], original_sha(original))
                assert_equal(len(self.submissions()) - start_submits, 1)
                result.update(delayed_lane_response=response, memory_no_resubmit_marker_binding=True)
                if phase in ("prewrite", "committed-throw"):
                    result["live_error_expired_retry"] = self.assert_no_replay_with_expired_receipt(
                        market_id, action_id, start_submits + 1)
                if phase == "committed-throw":
                    # Subsequent actual writes also throw after committing;
                    # compare restart with the LAST actual committed blob.
                    restart_expected = self.phase_events("committed_journal_test_throw_before_digest")[-1]
                    self.assert_complete_transition(saved_before, restart_expected, market_id, action_id)
                elif phase == "outcome-only":
                    restart_expected = self.latest_saved_journal(action_id)

            # Kill only exact retained client4 Popen identity. Both committed
            # phases restore the new full blob; prewrite restores the old
            # full blob and makes NO post-crash new-marker promise.
            result["actual_child_exit"] = self.kill_owned_client("atomic-v2-" + phase, restart_expected)
            if self.snapshot_thread is not None:
                self.snapshot_thread.join(timeout=15)
                assert not self.snapshot_thread.is_alive(), "Snapshot RPC thread survived its owned child"
                assert "response" not in self.snapshot_rpc_result
                assert "error_type" in self.snapshot_rpc_result
                result["snapshot_rpc_after_kill"] = dict(self.snapshot_rpc_result)
            self.reopen_and_compare("atomic-v2-" + phase, market_id, action_id, restart_expected, sequence)
            restored = action_row(self.local_saved(market_id), action_id)
            self.assert_unknown(restored["receipt"])
            assert_equal(restored["previously_certified"], phase != "prewrite")
            result["restart_exact_old_or_new_whole_blob"] = "old" if phase == "prewrite" else "new"
            if phase != "prewrite":
                result["restart_expired_retry"] = self.assert_no_replay_with_expired_receipt(
                    market_id, action_id, start_submits + 1)
            for relay in self.tls_relays:
                relay.configure()

            if phase == "outcome-only":
                # Relearn inclusion without an outcome; the distinct
                # outcome_evidence method is not modified by this relay seam.
                def without_outcome(method, body):
                    parsed = json.loads(body)
                    if method == "action" and parsed.get("ok") and parsed["result"].get("action_id") == action_id:
                        parsed["result"].pop("outcome_evidence", None)
                        parsed["result"]["outcome_evidence_error"] = "BUDGET"
                    return json.dumps(parsed, separators=(",", ":")).encode()

                for relay in self.tls_relays:
                    relay.configure(reply_mutation=without_outcome)
                self.client.listflowmeshmarkets()
                inclusion = self.fresh_verified_status(market_id, action_id)
                assert_equal(inclusion["outcome_verified"], False)
                saves_before = len(self.save_events())
                counts_before = [len(row["requests"]) for row in self.relay_capture()]
                time.sleep(1.1) # Same r5 bounded automatic-attempt window.
                upgraded = self.client.getflowmeshactionstatus(market_id, action_id)
                assert upgraded["certificate_verified"] and upgraded["outcome_verified"]
                assert_equal(len(self.save_events()) - saves_before, 0)
                outcome_requests = [request for relay, count in zip(self.relay_capture(), counts_before)
                                    for request in relay["requests"][count:]]
                assert_equal(sum(row["method"] == "outcome_evidence" for row in outcome_requests), 1)
                result.update(outcome_only_sync_write_delta=0, actual_outcome_requests=1, upgraded=upgraded)
                for relay in self.tls_relays:
                    relay.configure()
            else:
                result["fresh_verified_after_restart"] = self.fresh_verified_status(market_id, action_id)
            # In the prewrite phase ONLY fresh independent evidence now makes
            # the marker durable; the old restored blob did not supply it.
            durable = self.latest_saved_journal(action_id)
            assert action_row(durable["data"], action_id)["previously_certified"]
            result["final_expired_retry"] = self.assert_no_replay_with_expired_receipt(
                market_id, action_id, start_submits + 1, require_unknown=False)
            for relay in self.tls_relays:
                relay.configure()
            result["after_account"] = self.assert_one_order_effect(market_id, before_account, 1)
            assert_equal(len(self.submissions()) - start_submits, 1)
            result.update(original_submit_count=1, before_account=before_account)
            success = True
        finally:
            cleanup_errors = []
            if self.crash_token:
                # Preserve a held post-sync guard's target but explicitly
                # release both fixture barriers before graceful cleanup.
                try:
                    marker_path = self.client.datadir_path / "flowmesh-client-crash-guard.json"
                    marker = json.loads(marker_path.read_text(encoding="utf-8"))
                    marker.update(release_lane_before_apply=True, release_after_sync_before_digest=True)
                    temporary = marker_path.with_suffix(".next")
                    temporary.write_text(json.dumps(marker), encoding="utf-8")
                    temporary.replace(marker_path)
                except Exception as error:
                    cleanup_errors.append({"phase": "release_markers", "error_type": type(error).__name__})
            for thread in (self.snapshot_thread, self.crash_thread):
                if thread is not None:
                    try:
                        thread.join(timeout=35)
                    except Exception as error:
                        cleanup_errors.append({"phase": "join_owned_rpc", "error_type": type(error).__name__})
            for relay in self.tls_relays:
                try:
                    relay.configure()
                except Exception as error:
                    cleanup_errors.append({"phase": "reset_owned_relay", "error_type": type(error).__name__})
            result["owned_rpc_threads_stopped"] = all(thread is None or not thread.is_alive()
                                                       for thread in (self.snapshot_thread, self.crash_thread))
            result["fixture_cleanup_errors"] = cleanup_errors
            try:
                self.write_evidence("RESULTS.json", {"success": success and not cleanup_errors, "result": result,
                    "requests": self.relay_capture(), "hook_events": self.hook_events() if self.crash_token else [],
                    "scope": "one preselected phase, one new generated client, one exact original, no live data"})
            except Exception as error:
                cleanup_errors.append({"phase": "write_final_evidence", "error_type": type(error).__name__})
            assert result["owned_rpc_threads_stopped"], "Owned RPC thread did not stop"
            assert not cleanup_errors, cleanup_errors


if __name__ == "__main__":
    FlowMeshSnapshotAtomicFaultTest(__file__).main()
