#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Shared B3 bridge V2 vectors (doc/design/b3-bridge-v2-contracts-spec.md §2.5).

Writes one JSON file, bridge_v2_vectors.json, for the Foundry suite
(contracts/test/v2/VectorsV2.t.sol) and the B3 C++ unit tests. It holds
lineage ids, ruleset-2 SetHeaders, FinalizedBlockV2 encodings with digest V2
and certId, history leaves, roots and paths, leaf V2 with a withdrawal tree,
the bUSD V2 AssetId, the OI-12 K_OF_N bootstrap attestation message and digest
(contracts spec §5 step 3), the storage slots B3 proves by EIP-1186
(contracts spec §3.2, §6.6; Tether's mainnet layout) and the forwarder
section "forwarder_v2" (contracts spec §7.2, §8): F_d and F_x salts and
CREATE2 addresses, the EIP-191 refund-EOA message, the 67-byte kind 6 exit
block and the OI-2 deposit fee caps.

This generator is independent of the Solidity code: it uses only the python3
standard library (hashlib.sha256), the Keccak-256 implementation and the
secp256k1 public-key recovery below. It never touches the network, handles no
private key and is deterministic.

  gen_v2_vectors.py                 print the JSON to stdout
  gen_v2_vectors.py --out FILE      write FILE
  gen_v2_vectors.py --check [FILE]  exit 1 unless FILE matches byte for byte
                                    (default: bridge_v2_vectors.json beside
                                    this script)

Byte order: every hex value is in EVM order, i.e. big-endian and exactly
the bytes32 the contracts store and abi.encodePacked emits. B3 displays
uint256 hashes reversed (uint256::GetHex), so C++ readers compare raw bytes.
"""

import argparse
import difflib
import hashlib
import json
import sys
from pathlib import Path

SCHEMA = "b3-bridge-v2-vectors/1"
DEFAULT_FILE = Path(__file__).resolve().parent / "bridge_v2_vectors.json"

# -- Keccak-256 (original Keccak padding, as the EVM; not NIST SHA3-256) ------
_MASK64 = (1 << 64) - 1


def _keccak_tables():
    rotations = [[0] * 5 for _ in range(5)]
    x, y = 1, 0
    for t in range(24):
        rotations[x][y] = ((t + 1) * (t + 2) // 2) % 64
        x, y = y, (2 * x + 3 * y) % 5
    round_constants = []
    lfsr = 1
    for _ in range(24):
        constant = 0
        for j in range(7):
            if lfsr & 1:
                constant |= 1 << ((1 << j) - 1)
            lfsr = ((lfsr << 1) ^ (0x71 if lfsr & 0x80 else 0)) & 0xFF
        round_constants.append(constant)
    return rotations, round_constants


_ROTATIONS, _ROUND_CONSTANTS = _keccak_tables()


def _rol(value, shift):
    return ((value << shift) | (value >> (64 - shift))) & _MASK64


def _keccak_f1600(lanes):
    for round_constant in _ROUND_CONSTANTS:
        c = [lanes[x] ^ lanes[x + 5] ^ lanes[x + 10] ^ lanes[x + 15] ^ lanes[x + 20] for x in range(5)]
        d = [c[(x - 1) % 5] ^ _rol(c[(x + 1) % 5], 1) for x in range(5)]
        lanes = [lanes[i] ^ d[i % 5] for i in range(25)]
        b = [0] * 25
        for x in range(5):
            for y in range(5):
                b[y + 5 * ((2 * x + 3 * y) % 5)] = _rol(lanes[x + 5 * y], _ROTATIONS[x][y])
        lanes = [b[i] ^ (~b[(i + 1) % 5 + 5 * (i // 5)] & b[(i + 2) % 5 + 5 * (i // 5)]) for i in range(25)]
        lanes[0] ^= round_constant
    return lanes


def keccak256(data):
    rate = 136
    message = bytearray(data)
    message.append(0x01)
    while len(message) % rate:
        message.append(0)
    message[-1] |= 0x80
    lanes = [0] * 25
    for offset in range(0, len(message), rate):
        for i in range(rate // 8):
            lanes[i] ^= int.from_bytes(message[offset + 8 * i:offset + 8 * i + 8], "little")
        lanes = _keccak_f1600(lanes)
    return b"".join(lane.to_bytes(8, "little") for lane in lanes[:4])


# Self-check against published values before anything depends on it.
assert keccak256(b"").hex() == "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470"
assert keccak256(b"abc").hex() == "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45"


# -- secp256k1 public-key recovery (Ethereum ecrecover), affine arithmetic -----
_SECP_P = 2**256 - 2**32 - 977
_SECP_N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
_SECP_G = (
    0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
    0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8,
)


def _ec_add(a, b):
    if a is None:
        return b
    if b is None:
        return a
    if a[0] == b[0]:
        if (a[1] + b[1]) % _SECP_P == 0:
            return None
        slope = 3 * a[0] * a[0] * pow(2 * a[1], -1, _SECP_P) % _SECP_P
    else:
        slope = (b[1] - a[1]) * pow(b[0] - a[0], -1, _SECP_P) % _SECP_P
    x = (slope * slope - a[0] - b[0]) % _SECP_P
    return (x, (slope * (a[0] - x) - a[1]) % _SECP_P)


def _ec_mul(point, scalar):
    result = None
    while scalar:
        if scalar & 1:
            result = _ec_add(result, point)
        point = _ec_add(point, point)
        scalar >>= 1
    return result


def eth_address_of(point):
    """keccak256(X ‖ Y)[12:] of an uncompressed public key."""
    return keccak256(point[0].to_bytes(32, "big") + point[1].to_bytes(32, "big"))[12:]


def ecrecover(digest, signature):
    """The address that signed `digest` with the 65-byte r ‖ s ‖ v signature
    (v 27 or 28, low s as EIP-2 and cast produce)."""
    assert len(signature) == 65
    r = int.from_bytes(signature[:32], "big")
    s = int.from_bytes(signature[32:64], "big")
    v = signature[64]
    assert v in (27, 28) and 0 < r < _SECP_N and 0 < s <= _SECP_N // 2
    y_squared = (pow(r, 3, _SECP_P) + 7) % _SECP_P
    y = pow(y_squared, (_SECP_P + 1) // 4, _SECP_P)
    assert y * y % _SECP_P == y_squared
    if (y & 1) != v - 27:
        y = _SECP_P - y
    r_inverse = pow(r, -1, _SECP_N)
    e = int.from_bytes(digest, "big") % _SECP_N
    public_key = _ec_add(_ec_mul((r, y), s * r_inverse % _SECP_N), _ec_mul(_SECP_G, -e * r_inverse % _SECP_N))
    assert public_key is not None
    return eth_address_of(public_key)


# Self-check: private key 1 controls 0x7e5f…5bdf (a published value).
assert eth_address_of(_SECP_G).hex() == "7e5f4552091a69125d5dfcb7b8c2659029395bdf"


# -- encoding helpers ---------------------------------------------------------
def sha256(data):
    return hashlib.sha256(data).digest()


def tagged_hash(tag, message):
    """TaggedHash(t, m) = sha256(sha256(t) || sha256(t) || m)."""
    tag_hash = sha256(tag.encode())
    return sha256(tag_hash + tag_hash + message)


def u8(value):
    return value.to_bytes(1, "big")


def u16(value):
    return value.to_bytes(2, "big")


def u32(value):
    return value.to_bytes(4, "big")


def u64(value):
    return value.to_bytes(8, "big")


def u256(value):
    return value.to_bytes(32, "big")


def le64(value):
    return value.to_bytes(8, "little")


def b32(hex_string):
    value = bytes.fromhex(hex_string.removeprefix("0x"))
    assert len(value) == 32, hex_string
    return value


def address(hex_string):
    value = bytes.fromhex(hex_string.removeprefix("0x"))
    assert len(value) == 20, hex_string
    return value


def hx(data):
    return "0x" + bytes(data).hex()


# -- normative constants (contracts spec §2.2, §6.4; node changes §3.2) -------
TAG_FINALITY_V2 = "B3/FINALITY/V2"
TAG_LINEAGE_V2 = "B3/FINALITY/LINEAGE/V2"
TAG_ASSET_V2 = "B3/BRIDGE/ASSET/V2"
TAG_LEAF_V2 = "B3/BRIDGE/WITHDRAWAL/V2"
TAG_BOOTSTRAP_V2 = "B3/BRIDGE/BOOTSTRAP/V2"
LEAF_TAG_V2 = keccak256(TAG_LEAF_V2.encode())
assert hx(LEAF_TAG_V2) == "0xc250a4de311e4369a5d75a6fdd6351f1cc8ec35929170d1374a5367a5edd10f5"
# contracts/src/v2/B3TypesV2.sol BOOTSTRAP_TAG.
assert sha256(TAG_BOOTSTRAP_V2.encode()).hex() == "1e12d61c624b35a72fb66b8d7c57438dbf5d9324d885ec2e9c3362668e75dd8b"
BOOTSTRAP_MESSAGE_LENGTH = 301

RULESET_V2 = 2
HISTORY_DEPTH = 32
MEMBER_TREE_DEPTH = 13
ASSET_ID_VERSION = 2
ASSET_DECIMALS = 6

# Stand-in domain, deliberately non-symmetric so byte-order mistakes show.
STAND_IN_DOMAIN = bytes(range(32))
# contracts/deployments/ethereum-mainnet-v1.1.1.json:5 (b3_chain_domain).
MAINNET_DOMAIN = b32("a651262ae9031646047f290513d77ccae0fa9bd4e5afe7e07155a08d5dd1486a")
# getblockhash 0 of a -b3modernregtest -b3flowmeshtest node, display order
# (test/functional/test_framework/b3bridge.py REGTEST_MODERN_GENESIS_HASH).
REGTEST_MODERN_GENESIS_HASH = "10d0f5bb6fde880011fd56ea35dddbf9b32da2a7825f5579ac587a7904d6929a"

USDT = address("dac17f958d2ee523a2206206994597c13d831ec7")
STAND_IN_VAULT = address("22" * 20)
STAND_IN_RECIPIENT = address("33" * 20)
STAND_IN_ASSET_ID = b32("11" * 32)
STAND_IN_VERIFIER_REGTEST = address("44" * 20)
STAND_IN_VERIFIER_MAINNET = address("55" * 20)
STAND_IN_TOKEN_OWNER = address("66" * 20)
STAND_IN_UPGRADED_TOKEN = address("77" * 20)
# The EVM chain ids of anvil (regtest) and Ethereum mainnet.
REGTEST_EVM_CHAIN_ID = 31337
MAINNET_EVM_CHAIN_ID = 1

# Regtest fixture geometry (finality dependency §8 RT4 item 1: R_f = 262,
# R_f - 1 = 261, interval 5). V is the S4 verifier fixture's G + 50.
GENESIS_HEIGHT = 261
CHECKPOINT_INTERVAL = 5
BRIDGE_ACTIVATION_HEIGHT = GENESIS_HEIGHT + 50
HISTORY_LEAF_COUNT = 12

# The 1-member key of contracts/test/BlsCertificateVector.t.sol (B3's vendored
# blst, BlsK(1)), re-labelled ruleset 2: a real compressed G1 point.
MEMBER_PUBKEY = bytes.fromhex(
    "aa3b6ee8aadb63c8e064dafdc3d0ae794ded037af26e60829da1592db0a237dc42684307339f176c31d97714fd78aed2"
)
MEMBER_WEIGHT = 333


def regtest_chain_domain():
    """modern::ModernChainDomain(genesis, X) with X = genesis on regtest."""
    genesis_internal = bytes.fromhex(REGTEST_MODERN_GENESIS_HASH)[::-1]
    return tagged_hash("B3/MODERN/CHAIN", genesis_internal + genesis_internal)


def stand_in_block_hash(height):
    return sha256(b"B3/BRIDGE/V2/VECTOR/BLOCK" + u64(height))


# -- trees: keccak(left || right), zero-subtree padding, index bit l orders level l
def zero_hashes(depth):
    zeros = [bytes(32)]
    for _ in range(depth):
        zeros.append(keccak256(zeros[-1] + zeros[-1]))
    return zeros


def tree_root(leaves, depth):
    zeros = zero_hashes(depth)
    if not leaves:
        return zeros[depth]
    level = list(leaves)
    for height in range(depth):
        if len(level) % 2:
            level.append(zeros[height])
        level = [keccak256(level[i] + level[i + 1]) for i in range(0, len(level), 2)]
    assert len(level) == 1
    return level[0]


def tree_path(leaves, index, depth):
    zeros = zero_hashes(depth)
    assert index < len(leaves)
    siblings = []
    level = list(leaves)
    position = index
    for height in range(depth):
        sibling = position ^ 1
        siblings.append(level[sibling] if sibling < len(level) else zeros[height])
        if len(level) % 2:
            level.append(zeros[height])
        level = [keccak256(level[i] + level[i + 1]) for i in range(0, len(level), 2)]
        position >>= 1
    return siblings


def root_from_path(leaf, index, siblings):
    node = leaf
    for height, sibling in enumerate(siblings):
        node = keccak256(node + sibling) if ((index >> height) & 1) == 0 else keccak256(sibling + node)
    return node


# -- objects ------------------------------------------------------------------
def lineage_id(chain_domain, genesis_height, genesis_block_hash, genesis_set_hash):
    return tagged_hash(TAG_LINEAGE_V2, chain_domain + u64(genesis_height) + genesis_block_hash + genesis_set_hash)


def history_leaf(height, block_hash):
    return keccak256(u64(height) + block_hash)


def member_leaf(index, pubkey, weight):
    return keccak256(u32(index) + pubkey + u64(weight))


def encode_set_header(h):
    encoded = (
        u64(h["epoch"]) + u16(h["ruleset_version"]) + u32(h["validator_count"]) + u64(h["total_weight"])
        + u64(h["quorum_weight"]) + h["aggregate_pubkey"] + h["members_root"]
    )
    assert len(encoded) == 110
    return encoded


def encode_finalized_block(fb):
    encoded = (
        fb["lineage_id"] + u64(fb["height"]) + fb["block_hash"] + fb["withdrawal_root"] + fb["validator_set_hash"]
        + u64(fb["epoch"]) + u64(fb["checkpoint_index"]) + fb["checkpoint_history_root"] + u8(fb["bridge_flags"])
    )
    assert len(encoded) == 185
    return encoded


def digest_v2(chain_domain, fb):
    return tagged_hash(TAG_FINALITY_V2, chain_domain + encode_finalized_block(fb))


def bootstrap_message(chain_domain, evm_chain_id, verifier, start_set_hash, fb):
    """OI-12: chainDomain || u256 evmChainId || verifier || startSetHash || encode(fb)."""
    message = chain_domain + u256(evm_chain_id) + verifier + start_set_hash + encode_finalized_block(fb)
    assert len(message) == BOOTSTRAP_MESSAGE_LENGTH
    return message


def address_word(value):
    """A 20-byte address as a 32-byte storage word (12 zero bytes first)."""
    return bytes(12) + value


def mapping_slot(key_word, slot):
    """Solidity mapping entry: keccak256(key word || u256 slot)."""
    return keccak256(key_word + u256(slot))


def asset_id_preimage(tag, version, chain_domain, origin_chain_id, vault, token):
    tag_hash = sha256(tag.encode())
    preimage = (
        tag_hash + tag_hash + chain_domain + u8(version) + le64(origin_chain_id) + vault + token
        + u8(ASSET_DECIMALS) + u8(ASSET_DECIMALS)
    )
    assert len(preimage) == 147
    return preimage


def withdrawal_leaf_preimage(asset, withdrawal_id, chain_id, vault, token, recipient, amount, b3_height):
    preimage = (
        LEAF_TAG_V2 + asset + u64(withdrawal_id) + u64(chain_id) + vault + token + recipient + u256(amount)
        + u64(b3_height)
    )
    assert len(preimage) == 180
    return preimage


# -- anchors: the shared machinery reproduces committed V1 vectors ------------
def v1_anchors():
    # contracts/test/BridgeAssetIdVector.t.sol and src/test/bridge_asset_vector_tests.cpp.
    v1_asset_domain = bytes(range(32))
    v1_vault = address("11223344556677889900aabbccddeeff00112233")
    v1_asset = sha256(asset_id_preimage("B3/BRIDGE/ASSET/V1", 1, v1_asset_domain, 1, v1_vault, USDT))
    assert v1_asset.hex() == "72eca63ac9f140eee09f06cf86faed49cbe961b8b07e0dd291f76af30dc826d3"

    # contracts/test/BlsCertificateVector.t.sol (B3_BLST_DIGEST, from modern::FinalityDigest).
    v1_digest_domain = b32("d0" * 32)
    v1_fb = {
        "height": 811_001,
        "block_hash": b32("01" * 32),
        "withdrawal_root": b32("02" * 32),
        "validator_set_hash": b32("03" * 32),
        "epoch": 7,
    }
    v1_digest = tagged_hash(
        "B3/FINALITY/V1",
        v1_digest_domain + u64(v1_fb["height"]) + v1_fb["block_hash"] + v1_fb["withdrawal_root"]
        + v1_fb["validator_set_hash"] + u64(v1_fb["epoch"]),
    )
    assert v1_digest.hex() == "c02114826d2f2199a6bd2e186e51e25becd786c9a1b7464e7230cd76df098a65"

    return {
        "v1_asset_id": {
            "chain_domain": hx(v1_asset_domain),
            "origin_chain_id": 1,
            "vault": hx(v1_vault),
            "token": hx(USDT),
            "asset_id": hx(v1_asset),
        },
        "v1_finality_digest": {
            "chain_domain": hx(v1_digest_domain),
            "height": v1_fb["height"],
            "block_hash": hx(v1_fb["block_hash"]),
            "withdrawal_root": hx(v1_fb["withdrawal_root"]),
            "validator_set_hash": hx(v1_fb["validator_set_hash"]),
            "epoch": v1_fb["epoch"],
            "digest_v1": hx(v1_digest),
        },
    }


# -- sections -----------------------------------------------------------------
def set_headers():
    """Set'_0, Set'_1 (shared keys, finality dependency §6) and Set'_2."""
    leaves = [member_leaf(0, MEMBER_PUBKEY, MEMBER_WEIGHT)]
    members_root = tree_root(leaves, MEMBER_TREE_DEPTH)
    siblings = tree_path(leaves, 0, MEMBER_TREE_DEPTH)
    assert root_from_path(leaves[0], 0, siblings) == members_root

    headers = []
    for epoch in range(3):
        header = {
            "epoch": epoch,
            "ruleset_version": RULESET_V2,
            "validator_count": 1,
            "total_weight": MEMBER_WEIGHT,
            "quorum_weight": MEMBER_WEIGHT * 2 // 3 + 1,
            "aggregate_pubkey": MEMBER_PUBKEY,
            "members_root": members_root,
        }
        encoded = encode_set_header(header)
        headers.append({
            "epoch": epoch,
            "ruleset_version": RULESET_V2,
            "validator_count": header["validator_count"],
            "total_weight": header["total_weight"],
            "quorum_weight": header["quorum_weight"],
            "aggregate_pubkey": hx(MEMBER_PUBKEY),
            "members_root": hx(members_root),
            "members": [{
                "index": 0,
                "pubkey": hx(MEMBER_PUBKEY),
                "weight": MEMBER_WEIGHT,
                "leaf": hx(leaves[0]),
                "siblings": [hx(s) for s in siblings],
            }],
            "encoded": hx(encoded),
            "set_hash": hx(keccak256(encoded)),
        })
    return headers


def history():
    heights = [GENESIS_HEIGHT + i * CHECKPOINT_INTERVAL for i in range(HISTORY_LEAF_COUNT)]
    block_hashes = [stand_in_block_hash(h) for h in heights]
    leaves = [history_leaf(h, bh) for h, bh in zip(heights, block_hashes)]
    roots = [tree_root(leaves[:k], HISTORY_DEPTH) for k in range(HISTORY_LEAF_COUNT + 1)]
    assert roots[0] == zero_hashes(HISTORY_DEPTH)[HISTORY_DEPTH]

    # (count, index): leaf-0 paths for a bootstrap at index 1 and under a later
    # root; ancestry for consecutive certificates (index count-1) and after
    # skipped ones.
    path_cases = [(1, 0), (8, 0), (8, 7), (11, 0), (11, 5), (11, 10), (12, 11)]
    paths = []
    for count, index in path_cases:
        siblings = tree_path(leaves[:count], index, HISTORY_DEPTH)
        assert root_from_path(leaves[index], index, siblings) == roots[count]
        paths.append({
            "count": count,
            "index": index,
            "leaf": hx(leaves[index]),
            "root": hx(roots[count]),
            "siblings": [hx(s) for s in siblings],
        })

    return {
        "genesis_height": GENESIS_HEIGHT,
        "interval": CHECKPOINT_INTERVAL,
        "bridge_activation_height": BRIDGE_ACTIVATION_HEIGHT,
        "leaf_count": HISTORY_LEAF_COUNT,
        "leaves": [
            {"index": i, "height": heights[i], "block_hash": hx(block_hashes[i]), "leaf": hx(leaves[i])}
            for i in range(HISTORY_LEAF_COUNT)
        ],
        "roots": [hx(r) for r in roots],
        "paths": paths,
    }, heights, block_hashes, roots


def withdrawal_leaves():
    """Spec §6.4 vector plus a tree of ids 0..7 whose id-7 leaf is the vector."""
    amount = 1_000_000
    b3_height = 900_000
    chain_id = 1
    preimage = withdrawal_leaf_preimage(
        STAND_IN_ASSET_ID, 7, chain_id, STAND_IN_VAULT, USDT, STAND_IN_RECIPIENT, amount, b3_height
    )
    leaf = keccak256(preimage)
    assert hx(leaf) == "0x1cb905899ca0c488e158585c0208497d5415d1645035fbca3ca98ee2efb5ad64"

    leaf_v2 = [{
        "asset_id": hx(STAND_IN_ASSET_ID),
        "withdrawal_id": 7,
        "chain_id": chain_id,
        "vault": hx(STAND_IN_VAULT),
        "token": hx(USDT),
        "recipient": hx(STAND_IN_RECIPIENT),
        "amount": amount,
        "b3_height": b3_height,
        "preimage": hx(preimage),
        "leaf": hx(leaf),
    }]

    count = 8
    tree_leaves = [
        keccak256(withdrawal_leaf_preimage(
            STAND_IN_ASSET_ID, i, chain_id, STAND_IN_VAULT, USDT, STAND_IN_RECIPIENT, amount, b3_height
        ))
        for i in range(count)
    ]
    assert tree_leaves[7] == leaf
    root = tree_root(tree_leaves, HISTORY_DEPTH)
    paths = []
    for index in (0, 7):
        siblings = tree_path(tree_leaves, index, HISTORY_DEPTH)
        assert root_from_path(tree_leaves[index], index, siblings) == root
        paths.append({"index": index, "leaf": hx(tree_leaves[index]), "siblings": [hx(s) for s in siblings]})

    tree = {
        "asset_id": hx(STAND_IN_ASSET_ID),
        "chain_id": chain_id,
        "vault": hx(STAND_IN_VAULT),
        "token": hx(USDT),
        "recipient": hx(STAND_IN_RECIPIENT),
        "amount": amount,
        "b3_height": b3_height,
        "count": count,
        "leaves": [hx(x) for x in tree_leaves],
        "root": hx(root),
        "paths": paths,
    }
    return leaf_v2, tree, root


def asset_ids():
    preimage = asset_id_preimage(TAG_ASSET_V2, ASSET_ID_VERSION, MAINNET_DOMAIN, 1, STAND_IN_VAULT, USDT)
    asset = sha256(preimage)
    assert hx(asset) == "0xdb9e32397f3656fb2317c7829429f430cfefcf665c08a26774cb7b2fb3c76fcd"
    return [{
        "chain_domain": hx(MAINNET_DOMAIN),
        "origin_chain_id": 1,
        "vault": hx(STAND_IN_VAULT),
        "token": hx(USDT),
        "preimage": hx(preimage),
        "asset_id": hx(asset),
    }]


def bootstrap_attestations(headers, finalized_objects):
    """OI-12 (owner ruling R9): the K_OF_N bootstrap keys sign
    TaggedHash("B3/BRIDGE/BOOTSTRAP/V2", bootstrap_message(...)). Both cases
    attest finalized_block_v2[0] under another domain, chain id, verifier and
    start set, so every bound field changes the digest."""
    fb_index = 0
    fb = finalized_objects[fb_index]
    cases = [
        ("regtest", regtest_chain_domain(), REGTEST_EVM_CHAIN_ID, STAND_IN_VERIFIER_REGTEST, 0),
        ("mainnet", MAINNET_DOMAIN, MAINNET_EVM_CHAIN_ID, STAND_IN_VERIFIER_MAINNET, 1),
    ]
    out = []
    for name, domain, evm_chain_id, verifier, start_set in cases:
        start_set_hash = b32(headers[start_set]["set_hash"])
        message = bootstrap_message(domain, evm_chain_id, verifier, start_set_hash, fb)
        digest = tagged_hash(TAG_BOOTSTRAP_V2, message)
        # A bootstrap attestation never equals a committee certificate's message.
        assert digest != digest_v2(domain, fb)
        out.append({
            "name": name,
            "chain_domain": hx(domain),
            "evm_chain_id": evm_chain_id,
            "verifier": hx(verifier),
            "start_set": start_set,
            "start_set_hash": hx(start_set_hash),
            "finalized_block": fb_index,
            "fb_encoded": hx(encode_finalized_block(fb)),
            "message": hx(message),
            "digest": hx(digest),
        })
    assert out[0]["digest"] != out[1]["digest"]
    return out


# Storage layouts (contracts spec §3.2, §6.6; Tether's mainnet layout as
# contracts/regtest/MockTetherToken.sol mirrors it). Packing follows
# declaration order, first member in the low-order bits.
VERIFIER_SLOTS = {"liveness": 0, "current_set_hash": 1, "next_set_hash": 2, "latest": 4, "latest_cert_id": 6}
LIVENESS_FIELDS = [
    ("current_epoch", 0, 64),
    ("last_rotation_time", 64, 40),
    ("last_certificate_time", 104, 40),
    ("latest_bridge_height", 144, 64),
    ("status", 208, 8),
    ("deposits_closed", 216, 8),
]
LATEST_FIELDS = [("height", 0, 64), ("index", 64, 64), ("init_time", 128, 40)]
VAULT_SLOTS = {"next_deposit_id": 0, "locked": 1, "absorbed_total": 3}
VAULT_NEXT_DEPOSIT_ID_BITS = 64
TOKEN_SLOTS = {"owner_paused": 0, "balances": 2, "is_black_listed": 6, "upgraded_deprecated": 10}
TOKEN_SLOT0_FIELDS = [("owner", 0, 160), ("paused", 160, 8)]
TOKEN_SLOT10_FIELDS = [("upgraded_address", 0, 160), ("deprecated", 160, 8)]
ADDRESS_FIELDS = {"owner", "upgraded_address"}


def unused_from_bit(fields):
    end = 0
    for _, offset, bits in fields:
        assert offset == end, "fields are contiguous from bit 0"
        end = offset + bits
    return end


def packed_example(fields, values):
    word = 0
    entry = {}
    for name, offset, bits in fields:
        value = values[name]
        number = int.from_bytes(value, "big") if name in ADDRESS_FIELDS else value
        assert 0 <= number < (1 << bits), name
        word |= number << offset
        entry[name] = hx(value) if name in ADDRESS_FIELDS else value
    assert word < (1 << unused_from_bit(fields))
    return {"word": hx(u256(word)), **entry}


def field_list(fields):
    return [{"name": name, "offset": offset, "bits": bits} for name, offset, bits in fields]


def storage_layout():
    max40 = (1 << 40) - 1
    max64 = (1 << 64) - 1
    liveness_examples = [
        packed_example(LIVENESS_FIELDS, {
            "current_epoch": 3, "last_rotation_time": 1_750_016_911, "last_certificate_time": 1_750_017_244,
            "latest_bridge_height": 326, "status": 1, "deposits_closed": 0,
        }),
        packed_example(LIVENESS_FIELDS, {
            "current_epoch": max64, "last_rotation_time": max40, "last_certificate_time": max40,
            "latest_bridge_height": max64, "status": 3, "deposits_closed": 1,
        }),
    ]
    latest_examples = [
        packed_example(LATEST_FIELDS, {"height": 276, "index": 3, "init_time": 1_750_000_111}),
        packed_example(LATEST_FIELDS, {"height": max64, "index": max64, "init_time": max40}),
    ]
    vault_examples = [
        {"slot": VAULT_SLOTS["next_deposit_id"], "word": hx(u256(2)), "value": 2},
        {"slot": VAULT_SLOTS["next_deposit_id"], "word": hx(u256(max64)), "value": max64},
        {"slot": VAULT_SLOTS["locked"], "word": hx(u256(1_040_000_000)), "value": 1_040_000_000},
        {"slot": VAULT_SLOTS["absorbed_total"], "word": hx(u256(60_000_000)), "value": 60_000_000},
    ]
    token_slot0_examples = [
        packed_example(TOKEN_SLOT0_FIELDS, {"owner": STAND_IN_TOKEN_OWNER, "paused": 0}),
        packed_example(TOKEN_SLOT0_FIELDS, {"owner": STAND_IN_TOKEN_OWNER, "paused": 1}),
    ]
    token_slot10_examples = [
        packed_example(TOKEN_SLOT10_FIELDS, {"upgraded_address": bytes(20), "deprecated": 0}),
        packed_example(TOKEN_SLOT10_FIELDS, {"upgraded_address": STAND_IN_UPGRADED_TOKEN, "deprecated": 1}),
    ]

    balances = mapping_slot(address_word(STAND_IN_VAULT), TOKEN_SLOTS["balances"])
    blacklisted = mapping_slot(address_word(STAND_IN_VAULT), TOKEN_SLOTS["is_black_listed"])
    # cast 1.7.1: cast index address 0x2222222222222222222222222222222222222222 2 (and 6).
    assert balances.hex() == "74b48303b668c996c09b610daf2f53f83448dd15b9ac57e9e8fb1ff2f745c71d"
    assert blacklisted.hex() == "0a3e3a135c9eb9a4f916a39edbe9cf899cba91b2a6ec56f43a0ae8ee4b3185de"

    return {
        "verifier": {
            "slots": VERIFIER_SLOTS,
            "liveness_fields": field_list(LIVENESS_FIELDS),
            "liveness_unused_from_bit": unused_from_bit(LIVENESS_FIELDS),
            "liveness_examples": liveness_examples,
            "latest_fields": field_list(LATEST_FIELDS),
            "latest_unused_from_bit": unused_from_bit(LATEST_FIELDS),
            "latest_examples": latest_examples,
        },
        "vault": {
            "slots": VAULT_SLOTS,
            "next_deposit_id_bits": VAULT_NEXT_DEPOSIT_ID_BITS,
            "examples": vault_examples,
        },
        "token": {
            "slots": TOKEN_SLOTS,
            "slot0_fields": field_list(TOKEN_SLOT0_FIELDS),
            "slot0_unused_from_bit": unused_from_bit(TOKEN_SLOT0_FIELDS),
            "slot0_examples": token_slot0_examples,
            "slot10_fields": field_list(TOKEN_SLOT10_FIELDS),
            "slot10_unused_from_bit": unused_from_bit(TOKEN_SLOT10_FIELDS),
            "slot10_examples": token_slot10_examples,
        },
        "mapping_slots": [
            {
                "name": "token.balances[vault]",
                "key": hx(STAND_IN_VAULT),
                "slot": TOKEN_SLOTS["balances"],
                "storage_key": hx(balances),
            },
            {
                "name": "token.isBlackListed[vault]",
                "key": hx(STAND_IN_VAULT),
                "slot": TOKEN_SLOTS["is_black_listed"],
                "storage_key": hx(blacklisted),
            },
        ],
    }


# -- forwarders (contracts spec §7.2, §8; node changes §7.2, §11.2) -----------
TAG_FWD_DEPOSIT = "B3/FWD/DEPOSIT/V2"
TAG_FWD_EXIT = "B3/FWD/EXIT/OFT/V2"
FWD_DEPOSIT_TAG = keccak256(TAG_FWD_DEPOSIT.encode())
FWD_EXIT_TAG = keccak256(TAG_FWD_EXIT.encode())
# contracts/src/v2/B3ForwarderTypesV2.sol DEPOSIT_TAG and EXIT_TAG (cast keccak).
assert hx(FWD_DEPOSIT_TAG) == "0xcc2b88b5788e6a3d2f51c52fcb908e55a5386859799df6e4878912fdb1b01d2a"
assert hx(FWD_EXIT_TAG) == "0x0fdc29b9af6b10d38ef4781cb890c8a6d762464db73cc2f5800457cdf07b4c87"
# A stand-in: the real FORWARDER_INIT_CODE_HASH commits to B3Forwarder's
# metadata and is read from the build (DeployV2 manifest), never from here.
FWD_INIT_CODE_HASH_LABEL = "B3-VECTORS/forwarder-init-code"
FWD_INIT_CODE_HASH = keccak256(FWD_INIT_CODE_HASH_LABEL.encode())
FWD_SALT_PREIMAGE_LENGTH = 192
FWD_CREATE2_PREIMAGE_LENGTH = 85
FWD_MAX_BRIDGE_FEE_BPS = 50
# LayerZero endpoint ids: Ethereum (regtest anvil 31337), Arbitrum (regtest anvil 31338).
EID_ETHEREUM = 30101
EID_ARBITRUM = 30110
# Burn record exit block (node changes §7.2 item 5): u8 exit_version ‖ u32
# dst_eid ‖ to word ‖ u16 max_bridge_fee_bps ‖ u64 fee_cap ‖ refund_eoa.
EXIT_BLOCK_VERSION = 1
EXIT_BLOCK_LENGTH = 67
# contracts/script/DeployV2.s.sol DEFAULT_MAX_DEPOSIT_RAW (10,000 USDT).
MAX_DEPOSIT_RAW = 10_000_000_000
U64_MAX = (1 << 64) - 1
# Anvil's dev accounts 0 and 1 (public test keys of the "test … junk"
# mnemonic); the refund-EOA vectors carry their EIP-191 signatures.
ANVIL_ACCOUNT_0 = address("f39fd6e51aad88f6f4ce6ab8827279cfffb92266")
ANVIL_ACCOUNT_1 = address("70997970c51812dc3a010c7d01b50e0d17dc79c8")
REFUND_EOA_MESSAGE_PREFIX = "B3 bridge V2 refund EOA"
# cast 1.7.1, offline: cast wallet sign --mnemonic "test test test test test
# test test test test test test junk" --mnemonic-index <0|1> "<message>",
# over the refund_eoa_message of each case; ecrecover below checks them.
REFUND_EOA_SIGNATURES = {
    "regtest": "0xa3000ca3798b9de067a8f029e2ff2420a16a790042a9f67d7c166d583782316c"
    "2f08e9f568da67c99585920b84935688a44357bf0cce08a77a17f1de9e332c751b",
    "mainnet": "0x8f7e5401ac3a4170c62f5c573f4bca418d25da232d07d00ea87b5ac6c67e98e2"
    "1b61fd792cc0bda0ed6ab2dbddf3270be53f3876ca7ea97cadfbfc8cd97d1c8f1c",
}


def fwd_stand_in(label):
    """A non-symmetric stand-in address, so byte-order mistakes show."""
    return keccak256(("B3-VECTORS/forwarder/" + label).encode())[12:]


def uint_word(value, bits):
    assert 0 <= value < (1 << bits), (value, bits)
    return u256(value)


def recipient_v1(key_hash):
    """RECIPIENT_V1: 11 zero bytes, version 1 (P2PKH), the 20-byte key hash."""
    assert len(key_hash) == 20 and any(key_hash)
    return bytes(11) + u8(1) + key_hash


def deposit_salt_preimage(vault, b3_recipient, refund_eoa, fee_cap, not_after):
    """abi.encode(DEPOSIT_TAG, address vault, bytes32 b3Recipient, address
    refundEOA, uint96 feeCap, uint64 notAfter)."""
    preimage = (
        FWD_DEPOSIT_TAG + address_word(vault) + b3_recipient + address_word(refund_eoa) + uint_word(fee_cap, 96)
        + uint_word(not_after, 64)
    )
    assert len(preimage) == FWD_SALT_PREIMAGE_LENGTH
    return preimage


def exit_salt_preimage(dst_eid, to, max_bridge_fee_bps, fee_cap, refund_eoa):
    """abi.encode(EXIT_TAG, uint32 dstEid, bytes32 to, uint16 maxBridgeFeeBps,
    uint96 feeCap, address refundEOA)."""
    preimage = (
        FWD_EXIT_TAG + uint_word(dst_eid, 32) + to + uint_word(max_bridge_fee_bps, 16) + uint_word(fee_cap, 96)
        + address_word(refund_eoa)
    )
    assert len(preimage) == FWD_SALT_PREIMAGE_LENGTH
    return preimage


def create2_address(factory, salt, init_code_hash):
    """EIP-1014: keccak256(0xff ‖ factory ‖ salt ‖ initCodeHash)[12:]."""
    preimage = b"\xff" + factory + salt + init_code_hash
    assert len(preimage) == FWD_CREATE2_PREIMAGE_LENGTH
    return keccak256(preimage)[12:]


def exit_params_valid(dst_eid, to, max_bridge_fee_bps, refund_eoa, pinned_dst_eid):
    """B3OftExitFactory.isValid without EOA-ness (contracts spec §8)."""
    to_value = int.from_bytes(to, "big")
    return (
        pinned_dst_eid != 0 and dst_eid == pinned_dst_eid and to_value >> 160 == 0 and to_value != 0
        and max_bridge_fee_bps <= FWD_MAX_BRIDGE_FEE_BPS and any(refund_eoa)
    )


def refund_eoa_message(chain_domain, refund_eoa):
    """The text a wallet asks the refund EOA to sign (EIP-191 personal_sign):
    the raw chain-domain bytes and the lowercase address, both 0x-prefixed."""
    return f"{REFUND_EOA_MESSAGE_PREFIX}\nchain domain: {hx(chain_domain)}\nrefund EOA: {hx(refund_eoa)}"


def eip191_digest(message):
    data = message.encode()
    return keccak256(b"\x19Ethereum Signed Message:\n" + str(len(data)).encode() + data)


def encode_exit_block(dst_eid, to, max_bridge_fee_bps, fee_cap, refund_eoa):
    encoded = u8(EXIT_BLOCK_VERSION) + u32(dst_eid) + to + u16(max_bridge_fee_bps) + u64(fee_cap) + refund_eoa
    assert len(encoded) == EXIT_BLOCK_LENGTH
    return encoded


DEPOSIT_FEE_CAP_TXS = 4


def deposit_fee_cap(fee_per_tx):
    """OI-2 (P7 DepositForwarderFeeCapV2): the wallet caps a route (b)
    delivery at MAX_DEPOSIT_RAW per forwarder, so one flush deposits all of
    it, and fee_per_tx × 4 pays deploy, two flushes (one for a repeat
    arrival) and returnToOwner; it must fit uint64."""
    assert fee_per_tx > 0
    fee_cap = fee_per_tx * DEPOSIT_FEE_CAP_TXS
    assert fee_cap <= U64_MAX
    return fee_cap


def forwarders():
    """F_d and F_x salts and accounts, refund-EOA messages, exit blocks and the
    OI-2 fee caps. Factories, vaults, recipients, EOAs (except the anvil dev
    accounts) and the init-code hash are stand-ins."""
    deposit_factory = fwd_stand_in("deposit-factory")
    deposit_factory_successor = fwd_stand_in("deposit-factory-successor")
    vault = fwd_stand_in("vault")
    vault_successor = fwd_stand_in("vault-successor")
    exit_factory = fwd_stand_in("exit-factory")
    exit_factory_other = fwd_stand_in("exit-factory-ethereum")

    deposit_cases = [
        # (name, factory, vault, recipient key hash, refund EOA, feeCap, notAfter);
        # the feeCaps are stand-ins, not wallet derivations.
        ("launch_default", deposit_factory, vault, fwd_stand_in("b3-key-0"), ANVIL_ACCOUNT_0,
         15_000_000, 1_794_000_000),
        ("u64_max_fee_cap", deposit_factory, vault, fwd_stand_in("b3-key-1"), fwd_stand_in("refund-eoa-1"),
         U64_MAX, 1),
        ("successor_zero_fee_cap", deposit_factory_successor, vault_successor, fwd_stand_in("b3-key-2"),
         fwd_stand_in("refund-eoa-2"), 0, U64_MAX),
    ]
    deposit = []
    for name, factory, case_vault, key_hash, refund_eoa, fee_cap, not_after in deposit_cases:
        b3_recipient = recipient_v1(key_hash)
        preimage = deposit_salt_preimage(case_vault, b3_recipient, refund_eoa, fee_cap, not_after)
        salt = keccak256(preimage)
        account = create2_address(factory, salt, FWD_INIT_CODE_HASH)
        # OI-6: no derivation yields the vault (or the factory).
        assert account not in (case_vault, factory)
        deposit.append({
            "name": name,
            "factory": hx(factory),
            "vault": hx(case_vault),
            "b3_recipient": hx(b3_recipient),
            "refund_eoa": hx(refund_eoa),
            "fee_cap": fee_cap,
            "not_after": not_after,
            "salt_preimage": hx(preimage),
            "salt": hx(salt),
            "account": hx(account),
        })

    exit_cases = [
        # (name, factory, dstEid, `to` address, maxBridgeFeeBps, feeCap, refund EOA)
        ("arbitrum_default", exit_factory, EID_ARBITRUM, fwd_stand_in("arbitrum-to-0"), 0, 3_000_000,
         ANVIL_ACCOUNT_0),
        ("arbitrum_max_bps_u64_fee_cap", exit_factory, EID_ARBITRUM, fwd_stand_in("arbitrum-to-1"),
         FWD_MAX_BRIDGE_FEE_BPS, U64_MAX, fwd_stand_in("refund-eoa-3")),
        ("ethereum_eid_zero_fee_cap", exit_factory_other, EID_ETHEREUM, fwd_stand_in("ethereum-to-2"), 25, 0,
         fwd_stand_in("refund-eoa-4")),
    ]
    exits = []
    burn_exit_blocks = []
    for index, (name, factory, dst_eid, to_address, bps, fee_cap, refund_eoa) in enumerate(exit_cases):
        to = address_word(to_address)
        preimage = exit_salt_preimage(dst_eid, to, bps, fee_cap, refund_eoa)
        salt = keccak256(preimage)
        account = create2_address(factory, salt, FWD_INIT_CODE_HASH)
        exits.append({
            "name": name,
            "factory": hx(factory),
            "dst_eid": dst_eid,
            "to": hx(to),
            "max_bridge_fee_bps": bps,
            "fee_cap": fee_cap,
            "refund_eoa": hx(refund_eoa),
            "is_valid": exit_params_valid(dst_eid, to, bps, refund_eoa, EID_ARBITRUM),
            "salt_preimage": hx(preimage),
            "salt": hx(salt),
            "account": hx(account),
        })
        burn_exit_blocks.append({
            "exit_case": index,
            "recipient": hx(account),
            "fields": {
                "exit_version": EXIT_BLOCK_VERSION,
                "dst_eid": dst_eid,
                "to": hx(to),
                "max_bridge_fee_bps": bps,
                "fee_cap": fee_cap,
                "refund_eoa": hx(refund_eoa),
            },
            "encoded": hx(encode_exit_block(dst_eid, to, bps, fee_cap, refund_eoa)),
        })
    assert [e["is_valid"] for e in exits] == [True, True, False]

    refund_messages = []
    for name, domain, refund_eoa in (
        ("regtest", regtest_chain_domain(), ANVIL_ACCOUNT_0),
        ("mainnet", MAINNET_DOMAIN, ANVIL_ACCOUNT_1),
    ):
        message = refund_eoa_message(domain, refund_eoa)
        digest = eip191_digest(message)
        signature = bytes.fromhex(REFUND_EOA_SIGNATURES[name].removeprefix("0x"))
        assert ecrecover(digest, signature) == refund_eoa, name
        refund_messages.append({
            "name": name,
            "chain_domain": hx(domain),
            "refund_eoa": hx(refund_eoa),
            "message": message,
            "eip191_digest": hx(digest),
            "signature": hx(signature),
        })

    fee_cap_cases = [
        1,
        1_000_000,  # the wallet default
        2_500_000,
        # The uint64 boundary: the largest fee_per_tx whose × 4 fits.
        U64_MAX // DEPOSIT_FEE_CAP_TXS,
    ]
    fee_caps = [{"fee_per_tx": fee_per_tx, "fee_cap": deposit_fee_cap(fee_per_tx)} for fee_per_tx in fee_cap_cases]
    assert (fee_caps[-1]["fee_per_tx"] + 1) * DEPOSIT_FEE_CAP_TXS > U64_MAX

    return {
        "tags": {"deposit": hx(FWD_DEPOSIT_TAG), "exit": hx(FWD_EXIT_TAG)},
        "init_code_hash": hx(FWD_INIT_CODE_HASH),
        "pinned_dst_eid": EID_ARBITRUM,
        "deposit": deposit,
        "exit": exits,
        "refund_eoa_message": refund_messages,
        "burn_exit_block": burn_exit_blocks,
        "deposit_fee_cap": fee_caps,
    }


def build():
    headers = set_headers()
    history_section, heights, block_hashes, roots = history()
    leaf_v2, withdrawal_tree, withdrawal_root = withdrawal_leaves()

    genesis_block_hash = block_hashes[0]
    genesis_set_hash = b32(headers[0]["set_hash"])
    domains = [
        ("stand_in", STAND_IN_DOMAIN),
        ("mainnet", MAINNET_DOMAIN),
        ("regtest", regtest_chain_domain()),
    ]
    lineage = []
    lineage_ids = {}
    for name, domain in domains:
        value = lineage_id(domain, GENESIS_HEIGHT, genesis_block_hash, genesis_set_hash)
        lineage_ids[name] = value
        lineage.append({
            "name": name,
            "chain_domain": hx(domain),
            "genesis_height": GENESIS_HEIGHT,
            "genesis_block_hash": hx(genesis_block_hash),
            "genesis_set_hash": hx(genesis_set_hash),
            "lineage_id": hx(value),
        })

    # (lineage, index, epoch, flags): the bootstrap shape below V, then
    # certificates at exactly V and above V; the last one carries bit 0.
    cases = [(0, 1, 0, 0), (2, 10, 1, 0), (1, 11, 1, 1)]
    finalized = []
    finalized_objects = []
    for lineage_index, index, epoch, flags in cases:
        name, domain = domains[lineage_index]
        height = heights[index]
        fb = {
            "lineage_id": lineage_ids[name],
            "height": height,
            "block_hash": block_hashes[index],
            "withdrawal_root": withdrawal_root if height >= BRIDGE_ACTIVATION_HEIGHT else bytes(32),
            "validator_set_hash": b32(headers[epoch + 1]["set_hash"]),
            "epoch": epoch,
            "checkpoint_index": index,
            "checkpoint_history_root": roots[index],
            "bridge_flags": flags,
        }
        encoded = encode_finalized_block(fb)
        finalized_objects.append(fb)
        finalized.append({
            "lineage": lineage_index,
            "chain_domain": hx(domain),
            "lineage_id": hx(fb["lineage_id"]),
            "height": fb["height"],
            "block_hash": hx(fb["block_hash"]),
            "withdrawal_root": hx(fb["withdrawal_root"]),
            "validator_set_hash": hx(fb["validator_set_hash"]),
            "epoch": fb["epoch"],
            "checkpoint_index": fb["checkpoint_index"],
            "checkpoint_history_root": hx(fb["checkpoint_history_root"]),
            "bridge_flags": fb["bridge_flags"],
            "encoded": hx(encoded),
            "digest_v2": hx(digest_v2(domain, fb)),
            "cert_id": hx(keccak256(encoded)),
        })

    return {
        "schema": SCHEMA,
        "generator": "contracts/test/vectors/gen_v2_vectors.sh",
        "notes": [
            "Hex values are in EVM byte order (big-endian bytes32, as abi.encodePacked).",
            "Block hashes, the vault, recipient and the 0x11 asset id are stand-ins.",
            "set_header_v2 reuses the real BLS key of contracts/test/BlsCertificateVector.t.sol with ruleset 2.",
            "history.roots[k] is the depth-32 root over leaves 0..k-1; roots[0] is the empty root zero_32.",
            "forwarder_v2 factories, vaults, recipients, init_code_hash (keccak256 of "
            "\"B3-VECTORS/forwarder-init-code\") and EOAs other than anvil dev accounts 0 and 1 are stand-ins.",
            "bootstrap_attestation_v2 verifiers and the storage_layout_v2 owner, upgraded token and vault are stand-ins.",
        ],
        "tags": {
            "finality_v2": hx(sha256(TAG_FINALITY_V2.encode())),
            "lineage_v2": hx(sha256(TAG_LINEAGE_V2.encode())),
            "asset_v2": hx(sha256(TAG_ASSET_V2.encode())),
            "leaf_v2": hx(LEAF_TAG_V2),
            "bootstrap_v2": hx(sha256(TAG_BOOTSTRAP_V2.encode())),
        },
        "anchors": v1_anchors(),
        "lineage": lineage,
        "set_header_v2": headers,
        "finalized_block_v2": finalized,
        "history": history_section,
        "leaf_v2": leaf_v2,
        "withdrawal_tree_v2": withdrawal_tree,
        "asset_id_v2": asset_ids(),
        "bootstrap_attestation_v2": bootstrap_attestations(headers, finalized_objects),
        "storage_layout_v2": storage_layout(),
        "forwarder_v2": forwarders(),
    }


def render():
    return json.dumps(build(), indent=2) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--out", metavar="FILE", help="write the vectors to FILE")
    mode.add_argument("--check", metavar="FILE", nargs="?", const=str(DEFAULT_FILE),
                      help="compare FILE (default: the committed vectors) with a fresh render")
    args = parser.parse_args()

    text = render()
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
        return 0
    if args.check:
        path = Path(args.check)
        current = path.read_text(encoding="utf-8") if path.exists() else ""
        if current == text:
            print(f"{path}: up to date")
            return 0
        sys.stderr.writelines(difflib.unified_diff(
            current.splitlines(keepends=True), text.splitlines(keepends=True), str(path), "generated"))
        print(f"{path}: stale; regenerate with gen_v2_vectors.sh", file=sys.stderr)
        return 1
    sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
