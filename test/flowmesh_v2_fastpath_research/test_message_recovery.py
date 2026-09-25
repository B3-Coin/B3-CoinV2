"""Finite deterministic campaigns, not an exhaustive protocol proof."""
from copy import deepcopy
from dataclasses import replace
import random
import unittest

from pf_model import World, Cut, Invalid
from pf_checker import check


def prepared_by_bad_leader(w, value, targets):
    leader = w.adversary('PREPARE', 0, 0, value, targets=[])
    proposal = w.packet('PROPOSE', 0, 0, value, (leader, w.empty()))
    w.enqueue(proposal, targets)
    return leader, proposal


def signed(w, kind, view, value):
    return tuple(sorted((p for p in w.auth.published if p.kind == kind and
                         p.view == view and p.value == value), key=lambda p: p.sender))


class MessageRecoveryTests(unittest.TestCase):
    def settled(self, w, value='x', seats=None):
        check(w)
        for i in (set(range(w.n)) - w.byzantine if seats is None else seats):
            self.assertEqual(w.nodes[i].d['applied'], value)
            self.assertEqual(w.nodes[i].d['apply_count'], 1)

    def test_healthy_fast_path_fixed_sizes(self):
        for n in (4, 5, 7):
            with self.subTest(n=n):
                w = World(n)
                w.submit(via=n - 1)
                w.pump()
                self.settled(w)
                self.assertTrue(all(p.d['decision'].kind == 'FAST' for p in w.nodes))

    def test_leader_proposal_is_one_prepare_not_an_extra_vote(self):
        w = World()
        w.submit()
        w.pump()
        self.assertEqual(len(signed(w, 'PREPARE', 0, 'x')), 4)
        self.assertEqual(sum(p.sender == 0 for p in signed(w, 'PREPARE', 0, 'x')), 1)
        self.settled(w)

    def test_single_offline_replica_fast_path(self):
        for n in (4, 5, 7):
            with self.subTest(n=n):
                w = World(n)
                w.nodes[-1].crash()
                w.submit()
                w.pump()
                self.settled(w, seats=range(n - 1))
                w.nodes[-1].restart()
                w.tick()
                w.pump()
                self.settled(w)

    def test_n7_two_unavailable_falls_back_to_view0_slow(self):
        w = World(7, byzantine={5, 6})
        w.submit()
        w.pump()
        self.settled(w)
        self.assertTrue(all(w.nodes[i].d['decision'].kind == 'SLOW' for i in range(5)))

    def test_failed_leader_changes_view_without_new_request(self):
        for n in (4, 5, 7):
            with self.subTest(n=n):
                w = World(n, byzantine={0})
                w.submit(via=n - 1)
                w.pump()
                self.assertFalse(any(p.d['applied'] for p in w.nodes))
                w.tick(3)
                w.pump()
                self.settled(w)
                self.assertTrue(all(w.nodes[i].d['decision'].view == 1 for i in range(1, n)))

    def test_two_failed_leaders_n7(self):
        w = World(7, byzantine={0, 1})
        w.submit(via=6)
        w.pump()
        for _ in range(2):
            w.tick(3)
            w.pump()
        self.settled(w)
        self.assertTrue(all(w.nodes[i].d['decision'].view == 2 for i in range(2, 7)))

    def test_equivocation_with_two_conflicting_acceptances_recovers(self):
        w = World(byzantine={0})
        for node in w.nodes:
            node.body('x')
            node.body('y')
        prepared_by_bad_leader(w, 'x', [1])
        prepared_by_bad_leader(w, 'y', [2])
        w.pump()
        self.assertEqual(w.nodes[1].d['accepted'][0].value, 'x')
        self.assertEqual(w.nodes[2].d['accepted'][0].value, 'y')
        w.tick(3)
        w.pump()
        self.settled(w)
        nv = w.nodes[1].d['new_views'][1]
        self.assertEqual(nv.items[0].kind, 'EQ')
        self.assertNotIn(0, {r.sender for r in nv.items[1:]})

    def test_hidden_fast_certificate_survives_conflicting_leader_original(self):
        w = World(byzantine={0})
        for node in w.nodes:
            node.body('x')
            node.body('y')
        prepared_by_bad_leader(w, 'x', [1, 2])
        prepared_by_bad_leader(w, 'y', [3])
        # Exact votes are public, but no honest replica receives the full set.
        w.pump(drop=lambda i, p: p.kind in ('PREPARE', 'PC', 'FAST'))
        hidden = w.packet('FAST', 0, value='x', items=signed(w, 'PREPARE', 0, 'x'))
        w.certificate(hidden)
        self.assertFalse(any(p.d['decision'] for p in w.nodes))
        check(w)  # Hidden quorum is discovered independently, not by replicas.
        w.tick(3)
        w.pump()
        self.settled(w)
        w.enqueue(hidden)
        w.pump()
        self.settled(w)

    def test_pc_arriving_after_report_does_not_rewrite_report_or_emit_old_commit(self):
        w = World(7, byzantine={0, 6})
        for node in w.nodes:
            node.body('x')
        prepared_by_bad_leader(w, 'x', [1, 2, 3, 4])
        w.pump(drop=lambda i, p: p.kind == 'PREPARE')
        pc = w.packet('PC', 0, value='x', items=signed(w, 'PREPARE', 0, 'x'))
        w.certificate(pc)
        w.nodes[5].change_view(1)
        original = w.nodes[5].d['signed'][('REPORT', 1)]
        self.assertEqual(original.items[1].kind, 'EMPTY')
        w.nodes[5].receive(pc)
        self.assertEqual(w.nodes[5].d['signed'][('REPORT', 1)], original)
        self.assertNotIn(('COMMIT', 0), w.nodes[5].d['signed'])
        # Seat5 already entered view1 at time0; at time3 it may legitimately
        # enter view2 before the other seats enter view1. Give their next
        # declared timer its turn, rather than require one-timeout progress.
        for _ in range(2):
            w.tick(3)
            w.pump()
        self.settled(w)

    def test_votes_before_header_are_reconstructed(self):
        w = World()
        w.submit()
        w.pump(drop=lambda i, p: i == 3 and p.kind in ('PROPOSE', 'PC', 'FAST'))
        check(w)
        w.tick()
        w.pump()
        self.settled(w)

    def test_missing_body_requests_recover_without_new_client_action(self):
        w = World()
        w.submit()
        w.pump(drop=lambda i, p: i == 3 and p.kind == 'DATA')
        self.assertIsNone(w.nodes[3].d['applied'])
        self.assertTrue(w.nodes[3].pending)
        self.assertFalse(w.nodes[3].d['signed'])
        w.tick()
        w.pump()
        self.settled(w)

    def test_duplicate_certificates_do_not_reapply(self):
        w = World()
        w.submit()
        w.pump()
        proof = w.nodes[0].d['decision']
        for _ in range(12):
            w.enqueue(proof)
        w.pump()
        self.settled(w)

    def test_prepare_crash_boundaries_recover_exact_bytes(self):
        for point in ('before_compute', 'after_compute_before_durable',
                      'after_durable_before_publish', 'after_publish'):
            with self.subTest(point=point):
                w = World()
                node = w.nodes[2]
                node.cut = point
                w.submit()
                w.pump()
                self.assertFalse(node.online)
                old = deepcopy(node.d['signed'])
                node.restart()
                w.tick()
                w.pump()
                self.settled(w)
                for slot, vote in old.items():
                    self.assertEqual(node.d['signed'][slot], vote)

    def test_computed_undurable_signature_is_not_adversary_knowledge(self):
        w = World()
        w.nodes[0].body('x')
        w.nodes[0].cut = 'after_compute_before_durable'
        with self.assertRaises(Cut):
            w.nodes[0].propose('x')
        vote = next(iter(w.auth.computed))
        self.assertNotIn(vote, w.auth.published)
        w.nodes[1].receive(vote)
        self.assertIn('UNPUBLISHED_SIGNATURE_NOT_NETWORK_EVIDENCE', w.nodes[1].rejections)
        self.assertFalse(w.nodes[1].votes)
        check(w)

    def test_durable_unpublished_vote_retransmits_on_restart(self):
        w = World()
        node = w.nodes[0]
        node.body('x')
        node.cut = 'after_durable_before_publish'
        with self.assertRaises(Cut):
            node.propose('x')
        vote = node.d['signed'][('PREPARE', 0)]
        self.assertNotIn(vote, w.auth.published)
        node.restart()
        self.assertIn(vote, w.auth.published)
        w.pump()
        self.settled(w)

    def test_report_crash_preserves_atomic_view_and_exact_report(self):
        for point in ('before_compute', 'after_compute_before_durable',
                      'after_durable_before_publish', 'after_publish'):
            with self.subTest(point=point):
                w = World(byzantine={0})
                w.submit(via=3)
                w.pump()
                node = w.nodes[1]
                node.cut = point
                with self.assertRaises(Cut):
                    node.change_view(1)
                persisted = node.d['signed'].get(('REPORT', 1))
                self.assertEqual(node.d['view'], 1 if persisted else 0)
                node.restart()
                for _ in range(2):
                    w.tick(3)
                    w.pump()
                self.settled(w)
                if persisted:
                    self.assertEqual(node.d['signed'][('REPORT', 1)], persisted)

    def test_decision_and_application_crash_boundaries(self):
        for point in ('after_decision_before_apply', 'during_apply_before_atomic_commit', 'after_apply'):
            with self.subTest(point=point):
                w = World()
                node = w.nodes[2]
                node.cut = point
                w.submit()
                w.pump()
                self.assertFalse(node.online)
                self.assertIsNotNone(node.d['decision'])
                node.restart()
                w.pump()
                self.settled(w)

    def test_missing_required_local_guard_fences_restart(self):
        w = World()
        w.submit()
        w.pump()
        node = w.nodes[2]
        node.crash()
        old_applied = node.d['applied']
        node.d['guards'].clear()  # Deliberate test corruption, never recovery.
        node.restart()
        self.assertTrue(node.d['fenced'])
        self.assertIn('SAFE_REFUSAL', node.reason)
        self.assertEqual(node.d['applied'], old_applied)

    def test_missing_required_signed_body_fences_restart(self):
        w = World()
        w.submit()
        w.pump()
        node = w.nodes[2]
        node.crash()
        node.d['bodies'].clear()
        node.restart()
        self.assertTrue(node.d['fenced'])

    def test_restart_retains_view_change_evidence(self):
        w = World(byzantine={0})
        w.submit(via=3)
        w.pump()
        w.tick(3)
        # Pause between REPORT publication and receiving NEW_VIEW.
        for node in w.nodes[1:]:
            node.crash()
        for node in w.nodes[1:]:
            node.restart()
        w.pump()
        self.settled(w)
        for node in w.nodes[1:]:
            self.assertIn(1, node.d['new_views'])

    def test_duplicate_identity_does_not_form_certificate(self):
        w = World(byzantine={0})
        vote = w.adversary('PREPARE', 0, 0, 'x', targets=[])
        bad = w.packet('FAST', 0, value='x', items=(vote, vote, vote))
        with self.assertRaises(Invalid):
            w.certificate(bad)
        w.nodes[1].receive(bad)
        self.assertIsNone(w.nodes[1].d['decision'])
        check(w)

    def test_invalid_new_view_does_not_advance_or_sign(self):
        w = World(byzantine={1})
        bad = w.adversary('NEW_VIEW', 1, 1, 'y', targets=[])
        w.nodes[2].receive(bad)
        self.assertEqual(w.nodes[2].d['view'], 0)
        self.assertFalse(w.nodes[2].d['signed'])
        check(w)

    def test_complete_decision_prevents_fresh_new_view(self):
        w = World()
        w.submit()
        w.pump()
        before = set(w.auth.published)
        w.tick(20)
        w.pump()
        self.assertEqual(w.auth.published, before)
        self.settled(w)

    def test_idle_and_duplicate_traffic_do_not_authorize_timeout(self):
        w = World()
        w.tick(100)
        self.assertFalse(w.auth.published)
        self.assertTrue(all(p.d['view'] == 0 for p in w.nodes))

    def test_no_quorum_keeps_obligations_at_declared_test_bound(self):
        w = World()
        w.nodes[2].crash()
        w.nodes[3].crash()
        w.submit()
        w.pump()
        original = dict(w.nodes[0].d['signed'])
        for _ in range(4):
            w.tick(3)
            w.pump()
        self.assertFalse(any(p.d['applied'] for p in w.nodes))
        self.assertTrue(all(w.nodes[0].d['signed'][k] == p for k, p in original.items()))
        self.assertIn('BOUND_REACHED', w.nodes[0].reason)
        check(w)

    def test_independent_checker_detects_forged_publication(self):
        w = World()
        p = w.auth.compute(w.packet('PREPARE', 0, 2, 'x'))
        w.auth.published.add(p)
        with self.assertRaises(AssertionError):
            check(w)

    def test_independent_checker_detects_second_application(self):
        w = World()
        w.submit()
        w.pump()
        w.nodes[1].d['apply_count'] = 2
        with self.assertRaises(AssertionError):
            check(w)

    def test_finite_shuffled_delivery_campaign(self):
        for seed in range(24):
            with self.subTest(seed=seed):
                n = (4, 5, 7)[seed % 3]
                w = World(n, byzantine={0} if seed % 2 else ())
                w.submit(via=n - 1)
                w.pump(seed=seed)
                if w.byzantine:
                    w.tick(3)
                    w.pump(seed=seed + 100)
                self.settled(w)
                self.assertLess(w.steps, 1500)

    def test_finite_loss_then_delivery_and_restart_campaign(self):
        for seed in range(12):
            with self.subTest(seed=seed):
                rng = random.Random(seed + 900)
                w = World((4, 5, 7)[seed % 3])
                w.submit(via=w.n - 1)
                w.pump(seed=seed, drop=lambda i, p: rng.random() < 0.25)
                node = w.nodes[seed % w.n]
                node.crash()
                node.restart()
                for _ in range(3):
                    w.tick()
                    w.pump(seed=seed + 77)
                self.settled(w)

    def test_requested_body_wakes_waiting_new_view_without_timer(self):
        w = World(byzantine={0})
        # Seat1 is next leader, with authentic Q reports but no batch body.
        for node in w.nodes[2:]:
            node.body('x')
        for node in w.nodes[1:]:
            node.change_view(1)
        w.pump(drop=lambda i, p: p.kind == 'DATA')
        self.assertNotIn(('NEW_VIEW', 1), w.nodes[1].d['signed'])
        self.assertEqual(len(w.nodes[1].reports[1]), 3)
        w.nodes[1].receive(w.packet('DATA', value='x'))
        self.assertIn(('NEW_VIEW', 1), w.nodes[1].d['signed'])
        w.pump()
        self.settled(w)


if __name__ == '__main__':
    unittest.main()
