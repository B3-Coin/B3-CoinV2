# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""An independent model of the FlowMesh V2 engine (M3-ENG-05), written from
src/flowmesh2/ARCHITECTURE.md section 11 and the PROVISIONAL decisions D4,
D5 and D7 of src/flowmesh2/TODO.md. Profile fm2-dex1-regtest-provisional/1.

It shares no code with the C++ engine. Where the spec text leaves a choice
open, the choice made here is marked GAP; contrib/flowmesh2/checker/README.md
lists what the comparison with the engine showed for each.
"""

import collections
import copy

from . import bip340
from .codec import (
    BUY, SELL, GTC, IOC, POST_ONLY, KIND_PLACE, KIND_CANCEL, KIND_TRANSFER, MAX_ATOMS, U64_MAX, ZERO32,
    SRC_FACT, SRC_ACTION, SRC_SLOT, DEPOSIT_CREDITED, RESTING, FILLED, PARTIAL_RESTING, IOC_DONE, CANCELLED,
    TRANSFERRED, FEES_DISTRIBUTED, REJ_INSUFFICIENT_AVAILABLE, REJ_UNKNOWN_MARKET, REJ_UNKNOWN_ORDER,
    REJ_NOT_OWN_ORDER, REJ_WOULD_CROSS, REJ_FILL_CAP, REJ_OPEN_ORDER_CAP, REJ_BAD_PARAMS,
    REJ_NO_RECIPIENT, REJ_RESIDUAL_CROSS_UNSUPPORTED,
    Action, DecodeError, account_leaf_bytes, action_leaf, decode_cancel, decode_deposit, decode_place,
    decode_transfer, fact_leaf, merkle_root, nullifier_leaf_bytes, order_leaf_bytes, outcome_leaf, seat_pool,
    state_root_v0, tagged, totals_leaf_bytes, treasury, u64,
)

# Profile fm2-dex1-regtest-provisional/1 (PROFILE_DEX1_REGTEST.json).
# Why each fill-capped taker stopped, over every slot the model applied
# (diagnostic only; fm2_check prints it so a caps run shows which cap fired).
CAP_STOPS = collections.Counter()

PROFILE = {
    'fee_ppm': 100, 'seat_share_percent': 80, 'max_actions_per_slot': 1024, 'max_action_payload_bytes': 4096,
    'max_fills_per_order': 1024, 'max_fills_per_slot': 16384, 'max_open_orders_per_account': 1000,
    'nonce_window': 100, 'max_delay_slots': 1024,
}
MAX_SLOT_FACTS = 1024  # core/body.h, PROVISIONAL (O-27)
MAX_FACT_BYTES = 1024  # core/body.h, PROVISIONAL (O-27)
# Fact kinds whose codec is a stub: the slot fails with the stub's error (its task id), not INVALID_INPUT.
STUB_FACT_KINDS = {2: 'M7-SEAT-01', 3: 'M7-GOV-01', 4: 'MB1-B3D-02', 6: 'M8-EXIT-01'}
SUPPORTED_KINDS = (KIND_PLACE, KIND_CANCEL, KIND_TRANSFER)


class Invalid(Exception):
    """The slot validity predicate refused the slot: INVALID_INPUT with a SLOT_* detail,
    or a stub's NOT_IMPLEMENTED with its task id."""

    def __init__(self, detail, code='INVALID_INPUT'):
        super().__init__(detail)
        self.detail, self.code = detail, code


# SLOT_* details, in precedence order (engine/engine.h names them as the interface).
SLOT_PROFILE_MISMATCH = 'slot profile mismatch'
SLOT_DOMAIN_MISMATCH = 'slot domain mismatch'
SLOT_HEIGHT_EXHAUSTED = "parent height has no successor"
SLOT_ANCHOR_BELOW_PARENT = "anchor below the parent's anchor"
SLOT_ANCHOR_FORK = "anchor at the parent's height with another hash"
SLOT_FACTS_NOT_CONTIGUOUS = "facts not contiguous with the parent's facts_to"
SLOT_FACTS_REVERSED = 'facts_to before facts_from'
SLOT_FACTS_BEYOND_ANCHOR = 'fact range beyond the anchor'
SLOT_FACT_COUNT_CAP = 'more facts than MAX_SLOT_FACTS'
SLOT_FACT_BYTES_CAP = 'fact longer than MAX_FACT_BYTES'
SLOT_ACTION_COUNT_CAP = 'more originals than max_actions_per_slot'
SLOT_ACTION_BYTES_CAP = 'original longer than the action byte cap'
SLOT_FACT_MALFORMED = 'fact does not decode'
SLOT_FACT_KIND = 'fact kind the engine does not apply'
SLOT_FACT_CURSOR = 'fact cursor outside the range or out of order'
SLOT_DEPOSIT_REFUSED = 'deposit fact the ledger would refuse'
SLOT_ACTION_MALFORMED = 'original does not decode'
SLOT_ACTION_DOMAIN = 'action domain mismatch'
SLOT_ACTION_KIND = 'unsupported action kind'
SLOT_ACTION_PAYLOAD = 'action payload does not decode'
SLOT_ACTION_WINDOW = 'action outside its window'
SLOT_DUPLICATE_ACTION = 'duplicate ActionId'
SLOT_DUPLICATE_NONCE = 'two originals with one (account, nonce)'
SLOT_ACCOUNT_ABSENT = 'account absent in the parent state'
SLOT_NONCE_EXCLUDED = 'nonce excluded by the parent window'
SLOT_BAD_SIGNATURE = 'invalid action signature'

