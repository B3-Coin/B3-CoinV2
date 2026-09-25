"""Narrow provenance regressions, not full RULE-FV safety/liveness tests."""
from dataclasses import replace
import itertools
import unittest

from recovery_evidence_probe import Invalid, Probe, Proposal, counterexample, permutations_checked


class RecoveryEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.p = Probe()
        self.x, self.y = self.p.proposal('x'), self.p.proposal('y')
        self.reports = (self.p.report(1, self.x), self.p.report(2), self.p.report(3))

    def test_original_interpretation_gap_is_preserved(self):
        c = counterexample()
        self.assertTrue(c['leader_selected_y_is_rejected_without_pair'])
        self.assertEqual(c['follower_allowed_from_declared_fields_only'], ['x'])
        self.assertEqual(c['additional_report_not_selected'], 'seat0:y')

    def test_explicit_original_pair_makes_same_selection_possible(self):
        self.assertEqual(self.p.choices(self.reports, (self.x, self.y)), frozenset(('x', 'y')))

    def test_pair_and_report_order_do_not_change_selection(self):
        expected = {frozenset(('x', 'y'))}
        self.assertEqual(permutations_checked(self.p, self.reports, (self.x, self.y)), expected)
        self.assertEqual(permutations_checked(self.p, self.reports, (self.y, self.x)), expected)

    def test_private_receipt_of_conflict_does_not_change_carried_only_choice(self):
        p = Probe()
        x = p.proposal('x')
        reports = (p.report(1, x), p.report(2), p.report(3))
        before = p.choices(reports)
        p.proposal('y')
        self.assertEqual(p.choices(reports), before)

    def test_asserted_equivocation_boolean_is_not_evidence(self):
        with self.assertRaises(Invalid):
            self.p.choices(self.reports, True)

    def test_one_proposal_or_same_proposal_twice_is_not_evidence(self):
        for pair in ((self.x,), (self.x, self.x)):
            with self.subTest(pair=pair), self.assertRaises(Invalid):
                self.p.choices(self.reports, pair)

    def test_unissued_pair_member_is_refused(self):
        p = Probe()
        x = p.proposal('x')
        reports = (p.report(1, x), p.report(2), p.report(3))
        invented = Proposal(p.instance, 0, 0, 'y')
        with self.assertRaises(Invalid):
            p.choices(reports, (x, invented))

    def test_different_instance_view_or_signer_is_not_equivocation_evidence(self):
        for kwargs in ({'instance': 'OTHER'}, {'view': 1}, {'sender': 2}):
            with self.subTest(kwargs=kwargs), self.assertRaises(Invalid):
                self.p.choices(self.reports, (self.x, self.p.proposal('y', **kwargs)))

    def test_duplicate_status_sender_cannot_fill_quorum(self):
        with self.assertRaises(Invalid):
            self.p.choices((self.reports[0], self.reports[0], self.reports[2]))

    def test_unissued_otherwise_valid_report_is_refused(self):
        unissued = replace(self.reports[1], vote=self.x)
        with self.assertRaises(Invalid):
            self.p.choices((self.reports[0], unissued, self.reports[2]))

    def test_wrong_target_report_is_refused(self):
        for report in (replace(self.reports[0], target=2), self.p.report(1, self.x, target=2)):
            with self.subTest(report=report), self.assertRaises(Invalid):
                self.p.choices((report, *self.reports[1:]))

    def test_old_leader_exclusion_requires_full_three_other_statuses(self):
        with self.assertRaises(Invalid):
            self.p.choices((self.p.report(0), self.reports[0], self.reports[1]), (self.x, self.y))
        with self.assertRaises(Invalid):
            self.p.choices(self.reports[:2], (self.x, self.y))

    def test_two_originals_inside_statuses_need_no_extra_pair(self):
        reports = (self.p.report(1, self.x), self.p.report(2, self.y), self.p.report(3))
        self.assertEqual(self.p.choices(reports), frozenset(('x', 'y')))

    def test_hidden_fast_quorum_forces_its_value_in_selected_honest_statuses(self):
        reports = (self.p.report(1, self.x), self.p.report(2, self.x), self.p.report(3, self.y))
        # Leader0 + seats1,2 can form hidden FAST(x), but no FAST(y).
        self.assertEqual(self.p.choices(reports), frozenset(('x',)))
        self.assertEqual(self.p.choices(reports, (self.x, self.y)), frozenset(('x',)))

    def test_all_27_honest_vote_assignments_preserve_hidden_fast_value(self):
        checked = 0
        for votes in itertools.product((None, self.x, self.y), repeat=3):
            reports = tuple(self.p.report(i + 1, vote) for i, vote in enumerate(votes))
            allowed = self.p.choices(reports, (self.x, self.y))
            self.assertEqual(permutations_checked(self.p, reports, (self.x, self.y)), {allowed})
            for value in ('x', 'y'):
                # One equivocating leader may contribute to either hidden QC;
                # every honest seat in this bounded case votes at most once.
                if 1 + sum(v is not None and v.value == value for v in votes) >= 3:
                    self.assertEqual(allowed, frozenset((value,)))
            checked += 1
        self.assertEqual(checked, 27)


if __name__ == '__main__':
    unittest.main(verbosity=2)
