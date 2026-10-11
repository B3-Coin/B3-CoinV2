#!/usr/bin/env python3
"""Isolated native loopback probe. Never launches a node or opens wallet data.

Retains generated stores/logs and actual child exits. Does not promise WAN,
Qt/HTTPS, B3 reconciliation, native view-change or machine power-loss safety.
"""
import argparse
import hashlib
import json
import math
import pathlib
import platform
import queue
import statistics
import subprocess
import tempfile
import threading
import time


class Child:
    def __init__(self, binary, args, root, label):
        self.label = label
        self.events = []
        self.ready = queue.Queue()
        self.log = (root / f"{label}.jsonl").open("w")
        self.err = (root / f"{label}.stderr").open("w")
        self.proc = subprocess.Popen([str(binary), *map(str, args)], stdout=subprocess.PIPE,
                                     stderr=self.err, text=True)
        self.thread = threading.Thread(target=self.collect, daemon=True)
        self.thread.start()

    def collect(self):
        for line in self.proc.stdout:
            self.log.write(line)
            self.log.flush()
            try:
                event = json.loads(line)
                self.events.append(event)
                if event.get("event") == "ready":
                    self.ready.put(event)
            except json.JSONDecodeError:
                pass

    def wait(self, timeout=60):
        code = self.proc.wait(timeout=timeout)
        self.thread.join(timeout=2)
        self.log.close()
        self.err.close()
        return code


def checked(binary, args, root, label, expected=0, reason=None):
    run = subprocess.run([str(binary), *map(str, args)], capture_output=True,
                         text=True, timeout=60)
    (root / f"{label}.stdout").write_text(run.stdout)
    (root / f"{label}.stderr").write_text(run.stderr)
    if run.returncode != expected:
        raise RuntimeError(f"{label}: exit {run.returncode}, expected {expected}: {run.stderr}")
    if reason is not None and reason not in run.stderr:
        raise RuntimeError(f"{label}: wrong refusal reason: {run.stderr}")
    return [json.loads(line) for line in run.stdout.splitlines() if line.startswith("{")]


def distribution(values):
    values = sorted(values)
    return {"n": len(values), "median_ms": statistics.median(values)/1000,
            "p95_ms": values[math.ceil(.95*len(values))-1]/1000,
            "min_ms": values[0]/1000, "max_ms": values[-1]/1000}


def campaign(binary, root, measured, warmup):
    children = []
    summary = {"status": "FAILED", "exits": {}}
    try:
        ports = []
        for seat in (1, 2, 3):
            child = Child(binary, ["server", seat, root/f"seat{seat}", "none"], root, f"seat{seat}")
            children.append(child)
            ports.append(child.ready.get(timeout=30)["port"])
        leader = Child(binary, ["server", 0, root/"seat0", "none", *ports], root, "seat0")
        children.append(leader)
        port = leader.ready.get(timeout=30)["port"]
        # Contending opener must fail while an existing process owns the store.
        checked(binary, ["inspect", 0, root/"seat0"], root, "duplicate-owner", expected=1, reason="IO error: lock")
        client = Child(binary, ["client", port, root/"client", measured+warmup], root, "client")
        children.append(client)
        for child in reversed(children):
            code = child.wait(timeout=120)
            summary["exits"][child.label] = code
            if code or not any(e.get("event") == "clean_shutdown" for e in child.events):
                raise RuntimeError(f"{child.label}: exit {code} or missing clean shutdown")
        samples = [e for e in client.events if e.get("event") == "sample"]
        if len(samples) != measured+warmup or len({e["action_id"] for e in samples}) != len(samples):
            raise RuntimeError("sample/action count mismatch")
        replicas = [e for e in leader.events if e.get("event") == "replicas_applied"]
        if len(replicas) != len(samples):
            raise RuntimeError("replica application count mismatch")
        reopened = []
        for seat in range(4):
            reopened.extend(checked(binary, ["inspect", seat, root/f"seat{seat}"], root, f"reopen{seat}"))
        if len({e["state_root"] for e in reopened}) != 1 or any(e["committed"] != len(samples) for e in reopened):
            raise RuntimeError("reopened replicas differ")
        checked(binary, ["inspect-client", root/"client", len(samples)], root, "client-reopen")
        summary.update(status="PASS", warmup=warmup, measured=measured,
                       reopened=reopened,
                       latency={key: distribution([e[key] for e in samples[warmup:]])
                                for key in ("verified_us", "client_durable_us", "all_replicas_observed_us")})
    finally:
        # Only children created by this invocation. Preserve stores; no lock deletion.
        for child in children:
            if child.proc.poll() is None:
                child.proc.terminate()
                try:
                    child.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.proc.kill()
                    child.proc.wait(timeout=5)
                summary["exits"][child.label] = child.proc.returncode
        (root/"summary.json").write_text(json.dumps(summary, indent=2)+"\n")
    return summary


