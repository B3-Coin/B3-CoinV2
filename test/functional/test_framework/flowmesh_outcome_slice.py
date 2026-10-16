# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Pure, bounded observation/control logic for native-outcome-decision-slice-v1.

This module never calls RPC, signs, retries, logs, or changes native policy.
Only two allowlisted target replies survive. Native events retain their existing
32768-event/16MiB bounds; the stopped native artifact is separate from this
compact report. Decision 10 is a reserved attempt, NOT network dispatch.
"""

from collections import defaultdict, deque
import threading


PROFILE = "native-outcome-decision-slice-v1"
TARGET_SAMPLE = 3
CUTOFF_US = 10_000_000
DECISIONS = {
    1: "inclusion_not_currently_verified",
    2: "outcome_already_verified",
    3: "no_retained_inclusion_proof",
    4: "receipt_endpoint_not_configured",
    5: "endpoint_did_not_advertise_outcome",
    6: "prior_terminal_attempt_origin_not_proven_here",
    7: "endpoint_transport_cooldown",
    8: "recognized_budget_response_cooldown",
    9: "global_automatic_status_budget_not_charged",
    10: "attempt_reserved_before_parse_or_transport_not_dispatch",
    11: "request_returned_without_http_response_not_wire_delivery_proof",
    12: "malformed_response_envelope",
    13: "terminal_refusal_exact_cause_unproven_from_receipt_reason_code_alone",
    14: "recognized_standalone_budget_response",
    15: "client_verified_outcome",
    16: "received_but_unusable_evidence",
    17: "caught_local_exception_not_necessarily_terminal",
    18: "retained_entry_hash_mismatch",
}


def integer(value):
    return type(value) is int and value >= 0


def public_id(value):
    return isinstance(value, str) and len(value) == 64 and all(c in "0123456789abcdef" for c in value)


class OutcomeSlice:
    """One capture generation; one target owner; two replies; eight error codes.

    Worker callbacks only copy allowlisted scalars under this Python mutex.
    The main harness thread owns all capture control RPCs and file writes.
    """

    def __init__(self):
        self.lock = threading.Lock()
        self.started = False
        self.frozen = False
        self.start_bounds = None
        self.deadline = None
        self.identity = None
        self.observations = []
        self.errors = []
        self.active = None
        self.call_number = 0
        self.ready = None
        self.ready_us = None
        self.late_return = False

    def error(self, code):
        with self.lock:
            self._error(code)

    def _error(self, code):
        if code not in self.errors and len(self.errors) < 8:
            self.errors.append(code)

    def start(self, before, after):
        with self.lock:
            if self.started or not integer(before) or not integer(after) or before > after:
                raise ValueError("invalid or repeated slice start")
            self.started = True
            self.start_bounds = [before, after]
            self.deadline = before + CUTOFF_US

    def begin_call(self, sample, method, args, kwargs, started):
        with self.lock:
            if not self.started or self.frozen or self.ready or not isinstance(sample, dict):
                return None
            if sample.get("sample_id") != TARGET_SAMPLE or sample.get("phase") != "burst_8":
                return None
            if method not in ("submitflowmeshorder", "getflowmeshactionstatus", "retryflowmeshaction"):
                return None
            if self.active is not None:
                self._error("overlapping_target_calls")
                return None
            market = sample.get("market_id")
            account = sample.get("account_id")
            if not public_id(market) or not public_id(account) or kwargs or not args or args[0] != market:
                self._error("target_call_context_missing_or_changed")
                return None
            if method != "submitflowmeshorder" and (self.identity is None or len(args) != 2 or
                                                    args[1] != self.identity["action_id"]):
                self._error("target_status_identity_not_original")
                return None
            self.call_number += 1
            self.active = {"call_number": self.call_number, "method": method,
                           "start_host_us": started, "market_id": market, "account_id": account}
            return self.call_number

    def end_call(self, token, result, ended, failed=False):
        if token is None:
            return
        with self.lock:
            if self.frozen or self.active is None or self.active["call_number"] != token:
                return
            call, self.active = self.active, None
            if ended > self.deadline:
                self.late_return = True
                return
            if failed:
                self._error("target_original_rpc_exception")
                self.ready, self.ready_us = "target_rpc_exception", ended
                return
            if not isinstance(result, dict) or not public_id(result.get("action_id")):
                self._error("target_reply_identity_missing")
                return
            if call["method"] == "submitflowmeshorder":
                found = {key: call[key] for key in ("market_id", "account_id")}
                found["action_id"] = result["action_id"]
                if self.identity is not None and self.identity != found:
                    self._error("original_submission_identity_changed")
                    return
                self.identity = found
            if self.identity is None or result["action_id"] != self.identity["action_id"]:
                self._error("target_reply_identity_changed")
                return
            if "account_id" in result and result["account_id"] != self.identity["account_id"]:
                self._error("target_reply_account_changed")
                return
            first = not self.observations
            if not first and call["method"] != "getflowmeshactionstatus":
                self._error("post_inclusion_observation_not_original_status_call")
                return
            if first and not (result.get("receipt_state") == "certified_inclusion" and
                              result.get("certificate_verified") is True):
                return
            row = {**call, **self.identity, "end_host_us": ended,
                   "role": "first_verified_inclusion" if first else "next_existing_status_return"}
            for key in ("receipt_state", "reason_code"):
                if isinstance(result.get(key), str) and len(result[key]) <= 96:
                    row[key] = result[key]
            for key in ("certificate_verified", "outcome_verified"):
                if type(result.get(key)) is bool:
                    row[key] = result[key]
            if integer(result.get("microblock_sequence")):
                row["microblock_sequence"] = result["microblock_sequence"]
            if public_id(result.get("microblock_hash")):
                row["microblock_hash"] = result["microblock_hash"]
            self.observations.append(row)
            if result.get("outcome_verified") is True:
                self.ready, self.ready_us = "target_verified_outcome_observed", ended
            elif not first:
                self.ready, self.ready_us = "next_existing_status_returned", ended

    def stop_reason(self, now):
        with self.lock:
            if not self.started or self.frozen:
                return None
            if self.ready is not None:
                return self.ready
            return "fixed_cutoff" if now >= self.deadline else None

    def freeze(self, reason, requested_us):
        with self.lock:
            self.frozen = True
            return {"profile": PROFILE, "target_sample_id": TARGET_SAMPLE,
                    "start_host_us_bounds": self.start_bounds, "cutoff_eligible_host_us": self.deadline,
                    "stop_reason": reason, "stop_requested_host_us": requested_us,
                    "cutoff_overshoot_us": max(0, requested_us - self.deadline) if self.deadline else None,
                    "target_identity": self.identity, "target_observations": list(self.observations),
                    "target_call_censored_at_stop": dict(self.active) if self.active else None,
                    "target_return_after_cutoff_ignored": self.late_return, "observer_errors": list(self.errors)}


def analyze_capture(state, capture, stop_bounds):
    """Conservative exact-pair + clock-bracket correlation, never total-time guessing.

    Only spans wholly inside this generation AND conservatively inside the
    selected completed RPC are attributed. Missing/ambiguous matches fail the
    diagnostic. Other in-flight spans are censored, not counted as dropped.
    """
    out = {**state, "stop_host_us_bounds": stop_bounds, "diagnostic_complete": False,
           "availability_scope": "selected current-context call only; no universal availability/latency claim",
           "whole_workload_capture_complete": False,
           "other_inflight_completions_after_stop_may_be_omitted_without_drop_increment": True}
    errors = out["observer_errors"] = list(state["observer_errors"])
    events = capture.get("events", [])
    metadata = {key: capture.get(key) for key in
                ("generation", "enabled", "start_us", "stop_us", "dropped", "bytes", "max_bytes", "max_events")}
    out["native_capture"] = metadata
    native_start, native_stop = capture.get("start_us"), capture.get("stop_us")
    if (capture.get("enabled") is not False or capture.get("dropped") != 0 or
            not integer(native_start) or not integer(native_stop) or native_stop < native_start or
            not isinstance(events, list) or not integer(capture.get("bytes")) or
            capture.get("max_bytes") != 16 * 1024 * 1024 or capture.get("max_events") != 32768 or
            capture["bytes"] > capture["max_bytes"] or len(events) > capture["max_events"]):
        errors.append("native_capture_incomplete_or_wrong_bounds")
        return out
    rows = [wrapper.get("event", {}) for wrapper in events if isinstance(wrapper, dict) and
            wrapper.get("stream") == "FlowMeshNativeSpan" and isinstance(wrapper.get("event"), dict)]
    if any(isinstance(wrapper, dict) and isinstance(wrapper.get("event"), dict) and
           wrapper["event"].get("stage") == "trace_limit_reached" for wrapper in events):
        errors.append("native_trace_limit_marker")
    def contained(row):
        return (integer(row.get("started_us")) and integer(row.get("monotonic_us")) and
                native_start <= row["started_us"] <= row["monotonic_us"] <= native_stop)
    out["uncontained_native_spans_censored"] = sum(not contained(row) for row in rows)
    observations = state["target_observations"]
    if not observations:
        errors.append("no_completed_target_verified_inclusion_in_slice")
        return out
    verified = observations[-1].get("outcome_verified") is True
    out["client_verified_outcome_observed"] = verified
    chosen = observations[-1]
    first_success = chosen["role"] == "first_verified_inclusion" and verified
    if first_success:
        # QueryAction can discover inclusion WITH its outcome in this same
        # first status call. UpgradeOutcome was never entered; no decision
        # marker is required or invented for that ordinary success path.
        out["native_upgrade_decision_not_required_for_first_verified_inclusion"] = True
    if chosen["method"] != "getflowmeshactionstatus":
        out["native_decision_not_needed_for_already_verified_original_rpc"] = verified
        out["diagnostic_complete"] = verified and not errors
        return out
    before, after = state["start_host_us_bounds"]
    lower, upper = before - native_start, after - native_start
    out["clock_offset_us_bounds"] = [lower, upper]
    pair = state["target_identity"]
    matches = [row for row in rows if contained(row) and row.get("stage") == "ActionStatus" and
               row.get("market_id") == pair["market_id"] and row.get("action_id") == pair["action_id"] and
               row["started_us"] + lower >= chosen["start_host_us"] and
               row["monotonic_us"] + upper <= chosen["end_host_us"]]
    out["conservative_selected_rpc_native_matches"] = len(matches)
    if len(matches) != 1:
        errors.append("selected_rpc_native_clock_match_missing_or_ambiguous")
        return out
    parent = matches[0]
    if (not integer(parent.get("span_id")) or parent["span_id"] == 0 or
            not integer(parent.get("parent_span_id"))):
        errors.append("selected_native_status_span_identity_missing_or_invalid")
        return out
    fields = ("span_id", "parent_span_id", "started_us", "monotonic_us", "lock_requested_us",
              "lock_acquired_us", "lock_releasing_us", "outcome_upgrade_decision", "outcome_upgrade_endpoint",
              "outcome_upgrade_advertised", "outcome_upgrade_proof_bytes")
    out["selected_native_status"] = {key: parent[key] for key in fields if integer(parent.get(key))}
    by_parent = defaultdict(list)
    for row in rows:
        if contained(row) and integer(row.get("parent_span_id")) and integer(row.get("span_id")):
            by_parent[row["parent_span_id"]].append(row)
    descendants = {parent.get("span_id")}
    pending = deque(descendants)
    children = []
    while pending:
        for row in by_parent[pending.popleft()]:
            if row["span_id"] not in descendants:
                descendants.add(row["span_id"])
                pending.append(row["span_id"])
                children.append(row)
    https = [row for row in children if row.get("stage") == "https_request"]
    out["selected_status_https_child_count"] = len(https)
    out["https_dispatch_attempt_observed"] = any(integer(row.get("dispatch_started_us")) for row in https)
    decision = parent.get("outcome_upgrade_decision")
    if type(decision) is not int or decision not in DECISIONS:
        if not (first_success and "outcome_upgrade_decision" not in parent):
            errors.append("native_decision_missing_or_unknown")
    else:
        out["decision"] = decision
        out["decision_meaning"] = DECISIONS[decision]
        if decision == 9 and https:
            errors.append("global_budget_decision_has_unexpected_https_child")
        if decision == 15 and not verified:
            errors.append("verified_native_decision_disagrees_with_selected_reply")
        if decision == 13:
            out["exact_terminal_cause_unproven_from_receipt_alone"] = True
    if state["stop_reason"] in ("fixed_cutoff", "finally", "target_rpc_exception"):
        errors.append("selected_decision_window_incomplete")
    out["diagnostic_complete"] = not errors
    return out
