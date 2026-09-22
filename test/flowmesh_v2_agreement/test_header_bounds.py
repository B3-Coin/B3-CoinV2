"""Bounded proposal-header retention and incremental aggregation regressions.

All Byzantine signatures belong to configured seat0. Honest preparation and
commit votes come only from genuine replica receive/execute/sign transitions.
The finite hostile prefix deliberately delays tick() while input continues;
afterwards normal timers and fair delivery resume without new client ingress.
"""
from copy import deepcopy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_application import canonical, value_id
from fm_checker import check
from fm_protocol import PROFILE, message
from fm_simulator import Simulator
from test_model import Fixture
import test_recovery as recovery_cases


class HeaderBoundsTests(unittest.TestCase):
    def simulator(self):
        return Simulator(Fixture().model.snapshot(), byzantine=(0,))

    def receive(self, node, kind, data, source=0):
        node.receive(kind, data, source)
        for _ in range(PROFILE["limits"]["inbox"]):
            if not node.inbox:
                break
            node.pump()
        self.assertFalse(node.inbox)

    def proposal(self, sim, body):
        return sim.authentication.adversary_sign(message(
            "PROPOSE", 0, body["instance"], 0, value_id(body), new_view=None))

    def body_with_reference(self, node, body, kind, proof):
        self.receive(node, "DATA", {"type": "body", "id": value_id(body),
            "object": body, "reference": {"kind": kind, "data": proof}})

    def prepared_without_offers(self, sim):
        helper = recovery_cases.RecoveryTests()
        body = helper.body(sim)
        proposal = self.proposal(sim, body)
        for index in (1, 2, 3):
            self.body_with_reference(sim.nodes[index], body, "SIGNED", proposal)
        votes = helper.by_sender(helper.signatures(sim, "PREPARE"))
        qc = {"proposal": proposal, "prepares": [votes[index] for index in (1, 2, 3)]}
        self.receive(sim.nodes[1], "PREPARED", qc)
        self.assertEqual(sim.nodes[1].record["highest"], qc)
        self.assertTrue(all(not node.offers and not node.d["retained_bodies"]
                            for node in sim.nodes[1:]))
        return body, qc

    def assert_bounded(self, node):
        # RESOURCE BOUNDS are independent of the signing-safety checker.
        self.assertLessEqual(len(node.headers), PROFILE["admission"]["headers"])
        encoded = sum(len(canonical({"slot": list(key), "signed": header}))
                      for key, header in node.headers.items())
        self.assertEqual(node.header_bytes, encoded)
        self.assertEqual(sum(node.header_sizes.values()), encoded)
        self.assertEqual(set(node.header_sizes), set(node.headers))
        self.assertLessEqual(encoded, PROFILE["admission"]["header_bytes"])
        self.assertLessEqual(len(node.references), PROFILE["admission"]["references"])
        self.assertLessEqual(len(node.requests), PROFILE["admission"]["requests"])
        self.assertLessEqual(len(node.pending), PROFILE["admission"]["pending"])
        self.assertLessEqual(len(node.votes), PROFILE["admission"]["received_vote_buckets"])
        cleanup = node.last_header_work
        self.assertLessEqual(cleanup["headers_inspected"], 4 * PROFILE["admission"]["headers"])
        self.assertLessEqual(cleanup["references_inspected"], 2 * PROFILE["admission"]["references"])
        self.assertLessEqual(cleanup["requests_inspected"], 2 * PROFILE["admission"]["requests"])
        self.assertLessEqual(cleanup["pending_inspected"], 2 * PROFILE["admission"]["pending"])
        self.assertLessEqual(cleanup["vote_slots_checked"], 2 * cleanup["headers_removed"])
        # One event looks up only its affected candidate, never every header.
        work = node.last_aggregation_work
        self.assertLessEqual(work["keys_inspected"], 1)
        self.assertLessEqual(work["phase_checks"], 2)
        self.assertLessEqual(work["votes_inspected"], 2 * node.n)
        self.assertLessEqual(work["proofs_built"], 2)
        self.assertLessEqual(work["header_cache_lookups"], 1)
        self.assertLessEqual(work["durable_lookups"], 3)
        self.assertFalse(node.d["halt"])

    def churn(self, sim, node, start, stop, advance_clock=True):
        helper = recovery_cases.RecoveryTests()
        peaks = {"headers": 0, "header_bytes": 0, "aggregation_keys": 0,
                 "aggregation_votes": 0}
        previous = None
        for number in range(start, stop):
            if advance_clock:
                sim.now += PROFILE["admission"]["lease_ticks"] + 1
            body = helper.body(sim, amount=10000 + number)
            proposal = self.proposal(sim, body)
            self.body_with_reference(node, body, "SIGNED", proposal)
            self.assert_bounded(node)
            key = (0, value_id(body))
            self.assertIn(key, node.headers, "bounded cleanup must still admit the new valid header")
            if advance_clock and previous is not None:
                self.assertNotIn(previous, node.headers, "expired disposable header must be reclaimed on ingress")
            previous = key
            peaks["headers"] = max(peaks["headers"], len(node.headers))
            peaks["header_bytes"] = max(peaks["header_bytes"], node.header_bytes)
            peaks["aggregation_keys"] = max(peaks["aggregation_keys"],
                                             node.last_aggregation_work["keys_inspected"])
            peaks["aggregation_votes"] = max(peaks["aggregation_votes"],
                                              node.last_aggregation_work["votes_inspected"])
        return {"conflicting_bodies": stop, **peaks}

    def finish_without_new_ingress(self, sim, body):
        sent_offers = []
        def fair(source, target, kind, data):
            if kind == "OFFER":
                sent_offers.append((source, target))
            return 1
        sim.policy = fair
        # Explicitly resume the timers starved during the finite input prefix.
        for node in sim.nodes[1:]:
            node.tick()
        sim.run(350, stop=lambda s: s.settled())
        # PROGRESS is asserted directly, not inferred from checker success.
        self.assertTrue(sim.settled(), [(n.record["view"], n.last_reason) for n in sim.nodes[1:]])
        self.assertFalse(sim.exhausted)
        self.assertEqual(sent_offers, [], "an OFFER must not be used to rescue the fixture")
        self.assertEqual(len({node.d["snapshot"] for node in sim.nodes[1:]}), 1)
        for node in sim.nodes[1:]:
            self.assertEqual(node.d["parent"], value_id(body))
            self.assertEqual(node.d["records"][0]["apply_count"], 1)
            self.assertFalse(node.d["halt"])
            self.assert_bounded(node)
        check(sim)

    def test_100_200_300_conflicting_bodies_without_ticks_are_bounded_and_progress(self):
        sim = self.simulator()
        body, qc = self.prepared_without_offers(sim)
        node = sim.nodes[1]
        protected = deepcopy(node.d)
        deadline = node.deadline
        observations = []
        for start, stop in ((0, 100), (100, 200), (200, 300)):
            observations.append(self.churn(sim, node, start, stop))
            self.assertEqual(node.d, protected)
            self.assertEqual(node.record["prepared"][(0, value_id(body))], qc)
            self.assertEqual(node.deadline, deadline, "ingress must not move the existing deadline")
            self.assertLess(node.deadline, sim.now)
        check(sim)
        print({"header_ingress_without_ticks": observations})
        self.finish_without_new_ingress(sim, body)

    def test_header_count_pressure_at_disclosed_small_test_cap(self):
        # Cache-only fixture parameters; no quorum, phase or economic change.
        with patch.dict(PROFILE["admission"], headers=2, references_per_slot=32):
            sim = self.simulator()
            body, qc = self.prepared_without_offers(sim)
            node = sim.nodes[1]
            protected = deepcopy(node.d)
            self.churn(sim, node, 0, 12, advance_clock=False)
            self.assertEqual(node.d, protected)
            self.assertEqual(node.record["highest"], qc)
            self.assertLessEqual(len(node.headers), 2)
            self.assertGreater(len(sim.authentication.issued), len(node.headers))
            self.finish_without_new_ingress(sim, body)

    def test_header_byte_pressure_at_disclosed_small_test_cap(self):
        # A byte budget below three normal headers proves byte enforcement
        # separately from the 32-header count cap, including protected recovery.
        with patch.dict(PROFILE["admission"], header_bytes=1500, references_per_slot=32):
            sim = self.simulator()
            body, qc = self.prepared_without_offers(sim)
            node = sim.nodes[1]
            single = len(canonical({"slot": [0, value_id(body)], "signed": qc["proposal"]}))
            self.assertLess(single, PROFILE["admission"]["header_bytes"])
            self.assertGreater(single * 3, PROFILE["admission"]["header_bytes"])
            protected = deepcopy(node.d)
            self.churn(sim, node, 0, 12, advance_clock=False)
            self.assertEqual(node.d, protected)
            self.assertLessEqual(node.header_bytes, 1500)
            self.assertLess(len(node.headers), PROFILE["admission"]["headers"])
            # Larger later headers may be refused by this cache. Exact durable
            # evidence still permits normal agreement without raising its cap.
            self.finish_without_new_ingress(sim, body)

    def refusals(self, sim, reason, node=1):
        return [event for event in sim.trace if event["node"] == node
                and event["event"] == "refused" and event.get("kind") == "HEADER_CACHE"
                and event.get("reason") == reason and event.get("admitted") is False]

    def test_below_single_header_byte_cap_uses_durable_evidence_without_halt(self):
        # Deliberately tiny TEST cache policy. This cannot hold even one valid
        # header, but cache retention is not authority to stop durable voting.
        with patch.dict(PROFILE["admission"], header_bytes=128):
            sim = self.simulator()
            body, qc = self.prepared_without_offers(sim)
            node = sim.nodes[1]
            self.assertEqual(node.headers, {})
            self.assertEqual(node.header_bytes, 0)
            self.assertTrue(self.refusals(sim, "HEADER_BYTES_PRESSURE"))
            self.assertEqual(node.record["highest"], qc)
            self.assertIn(("PREPARE", 0), node.record["signed"])
            self.assertIn(("COMMIT", 0), node.record["signed"])
            issued = deepcopy(sim.authentication.issued)
            self.finish_without_new_ingress(sim, body)
            self.assertEqual(sim.authentication.issued[:len(issued)], issued)
            self.assertFalse(any(event["event"] == "header_cache_admitted" for event in sim.trace))

    def test_cache_pressure_fallback_does_not_accept_forged_or_wrong_instance_proposals(self):
        with patch.dict(PROFILE["admission"], header_bytes=128):
            sim = self.simulator()
            body, qc = self.prepared_without_offers(sim)
            node = sim.nodes[1]
            protected = deepcopy(node.d)
            refused_before = len(self.refusals(sim, "HEADER_BYTES_PRESSURE"))
            forged = deepcopy(qc["proposal"])
            forged["auth"] = "0" * 64
            self.receive(node, "SIGNED", forged)
            self.assertEqual(node.last_reason, "AUTHENTICATION")
            self.assertEqual(node.d, protected)
            other_instance = deepcopy(body["instance"])
            other_instance["config"] = "f" * 64
            wrong_context = sim.authentication.adversary_sign(message(
                "PROPOSE", 0, other_instance, 0, value_id(body), new_view=None))
            self.receive(node, "SIGNED", wrong_context)
            self.assertEqual(node.last_reason, "INSTANCE_CONTEXT")
            self.assertEqual(node.d, protected)
            self.assertEqual(len(self.refusals(sim, "HEADER_BYTES_PRESSURE")), refused_before)
            self.assertEqual(node.headers, {})
            self.assert_bounded(node)
            self.finish_without_new_ingress(sim, body)

    def commit_quorum(self, sim, body, qc):
        for index in (2, 3):
            self.receive(sim.nodes[index], "PREPARED", qc)
        sim.authentication.adversary_sign(message(
            "COMMIT", 0, body["instance"], 0, value_id(body)))
        helper = recovery_cases.RecoveryTests()
        votes = helper.by_sender(helper.signatures(sim, "COMMIT", value=value_id(body)))
        return [votes[index] for index in (0, 2, 3)]

    def test_full_protected_header_count_uses_exact_durable_preparation_for_commit(self):
        with patch.dict(PROFILE["admission"], headers=1):
            sim = self.simulator()
            old, alternate, qc = self.split_candidate(sim)
            node = sim.nodes[1]
            original = deepcopy(node.record["signed"][("PREPARE", 0)])
            self.body_with_reference(node, alternate, "PREPARED", qc)
            self.assertEqual(set(node.headers), {(0, value_id(old))})
            self.assertEqual(node.record["highest"], qc)
            self.assertTrue(self.refusals(sim, "PROTECTED_HEADER_CAPACITY"))
            for signed in self.commit_quorum(sim, alternate, qc):
                self.receive(node, "SIGNED", signed, signed["payload"]["sender"])
            self.assertEqual(node.d["sequence"], 1)
            self.assertEqual(node.d["records"][0]["signed"][("PREPARE", 0)], original)
            self.assertNotIn(("COMMIT", 0), node.d["records"][0]["signed"])
            self.assertEqual(node.d["records"][0]["apply_count"], 1)
            self.finish_without_new_ingress(sim, alternate)

    def test_default_32_header_count_with_sustained_same_clock_pressure(self):
        # Only the disposable reference-per-slot test parameter changes. The
        # default 32-header/64MiB budgets and all agreement rules stay intact.
        with patch.dict(PROFILE["admission"], references_per_slot=32):
            self.assertEqual(PROFILE["admission"]["headers"], 32)
            sim = self.simulator()
            body, qc = self.prepared_without_offers(sim)
            node = sim.nodes[1]
            protected = deepcopy(node.d)
            node.crash()
            node.restart()
            self.assertEqual(node.d, protected)
            self.assertFalse(node.references)
            self.assertEqual(set(node.headers), {(0, value_id(body))})
            observations = []
            for start, stop in ((0, 100), (100, 200), (200, 300)):
                observations.append(self.churn(sim, node, start, stop, advance_clock=False))
                self.assertEqual(sim.now, 0)
                self.assertEqual(len(node.headers), 32)
                self.assertEqual(node.d, protected)
                self.assertEqual(node.record["highest"], qc)
            self.assertTrue(any(event["event"] == "header_cache_removed"
                                and event["reason"] == "cache_pressure" for event in sim.trace))
            print({"default_count_header_pressure": observations})
            self.finish_without_new_ingress(sim, body)

    def split_candidate(self, sim):
        """One honest vote for A; a possible real prepared quorum for B."""
        helper = recovery_cases.RecoveryTests()
        old = helper.body(sim, 100)
        alternate = helper.body(sim, 200)
        old_proposal, proposal = self.proposal(sim, old), self.proposal(sim, alternate)
        self.body_with_reference(sim.nodes[1], old, "SIGNED", old_proposal)
        for index in (2, 3):
            self.body_with_reference(sim.nodes[index], alternate, "SIGNED", proposal)
        adversarial_vote = sim.authentication.adversary_sign(message(
            "PREPARE", 0, alternate["instance"], 0, value_id(alternate)))
        votes = helper.by_sender(helper.signatures(sim, "PREPARE", value=value_id(alternate)))
        qc = {"proposal": proposal, "prepares": [adversarial_vote, votes[2], votes[3]]}
        sim.nodes[1].proofs.check("prepared", qc, alternate["instance"])
        return old, alternate, qc

    def test_votes_before_conflicting_header_assemble_real_qc_without_resigning(self):
        sim = self.simulator()
        old, alternate, qc = self.split_candidate(sim)
        node = sim.nodes[1]
        original_vote = deepcopy(node.record["signed"][("PREPARE", 0)])
        self.receive(node, "SIGNED", qc["proposal"])
        self.assertNotIn((0, value_id(alternate)), node.headers)
        for vote in qc["prepares"]:
            self.receive(node, "SIGNED", vote, vote["payload"]["sender"])
        self.assertEqual(len(node.votes[(0, value_id(alternate), "PREPARE")]), node.q)
        self.assertNotIn((0, value_id(alternate)), node.record["prepared"])
        self.body_with_reference(node, alternate, "SIGNED", qc["proposal"])
        # A full QC is evidence even when this node accepted the other proposal.
        self.assertEqual(node.record["highest"], qc)
        self.assertEqual(node.record["accepted"][0]["body"], old)
        self.assertEqual(node.record["signed"][("PREPARE", 0)], original_vote)
        self.assertNotIn(("COMMIT", 0), node.record["signed"])
        self.assert_bounded(node)
        self.finish_without_new_ingress(sim, alternate)

    def test_evicted_candidate_valid_proof_requests_missing_body_and_recovers(self):
        sim = self.simulator()
        old, alternate, qc = self.split_candidate(sim)
        node = sim.nodes[1]
        self.body_with_reference(node, alternate, "SIGNED", qc["proposal"])
        original_vote = deepcopy(node.record["signed"][("PREPARE", 0)])
        self.churn(sim, node, 0, 300)
        self.assertNotIn((0, value_id(alternate)), node.headers)
        self.assertNotIn(value_id(alternate), node.bodies)
        self.receive(node, "PREPARED", qc)
        self.assertIn(("body", value_id(alternate)), node.requests)
        self.assertIsNone(node.record["highest"])
        self.body_with_reference(node, alternate, "PREPARED", qc)
        self.assertEqual(node.record["highest"], qc)
        self.assertEqual(node.record["accepted"][0]["body"], old)
        self.assertEqual(node.record["signed"][("PREPARE", 0)], original_vote)
        self.assertNotIn(("COMMIT", 0), node.record["signed"])
        self.assert_bounded(node)
        self.finish_without_new_ingress(sim, alternate)

    def test_commit_votes_before_prepared_header_are_applied_on_evidence_arrival(self):
        sim = self.simulator()
        old, alternate, qc = self.split_candidate(sim)
        node = sim.nodes[1]
        original_vote = deepcopy(node.record["signed"][("PREPARE", 0)])
        for index in (2, 3):
            self.receive(sim.nodes[index], "PREPARED", qc)
        sim.authentication.adversary_sign(message(
            "COMMIT", 0, alternate["instance"], 0, value_id(alternate)))
        helper = recovery_cases.RecoveryTests()
        commits = helper.by_sender(helper.signatures(sim, "COMMIT", value=value_id(alternate)))
        self.receive(node, "SIGNED", qc["proposal"])
        for index in (0, 2, 3):
            self.receive(node, "SIGNED", commits[index], index)
        self.assertEqual(len(node.votes[(0, value_id(alternate), "COMMIT")]), node.q)
        self.assertEqual(node.d["sequence"], 0)
        self.assertNotIn((0, value_id(alternate)), node.record["prepared"])
        issued_before = deepcopy(sim.authentication.issued)
        # This single body + complete preparation arrival must trigger the
        # already-collected COMMIT set; no new vote/retry/timer is injected.
        self.body_with_reference(node, alternate, "PREPARED", qc)
        self.assertEqual(node.d["sequence"], 1)
        self.assertEqual(node.d["parent"], value_id(alternate))
        rec = node.d["records"][0]
        self.assertEqual(rec["apply_count"], 1)
        self.assertEqual(rec["accepted"][0]["body"], old)
        self.assertEqual(rec["signed"][("PREPARE", 0)], original_vote)
        self.assertNotIn(("COMMIT", 0), rec["signed"])
        self.assertEqual(sim.authentication.issued, issued_before)
        self.assert_bounded(node)
        self.finish_without_new_ingress(sim, alternate)


if __name__ == "__main__":
    unittest.main()
