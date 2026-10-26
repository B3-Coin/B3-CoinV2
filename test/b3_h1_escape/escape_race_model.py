#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Bounded model of the H1 escape race (dependency document sections 5.1, 5.2,
13 H-H1 and 19.2), with the modern-PoS fork choice as implemented.

Run: python3 -I -B test/b3_h1_escape/escape_race_model.py (standard library
only, about a minute). The C++ counterparts are the escape_race_* cases in
src/test/finality_h1_tests.cpp. A pass is evidence about the rule as written,
not about the C++.

Abstractions (each one limits what a run shows):
- Keys: A is in force and adversarial, B is in force and offline (crashed),
  C is honest and online, in Snapshot(parent) from height 1 but not in the
  bootstrap set Snapshot(R_h - 1) = {A, B}. R_h = 1.
- Eligibility is deterministic: a key in force is eligible from its
  saturation round on (SAT_A, SAT_C), never before. Any key of
  Snapshot(parent) is eligible from round R_ESC (the escape).
- A block in round r on a parent with time T has time T + r + 1 and can be
  made only once the global clock reaches it (the future-time rule). There
  is no lateness rule, so a block can be published at any later step.
- Fork choice: most blocks (every modern block has the same work), then the
  branch whose first divergent block has the lower time, then lower hash.
  That is the lexicographic minimum of the (time, hash) path.
- C sees every published block at once, follows the best published tip, and
  makes one block on it at the first round it is eligible. B never acts.
- A, at every step, may make one block on any known block (at the step's
  time) and withhold or publish it, and may publish any withheld block.
- No reorganization horizon (it would only cap the depth found below).

Search: exhaustive over every adversary strategy within MAX_ADV blocks and
T_MAX steps (memoized on the state). Reported per parameter set:
- stall: the latest step at which C first has a block that stays in the
  best chain to T_MAX (T_MAX + 1 when it never does), maximised over A's
  strategies, next to the same with A silent;
- reorg: the most C blocks that were in the best chain at some step and are
  not in it at T_MAX, maximised over A's strategies;
- a determinism check: the schedule of every block equals the schedule
  recomputed from genesis along its path (H-H1), and the best tip is the
  same for every arrival order of the published set (by construction here:
  the choice depends on the set only).

--compare-fixes runs the same search under candidate rule changes. None of
them exists in the C++; each would be a consensus change for the owner.
- escape_wins: at the first divergent block, an escape block beats a
  non-escape sibling before time and hash are compared.
- lateness: a block, together with any ancestor not yet accepted, is refused
  when it is published more than LATE steps after its time. Every honest
  node sees a publication at the same step here, so this run cannot show
  the disagreement such a local-clock rule can cause.
- cumulative: the escape counts the rounds of every block since the epoch
  started (rotation or escape), so a block is an escape once that sum plus
  its own round reaches R_ESC.
