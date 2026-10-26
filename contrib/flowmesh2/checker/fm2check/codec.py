# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Codecs and hashes of the FlowMesh V2 checker (M3-ENG-05), written from
src/flowmesh2/ARCHITECTURE.md sections 9 and 11. Standard library only.

Hashes are 32-byte `bytes` in the order the C++ code stores them (the order
Python's hashlib prints), never uint256::GetHex order. Integers are little
endian, as Bitcoin serialization writes them.
"""

import hashlib
import json
import os
import struct

MAX_ATOMS = 1 << 62
U64_MAX = (1 << 64) - 1
ZERO32 = bytes(32)

# ---- Tagged hashes (section 9) --------------------------------------------

# The tag texts come from the public catalog src/flowmesh2/core/HASH_DOMAINS.json
# (its prefix is reserved for that registry, so it is not spelled out here).
_CATALOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 'src', 'flowmesh2', 'core',
                        'HASH_DOMAINS.json')
with open(_CATALOG, encoding='utf-8') as _f:
    _DOMAINS = json.load(_f)
assert _DOMAINS['format'] == 'fm2-hash-domains/1'
TAG_TEXTS = {row['name']: row['text'] for row in _DOMAINS['tags']}
assert all(text == _DOMAINS['prefix'] + name for name, text in TAG_TEXTS.items())
_TAG_CACHE = {}


def tagged(name, data=b''):
    """BIP340-style tagged hash: SHA256(SHA256(tag) || SHA256(tag) || data)."""
    prefix = _TAG_CACHE.get(name)
    if prefix is None:
        t = hashlib.sha256(TAG_TEXTS[name].encode()).digest()  # KeyError for a tag the registry lacks
        prefix = _TAG_CACHE[name] = t + t
    return hashlib.sha256(prefix + data).digest()


# ---- Merkle (section 9: RFC 6962 shape, tagged, size-bound root) ----------

def merkle_leaf(value):
    return tagged('MERKLE_LEAF', value)


def merkle_node(left, right):
    return tagged('MERKLE_NODE', left + right)


def _mth(nodes):
    """RFC 6962 Merkle tree hash over already-hashed leaves."""
    n = len(nodes)
    if n == 1:
        return nodes[0]
    k = 1
    while k * 2 < n:
        k *= 2
    return merkle_node(_mth(nodes[:k]), _mth(nodes[k:]))


def merkle_root(values):
    """Tagged(MERKLE_ROOT, u64 n || MTH); the empty tree hashes MERKLE_EMPTY."""
    values = list(values)
    if not values:
        return tagged('MERKLE_ROOT', u64(0) + tagged('MERKLE_EMPTY'))
    return tagged('MERKLE_ROOT', u64(len(values)) + _mth([merkle_leaf(v) for v in values]))


# ---- Serialization primitives ---------------------------------------------

def u8(v):
    return struct.pack('<B', v)


def u16(v):
    return struct.pack('<H', v)


def u32(v):
    return struct.pack('<I', v)


def u64(v):
    return struct.pack('<Q', v)


def i64(v):
    return struct.pack('<q', v)


def compact_size(n):
    if n < 253:
        return u8(n)
    if n <= 0xFFFF:
        return b'\xfd' + u16(n)
    if n <= 0xFFFFFFFF:
        return b'\xfe' + u32(n)
    return b'\xff' + u64(n)


class DecodeError(Exception):
    pass


class Reader:
    def __init__(self, data):
        self.data = bytes(data)
        self.pos = 0

    def take(self, n):
        if n < 0 or self.pos + n > len(self.data):
            raise DecodeError('truncated')
        out = self.data[self.pos:self.pos + n]
        self.pos += n
        return out

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack('<H', self.take(2))[0]

    def u32(self):
        return struct.unpack('<I', self.take(4))[0]

    def u64(self):
        return struct.unpack('<Q', self.take(8))[0]

    def i64(self):
        return struct.unpack('<q', self.take(8))[0]

    def h32(self):
        return self.take(32)

    def compact_size(self):
        """Canonical CompactSize, as Bitcoin's ReadCompactSize enforces."""
        first = self.u8()
        if first < 253:
            return first
        if first == 253:
            v = self.u16()
            if v < 253:
                raise DecodeError('non-canonical CompactSize')
            return v
        if first == 254:
            v = self.u32()
            if v <= 0xFFFF:
                raise DecodeError('non-canonical CompactSize')
            return v
        v = self.u64()
        if v <= 0xFFFFFFFF:
            raise DecodeError('non-canonical CompactSize')
        return v

    def end(self):
        if self.pos != len(self.data):
            raise DecodeError('trailing bytes')


def atoms_on_wire(v):
    if not 0 <= v <= MAX_ATOMS:
        raise DecodeError('amount outside [0, MAX_ATOMS]')
    return v


# ---- Identifiers (section 9) ----------------------------------------------

NATIVE = ZERO32  # AssetId::Native(): the all-zero id of B3


def account_id(xonly):
    return tagged('ACCOUNT', xonly)


def test_asset(name):
    return tagged('TEST_ASSET', name.encode())


def protocol_account(name, profile_hash):
    return tagged('PROTOCOL_ACCOUNT', name.encode() + profile_hash)


def seat_pool(profile_hash):
    return protocol_account('SEAT_POOL', profile_hash)


def treasury(profile_hash):
    return protocol_account('TREASURY', profile_hash)


class Market:
    """MarketDescriptor: base || quote || u64 base_atoms_per_lot || u64 quote_atoms_per_tick."""

    def __init__(self, base, quote, lot, tick):
        self.base, self.quote, self.lot, self.tick = base, quote, lot, tick

    def encode(self):
        return self.base + self.quote + u64(self.lot) + u64(self.tick)

    def id(self):
        return tagged('MARKET', self.encode())

    def leaf_value(self):
        return tagged('STATE_MARKETS', self.encode())

    def valid(self):
        return self.base != self.quote and 1 <= self.lot <= MAX_ATOMS and 1 <= self.tick <= MAX_ATOMS


# ---- Actions (section 9, as landed in R4) ---------------------------------

ACTION_VERSION = 1
KIND_PLACE, KIND_CANCEL, KIND_CANCEL_ALL, KIND_WITHDRAW, KIND_TRANSFER = 1, 2, 3, 4, 5
KNOWN_KINDS = range(1, 8)
BUY, SELL = 1, 2
GTC, IOC, POST_ONLY = 1, 2, 3


class Action:
    def __init__(self, domain, signer, nonce, ref_slot, max_delay, kind, payload, sig=bytes(64), version=ACTION_VERSION):
        self.version, self.domain, self.signer = version, domain, signer
        self.nonce, self.ref_slot, self.max_delay = nonce, ref_slot, max_delay
        self.kind, self.payload, self.sig = kind, bytes(payload), bytes(sig)

    def unsigned_msg(self):
        return (u8(self.version) + self.domain + self.signer + u64(self.nonce) + u64(self.ref_slot) +
                u16(self.max_delay) + u8(self.kind) + compact_size(len(self.payload)) + self.payload)

    def encode(self):
        return self.unsigned_msg() + self.sig

    def id(self):
        return tagged('ACTION', self.unsigned_msg())

    def account(self):
        return account_id(self.signer)

    @staticmethod
    def decode(data, max_payload):
        r = Reader(data)
        version = r.u8()
        if version != ACTION_VERSION:
            raise DecodeError('unknown ActionV2 version')
        domain, signer = r.h32(), r.h32()
        nonce, ref_slot, max_delay = r.u64(), r.u64(), r.u16()
        kind = r.u8()
        if kind not in KNOWN_KINDS:
            raise DecodeError('unknown ActionV2 kind')
        n = r.compact_size()
        if n > max_payload:
            raise DecodeError('payload above the cap')
        payload = r.take(n)
        sig = r.take(64)
        r.end()
        return Action(domain, signer, nonce, ref_slot, max_delay, kind, payload, sig, version)


def action_leaf(original_bytes):
    return tagged('ACTION_LEAF', original_bytes)


def place_payload(market_id, side, tif, price, lots):
    return market_id + u8(side) + u8(tif) + u64(price) + u64(lots)


def decode_place(payload):
    r = Reader(payload)
    market, side, tif, price, lots = r.h32(), r.u8(), r.u8(), r.u64(), r.u64()
    r.end()
    if side not in (BUY, SELL):
        raise DecodeError('unknown Side')
    if tif not in (GTC, IOC, POST_ONLY):
        raise DecodeError('unknown TimeInForce')
    return market, side, tif, price, lots


def cancel_payload(order_id):
    return order_id


def decode_cancel(payload):
    r = Reader(payload)
    order = r.h32()
    r.end()
    return order


def transfer_payload(to, asset, amount):
    return to + asset + i64(amount)


def decode_transfer(payload):
    r = Reader(payload)
    to, asset, amount = r.h32(), r.h32(), r.i64()
    r.end()
    atoms_on_wire(amount)
    return to, asset, amount


# ---- Facts (as landed in R4) ----------------------------------------------

FACT_DEPOSIT = 1
DEPOSIT_BYTES = 117


def deposit_fact(height, index, txid, vout, account, asset, amount):
    return u8(FACT_DEPOSIT) + u32(height) + u32(index) + txid + u32(vout) + account + asset + i64(amount)


def decode_deposit(data):
    r = Reader(data)
    if r.u8() != FACT_DEPOSIT:
        raise DecodeError('not a deposit')
    cursor = (r.u32(), r.u32())
    outpoint = (r.h32(), r.u32())
    account, asset, amount = r.h32(), r.h32(), r.i64()
    r.end()
    atoms_on_wire(amount)
    return cursor, outpoint, account, asset, amount


def fact_leaf(fact_bytes):
    return tagged('FACT', fact_bytes)


# ---- Slot header (section 9, 366 bytes) -----------------------------------

SLOT_HEADER_VERSION = 1
HEADER_FIELDS = ('version', 'domain', 'profile_hash', 'authority_epoch', 'height', 'parent_hash', 'parent_cert_hash',
                 'anchor_height', 'anchor_hash', 'facts_from', 'facts_to', 'seat_set_root', 'facts_root',
                 'actions_root', 'outcomes_root', 'state_root', 'action_count', 'outcome_count', 'commitment_version')


def encode_header(h):
    out = (u8(h['version']) + h['domain'] + h['profile_hash'] + u64(h['authority_epoch']) + u64(h['height']) +
           h['parent_hash'] + h['parent_cert_hash'] + u32(h['anchor_height']) + h['anchor_hash'] +
           u32(h['facts_from'][0]) + u32(h['facts_from'][1]) + u32(h['facts_to'][0]) + u32(h['facts_to'][1]) +
           h['seat_set_root'] + h['facts_root'] + h['actions_root'] + h['outcomes_root'] + h['state_root'] +
           u32(h['action_count']) + u32(h['outcome_count']) + u8(h['commitment_version']))
    assert len(out) == 366, len(out)
    return out


def decode_header(data):
    r = Reader(data)
    h = {'version': r.u8(), 'domain': r.h32(), 'profile_hash': r.h32(), 'authority_epoch': r.u64(), 'height': r.u64(),
         'parent_hash': r.h32(), 'parent_cert_hash': r.h32(), 'anchor_height': r.u32(), 'anchor_hash': r.h32(),
         'facts_from': (r.u32(), r.u32()), 'facts_to': (r.u32(), r.u32()), 'seat_set_root': r.h32(),
         'facts_root': r.h32(), 'actions_root': r.h32(), 'outcomes_root': r.h32(), 'state_root': r.h32(),
         'action_count': r.u32(), 'outcome_count': r.u32(), 'commitment_version': r.u8()}
    r.end()
    return h


def header_hash(h):
    return tagged('SLOT', encode_header(h))


# ---- Outcomes (as landed in R4) -------------------------------------------

SRC_FACT, SRC_ACTION, SRC_SLOT = 1, 2, 3

DEPOSIT_CREDITED, RESTING, FILLED, PARTIAL_RESTING, IOC_DONE, CANCELLED, TRANSFERRED, FEES_DISTRIBUTED = range(1, 9)
REJ_INSUFFICIENT_AVAILABLE, REJ_UNKNOWN_MARKET, REJ_UNKNOWN_ORDER, REJ_NOT_OWN_ORDER = 64, 65, 66, 67
REJ_ORDER_CLOSED, REJ_WOULD_CROSS, REJ_BOOK_FULL, REJ_FILL_CAP = 68, 69, 70, 71
REJ_PRICE_BAND, REJ_OPEN_ORDER_CAP, REJ_BAD_PARAMS, REJ_UNKNOWN_ASSET, REJ_NO_RECIPIENT = 72, 73, 74, 75, 76
REJ_RESIDUAL_CROSS_UNSUPPORTED = 96

CODE_NAMES = {v: k for k, v in globals().items() if k.isupper() and (k.startswith('REJ_') or k in (
    'DEPOSIT_CREDITED', 'RESTING', 'FILLED', 'PARTIAL_RESTING', 'IOC_DONE', 'CANCELLED', 'TRANSFERRED',
    'FEES_DISTRIBUTED'))}


def encode_fill(f):
    """137 bytes: maker || taker || market || u8 taker_side || u64 price || u64 lots || i64 quote || i64 fee
    || i64 seller_quote_before."""
    return (f['maker'] + f['taker'] + f['market'] + u8(f['taker_side']) + u64(f['price']) + u64(f['lots']) +
            i64(f['quote']) + i64(f['fee']) + i64(f['seller_quote_before']))


def encode_delta(d):
    """80 bytes: account || asset || i64 available_delta || i64 reserved_delta."""
    return d['account'] + d['asset'] + i64(d['available']) + i64(d['reserved'])


def encode_outcome(o):
    out = (u8(o['source']) + o['subject'] + o['account'] + u64(o['nonce']) + u16(o['code']) + u64(o['remaining']) +
           compact_size(len(o['fills'])) + b''.join(encode_fill(f) for f in o['fills']) +
           compact_size(len(o['deltas'])) + b''.join(encode_delta(d) for d in o['deltas']))
    return out


def outcome_leaf(o):
    return tagged('OUTCOME', encode_outcome(o))


# ---- State commitment v0 (as landed in R5 and R7) -------------------------

def account_leaf_bytes(account, balances, used_nonces, open_orders):
    """balances: {asset: (available, reserved)}; canonical: ascending assets, no all-zero entry."""
    rows = [(a, v) for a, v in sorted(balances.items()) if v != (0, 0)]
    out = account + compact_size(len(rows))
    for asset, (available, reserved) in rows:
        out += asset + i64(available) + i64(reserved)
    out += compact_size(len(used_nonces)) + b''.join(u64(n) for n in used_nonces)
    return out + u32(open_orders)


def order_leaf_bytes(o):
    """RestingOrderLeafV0, 153 bytes."""
    return (o['id'] + o['market'] + o['owner'] + u8(o['side']) + u64(o['price']) + u64(o['lots']) +
            u64(o['arrival_rank']) + i64(o['reserved_left']) + i64(o['cum_quote']) + i64(o['fee_charged']) +
            u64(o['fills']))


def totals_leaf_bytes(asset, deposits, effects, pending, fees_held):
    return asset + i64(deposits) + i64(effects) + i64(pending) + i64(fees_held)


def nullifier_leaf_bytes(txid, vout):
    return txid + u32(vout)


def state_root_v0(accounts, orders, markets, nullifiers, totals):
    return tagged('STATE', u8(0) + accounts + orders + markets + nullifiers + totals)
