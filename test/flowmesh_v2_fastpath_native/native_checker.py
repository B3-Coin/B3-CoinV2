"""Independent semantic observer for the native fixed-N4 recovery TEST harness.

No native/model verifier imported. Real BLS validation is done by the C++
workers; stdout issuance/guard facts are trusted test observations. This checker
does not claim independent cryptographic implementation or production auditing.
It retains ALL observed issued votes, including withheld/late evidence.
Unfenced status/guard snapshots are trusted observations of the worker, not
independent reads of synced disk. Counts cannot authenticate unseen signing
records or prove that particular records survived; process reopen tests and
the worker's storage validator cover that remaining boundary.
"""
from collections import defaultdict
from dataclasses import dataclass
import struct

KINDS = ('EMPTY', 'PREPARE', 'COMMIT', 'PC', 'FAST', 'SLOW', 'V0', 'EQ', 'REPORT', 'NEW_VIEW', 'PROPOSE')
SIGNED = {'PREPARE', 'COMMIT', 'REPORT', 'NEW_VIEW'}


@dataclass(frozen=True)
class Proof:
    kind: str
    view: int
    sender: int
    instance: str
    value: str | None
    items: tuple
    signature: bytes = bytes(96)


def decode(encoded):
    raw = bytes.fromhex(encoded)
    assert 0 < len(raw) <= 32768 and raw[0] == 1, 'codec bound/version'
    pos, count = 1, 0

    def take(n):
        nonlocal pos
        assert pos+n <= len(raw), 'truncated proof'
        out = raw[pos:pos+n]
        pos += n
        return out

    def node(depth):
        nonlocal count
        count += 1
        assert count <= 64 and depth <= 8, 'proof tree bound'
        tag, view, sender = struct.unpack('<Bii', take(9))
        assert tag < len(KINDS), 'kind'
        instance = take(32)[::-1].hex()
        has = take(1)[0]
        assert has <= 1, 'optional tag'
        value = take(32)[::-1].hex() if has else None
        children = take(1)[0]
        assert children <= 4, 'child bound'
        items = tuple(node(depth+1) for _ in range(children))
        return Proof(KINDS[tag], view, sender, instance, value, items, take(96))

    result = node(0)
    assert pos == len(raw), 'trailing bytes'
    return result


def encode(p):
    def node(p):
        return (struct.pack('<Bii', KINDS.index(p.kind), p.view, p.sender)
                + bytes.fromhex(p.instance)[::-1] + bytes([p.value is not None])
                + (bytes.fromhex(p.value)[::-1] if p.value is not None else b'')
                + bytes([len(p.items)]) + b''.join(node(x) for x in p.items) + p.signature)
    return (b'\x01'+node(p)).hex()


def empty(instance):
    return Proof('EMPTY', -1, -1, instance, None, ())


def walk(p):
    yield p
    for child in p.items:
        yield from walk(child)


