"""Research-only evidence-provenance probe, NOT an agreement implementation.

This extracts the empty-PC/empty-decision branch of the supplied RULE-FV
description. Synthetic objects are authenticated by an ideal issued registry.
There is no production codec, key, signature operation, runtime or storage.
"""
from dataclasses import dataclass
import itertools
import json


@dataclass(frozen=True)
class Proposal:
    instance: str
    view: int
    sender: int
    value: str


@dataclass(frozen=True)
class Report:
    instance: str
    target: int
    sender: int
    vote: Proposal | None


class Invalid(ValueError):
    pass


class Probe:
    """Four seats, leader 0; only two abstract values, one fixed instance."""
    def __init__(self):
        self.instance = 'SYNTHETIC-P2FV-EVIDENCE-PROBE/1'
        self.issued = set()

    def proposal(self, value, *, instance=None, view=0, sender=0):
        p = Proposal(instance or self.instance, view, sender, value)
        self.issued.add(p)
        return p

    def report(self, sender, vote=None, *, target=1, instance=None):
        r = Report(instance or self.instance, target, sender, vote)
        self.issued.add(r)
        return r

    def _proposal(self, p):
        if (type(p) is not Proposal or p not in self.issued
                or p.instance != self.instance or p.view != 0
                or p.sender != 0 or p.value not in ('x', 'y')):
            raise Invalid('INVALID_OR_UNAUTHENTICATED_LEADER_PROPOSAL')

    def choices(self, reports, evidence=()):
        """Pure selection from carried evidence; no access to receiver history.

        With a PC or decision this helper is inapplicable. For this empty-PC
        branch both interpretations of the global exclusion ambiguity agree.
        """
        if type(reports) is not tuple or len(reports) != 3:
            raise Invalid('REPORT_QUORUM')
        identities = set()
        proposals = []
        for r in reports:
            if (type(r) is not Report or r not in self.issued
                    or r.instance != self.instance or r.target != 1
                    or type(r.sender) is not int or not 0 <= r.sender < 4
                    or r.sender in identities):
                raise Invalid('INVALID_REPORT_OR_DUPLICATE_IDENTITY')
            identities.add(r.sender)
            if r.vote is not None:
                self._proposal(r.vote)
                proposals.append(r.vote)
        if type(evidence) is not tuple or len(evidence) not in (0, 2):
            raise Invalid('EVIDENCE_MUST_BE_ORIGINAL_SIGNED_PAIR')
        for p in evidence:
            self._proposal(p)
        if evidence and evidence[0].value == evidence[1].value:
            raise Invalid('NOT_EQUIVOCATION')
        # Two originals embedded in reports also provide the evidence; the
        # explicit pair is necessary only when the selected Q does not carry it.
        equivocation = len({p.value for p in proposals + list(evidence)}) > 1
        if equivocation:
            if 0 in identities:
                raise Invalid('EVIDENCE_STATUS_SET_COUNTS_OLD_LEADER')
            counts = {v: sum(p.value == v for p in proposals) for v in ('x', 'y')}
            qualified = sorted(v for v in counts if counts[v] >= 2)
            return frozenset(qualified[:1] or ('x', 'y'))
        return frozenset({p.value for p in proposals} or ('x', 'y'))


def counterexample():
    probe = Probe()
    x, y = probe.proposal('x'), probe.proposal('y')
    reports = (probe.report(1, x), probe.report(2), probe.report(3))
    # Literal report witness: a fourth authenticated status is "in hand" but
    # excluded from the final Q because it is from the equivocating L0.
    discarded_status = probe.report(0, y)
    leader_private_evidence = (reports[0].vote, discarded_status.vote)
    # The report says the leader can use statuses/evidence "in hand", but
    # its declared NEW_VIEW fields do not explicitly carry this external pair.
    leader = probe.choices(reports, leader_private_evidence)
    follower_without_pair = probe.choices(reports)
    follower_with_pair = probe.choices(reports, leader_private_evidence)
    return {
        'profile': 'p2fv-new-view-evidence-probe/1',
        'selected_statuses': ['seat1:x', 'seat2:empty', 'seat3:empty'],
        'additional_report_not_selected': 'seat0:y',
        'extra_evidence': ['leader0:signed-x', 'leader0:signed-y'],
        'leader_allowed_using_private_pair': sorted(leader),
        'follower_allowed_from_declared_fields_only': sorted(follower_without_pair),
        'leader_selected_y_is_rejected_without_pair': 'y' in leader and 'y' not in follower_without_pair,
        'follower_allowed_with_explicit_pair': sorted(follower_with_pair),
        'scope': 'interpretation mismatch, not conflicting finalized certificates or a running-wallet defect',
    }


def permutations_checked(probe, reports, evidence=()):
    return {probe.choices(tuple(p), evidence) for p in itertools.permutations(reports)}


if __name__ == '__main__':
    print(json.dumps(counterexample(), indent=2, sort_keys=True))