def interruptions(binary, root):
    results = []
    for cut in ("after-intent", "after-signed", "after-published", "after-decision", "after-applied"):
        store = root/cut
        published = checked(binary, ["exercise", store, cut], root, cut, expected=73)
        before = checked(binary, ["inspect", 0, store], root, f"{cut}-inspect")
        recovered = checked(binary, ["exercise", store, "none"], root, f"{cut}-recover")
        checked(binary, ["exercise", store, "none"], root, f"{cut}-duplicate")
        votes = [e["exact_vote"] for e in published+before+recovered if "exact_vote" in e]
        if votes and len(set(votes)) != 1:
            raise RuntimeError("recovery changed an issued vote")
        results.append({"cut": cut, "interruption_exit": 73, "reopen_exit": 0,
                        "recovery_exit": 0, "duplicate_exit": 0, "exact_vote_observations": len(votes)})
    for fault in ("missing-intent", "missing-snapshot", "bad-snapshot", "bad-record"):
        store = root/fault
        checked(binary, ["exercise", store, "none"], root, f"{fault}-setup")
        checked(binary, ["corrupt-test-store", store, fault], root, f"{fault}-inject")
        reason = {"missing-intent": "vote missing intent", "missing-snapshot": "missing/inconsistent applied snapshot",
                  "bad-snapshot": "missing/inconsistent applied snapshot", "bad-record": "bad stored packet"}[fault]
        checked(binary, ["inspect", 0, store], root, f"{fault}-refusal", expected=1, reason=reason)
        results.append({"fault": fault, "safe_refusal_exit": 1})
    (root/"interruptions.json").write_text(json.dumps(results, indent=2)+"\n")
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, required=True)
    parser.add_argument("--evidence", type=pathlib.Path, required=True)
    parser.add_argument("--samples", type=int, default=40)
    parser.add_argument("--warmup", type=int, default=2)
    parser.add_argument("--interruptions-only", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.samples <= 100 or not 0 <= args.warmup <= 10:
        parser.error("bounded samples 1..100 and warmup 0..10 required")
    binary = args.binary.resolve(strict=True)
    args.evidence.mkdir(parents=True, exist_ok=True)
    root = pathlib.Path(tempfile.mkdtemp(prefix="campaign-", dir=args.evidence))
    manifest = {"profile": "native-fast-view0-v1", "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
                "binary": str(binary), "platform": platform.platform(), "started_unix": time.time(),
                "samples": args.samples, "warmup": args.warmup,
                "boundary": "original signing + outbox sync to verified proof + terminal outbox sync",
                "excluded": ["Qt", "HTTPS", "B3 node/reconciliation", "native view-change", "WAN", "power-loss"]}
    (root/"manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
    print(json.dumps({"evidence": str(root)}), flush=True)
    if args.interruptions_only:
        print(json.dumps(interruptions(binary, root), indent=2))
    else:
        print(json.dumps(campaign(binary, root, args.samples, args.warmup), indent=2))


if __name__ == "__main__":
    main()