# Largest ActionV2 encoding: fixed prefix, CompactSize of the payload cap, payload, signature.
MAX_ACTION_BYTES = 1 + 32 + 32 + 8 + 8 + 2 + 1 + 3 + PROFILE['max_action_payload_bytes'] + 64


# ---- Nonce window (D7) -----------------------------------------------------

def nonce_acceptable(used, nonce, window):
    """`used` is ascending and holds at most `window` nonces: a nonce is new and
    either the window has room or it is above the lowest kept nonce."""
    if nonce in used:
        return False
    return len(used) < window or nonce > used[0]


def nonce_insert(used, nonce, window):
    out = sorted(used + [nonce])
    if len(out) > window:
        out.pop(0)
    return out


# ---- State -------------------------------------------------------------------

class State:
    def __init__(self, markets):
        self.accounts = {}    # AccountId -> {'bal': {asset: [available, reserved]}, 'nonces': [...], 'open': n}
        self.totals = {}      # AssetId -> [deposits, effects, pending, fees_held]
        self.nullifiers = set()
        self.markets = {m.id(): m for m in markets}
        self.orders = {}      # OrderId -> resting order dict (RestingOrderLeafV0 fields)

    def bal(self, account, asset):
        return self.accounts[account]['bal'].setdefault(asset, [0, 0])

    def ensure_account(self, account):
        return self.accounts.setdefault(account, {'bal': {}, 'nonces': [], 'open': 0})

    def account_leaves(self):
        # A leaf holds no all-zero balance (core/account_leaf.h).
        return [account_leaf_bytes(a, {k: tuple(v) for k, v in rec['bal'].items() if v != [0, 0]}, rec['nonces'], rec['open'])
                for a, rec in sorted(self.accounts.items())]

    def roots(self):
        acc = [tagged('STATE_ACCOUNTS', leaf) for leaf in self.account_leaves()]
        orders = [tagged('STATE_ORDERS', order_leaf_bytes(o)) for _, o in sorted(self.orders.items())]
        markets = [m.leaf_value() for _, m in sorted(self.markets.items())]
        nulls = [tagged('STATE_NULLIFIERS', nullifier_leaf_bytes(t, v)) for t, v in sorted(self.nullifiers)]
        # No all-zero totals either (ledger PutTotals).
        tots = [tagged('STATE_TOTALS', totals_leaf_bytes(a, *t)) for a, t in sorted(self.totals.items()) if any(t)]
        r = {'accounts': merkle_root(acc), 'orders': merkle_root(orders), 'markets': merkle_root(markets),
             'nullifiers': merkle_root(nulls), 'totals': merkle_root(tots)}
        r['state'] = state_root_v0(r['accounts'], r['orders'], r['markets'], r['nullifiers'], r['totals'])
        return r


def reservation(market, side, price, lots):
    """(asset, atoms): a BUY holds quote at its own price, a SELL holds base. None above MAX_ATOMS."""
    if side == BUY:
        q = lots * price
        atoms = q * market.tick if q <= MAX_ATOMS else MAX_ATOMS + 1
        return market.quote, (atoms if atoms <= MAX_ATOMS else None)
    atoms = lots * market.lot
    return market.base, (atoms if atoms <= MAX_ATOMS else None)


def quote_atoms(market, lots, price):
    return lots * price * market.tick


