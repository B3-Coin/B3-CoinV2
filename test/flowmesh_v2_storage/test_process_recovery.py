"""Actual spawned-process SIGKILL/reopen cases; synthetic four-seat agreement."""
from contextlib import contextmanager
from copy import deepcopy
from pathlib import Path
import random
import signal
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_process_harness import ProcessSimulator
from fm_application import value_id
from fm_checker import check
from test_model import BUYER, USD_A, Fixture
import test_recovery as recovery_cases


class ProcessRecoveryTests(unittest.TestCase):
    @contextmanager
    def case(self, suffix=""):
        with tempfile.TemporaryDirectory(prefix="fm3a-generated-") as directory:
            sim = ProcessSimulator(Fixture().model.snapshot(), directory)
            result = {"status": "incomplete"}
            try:
                yield sim
                result = check(sim)
            finally:
                sim.close()
                self.assertTrue(all(not node.process.is_alive() for node in sim.nodes))
                sim.record_result(self.id() + suffix, result)

    def finish(self, sim, count=1):
        sim.run(220, stop=lambda s: s.settled(count))
        self.assertTrue(sim.settled(count), [(n.last_reason, n.failure) for n in sim.nodes])
        roots = {n.d["snapshot"] for n in sim.nodes}
        self.assertEqual(len(roots), 1)
        self.assertTrue(all(n.d["records"][i]["apply_count"] == 1
                            for n in sim.nodes for i in range(count)))
        check(sim)

    def assert_killed(self, sim, node, point, phase=""):
        self.assertFalse(node.process.is_alive())
        self.assertEqual(node.last_cut, (point, phase))
        self.assertEqual(sim.exits[-1]["exit"], -signal.SIGKILL)

    def body(self, sim, amount=71, anchor=None):
        f = Fixture()
        node = sim.nodes[0]
        return node.application.build(node.instance, anchor or node.d["anchor"],
                                      {"deposits": [f.fact(BUYER, USD_A, amount)]})

    def test_A_kill_before_intent_durable_no_signature_escapes(self):
        with self.case() as sim:
            node = sim.nodes[0]
            node.call("set_fault", {"stage": "before_commit", "reason": "intent:PROPOSE", "kind": "kill"})
            body = sim.offer(body=self.body(sim), nodes=[0])
            self.assert_killed(sim, node, "storage:before_commit", "intent:PROPOSE")
            self.assertFalse(sim.authentication.issued)
            node.restart()
            self.finish(sim)
            self.assertEqual(node.d["parent"], value_id(body))

    def test_A2_unsigned_durable_intent_retains_exact_candidate(self):
        with self.case() as sim:
            node = sim.nodes[0]
            node.call("set_cut", "after_intent", "PROPOSE")
            body = sim.offer(body=self.body(sim), nodes=[0])
            self.assert_killed(sim, node, "after_intent", "PROPOSE")
            original = deepcopy(node.record["intents"][("PROPOSE", 0)])
            self.assertFalse(sim.authentication.issued)
            node.restart()
            self.finish(sim)
            self.assertEqual(node.d["records"][0]["intents"][("PROPOSE", 0)], original)
            self.assertEqual(node.d["records"][0]["signed"][("PROPOSE", 0)]["payload"], original)
            self.assertEqual(node.d["parent"], value_id(body))

    def test_B_durable_signature_before_publication_retransmits_exactly(self):
        with self.case() as sim:
            node = sim.nodes[0]
            node.call("set_cut", "after_record", "PROPOSE")
            sim.offer(body=self.body(sim), nodes=[0])
            self.assert_killed(sim, node, "after_record", "PROPOSE")
            original = deepcopy(node.record["signed"][("PROPOSE", 0)])
            self.assertEqual(len(sim.authentication.issued), 1)
            self.assertFalse(sim.published())
            node.restart()
            self.assertEqual(sim.authentication.issued, [original])
            self.assertIn(original, sim.published())
            self.finish(sim)

    def test_B2_commit_before_durable_callback_restores_signature_evidence(self):
        with self.case() as sim:
            node = sim.nodes[0]
            node.call("set_fault", {"stage": "after_commit", "reason": "signature:PROPOSE", "kind": "kill"})
            sim.offer(body=self.body(sim), nodes=[0])
            self.assert_killed(sim, node, "storage:after_commit", "signature:PROPOSE")
            original = deepcopy(sim.authentication.issued[0])
            self.assertFalse(sim.published())
            node.restart()
            self.assertEqual(sim.authentication.issued, [original])
            self.assertIn(original, sim.published())
            self.assertTrue(any(e["event"] == "durable" and e.get("recovered_from_disk")
                                for e in sim.trace))
            self.finish(sim)

    def test_C_publication_without_certificate_retains_voting_obligation(self):
        with self.case() as sim:
            node = sim.nodes[0]
            node.call("set_cut", "after_publish", "PROPOSE")
            original_body = sim.offer(body=self.body(sim), nodes=[0])
            self.assert_killed(sim, node, "after_publish", "PROPOSE")
            original = deepcopy(sim.published()[0])
            self.assertIsNone(node.record["decision"])
            node.restart()
            competing = self.body(sim, 72)
            sim.offer(body=competing, nodes=[0])
            self.finish(sim)
            self.assertEqual(node.d["records"][0]["signed"][("PROPOSE", 0)], original)
            self.assertEqual(node.d["parent"], value_id(original_body))

    def test_prepare_and_commit_votes_at_durability_and_publication_boundaries(self):
        for phase in ("PREPARE", "COMMIT"):
            for boundary in ("before_intent", "after_record", "after_publish"):
                with self.subTest(phase=phase, boundary=boundary), self.case(":" + phase + ":" + boundary) as sim:
                    helper = recovery_cases.RecoveryTests()
                    node = sim.nodes[1]
                    if phase == "COMMIT":
                        body, qc = helper.prepared_schedule(sim, holders=[], body=self.body(sim))
                    else:
                        body = sim.offer(body=self.body(sim))
                        proposal = helper.signatures(sim, "PROPOSE")[0]
                    if boundary == "before_intent":
                        node.call("set_fault", {"stage": "before_commit", "reason": "intent:" + phase,
                                                "kind": "kill"})
                    else:
                        node.call("set_cut", boundary, phase)
                    if phase == "COMMIT":
                        helper.wire(sim, "PREPARED", qc, 1)
                    else:
                        helper.deliver_signed(sim, proposal, 1)
                    self.assert_killed(sim, node,
                        "storage:before_commit" if boundary == "before_intent" else boundary,
                        "intent:" + phase if boundary == "before_intent" else phase)
                    issued = [s for s in helper.signatures(sim, phase) if s["payload"]["sender"] == 1]
                    self.assertIsNone(node.record["decision"])
                    if boundary == "before_intent":
                        self.assertEqual(issued, [])
                        self.assertNotIn((phase, 0), node.record["intents"])
                    else:
                        self.assertEqual(len(issued), 1)
                        original = deepcopy(node.record["signed"][(phase, 0)])
                        self.assertEqual(issued, [original])
                        self.assertEqual(original in sim.published(), boundary == "after_publish")
                    node.restart()
                    self.finish(sim)
                    recovered = node.d["records"][0]["signed"][(phase, 0)]
                    self.assertEqual(recovered["payload"]["value"], value_id(body))
                    if boundary != "before_intent":
                        self.assertEqual(recovered, original)
                    self.assertEqual([s for s in helper.signatures(sim, phase)
                                      if s["payload"]["sender"] == 1], [recovered])
                    self.assertIn(recovered, sim.published())

    def test_D_prepared_view_change_kill_preserves_highest_and_report(self):
        with self.case() as sim:
            # Keep view-0 COMMIT delivery unavailable so the successful end
            # really requires a new view, not just a late old certificate.
            sim.policy = lambda source, target, kind, data: (
                None if kind == "SIGNED" and data["payload"]["phase"] == "COMMIT"
                and data["payload"]["view"] == 0 else 1)
            helper = recovery_cases.RecoveryTests()
            body, qc = helper.prepared_schedule(sim, holders=[1], body=self.body(sim))
            node = sim.nodes[1]
            node.call("set_cut", "after_record", "VIEW_CHANGE")
            node.change_view(1)
            self.assert_killed(sim, node, "after_record", "VIEW_CHANGE")
            report = deepcopy(node.record["signed"][("VIEW_CHANGE", 1)])
            self.assertEqual(node.record["highest"], qc)
            node.restart()
            self.assertEqual(node.record["highest"], qc)
            self.assertEqual(node.record["signed"][("VIEW_CHANGE", 1)], report)
            self.finish(sim)
            self.assertEqual(node.d["parent"], value_id(body))
            self.assertGreaterEqual(node.d["records"][0]["decision"]["prepared"]["proposal"]["payload"]["view"], 1)

    def committed_cut(self, sim, point=None, storage_stage=None):
        helper = recovery_cases.RecoveryTests()
        body, qc = helper.prepared_schedule(sim, holders=[0, 1, 2, 3], body=self.body(sim))
        cert = helper.certificate(sim, qc)
        node = sim.nodes[1]
        if storage_stage:
            node.call("set_fault", {"stage": storage_stage, "reason": "application_and_marker", "kind": "kill"})
        else:
            node.call("set_cut", point, "")
        helper.wire(sim, "CERT", cert, 1)
        self.assert_killed(sim, node, "storage:" + storage_stage if storage_stage else point,
                           "application_and_marker" if storage_stage else "")
        return node, cert, body

    def test_E_decision_before_application_recovers_once(self):
        with self.case() as sim:
            node, cert, body = self.committed_cut(sim, "after_decision")
            self.assertIsNotNone(node.record["decision"])
            self.assertFalse(node.record["applied"])
            node.restart()
            self.finish(sim)
            before = node.d["snapshot"]
            for _ in range(3):
                recovery_cases.RecoveryTests().wire(sim, "CERT", cert, 1)
            self.assertEqual(node.d["snapshot"], before)
            self.assertEqual(node.d["records"][0]["apply_count"], 1)

    def test_F_atomic_application_row_commit_and_marker_cuts(self):
        for stage in ("after_snapshot_row", "after_rows", "before_commit", "after_commit", "after_fsync"):
            with self.subTest(stage=stage), self.case(":" + stage) as sim:
                node, cert, body = self.committed_cut(sim, storage_stage=stage)
                node.restart()
                self.finish(sim)
                for replica in sim.nodes:
                    self.assertEqual(replica.d["parent"], value_id(body))
                    self.assertEqual(replica.d["records"][0]["apply_count"], 1)
                before = node.d["snapshot"]
                recovery_cases.RecoveryTests().wire(sim, "CERT", cert, 1)
                self.assertEqual(node.d["snapshot"], before)

    def test_G_after_application_kill_and_clean_same_store_reopen(self):
        with self.case() as sim:
            node, cert, body = self.committed_cut(sim, "after_application")
            self.assertEqual(node.d["sequence"], 1)
            snapshot = node.d["snapshot"]
            node.restart()
            self.assertEqual(node.d["snapshot"], snapshot)
            self.finish(sim)
            node.close()
            self.assertEqual(sim.exits[-1]["exit"], 0)
            node.restart()
            recovery_cases.RecoveryTests().wire(sim, "CERT", cert, 1)
            self.assertEqual(node.d["snapshot"], snapshot)
            self.assertEqual(node.d["records"][0]["apply_count"], 1)

    def test_missing_volatile_ancestry_defers_then_fetches_without_new_instruction(self):
        with self.case() as sim:
            node = sim.nodes[0]
            anchor = max(sim.initial_anchors.values(), key=lambda a: a["height"])
            node.call("set_cut", "after_intent", "PROPOSE")
            body = sim.offer(body=self.body(sim, anchor=anchor), nodes=[0])
            intent = deepcopy(node.record["intents"][("PROPOSE", 0)])
            node.restart()
            self.assertNotIn(("PROPOSE", 0), node.record["signed"])
            self.assertTrue(node.requests)
            self.assertFalse(sim.authentication.issued)
            for _ in range(3):
                sim.now += 1
                node.retry()
            self.assertFalse(sim.authentication.issued)
            self.assertEqual(node.record["intents"][("PROPOSE", 0)], intent)
            self.finish(sim)
            self.assertEqual(node.d["parent"], value_id(body))

    def test_flush_failure_fences_process_then_reconciles_original_signature(self):
        with self.case() as sim:
            node = sim.nodes[0]
            node.call("set_fault", {"stage": "before_fsync", "reason": "signature:PROPOSE", "kind": "error"})
            sim.offer(body=self.body(sim), nodes=[0])
            self.assertFalse(node.process.is_alive())
            self.assertEqual(sim.exits[-1]["exit"], 75)
            self.assertIsNotNone(node.failure)
            self.assertFalse(sim.published())
            original = deepcopy(sim.authentication.issued[0])
            node.restart()
            self.assertEqual(sim.authentication.issued, [original])
            self.finish(sim)

    def test_failed_signature_record_keeps_previously_issued_vote_binding(self):
        with self.case() as sim:
            node = sim.nodes[0]
            node.call("set_fault", {"stage": "before_commit", "reason": "signature:PROPOSE", "kind": "error"})
            sim.offer(body=self.body(sim), nodes=[0])
            self.assertEqual(sim.exits[-1]["exit"], 75)
            self.assertFalse(sim.published())
            original = deepcopy(sim.authentication.issued[0])
            # Only the exact unsigned intent/body survived SQL rollback. The
            # external oracle still counts the already-issued, withheld vote.
            self.assertNotIn(("PROPOSE", 0), node.record["signed"])
            node.restart()
            self.assertEqual(sim.authentication.issued, [original])
            self.assertEqual(node.record["signed"][("PROPOSE", 0)], original)
            self.finish(sim)

    def test_predetermined_schedules_replay_equal_snapshots_after_kill(self):
        snapshots = []
        for seed in (31031, 31032):
            with self.case(":" + str(seed)) as sim:
                body = self.body(sim, 91)
                sim.nodes[0].call("set_cut", "after_publish", "PROPOSE")
                sim.offer(body=body, nodes=[0])
                sim.nodes[0].restart()
                rng = random.Random(seed)
                sim.run(220, stop=lambda s: s.settled(), chooser=lambda due: rng.choice(due))
                self.assertTrue(sim.settled())
                snapshots.append(sim.nodes[0].d["snapshot"])
                self.assertEqual(len({n.d["snapshot"] for n in sim.nodes}), 1)
        self.assertEqual(*snapshots)


if __name__ == "__main__":
    unittest.main()
