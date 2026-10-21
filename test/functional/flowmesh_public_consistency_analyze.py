#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Read-only, bounded PUBLIC response-coherence analysis, without daemon RPC.

The v1 entry prefix/tail decoder reconstructs the public entry commitment. It
does NOT authenticate BLS certificates, recompute action/state roots, resolve
chain pins, or reproduce a server race. HTTP/RPC interval joins are explicitly
correlations, not causal attribution. Output files must not already exist.
"""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct


MAX_BODY = 32 * 1024 * 1024
MAX_REPORT = 512 * 1024 * 1024
MAX_INPUT_BYTES = 1024 * 1024 * 1024
MAX_REQUESTS = 32768
MAX_ERRORS = 128
MAX_ENTRY = 2 * 1024 * 1024
ENTRY_HEADER = 215
ENTRY_TAIL = 216
STALE_HISTORY = "Reported history is stale/out of order"
IDENTITY = ("market_id", "domain", "execution_config_id", "account_id")
HEAD = ("next_microblock_sequence", "last_microblock_hash", "state_root", "epoch", "anchor_height", "anchor_hash")


def select(value, fields):
    return {key: value[key] for key in fields if key in value} if isinstance(value, dict) else {}


def compact_size(data, offset):
    if offset >= len(data):
        raise ValueError("missing action count")
    tag = data[offset]
    if tag < 253:
        return tag, offset + 1
    size = {253: 2, 254: 4, 255: 8}[tag]
    if offset + 1 + size > len(data):
        raise ValueError("truncated action count")
    value = int.from_bytes(data[offset + 1:offset + 1 + size], "little")
    if value < {253: 253, 254: 65536, 255: 4294967296}[tag]:
        raise ValueError("non-canonical action count")
    return value, offset + 1 + size


def decode_entry_prefix(payload_hex):
    """ProductionEntryCore fixed fields + ProductionEntryCommitmentV1 hash.

    Mirrors production_engine.h serialization and production_commitment.h's
    big-endian tagged identity. Action-vector bodies and certificate suffix
    remain opaque; this deliberately is not DecodeProductionEntry/VerifyEntry.
    """
    if not isinstance(payload_hex, str) or len(payload_hex) > (MAX_ENTRY + 1024) * 2:
        raise ValueError("certified payload exceeds supported bound")
    payload = bytes.fromhex(payload_hex)
    if len(payload) < 4:
        raise ValueError("missing certified entry length")
    size = int.from_bytes(payload[:4], "big")
    if not ENTRY_HEADER + 1 + ENTRY_TAIL <= size <= MAX_ENTRY or size > len(payload) - 4:
        raise ValueError("invalid certified entry length")
    entry = payload[4:4 + size]
    version, kind = struct.unpack_from("<HB", entry)
    if version != 1 or kind not in (1, 2):
        raise ValueError("unsupported entry version/kind")
    domain, market = entry[3:35], entry[35:67]
    epoch = int.from_bytes(entry[67:75], "little")
    seat_set = entry[75:107]
    sequence = int.from_bytes(entry[107:115], "little")
    parent = entry[115:147]
    anchor_height = int.from_bytes(entry[147:151], "little", signed=True)
    anchor_hash, previous_root = entry[151:183], entry[183:215]
    count, actions_start = compact_size(entry, ENTRY_HEADER)
    actions_end = size - ENTRY_TAIL
    if count > 1024 or actions_start > actions_end or (count == 0) != (actions_start == actions_end):
        raise ValueError("invalid bounded action-vector framing")
    tail = entry[-ENTRY_TAIL:]
    actions_root, result_root, state_root = tail[:32], tail[32:64], tail[64:96]
    effect_start, effect_count = struct.unpack_from("<QI", tail, 96)
    effect_root = tail[108:140]
    next_epoch = int.from_bytes(tail[140:148], "little")
    next_height = int.from_bytes(tail[148:152], "little", signed=True)
    next_anchor, next_seats = tail[152:184], tail[184:216]
    if anchor_height < 0:
        raise ValueError("negative production anchor")
    if kind == 1:
        if next_epoch or next_height != -1 or any(next_anchor) or any(next_seats):
            raise ValueError("unexpected execution handoff fields")
        next_height = 0  # Commitment() omits the null execution-only anchor.
    elif next_height < 0 or next_epoch != epoch + 1 or count:
        raise ValueError("invalid handoff prefix fields")
    identity = (struct.pack(">HB", version, kind) + domain + market + struct.pack(">Q", epoch) + seat_set +
        struct.pack(">Q", sequence) + parent + struct.pack(">Q", anchor_height) + anchor_hash + previous_root +
        actions_root + result_root + state_root + struct.pack(">QI", effect_start, effect_count) + effect_root +
        struct.pack(">QQ", next_epoch, next_height) + next_anchor + next_seats)
    tag = hashlib.sha256(b"B3/FLOWMESH/ENTRY/V1").digest()
    entry_hash = hashlib.sha256(tag + tag + identity).digest()[::-1].hex()
    return {"version": version, "kind": kind, "domain": domain[::-1].hex(), "market_id": market[::-1].hex(),
        "epoch": epoch, "sequence": sequence, "entry_hash": entry_hash, "parent_hash": parent[::-1].hex(),
        "state_root": state_root[::-1].hex(), "anchor_height": anchor_height,
        "anchor_hash": anchor_hash[::-1].hex(), "action_count": count, "entry_bytes": size,
        "certificate_suffix_bytes": len(payload) - 4 - size, "certificate_authenticated_by_analyzer": False}


def response_context(request, response):
    params = request.get("params", {})
    result = response.get("result", {})
    if not isinstance(result, dict):
        result = {}
    reported = result.get("reported_data", {})
    if not isinstance(reported, dict):
        reported = {}
    history = reported.get("history", {})
    if not isinstance(history, dict):
        history = {}
    entries = history.get("entries", [])
    if not isinstance(entries, list) or len(entries) > 1000:
        raise ValueError("invalid/oversized reported history array")
    context = {"method": request.get("method"), "request": select(params, IDENTITY + (
        "action_id", "cursor", "known_head", "before_sequence", "limit")),
        "response_ok": response.get("ok"), "public_error": response.get("error"),
        "status": select(result.get("status"), IDENTITY + HEAD),
        "cursor": result.get("cursor"), "gap": result.get("gap"), "more": result.get("more"),
        "oldest_event_id": result.get("oldest_event_id"), "latest_event_id": result.get("latest_event_id"),
        "reported_identity": select(reported, IDENTITY), "reported_unchanged": reported.get("unchanged"),
        "reported_snapshot": select(reported.get("snapshot"), HEAD),
        "reported_account": select(reported.get("account"), ("account_id", "next_sequence")),
        "history": {**select(history, ("available", "truncated", "oldest_retained_sequence", "next_before_sequence")),
            "entries": [select(row, ("sequence", "microblock_hash", "state_root", "kind")) for row in entries]}}
    if isinstance(result.get("certified_payload"), str):
        try:
            context["entry_prefix"] = decode_entry_prefix(result["certified_payload"])
        except ValueError as error:
            context["entry_prefix_decode_error"] = str(error)
    return context


def inspect_context(context, entries_by_hash):
    findings = []
    request, status, reported = context["request"], context["status"], context["reported_snapshot"]
    prefix = context.get("entry_prefix")
    method = context["method"]
    def issue(code, **detail):
        findings.append({"code": code, **detail})
    if "entry_prefix_decode_error" in context:
        issue("certified_entry_prefix_unsupported_or_malformed", error=context["entry_prefix_decode_error"])
    for field in IDENTITY[:3]:
        values = {where: value[field] for where, value in (
            ("request", request), ("status", status), ("reported_data", context["reported_identity"]),
            ("entry_prefix", prefix or {})) if field in value}
        if len(set(values.values())) > 1:
            issue("response_identity_mismatch", field=field, values=values)
    account = context["reported_account"].get("account_id")
    if account is not None and request.get("account_id") not in (None, account):
        issue("reported_account_mismatch", request=request["account_id"], reported=account)
    sequences = [row.get("sequence") for row in context["history"]["entries"]]
    context["history"]["entry_count"] = len(sequences)
    context["history"]["sequence_range"] = [min(sequences), max(sequences)] if sequences and all(type(v) is int for v in sequences) else None
    if any(type(v) is not int or v < 0 for v in sequences):
        issue("history_sequence_not_unsigned_integer")
    elif any(left <= right for left, right in zip(sequences, sequences[1:])):
        issue("history_not_strictly_descending", sequences=sequences)
    if prefix and method == "snapshot":
        expected = {"next_microblock_sequence": prefix["sequence"] + 1,
                    "last_microblock_hash": prefix["entry_hash"], "state_root": prefix["state_root"]}
        for projection_name, projection in (("status", status), ("reported_snapshot", reported)):
            for field, value in expected.items():
                if field in projection and projection[field] != value:
                    issue("snapshot_projection_differs_from_entry", projection=projection_name, field=field,
                          entry=value, reported=projection[field])
        over = [value for value in sequences if type(value) is int and value >= expected["next_microblock_sequence"]]
        if over:
            issue("snapshot_history_at_or_ahead_of_included_entry", entry_sequence=prefix["sequence"], offending_sequences=over)
        for row in context["history"]["entries"]:
            if row.get("sequence") != prefix["sequence"]:
                continue
            for field, entry_field in (("microblock_hash", "entry_hash"), ("state_root", "state_root")):
                if field in row and row[field] != prefix[entry_field]:
                    issue("snapshot_equal_height_history_commitment_conflict", field=field,
                          entry=prefix[entry_field], reported=row[field], sequence=prefix["sequence"])
    for projection_name, projection in (("status", status), ("reported_snapshot", reported)):
        bound = projection.get("next_microblock_sequence")
        over = [value for value in sequences if type(value) is int and type(bound) is int and value >= bound]
        if over:
            issue("history_at_or_ahead_of_reported_head", projection=projection_name, next_sequence=bound, offending_sequences=over)
    if method == "updates":
        old, new = request.get("cursor"), context.get("cursor")
        if isinstance(old, dict) and isinstance(new, dict):
            if old.get("instance_id") != new.get("instance_id") and context.get("gap") is not True:
                issue("cursor_instance_changed_without_gap", before=old, after=new)
            elif old.get("instance_id") == new.get("instance_id") and all(type(x) is int for x in (old.get("event_id"), new.get("event_id"))) and new["event_id"] < old["event_id"]:
                issue("cursor_event_id_regressed", before=old, after=new)
        known = entries_by_hash.get((request.get("market_id"), request.get("known_head")))
        if known:
            context["request_known_head_entry"] = known
            head, sequence = status.get("last_microblock_hash"), status.get("next_microblock_sequence")
            if type(sequence) is int and sequence < known["sequence"] + 1:
                issue("updates_reported_head_older_than_known_head", known_sequence=known["sequence"], reported_next=sequence)
            if sequence == known["sequence"] + 1 and head != known["entry_hash"]:
                issue("updates_conflicting_equal_height_head", known_hash=known["entry_hash"], reported_hash=head)
        context["updates_refresh_required_observation"] = (context.get("gap") is True or (
            "known_head" in request and status.get("last_microblock_hash") != request["known_head"]))
        # A newer update head normally triggers a fresh snapshot. Do NOT call
        # history ahead of known_head a stale-history failure by itself.
    context["findings"] = findings
    return context


def correlated_rejections(record, context, rpc_calls):
    stamp = record.get("host_monotonic_us")
    if type(stamp) is not int:
        return []
    matches = []
    for index, call in enumerate(rpc_calls):
        if "error" not in call or not call.get("start_host_us", stamp + 1) <= stamp <= call.get("end_host_us", stamp - 1):
            continue
        if any(field in call and field in context["request"] and call[field] != context["request"][field]
               for field in ("market_id", "account_id")):
            continue
        matches.append({"rpc_index": index, **select(call, ("method", "wallet", "sample_id", "market_id", "account_id",
            "start_host_us", "end_host_us", "error")), "relationship": "matching request-time/scope interval; not causal proof"})
    return matches


class Reader:
    def __init__(self):
        self.bytes_read = 0
        self.failure_count = 0
        self.errors = []

    def failure(self, path, error):
        self.failure_count += 1
        if len(self.errors) < MAX_ERRORS:
            self.errors.append({"path": str(path), "error": str(error)})

    def read_json(self, path, limit=MAX_BODY):
        if path.is_symlink():
            raise ValueError("symlink input refused")
        size = path.stat().st_size
        if size > limit or self.bytes_read + size > MAX_INPUT_BYTES:
            raise ValueError("analysis input byte bound exceeded")
        with path.open("rb") as stream:
            body = stream.read(limit + 1)
        self.bytes_read += len(body)
        if len(body) > limit:
            raise ValueError("file grew beyond analysis bound")
        return json.loads(body), {"path": str(path), "bytes": len(body), "sha256": hashlib.sha256(body).hexdigest()}


def analyze(fixture):
    fixture = Path(fixture).resolve()
    reader, rows, entries_by_hash, method_counts = Reader(), [], {}, Counter()
    report = {}
    if fixture.is_file():
        report, _ = reader.read_json(fixture, MAX_REPORT)
        fixture = fixture.parent
    else:
        for name in ("flowmesh-performance.json", "flowmesh-performance-faults.json"):
            if (fixture / name).is_file():
                report, _ = reader.read_json(fixture / name, MAX_REPORT)
                break
    if not isinstance(report, dict):
        raise ValueError("performance report must be an object")
    for field in ("http_requests", "rpc_calls"):
        if not isinstance(report.get(field, []), list):
            raise ValueError("performance report arrays malformed")
        if len(report.get(field, [])) > MAX_REQUESTS:
            reader.failure(fixture, f"report {field} count exceeds bounded analysis")
            report[field] = report[field][:MAX_REQUESTS]
    record_index = {}
    for record in report.get("http_requests", []):
        path = record.get("request_body", {}).get("path")
        if path:
            record_index[(Path(path).parent.name, record.get("request_id"))] = record
    trace = fixture / "public-http-trace"
    directories = []
    if not trace.is_dir() or trace.is_symlink():
        reader.failure(trace, "missing public trace directory or symlink")
    else:
        for path in trace.iterdir():
            if re.fullmatch(r"endpoint[0-9]+", path.name) and path.is_dir() and not path.is_symlink():
                directories.append(path)
                if len(directories) > 16:
                    reader.failure(trace, "endpoint directory bound exceeded")
                    directories = directories[:16]
                    break
    requests_seen = directory_entries_seen = unindexed_count = 0
    unindexed = []
    for directory in sorted(directories):
        for request_path in directory.iterdir():
            directory_entries_seen += 1
            if directory_entries_seen > MAX_REQUESTS * 4:
                reader.failure(directory, "directory entry count bound exceeded")
                break
            match = re.fullmatch(r"([0-9]{8})-request\.json", request_path.name)
            if not match:
                continue
            requests_seen += 1
            if requests_seen > MAX_REQUESTS:
                reader.failure(directory, "analysis request count bound exceeded")
                break
            request_id = int(match[1])
            record = record_index.get((directory.name, request_id), {})
            if not record:
                unindexed_count += 1
                if len(unindexed) < MAX_ERRORS:
                    unindexed.append({"endpoint_directory": directory.name, "request_id": request_id})
            try:
                request, request_artifact = reader.read_json(request_path)
                if not isinstance(request, dict) or not isinstance(request.get("params"), dict):
                    raise ValueError("invalid public request shape")
                method_counts[str(request.get("method"))] += 1
                responses_seen = 0
                for stage in ("upstream", "delivered", "relay_error"):
                    response_path = directory / f"{request_id:08d}-{stage}.json"
                    if not response_path.exists():
                        continue
                    responses_seen += 1
                    response, artifact = reader.read_json(response_path)
                    if not isinstance(response, dict):
                        raise ValueError("invalid public response shape")
                    context = response_context(request, response)
                    prefix = context.get("entry_prefix")
                    if prefix:
                        entries_by_hash[(prefix["market_id"], prefix["entry_hash"])] = prefix
                    if record:
                        for label, actual in (("request_body", request_artifact), (stage + "_body", artifact)):
                            expected = record.get(label, {})
                            if expected and (expected.get("sha256") != actual["sha256"] or expected.get("bytes") != actual["bytes"]):
                                reader.failure(response_path, "retained artifact differs from report size/hash")
                    if request.get("method") not in {"snapshot", "updates"} and response.get("ok") is not False:
                        continue
                    row = {"endpoint_directory": directory.name, "request_id": request_id, "stage": stage,
                        "endpoint": record.get("endpoint"), "request_artifact": request_artifact,
                        "response_artifact": artifact, "context": context,
                        "http_status": record.get(stage + "_http_status"),
                        **select(record, ("host_monotonic_us", "upstream_started_us", "upstream_completed_us",
                            "client_response_attempted_us", "client_response_completed_us", "handler_completed_us"))}
                    row["overlapping_client_rpc_rejections"] = correlated_rejections(record, context, report.get("rpc_calls", []))
                    rows.append(row)
                if not responses_seen:
                    reader.failure(request_path, "no retained response body; transport error=" + str(record.get("transport_error")))
            except (OSError, ValueError, TypeError, KeyError) as error:
                reader.failure(request_path, error)
        if requests_seen > MAX_REQUESTS or directory_entries_seen > MAX_REQUESTS * 4:
            break
    for row in rows:
        inspect_context(row["context"], entries_by_hash)
    rows.sort(key=lambda row: (row.get("host_monotonic_us", 0), row["endpoint_directory"], row["request_id"], row["stage"]))
    findings = Counter(finding["code"] for row in rows for finding in row["context"]["findings"])
    rpc_errors = [{"rpc_index": index, **select(call, ("method", "wallet", "sample_id", "market_id", "account_id",
        "start_host_us", "end_host_us", "error"))} for index, call in enumerate(report.get("rpc_calls", [])) if "error" in call]
    return {"format_version": 1, "fixture": str(fixture), "report_present": bool(report),
        "scope": "public wire coherence only; entry identity reconstructed, certificates and execution not authenticated",
        "old_failure_reproduced": "not inferred; consult observed wire findings and independently recorded RPC errors",
        "capture_complete_as_reported": report.get("public_trace", {}).get("complete_capture"),
        "analysis_complete": reader.failure_count == 0, "analysis_bytes_read": reader.bytes_read,
        "analysis_complete_meaning": "bounded retained-file scan succeeded; not a source-capture closure guarantee",
        "requests_without_report_metadata_count": unindexed_count,
        "requests_without_report_metadata": unindexed,
        "rpc_correlation_metadata_complete": bool(report) and unindexed_count == 0,
        "analysis_failure_count": reader.failure_count, "analysis_errors": reader.errors,
        "public_requests_seen": min(requests_seen, MAX_REQUESTS), "method_counts": dict(method_counts),
        "snapshot_updates_and_error_response_count": len(rows), "observed_entry_identities": len(entries_by_hash),
        "finding_counts": dict(findings), "client_rpc_errors": rpc_errors,
        "stale_history_rpc_rejection_count": sum(STALE_HISTORY in row["error"].get("rpc_message", "") for row in rpc_errors),
        "responses": rows}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path, help="retained fixture directory or its performance JSON report")
    parser.add_argument("--output", type=Path, help="new derived output file; never overwrites an existing artifact")
    options = parser.parse_args()
    result = analyze(options.fixture)
    output = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if options.output:
        with options.output.open("x", encoding="utf-8") as stream:
            stream.write(output)
    else:
        print(output, end="")
    return 0 if result["analysis_complete"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
