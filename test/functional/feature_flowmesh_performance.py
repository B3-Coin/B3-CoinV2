#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Opt-in, bounded open-loop real-daemon FlowMesh performance experiment.

Four generated regtest validators use independent TCP. One engine-off HTTPS
client hosts four generated buyer wallets and a generated maker. The maker's
credited standing ask supplies actual fills; buyers cycle fill/bid/cancel.
Every offered arrival is retained, including queue drops and failed actions.
This is a single-client-process experiment, not independent-client/WAN load.

Run directly with --configfile=<optimized-build>/test/config.ini --nocleanup.
Default: 12-action pilot, 10-second idle, two 20-second windows at each of
0.5/1/2/4 actions/sec, stopping rate escalation on a gate miss, then 8-action
burst if correctness remains intact. --performance-markets=2 adds a second
fresh market before the single pre-bootstrap restart. Production debug logging
is disabled unless --performance-diagnostic is explicitly selected.
"""

import argparse
import hashlib
import json
import math
import queue
import statistics
import sys
import threading
import time
from decimal import Decimal
from pathlib import Path
from urllib.parse import quote

from feature_flowmesh_latency import FlowMeshLatencyTest, PRE_ADMISSION_REJECTIONS, distribution, host_us
from feature_flowmesh_release import TEST_ASSET_DEPOSIT, TEST_ASSET_SUPPLY, TRADE_PRICE
from test_framework.authproxy import JSONRPCException
from test_framework.flowmesh_public_trace import attribute_requests, public_response_context, reply_rejected
from test_framework.util import assert_equal, get_rpc_proxy, rpc_url


RATES = (0.5, 1.0, 2.0, 4.0)
BUYERS = 4
KINDS = ("fill_bid", "resting_bid", "cancel")
BALANCE_FIELDS = ("base_available", "base_reserved", "b3_available_atoms", "b3_reserved_atoms")
MAX_HTTP_TRACE_RECORDS = 32768
MAX_HTTP_TRACE_BYTES = 64 * 1024 * 1024
MAX_RPC_TRACE_RECORDS = 32768
MAX_RPC_TRACE_BYTES = 256 * 1024 * 1024
MAX_RPC_CONTEXT_BYTES = 32 * 1024
READ_RECOVERY_DELAYS = (.5, 1.0, 2.0, 4.0)
PUBLIC_TRACE_QUIESCENCE_SECONDS = 15


def bounded_seconds(value):
    value = float(value)
    if not math.isfinite(value) or not 1 <= value <= 60:
        raise argparse.ArgumentTypeError("must be finite and in 1..60 seconds")
    return value


def arrival_offsets(count, rate):
    """Predetermined arrivals, never completion-relative (including bursts)."""
    if count < 0 or rate < 0:
        raise ValueError("negative schedule")
    return [0.0 if rate == 0 else index / rate for index in range(count)]


def expected_balance(before, kind):
    after = {field: before[field] for field in BALANCE_FIELDS}
    if kind == "fill_bid":
        after["base_available"] += 1
        after["b3_available_atoms"] -= TRADE_PRICE
    elif kind == "resting_bid":
        after["b3_available_atoms"] -= TRADE_PRICE // 2
        after["b3_reserved_atoms"] += TRADE_PRICE // 2
    elif kind == "cancel":
        after["b3_available_atoms"] += TRADE_PRICE // 2
        after["b3_reserved_atoms"] -= TRADE_PRICE // 2
    else:
        raise ValueError("unknown action kind")
    return after


def compact_error(error):
    """Never retain RPC URL/authentication details from transport exceptions."""
    result = {"type": type(error).__name__}
    if isinstance(error, JSONRPCException):
        result["rpc_code"] = error.error.get("code")
        result["rpc_message"] = error.error.get("message")
    elif isinstance(error, AssertionError):
        result["assertion"] = str(error)[:1000]
    return result


class ObservedClientRPC:
    """Measure existing wallet RPC calls; never inject API fields or headers."""
    def __init__(self, owner, rpc, wallet):
        self.owner, self.rpc, self.wallet = owner, rpc, wallet

    def __getattr__(self, method):
        call = getattr(self.rpc, method)
        def observed(*args, **kwargs):
            sample = getattr(self.owner.trace_local, "sample", None)
            row = {"method": method, "wallet": self.wallet, "start_host_us": host_us()}
            if sample is not None:
                row.update(sample_id=sample["sample_id"], market_id=sample["market_id"],
                           account_id=sample.get("account_id"))
            with self.owner.report_lock:
                reserved = MAX_RPC_CONTEXT_BYTES + 2048
                if (len(self.owner.rpc_calls) < MAX_RPC_TRACE_RECORDS and
                        self.owner.rpc_trace_bytes + reserved <= MAX_RPC_TRACE_BYTES):
                    self.owner.rpc_calls.append(row)
                    self.owner.rpc_trace_bytes += reserved
                else:
                    self.owner.rpc_trace_dropped += 1
            try:
                result = call(*args, **kwargs)
                row["end_host_us"] = host_us()
                self.owner.trace_local.last_rpc_return_us = row["end_host_us"]
            except Exception as error:
                row.setdefault("end_host_us", host_us())
                self.owner.trace_local.last_rpc_return_us = row["end_host_us"]
                row["error"] = compact_error(error)
                raise
            finally:
                # Diagnostic failures must not replace an original RPC error
                # or turn a successful signing call into an ambiguous retry.
                try:
                    self.owner.drain_relay_observations()
                except Exception as error:
                    row["capture_error"] = compact_error(error)
            try:
                row["response_context"] = public_response_context(json.dumps({"result": result}, default=str).encode())
                if len(json.dumps(row["response_context"]).encode()) > MAX_RPC_CONTEXT_BYTES:
                    row["response_context"] = {"inline_context_omitted_for_size": True}
                    row["capture_error"] = {"reason": "wallet response context exceeded bounded reservation"}
            except Exception as error:
                row["capture_error"] = compact_error(error)
            return result
        return observed


def submit_context(record):
    """Semantic ActionIds are market-scoped; never join on ActionId alone.

    The retained public HTTPS body is the authority for this observation.
    Do not filter by the expected signed-byte hash: that would conceal a real
    changed-payload retry inside the same market.
    """
    body = json.loads(bytes.fromhex(record["body_hex"]))
    assert_equal(body["method"], "submit")
    params = body["params"]
    assert_equal(params["action_id"], record["action_id"])
    assert_equal(params["action_hex"], record["action_hex"])
    return params["market_id"], params["action_id"]


def summarize_window(samples, backlog, duration, elapsed_to_drain=None):
    complete = [row for row in samples if row["status"] == "complete"]
    metrics = {}
    for field in ("client_certified_ms", "client_certified_from_offer_ms", "admitted_observed_ms",
                  "account_state_verified_ms", "account_state_verified_from_offer_ms",
                  "every_replica_observed_ms", "initial_response_ms", "worker_queue_ms",
                  "scheduler_lateness_ms"):
        metrics[field] = distribution([row[field] for row in samples if field in row], 600)
    certification = metrics["client_certified_ms"]
    edge = max(1, min(30, len(backlog) // 4))
    early = statistics.median([row["outstanding"] for row in backlog[:edge]]) if backlog else 0
    late = statistics.median([row["outstanding"] for row in backlog[-edge:]]) if backlog else 0
    queue_growth = late >= early + 2 and late >= 3
    filled = [row for row in complete if row.get("kind") == "fill_bid"]
    read_consistency = all(row.get("read_consistency_pass", True) for row in samples)
    in_window_fills = sum(row.get("account_verified_during_offer_window", False) for row in filled)
    return {
        "offered": len(samples), "completed": len(complete),
        "certified_inclusion_count": sum("client_certified_host_us" in row for row in samples),
        "read_consistency_pass": read_consistency,
        "failed_followup_read_count": sum(len(row.get("read_consistency_failures", [])) for row in samples),
        "read_recovered_action_count": sum(row.get("account_read_recovered", False) for row in samples),
        "failed": sum(row["status"] == "failed" for row in samples),
        "dropped": sum(row["status"] == "dropped" for row in samples),
        "unresolved": sum(row["status"] not in {"complete", "failed", "dropped"} for row in samples),
        "certified_during_offer_window": sum(row.get("certified_during_offer_window", False) for row in samples),
        "account_verified_during_offer_window": sum(row.get("account_verified_during_offer_window", False) for row in samples),
        "completed_during_offer_window": sum(row.get("completed_during_offer_window", False) for row in samples),
        "certified_actions_per_offer_second": sum(row.get("certified_during_offer_window", False) for row in samples) / duration if duration else None,
        "completed_actions_per_offer_second": sum(row.get("completed_during_offer_window", False) for row in samples) / duration if duration else None,
        "completed_actions_per_elapsed_second": len(complete) / elapsed_to_drain if elapsed_to_drain else None,
        "elapsed_seconds_through_drain": elapsed_to_drain,
        "admitted_observed_count": sum("admitted_observed_host_us" in row for row in samples),
        "by_kind": {kind: {state: sum(row.get("kind") == kind and row["status"] == state for row in samples)
                           for state in ("complete", "failed", "dropped")}
                    for kind in KINDS},
        "authenticated_filled_units": len(filled),
        "authenticated_filled_units_during_offer_window": in_window_fills,
        "authenticated_filled_units_per_offer_second": in_window_fills / duration if duration else None,
        "quantile_scope": "nearest-rank empirical sample; small samples do not establish tail guarantees",
        "few_samples": len(complete) < 100,
        "metrics": metrics, "queue_growth": queue_growth,
        "early_outstanding_median": early, "late_outstanding_median": late,
        "workload_completion_pass": len(complete) == len(samples) and bool(samples),
        "correctness_pass": len(complete) == len(samples) and bool(samples) and read_consistency,
        "performance_pass": bool(samples) and len(complete) == len(samples) and read_consistency and not queue_growth and
            certification["p50_ms"] <= 200 and certification["p95_ms"] <= 600,
        "offer_inclusive_target_pass": bool(samples) and len(complete) == len(samples) and
            metrics["client_certified_from_offer_ms"]["p50_ms"] <= 200 and
            metrics["client_certified_from_offer_ms"]["p95_ms"] <= 600,
    }


class FlowMeshPerformanceTest(FlowMeshLatencyTest):
    def add_options(self, parser):
        super().add_options(parser)
        parser.add_argument("--performance-diagnostic", action="store_true",
                            help="Enable inherited BENCH logging; never mix with production headline samples")
        parser.add_argument("--performance-markets", type=int, choices=(1, 2), default=1)
        parser.add_argument("--performance-window-seconds", type=bounded_seconds, default=20)
        parser.add_argument("--performance-idle-seconds", type=bounded_seconds, default=10)
        parser.add_argument("--performance-action-timeout", type=bounded_seconds, default=60)
        parser.add_argument("--performance-fail-on-gate", action="store_true",
                            help="Also fail process exit on a measured performance miss; correctness always gates exit")
        parser.add_argument("--performance-profile", choices=("ladder", "matched-repair", "native-queue"), default="ladder",
                            help="matched-repair fixes two markets, 20-second windows, two repeats at 0.5/s, and burst8")
        parser.add_argument("--performance-public-trace", action="store_true",
                            help="Bounded full public HTTPS bodies plus RPC/context accounting in generated fixture only")
        parser.add_argument("--performance-memory-trace", action="store_true",
                            help="Start bounded regtest-only memory timing after setup; freeze after measured work")
        parser.add_argument("--performance-read-recovery", action="store_true",
                            help="Optional bounded read-only follow-up recovery; preserves read failure and fails final exit")

    def set_test_params(self):
        if self.options.performance_profile == "matched-repair":
            self.options.performance_markets = 2
            self.options.performance_window_seconds = 20
            self.options.performance_idle_seconds = 10
        self.performance_rates = (.5,) if self.options.performance_profile == "matched-repair" else RATES
        if self.options.performance_profile == "native-queue":
            self.options.performance_markets = 2
            self.options.performance_idle_seconds = 1
            self.performance_rates = ()
        if self.options.performance_memory_trace and self.options.performance_diagnostic:
            raise ValueError("Memory capture requires BENCH logging disabled")
        self.options.latency_production_logging = not self.options.performance_diagnostic
        super().set_test_params()
        self.market_specs = []
        self.workers = []
        self.worker_stop = threading.Event()
        self.scheduler_stop = threading.Event()
        self.report_lock = threading.Lock()
        self.performance_report = {}
        self.offered_counter = 0
        self.threads = []
        self.http_requests = []
        self.http_trace_bytes = self.http_trace_dropped = self.rpc_trace_dropped = self.rpc_trace_bytes = 0
        self.rpc_calls = []
        self.trace_local = threading.local()
        self.trace_drain_lock = threading.Lock()
        self.memory_capture_nodes = []

    def begin_memory_capture(self):
        if not self.options.performance_memory_trace:
            return
        captures = self.performance_report.setdefault("memory_capture", {})
        for node in [*self.nodes, self.client]:
            index = node.index
            before = host_us()
            result = node.flowmeshtiming("start")
            after = host_us()
            self.memory_capture_nodes.append(index)
            captures[str(index)] = {"start": result, "start_host_us_bounds": [before, after],
                "offset_us_bounds": [before - result["start_us"], after - result["start_us"]]}

    def stop_nodes(self, wait=0):
        # Preserve actual Popen outcomes even after the framework releases its handles.
        processes = [(node.index, node.process) for node in self.nodes if node.process]
        try:
            super().stop_nodes(wait)
        finally:
            path = Path(self.options.tmpdir, "native-child-exits.jsonl")
            with path.open("a") as output:
                for index, process in processes:
                    output.write(json.dumps({"node": index, "pid": process.pid,
                        "returncode": process.poll(), "observed_host_us": host_us()}) + "\n")

    def end_memory_capture(self):
        for index in self.memory_capture_nodes:
            node = self.client if index == self.client.index else self.nodes[index]
            result = node.flowmeshtiming("stop")
            path = Path(self.options.tmpdir, f"memory-timing-node{index}.json")
            path.write_text(json.dumps(result, indent=2) + "\n")
            limited = sum(row["event"].get("stage") == "trace_limit_reached" for row in result["events"])
            self.performance_report["memory_capture"][str(index)].update(
                file=path.name, count=len(result["events"]), dropped=result["dropped"], limit_markers=limited)
            if result["dropped"] or limited:
                self.invalidate_final_result()
                raise AssertionError("Incomplete bounded timing capture; retain partial evidence")
        self.memory_capture_nodes.clear()

    def shutdown(self):
        process = self.client.process if self.client is not None else None
        try:
            return super().shutdown()
        finally:
            if process is not None and Path(self.options.tmpdir).exists():
                with Path(self.options.tmpdir, "native-child-exits.jsonl").open("a") as output:
                    output.write(json.dumps({"node": self.client.index, "pid": process.pid,
                        "returncode": process.poll(), "observed_host_us": host_us()}) + "\n")

    def start_ordinary_client(self):
        super().start_ordinary_client()
        if self.options.performance_public_trace:
            for index, relay in enumerate(self.tls_relays):
                relay.enable_public_capture(Path(self.options.tmpdir, "public-http-trace", f"endpoint{index}"))
            self.public_trace_started_host_us = host_us()

    def drain_relay_observations(self):
        if not self.options.performance_public_trace:
            return super().drain_relay_observations()
        with self.trace_drain_lock:
            return self._drain_public_relay_observations()

    def _drain_public_relay_observations(self):
        for relay in self.tls_relays:
            with relay.lock:
                assert not relay.unavailable and not relay.drop_submit_once
                assert relay.reply_mutation is None and not relay.response_hold_ms
                # Observation bounds never alter forwarding or request limits.
                # Any lost record is retained as a failing capture counter.
                completed = [row for row in relay.requests if "handler_completed_us" in row]
                relay.requests[:] = [row for row in relay.requests if "handler_completed_us" not in row]
                relay.record_bytes = sum(len(json.dumps(row).encode()) + row.get("completion_reservation_bytes", 1024)
                                         for row in relay.requests)
                for original in completed:
                    row = {"endpoint": relay.url, **dict(original)}
                    self.relay_method_counts[row["method"]] += 1
                    if row["method"] == "submit":
                        self.submit_records.append(row)
                    size = len(json.dumps(row).encode())
                    if len(self.http_requests) < MAX_HTTP_TRACE_RECORDS and self.http_trace_bytes + size <= MAX_HTTP_TRACE_BYTES:
                        self.http_requests.append(row)
                        self.http_trace_bytes += size
                    else:
                        self.http_trace_dropped += 1
        assert len(self.submit_records) <= 4096, "bounded signed-action observation archive exceeded"

    def configure_fresh_market(self, market_id):
        """Select all generated fresh-market modes in one pre-bootstrap stop."""
        assert self.preagreement_market is None and not self.pos_running
        self.preagreement_market = market_id
        markets = [market_id]
        if self.options.performance_markets == 2:
            node = self.nodes[0]
            asset = node.issueasset(TEST_ASSET_SUPPLY, 2)["asset_id"]
            bootstrap = node.flowmeshdeposit(asset, asset, TEST_ASSET_DEPOSIT,
                {"minconf": 0, "include_unsafe": True, "market_bootstrap": True})
            self.market_specs.append({"market_id": bootstrap["market_id"], "asset": asset})
            markets.append(bootstrap["market_id"])
        self.synchronize_mempools()
        mempool_before = set(self.nodes[0].getrawmempool())
        self.stop_nodes()
        for index, args in enumerate(self.extra_args):
            for market in markets:
                flag = f"-flowmeshpreagreementmarket={market}"
                if flag not in args:
                    args.append(flag)
                if flag not in self.nodes[index].extra_args:
                    self.nodes[index].extra_args.append(flag)
        self.start_nodes()
        self.set_chain_time(self.mock_time)
        for index in range(1, self.num_nodes):
            self.connect_nodes(index, index - 1)
        self.sync_blocks()
        assert mempool_before.issubset(set(self.nodes[0].getrawmempool()))
        self.wait_for_independent_mesh()

    def independent_rpc(self, node, wallet=None):
        # Unlike TestNode.get_wallet_rpc(), this creates a NEW HTTP connection.
        proxy = get_rpc_proxy(rpc_url(node.datadir_path, node.index, self.chain, node.rpchost),
                              node.index, timeout=15, coveragedir=self.options.coveragedir)
        rpc = proxy if wallet is None else proxy / ("wallet/" + quote(wallet, safe=""))
        if self.options.performance_public_trace and node is self.client and wallet is not None:
            return ObservedClientRPC(self, rpc, wallet)
        return rpc

    def observed_rpc_return_us(self):
        # Capture/archive processing follows RPC completion. Do not charge
        # that observer work to an already received certificate response.
        if self.options.performance_public_trace:
            return self.trace_local.last_rpc_return_us
        return host_us()

    def followup_account_read(self, rpc, market, sample, deadline):
        """Recover only an authenticated read, never signing or action status."""
        delays = READ_RECOVERY_DELAYS if self.options.performance_read_recovery else ()
        while True:
            attempt = sample.get("read_recovery_attempts", 0)
            self.stop_requested(deadline)
            started = host_us()
            try:
                result = self.authenticated_account(rpc, market)
                sample.setdefault("read_consistency_pass", True)
                sample.setdefault("account_read_observations", []).append({
                    "started_host_us": started, "observed_host_us": self.observed_rpc_return_us(), "success": True,
                    "recovery_attempt": attempt})
                if sample.get("read_consistency_failures"):
                    sample["account_read_rpc_recovered"] = True
                return result
            except (JSONRPCException, OSError, TimeoutError) as error:
                failure = {"started_host_us": started, "observed_host_us": self.observed_rpc_return_us(),
                           "recovery_attempt": attempt, "error": compact_error(error)}
                sample["read_consistency_pass"] = False
                sample.setdefault("read_consistency_failures", []).append(failure)
                sample.setdefault("first_balance_read_error", failure)
                if attempt == len(delays) or time.monotonic() + delays[attempt] >= deadline:
                    raise
                # The original submission/certification clocks remain intact.
                sample["read_recovery_attempts"] = attempt + 1
                if self.worker_stop.wait(delays[attempt]):
                    raise

    def phase_can_continue(self, summary):
        key = "workload_completion_pass" if self.options.performance_read_recovery else "correctness_pass"
        return summary[key]

    @staticmethod
    def authenticated_account(rpc, market):
        data = rpc.getflowmeshmarketdata(market, {"limit": 100, "curve_limit": 8})
        verification = data["verification"]
        assert_equal(verification["source"], "remote_endpoint")
        assert_equal(verification["certificate_verified"], True)
        assert_equal(verification["account_state_verified"], True)
        assert_equal(verification["execution_result_verified"], False)
        assert_equal(verification["history_endpoint_reported"], True)
        return data

    def wait_setup_account(self, rpc, market, predicate):
        found = {}
        def ready():
            data = self.authenticated_account(rpc, market)
            if "account" in data and predicate(data["account"]):
                found.update(data["account"])
                return True
            return False
        self.wait_until(ready, timeout=90, check_interval=.1)
        return found

    def setup_receipt(self, rpc, market, response):
        action_id = response["action_id"]
        deadline = time.monotonic() + 90
        next_retry = time.monotonic() + 1
        while True:
            status = rpc.getflowmeshactionstatus(market, action_id)
            assert_equal(status["action_id"], action_id)
            if status["receipt_state"] == "certified_inclusion":
                assert_equal(status["certificate_verified"], True)
                return status
            retryable = status["receipt_state"] == "unknown" or (
                status["receipt_state"] == "rejected" and status.get("reason") in PRE_ADMISSION_REJECTIONS)
            assert status["receipt_state"] != "rejected" or retryable, status
            assert time.monotonic() < deadline, "setup action did not certify"
            if retryable and time.monotonic() >= next_retry:
                retry = rpc.retryflowmeshaction(market, action_id)
                assert_equal(retry["action_id"], action_id)
                next_retry = time.monotonic() + 1
            time.sleep(.05)

    def setup_wallets(self):
        self.start_ordinary_client()
        names = [self.default_wallet_name, "performance_buyer_1", "performance_buyer_2",
                 "performance_buyer_3", "performance_maker"]
        for name in names[1:]:
            self.client.createwallet(wallet_name=name, load_on_startup=True)
        self.wallet_rpcs = {name: self.independent_rpc(self.client, name) for name in names}
        maker_name = names[-1]
        for name, rpc in self.wallet_rpcs.items():
            address = rpc.getnewaddress()
            self.nodes[0].sendtoaddress(address, Decimal("24"))
            if name == maker_name:
                for spec in self.market_specs:
                    self.nodes[0].sendasset(spec["asset"], 1000, address)
        self.synchronize_mempools()
        self.mine_pos_blocks(1, allow_overshoot=True)
        self.wait_client_b3_sync()
        deposits = []
        for spec in self.market_specs:
            market, asset = spec["market_id"], spec["asset"]
            for name, rpc in self.wallet_rpcs.items():
                deposit = rpc.flowmeshdeposit(asset, asset if name == maker_name else "B3",
                                              1000 if name == maker_name else Decimal("10"))
                assert_equal(deposit["market_id"], market)
                self.publish_client_transaction(deposit)
                deposits.append((name, market, deposit))
        self.mine_pos_blocks(30, allow_overshoot=True)
        self.wait_client_b3_sync()
        for name, market, deposit in deposits:
            rpc = self.wallet_rpcs[name]
            response = rpc.submitflowmeshdeposit(market, deposit["deposit_txid"], deposit["deposit_vout"])
            self.setup_receipt(rpc, market, response)
        for spec in self.market_specs:
            market = spec["market_id"]
            operations = self.publish_until_vault_operations(market, len(names))
            assert_equal(len(operations), len(names))
            for operation in operations:
                assert_equal(operation["kind"], "deposit-sweep")
                transaction = self.wallet_rpcs[maker_name].createflowmeshvaulttx(operation["effect_id"])
                self.publish_client_transaction(transaction)
            self.wait_for_market_convergence(market)
            maker = self.wallet_rpcs[maker_name]
            initial = self.wait_setup_account(maker, market, lambda row: row["base_available"] == 1000)
            response = maker.submitflowmeshorder(market, "ask", TRADE_PRICE, 1000, initial["next_sequence"])
            self.setup_receipt(maker, market, response)
            spec["maker_initial"] = self.wait_setup_account(maker, market,
                lambda row: row["base_reserved"] == 1000 and row["next_sequence"] == initial["next_sequence"] + 1)
            spec["maker_wallet"] = maker_name
            for name in names[:-1]:
                account = self.wait_setup_account(self.wallet_rpcs[name], market,
                                                  lambda row: row["b3_available_atoms"] == 10_000_000_000)
                self.workers.append({"wallet": name, "market_id": market, "account": account,
                                     "ordinal": 0, "queue": queue.Queue(maxsize=32), "failed": False})
        assert_equal(len({worker["account"]["account_id"] for worker in self.workers}), BUYERS)
        self.drain_relay_observations()
        self.performance_report["setup"] = {"markets": self.market_specs, "buyer_wallets": BUYERS,
                                            "maker_wallets": 1, "fully_credited_and_swept": True}

    def stop_requested(self, deadline):
        if self.worker_stop.is_set():
            raise AssertionError("worker stopped with unresolved action")
        if time.monotonic() >= deadline:
            raise AssertionError("bounded action deadline exceeded")

    def execute_action(self, worker, rpc, replicas, sample):
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
        while len(sample["replica_observations"]) < 4:
            self.stop_requested(deadline)
            for index, replica in enumerate(replicas):
                if str(index) in sample["replica_observations"]:
                    continue
                data = replica.getflowmeshmarketdata(market, {"limit": 100, "curve_limit": 1})
                if self.contains_target(data, *target):
                    stamp = host_us()
                    sample["replica_observations"][str(index)] = {
                        "observed_host_us": stamp, "observed_ms": (stamp - started) / 1000,
                        "sequence": target[0], "hash": target[1]}
            if len(sample["replica_observations"]) < 4:
                time.sleep(.005)
        applied = max(row["observed_host_us"] for row in sample["replica_observations"].values())
        retained = [row for row in rpc.listflowmeshactions(market)["actions"] if row["action_id"] == action_id]
        assert_equal(len(retained), 1)
        assert_equal(retained[0]["sequence"], sequence)
        sample["retained_action"] = retained[0]
        sample.update(every_replica_observed_host_us=applied, every_replica_observed_ms=(applied - started) / 1000,
                      complete=True, status="complete", completed_host_us=host_us())
        worker["account"] = account

    def worker_loop(self, worker):
        rpc = self.independent_rpc(self.client, worker["wallet"])
        replicas = [self.independent_rpc(node, self.default_wallet_name) for node in self.nodes]
        while True:
            try:
                sample = worker["queue"].get(timeout=.1)
            except queue.Empty:
                if self.worker_stop.is_set():
                    return
                continue
            try:
                if sample is None:
                    return
                if worker["failed"] or self.worker_stop.is_set():
                    sample.update(status="dropped", error={"reason": "previous unresolved account action"})
                    continue
                try:
                    self.trace_local.sample = sample
                    self.execute_action(worker, rpc, replicas, sample)
                except Exception as error:
                    worker["failed"] = True
                    sample.update(status="failed", error=compact_error(error), failed_host_us=host_us())
                    sample["failed_stage"] = ("before_certificate_inclusion" if "client_certified_host_us" not in sample else
                                              "followup_account_read" if "account_state_verified_host_us" not in sample else
                                              "replica_or_retained_action_observation")
                finally:
                    self.trace_local.sample = None
            finally:
                worker["queue"].task_done()

    def schedule_window(self, window, selected, offsets):
        start = window["start_host_us"]
        try:
            for index, offset in enumerate(offsets):
                scheduled = start + round(offset * 1_000_000)
                while host_us() < scheduled and not self.scheduler_stop.is_set():
                    self.scheduler_stop.wait(min(.01, (scheduled - host_us()) / 1_000_000))
                worker = selected[index % len(selected)]
                kind = "fill_bid" if self.options.performance_profile == "native-queue" else KINDS[worker["ordinal"] % len(KINDS)]
                worker["ordinal"] += 1
                stamp = host_us()
                sample = {"sample_id": self.offered_counter, "wallet": worker["wallet"],
                    "market_id": worker["market_id"], "phase": window["label"], "kind": kind,
                    "scheduled_host_us": scheduled, "enqueued_host_us": stamp,
                    "scheduler_lateness_ms": (stamp - scheduled) / 1000,
                    "status": "offered", "attempts": [], "receipt_observations": [],
                    "replica_observations": {}, "complete": False}
                self.offered_counter += 1
                with self.report_lock:
                    self.performance_report["samples"].append(sample)
                    window["samples"].append(sample)
                if self.scheduler_stop.is_set():
                    sample.update(status="dropped", error={"reason": "scheduler stopped; scheduled offer retained"})
                    continue
                try:
                    worker["queue"].put_nowait(sample)
                except queue.Full:
                    worker["failed"] = True
                    sample.update(status="dropped", error={"reason": "bounded account queue full"})
        except Exception as error:
            window["scheduler_error"] = compact_error(error)
        finally:
            window["scheduler_finished_host_us"] = host_us()

    def run_window(self, label, *, rate, duration, count=None):
        count = math.ceil(rate * duration) if count is None else count
        offsets = arrival_offsets(count, rate)
        window = {"label": label, "rate_actions_per_second": rate, "duration_seconds": duration,
                  "planned_offers": count, "start_host_us": host_us() + 100_000,
                  "samples": [], "backlog": []}
        self.performance_report["windows"].append(window)
        heights = [node.getblockcount() for node in self.nodes]
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
        window["heights"] = {"before": heights, "after": [node.getblockcount() for node in self.nodes]}
        window["drained_host_us"] = host_us()
        window["summary"] = summarize_window(window["samples"], window["backlog"], duration,
                                              (window["drained_host_us"] - window["start_host_us"]) / 1_000_000)
        window["summary"]["planned_offers_preserved"] = len(window["samples"]) == count
        window["summary"]["b3_advanced"] = all(after > before for before, after in
                                               zip(heights, window["heights"]["after"]))
        if not window["summary"]["planned_offers_preserved"] or "scheduler_error" in window:
            window["summary"]["correctness_pass"] = window["summary"]["performance_pass"] = False
        self.log.info("FLOWMESH_PERFORMANCE_WINDOW %s", json.dumps(
            {"label": label, **window["summary"]}, sort_keys=True))
        self.write_report()
        return window["summary"]

    def check_signed_observations(self):
        self.drain_relay_observations()
        for sample in self.performance_report["samples"]:
            if "action_id" not in sample or "retained_action" not in sample:
                continue
            copies = [row for row in self.submit_records
                      if submit_context(row) == (sample["market_id"], sample["action_id"])]
            assert copies, "signed action missing HTTPS observation"
            assert all(row["forwarded"] and not row["response_dropped"] for row in copies)
            payloads = {row["action_hex"] for row in copies}
            assert_equal(len(payloads), 1)
            payload = bytes.fromhex(payloads.pop())
            saved = sample["retained_action"]
            assert_equal(hashlib.sha256(payload).hexdigest(), saved["signed_bytes_sha256"])
            assert_equal(len(payload), saved["signed_bytes_size"])
            sample["signed_action"] = {"sha256": saved["signed_bytes_sha256"], "size": len(payload),
                                       "https_submit_count": len(copies), "exact_bytes_preserved": True}

    def check_makers(self):
        for spec in self.market_specs:
            market = spec["market_id"]
            fills = sum(row["status"] == "complete" and row["kind"] == "fill_bid" and row["market_id"] == market
                        for row in self.performance_report["samples"])
            before = spec["maker_initial"]
            rpc = self.wallet_rpcs[spec["maker_wallet"]]
            account = self.wait_setup_account(rpc, market, lambda row: row["base_reserved"] == 1000 - fills)
            assert_equal(account["base_available"], 0)
            assert_equal(account["b3_available_atoms"] - before["b3_available_atoms"],
                         fills * (TRADE_PRICE - TRADE_PRICE // 10_000))
            spec["maker_final"] = account
            spec["verified_filled_units"] = fills
        self.performance_report["maker_conservation_pass"] = True

    def assert_performance_engine_off(self):
        for name, rpc in self.wallet_rpcs.items():
            info, validator = rpc.getflowmeshclientinfo(), rpc.getflowmeshvalidatorinfo()
            assert_equal(info["backend"], "remote")
            assert_equal(info["engine_enabled"], False)
            assert_equal(validator["service_available"], False)
            assert_equal(validator["armed"], False)
            assert_equal(validator["wallet_key_count"], 0)
        assert not (self.client.chain_path / "flowmesh" / "network").exists()
        for spec in self.market_specs:
            assert not (self.client.chain_path / "flowmesh" / spec["market_id"]).exists()

    def invalidate_final_result(self):
        self.performance_report["correctness_pass"] = False
        self.performance_report["performance_pass"] = False
        if "fault_scenario_pass" in self.performance_report:
            self.performance_report["fault_scenario_pass"] = False

    def finalize_public_trace(self):
        if not self.options.performance_public_trace:
            return
        finalization = {"started_host_us": host_us(), "quiescence_timeout_seconds": PUBLIC_TRACE_QUIESCENCE_SECONDS,
                        "quiescent": False}
        self.performance_report["public_trace_finalization"] = finalization
        # Stop admission only after the workload/worker cleanup. No request or
        # timing policy is changed while the measured windows are running.
        for relay in self.tls_relays:
            relay.begin_public_capture_finalization()
        deadline = time.monotonic() + PUBLIC_TRACE_QUIESCENCE_SECONDS
        while True:
            self.drain_relay_observations()
            progress = [relay.public_capture_progress() for relay in self.tls_relays]
            rpc_pending = sum("end_host_us" not in row for row in self.rpc_calls)
            quiescent = all(row["admission_closed"] and not any(row[key] for key in (
                "active_handlers", "pending_records", "unarchived_records", "writes_in_progress")) for row in progress)
            if quiescent and not rpc_pending:
                finalization["quiescent"] = True
                break
            if time.monotonic() >= deadline:
                break
            time.sleep(.02)
        finalization.update(completed_host_us=host_us(), relay_progress=progress,
                            rpc_calls_in_progress=rpc_pending, timed_out=not finalization["quiescent"])
        if not finalization["quiescent"]:
            self.invalidate_final_result()

    def finish_report(self, primary_exception_in_flight, filename, log_label):
        """Finalize before the last report and fail exit without masking a cause."""
        try:
            self.finalize_public_trace()
        except Exception as error:
            self.performance_report["capture_finalization_error"] = compact_error(error)
            self.invalidate_final_result()
        try:
            self.write_report()
        except Exception:
            # Preserve an earlier workload failure even if its final report
            # cannot be written; report failure never turns either path green.
            self.log.exception("Final performance report could not be written")
            if not primary_exception_in_flight:
                raise
            return
        self.log.info("%s %s", log_label, Path(self.options.tmpdir, filename))
        final_failure = (not self.performance_report.get("all_harness_threads_stopped", False) or
                         not self.performance_report.get("read_consistency_pass", False) or
                         "capture_finalization_error" in self.performance_report or
                         (self.options.performance_public_trace and
                          not self.performance_report.get("public_trace", {}).get("complete_capture", False)))
        if final_failure and not primary_exception_in_flight:
            raise AssertionError("Final cleanup/read/capture checks failed; failure report preserved")

    def prepare_public_trace_report(self):
        samples = self.performance_report["samples"]
        planned = self.performance_report.get("campaign", {}).get("planned_windows", [])
        windows = self.performance_report["windows"]
        self.performance_report["campaign_progress"] = {
            "started_windows": [row["label"] for row in windows],
            "unrun_windows": [label for label in planned if not any(row["label"] == label for row in windows)],
            "windows_without_completed_summary": [row["label"] for row in windows if "summary" not in row]}
        self.performance_report["read_consistency_pass"] = all(row.get("read_consistency_pass", True) for row in samples)
        self.performance_report["certified_inclusion_count"] = sum("client_certified_host_us" in row for row in samples)
        self.performance_report["failed_followup_read_count"] = sum(len(row.get("read_consistency_failures", [])) for row in samples)
        if not self.performance_report["read_consistency_pass"]:
            self.invalidate_final_result()
        if not self.options.performance_public_trace:
            return
        self.drain_relay_observations()
        captures = [relay.public_capture.snapshot() for relay in self.tls_relays if relay.public_capture is not None]
        progress = [relay.public_capture_progress() for relay in self.tls_relays]
        rpc_pending = sum("end_host_us" not in row for row in self.rpc_calls)
        finalization = self.performance_report.get("public_trace_finalization", {})
        dropped = sum(relay.records_dropped for relay in self.tls_relays)
        capture_errors = sum("capture_error" in row for row in self.rpc_calls)
        capture_failures = (any(row["failure_count"] for row in captures) or
                            dropped + self.http_trace_dropped + self.rpc_trace_dropped + capture_errors != 0 or
                            "capture_finalization_error" in self.performance_report)
        accounting = attribute_requests(self.http_requests, samples, self.rpc_calls)
        for sample in samples:
            sample["http_requests"] = accounting["per_sample"][str(sample["sample_id"])]
            calls = [row for row in self.rpc_calls if row.get("sample_id") == sample["sample_id"]]
            sample["client_rpc_calls"] = {
                "count": len(calls), "errors": sum("error" in row for row in calls),
                "methods": {method: sum(row["method"] == method for row in calls)
                            for method in sorted({row["method"] for row in calls})}}
        self.performance_report.update(
            http_requests=self.http_requests, rpc_calls=self.rpc_calls, http_request_accounting=accounting,
            public_trace={"started_host_us": getattr(self, "public_trace_started_host_us", None),
                "scope": "all public HTTPS bodies after generated engine-off client startup checks; no HTTP headers or RPC credentials",
                "captures": captures, "relay_records_dropped": dropped,
                "relay_progress": progress, "rpc_calls_in_progress": rpc_pending,
                "finalized": "completed_host_us" in finalization,
                "http_archive_records_dropped": self.http_trace_dropped, "rpc_records_dropped": self.rpc_trace_dropped,
                "rpc_capture_error_count": capture_errors,
                "rpc_archive_bytes_reserved": self.rpc_trace_bytes,
                "pre_capture_http_records": sum(not row.get("public_capture_enabled", False) for row in self.http_requests),
                "http_archive_bytes": self.http_trace_bytes,
                "complete_capture": bool(captures) and len(captures) == len(self.tls_relays) and finalization.get("quiescent", False) and
                    self.performance_report.get("all_harness_threads_stopped", False) and not rpc_pending and
                    all(row["complete_capture"] for row in captures) and not capture_failures and
                    all(row["admission_closed"] and not any(row[key] for key in (
                        "active_handlers", "pending_records", "unarchived_records", "writes_in_progress")) for row in progress)})
        # A live report is explicitly provisional, not a failed workload just
        # because an observer is busy. Real loss stays sticky at any time;
        # final incomplete capture invalidates every scenario-level pass flag.
        if capture_failures or (finalization and not self.performance_report["public_trace"]["complete_capture"]):
            self.invalidate_final_result()
        for window in self.performance_report["windows"]:
            end = window.get("drained_host_us")
            if end is None:
                continue
            counts = {}
            rejected = {}
            api_rejected = {}
            for row in self.http_requests:
                if window["start_host_us"] <= row["host_monotonic_us"] <= end:
                    method = row["method"]
                    counts[method] = counts.get(method, 0) + 1
                    if row.get("upstream_http_status", row.get("client_http_status", 200)) >= 400:
                        rejected[method] = rejected.get(method, 0) + 1
                    if reply_rejected(row):
                        api_rejected[method] = api_rejected.get(method, 0) + 1
            offered = len(window["samples"])
            window["http_requests"] = {"methods": counts, "http_rejections": rejected,
                                       "rejected_replies": api_rejected,
                                       "total": sum(counts.values()), "offered_actions": offered,
                                       "requests_per_offered_action": sum(counts.values()) / offered if offered else None}

    def write_report(self):
        if not self.performance_report:
            return
        self.prepare_public_trace_report()
        self.performance_report["https_submit_records"] = self.submit_records
        self.performance_report["https_method_counts"] = dict(self.relay_method_counts)
        # Samples are shared with windows in memory; serialize them only once.
        snapshot = {**self.performance_report, "windows": [
            {key: ([row["sample_id"] for row in value] if key == "samples" else value)
             for key, value in window.items()} for window in self.performance_report["windows"]]}
        Path(self.options.tmpdir, "flowmesh-performance.json").write_text(
            json.dumps(snapshot, indent=2, sort_keys=True, default=str) + "\n", encoding="utf-8")

    def run_test(self):
        self.performance_report = {
            "format_version": 1, "fixture": "bounded_open_loop_four_validator_https",
            "logging_mode": "detailed_bench" if self.options.performance_diagnostic else "production_debug_disabled",
            "scope": {"operators": 4, "ordinary_client_processes": 1, "buyer_accounts": BUYERS,
                "markets": self.options.performance_markets, "transport": "independent TCP + ordinary HTTPS via TLS relay",
                "scheduled_offer_clock": "Python monotonic; fixed deadlines independent of completion",
                "primary_gate_clock": "original client submission RPC call to client verified certificate inclusion",
                "primary_gate": {"p50_ms": 200, "p95_ms": 600},
                "account_sequencing": "one unresolved instruction per account/market; bounded queue preserves offered arrivals",
                "client_serialization": "wallets share one daemon backend and its existing m_work mutex",
                "replica_times": "serial observer upper bounds, not disk-append timestamps",
                "fill_evidence": "authenticated buyer/maker balance deltas plus exact fees; endpoint history reported separately",
                "execution_result_verified": False, "faults_injected": False,
                "not_qualified": ["WAN", "multiple client processes", "power-loss durability", "fault recovery"]},
            "samples": [], "windows": [], "logging_checks": [], "clock_offsets": {},
            "correctness_pass": False, "performance_pass": False,
            "campaign": {"profile": self.options.performance_profile, "rates": list(self.performance_rates),
                "planned_windows": ["pilot", *[f"sustained_{rate:g}_repeat_{repeat + 1}"
                    for rate in self.performance_rates for repeat in range(2)], "burst_8"],
                "window_seconds": self.options.performance_window_seconds, "repeats_per_rate": 2,
                "pilot_actions": 12, "pilot_rate": .5, "idle_seconds": self.options.performance_idle_seconds,
                "burst_actions": 8, "read_recovery_enabled": self.options.performance_read_recovery,
                "read_recovery_delays_seconds": list(READ_RECOVERY_DELAYS) if self.options.performance_read_recovery else [],
                "read_recovery_never_converts_first_read_failure_to_success": True,
                "public_trace_enabled": self.options.performance_public_trace}}
        self.latency_report = self.performance_report
        if self.options.performance_profile == "native-queue":
            self.performance_report["campaign"].update(planned_windows=["isolated_fill", "burst_8"],
                pilot_actions=1, pilot_rate=0, repeats_per_rate=0,
                purpose="Queue attribution, not sustained-load performance qualification")
        try:
            market, asset = self.bootstrap_latency_market()
            self.market_specs.insert(0, {"market_id": market, "asset": asset})
            for spec in self.market_specs[1:]:
                self.wait_for_market_convergence(spec["market_id"], require_unpaused=False)
                checkpoint = self.publish_checkpoint(spec["market_id"])
                assert_equal(checkpoint["sequence"], 0)
            self.setup_wallets()
            self.assert_performance_engine_off()
            for spec in self.market_specs:
                if self.options.performance_diagnostic:
                    self.calibrate_clocks(spec["market_id"], "before")
                else:
                    self.verify_production_logging(spec["market_id"], "before")
            self.begin_memory_capture()
            for worker in self.workers:
                thread = threading.Thread(target=self.worker_loop, args=(worker,),
                                          name="flowmesh-buyer-worker")
                thread.start()
                self.threads.append(thread)
            self.begin_b3_workload(range(4))
            pilot = (self.run_window("isolated_fill", rate=0, duration=1, count=1)
                     if self.options.performance_profile == "native-queue" else
                     self.run_window("pilot", rate=.5, duration=24, count=12))
            assert self.phase_can_continue(pilot), "pilot account completion failed; retain report"
            idle_start = host_us()
            deadline = time.monotonic() + self.options.performance_idle_seconds
            while time.monotonic() < deadline:
                self.pump_b3()
                self.drain_relay_observations()
                time.sleep(.02)
            self.performance_report["idle"] = {"start_host_us": idle_start, "end_host_us": host_us(), "offered": 0}
            stop_reason = None
            for rate in self.performance_rates:
                summaries = []
                for repeat in range(2):
                    summary = self.run_window(f"sustained_{rate:g}_repeat_{repeat + 1}", rate=rate,
                                              duration=self.options.performance_window_seconds)
                    summaries.append(summary)
                    if not self.phase_can_continue(summary):
                        stop_reason = "correctness failure"
                        break
                if stop_reason or any(not row["performance_pass"] for row in summaries):
                    stop_reason = stop_reason or "latency gate miss or persistent queue growth"
                    self.performance_report["escalation_stopped"] = {"after_rate": rate, "reason": stop_reason,
                        "unrun_rates": [value for value in self.performance_rates if value > rate]}
                    break
            if all(self.phase_can_continue(window["summary"]) for window in self.performance_report["windows"]):
                self.run_window("burst_8", rate=0, duration=1, count=8)
            self.end_b3_workload()
            self.end_memory_capture()
            self.check_signed_observations()
            if all(sample["status"] == "complete" for sample in self.performance_report["samples"]):
                self.check_makers()
            else:
                self.performance_report["maker_conservation_pass"] = False
            self.wait_for_independent_mesh()
            self.assert_no_b3_flowmesh_traffic()
            self.assert_performance_engine_off()
            for spec in self.market_specs:
                if self.options.performance_diagnostic:
                    self.calibrate_clocks(spec["market_id"], "after")
                else:
                    self.verify_production_logging(spec["market_id"], "after")
            if self.options.performance_diagnostic:
                for node, calibrations in self.clock_samples.items():
                    lower = max(row["offset_us_bounds"][0] for row in calibrations)
                    upper = min(row["offset_us_bounds"][1] for row in calibrations)
                    assert lower <= upper, "clock offset intervals do not intersect"
                    self.performance_report["clock_offsets"][node] = {
                        "offset_us_bounds": [lower, upper], "calibrations": calibrations}
            windows = self.performance_report["windows"]
            self.performance_report["correctness_pass"] = all(row["summary"]["correctness_pass"] for row in windows)
            measured = [row for row in windows if row["label"].startswith("sustained_")]
            self.performance_report["entire_requested_ladder_pass"] = len(measured) == len(self.performance_rates) * 2 and all(
                row["summary"]["performance_pass"] and row["summary"]["b3_advanced"] for row in measured)
            passing_rates = [rate for rate in self.performance_rates if
                len(rows := [row for row in measured if row["rate_actions_per_second"] == rate]) == 2 and
                all(row["summary"]["performance_pass"] and row["summary"]["b3_advanced"] for row in rows)]
            self.performance_report["highest_measured_passing_rate"] = max(passing_rates) if passing_rates else None
            self.performance_report["performance_pass"] = bool(passing_rates) and self.performance_report["correctness_pass"]
            self.prepare_public_trace_report()
            assert self.performance_report["read_consistency_pass"], "follow-up read consistency failed; recovered reads remain recorded failures"
            assert self.performance_report["correctness_pass"], "offered actions failed correctness; retain report"
            if self.options.performance_fail_on_gate:
                assert self.performance_report["performance_pass"], "performance gate missed; retain report"
        except Exception as error:
            self.performance_report["error"] = compact_error(error)
            raise
        finally:
            primary_exception_in_flight = sys.exc_info()[0] is not None
            self.scheduler_stop.set()
            self.worker_stop.set()
            for worker in self.workers:
                if any(thread.is_alive() and thread.name == "flowmesh-buyer-worker" for thread in self.threads):
                    try:
                        worker["queue"].put(None, timeout=1)
                    except queue.Full:
                        pass
            for thread in self.threads:
                thread.join(timeout=20)
            self.performance_report["all_harness_threads_stopped"] = not any(thread.is_alive() for thread in self.threads)
            if not self.performance_report["all_harness_threads_stopped"]:
                self.invalidate_final_result()
                self.performance_report["cleanup_error"] = {
                    "type": "AssertionError", "reason": "harness thread survived bounded cleanup joins"}
            self.end_b3_workload()
            if self.memory_capture_nodes:
                try:
                    self.end_memory_capture()
                except Exception as error:
                    self.performance_report["memory_capture_error"] = compact_error(error)
                    self.invalidate_final_result()
            self.finish_report(primary_exception_in_flight, "flowmesh-performance.json", "FLOWMESH_PERFORMANCE_REPORT")


if __name__ == "__main__":
    FlowMeshPerformanceTest(__file__).main()
