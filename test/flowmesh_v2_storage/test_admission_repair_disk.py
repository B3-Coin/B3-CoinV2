"""R2/R3 public-ingress regressions using actual SQLite-backed replicas.

These use same-process scheduling and real database close/reopen, not child
termination. The distinct view-transition suite covers actual SIGKILL cases.
Authentication, anchors, members and test balances remain synthetic.
"""
from contextlib import contextmanager
from copy import deepcopy
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_process_harness import DiskSimulator
from fm_disk_replica import DiskReplica
from fm_application import value_id
from fm_checker import check
from fm_protocol import PROFILE
from test_model import Fixture
# Module imports deliberately avoid rediscovering the helper's TestCase class.
import test_vote_retention as vote_cases


class AdmissionRepairDiskTests(unittest.TestCase):
    @contextmanager
    def case(self):
        with tempfile.TemporaryDirectory(prefix="fm3a-admission-repair-generated-") as directory:
            sim = DiskSimulator(Fixture().model.snapshot(), directory, byzantine=(0,))
            try:
                yield sim, Path(directory)
                check(sim)
            finally:
                sim.close()
                self.assertTrue(all(node._closed for node in sim.nodes))
                self.assertTrue(all(node.store._connection is None for node in sim.nodes))
                self.assertTrue(all(node.store._lock_fd is None for node in sim.nodes))

    def reopen(self, sim, directory, index):
        """Close the old real store before acquiring its exclusive lock anew."""
        old = sim.nodes[index]
        head = deepcopy(old.store.head)
        old.close()
        self.assertTrue(old._closed)
        self.assertIsNone(old.store._connection)
        self.assertIsNone(old.store._lock_fd)
        node = DiskReplica(index, sim.n, sim.authentication.signer(index),
            sim.authentication.verify, sim.initial_snapshot, sim.initial_anchor,
            sim.send, lambda: sim.now, sim.record, directory=directory / str(index),
            create=False, trusted_head=head)
        sim.nodes[index] = node
        self.assertEqual(node.store.head, head)
        node.restart()
        return node

    def test_R2_exact_150_then_300_and_450_cycles_no_durable_growth_reopen_progress(self):
        with self.case() as (sim, directory):
            helper = vote_cases.VoteRetentionTests()
            node = sim.nodes[1]
            before = deepcopy(node.d)
            head = deepcopy(node.store.head)
            observations = []
            for start, stop in ((0, 150), (150, 300), (300, 450)):
                observations.append(helper.churn(sim, node, start, stop))
                # RESOURCE BOUNDS: repeated reference expiry cannot grow either
                # disposable votes or the real durable journal/materialization.
                self.assertEqual(node.store.head, head)
                self.assertEqual(node.store.load(), before)
                self.assertEqual(node.d, before)
                self.assertLessEqual(len(node.votes), 2)
                self.assertLessEqual(node.last_vote_cleanup_work["buckets_inspected"],
                                     PROFILE["admission"]["received_vote_buckets"])
                self.assertEqual(len(sim.authentication.issued), stop * 4)
                self.assertFalse(node.d["halt"])
            self.assertTrue(all(item["peak_buckets"] <= 2 for item in observations))
            issued = deepcopy(sim.authentication.issued)
            self.assertEqual(len(issued), 1800, "independent issuance audit must not be pruned")
            check(sim)  # SAFETY is not used as the resource/progress assertion.
            node = self.reopen(sim, directory, 1)
            self.assertEqual(node.d, before)
            self.assertEqual(node.store.head, head)
            self.assertEqual(node.store.load(), before)
            self.assertFalse(node.votes)
            self.assertEqual(sim.authentication.issued, issued)
            # PROGRESS UNDER THE DECLARED CONDITIONS: a legitimate sparse
            # request reaches honest proposer1 despite failed first proposer0.
            body = sim.offer(nodes=[1])
            sim.run(350, stop=lambda s: s.settled())
            self.assertTrue(sim.settled(), [n.last_reason for n in sim.nodes])
            for replica in sim.nodes[1:]:
                self.assertEqual(replica.d["parent"], value_id(body))
                self.assertEqual(replica.d["records"][0]["apply_count"], 1)
                self.assertEqual(replica.store.load(), replica.d)
                self.assertFalse(replica.d["halt"])
            self.assertEqual(sim.authentication.issued[:len(issued)], issued)
            print({"disk_received_vote_retention": observations,
                   "same_directory_sqlite_reopened": True,
                   "honest_applied": [node.d["sequence"] for node in sim.nodes[1:]]})

    def test_R3_disk_remote_offer_without_reports_cannot_start_timer_or_advance(self):
        with self.case() as (sim, _):
            node = sim.nodes[2]
            node.change_view(1)
            original = deepcopy(node.record["signed"][("VIEW_CHANGE", 1)])
            protected = deepcopy(node.d)
            head = deepcopy(node.store.head)
            self.assertFalse(node.reports)
            self.assertIsNone(node.deadline)
            body = node.application.build(node.instance, node.d["anchor"], {})
            node.receive("OFFER", body, 0)
            node.pump()
            self.assertIn(value_id(body), node.offers)
            self.assertIsNone(node.deadline)
            for now in (60, 120, 240):
                sim.now = now
                node.tick()
                # TIMER ELIGIBILITY independently excludes hidden premature
                # view changes or storage writes after the old deadline.
                self.assertEqual(node.record["view"], 1)
                self.assertEqual(node.record["mode"], "CHANGING")
                self.assertIsNone(node.deadline)
                self.assertFalse(node.reports)
                self.assertEqual(node.store.head, head)
                self.assertEqual(node.store.load(), protected)
            self.assertEqual(node.record["signed"][("VIEW_CHANGE", 1)], original)
            self.assertEqual([signed for signed in sim.authentication.issued
                              if signed["payload"]["sender"] == 2], [original])

    def test_R3_disk_genuine_report_quorum_permits_timer_without_offer_substitution(self):
        with self.case() as (sim, _):
            target = sim.nodes[3]  # Not the view1 proposer.
            for node in sim.nodes[1:]:
                node.change_view(1)
            reports = [signed for signed in sim.authentication.issued
                       if signed["payload"]["phase"] == "VIEW_CHANGE"
                       and signed["payload"]["view"] == 1]
            self.assertEqual(len(reports), target.q)
            before = deepcopy(target.d)
            head = deepcopy(target.store.head)
            for index, signed in enumerate(reports):
                target.receive("SIGNED", signed, signed["payload"]["sender"])
                target.pump()
                if index + 1 < target.q:
                    self.assertIsNone(target.deadline)
            expected = sim.now + PROFILE["timers"]["initial_ticks"] * 2
            self.assertEqual(target.deadline, expected)
            self.assertEqual(target.store.head, head, "timer activation itself is volatile")
            self.assertEqual(target.store.load(), before)
            body = target.application.build(target.instance, target.d["anchor"], {})
            target.receive("OFFER", body, 0)
            target.pump()
            self.assertEqual(target.deadline, expected, "request traffic must not reset eligible timer")
            sim.now = expected
            target.tick()
            self.assertEqual(target.record["view"], 2)
            self.assertEqual(target.record["mode"], "CHANGING")
            self.assertIsNone(target.deadline)
            self.assertIn(("VIEW_CHANGE", 2), target.record["signed"])
            self.assertEqual(target.store.load(), target.d)


if __name__ == "__main__":
    unittest.main()
