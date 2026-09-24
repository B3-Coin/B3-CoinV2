#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""One bounded fault experiment using only fresh generated regtest state.

Four validators, a funded engine-off client, actual matched orders/cancels:
12 baseline actions, graceful node3 stop and six live-quorum actions, restart
with BULK held for four actions, BULK throttled for four, exact catch-up,
explicit rearm, then four recovered actions. No full withdrawal/reindex
campaign, store reset, key replacement, quorum change, or unproven leader claim.

This is an opt-in fault-latency report, separate from healthy-load gates.
Default action deadline is 60 seconds. Failed scenarios stop without rearming
or repairing signing state. Parent shutdown records actual child exit status.
"""

import json
import math
import queue
import sys
import threading
import time
from pathlib import Path

from feature_flowmesh_latency import PRE_ADMISSION_REJECTIONS, distribution, host_us
from feature_flowmesh_performance import (
    BALANCE_FIELDS, FlowMeshPerformanceTest, TRADE_PRICE,
    arrival_offsets, compact_error, expected_balance, summarize_window,
)
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal


class FlowMeshPerformanceFaultsTest(FlowMeshPerformanceTest):
    def set_test_params(self):
        super().set_test_params()
        self.required_replicas = [0, 1, 2, 3]
        self.returning_rpc = None
        self.next_returning_observation_us = 0
        self.recovery = {}

    def pump_b3(self):
        super().pump_b3()
        if self.returning_rpc is not None and host_us() >= self.next_returning_observation_us:
            self.next_returning_observation_us = host_us() + 100_000
            self.observe_returning_replica()

    def observe_returning_replica(self):
        pending = [row for row in list(self.performance_report.get("samples", []))
                   if row.get("required_replicas") == [0, 1, 2] and
                   "certified_status" in row and "returning_replica_observation" not in row]
        if not pending:
            return
        market = self.market_specs[0]["market_id"]
        data = self.returning_rpc.getflowmeshmarketdata(market, {"limit": 100, "curve_limit": 1})
        observed = host_us()
        for sample in pending:
            status = sample["certified_status"]
            if not self.contains_target(data, status["microblock_sequence"], status["microblock_hash"]):
                continue
            row = {"node": 3, "observed_host_us": observed,
                   "sequence": status["microblock_sequence"], "hash": status["microblock_hash"],
                   "from_original_submission_ms": (observed - sample["initial_submission_host_us"]) / 1000,
                   "from_restart_ms": (observed - self.recovery["restart_host_us"]) / 1000,
                   "during_phase": self.recovery.get("phase"),
                   "meaning": "exact certificate observed in returned replica applied history; not disk timestamp"}
            if "bulk_release_host_us" in self.recovery:
                row["from_bulk_release_ms"] = (observed - self.recovery["bulk_release_host_us"]) / 1000
            sample["returning_replica_observation"] = row

    # The signed-action path below preserves the healthy fixture's receipt and
    # authenticated-account checks. Only replica selection/labels differ.
    def execute_action(self, worker, rpc, replicas, sample):
        required = tuple(self.required_replicas)
        sample["required_replicas"] = list(required)
        sample["replica_scope"] = "all_four" if len(required) == 4 else "live_quorum_only"
        # Recreate connections after restart; node3 has a fresh RPC cookie.
        replicas = {index: self.independent_rpc(self.nodes[index], self.default_wallet_name)
                    for index in required}
        market, kind = worker["market_id"], sample["kind"]
        before = dict(worker["account"])
        sequence = before["next_sequence"]
        sample.update(status="running", account_sequence=sequence, account_id=before["account_id"],
                      account_before={field: before[field] for field in BALANCE_FIELDS})
        started = host_us()
        sample["initial_submission_host_us"] = started
        sample["worker_queue_ms"] = (started - sample["enqueued_host_us"]) / 1000
        deadline = time.monotonic() + self.options.performance_action_timeout
        params = (market, "bid", sequence) if kind == "cancel" else (
            market, "bid", TRADE_PRICE if kind == "fill_bid" else TRADE_PRICE // 2, 1, sequence)
        method = "cancelflowmeshorder" if kind == "cancel" else "submitflowmeshorder"
        response = None
        while response is None:
            self.stop_requested(deadline)
            try:
                response = getattr(rpc, method)(*params)
            except JSONRPCException as error:
                if error.error.get("code") == -1 and error.error.get("message") in PRE_ADMISSION_REJECTIONS:
                    sample["attempts"].append({"explicit_pre_admission_rejection": error.error["message"],
                                                "observed_host_us": host_us()})
                    time.sleep(.01)
                    continue
                raise
            except Exception as error:
                # A transport exception is ambiguous. Inspect the local saved
                # outbox; NEVER sign the sequence again after this path.
                sample["attempts"].append({"ambiguous_rpc_error": compact_error(error), "observed_host_us": host_us()})
                recovery = self.independent_rpc(self.client, worker["wallet"])
                matches = [row for row in recovery.listflowmeshactions(market)["actions"]
                           if row.get("sequence") == sequence]
                assert_equal(len(matches), 1)
                response = matches[0]["receipt"]
                rpc = recovery
        observed = self.observed_rpc_return_us()
        action_id = response["action_id"]
        assert_equal(len(action_id), 64)
        sample.update(action_id=action_id, initial_response=response, initial_response_host_us=observed,
                      initial_response_ms=(observed - started) / 1000)
        status = response
        status_observed = observed
        next_retry = time.monotonic() + 1
        while True:
            state = status["receipt_state"]
            assert_equal(status["action_id"], action_id)
            assert state in {"queued", "admitted", "rejected", "unknown", "certified_inclusion"}, status
            if not sample["receipt_observations"] or state != sample["receipt_observations"][-1]["receipt_state"]:
                sample["receipt_observations"].append({"receipt_state": state, "observed_host_us": status_observed})
            if state == "admitted" and "admitted_observed_host_us" not in sample:
                sample["admitted_observed_host_us"] = status_observed
                sample["admitted_observed_ms"] = (status_observed - started) / 1000
            if state == "certified_inclusion":
                break
            self.stop_requested(deadline)
            assert_equal(status["action_id"], action_id)
            retryable = status["receipt_state"] == "unknown" or (
                status["receipt_state"] == "rejected" and status.get("reason") in PRE_ADMISSION_REJECTIONS)
            assert status["receipt_state"] != "rejected" or retryable, status
            if retryable and time.monotonic() >= next_retry:
                retry = rpc.retryflowmeshaction(market, action_id)
                assert_equal(retry["action_id"], action_id)
                status, status_observed = retry, self.observed_rpc_return_us()
                sample["attempts"].append({"same_signed_action_retry": True, "observed_host_us": status_observed,
                                            "receipt_state": retry["receipt_state"]})
                next_retry = time.monotonic() + 1
                continue
            time.sleep(.005)
            status = rpc.getflowmeshactionstatus(market, action_id)
            status_observed = self.observed_rpc_return_us()
        certified = status_observed
        assert_equal(status["certificate_verified"], True)
        assert_equal(status["outcome_verified"], False)
        sample.update(certified_status=status, client_certified_host_us=certified,
                      client_certified_ms=(certified - started) / 1000,
                      client_certified_from_offer_ms=(certified - sample["scheduled_host_us"]) / 1000)
        expected = expected_balance(before, kind)
        while True:
            self.stop_requested(deadline)
            data = self.followup_account_read(rpc, market, sample, deadline)
            account = data["account"]
            if account["next_sequence"] >= sequence + 1:
                assert_equal(account["next_sequence"], sequence + 1)
                assert_equal({field: account[field] for field in BALANCE_FIELDS}, expected)
                if sample.get("read_consistency_failures"):
                    sample["account_read_recovered"] = True
                break
            time.sleep(.005)
        account_observed = self.observed_rpc_return_us()
        sample.update(account_state_verified_host_us=account_observed,
                      account_state_verified_ms=(account_observed - started) / 1000,
                      account_state_verified_from_offer_ms=(account_observed - sample["scheduled_host_us"]) / 1000,
                      account_after=expected, certified_account_state_checked=True,
                      fill_evidence={"expected_fill_units": 1 if kind == "fill_bid" else 0,
                          "authenticated_balance_delta_checked": True, "execution_result_verified": False,
                          "history_endpoint_reported": True,
                          "reported_matching_entries": [row for row in data["history"]["entries"]
                              if row["microblock_hash"] == status["microblock_hash"]]})
        target = (status["microblock_sequence"], status["microblock_hash"])
        while len(sample["replica_observations"]) < len(required):
            self.stop_requested(deadline)
            for index, replica in replicas.items():
                if str(index) in sample["replica_observations"]:
                    continue
                data = replica.getflowmeshmarketdata(market, {"limit": 100, "curve_limit": 1})
                if self.contains_target(data, *target):
                    stamp = host_us()
                    sample["replica_observations"][str(index)] = {
                        "observed_host_us": stamp, "observed_ms": (stamp - started) / 1000,
                        "sequence": target[0], "hash": target[1]}
            if len(sample["replica_observations"]) < len(required):
                time.sleep(.005)
        applied = max(row["observed_host_us"] for row in sample["replica_observations"].values())
        retained = [row for row in rpc.listflowmeshactions(market)["actions"] if row["action_id"] == action_id]
        assert_equal(len(retained), 1)
        assert_equal(retained[0]["sequence"], sequence)
        sample["retained_action"] = retained[0]
        sample.update(required_replica_observed_host_us=applied,
                      required_replica_observed_ms=(applied - started) / 1000,
                      complete=True, status="complete", completed_host_us=host_us())
        if len(required) == 4:
            sample.update(every_replica_observed_host_us=applied,
                          every_replica_observed_ms=(applied - started) / 1000)
        worker["account"] = account

    def run_window(self, label, *, rate, duration, count=None):
        count = math.ceil(rate * duration) if count is None else count
        offsets = arrival_offsets(count, rate)
        window = {"label": label, "rate_actions_per_second": rate, "duration_seconds": duration,
                  "planned_offers": count, "start_host_us": host_us() + 100_000,
                  "samples": [], "backlog": []}
        self.performance_report["windows"].append(window)
        indices = tuple(self.required_replicas)
        window["required_replicas"] = list(indices)
        heights = [self.nodes[index].getblockcount() for index in indices]
        scheduler = threading.Thread(target=self.schedule_window, args=(window, self.workers, offsets),
                                     name="flowmesh-offer-scheduler")
        scheduler.start()
        self.threads.append(scheduler)
        end_us = window["start_host_us"] + round(duration * 1_000_000)
        next_backlog = 0
        while host_us() < end_us or scheduler.is_alive():
            self.pump_b3()
            self.drain_relay_observations()
            now = host_us()
            if now >= next_backlog:
                with self.report_lock:
                    outstanding = sum(row["status"] not in {"complete", "failed", "dropped"}
                                      for row in window["samples"])
                window["backlog"].append({"observed_host_us": now, "outstanding": outstanding})
                next_backlog = now + 100_000
            time.sleep(.01)
        scheduler.join()
        window["offer_window_end_host_us"] = end_us
        deadline = time.monotonic() + self.options.performance_action_timeout + 20
        while any(worker["queue"].unfinished_tasks for worker in self.workers):
            self.pump_b3()
            self.drain_relay_observations()
            if time.monotonic() >= deadline:
                self.worker_stop.set()
                window["drain_timeout"] = True
                break
            time.sleep(.01)
        for sample in window["samples"]:
            sample["certified_during_offer_window"] = sample.get("client_certified_host_us", end_us + 1) <= end_us
            sample["account_verified_during_offer_window"] = sample.get("account_state_verified_host_us", end_us + 1) <= end_us
            sample["completed_during_offer_window"] = sample.get("completed_host_us", end_us + 1) <= end_us
        window["heights"] = {"node_indices": list(indices), "before": heights,
                             "after": [self.nodes[index].getblockcount() for index in indices]}
        window["drained_host_us"] = host_us()
        window["summary"] = summarize_window(window["samples"], window["backlog"], duration,
                                              (window["drained_host_us"] - window["start_host_us"]) / 1_000_000)
        window["summary"]["planned_offers_preserved"] = len(window["samples"]) == count
        window["summary"]["b3_advanced"] = all(after > before for before, after in
                                               zip(heights, window["heights"]["after"]))
        if not window["summary"]["planned_offers_preserved"] or "scheduler_error" in window:
            window["summary"]["correctness_pass"] = window["summary"]["performance_pass"] = False
        window["summary"]["healthy_latency_target_applicable"] = False
        window["summary"]["healthy_target_comparison_only"] = window["summary"].pop("performance_pass")
        window["summary"]["metrics"]["required_replica_observed_ms"] = distribution(
            [row["required_replica_observed_ms"] for row in window["samples"]
             if "required_replica_observed_ms" in row], 600)
        self.log.info("FLOWMESH_FAULT_PERFORMANCE_WINDOW %s", json.dumps(
            {"label": label, **window["summary"]}, sort_keys=True))
        self.write_report()
        return window["summary"]

    def fault_phase(self, label, count, replicas):
        assert not any(worker["queue"].unfinished_tasks for worker in self.workers)
        self.required_replicas = list(replicas)
        self.recovery["phase"] = label
        summary = self.run_window(label, rate=.5, duration=count * 2, count=count)
        assert self.phase_can_continue(summary), "fault phase account proof unavailable; no scenario escalation"
        assert summary["b3_advanced"], "B3 did not advance during the selected live-replica phase"
        return summary

    def wait_recovery_target(self, target, *, require_signing=False):
        deadline = time.monotonic() + 60
        market = self.market_specs[0]["market_id"]
        while True:
            self.pump_b3()
            data = self.nodes[3].getflowmeshmarketdata(market, {"limit": 100, "curve_limit": 1})
            observed = host_us()
            snapshot = data["snapshot"]
            applied = bool(snapshot["running"] and snapshot["halt"] == "none" and
                           self.contains_target(data, target["sequence"], target["hash"]))
            if applied and snapshot["next_microblock_sequence"] == target["sequence"] + 1:
                assert_equal(snapshot["state_root"], target["state_root"])
            status_ready = (snapshot.get("observer_only") is False and
                            snapshot.get("paused") is False and
                            snapshot.get("chain_reconciling") is False and
                            snapshot.get("error") == "")
            self.recovery["last_readiness_observation"] = {
                "observed_host_us": observed, "exact_target_applied": applied,
                "status": {key: snapshot.get(key) for key in (
                    "running", "halt", "observer_only", "paused", "chain_reconciling", "error")},
                "signing_share_proven": False,
            }
            if applied and (not require_signing or status_ready):
                return observed
            assert time.monotonic() < deadline, "returning replica did not reach bounded target"
            time.sleep(.05)

    def capture_clock_bounds(self):
        if not self.options.performance_diagnostic:
            return
        for node, calibrations in self.clock_samples.items():
            if node == "3":
                # A restart creates a new process epoch. Keep both raw
                # calibrations but do not synthesize one cross-epoch bridge.
                self.performance_report["restarted_process_clock_calibrations"] = calibrations
                continue
            lower = max(row["offset_us_bounds"][0] for row in calibrations)
            upper = min(row["offset_us_bounds"][1] for row in calibrations)
            assert lower <= upper, "clock offset intervals do not intersect"
            self.performance_report["clock_offsets"][node] = {
                "offset_us_bounds": [lower, upper], "calibrations": calibrations}

    def write_report(self):
        if not self.performance_report:
            return
        self.prepare_public_trace_report()
        self.performance_report["https_submit_records"] = self.submit_records
        self.performance_report["https_method_counts"] = dict(self.relay_method_counts)
        snapshot = {**self.performance_report, "windows": [
            {key: ([row["sample_id"] for row in value] if key == "samples" else value)
             for key, value in window.items()} for window in self.performance_report["windows"]]}
        Path(self.options.tmpdir, "flowmesh-performance-faults.json").write_text(
            json.dumps(snapshot, indent=2, sort_keys=True, default=str) + "\n", encoding="utf-8")

    def run_test(self):
        assert_equal(self.options.performance_markets, 1)
        self.performance_report = {
            "format_version": 1, "fixture": "bounded_fault_four_validator_https",
            "logging_mode": "detailed_bench" if self.options.performance_diagnostic else "production_debug_disabled",
            "scope": {"operators": 4, "ordinary_client_processes": 1, "buyer_accounts": 4,
                "markets": 1, "transport": "independent TCP + ordinary HTTPS through test TLS relays",
                "faults_injected": True, "healthy_latency_target_applicable": False,
                "signed_actions": "same funded maker and buyer fixtures as healthy benchmark",
                "required_replica_semantics": "0/1/2 during fault phases; all four only after separate catch-up and rearm",
                "returning_replica_semantics": "exact historical certificate checks; timestamps from host monotonic observations",
                "client_serialization": "wallets share one daemon backend and its existing m_work mutex",
                "execution_result_verified": False,
                "fill_evidence": "authenticated buyer and maker balance deltas; adjacent endpoint history separately reported",
                "restart": "graceful stop and reopen same generated node3 datadir, original keys and signing journals",
                "not_qualified": ["WAN", "SIGKILL/power loss", "failed proposer", "hostile ingress", "production recovery"]},
            "samples": [], "windows": [], "logging_checks": [], "clock_offsets": {},
            "correctness_pass": False, "fault_scenario_pass": False, "performance_pass": None,
            "recovery": self.recovery,
            "campaign": {"read_recovery_enabled": self.options.performance_read_recovery,
                "planned_windows": ["baseline_before_fault", "node3_offline", "node3_bulk_held",
                    "node3_bulk_throttled", "node3_recovered_all_four"],
                "public_trace_enabled": self.options.performance_public_trace,
                "read_recovery_never_converts_first_read_failure_to_success": True},
            "failed_proposer": {"qualified": False,
                "reason": "offline transport identity does not prove proposer selection; no deterministic proposer fault injected"},
            "hostile_ingress": {"qualified": False, "reason": "outside this bounded existing-proxy scenario"}}
        self.latency_report = self.performance_report
        try:
            market, asset = self.bootstrap_latency_market()
            self.market_specs.insert(0, {"market_id": market, "asset": asset})
            self.setup_wallets()
            self.assert_performance_engine_off()
            if self.options.performance_diagnostic:
                self.calibrate_clocks(market, "before_node3_restart")
            else:
                self.verify_production_logging(market, "before_fault")
            for worker in self.workers:
                thread = threading.Thread(target=self.worker_loop, args=(worker,), name="flowmesh-buyer-worker")
                thread.start()
                self.threads.append(thread)

            self.begin_b3_workload(range(4))
            self.fault_phase("baseline_before_fault", 12, range(4))
            self.end_b3_workload()
            before = self.target(self.nodes[0].getflowmeshmarketdata(market)["snapshot"])
            original_validator = self.nodes[3].getflowmeshvalidatorinfo()
            original_datadir = self.nodes[3].datadir_path
            process = self.nodes[3].process
            self.recovery.update(before_target=before, stop_requested_host_us=host_us())
            self.stop_node(3)
            self.recovery.update(stopped_host_us=host_us(), graceful_stop_exit_code=process.returncode)
            assert_equal(process.returncode, 0)
            self.wait_for_independent_mesh([0, 1, 2])
            self.begin_b3_workload([0, 1, 2])
            self.fault_phase("node3_offline", 6, [0, 1, 2])
            offline_target = self.target(self.nodes[0].getflowmeshmarketdata(market)["snapshot"])
            assert offline_target["sequence"] > before["sequence"]
            self.recovery["offline_target"] = offline_target

            delayed_before = self.fault.snapshot()["delayed_bulk_bytes"]
            self.fault.configure(self.operator_keys[3], hold=True)
            self.recovery["restart_host_us"] = host_us()
            self.recovery["phase"] = "restart_bulk_held"
            self.start_node(3)
            self.nodes[3].setmocktime(self.mock_time)
            self.connect_nodes(3, 2)
            self.wait_for_independent_mesh()
            self.recovery["authenticated_mesh_observed_host_us"] = host_us()
            self.recovery["restart_to_authenticated_mesh_ms"] = (
                self.recovery["authenticated_mesh_observed_host_us"] - self.recovery["restart_host_us"]) / 1000
            validator = self.nodes[3].getflowmeshvalidatorinfo()
            assert_equal(validator["armed"], False)
            assert_equal(validator["wallet_key_count"], 1)
            assert_equal(validator["wallet_bls_pubkeys"], original_validator["wallet_bls_pubkeys"])
            assert_equal(self.nodes[3].datadir_path, original_datadir)
            self.recovery["returned_unarmed"] = True
            self.recovery["same_generated_datadir_and_wallet_keys"] = True
            assert not self.target_applied(3, market, offline_target), "bulk hold did not retain the selected catch-up gap"
            self.recovery["offline_target_missing_with_bulk_held"] = True
            self.returning_rpc = self.independent_rpc(self.nodes[3], self.default_wallet_name)
            # Node3 remains unarmed/non-staking, but follows the same B3 time.
            self.clock_indices = [0, 1, 2, 3]
            self.fault_phase("node3_bulk_held", 4, [0, 1, 2])
            assert self.fault.snapshot()["bulk_held"]
            self.recovery["held_profile_after_actions"] = self.fault.snapshot()
            held_data = self.nodes[3].getflowmeshmarketdata(market, {"limit": 100, "curve_limit": 1})
            held_snapshot = held_data["snapshot"]
            held_applied = bool(held_snapshot["running"] and held_snapshot["halt"] == "none" and
                                self.contains_target(held_data, offline_target["sequence"], offline_target["hash"]))
            self.recovery["offline_target_after_held_actions"] = {
                "observed_host_us": host_us(), "target": offline_target,
                "exact_target_applied": held_applied,
                "status": {key: held_snapshot.get(key) for key in (
                    "running", "halt", "observer_only", "paused", "chain_reconciling", "error")},
                "classification": "caught_up_while_bulk_hold_configured_possible_alternate_path" if held_applied
                    else "exact_target_still_missing_after_held_actions",
                "path_attribution_proven": False,
            }

            self.recovery["bulk_release_host_us"] = host_us()
            self.fault.configure(self.operator_keys[3], rate=2048)
            self.fault_phase("node3_bulk_throttled", 4, [0, 1, 2])
            target = self.target(self.nodes[0].getflowmeshmarketdata(market)["snapshot"])
            self.recovery["catchup_target"] = target
            self.recovery["catchup_target_selected_host_us"] = host_us()
            caught_up = self.wait_recovery_target(target)
            self.recovery["catchup_ready_observed_host_us"] = caught_up
            self.recovery["restart_to_catchup_ready_ms"] = (caught_up - self.recovery["restart_host_us"]) / 1000
            self.recovery["bulk_release_to_catchup_ready_ms"] = (caught_up - self.recovery["bulk_release_host_us"]) / 1000
            assert_equal(self.market_status(self.nodes[3], market)["observer_only"], True)
            self.observe_returning_replica()
            missing = [row["sample_id"] for row in self.performance_report["samples"]
                       if row.get("required_replicas") == [0, 1, 2] and
                       "returning_replica_observation" not in row]
            assert not missing, f"returning replica exact targets unobserved: {missing}"
            delayed_after = self.fault.snapshot()["delayed_bulk_bytes"]
            assert delayed_after > delayed_before
            self.recovery["delayed_bulk_bytes"] = delayed_after - delayed_before
            self.recovery["throttled_profile_after_catchup"] = self.fault.snapshot()
            self.fault.configure()

            self.recovery["phase"] = "explicit_rearm"
            self.recovery["rearm_requested_host_us"] = host_us()
            armed = self.nodes[3].startflowmeshvalidator()
            self.recovery["rearm_response_host_us"] = host_us()
            assert_equal(armed["running"], True)
            assert_equal(armed["armed_keys"], 1)
            ready = self.wait_recovery_target(target, require_signing=True)
            self.recovery["eligible_observed_host_us"] = ready
            self.recovery["explicit_rearm_to_eligible_ms"] = (ready - self.recovery["rearm_requested_host_us"]) / 1000
            self.recovery["eligibility_evidence"] = "exact target applied; unpaused, unreconciled, error-free non-observer status; not signing proof"
            self.recovery["signing_share_observed"] = False
            assert_equal(self.nodes[3].startstaking()["running"], True)
            self.fault_phase("node3_recovered_all_four", 4, range(4))
            if self.options.performance_diagnostic:
                proof = self.returning_share(market, target["sequence"] + 1)
                if proof is not None:
                    self.recovery["signing_share_observed"] = True
                    self.recovery["accepted_share_proof"] = proof
            self.end_b3_workload()
            self.returning_rpc = None
            self.check_signed_observations()
            self.check_makers()
            self.wait_for_independent_mesh()
            self.assert_no_b3_flowmesh_traffic()
            self.assert_performance_engine_off()
            if self.options.performance_diagnostic:
                self.calibrate_clocks(market, "after_node3_restart")
                self.capture_clock_bounds()
            else:
                self.verify_production_logging(market, "after_fault")
            self.performance_report["correctness_pass"] = True
            self.performance_report["fault_scenario_pass"] = True
            self.performance_report["fault_coverage_completed"] = True
            self.prepare_public_trace_report()
            assert self.performance_report["read_consistency_pass"], "follow-up read consistency failed; recovered reads remain recorded failures"
        except Exception as error:
            self.performance_report["error"] = compact_error(error)
            self.performance_report["stopped_at_phase"] = self.recovery.get("phase", "setup")
            raise
        finally:
            primary_exception_in_flight = sys.exc_info()[0] is not None
            # Restore only byte-proxy policy; never reset or automatically arm
            # a failed generated signing store to make a scenario pass.
            self.fault.configure()
            self.returning_rpc = None
            self.scheduler_stop.set()
            self.worker_stop.set()
            for worker in self.workers:
                try:
                    worker["queue"].put_nowait(None)
                except queue.Full:
                    pass
            for thread in self.threads:
                thread.join(timeout=20)
            stopped = not any(thread.is_alive() for thread in self.threads)
            self.performance_report["all_harness_threads_stopped"] = stopped
            if not stopped:
                self.invalidate_final_result()
                self.performance_report["cleanup_error"] = {
                    "type": "AssertionError", "reason": "harness thread survived bounded cleanup joins"}
            self.end_b3_workload()
            self.performance_report["final_fault_proxy"] = self.fault.snapshot()
            self.finish_report(primary_exception_in_flight, "flowmesh-performance-faults.json", "FLOWMESH_FAULT_PERFORMANCE_REPORT")


if __name__ == "__main__":
    FlowMeshPerformanceFaultsTest(__file__).main()
