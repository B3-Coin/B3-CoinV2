# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Bounded PUBLIC trading-body diagnostics; never capture HTTP credentials.

Full bytes are separate artifacts. Compact contexts are observations only,
not independent certificate/execution verification. A bound or write failure
is explicit and makes complete_capture false; forwarding behavior is unchanged.
"""

from collections import Counter
import hashlib
import json
from pathlib import Path
import threading


MAX_CAPTURE_BYTES = 256 * 1024 * 1024
MAX_CAPTURE_FILES = 32768
MAX_CAPTURE_BODY = 32 * 1024 * 1024
MAX_CAPTURE_ERRORS = 32
IDENTITY_FIELDS = ("market_id", "account_id", "domain", "execution_config_id", "action_id")
HEAD_FIELDS = ("next_microblock_sequence", "microblock_sequence", "sequence", "last_microblock_hash",
               "microblock_hash", "state_root", "anchor_height", "anchor_hash", "running", "paused",
               "chain_reconciling", "observer_only", "halt", "error")


def selected(value, fields):
    return {key: value[key] for key in fields if key in value} if isinstance(value, dict) else {}


def public_request_context(request):
    if not isinstance(request, dict):
        return {"malformed": True}
    params = request.get("params", {})
    context = {"method": request.get("method"), **selected(params, IDENTITY_FIELDS)}
    context.update(selected(params, ("cursor", "known_head", "expected_head", "before_sequence", "limit", "effect_id")))
    if isinstance(params, dict) and isinstance(params.get("action_hex"), str):
        try:
            payload = bytes.fromhex(params["action_hex"])
            if len(payload) >= 41:
                # Public signer prefix in the existing production Action codec.
                context["account_id"] = payload[:32][::-1].hex()
                context["account_sequence"] = int.from_bytes(payload[32:40], "little")
                context["action_type"] = payload[40]
        except ValueError:
            context["action_payload_decode_error"] = True
    return context


def public_response_context(body):
    try:
        response = json.loads(body)
    except (ValueError, UnicodeError):
        return {"json_valid": False}
    if not isinstance(response, dict):
        return {"json_valid": True, "json_type": type(response).__name__}
    context = {"json_valid": True, **selected(response, ("ok", "error"))}
    result = response.get("result", response)
    if not isinstance(result, dict):
        context["result_type"] = type(result).__name__
        if isinstance(result, list):
            context["result_count"] = len(result)
            context["market_contexts"] = [selected(row, IDENTITY_FIELDS + HEAD_FIELDS) for row in result[:256]]
        return context
    context.update(selected(result, IDENTITY_FIELDS + HEAD_FIELDS + (
        "receipt_state", "reason", "accepted", "certificate_verified", "outcome_verified", "evidence_error",
        "cursor", "gap", "more", "oldest_event_id", "latest_event_id")))
    for field in ("status", "snapshot"):
        if isinstance(result.get(field), dict):
            context[field] = selected(result[field], IDENTITY_FIELDS + HEAD_FIELDS)
    if isinstance(result.get("events"), list):
        events = result["events"]
        context["events"] = {"count": len(events), "context_truncated": len(events) > 256,
            "entries": [selected(row, IDENTITY_FIELDS + HEAD_FIELDS + (
                "event_id", "kind", "signed_action_hash", "receipt_state", "reason")) for row in events[:256]]}
    reported = result.get("reported_data", result)
    if isinstance(reported, dict):
        projection = selected(reported, IDENTITY_FIELDS + ("unchanged",))
        projection["snapshot"] = selected(reported.get("snapshot"), HEAD_FIELDS)
        projection["account"] = selected(reported.get("account"), (
            "account_id", "next_sequence", "base_available", "base_reserved", "b3_available_atoms", "b3_reserved_atoms"))
        history = reported.get("history")
        if isinstance(history, dict):
            summary = selected(history, ("available", "truncated", "oldest_retained_sequence", "next_before_sequence"))
            entries = history.get("entries")
            if isinstance(entries, list):
                summary["entry_count"] = len(entries)
                summary["heads"] = [selected(row, ("sequence", "microblock_hash", "state_root")) for row in entries[:256]]
                sequences = [row.get("sequence") for row in entries if isinstance(row, dict) and type(row.get("sequence")) is int]
                summary["sequence_range"] = [min(sequences), max(sequences)] if sequences else None
                summary["strictly_descending"] = all(left > right for left, right in zip(sequences, sequences[1:]))
                summary["context_truncated"] = len(entries) > 256
            projection["history"] = summary
        if any(projection.values()):
            context["reported_data"] = projection
    for field in ("certified_payload", "state_bytes"):
        if isinstance(result.get(field), str):
            try:
                raw = bytes.fromhex(result[field])
                context[field] = {"bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
            except ValueError:
                context[field] = {"invalid_hex": True}
    return context


def reply_rejected(record):
    context = record.get("upstream_context", record.get("relay_error_context", {}))
    return (record.get("upstream_http_status", record.get("client_http_status", 200)) >= 400 or
            context.get("ok") is False or context.get("receipt_state") == "rejected")


class PublicBodyCapture:
    def __init__(self, directory, *, max_bytes=MAX_CAPTURE_BYTES, max_files=MAX_CAPTURE_FILES):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=False)
        self.max_bytes, self.max_files = max_bytes, max_files
        self.bytes_reserved = self.files_reserved = self.failure_count = 0
        self.errors = []
        self.lock = threading.Lock()

    def note_failure(self, reason, *, request_id=None, stage=None):
        with self.lock:
            self.failure_count += 1
            if len(self.errors) < MAX_CAPTURE_ERRORS:
                self.errors.append({"request_id": request_id, "stage": stage, "error": reason})

    def capture(self, request_id, stage, body):
        assert stage in {"request", "upstream", "delivered", "relay_error"}
        digest = hashlib.sha256(body).hexdigest()
        metadata = {"bytes": len(body), "sha256": digest, "complete": False}
        with self.lock:
            if len(body) > MAX_CAPTURE_BODY or self.bytes_reserved + len(body) > self.max_bytes or self.files_reserved >= self.max_files:
                self.failure_count += 1
                metadata["error"] = "bounded public capture budget exhausted"
                if len(self.errors) < MAX_CAPTURE_ERRORS:
                    self.errors.append({"request_id": request_id, "stage": stage, "error": metadata["error"]})
                return metadata
            self.bytes_reserved += len(body)
            self.files_reserved += 1
        path = self.directory / f"{request_id:08d}-{stage}.json"
        try:
            with path.open("xb") as stream:
                stream.write(body)
            metadata.update(path=str(path), complete=True)
        except OSError as error:
            with self.lock:
                self.failure_count += 1
                if len(self.errors) < MAX_CAPTURE_ERRORS:
                    self.errors.append({"request_id": request_id, "stage": stage, "error": type(error).__name__})
            metadata["error"] = type(error).__name__
        return metadata

    def snapshot(self):
        with self.lock:
            return {"directory": str(self.directory), "bytes_reserved": self.bytes_reserved,
                    "files_reserved": self.files_reserved, "max_bytes": self.max_bytes,
                    "max_files": self.max_files, "failure_count": self.failure_count,
                    "complete_capture": self.failure_count == 0, "errors": list(self.errors)}


def attribute_requests(records, samples, rpc_calls):
    """Join only explicit identities and bounded call intervals; never guess.

    A method lacking an account/action may overlap several in-flight wallet
    calls. Such a request remains shared/ambiguous, even if one match seems
    more likely. Count each network request once, including rejected replies.
    """
    samples_by_id = {row["sample_id"]: row for row in samples}
    counts = {key: Counter() for key in samples_by_id}
    rejected = {key: Counter() for key in samples_by_id}
    api_rejected = {key: Counter() for key in samples_by_id}
    totals, unattributed = Counter(), Counter()
    for record in records:
        record.pop("attributed_sample_id", None)
        record.pop("attribution", None)
        method = record["method"]
        totals[method] += 1
        context = record.get("request_context", {})
        market, action = context.get("market_id"), context.get("action_id")
        candidates = {key for key, sample in samples_by_id.items()
                      if action is not None and sample.get("action_id") == action and sample.get("market_id") == market}
        basis = "exact_market_action" if candidates else "unique_matching_active_rpc_interval"
        if not candidates and action is None:
            stamp = record["host_monotonic_us"]
            for call in rpc_calls:
                sample_id = call.get("sample_id")
                if sample_id not in samples_by_id or not call["start_host_us"] <= stamp <= call.get("end_host_us", stamp):
                    continue
                sample = samples_by_id[sample_id]
                if market is not None and market != sample["market_id"]:
                    continue
                account = context.get("account_id")
                if account is not None and account != sample.get("account_id"):
                    continue
                candidates.add(sample_id)
        record["attribution_candidate_sample_ids"] = sorted(candidates)
        if len(candidates) == 1:
            sample_id = next(iter(candidates))
            record["attributed_sample_id"] = sample_id
            record["attribution"] = basis
            counts[sample_id][method] += 1
            if record.get("upstream_http_status", record.get("client_http_status", 200)) >= 400:
                rejected[sample_id][method] += 1
            if reply_rejected(record):
                api_rejected[sample_id][method] += 1
        else:
            unattributed[method] += 1
            record["attribution"] = "shared_or_ambiguous" if candidates else "setup_or_unattributed"
    return {"method_totals": dict(totals), "shared_or_unattributed": dict(unattributed),
            "per_sample": {str(key): {"requests": dict(counts[key]), "http_rejections": dict(rejected[key]),
                                      "rejected_replies": dict(api_rejected[key]),
                                      "total": sum(counts[key].values())} for key in samples_by_id}}
