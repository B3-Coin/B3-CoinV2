"""Deterministic bounded-delivery and historical catch-up regressions."""
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_simulator import Simulator
from test_model import Fixture


class RetryBoundsTests(unittest.TestCase):
    def history(self, count, n=4, nodes=None, policy=None):
        sim = Simulator(Fixture().model.snapshot(), n=n, policy=policy)
        for sequence in range(count):
            sim.offer(nodes=nodes)
            sim.run(180, stop=lambda s: s.settled(sequence + 1, nodes=nodes))
            self.assertTrue(sim.settled(sequence + 1, nodes=nodes))
            # The scheduler may lose already queued retransmissions. Durable
            # history must remain recoverable after this finite loss.
            sim.events.clear()
        return sim

    def test_retry_does_not_rebroadcast_all_retained_decisions(self):
        sim = self.history(31)
        node = sim.nodes[0]
        node.retry()
        certificates = [event for event in sim.events
                        if event.source == node.index and event.kind == "CERT"
                        and event.destination == node.index]
        self.assertLessEqual(len(certificates), 2,
                             "one retry rebroadcasts every historical decision")
        self.assertEqual(sum(rec["applied"] for rec in node.d["records"].values()), 31)


if __name__ == "__main__":
    unittest.main()