def fee_total(q, ppm):
    return -(-(q * ppm) // 1_000_000)


# ---- The slot validity predicate (section 11, "As landed in R7") -----------

def canonical_order(parent_hash, height, actions):
    """Sort by Tagged(ORDER, parent_hash || u64 height || ActionId), then give each
    account's positions to its actions in ascending nonce. Returns indices."""
    # GAP: keys compare as raw bytes (memcmp), as hashlib prints them.
    keyed = sorted(range(len(actions)), key=lambda i: tagged('ORDER', parent_hash + u64(height) + actions[i].id()))
    by_account = {}
    for i in keyed:
        by_account.setdefault(actions[i].account(), []).append(i)
    for idxs in by_account.values():
        idxs.sort(key=lambda i: actions[i].nonce)
    taken = {a: iter(v) for a, v in by_account.items()}
    return [next(taken[actions[i].account()]) for i in keyed]


def validate(parent, inputs):
    """Returns (deposits, actions, order) or raises Invalid."""
    head, st = parent['header'], parent['state']
    if inputs['profile_hash'] != head['profile_hash']:
        raise Invalid(SLOT_PROFILE_MISMATCH)
    if inputs['domain'] != head['domain']:
        raise Invalid(SLOT_DOMAIN_MISMATCH)
    if head['height'] == U64_MAX:
        raise Invalid(SLOT_HEIGHT_EXHAUSTED)
    a_h, a_hash = inputs['anchor']
    if a_h < head['anchor_height']:
        raise Invalid(SLOT_ANCHOR_BELOW_PARENT)
    if a_h == head['anchor_height'] and a_hash != head['anchor_hash']:
        raise Invalid(SLOT_ANCHOR_FORK)
    f_from, f_to = inputs['facts_from'], inputs['facts_to']
    if f_from != head['facts_to']:
        raise Invalid(SLOT_FACTS_NOT_CONTIGUOUS)
    if f_to < f_from:
        raise Invalid(SLOT_FACTS_REVERSED)
    # GAP: "nothing above the anchor" read as: the range ends at or before (anchor+1, 0).
    if f_to > (a_h + 1, 0):
        raise Invalid(SLOT_FACTS_BEYOND_ANCHOR)
    facts, originals = inputs['facts'], inputs['originals']
    if len(facts) > MAX_SLOT_FACTS:
        raise Invalid(SLOT_FACT_COUNT_CAP)
    if any(len(f) > MAX_FACT_BYTES for f in facts):
        raise Invalid(SLOT_FACT_BYTES_CAP)
    if len(originals) > PROFILE['max_actions_per_slot']:
        raise Invalid(SLOT_ACTION_COUNT_CAP)
    if any(len(o) > MAX_ACTION_BYTES for o in originals):
        raise Invalid(SLOT_ACTION_BYTES_CAP)

    deposits, last, seen_out, credit = [], None, set(), {}
    for f in facts:
        if not f:
            raise Invalid(SLOT_FACT_MALFORMED)
        if f[0] in STUB_FACT_KINDS:
            # GAP (filled from engine/engine.cpp): a stub kind's error propagates unchanged.
            raise Invalid(STUB_FACT_KINDS[f[0]], code='NOT_IMPLEMENTED')
        if f[0] != 1:
            # Kinds 5 (payout) and 7 (source range) are outside DEX-1 and outside this
            # model; the batches never carry them. Any other kind does not decode.
            if f[0] in (5, 7):
                raise NotImplementedError('fact kind %d is outside the model' % f[0])
            raise Invalid(SLOT_FACT_MALFORMED)
        try:
            cursor, outpoint, account, asset, amount = decode_deposit(f)
        except DecodeError:
            raise Invalid(SLOT_FACT_MALFORMED)
        if not (f_from <= cursor < f_to) or (last is not None and cursor <= last):
            raise Invalid(SLOT_FACT_CURSOR)
        last = cursor
        # GAP (filled from engine/engine.cpp): refused are a non-positive amount, a
        # protocol account, an outpoint already credited and one repeated in the slot.
        # A balance or total above MAX_ATOMS is an error (O-34) whose code is outside the model.
        protocol = (seat_pool(head['profile_hash']), treasury(head['profile_hash']))
        if amount <= 0 or account in protocol or outpoint in st.nullifiers or outpoint in seen_out:
            raise Invalid(SLOT_DEPOSIT_REFUSED)
        seen_out.add(outpoint)
        held = st.accounts.get(account, {'bal': {}})['bal'].get(asset, [0, 0])
        k = (account, asset)
        credit[k] = credit.get(k, held[0]) + amount
        tot = st.totals.get(asset, [0, 0, 0, 0])[0] + sum(d[4] for d in deposits if d[3] == asset) + amount
        if credit[k] > MAX_ATOMS or tot > MAX_ATOMS:
            raise NotImplementedError('a deposit above MAX_ATOMS is outside the model')
        deposits.append((cursor, outpoint, account, asset, amount, f))

    height = head['height'] + 1
    actions, payloads, ids, pairs = [], [], set(), set()
    for o in originals:
        try:
            a = Action.decode(o, PROFILE['max_action_payload_bytes'])
        except DecodeError:
            raise Invalid(SLOT_ACTION_MALFORMED)
        if a.domain != head['domain']:
            raise Invalid(SLOT_ACTION_DOMAIN)
        if a.kind not in SUPPORTED_KINDS:
            raise Invalid(SLOT_ACTION_KIND)
        try:
            payload = {KIND_PLACE: decode_place, KIND_CANCEL: decode_cancel, KIND_TRANSFER: decode_transfer}[a.kind](a.payload)
        except DecodeError:
            raise Invalid(SLOT_ACTION_PAYLOAD)
        if not (1 <= a.max_delay <= PROFILE['max_delay_slots']) or not (height > a.ref_slot and height - a.ref_slot <= a.max_delay):
            raise Invalid(SLOT_ACTION_WINDOW)
        if a.id() in ids:
            raise Invalid(SLOT_DUPLICATE_ACTION)
        ids.add(a.id())
        if (a.account(), a.nonce) in pairs:
            raise Invalid(SLOT_DUPLICATE_NONCE)
        pairs.add((a.account(), a.nonce))
        if a.account() not in st.accounts:
            raise Invalid(SLOT_ACCOUNT_ABSENT)
        if not nonce_acceptable(st.accounts[a.account()]['nonces'], a.nonce, PROFILE['nonce_window']):
            raise Invalid(SLOT_NONCE_EXCLUDED)
        actions.append(a)
        payloads.append(payload)
    # The ascending insert of each account's nonces.
    windows = {}
    for a in sorted(actions, key=lambda x: (x.account(), x.nonce)):
        w = windows.get(a.account(), st.accounts[a.account()]['nonces'])
        if not nonce_acceptable(w, a.nonce, PROFILE['nonce_window']):
            raise Invalid(SLOT_NONCE_EXCLUDED)
        windows[a.account()] = nonce_insert(w, a.nonce, PROFILE['nonce_window'])
    for a in actions:
        if not bip340.verify(a.id(), a.signer, a.sig):
            raise Invalid(SLOT_BAD_SIGNATURE)
    order = canonical_order(parent['hash'], height, actions)
    return deposits, actions, payloads, order


# ---- Execution (phases 1-9) --------------------------------------------------

POST_ONLY_BLOCK, REST_BLOCK = 0, 1


def arrival_rank(height, block, rank):
    # GAP: ArrivalRank(height, phase, rank) is not given by the text: a block of
    # max_actions_per_slot ranks per (height, phase), post-only before rest.
    return (height * 2 + block) * PROFILE['max_actions_per_slot'] + rank


class Slot:
    def __init__(self, parent, deposits, actions, payloads, order, height, profile_hash):
        self.st = copy.deepcopy(parent['state'])
        self.deposits, self.actions, self.payloads, self.order = deposits, actions, payloads, order
        self.height, self.profile_hash = height, profile_hash
        self.outcomes = {}        # canonical rank -> outcome
        self.fact_outcomes = []
        self.fill_budget = PROFILE['max_fills_per_slot']

    def outcome(self, rank, code, remaining=0, fills=(), deltas=()):
        a = self.actions[self.order[rank]]
        self.outcomes[rank] = {'source': SRC_ACTION, 'subject': a.id(), 'account': a.account(), 'nonce': a.nonce,
                               'code': code, 'remaining': remaining, 'fills': list(fills), 'deltas': list(deltas)}

    def run(self):
        st = self.st
        for a in (self.actions[i] for i in self.order):
            rec = st.accounts[a.account()]
            rec['nonces'] = nonce_insert(rec['nonces'], a.nonce, PROFILE['nonce_window'])
        self.phase_deposits()
        kinds = [(r, self.actions[i], self.payloads[i]) for r, i in enumerate(self.order)]
        for r, a, p in kinds:
            if a.kind == KIND_TRANSFER:
                self.transfer(r, a, p)
        for r, a, p in kinds:
            if a.kind == KIND_CANCEL:
                self.cancel(r, a, p)
        for r, a, p in kinds:
            if a.kind == KIND_PLACE and p[2] == POST_ONLY:
                self.post_only(r, a, p)
        takers = [self.reserve_taker(r, a, p) for r, a, p in kinds if a.kind == KIND_PLACE and p[2] != POST_ONLY]
        takers = [t for t in takers if t is not None]
        for mid in sorted({t['market'] for t in takers}):
            self.sweep(mid, [t for t in takers if t['market'] == mid])
        self.phase_rest(takers)
        fee_outcomes = self.phase_fees()
        self.check_conservation()
        return self.fact_outcomes + [self.outcomes[r] for r in range(len(self.order))] + fee_outcomes

    # Phase 1
    def phase_deposits(self):
        st = self.st
        for cursor, outpoint, account, asset, amount, raw in self.deposits:
            st.ensure_account(account)
            st.bal(account, asset)[0] += amount
            st.totals.setdefault(asset, [0, 0, 0, 0])[0] += amount
            st.nullifiers.add(outpoint)
            # GAP: a FACT outcome's subject is the FactId; nonce 0.
            self.fact_outcomes.append({'source': SRC_FACT, 'subject': fact_leaf(raw), 'account': account, 'nonce': 0,
                                       'code': DEPOSIT_CREDITED, 'remaining': 0, 'fills': [],
                                       'deltas': [delta(account, asset, amount, 0)]})

    # Phase 2
    def transfer(self, rank, a, p):
        st = self.st
        to, asset, amount = p
        frm = a.account()
        # O-7: BAD_PARAMS (self or zero), then NO_RECIPIENT, then INSUFFICIENT_AVAILABLE.
        # An asset nobody holds is simply not available: there is no UNKNOWN_ASSET here.
        if to == frm or amount == 0:
            return self.outcome(rank, REJ_BAD_PARAMS)
        if to not in st.accounts:
            return self.outcome(rank, REJ_NO_RECIPIENT)
        held = st.accounts[frm]['bal'].get(asset, [0, 0])
        if held[0] < amount:
            return self.outcome(rank, REJ_INSUFFICIENT_AVAILABLE)
        if st.accounts[to]['bal'].get(asset, [0, 0])[0] + amount > MAX_ATOMS:
            # O-34: an error, so the slot has no candidate; its code is outside the model.
            raise NotImplementedError('a transfer above MAX_ATOMS is outside the model')
        st.bal(frm, asset)[0] -= amount
        st.bal(to, asset)[0] += amount
        self.outcome(rank, TRANSFERRED, deltas=net_deltas([(frm, asset, -amount, 0), (to, asset, amount, 0)]))

    # Phase 3
    def cancel(self, rank, a, order_id):
        st = self.st
        o = st.orders.get(order_id)
        if o is None:
            return self.outcome(rank, REJ_UNKNOWN_ORDER)
        if o['owner'] != a.account():
            return self.outcome(rank, REJ_NOT_OWN_ORDER)
        asset = o['asset']
        st.bal(o['owner'], asset)[0] += o['reserved_left']
        st.bal(o['owner'], asset)[1] -= o['reserved_left']
        st.accounts[o['owner']]['open'] -= 1
        del st.orders[order_id]
        # GAP: the spec does not say; the engine gives CANCELLED remaining_lots 0
        # (only RESTING and PARTIAL_RESTING carry lots).
        self.outcome(rank, CANCELLED,
                     deltas=[delta(o['owner'], asset, o['reserved_left'], -o['reserved_left'])])

    def precheck_place(self, rank, a, p):
        """The PLACE refusals shared by post-only and takers, up to the cross check."""
        mid, side, tif, price, lots = p
        m = self.st.markets.get(mid)
        if m is None:
            self.outcome(rank, REJ_UNKNOWN_MARKET)
            return None
        asset, atoms = reservation(m, side, price, lots)
        if price == 0 or lots == 0 or atoms is None:
            self.outcome(rank, REJ_BAD_PARAMS)
            return None
        return m, asset, atoms

    def best_opposite(self, mid, side):
        """The resting orders a `side` order would meet, best first (price, then arrival)."""
        opp = SELL if side == BUY else BUY
        book = [o for o in self.st.orders.values() if o['market'] == mid and o['side'] == opp]
        if opp == SELL:
            book.sort(key=lambda o: (o['price'], o['arrival_rank']))
        else:
            book.sort(key=lambda o: (-o['price'], o['arrival_rank']))
        return book

    def crosses(self, side, limit, maker_price):
        return maker_price <= limit if side == BUY else maker_price >= limit

    def reserve(self, rank, a, asset, atoms, takes_slot):
        st = self.st
        rec = st.accounts[a.account()]
        if takes_slot and rec['open'] >= PROFILE['max_open_orders_per_account']:
            self.outcome(rank, REJ_OPEN_ORDER_CAP)
            return False
        if rec['bal'].get(asset, [0, 0])[0] < atoms:
            self.outcome(rank, REJ_INSUFFICIENT_AVAILABLE)
            return False
        st.bal(a.account(), asset)[0] -= atoms
        st.bal(a.account(), asset)[1] += atoms
        if takes_slot:
            rec['open'] += 1
        return True

    # Phase 4
    def post_only(self, rank, a, p):
        pre = self.precheck_place(rank, a, p)
        if pre is None:
            return
        m, asset, atoms = pre
        mid, side, tif, price, lots = p
        book = self.best_opposite(mid, side)
        if book and self.crosses(side, price, book[0]['price']):
            return self.outcome(rank, REJ_WOULD_CROSS)
        if not self.reserve(rank, a, asset, atoms, True):
            return
        self.st.orders[a.id()] = new_order(a, mid, side, price, lots, arrival_rank(self.height, POST_ONLY_BLOCK, rank),
                                           atoms, asset)
        self.outcome(rank, RESTING, remaining=lots, deltas=[delta(a.account(), asset, -atoms, atoms)])

    # Phase 5
    def reserve_taker(self, rank, a, p):
        pre = self.precheck_place(rank, a, p)
        if pre is None:
            return None
        m, asset, atoms = pre
        mid, side, tif, price, lots = p
        if not self.reserve(rank, a, asset, atoms, tif == GTC):
            return None
        return {'rank': rank, 'action': a, 'id': a.id(), 'owner': a.account(), 'market': mid, 'side': side, 'tif': tif,
                'price': price, 'lots': lots, 'remaining': lots, 'reserved_left': atoms, 'reserved_at_place': atoms,
                'asset': asset,
                'cum_quote': 0, 'fee_charged': 0, 'fills': [], 'deltas': {}, 'stop': 'NO_CROSS'}

    def sweep(self, mid, takers):
        m = self.st.markets[mid]
        for side in (BUY, SELL):
            group = sorted((t for t in takers if t['side'] == side), key=lambda t: t['rank'])
            while self.fill_budget > 0:
                book = self.best_opposite(mid, side)
                if not book:
                    break
                level_price = book[0]['price']
                eligible = [t for t in group if t['remaining'] > 0 and self.crosses(side, t['price'], level_price)
                            and len(t['fills']) < PROFILE['max_fills_per_order']]
                if not eligible:
                    break
                makers = [o for o in book if o['price'] == level_price]
                level_lots = sum(o['lots'] for o in makers)
                alloc = allocate_pro_rata(level_lots, [t['remaining'] for t in eligible], [t['rank'] for t in eligible])
                progressed = False
                queue = list(makers)
                for t, share in zip(eligible, alloc):
                    while share > 0 and queue and self.fill_budget > 0 and len(t['fills']) < PROFILE['max_fills_per_order']:
                        o = queue[0]
                        lots = min(share, o['lots'])
                        self.fill(m, t, o, lots)
                        progressed = True
                        share -= lots
                        if o['lots'] == 0:
                            queue.pop(0)
                if not progressed:
                    break
            # GAP (filled from engine/matcher.h): each taker's stop is fixed from
            # the book when its side's sweep ends, before phase 6 rests anything.
            book = self.best_opposite(mid, side)
            for t in group:
                if t['remaining'] == 0:
                    t['stop'] = 'FILLED'
                elif not book or not self.crosses(side, t['price'], book[0]['price']):
                    t['stop'] = 'NO_CROSS'
                elif len(t['fills']) >= PROFILE['max_fills_per_order']:
                    t['stop'] = 'ORDER_FILL_CAP'
                else:
                    t['stop'] = 'SLOT_FILL_CAP'

    def fill(self, m, t, o, lots):
        st = self.st
        price = o['price']
        q = quote_atoms(m, lots, price)
        base = lots * m.lot
        buyer, seller = (t, o) if t['side'] == BUY else (o, t)
        before = seller['cum_quote']
        fee = fee_total(before + q, PROFILE['fee_ppm']) - seller['fee_charged']
        seller['cum_quote'] += q
        buyer['cum_quote'] += q
        seller['fee_charged'] += fee
        # Base: seller reserved -> buyer available. Quote: buyer reserved -> seller available, less the fee.
        st.bal(seller['owner'], m.base)[1] -= base
        st.bal(buyer['owner'], m.base)[0] += base
        st.bal(buyer['owner'], m.quote)[1] -= q
        st.bal(seller['owner'], m.quote)[0] += q - fee
        st.totals.setdefault(m.quote, [0, 0, 0, 0])[3] += fee
        seller['reserved_left'] -= base
        buyer['reserved_left'] -= q
        for party in (t, o):
            party['lots' if party is o else 'remaining'] -= lots
        o['fills'] += 1
        self.fill_budget -= 1
        f = {'maker': o['id'], 'taker': t['id'], 'market': m.id(), 'taker_side': t['side'], 'price': price, 'lots': lots,
             'quote': q, 'fee': fee, 'seller_quote_before': before}
        t['fills'].append(f)
        # Both legs of the fill go into the taker's deltas (net_deltas sorts them).
        for acct, asset, av, rs in ((seller['owner'], m.base, 0, -base), (buyer['owner'], m.base, base, 0),
                                    (buyer['owner'], m.quote, 0, -q), (seller['owner'], m.quote, q - fee, 0)):
            d = t['deltas'].setdefault((acct, asset), [0, 0])
            d[0] += av
            d[1] += rs
        if o['lots'] == 0:
            st.accounts[o['owner']]['open'] -= 1
            del st.orders[o['id']]

    # Phase 6
    def phase_rest(self, takers):
        st = self.st
        # The residual cross: per market, among the GTC remainders that left
        # the sweep with NO_CROSS (engine/residual.h).
        crossing = set()
        for mid in {t['market'] for t in takers}:
            rest = [t for t in takers if t['market'] == mid and t['tif'] == GTC and t['remaining'] > 0
                    and t['stop'] == 'NO_CROSS']
            buys = [t['price'] for t in rest if t['side'] == BUY]
            sells = [t['price'] for t in rest if t['side'] == SELL]
            if buys and sells:
                crossing |= {t['id'] for t in rest if (t['side'] == BUY and t['price'] >= min(sells)) or
                             (t['side'] == SELL and t['price'] <= max(buys))}
        for t in sorted(takers, key=lambda t: t['rank']):
            a, asset = t['action'], t['asset']
            m = st.markets[t['market']]
            keep, remaining = 0, 0
            if t['remaining'] == 0:
                code = FILLED
            elif t['stop'] in ('ORDER_FILL_CAP', 'SLOT_FILL_CAP'):
                code = REJ_FILL_CAP
                CAP_STOPS[t['stop']] += 1
            elif t['tif'] == IOC:
                code = IOC_DONE
            elif t['id'] in crossing:
                code = REJ_RESIDUAL_CROSS_UNSUPPORTED
            else:
                # GAP: remaining_lots is non-zero only for an order that rests.
                code = PARTIAL_RESTING if t['fills'] else RESTING
                _, keep = reservation(m, t['side'], t['price'], t['remaining'])
                remaining = t['remaining']
            release = t['reserved_left'] - keep
            if release:
                st.bal(t['owner'], asset)[0] += release
                st.bal(t['owner'], asset)[1] -= release
                d = t['deltas'].setdefault((t['owner'], asset), [0, 0])
                d[0] += release
                d[1] -= release
            if keep:
                st.orders[t['id']] = new_order(a, t['market'], t['side'], t['price'], t['remaining'],
                                               arrival_rank(self.height, REST_BLOCK, t['rank']), keep, asset,
                                               cum_quote=t['cum_quote'], fee_charged=t['fee_charged'],
                                               fills=len(t['fills']))
            elif t['tif'] == GTC:
                st.accounts[t['owner']]['open'] -= 1
            # The taker's own reservation is its first delta.
            deltas = [(t['owner'], asset, -t_res(t), t_res(t))] + [(k[0], k[1], v[0], v[1]) for k, v in t['deltas'].items()]
            self.outcome(t['rank'], code, remaining=remaining, fills=t['fills'], deltas=net_deltas(deltas))

    # Phase 8
    def phase_fees(self):
        st = self.st
        out = []
        for asset in sorted(st.totals):
            fees = st.totals[asset][3]
            if fees == 0:
                continue
            seat = fees * PROFILE['seat_share_percent'] // 100
            pool, tre = seat_pool(self.profile_hash), treasury(self.profile_hash)
            entries = []
            for acct, amount in ((pool, seat), (tre, fees - seat)):
                # GAP: a zero share (fees of 1 atom give the seat pool 0) creates
                # no protocol account; the engine's credit of 0 is a no-op.
                if amount:
                    st.ensure_account(acct)
                    st.bal(acct, asset)[0] += amount
                entries.append((acct, asset, amount, 0))
            deltas = net_deltas(entries)
            st.totals[asset][3] = 0
            # GAP: the SLOT outcome's account is zero.
            out.append({'source': SRC_SLOT, 'subject': asset, 'account': ZERO32, 'nonce': 0, 'code': FEES_DISTRIBUTED,
                        'remaining': 0, 'fills': [], 'deltas': deltas})
        return out

    def check_conservation(self):
        st = self.st
        for asset, (dep, eff, pend, held) in st.totals.items():
            have = sum(rec['bal'].get(asset, [0, 0])[0] + rec['bal'].get(asset, [0, 0])[1] for rec in st.accounts.values())
            if have + pend + held != dep - eff:
                raise AssertionError('checker conservation broken for an asset')


def t_res(t):
    return t['reserved_at_place']


def delta(account, asset, available, reserved):
    return {'account': account, 'asset': asset, 'available': available, 'reserved': reserved}


def net_deltas(entries):
    """An outcome's deltas: netted per (account, asset), sorted bytewise, all-zero entries dropped.
    GAP (filled from engine/engine.cpp DeltaSet): the spec does not give the order."""
    out = {}
    for account, asset, av, rs in entries:
        cur = out.setdefault((account, asset), [0, 0])
        cur[0] += av
        cur[1] += rs
    return [delta(k[0], k[1], v[0], v[1]) for k, v in sorted(out.items()) if v != [0, 0]]


def new_order(a, mid, side, price, lots, rank, reserved, asset, cum_quote=0, fee_charged=0, fills=0):
    return {'id': a.id(), 'market': mid, 'owner': a.account(), 'side': side, 'price': price, 'lots': lots,
            'arrival_rank': rank, 'reserved_left': reserved, 'cum_quote': cum_quote, 'fee_charged': fee_charged,
            'fills': fills, 'asset': asset}


def allocate_pro_rata(supply, demands, ranks):
    """D4: all demands if they fit, else floor shares plus largest remainders, ties to the lower rank."""
    total = sum(demands)
    if total <= supply:
        return list(demands)
    alloc = [supply * d // total for d in demands]
    rem = [supply * d % total for d in demands]
    left = supply - sum(alloc)
    for i in sorted(range(len(demands)), key=lambda i: (-rem[i], ranks[i]))[:left]:
        alloc[i] += 1
    return alloc


# ---- One slot end to end -------------------------------------------------------

def genesis(domain, profile_hash, epoch, seat_set_root, anchor, facts_start, markets):
    st = State(markets)
    empty = merkle_root([])
    h = {'version': 1, 'domain': domain, 'profile_hash': profile_hash, 'authority_epoch': epoch, 'height': 0,
         'parent_hash': ZERO32, 'parent_cert_hash': ZERO32, 'anchor_height': anchor[0], 'anchor_hash': anchor[1],
         'facts_from': facts_start, 'facts_to': facts_start, 'seat_set_root': seat_set_root, 'facts_root': empty,
         'actions_root': empty, 'outcomes_root': empty, 'state_root': st.roots()['state'], 'action_count': 0,
         'outcome_count': 0, 'commitment_version': 0}
    return {'header': h, 'state': st, 'cert_hash': ZERO32}


def apply_slot(parent, inputs):
    """Returns (child_head, originals_in_canonical_order, outcomes, roots) or raises Invalid."""
    from .codec import header_hash
    parent = dict(parent, hash=header_hash(parent['header']))
    deposits, actions, payloads, order = validate(parent, inputs)
    head = parent['header']
    slot = Slot(parent, deposits, actions, payloads, order, head['height'] + 1, head['profile_hash'])
    outcomes = slot.run()
    roots = slot.st.roots()
    canonical = [inputs['originals'][i] for i in order]
    h = dict(head)
    h.update({'height': head['height'] + 1, 'parent_hash': parent['hash'], 'parent_cert_hash': parent['cert_hash'],
              'anchor_height': inputs['anchor'][0], 'anchor_hash': inputs['anchor'][1],
              'facts_from': inputs['facts_from'], 'facts_to': inputs['facts_to'],
              'facts_root': merkle_root(fact_leaf(f) for f in inputs['facts']),
              'actions_root': merkle_root(action_leaf(o) for o in canonical),
              'outcomes_root': merkle_root(outcome_leaf(o) for o in outcomes), 'state_root': roots['state'],
              'action_count': len(canonical), 'outcome_count': len(outcomes)})
    return {'header': h, 'state': slot.st, 'cert_hash': None}, canonical, outcomes, roots
