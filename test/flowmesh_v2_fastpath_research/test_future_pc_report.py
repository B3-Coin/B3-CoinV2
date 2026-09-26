"""Native-port gate: retained PCs must precede the REPORT target, not be omitted."""
import unittest
from pf_model import World, Cut, Invalid
from pf_checker import check


class FuturePCReportTests(unittest.TestCase):
    def lagging(self, view=1):
        w = World(4)
        for node in w.nodes:
            node.body('x')
        for node in w.nodes[1:]:
            node.change_view(view)
        w.pump(drop=lambda recipient, packet: recipient == 0 and packet.kind != 'PC')
        node = w.nodes[0]
        self.assertEqual(node.d['view'], 0)
        self.assertEqual(node.d['highest'].view, view)
        self.assertIsNone(node.d['decision'])
        check(w)
        return w, node

    def test_future_pc_requires_later_report_target(self):
        w, node = self.lagging()
        node.change_view(1)
        check(w)
        self.assertNotIn(('REPORT', 1), node.d['signed'])
        report = node.d['signed'][('REPORT', 2)]
        self.assertEqual(report.items[1], node.d['highest'])
        self.assertEqual(node.d['mode'], 'CHANGING')
        self.assertNotIn(('PREPARE', 2), node.d['signed'])

    def test_future_pc_beyond_profile_bound_preserves_obligations(self):
        w, node = self.lagging(2)
        before = dict(node.d['signed'])
        highest = node.d['highest']
        node.change_view(1)
        self.assertEqual(node.d['signed'], before)
        self.assertEqual(node.d['highest'], highest)
        self.assertEqual(node.reason, 'TEST_VIEW_BOUND_REQUIRED_BY_RETAINED_PC')
        check(w)

    def test_existing_timer_uses_verified_pc_without_new_request(self):
        w, node = self.lagging()
        w.now = 3
        node.timer()
        check(w)
        self.assertEqual(node.d['view'], 2)
        self.assertEqual(node.d['signed'][('REPORT', 2)].items[1].view, 1)

    def test_report_cut_reopens_exact_valid_report(self):
        w, node = self.lagging()
        node.cut = 'after_durable_before_publish'
        with self.assertRaises(Cut):
            node.change_view(1)
        report = node.d['signed'][('REPORT', 2)]
        node.restart()
        self.assertFalse(node.d['fenced'])
        self.assertIn(report, w.auth.published)
        self.assertEqual(node.d['signed'][('REPORT', 2)], report)
        check(w)

    def test_delayed_decision_still_applies_after_target_advance(self):
        w, node = self.lagging()
        node.change_view(1)
        decision = w.nodes[1].d['decision']
        node.receive(decision)
        node.receive(decision)
        self.assertEqual(node.d['applied'], 'x')
        self.assertEqual(node.d['apply_count'], 1)
        check(w)

    def test_invalid_peer_pc_does_not_authorize_target_jump(self):
        w = World(4, byzantine={0})
        node = w.nodes[1]
        node.body('x')
        vote = w.adversary('PREPARE', 2, 0, 'x', targets=[])
        node.receive(w.packet('PC', 2, value='x', items=(vote,)))
        self.assertIsNone(node.d['highest'])
        node.change_view(1)
        self.assertEqual(node.d['view'], 1)
        check(w)


if __name__ == '__main__':
    unittest.main()
