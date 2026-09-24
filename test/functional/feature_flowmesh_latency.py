#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Bounded healthy-load HTTPS latency sample on four fresh regtest operators.

The fifth process is an ordinary engine-off wallet. Its signed bids/cancels
cross the existing transparent test TLS relays, with no injected faults. B3
continues producing blocks. This intentionally does not run the inherited
withdrawal/reindex/fault campaign. It measures a sequential local workload,
not throughput, WAN latency, crash durability, or a universal latency bound.

Example: feature_flowmesh_latency.py --latency-warmup-pairs=2
    --latency-measured-pairs=10 --latency-observe-only
An observe-only run still records performance_pass=false on a target miss;
it never relaxes certificate, account-state, or signed-byte checks.

--latency-production-logging disables debug categories on all five processes.
That mode uses client/all-replica observed upper bounds and explicitly leaves
debug-trace admission/durable timestamps unknown. The full BENCH trace mode
remains the default and retains its additional durable-event assertions.
"""

import argparse
import hashlib
import json
import math
import time
from collections import Counter
from decimal import Decimal
from pathlib import Path

from feature_flowmesh_preagreement import FlowMeshPreagreementTest
from feature_flowmesh_release import A2, A3, B3_ARGS, CORRIDOR_END, TEST_ASSET_DEPOSIT, TEST_ASSET_SUPPLY, TRADE_PRICE
from test_framework.authproxy import JSONRPCException
from test_framework.flowmesh_client_tls import FlowMeshTLSFaultRelay
from test_framework.test_node import TestNode
from test_framework.util import assert_equal, get_datadir_path, initialize_datadir, p2p_port


PRE_ADMISSION_REJECTIONS = {
    "FlowMesh service is reconciling the B3 tip",
    "FlowMesh market is paused (at least four active seats are required)",
}


def positive_number(value):
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise argparse.ArgumentTypeError("must be finite and positive")
    return result


def host_us():
    return time.monotonic_ns() // 1000


def distribution(values, target_ms):
    ordered = sorted(values)
    result = {"count": len(ordered), "over_target_count": sum(value > target_ms for value in ordered)}
    for name, fraction in (("p50_ms", .50), ("p95_ms", .95), ("p99_ms", .99)):
        result[name] = ordered[math.ceil(len(ordered) * fraction) - 1] if ordered else None
    result["max_ms"] = ordered[-1] if ordered else None
    return result


class FlowMeshLatencyTest(FlowMeshPreagreementTest):
    def add_options(self, parser):
        super().add_options(parser)
        parser.add_argument("--latency-warmup-pairs", type=int, default=20)
        parser.add_argument("--latency-measured-pairs", type=int, default=50)
        parser.add_argument("--latency-target-ms", type=positive_number, default=200)
        parser.add_argument("--latency-poll-ms", type=positive_number, default=5)
        parser.add_argument("--latency-observe-only", action="store_true")
        parser.add_argument("--latency-production-logging", action="store_true",
                            help="Disable all daemon debug categories; require verified client/all-replica history observation without reconstructing BENCH timestamps")

    def set_test_params(self):
        super().set_test_params()
        self.latency_samples = []
        self.submit_records = []
        self.relay_method_counts = Counter()
        self.clock_samples = {str(index): [] for index in range(4)}
        self.latency_report = {}
        if self.options.latency_production_logging:
            for args in self.extra_args:
                args.append("-debug=0")

    def start_node(self, i, extra_args=None, *args, **kwargs):
        if self.options.latency_production_logging:
            # Parent restart hooks may append -debug=bench to an explicit
            # argument list. Last -debug=0 must come after those hooks.
            extra_args = [*(self.nodes[i].extra_args if extra_args is None else extra_args), "-debug=0"]
        return super().start_node(i, extra_args, *args, **kwargs)

    def start_nodes(self, extra_args=None, *args, **kwargs):
        if self.options.latency_production_logging:
            selected = [None] * self.num_nodes if extra_args is None else extra_args
            extra_args = [[*(node.extra_args if selected[index] is None else selected[index]), "-debug=0"]
                          for index, node in enumerate(self.nodes)]
        return super().start_nodes(extra_args, *args, **kwargs)

    def start_ordinary_client(self):
        if not self.options.latency_production_logging:
            return super().start_ordinary_client()
        # Same generated client/relay setup as the inherited fixture; the
        # only process-configuration difference is the final -debug=0. Keep
        # it in both retained argument lists for any subsequent client start.
        for port, upstream in zip(self.client_api_ports, self.api_ports):
            self.tls_relays.append(FlowMeshTLSFaultRelay(port, upstream, self.pki))
        initialize_datadir(self.options.tmpdir, 4, self.chain, self.disable_autoconnect)
        self.client_args = [*B3_ARGS, "-enableflowmeshvalidator=0", "-debug=bench",
                            f"-port={p2p_port(12)}", f"-bind=127.0.0.1:{p2p_port(12)}",
                            f"-flowmeshendpointca={self.pki['ca']}",
                            *[f"-flowmeshendpoint={relay.url}" for relay in self.tls_relays], "-debug=0"]
        self.client = TestNode(4, get_datadir_path(self.options.tmpdir, 4), chain=self.chain,
                               rpchost=None, timewait=self.rpc_timeout,
                               timeout_factor=self.options.timeout_factor, binaries=self.get_binaries(),
                               coverage_dir=self.options.coveragedir, cwd=self.options.tmpdir,
                               extra_args=self.client_args, uses_wallet=True)
        self.client.start()
        self.client.wait_for_rpc_connection()
        self.client.createwallet(wallet_name=self.default_wallet_name, load_on_startup=True)
        self.sync_client_time()
        self.client.addnode(f"127.0.0.1:{p2p_port(0)}", "onetry")
        self.wait_client_b3_sync()
        self.assert_engine_off()

    def verify_production_logging(self, market_id, phase):
        observations = {}
        for index, node in enumerate([*self.nodes, self.client]):
            categories = node.logging()
            enabled = sorted(name for name, enabled in categories.items() if enabled)
            observations[str(index)] = {"enabled_debug_categories": enabled,
                                        "enabled_debug_category_count": len(enabled),
                                        "bench_enabled": categories["bench"]}
            assert not enabled, f"production-logging mode has active debug categories on node {index}: {enabled}"
            if index < self.num_nodes:
                delivery = node.getflowmeshdeliveryinfo(market_id)["markets"]
                assert_equal(len(delivery), 1)
                assert_equal(delivery[0]["market_id"], market_id)
                observations[str(index)]["runtime_trace_bytes"] = delivery[0]["trace_bytes"]
                observations[str(index)]["runtime_trace_events_dropped"] = delivery[0]["trace_events_dropped"]
                assert_equal(delivery[0]["trace_bytes"], 0)
                assert_equal(delivery[0]["trace_events_dropped"], 0)
        self.latency_report["logging_checks"].append({"phase": phase, "process_count": len(observations),
                                                     "processes": observations})

    def bootstrap_latency_market(self):
        """Use the release fixture's transition helpers, not its full workload."""
        n0 = self.nodes[0]
        self.set_chain_time(self.mock_time)
        self.mine_corridor(101)
        for node in self.nodes[1:]:
            n0.sendtoaddress(node.get_deterministic_priv_key().address, 120)
        self.synchronize_mempools()
        self.mine_corridor(1)
        for node, amount in zip(self.nodes, (40, 30, 20, 10)):
            node.createstake(amount)
            node.bindfinalitykey()
        self.synchronize_mempools()
        self.mine_corridor(1)
        self.mine_corridor(CORRIDOR_END - n0.getblockcount())
        for node in self.nodes:
            self.wait_until(lambda n=node: n.getstakinginfo()["active"] > Decimal("0"), timeout=60)
        self.mine_pos_blocks(A2 - 1 - n0.getblockcount())
        assert_equal(n0.getblockcount(), A2 - 1)
        asset = n0.issueasset(TEST_ASSET_SUPPLY, 2)["asset_id"]
        seats = [node.bindflowmeshseat() for node in self.nodes]
        assert_equal(len({seat["seat_id"] for seat in seats}), 4)
        assert_equal(len({seat["bls_pubkey"] for seat in seats}), 4)
        bootstrap = n0.flowmeshdeposit(asset, asset, TEST_ASSET_DEPOSIT,
                                      {"minconf": 0, "include_unsafe": True, "market_bootstrap": True})
        market_id = bootstrap["market_id"]
        self.configure_fresh_market(market_id)
        self.synchronize_mempools()
        self.mine_pos_blocks(1)
        assert_equal(n0.getblockcount(), A2)
        for node in self.nodes:
            started = node.startflowmeshvalidator()
            assert_equal(started["running"], True)
            assert_equal(started["armed_keys"], 1)
        # Activation and the bootstrap transaction were pinned exactly above.
        # This is a minimum-depth wait, not another activation-boundary test:
        # concurrent honest staking may finish one extra block before stop.
        self.mine_pos_blocks(A3 - n0.getblockcount(), allow_overshoot=True)
        assert n0.getblockcount() >= A3
        self.wait_for_market_convergence(market_id, require_unpaused=False)
        checkpoint = self.publish_checkpoint(market_id)
        assert_equal(checkpoint["sequence"], 0)
        assert_equal(checkpoint["effect_count"], 0)
        self.last_market = market_id
        return market_id, asset

    def fund_latency_client(self, market_id, asset):
        self.start_ordinary_client()
        self.wait_until(lambda: any(row["market_id"] == market_id for row in self.client.listflowmeshmarkets()), timeout=60)
        funding = self.nodes[0].sendtoaddress(self.client.getnewaddress(), Decimal("3"))
        self.synchronize_mempools()
        self.mine_pos_blocks(1, allow_overshoot=True)
        self.wait_client_b3_sync()
        assert self.client.gettransaction(funding)["confirmations"] >= 1
        deposit = self.client.flowmeshdeposit(asset, "B3", Decimal("1"))
        assert_equal(deposit["market_id"], market_id)
        self.publish_client_transaction(deposit)
        self.mine_pos_blocks(30, allow_overshoot=True)
        self.wait_client_b3_sync()
        self.observe_action(market_id, "submitflowmeshdeposit",
                            (market_id, deposit["deposit_txid"], deposit["deposit_vout"]), "latency_setup_deposit")
        account = self.wait_client_balance(market_id, lambda row: row["b3_available"] == Decimal("1"))
        assert_equal(account["base_available"], 0)
        operations = self.publish_until_vault_operations(market_id, 1)
        sweep = next(row for row in operations if row["account_id"] == account["account_id"] and row["kind"] == "deposit-sweep")
        transaction = self.client.createflowmeshvaulttx(sweep["effect_id"])
        self.publish_client_transaction(transaction)
        self.wait_for_market_convergence(market_id)
        self.drain_relay_observations()
        return self.client_balance(market_id)

    def drain_relay_observations(self):
        """Archive completed submit records; bound read-only poll bookkeeping.

        Only completed relay observations are removed, under the relay lock.
        In-flight records remain owned by their handlers. This does not alter
        messages, fault configuration, queue limits, or production behavior.
        """
        for relay in self.tls_relays:
            with relay.lock:
                assert_equal(relay.records_dropped, 0)
                assert not relay.unavailable and not relay.drop_submit_once
                assert relay.reply_mutation is None and not relay.response_hold_ms
                completed = [row for row in relay.requests if "handler_completed_us" in row]
                relay.requests[:] = [row for row in relay.requests if "handler_completed_us" not in row]
                # Re-reserve conservatively for the few unfinished handlers.
                relay.record_bytes = sum(len(json.dumps(row).encode("utf-8")) + 1024 for row in relay.requests)
                for row in completed:
                    self.relay_method_counts[row["method"]] += 1
                    if row["method"] == "submit":
                        self.submit_records.append({"endpoint": relay.url, **dict(row)})
        assert len(self.submit_records) <= 4096, "bounded signed-action observation archive exceeded"

    def calibrate_clocks(self, market_id, phase):
        for index, node in enumerate(self.nodes):
            before = host_us()
            info = node.getflowmeshdeliveryinfo(market_id)
            after = host_us()
            assert_equal(len(info["markets"]), 1)
            row = info["markets"][0]
            assert_equal(row["market_id"], market_id)
            assert_equal(row["trace_events_dropped"], 0)
            assert_equal(row["trace_global_events_dropped"], 0)
            sampled = row["sampled_monotonic_us"]
            self.clock_samples[str(index)].append({"phase": phase, "host_before_us": before,
                                                   "host_after_us": after, "node_sampled_us": sampled,
                                                   "offset_us_bounds": [before - sampled, after - sampled]})

    @staticmethod
    def contains_target(data, sequence, block_hash):
        snapshot = data["snapshot"]
        assert_equal(snapshot["running"], True)
        assert_equal(snapshot["halt"], "none")
        if not snapshot["certified"] or snapshot["next_microblock_sequence"] <= sequence:
            return False
        if snapshot["next_microblock_sequence"] == sequence + 1:
            return snapshot["last_microblock_hash"] == block_hash
        return any(row["sequence"] == sequence and row["microblock_hash"] == block_hash for row in data["history"]["entries"])

    def timed_action(self, market_id, phase, pair, kind, sequence):
        self.pump_b3()
        method = "submitflowmeshorder" if kind == "bid" else "cancelflowmeshorder"
        params = (market_id, "bid", TRADE_PRICE // 2, 1, sequence) if kind == "bid" else (market_id, "bid", sequence)
        sample = {"phase": phase, "pair": pair, "kind": kind, "account_sequence": sequence,
                  "attempts": [], "replica_observations": {}, "receipt_observations": [], "complete": False}
        if self.options.latency_production_logging:
            sample["debug_trace_observations"] = {"availability": "unknown", "reason": "BENCH disabled",
                                                   "replica_durable_timestamp_count": 0,
                                                   "replica_admission_timestamp_count": 0}
        self.latency_samples.append(sample)
        start = host_us()
        sample["initial_submission_host_us"] = start
        deadline = time.monotonic() + 90
        response = None
        while response is None:
            before = host_us()
            try:
                response = getattr(self.client, method)(*params)
            except JSONRPCException as error:
                if error.error.get("code") != -1 or error.error.get("message") not in PRE_ADMISSION_REJECTIONS:
                    raise
                sample["attempts"].append({"before_host_us": before, "after_host_us": host_us(),
                                           "explicit_pre_admission_rejection": error.error["message"]})
                assert time.monotonic() < deadline, sample
                self.pump_b3()
                time.sleep(self.options.latency_poll_ms / 1000)
        initial_observed = host_us()
        sample.update(action_id=response["action_id"], initial_response=response,
                      initial_response_host_us=initial_observed,
                      initial_response_ms=(initial_observed - start) / 1000)
        assert_equal(response["sequence"], sequence)
        action_id = response["action_id"]
        assert len(action_id) == 64
        status = response
        status_observed = initial_observed
        target = None
        next_retry = time.monotonic() + 1
        while True:
            state = status["receipt_state"]
            assert_equal(status["action_id"], action_id)
            assert state in {"queued", "admitted", "rejected", "unknown", "certified_inclusion"}, status
            if not sample["receipt_observations"] or state != sample["receipt_observations"][-1]["receipt_state"]:
                sample["receipt_observations"].append({"receipt_state": state, "observed_host_us": status_observed})
            if state == "admitted" and "admitted_observed_host_us" not in sample:
                sample["admitted_observed_host_us"] = status_observed
                sample["admitted_observed_ms"] = (status_observed - start) / 1000
            retryable = state == "unknown" or (state == "rejected" and status["reason"] in PRE_ADMISSION_REJECTIONS)
            assert state != "rejected" or retryable, status
            if state == "certified_inclusion" and target is None:
                assert_equal(status["certificate_verified"], True)
                assert_equal(status["outcome_verified"], False)
                target = (status["microblock_sequence"], status["microblock_hash"])
                sample.update(certified_status=status, client_certified_host_us=status_observed,
                              client_certified_ms=(status_observed - start) / 1000)
            if target is not None:
                # Same host clock as submission. Serial RPC observation is an
                # explicit upper bound, not a timestamp of disk completion.
                for index, node in enumerate(self.nodes):
                    if str(index) in sample["replica_observations"]:
                        continue
                    before = host_us()
                    data = node.getflowmeshmarketdata(market_id, {"limit": 100, "curve_limit": 1})
                    after = host_us()
                    if self.contains_target(data, *target):
                        sample["replica_observations"][str(index)] = {"request_host_us": before,
                            "observed_host_us": after, "observed_ms": (after - start) / 1000,
                            "sequence": target[0], "hash": target[1]}
                if len(sample["replica_observations"]) == 4:
                    break
            assert time.monotonic() < deadline, f"action certification/replica observation timed out: {sample}"
            if retryable and time.monotonic() >= next_retry:
                before = host_us()
                retry = self.client.retryflowmeshaction(market_id, action_id)
                assert_equal(retry["action_id"], action_id)
                sample["attempts"].append({"same_signed_action_retry": True, "before_host_us": before,
                                           "after_host_us": host_us(), "receipt_state": retry["receipt_state"]})
                next_retry = time.monotonic() + 1
            self.pump_b3()
            time.sleep(self.options.latency_poll_ms / 1000)
            if target is None:
                status = self.client.getflowmeshactionstatus(market_id, action_id)
                status_observed = host_us()
        sample["every_replica_observed_ms"] = max(row["observed_ms"] for row in sample["replica_observations"].values())
        sample["complete"] = True
        self.drain_relay_observations()
        return sample

    def verify_signed_actions(self, market_id):
        self.drain_relay_observations()
        retained = self.client.listflowmeshactions(market_id)
        assert_equal(retained["source"], "local-retained-outbox")
        saved = {row["action_id"]: row for row in retained["actions"]}
        assert_equal(len({row["action_id"] for row in self.latency_samples}), len(self.latency_samples))
        for sample in self.latency_samples:
            action = saved[sample["action_id"]]
            assert_equal(action["sequence"], sample["account_sequence"])
            copies = [row for row in self.submit_records if row["action_id"] == sample["action_id"]]
            assert copies, sample
            assert all(row["forwarded"] and not row["response_dropped"] for row in copies)
            exact = {row["action_hex"] for row in copies}
            assert_equal(len(exact), 1)
            raw = bytes.fromhex(exact.pop())
            assert_equal(hashlib.sha256(raw).hexdigest(), action["signed_bytes_sha256"])
            assert_equal(len(raw), action["signed_bytes_size"])
            sample["signed_action"] = {"sha256": action["signed_bytes_sha256"], "size": len(raw),
                                        "https_submit_count": len(copies), "exact_bytes_preserved": True}
        self.latency_report["https_submit_records"] = self.submit_records
        self.latency_report["https_method_counts"] = dict(self.relay_method_counts)

    def attach_durable_observations(self, market_id):
        """Join exact action/block identities; never subtract clock origins."""
        for index, node in enumerate(self.nodes):
            key = str(index)
            calibrations = self.clock_samples[key]
            lower = max(row["offset_us_bounds"][0] for row in calibrations)
            upper = min(row["offset_us_bounds"][1] for row in calibrations)
            assert lower <= upper, f"node {index} clock-offset intervals do not overlap"
            self.latency_report["clock_offsets"][key] = {"offset_us_bounds": [lower, upper],
                                                        "uncertainty_us": upper - lower,
                                                        "calibrations": calibrations}
            wanted_actions = {row["action_id"] for row in self.latency_samples}
            wanted_blocks = {row["certified_status"]["microblock_hash"] for row in self.latency_samples}
            admissions, durable, included = {}, {}, {}
            assert node.debug_log_path.stat().st_size < 192 * 1024 * 1024
            with node.debug_log_path.open(encoding="utf-8") as stream:
                for line in stream:
                    if "FlowMeshTrace " not in line:
                        continue
                    event = json.loads(line.split("FlowMeshTrace ", 1)[1])
                    if event.get("market_id") != market_id:
                        continue
                    identity = event["object_id"]
                    if event["stage"] == "action_verified" and identity in wanted_actions:
                        admissions.setdefault(identity, event)
                    if event["stage"] == "durably_applied" and identity in wanted_blocks:
                        assert identity not in durable, f"duplicate durable apply: {identity} on node {index}"
                        durable[identity] = event
                    if event["stage"] == "action_certified" and identity in wanted_actions:
                        assert identity not in included, f"duplicate action certification: {identity} on node {index}"
                        included[identity] = event
            for sample in self.latency_samples:
                action_id = sample["action_id"]
                status = sample["certified_status"]
                event = durable[status["microblock_hash"]]
                assert_equal(event["sequence"], status["microblock_sequence"])
                assert_equal(included[action_id]["related_object_id"], status["microblock_hash"])
                raw_time = event["monotonic_us"]
                start = sample["initial_submission_host_us"]
                bounds = [(raw_time + offset - start) / 1000 for offset in (lower, upper)]
                assert bounds[1] >= 0, "durable apply cannot predate initial submission"
                sample.setdefault("replica_durable", {})[key] = {"event": event, "latency_ms_bounds": bounds,
                    "host_timestamp_us_bounds": [raw_time + lower, raw_time + upper]}
                # A receiver can learn the action first through its certified
                # entry; lack of pool admission is not an invented zero time.
                admission = admissions.get(action_id)
                sample.setdefault("replica_admission", {})[key] = None if admission is None else {
                    "event": admission,
                    "latency_ms_bounds": [(admission["monotonic_us"] + offset - start) / 1000 for offset in (lower, upper)]}
        for sample in self.latency_samples:
            sample["every_replica_durable_upper_ms"] = max(row["latency_ms_bounds"][1] for row in sample["replica_durable"].values())
            admissions = [row["latency_ms_bounds"][1] for row in sample["replica_admission"].values() if row is not None]
            assert admissions, "no observed authenticated admission on any operator"
            sample["first_operator_admission_upper_ms"] = min(admissions)

    def summarize(self):
        report = self.latency_report
        metrics = ("initial_response_ms", "admitted_observed_ms", "first_operator_admission_upper_ms",
                   "client_certified_ms", "every_replica_observed_ms", "every_replica_durable_upper_ms")
        for phase in ("warmup", "measured"):
            rows = [row for row in self.latency_samples if row["phase"] == phase]
            report["summary"][phase] = {"actions": len(rows), "completed": sum(row["complete"] for row in rows),
                "bid_count": sum(row["kind"] == "bid" for row in rows), "cancel_count": sum(row["kind"] == "cancel" for row in rows),
                "explicit_pre_admission_rejections": sum("explicit_pre_admission_rejection" in attempt for row in rows for attempt in row["attempts"]),
                "same_signed_action_retries": sum(attempt.get("same_signed_action_retry", False) for row in rows for attempt in row["attempts"]),
                "metrics": {metric: distribution([row[metric] for row in rows if metric in row], self.options.latency_target_ms)
                            for metric in metrics},
                "replicas": {str(index): {
                    "observed": distribution([row["replica_observations"][str(index)]["observed_ms"] for row in rows
                                              if str(index) in row["replica_observations"]], self.options.latency_target_ms),
                    "durable_upper": distribution([row["replica_durable"][str(index)]["latency_ms_bounds"][1] for row in rows
                                                   if str(index) in row.get("replica_durable", {})], self.options.latency_target_ms)}
                             for index in range(4)}}
        measured = report["summary"]["measured"]
        gates = ("client_certified_ms", "every_replica_observed_ms", "every_replica_durable_upper_ms")
        if self.options.latency_production_logging:
            # No observation is not a zero-millisecond event or a passing
            # durable-timestamp sample. Keep unknown metrics explicit.
            unknown = {"availability": "unknown", "reason": "BENCH disabled; exact trace timestamps not reconstructed",
                       "count": 0, "over_target_count": None, "p50_ms": None,
                       "p95_ms": None, "p99_ms": None, "max_ms": None}
            for phase in ("warmup", "measured"):
                for metric in ("first_operator_admission_upper_ms", "every_replica_durable_upper_ms"):
                    report["summary"][phase]["metrics"][metric] = dict(unknown)
                for replica in report["summary"][phase]["replicas"].values():
                    replica["durable_upper"] = dict(unknown)
            gates = ("client_certified_ms", "every_replica_observed_ms")
        report["performance_pass"] = bool(report["correctness_pass"] and measured["actions"] == self.options.latency_measured_pairs * 2 and
            all(measured["metrics"][metric]["count"] == measured["actions"] and
                measured["metrics"][metric]["over_target_count"] == 0 for metric in gates))
        report["performance_gate_metrics"] = list(gates)

    def run_test(self):
        warm, measured = self.options.latency_warmup_pairs, self.options.latency_measured_pairs
        assert warm >= 0 and measured >= 1 and warm + measured <= 200, "require 0 <= warmup, 1 <= measured, total <= 200 pairs"
        assert self.options.latency_poll_ms <= 100, "poll interval must be <= 100 ms"
        assert_equal(self.options.qt_review_hold_seconds, 0)
        report_path = Path(self.options.tmpdir) / "flowmesh-latency.json"
        self.latency_report = {"fixture": "healthy_local_four_operator_https_preagreement", "format_version": 1,
            "operators": 4, "ordinary_clients": 1, "target_ms": self.options.latency_target_ms,
            "warmup_pairs": warm, "measured_pairs": measured, "poll_interval_ms": self.options.latency_poll_ms,
            "observe_only": self.options.latency_observe_only, "correctness_pass": False, "performance_pass": False,
            "logging_mode": "production_debug_disabled" if self.options.latency_production_logging else "detailed_bench",
            "logging_checks": [],
            "summary": {}, "clock_offsets": {}, "samples": self.latency_samples,
            "scope": {"faults_injected": False, "concurrent_signed_actions": 1, "transport": "ordinary HTTPS via transparent test TLS relay",
                "client_certificate_is_execution_result": False, "durability": "runtime synchronous durable-append event, not crash/power-loss qualification",
                "observed_times": "same Python monotonic clock; serial RPC observation includes polling/HTTPS/observer overhead",
                "durable_times": "per-node clock mapped through intersected before/after RPC offset bounds; upper bound gates performance",
                "percentiles": "nearest rank; warmup excluded; every measured action must meet target",
                "not_qualified": ["WAN", "high concurrency", "sustained throughput", "universal latency guarantee", "fault recovery"]}}
        if self.options.latency_production_logging:
            self.latency_report["scope"].update(
                durability="all four replicas expose the exact certificate in their applied canonical history; observation is an upper bound, not a disk timestamp or crash qualification",
                durable_times="unknown: BENCH debug-file timestamps are not collected/reconstructed in production-logging mode",
                admission_times="client receipt observations retained; operator debug-event timestamps unknown",
                debug_logging="-debug=0 appended to generated operator/client startup and restart arguments; ordinary non-debug daemon logging remains enabled")
        try:
            market_id, asset = self.bootstrap_latency_market()
            self.latency_report["market_id"] = market_id
            account = self.fund_latency_client(market_id, asset)
            initial_sequence = account["next_sequence"]
            self.latency_report["initial_account_sequence"] = initial_sequence
            self.latency_report["operator_subversions"] = [node.getnetworkinfo()["subversion"] for node in self.nodes]
            for phase, pairs in (("warmup", warm), ("measured", measured)):
                if self.options.latency_production_logging:
                    self.verify_production_logging(market_id, phase + "_before")
                else:
                    self.calibrate_clocks(market_id, phase + "_before")
                self.begin_b3_workload(range(4))
                heights_before = [node.getblockcount() for node in self.nodes]
                try:
                    for pair in range(pairs):
                        for kind in ("bid", "cancel"):
                            self.timed_action(market_id, phase, pair, kind, account["next_sequence"])
                            previous_sequence = account["next_sequence"]
                            account = self.wait_client_balance(market_id, lambda row: row["next_sequence"] == previous_sequence + 1)
                            assert_equal(account["next_sequence"], previous_sequence + 1)
                            assert_equal(account["base_available"], 0)
                            reserved = Decimal("0.05") if kind == "bid" else Decimal("0")
                            assert_equal(account["b3_reserved"], reserved)
                            assert_equal(account["b3_available"], Decimal("1") - reserved)
                            self.latency_samples[-1]["certified_account_state_checked"] = True
                        self.log.info("FlowMesh latency %s pair %d/%d complete", phase, pair + 1, pairs)
                    heights_after = [node.getblockcount() for node in self.nodes]
                    self.latency_report[phase + "_b3_heights"] = {"before": heights_before, "after": heights_after}
                    if phase == "measured":
                        assert all(after > before for before, after in zip(heights_before, heights_after)), "B3 must advance during the measured workload"
                finally:
                    self.end_b3_workload()
                if self.options.latency_production_logging:
                    self.verify_production_logging(market_id, phase + "_after")
                else:
                    self.calibrate_clocks(market_id, phase + "_after")
            assert_equal(account["next_sequence"], initial_sequence + 2 * (warm + measured))
            assert_equal(account["b3_available"], Decimal("1"))
            assert_equal(account["b3_reserved"], Decimal("0"))
            self.wait_for_independent_mesh()
            self.assert_no_b3_flowmesh_traffic()
            self.assert_engine_off()
            self.verify_signed_actions(market_id)
            if not self.options.latency_production_logging:
                self.attach_durable_observations(market_id)
            self.latency_report["correctness_pass"] = True
        except Exception as error:
            self.latency_report["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            self.summarize()
            report_path.write_text(json.dumps(self.latency_report, sort_keys=True, indent=2, default=str) + "\n", encoding="utf-8")
            self.log.info("FLOWMESH_LATENCY_REPORT %s correctness_pass=%s performance_pass=%s", report_path,
                          self.latency_report["correctness_pass"], self.latency_report["performance_pass"])
            self.log.info("FLOWMESH_LATENCY_SUMMARY %s", json.dumps(self.latency_report["summary"], sort_keys=True))
        if not self.options.latency_observe_only:
            assert self.latency_report["performance_pass"], f"measured latency target missed; see {report_path}"


if __name__ == "__main__":
    FlowMeshLatencyTest(__file__).main()
