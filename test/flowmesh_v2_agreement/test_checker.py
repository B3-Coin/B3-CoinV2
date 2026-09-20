"""Independent oracle tests, including deliberately impossible bad snapshots.

Negative controls below sometimes use a trusted signer or corrupt acknowledged
state directly. They test that the external oracle can detect a bad history;
they are NOT schedules attainable under the advertised adversary/fault model.
"""
from copy import deepcopy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_checker import check
from fm_protocol import Proofs, message
from fm_simulator import Simulator
from test_model import Fixture, BUYER, USD_A


class CheckerTests(unittest.TestCase):
    def make(self, n=4, byzantine=(), policy=None):
        return Simulator(Fixture().model.snapshot(), n, byzantine, policy=policy)

    def settled(self, n=4, byzantine=()):
        sim = self.make(n, byzantine)
        sim.offer()
        sim.run(250, stop=lambda s: s.settled())
        self.assertTrue(sim.settled())
        return sim

    def hidden(self):
        def policy(source, destination, kind, data):
            if kind == "CERT" or (kind == "SIGNED" and data["payload"]["phase"] == "COMMIT"):
                return None
            return 1
        sim = self.make(policy=policy)
        sim.offer()
        sim.run(10)
        return sim

    def unsafe_mint(self, sim, phase, sender, instance, view, value=None, **extras):
        # Deliberate test-only trust-boundary bypass, never a Byzantine API.
        return sim.authentication.signer(sender)(message(phase, sender, instance, view, value, **extras))

    def test_normal_both_rosters_and_honest_view_changes(self):
        for n, faulty in ((4, ()), (7, ()), (4, (0,)), (7, (0, 1))):
            with self.subTest(n=n, faulty=faulty):
                summary = check(self.settled(n, faulty))
                self.assertEqual(summary["applied_records"], n - len(faulty))
                self.assertEqual(summary["decisions"], 1)
                self.assertGreaterEqual(summary["possible_commit_quorums"], 1)

    def test_checker_never_calls_implementation_proof_verifier(self):
        sim = self.settled(byzantine=(0,))
        before = deepcopy((sim.trace, sim.authentication.issued, [n.d for n in sim.nodes]))
        with patch.object(Proofs, "check", side_effect=AssertionError("implementation oracle called")):
            self.assertEqual(check(sim)["decisions"], 1)
        self.assertEqual(before, (sim.trace, sim.authentication.issued, [n.d for n in sim.nodes]))

    def test_all_issued_commits_count_without_any_delivered_certificate(self):
        sim = self.hidden()
        self.assertFalse(sim.settled())
        self.assertTrue(all(node.record["decision"] is None for node in sim.nodes))
        summary = check(sim)
        self.assertEqual(summary["possible_commit_quorums"], 1)
        self.assertEqual(summary["decisions"], 0)

    def test_signed_but_unpublished_crash_record_is_audited(self):
        sim = self.make()
        sim.nodes[0].cut = ("after_record", "PROPOSE")
        sim.offer()
        self.assertFalse(sim.nodes[0].alive)
        summary = check(sim)
        self.assertEqual(summary["issued_signatures"], 1)
        self.assertEqual(summary["withheld_signatures"], 1)
        sim.nodes[0].record["signed"].clear()
        with self.assertRaisesRegex(AssertionError, "CHECKER_SIGNATURE_NOT_DURABLE"):
            check(sim)

    def test_possible_quorum_counts_only_unpublished_crash_commit_signatures(self):
        sim = self.make()
        for node in sim.nodes:
            node.cut = ("after_record", "COMMIT")
        sim.offer()
        sim.run(10)
        self.assertTrue(all(not node.alive for node in sim.nodes))
        self.assertEqual(sim.published(phase="COMMIT"), [])
        summary = check(sim)
        self.assertEqual(summary["possible_commit_quorums"], 1)
        self.assertEqual(summary["withheld_signatures"], 4)
        self.assertEqual(summary["decisions"], 0)

    def test_unfinished_intent_without_signature_is_safe(self):
        sim = self.make()
        sim.nodes[0].cut = ("after_intent", "PROPOSE")
        sim.offer()
        self.assertEqual(check(sim)["issued_signatures"], 0)

    def test_negative_control_two_hidden_cross_view_commit_quorums(self):
        sim = self.make()
        instance = sim.nodes[0].instance
        for view, value in ((0, "a" * 64), (1, "b" * 64)):
            for sender in (0, 1, 2):
                self.unsafe_mint(sim, "COMMIT", sender, instance, view, value)
        self.assertEqual(sim.trace, [])
        self.assertFalse(any(node.record["decision"] for node in sim.nodes))
        with self.assertRaisesRegex(AssertionError, "CHECKER_CONFLICTING_POSSIBLE_COMMIT_QUORUMS"):
            check(sim)

    def test_negative_control_conflicting_parent_contexts_at_same_sequence(self):
        sim = self.make()
        for view, parent in ((0, "a" * 64), (1, "b" * 64)):
            instance = dict(sim.nodes[0].instance, parent=parent)
            for sender in (0, 1, 2):
                self.unsafe_mint(sim, "COMMIT", sender, instance, view, "c" * 64)
        with self.assertRaisesRegex(AssertionError, "CHECKER_CONFLICTING_POSSIBLE_COMMIT_QUORUMS"):
            check(sim)

    def test_negative_control_immutable_view_change_full_payload(self):
        sim = self.make()
        instance = sim.nodes[0].instance
        self.unsafe_mint(sim, "VIEW_CHANGE", 0, instance, 1, prepared=None, decision=None)
        self.unsafe_mint(sim, "VIEW_CHANGE", 0, instance, 1, prepared={}, decision=None)
        with self.assertRaisesRegex(AssertionError, "CHECKER_HONEST_SLOT_EQUIVOCATION"):
            check(sim)

    def test_negative_control_immutable_new_view_full_payload(self):
        sim = self.make()
        instance = sim.nodes[0].instance
        self.unsafe_mint(sim, "NEW_VIEW", 1, instance, 1, "a" * 64, reports=[])
        self.unsafe_mint(sim, "NEW_VIEW", 1, instance, 1, "a" * 64, reports=[{}])
        with self.assertRaisesRegex(AssertionError, "CHECKER_HONEST_SLOT_EQUIVOCATION"):
            check(sim)

    def test_byzantine_wrong_domain_and_malformed_messages_cannot_count(self):
        sim = self.make(byzantine=(3,))
        instance = dict(sim.nodes[0].instance, domain="foreign-network")
        sim.authentication.adversary_sign(message("COMMIT", 3, instance, 0, "a" * 64))
        sim.authentication.adversary_sign({"sender": 3, "phase": "COMMIT"})
        summary = check(sim)
        self.assertEqual(summary["ignored_byzantine_malformed"], 2)
        self.assertEqual(summary["possible_commit_quorums"], 0)

    def test_repeated_byzantine_identity_does_not_manufacture_a_quorum(self):
        sim = self.make(byzantine=(3,))
        signed = sim.authentication.adversary_sign(
            message("COMMIT", 3, sim.nodes[0].instance, 0, "a" * 64))
        sim.authentication.issued.extend([deepcopy(signed)] * 8)
        self.assertEqual(check(sim)["possible_commit_quorums"], 0)

    def test_negative_control_authentication_audit_tampering(self):
        sim = self.settled()
        sim.authentication.issued[0]["payload"]["value"] = "f" * 64
        with self.assertRaisesRegex(AssertionError, "CHECKER_AUDIT_AUTHENTICATION"):
            check(sim)

    def test_negative_control_published_token_must_exist_and_match_sender(self):
        for mutation in ("token", "sender"):
            sim = self.settled()
            event = next(e for e in sim.trace if e["event"] == "publish")
            if mutation == "token":
                event["auth"] = "f" * 64
            else:
                event["node"] = (event["node"] + 1) % sim.n
            with self.subTest(mutation=mutation), self.assertRaisesRegex(
                    AssertionError, "CHECKER_PUBLISHED_UNAUTHENTICATED"):
                check(sim)

    def test_negative_control_durable_signature_and_intent_must_be_exact(self):
        for field, reason in (("signed", "CHECKER_SIGNATURE_NOT_DURABLE"),
                              ("intents", "CHECKER_INTENT_NOT_DURABLE")):
            sim = self.settled()
            rec = sim.nodes[0].d["records"][0]
            del rec[field][("PREPARE", 0)]
            with self.subTest(field=field), self.assertRaisesRegex(AssertionError, reason):
                check(sim)

    def test_negative_control_prepare_requires_durable_accepted_proposal(self):
        sim = self.hidden()
        sim.nodes[1].record["accepted"].clear()
        with self.assertRaisesRegex(AssertionError, "CHECKER_VOTE_WITHOUT_ACCEPTED_PROPOSAL"):
            check(sim)

    def test_negative_control_nonzero_vote_requires_exact_durable_new_view(self):
        sim = self.settled(byzantine=(0,))
        sim.nodes[1].d["records"][0]["new_views"].clear()
        with self.assertRaisesRegex(AssertionError, "CHECKER_VOTE_WITHOUT_EXACT_DURABLE_NEW_VIEW"):
            check(sim)

    def test_negative_control_commit_requires_durable_prepared_quorum(self):
        sim = self.hidden()
        sim.nodes[1].record["prepared"].clear()
        with self.assertRaisesRegex(AssertionError, "CHECKER_COMMIT_WITHOUT_DURABLE_PREPARED"):
            check(sim)

    def test_negative_control_nested_duplicate_signer_proof_rejected(self):
        sim = self.settled()
        cert = sim.nodes[1].d["records"][0]["decision"]
        cert["commits"][-1] = deepcopy(cert["commits"][0])
        with self.assertRaisesRegex(AssertionError, "CHECKER_QUORUM_IDENTITIES"):
            check(sim)

    def test_negative_control_applied_marker_requires_certificate(self):
        sim = self.settled()
        sim.nodes[1].d["records"][0]["decision"] = None
        with self.assertRaisesRegex(AssertionError, "CHECKER_APPLIED_WITHOUT_DECISION"):
            check(sim)

    def test_negative_control_application_is_exactly_once(self):
        sim = self.settled()
        sim.nodes[1].d["records"][0]["apply_count"] = 2
        with self.assertRaisesRegex(AssertionError, "CHECKER_APPLY_ONCE"):
            check(sim)

    def test_negative_control_applied_body_must_match_proof_and_root(self):
        sim = self.settled()
        sim.nodes[1].d["records"][0]["body"]["result"] = "f" * 64
        with self.assertRaisesRegex(AssertionError, "CHECKER_BODY_IDENTITY"):
            check(sim)

    def test_negative_control_live_snapshot_requires_reexecuted_prefix(self):
        sim = self.settled()
        fixture = Fixture()
        altered = sim.nodes[1].application.preview(
            {"deposits": [fixture.fact(BUYER, USD_A, 123)]})
        sim.nodes[1].d["snapshot"] = altered
        with self.assertRaisesRegex(AssertionError, "CHECKER_LIVE_STATE_NOT_APPLIED_PREFIX"):
            check(sim)

    def test_historical_records_and_parent_continuity_across_sequences(self):
        sim = self.settled()
        sim.offer()
        sim.run(100, stop=lambda s: s.settled(2))
        self.assertEqual(check(sim)["applied_records"], 8)
        sim.nodes[1].d["records"][1]["instance"]["parent"] = "f" * 64
        with self.assertRaisesRegex(AssertionError, "CHECKER_SIGNED_HISTORY_MISSING|CHECKER_PARENT_CONTINUITY"):
            check(sim)

    def test_recovery_retains_application_and_signature_history(self):
        sim = self.settled()
        for node in sim.nodes:
            node.crash()
            node.restart(suspected_rollback=True)
        self.assertEqual(check(sim)["applied_records"], 4)


if __name__ == "__main__":
    unittest.main()
