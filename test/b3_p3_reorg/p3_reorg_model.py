#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Bounded model of B3 finality voting against reorganizations: V1's
ancestry lock beside V2 P3 (dependency document sections 1, 9 and 13; Stage 0
section 7, `doc/design/b3-flowmesh-v2-stage0.md:345-430`, `:547-581`; R1
`doc/design/v2-stage0-r1-consensus.md:47-58`, `:118-140`).

Run: python3 -I -B test/b3_p3_reorg/p3_reorg_model.py (standard library only,
about 15 minutes and 3 GB; --only LABEL runs one scenario). Nothing here
exists in b3d yet: P3 has no C++, so a pass is evidence about the protocol
as written, not about any implementation.

The B3 chain is one fork. Checkpoint candidates A and B are the two blocks at
one checkpoint height, both extending the last committed checkpoint. Which of
them a node's tip contains changes whenever the adversary delivers blocks
(any honest node may flip at any step), because fork choice ignores votes.

V1 (the positive controls, `src/node/finality_signature.cpp:607-680`):
- a member signs the checkpoint on its tip at h1 and at h2 (h2 is the next
  checkpoint on the same branch) at most once each, and only when its tip
  extends its ancestry lock; signing moves the lock to the signed block;
- a certificate is a quorum of signatures on one block; the only unlock is a
  certificate at a newer height on the member's own tip.

P3 (single-sequence PBFT-style, Stage 0 section 7):
- views 0..VIEWS-1, leader of view v is member v mod n; an honest leader
  proposes its tip in view 0, and in a later view the value a quorum of
  VIEW_CHANGE reports forces (the highest prepared), else its tip;
- a member PREPAREs at most once per view, becomes prepared on observing a
  quorum of PREPAREs for its value in its view, then COMMITs; a quorum of
  COMMITs decides, and a member that learns a decision pins its tip to it;
- a timeout moves a member one view up and sends its highest prepared value
  with the proof;
- validity is "pure" (Stage 0: a candidate extending the committed parent
  with its branch data is valid on any tip, `v2-stage0-r1-consensus.md:118`)
  or, in the tip variant, "the candidate is on my tip". Under pure validity
  no vote reads a tip, so the state keeps none, an honest leader may propose
  either block, and a member learning a decision may be on either branch:
  a forced reorg is then possible by construction, as Stage 0 intends.
Byzantine members have sent every PREPARE, COMMIT and VIEW_CHANGE report
they could (a report must carry a real quorum), in every view of the bound;
they send nothing after stabilization. Crashes are crash-stop.

Quorum: headcount floor(2n/3)+1 and weight floor(2W/3)+1 (dual), or only one
of them (`v2-stage0-r1-consensus.md:47-55`).

Bound: VIEWS = 3, or 2 where all four members are honest or tips are kept
(those at three views exceed 10^7 states); the line of each scenario names
its bound. The search stops at the first S1 violation.

Checks, over every reachable state within the bound (breadth-first, so each
trace below is a shortest one):
- S1 agreement: no two different values are ever decidable.
- L1 progress after stabilization: no further faults or Byzantine messages,
  B3 fork choice settles every live honest tip on one branch X that the
  adversary picks, and fresh views follow. L1 holds in a state if, for both
  X, some honest leader can justify a value that a quorum of live honest
  members accepts (P3), or a quorum of live honest members may still sign on
  X (V1). A stall is a violation only when the live honest members form a
  quorum; otherwise it is reported as beyond the fault bound.
- forced reorg (P3): a member learns a decision for the branch its tip has
  left. B3 must then reorganize back to the decision (Stage 0 `:576-581`).

