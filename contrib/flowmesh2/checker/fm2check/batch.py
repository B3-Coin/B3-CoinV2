# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Seeded batches of slots (fm2-trace-batch/1) for M3-ENG-05: random, adversarial,
dust, caps and slotcap. The batch is the only input shared by fm2-trace and the model."""

import hashlib
import random

from . import bip340
from .codec import (
    BUY, SELL, GTC, IOC, POST_ONLY, KIND_PLACE, KIND_CANCEL, KIND_CANCEL_ALL, KIND_TRANSFER, NATIVE, Action, Market, account_id,
    cancel_payload, deposit_fact, place_payload, test_asset, transfer_payload,
)

FORMAT = 'fm2-trace-batch/1'
BASE_HEIGHT = 100
RUSD, TOK = test_asset('rUSD'), test_asset('TOK')
MARKETS = (Market(NATIVE, RUSD, 1_000_000, 1), Market(TOK, RUSD, 1, 10))
SLOTCAP_MAKERS = 17  # 17,000 resting lots: more than max_fills_per_slot (16,384)
KINDS = ('random', 'adversarial', 'dust', 'caps', 'slotcap')


def h(text):
    return hashlib.sha256(text.encode()).digest()


class Raw:
    """Original bytes that are not a well-formed action."""

    def __init__(self, data):
        self.data = data

    def encode(self):
        return self.data


class Key:
    def __init__(self, i):
        self.sec = h('fm2/check/key/%d' % i)
        self.pub = bip340.pubkey(self.sec)
        self.account = account_id(self.pub)
        self.next_nonce = 0


class Generator:
    def __init__(self, kind, seed, slots=12, users=6):
        assert kind in KINDS
        self.kind, self.rng = kind, random.Random('%s/%d' % (kind, seed))
        self.domain = h('fm2/check/domain')
        self.keys = [Key(i) for i in range(users if kind != 'slotcap' else SLOTCAP_MAKERS + 2)]
        self.slots_wanted = slots
        self.height = 0          # the height the next slot proposes, minus one (accepted slots only)
        self.anchor = BASE_HEIGHT
        self.facts_to = (BASE_HEIGHT + 1, 0)
        self.outpoints = 0
        self.orders = []         # ActionIds of PLACE actions seen so far
        self.funded = set()

    # ---- pieces ----
    def amount(self, big, deposit=False):
        r = self.rng
        if self.kind == 'dust' and not deposit:
            return r.choice((1, 2, 3, 7, 9, 10, 11, 99, 101, 9_999))
        return r.randrange(1, big)

    def deposit(self, cursor, key, asset, amount):
        self.outpoints += 1
        return deposit_fact(cursor[0], cursor[1], h('fm2/check/tx/%d' % self.outpoints), self.rng.randrange(4),
                            key.account, asset, amount)

    def sign(self, key, kind, payload, nonce=None, ref_slot=None, max_delay=None, domain=None):
        if nonce is None:
            nonce = key.next_nonce
            key.next_nonce += 1
        a = Action(domain or self.domain, key.pub, nonce, self.height if ref_slot is None else ref_slot,
                   self.rng.choice((1, 2, 10, 1024)) if max_delay is None else max_delay, kind, payload)
        a.sig = bip340.sign(a.id(), key.sec, self.rng.randbytes(32))
        return a

    def place(self, key):
        r = self.rng
        m = r.choice(MARKETS)
        side = r.choice((BUY, SELL))
        tif = r.choice((GTC, GTC, IOC, POST_ONLY))
        if self.kind == 'dust':
            price, lots = r.choice((1, 2, 3, 5)), r.choice((1, 1, 2, 3))
        elif m is MARKETS[0]:
            price, lots = r.randrange(95, 106), r.randrange(1, 30)
        else:
            price, lots = r.randrange(9, 13), r.randrange(1, 5_000)
        a = self.sign(key, KIND_PLACE, place_payload(m.id(), side, tif, price, lots))
        self.orders.append(a.id())
        return a

    def action(self, key):
        r = self.rng.random()
        if r < 0.15 and self.orders:
            return self.sign(key, KIND_CANCEL, cancel_payload(self.rng.choice(self.orders)))
        if r < 0.25:
            to = self.rng.choice(self.keys)
            return self.sign(key, KIND_TRANSFER, transfer_payload(to.account, self.rng.choice((NATIVE, RUSD, TOK)),
                                                                  self.amount(5_000_000)))
        return self.place(key)

    # ---- adversarial mutations ----
    def adversarial(self, slot, actions, traders):
        """Edit the slot or its actions; True iff the whole slot is now invalid.
        The other mutations leave a valid slot whose action gets a typed refusal."""
        r = self.rng
        choice = r.randrange(25)
        keys = [k for k in self.keys if k.account in traders]
        if not keys:
            return False
        k = r.choice(keys)
        place = place_payload(MARKETS[0].id(), BUY, GTC, 100, 1)
        if choice == 0 and actions:
            a = actions[0]
            a.sig = a.sig[:10] + bytes([a.sig[10] ^ 1]) + a.sig[11:]      # bad signature
            return True
        if choice == 1 and k.next_nonce > 0:
            actions.append(self.sign(k, KIND_PLACE, place, nonce=0))        # nonce already used
            return True
        if choice == 2 and actions:
            actions.append(actions[0])                                        # duplicate ActionId
            return True
        if choice == 3:
            slot['anchor'] = {'height': str(self.parent_anchor),              # the parent's anchor, forked
                              'hash': h('fm2/check/fork/%d' % self.parent_anchor).hex()}
            return True
        if choice == 4:
            slot['facts_from'] = [str(self.facts_to[0]), str(self.facts_to[1] + 1)]
            return True
        if choice == 5:
            actions.append(self.sign(k, KIND_PLACE, place, ref_slot=self.height + 1))   # not yet valid
            return True
        if choice == 10:
            actions.append(self.sign(k, KIND_PLACE, place, max_delay=0))
            return True
        if choice == 13:
            actions.append(self.sign(k, KIND_PLACE, place, domain=h('other domain')))
            return True
        facts = [bytes.fromhex(f) for f in slot['facts']]
        if choice == 17 and facts:
            # A second deposit of an outpoint this slot already credits.
            last = (int.from_bytes(facts[-1][1:5], 'little'), int.from_bytes(facts[-1][5:9], 'little'))
            dup = facts[0][:1] + last[0].to_bytes(4, 'little') + (last[1] + 1).to_bytes(4, 'little') + facts[0][9:]
            slot['facts'].append(dup.hex())
            end = (last[0], last[1] + 2)
            if end > (int(slot['facts_to'][0]), int(slot['facts_to'][1])):
                slot['facts_to'] = [str(end[0]), str(end[1])]
            return True
        if choice == 18:
            slot['facts_to'] = [str(int(slot['anchor']['height']) + 2), '0']        # beyond the anchor
            return True
        if choice == 19 and len(facts) >= 2:
            slot['facts'][0], slot['facts'][1] = slot['facts'][1], slot['facts'][0]  # cursors out of order
            return True
        if choice == 20 and facts:
            kind = r.choice((0, 2, 3, 4, 6, 8, 255))                                 # a stub kind, or no kind
            slot['facts'][0] = (bytes([kind]) + facts[0][1:]).hex()
            return True
        if choice == 21 and facts:
            slot['facts'][0] = (facts[0] + b'\x00').hex()                          # one byte too long
            return True
        if choice == 22:
            actions.append(self.sign(k, KIND_CANCEL_ALL, b''))                       # a known kind, unsupported
            return True
        if choice == 23:
            actions.append(Raw(self.sign(k, KIND_PLACE, place).encode()[:-1]))      # truncated
            return True
        if choice == 24:
            actions.append(self.sign(k, KIND_TRANSFER, transfer_payload(r.choice(self.keys).account, RUSD, 0)))
            return False
        refused = {
            6: lambda: self.sign(k, KIND_PLACE, place_payload(h('no market'), BUY, GTC, 100, 1)),
            7: lambda: self.sign(k, KIND_PLACE, place_payload(MARKETS[1].id(), SELL, GTC, 1, 1 << 62)),
            8: lambda: self.sign(k, KIND_TRANSFER, transfer_payload(k.account, RUSD, 5)),
            9: lambda: self.sign(k, KIND_TRANSFER, transfer_payload(h('nobody'), RUSD, 5)),
            11: lambda: self.sign(k, KIND_PLACE, place_payload(MARKETS[0].id(), BUY, GTC, 0, 1)),
            12: lambda: self.sign(k, KIND_CANCEL, cancel_payload(h('no order'))),
            14: lambda: self.sign(k, KIND_TRANSFER, transfer_payload(r.choice(self.keys).account, TOK, 1 << 61)),
            16: lambda: self.sign(k, KIND_TRANSFER, transfer_payload(r.choice(self.keys).account, h('no asset'), 1)),
        }.get(choice, lambda: self.sign(k, KIND_PLACE, place_payload(MARKETS[1].id(), BUY, POST_ONLY, 1 << 40, 1 << 20)))
        actions.append(refused())
        return False

    # ---- slots ----
    def slot(self):
        r = self.rng
        self.parent_anchor = self.anchor
        if r.random() < 0.7:
            self.anchor += 1
        # Facts of block b are visible once the anchor reaches b: a range may end
        # anywhere up to (anchor + 1, 0). Deposits sit in the cursor's block.
        facts, cursor = [], self.facts_to
        facts_to = cursor
        traders = set(self.funded)  # an account exists only after the slot that credits it
        if cursor[0] <= self.anchor:
            first = len(self.funded) < len(self.keys)
            for i in range(len(self.keys) if first else r.randrange(0, 3)):
                key = self.keys[i] if first else r.choice(self.keys)
                for asset, big in ((NATIVE, 10**12), (RUSD, 10**10), (TOK, 10**7)):
                    facts.append(self.deposit(cursor, key, asset, self.amount(big, deposit=True)))
                    cursor = (cursor[0], cursor[1] + 1)
                self.funded.add(key.account)
            facts_to = (self.anchor + 1, 0) if r.random() < 0.8 else cursor
        actions = []
        if self.height > 0:
            for key in r.sample(self.keys, r.randrange(1, len(self.keys) + 1)):
                if key.account in traders:
                    for _ in range(r.randrange(1, 4)):
                        actions.append(self.action(key))
        slot = {
            'anchor': {'height': str(self.anchor), 'hash': h('fm2/check/block/%d' % self.anchor).hex()},
            'facts_from': [str(self.facts_to[0]), str(self.facts_to[1])],
            'facts_to': [str(facts_to[0]), str(facts_to[1])],
            'facts': [f.hex() for f in facts],
            'cert_hash': r.randbytes(32).hex(),
        }
        invalid = False
        if self.kind == 'adversarial' and self.height > 0 and r.random() < 0.5:
            invalid = self.adversarial(slot, actions, traders)
        r.shuffle(actions)
        slot['originals'] = [a.encode().hex() for a in actions]
        return slot, facts_to, invalid

    def caps_slots(self):
        """The caps kind: two makers rest 1-lot asks until the open-order cap
        refuses them, then takers sweep more makers than max_fills_per_order."""
        r = self.rng
        a, b, c, d = self.keys[:4]
        slots = [self.plain_slot(deposits=[(k, asset, amount) for k in self.keys
                                           for asset, amount in ((NATIVE, 10**13), (RUSD, 10**12))], actions=[])]
        price = r.randrange(95, 100)
        for _ in range(6):
            actions = [self.sign(k, KIND_PLACE, place_payload(MARKETS[0].id(), SELL, POST_ONLY, price + r.randrange(2), 1))
                       for k in (a, b) for _ in range(200)]
            slots.append(self.plain_slot([], actions))
        takers = [self.sign(c, KIND_PLACE, place_payload(MARKETS[0].id(), BUY, GTC, price + 1, 1500)),
                  self.sign(d, KIND_PLACE, place_payload(MARKETS[0].id(), BUY, IOC, price + 1, r.randrange(1, 900)))]
        slots.append(self.plain_slot([], takers))
        return slots

    def slotcap_slots(self):
        """The slotcap kind: SLOTCAP_MAKERS makers rest 1,000 one-lot asks each
        at two prices, then one slot of 17 takers of 1,024 lots each asks for more
        fills than max_fills_per_slot. Pro-rata keeps every taker near 1,000
        fills, below max_fills_per_order, so the slot budget stops the last ones."""
        r = self.rng
        makers, takers = self.keys[:SLOTCAP_MAKERS], self.keys[SLOTCAP_MAKERS:]
        slots = [self.plain_slot(deposits=[(k, NATIVE, 10**13) for k in makers] + [(k, RUSD, 10**13) for k in takers],
                                 actions=[])]
        price = r.randrange(95, 100)
        for k in makers:
            actions = [self.sign(k, KIND_PLACE, place_payload(MARKETS[0].id(), SELL, POST_ONLY, price + i % 2, 1))
                       for i in range(1000)]
            slots.append(self.plain_slot([], actions))
        sweep = [self.sign(takers[i % 2], KIND_PLACE, place_payload(MARKETS[0].id(), BUY, (GTC, IOC)[i % 2], price + 1, 1024))
                 for i in range(17)]
        slots.append(self.plain_slot([], sweep))
        return slots

    def plain_slot(self, deposits, actions):
        """One valid slot at the next anchor: these deposits, these actions."""
        self.anchor += 1
        facts, cursor = [], self.facts_to
        for key, asset, amount in deposits:
            facts.append(self.deposit(cursor, key, asset, amount))
            cursor = (cursor[0], cursor[1] + 1)
        facts_to = (self.anchor + 1, 0)
        slot = {
            'anchor': {'height': str(self.anchor), 'hash': h('fm2/check/block/%d' % self.anchor).hex()},
            'facts_from': [str(self.facts_to[0]), str(self.facts_to[1])],
            'facts_to': [str(facts_to[0]), str(facts_to[1])],
            'facts': [f.hex() for f in facts], 'cert_hash': self.rng.randbytes(32).hex(),
            'originals': [x.encode().hex() for x in actions],
        }
        self.height += 1
        self.facts_to = facts_to
        return slot

    def batch(self):
        fixed = {'caps': self.caps_slots, 'slotcap': self.slotcap_slots}.get(self.kind)
        slots = fixed() if fixed else []
        for _ in range(self.slots_wanted if not fixed else 0):
            snapshot = (self.anchor, [k.next_nonce for k in self.keys], self.funded.copy(), list(self.orders))
            slot, facts_to, invalid = self.slot()
            slots.append(slot)
            if not invalid:
                # The slot should be accepted. Either way both sides see it, and
                # a wrong guess here only makes later slots refused, not unchecked.
                self.height += 1
                self.facts_to = facts_to
            else:
                self.anchor, nonces, self.funded, self.orders = snapshot
                for k, n in zip(self.keys, nonces):
                    k.next_nonce = n
        return {
            'format': FORMAT, 'domain': self.domain.hex(), 'authority_epoch': '1',
            'seat_set_root': h('fm2/check/seats').hex(),
            'genesis_anchor': {'height': str(BASE_HEIGHT), 'hash': h('fm2/check/block/%d' % BASE_HEIGHT).hex()},
            'facts_start': [str(BASE_HEIGHT + 1), '0'],
            'markets': [{'base': m.base.hex(), 'quote': m.quote.hex(), 'lot': str(m.lot), 'tick': str(m.tick)}
                        for m in MARKETS],
            'slots': slots,
        }
