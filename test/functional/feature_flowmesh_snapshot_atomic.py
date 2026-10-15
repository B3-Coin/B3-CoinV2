#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Test-first snapshot storage-count control on one NEW generated client.

Version 1 deliberately implements ONLY the successful snapshot count phase.
The frozen two-save client must fail the default one-save assertion, after
retaining the actual count and exact restart-field comparison. The explicit
--snapshot-expected-sync-writes=2 option is a baseline positive control, not
qualification of an atomic implementation. No production change is implied.

This inherits the unchanged r5 generated legacy bootstrap and exact owned
child cleanup. Only client4 uses a separately compiled crash-hook daemon.
It does not qualify failed/uncertain writes, SIGKILL, power loss or an outcome-
only upgrade; those require separately reviewed phases and hook boundaries.
"""

import copy
import hashlib
import json
from pathlib import Path

from feature_flowmesh_client_durability import FlowMeshClientDurabilityTest, original_sha
from test_framework.util import assert_equal


class FlowMeshSnapshotAtomicTest(FlowMeshClientDurabilityTest):
    def add_options(self, parser):
        super().add_options(parser)
        parser.add_argument("--snapshot-expected-sync-writes", type=int, choices=(1, 2), default=1,
                            help="1: test-first successor target (default); 2: frozen baseline positive control only")

    def set_test_params(self):
        super().set_test_params()
        self.crash_cases = ["snapshot-sync-count-v1"]

    def run_test(self):
        # Preserve r5's exact generated bootstrap, independent-mesh checks,
        # no-preagreement profile, workload seam and all-child exit capture.
        super().run_test()

    def count_phase_saves(self, case):
        return [row for row in self.save_events() if row["case"] == case]

    def qualification_workload(self, market_id):
        self.last_market = market_id
        self.crash_evidence = Path(self.options.tmpdir, "client-snapshot-atomic-v1")
        self.crash_evidence.mkdir(exist_ok=False)
        success = False
        result = {
            "format_version": 1,
            "profile": "generated-regtest-snapshot-sync-count-v1",
            "bootstrap_profile": "generated-legacy-minimal-genesis-v1",
            "phase": "successful_new_head_and_original_inclusion",
            "attempts": 1,
            "expected_snapshot_sync_writes": self.options.snapshot_expected_sync_writes,
            "baseline_positive_control_only": self.options.snapshot_expected_sync_writes == 2,
            "full_release_reindex_qualification": False,
            "failed_write_qualified": False,
            "unknown_write_qualified": False,
            "kill_after_sync_before_publication_qualified": False,
            "power_loss_qualified": False,
            "outcome_only_upgrade_qualified": False,
        }
        try:
            self.initialize_crash_client(market_id)
            self.end_b3_workload()
            self.sync_blocks(timeout=120)
            self.wait_client_b3_sync()
            before_account = self.client_balance(market_id)
            sequence = before_account["next_sequence"]
            start_submits = len(self.submissions())
            self.configure_hook("snapshot-v1-delayed-lane", hold_lane=True)
            self.begin_lane_order(market_id, sequence)
            holder = {}

            def lane_held():
                rows = [row for row in self.hook_events()
                        if row["pid"] == self.client.process.pid
                        and row["case"] == "snapshot-v1-delayed-lane"
                        and row["stage"] == "lane_reply_before_apply_held"]
                if not rows:
                    return False
                holder["event"] = rows[-1]
                return True

            self.wait_until(lane_held, timeout=15, check_interval=.01)
            action_id = holder["event"]["data"]["action_id"]
            assert_equal(len(self.submissions()) - start_submits, 1)
            saved_before = self.latest_saved_journal(action_id)
            original = action_row(saved_before["data"], action_id)
            old_head = market_head(saved_before["data"], market_id)
            assert original["may_have_been_sent"] and not original["previously_certified"]
            assert_equal(original["market_id"], market_id)

            def operator_has_certificate():
                receipt = self.operator_action_status(market_id, action_id)
                return (receipt["certificate_verified"] and receipt["receipt_state"] == "certified_inclusion"
                        and bool(receipt.get("certified_payload")) and "evidence_error" not in receipt)

            # Do not query client action status here: its separate observation
            # would persist the marker before the snapshot being measured.
            self.wait_until(operator_has_certificate, timeout=10, check_interval=.1)
            case = "snapshot-v1-counted-install"
            self.configure_hook(case, hold_lane=True)
            assert_equal(len(self.count_phase_saves(case)), 0)
            request_counts = [len(row["requests"]) for row in self.relay_capture()]
            snapshot = self.client.getflowmeshmarketdata(market_id, {"limit": 1, "curve_limit": 2})
            saves = self.count_phase_saves(case)
            requests = [request for relay, count in zip(self.relay_capture(), request_counts)
                        for request in relay["requests"][count:]]
            # A cached read is not evidence of a cheaper InstallSnapshot.
            assert any(request["method"] == "snapshot" for request in requests), "No actual snapshot request"
            assert_equal(snapshot["market_id"], market_id)
            assert_equal(snapshot["verification"]["certificate_verified"], True)
            assert_equal(snapshot["verification"]["account_state_verified"], True)
            assert saves, "No actual synchronous journal save returned during the snapshot"
            final = saves[-1]
            new_head = market_head(final["data"], market_id)
            final_original = action_row(final["data"], action_id)
            assert new_head["sequence"] > old_head["sequence"], "The measured snapshot did not advance the retained head"
            assert final_original["previously_certified"] and final_original["may_have_been_sent"]
            assert final_original["action_hex"] == original["action_hex"], "Exact retained original bytes changed"
            expected = copy.deepcopy(saved_before["data"])
            action_row(expected, action_id)["previously_certified"] = True
            market_head(expected, market_id).update(new_head)
            # Compare every restart-relevant field, not only the two changed
            # flags. Receipt text is deliberately not restored by the client.
            assert restart_fields(final["data"]) == restart_fields(expected), "Unexpected restart-relevant journal change"
            transitions = [{"head": market_head(row["data"], market_id),
                            "previously_certified": action_row(row["data"], action_id)["previously_certified"],
                            "monotonic_us": row["monotonic_us"]} for row in saves]
            assert all(row["head"] == new_head for row in transitions)
            if len(transitions) == 2:
                assert not transitions[0]["previously_certified"] and transitions[1]["previously_certified"]
            # Retain the decisive measurement BEFORE the default assertion,
            # so the expected baseline failure cannot erase its evidence.
            result.update(action_id=action_id, market_id=market_id,
                          actual_snapshot_sync_writes=len(saves), save_transitions=transitions,
                          old_head=old_head, new_head=new_head,
                          exact_original_sha256=original_sha(original),
                          exact_original_bytes_unchanged=True,
                          final_restart_fields_equal_expected=True,
                          final_restart_fields_sha256=restart_sha(final["data"]))
            self.write_evidence("COUNTED-SNAPSHOT.json", {
                "result": result, "held_lane": holder["event"],
                "before": saved_before, "saves": saves, "snapshot": snapshot,
                "expected_restart_fields": restart_fields(expected), "requests": requests})

            unchanged_before = len(self.save_events())
            unchanged = self.client.getflowmeshmarketdata(market_id, {"limit": 1, "curve_limit": 2})
            assert_equal(unchanged["verification"]["certificate_verified"], True)
            unchanged_delta = len(self.save_events()) - unchanged_before
            assert_equal(unchanged_delta, 0)
            result["unchanged_read_sync_write_delta"] = unchanged_delta

            # Deliver the already received original response, never submit a
            # second instruction. Its verified-return fast path must not add
            # a sync merely because snapshot inclusion is already durable.
            lane_before = len(self.save_events())
            self.configure_hook("snapshot-v1-release-lane", hold_lane=True, release_lane=True)
            self.crash_thread.join(timeout=15)
            assert not self.crash_thread.is_alive(), "The held original lane did not finish within its bound"
            assert "response" in self.crash_rpc_result, self.crash_rpc_result.get("error_type", "No lane response")
            response = self.crash_rpc_result["response"]
            assert_equal(response["action_id"], action_id)
            assert_equal(response["certificate_verified"], True)
            lane_delta = len(self.save_events()) - lane_before
            assert_equal(lane_delta, 0)
            assert_equal(len(self.submissions()) - start_submits, 1)
            retained = action_row(self.local_saved(market_id), action_id)
            assert retained["previously_certified"] and retained["may_have_been_sent"]
            assert_equal(retained["signed_bytes_sha256"], original_sha(original))
            after_account = self.assert_one_order_effect(market_id, before_account, 1)
            result.update(delayed_lane_response=response, delayed_lane_sync_write_delta=lane_delta,
                          original_submit_count=1, before_account=before_account, after_account=after_account)
            self.write_evidence("BEFORE-TARGET-ASSERTION.json", result)
            assert_equal(len(saves), self.options.snapshot_expected_sync_writes)
            success = True
        finally:
            if self.crash_token:
                self.configure_hook("snapshot-v1-cleanup", hold_lane=True, release_lane=True)
            if self.crash_thread is not None:
                self.crash_thread.join(timeout=35)
                result["submission_thread_stopped"] = not self.crash_thread.is_alive()
            for relay in self.tls_relays:
                relay.configure()
            self.write_evidence("RESULTS.json", {
                "success": success, "result": result, "requests": self.relay_capture(),
                "hook_events": self.hook_events() if self.crash_token else [],
                "scope": "one generated regtest client; successful snapshot count phase only"})
            assert self.crash_thread is None or not self.crash_thread.is_alive(), "Owned submission thread did not stop"


def action_row(root, action_id):
    rows = [row for row in root["actions"] if row["action_id"] == action_id]
    assert_equal(len(rows), 1)
    return rows[0]


def market_head(root, market_id):
    rows = [row for row in root["heads"] if row["market_id"] == market_id]
    assert_equal(len(rows), 1)
    return rows[0]


def restart_fields(root):
    return {"version": root["version"], "heads": copy.deepcopy(root["heads"]),
            "actions": [{key: copy.deepcopy(value) for key, value in row.items() if key != "receipt"}
                        for row in root["actions"]]}


def restart_sha(root):
    encoded = json.dumps(restart_fields(root), sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


if __name__ == "__main__":
    FlowMeshSnapshotAtomicTest(__file__).main()
