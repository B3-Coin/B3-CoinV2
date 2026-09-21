"""R3: requests cannot replace the protocol's same-target report quorum."""
from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_protocol import PROFILE
from fm_simulator import Simulator
from fm_checker import check
from test_model import Fixture


class TimerEligibilityTests(unittest.TestCase):
    def simulator(self, n=4):
        return Simulator(Fixture().model.snapshot(), n=n, byzantine=(0,))

    def remote_offer(self, sim, node):
        body = node.application.build(node.instance, node.d["anchor"], {})
        node.receive("OFFER", body, 0)
        node.pump()
        return body

    def test_remote_offer_with_zero_reports_does_not_start_changing_timer(self):
        sim = self.simulator()
        node = sim.nodes[2]
        node.change_view(1)
        original = deepcopy(node.record["signed"][("VIEW_CHANGE", 1)])
        self.assertEqual(node.reports, {})
        self.assertIsNone(node.deadline)
        self.remote_offer(sim, node)
        self.assertIsNone(node.deadline)
        for now in (60, 120, 240):
            sim.now = now
            node.tick()
            self.assertEqual(node.record["view"], 1)
            self.assertIsNone(node.deadline)
        self.assertEqual(node.record["signed"][("VIEW_CHANGE", 1)], original)
        check(sim)

    def test_local_offer_and_pending_retry_cannot_bypass_changing_predicate(self):
        sim = self.simulator()
        node = sim.nodes[2]
        node.change_view(1)
        body = node.application.build(node.instance, node.d["anchor"], {})
        node.offer(body)
        node.retry()
        node.pump()
        self.assertIsNone(node.deadline)
        self.assertEqual(node.record["view"], 1)
        self.assertEqual(node.reports, {})
        check(sim)

    def test_restart_with_durable_offer_waits_for_reports(self):
        sim = self.simulator()
        node = sim.nodes[2]
        sim.offer(nodes=[2])
        node.change_view(1)
        before = deepcopy(node.record["signed"][("VIEW_CHANGE", 1)])
        node.crash()
        node.restart()
        self.assertEqual(node.record["signed"][("VIEW_CHANGE", 1)], before)
        self.assertIsNone(node.deadline)
        self.assertEqual(node.record["mode"], "CHANGING")
        check(sim)

    def test_quorum_of_genuine_same_target_reports_starts_timer(self):
        for n in (4, 7):
            with self.subTest(n=n):
                sim = self.simulator(n)
                target = sim.nodes[n - 1]  # not the view-1 proposer
                for node in sim.nodes[1:]:
                    node.change_view(1)
                reports = [s for s in sim.authentication.issued
                           if s["payload"]["phase"] == "VIEW_CHANGE"]
                for index, signed in enumerate(reports[:target.q]):
                    target.receive("SIGNED", signed, signed["payload"]["sender"])
                    target.pump()
                    if index + 1 < target.q:
                        self.assertIsNone(target.deadline)
                self.assertEqual(target.deadline, sim.now + 60)
                old_deadline = target.deadline
                self.remote_offer(sim, target)
                target.receive("SIGNED", reports[0], reports[0]["payload"]["sender"])
                target.pump()
                self.assertEqual(target.deadline, old_deadline)
                check(sim)

    def test_reports_for_another_target_are_not_timer_evidence(self):
        sim = self.simulator()
        target = sim.nodes[3]
        target.change_view(2)
        for node in sim.nodes[1:3]:
            node.change_view(1)
        for signed in list(sim.authentication.issued):
            if signed["payload"]["phase"] == "VIEW_CHANGE":
                target.receive("SIGNED", signed, signed["payload"]["sender"])
                target.pump()
        self.remote_offer(sim, target)
        self.assertEqual(target.record["view"], 2)
        self.assertIsNone(target.deadline)
        check(sim)

    def test_active_request_still_starts_unchanged_timer(self):
        sim = self.simulator()
        node = sim.nodes[2]
        self.remote_offer(sim, node)
        self.assertEqual(node.record["mode"], "ACTIVE")
        self.assertEqual(node.deadline, PROFILE["timers"]["initial_ticks"])
        check(sim)

    def test_sparse_idle_nonleader_request_completes_with_failed_first_leader(self):
        sim = self.simulator()
        self.assertTrue(all(node.deadline is None for node in sim.nodes[1:]))
        sim.offer(nodes=[2])
        sim.run(240, stop=lambda s: s.settled())
        self.assertTrue(sim.settled())
        self.assertTrue(any(e["event"] == "durable" and
                            e.get("reason") == "accepted_new_view" for e in sim.trace))
        for node in sim.nodes[1:]:
            self.assertEqual(node.d["records"][0]["apply_count"], 1)
            self.assertFalse(node.d["halt"])
        check(sim)


if __name__ == "__main__":
    unittest.main()
