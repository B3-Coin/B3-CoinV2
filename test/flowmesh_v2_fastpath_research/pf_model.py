"""P2-FV message/restart TEST model. Not a node or a production protocol.

One abstract executed batch, fixed synthetic membership, ideal authentication,
atomic durable memory and explicit publication. No measured latency or disk I/O.
The previously frozen evidence-only probe remains separate.
"""
from copy import deepcopy
from dataclasses import dataclass
from itertools import combinations
import random


SIGNED = frozenset({'PREPARE', 'COMMIT', 'REPORT', 'NEW_VIEW'})
VALUES = frozenset({'x', 'y'})


class Invalid(ValueError):
    pass


class Cut(Exception):
    pass


@dataclass(frozen=True)
class Packet:
    kind: str
    view: int
    sender: int
    value: str | None
    items: tuple = ()
    instance: str = 'P2FV-MESSAGE-TEST/1'


def walk(packet):
    yield packet
    for item in packet.items:
        yield from walk(item)


class Auth:
    def __init__(self):
        self.computed = set()
        self.published = set()

    def compute(self, packet):
        self.computed.add(packet)
        return packet


class World:
    def __init__(self, n=4, byzantine=(), max_view=2):
        if n not in (4, 5, 7):
            raise ValueError('TEST_MEMBERSHIP_ONLY')
        self.n = n
        self.f = (n - 1) // 3
        self.q = n - self.f
        self.t = min(self.f, (n - 3 * self.f + 1) // 2)
        self.fast = n - self.t
        self.instance = f'P2FV-MESSAGE-TEST/1/n={n}'
        self.byzantine = set(byzantine)
        if not self.byzantine <= set(range(n)) or len(self.byzantine) > self.f:
            raise ValueError('FAULT_BUDGET')
        self.max_view = max_view
        self.auth = Auth()
        self.now = 0
        self.nodes = [Node(self, i) for i in range(n)]
        self.queue = []
        self.publications = []
        self.steps = 0
        self.dropped = 0
        self.max_queue = 0

    def packet(self, kind, view=-1, sender=-1, value=None, items=()):
        return Packet(kind, view, sender, value, tuple(items), self.instance)

    def empty(self):
        return self.packet('EMPTY')

    def authentic(self, p, kind=None):
        if (type(p) is not Packet or p.instance != self.instance or
                p.kind not in SIGNED or p not in self.auth.computed or
                type(p.sender) is not int or not 0 <= p.sender < self.n or
                type(p.view) is not int or not 0 <= p.view <= self.max_view or
                (kind is not None and p.kind != kind)):
            raise Invalid('AUTH_OR_CONTEXT')

    def vote(self, p, kind):
        self.authentic(p, kind)
        if p.items or p.value not in VALUES:
            raise Invalid('VOTE_SHAPE')

    def certificate(self, p):
        if (type(p) is not Packet or p.instance != self.instance or
                p.kind not in ('PC', 'FAST', 'SLOW') or not p.items or
                p.value not in VALUES or not 0 <= p.view <= self.max_view):
            raise Invalid('CERTIFICATE_SHAPE')
        kind = 'COMMIT' if p.kind == 'SLOW' else 'PREPARE'
        senders = set()
        for vote in p.items:
            self.vote(vote, kind)
            if vote.view != p.view or vote.value != p.value or vote.sender in senders:
                raise Invalid('CERTIFICATE_MATCH_OR_DUPLICATE')
            senders.add(vote.sender)
        if len(senders) < (self.fast if p.kind == 'FAST' else self.q):
            raise Invalid('CERTIFICATE_QUORUM')
        if p.kind == 'FAST' and (p.view != 0 or 0 not in senders):
            raise Invalid('FAST_CONTEXT')
        if p.kind == 'SLOW' and p.view == 0 and self.fast == self.q:
            raise Invalid('NO_SLOW_VIEW_ZERO_WHEN_FAST_EQUALS_Q')
        return p

    def report(self, p):
        self.authentic(p, 'REPORT')
        if p.view < 1 or p.value is not None or len(p.items) != 3:
            raise Invalid('REPORT_SHAPE')
        v0, pc, decision = p.items
        if v0.kind != 'EMPTY':
            if v0.kind != 'V0' or len(v0.items) != 2 or v0.instance != self.instance:
                raise Invalid('REPORT_V0_SHAPE')
            own, leader = v0.items
            self.vote(own, 'PREPARE')
            self.vote(leader, 'PREPARE')
            if (own.view != 0 or leader.view != 0 or own.sender != p.sender or
                    leader.sender != 0 or own.value != leader.value):
                raise Invalid('REPORT_V0_LINK')
        if pc.kind != 'EMPTY':
            self.certificate(pc)
            if pc.kind != 'PC' or pc.view >= p.view:
                raise Invalid('REPORT_PC_CONTEXT')
        if decision.kind != 'EMPTY':
            self.certificate(decision)
            if decision.kind not in ('FAST', 'SLOW') or decision.view >= p.view:
                raise Invalid('REPORT_DECISION_CONTEXT')

    def choices(self, view, reports, eq):
        if len(reports) != self.q or len({r.sender for r in reports}) != self.q:
            raise Invalid('REPORT_SET')
        originals = []
        for report in reports:
            self.report(report)
            if report.view != view:
                raise Invalid('REPORT_TARGET')
            if report.items[0].kind != 'EMPTY':
                originals.append(report.items[0].items[1])
        if eq.kind != 'EMPTY':
            if eq.kind != 'EQ' or len(eq.items) != 2 or eq.instance != self.instance:
                raise Invalid('EQUIVOCATION_SHAPE')
            for p in eq.items:
                self.vote(p, 'PREPARE')
                if p.sender != 0 or p.view != 0:
                    raise Invalid('EQUIVOCATION_CONTEXT')
            if eq.items[0].value == eq.items[1].value:
                raise Invalid('NOT_EQUIVOCATION')
            originals.extend(eq.items)
        equivocation = len({p.value for p in originals}) > 1
        # TEST choice: global exclusion also precedes PC selection. Not ratified
        # economic/mainnet policy; the research text's precedence is ambiguous.
        if equivocation and any(r.sender == 0 for r in reports):
            raise Invalid('EXCLUDE_EQUIVOCATING_OLD_LEADER')
        if any(r.items[2].kind != 'EMPTY' for r in reports):
            raise Invalid('APPLY_DECISION_INSTEAD')
        pcs = [r.items[1] for r in reports if r.items[1].kind != 'EMPTY']
        if pcs:
            highest = max(p.view for p in pcs)
            values = {p.value for p in pcs if p.view == highest}
            if len(values) != 1:
                raise Invalid('CONFLICTING_HIGHEST_PC')
            return values
        if equivocation:
            counts = {v: sum(r.items[0].kind == 'V0' and
                            r.items[0].items[0].value == v for r in reports)
                      for v in VALUES}
            selected = sorted(v for v in VALUES if counts[v] >= self.f + self.t)
            return set(selected[:1]) or set(VALUES)
        return {p.value for p in originals} or set(VALUES)

    def new_view(self, p):
        self.authentic(p, 'NEW_VIEW')
        if p.view < 1 or p.sender != p.view % self.n or len(p.items) != self.q + 1:
            raise Invalid('NEW_VIEW_SHAPE')
        if p.value not in self.choices(p.view, p.items[1:], p.items[0]):
            raise Invalid('NEW_VIEW_CHOICE')

    def proposal(self, p):
        if p.kind != 'PROPOSE' or len(p.items) != 2 or p.instance != self.instance:
            raise Invalid('PROPOSAL_SHAPE')
        leader, nv = p.items
        self.vote(leader, 'PREPARE')
        if (p.view != leader.view or p.value != leader.value or
                p.sender != p.view % self.n or p.sender != leader.sender):
            raise Invalid('PROPOSAL_LEADER_OR_VALUE')
        if p.view == 0:
            if nv.kind != 'EMPTY':
                raise Invalid('VIEW_ZERO_NEW_VIEW')
        else:
            self.new_view(nv)
            if nv.view != p.view or nv.value != p.value:
                raise Invalid('PROPOSAL_NEW_VIEW_LINK')

    def ingress(self, p):
        if not isinstance(p, Packet) or p.instance != self.instance:
            raise Invalid('INSTANCE')
        if any(x.kind in SIGNED and x not in self.auth.published for x in walk(p)):
            raise Invalid('UNPUBLISHED_SIGNATURE_NOT_NETWORK_EVIDENCE')

    def emit(self, node, packet, targets=None):
        for p in walk(packet):
            if p.kind not in SIGNED or p in self.auth.published:
                continue
            if p.sender != node.id or node.d['signed'].get((p.kind, p.view)) != p:
                raise Invalid('PUBLICATION_WITHOUT_DURABLE_SIGNATURE')
            self.auth.published.add(p)
        self.publications.append({'sender': node.id, 'packet': packet,
                                  'durable': deepcopy(node.d)})
        self.enqueue(packet, targets)

    def enqueue(self, packet, targets=None):
        self.queue.extend((i, packet) for i in
                          (range(self.n) if targets is None else targets))
        self.max_queue = max(self.max_queue, len(self.queue))
        if len(self.queue) > 20000:
            raise AssertionError('BOUNDED_TEST_QUEUE_EXHAUSTED')

    def adversary(self, kind, view, sender, value=None, items=(), targets=None):
        if sender not in self.byzantine:
            raise Invalid('ADVERSARY_CANNOT_SIGN_FOR_HONEST_SEAT')
        p = self.auth.compute(self.packet(kind, view, sender, value, items))
        self.auth.published.add(p)
        self.enqueue(p, targets)
        return p

    def pump(self, limit=5000, seed=None, drop=None):
        rng = random.Random(seed)
        work = 0
        while self.queue and work < limit:
            i = rng.randrange(len(self.queue)) if seed is not None else 0
            recipient, packet = self.queue.pop(i)
            node = self.nodes[recipient]
            work += 1
            if recipient in self.byzantine or not node.online or (drop and drop(recipient, packet)):
                self.dropped += 1
                continue
            node.receive(packet)
        self.steps += work
        if work == limit and self.queue:
            raise AssertionError('BOUNDED_DELIVERY_LIMIT')
        return work

    def submit(self, value='x', via=0):
        self.nodes[via].body(value)
        self.enqueue(self.packet('DATA', value=value))

    def tick(self, steps=1):
        self.now += steps
        for node in self.nodes:
            if node.online and node.id not in self.byzantine:
                node.timer()


class Node:
    def __init__(self, world, identity):
        self.w, self.id = world, identity
        self.d = {'view': 0, 'mode': 'ACTIVE', 'accepted': {}, 'new_views': {},
                  'highest': None, 'v0': None, 'signed': {}, 'guards': {},
                  'decision': None, 'applied': None, 'apply_count': 0,
                  'fenced': False, 'bodies': set()}
        self.online = True
        self.cut = None
        self.rejections = []
        self.reason = ''
        self._volatile()

    def _volatile(self):
        self.votes = {}
        self.reports = {}
        self.pending = set()
        self.sent_proofs = set()
        self.deadline = self.w.now + 3
        self.last_retry = self.w.now

    def failpoint(self, point):
        if self.cut == point:
            self.cut = None
            self.online = False
            raise Cut(point)

    def guard(self, proposal=None, prepared=None, view=None, mode=None):
        return {'pre_view': self.d['view'],
                'view': self.d['view'] if view is None else view,
                'mode': self.d['mode'] if mode is None else mode,
                'proposal': proposal, 'new_view': self.d['new_views'].get(
                    self.d['view'] if view is None else view),
                'prepared': prepared, 'highest': self.d['highest'],
                'v0': self.d['v0'], 'decision': self.d['decision'],
                'fenced': self.d['fenced'],
                'body_available': proposal is not None and proposal.value in self.d['bodies']}

    def sign(self, packet, guard, changes=(), root=None):
        slot = (packet.kind, packet.view)
        old = self.d['signed'].get(slot)
        if old is not None:
            if old != packet:
                raise Invalid('DURABLE_SIGNING_SLOT_ALREADY_USED')
            return old
        if self.d['fenced']:
            raise Invalid('FENCED')
        self.failpoint('before_compute')
        self.w.auth.compute(packet)
        self.failpoint('after_compute_before_durable')
        d = deepcopy(self.d)
        d.update(changes)
        d['signed'][slot] = packet
        d['guards'][packet] = deepcopy(guard)
        self.d = d  # Atomic durable-memory boundary, NOT physical storage.
        self.failpoint('after_durable_before_publish')
        self.w.emit(self, root or packet)
        self.failpoint('after_publish')
        return packet

    def body(self, value):
        if value not in VALUES:
            raise Invalid('UNKNOWN_ABSTRACT_EXECUTED_VALUE')
        self.d['bodies'].add(value)

    def propose(self, value):
        view = self.d['view']
        if (self.id != view % self.w.n or self.d['mode'] != 'ACTIVE' or
                self.d['decision'] or value not in self.d['bodies']):
            return
        if ('PREPARE', view) in self.d['signed']:
            return
        nv = self.d['new_views'].get(view, self.w.empty())
        vote = self.w.packet('PREPARE', view, self.id, value)
        p = self.w.packet('PROPOSE', view, self.id, value, (vote, nv))
        # Validate NEW_VIEW before computing the original leader vote.
        if view:
            self.w.new_view(nv)
            if nv.value != value:
                raise Invalid('LEADER_MUST_PROPOSE_NEW_VIEW_VALUE')
        accepted = dict(self.d['accepted'])
        accepted[view] = p
        changes = {'accepted': accepted}
        if view == 0:
            changes['v0'] = self.w.packet('V0', 0, self.id, value, (vote, vote))
        self.sign(vote, self.guard(p), changes, root=p)

    def receive(self, packet):
        if not self.online or self.d['fenced']:
            return
        try:
            self.w.ingress(packet)
            kind = packet.kind
            if kind == 'DATA':
                self.body(packet.value)
                pending = sorted(self.pending, key=lambda p: (p.view, p.kind, p.value or ''))
                self.pending.clear()
                for p in pending:
                    self.receive(p)
                if self.d['decision']:
                    self.apply()
                elif self.d['view'] == 0:
                    self.propose(packet.value)
                elif self.d['view'] in self.d['new_views']:
                    self.propose(self.d['new_views'][self.d['view']].value)
                elif self.d['mode'] == 'CHANGING' and self.id == self.d['view'] % self.w.n:
                    # Requested body arrival completes the existing view work;
                    # do not wait for a retry timer or a new client request.
                    self.make_view(self.d['view'])
            elif kind == 'NEED':
                if packet.value in self.d['bodies']:
                    self.w.enqueue(self.w.packet('DATA', value=packet.value), [packet.sender])
            elif kind == 'PROPOSE':
                self.on_proposal(packet)
            elif kind in ('PREPARE', 'COMMIT'):
                self.w.vote(packet, kind)
                self.add_vote(packet)
                self.aggregate(packet.view, packet.value)
            elif kind == 'PC':
                self.on_pc(packet)
            elif kind in ('FAST', 'SLOW'):
                self.decide(packet)
            elif kind == 'REPORT':
                self.on_report(packet)
            elif kind == 'NEW_VIEW':
                self.install_view(packet)
            else:
                raise Invalid('UNKNOWN_MESSAGE')
        except Invalid as error:
            self.rejections.append(str(error))
        except Cut:
            pass

    def require_body(self, packet):
        if packet.value in self.d['bodies']:
            return True
        self.pending.add(packet)
        self.w.enqueue(self.w.packet('NEED', sender=self.id, value=packet.value))
        return False

    def on_proposal(self, p):
        self.w.proposal(p)
        if not self.require_body(p):
            return
        if p.view > 0 and p.view >= self.d['view']:
            self.install_view(p.items[1], drive=False)
        self.add_vote(p.items[0])
        view = self.d['view']
        if p.view != view or self.d['mode'] != 'ACTIVE' or self.d['decision']:
            self.aggregate(p.view, p.value)
            return
        old = self.d['accepted'].get(view)
        if old is not None and old.value != p.value:
            raise Invalid('ACCEPTED_DIFFERENT_PROPOSAL')
        if ('PREPARE', view) not in self.d['signed']:
            vote = self.w.packet('PREPARE', view, self.id, p.value)
            accepted = dict(self.d['accepted'])
            accepted[view] = p
            changes = {'accepted': accepted}
            if view == 0:
                changes['v0'] = self.w.packet('V0', 0, self.id, p.value, (vote, p.items[0]))
            self.sign(vote, self.guard(p), changes)
        self.aggregate(p.view, p.value)

    def add_vote(self, p):
        key = (p.kind, p.view, p.value)
        self.votes.setdefault(key, {})[p.sender] = p

    def aggregate(self, view, value):
        prepares = self.votes.get(('PREPARE', view, value), {})
        commits = self.votes.get(('COMMIT', view, value), {})
        if view == 0 and len(prepares) >= self.w.fast and 0 in prepares:
            self.decide(self.w.packet('FAST', view, value=value,
                                     items=tuple(prepares[s] for s in sorted(prepares))))
        if len(prepares) >= self.w.q:
            self.on_pc(self.w.packet('PC', view, value=value,
                                    items=tuple(prepares[s] for s in sorted(prepares))))
        if len(commits) >= self.w.q and (view > 0 or self.w.fast > self.w.q):
            self.decide(self.w.packet('SLOW', view, value=value,
                                     items=tuple(commits[s] for s in sorted(commits))))

    def on_pc(self, p):
        self.w.certificate(p)
        if p.kind != 'PC':
            raise Invalid('NOT_PC')
        if not self.require_body(p):
            return
        highest = self.d['highest']
        if highest and highest.view == p.view and highest.value != p.value:
            self.d['fenced'] = True
            raise Invalid('CONFLICTING_PC_SAFETY_STOP')
        if highest is None or p.view > highest.view:
            self.d['highest'] = p
        key = ('PC', p.view, p.value)
        if key not in self.sent_proofs:
            self.sent_proofs.add(key)
            self.w.emit(self, p)
        view = self.d['view']
        accepted = self.d['accepted'].get(view)
        if (self.d['mode'] == 'ACTIVE' and p.view == view and accepted is not None and
                accepted.value == p.value and not self.d['decision'] and
                (view > 0 or self.w.fast > self.w.q)):
            vote = self.w.packet('COMMIT', view, self.id, p.value)
            self.sign(vote, self.guard(accepted, p))

    def decide(self, p):
        self.w.certificate(p)
        if p.kind not in ('FAST', 'SLOW'):
            raise Invalid('NOT_DECISION')
        if not self.require_body(p):
            return
        if self.d['decision'] and self.d['decision'].value != p.value:
            self.d['fenced'] = True
            raise Invalid('CONFLICTING_DECISION')
        if self.d['decision'] is None:
            self.d['decision'] = p
            self.failpoint('after_decision_before_apply')
        self.apply()
        key = ('DECISION', p.value)
        if key not in self.sent_proofs:
            self.sent_proofs.add(key)
            self.w.emit(self, self.d['decision'])

    def apply(self):
        p = self.d['decision']
        if not p or p.value not in self.d['bodies'] or self.d['applied'] is not None:
            return
        self.failpoint('during_apply_before_atomic_commit')
        d = deepcopy(self.d)
        d['applied'], d['apply_count'] = p.value, 1
        self.d = d
        self.failpoint('after_apply')

    def change_view(self, target):
        if (target <= self.d['view'] or target > self.w.max_view or self.d['decision'] or
                self.d['fenced'] or not self.online):
            return
        items = tuple(p or self.w.empty() for p in
                      (self.d['v0'], self.d['highest'], self.d['decision']))
        r = self.w.packet('REPORT', target, self.id, items=items)
        g = self.guard(view=target, mode='CHANGING')
        self.sign(r, g, {'view': target, 'mode': 'CHANGING'})
        self.deadline = self.w.now + 3

    def on_report(self, r):
        self.w.report(r)
        self.reports.setdefault(r.view, {}).setdefault(r.sender, r)
        if r.items[2].kind != 'EMPTY':
            self.decide(r.items[2])
        if (r.view > self.d['view'] and len(self.reports[r.view]) >= self.w.f + 1):
            self.change_view(r.view)
        if self.id == r.view % self.w.n and r.view == self.d['view']:
            self.make_view(r.view)

    def make_view(self, view):
        reports = self.reports.get(view, {})
        if self.d['decision'] or ('NEW_VIEW', view) in self.d['signed']:
            return
        if self.d['mode'] != 'CHANGING' or len(reports) < self.w.q:
            return
        originals = {r.items[0].items[1] for r in reports.values() if r.items[0].kind == 'V0'}
        pair = sorted(originals, key=lambda p: p.value)
        eq = self.w.packet('EQ', 0, items=(pair[0], pair[-1])) if len(pair) > 1 else self.w.empty()
        for selection in combinations(sorted(reports.values(), key=lambda r: r.sender), self.w.q):
            try:
                choices = self.w.choices(view, selection, eq)
            except Invalid:
                continue
            have = sorted(choices & self.d['bodies'])
            if not have:
                for value in sorted(choices):
                    self.w.enqueue(self.w.packet('NEED', sender=self.id, value=value))
                return
            nv = self.w.packet('NEW_VIEW', view, self.id, have[0], (eq, *selection))
            self.sign(nv, self.guard())
            return

    def install_view(self, nv, drive=True):
        self.w.new_view(nv)
        if nv.view < self.d['view'] or self.d['decision']:
            return
        old = self.d['new_views'].get(nv.view)
        if old is not None and old != nv:
            raise Invalid('DIFFERENT_NEW_VIEW_ALREADY_INSTALLED')
        if not self.require_body(nv):
            return
        self.d['view'] = nv.view
        self.d['mode'] = 'ACTIVE'
        self.d['new_views'][nv.view] = nv
        # An individual old PC does not veto the validated NEW_VIEW rule.
        if drive:
            self.propose(nv.value)

    def timer(self):
        if not self.online or self.d['fenced']:
            return
        if self.w.now >= self.deadline and self.d['bodies'] and not self.d['decision']:
            if self.d['view'] < self.w.max_view:
                try:
                    self.change_view(self.d['view'] + 1)
                except Cut:
                    return
            else:
                self.reason = 'TEST_VIEW_BOUND_REACHED_WITH_OBLIGATIONS_PRESERVED'
        if self.w.now > self.last_retry:
            self.last_retry = self.w.now
            self.retransmit()
            if self.d['mode'] == 'CHANGING' and self.id == self.d['view'] % self.w.n:
                try:
                    self.make_view(self.d['view'])
                except Cut:
                    return

    def retransmit(self):
        if self.d['decision']:
            self.w.emit(self, self.d['decision'])
            return
        for (kind, view), packet in sorted(self.d['signed'].items()):
            # Bounded test profile: at most four slots * three views.
            if kind == 'PREPARE' and self.id == view % self.w.n:
                self.w.emit(self, self.d['accepted'][view])
            else:
                self.w.emit(self, packet)
        for packet in sorted(self.pending, key=lambda p: (p.view, p.kind, p.value or '')):
            self.w.enqueue(self.w.packet('NEED', sender=self.id, value=packet.value))

    def crash(self):
        self.online = False
        self._volatile()

    def restart(self):
        self._volatile()
        self.online = True
        try:
            for slot, packet in self.d['signed'].items():
                self.w.authentic(packet)
                guard = self.d['guards'].get(packet)
                if slot != (packet.kind, packet.view) or guard is None:
                    raise Invalid('MISSING_DURABLE_SIGNATURE_JUSTIFICATION')
                if packet.kind in ('PREPARE', 'COMMIT'):
                    proposal = guard['proposal']
                    if proposal is None or proposal.value not in self.d['bodies']:
                        raise Invalid('MISSING_DURABLE_PROPOSAL_OR_BODY')
                    self.w.proposal(proposal)
                    if packet.kind == 'COMMIT':
                        self.w.certificate(guard['prepared'])
                elif packet.kind == 'REPORT':
                    self.w.report(packet)
                elif packet.kind == 'NEW_VIEW':
                    self.w.new_view(packet)
            if self.d['decision']:
                self.w.certificate(self.d['decision'])
                self.apply()
            if not self.d['fenced']:
                self.retransmit()
        except (Invalid, AttributeError, KeyError, TypeError) as error:
            self.d['fenced'] = True
            self.reason = 'RESTART_SAFE_REFUSAL:' + str(error)
        except Cut:
            pass