class Checker:
    def __init__(self, instance, byzantine=()):
        self.instance = instance
        self.bad = set(byzantine)
        assert len(self.bad) <= 1 and self.bad <= set(range(4))
        self.issued = {}
        self.guards = {}
        self.authenticated = set()
        self.last_view = defaultdict(int)
        self.last_count = defaultdict(int)
        self.last_signed_count = defaultdict(int)
        self.obligations = {}
        self.decisions = set()
        self.audit_events = 0

    def proof(self, p, placeholder=None):
        assert p.instance == self.instance, 'foreign instance'
        assert len(p.items) <= 4
        if p.kind in SIGNED:
            assert 0 <= p.sender < 4 and 0 <= p.view <= 2
            assert p in self.authenticated or p == placeholder, 'unobserved signature evidence'
        else:
            assert p.signature == bytes(96), 'signed unsigned-wrapper'
        k, it = p.kind, p.items
        if k == 'EMPTY':
            assert p == empty(self.instance), 'noncanonical empty'
            return
        for child in it:
            self.proof(child, placeholder)
        if k in ('PREPARE', 'COMMIT'):
            assert p.value is not None and not it
            if k == 'COMMIT':
                assert p.view > 0, 'view0 commit disabled'
        elif k in ('PC', 'FAST', 'SLOW'):
            assert 0 <= p.view <= 2 and p.value is not None
            assert 3 <= len(it) <= 4 and len({x.sender for x in it}) == len(it), 'quorum/duplicates'
            phase = 'COMMIT' if k == 'SLOW' else 'PREPARE'
            assert all((x.kind,x.view,x.value) == (phase,p.view,p.value) for x in it)
            if k == 'FAST':
                assert p.view == 0 and 0 in {x.sender for x in it}
            if k == 'SLOW':
                assert p.view > 0
        elif k == 'V0':
            assert p.view == 0 and len(it) == 2 and it[0].sender == p.sender and it[1].sender == 0
            assert all((x.kind,x.view,x.value) == ('PREPARE',0,p.value) for x in it)
        elif k == 'EQ':
            assert p.view == 0 and p.sender == -1 and p.value is None and len(it) == 2
            assert all((x.kind,x.view,x.sender) == ('PREPARE',0,0) for x in it)
            assert it[0].value != it[1].value
        elif k == 'REPORT':
            assert p.view > 0 and p.value is None and len(it) == 3
            original, pc, decision = it
            assert original.kind == 'EMPTY' or original.kind == 'V0' and original.sender == p.sender
            assert pc.kind == 'EMPTY' or pc.kind == 'PC' and pc.view < p.view
            assert decision.kind == 'EMPTY' or decision.kind in ('FAST','SLOW') and decision.view < p.view
        elif k == 'NEW_VIEW':
            assert p.view > 0 and p.sender == p.view % 4 and len(it) == 4 and p.value is not None
            eq, *reports = it
            assert eq.kind in ('EQ','EMPTY')
            assert len({r.sender for r in reports}) == 3
            assert all(r.kind == 'REPORT' and r.view == p.view for r in reports)
            originals = {r.items[0].items[1].value for r in reports if r.items[0].kind == 'V0'}
            if eq.kind == 'EQ':
                originals.update(x.value for x in eq.items)
            equivocation = len(originals) > 1
            assert not equivocation or 0 not in {r.sender for r in reports}
            assert all(r.items[2].kind == 'EMPTY' for r in reports)
            pcs = [r.items[1] for r in reports if r.items[1].kind == 'PC']
            if pcs:
                highest = max(x.view for x in pcs)
                values = {x.value for x in pcs if x.view == highest}
                assert len(values) == 1 and p.value in values
            elif equivocation:
                counts = defaultdict(int)
                for r in reports:
                    if r.items[0].kind == 'V0':
                        counts[r.items[0].value] += 1
                eligible = {x for x,n in counts.items() if n >= 2}
                assert len(eligible) <= 1  # Three reports cannot carry 2+2.
                assert not eligible or p.value in eligible
            else:
                assert len(originals) <= 1 and (not originals or p.value in originals)
        elif k == 'PROPOSE':
            assert len(it) == 2 and p.sender == p.view % 4
            assert (it[0].kind,it[0].view,it[0].sender,it[0].value) == ('PREPARE',p.view,p.sender,p.value)
            if p.view == 0:
                assert it[1].kind == 'EMPTY'
            else:
                assert (it[1].kind,it[1].view,it[1].value) == ('NEW_VIEW',p.view,p.value)
        else:
            raise AssertionError('unknown proof')

    def obligation_fields(self, seat, fields):
        """Validate full evidence, without authenticating anything from a claim."""
        for name, kinds in (('v0', ('V0',)), ('highest', ('PC',)),
                            ('decision', ('FAST', 'SLOW'))):
            item = fields[name]
            self.proof(item)
            assert item.kind == 'EMPTY' or item.kind in kinds, 'wrong obligation kind: '+name
        original = fields['v0']
        if original.kind != 'EMPTY':
            assert original.sender == seat, 'wrong original owner'
            assert self.issued.get((seat, 'PREPARE', 0)) == original.items[0], 'original without observed local signing record'
            proposal = decode(self.guards[seat, 'PREPARE', 0]['proposal'])
            leader = proposal.items[0]
            # An allowed pre-sign leader placeholder becomes its exact issued vote.
            if leader.signature == bytes(96) and seat == 0:
                leader = original.items[0]
            assert original.items[1] == leader, 'changed original leader evidence'

    def retain_obligations(self, seat, fields):
        """Only call for a new signing prestate or an unfenced status snapshot."""
        self.obligation_fields(seat, fields)
        previous = self.obligations.get(seat, {k: empty(self.instance)
                                              for k in ('v0', 'highest', 'decision')})
        if previous['v0'].kind != 'EMPTY':
            assert fields['v0'] == previous['v0'], 'lost/changed original vote obligation'
        highest, old_highest = fields['highest'], previous['highest']
        if old_highest.kind != 'EMPTY':
            assert highest.kind == 'PC' and highest.view >= old_highest.view, 'lost/regressed highest PC obligation'
            assert highest.view != old_highest.view or highest.value == old_highest.value, 'changed highest PC value'
        decision, old_decision = fields['decision'], previous['decision']
        if old_decision.kind != 'EMPTY':
            assert decision.kind in ('FAST', 'SLOW') and decision.value == old_decision.value, 'lost/changed decision obligation'
        self.obligations[seat] = {k: fields[k] for k in previous}
        if decision.kind != 'EMPTY':
            self.decisions.add(decision.value)
            assert len(self.decisions) <= 1, 'conflicting retained decisions'

    def issued_event(self, seat, event):
        p = decode(event['issued'])
        assert p.kind in SIGNED and p.sender == seat
        self.authenticated.add(p)
        if seat in self.bad:
            self.proof(p)
            self.hidden()
            return
        g = event['guard']
        key = seat,p.kind,p.view
        if key in self.issued:
            assert self.issued[key] == p and self.guards[key] == g, 'changed retransmission/guard'
            return
        assert not g['fenced'] and g['view'] == p.view
        assert self.last_view[seat] <= g['pre_view'] <= p.view, 'new old-view signature'
        self.last_view[seat] = p.view
        fields = {k: decode(g[k]) if g[k] else empty(self.instance)
                  for k in ('proposal','new_view','prepared','highest','v0','decision')}
        # Only the leader's OWN first PREPARE may be an unsigned planned
        # placeholder in its pre-sign proposal. Never a remote signature.
        placeholder = None
        if p.kind == 'PREPARE' and seat == p.view % 4 and fields['proposal'].items:
            candidate = fields['proposal'].items[0]
            if candidate.signature == bytes(96):
                assert (candidate.kind,candidate.view,candidate.sender,candidate.value) == (p.kind,p.view,p.sender,p.value)
                placeholder = candidate
        for item in fields.values():
            self.proof(item, placeholder)
        self.proof(p)
        self.retain_obligations(seat, fields)
        assert fields['new_view'].kind in ('EMPTY', 'NEW_VIEW'), 'wrong new-view guard kind'
        if fields['new_view'].kind != 'EMPTY':
            assert fields['new_view'].view == p.view, 'wrong new-view guard target'
        if p.kind in ('PREPARE','COMMIT'):
            assert not g['changing'] and g['pre_view'] == p.view and g['body_available']
            proposal = fields['proposal']
            assert (proposal.kind,proposal.view,proposal.value) == ('PROPOSE',p.view,p.value)
            if p.view:
                assert fields['new_view'] == proposal.items[1]
            else:
                assert fields['new_view'].kind == 'EMPTY', 'view-zero new-view guard'
            if p.kind == 'COMMIT':
                pc = fields['prepared']
                assert (pc.kind,pc.view,pc.value) == ('PC',p.view,p.value)
                highest = fields['highest']
                assert highest.kind == 'PC' and highest.view >= pc.view, 'commit lost prepared PC obligation'
                assert highest.view != pc.view or highest.value == pc.value, 'commit highest PC conflict'
            else:
                assert fields['prepared'].kind == 'EMPTY', 'prepare with prepared guard'
            assert fields['decision'].kind == 'EMPTY' or fields['decision'].value == p.value
        elif p.kind == 'REPORT':
            assert g['changing'] and g['pre_view'] < p.view
            assert not g['body_available'] and fields['proposal'].kind == fields['prepared'].kind == 'EMPTY'
            assert p.items == tuple(fields[k] for k in ('v0','highest','decision')), 'untruthful report'
        elif p.kind == 'NEW_VIEW':
            assert g['changing'] and g['pre_view'] == p.view and fields['decision'].kind == 'EMPTY'
            assert not g['body_available'] and fields['proposal'].kind == fields['prepared'].kind == 'EMPTY'
        self.issued[key] = p
        self.guards[key] = g
        if p.kind == 'PREPARE' and p.view == 0:
            leader = fields['proposal'].items[0]
            if leader == placeholder:
                leader = p
            self.obligations[seat]['v0'] = Proof('V0', 0, seat, self.instance, p.value, (p, leader))
        self.audit_events += 1
        self.hidden()

    def hidden(self):
        votes = defaultdict(set)
        for p in self.authenticated:
            if p.kind in ('PREPARE','COMMIT'):
                votes[p.kind,p.view,p.value].add(p.sender)
        for (phase,view,value), voters in votes.items():
            if len(voters) >= 3 and ((phase == 'PREPARE' and view == 0 and 0 in voters)
                                    or (phase == 'COMMIT' and view > 0)):
                self.decisions.add(value)
        assert len(self.decisions) <= 1, 'hidden conflicting decisions'

    def status(self, seat, status):
        if seat in self.bad:
            return
        assert status['seat'] == seat and status['instance'] == self.instance, 'status identity/instance'
        # A failed sync may leave novel, undurable in-memory fields in the
        # fenced response. Do not promote them into authenticated/durable facts.
        if status['fenced']:
            return
        assert status['view'] >= self.last_view[seat], 'view regressed'
        self.last_view[seat] = status['view']
        fields = {k: decode(status[k]) if status[k] else empty(self.instance)
                  for k in ('v0', 'highest', 'decision')}
        self.retain_obligations(seat, fields)
        signed_count = status['signed_count']
        observed = sum(author == seat for author, _, _ in self.issued)
        assert type(signed_count) is int and signed_count >= max(observed, self.last_signed_count[seat]), 'lost signing records/count'
        self.last_signed_count[seat] = signed_count
        count = status['apply_count']
        assert type(count) is int and self.last_count[seat] <= count <= 1, 'application duplicated/regressed'
        assert (status['applied'] is None) == (count == 0), 'application/count mismatch'
        self.last_count[seat] = count
        if count:
            value = status['applied']
            assert fields['decision'].kind in ('FAST', 'SLOW') and fields['decision'].value == value, 'application without matching decision certificate'
            self.decisions.add(value)
            assert value and len(self.decisions) == 1, 'applied different decision'
