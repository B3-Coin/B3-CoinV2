"""Received-vote cache ownership, expiry and recovery (synthetic identities)."""
from copy import deepcopy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_application import value_id
from fm_checker import check
from fm_protocol import PROFILE, message
from fm_simulator import Simulator
from test_model import Fixture
import test_recovery as recovery_cases


class VoteRetentionTests(unittest.TestCase):
    def simulator(self):
        return Simulator(Fixture().model.snapshot(), byzantine=(0,))

    def receive(self, node, signed):
        node.receive("SIGNED", signed, signed["payload"]["sender"])
        node.pump()

    def junk(self, s, node, number, phase="PREPARE"):
        value = f"{number:064x}"
        proposal = s.authentication.adversary_sign(message(
            "PROPOSE", 0, node.instance, 0, value, new_view=None))
        self.receive(node, proposal)
        vote = s.authentication.adversary_sign(message(
            phase, 0, node.instance, 0, value))
        self.receive(node, vote)
        return vote

    def assert_bounded(self, node):
        cap = PROFILE["admission"]["received_vote_buckets"]
        self.assertLessEqual(len(node.votes), cap)
        self.assertLessEqual(sum(map(len, node.votes.values())), cap * node.n)
        self.assertLessEqual(node.last_vote_cleanup_work["buckets_inspected"], cap)
        # Only the current record is scanned: accepted, prepared, five intent
        # phases and NEW_VIEW are each bounded by configured views in this
        # fixed-membership, at-most-f-Byzantine model. No historical walk.
        self.assertLessEqual(node.last_vote_cleanup_work["obligation_entries_inspected"],
                             PROFILE["limits"]["views"] * 8)
        self.assertFalse(node.d["halt"])

    def churn(self, s, node, start, stop):
        peak_buckets = peak_scan = 0
        for cycle in range(start, stop):
            s.now = cycle * (PROFILE["admission"]["lease_ticks"] + 1)
            for offset in range(2):
                self.junk(s, node, 2 * cycle + offset + 1)
                self.assert_bounded(node)
            node.tick()
            for _ in range(PROFILE["limits"]["inbox"]):
                if not node.inbox:
                    break
                node.pump()
            self.assertFalse(node.inbox)
            self.assert_bounded(node)
            peak_buckets = max(peak_buckets, len(node.votes))
            peak_scan = max(peak_scan, node.last_vote_cleanup_work["buckets_inspected"])
        return {"cycles": stop, "peak_buckets": peak_buckets,
                "peak_cleanup_buckets_inspected": peak_scan,
                "retained_buckets": len(node.votes),
                "issued_audit_objects": len(s.authentication.issued)}

    def test_exact_150_cycle_reproducer_and_increasing_cycles_stay_bounded(self):
        s = self.simulator()
        node = s.nodes[1]
        before = deepcopy(node.d)
        observations = []
        for start, stop in ((0, 150), (150, 300), (300, 450)):
            observations.append(self.churn(s, node, start, stop))
            self.assertEqual(node.d, before)
            self.assertLessEqual(len(node.votes), 2)
            self.assertEqual(len(s.authentication.issued), stop * 4)
        self.assertTrue(all(item["peak_buckets"] <= 2 for item in observations))
        # Audit history is deliberately independent and grows with issuance;
        # it is not the bounded replica cache and must not be pruned with it.
        self.assertEqual(len(s.authentication.issued), 1800)
        check(s)
        print({"received_vote_retention": observations})

    def test_expiry_without_new_votes_reclaims_only_disposable_buckets(self):
        s = self.simulator()
        node = s.nodes[1]
        signed = self.junk(s, node, 1)
        self.assertEqual(len(node.votes), 1)
        s.now = PROFILE["admission"]["lease_ticks"] + 1
        node.retry()  # Periodic API, not an internal index mutation.
        self.assertFalse(node.votes)
        self.assertEqual(node.last_vote_cleanup_work["buckets_expired"], 1)
        self.assertIn(signed, s.authentication.issued)
        self.assert_bounded(node)
        check(s)

    def test_delayed_vote_for_accepted_proposal_survives_reference_expiry(self):
        s = self.simulator()
        schedule = recovery_cases.RecoveryTests()
        body, _ = schedule.prepared_schedule(s, participants=[1, 2, 3])
        node = s.nodes[2]
        votes = schedule.by_sender(schedule.signatures(s, "PREPARE"))
        self.receive(node, votes[1])
        key = (0, value_id(body), "PREPARE")
        retained = deepcopy(node.votes[key])
        s.now += PROFILE["admission"]["lease_ticks"] + 1
        node.retry()
        self.assertEqual(node.votes[key], retained)
        self.receive(node, votes[2])
        self.receive(node, votes[3])
        self.assertIn((0, value_id(body)), node.record["prepared"])
        self.assertIn(("COMMIT", 0), node.record["signed"])
        s.run(350, stop=lambda sim: sim.settled())
        self.assertTrue(s.settled())
        check(s)

    def test_expired_unaccepted_votes_can_be_retransmitted_after_body_recovery(self):
        s = self.simulator()
        node = s.nodes[2]
        schedule = recovery_cases.RecoveryTests()
        body = schedule.body(s)
        vid = value_id(body)
        proposal = s.authentication.adversary_sign(message(
            "PROPOSE", 0, node.instance, 0, vid, new_view=None))
        vote = s.authentication.adversary_sign(message("PREPARE", 0, node.instance, 0, vid))
        self.receive(node, proposal)
        self.receive(node, vote)
        s.now += PROFILE["admission"]["lease_ticks"] + 1
        node.retry()
        self.assertFalse(node.votes)
        self.receive(node, proposal)
        node.receive("DATA", {"type": "body", "id": vid, "object": body,
                              "reference": {"kind": "SIGNED", "data": proposal}}, 0)
        while node.inbox:
            node.pump()
        self.receive(node, vote)
        self.assertEqual(node.votes[(0, vid, "PREPARE")][0], vote)
        self.assertIn(("PREPARE", 0), node.record["signed"])
        self.assert_bounded(node)
        check(s)

    def test_stale_header_alone_does_not_protect_received_vote(self):
        s = self.simulator()
        schedule = recovery_cases.RecoveryTests()
        node = s.nodes[2]
        # The public proposal path retains a header before rejecting it for
        # conflict with this replica's already accepted proposal. A bare
        # cached header does not make the competing votes indispensable.
        accepted, _ = schedule.prepared_schedule(s, participants=[1, 2, 3])
        other = schedule.body(s, 200)
        vid = value_id(other)
        proposal = s.authentication.adversary_sign(message(
            "PROPOSE", 0, node.instance, 0, vid, new_view=None))
        node.receive("DATA", {"type": "body", "id": vid, "object": other,
                              "reference": {"kind": "SIGNED", "data": proposal}}, 0)
        while node.inbox:
            node.pump()
        self.assertIn((0, vid), node.headers)
        self.assertEqual(node.record["accepted"][0]["body"], accepted)
        vote = s.authentication.adversary_sign(message("PREPARE", 0, node.instance, 0, vid))
        self.receive(node, vote)
        self.assertIn((0, vid, "PREPARE"), node.votes)
        s.now += PROFILE["admission"]["lease_ticks"] + 1
        node.retry()
        self.assertNotIn((0, vid, "PREPARE"), node.votes)
        self.receive(node, vote)
        self.assertEqual(node.last_reason, "UNREFERENCED_VOTE")
        self.assertNotIn((0, vid, "PREPARE"), node.votes)
        self.assert_bounded(node)
        check(s)

    def test_prepared_commit_and_new_view_evidence_survive_pressure_and_restart(self):
        s = self.simulator()
        schedule = recovery_cases.RecoveryTests()
        body, qc = schedule.prepared_schedule(s, holders=[2], participants=[1, 2, 3])
        node = s.nodes[2]
        original_commit = deepcopy(node.record["signed"][("COMMIT", 0)])
        self.receive(node, original_commit)
        commit_key = (0, value_id(body), "COMMIT")
        for cycle in range(8):
            s.now += PROFILE["admission"]["lease_ticks"] + 1
            self.junk(s, node, 10000 + 2 * cycle)
            self.junk(s, node, 10001 + 2 * cycle)
            self.assertEqual(node.votes[commit_key][2], original_commit)
            self.assertEqual(node.record["highest"], qc)
            self.assert_bounded(node)
        for index in (1, 2, 3):
            s.nodes[index].change_view(1)
        reports = schedule.reports(s, 1, byzantine_empty=False)
        for index in (1, 2, 3):
            schedule.deliver_signed(s, reports[index], 1)
        nv = schedule.signatures(s, "NEW_VIEW", 1)[0]
        schedule.deliver_signed(s, nv, 2)
        protected = deepcopy(node.d)
        for number in range(20000, 20040):
            self.junk(s, node, number)
            self.assert_bounded(node)
        self.assertEqual(node.d, protected)
        node.crash()
        node.restart()
        self.assertEqual(node.d, protected)
        self.assertEqual(node.record["new_views"][1], nv)
        self.assertEqual(node.record["prepared"][(0, value_id(body))], qc)
        self.assertEqual(node.record["signed"][("COMMIT", 0)], original_commit)
        s.run(350, stop=lambda sim: sim.settled())
        self.assertTrue(s.settled())
        check(s)

    def test_protected_bucket_displaces_disposable_at_explicit_small_test_cap(self):
        # This cap is a disclosed TEST-only pressure parameter, not a voting
        # rule. Construct the simulator under it so synthetic config agrees.
        with patch.dict(PROFILE["admission"], received_vote_buckets=2):
            s = self.simulator()
            schedule = recovery_cases.RecoveryTests()
            body, _ = schedule.prepared_schedule(s, participants=[1, 2, 3])
            node = s.nodes[2]
            self.junk(s, node, 10000, "PREPARE")
            self.junk(s, node, 10000, "COMMIT")
            self.assertEqual(len(node.votes), 2)
            vote = schedule.by_sender(schedule.signatures(s, "PREPARE"))[1]
            self.receive(node, vote)
            self.assertEqual(node.votes[(0, value_id(body), "PREPARE")][1], vote)
            self.assertEqual(len(node.votes), 2)
            self.assertTrue(any(event["event"] == "cache_evicted" and
                                event["type"] == "received_vote_bucket" for event in s.trace))
            self.assert_bounded(node)
            check(s)

    def test_disposable_pressure_rejects_without_permanent_halt(self):
        with patch.dict(PROFILE["admission"], received_vote_buckets=2):
            s = self.simulator()
            node = s.nodes[1]
            self.junk(s, node, 1, "PREPARE")
            self.junk(s, node, 1, "COMMIT")
            before = deepcopy(node.votes)
            self.junk(s, node, 2, "PREPARE")
            self.assertEqual(node.last_reason, "RECEIVED_VOTE_PRESSURE")
            self.assertEqual(node.votes, before)
            self.assert_bounded(node)
            check(s)

    def test_legitimate_agreement_after_bounded_campaign_and_supported_restart(self):
        s = self.simulator()
        node = s.nodes[1]
        self.churn(s, node, 0, 150)
        original = deepcopy(node.d)
        issued = deepcopy(s.authentication.issued)
        node.crash()
        node.restart()
        self.assertEqual(node.d, original)
        self.assertFalse(node.votes)
        self.assertEqual(s.authentication.issued, issued)
        body = s.offer(nodes=[1])
        s.run(350, stop=lambda sim: sim.settled())
        self.assertTrue(s.settled(), [n.last_reason for n in s.nodes])
        for index in (1, 2, 3):
            self.assertEqual(s.nodes[index].d["parent"], value_id(body))
            self.assertEqual(s.nodes[index].d["records"][0]["apply_count"], 1)
        self.assertTrue(all(signed in s.authentication.issued for signed in issued))
        check(s)


if __name__ == "__main__":
    unittest.main()
