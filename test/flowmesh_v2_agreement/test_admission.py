"""Bounded untrusted-data admission regressions; synthetic replicas only."""
from copy import deepcopy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_application import value_id
from fm_checker import check
from fm_protocol import PROFILE
from fm_simulator import Simulator
from test_model import Fixture


class AdmissionTests(unittest.TestCase):
    def simulator(self, n=4):
        return Simulator(Fixture().model.snapshot(), n=n, byzantine=(n - 1,))

    def deliver(self, node, body, identity=None, reference=None):
        packet = {"type": "body", "id": identity or value_id(body), "object": body}
        if reference is not None:
            packet["reference"] = reference
        node.receive("DATA", packet, node.n - 1)
        node.pump()

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


if __name__ == "__main__":
    unittest.main()
