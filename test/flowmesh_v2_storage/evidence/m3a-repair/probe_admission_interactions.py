"""Additional read-only probes of the frozen revision's public receive path."""
from pathlib import Path
import json
import sys
import tempfile

from _frozen_baseline import source_from_command_line

SOURCE = source_from_command_line()
sys.path.insert(0, str(SOURCE / "test/flowmesh_v2_storage"))
from fm_process_harness import DiskSimulator
from fm_protocol import PROFILE, message
from fm_checker import check
from test_model import Fixture


def receive(node, signed):
    node.receive("SIGNED", signed, 0)
    node.pump()


def reference_churn():
    with tempfile.TemporaryDirectory(prefix="review-generated-admission-") as directory:
        sim = DiskSimulator(Fixture().model.snapshot(), directory, byzantine=(0,))
        try:
            node = sim.nodes[1]
            start_head = node.store.head
            for batch in range(150):
                # Finite pre-synchrony schedule: inputs arrive just after old
                # leases expire. The replica also ticks and drains recovery work
                # each cycle; no internal tables are edited.
                sim.now = batch * (PROFILE["admission"]["lease_ticks"] + 1)
                for offset in range(2):
                    value = f"{2 * batch + offset + 1:064x}"
                    proposal = sim.authentication.adversary_sign(message(
                        "PROPOSE", 0, node.instance, 0, value, new_view=None))
                    receive(node, proposal)
                    vote = sim.authentication.adversary_sign(message(
                        "PREPARE", 0, node.instance, 0, value))
                    receive(node, vote)
                node.tick()
                for _ in range(PROFILE["limits"]["inbox"]):
                    if not node.inbox:
                        break
                    node.pump()
                assert not node.inbox
            result = {"probe": "reference_churn", "references": len(node.references),
                      "requests": len(node.requests), "pending": len(node.pending),
                      "bodies": len(node.bodies), "vote_buckets": len(node.votes),
                      "object_limit": PROFILE["limits"]["objects"],
                      "durable_head_unchanged": node.store.head == start_head,
                      "halt": node.d["halt"], "checker": check(sim)}
            print(json.dumps(result, sort_keys=True))
        finally:
            sim.close()


def offer_timer():
    with tempfile.TemporaryDirectory(prefix="review-generated-timer-") as directory:
        sim = DiskSimulator(Fixture().model.snapshot(), directory, byzantine=(0,))
        try:
            node = sim.nodes[2]
            node.change_view(1)
            assert not node.reports and node.deadline is None
            body = node.application.build(node.instance, node.d["anchor"], {})
            node.receive("OFFER", body, 0)
            node.pump()
            deadline = node.deadline
            sim.now = deadline
            node.tick()
            print(json.dumps({"probe": "remote_offer_in_changing", "report_count": len(node.reports),
                              "deadline_created_by_offer": deadline, "view_after_timeout": node.record["view"],
                              "mode": node.record["mode"], "checker": check(sim)}, sort_keys=True))
        finally:
            sim.close()


if __name__ == "__main__":
    reference_churn()
    offer_timer()
