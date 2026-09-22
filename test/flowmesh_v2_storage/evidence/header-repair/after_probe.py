"""AFTER measurement of the original timer-delayed header-ingress pattern.

Takes an explicit checkout. Reports HEAD, dirty state and loaded source hashes;
an uncommitted repair is never represented as part of its parent HEAD.
Only diagnostics and generated synthetic fixtures are used.
"""
from copy import deepcopy
import hashlib
import inspect
import json
from pathlib import Path
import subprocess
import sys


if len(sys.argv) != 2:
    raise SystemExit("Usage: python3.14 -B after_probe.py SOURCE_CHECKOUT")
source = Path(sys.argv[1]).resolve(strict=True)


def git(*arguments):
    return subprocess.check_output(
        ["git", "-C", str(source), *arguments], text=True).strip()


def source_hashes():
    result = {}
    for relative in ("test/flowmesh_v2_agreement", "test/flowmesh_v2_model"):
        for pattern in ("*.py", "*.json"):
            for path in sorted((source / relative).glob(pattern)):
                result[path.relative_to(source).as_posix()] = hashlib.sha256(
                    path.read_bytes()).hexdigest()
    return result


if git("rev-parse", "--show-prefix"):
    raise SystemExit("Provide the source repository root")
hashes_before = source_hashes()
provenance = {
    "type": "source_provenance", "head": git("rev-parse", "HEAD"),
    "dirty": bool(git("status", "--porcelain", "--untracked-files=all")),
    "python": sys.version.split()[0], "source_sha256": hashes_before,
    "probe_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
    "qualification": "diagnostic working-tree result if dirty; not final committed-tree qualification",
}
sys.path.insert(0, str(source / "test/flowmesh_v2_agreement"))
from fm_simulator import Simulator
from fm_application import canonical, value_id
from fm_protocol import PROFILE, message
from test_model import Fixture
import test_recovery

assert PROFILE["profile_id"] == "flowmesh-v2-single-sequence-pbft-test/4"


def measure_aggregate(node, sim, key):
    """Observe the real keyed call, separately from model diagnostic counters."""
    aggregate_code = node._aggregate.__func__.__code__
    lookup_code = node._lookup_header.__func__.__code__
    lines, start = inspect.getsourcelines(node._lookup_header)
    matching = [start + offset for offset, line in enumerate(lines)
                if line.strip() == "proposal = self.headers.get(key)"]
    assert len(matching) == 1, "Update explicit instrumentation for a changed lookup"
    lookup_line = matching[0]
    counts = dict(aggregate_calls=0, key_lookup_calls=0, header_cache_lookups=0)

    def trace(frame, event, arg):
        if frame.f_code is aggregate_code:
            if event == "call":
                counts["aggregate_calls"] += 1
            return trace
        if frame.f_code is lookup_code:
            if event == "call":
                counts["key_lookup_calls"] += 1
            elif event == "line" and frame.f_lineno == lookup_line:
                counts["header_cache_lookups"] += 1
            return trace
        return None

    # Excludes last_aggregation_work: updating diagnostic counters is intended.
    observed = lambda: (node.d, node.votes, node.headers, node.header_sizes,
        node.header_bytes, node.bodies, node.announced, node.requests,
        node.references, node.pending, node.offers, node.deadline,
        sim.events, sim.trace, sim.authentication.issued)
    before = deepcopy(observed())
    previous = sys.gettrace()
    assert previous is None, "Do not combine this probe with another trace hook"
    try:
        sys.settrace(trace)
        node._aggregate(key)
    finally:
        sys.settrace(previous)
    assert observed() == before, "Measurement changed semantic state or traffic"
    assert counts == dict(aggregate_calls=1, key_lookup_calls=1, header_cache_lookups=1)
    return counts


print(json.dumps(provenance, sort_keys=True), flush=True)
sim = Simulator(Fixture().model.snapshot(), byzantine=(0,))
node = sim.nodes[1]
peak_cleanup_work = {}
for i in range(300):
    sim.now = i * (PROFILE["admission"]["lease_ticks"] + 1)
    body = test_recovery.RecoveryTests().body(sim, 1000 + i)
    identity = value_id(body)
    proposal = sim.authentication.adversary_sign(message(
        "PROPOSE", 0, node.instance, 0, identity, new_view=None))
    node.receive("DATA", {"type": "body", "id": identity, "object": body,
                          "reference": {"kind": "SIGNED", "data": proposal}}, 0)
    node.pump()
    for name, count in node.last_header_work.items():
        peak_cleanup_work[name] = max(peak_cleanup_work.get(name, 0), count)

    if i + 1 in (100, 200, 300):
        actual = measure_aggregate(node, sim, (0, identity))
        encoded_headers = [{"key": [view, vid], "proposal": signed}
                           for (view, vid), signed in node.headers.items()]
        byte_sum = sum(len(canonical({"slot": [view, vid], "signed": signed}))
                       for (view, vid), signed in node.headers.items())
        assert byte_sum == node.header_bytes == sum(node.header_sizes.values())
        assert set(node.header_sizes) == set(node.headers)
        print(json.dumps({
            "type": "measurement", "inputs": i + 1,
            "diagnostic": "original_header_pattern_on_repaired_tree",
            "profile": PROFILE["profile_id"],
            "clock_step": PROFILE["admission"]["lease_ticks"] + 1,
            "tick_calls": 0, "time": sim.now, "deadline": node.deadline,
            "view": node.record["view"], "headers": len(node.headers),
            "header_lookup_entries": len(node.headers),
            "header_size_index_entries": len(node.header_sizes),
            "aggregation_secondary_index_entries": None,
            "bodies": len(node.bodies), "references": len(node.references),
            "requests": len(node.requests), "pending": len(node.pending),
            "received_vote_buckets": len(node.votes),
            "accepted": len(node.record["accepted"]),
            "prepared": len(node.record["prepared"]),
            "serialized_header_cache_bytes": len(canonical(encoded_headers)),
            "serialized_signed_headers_bytes": sum(len(canonical(h))
                                                    for h in node.headers.values()),
            "budget_accounted_header_entry_bytes": byte_sum,
            "header_count_cap": PROFILE["admission"]["headers"],
            "header_byte_cap": PROFILE["admission"]["header_bytes"],
            "actual_keyed_aggregation_observations": actual,
            "reported_aggregation_work": node.last_aggregation_work,
            "peak_reported_header_cleanup_work": peak_cleanup_work,
            "measurement_changed_semantic_state": False,
            "halt": node.d["halt"],
            "caveat": "timer servicing deliberately withheld; no initial preparedQC and not a fair-tick liveness claim"
        }, sort_keys=True), flush=True)

assert source_hashes() == hashes_before, "Source changed during measurement"
assert git("rev-parse", "HEAD") == provenance["head"], "HEAD changed during measurement"
print(json.dumps({"type": "completed", "loaded_source_unchanged": True,
                  "inputs": 300, "tick_calls": 0, "child_processes": 0}, sort_keys=True))
