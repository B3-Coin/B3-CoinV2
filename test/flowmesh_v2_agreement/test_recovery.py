"""Adversarial prepared-proof, view-change, crash and catch-up schedules.

These tests schedule genuine honest transitions. The external test driver may
collect transferable proofs, while arbitrary signatures come only from the
configured Byzantine identities. No honest journal or vote guard is bypassed.
"""
from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_application import value_id
from fm_protocol import PROFILE, message
from fm_simulator import Simulator
from test_model import BUYER, USD_A, Fixture


class RecoveryTests(unittest.TestCase):
    def simulator(self, n=4, byzantine=()):
        return Simulator(Fixture().model.snapshot(), n=n, byzantine=byzantine)

    def body(self, s, amount=100):
        f = Fixture()
        node = next(node for node in s.nodes if node.index not in s.byzantine)
        return node.application.build(node.instance, node.d["anchor"],
                                      {"deposits": [f.fact(BUYER, USD_A, amount)]})

    def signatures(self, s, phase, view=0, value=None):
        return [signed for signed in s.authentication.issued
                if signed["payload"]["phase"] == phase
                and signed["payload"]["view"] == view
                and (value is None or signed["payload"]["value"] == value)]

    def by_sender(self, signatures):
        return {signed["payload"]["sender"]: signed for signed in signatures}

    def deliver_signed(self, s, signed, destination):
        self.assertTrue(s.deliver_where(
            lambda e: e.destination == destination and e.kind == "SIGNED"
            and e.data == signed), "required signed message absent from scheduler")

    def wire(self, s, kind, data, destination, source=0):
        s.send(source, kind, data, destination)
        self.assertTrue(s.deliver_where(lambda e: e.kind == kind
                                      and e.destination == destination and e.data == data))

    def audit(self, s):
        from fm_checker import check
        result = check(s)
        self.assertFalse(s.exhausted)
        for index, node in enumerate(s.nodes):
            if index not in s.byzantine:
                self.assertFalse(node.d["halt"])
        return result

    def prepared_schedule(self, s, holders=(), body=None, participants=None):
        """Give only holders a complete proof; everyone else has individual votes."""
        body = body or self.body(s)
        participants = ([i for i in range(s.n) if i not in s.byzantine]
                        if participants is None else list(participants))
        s.offer(body=body, nodes=participants)
        vid = value_id(body)
        if 0 in s.byzantine:
            proposal = s.byzantine_message(message(
                "PROPOSE", 0, body["instance"], 0, vid, new_view=None), targets=participants)
        else:
            proposal = self.signatures(s, "PROPOSE", value=vid)[0]
        for index in participants:
            self.deliver_signed(s, proposal, index)
        for index in sorted(s.byzantine):
            s.byzantine_message(message("PREPARE", index, body["instance"], 0, vid),
                                targets=sorted(s.byzantine))
        votes = self.by_sender(self.signatures(s, "PREPARE", value=vid))
        q = s.nodes[0].q
        self.assertGreaterEqual(len(votes), q)
        qc = {"proposal": proposal, "prepares": [votes[i] for i in sorted(votes)[:q]]}
        s.nodes[participants[0]].proofs.check("prepared", qc, body["instance"])
        for index in holders:
            self.wire(s, "PREPARED", qc, index)
        self.assertEqual({i for i in participants if s.nodes[i].record["highest"]}, set(holders))
        return body, qc

    def certificate(self, s, qc, include_byzantine=True):
        p = qc["proposal"]["payload"]
        if include_byzantine:
            for index in sorted(s.byzantine):
                s.byzantine_message(message("COMMIT", index, p["instance"], p["view"], p["value"]),
                                    targets=sorted(s.byzantine))
        votes = self.by_sender(self.signatures(s, "COMMIT", p["view"], p["value"]))
        q = s.nodes[0].q
        self.assertGreaterEqual(len(votes), q)
        cert = {"prepared": qc, "commits": [votes[i] for i in sorted(votes)[:q]]}
        s.nodes[0].proofs.check("commit", cert, p["instance"])
        return cert

    def timeout(self, s, indexes):
        deadlines = [s.nodes[index].deadline for index in indexes]
        self.assertTrue(all(deadline is not None for deadline in deadlines))
        s.now = max(s.now, max(deadlines))
        for index in indexes:
            s.nodes[index].tick()

    def reports(self, s, target, byzantine_empty=True):
        instance = s.nodes[next(i for i in range(s.n) if i not in s.byzantine)].instance
        if byzantine_empty:
            for index in sorted(s.byzantine):
                s.byzantine_message(message("VIEW_CHANGE", index, instance, target,
                                           prepared=None, decision=None))
        return self.by_sender(self.signatures(s, "VIEW_CHANGE", target))

    def test_E_omitted_lone_preparation_cannot_veto_valid_new_view(self):
        for n, late in ((4, False), (7, False), (4, True), (7, True)):
            with self.subTest(n=n, learned_after_report=late):
                s = self.simulator(n, byzantine=(0,))
                old, replacement = sorted([self.body(s, 100), self.body(s, 200)],
                                          key=value_id, reverse=True)
                holder = n - 1
                old, qc = self.prepared_schedule(s, [] if late else [holder], old)
                s.offer(body=replacement, nodes=range(1, n))
                self.timeout(s, list(range(1, n)))
                reports = self.reports(s, 1)
                original_report = deepcopy(reports[holder])
                if late:
                    self.assertIsNone(original_report["payload"]["prepared"])
                    self.wire(s, "PREPARED", qc, holder)
                    self.assertEqual(s.nodes[holder].record["highest"], qc)
                    self.assertNotIn(("COMMIT", 0), s.nodes[holder].record["signed"])
                    self.assertEqual(s.nodes[holder].record["signed"][("VIEW_CHANGE", 1)],
                                     original_report)
                chosen = list(range(s.nodes[0].q))
                self.assertNotIn(holder, chosen)
                self.assertTrue(all(reports[i]["payload"]["prepared"] is None for i in chosen))
                for index in chosen:
                    self.deliver_signed(s, reports[index], 1)
                nv = self.signatures(s, "NEW_VIEW", 1)[0]
                self.assertEqual(nv["payload"]["value"], value_id(replacement))
                self.deliver_signed(s, nv, holder)
                self.assertEqual(s.nodes[holder].record["new_views"][1], nv)
                self.assertEqual(s.nodes[holder].record["highest"], qc)
                self.assertEqual(s.nodes[holder].record["signed"][("VIEW_CHANGE", 1)],
                                 original_report)
                self.deliver_signed(s, nv, 1)
                proposal = self.signatures(s, "PROPOSE", 1)[0]
                self.deliver_signed(s, proposal, holder)
                vote = s.nodes[holder].record["signed"][("PREPARE", 1)]
                self.assertEqual(vote["payload"]["value"], value_id(replacement))
                self.assertIn((0, value_id(old)), s.nodes[holder].record["prepared"])
                s.run(160, stop=lambda sim: sim.settled())
                self.assertTrue(s.settled())
                for index in range(1, n):
                    self.assertEqual(s.nodes[index].d["parent"], value_id(replacement))
                self.audit(s)

    def test_F_hidden_commit_quorum_forces_intersecting_view_report(self):
        for n in (4, 7):
            with self.subTest(n=n):
                faulty = (1,) if n == 4 else (1, n - 1)
                s = self.simulator(n, byzantine=faulty)
                honest = [i for i in range(n) if i not in faulty]
                holders = honest[:s.nodes[0].q - len(faulty)]
                old, qc = self.prepared_schedule(s, holders)
                cert = self.certificate(s, qc)
                for signed in [qc["proposal"]] + qc["prepares"] + cert["commits"]:
                    self.deliver_signed(s, signed, 1)
                adversary_received = {event.data["auth"] for event in s.adversary_inbox
                                      if event.destination == 1 and event.kind == "SIGNED"}
                self.assertTrue({signed["auth"] for signed in
                                 [qc["proposal"]] + qc["prepares"] + cert["commits"]}
                                <= adversary_received)
                self.assertTrue(all(node.record["decision"] is None for node in s.nodes))
                self.assertEqual(len(cert["commits"]), s.nodes[0].q)
                self.assertGreaterEqual(len(holders), s.nodes[0].f + 1)
                audit = self.audit(s)  # Count the quorum before anyone has its certificate.
                self.assertEqual(audit["possible_commit_quorums"], 1)
                self.assertEqual(audit["decisions"], 0)
                self.timeout(s, honest)
                reports = self.reports(s, 1)
                chosen = sorted([i for i in range(n) if i not in holders]
                                + holders[-1:])
                self.assertEqual(len(chosen), s.nodes[0].q)
                report_set = [reports[i] for i in chosen]
                self.assertEqual([i for i in chosen if reports[i]["payload"]["prepared"]], holders[-1:])
                other = self.body(s, 200)
                s.offer(body=other, nodes=honest)
                bad = s.byzantine_message(message("NEW_VIEW", 1, old["instance"], 1,
                                                  value_id(other), reports=report_set))
                for index in honest:
                    self.deliver_signed(s, bad, index)
                    self.assertEqual(s.nodes[index].last_reason, "HIGHEST_PREPARED_SELECTION")
                    self.assertNotIn(1, s.nodes[index].record["new_views"])
                good = s.byzantine_message(message("NEW_VIEW", 1, old["instance"], 1,
                                                   value_id(old), reports=report_set))
                proposal = s.byzantine_message(message("PROPOSE", 1, old["instance"], 1,
                                                       value_id(old), new_view=good))
                for index in honest:
                    self.deliver_signed(s, good, index)
                    self.deliver_signed(s, proposal, index)
                self.assertTrue(all(v["payload"]["value"] == value_id(old)
                                    for v in self.signatures(s, "PREPARE", 1)))
                # The old complete certificate remained solely with the adversary
                # until this explicit reveal, despite changing views in between.
                for index in honest:
                    self.wire(s, "CERT", cert, index, source=1)
                    self.assertEqual(s.nodes[index].d["parent"], value_id(old))
                self.audit(s)

    def test_G_late_preparation_does_not_rewrite_immutable_report(self):
        s = self.simulator(byzantine=(0,))
        body, qc = self.prepared_schedule(s)
        self.timeout(s, [1, 2, 3])
        reports = self.reports(s, 1)
        original = deepcopy(reports[2])
        self.assertIsNone(original["payload"]["prepared"])
        self.wire(s, "PREPARED", qc, 2)
        self.assertEqual(s.nodes[2].record["highest"], qc)
        self.assertNotIn(("COMMIT", 0), s.nodes[2].record["signed"])
        s.nodes[2].crash()
        s.nodes[2].restart()
        self.assertEqual(s.nodes[2].record["signed"][("VIEW_CHANGE", 1)], original)
        for index in (0, 2, 3):
            self.deliver_signed(s, reports[index], 2)
        self.timeout(s, [2])
        next_report = s.nodes[2].record["signed"][("VIEW_CHANGE", 2)]
        self.assertEqual(next_report["payload"]["prepared"], qc)
        self.assertEqual(s.nodes[2].record["signed"][("VIEW_CHANGE", 1)], original)
        self.assertEqual([v for v in self.signatures(s, "VIEW_CHANGE", 1)
                          if v["payload"]["sender"] == 2], [original])
        self.assertEqual(value_id(body), qc["proposal"]["payload"]["value"])
        self.audit(s)

    def test_G_future_prepared_proof_carries_new_view_but_needs_accepted_proposal(self):
        s = self.simulator(byzantine=(0,))
        body, old_qc = self.prepared_schedule(s)
        self.timeout(s, [1, 2])
        reports = self.reports(s, 1)
        for index in (0, 1, 2):
            self.deliver_signed(s, reports[index], 1)
        nv = self.signatures(s, "NEW_VIEW", 1)[0]
        self.deliver_signed(s, nv, 1)
        proposal = self.signatures(s, "PROPOSE", 1)[0]
        for index in (1, 2):
            self.deliver_signed(s, proposal, index)
        s.byzantine_message(message("PREPARE", 0, body["instance"], 1, value_id(body)))
        votes = self.by_sender(self.signatures(s, "PREPARE", 1))
        future_qc = {"proposal": proposal, "prepares": [votes[i] for i in (0, 1, 2)]}
        laggard = s.nodes[3]
        self.assertEqual(laggard.record["view"], 0)
        self.wire(s, "PREPARED", future_qc, 3)
        self.assertEqual(laggard.record["view"], 1)
        self.assertEqual(laggard.record["new_views"][1], nv)
        self.assertEqual(laggard.record["highest"], future_qc)
        self.assertNotIn(1, laggard.record["accepted"])
        self.assertNotIn(("PREPARE", 1), laggard.record["signed"])
        self.assertNotIn(("COMMIT", 1), laggard.record["signed"])
        # Older valid evidence is retained independently and cannot downgrade it.
        self.wire(s, "PREPARED", old_qc, 3)
        self.assertEqual(laggard.record["highest"], future_qc)
        self.assertIn((0, value_id(body)), laggard.record["prepared"])
        self.deliver_signed(s, proposal, 3)
        self.wire(s, "PREPARED", future_qc, 3)
        self.assertEqual(laggard.record["signed"][("COMMIT", 1)]["payload"]["value"],
                         value_id(body))
        self.audit(s)

    def test_K_old_complete_certificate_applies_after_timeout_and_restart(self):
        for n in (4, 7):
            with self.subTest(n=n):
                s = self.simulator(n)
                body, qc = self.prepared_schedule(s, list(range(n)))
                cert = self.certificate(s, qc)
                node = s.nodes[n - 1]
                before = node.d["snapshot"]
                self.timeout(s, [n - 1])
                self.assertEqual(node.record["mode"], "CHANGING")
                old_votes = deepcopy(node.record["signed"])
                node.crash()
                node.restart()
                self.assertEqual(node.d["snapshot"], before)
                self.wire(s, "CERT", cert, n - 1)
                self.assertEqual(node.d["sequence"], 1)
                self.assertEqual(node.d["parent"], value_id(body))
                self.assertEqual(node.d["records"][0]["signed"], old_votes)
                after = node.d["snapshot"]
                for _ in range(3):
                    node.crash()
                    node.restart()
                    self.wire(s, "CERT", cert, n - 1)
                    self.assertEqual(node.d["snapshot"], after)
                    self.assertEqual(node.d["records"][0]["apply_count"], 1)
                    self.assertEqual(node.d["sequence"], 1)
                self.audit(s)

    def test_L_single_byzantine_future_claim_neither_advances_nor_resets_timer(self):
        for n in (4, 7):
            with self.subTest(n=n):
                s = self.simulator(n, byzantine=(0,))
                body = s.offer(body=self.body(s), nodes=range(1, n))
                node = s.nodes[1]
                deadline = node.deadline
                for target in (PROFILE["limits"]["views"] - 1, 10**9):
                    report = s.byzantine_message(message("VIEW_CHANGE", 0, body["instance"],
                                                        target, prepared=None, decision=None))
                    for _ in range(5):
                        self.wire(s, "SIGNED", report, 1)
                        self.assertEqual(node.record["view"], 0)
                        self.assertEqual(node.deadline, deadline)
                        self.assertEqual(node.record["mode"], "ACTIVE")
                self.timeout(s, [1])
                self.assertEqual(node.record["view"], 1)
                self.assertEqual(node.record["mode"], "CHANGING")
                self.assertIsNone(node.deadline)
                self.assertEqual(len(self.signatures(s, "VIEW_CHANGE", 1)), 1)
                self.audit(s)

    def test_M_prepare_and_commit_crash_boundaries_preserve_exact_votes(self):
        for n in (4, 7):
            for phase in ("PREPARE", "COMMIT"):
                for cut in ("before_record", "after_intent", "after_record", "after_publish"):
                    with self.subTest(n=n, phase=phase, cut=cut):
                        s = self.simulator(n)
                        node = s.nodes[1]
                        if phase == "PREPARE":
                            body = s.offer(body=self.body(s))
                            proposal = self.signatures(s, "PROPOSE")[0]
                            node.cut = (cut, phase)
                            self.deliver_signed(s, proposal, 1)
                        else:
                            body, qc = self.prepared_schedule(s)
                            node.cut = (cut, phase)
                            self.wire(s, "PREPARED", qc, 1)
                        self.assertFalse(node.alive)
                        slot = (phase, 0)
                        self.assertIn(0, node.record["accepted"])
                        self.assertEqual(slot in node.record["intents"], cut != "before_record")
                        self.assertEqual(slot in node.record["signed"], cut in ("after_record", "after_publish"))
                        durable = deepcopy(node.record["signed"].get(slot))
                        published = [v for v in s.published(phase, 0)
                                     if v["payload"]["sender"] == 1]
                        self.assertEqual(bool(published), cut == "after_publish")
                        self.audit(s)
                        node.restart()
                        # A cut before intent persistence leaves no obligation to
                        # resume a signature. Deliver the genuine retained trigger
                        # again before peers can finish without this replica.
                        if phase == "PREPARE":
                            s.nodes[0].retry()
                            self.deliver_signed(s, proposal, 1)
                        else:
                            self.wire(s, "PREPARED", qc, 1)
                        s.run(160, stop=lambda sim: sim.settled())
                        self.assertTrue(s.settled())
                        signed = node.d["records"][0]["signed"][slot]
                        if durable is not None:
                            self.assertEqual(signed, durable)
                        issued = [v for v in self.signatures(s, phase, value=value_id(body))
                                  if v["payload"]["sender"] == 1]
                        self.assertEqual(issued, [signed])
                        before = deepcopy(node.d["records"][0]["signed"])
                        node.crash()
                        node.restart()
                        node.retry()
                        self.assertEqual(node.d["records"][0]["signed"], before)
                        self.assertEqual(node.d["records"][0]["apply_count"], 1)
                        self.audit(s)

    def test_N_decision_and_application_crashes_replay_once(self):
        for n in (4, 7):
            for cut in ("after_decision", "after_application"):
                with self.subTest(n=n, cut=cut):
                    s = self.simulator(n)
                    body, qc = self.prepared_schedule(s, list(range(n)))
                    cert = self.certificate(s, qc)
                    node = s.nodes[1]
                    original = node.d["snapshot"]
                    signed = deepcopy(node.record["signed"])
                    node.cut = (cut, "")
                    self.wire(s, "CERT", cert, 1)
                    self.assertFalse(node.alive)
                    self.assertEqual(node.d["records"][0]["decision"], cert)
                    self.assertEqual(node.d["records"][0]["applied"], cut == "after_application")
                    self.assertEqual(node.d["sequence"], int(cut == "after_application"))
                    if cut == "after_decision":
                        self.assertEqual(node.d["snapshot"], original)
                    node.restart()
                    expected = node.d["snapshot"]
                    self.assertEqual(node.d["sequence"], 1)
                    self.assertEqual(node.d["parent"], value_id(body))
                    for _ in range(3):
                        self.wire(s, "CERT", cert, 1)
                        node.crash()
                        node.restart()
                        self.assertEqual(node.d["records"][0]["apply_count"], 1)
                        self.assertEqual(node.d["records"][0]["signed"], signed)
                        self.assertEqual(node.d["sequence"], 1)
                        self.assertEqual(node.d["snapshot"], expected)
                    self.audit(s)

    def test_O_known_rollback_fences_signing_permanently_but_allows_catch_up(self):
        # This injects a known/suspected loss of freshness. An undetectable,
        # coherently restored old backup is explicitly outside the guarantee.
        for n in (4, 7):
            with self.subTest(n=n):
                s = self.simulator(n)
                body, qc = self.prepared_schedule(s)
                node = s.nodes[1]
                signed = deepcopy(node.record["signed"])
                issued = [v for v in s.authentication.issued if v["payload"]["sender"] == 1]
                node.crash()
                node.restart(suspected_rollback=True)
                self.wire(s, "PREPARED", qc, 1)
                self.assertTrue(node.d["fenced"])
                self.assertEqual(node.record["highest"], qc)
                self.assertEqual(node.record["signed"], signed)
                for _ in range(2):
                    node.crash()
                    node.restart()
                    self.assertTrue(node.d["fenced"])
                for index in range(n):
                    if index != 1:
                        self.wire(s, "PREPARED", qc, index)
                cert = self.certificate(s, qc)
                self.wire(s, "CERT", cert, 1)
                self.assertEqual(node.d["parent"], value_id(body))
                s.run(80, stop=lambda sim: sim.settled())
                self.assertTrue(s.settled())
                second = s.offer()
                s.run(160, stop=lambda sim: sim.settled(2))
                self.assertTrue(s.settled(2))
                self.assertEqual(node.d["parent"], value_id(second))
                self.assertEqual([v for v in s.authentication.issued
                                  if v["payload"]["sender"] == 1], issued)
                self.assertTrue(node.d["fenced"])
                self.audit(s)

    def test_N_restart_after_decision_recovers_missing_volatile_anchor_evidence(self):
        s = self.simulator()
        anchor = max(s.nodes[0].anchors.values(), key=lambda item: item["height"])
        self.assertEqual(anchor["height"], 855501)
        body = s.nodes[0].application.build(s.nodes[0].instance, anchor, {})
        body, qc = self.prepared_schedule(s, list(range(s.n)), body=body)
        cert = self.certificate(s, qc)
        node = s.nodes[1]
        node.cut = ("after_decision", "")
        self.wire(s, "CERT", cert, 1)
        self.assertFalse(node.alive)
        self.assertEqual(node.d["records"][0]["decision"], cert)
        self.assertFalse(node.d["records"][0]["applied"])
        # Restart loses only volatile anchor evidence. Durable decision/body
        # remain, so recovery must fetch the exact evidence and apply once.
        node.restart()
        s.run(160, stop=lambda sim: sim.settled())
        self.assertTrue(s.settled())
        self.assertEqual(node.d["anchor"], anchor)
        self.assertEqual(node.d["parent"], value_id(body))
        self.assertEqual(node.d["records"][0]["apply_count"], 1)
        self.audit(s)

    def test_N_sole_final_offer_survives_crash_before_proposal_intent(self):
        s = self.simulator()
        leader = s.nodes[0]
        body = self.body(s)
        self.assertEqual(body["anchor"], s.initial_anchor)
        leader.cut = ("before_record", "PROPOSE")
        s.offer(body=body, nodes=[0])
        self.assertFalse(leader.alive)
        self.assertEqual(leader.d["retained_bodies"], {value_id(body): body})
        self.assertNotIn(("PROPOSE", 0), leader.record["intents"])
        self.assertEqual(self.signatures(s, "PROPOSE"), [])
        leader.restart()
        # This is the final client offer. Recovery has only its durable body
        # and fair protocol delivery; no client resubmission wakes it up.
        s.run(160, stop=lambda sim: sim.settled())
        self.assertTrue(s.settled(), "durable final offer lost its pending-work recovery path")
        for node in s.nodes:
            self.assertEqual(node.d["parent"], value_id(body))
            self.assertEqual(node.d["records"][0]["apply_count"], 1)
        self.audit(s)

    def test_N_restarted_prepared_replica_aggregates_commit_votes_without_new_proposal(self):
        s = self.simulator()
        body, qc = self.prepared_schedule(s, list(range(s.n)))
        self.assertEqual(body["anchor"], s.initial_anchor)
        cert = self.certificate(s, qc)
        node = s.nodes[1]
        accepted = deepcopy(node.record["accepted"][0])
        self.assertIsNone(node.record["decision"])
        node.crash()
        node.restart()
        self.assertEqual(node.record["accepted"][0], accepted)
        self.assertEqual(node.record["prepared"][(0, value_id(body))], qc)
        # Deliver exactly q genuine COMMIT votes, with no proposal replay or
        # complete certificate delivery to reconstruct the lost header cache.
        delivered_before = s.delivered
        for signed in cert["commits"]:
            self.deliver_signed(s, signed, 1)
        self.assertEqual(s.delivered - delivered_before, node.q)
        self.assertEqual(node.d["sequence"], 1,
                         "durable accepted proposal and PreparedQC cannot aggregate fresh COMMIT quorum")
        self.assertEqual(node.d["parent"], value_id(body))
        self.assertEqual(node.d["records"][0]["apply_count"], 1)
        self.audit(s)

    def test_Q_missing_body_certificate_defers_without_vote_then_fetches(self):
        for n in (4, 7):
            with self.subTest(n=n):
                s = self.simulator(n)
                participants = list(range(n - 1))
                body, qc = self.prepared_schedule(s, participants, participants=participants)
                cert = self.certificate(s, qc)
                index, node = n - 1, s.nodes[n - 1]
                before = node.d["snapshot"]
                self.assertNotIn(value_id(body), node.bodies)
                self.wire(s, "CERT", cert, index)
                self.assertEqual(node.last_reason, "NEED_DATA:body:" + value_id(body))
                self.assertEqual(node.d["snapshot"], before)
                self.assertEqual(node.d["sequence"], 0)
                self.assertIsNone(node.record["decision"])
                self.assertEqual(node.record["signed"], {})
                self.assertEqual(len(node.pending), 1)
                serial = s.serial
                self.assertTrue(s.deliver_where(lambda e: e.kind == "GET"
                                               and e.source == index and e.destination == 0))
                self.assertTrue(s.deliver_where(lambda e: e.kind == "DATA" and e.serial > serial
                                               and e.source == 0 and e.destination == index
                                               and e.data["id"] == value_id(body)))
                node.pump()
                self.assertEqual(node.d["sequence"], 1)
                self.assertEqual(node.d["parent"], value_id(body))
                self.assertEqual(node.d["records"][0]["apply_count"], 1)
                self.assertEqual(node.d["records"][0]["signed"], {})
                self.assertFalse(node.pending)
                self.audit(s)


if __name__ == "__main__":
    unittest.main()
