#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Offline BENCH diagnostics; never starts nodes or changes the input evidence.

Usage: flowmesh_performance_analyze.py /path/to/fixture --output analysis.json
Accepts flowmesh-performance.json or flowmesh-latency.json and retained
node*/regtest/debug.log files. Override logs with repeated --trace NODE=PATH.
Without --output, JSON is printed. Production-logging reports remain useful
for host observations, but missing trace stages are explicitly unknown.
"""

import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import re


MAX_FILE_BYTES = 256 * 1024 * 1024
MAX_EVENTS = 500_000
ZERO = "0" * 64
EVENT_FIELDS = ("node", "segment", "source", "line", "event_id", "monotonic_us",
                "stage", "kind", "market_id", "epoch", "sequence", "object_id",
                "related_object_id", "wire_hash", "round", "seat_index", "peer", "reason")
# Each row is an actual same-process bracket, not a claim about CPU-only work.
BRACKETS = {
    "execution": ("execution_started", ("execution_completed", "execution_failed")),
    "candidate_persistence": ("candidate_persist_started", ("candidate_durable", "candidate_persist_failed")),
    "proposal_signing": ("proposal_signing_started", ("proposal_signed", "proposal_signing_failed")),
    "proposal_authentication": ("proposal_received", ("proposal_authenticated",)),
    "proposal_validation_including_execution": ("proposal_received", ("proposal_verified",)),
    "attestation_signing": ("attestation_signing_started", ("attestation_signed", "attestation_signing_failed")),
    "attestation_verification": ("attestation_verification_started", ("attestation_signature_verified",)),
    "certificate_assembly": ("certificate_assembly_started", ("certificate_formed", "certificate_assembly_failed")),
    "publication_including_validation_and_sync": ("publication_started", ("durably_applied", "publication_failed")),
}
TLS_BRACKETS = {
    "relay_upstream_request": ("upstream_started_us", "upstream_completed_us"),
    "relay_response_write": ("client_response_attempted_us", "client_response_completed_us"),
    "relay_handler": ("host_monotonic_us", "handler_completed_us"),
}
UNKNOWN = {
    "client_proof_verification": "No client proof-verification start/end instrumentation; client-certified observation includes polling, RPC and HTTPS work.",
    "final_fsync_only": "Publication includes execution, certificate verification, encoding, synchronous LevelDB append and state publication; final fsync is not separately timed.",
    "agreement_journal_fsync_only": "Preliminary-agreement intent, signed-record and decision sync writes have no separate timing brackets.",
    "exact_prepare_commit_quorum": "Agreement relay/verification observations are not exact internal quorum or decision timestamps.",
}


def numeric(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(value)


def stats(values):
    values = sorted(value for value in values if numeric(value))
    result = {"availability": "measured" if values else "unknown", "count": len(values)}
    result.update({key: values[math.ceil(len(values) * fraction) - 1] if values else None
                   for key, fraction in (("p50_ms", .5), ("p95_ms", .95), ("p99_ms", .99))})
    result.update(min_ms=values[0] if values else None, max_ms=values[-1] if values else None,
                  mean_ms=sum(values) / len(values) if values else None,
                  sum_ms=sum(values) if values else None)
    return result


def compact(event):
    return {key: event[key] for key in EVENT_FIELDS if key in event}


def context(event):
    return tuple(event.get(key) for key in ("node", "segment", "market_id", "epoch", "sequence"))


def same_object(begin, end):
    left, right = begin.get("object_id"), end.get("object_id")
    return left in (None, ZERO) or right in (None, ZERO) or left == right


def span(name, begin, end):
    elapsed = (end["monotonic_us"] - begin["monotonic_us"]) / 1000
    return {"stage": name, "elapsed_ms": elapsed, "start": compact(begin), "end": compact(end),
            "valid": elapsed >= 0}


def runtime_spans(events):
    pending, handlers, rows = {}, {}, []
    for event in events:
        key = context(event)
        stage = event["stage"]
        if stage in ("message_processing", "tick_market_lock_acquired"):
            # Runtime has one worker. A newer handler closes the matching window.
            handlers[(event["node"], event["segment"])] = event if stage == "message_processing" else None
            fields = {name: int(value) for name, value in re.findall(r"(\w+)=([0-9]+)", event.get("reason", ""))}
            for name, first, last in (("worker_market_lock_wait", "lock_request_us", "locked_us"),
                                      ("tick_request_to_dequeue", "request_us", "dequeue_us")):
                if first in fields and last in fields and fields[first] > 0:
                    begin, end = dict(event), dict(event)
                    begin["monotonic_us"], end["monotonic_us"] = fields[first], fields[last]
                    rows.append(span(name, begin, end))
        if stage == "agreement_verified":
            begin = handlers.get((event["node"], event["segment"]))
            if begin and context(begin) == key and begin.get("kind") == event.get("kind") == "fmagree" and begin.get("peer") == event.get("peer"):
                rows.append(span("agreement_receive_prefix_including_pump", begin, event))
                handlers[(event["node"], event["segment"])] = None
        for name, (first, last) in BRACKETS.items():
            if stage == first:
                pending[(key, name)] = event
            elif stage in last:
                begin = pending.pop((key, name), None)
                if begin and same_object(begin, event):
                    rows.append(span(name, begin, event))
    return rows


def read_json(path):
    if path.stat().st_size > MAX_FILE_BYTES:
        raise ValueError(f"Input exceeds {MAX_FILE_BYTES} bytes: {path}")
    with path.open(encoding="utf-8") as stream:
        return json.load(stream)


def read_traces(paths, samples):
    slots = {(sample.get("market_id"), sample.get("certified_status", {}).get("microblock_sequence")) for sample in samples}
    actions = {sample.get("action_id") for sample in samples if sample.get("action_id")}
    events, coverage = [], []
    for node, path in paths:
        if path.stat().st_size > MAX_FILE_BYTES:
            raise ValueError(f"Trace exceeds {MAX_FILE_BYTES} bytes: {path}")
        previous, segment, retained, malformed = {}, 0, 0, 0
        with path.open(encoding="utf-8", errors="replace") as stream:
            for line_number, line in enumerate(stream, 1):
                if "FlowMeshTrace " not in line:
                    continue
                try:
                    event = json.loads(line.split("FlowMeshTrace ", 1)[1])
                    if not isinstance(event, dict) or not isinstance(event.get("stage"), str) or not numeric(event.get("monotonic_us")):
                        raise ValueError("Malformed event")
                except (ValueError, TypeError):
                    malformed += 1
                    continue
                market, event_id = event.get("market_id"), event.get("event_id")
                if numeric(event_id):
                    if market in previous and event_id <= previous[market]:
                        segment += 1
                        previous.clear()
                    previous[market] = event_id
                if (market, event.get("sequence")) not in slots and event.get("object_id") not in actions:
                    continue
                event.update(node=str(node), segment=segment, source=str(path), line=line_number)
                events.append(event)
                retained += 1
                if len(events) > MAX_EVENTS:
                    raise ValueError(f"More than {MAX_EVENTS} correlated events; narrow the report or phase")
        coverage.append({"node": str(node), "path": str(path), "retained_events": retained,
                         "malformed_trace_lines": malformed, "detected_segments": segment + 1})
    return events, coverage


def calibrated_event(event, sample, offsets, multi_segment):
    result = compact(event)
    calibration = offsets.get(event["node"], {})
    bounds = calibration.get("offset_us_bounds")
    start = sample.get("initial_submission_host_us")
    if (event["node"] not in multi_segment and numeric(start) and isinstance(bounds, list) and len(bounds) == 2
            and all(numeric(value) for value in bounds) and bounds[0] <= bounds[1]):
        result["from_submission_ms_bounds"] = [(event["monotonic_us"] + offset - start) / 1000 for offset in bounds]
    else:
        result["from_submission_ms_bounds"] = None
    return result


def summarize_stages(rows, names=()):
    grouped = defaultdict(list)
    for name in names:
        grouped[name] = []
    for row in rows:
        if row.get("valid", True):
            grouped[row["stage"]].append(row["elapsed_ms"])
    ranked = [{"stage": name, **stats(values)} for name, values in grouped.items()]
    return sorted(ranked, key=lambda row: (row["mean_ms"] is not None, row["mean_ms"] or 0), reverse=True)


def analyze(report, events, coverage, phase="measured", outlier_ms=600, event_limit=256):
    samples = [dict(sample, market_id=sample.get("market_id", report.get("market_id")))
               for sample in report.get("samples", []) if phase == "all" or sample.get("phase") == phase]
    if not samples:
        raise ValueError(f"No samples for phase {phase!r}")
    spans = runtime_spans(events)
    offsets = report.get("clock_offsets", {})
    multi_segment = {row["node"] for row in coverage if row["detected_segments"] > 1}
    by_slot, by_action = defaultdict(list), defaultdict(list)
    for event in events:
        by_slot[(event.get("market_id"), event.get("sequence"))].append(event)
        by_action[event.get("object_id")].append(event)
    tls = defaultdict(list)
    for record in report.get("https_submit_records", []):
        if record.get("action_id"):
            tls[record["action_id"]].append(record)
    tls_rows, rows, outliers = [], [], []
    for index, sample in enumerate(samples):
        status = sample.get("certified_status", {})
        action, block, sequence = sample.get("action_id"), status.get("microblock_hash"), status.get("microblock_sequence")
        relevant = list(by_slot.get((sample.get("market_id"), sequence), [])) if sequence is not None else []
        seen = {(event["node"], event["line"]) for event in relevant}
        relevant += [event for event in by_action.get(action, []) if (event["node"], event["line"]) not in seen]
        host_metrics = {key: value for key, value in sample.items() if key.endswith("_ms") and numeric(value)}
        for key, field in (("client_certified_ms", "client_certified_host_us"),
                           ("initial_response_ms", "initial_response_host_us"),
                           ("every_replica_observed_ms", "every_replica_observed_host_us"),
                           ("account_state_verified_ms", "account_state_verified_host_us")):
            if key not in host_metrics and numeric(sample.get(field)) and numeric(sample.get("initial_submission_host_us")):
                host_metrics[key] = (sample[field] - sample["initial_submission_host_us"]) / 1000
        row = {"sample_id": sample.get("sample_id", index), "action_id": action, "microblock_hash": block,
               "market_id": sample.get("market_id"), "microblock_sequence": sequence, "phase": sample.get("phase"),
               "kind": sample.get("kind"), "wallet": sample.get("wallet"), "status": sample.get("status"),
               "host_observations": host_metrics, "nodes": {}, "tls_requests": []}
        per_node = defaultdict(list)
        for event in relevant:
            per_node[(event["node"], event["segment"])].append(event)
        for (node, segment), node_events in per_node.items():
            node_events.sort(key=lambda event: event["line"])
            admissions = [event for event in node_events if event["stage"] == "action_verified" and event.get("object_id") == action]
            durable = next((event for event in node_events if event["stage"] == "durably_applied" and event.get("object_id") == block), None)
            lifecycle = None
            if durable:
                begin = next((event for event in admissions if event["monotonic_us"] <= durable["monotonic_us"]), None)
                if begin:
                    lifecycle = span("authenticated_pool_admission_to_durable_apply", begin, durable)
            row["nodes"][f"{node}:{segment}"] = {
                "correlated_event_count": len(node_events), "admission_to_durable": lifecycle,
                "durable_event": calibrated_event(durable, sample, offsets, multi_segment) if durable else None}
        for record in tls.get(action, []):
            request = {key: record[key] for key in ("endpoint", "record_index", "method", "action_id", "forwarded", "response_dropped") if key in record}
            request["timestamps_us"] = {key: value for key, value in record.items() if key.endswith("_us") and numeric(value)}
            request["spans_ms"] = {}
            for name, (begin, end) in TLS_BRACKETS.items():
                if numeric(record.get(begin)) and numeric(record.get(end)):
                    elapsed = (record[end] - record[begin]) / 1000
                    request["spans_ms"][name] = elapsed
                    tls_rows.append({"stage": name, "elapsed_ms": elapsed, "valid": elapsed >= 0,
                                     "action_id": action, "endpoint": record.get("endpoint")})
            row["tls_requests"].append(request)
        rows.append(row)
        breaches = {name: value for name, value in host_metrics.items()
                    if name in ("client_certified_ms", "every_replica_observed_ms", "account_state_verified_ms") and value > outlier_ms}
        if breaches:
            outlier = dict(row, over_threshold=breaches, attempts=sample.get("attempts", []), timelines={})
            for (node, segment), node_events in per_node.items():
                key = f"{node}:{segment}"
                outlier["timelines"][key] = {
                    "events": [calibrated_event(event, sample, offsets, multi_segment) for event in node_events[:event_limit]],
                    "omitted_events": max(0, len(node_events) - event_limit),
                    "correlation": "Exact action/block identities plus context from the same market/slot; context events are not necessarily this action's critical path."}
            outliers.append(outlier)
    lifecycle_rows = [node["admission_to_durable"] for row in rows for node in row["nodes"].values() if node["admission_to_durable"]]
    return {
        "format_version": 1, "analysis_kind": "offline_trace_diagnostic_not_headline_performance",
        "fixture": report.get("fixture"), "logging_mode": report.get("logging_mode"), "phase": phase,
        "samples": len(samples), "source_correctness_pass": report.get("correctness_pass"),
        "source_performance_pass": report.get("performance_pass"), "trace_coverage": coverage,
        "trace_availability": "observed" if events else "unknown: no correlated BENCH events",
        "unknown_segments": {name: {"availability": "unknown", "reason": reason} for name, reason in UNKNOWN.items()},
        "ranked_runtime_stage_aggregates": summarize_stages(spans, BRACKETS),
        "ranked_tls_stage_aggregates": summarize_stages(tls_rows, TLS_BRACKETS),
        "lifecycle_aggregates": summarize_stages(lifecycle_rows),
        "largest_runtime_spans": sorted((row for row in spans if row["valid"]), key=lambda row: row["elapsed_ms"], reverse=True)[:30],
        "negative_span_observations": [row for row in spans if not row["valid"]],
        "outlier_threshold_ms": outlier_ms, "outliers": outliers, "rows": rows,
        "interpretation": [
            "Runtime durations subtract timestamps only within the same node and detected trace segment. Host and relay durations use their own recorded clock.",
            "Node-to-host timelines use only supplied clock_offsets bounds; absent/invalid calibration or multiple detected segments leaves cross-clock timing unknown.",
            "Rankings sort mean measured bracket duration; counts include repeated calls. Spans and replica lifecycles overlap and cannot be added into an end-to-end budget.",
            "Agreement receive prefix includes Receive/Pump, execution, journal writes and relay work. Publication is not a pure fsync measurement.",
            "No timer, retry, round or observed publication establishes the cause of a slow action. Missing pairs and malformed/missing log evidence are not zero-duration stages.",
            "Host completion observations include polling/RPC overhead. TLS handler observation begins after incoming TLS and HTTP parsing, and is not a full client request timer.",
            "Outliers retain identities, source lines and bounded per-node timelines. Account-state and all-replica observations are distinct from client certificate observation.",
            "A trace segment is conservatively split when an event cursor repeats or moves backward; unrelated trace loss cannot be detected without recorded counters.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="Raw report JSON or its fixture directory")
    parser.add_argument("--trace", action="append", default=[], metavar="NODE=PATH")
    parser.add_argument("--phase", default="measured", help="Sample phase, or all (default: measured)")
    parser.add_argument("--outlier-ms", type=float, default=600)
    parser.add_argument("--events-per-outlier-node", type=int, default=256)
    parser.add_argument("--output", type=Path, help="New derived JSON; existing files are never overwritten")
    args = parser.parse_args()
    if not math.isfinite(args.outlier_ms) or args.outlier_ms <= 0 or not 1 <= args.events_per_outlier_node <= 4096:
        parser.error("Require positive finite outlier threshold and 1..4096 events per outlier node")
    try:
        source = args.input.resolve()
        if source.is_dir():
            source = next((source / name for name in ("flowmesh-performance.json", "flowmesh-latency.json") if (source / name).is_file()), None)
            if source is None:
                raise ValueError("Directory contains neither flowmesh-performance.json nor flowmesh-latency.json")
        report = read_json(source)
        samples = [dict(sample, market_id=sample.get("market_id", report.get("market_id")))
                   for sample in report.get("samples", []) if args.phase == "all" or sample.get("phase") == args.phase]
        paths = []
        for value in args.trace:
            node, separator, path = value.partition("=")
            if not separator or not node or not path:
                raise ValueError("--trace requires NODE=PATH")
            paths.append((node, Path(path).resolve()))
        if not paths:
            paths = [(path.parents[1].name.removeprefix("node"), path) for path in sorted(source.parent.glob("node*/regtest/debug.log"))]
        if len({node for node, _ in paths}) != len(paths):
            raise ValueError("Each --trace node identity must be unique")
        events, coverage = read_traces(paths, samples)
        result = analyze(report, events, coverage, args.phase, args.outlier_ms, args.events_per_outlier_node)
        result["source_report"] = str(source)
        serialized = json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8") as stream:
                stream.write(serialized)
            print(json.dumps({"output": str(args.output.resolve()), "samples": result["samples"], "outliers": len(result["outliers"]),
                              "trace_availability": result["trace_availability"]}, sort_keys=True))
        else:
            print(serialized, end="")
    except (OSError, ValueError, TypeError, KeyError) as error:
        parser.exit(1, f"Analysis failed: {error}\n")


if __name__ == "__main__":
    main()
