#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Bounded, offline analysis of 9bf2b0b BENCH streams; never starts a node.

Usage: flowmesh_performance_worker_analyze.py FIXTURE --output NEW.json
Or supply --trace NODE=PATH repeatedly. No host/node clock conversion is made.
Completed spans include early returns, child calls, scheduling and diagnostic
logging inside the bracket: they are not success, CPU-only or causal claims.
"""

import argparse
from bisect import bisect_left, bisect_right
from collections import Counter, defaultdict
import json
import math
from pathlib import Path
import re


MAX_FILE_BYTES = 256 * 1024 * 1024
MAX_TOTAL_BYTES = 1024 * 1024 * 1024
MAX_LINE_BYTES = 64 * 1024
MAX_FILES = 64
MAX_EVENTS = 500_000
MAX_INTERVALS = 1_000_000
MARKER = re.compile(r"\b(FlowMesh(?:Bench|Worker|Service|Store)?Trace) ")
STARTUP = re.compile(r"\b(?:B3Coin|Bitcoin) Core version ")
REFERENCE_FIELDS = ("node", "source", "segment", "line", "trace_stream", "stream", "stage",
                    "monotonic_us", "thread_id", "market_id", "epoch", "sequence",
                    "event_id", "diagnostic_event_id", "object_id", "wire_hash", "peer", "round",
                    "span_id", "parent_span_id", "agreement_stage", "seat_index", "reason")
WORKER_PAIRS = {
    "queue_mutex_wait": ("queue_lock_requested_us", "queue_locked_us"),
    "condition_variable_wait_including_idle_and_reacquire": ("wait_started_us", "wait_returned_us"),
    "dequeue_selection": ("wait_returned_us", "dequeued_us"),
    "dequeue_to_processing_including_diagnostic_hashing": ("dequeued_us", "processing_started_us"),
    "selected_work": ("work_started_us", "processing_completed_us"),
    "finish_queue_mutex_wait": ("finish_lock_requested_us", "finish_locked_us"),
    "queue_push": ("enqueue_started_us", "enqueued_us"),
    "notify_call": ("notified_us", "notify_completed_us"),
}
LIMITS = {
    "overlap": "All brackets are inclusive and may overlap or nest. No cross-stage total or exclusive CPU time is computed.",
    "wake": "An observed notification-to-CV-return bracket cannot separate OS scheduling from CV mutex reacquisition, nor prove which notification woke the worker.",
    "queue_join": "Wire hash is not a queue-item ID. Only unique observed accepted enqueue/dequeue pairs are joined; duplicates are unknown. Capture loss and priority eviction are not individually instrumented.",
    "fsync": "Synchronous WriteBatch(true) includes LevelDB and OS work. Device fsync alone remains unknown; handoff certificate fields cover the whole StageHandoff validation.",
    "clock": "Only the same node/source/process segment is joined. No host-clock subtraction or cross-node latency is computed. Production runtime/store/service use steady clocks; fake-clock unit traces are not a performance source.",
    "logging": "BENCH logs perturb scheduling and lock hold times. Endpoints precede their completion log, but enclosing spans include child logging. Missing/capped spans are unknown, not zero.",
    "success": "Completed instrumentation means the scope ended, including early failure. It does not establish certification or account-read success.",
    "client": "Client first transmission, HTTPS/proof verification and client persistence are not measured by these streams.",
}


def timestamp(value):
    return isinstance(value, int) and not isinstance(value, bool) and value > 0


def valid_event(event):
    if not isinstance(event, dict) or not isinstance(event.get("stage"), str) or not timestamp(event.get("monotonic_us")):
        return False
    for field in ("market_id", "object_id", "wire_hash", "reason", "work", "stream"):
        if field in event and not isinstance(event[field], str):
            return False
    for field in ("epoch", "sequence", "event_id", "diagnostic_event_id", "span_id", "parent_span_id", "thread_id", "peer"):
        if field in event and (not isinstance(event[field], int) or isinstance(event[field], bool)):
            return False
    if "timing" in event and not isinstance(event["timing"], dict):
        return False
    for fields in (event, event.get("timing", {})):
        if any(key.endswith("_us") and (not isinstance(value, int) or isinstance(value, bool) or value < 0)
               for key, value in fields.items()):
            return False
    return True


def scope(event):
    return tuple(event.get(key) for key in ("node", "source", "segment"))


def ref(event):
    return {key: event[key] for key in REFERENCE_FIELDS if key in event}


def stats(values):
    values = sorted(values)
    return {"count": len(values), "availability": "measured" if values else "unknown",
            "mean_ms": sum(values) / len(values) if values else None,
            "max_ms": values[-1] if values else None,
            **{f"p{int(q * 100)}_ms": values[math.ceil(len(values) * q) - 1] if values else None
               for q in (.5, .95, .99)}}


def read_traces(paths):
    """Fail closed on resource bounds; malformed lines are explicit coverage loss."""
    if len(paths) > MAX_FILES:
        raise ValueError("Trace file count limit exceeded")
    events, coverage, total_bytes = [], [], 0
    seen_paths = set()
    for node, supplied in paths:
        path = Path(supplied).resolve()
        if path in seen_paths:
            raise ValueError(f"Duplicate trace path: {path}")
        seen_paths.add(path)
        size = path.stat().st_size
        total_bytes += size
        if size > MAX_FILE_BYTES or total_bytes > MAX_TOTAL_BYTES:
            raise ValueError("Trace byte limit exceeded; narrow the input logs")
        segment, previous_ids, counts = 0, {}, Counter()
        markers, reset_evidence = [], []
        seen_event = False
        with path.open("rb") as stream:
            line_number = 0
            while True:
                raw = stream.readline(MAX_LINE_BYTES + 1)
                if not raw:
                    break
                line_number += 1
                if len(raw) > MAX_LINE_BYTES:
                    raise ValueError(f"Trace line exceeds limit: {path}:{line_number}")
                line = raw.decode("utf-8", errors="replace")
                if STARTUP.search(line) and seen_event:
                    segment += 1
                    previous_ids.clear()
                    seen_event = False
                    reset_evidence.append({"line": line_number, "reason": "startup_marker"})
                match = MARKER.search(line)
                if not match:
                    continue
                try:
                    event = json.loads(line[match.end():])
                    if not valid_event(event):
                        raise ValueError("Invalid trace row")
                except (ValueError, TypeError, RecursionError):
                    counts["malformed"] += 1
                    continue
                family = match[1]
                if family == "FlowMeshTrace" and timestamp(event.get("event_id")):
                    market, event_id = event.get("market_id"), event["event_id"]
                    if market in previous_ids and event_id <= previous_ids[market]:
                        segment += 1
                        previous_ids.clear()
                        reset_evidence.append({"line": line_number, "reason": "runtime_event_id_reset"})
                    previous_ids[market] = event_id
                event.update(node=str(node), source=str(path), segment=segment,
                             line=line_number, trace_stream=family)
                events.append(event)
                counts[family] += 1
                seen_event = True
                if event["stage"] == "trace_limit_reached":
                    markers.append(ref(event))
                if len(events) > MAX_EVENTS:
                    raise ValueError("Trace event limit exceeded; narrow the input logs")
        coverage.append({"node": str(node), "path": str(path), "bytes": size,
                         "counts": dict(counts), "detected_segments": segment + 1,
                         "reset_evidence": reset_evidence, "truncation_markers": markers,
                         "censored_by_explicit_limit": bool(markers),
                         "runtime_byte_cap_status": "unknown_without_delivery_snapshot; FlowMeshTrace does not emit a terminal byte-cap marker",
                         "coverage_note": "No marker does not prove a complete capture. Log start/end, malformed rows, logger loss and spans unfinished at exit may omit work."})
    return events, coverage


def analyze(events, coverage=(), detail_limit=256, outlier_ms=600):
    if not 1 <= detail_limit <= 4096 or not math.isfinite(outlier_ms) or outlier_ms < 0:
        raise ValueError("Invalid output bounds")
    rows, invalid, missing = [], [], Counter()

    def add(name, event, start, end, other=None, **extra):
        if not timestamp(start) or not timestamp(end):
            missing[name] += 1
            return None
        row = {"stage": name, "started_us": start, "completed_us": end,
               "elapsed_ms": (end - start) / 1000, "event": ref(event), **extra}
        if other is not None:
            row["other_event"] = ref(other)
        if end < start:
            if len(invalid) < detail_limit:
                invalid.append(row)
            missing[f"invalid_negative:{name}"] += 1
            return None
        rows.append(row)
        if len(rows) > MAX_INTERVALS:
            raise ValueError("Derived interval limit exceeded; narrow the input logs")
        return row

    agreement, workers, notifications, admissions, dequeues = defaultdict(list), [], defaultdict(list), defaultdict(list), defaultdict(list)
    gates, queue_observations, reconciling_observations = defaultdict(list), defaultdict(list), Counter()
    streams = Counter()
    for event in events:
        family, stage = event["trace_stream"], event["stage"]
        streams[family] += 1
        if stage == "trace_limit_reached":
            continue  # Terminal markers are never data samples.
        if family in ("FlowMeshTrace", "FlowMeshBenchTrace"):
            if timestamp(event.get("started_us")):
                name = "agreement." + str(event.get("reason")) if stage == "agreement_span" else "runtime." + stage
                row = add(name, event, event["started_us"], event["monotonic_us"])
                if row is not None and stage == "agreement_span":
                    agreement[scope(event) + (event.get("market_id"), event.get("span_id"))].append(row)
            if stage in ("message_processing", "tick_market_lock_acquired"):
                fields = {name: int(value) for name, value in re.findall(r"(\w+)=([0-9]+)", event.get("reason", ""))}
                add("runtime.market_mutex_wait", event, fields.get("lock_request_us"), fields.get("locked_us"))
            if stage == "message_chain_gate":
                match = re.search(r"\bopen=([01])\b", event.get("reason", ""))
                reconciling_observations["message_gate_open" if match and match[1] == "1" else "message_gate_closed_or_unparsed"] += 1
        elif family == "FlowMeshWorkerTrace":
            for name, (first, last) in WORKER_PAIRS.items():
                # Non-applicable fields are zero and intentionally not samples.
                if event.get(first) or event.get(last):
                    add("worker." + stage + "." + name, event, event.get(first), event.get(last), work=event.get("work"))
            if stage == "worker_iteration":
                workers.append(event)
                if event.get("delivery_completion"):
                    add("worker.delivery_completion_processing", event, event.get("processing_started_us"), event.get("work_started_us"))
                if event.get("work") == "tick":
                    add("worker.tick_request_to_dequeue", event, event.get("tick_requested_us"), event.get("dequeued_us"))
            if timestamp(event.get("notified_us")) and event.get("worker_waiting") is True:
                notifications[scope(event)].append(event)
            if all(key in event for key in ("wire_hash", "peer", "market_id", "epoch", "sequence")):
                key = scope(event) + tuple(event[key] for key in ("market_id", "epoch", "sequence", "peer", "wire_hash"))
                if stage == "wire_enqueue" and event.get("queue_result") == 0 and not event.get("coalesced") and timestamp(event.get("enqueued_us")):
                    admissions[key].append(event)
                if stage == "worker_iteration" and event.get("work") == "message" and timestamp(event.get("dequeued_us")):
                    dequeues[key].append(event)
            queue_observations[scope(event)].append(event)
        elif family == "FlowMeshServiceTrace":
            if stage in ("gate_closed", "gate_opened"):
                gates[scope(event)].append(event)
            timing = event.get("timing")
            if isinstance(timing, dict):
                add("service." + stage + ".inclusive", event, timing.get("started_us"), event["monotonic_us"])
                for lock in ("mutex", "cs_main"):
                    if timing.get(lock + "_requested_us") or timing.get(lock + "_acquired_us"):
                        add("service." + stage + "." + lock + "_wait", event,
                            timing.get(lock + "_requested_us"), timing.get(lock + "_acquired_us"))
                if isinstance(timing.get("chain_reconciling"), bool):
                    reconciling_observations[str(timing["chain_reconciling"]).lower()] += 1
        elif family == "FlowMeshStoreTrace":
            add("store." + stage + ".inclusive", event, event.get("started_us"), event["monotonic_us"])
            for name, first, last in (
                    ("mutex_wait", "lock_requested_us", "lock_acquired_us"),
                    ("execution", "execution_started_us", "execution_completed_us"),
                    ("handoff_validation_including_certificate" if stage == "append_handoff" else "certificate_verification", "certificate_started_us", "certificate_completed_us"),
                    ("record_encoding", "encode_started_us", "encode_completed_us"),
                    ("batch_construction", "batch_started_us", "batch_completed_us"),
                    ("synchronous_db_call_not_fsync_only", "sync_started_us", "sync_completed_us")):
                if event.get(first) or event.get(last):
                    add("store." + stage + "." + name, event, event.get(first), event.get(last))

    nesting = Counter()
    for key, members in agreement.items():
        for row in members:
            parent_id = row["event"].get("parent_span_id")
            parent = agreement.get(key[:-1] + (parent_id,), [])
            status = "duplicate_span_id" if len(members) != 1 else "root" if parent_id == 0 else "missing_parent"
            if len(members) == 1 and parent_id != 0 and parent:
                status = "ambiguous_parent" if len(parent) != 1 else "matched"
                if len(parent) == 1:
                    p = parent[0]
                    if not (p["started_us"] <= row["started_us"] <= row["completed_us"] <= p["completed_us"]):
                        status = "parent_not_containing_child"
                    else:
                        row["parent_event"] = p["event"]
                        if row["stage"] == "agreement.journal_write_batch_sync" and p["stage"].startswith("agreement.persist_"):
                            add(p["stage"] + ".before_sync_including_serialization", row["event"], p["started_us"], row["started_us"], p["event"])
            row["nesting"] = status
            nesting[status] += 1

    queue_joins = Counter()
    for key in admissions.keys() | dequeues.keys():
        before, after = admissions.get(key, []), dequeues.get(key, [])
        if len(before) == len(after) == 1:
            row = add("worker.unique_observed_enqueue_to_dequeue", after[0], before[0]["enqueued_us"], after[0]["dequeued_us"], before[0], correlation="unique_observed_wire_match_not_queue_id")
            queue_joins["unique_observed_pair" if row else "invalid_pair"] += 1
        else:
            queue_joins["ambiguous_repeated_wire" if len(before) > 1 or len(after) > 1 else "unmatched"] += 1

    for key in notifications:
        notifications[key].sort(key=lambda event: event["notified_us"])
    notification_times = {key: [event["notified_us"] for event in value] for key, value in notifications.items()}
    wake_joins = Counter()
    for event in workers:
        first, last = event.get("wait_started_us"), event.get("wait_returned_us")
        if not timestamp(first) or not timestamp(last) or last < first:
            continue
        key = scope(event)
        times = notification_times.get(key, [])
        left, right = bisect_left(times, first), bisect_right(times, last)
        if left < right:
            notification = notifications[key][left]
            add("worker.observed_notification_to_cv_return_including_scheduling_and_reacquire", event,
                notification["notified_us"], last, notification, observed_notifications_in_wait=right - left,
                correlation="first_observed_notification_not_proven_wake_cause")
            wake_joins["observed_notification_bracket"] += 1
        else:
            wake_joins["no_observed_notification_in_wait"] += 1

    gate_coverage = Counter()
    for grouped in gates.values():
        closed = None
        for event in sorted(grouped, key=lambda value: (value["monotonic_us"], value["line"])):
            if event["stage"] == "gate_closed":
                if closed is None:
                    closed = event
                else:
                    gate_coverage["repeated_closed_observation"] += 1
            elif closed is not None:
                add("service.observed_reconciliation_gate_closed", event, closed["monotonic_us"], event["monotonic_us"], closed)
                gate_coverage["closed_open_pair"] += 1
                closed = None
            else:
                gate_coverage["open_without_observed_close"] += 1
        if closed is not None:
            gate_coverage["closed_without_observed_open"] += 1

    grouped = defaultdict(list)
    for row in rows:
        grouped[row["stage"]].append(row["elapsed_ms"])
    aggregates = [{"stage": name, **stats(values)} for name, values in grouped.items()]
    aggregates.sort(key=lambda item: (-item["mean_ms"], item["stage"]))
    largest = sorted(rows, key=lambda item: item["elapsed_ms"], reverse=True)
    queue_summary = []
    for key, selected in sorted(queue_observations.items()):
        def maximum(field):
            values = [event[field] for event in selected if isinstance(event.get(field), int) and not isinstance(event[field], bool) and event[field] >= 0]
            return max(values) if values else None
        queue_summary.append({"node": key[0], "source": key[1], "segment": key[2], "observations": len(selected),
                              "max_observed_queue_before": maximum("queue_before"), "max_observed_queue_after": maximum("queue_after"),
                              "max_observed_queue_bytes": maximum("queue_bytes"), "max_observed_control_depth": maximum("control_depth"),
                              "interpretation": "Point observations, not a time-weighted queue distribution or continuous maximum."})
    return {"format_version": 1, "scope": "offline_BENCH_diagnostics_not_headline_performance",
            "measurement_limits": LIMITS, "coverage": list(coverage), "stream_counts": dict(streams),
            "censored_capture": True if any(item.get("censored_by_explicit_limit") or item.get("counts", {}).get("malformed") for item in coverage) else None,
            "coverage_warning": "Caps are process-lifetime and may be exhausted during bootstrap; no reset or complete measured-window coverage is assumed.",
            "selection": "All retained trace events, including bootstrap. Not filtered to a host benchmark phase; use market/sequence/event references for action correlation.",
            "resource_limits": {"files": MAX_FILES, "bytes_per_file": MAX_FILE_BYTES, "total_input_bytes": MAX_TOTAL_BYTES,
                                "line_bytes": MAX_LINE_BYTES, "events": MAX_EVENTS, "derived_intervals": MAX_INTERVALS,
                                "output_interval_details_per_list": detail_limit},
            "new_agreement_spans_available": bool(agreement), "ranked_inclusive_bracket_aggregates": aggregates,
            "aggregate_ranking": "Descending mean duration; includes nested spans and idle waits. Not a ranking of independent bottleneck costs.",
            "interval_count": len(rows), "largest_intervals": largest[:detail_limit],
            "outlier_threshold_ms": outlier_ms, "outlier_count": sum(row["elapsed_ms"] > outlier_ms for row in rows),
            "outliers": [row for row in largest if row["elapsed_ms"] > outlier_ms][:detail_limit],
            "detail_limit": detail_limit, "invalid_intervals": invalid, "missing_or_invalid_brackets": dict(missing),
            "agreement_nesting": dict(nesting), "queue_correlations": dict(queue_joins), "wake_correlations": dict(wake_joins),
            "queue_observations": queue_summary, "reconciliation_observations": dict(reconciling_observations),
            "reconciliation_coverage": dict(gate_coverage),
            "historical_1282_932_ms_gap": {"attribution": "unknown", "measured_in_this_analysis": False,
                "reason": "The retained old execution_completed to stage=4 VIEW_CHANGE publication bracket has no new internal spans. A new run cannot retroactively identify its cause."}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", nargs="?", type=Path)
    parser.add_argument("--trace", action="append", default=[], metavar="NODE=PATH")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--detail-limit", type=int, default=256)
    parser.add_argument("--outlier-ms", type=float, default=600)
    args = parser.parse_args()
    try:
        paths = []
        for item in args.trace:
            node, separator, path = item.partition("=")
            if not separator or not node or not path:
                raise ValueError("--trace requires NODE=PATH")
            paths.append((node, Path(path)))
        if not paths and args.fixture:
            paths = [(path.parent.parent.name.removeprefix("node"), path)
                     for path in sorted(args.fixture.glob("node*/regtest/debug.log"))]
        if not paths:
            raise ValueError("No trace logs found; provide a fixture or --trace NODE=PATH")
        events, coverage = read_traces(paths)
        result = analyze(events, coverage, args.detail_limit, args.outlier_ms)
        encoded = json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as stream:
                stream.write(encoded)
        else:
            print(encoded, end="")
    except (OSError, ValueError) as error:
        parser.exit(2, f"error: {error}\n")


if __name__ == "__main__":
    main()
