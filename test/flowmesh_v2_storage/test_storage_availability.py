"""Availability regressions through real disk stores with same-process scheduling.

These load, retry and evidence-refusal tests are NOT process-kill evidence.
The separate ProcessSimulator matrix owns that claim. No protocol predicate,
synthetic signing rule, DATA/OFFER cap or retry budget is changed here.
"""
from copy import deepcopy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "flowmesh_v2_agreement"))
sys.path.insert(0, str(HERE.parent / "flowmesh_v2_model"))

from fm_application import canonical, value_id
from fm_checker import check
from fm_codec import encode
from fm_disk_replica import DiskReplica
from fm_disk_store import DiskStore, StorageError
from fm_process_harness import DiskSimulator
from fm_protocol import PROFILE
import test_admission as admission_cases
import test_model as model_cases
import test_recovery as recovery_cases
import test_retry_bounds as retry_cases


class StorageAvailabilityTests(unittest.TestCase):
    def simulator(self, **kwargs):
        generated = tempfile.TemporaryDirectory(prefix="fm-v2-availability-")
        self.addCleanup(generated.cleanup)
        simulator = DiskSimulator(model_cases.Fixture().model.snapshot(),
                                  generated.name, **kwargs)
        self.addCleanup(simulator.close)
        return simulator

    def assert_disk_matches_memory(self, simulator):
        for node in simulator.nodes:
            self.assertEqual(encode(node.store.load()), encode(node.d))

    def assert_work_bounded(self, work):
        for field, cap in (("records_inspected", "records_per_retry"),
                           ("objects_processed", "objects_per_retry"),
                           ("messages_scheduled", "messages_per_retry"),
                           ("encoded_bytes", "payload_bytes_per_retry"),
                           ("scheduled_payload_bytes", "payload_bytes_per_retry")):
            self.assertLessEqual(work[field], PROFILE["delivery"][cap], (field, work))

    def settle(self, simulator, sequence=1, nodes=None):
        simulator.run(400, stop=lambda s: s.settled(sequence, nodes=nodes))
        self.assertTrue(simulator.settled(sequence, nodes=nodes),
                        [node.last_reason for node in simulator.nodes])
        self.assertFalse(simulator.exhausted)

    def test_257_correct_hash_unsolicited_bodies_make_zero_durable_writes(self):
        for n in (4, 7):
            with self.subTest(n=n):
                sim = self.simulator(n=n, byzantine=(n - 1,))
                node = sim.nodes[0]
                original, head = deepcopy(node.d), deepcopy(node.store.head)
                with patch.object(node.store, "commit", wraps=node.store.commit) as commits:
                    for index in range(257):
                        junk = {"unrequested_invalid_body": index}
                        node.receive("DATA", {"type": "body", "id": value_id(junk),
                                              "object": junk}, n - 1)
                        node.pump()
                    # This restart resets volatile queues in the same process;
                    # it is explicitly not a process exit or a database reopen.
                    node.restart()
                    self.assertEqual(commits.call_count, 0)
                self.assertEqual(node.store.head, head)
                self.assertEqual(encode(node.store.load()), encode(original))
                self.assertEqual(node.d, original)
                self.assertFalse(node.bodies or node.requests or node.references or node.pending)
                self.assertFalse(node.d["halt"])
                sim.offer(nodes=[1])
                self.settle(sim)
                self.assert_disk_matches_memory(sim)
                check(sim)

    def test_256_remote_offers_stay_disposable_and_next_local_request_completes(self):
        sim = self.simulator(byzantine=(3,))
        node = sim.nodes[1]  # nonleader now; proposer for the next sequence
        original, head = deepcopy(node.d), deepcopy(node.store.head)
        observed_ids = set()
        helper = recovery_cases.RecoveryTests()
        with patch.object(node.store, "commit", wraps=node.store.commit) as commits:
            for amount in range(256):
                body = helper.body(sim, 1000 + amount)
                observed_ids.add(value_id(body))
                node.receive("OFFER", body, 3)
                node.pump()
                self.assertLessEqual(len(node.offers), PROFILE["admission"]["remote_offers"])
                self.assertLessEqual(len(node.bodies), PROFILE["limits"]["objects"])
            self.assertEqual(commits.call_count, 0)
        self.assertEqual(len(observed_ids), 256)
        self.assertTrue(node.bodies)
        self.assertEqual(node.store.head, head)
        self.assertEqual(encode(node.store.load()), encode(original))
        self.assertEqual(node.d, original)
        self.assertFalse(node.d["retained_bodies"])
        self.assertEqual(node.last_reason, "REMOTE_OFFER_PRESSURE")
        sim.offer(nodes=[0])
        self.settle(sim)
        legitimate = sim.offer(nodes=[1])
        self.assertIn(value_id(legitimate), node.d["retained_bodies"])
        self.assertFalse(observed_ids & set(node.d["retained_bodies"]))
        self.assertFalse(node.d["halt"])
        self.settle(sim, 2)
        self.assert_disk_matches_memory(sim)
        check(sim)

    def test_retry_work_at_1_4_16_31_decisions_is_bounded_and_performs_no_disk_writes(self):
        sim = self.simulator()
        measurements = []
        for sequence in range(31):
            sim.offer()
            self.settle(sim, sequence + 1)
            sim.events.clear()
            if sequence + 1 not in (1, 4, 16, 31):
                continue
            node, sent = sim.nodes[0], []
            records, emit = node.d["records"], node.emit
            head = deepcopy(node.store.head)
            issued = len(sim.authentication.issued)
            # The inherited assertion wrapper fails on any history iteration.
            node.d["records"] = retry_cases.IndexedHistory(records)
            node.emit = lambda source, kind, data, destination: sent.append((kind, data, destination))
            node.history_requests[3] = 0
            sim.now += 1
            try:
                with patch.object(node.store, "commit", wraps=node.store.commit) as commits:
                    node.retry()
                    self.assertEqual(commits.call_count, 0)
            finally:
                node.d["records"], node.emit = records, emit
            work = deepcopy(node.retry_work)
            self.assert_work_bounded(work)
            self.assertEqual(work["records_inspected"], 3)
            self.assertEqual(work["messages_scheduled"],
                             sum(sim.n if destination is None else 1
                                 for _, _, destination in sent))
            self.assertEqual(work["encoded_bytes"],
                             sum(len(canonical(data)) for _, data, _ in sent))
            self.assertEqual(work["scheduled_payload_bytes"],
                             sum(len(canonical(data)) * (sim.n if destination is None else 1)
                                 for _, data, destination in sent))
            self.assertEqual(node.store.head, head)
            self.assertEqual(len(sim.authentication.issued), issued)
            self.assertEqual(sum(record["applied"] for record in records.values()), sequence + 1)
            self.assertTrue(all(record["apply_count"] == 1 for record in records.values()
                                if record["applied"]))
            measurements.append(work)
        self.assertEqual(len(measurements), 4)
        self.assertEqual(len({work["objects_processed"] for work in measurements}), 1)
        self.assertEqual(len({work["messages_scheduled"] for work in measurements}), 1)
        self.assert_disk_matches_memory(sim)
        check(sim)

    def test_laggard_fetches_eight_decisions_after_same_process_restart_exactly_once(self):
        sim = self.simulator(policy=lambda source, target, kind, data:
                             None if source == 3 or target == 3 else 1)
        for sequence in range(8):
            sim.offer(nodes=[0, 1, 2])
            self.settle(sim, sequence + 1, nodes=[0, 1, 2])
            sim.events.clear()
        laggard = sim.nodes[3]
        self.assertEqual(laggard.d["sequence"], 0)
        self.assertFalse(laggard.record["signed"])
        untouched_head = deepcopy(laggard.store.head)
        sim.policy = None
        laggard.crash()
        laggard.restart()  # same interpreter and open store; no process-kill claim
        self.assertEqual(laggard.store.head, untouched_head)
        self.settle(sim, 8)
        self.assertEqual(laggard.d["parent"], sim.nodes[0].d["parent"])
        self.assertTrue(any(event["event"] == "delivered" and event["kind"] == "GET_CERT"
                            for event in sim.trace))
        self.assertEqual(sum(record["applied"] for record in laggard.d["records"].values()), 8)
        self.assertTrue(all(record["apply_count"] == 1
                            for record in laggard.d["records"].values() if record["applied"]))
        after, issued = deepcopy(laggard.store.head), len(sim.authentication.issued)
        with patch.object(laggard.store, "commit", wraps=laggard.store.commit) as commits:
            for sequence in range(8):
                certificate = sim.nodes[0].d["records"][sequence]["decision"]
                for _ in range(3):
                    laggard.receive("CERT", deepcopy(certificate), 0)
                    laggard.pump()
            self.assertEqual(commits.call_count, 0)
        self.assertEqual(laggard.store.head, after)
        self.assertEqual(len(sim.authentication.issued), issued)
        self.assert_disk_matches_memory(sim)
        check(sim)

    def test_reopen_refuses_missing_or_corrupt_protected_evidence_without_signing(self):
        for corruption in ("missing-accepted", "forged-accepted"):
            with self.subTest(corruption=corruption):
                sim = self.simulator()
                body = sim.offer(nodes=[0])
                node = sim.nodes[1]
                admission_cases.AdmissionTests().inline(sim, node, body)
                self.assertIn(("PREPARE", 0), node.record["signed"])
                directory, trusted = node.store.directory, deepcopy(node.store.head)
                identity = {"config": node.config, "index": node.index, "n": node.n,
                            "genesis": node.genesis}
                node.close()
                with DiskStore(directory, identity, trusted_head=trusted) as store:
                    old = store.load()
                    invalid = deepcopy(old)
                    if corruption == "missing-accepted":
                        invalid["records"][0]["accepted"].clear()
                    else:
                        invalid["records"][0]["accepted"][0]["signed"]["auth"] = "generated-invalid-auth"
                    # A coherent low-level journal cannot authenticate protocol
                    # evidence. The replica must reject this upon reopening.
                    store.commit(old, invalid, "GENERATED_INVALID_RECOVERY_EVIDENCE")
                    observed = deepcopy(store.head)
                signatures, queued = deepcopy(sim.authentication.issued), len(sim.events)
                with self.assertRaises(StorageError) as caught:
                    DiskReplica(node.index, sim.n, sim.authentication.signer(node.index),
                                sim.authentication.verify, sim.initial_snapshot, sim.initial_anchor,
                                sim.send, lambda: sim.now, sim.record, directory=directory,
                                trusted_head=observed)
                self.assertEqual(caught.exception.code, "RECOVERY_EVIDENCE_INVALID")
                self.assertEqual(sim.authentication.issued, signatures)
                self.assertEqual(len(sim.events), queued)
                with DiskStore(directory, identity, trusted_head=observed) as preserved:
                    self.assertEqual(encode(preserved.load()), encode(invalid))
                    self.assertEqual(preserved.head, observed)


if __name__ == "__main__":
    unittest.main()
