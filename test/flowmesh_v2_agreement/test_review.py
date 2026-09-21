"""Minimized internal-review regressions; no private or production evidence."""
from copy import deepcopy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_application import digest, value_id
from fm_protocol import PROFILE, message
from fm_simulator import Simulator
from fm_checker import check
from test_model import Fixture


class ReviewTests(unittest.TestCase):
    def test_sparse_offer_to_nonleader_reaches_agreement_without_next_request(self):
        for n in (4, 7):
            s = Simulator(Fixture().model.snapshot(), n=n)
            original = s.offer(nodes=[1])
            s.run(180, stop=lambda sim: sim.settled())
            self.assertTrue(s.settled(), [node.last_reason for node in s.nodes])
            self.assertEqual({node.d["parent"] for node in s.nodes}, {value_id(original)})
            check(s)

    def test_local_resource_exhaustion_preserves_intent_and_halts(self):
        # Reduced bound is a labelled boundary injection, not a mainnet profile.
        with patch.dict(PROFILE["limits"], messages=0):
            s = Simulator(Fixture().model.snapshot())
            s.offer(nodes=[0])
            node = s.nodes[0]
            self.assertEqual(node.d["halt"], "AUTH_AUDIT_LIMIT")
            self.assertIn(("PROPOSE", 0), node.record["intents"])
            self.assertFalse(node.record["signed"])
            self.assertFalse(s.authentication.issued)
            node.restart()
            self.assertEqual(node.d["halt"], "AUTH_AUDIT_LIMIT")

    def test_exact_new_view_cannot_be_substituted_after_acceptance(self):
        for reverse in (False, True):
            with self.subTest(reverse=reverse):
                s = Simulator(Fixture().model.snapshot(), byzantine=(1,))
                body = s.offer()
                s.now = max(n.deadline or 0 for n in s.nodes)
                for i in (0, 2, 3):
                    s.nodes[i].tick()
                own = s.byzantine_message(message("VIEW_CHANGE", 1, body["instance"], 1,
                                                  prepared=None, decision=None))
                reports = {v["payload"]["sender"]: v for v in s.authentication.issued
                           if v["payload"]["phase"] == "VIEW_CHANGE"}
                self.assertEqual(reports[1], own)
                variants = []
                for subset in ((0, 1, 2), (0, 2, 3)):
                    variants.append(s.byzantine_message(message("NEW_VIEW", 1, body["instance"],
                                      1, value_id(body), reports=[reports[i] for i in subset])))
                first, second = variants[::-1] if reverse else variants
                node = s.nodes[2]
                node.receive("SIGNED", first, 1)
                node.pump()
                self.assertEqual(node.record["new_views"][1], first)
                proposal = s.byzantine_message(message("PROPOSE", 1, body["instance"], 1,
                                                       value_id(body), new_view=second))
                node.receive("SIGNED", proposal, 1)
                node.pump()
                self.assertNotIn(("PREPARE", 1), node.record["signed"])
                self.assertEqual(node.record["new_views"][1], first)
                self.assertEqual(node.last_reason, "CONFLICTING_NEW_VIEW")
                check(s)

    def test_forged_future_context_does_not_occupy_pending_cache(self):
        s = Simulator(Fixture().model.snapshot())
        node = s.nodes[0]
        future = dict(node.instance, sequence=1)
        for i in range(PROFILE["limits"]["inbox"] + 1):
            node.receive("SIGNED", {"payload": message("PREPARE", 1, future, 0,
                          digest("fake", i)), "auth": "not-issued"}, 1)
            node.pump()
        self.assertFalse(node.pending)
        self.assertFalse(node.d["halt"])
        self.assertEqual(node.last_reason, "AUTHENTICATION")
        self.assertFalse(s.authentication.issued)

    def test_authenticated_irrelevant_future_votes_do_not_halt_or_forget_obligations(self):
        s = Simulator(Fixture().model.snapshot(), byzantine=(3,))
        node = s.nodes[0]
        s.offer()
        obligations = deepcopy(node.record["signed"])
        future = dict(node.instance, sequence=1)
        for i in range(PROFILE["limits"]["inbox"] + 1):
            packet = s.authentication.adversary_sign(message("PREPARE", 3, future, 0,
                                                            digest("unavailable", i)))
            node.receive("SIGNED", packet, 3)
            node.pump()
        self.assertFalse(node.d["halt"])
        self.assertFalse(node.pending)
        self.assertFalse(node.requests)
        self.assertEqual(node.last_reason, "MISSING_PARENT_NOT_RETAINED")
        self.assertLessEqual(len(node._history_sent), node.n)
        self.assertEqual(node.record["signed"], obligations)
        node.restart()
        self.assertFalse(node.d["halt"])
        self.assertEqual(node.record["signed"], obligations)
        s.run(stop=lambda sim: sim.settled())
        self.assertTrue(s.settled())
        for slot, signed in obligations.items():
            self.assertEqual(node.d["records"][0]["signed"][slot], signed)
        check(s)


if __name__ == "__main__":
    unittest.main()
