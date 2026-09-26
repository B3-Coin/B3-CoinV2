"""Semantic observer controls, not BLS or native storage tests.

The deterministic signature bytes below stand for the observer's explicitly
trusted issuance facts. They do not forge messages accepted by native workers.
"""
import copy
import unittest

from native_checker import Checker, Proof, empty, encode


INSTANCE = '11' * 32
X = '22' * 32
Y = '33' * 32
E = empty(INSTANCE)


def packet(kind, view, sender, value=None, items=()):
    signature = bytes([sender + 1]) * 96 if kind in ('PREPARE', 'COMMIT', 'REPORT', 'NEW_VIEW') else bytes(96)
    return Proof(kind, view, sender, INSTANCE, value, tuple(items), signature)


def guard(view, pre_view=None, changing=False, **fields):
    result = dict(view=view, pre_view=view if pre_view is None else pre_view,
                  changing=changing, body_available=not changing, fenced=False)
    for name in ('proposal', 'new_view', 'prepared', 'highest', 'v0', 'decision'):
        result[name] = encode(fields[name]) if fields.get(name) else ''
    return result


class Trace:
    def __init__(self):
        self.checker = Checker(INSTANCE)
        self.events = {}
        self.originals = {}
        self.views = {s: 0 for s in range(4)}

    def issue(self, p, g):
        event = dict(issued=encode(p), guard=g)
        self.checker.issued_event(p.sender, event)
        self.events[p.sender, p.kind, p.view] = event
        self.views[p.sender] = p.view
        if p.kind == 'PREPARE' and p.view == 0:
            from native_checker import decode
            leader = decode(g['proposal']).items[0]
            if leader.signature == bytes(96):
                leader = p
            self.originals[p.sender] = packet('V0', 0, p.sender, p.value, (p, leader))
        return p

    def prepare_zero(self, seats=(0, 1, 2)):
        leader = packet('PREPARE', 0, 0, X)
        proposal = packet('PROPOSE', 0, 0, X, (leader, E))
        return tuple(self.issue(packet('PREPARE', 0, s, X), guard(0, proposal=proposal)) for s in seats)

    def report(self, seat, target, highest=None):
        original = self.originals.get(seat, E)
        p = packet('REPORT', target, seat, items=(original, highest or E, E))
        return self.issue(p, guard(target, self.views[seat], True, v0=original, highest=highest))

    def prepare_one(self):
        reports = tuple(self.report(s, 1) for s in (1, 2, 3))
        nv = self.issue(packet('NEW_VIEW', 1, 1, X, (E, *reports)), guard(1, changing=True))
        leader = packet('PREPARE', 1, 1, X)
        proposal = packet('PROPOSE', 1, 1, X, (leader, nv))
        votes = tuple(self.issue(packet('PREPARE', 1, s, X), guard(1, proposal=proposal, new_view=nv))
                      for s in (1, 2, 3))
        return packet('PC', 1, -1, X, votes), proposal, nv

    def status(self, seat, /, **changes):
        state = dict(seat=seat, instance=INSTANCE, view=self.views[seat], fenced=False,
                     apply_count=0, applied=None, signed_count=sum(s == seat for s, _, _ in self.events),
                     v0=encode(self.originals[seat]) if seat in self.originals else '', highest='', decision='')
        state.update(changes)
        self.checker.status(seat, state)
        return state


