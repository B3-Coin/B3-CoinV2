"""Bounded untrusted-data admission regressions; synthetic replicas only."""
from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_application import anchor_chain, value_id
from fm_checker import check
from fm_protocol import PROFILE, message
from fm_simulator import Simulator
from test_model import BUYER, USD_A, Fixture
import test_recovery as recovery_cases


class AdmissionTests(unittest.TestCase):
    def simulator(self, n=4):
        return Simulator(Fixture().model.snapshot(), n=n, byzantine=(n - 1,))

    def deliver(self, node, body, identity=None, reference=None):
        packet = {"type": "body", "id": identity or value_id(body), "object": body}
        if reference is not None:
            packet["reference"] = reference
        node.receive("DATA", packet, node.n - 1)
        node.pump()

    def body(self, s, amount=100):
        return recovery_cases.RecoveryTests().body(s, amount)

    def drain(self, node):
        for _ in range(2 * PROFILE["admission"]["pending"] + 4):
            if not node.inbox:
                return
            node.pump()
        self.fail("admission replay did not drain within its bounded queue")

    def audit(self, s):
        self.assertFalse(s.exhausted)
        for i, node in enumerate(s.nodes):
            if i not in s.byzantine:
                self.assertFalse(node.d["halt"])
                self.assertLessEqual(len(node.bodies), PROFILE["limits"]["objects"])
                self.assertLessEqual(len(node.references), PROFILE["admission"]["references"])
                self.assertLessEqual(len(node.requests), PROFILE["admission"]["requests"])
                self.assertLessEqual(len(node.pending), PROFILE["admission"]["pending"])
        check(s)

    def finish(self, s, identity=None):
        s.run(350, stop=lambda sim: sim.settled())
        self.assertTrue(s.settled(), [node.last_reason for node in s.nodes])
        if identity is not None:
            for i, node in enumerate(s.nodes):
                if i not in s.byzantine:
                    self.assertEqual(node.d["parent"], identity)
                    self.assertEqual(node.d["records"][0]["apply_count"], 1)
        self.audit(s)

    def inline(self, s, node, body):
        self.assertTrue(s.deliver_where(
            lambda event: event.destination == node.index and event.kind == "DATA"
            and event.data.get("id") == value_id(body)
            and event.data.get("reference", {}).get("kind") == "SIGNED"))
        self.drain(node)

    def test_exact_257_unsolicited_invalid_bodies_do_not_halt_signing(self):
        for n in (4, 7):
            with self.subTest(n=n):
                s = self.simulator(n)
                node = s.nodes[0]
                initial = deepcopy(node.d)
                for i in range(257):
                    self.deliver(node, {"unrequested_invalid_body": i})
                    s.record(0, "admission_observation", {
                        "received": i + 1, "cached_bodies": len(node.bodies),
                        "pending": len(node.pending), "halt": node.d["halt"],
                        "last_reason": node.last_reason, "sequence": node.d["sequence"]})
                before = {"cached_bodies": len(node.bodies), "halt": node.d["halt"]}
                node.restart()
                s.record(0, "restart_admission_observation", {
                    "before": before, "after": {"cached_bodies": len(node.bodies),
                    "halt": node.d["halt"], "sequence": node.d["sequence"]}})
                self.assertFalse(node.d["halt"])
                self.assertEqual(node.d, initial)
                self.assertEqual(len(node.bodies), 0)
                s.offer(nodes=[1])
                s.run(220, stop=lambda sim: sim.settled())
                self.assertTrue(s.settled())
                check(s)

    def test_plausible_unrequested_bodies_do_not_allocate_or_apply(self):
        s = self.simulator()
        node = s.nodes[1]
        initial = deepcopy(node.d)
        # Every body is an independently valid application preview with the
        # correct current context, but no protocol party has requested it.
        for i in range(257):
            self.deliver(node, self.body(s, 1000 + i))
            self.assertEqual(node.last_reason, "UNREQUESTED_BODY_DATA")
        self.assertEqual(node.d, initial)
        self.assertFalse(node.bodies or node.requests or node.references or node.pending)
        self.audit(s)
        body = s.offer(nodes=[0])
        self.finish(s, value_id(body))

    def test_wrong_hash_malformed_and_oversized_bodies_are_refused(self):
        for case in ("wrong_hash", "malformed", "extra_field", "oversized"):
            with self.subTest(case=case):
                s = self.simulator()
                node = s.nodes[1]
                initial = deepcopy(node.d)
                body = self.body(s)
                identity = None
                if case == "wrong_hash":
                    identity = "0" * 64
                elif case == "malformed":
                    body = ["not", "a", "candidate"]
                elif case == "extra_field":
                    body["unexpected"] = True
                else:
                    body["padding"] = "x" * (PROFILE["limits"]["proof_bytes"] + 1)
                self.deliver(node, body, identity=identity)
                reasons = [event["reason"] for event in s.trace
                           if event["event"] == "refused" and event["node"] == node.index]
                self.assertEqual(reasons[-1], {"wrong_hash": "BODY_HASH",
                    "malformed": "DATA_BODY_SHAPE", "extra_field": "DATA_BODY_SHAPE",
                    "oversized": "WIRE_BYTES"}[case])
                self.assertEqual(node.d, initial)
                self.assertFalse(node.bodies or node.requests or node.references or node.pending)
                self.audit(s)
                accepted = s.offer(nodes=[0])
                self.finish(s, value_id(accepted))

    def test_inline_proposal_data_needs_no_previous_fetch(self):
        for n in (4, 7):
            with self.subTest(n=n):
                s = self.simulator(n)
                body = s.offer(body=self.body(s), nodes=[0])
                node = s.nodes[1]
                before = node.d["snapshot"]
                self.assertFalse(node.requests or node.references or node.bodies)
                self.inline(s, node, body)
                self.assertEqual(node.bodies[value_id(body)], body)
                self.assertEqual(node.record["signed"][("PREPARE", 0)]["payload"]["value"], value_id(body))
                self.assertEqual(node.d["snapshot"], before)
                self.assertEqual(node.d["sequence"], 0)
                self.audit(s)
                self.finish(s, value_id(body))

    def test_duplicate_valid_body_does_not_grow_cache_or_repeat_signature(self):
        s = self.simulator()
        body = s.offer(body=self.body(s), nodes=[0])
        node = s.nodes[1]
        self.inline(s, node, body)
        proposal = node.record["accepted"][0]["signed"]
        original = deepcopy(node.d)
        signatures = deepcopy(s.authentication.issued)
        for _ in range(20):
            self.deliver(node, body)
            self.deliver(node, body, reference={"kind": "SIGNED", "data": proposal})
        self.assertEqual(node.d, original)
        self.assertEqual(s.authentication.issued, signatures)
        self.assertEqual(set(node.bodies), {value_id(body)})
        self.audit(s)
        self.finish(s, value_id(body))

    def test_forged_irrelevant_and_insufficient_proof_references_are_refused(self):
        for case in ("forged", "other_value", "missing_quorum", "other_context"):
            with self.subTest(case=case):
                s = self.simulator()
                body = s.offer(body=self.body(s), nodes=[0])
                proposal = deepcopy(s.published("PROPOSE")[0])
                reference = {"kind": "SIGNED", "data": proposal}
                node = s.nodes[1]
                initial = deepcopy(node.d)
                if case == "forged":
                    proposal["auth"] = "forged-admission-reference"
                elif case == "other_value":
                    body = self.body(s, 200)
                elif case == "missing_quorum":
                    reference = {"kind": "PREPARED", "data": {"proposal": proposal, "prepares": []}}
                else:
                    body["instance"]["parent"] = "f" * 64
                self.deliver(node, body, reference=reference)
                self.assertEqual(node.d, initial)
                self.assertFalse(node.bodies or node.requests or node.pending)
                self.assertTrue(any(event["event"] == "refused" and event["node"] == node.index
                                    for event in s.trace))
                self.audit(s)
                self.finish(s)

    def test_byzantine_hash_advertisements_have_bounded_references_and_requests(self):
        s = Simulator(Fixture().model.snapshot(), byzantine=(0,))
        node = s.nodes[1]
        original = deepcopy(node.d)
        for i in range(257):
            signed = s.authentication.adversary_sign(message(
                "PROPOSE", 0, node.instance, 0, f"{i + 1:064x}", new_view=None))
            node.receive("SIGNED", signed, 0)
            node.pump()
        self.assertEqual(node.d, original)
        self.assertFalse(node.bodies)
        self.assertLessEqual(len(node.references), PROFILE["admission"]["references_per_slot"])
        self.assertLessEqual(len(node.requests), PROFILE["admission"]["references_per_slot"])
        self.assertLessEqual(len(node.pending), PROFILE["admission"]["references_per_slot"])
        self.audit(s)
        body = s.offer(nodes=[1])
        self.finish(s, value_id(body))

    def test_expired_reply_is_rejected_then_refreshed_request_recovers(self):
        s = self.simulator()
        body = s.offer(body=self.body(s), nodes=[0])
        node = s.nodes[1]
        proposal = s.published("PROPOSE")[0]
        recovery_cases.RecoveryTests().deliver_signed(s, proposal, node.index)
        slot = ("body", value_id(body))
        self.assertIn(slot, node.requests)
        initial = deepcopy(node.d)
        # Withhold all scheduled deliveries and timer events through this
        # lease. Expired data cannot renew its own admission authority.
        s.now = node.requests[slot]["expires"] + 1
        self.deliver(node, body)
        self.assertEqual(node.last_reason, "UNREQUESTED_BODY_DATA")
        self.assertNotIn(value_id(body), node.bodies)
        self.assertEqual(node.d, initial)
        node.receive("SIGNED", proposal, 0)
        node.pump()
        self.assertGreater(node.requests[slot]["expires"], s.now)
        self.deliver(node, body)
        self.drain(node)
        self.assertIn(value_id(body), node.bodies)
        self.assertIn(("PREPARE", 0), node.record["signed"])
        self.audit(s)
        self.finish(s, value_id(body))

    def test_referenced_invalid_application_body_cannot_mutate_accounting(self):
        s = Simulator(Fixture().model.snapshot(), byzantine=(0,))
        node = s.nodes[1]
        initial = deepcopy(node.d)
        body = self.body(s)
        body["result"] = "0" * 64
        signed = s.authentication.adversary_sign(message(
            "PROPOSE", 0, node.instance, 0, value_id(body), new_view=None))
        self.deliver(node, body, reference={"kind": "SIGNED", "data": signed})
        self.assertEqual(node.d, initial)
        self.assertNotIn(value_id(body), node.bodies)
        self.assertFalse(node.record["signed"])
        self.assertTrue(any(event["event"] == "refused" for event in s.trace))
        self.audit(s)
        accepted = s.offer(nodes=[1])
        self.finish(s, value_id(accepted))

    def test_missing_anchor_defers_inline_body_without_fabricated_validation(self):
        s = self.simulator()
        chain = anchor_chain()
        anchor = max(chain.values(), key=lambda item: item["height"])
        s.nodes[0].anchors.update(deepcopy(chain))
        f = Fixture()
        body = s.nodes[0].application.build(s.nodes[0].instance, anchor,
                                          {"deposits": [f.fact(BUYER, USD_A, 100)]})
        s.offer(body=body, nodes=[0])
        node = s.nodes[1]
        # A supported restart loses this receiver's volatile ancestry while
        # the sender and independent checker retain the real source evidence.
        node.crash()
        node.restart()
        before = deepcopy(node.d)
        self.inline(s, node, body)
        self.assertEqual(node.d, before)
        self.assertNotIn(value_id(body), node.bodies)
        self.assertFalse(node.record["signed"])
        self.assertTrue(node.pending)
        self.assertTrue(any(key[0] == "anchor" for key in node.requests))
        self.audit(s)
        self.finish(s, value_id(body))

    def test_junk_before_and_during_a_valid_decision_preserves_progress(self):
        s = self.simulator()
        for node in s.nodes[:3]:
            for i in range(90):
                self.deliver(node, {"unrequested_invalid_body": i})
        body = s.offer(body=self.body(s), nodes=[0])
        self.inline(s, s.nodes[1], body)
        for node in s.nodes[:3]:
            for i in range(90, 180):
                self.deliver(node, {"unrequested_invalid_body": i})
        self.audit(s)
        self.finish(s, value_id(body))

    def test_pressure_preserves_prepared_commit_evidence_across_restart(self):
        s = self.simulator()
        schedule = recovery_cases.RecoveryTests()
        body, qc = schedule.prepared_schedule(s, holders=[1], participants=[0, 1, 2])
        node = s.nodes[1]
        before = deepcopy(node.d)
        self.assertEqual(node.record["highest"], qc)
        self.assertIn(("COMMIT", 0), node.record["signed"])
        for i in range(257):
            self.deliver(node, self.body(s, 1000 + i))
        self.assertEqual(node.d, before)
        self.assertEqual(node.bodies[value_id(body)], body)
        self.audit(s)
        node.crash()
        node.restart()
        self.assertEqual(node.d, before)
        self.assertEqual(node.bodies[value_id(body)], body)
        self.audit(s)
        self.finish(s, value_id(body))

    def test_certificate_body_arriving_after_view_change_recovers_old_decision(self):
        for n in (4, 7):
            with self.subTest(n=n):
                s = Simulator(Fixture().model.snapshot(), n=n)
                schedule = recovery_cases.RecoveryTests()
                participants = list(range(n - 1))
                body, qc = schedule.prepared_schedule(s, holders=participants, participants=participants)
                cert = schedule.certificate(s, qc)
                node = s.nodes[-1]
                node.change_view(1)
                old_votes = deepcopy(node.record["signed"])
                before = node.d["snapshot"]
                schedule.wire(s, "CERT", cert, node.index)
                self.assertIn(("body", value_id(body)), node.requests)
                for i in range(257):
                    self.deliver(node, {"unrequested_invalid_body": i})
                self.assertEqual(node.d["snapshot"], before)
                self.assertEqual(node.record["signed"], old_votes)
                self.audit(s)
                self.deliver(node, body)
                self.drain(node)
                self.assertEqual(node.d["sequence"], 1)
                self.assertEqual(node.d["records"][0]["signed"], old_votes)
                self.assertEqual(node.d["records"][0]["apply_count"], 1)
                self.finish(s, value_id(body))


if __name__ == "__main__":
    unittest.main()
