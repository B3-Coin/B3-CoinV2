# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Test-only modern B3 block codec/signing for disposable FlowMesh regtest.

The upstream Python transaction codec deliberately lacks B3's MPA. Keep this
small adapter separate: base txid and witness hash EXCLUDE MPA, while the
payload-root cell commits canonical MPA sections and TX_MODERN includes them.
Never use these Python signing helpers with non-test keys.
"""

import copy
import io
import struct
import unittest

from test_framework.key import TaggedHash, compute_xonly_pubkey, sign_schnorr, verify_schnorr
from test_framework.messages import (
    CBlockHeader, COutPoint, CTransaction, CTxIn, CTxInWitness, CTxOut,
    CTxWitness, hash256, ser_compact_size, ser_string, ser_vector,
)
from test_framework.script import CScript, OP_DROP, OP_FALSE, OP_TRUE


MAX_BLOCK_FIXTURE_BYTES = 4 * 1024 * 1024
MODERN_MARKER_MASK = 0xe8000000
MODERN_MARKER = 0x28000000
WITNESS_PREFIX = bytes.fromhex("6a24aa21a9ed")


def exact(stream, size):
    assert 0 <= size <= MAX_BLOCK_FIXTURE_BYTES
    result = stream.read(size)
    assert len(result) == size, "Truncated modern block fixture"
    return result


def compact(stream, maximum=MAX_BLOCK_FIXTURE_BYTES):
    first = exact(stream, 1)[0]
    if first < 253:
        value = first
    else:
        size, minimum = {253: (2, 253), 254: (4, 65536), 255: (8, 2**32)}[first]
        value = int.from_bytes(exact(stream, size), "little")
        assert value >= minimum, "Noncanonical CompactSize"
    assert value <= maximum, "Oversized modern block fixture collection"
    return value


def merkle(hashes):
    hashes = list(hashes)
    assert hashes and all(len(item) == 32 for item in hashes)
    while len(hashes) > 1:
        if len(hashes) % 2:
            hashes.append(hashes[-1])
        hashes = [hash256(hashes[i] + hashes[i + 1]) for i in range(0, len(hashes), 2)]
    return hashes[0]


class ModernTransaction(CTransaction):
    __slots__ = ("mpa",)

    def __init__(self, tx=None):
        super().__init__(tx)
        self.mpa = copy.deepcopy(tx.mpa) if isinstance(tx, ModernTransaction) else []

    def deserialize(self, stream):
        self.version = int.from_bytes(exact(stream, 4), "little")
        count = compact(stream, 100000)
        flags = 0
        if count == 0:
            flags = exact(stream, 1)[0]
            assert flags in (1, 2, 3), "Unknown/empty modern transaction flags"
            count = compact(stream, 100000)
        assert count > 0
        self.vin = []
        for _ in range(count):
            item = CTxIn()
            item.deserialize(stream)
            self.vin.append(item)
        self.vout = []
        for _ in range(compact(stream, 100000)):
            item = CTxOut()
            item.deserialize(stream)
            self.vout.append(item)
        self.wit = CTxWitness()
        if flags & 1:
            self.wit.vtxinwit = [CTxInWitness() for _ in self.vin]
            self.wit.deserialize(stream)
            assert not self.wit.is_null(), "Superfluous witness fixture"
        self.mpa = []
        if flags & 2:
            count = compact(stream, 64)
            assert count > 0
            for _ in range(count):
                record_type, version = struct.unpack("<HH", exact(stream, 4))
                self.mpa.append((record_type, version, exact(stream, compact(stream))))
        self.nLockTime = int.from_bytes(exact(stream, 4), "little")

    @classmethod
    def from_hex(cls, value):
        assert isinstance(value, str) and 0 < len(value) <= 2 * MAX_BLOCK_FIXTURE_BYTES
        raw = bytes.fromhex(value)
        stream = io.BytesIO(raw)
        result = cls()
        result.deserialize(stream)
        assert stream.read() == b"" and result.serialize_modern() == raw
        return result

    def mpa_section(self):
        if not self.mpa:
            return b""
        return ser_compact_size(len(self.mpa)) + b"".join(
            struct.pack("<HH", kind, version) + ser_string(payload)
            for kind, version, payload in self.mpa)

    def serialize_modern(self):
        flags = (0 if self.wit.is_null() else 1) | (2 if self.mpa else 0)
        return (struct.pack("<I", self.version) + (bytes([0, flags]) if flags else b"") +
                ser_vector(self.vin) + ser_vector(self.vout) +
                (self.wit.serialize() if flags & 1 else b"") + self.mpa_section() +
                struct.pack("<I", self.nLockTime))


class ModernBlock(CBlockHeader):
    def __init__(self, header=None):
        super().__init__(header)
        self.transactions = []
        self.signature = b""

    @classmethod
    def from_hex(cls, value):
        assert isinstance(value, str) and 0 < len(value) <= MAX_BLOCK_FIXTURE_BYTES * 2
        raw = bytes.fromhex(value)
        stream = io.BytesIO(raw)
        result = cls()
        result.deserialize(stream)  # Header only.
        assert result.nVersion & MODERN_MARKER_MASK == MODERN_MARKER
        for _ in range(compact(stream, 100000)):
            tx = ModernTransaction()
            tx.deserialize(stream)
            result.transactions.append(tx)
        result.signature = exact(stream, compact(stream, 64))
        assert len(result.signature) == 64, "Fixture must already be modern PoS"
        assert stream.read() == b"" and result.serialize_modern() == raw
        assert result.hashMerkleRoot == int.from_bytes(
            merkle(hash256(tx.serialize_without_witness()) for tx in result.transactions), "little")
        return result

    def serialize_modern(self):
        return (self._serialize_header() + ser_compact_size(len(self.transactions)) +
                b"".join(tx.serialize_modern() for tx in self.transactions) + ser_string(self.signature))


def payload_root(transactions):
    return merkle(TaggedHash("B3/MPA/LEAF/V1", struct.pack(">I", index) +
                             (TaggedHash("B3/MPA/SECTION/V1", tx.mpa_section()) if tx.mpa else bytes(32)))
                  for index, tx in enumerate(transactions))


def metadata_type(script):
    # Prior coinbase is node-validated; recognize all existing metadata cells
    # so neither an old FINALITY_CERT nor its old payload root is copied.
    try:
        first = next(iter(CScript(script)))
    except (StopIteration, ValueError):
        return None
    if isinstance(first, bytes) and first.startswith(b"B3MC") and len(first) >= 40:
        return int.from_bytes(first[4:6], "big")
    return None


def build_modern_block(parent, transaction, height, timestamp, domain, secret):
    """Underclaim the producer reward; preserve prior mandatory treasury output.

    Only used in the fixed regtest fixture: modern subsidy does not halve,
    interval=round_seconds=1, and a current validator is eligible at the
    deliberately saturated round. No finality certificate is required in
    every block, so omit the prior coinbase's cells/MPA rather than replay it.
    """
    assert len(domain) == 32 and height > 1 and timestamp > parent.nTime
    key, _ = compute_xonly_pubkey(secret)
    assert key is not None
    block = ModernBlock(parent)
    block.hashPrevBlock = parent.hash_int
    block.nTime = timestamp
    block.nNonce = 0  # Modern PoS consensus requires zero, not a mining nonce.
    coinbase = ModernTransaction(parent.transactions[0])
    assert len(coinbase.vin) == 1 and coinbase.vin[0].prevout.hash == 0
    coinbase.vin[0].scriptSig = CScript([height, key])
    coinbase.nLockTime = height - 1
    coinbase.mpa = []
    coinbase.wit = CTxWitness()
    coinbase.vout = [CTxOut(0, CScript([OP_TRUE]))] + [
        copy.deepcopy(output) for output in coinbase.vout[1:]
        if metadata_type(output.scriptPubKey) is None and not output.scriptPubKey.startswith(WITNESS_PREFIX)]
    block.transactions = [coinbase, ModernTransaction(transaction)]
    root = payload_root(block.transactions)
    coinbase.vout.append(CTxOut(0, CScript([b"B3MC" + struct.pack(">HH", 8, 1) + root, OP_DROP, OP_FALSE])))
    if not transaction.wit.is_null():
        coinbase.wit.vtxinwit = [CTxInWitness()]
        coinbase.wit.vtxinwit[0].scriptWitness.stack = [bytes(32)]
        witness_root = merkle([bytes(32), hash256(transaction.serialize_with_witness())])
        coinbase.vout.append(CTxOut(0, WITNESS_PREFIX + hash256(witness_root + bytes(32))))
    block.hashMerkleRoot = int.from_bytes(
        merkle(hash256(tx.serialize_without_witness()) for tx in block.transactions), "little")
    digest = TaggedHash("B3/MODERN/POS/SIG/V1", domain + hash256(block._serialize_header()))
    block.signature = sign_schnorr(secret, digest)
    assert block.signature is not None and verify_schnorr(key, block.signature, digest)
    assert len(block.serialize_modern()) <= MAX_BLOCK_FIXTURE_BYTES
    return block


class TestFlowMeshModernBlock(unittest.TestCase):
    def fixture(self, witness=False):
        tx = ModernTransaction()
        tx.vin = [CTxIn(COutPoint(1, 0), b"signed-input")]
        tx.vout = [CTxOut(1, b"\x51")]
        tx.mpa = [(8, 1, b"\x00\x01" + bytes(558))]
        if witness:
            tx.wit.vtxinwit = [CTxInWitness()]
            tx.wit.vtxinwit[0].scriptWitness.stack = [b"signed-witness"]
        return tx

    def test_payload_leaf_matches_cpp_golden(self):
        # src/test/payload_root_tests.cpp:95-96 pin raw hash bytes (HexStr).
        self.assertEqual(TaggedHash("B3/MPA/LEAF/V1", bytes(36)).hex(),
                         "3d93dae34da8e9684ba8bfb3ec6ae5ec4c68ac57d5c6fd374c955f9719810aa1")
        self.assertEqual(TaggedHash("B3/MPA/LEAF/V1", struct.pack(">I", 1) + bytes(32)).hex(),
                         "1858c79e4c9458d8ea6c9b79ec9a6656f79ffd528eede449bb77675b2a9ce92e")

    def test_roundtrip_and_mpa_exclusion_from_txid_witness(self):
        for witness in (False, True):
            tx = self.fixture(witness)
            decoded = ModernTransaction.from_hex(tx.serialize_modern().hex())
            self.assertEqual(decoded.serialize_with_witness(), tx.serialize_with_witness())
            changed = ModernTransaction(tx)
            changed.mpa[0] = (8, 1, bytes(560))
            self.assertEqual(changed.txid_hex, tx.txid_hex)
            self.assertEqual(changed.serialize_with_witness(), tx.serialize_with_witness())
            self.assertNotEqual(changed.serialize_modern(), tx.serialize_modern())
            self.assertNotEqual(payload_root([changed]), payload_root([tx]))

    def test_rebuilt_roots_and_producer_signature_follow_mutation(self):
        parent = ModernBlock()
        parent.nVersion = MODERN_MARKER
        parent.nTime = 1000
        parent.nBits = 0x207fffff
        cb = ModernTransaction()
        cb.vin = [CTxIn(COutPoint(0, 0xffffffff), CScript([100, bytes([2]) * 32]))]
        cb.vout = [CTxOut(100, b"\x51"), CTxOut(10, b"\x51")]
        parent.transactions = [cb]
        secret, domain = (7).to_bytes(32, "big"), bytes([3]) * 32
        tx = self.fixture(True)
        first = build_modern_block(parent, tx, 101, 1017, domain, secret)
        tx.mpa[0] = (8, 1, bytes(560))
        second = build_modern_block(parent, tx, 101, 1017, domain, secret)
        self.assertNotEqual(first.hash_hex, second.hash_hex)
        self.assertNotEqual(first.signature, second.signature)
        self.assertEqual(first.transactions[1].serialize_with_witness(), second.transactions[1].serialize_with_witness())
        self.assertEqual(first.transactions[0].vout[0].nValue, 0)
        self.assertEqual(first.transactions[0].vout[1].nValue, 10)
        for block in (first, second):
            self.assertEqual(ModernBlock.from_hex(block.serialize_modern().hex()).hash_hex, block.hash_hex)
            key, _ = compute_xonly_pubkey(secret)
            digest = TaggedHash("B3/MODERN/POS/SIG/V1", domain + hash256(block._serialize_header()))
            self.assertTrue(verify_schnorr(key, block.signature, digest))

    def test_truncated_and_noncanonical_data_rejected(self):
        tx = self.fixture()
        with self.assertRaises(AssertionError):
            ModernTransaction.from_hex(tx.serialize_modern()[:-1].hex())
        with self.assertRaises(AssertionError):
            compact(io.BytesIO(b"\xfd\x01\x00"))
