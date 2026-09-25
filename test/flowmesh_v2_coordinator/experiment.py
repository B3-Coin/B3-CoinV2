"""Isolate coordinator selection; ticks are NOT native trading latency.

The temporary profile is hashed into all agreement instances. Cases run
sequentially; no replica/store from one profile may operate in another scope.
This reuses, rather than copies, the accounting/agreement/storage models.
"""
from collections import Counter
from contextlib import contextmanager
from copy import deepcopy
import json
from pathlib import Path
import sys
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
for folder in ("flowmesh_v2_agreement", "flowmesh_v2_storage"):
    sys.path.insert(0, str(ROOT / "test" / folder))

from fm_application import Application, canonical, value_id
from fm_checker import check
from fm_protocol import PROFILE, Proofs, proposer
from fm_simulator import Simulator
from model import Model, action, action_id
from test_model import Fixture, BUYER, SELLER, COIN, USD_A, ident, limit_curve

EXPERIMENT = json.loads(Path(__file__).with_name("TEST_PROFILE.json").read_text())
_active = False


@contextmanager
def profile(span):
    """Select only a declared experiment, before constructing any test replicas."""
    global _active
    if _active or type(span) is not int or span not in EXPERIMENT["alternatives"]:
        raise ValueError("EXCLUSIVE_DECLARED_TEST_PROFILE_REQUIRED")
    selected = deepcopy(PROFILE)
    selected.update(profile_id=f"flowmesh-coordinator-test/1/{span}",
                    proposer=EXPERIMENT["selection"],
                    coordinator_experiment={"version": "bounded-tenure/1",
                                            "batches_per_coordinator": span})
    _active = True
    try:
        with patch.dict(PROFILE, selected, clear=True):
            yield deepcopy(selected)
    finally:
        _active = False


def fixture():
    f = Fixture()
    f.fund(BUYER, USD_A, 2_000_000)
    f.fund(SELLER, COIN, 100)
    return f


def trade(f, sequence=0):
    return {"actions": [
        action(BUYER, sequence, "OPEN", market=f.market, side="BUY",
               curve=limit_curve("BUY", 20_000, 1)),
        action(SELLER, sequence, "OPEN", market=f.market, side="SELL",
               curve=limit_curve("SELL", 20_000, 1)),
    ]}


def burst_fixture():
    f = Fixture()
    actions = []
    for i in range(8):
        account, side = ident(200 + i), "BUY" if i < 4 else "SELL"
        f.fund(account, USD_A if side == "BUY" else COIN,
               20_001 if side == "BUY" else 1)
        actions.append(action(account, 0, "OPEN", market=f.market, side=side,
                              curve=limit_curve(side, 20_000, 1)))
    return f, {"actions": actions}


def verify_applied(sim, index, sequence, original):
    """Read one replica's durable record; never treat pool admission as final.

    Synthetic client verification, not the native remote-client proof format.
    The external historical checker is run separately for all issued votes.
    """
    node = sim.nodes[index]
    record = node.d["records"].get(sequence)
    if not record or not record["applied"]:
        return None
    cert, body = record["decision"], record["body"]
    verified = Proofs(sim.n, sim.authentication.verify).check("commit", cert, record["instance"])
    if value_id(body) != verified["value"] or canonical(body) != canonical(original):
        raise AssertionError("CLIENT_EXACT_INSTRUCTION_OR_VALUE_MISMATCH")
    snapshot = Application(record["before"]).validate(
        body, record["instance"], record["anchor_before"], node.anchors)
    if Model.restore(snapshot).digest() != body["result"] or record["apply_count"] != 1:
        raise AssertionError("CLIENT_RESULT_OR_APPLICATION_MISMATCH")
    return verified


