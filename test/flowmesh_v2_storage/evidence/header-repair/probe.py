"""Frozen header diagnostic plus read-only work measurements.

The proposal schedule is the retained 300-input diagnostic: valid synthetic
Byzantine proposer0, DATA plus PROPOSE reference, distinct bodies, 31-tick
clock increments, and deliberately no timer servicing. This is not a fair-tick
liveness counterexample. Measurement calls to the existing _aggregate method
are separately labelled and assert that they change no model state.
"""
import ast
from copy import deepcopy
import inspect
import json
import sys
import textwrap

from _frozen_baseline import source_from_command_line

source = source_from_command_line()
sys.path.insert(0, str(source / "test/flowmesh_v2_agreement"))
from fm_simulator import Simulator
from fm_application import canonical, value_id
from fm_protocol import PROFILE, message
from test_model import Fixture
import test_recovery


def measure_aggregate(node, sim):
    """Count actual loop-body entries with a trace hook, not estimated len()."""
    method = node._aggregate
    lines, start = inspect.getsourcelines(method)
    parsed = ast.parse(textwrap.dedent("".join(lines)))
    loop = parsed.body[0].body[0]
    assert isinstance(loop, ast.For)
    assert ast.unparse(loop.iter) == "list(self.headers.items())"
    inspection_line = start + loop.body[0].lineno - 1
    code = method.__func__.__code__
    counts = {"calls": 0, "headers_inspected": 0}

    def trace(frame, event, arg):
        if frame.f_code is code:
            if event == "call":
                counts["calls"] += 1
            elif event == "line" and frame.f_lineno == inspection_line:
                counts["headers_inspected"] += 1
            return trace
        return None

    # The existing aggregate helper can act when quorum evidence exists.
    # This diagnostic supplies none: prove measurement is a no-op here.
    observed = lambda: (node.d, node.votes, node.headers, node.bodies,
        node.announced, node.requests, node.references, node.pending,
        node.offers, node.deadline, sim.events, sim.trace,
        sim.authentication.issued)
    before = deepcopy(observed())
    previous = sys.gettrace()
    assert previous is None, "Do not combine this probe with another trace hook"
    try:
        sys.settrace(trace)
        method()
    finally:
        sys.settrace(previous)
    assert observed() == before, "The diagnostic aggregate call changed state"
    assert counts["calls"] == 1
    return counts


sim = Simulator(Fixture().model.snapshot(), byzantine=(0,))
node = sim.nodes[1]
for i in range(300):
    sim.now = i * (PROFILE["admission"]["lease_ticks"] + 1)
    body = test_recovery.RecoveryTests().body(sim, 1000 + i)
    identity = value_id(body)
    proposal = sim.authentication.adversary_sign(message(
        "PROPOSE", 0, node.instance, 0, identity, new_view=None))
    node.receive("DATA", {"type": "body", "id": identity, "object": body,
                          "reference": {"kind": "SIGNED", "data": proposal}}, 0)
    node.pump()

    if i + 1 in (100, 200, 300):
        work = measure_aggregate(node, sim)
        encoded_headers = [{"key": [view, vid], "proposal": signed}
                           for (view, vid), signed in node.headers.items()]
        print(json.dumps({
            "diagnostic": "header_growth_without_timer_servicing",
            "baseline": "00d41abc85c3fc15d6c4191bcafd699cc16b7836",
            "profile": PROFILE["profile_id"],
            "inputs": i + 1, "clock_step": PROFILE["admission"]["lease_ticks"] + 1,
            "tick_calls": 0, "time": sim.now, "deadline": node.deadline,
            "view": node.record["view"], "headers": len(node.headers),
            "header_lookup_entries": len(node.headers),
            "aggregation_secondary_index_entries": None,
            "bodies": len(node.bodies), "references": len(node.references),
            "requests": len(node.requests), "pending": len(node.pending),
            "received_vote_buckets": len(node.votes),
            "accepted": len(node.record["accepted"]),
            "serialized_header_cache_bytes": len(canonical(encoded_headers)),
            "serialized_signed_headers_bytes": sum(len(canonical(h))
                                                    for h in node.headers.values()),
            "diagnostic_aggregate_calls": work["calls"],
            "actual_aggregate_header_inspections": work["headers_inspected"],
            "measurement_changed_model_state": False,
            "halt": node.d["halt"],
            "caveat": "timer servicing intentionally withheld; not the R2 fair-tick schedule"
        }, sort_keys=True), flush=True)
