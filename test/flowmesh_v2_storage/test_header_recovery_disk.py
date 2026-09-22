"""Header-pressure and catch-up tests with real SQLite close/reopen.

Same-process deterministic scheduling; these do not claim another process-kill
or power-loss campaign. Every identity, balance and anchor remains synthetic.
"""
from contextlib import contextmanager
from copy import deepcopy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_process_harness import DiskSimulator
from fm_application import value_id
from fm_checker import check
from fm_protocol import PROFILE, message
from test_model import Fixture
import test_header_bounds as header_cases
import test_admission_repair_disk as disk_cases
import test_recovery as recovery_cases


class HeaderRecoveryDiskTests(unittest.TestCase):
    @contextmanager
    def case(self):
        with tempfile.TemporaryDirectory(prefix="fm3a-header-repair-generated-") as directory:
            sim = DiskSimulator(Fixture().model.snapshot(), directory, byzantine=(0,))
            try:
                yield sim, Path(directory), header_cases.HeaderBoundsTests()
                check(sim)
            finally:
                sim.close()
                self.assertTrue(all(node._closed and node.store._connection is None
                                    and node.store._lock_fd is None for node in sim.nodes))

    def test_disk_100_200_300_no_tick_pressure_preserves_obligations_and_reopens(self):
        with self.case() as (sim, directory, helper):
            body, qc = helper.prepared_without_offers(sim)
            node = sim.nodes[1]
            protected = deepcopy(node.d)
            head = deepcopy(node.store.head)
            observations = []
            for start, stop in ((0, 100), (100, 200), (200, 300)):
                observations.append(helper.churn(sim, node, start, stop))
                self.assertEqual(node.d, protected)
                self.assertEqual(node.store.head, head)
                self.assertEqual(node.store.load(), protected)
            issued = deepcopy(sim.authentication.issued)
            node = disk_cases.AdmissionRepairDiskTests().reopen(sim, directory, 1)
            self.assertEqual(node.d, protected)
            self.assertEqual(node.store.load(), protected)
            self.assertEqual(node.record["highest"], qc)
            self.assertEqual(node.store.head, head)
            self.assertEqual(sim.authentication.issued, issued)
            helper.assert_bounded(node)
            helper.finish_without_new_ingress(sim, body)
            self.assertEqual(sim.authentication.issued[:len(issued)], issued)
            print({"disk_header_no_tick_campaign": observations,
                   "same_directory_reopened": True,
                   "honest_applied": [n.d["sequence"] for n in sim.nodes[1:]]})

    def test_disk_evicted_candidate_missing_body_reopens_and_recovers_exact_proof(self):
        with self.case() as (sim, directory, helper):
            old, alternate, qc = helper.split_candidate(sim)
            node = sim.nodes[1]
            helper.body_with_reference(node, alternate, "SIGNED", qc["proposal"])
            original = deepcopy(node.record["signed"][("PREPARE", 0)])
            protected = deepcopy(node.d)
            head = deepcopy(node.store.head)
            helper.churn(sim, node, 0, 300)
            self.assertNotIn((0, value_id(alternate)), node.headers)
            self.assertNotIn(value_id(alternate), node.bodies)
            helper.receive(node, "PREPARED", qc)
            self.assertIn(("body", value_id(alternate)), node.requests)
            self.assertIsNone(node.record["highest"])
            self.assertEqual(node.d, protected)
            self.assertEqual(node.store.head, head)
            node = disk_cases.AdmissionRepairDiskTests().reopen(sim, directory, 1)
            self.assertEqual(node.d, protected)
            # Ordinary peer retransmission of the exact existing QC restores
            # a disposable request lost on restart; this is not a new action.
            helper.receive(node, "PREPARED", qc)
            self.assertIn(("body", value_id(alternate)), node.requests)
            helper.body_with_reference(node, alternate, "PREPARED", qc)
            self.assertEqual(node.record["highest"], qc)
            self.assertEqual(node.record["accepted"][0]["body"], old)
            self.assertEqual(node.record["signed"][("PREPARE", 0)], original)
            self.assertNotIn(("COMMIT", 0), node.record["signed"])
            self.assertEqual(node.store.load(), node.d)
            helper.assert_bounded(node)
            helper.finish_without_new_ingress(sim, alternate)

    def test_disk_precollected_commit_votes_apply_once_when_prepared_evidence_arrives(self):
        with self.case() as (sim, _, helper):
            old, alternate, qc = helper.split_candidate(sim)
            node = sim.nodes[1]
            original = deepcopy(node.record["signed"][("PREPARE", 0)])
            for index in (2, 3):
                helper.receive(sim.nodes[index], "PREPARED", qc)
            sim.authentication.adversary_sign(message(
                "COMMIT", 0, alternate["instance"], 0, value_id(alternate)))
            schedule = recovery_cases.RecoveryTests()
            commits = schedule.by_sender(schedule.signatures(sim, "COMMIT", value=value_id(alternate)))
            helper.receive(node, "SIGNED", qc["proposal"])
            for index in (0, 2, 3):
                helper.receive(node, "SIGNED", commits[index], index)
            self.assertEqual(len(node.votes[(0, value_id(alternate), "COMMIT")]), node.q)
            self.assertEqual(node.d["sequence"], 0)
            issued = deepcopy(sim.authentication.issued)
            helper.body_with_reference(node, alternate, "PREPARED", qc)
            self.assertEqual(node.d["sequence"], 1)
            self.assertEqual(node.d["parent"], value_id(alternate))
            self.assertEqual(node.d["records"][0]["apply_count"], 1)
            self.assertEqual(node.d["records"][0]["accepted"][0]["body"], old)
            self.assertEqual(node.d["records"][0]["signed"][("PREPARE", 0)], original)
            self.assertEqual(sim.authentication.issued, issued)
            self.assertEqual(node.store.load(), node.d)
            helper.assert_bounded(node)
            helper.finish_without_new_ingress(sim, alternate)

    def test_disk_below_single_header_byte_cap_reopens_and_progresses_from_durable_evidence(self):
        with patch.dict(PROFILE["admission"], header_bytes=128):
            with self.case() as (sim, directory, helper):
                body, qc = helper.prepared_without_offers(sim)
                node = sim.nodes[1]
                protected = deepcopy(node.d)
                head = deepcopy(node.store.head)
                issued = deepcopy(sim.authentication.issued)
                self.assertFalse(node.headers)
                self.assertTrue(helper.refusals(sim, "HEADER_BYTES_PRESSURE"))
                node = disk_cases.AdmissionRepairDiskTests().reopen(sim, directory, 1)
                self.assertEqual(node.d, protected)
                self.assertEqual(node.store.load(), protected)
                self.assertEqual(node.store.head, head)
                self.assertEqual(sim.authentication.issued, issued)
                self.assertEqual(node.record["highest"], qc)
                self.assertFalse(node.headers)
                self.assertIsNotNone(node.deadline, "accepted durable work must not need a cache entry for its timer")
                helper.assert_bounded(node)
                helper.finish_without_new_ingress(sim, body)
                self.assertFalse(any(event["event"] == "header_cache_admitted" for event in sim.trace))

    def test_disk_full_protected_slot_count_reopens_and_aggregates_exact_preparation(self):
        with patch.dict(PROFILE["admission"], headers=1):
            with self.case() as (sim, directory, helper):
                old, alternate, qc = helper.split_candidate(sim)
                node = sim.nodes[1]
                original = deepcopy(node.record["signed"][("PREPARE", 0)])
                helper.body_with_reference(node, alternate, "PREPARED", qc)
                self.assertTrue(helper.refusals(sim, "PROTECTED_HEADER_CAPACITY"))
                self.assertEqual(set(node.headers), {(0, value_id(old))})
                protected = deepcopy(node.d)
                head = deepcopy(node.store.head)
                node = disk_cases.AdmissionRepairDiskTests().reopen(sim, directory, 1)
                self.assertEqual(node.d, protected)
                self.assertEqual(node.store.load(), protected)
                self.assertEqual(node.store.head, head)
                self.assertEqual(set(node.headers), {(0, value_id(old))})
                self.assertEqual(node.record["highest"], qc)
                for signed in helper.commit_quorum(sim, alternate, qc):
                    helper.receive(node, "SIGNED", signed, signed["payload"]["sender"])
                # The complete preparation is durable, but unrelated body B
                # was volatile because this node accepted A. Individual votes
                # do not manufacture a body request or authorize guessed data.
                self.assertEqual(node.d["sequence"], 0)
                self.assertEqual(node.last_reason, "VOTE_CANNOT_REQUEST_DATA")
                self.assertFalse(node.requests)
                self.assertNotIn(value_id(alternate), node.bodies)
                self.assertEqual(node.record["prepared"][(0, value_id(alternate))], qc)
                self.assertEqual(len(node.votes[(0, value_id(alternate), "COMMIT")]), node.q)
                # Normal retransmission of this exact existing proof requests
                # B; the response then evaluates the already collected votes.
                # No replacement instruction or new vote is introduced.
                issued = deepcopy(sim.authentication.issued)
                helper.receive(node, "PREPARED", qc)
                self.assertIn(("body", value_id(alternate)), node.requests)
                helper.body_with_reference(node, alternate, "PREPARED", qc)
                self.assertEqual(node.d["sequence"], 1)
                self.assertEqual(sim.authentication.issued, issued)
                self.assertEqual(node.d["records"][0]["signed"][("PREPARE", 0)], original)
                self.assertNotIn(("COMMIT", 0), node.d["records"][0]["signed"])
                self.assertEqual(node.d["records"][0]["apply_count"], 1)
                self.assertEqual(node.store.load(), node.d)
                helper.assert_bounded(node)
                helper.finish_without_new_ingress(sim, alternate)


if __name__ == "__main__":
    unittest.main()
