#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Shared B3 bridge V2 vectors (doc/design/b3-bridge-v2-contracts-spec.md §2.5).

Writes one JSON file, bridge_v2_vectors.json, for the Foundry suite
(contracts/test/v2/VectorsV2.t.sol) and the B3 C++ unit tests. It holds
lineage ids, ruleset-2 SetHeaders, FinalizedBlockV2 encodings with digest V2
and certId, history leaves, roots and paths, leaf V2 with a withdrawal tree
and the bUSD V2 AssetId. F_d and F_x address vectors join with the forwarder
slice.

This generator is independent of the Solidity code: it uses only the python3
standard library (hashlib.sha256) and the Keccak-256 implementation below. It
never touches the network and is deterministic.

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
LEAF_TAG_V2 = keccak256(TAG_LEAF_V2.encode())
assert hx(LEAF_TAG_V2) == "0xc250a4de311e4369a5d75a6fdd6351f1cc8ec35929170d1374a5367a5edd10f5"

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
            "F_d and F_x address vectors are added with the forwarder slice.",
        ],
        "tags": {
            "finality_v2": hx(sha256(TAG_FINALITY_V2.encode())),
            "lineage_v2": hx(sha256(TAG_LINEAGE_V2.encode())),
            "asset_v2": hx(sha256(TAG_ASSET_V2.encode())),
            "leaf_v2": hx(LEAF_TAG_V2),
        },
        "anchors": v1_anchors(),
        "lineage": lineage,
        "set_header_v2": headers,
        "finalized_block_v2": finalized,
        "history": history_section,
        "leaf_v2": leaf_v2,
        "withdrawal_tree_v2": withdrawal_tree,
        "asset_id_v2": asset_ids(),
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