It also reports the lowest final chain height any strategy can force.
"""
import argparse
import functools
import hashlib
import itertools
import json
import sys

A, B, C = 'A', 'B', 'C'
BOOT = frozenset({A, B})
SNAP = frozenset({A, B, C})


def hash_of(block):
    return hashlib.sha256(repr(block).encode()).hexdigest()


class Model:
    LATE = 1  # steps a publication may trail its block's time ('lateness')

    def __init__(self, E, R_ESC, SAT_A, SAT_C, T_MAX, MAX_ADV, variant=()):
        self.E, self.R, self.SA, self.SC, self.T, self.K = E, R_ESC, SAT_A, SAT_C, T_MAX, MAX_ADV
        self.v = frozenset(variant)
        self.genesis = ('G',)
        self.info = {self.genesis: dict(height=0, time=0, sched=None, path=(), escape=False)}

    # Block = (parent, producer, round). Its schedule follows section 19.2.
    def step(self, parent, rnd):
        """(schedule, is_escape) of a child of `parent` in round `rnd`."""
        h = self.info[parent]['height'] + 1
        if h == 1:  # R_h: bootstrap from Snapshot(R_h - 1)
            p = dict(start=1, cur=BOOT, nxt=BOOT, escapes=0, cum=0)
        else:
            p = dict(self.info[parent]['sched'])
            if h >= p['start'] + self.E:  # rotation, no certificate
                p['start'], p['cur'], p['nxt'], p['cum'] = h, p['nxt'], SNAP, 0
        if p['cum'] + rnd >= self.R:  # escape, applied last (cum stays 0 unless 'cumulative')
            p.update(start=h, cur=SNAP, nxt=SNAP, escapes=p['escapes'] + 1, cum=0)
            return p, True
        if 'cumulative' in self.v:
            p['cum'] += rnd
        return p, False

    def child_sched(self, parent, rnd):
        return self.step(parent, rnd)[0]

    def eligible(self, parent, key, rnd):
        p, escape = self.step(parent, rnd)
        if key not in p['cur']:
            return False
        if escape:
            return True
        return rnd >= (self.SA if key == A else self.SC)

    def add(self, parent, key, rnd):
        block = (parent, key, rnd)
        if block not in self.info:
            pi = self.info[parent]
            t = pi['time'] + rnd + 1
            sched, escape = self.step(parent, rnd)
            rank = ((0 if escape else 1, t, hash_of(block)) if 'escape_wins' in self.v
                    else (t, hash_of(block)))
            self.info[block] = dict(height=pi['height'] + 1, time=t, sched=sched,
                                    path=pi['path'] + (rank,), escape=escape)
        return block

    def _best(self, published):
        cands = list(published) + [self.genesis]
        top = max(self.info[b]['height'] for b in cands)
        return min((b for b in cands if self.info[b]['height'] == top), key=lambda b: self.info[b]['path'])

    def accept(self, published, proposed, t):
        """The published set honest nodes hold after a publication at step t."""
        if 'lateness' not in self.v:
            return proposed
        ok = set(published)
        for b in sorted(proposed - published, key=lambda b: self.info[b]['height']):
            path, x = [], b
            while x != self.genesis and x not in ok:
                path.append(x)
                x = x[0]
            if all(t <= self.info[y]['time'] + self.LATE for y in path):
                ok.update(path)
        return frozenset(ok)

    def chain(self, tip):
        out = set()
        while tip != self.genesis:
            out.add(tip)
            tip = tip[0]
        return out

    def honest_step(self, published, t):
        """C makes one block on the best tip if eligible at this step."""
        tip = self._best(published)
        rnd = t - self.info[tip]['time'] - 1
        if rnd >= 0 and self.eligible(tip, C, rnd):
            return published | {self.add(tip, C, rnd)}
        return published

    def check_schedule(self, block):
        """H-H1: recompute the schedule from genesis along the path."""
        path = []
        b = block
        while b != self.genesis:
            path.append(b)
            b = b[0]
        sched = None
        for blk in reversed(path):
            parent, _, rnd = blk
            sched = self.child_sched(parent, rnd)
        return sched == self.info[block]['sched']

    def run(self, silent=False):
        """Worst case over A's strategies, one (value, moves) pair per metric:
        stall, C blocks lost, deepest single reorg, minus the final height."""
        @functools.lru_cache(maxsize=None)
        def go(t, published, withheld, adv_used, c_seen, depth):
            # c_seen: C blocks that have been in the best chain at some step.
            # depth: the most C blocks one switch of the best tip has removed.
            if t > self.T:
                final = self.chain(self._best(published))
                lost = len([b for b in c_seen if b not in final])
                stays = [self.info[b]['time'] for b in final if b[1] == C]
                first = min(stays) if stays else self.T + 1
                height = self.info[self._best(published)]['height']
                return (first, (), lost, (), depth, (), -height, ())
            best = None
            for pub, wh, used, act in self.adversary_moves(t, published, withheld, adv_used, silent):
                before = self.chain(self._best(published))
                pub = self.accept(published, pub, t)
                p2 = self.honest_step(pub, t)
                tip = self._best(p2)
                now = self.chain(tip)
                removed = len([b for b in before if b[1] == C and b not in now])
                seen = c_seen | frozenset(b for b in now if b[1] == C)
                r = go(t + 1, p2, wh, used, seen, max(depth, removed))
                step = ((t, act, tip[1] if tip != self.genesis else '-', self.info[tip]['height']),)
                cand = (r[0], step + r[1], r[2], step + r[3], r[4], step + r[5], r[6], step + r[7])
                if best is None:
                    best = list(cand)
                    continue
                for i in (0, 2, 4, 6):  # each metric keeps the strategy that maximises it
                    if cand[i] > best[i]:
                        best[i], best[i + 1] = cand[i], cand[i + 1]
            return tuple(best)
        return go(1, frozenset(), frozenset(), 0, frozenset(), 0)

    def adversary_moves(self, t, published, withheld, used, silent):
        yield published, withheld, used, 'wait'  # do nothing
        if silent:
            return
        known = published | withheld | {self.genesis}
        for wset in self._subsets(withheld):  # publish any subset of withheld
            if wset:
                yield published | wset, withheld - wset, used, 'publish ' + self.names(wset)
        if used >= self.K:
            return
        for parent in known:
            rnd = t - self.info[parent]['time'] - 1
            if rnd < 0 or not self.eligible(parent, A, rnd):
                continue
            blk = self.add(parent, A, rnd)
            if blk in known:
                continue
            for wset in self._subsets(withheld):
                yield published | wset | {blk}, withheld - wset, used + 1, 'make+publish ' + self.name(blk) + (' publish ' + self.names(wset) if wset else '')  # publish now
            yield published, withheld | {blk}, used + 1, 'make+withhold ' + self.name(blk)  # withhold

    def name(self, b):
        i = self.info[b]
        return '%s@h%d/r%d/t%d' % (b[1], i['height'], b[2], i['time'])

    def names(self, bs):
        return ','.join(sorted(self.name(b) for b in bs))

    @staticmethod
    def _subsets(s):
        s = sorted(s, key=repr)
        for n in range(len(s) + 1):
            for c in itertools.combinations(s, n):
                yield frozenset(c)


def slow_producer(m):
    """A's simplest strategy: on every tip where C may not produce in a normal
    round, A produces in the last round that is not an escape (R_ESC - 1, or
    earlier under 'cumulative')."""
    published, trace = frozenset(), []
    for t in range(1, m.T + 1):
        tip = m._best(published)
        rnd = t - m.info[tip]['time'] - 1
        normal = [r for r in range(0, m.R) if not m.step(tip, r)[1]]
        c_normal = any(m.eligible(tip, C, r) for r in normal)
        if normal and rnd == normal[-1] and not c_normal and m.eligible(tip, A, rnd):
            published |= {m.add(tip, A, rnd)}
        published = m.honest_step(published, t)
        tip = m._best(published)
        trace.append(dict(t=t, height=m.info[tip]['height'], tip_by=tip[1] if tip != m.genesis else '-'))
    return trace


VARIANTS = ((), ('escape_wins',), ('lateness',), ('cumulative',), ('escape_wins', 'cumulative'))


def compare_fixes():
    grid = [(1, 3, 0, 0, 9, 3), (2, 3, 0, 0, 11, 3), (1, 4, 0, 0, 11, 3)]
    for variant in VARIANTS:
        label = '+'.join(variant) or 'current'
        for E, R, SA, SC, T, K in grid:
            silent = Model(E, R, SA, SC, T, K, variant).run(silent=True)
            m = Model(E, R, SA, SC, T, K, variant)
            worst = m.run()
            bad_sched = [b for b in m.info if b != m.genesis and not m.check_schedule(b)]
            print(json.dumps(dict(variant=label, E=E, R_ESC=R, T_MAX=T, MAX_ADV=K,
                                  first_kept_C_block_time_A_silent=silent[0],
                                  first_kept_C_block_time_worst=worst[0],
                                  C_blocks_lost_worst=worst[2], deepest_single_reorg_of_C_blocks=worst[4],
                                  lowest_final_height=-worst[6], final_height_A_silent=-silent[6],
                                  schedule_mismatches=len(bad_sched))), flush=True)
        for E, R in ((1, 3), (2, 3), (3, 4)):
            m = Model(E, R, 0, 0, 2 * E * R + 2 * R + 4, 99, variant)
            trace = slow_producer(m)
            c_first = next((row['t'] for row in trace if row['tip_by'] == C), None)
            print(json.dumps(dict(variant=label, strategy='slow_producer', E=E, R_ESC=R, steps=m.T,
                                  first_step_with_a_C_tip=c_first, final_height=trace[-1]['height'])),
                  flush=True)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--compare-fixes', action='store_true',
                        help='run the search under the candidate rule changes (see the module docstring)')
    if parser.parse_args().compare_fixes:
        return compare_fixes()
    grid = [
        # E, R_ESC, SAT_A, SAT_C, T_MAX, MAX_ADV
        (1, 3, 0, 0, 9, 3),
        (1, 3, 2, 0, 9, 3),
        (2, 3, 0, 0, 11, 3),
        (1, 4, 0, 0, 11, 3),
        (2, 2, 0, 0, 10, 4),
    ]
    first = True
    for E, R, SA, SC, T, K in grid:
        silent = Model(E, R, SA, SC, T, K).run(silent=True)
        m = Model(E, R, SA, SC, T, K)
        worst = m.run()
        bad_sched = [b for b in m.info if b != m.genesis and not m.check_schedule(b)]
        print(json.dumps(dict(E=E, R_ESC=R, SAT_A=SA, SAT_C=SC, T_MAX=T, MAX_ADV=K,
                              first_kept_C_block_time_A_silent=silent[0],
                              first_kept_C_block_time_worst=worst[0],
                              C_blocks_lost_worst=worst[2], deepest_single_reorg_of_C_blocks=worst[4],
                              blocks_explored=len(m.info) - 1, schedule_mismatches=len(bad_sched))), flush=True)
        if first:
            first = False
            print('  worst stall strategy (t, adversary action, tip producer, tip height):')
            for step in worst[1]:
                print('    ', step)
            print('  deepest reorg strategy:')
            for step in worst[5]:
                print('    ', step)
    for E, R in ((1, 3), (2, 3), (3, 4)):
        m = Model(E, R, 0, 0, 2 * E * R + 2 * R + 4, 99)
        trace = slow_producer(m)
        c_first = next((row['t'] for row in trace if row['tip_by'] == C), None)
        print(json.dumps(dict(strategy='slow_producer', E=E, R_ESC=R,
                              first_step_with_a_C_tip=c_first, bound_2E_times_R=2 * E * R)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
