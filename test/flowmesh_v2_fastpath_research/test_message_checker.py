"""Negative controls for the independent oracle, not replica mutations."""
import unittest
from pf_model import World
from pf_checker import check


class MessageCheckerTests(unittest.TestCase):
    def slow(self):
        w = World(7, byzantine={5, 6})
        w.submit()
        w.pump()
        check(w)
        return w

    def corrupt_guard(self, field, value):
        w = self.slow()
        for record in w.publications:
            if record['packet'].kind == 'COMMIT':
                record['durable']['guards'][record['packet']][field] = value
                break
        else:
            self.fail('fixture emitted no COMMIT')
        with self.assertRaises(AssertionError):
            check(w)

    def test_commit_requires_proposal(self):
        self.corrupt_guard('proposal', None)

    def test_commit_requires_body(self):
        self.corrupt_guard('body_available', False)

    def test_commit_requires_current_view_at_signing(self):
        self.corrupt_guard('pre_view', 1)

    def test_commit_requires_pc(self):
        self.corrupt_guard('prepared', None)

    def test_commit_requires_active_mode(self):
        self.corrupt_guard('mode', 'CHANGING')

    def test_application_count_cannot_regress(self):
        w = self.slow()
        w.nodes[1].d['apply_count'] = 0
        w.nodes[1].d['applied'] = None
        with self.assertRaises(AssertionError):
            check(w)

    def test_view_cannot_regress(self):
        w = World(byzantine={0})
        w.submit(via=3)
        w.pump()
        w.tick(3)
        w.pump()
        check(w)
        w.nodes[1].d['view'] = 0
        with self.assertRaises(AssertionError):
            check(w)

    def test_new_view_cannot_ignore_already_known_decision(self):
        w = World(byzantine={0})
        w.submit(via=3)
        w.pump()
        w.tick(3)
        w.pump()
        for record in w.publications:
            if record['packet'].kind == 'NEW_VIEW':
                record['durable']['guards'][record['packet']]['decision'] = w.nodes[1].d['decision']
                break
        else:
            self.fail('fixture emitted no NEW_VIEW')
        with self.assertRaises(AssertionError):
            check(w)


if __name__ == '__main__':
    unittest.main()