def finish_batch(sim, body, required, budget=180):
    start, sequence = sim.now, body["instance"]["sequence"]
    before = len(sim.authentication.issued)
    delivered_before = sim.delivered
    observations = {}
    # Exactly one original offer; recovery does not submit another request.
    sim.offer(body=body, nodes=required)

    def done(s):
        for i in required:
            if i not in observations and verify_applied(s, i, sequence, body):
                observations[i] = s.now
        return len(observations) == len(required)

    sim.run(budget, stop=done)
    audit = check(sim)
    if sim.exhausted or any(sim.nodes[i].d["halt"] for i in required):
        raise AssertionError("EXPERIMENT_RESOURCE_OR_SAFETY_HALT")
    completed = len(observations) == len(required)
    issued = sim.authentication.issued[before:]
    phase_counts = Counter(x["payload"]["phase"] for x in issued)
    proof = verify_applied(sim, required[0], sequence, body) if completed else None
    return {"sequence": sequence, "value": value_id(body), "completed": completed,
            "original_action_ids": [action_id(a) for a in body["batch"]["actions"]],
            "first_verified_durable_ticks": min(observations.values()) - start if observations else None,
            "all_required_applied_ticks": max(observations.values()) - start if completed else None,
            "replica_observation_ticks": {str(i): at - start for i, at in observations.items()},
            "deciding_view": proof["view"] if proof else None,
            "deciding_proposer": proof["sender"] if proof else None,
            "delivered_messages": sim.delivered - delivered_before,
            "issued_phases": dict(sorted(phase_counts.items())),
            "checker": audit}


def campaign(span, unavailable=(), batches=8, burst=False):
    with profile(span):
        f, batch = burst_fixture() if burst else (fixture(), None)
        sim = Simulator(f.model.snapshot())
        required = [i for i in range(4) if i not in unavailable]
        for i in unavailable:
            sim.nodes[i].crash()
        rows = []
        for sequence in range(1 if burst else batches):
            node = sim.nodes[required[0]]
            body = node.application.build(node.instance, node.d["anchor"],
                                          batch if burst else trade(f, sequence))
            row = finish_batch(sim, body, required)
            rows.append(row)
            if not row["completed"]:
                break
        final = Model.restore(sim.nodes[required[0]].d["snapshot"])
        final.assert_invariants()
        applied = sim.nodes[required[0]].d["sequence"]
        if not burst and (
                final._get("spot", BUYER, COIN) != applied
                or final._get("spot", SELLER, COIN) != 100 - applied
                or final._get("spot", BUYER, USD_A) != 2_000_000 - 20_001 * applied
                or final._get("spot", SELLER, USD_A) != 19_999 * applied):
            raise AssertionError("EXPECTED_MATCHED_FILL_OR_QUOTE_FEE_MISSING")
        if any(sim.nodes[i].d["snapshot"] != sim.nodes[required[0]].d["snapshot"] for i in required):
            raise AssertionError("REPLICA_ACCOUNTING_DIVERGENCE")
        return {"span": span, "unavailable": list(unavailable), "burst": burst,
                "time_unit": "logical_tick_not_millisecond", "samples": rows,
                "final_accounting_digest": final.digest(),
                "applied_batches": applied,
                "trace": json.loads(sim.retain_trace())}


def placement_effect(placement, unavailable_hosts):
    """Host groups are correlated failures, never extra votes/automatic removal."""
    if (type(placement) is not list or len(placement) != 4
            or any(type(host) is not str or not host for host in placement)):
        raise ValueError("FOUR_EXPLICIT_SEAT_PLACEMENTS_REQUIRED")
    return tuple(i for i, host in enumerate(placement) if host in unavailable_hosts)


def main():
    # The caller may redirect this synthetic evidence; no wallet data is read.
    if len(sys.argv) != 1:
        raise SystemExit("Run without arguments; redirect stdout to preserve JSON evidence.")
    results = []
    for label, down, count, burst in (
            ("healthy_matched_sequence", (), 8, False),
            ("eight_request_batch", (), 1, True),
            ("scheduled_leader_unavailable", (0,), 4, False),
            ("noncoordinator_unavailable", (3,), 4, False),
            ("two_seats_unavailable", (2, 3), 1, False)):
        for span in (1, 4):
            result = campaign(span, down, count, burst)
            result["case"] = label
            results.append(result)
    print(json.dumps({"experiment": EXPERIMENT, "results": results}, sort_keys=True))


if __name__ == "__main__":
    main()
