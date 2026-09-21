"""R1 process-kill regressions and R1/R2/R3 interactions.

The exact review schedule submits once through remote OFFER to node0.  Seat3
is silent Byzantine; after the interrupted view transition, the three honest
seats receive timely traffic.  No post-reopen local offer or manual rearming
is used.  These are process-kill tests, not machine-power-loss qualification.
"""
from contextlib import contextmanager
from copy import deepcopy
from pathlib import Path
import signal
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_process_harness import ProcessSimulator
from fm_application import value_id
from fm_checker import check
from fm_protocol import PROFILE, message
from test_model import Fixture
import test_recovery as recovery_cases


class ViewTransitionRepairTests(unittest.TestCase):
    @contextmanager
    def case(self, suffix=""):
        with tempfile.TemporaryDirectory(prefix="fm3a-view-repair-generated-") as directory:
            sim = ProcessSimulator(Fixture().model.snapshot(), directory, byzantine=(3,))
            result = {"status": "incomplete"}
            try:
                yield sim
                # SAFETY is checked separately from every progress/resource assertion.
                result = check(sim)
            finally:
                sim.close()
                self.assertTrue(all(not node.process.is_alive() for node in sim.nodes))
                self.assertTrue(all(item["exit"] in (0, -signal.SIGKILL) for item in sim.exits))
                sim.record_result(self.id() + suffix, result)

    def prepare_exact_review_schedule(self, sim):
        helper = recovery_cases.RecoveryTests()
        # Dropping retransmitted OFFERs before timely delivery is the original
        # pre-synchrony schedule, not a post-restart liveness shortcut.
        sim.policy = lambda source, target, kind, data: None if kind == "OFFER" else 1
        body = helper.body(sim)
        sim.nodes[0].receive("OFFER", body, 3)
        sim.nodes[0].pump()
        proposal = helper.signatures(sim, "PROPOSE")[0]
        helper.deliver_signed(sim, proposal, 0)
        for index in (1, 2):
            helper.wire(sim, "DATA", {
                "type": "body", "id": proposal["payload"]["value"], "object": body,
                "reference": {"kind": "SIGNED", "data": proposal}}, index)
        prepares = helper.by_sender(helper.signatures(sim, "PREPARE"))
        qc = {"proposal": proposal, "prepares": [prepares[i] for i in (0, 1, 2)]}
        helper.wire(sim, "PREPARED", qc, 0)
        self.assertEqual(sim.nodes[0].record["highest"], qc)
        self.assertTrue(all(not node.d["retained_bodies"] for node in sim.nodes[:3]),
                        "fixture must not add a durable local client offer")
        self.assertTrue(all(node.deadline is not None for node in sim.nodes[:3]))
        return helper, body, qc

    def timeout_with_cut(self, sim, stage=None, reason=None, cut=None):
        node = sim.nodes[0]
        if stage is not None:
            node.call("set_fault", {"stage": stage, "reason": reason, "kind": "kill"})
        elif cut is not None:
            node.call("set_cut", cut, "VIEW_CHANGE")
        recovery_cases.RecoveryTests().timeout(sim, [0, 1, 2])
        if stage is not None or cut is not None:
            self.assertFalse(node.process.is_alive())
            self.assertEqual(sim.exits[-1]["exit"], -signal.SIGKILL)
            self.assertEqual(node.last_cut,
                             ("storage:" + stage, reason) if stage is not None
                             else (cut, "VIEW_CHANGE"))

    def expected_report(self, body, qc):
        return message("VIEW_CHANGE", 0, body["instance"], 1, prepared=qc, decision=None)

    def report_signatures(self, sim):
        return [signed for signed in sim.authentication.issued
                if signed["payload"]["sender"] == 0
                and signed["payload"]["phase"] == "VIEW_CHANGE"
                and signed["payload"]["view"] == 1]

    def finish_without_new_ingress(self, sim, body, qc, allow_offer_retry=False):
        after_reopen_offers = []

        def timely(source, target, kind, data):
            if kind == "OFFER":
                after_reopen_offers.append((source, target, value_id(data)))
            return 1

        sim.policy = timely
        sim.run(200, stop=lambda s: s.settled())
        # PROGRESS UNDER THE DECLARED CONDITIONS: all three honest seats apply.
        self.assertTrue(sim.settled(), [(n.index, n.d["sequence"], n.record["view"],
                                       n.record["mode"], n.deadline, n.last_reason)
                                      for n in sim.nodes[:3]])
        self.assertFalse(sim.exhausted)
        self.assertEqual(len({node.d["snapshot"] for node in sim.nodes[:3]}), 1)
        for node in sim.nodes[:3]:
            self.assertEqual(node.d["parent"], value_id(body))
            self.assertEqual(node.d["records"][0]["apply_count"], 1)
            self.assertFalse(node.d["halt"])
        self.assertEqual(sim.nodes[3].d["sequence"], 0, "silent Byzantine seat must remain silent")
        if not allow_offer_retry:
            self.assertEqual(after_reopen_offers, [], "no fresh or retransmitted OFFER may rescue R1")
        reports = self.report_signatures(sim)
        self.assertEqual(len(reports), 1, "supported reopen must not issue a replacement report")
        self.assertEqual(reports[0]["payload"], self.expected_report(body, qc))
        original = sim.nodes[0].d["records"][0]
        self.assertEqual(original["intents"][("VIEW_CHANGE", 1)], reports[0]["payload"])
        self.assertEqual(original["signed"][("VIEW_CHANGE", 1)], reports[0])
        self.assertEqual(original["prepared"][(0, value_id(body))], qc)
        self.assertIn(reports[0], sim.published())
        check(sim)

    def test_R1_exact_review_control_without_kill(self):
        with self.case() as sim:
            _, body, qc = self.prepare_exact_review_schedule(sim)
            self.timeout_with_cut(sim)
            self.finish_without_new_ingress(sim, body, qc)
            self.assertFalse(any(item["exit"] == -signal.SIGKILL for item in sim.exits))

    def test_R1_exact_after_commit_reopen_completes_without_new_traffic(self):
        with self.case() as sim:
            _, body, qc = self.prepare_exact_review_schedule(sim)
            self.timeout_with_cut(sim, stage="after_commit", reason="enter_view:timeout")
            self.assertEqual(self.report_signatures(sim), [])
            node = sim.nodes[0]
            node.restart()
            self.assertEqual(node.record["view"], 1)
            self.assertEqual(node.record["mode"], "CHANGING")
            self.assertEqual(node.record["intents"][("VIEW_CHANGE", 1)], self.expected_report(body, qc))
            # TIMER ELIGIBILITY: restart resumes the report, not an unauthorized timer.
            self.assertIsNone(node.deadline)
            self.finish_without_new_ingress(sim, body, qc)

    def test_R1_atomic_view_entry_before_commit_and_after_flush(self):
        for stage in ("before_commit", "after_fsync"):
            with self.subTest(stage=stage), self.case(":" + stage) as sim:
                _, body, qc = self.prepare_exact_review_schedule(sim)
                self.timeout_with_cut(sim, stage=stage, reason="enter_view:timeout")
                self.assertEqual(self.report_signatures(sim), [])
                node = sim.nodes[0]
                node.restart()
                if stage == "before_commit":
                    self.assertEqual(node.record["view"], 0)
                    self.assertNotIn(("VIEW_CHANGE", 1), node.record["intents"])
                else:
                    self.assertEqual(node.record["view"], 1)
                    self.assertEqual(node.record["intents"][("VIEW_CHANGE", 1)],
                                     self.expected_report(body, qc))
                    self.assertIsNone(node.deadline)
                self.finish_without_new_ingress(sim, body, qc)

    def test_R1_report_intent_signature_publication_boundaries_preserve_exact_object(self):
        cuts = (("before_commit", "intent:VIEW_CHANGE", None),
                (None, None, "after_intent"),
                ("after_commit", "signature:VIEW_CHANGE", None),
                (None, None, "after_record"),
                (None, None, "after_publish"))
        for stage, reason, cut in cuts:
            suffix = stage + ":" + reason if stage is not None else cut
            with self.subTest(cut=suffix), self.case(":" + suffix) as sim:
                _, body, qc = self.prepare_exact_review_schedule(sim)
                self.timeout_with_cut(sim, stage, reason, cut)
                issued_before = deepcopy(self.report_signatures(sim))
                if reason == "intent:VIEW_CHANGE" or cut == "after_intent":
                    self.assertEqual(issued_before, [])
                else:
                    self.assertEqual(len(issued_before), 1)
                    self.assertEqual(issued_before[0]["payload"], self.expected_report(body, qc))
                    self.assertEqual(issued_before[0] in sim.published(), cut == "after_publish")
                sim.nodes[0].restart()
                self.assertEqual(sim.nodes[0].record["intents"][("VIEW_CHANGE", 1)],
                                 self.expected_report(body, qc))
                self.assertIsNone(sim.nodes[0].deadline)
                if issued_before:
                    self.assertEqual(self.report_signatures(sim), issued_before)
                    self.assertIn(issued_before[0], sim.published())
                self.finish_without_new_ingress(sim, body, qc)
                if issued_before:
                    self.assertEqual(self.report_signatures(sim), issued_before)

    def test_R1_R3_additional_valid_offer_cannot_supply_forbidden_timer(self):
        with self.case() as sim:
            helper, body, qc = self.prepare_exact_review_schedule(sim)
            self.timeout_with_cut(sim, stage="after_commit", reason="enter_view:timeout")
            node = sim.nodes[0]
            node.restart()
            before_intent = deepcopy(node.record["intents"][("VIEW_CHANGE", 1)])
            self.assertEqual(node.record["view"], 1)
            self.assertIsNone(node.deadline)
            unrelated_offer = helper.body(sim, amount=101)
            node.receive("OFFER", unrelated_offer, 3)
            node.pump()
            self.assertIn(value_id(unrelated_offer), node.offers)
            self.assertEqual(node.record["mode"], "CHANGING")
            # TIMER ELIGIBILITY is a negative assertion before any report delivery.
            self.assertIsNone(node.deadline)
            self.assertEqual(node.record["intents"][("VIEW_CHANGE", 1)], before_intent)
            self.finish_without_new_ingress(sim, body, qc, allow_offer_retry=True)

    def test_R1_R2_R3_disposable_vote_pressure_during_recovery(self):
        with self.case() as sim:
            helper, body, qc = self.prepare_exact_review_schedule(sim)
            self.timeout_with_cut(sim, stage="after_commit", reason="enter_view:timeout")
            node = sim.nodes[0]
            node.restart()
            protected = deepcopy(node.record)
            witness = deepcopy(node.witness)
            issued_before = deepcopy([s for s in sim.authentication.issued
                                      if s["payload"]["sender"] != 3])
            # Seat3 is not the scheduled proposer in view0/1 and cannot forge
            # honest proposal references. Its authenticated unknown-value votes
            # must be refused; delayed votes for the protected candidate remain
            # usable. R2's separate Byzantine-proposer campaign covers admission
            # followed by 150 successive reference-expiry cycles.
            for index in range(150):
                for phase in ("PREPARE", "COMMIT"):
                    junk = sim.authentication.adversary_sign(message(
                        phase, 3, node.instance, 0, f"{index + 1:064x}"))
                    node.receive("SIGNED", junk, 3)
                    node.pump()
                    self.assertEqual(node.last_reason, "UNREFERENCED_VOTE")
                # RESOURCE BOUNDS use the real child's public-inspection state.
                self.assertLessEqual(len(node.votes), PROFILE["admission"]["received_vote_buckets"])
                self.assertLessEqual(node.last_vote_cleanup_work["buckets_inspected"],
                                     PROFILE["admission"]["received_vote_buckets"])
                self.assertEqual(node.last_vote_cleanup_work["obligation_entries_inspected"],
                                 sum(len(protected[name]) for name in
                                     ("accepted", "prepared", "intents", "new_views")))
                self.assertLessEqual(len(node.references), PROFILE["admission"]["references"])
                self.assertLessEqual(len(node.requests), PROFILE["admission"]["requests"])
                self.assertLessEqual(len(node.pending), PROFILE["admission"]["pending"])
                self.assertEqual(node.record, protected, "junk must not alter protected voting evidence")
                self.assertEqual(node.witness, witness, "disposable vote rejection must not write signer storage")
                self.assertIsNone(node.deadline, "junk cannot provide required view-change evidence")
                self.assertFalse(node.d["halt"])
            self.assertEqual([s for s in sim.authentication.issued if s["payload"]["sender"] != 3], issued_before)
            for signed in qc["prepares"]:
                node.receive("SIGNED", signed, signed["payload"]["sender"])
                node.pump()
            self.assertEqual(node.record["prepared"][(0, value_id(body))], qc)
            self.assertIsNone(node.deadline)
            self.finish_without_new_ingress(sim, body, qc)


if __name__ == "__main__":
    unittest.main()
