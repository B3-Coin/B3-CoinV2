#!/usr/bin/env python3
"""Connect the pinned Ethereum peer adapter to B3's native proof verifier.

The collector supplies untrusted proof bytes through an OS pipe. Only the
native process verifies and durably accepts light-client updates. Saved JSON
diagnostics are not source capabilities and cannot enable the Arbitrum bridge.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import re
import subprocess
import tempfile
import threading
import time

from build_peer_collector import bounded_version, clean_environment, sha256, NIMBUS_COMMIT, FORMAT

MAX_LINE = 262144
MAX_RESULTS = 256
MAX_DIAGNOSTIC_BYTES = 4 * 1024 * 1024
MAX_LOG_BYTES = 1024 * 1024


def pinned_binary(path: Path, expected: str) -> Path:
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise ValueError("Executable pin must be a lowercase SHA256 digest")
    if path.is_symlink():
        raise ValueError("Pinned executable cannot be a symlink")
    actual = path.resolve(strict=True)
    if not actual.is_file() or sha256(actual) != expected:
        raise ValueError("Executable does not match its explicit content pin")
    return actual


def policy_input(path: Path) -> tuple[Path, dict]:
    if path.is_symlink():
        raise ValueError("Policy cannot be a symlink")
    actual = path.resolve(strict=True)
    if not actual.is_file() or actual.stat().st_size > 32768:
        raise ValueError("Policy is missing or exceeds its input bound")
    policy = json.loads(actual.read_text(encoding="utf-8"))
    if (policy.get("format") != "b3-ethereum-peer-policy/1" or
            policy.get("mainnet_enabled") is not False or type(policy.get("chain_id")) is not int or
            policy.get("chain_id") != 1 or
            not re.fullmatch(r"0x[0-9a-fA-F]{64}", str(policy.get("trusted_root", "")))):
        raise ValueError("An explicit disabled Ethereum-only checkpoint policy is required")
    return actual, policy


def backend_arguments(binary: Path, policy: dict, directory: str, port: int) -> list[str]:
    if type(port) is not int or not 1024 <= port <= 65533:
        raise ValueError("Peer base port must leave three unprivileged ports")
    return [str(binary), "--network=mainnet", "--no-el", "--nat=none",
            "--max-peers=8", "--hard-max-peers=12", "--metrics=false",
            "--trusted-block-root=" + policy["trusted_root"], "--data-dir=" + directory,
            "--tcp-port=" + str(port), "--udp-port=" + str(port),
            "--quic-port=" + str(port + 1)]


def valid_result(value: object) -> dict:
    if not isinstance(value, dict) or value.get("format") != "b3-ethereum-peer-verification/1":
        raise ValueError("Native verifier emitted an unsupported diagnostic")
    if (value.get("mainnet_enabled") is not False or
            value.get("arbitrum_authority") is not False or
            value.get("writes_enabled") is not False):
        raise ValueError("Native verifier diagnostic crossed the read-only boundary")
    return value


def final_anchor_verified(value: dict | None) -> bool:
    return (value is not None and value.get("result") == "READY" and
            value.get("fresh_finalized_execution") is True and
            value.get("fenced") is False)


def consistent_result(previous: dict | None, value: dict) -> None:
    if (type(value.get("sequence")) is not int or not 0 <= value["sequence"] <= 65536 or
            any(not re.fullmatch(r"[0-9a-f]{64}", str(value.get(key, "")))
                for key in ("policy_sha256", "journal_sha256"))):
        raise ValueError("Native provenance fields are malformed")
    if previous is not None and (
            value["policy_sha256"] != previous["policy_sha256"] or
            value["sequence"] < previous["sequence"] or
            (value["sequence"] == previous["sequence"] and
             value["journal_sha256"] != previous["journal_sha256"])):
        raise ValueError("Native policy or journal provenance changed inconsistently")


def _drain_log(pipe, result: dict, name: str) -> None:
    digest, total = hashlib.sha256(), 0
    while True:
        chunk = pipe.read(16384)
        if not chunk:
            break
        total += len(chunk)
        # Retain only hashes and bounded counts; peer logs never become public
        # proof authority or unbounded memory/disk output.
        if total <= MAX_LOG_BYTES:
            digest.update(chunk)
    result[name] = {"bytes": min(total, MAX_LOG_BYTES + 1),
                    "truncated": total > MAX_LOG_BYTES, "bounded_sha256": digest.hexdigest()}


def run_session(backend: Path, verifier: Path, policy_path: Path, policy: dict,
                journal: Path, seconds: int, port: int) -> dict:
    if type(seconds) is not int or not 1 <= seconds <= 86400:
        raise ValueError("Session duration must be between one second and one day")
    env = clean_environment()
    version = bounded_version(backend)
    if version != {
            "format": FORMAT, "upstream_commit": NIMBUS_COMMIT, "authority": "availability-only"}:
        raise ValueError("Backend is not the reviewed pinned Nimbus proof adapter")
    events: queue.Queue = queue.Queue(maxsize=8)
    stopped = threading.Event()
    logs, results = {}, []
    collector = native = None
    threads = []
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="b3-eth-peer-") as directory:
        try:
            collector = subprocess.Popen(backend_arguments(backend, policy, directory, port),
                                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                         stderr=subprocess.PIPE, env=env)
            native = subprocess.Popen([str(verifier), "--policy", str(policy_path),
                                       "--journal", str(journal)], stdin=collector.stdout,
                                      stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
            collector.stdout.close()
            def post(kind: str, value) -> None:
                while not stopped.is_set():
                    try:
                        events.put((kind, value), timeout=0.1)
                        return
                    except queue.Full:
                        pass
            def read_results() -> None:
                try:
                    while not stopped.is_set():
                        raw = native.stdout.readline(MAX_LINE + 1)
                        if not raw:
                            post("eof", None)
                            return
                        if len(raw) > MAX_LINE or not raw.endswith(b"\n"):
                            raise ValueError("Native diagnostic frame exceeds its bound")
                        post("result", valid_result(json.loads(raw)))
                except Exception:
                    post("error", None)
            threads = [threading.Thread(target=read_results, daemon=True)]
            for name, pipe in (("backend", collector.stderr), ("native", native.stderr)):
                threads.append(threading.Thread(target=_drain_log, args=(pipe, logs, name), daemon=True))
            for thread in threads:
                thread.start()
            total, dropped, live_updates, reason, last_result = 0, 0, 0, "deadline", None
            first_anchor_slot = None
            def retain(value: dict) -> None:
                nonlocal total, dropped, live_updates, last_result, first_anchor_slot
                consistent_result(last_result, value)
                anchor = value.get("ethereum_finalized_execution")
                if first_anchor_slot is None and isinstance(anchor, dict):
                    slot = anchor.get("beacon_slot")
                    if type(slot) is int and slot >= 0:
                        first_anchor_slot = slot
                if (value.get("result") == "OK" and value.get("accepted") is True and
                        value["sequence"] >= 2 and
                        (last_result is None or value["sequence"] > last_result["sequence"])):
                    live_updates = min(2**32 - 1, live_updates + 1)
                last_result = value
                size = len(json.dumps(value))
                if len(results) < MAX_RESULTS and total + size <= MAX_DIAGNOSTIC_BYTES:
                    total += size
                    results.append(value)
                else:
                    dropped = min(2**32 - 1, dropped + 1)
            while time.monotonic() - started < seconds:
                try:
                    kind, value = events.get(timeout=min(0.2, max(0.01, seconds - (time.monotonic() - started))))
                except queue.Empty:
                    if native.poll() is not None:
                        raise ValueError("Native verifier exited before the session deadline")
                    continue
                if kind != "result":
                    raise ValueError("Native proof stream stopped or emitted invalid diagnostics")
                retain(value)
                if len(results) >= MAX_RESULTS - 1 or total >= MAX_DIAGNOSTIC_BYTES - MAX_LINE:
                    reason = "bounded-result-limit"
                    break
            # Stop the availability producer first. Its pipe then reaches EOF,
            # so the native verifier can finish its journal and emit a final
            # clock-checked READY before exiting. A killed/unhealthy verifier
            # cannot leave a historical fresh row labelled currently verified.
            if collector.poll() is not None or native.poll() is not None:
                raise ValueError("Peer or native process exited unexpectedly")
            collector.terminate()
            collector.wait(timeout=10)
            eof_deadline = time.monotonic() + 15
            while True:
                remaining = eof_deadline - time.monotonic()
                if remaining <= 0:
                    raise ValueError("Native verifier did not finish its durable proof stream")
                try:
                    kind, value = events.get(timeout=min(0.2, remaining))
                except queue.Empty:
                    continue
                if kind == "eof":
                    break
                if kind != "result":
                    raise ValueError("Native verifier did not close with valid diagnostics")
                retain(value)
            if native.wait(timeout=5) != 0 or last_result is None or last_result.get("result") != "READY":
                raise ValueError("Native verifier refused its final durable/freshness status")
            fresh = final_anchor_verified(last_result)
            bls_verified = fresh and last_result["sequence"] >= 2
            final_slot = (last_result.get("ethereum_finalized_execution") or {}).get("beacon_slot")
            head_advanced = (type(final_slot) is int and first_anchor_slot is not None and
                             final_slot > first_anchor_slot)
            result = ("ETHEREUM_LIVE_UPDATE_VERIFIED" if bls_verified and live_updates else
                      "ETHEREUM_VERIFIED_REPLAY_ONLY" if bls_verified else
                      "TRUSTED_BOOTSTRAP_ONLY" if fresh else "NO_FRESH_ANCHOR")
            return {"format": "b3-ethereum-peer-session/1", "mainnet_enabled": False,
                    "arbitrum_authority": False, "writes_enabled": False,
                    "result": result, "native_bls_update_verified": bls_verified,
                    "live_bls_update_verified": bool(fresh and live_updates),
                    "accepted_live_updates": live_updates,
                    "finalized_head_advanced_this_run": bool(fresh and head_advanced),
                    "stop_reason": reason, "duration_seconds": round(time.monotonic() - started, 3),
                    "native_results": results, "final_native_result": last_result,
                    "omitted_intermediate_results": dropped, "logs": logs,
                    "limitations": ["Ethereum finalized anchors only; no Arbitrum execution/source authority",
                                    "Bootstrap assumes an explicitly trusted recent weak-subjectivity checkpoint",
                                    "Missing or censored peer proofs stop progress and never grant credit"]}
        finally:
            stopped.set()
            # Only children created by this call are stopped. The native
            # journal recovery fences a torn tail; a partial frame never grants
            # authority merely because the owner reached its time bound.
            for process in (collector, native):
                if process is not None and process.poll() is None:
                    process.terminate()
            for process in (collector, native):
                if process is not None:
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=10)
            for thread in threads:
                thread.join(timeout=2)
            for process in (collector, native):
                if process is not None:
                    for stream in (process.stdout, process.stderr):
                        if stream is not None:
                            stream.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", type=Path, required=True)
    parser.add_argument("--backend-sha256", required=True)
    parser.add_argument("--verifier", type=Path, required=True)
    parser.add_argument("--verifier-sha256", required=True)
    parser.add_argument("--policy", type=Path, required=True)
    parser.add_argument("--journal", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=300)
    parser.add_argument("--peer-base-port", type=int, default=19000)
    args = parser.parse_args()
    if os.name != "posix":
        raise ValueError("The reviewed owned peer process and durable journal require Linux/WSL")
    backend = pinned_binary(args.backend, args.backend_sha256)
    verifier = pinned_binary(args.verifier, args.verifier_sha256)
    policy_path, policy = policy_input(args.policy)
    report = args.report.absolute()
    if report.exists() or report.is_symlink():
        raise ValueError("Public session report must be a fresh path")
    report.parent.mkdir(parents=True, exist_ok=True)
    result = run_session(backend, verifier, policy_path, policy, args.journal.absolute(),
                         args.seconds, args.peer_base_port)
    result.update(backend_sha256=args.backend_sha256, verifier_sha256=args.verifier_sha256,
                  policy_file_sha256=sha256(policy_path))
    with report.open("x", encoding="utf-8") as stream:
        stream.write(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"result": result["result"], "mainnet_enabled": False,
                      "arbitrum_authority": False}, sort_keys=True))
    return 0 if result["result"] == "ETHEREUM_LIVE_UPDATE_VERIFIED" else 2


if __name__ == "__main__":
    raise SystemExit(main())