class NativeCheckerTests(unittest.TestCase):
    def test_hidden_fast_without_local_application(self):
        t = Trace()
        t.prepare_zero()
        for seat in (0, 1, 2):
            t.status(seat)
        self.assertEqual(t.checker.decisions, {X})
        self.assertEqual(t.checker.last_count[1], 0)

    def test_hidden_conflicting_fast_predicate(self):
        c = Checker(INSTANCE)
        # Unit-level control of the retained-vote predicate, independent of the
        # earlier slot guard which would also reject these conflicting votes.
        c.authenticated.update(packet('PREPARE', 0, s, value) for value in (X, Y) for s in (0, 1, 2))
        with self.assertRaisesRegex(AssertionError, 'hidden conflicting decisions'):
            c.hidden()

    def test_hidden_conflicting_slow_predicate(self):
        c = Checker(INSTANCE)
        c.authenticated.update(packet('COMMIT', v, s, value) for v, value in ((1, X), (2, Y)) for s in (1, 2, 3))
        with self.assertRaisesRegex(AssertionError, 'hidden conflicting decisions'):
            c.hidden()

    def test_original_report_omission_rejected_before_status(self):
        t = Trace()
        t.prepare_zero((0, 1))
        p = packet('REPORT', 1, 1, items=(E, E, E))
        with self.assertRaisesRegex(AssertionError, 'original vote obligation'):
            t.issue(p, guard(1, 0, True))

    def test_original_status_omission_rejected(self):
        t = Trace()
        t.prepare_zero((0, 1))
        with self.assertRaisesRegex(AssertionError, 'original vote obligation'):
            t.status(1, v0='')

    def test_original_leader_evidence_cannot_be_replaced(self):
        t = Trace()
        votes = t.prepare_zero((0, 1))
        wrong = packet('V0', 0, 1, X, (votes[1], votes[1]))
        with self.assertRaises(AssertionError):
            t.status(1, v0=encode(wrong))

    def test_original_owner_cannot_be_replaced(self):
        t = Trace()
        t.prepare_zero((0, 1))
        with self.assertRaisesRegex(AssertionError, 'wrong original owner'):
            t.status(1, v0=encode(t.originals[0]))

    def test_future_pc_cannot_be_omitted_from_report(self):
        t = Trace()
        pc, _, _ = t.prepare_one()
        t.status(0, highest=encode(pc))
        with self.assertRaisesRegex(AssertionError, 'highest PC obligation'):
            t.report(0, 1)

    def test_future_pc_target_advance_is_accepted(self):
        t = Trace()
        pc, _, _ = t.prepare_one()
        t.status(0, highest=encode(pc))
        t.report(0, 2, highest=pc)
        t.status(0, highest=encode(pc))

    def test_pc_at_report_target_is_rejected(self):
        t = Trace()
        pc, _, _ = t.prepare_one()
        with self.assertRaises(AssertionError):
            t.report(0, 1, highest=pc)

    def test_highest_pc_cannot_be_lost_on_restart_status(self):
        t = Trace()
        pc, _, _ = t.prepare_one()
        t.status(0, highest=encode(pc))
        with self.assertRaisesRegex(AssertionError, 'highest PC obligation'):
            t.status(0)

    def test_wrong_highest_kind_rejected(self):
        t = Trace()
        votes = t.prepare_zero((0, 1))
        with self.assertRaisesRegex(AssertionError, 'wrong obligation kind: highest'):
            t.status(1, highest=encode(votes[0]))

    def test_application_requires_certificate(self):
        t = Trace()
        with self.assertRaisesRegex(AssertionError, 'application without matching decision certificate'):
            t.status(1, apply_count=1, applied=X)

    def test_application_and_restart_with_full_certificate(self):
        t = Trace()
        votes = t.prepare_zero()
        fast = packet('FAST', 0, -1, X, votes)
        for _ in range(2):
            t.status(1, apply_count=1, applied=X, decision=encode(fast))

    def test_application_must_match_certificate(self):
        t = Trace()
        fast = packet('FAST', 0, -1, X, t.prepare_zero())
        with self.assertRaisesRegex(AssertionError, 'application without matching decision certificate'):
            t.status(1, apply_count=1, applied=Y, decision=encode(fast))

    def test_decision_cannot_be_lost_before_application(self):
        t = Trace()
        fast = packet('FAST', 0, -1, X, t.prepare_zero())
        t.status(1, decision=encode(fast))
        with self.assertRaisesRegex(AssertionError, 'decision obligation'):
            t.status(1)

    def test_application_count_requires_applied_value(self):
        with self.assertRaisesRegex(AssertionError, 'application/count mismatch'):
            Trace().status(1, apply_count=0, applied=X)

    def test_unobserved_status_signature_is_not_authenticated(self):
        t = Trace()
        fast = packet('FAST', 0, -1, X, tuple(packet('PREPARE', 0, s, X) for s in (0, 1, 2)))
        with self.assertRaisesRegex(AssertionError, 'unobserved signature evidence'):
            t.status(1, decision=encode(fast))

    def test_status_instance_and_seat_are_bound(self):
        for field, value in (('instance', Y), ('seat', 2)):
            with self.subTest(field=field), self.assertRaisesRegex(AssertionError, 'status identity/instance'):
                Trace().status(1, **{field: value})

    def test_signed_count_covers_observed_slots(self):
        t = Trace()
        t.prepare_zero((0, 1))
        with self.assertRaisesRegex(AssertionError, 'lost signing records/count'):
            t.status(1, signed_count=0)

    def test_signed_count_cannot_regress(self):
        t = Trace()
        # A count alone must not add unseen signatures to the authenticated set.
        t.status(1, signed_count=1)
        self.assertFalse(t.checker.authenticated)
        with self.assertRaisesRegex(AssertionError, 'lost signing records/count'):
            t.status(1, signed_count=0)

    def test_old_retransmission_guard_ignores_newer_obligations(self):
        t = Trace()
        t.prepare_zero((0, 1))
        old_event = copy.deepcopy(t.events[1, 'PREPARE', 0])
        t.report(1, 1)
        t.status(1)
        t.checker.issued_event(1, old_event)
        self.assertEqual(t.checker.last_view[1], 1)

    def test_changed_retransmission_guard_rejected(self):
        t = Trace()
        t.prepare_zero((0, 1))
        old_event = copy.deepcopy(t.events[1, 'PREPARE', 0])
        old_event['guard']['highest'] = encode(E)
        with self.assertRaisesRegex(AssertionError, 'changed retransmission/guard'):
            t.checker.issued_event(1, old_event)

    def test_fenced_memory_does_not_promote_novel_obligations(self):
        t = Trace()
        # Failed persist may expose computed but unpublished bytes. Reopen is
        # checked against the last good snapshot, not this undurable memory.
        unobserved = packet('FAST', 0, -1, X, tuple(packet('PREPARE', 0, s, X) for s in (0, 1, 2)))
        t.status(1, fenced=True, view=2, apply_count=1, applied=X, signed_count=9,
                 v0='not a proof', highest='not a proof', decision=encode(unobserved))
        self.assertFalse(t.checker.authenticated)
        self.assertFalse(t.checker.decisions)
        t.status(1)

    def test_skeletal_fenced_status_preserves_last_good_snapshot(self):
        t = Trace()
        t.prepare_zero((0, 1))
        t.status(1)
        t.checker.status(1, dict(seat=1, instance=INSTANCE, fenced=True,
                                storage_usable=False, reason='STORAGE_SAFE_REFUSAL'))
        t.status(1)

    def test_fenced_status_does_not_erase_good_obligations(self):
        t = Trace()
        t.prepare_zero((0, 1))
        t.status(1)
        t.status(1, fenced=True, v0='', signed_count=0)
        with self.assertRaisesRegex(AssertionError, 'original vote obligation'):
            t.status(1, v0='')

    def test_new_fenced_issuance_rejected(self):
        t = Trace()
        p = packet('REPORT', 1, 1, items=(E, E, E))
        g = guard(1, 0, True)
        g['fenced'] = True
        with self.assertRaises(AssertionError):
            t.issue(p, g)

    def test_commit_requires_retained_prepared_pc(self):
        t = Trace()
        pc, proposal, nv = t.prepare_one()
        with self.assertRaisesRegex(AssertionError, 'commit lost prepared PC obligation'):
            t.issue(packet('COMMIT', 1, 1, X), guard(1, proposal=proposal, new_view=nv, prepared=pc))

    def test_valid_commit_with_prepared_pc(self):
        t = Trace()
        pc, proposal, nv = t.prepare_one()
        t.issue(packet('COMMIT', 1, 1, X), guard(1, proposal=proposal, new_view=nv, prepared=pc, highest=pc))
        t.status(1, highest=encode(pc))

    def test_new_view_without_value_rejected(self):
        t = Trace()
        reports = tuple(t.report(s, 1) for s in (1, 2, 3))
        with self.assertRaises(AssertionError):
            t.issue(packet('NEW_VIEW', 1, 1, items=(E, *reports)), guard(1, changing=True))


if __name__ == '__main__':
    unittest.main()
