"""Publication-review regression: first signature needs locally held evidence."""
from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_application import NeedData
from fm_checker import check
from fm_protocol import PROFILE
from fm_simulator import Simulator
from test_model import Fixture
import test_recovery


class PublicationRecoveryTests(unittest.TestCase):
    def interrupted(self, phase, point="after_intent"):
        s = Simulator(Fixture().model.snapshot())
        helper = test_recovery.RecoveryTests()
        anchor = max(s.nodes[0].anchors.values(), key=lambda a: a["height"])
        body = s.nodes[0].application.build(s.nodes[0].instance, anchor, {})
        node = s.nodes[0 if phase == "PROPOSE" else 1]
        if phase == "PROPOSE":
            node.cut = (point, phase)
            s.offer(body=body)
        elif phase == "PREPARE":
            s.offer(body=body)
            node.cut = (point, phase)
            helper.deliver_signed(s, helper.signatures(s, "PROPOSE")[0], 1)
        elif phase == "COMMIT":
            body, qc = helper.prepared_schedule(s, body=body)
            node.cut = (point, phase)
            helper.wire(s, "PREPARED", qc, 1)
        else:
            self.assertEqual(phase, "NEW_VIEW")
            s.offer(body=body)
            helper.timeout(s, range(s.n))
            reports = helper.reports(s, 1)
            node.cut = (point, phase)
            for index in range(node.q):
                helper.deliver_signed(s, reports[index], 1)
        self.assertFalse(node.alive)
        return s, node, body, (phase, 1 if phase == "NEW_VIEW" else 0)

    def fetch_only(self, s, node, slot):
        # Deliver only the recovery GET/DATA path, not another proposal/QC.
        for _ in range(40):
            if slot in node.record["signed"]:
                return
            self.assertTrue(s.deliver_where(lambda e: e.kind == "GET" or
                (e.kind == "DATA" and e.destination == node.index
                 and e.data["type"] == "anchor")), "no evidence recovery work")
        self.fail("bounded evidence recovery did not finish original intent")

    def test_restart_unsigned_intent_waits_for_local_evidence(self):
        for phase in ("PROPOSE", "PREPARE", "COMMIT", "NEW_VIEW"):
            with self.subTest(phase=phase):
                s, node, body, slot = self.interrupted(phase)
                original = deepcopy(node.record["intents"][slot])
                node.restart()
                with self.assertRaises(NeedData):
                    node._valid_body(body)
                self.assertNotIn(slot, node.record["signed"])
                self.assertEqual(node.record["intents"][slot], original)
                self.assertTrue(any(e.kind == "GET" and e.source == node.index
                                    for e in s.events))
                self.fetch_only(s, node, slot)
                self.assertEqual(node.record["signed"][slot]["payload"], original)
                check(s)
                s.run(160, stop=lambda sim: sim.settled())
                self.assertTrue(s.settled())
                self.assertTrue(all(n.d["records"][0]["apply_count"] == 1 for n in s.nodes))
                check(s)

    def test_timeout_does_not_resume_abandoned_unsigned_intent(self):
        s, node, body, slot = self.interrupted("PREPARE")
        original = deepcopy(node.record["intents"][slot])
        node.restart()
        self.assertNotIn(slot, node.record["signed"])
        node.change_view(1)
        for obj in sorted(s.initial_anchors.values(), key=lambda a: a["height"]):
            node.receive("DATA", {"type": "anchor", "id": obj["hash"], "object": obj}, 0)
            node.pump()
        node.retry()
        self.assertNotIn(slot, node.record["signed"])
        self.assertEqual(node.record["intents"][slot], original)
        check(s)

    def test_missing_evidence_retry_preserves_intent_and_no_signature(self):
        s, node, _, slot = self.interrupted("COMMIT")
        original = deepcopy(node.record["intents"][slot])
        node.restart()
        self.assertNotIn(slot, node.record["signed"])
        before = len([e for e in s.trace if e["event"] == "defer"])
        for _ in range(2):
            s.now = node.next_retry
            node.tick()
            self.assertNotIn(slot, node.record["signed"])
            self.assertEqual(node.record["intents"][slot], original)
        self.assertGreater(len([e for e in s.trace if e["event"] == "defer"]), before)
        self.assertLess(len(s.events), PROFILE["limits"]["events"])
        check(s)

    def test_already_signed_intent_retransmits_exactly_without_new_signature(self):
        for phase in ("PREPARE", "COMMIT"):
            with self.subTest(phase=phase):
                s, node, body, slot = self.interrupted(phase, "after_record")
                original = deepcopy(node.record["signed"][slot])
                issued = len(s.authentication.issued)
                node.restart()
                with self.assertRaises(NeedData):
                    node._valid_body(body)
                self.assertEqual(len(s.authentication.issued), issued)
                self.assertEqual(node.record["signed"][slot], original)
                self.assertTrue(any(e.kind == "SIGNED" and e.source == node.index
                                    and e.data == original for e in s.events))
                check(s)


if __name__ == "__main__":
    unittest.main()
