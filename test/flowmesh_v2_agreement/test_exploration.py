"""Predeclared finite schedule spaces, not an all-schedules safety proof.

Exhaust: 8 assignments of the first Byzantine proposal to 3 honest replicas
times 6 first-delivery permutations = 48 schedules. Then deliver the opposite
proposal to each receiver and a fair recovery tail. Random campaign: eight fixed
seeds, both roster sizes, lossy/reordered/duplicated prefix + explicit fair tail.
"""
from itertools import permutations
from pathlib import Path
import random
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_application import initial_anchor, value_id
from fm_protocol import PROFILE, message
from fm_simulator import Simulator, randomized_policy
from fm_checker import check
from test_model import Fixture, BUYER, SELLER, USD_A, COIN


class ExplorationTests(unittest.TestCase):
    def test_all_split_assignments_and_first_delivery_orders(self):
        cases = 0
        for mask in range(8):
            for ordering in permutations((1, 2, 3)):
                with self.subTest(mask=mask, order=ordering):
                    f = Fixture()
                    s = Simulator(f.model.snapshot(), byzantine=(0,))
                    a = s.offer({"deposits": [f.fact(BUYER, USD_A, 5)]})
                    b = s.nodes[1].application.build(s.nodes[1].instance, initial_anchor(),
                                                    {"deposits": [f.fact(SELLER, COIN, 7)]})
                    bodies = (a, b)
                    for i in (1, 2, 3):
                        for body in bodies:
                            s.nodes[i]._store_body(body)
                    for i in ordering:
                        body = bodies[(mask >> (i - 1)) & 1]
                        signed = s.authentication.adversary_sign(message("PROPOSE", 0, s.nodes[i].instance,
                                                                         0, value_id(body), new_view=None))
                        s.send(0, "SIGNED", signed, i)
                        self.assertTrue(s.deliver_where(lambda e: e.destination == i and e.kind == "SIGNED"
                                                        and e.data["auth"] == signed["auth"]))
                        check(s)
                    # Full equivocating payload reaches everyone, but no honest
                    # phase/view slot is replaced after first acceptance.
                    for i in (1, 2, 3):
                        other = bodies[1 - ((mask >> (i - 1)) & 1)]
                        s.byzantine_message(message("PROPOSE", 0, s.nodes[i].instance,
                                                    0, value_id(other), new_view=None), [i])
                    s.run(8)
                    check(s)
                    s.run(150, stop=lambda x: x.settled())
                    self.assertTrue(s.settled(), (mask, ordering, [n.last_reason for n in s.nodes]))
                    check(s)
                    cases += 1
        self.assertEqual(cases, 48)

    def test_predeclared_random_prefixes_and_fair_recovery_tails(self):
        cases = 0
        for n in (4, 7):
            for offset, seed in enumerate(PROFILE["random_seeds"]):
                with self.subTest(n=n, seed=seed):
                    f = Fixture()
                    byzantine = (0,) if offset % 2 else ()
                    s = Simulator(f.model.snapshot(), n=n, byzantine=byzantine,
                                  policy=randomized_policy(seed, maximum_delay=8, drop_percent=15, duplicate_percent=20))
                    s.offer({"deposits": [f.fact(BUYER, USD_A, 50)]})
                    rng = random.Random(seed ^ 0x5A5A)
                    chooser = lambda due: rng.choice(due)
                    s.run(12, chooser=chooser)
                    check(s)
                    # One supported crash is added to the asynchronous prefix;
                    # no progress claim until it has returned in the fair tail.
                    victim = n - 1
                    s.nodes[victim].crash()
                    s.run(12, chooser=chooser)
                    check(s)
                    s.nodes[victim].restart()
                    s.run(12, chooser=chooser)
                    check(s)
                    s.policy = None  # explicit eventual delivery/participation
                    s.run(350, stop=lambda x: x.settled())
                    self.assertTrue(s.settled(), (n, seed, [n.last_reason for n in s.nodes]))
                    check(s)
                    s.offer({"deposits": [f.fact(SELLER, COIN, 3)]})
                    s.run(150, stop=lambda x: x.settled(2))
                    self.assertTrue(s.settled(2))
                    check(s)
                    cases += 1
        self.assertEqual(cases, 16)


if __name__ == "__main__":
    unittest.main()
