#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Capture a real Ethereum mainnet receipts-trie inclusion proof as a C++ fixture.

Bridge proposal stage 2 ("captured offline, checked in as hex"). The script
rebuilds the ENTIRE receipts trie of a finalized block from eth_getBlockReceipts
using an independent pure-Python Keccak/RLP/MPT implementation and requires the
computed root to equal the block header's receiptsRoot before emitting anything
-- so the fixture is anchored to Ethereum mainnet consensus, not to this script.

Usage: capture_eth_receipts_fixture.py [RPC_URL] [BLOCK_TAG] > fixture.h
"""
import json
import sys
import urllib.request

RPC = sys.argv[1] if len(sys.argv) > 1 else "https://ethereum-rpc.publicnode.com"
TAG = sys.argv[2] if len(sys.argv) > 2 else "finalized"

# ---------------- keccak-256 (pure python, FIPS-202 permutation, pad 0x01) ---
ROT = [[0, 36, 3, 41, 18], [1, 44, 10, 45, 2], [62, 6, 43, 15, 61],
       [28, 55, 25, 21, 56], [27, 20, 39, 8, 14]]
RC = [0x0000000000000001, 0x0000000000008082, 0x800000000000808A, 0x8000000080008000,
      0x000000000000808B, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
      0x000000000000008A, 0x0000000000000088, 0x0000000080008009, 0x000000008000000A,
      0x000000008000808B, 0x800000000000008B, 0x8000000000008089, 0x8000000000008003,
      0x8000000000008002, 0x8000000000000080, 0x000000000000800A, 0x800000008000000A,
      0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008]
M = (1 << 64) - 1


def _rotl(x, n):
    return ((x << n) | (x >> (64 - n))) & M


def _keccak_f(a):
    for rnd in range(24):
        c = [a[x][0] ^ a[x][1] ^ a[x][2] ^ a[x][3] ^ a[x][4] for x in range(5)]
        d = [c[(x - 1) % 5] ^ _rotl(c[(x + 1) % 5], 1) for x in range(5)]
        for x in range(5):
            for y in range(5):
                a[x][y] ^= d[x]
        b = [[0] * 5 for _ in range(5)]
        for x in range(5):
            for y in range(5):
                b[y][(2 * x + 3 * y) % 5] = _rotl(a[x][y], ROT[x][y])
        for x in range(5):
            for y in range(5):
                a[x][y] = b[x][y] ^ ((~b[(x + 1) % 5][y]) & b[(x + 2) % 5][y])
        a[0][0] ^= RC[rnd]


def keccak256(data: bytes) -> bytes:
    rate = 136
    a = [[0] * 5 for _ in range(5)]
    data = data + b"\x01" + b"\x00" * (rate - 1 - len(data) % rate)
    data = data[: len(data) - 1] + bytes([data[-1] | 0x80])
    for off in range(0, len(data), rate):
        blk = data[off:off + rate]
        for i in range(rate // 8):
            lane = int.from_bytes(blk[8 * i:8 * i + 8], "little")
            a[i % 5][i // 5] ^= lane
        _keccak_f(a)
    out = b""
    for i in range(4):
        out += (a[i % 5][i // 5]).to_bytes(8, "little")
    return out


# ---------------- RLP ---------------------------------------------------------
def rlp(x):
    if isinstance(x, int):
        assert x >= 0
        x = x.to_bytes((x.bit_length() + 7) // 8, "big") if x else b""
    if isinstance(x, (bytes, bytearray)):
        x = bytes(x)
        if len(x) == 1 and x[0] < 0x80:
            return x
        return _len_prefix(len(x), 0x80) + x
    assert isinstance(x, (list, tuple))
    body = b"".join(rlp(i) for i in x)
    return _len_prefix(len(body), 0xC0) + body


def _len_prefix(n, offset):
    if n <= 55:
        return bytes([offset + n])
    nb = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([offset + 55 + len(nb)]) + nb


# ---------------- MPT (build + prove) -----------------------------------------
def _hp(nibbles, leaf):
    flags = 2 if leaf else 0
    if len(nibbles) % 2:
        out = [(flags | 1) << 4 | nibbles[0]]
        rest = nibbles[1:]
    else:
        out = [flags << 4]
        rest = nibbles
    for i in range(0, len(rest), 2):
        out.append(rest[i] << 4 | rest[i + 1])
    return bytes(out)


def _nibbles(key: bytes):
    out = []
    for b in key:
        out += [b >> 4, b & 0x0F]
    return out


def _build(pairs):
    """pairs: list of (nibble_list, value). Returns a node structure."""
    assert pairs
    if len(pairs) == 1:
        nib, val = pairs[0]
        return [_hp(nib, True), val]
    # longest common prefix
    first = pairs[0][0]
    lcp = 0
    while all(len(p[0]) > lcp and p[0][lcp] == first[lcp] for p in pairs) and lcp < len(first):
        lcp += 1
    if lcp:
        child = _build([(nib[lcp:], v) for nib, v in pairs])
        return [_hp(first[:lcp], False), _ref(child)]
    branch = [b""] * 17
    for d in range(16):
        sub = [(nib[1:], v) for nib, v in pairs if nib and nib[0] == d]
        if sub:
            branch[d] = _ref(_build(sub))
    term = [v for nib, v in pairs if not nib]
    if term:
        branch[16] = term[0]
    return branch


def _ref(node):
    enc = rlp(node)
    return node if len(enc) < 32 else keccak256(enc)


def _root(node):
    return keccak256(rlp(node))


def _prove(node, nib, proof):
    """Collect the hashed-node chain for key nibbles nib starting at node."""
    proof.append(rlp(node))
    while True:
        if len(node) == 17:
            if not nib:
                return
            child = node[nib[0]]
            nib = nib[1:]
        else:
            path_is_leaf = node[0][0] >> 4 in (2, 3)
            odd = (node[0][0] >> 4) & 1
            plen = (len(node[0]) - 1) * 2 + odd
            nib = nib[plen:]
            if path_is_leaf:
                return
            child = node[1]
        if isinstance(child, list):
            node = child  # inline, not an independent proof node
        else:
            assert isinstance(child, bytes) and len(child) == 32
            # find by re-walk impossible; we carry structure, so this branch
            # is only reached for hashed children captured in _build via _ref.
            raise RuntimeError("hashed child lost structure")


class Trie:
    """Structure-preserving build so proofs can be generated."""

    def __init__(self, items):
        self.pairs = sorted(((_nibbles(k), v) for k, v in items), key=lambda p: p[0])
        self.node = _build_keep(self.pairs)

    def root(self):
        return _root_keep(self.node)

    def prove(self, key):
        nib = _nibbles(key)
        node = self.node
        proof = []
        while True:
            enc = rlp(_strip(node))
            if len(enc) >= 32 or not proof:
                proof.append(enc)
            if isinstance(node, dict) and node["type"] == "branch":
                if not nib:
                    return proof
                nxt = node["children"][nib[0]]
                assert nxt is not None
                nib = nib[1:]
                node = nxt
            else:
                plen = len(node["path"])
                assert nib[:plen] == node["path"]
                nib = nib[plen:]
                if node["type"] == "leaf":
                    return proof
                node = node["child"]


def _build_keep(pairs):
    assert pairs
    if len(pairs) == 1:
        nib, val = pairs[0]
        return {"type": "leaf", "path": nib, "value": val}
    first = pairs[0][0]
    lcp = 0
    while all(len(p[0]) > lcp and p[0][lcp] == first[lcp] for p in pairs) and lcp < len(first):
        lcp += 1
    if lcp:
        child = _build_keep([(nib[lcp:], v) for nib, v in pairs])
        return {"type": "ext", "path": first[:lcp], "child": child}
    children = [None] * 16
    value = b""
    for d in range(16):
        sub = [(nib[1:], v) for nib, v in pairs if nib and nib[0] == d]
        if sub:
            children[d] = _build_keep(sub)
    term = [v for nib, v in pairs if not nib]
    if term:
        value = term[0]
    return {"type": "branch", "children": children, "value": value}


def _strip(node):
    if node["type"] == "leaf":
        return [_hp(node["path"], True), node["value"]]
    if node["type"] == "ext":
        return [_hp(node["path"], False), _ref(_strip(node["child"]))]
    out = []
    for c in node["children"]:
        out.append(b"" if c is None else _ref(_strip(c)))
    out.append(node["value"])
    return out


def _root_keep(node):
    return keccak256(rlp(_strip(node)))


# ---------------- receipt encoding --------------------------------------------
def hx(s):
    return bytes.fromhex(s[2:] if s.startswith("0x") else s)


def encode_receipt(r):
    status = int(r["status"], 16)
    cum = int(r["cumulativeGasUsed"], 16)
    bloom = hx(r["logsBloom"])
    logs = [[hx(l["address"]), [hx(t) for t in l["topics"]], hx(l["data"])] for l in r["logs"]]
    body = rlp([status, cum, bloom, logs])
    rtype = int(r.get("type", "0x0"), 16)
    return body if rtype == 0 else bytes([rtype]) + body


# ---------------- main ---------------------------------------------------------
def call(method, params):
    req = urllib.request.Request(RPC, data=json.dumps(
        {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode(),
        headers={"Content-Type": "application/json", "User-Agent": "curl/8.7.1"})
    with urllib.request.urlopen(req, timeout=30) as f:
        resp = json.load(f)
    if "error" in resp:
        raise RuntimeError(resp["error"])
    return resp["result"]


def main():
    blk = call("eth_getBlockByNumber", [TAG, False])
    number = int(blk["number"], 16)
    receipts = call("eth_getBlockReceipts", [blk["number"]])
    assert len(receipts) == len(blk["transactions"])
    items = [(rlp(i), encode_receipt(r)) for i, r in enumerate(receipts)]
    trie = Trie(items)
    root = trie.root()
    want = hx(blk["receiptsRoot"])
    assert root == want, f"receipts root mismatch: {root.hex()} != {want.hex()}"
    sys.stderr.write(f"block {number}: {len(receipts)} receipts, root MATCHES header\n")

    # Pick a receipt with an ERC-20 Transfer log if present, else the last one.
    transfer_topic = "0xddf252ad1be2c89b69c2b068fc378daa952ba7f163c4a11628f55a4df523b3ef"
    idx = len(receipts) - 1
    for i, r in enumerate(receipts):
        if any(l["topics"] and l["topics"][0] == transfer_topic for l in r["logs"]):
            idx = i
            break
    picks = sorted({0, idx, len(receipts) // 2, len(receipts) - 1})

    print("// Generated by contrib/b3bridge/capture_eth_receipts_fixture.py -- DO NOT EDIT.")
    print(f"// Ethereum mainnet block {number} ({blk['hash']}), tag '{TAG}',")
    print(f"// captured from {RPC}. The generator rebuilt the full receipts trie")
    print("// and matched the header receiptsRoot before emitting these proofs.")
    print("#ifndef B3COIN_TEST_DATA_ETH_RECEIPTS_PROOF_FIXTURE_H")
    print("#define B3COIN_TEST_DATA_ETH_RECEIPTS_PROOF_FIXTURE_H")
    print("#include <string>\n#include <vector>")
    print("namespace eth_receipts_fixture {")
    print(f"inline constexpr uint64_t BLOCK_NUMBER{{{number}}};")
    print(f"inline const std::string RECEIPTS_ROOT_HEX{{\"{want.hex()}\"}};")
    print(f"inline constexpr size_t RECEIPT_COUNT{{{len(receipts)}}};")
    print("struct ProofCase { uint64_t index; std::string key_hex; std::string value_hex; std::vector<std::string> proof_hex; };")
    print("inline const std::vector<ProofCase> CASES{")
    for i in picks:
        key = rlp(i)
        proof = trie.prove(key)
        val = items[i][1]
        print("    {%d, \"%s\", \"%s\"," % (i, key.hex(), val.hex()))
        print("     {")
        for node in proof:
            print(f"      \"{node.hex()}\",")
        print("     }},")
    print("};")
    print("} // namespace eth_receipts_fixture")
    print("#endif")


if __name__ == "__main__":
    main()
