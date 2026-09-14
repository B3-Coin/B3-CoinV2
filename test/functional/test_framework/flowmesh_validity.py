# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Bounded type-8/type-9 transaction and block parity for isolated regtest.

Call before broadcasting the original, fully signed, currently valid fixture.
Only decoderawtransaction, getbestblockhash, getflowmeshclientinfo and
testmempoolaccept are used. No mutation is signed, submitted or mined.

The separate assert_flowmesh_block_parity helper DOES submit authenticated
modern blocks and connects one exact valid block independently on all five
nodes. It is restricted to the newly created disposable regtest datadirs.
Pure mutation tests below test byte preservation, not B3 consensus validity.
"""

import hashlib
import struct
import unittest
from pathlib import Path

from test_framework.address import base58_to_byte
from test_framework.authproxy import log as rpc_log
from test_framework.flowmesh_block import ModernBlock, ModernTransaction, build_modern_block
from test_framework.key import TaggedHash, compute_xonly_pubkey
from test_framework.messages import ser_compact_size
from test_framework.util import assert_equal


MAX_FIXTURE_BYTES = 4 * 1024 * 1024
MAX_PAYLOAD_BYTES = 32768


def invalid_flowmesh_variants(valid_hex, decoded, record_type):
    """Return two same-length MPA-only mutants of a single-record transaction.

    TX_MODERN serializes its MPA after inputs, outputs and any witnesses,
    immediately before nLockTime (src/primitives/transaction.h). Match that
    entire canonical tail, not an ambiguous substring search or the upstream
    Python CTransaction decoder, which does not implement B3's MPA extension.
    """
    assert record_type in (8, 9), record_type
    assert isinstance(valid_hex, str) and len(valid_hex) <= MAX_FIXTURE_BYTES * 2
    raw = bytes.fromhex(valid_hex)
    assert len(raw) > 6 and raw[4] == 0 and raw[5] in (2, 3), "Expected TX_MODERN MPA flags"
    records = decoded["mpa"]
    assert len(records) == 1, "Fixture must contain exactly one type-8/type-9 record"
    record = records[0]
    assert_equal(record["type"], record_type)
    assert_equal(record["version"], 1)
    assert isinstance(record["payload"], str) and len(record["payload"]) <= MAX_PAYLOAD_BYTES * 2
    payload = bytes.fromhex(record["payload"])
    assert 0 < len(payload) <= MAX_PAYLOAD_BYTES
    locktime = struct.pack("<I", decoded["locktime"])
    tail = b"\x01" + struct.pack("<HH", record_type, 1) + ser_compact_size(len(payload)) + payload + locktime
    assert raw.endswith(tail), "Decoded MPA does not match the exact canonical transaction tail"
    offset = len(raw) - len(locktime) - len(payload)

    def variant(name, replacement, reason, detail):
        assert len(replacement) == len(payload) and replacement != payload
        mutant = raw[:offset] + replacement + raw[offset + len(payload):]
        assert mutant[:offset] == raw[:offset] and mutant[offset + len(payload):] == raw[offset + len(payload):]
        return {"name": name, "hex": mutant.hex(), "reject_reason": reason,
                "reject_detail_contains": detail, "payload_offset": offset}

    malformed = bytearray(payload)
    malformed[0] = 0xff
    if record_type == 8:
        # Inner checkpoint integers are BIG-endian, unlike the outer MPA
        # type/version and transaction locktime. The existing codec test pins
        # prefix 00 01 01, execution core463, handoff core543, and a 4-seat
        # certificate560 = core463 + bitmap1 + aggregate signature96.
        assert payload[:2] == b"\x00\x01" and len(payload) > 99
        assert payload[2] in (1, 2)
        core_size = 463 if payload[2] == 1 else 543
        assert 1 <= len(payload) - core_size - 96 <= 625
        # Aggregate signature is the final96 bytes, after core and bitmap.
        assert any(payload[-96:]), "Original certificate signature must be nonzero"
        return [
            variant("checkpoint_bad_inner_version", malformed, "bad-mpa", "mpa-malformed-flowmesh-checkpoint"),
            variant("checkpoint_zero_bls_signature", payload[:-96] + bytes(96), "bad-flowmesh-authorization",
                    "FlowMesh checkpoint certificate is invalid"),
        ]
    # Frozen type-9 proof begins kind[1] || checkpoint_id[32]. Kind2 is a
    # withdrawal, not a deposit sweep. Keep its complete effect/branch intact.
    assert payload[0] == 2 and len(payload) >= 257, "Expected a withdrawal proof"
    assert payload[33] == 2, "Expected the nested withdrawal-receipt effect"
    # Effect is219 bytes; leaf index is BE32, branch count is one byte.
    assert int.from_bytes(payload[252:256], "big") < 4096
    assert payload[256] <= 12 and len(payload) == 257 + 32 * payload[256]
    unknown_checkpoint = bytes([0xff]) * 32
    assert payload[1:33] != unknown_checkpoint
    return [
        variant("withdrawal_bad_proof_kind", malformed, "bad-mpa", "mpa-malformed-flowmesh-vault-proof"),
        variant("withdrawal_unconnected_checkpoint", payload[:1] + unknown_checkpoint + payload[33:],
                "bad-flowmesh-authorization", "FlowMesh vault proof checkpoint is not connected"),
    ]


def assert_flowmesh_mempool_parity(enabled_nodes, disabled_node, valid_hex, record_type):
    """Compare one original and its exact invalid variants at a common B3 tip.

    Return JSON-compatible evidence. Caller retains its existing single
    broadcast of the ORIGINAL valid transaction only, after this returns.
    A stale/conflicting valid baseline, wrong mode, unrelated signature/policy
    rejection, changing B3 tip or differing invalid reason fails the check.
    """
    assert isinstance(valid_hex, str) and 0 < len(valid_hex) <= MAX_FIXTURE_BYTES * 2
    enabled_nodes = list(enabled_nodes)
    assert 1 <= len(enabled_nodes) <= 8
    nodes = [*enabled_nodes, disabled_node]
    assert len({id(node) for node in nodes}) == len(nodes)
    modes = [node.getflowmeshclientinfo()["engine_enabled"] for node in nodes]
    assert_equal(modes, [True] * len(enabled_nodes) + [False])
    tips = [node.getbestblockhash() for node in nodes]
    assert_equal(tips, [tips[0]] * len(nodes))
    baseline = [node.testmempoolaccept([valid_hex])[0] for node in nodes]
    assert all(decision["allowed"] for decision in baseline), baseline
    decoded = nodes[0].decoderawtransaction(valid_hex)
    variants = invalid_flowmesh_variants(valid_hex, decoded, record_type)
    evidence = {"scope": "transaction_validation_testmempoolaccept", "block_parity_qualified": False,
                "record_type": record_type, "B3_tip": tips[0], "engine_enabled": modes,
                "valid_txid": decoded["txid"], "valid_ptxid": decoded["ptxid"],
                "valid_wire_sha256": hashlib.sha256(bytes.fromhex(valid_hex)).hexdigest(),
                "valid_decisions": baseline, "invalid_variants": []}
    for item in variants:
        mutant = nodes[0].decoderawtransaction(item["hex"])
        # No wallet signature, spend destination, amount, nonce or fee changed.
        for field in ("txid", "version", "vin", "vout", "locktime"):
            assert_equal(mutant[field], decoded[field])
        assert mutant["ptxid"] != decoded["ptxid"]
        decisions = [node.testmempoolaccept([item["hex"]])[0] for node in nodes]
        for decision in decisions:
            assert_equal(decision["allowed"], False)
            assert_equal(decision["reject-reason"], item["reject_reason"])
            assert item["reject_detail_contains"] in decision.get("reject-details", ""), decision
        evidence["invalid_variants"].append({
            "name": item["name"], "txid": mutant["txid"], "ptxid": mutant["ptxid"],
            "wire_sha256": hashlib.sha256(bytes.fromhex(item["hex"])).hexdigest(),
            "payload_offset": item["payload_offset"], "decisions": decisions})
    # Re-read the positive control: rejection must not have consumed its
    # inputs or admitted a same-base-txid mutant into any mempool.
    after = [node.testmempoolaccept([valid_hex])[0] for node in nodes]
    assert all(decision["allowed"] for decision in after), after
    assert_equal([node.getbestblockhash() for node in nodes], tips)
    return evidence


def assert_flowmesh_block_parity(enabled_nodes, disabled_node, valid_hex, record_type, *, fixture_root, advance_time):
    """Validate actual signed blocks independently on four operators + client.

    STRICTLY disposable regtest. Briefly disable only ordinary B3 networking
    so every node must independently submit/connect the same positive block,
    not report a relayed duplicate. No invalidateblock, rewind, key import or
    consensus override. The original transaction is mined once by this helper;
    callers must not then broadcast or mine it again.

    A proposal-mode TestBlockValidity positive control runs before and after
    invalid submitblock calls. It is NOT claimed to authenticate the producer:
    the final successful submitblock/active-tip transition does that. Each
    invalid block also has freshly rebuilt commitments and a valid signature.
    """
    enabled_nodes = list(enabled_nodes)
    assert len(enabled_nodes) == 4
    nodes = [*enabled_nodes, disabled_node]
    root = Path(fixture_root).resolve()
    for node in nodes:
        assert node.datadir_path.resolve().is_relative_to(root)
        assert "-b3modernregtest" in node.extra_args and "-b3flowmeshtest" in node.extra_args
        assert_equal(node.getblockchaininfo()["chain"], "regtest")
        assert_equal(node.getstakinginfo()["staking"]["running"], False)
        assert_equal(node.getnetworkinfo()["networkactive"], True)
    modes = [node.getflowmeshclientinfo()["engine_enabled"] for node in nodes]
    assert_equal(modes, [True] * 4 + [False])
    tips = [node.getbestblockhash() for node in nodes]
    assert_equal(tips, [tips[0]] * 5)
    parent = ModernBlock.from_hex(nodes[0].getblock(tips[0], 0))
    assert_equal(parent.hash_hex, tips[0])
    height = nodes[0].getblockcount() + 1
    # This fixture's subsidy is constant (default reward/halving settings).
    # Refuse a caller that changes the assumptions behind copied treasury.
    for node in nodes:
        assert "-b3blockinterval=1" in node.extra_args and "-b3roundseconds=1" in node.extra_args
        assert not any(arg.startswith(("-b3halvinginterval=", "-b3modernreward=", "-b3treasurypercent="))
                       for arg in node.extra_args)
    staking = nodes[0].getstakinginfo()
    assert staking["active_weight"] > 0 and staking["active_weight"] * 65536 >= staking["total_active_weight"]
    public_key = bytes.fromhex(staking["validator_key"])
    # CWallet stores its separate validator identity as an inactive pk(WIF)
    # descriptor. This is a newly created TEST wallet, not an operator export.
    # AuthServiceProxy normally logs full responses: suppress that logger for
    # this one call, restore it even on failure, and never include descriptors
    # or secret material in an assertion, exception, return value or artifact.
    was_disabled = rpc_log.disabled
    rpc_log.disabled = True
    try:
        descriptors = nodes[0].listdescriptors(True)["descriptors"]
    finally:
        rpc_log.disabled = was_disabled
    secret = None
    text, raw = None, None
    for row in descriptors:
        text = row["desc"].split("#", 1)[0]
        if not text.startswith("pk(") or not text.endswith(")"):
            continue
        try:
            raw, _ = base58_to_byte(text[3:-1])
        except (ValueError, AssertionError):
            continue
        if len(raw) == 33 and raw[-1] == 1 and compute_xonly_pubkey(raw[:32])[0] == public_key:
            assert secret is None, "Duplicate test validator descriptor"
            secret = raw[:32]
    del descriptors, text, raw
    assert secret is not None, "Fresh fixture validator descriptor unavailable"
    genesis = bytes.fromhex(nodes[0].getblockhash(0))[::-1]
    # -b3modernregtest pins final_legacy_hash == genesis, chainparams.cpp.
    domain = TaggedHash("B3/MODERN/CHAIN", genesis + genesis)
    transaction = ModernTransaction.from_hex(valid_hex)
    decoded = nodes[0].decoderawtransaction(valid_hex)
    assert_equal(transaction.txid_hex, decoded["txid"])
    assert_equal(hashlib.sha256(hashlib.sha256(transaction.serialize_modern()).digest()).digest()[::-1].hex(), decoded["ptxid"])
    variants = invalid_flowmesh_variants(valid_hex, decoded, record_type)
    timestamp = parent.nTime + 1 + 16  # Exact saturated round16, unchanged V1 rule.
    valid = build_modern_block(parent, transaction, height, timestamp, domain, secret)
    blocks = [(item, build_modern_block(parent, ModernTransaction.from_hex(item["hex"]),
                                        height, timestamp, domain, secret)) for item in variants]
    del secret
    assert len({valid.hash_hex, *(block.hash_hex for _, block in blocks)}) == 3
    evidence = {"scope": "actual_block_validation_submitblock", "block_parity_qualified": False,
                "record_type": record_type, "B3_parent": tips[0], "height": height,
                "engine_enabled": modes, "producer_key": public_key.hex(), "round": 16,
                "valid_txid": decoded["txid"], "valid_ptxid": decoded["ptxid"],
                "valid_block_hash": valid.hash_hex, "valid_block_hex": valid.serialize_modern().hex(),
                "invalid_variants": []}
    outbound = [[peer["addr"] for peer in node.getpeerinfo() if not peer["inbound"]] for node in nodes]
    assert all(address.startswith("127.0.0.1:") for addresses in outbound for address in addresses)
    isolated = []
    try:
        for node in nodes:
            node.setnetworkactive(False)
            isolated.append(node)
        assert_equal([node.getbestblockhash() for node in nodes], tips)
        advance_time(timestamp)
        proposal = {"mode": "proposal", "data": evidence["valid_block_hex"]}
        before = [node.getblocktemplate(proposal) for node in nodes]
        assert_equal(before, [None] * 5)
        evidence["valid_testblockvalidity_before"] = before
        for item, block in blocks:
            # MPA grammar fails at bad-mpa; both certificate/proof authority
            # mutants reach the consensus checkpoint index, not mempool RPC
            # authorization. Never accept merkle, witness or PoS failures.
            expected = "bad-mpa" if item["reject_reason"] == "bad-mpa" else "bad-flowmesh-checkpoint"
            raw_hex = block.serialize_modern().hex()
            decisions = []
            for node in nodes:
                with node.assert_debug_log([item["reject_detail_contains"]], timeout=5):
                    result = node.submitblock(raw_hex)
                    assert_equal(result, expected)
                decisions.append(result)
                assert_equal(node.getbestblockhash(), tips[0])
            evidence["invalid_variants"].append({
                "name": item["name"], "block_hash": block.hash_hex, "block_hex": raw_hex,
                "reject_reason": expected, "reject_detail_observed": item["reject_detail_contains"],
                "decisions": decisions})
        after = [node.getblocktemplate(proposal) for node in nodes]
        assert_equal(after, [None] * 5)
        evidence["valid_testblockvalidity_after"] = after
        decisions = [node.submitblock(evidence["valid_block_hex"]) for node in nodes]
        assert_equal(decisions, [None] * 5)
        assert_equal([node.getbestblockhash() for node in nodes], [valid.hash_hex] * 5)
        assert_equal([node.getblock(valid.hash_hex, 0) for node in nodes], [evidence["valid_block_hex"]] * 5)
        evidence["valid_submitblock_decisions"] = decisions
        evidence["block_parity_qualified"] = True
        return evidence
    finally:
        for node in isolated:
            node.setnetworkactive(True)
        for node, addresses in zip(nodes, outbound):
            for address in addresses:
                node.addnode(address, "onetry")


class TestFlowMeshValidityMutations(unittest.TestCase):
    """Synthetic byte fixtures only; no node, key or consensus claim."""

    @staticmethod
    def fixture(record_type, payload):
        # Arbitrary preserved bytes stand in for vin/vout/script signatures.
        prefix = b"\x02\x00\x00\x00\x00\x02" + b"preserved signed input and outputs"
        frame = b"\x01" + struct.pack("<HH", record_type, 1) + ser_compact_size(len(payload))
        raw = prefix + frame + payload + bytes(4)
        decoded = {"mpa": [{"type": record_type, "version": 1, "payload": payload.hex()}], "locktime": 0}
        return raw.hex(), decoded

    def test_checkpoint_mutations_preserve_every_non_payload_byte(self):
        payload = b"\x00\x01\x01" + bytes([0x11]) * 460 + b"\x07" + bytes([0x33]) * 96
        self.assertEqual(len(payload), 560)  # Existing C++ codec golden size.
        raw, decoded = self.fixture(8, payload)
        variants = invalid_flowmesh_variants(raw, decoded, 8)
        original = bytes.fromhex(raw)
        bad_version = bytes.fromhex(variants[0]["hex"])
        self.assertEqual([i for i, pair in enumerate(zip(original, bad_version)) if pair[0] != pair[1]], [variants[0]["payload_offset"]])
        bad_signature = bytes.fromhex(variants[1]["hex"])
        self.assertEqual(bad_signature[-100:-4], bytes(96))
        self.assertEqual(original[:-100], bad_signature[:-100])
        self.assertEqual(original[-4:], bad_signature[-4:])

    def test_withdrawal_changes_only_kind_or_checkpoint_not_effect(self):
        payload = b"\x02" + bytes([0x11]) * 32 + b"\x02" + bytes([0x22]) * 218 + struct.pack(">I", 0) + b"\x00"
        self.assertEqual(len(payload), 257)  # Existing zero-depth proof size.
        raw, decoded = self.fixture(9, payload)
        variants = invalid_flowmesh_variants(raw, decoded, 9)
        original = bytes.fromhex(raw)
        for item in variants:
            mutant = bytes.fromhex(item["hex"])
            self.assertEqual(mutant[:item["payload_offset"]], original[:item["payload_offset"]])
            self.assertEqual(mutant[item["payload_offset"] + 33:], original[item["payload_offset"] + 33:])
        self.assertEqual(bytes.fromhex(variants[1]["hex"])[variants[1]["payload_offset"] + 1:variants[1]["payload_offset"] + 33], bytes([0xff]) * 32)

    def test_inner_big_endian_is_distinct_from_outer_mpa_little_endian(self):
        payload = b"\x00\x01\x01" + bytes([0x11]) * 460 + b"\x07" + bytes([0x33]) * 96
        raw, decoded = self.fixture(8, payload)
        item = invalid_flowmesh_variants(raw, decoded, 8)[0]
        # Count1, type8 LE, version1 LE, length560 CompactSize, inner version1 BE.
        self.assertEqual(bytes.fromhex(raw)[item["payload_offset"] - 8:item["payload_offset"] + 3],
                         bytes.fromhex("0108000100fd3002000101"))
        raw, decoded = self.fixture(8, b"\x01\x00" + payload[2:])
        with self.assertRaises(AssertionError):
            invalid_flowmesh_variants(raw, decoded, 8)

    def test_wrong_record_tail_and_deposit_sweep_fail_closed(self):
        raw, decoded = self.fixture(9, b"\x02" + bytes([0x11]) * 256)
        with self.assertRaises(AssertionError):
            invalid_flowmesh_variants(raw + "00", decoded, 9)
        with self.assertRaises(AssertionError):
            invalid_flowmesh_variants(raw, decoded, 8)
        decoded["mpa"].append(decoded["mpa"][0])
        with self.assertRaises(AssertionError):
            invalid_flowmesh_variants(raw, decoded, 9)
        raw, decoded = self.fixture(9, b"\x01" + bytes([0x11]) * 260)
        with self.assertRaises(AssertionError):
            invalid_flowmesh_variants(raw, decoded, 9)
