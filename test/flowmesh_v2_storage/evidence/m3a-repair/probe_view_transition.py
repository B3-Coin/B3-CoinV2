"""Read-only review probe; imports exact frozen source, never patches it."""
from pathlib import Path
import json
import sys
import tempfile

from _frozen_baseline import source_from_command_line

SOURCE = source_from_command_line()
sys.path.insert(0, str(SOURCE / "test/flowmesh_v2_storage"))
from fm_process_harness import ProcessSimulator
from fm_checker import check
from test_model import Fixture
from test_recovery import RecoveryTests


def run(kill_boundary):
    with tempfile.TemporaryDirectory(prefix="review-generated-view-") as directory:
        sim = ProcessSimulator(Fixture().model.snapshot(), directory, byzantine=(3,))
        try:
            helper = RecoveryTests()
            # A body submitted once via remote OFFER, not durable local ingress.
            # Before recovery the schedule drops retransmitted OFFERs, as allowed
            # before timely delivery. Proposal/body references are delivered.
            sim.policy = lambda source, target, kind, data: None if kind == "OFFER" else 1
            body = helper.body(sim)
            sim.nodes[0].receive("OFFER", body, 3)
            sim.nodes[0].pump()
            proposal = helper.signatures(sim, "PROPOSE")[0]
            helper.deliver_signed(sim, proposal, 0)
            for index in (1, 2):
                helper.wire(sim, "DATA", {"type": "body", "id": proposal["payload"]["value"],
                    "object": body, "reference": {"kind": "SIGNED", "data": proposal}}, index)
            prepares = helper.by_sender(helper.signatures(sim, "PREPARE"))
            qc = {"proposal": proposal, "prepares": [prepares[i] for i in (0, 1, 2)]}
            helper.wire(sim, "PREPARED", qc, 0)
            if kill_boundary:
                sim.nodes[0].call("set_fault", {"stage": "after_commit", "reason": "enter_view:timeout", "kind": "kill"})
            helper.timeout(sim, [0, 1, 2])
            if kill_boundary:
                assert sim.exits[-1]["exit"] == -9
                sim.nodes[0].restart()
                assert sim.nodes[0].record["view"] == 1
                assert ("VIEW_CHANGE", 1) not in sim.nodes[0].record["intents"]
            sim.policy = None  # All honest traffic now delivered in one tick.
            sim.run(200, stop=lambda s: s.settled())
            result = {"kill_at_enter_view_commit": kill_boundary,
                      "settled": sim.settled(), "time": sim.now,
                      "exhausted": sim.exhausted, "checker": check(sim),
                      "nodes": [{"index": n.index, "sequence": n.d["sequence"],
                                 "view": n.record["view"], "mode": n.record["mode"],
                                 "deadline": n.deadline, "halt": n.d["halt"],
                                 "view_change_intents": [list(k) for k in n.record["intents"] if k[0] == "VIEW_CHANGE"],
                                 "view_change_signatures": [list(k) for k in n.record["signed"] if k[0] == "VIEW_CHANGE"]}
                                for n in sim.nodes[:3]]}
            print(json.dumps(result, sort_keys=True), flush=True)
            if kill_boundary:
                assert not sim.settled()
                assert all(n.record["mode"] == "CHANGING" and n.record["view"] == 1
                           and n.deadline is None and not n.d["halt"] for n in sim.nodes[:3])
            else:
                assert sim.settled()
        finally:
            sim.close()
            print(json.dumps({"exits": sim.exits,
                              "all_children_exited": all(not n.process.is_alive() for n in sim.nodes)}), flush=True)


if __name__ == "__main__":
    run(False)
    run(True)
