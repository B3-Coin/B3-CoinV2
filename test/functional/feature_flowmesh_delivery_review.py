#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Repeatable, observation-only extension of the independent delivery gate.

The original qualification harness and its historical metric names stay intact.
This run uses the same isolated four-seat fixture and the same two graceful
bulk-fault cycles, but records continuously, distinguishes all latency origins,
and requires a returning FN's actual share to be verified by another runtime.
It does not qualify WAN delivery, engine-off clients, or crash recovery.
"""

import hashlib
import json
import math
import shutil
import time
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

from feature_flowmesh_independent import FlowMeshIndependentTest
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal


PRE_ADMISSION_REFUSALS = {
    "FlowMesh service is reconciling the B3 tip",
    "FlowMesh market is paused (at least four active seats are required)",
}
EVENT_POLL_SECONDS = .2
REPLICA_POLL_SECONDS = .1
MAX_RECORDS = 250_000
MAX_RECORD_BYTES = 128 * 1024 * 1024
MAX_PARSED_LOG_BYTES_PER_NODE = 192 * 1024 * 1024


def monotonic_us():
    return time.monotonic_ns() // 1000


def public_bytes_hash(value):
    """Match uint256::GetHex(Hash(compressed_public_bytes)), not text hashing."""
    first = hashlib.sha256(bytes.fromhex(value)).digest()
    return hashlib.sha256(first).digest()[::-1].hex()


def contains_target(data, target):
    """A larger sequence alone is insufficient: require the exact ancestor."""
    snapshot = data["snapshot"]
    if (not snapshot["certified"] or not snapshot["running"] or
            snapshot["halt"] != "none" or
            snapshot["next_microblock_sequence"] <= target["sequence"]):
        return False
    if snapshot["next_microblock_sequence"] == target["sequence"] + 1:
        assert_equal(snapshot["last_microblock_hash"], target["hash"])
        if "state_root" in target:
            assert_equal(snapshot["state_root"], target["state_root"])
        return True
    matches = [entry for entry in data["history"]["entries"]
               if entry["sequence"] == target["sequence"]]
    if not matches:
        return False
    assert_equal(len(matches), 1)
    assert_equal(matches[0]["microblock_hash"], target["hash"])
    return True


class EventCursor:
    """Deduplicate a bounded ring and explicitly report missing cursor ranges."""

    def __init__(self):
        self.last = None
        self.segment = 0

    def restart(self):
        self.last = None
        self.segment += 1

    def consume(self, row, before_us, after_us):
        last = row["last_event_id"]
        reset = self.last is not None and last < self.last
        if reset:
            self.restart()
        previous = self.last
        events = row["events"]
        ids = [event["event_id"] for event in events]
        assert ids == sorted(set(ids)), ids
        assert not ids or ids[-1] <= last
        fresh = [event for event in events if previous is None or event["event_id"] > previous]
        gaps = []
        expected = (previous + 1) if previous is not None else (ids[0] if ids else last + 1)
        for event in fresh:
            if event["event_id"] > expected:
                gaps.append([expected, event["event_id"] - 1])
            expected = event["event_id"] + 1
        if expected <= last:
            gaps.append([expected, last])
        self.last = last
        sampled = row["sampled_monotonic_us"]
        return {"segment": self.segment, "cursor_before": previous, "cursor_after": last,
                "reset_detected": reset, "missing_event_id_ranges": gaps,
                # Initial history before the first retained event was never
                # observed; do not disguise it as a subsequent polling gap.
                "initial_history_unobserved": ids[0] - 1 if previous is None and ids else 0,
                "node_sampled_monotonic_us": sampled,
                "node_to_host_offset_us_bounds": [before_us - sampled, after_us - sampled],
                "events_dropped_from_ring": row["events_dropped"],
                "trace_events_dropped": row["trace_events_dropped"], "events": fresh}


class FlowMeshDeliveryReviewTest(FlowMeshIndependentTest):
    def run_test(self):
        # The framework requires an explicit override even for a deliberate
        # fixture-preserving subclass; its virtual qualification hook is ours.
        super().run_test()

    def add_options(self, parser):
        super().add_options(parser)
        parser.add_argument("--review-baseline-only", action="store_true",
                            help="Run the same three baseline repetitions but omit the two fault cycles")
        parser.add_argument("--review-run-label", default="instrumented_delivery_review",
                            help="Artifact label; does not alter workload or safety rules")

    def set_test_params(self):
        super().set_test_params()
        # Default per-datadir debug.log, append across graceful restarts. The
        # bounded production mirrors contain public hashes, never key material.
        for args in self.extra_args:
            args += ["-debug=bench", "-shrinkdebugfile=0"]
        self.review_active = False
        self.review_market = None
        self.cursors = [EventCursor() for _ in range(self.num_nodes)]
        self.next_event_poll = [0] * self.num_nodes
        self.event_rows = {}
        self.event_gaps = []
        self.rpc_stats = {}
        self.network_snapshots = []
        self.clock_bridges = []
        self.service_events = []
        self.runtime_trace_limits = []
        self.baselines = []
        self.vote_proofs = []
        self.log_offsets = [0] * self.num_nodes
        self.log_bytes = [0] * self.num_nodes
        self.log_last_id = [None] * self.num_nodes
        self.log_segments = [0] * self.num_nodes
        self.log_parse_errors = []
        self.record_count = self.record_bytes = self.records_dropped = 0
        self.observation_work_us = 0
        self.records = None
        self.binary_identity = []

    def restart_node(self, i, extra_args=None, clear_addrman=False, *, expected_stderr=""):
        if extra_args is not None:
            extra_args = list(extra_args) + ["-debug=bench", "-shrinkdebugfile=0"]
        return super().restart_node(i, extra_args, clear_addrman, expected_stderr=expected_stderr)

    def record(self, kind, **fields):
        if self.records is None:
            return
        row = json.dumps({"record": kind, **fields}, sort_keys=True, default=str) + "\n"
        size = len(row.encode("utf-8"))
        if self.record_count >= MAX_RECORDS or self.record_bytes + size > MAX_RECORD_BYTES:
            self.records_dropped += 1
            return
        self.records.write(row)
        self.record_count += 1
        self.record_bytes += size

    def measured_rpc(self, index, method, *params):
        """Bound the observation time; no assumption about cross-process epochs."""
        before = monotonic_us()
        outcome = "returned"
        try:
            result = getattr(self.nodes[index], method)(*params)
        except Exception:
            outcome = "raised"
            raise
        finally:
            after = monotonic_us()
            key = f"{index}:{method}"
            stats = self.rpc_stats.setdefault(key, {"calls": 0, "wall_us": 0, "max_wall_us": 0})
            stats["calls"] += 1
            stats["wall_us"] += after - before
            stats["max_wall_us"] = max(stats["max_wall_us"], after - before)
            self.record("rpc_call", node=index, method=method, host_before_us=before,
                        host_after_us=after, outcome=outcome)
        return result, before, after

    def retain_event(self, index, segment, event, source):
        key = (index, segment, event["event_id"])
        if key not in self.event_rows and len(self.event_rows) < MAX_RECORDS:
            self.event_rows[key] = {"node": index, "segment": segment,
                                    "source": source, "event": event}

    def collect_events(self, *, force=False, indices=None):
        if not self.review_active:
            return
        started = monotonic_us()
        for index in range(self.num_nodes) if indices is None else indices:
            if not self.nodes[index].running:
                continue
            if not force and time.monotonic() < self.next_event_poll[index]:
                continue
            result, before, after = self.measured_rpc(index, "getflowmeshdeliveryinfo", self.review_market)
            self.next_event_poll[index] = time.monotonic() + EVENT_POLL_SECONDS
            assert_equal(result["remote_receipt_proven"], False)
            assert_equal(len(result["markets"]), 1)
            row = result["markets"][0]
            assert_equal(row["market_id"], self.review_market)
            frame = self.cursors[index].consume(row, before, after)
            if row["trace_events_dropped"]:
                limit = {"node": index, "segment": frame["segment"],
                         "trace_events_dropped": row["trace_events_dropped"]}
                if not self.runtime_trace_limits or self.runtime_trace_limits[-1] != limit:
                    self.runtime_trace_limits.append(limit)
            if frame["missing_event_id_ranges"] or frame["reset_detected"]:
                self.event_gaps.append({"node": index, **{key: value for key, value in frame.items()
                                                          if key != "events"}})
            for event in frame["events"]:
                self.retain_event(index, frame["segment"], event, "rpc_ring")
            counters = {key: value for key, value in row.items() if key != "events"}
            self.clock_bridges.append({"node": index, "segment": frame["segment"],
                                       "host_before_us": before, "host_after_us": after,
                                       "node_sampled_monotonic_us": frame["node_sampled_monotonic_us"],
                                       "node_to_host_offset_us_bounds": frame["node_to_host_offset_us_bounds"]})
            self.record("runtime_snapshot", node=index, host_before_us=before, host_after_us=after,
                        forced=force, **frame, counters=counters)
        self.observation_work_us += monotonic_us() - started

    def wait_until(self, test_function, timeout=60, check_interval=.05):
        def observed():
            self.collect_events()
            return test_function()
        return super().wait_until(observed, timeout=timeout, check_interval=check_interval)

    def network_boundary(self, label):
        self.collect_events(force=True)
        for index, node in enumerate(self.nodes):
            if not node.running:
                continue
            snapshot, before, after = self.measured_rpc(index, "getflowmeshnetworkinfo")
            row = {"scenario_boundary": label, "node": index, "segment": self.cursors[index].segment,
                   "host_before_us": before, "host_after_us": after, "network": snapshot}
            self.network_snapshots.append(row)
            self.record("network_snapshot", **row)
        self.records.flush()

    def stop_node(self, i, expected_stderr="", wait=0):
        if self.review_active:
            self.collect_events(force=True, indices=[i])
            self.read_trace_logs(indices=[i])
            self.record("node_stop_started", node=i, host_us=monotonic_us(), segment=self.cursors[i].segment)
        result = super().stop_node(i, expected_stderr, wait)
        if self.review_active:
            # Consume the old process's shutdown suffix before assigning a new
            # segment, even if its final ring events were never polled.
            self.read_trace_logs(indices=[i])
        return result

    def start_node(self, i, *args, **kwargs):
        if self.review_active:
            self.cursors[i].restart()
            self.log_segments[i] = self.cursors[i].segment
            self.log_last_id[i] = None
            self.next_event_poll[i] = 0
            self.record("node_start_started", node=i, host_us=monotonic_us(), segment=self.cursors[i].segment)
        return super().start_node(i, *args, **kwargs)

    def read_trace_logs(self, indices=None):
        """Read only the public JSON trace markers, from bounded saved offsets."""
        started = monotonic_us()
        for index in range(self.num_nodes) if indices is None else indices:
            path = self.nodes[index].debug_log_path
            if not path.exists():
                self.log_parse_errors.append({"node": index, "reason": "debug_log_missing"})
                continue
            if path.stat().st_size < self.log_offsets[index]:
                self.log_parse_errors.append({"node": index, "reason": "debug_log_truncated"})
                self.log_offsets[index] = 0
            with path.open("rb") as stream:
                stream.seek(self.log_offsets[index])
                while self.log_bytes[index] < MAX_PARSED_LOG_BYTES_PER_NODE:
                    offset = stream.tell()
                    line = stream.readline()
                    if not line or not line.endswith(b"\n"):
                        stream.seek(offset)
                        break
                    self.log_bytes[index] += len(line)
                    for marker, source in ((b"FlowMeshTrace ", "runtime_bench"),
                                           (b"FlowMeshNetTrace ", "network_bench"),
                                           (b"FlowMeshServiceTrace ", "service_bench")):
                        if marker not in line:
                            continue
                        try:
                            event = json.loads(line.split(marker, 1)[1])
                        except (ValueError, UnicodeError):
                            self.log_parse_errors.append({"node": index, "offset": offset,
                                                          "reason": "trace_json_invalid"})
                            break
                        if source != "service_bench" and event.get("market_id") != self.review_market:
                            break
                        segment = self.log_segments[index]
                        if source == "runtime_bench":
                            previous = self.log_last_id[index]
                            if previous is not None and event["event_id"] < previous:
                                segment += 1
                                self.log_segments[index] = segment
                            self.log_last_id[index] = event["event_id"]
                            self.retain_event(index, segment, event, source)
                        elif source == "service_bench" and len(self.service_events) < MAX_RECORDS:
                            self.service_events.append({"node": index, "segment": segment, "event": event})
                        self.record(source, node=index, segment=segment, debug_log_offset=offset, event=event)
                        break
                self.log_offsets[index] = stream.tell()
        self.observation_work_us += monotonic_us() - started

    def market_read(self, index, market_id, *, limit=1):
        return self.measured_rpc(index, "getflowmeshmarketdata", market_id,
                                 {"limit": limit, "curve_limit": 2})

    def target_applied(self, index, market_id, target, *, require_signing=False, require_unpaused=False):
        if not self.review_active:
            return super().target_applied(index, market_id, target, require_signing=require_signing,
                                          require_unpaused=require_unpaused)
        data, _, _ = self.market_read(index, market_id, limit=100)
        snapshot = data["snapshot"]
        if ((require_signing and snapshot["observer_only"]) or
                (require_unpaused and snapshot["paused"])):
            return False
        return contains_target(data, target)

    def submit_observed(self, market_id, index, side, *, price=None, quantity=1,
                        indices=range(4), label="four_live"):
        node = self.nodes[index]
        indices = list(indices)
        sequence = node.getflowmeshbalance(market_id)["account"]["next_sequence"]
        b3_height = node.getblockcount()
        method = "cancelflowmeshorder" if price is None else "submitflowmeshorder"
        params = (market_id, side, sequence) if price is None else (market_id, side, price, quantity, sequence)
        attempts, response = [], None
        submitted_us = admitted_us = None

        def admit():
            nonlocal response, admitted_us
            self.pump_b3()
            assert len(attempts) < 120, "bounded pre-admission retry count exhausted"
            before = monotonic_us()
            attempt = {"host_before_us": before}
            attempts.append(attempt)
            try:
                response, _, admitted_us = self.measured_rpc(index, method, *params)
                attempt.update(host_after_us=admitted_us, outcome="admission_response")
                return True
            except JSONRPCException as error:
                attempt.update(host_after_us=monotonic_us(), outcome="rpc_error",
                               error_code=error.error.get("code"), reason=error.error.get("message"))
                # Exact explicit pre-admission errors only. Reconciliation can
                # be reported after wallet signing but before runtime admission;
                # it is not an uncertain result. Never retry any other failure,
                # change the explicit account sequence, or invent another order.
                if error.error.get("code") != -1 or error.error.get("message") not in PRE_ADMISSION_REFUSALS:
                    raise
                attempt["outcome"] = "explicit_pre_admission_refusal"
                return False
            finally:
                self.record("action_attempt", scenario=label, node=index, method=method,
                            account_sequence=sequence, initial_submission_us=submitted_us, **attempt)

        # Include the first wait's event collection and B3 pump as well as
        # every explicit-refusal retry. Individual RPC bounds stay separate.
        submitted_us = monotonic_us()
        self.wait_until(admit, timeout=30, check_interval=.025)
        assert_equal(response["accepted"], True)
        assert_equal(response["sequence"], sequence)
        heads = {other: {} for other in indices}
        next_poll = {other: 0 for other in indices}
        latest, client_us = {}, None
        rotation = 0

        def observe_one(other):
            data, before, after = self.market_read(other, market_id)
            snapshot = data["snapshot"]
            if snapshot["certified"] and snapshot["running"] and snapshot["halt"] == "none":
                head = (snapshot["next_microblock_sequence"] - 1, snapshot["last_microblock_hash"])
                heads[other].setdefault(head, {"host_before_us": before, "host_after_us": after})
            next_poll[other] = time.monotonic() + REPLICA_POLL_SECONDS
            self.record("market_observation", node=other, scenario=label, action_id=response["action_id"],
                        host_before_us=before, host_after_us=after, snapshot=snapshot,
                        account_next_sequence=data.get("account", {}).get("next_sequence"))
            return data, after

        def certified():
            nonlocal client_us, rotation
            self.pump_b3()
            data, after = observe_one(index)
            if data.get("account", {}).get("next_sequence", 0) > sequence:
                assert data["snapshot"]["certified"]
                latest.update(data)
                client_us = after
            # Observe other replicas throughout the client wait, not only
            # afterwards. Rotating order avoids a permanently last FN3 poll.
            others = [other for other in indices if other != index]
            if others:
                rotation %= len(others)
                others = others[rotation:] + others[:rotation]
                rotation += 1
            for other in others:
                if time.monotonic() >= next_poll[other]:
                    observe_one(other)
            return client_us is not None

        try:
            self.wait_until(certified, timeout=90, check_interval=.025)
        except AssertionError:
            self.dump_diagnostics(indices, market_id, sequence + 1)
            raise
        target = self.target(latest["snapshot"])
        observations = {}
        head_key = (target["sequence"], target["hash"])
        for other in indices:
            if head_key in heads[other]:
                observations[str(other)] = heads[other][head_key]

        def applied():
            self.pump_b3()
            for other in indices:
                if str(other) in observations or time.monotonic() < next_poll[other]:
                    continue
                data, before, after = self.market_read(other, market_id, limit=100)
                next_poll[other] = time.monotonic() + REPLICA_POLL_SECONDS
                if contains_target(data, target):
                    observations[str(other)] = {"host_before_us": before, "host_after_us": after}
            return len(observations) == len(indices)

        self.wait_until(applied, timeout=120, check_interval=.025)
        replica_ms = {other: (bounds["host_after_us"] - submitted_us) / 1000
                      for other, bounds in observations.items()}
        last_replica_us = max(bounds["host_after_us"] for bounds in observations.values())
        sample = {"scenario": label, "client_node": index, "action_id": response["action_id"],
                  "account_sequence": sequence, "target": target, "attempts": attempts,
                  "initial_submission_host_us": submitted_us,
                  "admission_response_host_us": admitted_us, "client_certified_host_us": client_us,
                  "submission_to_admission_response_ms": (admitted_us - submitted_us) / 1000,
                  "admission_response_to_client_certified_ms": (client_us - admitted_us) / 1000,
                  "submission_to_client_certified_ms": (client_us - submitted_us) / 1000,
                  "submission_to_replica_observed_ms": replica_ms,
                  "replica_observation_host_bounds_us": observations,
                  "client_to_last_observed_replica_signed_ms": (last_replica_us - client_us) / 1000,
                  "client_to_last_replica_additional_observed_wait_ms": max(0, last_replica_us - client_us) / 1000,
                  "B3_height_before_submission": b3_height,
                  "B3_height_after_replica_observation": node.getblockcount()}
        self.samples.append(sample)
        self.record("action_result", **sample)
        self.log.info("FMNET_REVIEW_ACTION_RESULT %s", json.dumps(sample, sort_keys=True))
        return latest

    def returning_vote_proof(self, market_id, minimum_sequence, key_hash):
        fields = ("sequence", "object_id", "epoch", "seat_set_hash", "seat_index", "bls_key_hash", "signature_hash")
        signed = {}
        for row in self.event_rows.values():
            event = row["event"]
            if (row["node"] == 3 and event["stage"] == "attestation_signed" and
                    event["sequence"] >= minimum_sequence and event.get("bls_key_hash") == key_hash and
                    all(field in event for field in fields)):
                signed[tuple(event[field] for field in fields)] = row
        for row in self.event_rows.values():
            event = row["event"]
            if row["node"] == 3 or event["stage"] != "attestation_verified" or not all(field in event for field in fields):
                continue
            identity = tuple(event[field] for field in fields)
            if identity not in signed:
                continue
            candidate = {"sequence": event["sequence"], "hash": event["object_id"]}
            data, _, _ = self.market_read(row["node"], market_id, limit=100)
            if not contains_target(data, candidate):
                continue
            # The actual vote must belong to a durably certified candidate,
            # not merely any previously authenticated proposal.
            return {"returning_node": 3, "receiving_node": row["node"], "candidate": candidate,
                    "signed_event": signed[identity], "accepted_event": row,
                    "receiver_durable_target": self.target(data["snapshot"]),
                    "claim": "same FN3 BLS share verified by another runtime for a durable certified candidate; not proof that share was included in its aggregate"}
        return None

    def recover_slow_peer(self, market_id, cycle):
        self.network_boundary(f"fault_cycle_{cycle}_before")
        super().recover_slow_peer(market_id, cycle)
        info, _, _ = self.measured_rpc(3, "getflowmeshvalidatorinfo")
        assert_equal(info["wallet_armed_key_count"], 1)
        assert_equal(len(info["wallet_bls_pubkeys"]), 1)
        key_hash = public_bytes_hash(info["wallet_bls_pubkeys"][0])
        minimum_sequence = self.recoveries[-1]["target"]["sequence"] + 1
        proof = None
        # The inherited recovered pair may already prove participation. If
        # not, at most three additional ordinary bid/cancel pairs are allowed;
        # never lower the quorum or mistake arming/catch-up for a signature.
        for extra_pair in range(4):
            self.collect_events(force=True)
            self.read_trace_logs()
            proof = self.returning_vote_proof(market_id, minimum_sequence, key_hash)
            if proof is not None:
                break
            if extra_pair < 3:
                self.pair_workload(market_id, range(4), 1, f"returned_vote_{cycle}_{extra_pair}")
        assert proof is not None, "FN3 rearmed, but no exact accepted remote share for a certified candidate was observed"
        proof["cycle"] = cycle
        self.vote_proofs.append(proof)
        self.recoveries[-1]["signing_share_observed"] = True
        self.recoveries[-1]["accepted_share_proof"] = proof
        self.record("returning_validator_share", **proof)
        self.network_boundary(f"fault_cycle_{cycle}_after")

    def qualification_workload(self, market_id):
        self.last_market = self.review_market = market_id
        self.wait_for_independent_mesh()
        self.records = Path(self.options.tmpdir, "fmnet-delivery-review-events.jsonl").open("w", encoding="utf-8")
        for index, node in enumerate(self.nodes):
            self.log_offsets[index] = node.debug_log_path.stat().st_size
            binary = Path(shutil.which(str(node.args[0])) or str(node.args[0])).resolve()
            digest = hashlib.sha256()
            with binary.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            self.binary_identity.append({"node": index, "path": str(binary), "sha256": digest.hexdigest(),
                                         "size_bytes": binary.stat().st_size})
        started_us = monotonic_us()
        first_height = self.nodes[0].getblockcount()
        success = False
        cleanup_errors = []
        self.review_active = True
        try:
            self.network_boundary("review_before")
            self.begin_b3_workload(range(4))
            # Same six actions and block-advancement condition each repetition;
            # no warm-up exclusion or percentile mixing with the fault cycles.
            for repetition in range(3):
                label = f"normal_baseline_{repetition}"
                first_sample = len(self.samples)
                height = self.nodes[0].getblockcount()
                self.network_boundary(label + "_before")
                self.pair_workload(market_id, range(4), 2, label)
                self.matched_trade(market_id, range(4), label + "_trade")
                self.wait_for_new_b3_block(height)
                self.network_boundary(label + "_after")
                self.baselines.append({"repetition": repetition, "first_sample": first_sample,
                                       "sample_count": len(self.samples) - first_sample,
                                       "B3_height_before": height, "B3_height_after": self.nodes[0].getblockcount()})
                assert_equal(self.baselines[-1]["sample_count"], 6)
            for cycle in range(0 if self.options.review_baseline_only else 2):
                self.recover_slow_peer(market_id, cycle)
            self.network_boundary("review_after")
            success = True
        finally:
            self.fault.configure()
            try:
                self.collect_events(force=True)
                self.read_trace_logs()
                self.end_b3_workload()
                self.assert_no_b3_flowmesh_traffic([index for index, node in enumerate(self.nodes) if node.running])
            except Exception as error:
                cleanup_errors.append(type(error).__name__)
                if success:
                    success = False
                    raise
            finally:
                self.review_active = False
                elapsed_us = monotonic_us() - started_us
                report = self.review_report(success, elapsed_us, first_height, cleanup_errors)
                Path(self.options.tmpdir, "fmnet-delivery-review.json").write_text(
                    json.dumps(report, sort_keys=True, indent=2, default=str) + "\n", encoding="utf-8")
                self.records.close()
                self.records = None
                self.log.info("FMNET_DELIVERY_REVIEW_RESULT %s", json.dumps(report, sort_keys=True, default=str))

    def review_report(self, success, elapsed_us, first_height, cleanup_errors):
        groups = {}
        for label in sorted({sample["scenario"] for sample in self.samples}):
            values = sorted(sample["submission_to_client_certified_ms"]
                            for sample in self.samples if sample["scenario"] == label)
            groups[label] = {"count": len(values), **{f"p{int(p * 100)}_ms": values[math.ceil(len(values) * p) - 1]
                                                      for p in (.5, .95, .99)}}
        action_links = {}
        for row in self.event_rows.values():
            event = row["event"]
            if event["stage"] in {"action_selected", "action_in_proposal", "action_certified"}:
                action_links.setdefault(event["object_id"], set()).add(event["related_object_id"])
        action_trace_index = []
        for sample in self.samples:
            action = sample["action_id"]
            candidates = action_links.get(action, set())
            selected = [row for row in self.event_rows.values()
                        if row["event"]["object_id"] in {action, *candidates}]
            action_trace_index.append({"action_id": action, "candidate_hashes": sorted(candidates),
                                       "observed_event_count": len(selected),
                                       "events": [{"node": row["node"], "segment": row["segment"],
                                                   **{key: row["event"][key] for key in
                                                      ("event_id", "monotonic_us", "stage", "object_id", "round", "reason")
                                                      if key in row["event"]}}
                                                  for row in sorted(selected, key=lambda row: (row["node"], row["segment"], row["event"]["monotonic_us"]))[:256]],
                                       "references_truncated": len(selected) > 256})
        net_trace_limits = [{"node": row["node"], "segment": row["segment"],
                             "trace_events_dropped": row["network"]["trace_events_dropped"]}
                            for row in self.network_snapshots if row["network"]["trace_events_dropped"]]
        return {"success": success, "elapsed_ms": elapsed_us / 1000,
                "run_label": self.options.review_run_label, "binary_identity": self.binary_identity,
                "baseline_only": self.options.review_baseline_only,
                "samples": self.samples, "baseline_repetitions": self.baselines,
                "scenario_submission_to_certification": groups, "recoveries": self.recoveries,
                "returning_validator_accepted_shares": self.vote_proofs,
                "new_B3_blocks_during_workload": self.nodes[0].getblockcount() - first_height,
                "network_boundaries": self.network_snapshots, "event_ring_gaps": self.event_gaps,
                "clock_bridges": self.clock_bridges,
                "action_trace_index": action_trace_index,
                "runtime_trace_limits": self.runtime_trace_limits,
                "network_trace_limits": net_trace_limits,
                "service_trace_limit_reached": any(row["event"]["stage"] == "trace_limit_reached"
                                                    for row in self.service_events),
                "service_gate_trace_records": len(self.service_events),
                "tracked_rpc_calls": self.rpc_stats, "observation_work_ms": self.observation_work_us / 1000,
                "observation_work_fraction": self.observation_work_us / elapsed_us,
                "event_poll_interval_ms_per_node": EVENT_POLL_SECONDS * 1000,
                "replica_poll_interval_ms": REPLICA_POLL_SECONDS * 1000, "client_poll_interval_ms": 25,
                "forced_event_polls": "scenario, stop, proof, and final boundaries; each call has recorded bounds",
                "RPC_accounting_scope": "all new measurement RPCs and action attempts; inherited setup, B3 pump and balance assertions are not included",
                "record_count": self.record_count, "record_bytes": self.record_bytes,
                "records_dropped": self.records_dropped, "record_limits": [MAX_RECORDS, MAX_RECORD_BYTES],
                "log_parse_errors": self.log_parse_errors, "log_bytes_parsed_per_node": self.log_bytes,
                "log_parse_byte_limit_per_node": MAX_PARSED_LOG_BYTES_PER_NODE,
                "log_parse_limit_reached": any(size >= MAX_PARSED_LOG_BYTES_PER_NODE for size in self.log_bytes),
                "event_file": str(Path(self.options.tmpdir, "fmnet-delivery-review-events.jsonl")),
                "node_debug_logs": [str(node.debug_log_path) for node in self.nodes],
                "cleanup_errors": cleanup_errors, "fault_proxy": self.fault.snapshot(),
                "measurement": [
                    "Submission starts before the first admission wait and B3 pump and survives explicit pre-admission retries; each RPC's later start/end is separately recorded.",
                    "Admission response time is not the exact service admission time; correlate local_action_admitted events separately.",
                    "Replica RPC observations are sampled upper bounds, collected during the client wait; signed differences may show an earlier replica observation.",
                    "Every runtime snapshot supplies a node-clock sample bracketed by host monotonic RPC times; cross-process delays require these bounds, not raw timestamp subtraction.",
                    "Ring eviction is not automatically trace loss: missing cursor ranges and BENCH cap counters remain explicit.",
                    "Known refusal counts do not measure every reconciliation interval; inspect source gate events too.",
                    "Service gate_opened is only a flag-write observation and may immediately reclose; it is not signing readiness.",
                    "Transport queue trace timestamps are post-admission observations and may follow a fast worker's socket-write trace.",
                    "Trusted local durable runtime reads, not remote balance proofs or network ACKs.",
                    "Restart-to-catch-up includes deliberately held bulk, workload and a later fixed target; it is not pure transfer time.",
                    "2KiB/s throttling applies per bulk connection direction, not to aggregate FN bandwidth.",
                    "Same-host isolated regtest datadirs, not a claim that the host is otherwise idle or dedicated.",
                ], "WAN_qualified": False, "engine_off_remote_client_qualified": False,
                "crash_restart_qualified": False, "automatic_signing_gate_qualified": False}


class DeliveryReviewMeasurementTests(unittest.TestCase):
    """Offline helpers only; no daemon or wallet is started by these tests."""

    @staticmethod
    def row(ids, last=None):
        return {"events": [{"event_id": value} for value in ids],
                "last_event_id": max(ids, default=0) if last is None else last,
                "sampled_monotonic_us": 100, "events_dropped": 8, "trace_events_dropped": 0}

    def test_cursor_dedup_gap_and_clock_bounds(self):
        cursor = EventCursor()
        first = cursor.consume(self.row([5, 6]), 1000, 1010)
        self.assertEqual(first["initial_history_unobserved"], 4)
        self.assertEqual(first["node_to_host_offset_us_bounds"], [900, 910])
        self.assertEqual(first["missing_event_id_ranges"], [])
        repeated = cursor.consume(self.row([5, 6]), 1100, 1120)
        self.assertEqual(repeated["events"], [])
        gapped = cursor.consume(self.row([9, 10]), 1200, 1220)
        self.assertEqual(gapped["missing_event_id_ranges"], [[7, 8]])
        self.assertEqual([event["event_id"] for event in gapped["events"]], [9, 10])

    def test_cursor_process_restart_is_a_new_segment(self):
        cursor = EventCursor()
        cursor.consume(self.row([20, 21]), 1000, 1010)
        reset = cursor.consume(self.row([1, 2]), 1200, 1220)
        self.assertTrue(reset["reset_detected"])
        self.assertEqual(reset["segment"], 1)
        cursor.restart()
        explicit = cursor.consume(self.row([100, 101]), 1400, 1420)
        self.assertEqual(explicit["segment"], 2)
        self.assertEqual(explicit["cursor_before"], None)

    def test_descendant_requires_exact_history_hash(self):
        data = {"snapshot": {"certified": True, "running": True, "halt": "none",
                             "next_microblock_sequence": 5, "last_microblock_hash": "new"},
                "history": {"entries": []}}
        target = {"sequence": 2, "hash": "fixed"}
        self.assertFalse(contains_target(data, target))
        data["history"]["entries"] = [{"sequence": 2, "microblock_hash": "fixed"}]
        self.assertTrue(contains_target(data, target))
        data["history"]["entries"][0]["microblock_hash"] = "conflict"
        with self.assertRaises(AssertionError):
            contains_target(data, target)

    def test_initial_time_and_nonce_survive_only_explicit_refusal(self):
        harness = Mock()
        harness.nodes = [Mock()]
        harness.nodes[0].getflowmeshbalance.return_value = {"account": {"next_sequence": 7}}
        harness.nodes[0].getblockcount.return_value = 100
        harness.samples = []
        clock = [0]
        calls = []

        def pump():
            clock[0] += 1000

        def wait(predicate, **_):
            for _ in range(10):
                if predicate():
                    return
            self.fail("unexpected helper wait")

        def action_rpc(index, method, *params):
            calls.append((index, method, params))
            before = clock[0]
            clock[0] += 500
            if len(calls) == 1:
                raise JSONRPCException({"code": -1, "message": "FlowMesh service is reconciling the B3 tip"})
            return {"accepted": True, "sequence": 7, "action_id": "action"}, before, clock[0]

        data = {"snapshot": {"certified": True, "running": True, "halt": "none",
                             "next_microblock_sequence": 3, "last_microblock_hash": "target", "state_root": "root"},
                "account": {"next_sequence": 8}, "history": {"entries": []}}

        def market_read(*_, **__):
            before = clock[0]
            clock[0] += 500
            return data, before, clock[0]

        harness.pump_b3.side_effect = pump
        harness.wait_until.side_effect = wait
        harness.measured_rpc.side_effect = action_rpc
        harness.market_read.side_effect = market_read
        harness.target.side_effect = FlowMeshIndependentTest.target
        with patch(__name__ + ".monotonic_us", side_effect=lambda: clock[0]):
            FlowMeshDeliveryReviewTest.submit_observed(harness, "market", 0, "bid", price=10, indices=[0])
        sample = harness.samples[0]
        self.assertEqual(sample["initial_submission_host_us"], 0)
        self.assertEqual(sample["attempts"][0]["host_before_us"], 1000)
        self.assertEqual(sample["submission_to_admission_response_ms"], 3)
        self.assertEqual(sample["admission_response_to_client_certified_ms"], 1.5)
        self.assertEqual(calls, [(0, "submitflowmeshorder", ("market", "bid", 10, 1, 7))] * 2)
        self.assertEqual(sample["attempts"][0]["outcome"], "explicit_pre_admission_refusal")

        # An arbitrary code -1 is still uncertain/unsupported: no second call.
        harness.measured_rpc.side_effect = JSONRPCException({"code": -1, "message": "unrecognized failure"})
        harness.measured_rpc.reset_mock()
        with self.assertRaises(JSONRPCException):
            FlowMeshDeliveryReviewTest.submit_observed(harness, "market", 0, "bid", price=10, indices=[0])
        self.assertEqual(harness.measured_rpc.call_count, 1)


if __name__ == "__main__":
    FlowMeshDeliveryReviewTest(__file__).main()
