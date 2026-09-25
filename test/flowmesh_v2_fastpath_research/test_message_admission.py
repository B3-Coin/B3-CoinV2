"""Regressions from separate review of d87b225; do not weaken the oracle."""
from dataclasses import replace
import unittest

from pf_model import World, Invalid
from pf_checker import check
from test_message_recovery import prepared_by_bad_leader


class MessageAdmissionTests(unittest.TestCase):
    def test_foreign_empty_cannot_enter_honest_new_view(self):
        w = World(byzantine={0})
        for node in w.nodes[1:]:
            node.body('x')
        empty = w.empty()
        bad = w.adversary('REPORT', 1, 0, items=(replace(empty, instance='OTHER-INSTANCE'), empty, empty), targets=[1])
        for node in w.nodes[1:3]:
            node.change_view(1)
        w.pump()
        self.assertNotIn(0, w.nodes[1].reports.get(1, {}))
        check(w)
        w.nodes[3].change_view(1)
        w.pump()
        self.assertTrue(all(w.nodes[i].d['applied'] == 'x' for i in range(1, 4)))
        check(w)

    def test_empty_shape_variants_are_refused(self):
        for field, value in (('view', 0), ('sender', 0), ('value', 'x'), ('items', (None,))):
            with self.subTest(field=field):
                w = World(byzantine={0})
                bad = w.adversary('REPORT', 1, 0,
                    items=(replace(w.empty(), **{field: value}), w.empty(), w.empty()), targets=[])
                with self.assertRaises(Invalid):
                    w.report(bad)

    def test_v0_wrapper_must_match_original_vote_metadata(self):
        for field, value in (('view', 1), ('sender', 1), ('value', 'y')):
            with self.subTest(field=field):
                w = World(byzantine={0})
                p = w.adversary('PREPARE', 0, 0, 'x', targets=[])
                original = w.packet('V0', 0, 0, 'x', (p, p))
                bad = w.adversary('REPORT', 1, 0,
                    items=(replace(original, **{field: value}), w.empty(), w.empty()), targets=[])
                with self.assertRaises(Invalid):
                    w.report(bad)

    def test_missing_retained_v0_cannot_authorize_empty_report(self):
        w = World(byzantine={0})
        for node in w.nodes[1:]:
            node.body('x')
        prepared_by_bad_leader(w, 'x', [1])
        w.pump(drop=lambda i, p: p.kind == 'PREPARE')
        node = w.nodes[1]
        node.crash()
        node.d['v0'] = None  # Deliberate inconsistent-store fault, not normal crash.
        node.restart()
        self.assertTrue(node.d['fenced'])
        node.change_view(1)
        self.assertNotIn(('REPORT', 1), node.d['signed'])

    def test_checker_independently_rejects_lost_v0_record(self):
        w = World(byzantine={0})
        for node in w.nodes[1:]:
            node.body('x')
        prepared_by_bad_leader(w, 'x', [1])
        w.pump(drop=lambda i, p: p.kind == 'PREPARE')
        w.nodes[1].d['v0'] = None
        with self.assertRaises(AssertionError):
            check(w)


if __name__ == '__main__':
    unittest.main()
