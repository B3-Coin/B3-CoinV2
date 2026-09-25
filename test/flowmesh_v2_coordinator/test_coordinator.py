"""Bounded scheduling/placement checks, NOT native performance qualification."""
from copy import deepcopy
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from experiment import (profile, fixture, trade, campaign, placement_effect,
                        finish_batch, verify_applied, burst_fixture)
from fm_application import value_id, canonical
from fm_checker import check, _Audit
from fm_protocol import PROFILE, Proofs, Invalid, message, proposer
from fm_simulator import Simulator, randomized_policy
from fm_process_harness import DiskSimulator
from fm_disk_replica import DiskReplica
from fm_disk_store import StorageError
from model import Model, action_id
from test_model import BUYER, SELLER, COIN, USD_A


class CoordinatorTests(unittest.TestCase):
    def fresh(self, **kwargs):
        f = fixture()
        return f, Simulator(f.model.snapshot(), **kwargs)

    def body(self, f, sim, sequence=0, node=0):
        n = sim.nodes[node]
        return n.application.build(n.instance, n.d["anchor"], trade(f, sequence))

    def test_original_profile_is_unchanged_and_restored(self):
        before = deepcopy(PROFILE)
        self.assertNotIn("coordinator_experiment", before)
        self.assertEqual(proposer({"sequence": 5}, 2, 4), 3)
        with profile(4):
            self.assertEqual(proposer({"sequence": 5}, 2, 4), 3)
            self.assertEqual(proposer({"sequence": 2}, 0, 4), 0)
        self.assertEqual(PROFILE, before)

    def test_exact_declared_schedules_and_bounded_handoff(self):
        for span, expected in ((1, [0, 1, 2, 3, 0, 1, 2, 3]),
                               (4, [0, 0, 0, 0, 1, 1, 1, 1])):
            with profile(span):
                self.assertEqual([proposer({"sequence": i}, 0, 4) for i in range(8)], expected)
                self.assertEqual([proposer({"sequence": 1}, v, 4) for v in range(4)],
                                 [(expected[1] + v) % 4 for v in range(4)])

    def test_invalid_or_overlapping_profile_selection_refuses(self):
        for invalid in (True, 0, 2, -1, "4"):
            with self.assertRaises(ValueError), profile(invalid):
                self.fail("invalid profile entered")
        with profile(4):
            with self.assertRaises(ValueError), profile(1):
                self.fail("mixed live profiles entered")
            for bad in ({"version": "bounded-tenure/1", "batches_per_coordinator": True},
                        {"version": "bounded-tenure/2", "batches_per_coordinator": 4}):
                with patch.dict(PROFILE, coordinator_experiment=bad):
                    with self.assertRaisesRegex(Invalid, "COORDINATOR_TEST_PROFILE"):
                        proposer({"sequence": 0}, 0, 4)
                    with self.assertRaisesRegex(AssertionError, "COORDINATOR_TEST_PROFILE"):
                        check(Simulator(fixture().model.snapshot()))

    def test_schedule_is_bound_into_instance_and_old_profile_message_rejects(self):
        with profile(1):
            f, sim = self.fresh()
            old_instance = sim.nodes[0].instance
        with profile(4):
            f, sim = self.fresh()
            self.assertNotEqual(old_instance["config"], sim.nodes[0].instance["config"])
            # Authenticate the foreign-context payload to isolate context checks
            # from the simulator's separate authentication oracle.
            signed = sim.authentication.signer(0)(message(
                "PROPOSE", 0, old_instance, 0, "a" * 64, new_view=None))
            with self.assertRaisesRegex(Invalid, "PHASE_OR_INSTANCE"):
                sim.nodes[0].proofs.check("proposal", signed, sim.nodes[0].instance)

    def test_independent_checker_rejects_wrong_scheduled_proposer(self):
        with profile(4):
            f, sim = self.fresh()
            row = finish_batch(sim, self.body(f, sim), list(range(4)))
            self.assertTrue(row["completed"])
            body = self.body(f, sim, 1)
            # At sequence 1 the candidate schedule still requires seat 0.
            forged = sim.authentication.signer(1)(message(
                "PROPOSE", 1, body["instance"], 0, value_id(body), new_view=None))
            audit = _Audit(sim)
            audit.tokens = {s["auth"]: s for s in sim.authentication.issued}
            # Even if the replica helper is wrong, the independent arithmetic
            # still rejects it; no history pruning or checker shortcut.
            with patch("fm_protocol.proposer", return_value=1):
                self.assertEqual(Proofs(4, sim.authentication.verify).check(
                    "proposal", forged, body["instance"])["sender"], 1)
                with self.assertRaisesRegex(AssertionError, "CHECKER_PROPOSER"):
                    audit.proof("proposal", forged, body["instance"])

    def test_healthy_policy_alone_does_not_reduce_agreement_steps(self):
        rows = [campaign(span, batches=5) for span in (1, 4)]
        self.assertEqual(rows[0]["final_accounting_digest"], rows[1]["final_accounting_digest"])
        for result in rows:
            self.assertEqual([r["all_required_applied_ticks"] for r in result["samples"]], [3] * 5)
            self.assertTrue(all(r["issued_phases"] == {"PROPOSE": 1, "PREPARE": 4, "COMMIT": 4}
                                for r in result["samples"]))
        self.assertEqual([r["deciding_proposer"] for r in rows[1]["samples"]], [0, 0, 0, 0, 1])

    def test_unavailable_tenure_leader_repeats_wait_instead_of_private_skip(self):
        control = campaign(1, unavailable=(0,), batches=4)
        candidate = campaign(4, unavailable=(0,), batches=4)
        self.assertEqual(control["final_accounting_digest"], candidate["final_accounting_digest"])
        self.assertEqual([r["deciding_view"] for r in control["samples"]], [1, 0, 0, 0])
        self.assertEqual([r["deciding_view"] for r in candidate["samples"]], [1, 1, 1, 1])
        self.assertGreater(sum(r["all_required_applied_ticks"] for r in candidate["samples"]),
                           sum(r["all_required_applied_ticks"] for r in control["samples"]))

    def test_eight_original_requests_one_batch_match_and_preserve_exact_instructions(self):
        for span in (1, 4):
            with profile(span):
                f, batch = burst_fixture()
                sim = Simulator(f.model.snapshot())
                n = sim.nodes[0]
                body = n.application.build(n.instance, n.d["anchor"], batch)
                row = finish_batch(sim, body, list(range(4)))
                self.assertTrue(row["completed"])
                model = Model.restore(n.d["snapshot"])
                self.assertEqual(len(row["original_action_ids"]), 8)
                self.assertEqual(len(model.state["outcomes"]), 8)
                self.assertTrue(all(o["status"] == "ACCEPTED" for o in model.state["outcomes"].values()))
                for a in batch["actions"]:
                    key = action_id(a)
                    self.assertEqual(json.loads(model.state["instructions"][key]),
                                     {k: v for k, v in a.items() if k != "authorized"})
                buys = [a["account"] for a in batch["actions"] if a["params"]["side"] == "BUY"]
                self.assertEqual(sum(model._get("spot", who, COIN) for who in buys), 4)
                model.assert_invariants()

    def test_duplicate_certificates_and_exact_actions_do_not_reexecute(self):
        with profile(4):
            f, sim = self.fresh()
            body = self.body(f, sim)
            self.assertTrue(finish_batch(sim, body, list(range(4)))["completed"])
            original = deepcopy(sim.nodes[0].d["records"][0])
            for n in sim.nodes:
                n.receive("CERT", original["decision"], 0)
                n.pump()
                self.assertEqual(n.d["records"][0]["apply_count"], 1)
            before = Model.restore(sim.nodes[0].d["snapshot"])
            n = sim.nodes[0]
            repeated = n.application.build(n.instance, n.d["anchor"], body["batch"])
            self.assertTrue(finish_batch(sim, repeated, list(range(4)))["completed"])
            after = Model.restore(n.d["snapshot"])
            self.assertEqual(before.state, after.state)
            check(sim)

    def test_nonleader_sparse_ingress_recovers_without_second_offer(self):
        with profile(4):
            f, sim = self.fresh()
            body = self.body(f, sim, node=3)
            sim.offer(body=body, nodes=[3])
            sim.run(120, stop=lambda s: s.settled())
            self.assertTrue(sim.settled())
            self.assertTrue(all(verify_applied(sim, i, 0, body) for i in range(4)))
            check(sim)

    def test_coordinator_equivocation_recovers_with_preserved_votes(self):
        with profile(4):
            f, sim = self.fresh(byzantine=(0,))
            a = self.body(f, sim, node=1)
            b = sim.nodes[1].application.build(sim.nodes[1].instance, sim.nodes[1].d["anchor"], {})
            for i, body in ((1, a), (2, b), (3, b)):
                sim.nodes[i].offer(body)
                sim.byzantine_message(message("PROPOSE", 0, body["instance"], 0,
                                              value_id(body), new_view=None), [i])
            sim.run(8)
            old = deepcopy(sim.authentication.issued)
            self.assertEqual(len({s["payload"]["value"] for s in sim.published("PREPARE", 0)}), 2)
            sim.run(180, stop=lambda s: s.settled())
            self.assertTrue(sim.settled())
            self.assertEqual(sim.authentication.issued[:len(old)], old)
            self.assertEqual(len({sim.nodes[i].d["parent"] for i in (1, 2, 3)}), 1)
            check(sim)

    def test_lost_body_and_certificate_recover_through_existing_requests(self):
        with profile(4):
            f, sim = self.fresh(policy=lambda s, d, k, obj: None if d == 3 else 1)
            body = self.body(f, sim)
            self.assertTrue(finish_batch(sim, body, [0, 1, 2])["completed"])
            self.assertEqual(sim.nodes[3].d["sequence"], 0)
            sim.policy = None
            sim.run(150, stop=lambda s: s.settled())
            self.assertTrue(sim.settled())
            self.assertTrue(verify_applied(sim, 3, 0, body))
            check(sim)

    def test_restart_pending_obligations_keeps_same_signed_objects(self):
        with profile(4):
            f, sim = self.fresh(policy=lambda s, d, k, obj: None if k == "SIGNED"
                                 and obj["payload"]["phase"] == "COMMIT" else 1)
            body = self.body(f, sim)
            sim.offer(body=body)
            sim.run(8)
            old = deepcopy(sim.authentication.issued)
            self.assertFalse(sim.settled())
            self.assertTrue(sim.published("COMMIT"))
            for n in sim.nodes:
                n.crash()
                n.restart()
            sim.policy = None
            sim.run(150, stop=lambda s: s.settled())
            self.assertTrue(sim.settled())
            self.assertEqual(sim.authentication.issued[:len(old)], old)
            check(sim)

    def test_partition_heals_without_new_request_or_cleared_obligation(self):
        with profile(4):
            f, sim = self.fresh()
            sim.partitions = ({0, 1}, {2, 3})
            body = self.body(f, sim)
            sim.offer(body=body)
            sim.run(65)
            self.assertFalse(sim.settled())
            before = deepcopy(sim.authentication.issued)
            sim.partitions = None
            sim.run(180, stop=lambda s: s.settled())
            self.assertTrue(sim.settled())
            self.assertEqual(sim.authentication.issued[:len(before)], before)
            check(sim)

    def test_host_placement_does_not_create_votes_or_change_quorum(self):
        self.assertEqual(placement_effect(["poolA", "poolA", "self2", "poolB"], {"poolA"}), (0, 1))
        self.assertEqual(placement_effect(["A", "B", "C", "D"], {"A"}), (0,))
        self.assertEqual(placement_effect(["A"] * 4, {"A"}), (0, 1, 2, 3))
        with self.assertRaises(ValueError):
            placement_effect(["A"], {"A"})

    def test_one_independent_host_failure_progress_two_seats_failure_stops(self):
        live = campaign(4, unavailable=placement_effect(["A", "B", "C", "D"], {"D"}), batches=1)
        blocked = campaign(4, unavailable=placement_effect(["self0", "self1", "pool", "pool"], {"pool"}), batches=1)
        self.assertTrue(live["samples"][0]["completed"])
        self.assertFalse(blocked["samples"][0]["completed"])
        self.assertEqual(blocked["samples"][0]["checker"]["possible_commit_quorums"], 0)

    def test_one_host_all_signers_is_one_failure_domain(self):
        with profile(4):
            f, sim = self.fresh()
            for i in placement_effect(["A"] * 4, {"A"}):
                sim.nodes[i].crash()
            sim.offer(body=self.body(f, sim))
            sim.run(40)
            self.assertTrue(all(n.d["sequence"] == 0 for n in sim.nodes))
            self.assertEqual(check(sim)["possible_commit_quorums"], 0)

    def test_seven_seats_and_predetermined_loss_schedules(self):
        with profile(4):
            for seed in (45312, 45313):
                f, sim = self.fresh(n=7, policy=randomized_policy(seed))
                body = self.body(f, sim)
                sim.offer(body=body, nodes=[6])
                sim.run(180, stop=lambda s: s.settled())
                self.assertTrue(sim.settled(), (seed, [n.last_reason for n in sim.nodes]))
                check(sim)

    def test_sqlite_reopen_nonzero_sequence_preserves_selection_and_exactly_once(self):
        with profile(4), tempfile.TemporaryDirectory(prefix="fm-coordinator-generated-") as directory:
            f = fixture()
            sim = DiskSimulator(f.model.snapshot(), directory)
            try:
                body = self.body(f, sim)
                self.assertTrue(finish_batch(sim, body, list(range(4)))["completed"])
                # Sequence 1 differentiates the candidate from original rotation.
                n = sim.nodes[0]
                sim.policy = lambda s, d, k, obj: None if k == "SIGNED" and obj["payload"]["phase"] == "COMMIT" else 1
                next_body = self.body(f, sim, 1)
                sim.offer(body=next_body)
                sim.run(8)
                self.assertEqual(n.d["sequence"], 1)
                self.assertIn(("COMMIT", 0), n.record["signed"])
                saved = deepcopy(n.d)
                head = deepcopy(n.store.head)
                with self.assertRaises(StorageError):
                    DiskReplica(0, 4, sim.authentication.signer(0), sim.authentication.verify,
                                sim.initial_snapshot, sim.initial_anchor, sim.send,
                                lambda: sim.now, sim.record, directory=Path(directory) / "0")
                n.close()
                reopened = DiskReplica(0, 4, sim.authentication.signer(0), sim.authentication.verify,
                                       sim.initial_snapshot, sim.initial_anchor, sim.send,
                                       lambda: sim.now, sim.record, directory=Path(directory) / "0",
                                       trusted_head=head)
                sim.nodes[0] = reopened
                self.assertEqual(reopened.d, saved)
                reopened.restart()
                sim.policy = None
                sim.run(150, stop=lambda s: s.settled(2))
                self.assertTrue(sim.settled(2))
                self.assertEqual(reopened.d["records"][1]["apply_count"], 1)
                self.assertEqual(reopened.d["records"][1]["signed"][("COMMIT", 0)], saved["records"][1]["signed"][("COMMIT", 0)])
                self.assertEqual(reopened.store.load(), reopened.d)
                check(sim)
            finally:
                sim.close()
                self.assertTrue(all(n._closed and n.store._lock_fd is None for n in sim.nodes))

    def test_sqlite_different_profile_cannot_reinterpret_saved_signing_history(self):
        with tempfile.TemporaryDirectory(prefix="fm-coordinator-domain-generated-") as directory:
            with profile(4):
                f = fixture()
                sim = DiskSimulator(f.model.snapshot(), directory)
                try:
                    self.assertTrue(finish_batch(sim, self.body(f, sim), list(range(4)))["completed"])
                    saved = deepcopy(sim.nodes[0].d)
                    head = deepcopy(sim.nodes[0].store.head)
                finally:
                    sim.close()
            with profile(1):
                other = Simulator(f.model.snapshot())
                with self.assertRaises(StorageError):
                    DiskReplica(0, 4, other.authentication.signer(0), other.authentication.verify,
                                other.initial_snapshot, other.initial_anchor, other.send,
                                lambda: other.now, other.record, directory=Path(directory) / "0")
            with profile(4):
                node = DiskReplica(0, 4, sim.authentication.signer(0), sim.authentication.verify,
                                   sim.initial_snapshot, sim.initial_anchor, sim.send,
                                   lambda: sim.now, sim.record, directory=Path(directory) / "0",
                                   trusted_head=head)
                try:
                    self.assertEqual(node.d, saved)
                    self.assertEqual(node.store.head, head)
                finally:
                    node.close()


if __name__ == "__main__":
    unittest.main()