Exit status 1 when any scenario's result differs from its expected line.
"""
import argparse
import collections
import functools
import itertools
import sys

A, B = 1, 2
NAME = {0: '-', A: 'A', B: 'B'}


class Config:
    def __init__(self, label, expect, n=4, weights=None, byz=(), rule='dual', validity='pure',
                 crashes=0, views=3, mutants=()):
        self.label, self.expect, self.n = label, expect, n
        self.weights = tuple(weights or (1,) * n)
        self.byz, self.rule, self.validity = tuple(byz), rule, validity
        self.crashes, self.views, self.mutants = crashes, views, frozenset(mutants)
        self.honest = tuple(i for i in range(n) if i not in self.byz)
        short = 1 if 'quorum_minus_one' in self.mutants else 0
        self.qh = 2 * n // 3 + 1 - short
        self.qw = 2 * sum(self.weights) // 3 + 1 - short
        self.byz_mask = sum(1 << i for i in self.byz)
        self.table = [self.quorum_of(mask) for mask in range(1 << n)]

    def quorum_of(self, mask):
        members = [i for i in range(self.n) if mask >> i & 1]
        count_ok = len(members) >= self.qh
        weight_ok = sum(self.weights[i] for i in members) >= self.qw
        return {'count': count_ok, 'weight': weight_ok, 'dual': count_ok and weight_ok}[self.rule]

    def quorum(self, members):
        return self.table[sum(1 << i for i in set(members))]


def subsets(items):
    for size in range(1, len(items) + 1):
        yield from itertools.combinations(items, size)


class Explorer:
    """Breadth-first search with parent links for shortest traces."""

    def __init__(self, cfg):
        self.cfg = cfg

    def explore(self):
        start = self.initial()
        parent = {start: None}
        queue = collections.deque([start])
        found = {}
        while queue:
            st = queue.popleft()
            for name, verdict in self.check(st):
                found.setdefault(name, (verdict, st))
            if 'S1' in found:
                break
            for label, nxt, event in self.successors(st):
                if nxt not in parent:
                    parent[nxt] = (st, label)
                    queue.append(nxt)
                    if event:
                        found.setdefault(event, ('reachable', nxt))
        self.parent = parent
        return len(parent), found

    def trace(self, st):
        steps = []
        while self.parent[st] is not None:
            st, label = self.parent[st]
            steps.append(label)
        return list(reversed(steps))


P3Rep = collections.namedtuple('P3Rep', 'view prep comm prepared vc decided tip crashed')


class P3(Explorer):
    def initial(self):
        v = self.cfg.views
        rep = P3Rep(0, (0,) * v, (0,) * v, None, (None,) * v, 0, self.tips(), False)
        return (tuple(rep for _ in self.cfg.honest), (0,) * v, self.cfg.crashes)

    def tips(self):
        """Pure validity never reads a tip, so the state keeps none (0)."""
        return A if self.cfg.validity == 'tip' else 0

    def own_values(self, rep):
        """What an honest leader proposes when free: the block on its tip."""
        return {rep.tip} if rep.tip else {A, B}

    def valid(self, rep, value):
        return self.cfg.validity == 'pure' or rep.tip == value

    def mask(self, st, field, view, value):
        mask = 0
        for k, i in enumerate(self.cfg.honest):
            if getattr(st[0][k], field)[view] == value:
                mask |= 1 << i
        return mask

    def qc(self, st, view, value):
        return self.cfg.table[self.mask(st, 'prep', view, value) | self.cfg.byz_mask]

    def decidable(self, st):
        return {c for c in (A, B) for v in range(self.cfg.views)
                if self.cfg.table[self.mask(st, 'comm', v, c) | self.cfg.byz_mask]}

    def report(self, rep):
        if 'forget_prepared' in self.cfg.mutants or rep.prepared is None:
            return ()
        return rep.prepared

    @functools.lru_cache(maxsize=None)
    def justified(self, view, honest_reports, qcs):
        """(forced values, free) over every quorum of VIEW_CHANGE reports."""
        reporters = list(honest_reports) + [(i, ((),) + qcs) for i in self.cfg.byz]
        forced, free = set(), 'free_new_view' in self.cfg.mutants
        for chosen in subsets(reporters):
            if not self.cfg.quorum([i for i, _ in chosen]):
                continue
            for combo in itertools.product(*[opts for _, opts in chosen]):
                prepared = [r for r in combo if r]
                if not prepared:
                    free = True
                    continue
                top = max(r[0] for r in prepared)
                forced |= {r[1] for r in prepared if r[0] == top}
        return frozenset(forced), free

    def view_options(self, st, view):
        reps = st[0]
        honest = tuple((i, (reps[k].vc[view],)) for k, i in enumerate(self.cfg.honest)
                       if reps[k].vc[view] is not None)
        qcs = tuple((v, c) for v in range(view) for c in (A, B) if self.qc(st, v, c))
        return self.justified(view, honest, qcs)

    def proposals(self, st, view):
        leader = view % self.cfg.n
        if leader in self.cfg.byz:
            if view == 0:
                return {A, B}
            forced, free = self.view_options(st, view)
            return {A, B} if free else set(forced)
        value = st[1][view]
        return {value} if value else set()

    def successors(self, st):
        reps, props, crashes = st
        cfg = self.cfg
        decidable = sorted(self.decidable(st))
        for k, i in enumerate(cfg.honest):
            r = reps[k]
            if r.crashed or r.decided:
                continue
            v = r.view

            def put(**changes):
                new = list(reps)
                new[k] = r._replace(**changes)
                return tuple(new)

            if v % cfg.n == i and props[v] == 0:
                if v == 0:
                    values = self.own_values(r)
                else:
                    forced, free = self.view_options(st, v)
                    values = {c for c in forced if self.valid(r, c)} | (self.own_values(r) if free else set())
                for c in sorted(values):
                    new_props = props[:v] + (c,) + props[v + 1:]
                    yield f'm{i} proposes {NAME[c]} in view {v}', (reps, new_props, crashes), None
            if r.prep[v] == 0:
                for c in sorted(self.proposals(st, v)):
                    if self.valid(r, c):
                        prep = r.prep[:v] + (c,) + r.prep[v + 1:]
                        yield f'm{i} prepares {NAME[c]} in view {v}', (put(prep=prep), props, crashes), None
            c = r.prep[v]
            if c and (r.prepared is None or r.prepared[0] < v) and self.qc(st, v, c):
                yield f'm{i} is prepared on {NAME[c]} in view {v}', (put(prepared=(v, c)), props, crashes), None
            if r.prepared and r.prepared[0] == v and r.comm[v] == 0 and self.valid(r, r.prepared[1]):
                c = r.prepared[1]
                comm = r.comm[:v] + (c,) + r.comm[v + 1:]
                yield f'm{i} commits {NAME[c]} in view {v}', (put(comm=comm), props, crashes), None
            for c in decidable:
                event = 'forced reorg' if r.tip != c else None
                yield (f'm{i} learns the decision {NAME[c]} (tip {NAME[r.tip] if r.tip else "anywhere"})',
                       (put(decided=c, tip=c if r.tip else 0), props, crashes), event)
            if v + 1 < cfg.views:
                vc = r.vc[:v + 1] + (self.report(r),) + r.vc[v + 2:]
                label = f'm{i} times out to view {v + 1}, reporting {self.describe(self.report(r))}'
                yield label, (put(view=v + 1, vc=vc), props, crashes), None
            if r.tip:
                yield f'm{i} tip moves to {NAME[3 - r.tip]}', (put(tip=3 - r.tip), props, crashes), None
            if crashes:
                yield f'm{i} crashes', (put(crashed=True), props, crashes - 1), None

    @staticmethod
    def describe(report):
        return f'prepared {NAME[report[1]]} in view {report[0]}' if report else 'nothing prepared'

    def check(self, st):
        reps = st[0]
        cfg = self.cfg
        values = self.decidable(st) | {r.decided for r in reps if r.decided}
        if len(values) > 1:
            yield 'S1', 'violated'
            return
        live = [i for k, i in enumerate(cfg.honest) if not reps[k].crashed]
        if any(r.decided for r in reps):
            return
        if not cfg.quorum(live):
            yield 'L1 beyond the fault bound', 'stalled'
            return
        for v in range(cfg.views):
            for c in (A, B):
                if cfg.quorum([i for i in live if reps[cfg.honest.index(i)].comm[v] == c]):
                    return
        by_member = {i: reps[k] for k, i in enumerate(cfg.honest)}
        for settled in (A, B):
            if not self.recovers(by_member, live, settled):
                yield 'L1', f'violated (tips settle on {NAME[settled]})'
                return

    def recovers(self, by_member, live, settled):
        for chosen in subsets(live):
            if not self.cfg.quorum(chosen):
                continue
            reports = [self.report(by_member[i]) for i in chosen]
            prepared = [r for r in reports if r]
            if not prepared or 'free_new_view' in self.cfg.mutants:
                values = {settled}
            else:
                top = max(r[0] for r in prepared)
                values = {r[1] for r in prepared if r[0] == top}
            for c in values:
                if self.cfg.validity == 'pure' or c == settled:
                    return True
        return False


V1Rep = collections.namedtuple('V1Rep', 'sig1 sig2 lock tip crashed')


class V1(Explorer):
    def initial(self):
        return (tuple(V1Rep(0, 0, (0, 0), A, False) for _ in self.cfg.honest), self.cfg.crashes)

    def cert(self, st, field, value):
        reps = st[0]
        signers = [i for k, i in enumerate(self.cfg.honest) if getattr(reps[k], field) == value]
        return self.cfg.quorum(signers + list(self.cfg.byz))

    def successors(self, st):
        reps, crashes = st
        for k, i in enumerate(self.cfg.honest):
            r = reps[k]
            if r.crashed:
                continue

            def put(**changes):
                new = list(reps)
                new[k] = r._replace(**changes)
                return tuple(new)

            lock_ok = r.lock[1] in (0, r.tip)
            if r.sig1 == 0 and lock_ok and r.lock[0] < 1:
                yield f'm{i} signs {NAME[r.tip]} at h1', (put(sig1=r.tip, lock=(1, r.tip)), crashes), None
            if r.sig2 == 0 and lock_ok and r.lock[0] < 2:
                yield f'm{i} signs {NAME[r.tip]}2 at h2', (put(sig2=r.tip, lock=(2, r.tip)), crashes), None
            if r.lock[0] == 1 and r.lock[1] != r.tip and self.cert(st, 'sig2', r.tip):
                yield f'm{i} unlocks on the h2 certificate', (put(lock=(2, r.tip)), crashes), None
            yield f'm{i} tip moves to {NAME[3 - r.tip]}', (put(tip=3 - r.tip), crashes), None
            if crashes:
                yield f'm{i} crashes', (put(crashed=True), crashes - 1), None

    def check(self, st):
        reps = st[0]
        cfg = self.cfg
        live = [i for k, i in enumerate(cfg.honest) if not reps[k].crashed]
        if not cfg.quorum(live):
            yield 'L1 beyond the fault bound', 'stalled'
            return
        for settled in (A, B):
            unlocked = [i for i in live if reps[cfg.honest.index(i)].lock[1] in (0, settled)]
            if not cfg.quorum(unlocked) and not self.cert(st, 'sig2', settled):
                orphaned = len(live) - len(unlocked)
                yield 'L1', f'violated (tips settle on {NAME[settled]}, {orphaned} orphan-locked)'
                if orphaned == 1:
                    yield 'L1 with one orphan lock', f'violated (tips settle on {NAME[settled]})'
                return


SCENARIOS = [
    (V1, Config('PC1 V1 ancestry lock, all honest', 'L1 violated')),
    (V1, Config('PC2 V1 ancestry lock, one crash', 'L1 violated with one orphan lock', crashes=1)),
    (P3, Config('P3 no Byzantine, one crash', 'S1 holds, L1 holds, forced reorg', crashes=1, views=2)),
    (P3, Config('P3 Byzantine m0 (view-0 leader)', 'S1 holds, L1 holds, forced reorg', byz=(0,))),
    (P3, Config('P3 Byzantine m1 (view-1 leader)', 'S1 holds, L1 holds, forced reorg', byz=(1,))),
    (P3, Config('P3 Byzantine m3 (never leads)', 'S1 holds, L1 holds, forced reorg', byz=(3,))),
    (P3, Config('P3 tip validity, no Byzantine', 'S1 holds, L1 violated, forced reorg', validity='tip',
                views=2)),
    (P3, Config('P3 tip validity, Byzantine m3', 'S1 holds, L1 violated, forced reorg', validity='tip', byz=(3,),
                views=2)),
    (P3, Config('NC1 P3 with two Byzantine of four', 'S1 violated', byz=(0, 3))),
    (P3, Config('NC2 P3 weight-only, heavy m0 Byzantine', 'S1 violated', weights=(4, 1, 1, 1),
                byz=(0,), rule='weight')),
    (P3, Config('NC2 P3 dual quorum, heavy m0 Byzantine', 'S1 holds, L1 beyond the fault bound, forced reorg',
                weights=(4, 1, 1, 1), byz=(0,))),
    (P3, Config('mutant: NEW_VIEW ignores prepared reports', 'S1 violated', byz=(3,),
                mutants=('free_new_view',))),
    (P3, Config('mutant: reports forget the prepared value', 'S1 violated', byz=(3,),
                mutants=('forget_prepared',))),
    (P3, Config('mutant: both quorum thresholds one short', 'S1 violated', byz=(3,),
                mutants=('quorum_minus_one',))),
]


def summarize(kind, found):
    parts = []
    if 'S1' in found:
        parts.append('S1 violated')
        return ', '.join(parts)
    if kind is P3:
        parts.append('S1 holds')
    if 'L1 with one orphan lock' in found:
        parts.append('L1 violated with one orphan lock')
    elif 'L1' in found:
        parts.append('L1 violated')
    elif 'L1 beyond the fault bound' in found:
        parts.append('L1 beyond the fault bound')
    else:
        parts.append('L1 holds')
    if 'forced reorg' in found:
        parts.append('forced reorg')
    return ', '.join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--traces', action='store_true', help='print a shortest trace for each finding')
    parser.add_argument('--only', help='run only the scenarios whose label contains this text')
    parser.add_argument('--views', type=int, help='override the view bound')
    args = parser.parse_args()
    failures = 0
    for kind, cfg in SCENARIOS:
        if args.only and args.only not in cfg.label:
            continue
        if args.views:
            cfg.views = args.views
        explorer = kind(cfg)
        count, found = explorer.explore()
        got = summarize(kind, found)
        status = 'ok' if got == cfg.expect else 'UNEXPECTED'
        failures += status != 'ok'
        bound = f'{cfg.views} views, ' if kind is P3 else ''
        print(f'{cfg.label}: {got} [{bound}{count} states] {status}', flush=True)
        if status != 'ok':
            print(f'  expected: {cfg.expect}')
        if args.traces:
            for name in sorted(found):
                verdict, st = found[name]
                steps = explorer.trace(st)
                print(f'  {name}: {verdict}, {len(steps)} steps')
                for step in steps:
                    print(f'    {step}')
    print('all scenarios as expected' if not failures else f'{failures} scenario(s) differ from expectations')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
