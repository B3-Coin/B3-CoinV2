#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Regtest bridge harness helpers (doc/design/b3-bridge-v2-regtest-plan.md §3).

REGTEST ONLY. This module provides:

- the stdlib keccak/RLP/MPT and receipt codecs from contrib/b3bridge, an
  independent RLP decoder and Merkle-Patricia proof verifier, and
  eth_getProof account/storage verification;
- static ABI helpers for calls against a local anvil;
- the B3 RECIPIENT_V1 encoding and the modern regtest chain domain;
- the synthetic beacon (contrib/b3bridge/regtest/mock_beacon.py), the
  -b3bridgepins writer with the plan §3.3 checks, a direct mint-plan
  builder through b3-bridge-ethcheck, deposit discovery and a one-shot run
  of the unchanged relayer as a subprocess (plan §3.3, §3.4);
- bridge_evidence.json (plan §3.7) and helpers that return the refusal a
  check observed, so evidence records observations rather than copies of
  the expected value;
- B3BridgeChainMixin: the four-validator modern regtest fixture with live
  BLS finality (plan §3.6), freeze/resume around wallet checks, and
  cleanup of local helper processes;
- offline unit tests of the harness's regtest-only refusals
  (TestFrameworkB3BridgeGuards, run by feature_framework_unit_tests.py).

The relayer module is imported only as a library; its main() is never
called in-process, only as an unchanged subprocess by run_relayer_once().
Nothing in this module contacts a non-local endpoint.
"""

from collections import namedtuple
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.parse
from decimal import Decimal

from test_framework.address import base58_to_byte
from test_framework.anvil import (
    Anvil,
    AnvilRPCError,
    B3BRIDGE_CONTRIB_DIR,
    artifact,
    assert_local_url,
    find_foundry_tool,
    forge_build,
    forge_test,
    import_b3bridge_contrib,
    local_json_rpc,
    normalize_address,
)
from test_framework.authproxy import JSONRPCException
from test_framework.key import TaggedHash
from test_framework.test_framework import SkipTest
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal, get_auth_cookie

receipts = import_b3bridge_contrib("capture_eth_receipts_fixture")
keccak256 = receipts.keccak256
rlp = receipts.rlp
Trie = receipts.Trie
encode_receipt = receipts.encode_receipt
encode_exec_header = import_b3bridge_contrib("eth_live_test").encode_exec_header
# Library use only: the relayer's main() is never called in-process.
relayer = import_b3bridge_contrib("b3_bridge_relayer")
mock_beacon = import_b3bridge_contrib("mock_beacon")
SyntheticBeacon = mock_beacon.SyntheticBeacon
MockBeaconError = mock_beacon.MockBeaconError
RELAYER_SCRIPT = str(B3BRIDGE_CONTRIB_DIR / "b3_bridge_relayer.py")

# The fixture module derives a default PUBLIC endpoint from sys.argv when it
# is imported. The harness never calls its fetchers; clearing the endpoint
# makes any accidental call fail instead of leaving the machine.
receipts.RPC = None

# -- chain constants (plan §3.6) ---------------------------------------------
REGTEST_ETH_CHAIN_ID = 31337
CORRIDOR_END = 130
MODERN_START = CORRIDOR_END + 1  # M: first modern-PoS block.
A1 = MODERN_START + 1
A2 = MODERN_START + 2
# A3 = FlowMesh activation = bUSD bridge activation (and withdrawal) height.
BRIDGE_ACTIVATION_HEIGHT = A2 + 30
EPOCH_LENGTH = 40
CHECKPOINT_INTERVAL = 5
VALIDATOR_STAKES = (40, 30, 20, 10)
VALIDATOR_COUNT = len(VALIDATOR_STAKES)

B3_BRIDGE_ARGS = [
    "-b3modernregtest",
    "-b3flowmeshtest",
    f"-b3corridorlength={CORRIDOR_END}",
    "-b3corridorreward=2000000000000",  # 2,000 B3 test-only funding blocks.
    "-b3blockinterval=1",
    "-b3roundseconds=1",
    f"-b3epochlength={EPOCH_LENGTH}",
    f"-b3checkpointinterval={CHECKPOINT_INTERVAL}",
    "-b3checkpointdepth=3",
    f"-b3maxepochextension={EPOCH_LENGTH}",
    f"-b3minfinalityset={VALIDATOR_COUNT}",
    "-fallbackfee=0.00001",
    "-addresstype=legacy",
    "-changetype=legacy",
]


def b3_bridge_args(pins_path=None):
    """Node arguments for the bridge fixture, plus the pin file when given."""
    args = list(B3_BRIDGE_ARGS)
    if pins_path is not None:
        assert os.path.isabs(pins_path), "-b3bridgepins needs an absolute path"
        args.append(f"-b3bridgepins={pins_path}")
    return args


# getblockhash 0 of a -b3modernregtest -b3flowmeshtest node (captured
# 2026-10-03 at 44acff3; also contrib/flowmesh-regtest/README.md). It does
# not depend on the corridor or pin knobs. Asserted at runtime.
REGTEST_MODERN_GENESIS_HASH = "10d0f5bb6fde880011fd56ea35dddbf9b32da2a7825f5579ac587a7904d6929a"


def regtest_chain_domain(genesis_hash=REGTEST_MODERN_GENESIS_HASH):
    """modern::ModernChainDomain(genesis, X) with X = genesis on regtest.

    TaggedHash("B3/MODERN/CHAIN", g || g) over internal-order bytes, i.e.
    sha256(tag || tag || g_le || g_le) with tag = sha256("B3/MODERN/CHAIN").
    Returned in Ethereum/wire order, the bytes32 the contracts store.
    """
    genesis_le = bytes.fromhex(genesis_hash)[::-1]
    return TaggedHash("B3/MODERN/CHAIN", genesis_le + genesis_le)


def assert_regtest_genesis(node):
    assert_equal(node.getblockhash(0), REGTEST_MODERN_GENESIS_HASH)


# -- byte helpers -------------------------------------------------------------
def hx(value):
    """bytes from a 0x-prefixed (or bare) hex string; bytes pass through."""
    if isinstance(value, (bytes, bytearray)):
        return bytes(value)
    return bytes.fromhex(value[2:] if value.startswith("0x") else value)


def to_0x(data):
    return "0x" + bytes(data).hex()


# -- RLP decoding (strict, canonical) ----------------------------------------
def _rlp_length(data, pos, size):
    if size == 0 or pos + size > len(data):
        raise ValueError("RLP: truncated length")
    if data[pos] == 0:
        raise ValueError("RLP: length has a leading zero")
    length = int.from_bytes(data[pos:pos + size], "big")
    if length <= 55:
        raise ValueError("RLP: long form used for a short item")
    return length


def _rlp_item(data, pos):
    if pos >= len(data):
        raise ValueError("RLP: truncated item")
    prefix = data[pos]
    if prefix < 0x80:
        return data[pos:pos + 1], pos + 1
    if prefix <= 0xB7:
        start, length = pos + 1, prefix - 0x80
        if length == 1 and start < len(data) and data[start] < 0x80:
            raise ValueError("RLP: single byte below 0x80 must encode itself")
    elif prefix <= 0xBF:
        size = prefix - 0xB7
        start = pos + 1 + size
        length = _rlp_length(data, pos + 1, size)
    elif prefix <= 0xF7:
        start, length = pos + 1, prefix - 0xC0
    else:
        size = prefix - 0xF7
        start = pos + 1 + size
        length = _rlp_length(data, pos + 1, size)
    end = start + length
    if end > len(data):
        raise ValueError("RLP: item exceeds input")
    if prefix <= 0xBF:
        return data[start:end], end
    items, cursor = [], start
    while cursor < end:
        item, cursor = _rlp_item(data, cursor)
        items.append(item)
    if cursor != end:
        raise ValueError("RLP: list payload overruns its length")
    return items, end


def rlp_decode(data):
    """Decode one canonical RLP item into nested lists of bytes."""
    data = bytes(data)
    item, end = _rlp_item(data, 0)
    if end != len(data):
        raise ValueError("RLP: trailing bytes")
    return item


def rlp_uint(value):
    """Canonical RLP integer payload (no leading zero bytes) to int."""
    if not isinstance(value, bytes):
        raise ValueError("RLP: integer is a list")
    if value[:1] == b"\x00":
        raise ValueError("RLP: integer has a leading zero")
    return int.from_bytes(value, "big")


# -- Merkle-Patricia proofs ---------------------------------------------------
EMPTY_TRIE_ROOT = keccak256(rlp(b""))
EMPTY_CODE_HASH = keccak256(b"")


def _nibbles(key):
    out = []
    for byte in key:
        out += [byte >> 4, byte & 0x0F]
    return out


def _hex_prefix_decode(encoded):
    if not isinstance(encoded, bytes) or not encoded:
        raise ValueError("MPT: empty hex-prefix path")
    flag = encoded[0] >> 4
    if flag > 3:
        raise ValueError("MPT: bad hex-prefix flag")
    nibbles = [encoded[0] & 0x0F] if flag & 1 else []
    if not flag & 1 and encoded[0] & 0x0F:
        raise ValueError("MPT: even path with a non-zero pad nibble")
    return nibbles + _nibbles(encoded[1:]), bool(flag & 2)


def verify_mpt_proof(root, key, nodes):
    """Walk an Ethereum MPT proof from root along key.

    Handles branch, extension and leaf nodes, including inline (< 32 byte)
    children embedded in their parent. Returns the stored value, or None
    when the proof shows the key is absent. Raises ValueError when the
    proof does not verify against root (wrong hash, malformed node,
    missing or unused proof node).
    """
    path = _nibbles(bytes(key))
    proof = [hx(node) for node in nodes]
    ref, pos, used = hx(root), 0, 0
    while True:
        if isinstance(ref, list):
            node = ref
        elif ref == b"":
            value = None
            break
        else:
            if len(ref) != 32:
                raise ValueError("MPT: bad child reference")
            if used >= len(proof):
                raise ValueError("MPT: proof is missing a node")
            encoded = proof[used]
            used += 1
            if keccak256(encoded) != ref:
                raise ValueError("MPT: node does not match its reference")
            node = rlp_decode(encoded)
        if isinstance(node, bytes):
            if node:
                raise ValueError("MPT: unexpected scalar node")
            value = None  # The empty trie.
            break
        if len(node) == 17:
            if pos == len(path):
                if not isinstance(node[16], bytes):
                    raise ValueError("MPT: branch value is a list")
                value = node[16] or None
                break
            ref = node[path[pos]]
            pos += 1
            continue
        if len(node) != 2:
            raise ValueError("MPT: node has neither 2 nor 17 items")
        segment, is_leaf = _hex_prefix_decode(node[0])
        if path[pos:pos + len(segment)] != segment:
            value = None  # The key diverges inside this node.
            break
        pos += len(segment)
        if is_leaf:
            if not isinstance(node[1], bytes):
                raise ValueError("MPT: leaf value is a list")
            value = node[1] if pos == len(path) else None
            break
        if pos == len(path):
            raise ValueError("MPT: extension consumes the whole key")
        ref = node[1]
    if used != len(proof):
        raise ValueError("MPT: proof has unused nodes")
    return value


def verify_account_storage_proof(state_root, address, slot, proof):
    """Verify an eth_getProof result for one account and one storage slot.

    Both levels are checked against state_root here; every number the RPC
    reports is cross-checked against the proven values. Returns the proven
    account fields and the slot value; raises ValueError on any mismatch.
    """
    address = normalize_address(address)
    if normalize_address(proof["address"]) != address:
        raise ValueError("eth_getProof answered for another address")
    account_rlp = verify_mpt_proof(state_root, keccak256(hx(address)), proof["accountProof"])
    if account_rlp is None:
        nonce = balance = 0
        storage_root, code_hash = EMPTY_TRIE_ROOT, EMPTY_CODE_HASH
        reported_roots = {(EMPTY_TRIE_ROOT, EMPTY_CODE_HASH), (bytes(32), bytes(32))}
    else:
        fields = rlp_decode(account_rlp)
        if not isinstance(fields, list) or len(fields) != 4:
            raise ValueError("account leaf is not [nonce, balance, storageRoot, codeHash]")
        nonce, balance = rlp_uint(fields[0]), rlp_uint(fields[1])
        storage_root, code_hash = fields[2], fields[3]
        if not all(isinstance(root, bytes) and len(root) == 32
                   for root in (storage_root, code_hash)):
            raise ValueError("account roots are not 32 bytes")
        reported_roots = {(storage_root, code_hash)}
    if (int(proof["nonce"], 16), int(proof["balance"], 16)) != (nonce, balance):
        raise ValueError("eth_getProof nonce/balance differ from the proven account")
    if (hx(proof["storageHash"]), hx(proof["codeHash"])) not in reported_roots:
        raise ValueError("eth_getProof storageHash/codeHash differ from the proven account")
    entries = [entry for entry in proof["storageProof"] if int(entry["key"], 16) == slot]
    if len(entries) != 1:
        raise ValueError(f"eth_getProof has no single entry for slot {slot:#x}")
    raw = verify_mpt_proof(storage_root, keccak256(slot.to_bytes(32, "big")), entries[0]["proof"])
    value = 0 if raw is None else rlp_uint(rlp_decode(raw))
    if int(entries[0]["value"], 16) != value:
        raise ValueError("eth_getProof storage value differs from the proven value")
    return {"nonce": nonce, "balance": balance, "storage_root": storage_root,
            "code_hash": code_hash, "value": value}


def mapping_slot(key_word, slot):
    """Solidity mapping storage slot keccak(pad32(key) || pad32(slot))."""
    return int.from_bytes(keccak256(bytes(key_word) + slot.to_bytes(32, "big")), "big")


# -- static ABI helpers -------------------------------------------------------
def selector(signature):
    return keccak256(signature.encode())[:4]


def enc_uint(value, bits=256):
    assert isinstance(value, int) and 0 <= value < 1 << bits, value
    return value.to_bytes(32, "big")


def enc_address(address):
    return bytes(12) + hx(normalize_address(address))


def enc_bytes32(value):
    data = hx(value)
    assert len(data) == 32, "bytes32 needs exactly 32 bytes"
    return data


def abi_call(signature, *words):
    """Calldata for a function whose arguments are all static words."""
    for word in words:
        assert isinstance(word, bytes) and len(word) == 32, "encode each argument first"
    return selector(signature) + b"".join(words)


def dec_word(data, index=0):
    word = bytes(data[32 * index:32 * index + 32])
    assert len(word) == 32, "ABI return data is too short"
    return word


def dec_uint(data, index=0):
    return int.from_bytes(dec_word(data, index), "big")


def dec_address(data, index=0):
    word = dec_word(data, index)
    assert word[:12] == bytes(12), "ABI address has non-zero padding"
    return "0x" + word[12:].hex()


def call_uint(anvil, to, signature, *words, block="latest"):
    return dec_uint(anvil.call(to, abi_call(signature, *words), block))


def call_bytes32(anvil, to, signature, *words, block="latest"):
    return dec_word(anvil.call(to, abi_call(signature, *words), block))


def call_address(anvil, to, signature, *words, block="latest"):
    return dec_address(anvil.call(to, abi_call(signature, *words), block))


# -- B3 recipient --------------------------------------------------------------
RECIPIENT_V1_P2PKH = 1  # Consensus::BRIDGE_RECIPIENT_VERSION_P2PKH_V1
REGTEST_P2PKH_VERSION = 111


def recipient_v1(address):
    """RECIPIENT_V1 bytes32: 11 zero bytes, 0x01, then the P2PKH key hash."""
    key_hash, version = base58_to_byte(address)
    assert_equal(version, REGTEST_P2PKH_VERSION)
    assert_equal(len(key_hash), 20)
    return bytes(11) + bytes([RECIPIENT_V1_P2PKH]) + key_hash


# -- synthetic beacon domain (plan §3.4) --------------------------------------
# Derived here independently of mock_beacon.py and bridge/regtest_beacon.h;
# all three must agree (asserted below and by the tool's output root).
REGTEST_GVR = hashlib.sha256(b"B3-REGTEST-SYNC-COMMITTEE-V1/genesis_validators_root").digest()
REGTEST_FORK_VERSION = bytes.fromhex("b3000001")
assert REGTEST_GVR == mock_beacon.DEFAULT_GENESIS_VALIDATORS_ROOT
assert REGTEST_FORK_VERSION == mock_beacon.DEFAULT_FORK_VERSION
# Consensus::ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT (src/consensus/bridge_params.h).
ETHEREUM_MAINNET_GVR = bytes.fromhex(
    "4b363db94e286120d76eb905340fdd4e54bfe9f06bf33ff6cf5ad27f511bfe95")
SYNTHETIC_FORK_NAMESPACE = 0xB3
PERIOD_SLOTS = 8192
SLOTS_PER_EPOCH = 32
SYNC_COMMITTEE_SUPERMAJORITY = 342
MAX_SYNC_LAG_SLOTS = 8192

# -- -b3bridgepins file (plan §3.3) -------------------------------------------
PINS_FORMAT = "b3-regtest-bridge-pins/1"
# bridge::MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS (src/bridge/proof.h:43).
MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS = 20_000
# CChainParams::B3RegTestBridgePins defaults (raw bUSD units).
DEFAULT_MAX_PER_BLOCK = 1_000_000_000
DEFAULT_MAX_PER_EPOCH = 10_000_000_000
MAX_MONEY = 662_200_000_000 * 1_000_000  # src/consensus/amount.h
# The V1 withdrawal leg is not exercised against the mock verifier; this
# deterministic, non-zero placeholder only satisfies the node's Valid().
REGTEST_BOOTSTRAP_SET_HASH = hashlib.sha256(
    b"B3-REGTEST-BRIDGE-BOOTSTRAP-VALIDATOR-SET-V1").digest()


class BridgePinError(ValueError):
    """A pin file the harness refuses to write (plan §3.3 pin-writer checks)."""


def _anvil_url(anvil):
    return assert_local_url(anvil if isinstance(anvil, str) else anvil.url)


def write_regtest_bridge_pins(path, anvil, vault, token, verifier, deployment_block,
                              bootstrap_output, horizon_epochs=1024, caps=None, *,
                              gvr=REGTEST_GVR, fork_version=REGTEST_FORK_VERSION,
                              min_bridge_validators=4, max_bridge_validators=64,
                              min_bridge_total_weight=4, max_epoch_lag=86_400,
                              bootstrap_validator_set_hash=REGTEST_BOOTSTRAP_SET_HASH,
                              check=True):
    """Write a -b3bridgepins file for a local V1 deployment; return its JSON.

    Code hashes, MAX_DEPOSIT_RAW and the vault's immutables are read from
    anvil. With check (the default) the writer refuses, before writing:
    a chain id other than 31337; the mainnet genesis validators root or a
    fork version outside the 0xb3 namespace; a checkpoint outside
    deployment..deployment+20,000 (check 4); MAX_DEPOSIT_RAW above the
    per-block cap (check 1); a vault BRIDGE_ACTIVATION_HEIGHT other than
    A3 = 163 (check 3); a vault that names another token, verifier, chain
    or verifier code hash; a missing runtime; and a checkpoint that is not
    on this anvil. Code is read at latest only: anvil keeps just its newest
    500 states, so the deployment block may be out of its history.
    check=False writes the file anyway, for node-refusal tests only.
    caps may set max_per_block and/or max_per_epoch; absent caps use the
    node defaults (1e9 / 1e10).
    """
    assert os.path.isabs(path), "-b3bridgepins needs an absolute path"
    caps = dict(caps or {})
    assert set(caps) <= {"max_per_block", "max_per_epoch"}, caps
    vault, token, verifier = (normalize_address(a) for a in (vault, token, verifier))
    gvr = hx(gvr)
    fork_version = hx(fork_version)
    checkpoint_block = bootstrap_output["checkpoint_block"]
    checkpoint_slot = bootstrap_output["checkpoint_slot"]

    def refuse(reason):
        if check:
            raise BridgePinError(f"refusing to write {path}: {reason}")

    if isinstance(anvil, str):
        chain_id = int(local_json_rpc(_anvil_url(anvil), "eth_chainId"), 16)
    else:
        chain_id = anvil.chain_id_rpc()
    if chain_id != REGTEST_ETH_CHAIN_ID:
        refuse(f"anvil chain id {chain_id} is not the regtest origin chain {REGTEST_ETH_CHAIN_ID}")
    if gvr in (bytes(32), ETHEREUM_MAINNET_GVR):
        refuse("the genesis validators root is zero or the Ethereum mainnet root")
    if len(fork_version) != 4 or fork_version[0] != SYNTHETIC_FORK_NAMESPACE:
        refuse(f"fork version 0x{fork_version.hex()} is outside the synthetic 0xb3 namespace")
    if len({vault, token, verifier}) != 3:
        refuse("vault, token and verifier addresses must be pairwise distinct")
    if not (1 <= deployment_block <= checkpoint_block <=
            deployment_block + MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS):
        refuse(f"checkpoint execution block {checkpoint_block} must lie within "
               f"{MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS} blocks at or after the "
               f"deployment block {deployment_block} (check 4)")

    def rpc(method, *params):
        return local_json_rpc(_anvil_url(anvil), method, list(params))

    def code_hash(address):
        code = hx(rpc("eth_getCode", address, "latest"))
        if not code:
            refuse(f"no runtime code at {address}")
        return keccak256(code)

    def vault_word(signature):
        return dec_word(hx(rpc("eth_call", {"to": vault, "data": to_0x(selector(signature))},
                               "latest")))

    if check:
        anchor = rpc("eth_getBlockByNumber", hex(checkpoint_block), False)
        if (anchor is None or relayer.norm_hex(anchor["hash"], 32) !=
                relayer.norm_hex(bootstrap_output["checkpoint_block_hash"], 32)):
            refuse(f"checkpoint block {checkpoint_block} is not on this anvil chain")
    max_deposit_raw = int.from_bytes(vault_word("MAX_DEPOSIT_RAW()"), "big")
    activation_height = int.from_bytes(vault_word("BRIDGE_ACTIVATION_HEIGHT()"), "big")
    if activation_height != BRIDGE_ACTIVATION_HEIGHT:
        refuse(f"vault BRIDGE_ACTIVATION_HEIGHT {activation_height} is not "
               f"A3 = {BRIDGE_ACTIVATION_HEIGHT} (check 3)")
    hashes = {name: code_hash(address) for name, address in
              (("vault", vault), ("token", token), ("verifier", verifier))}
    if check:
        immutables = {
            "ORIGIN_TOKEN()": enc_address(token),
            "verifier()": enc_address(verifier),
            "ORIGIN_CHAIN_ID()": enc_uint(REGTEST_ETH_CHAIN_ID),
            "VERIFIER_CODE_HASH()": hashes["verifier"],
            "B3_CHAIN_DOMAIN()": regtest_chain_domain(),
        }
        for signature, expected in immutables.items():
            if vault_word(signature) != expected:
                refuse(f"vault {signature} does not match the pinned deployment")
    block_cap = caps.get("max_per_block", DEFAULT_MAX_PER_BLOCK)
    epoch_cap = caps.get("max_per_epoch", DEFAULT_MAX_PER_EPOCH)
    if not 0 < max_deposit_raw <= block_cap <= epoch_cap <= MAX_MONEY:
        refuse(f"caps must satisfy 0 < MAX_DEPOSIT_RAW ({max_deposit_raw}) <= max_per_block "
               f"({block_cap}) <= max_per_epoch ({epoch_cap}) <= MAX_MONEY (check 1)")

    v1 = {
        "origin_chain_id": REGTEST_ETH_CHAIN_ID,
        "vault": vault,
        "token": token,
        "origin_deployment_block": deployment_block,
        "vault_runtime_code_hash": to_0x(hashes["vault"]),
        "token_runtime_code_hash": to_0x(hashes["token"]),
        "max_deposit_raw": max_deposit_raw,
        "light_client": {
            "trusted_checkpoint_root": bootstrap_output["checkpoint_root"],
            "trusted_checkpoint_slot": checkpoint_slot,
            "checkpoint_execution_block": checkpoint_block,
            "genesis_validators_root": to_0x(gvr),
            "fork_schedule": [{"activation_epoch": 0, "fork_version": to_0x(fork_version)}],
            "fork_schedule_valid_through_epoch": checkpoint_slot // SLOTS_PER_EPOCH + horizon_epochs,
            "electra_epoch": 0,
            "min_sync_committee_participants": SYNC_COMMITTEE_SUPERMAJORITY,
            "max_sync_lag_slots": MAX_SYNC_LAG_SLOTS,
        },
        "verifier": verifier,
        "verifier_runtime_code_hash": to_0x(hashes["verifier"]),
        "bootstrap_validator_set_hash": to_0x(bootstrap_validator_set_hash),
        "min_bridge_validators": min_bridge_validators,
        "max_bridge_validators": max_bridge_validators,
        "min_bridge_total_weight": min_bridge_total_weight,
        "max_epoch_lag": max_epoch_lag,
    }
    v1.update(caps)
    document = {"format": PINS_FORMAT, "v1_decentralized": v1}
    with open(path, "w", encoding="utf8") as f:
        json.dump(document, f, indent=2, sort_keys=True)
        f.write("\n")
    return document


def file_sha256(path):
    """SHA256 of a file's raw bytes, as the node records a pin file."""
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


# -- deposits and the direct mint plan (relayer library, plan §3.4) -----------
class LocalEthRpc:
    """The relayer JsonRpc.call() interface over a local anvil, never proxied."""

    def __init__(self, url):
        self.url = assert_local_url(url)

    def call(self, method, params=()):
        try:
            return local_json_rpc(self.url, method, list(params))
        except AnvilRPCError as e:
            raise relayer.RpcError({"code": e.code, "message": e.message}) from None


def find_deposits(anvil, vault, token, first, last=None):
    """Every vault Deposit of `token` in blocks first..last (default: head).

    Each header is checked against its hash, then
    relayer.authenticated_block_deposits() authenticates the complete
    receipt array against the receipts root before reading any log. Each
    result carries the relayer's deposit fields plus the vault.
    """
    eth = LocalEthRpc(_anvil_url(anvil))
    identity = {"vault": relayer.norm_hex(normalize_address(vault), 20),
                "token": relayer.norm_hex(normalize_address(token), 20)}
    if last is None:
        last = int(eth.call("eth_blockNumber"), 16)
    found = []
    for number in range(first, last + 1):
        header = eth.call("eth_getBlockByNumber", [hex(number), False])
        if keccak256(encode_exec_header(header)) != hx(header["hash"]):
            raise AssertionError(f"anvil header {number} does not hash to its hash")
        for deposit in relayer.authenticated_block_deposits(eth, header, identity):
            deposit["vault"] = identity["vault"]
            found.append(deposit)
    return found


def build_direct_mint_plan(workdir, beacon, anvil_url, deposit, registry_id, updates,
                           *, payload_tool=None, vault=None, token=None,
                           max_ancestry=relayer.MAX_ANCESTRY_DISTANCE):
    """Bootstrap, updates and one mint for `deposit`, verified by ethcheck.

    updates lists finalized execution blocks (ints), tool update specs
    (dicts) or ready tool outputs; the last one's finalized block is the
    receipt proof's source. The workdir gets the relayer's own config.json
    (from the beacon domain: electra_epoch 0, min_participants 342),
    bootstrap.json and updates.json, then relayer.capture_receipt() (no
    retained B3 anchor) and relayer.emit_plan() run b3-bridge-ethcheck
    --emit-payloads (default: next to the beacon tool). Returns the plan:
    bootstrap, updates, backfills and mint records with payload_hex.
    """
    workdir = Path(workdir)
    workdir.mkdir(parents=True, exist_ok=True)
    payload_tool = payload_tool or os.path.join(os.path.dirname(beacon.tool), "b3-bridge-ethcheck")
    bootstrap = beacon.bootstrap()
    specs = [u if isinstance(u, dict) else {"finalized": u}
             for u in updates if not (isinstance(u, dict) and "data" in u)]
    produced = iter(beacon.updates(specs) if specs else [])
    outputs = [u if isinstance(u, dict) and "data" in u else next(produced) for u in updates]
    final = outputs[-1] if outputs else None
    config = {
        "genesis_validators_root": to_0x(beacon.gvr),
        "forks": [{"epoch": 0, "version": to_0x(beacon.fork_version)}],
        "electra_epoch": beacon.electra_epoch,
        "min_participants": SYNC_COMMITTEE_SUPERMAJORITY,
        "trusted_root": bootstrap["checkpoint_root"],
    }
    common = (config, beacon.api_object(bootstrap), [beacon.api_object(u) for u in outputs], None,
              final["finalized_block"] if final else bootstrap["checkpoint_block"],
              relayer.norm_hex(final["finalized_block_hash"] if final
                               else bootstrap["checkpoint_block_hash"], 32))
    relayer.write_common(workdir, common)
    relayer.capture_receipt(workdir, deposit, common, LocalEthRpc(assert_local_url(anvil_url)),
                            relayer.norm_hex(vault or deposit["vault"], 20),
                            relayer.norm_hex(token or deposit["token"], 20),
                            registry_id, max_ancestry, source_anchor=None)
    plan = relayer.emit_plan(payload_tool, workdir)
    assert plan.get("bootstrap") is not None, "ethcheck emitted no bootstrap"
    assert plan.get("mint") is not None, "ethcheck emitted no mint"
    return plan


# -- the unchanged relayer as a subprocess ------------------------------------
RelayerRun = namedtuple("RelayerRun", ["returncode", "events", "stderr"])


def _local_process_env():
    """Environment for a local helper: no HTTP proxy, no bytecode writes."""
    env = {key: value for key, value in os.environ.items()
           if not key.lower().endswith("_proxy")}
    # A non-empty *_proxy environment makes urllib ignore system proxy
    # settings; no_proxy then sends both local hosts direct.
    env["no_proxy"] = env["NO_PROXY"] = "127.0.0.1,localhost"
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    return env


def b3_rpc_endpoint(node):
    """(plain URL, auth arguments) for a running node's RPC interface."""
    parts = urllib.parse.urlsplit(node.url)
    url = assert_local_url(f"http://{parts.hostname}:{parts.port}")
    cookie = Path(node.chain_path) / ".cookie"
    if cookie.is_file():
        return url, ["--b3-cookie", str(cookie)]
    user, password = get_auth_cookie(node.datadir_path, node.chain)
    return url, ["--b3-rpc-user", user, "--b3-rpc-password", password]


def run_relayer_once(node, wallet, state, beacon_url, *, ethereum_url, payload_tool,
                     trusted_root, work_root=None, max_fee_b3="0.1",
                     daily_fee_budget_b3="5", extra_args=(), timeout=900):
    """Run contrib/b3bridge/b3_bridge_relayer.py once (--one-shot), unchanged.

    The primary Ethereum provider is ethereum_url on 127.0.0.1 and the
    witness is the same anvil as http://localhost (a distinct origin).
    Returns RelayerRun(returncode, events, stderr): events are the relayer's
    JSON log lines in order; a failed cycle has returncode 1 and a
    "cycle_failed" event.
    """
    eth = urllib.parse.urlsplit(assert_local_url(ethereum_url))
    b3_url, auth = b3_rpc_endpoint(node)
    state = os.path.abspath(state)
    os.makedirs(os.path.dirname(state), exist_ok=True)
    command = [
        sys.executable, "-B", RELAYER_SCRIPT, "--one-shot",
        "--ethereum-rpc", f"http://127.0.0.1:{eth.port}",
        "--ethereum-rpc-secondary", f"http://localhost:{eth.port}",
        "--beacon-url", assert_local_url(beacon_url),
        "--b3-rpc-url", b3_url, *auth,
        "--wallet", wallet,
        "--payload-tool", os.path.abspath(payload_tool),
        "--trusted-root", relayer.norm_hex(trusted_root, 32),
        "--ethereum-chain-id", str(REGTEST_ETH_CHAIN_ID),
        "--max-fee-b3", str(max_fee_b3),
        "--daily-fee-budget-b3", str(daily_fee_budget_b3),
        "--state", state,
        "--work-root", os.path.abspath(work_root or f"{state}.work"),
        *extra_args,
    ]
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                            stdin=subprocess.DEVNULL, env=_local_process_env(),
                            cwd=os.path.dirname(state))
    events = []
    for line in result.stderr.splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if isinstance(event, dict) and "event" in event:
            events.append(event)
    return RelayerRun(result.returncode, events, result.stderr)


# -- observed refusals ----------------------------------------------------------
def rpc_refusal(code, message, fun, *args, **kwds):
    """assert_raises_rpc_error() that returns the error message it observed."""
    try:
        fun(*args, **kwds)
    except JSONRPCException as e:
        observed = e.error["message"]
        assert_equal(e.error["code"], code)
        assert message in observed, f"expected {message!r} in {observed!r}"
        return observed
    raise AssertionError(f"no RPC error; expected {code}: {message}")


def start_refused(node, extra_args, expected_msg):
    """node.assert_start_raises_init_error() for a partial message.

    Returns the stderr text the node printed, read from the file the test
    framework kept for that start.
    """
    before = set(os.listdir(node.stderr_dir))
    node.assert_start_raises_init_error(extra_args=extra_args,
                                        expected_msg=re.escape(expected_msg),
                                        match=ErrorMatch.PARTIAL_REGEX)
    new = [name for name in os.listdir(node.stderr_dir) if name not in before]
    assert_equal(len(new), 1)
    with open(os.path.join(node.stderr_dir, new[0]), encoding="utf8") as f:
        return f.read().strip()


# -- evidence (plan §3.7) -----------------------------------------------------
class BridgeEvidence:
    """bridge_evidence.json in the test's tmpdir, rewritten after each record.

    Steps carry the B3 height and Ethereum block at that point plus the
    blocks elapsed since the previous step on each chain.
    """

    FORMAT = "b3-bridge-evidence/1"

    def __init__(self, tmpdir, test_name):
        self.path = os.path.join(tmpdir, "bridge_evidence.json")
        self.data = {
            "format": self.FORMAT,
            "test": test_name,
            "started": int(time.time()),
            "steps": [],
            "jumps": [],
            "assertions": [],
            "gas": [],
            "stranded": [],
            "notes": [],
        }
        self._last_heights = {}
        self.write()

    def step(self, name, b3_height=None, eth_block=None, **extra):
        entry = {"name": name, "time": int(time.time())}
        for chain, key, height in (("b3", "b3_height", b3_height),
                                   ("eth", "eth_block", eth_block)):
            if height is None:
                continue
            entry[key] = height
            previous = self._last_heights.get(chain)
            entry[f"{chain}_blocks"] = None if previous is None else height - previous
            self._last_heights[chain] = height
        entry.update(extra)
        self.data["steps"].append(entry)
        self.write()
        return entry

    def jump(self, clock, seconds, before=None, after=None, **extra):
        self.data["jumps"].append({"clock": clock, "seconds": seconds,
                                   "before": before, "after": after, **extra})
        self.write()

    def assertion(self, name, expected=None, actual=None, **extra):
        """Record an assertion that the caller has already made.

        actual is what the check observed (a returned value, a refusal
        message, a file read back); leave it None when nothing was measured.
        """
        self.data["assertions"].append({"name": name, "expected": expected,
                                        "actual": actual, **extra})
        self.write()

    def gas(self, label, gas_used, **extra):
        self.data["gas"].append({"label": label, "gas_used": gas_used, **extra})
        self.write()

    def stranded(self, label, amount, **extra):
        self.data["stranded"].append({"label": label, "amount": amount, **extra})
        self.write()

    def note(self, text, **extra):
        self.data["notes"].append({"text": text, **extra})
        self.write()

    def write(self):
        temporary = self.path + ".tmp"
        with open(temporary, "w", encoding="utf8") as f:
            json.dump(self.data, f, indent=2, sort_keys=True, default=_json_default)
            f.write("\n")
        os.replace(temporary, self.path)


def _json_default(value):
    if isinstance(value, (bytes, bytearray)):
        return to_0x(value)
    if isinstance(value, Decimal):
        return str(value)
    raise TypeError(f"{type(value).__name__} is not JSON serialisable")


# -- B3 four-validator fixture ------------------------------------------------
class B3BridgeChainMixin:
    """Bridge fixture helpers for a BitcoinTestFramework subclass.

    Use as `class Test(B3BridgeChainMixin, BitcoinTestFramework)`. Nodes 0-3
    are the validators. Any further node (e.g. a sync checker that is not
    started) is ignored by the helpers below, which act only on running
    nodes. The corridor/stake helpers are copied from
    feature_flowmesh_release.py:79-199, which is left unchanged.
    """

    # -- configuration, tools and local processes --------------------------
    @property
    def bridge_srcdir(self):
        return self.config["environment"]["SRCDIR"]

    @property
    def bridge_builddir(self):
        return self.config["environment"]["BUILDDIR"]

    def bridge_tool(self, name):
        return os.path.join(self.bridge_builddir, "bin",
                            name + self.config["environment"].get("EXEEXT", ""))

    def skip_if_no_bridge_tools(self, tools=("b3-bridge-ethcheck", "b3-bridge-regtest-beacon")):
        """Skip unless the B3_BRIDGE_TOOLS executables were built."""
        found = {}
        for name in tools:
            path = self.bridge_tool(name)
            if not (os.path.isfile(path) and os.access(path, os.X_OK)):
                raise SkipTest(f"{name} not built (configure with -DB3_BRIDGE_TOOLS=ON)")
            found[name] = path
        self.bridge_tools = found
        return found

    def skip_if_no_foundry(self, tools=("anvil", "forge")):
        """Skip unless the local foundry binaries exist (never installed here)."""
        found = {}
        for name in tools:
            path = find_foundry_tool(name)
            if path is None:
                raise SkipTest(f"foundry tool {name} not found")
            found[name] = path
        self.foundry = found
        return found

    def register_bridge_process(self, process):
        """Have shutdown() call process.stop(), e.g. anvil or a mock beacon."""
        if not hasattr(self, "_bridge_processes"):
            self._bridge_processes = []
        self._bridge_processes.append(process)
        return process

    def start_anvil(self, name="anvil", chain_id=REGTEST_ETH_CHAIN_ID):
        """Start a local anvil that shutdown() stops even on failure."""
        anvil = Anvil(self.foundry["anvil"], os.path.join(self.options.tmpdir, name),
                      chain_id=chain_id)
        self.register_bridge_process(anvil)
        return anvil.start()

    def shutdown(self):
        for process in reversed(getattr(self, "_bridge_processes", [])):
            try:
                process.stop()
            except Exception as e:
                self.log.warning(f"Stopping bridge helper {process!r} failed: {e}")
        self._bridge_processes = []
        return super().shutdown()

    def bridge_forge_build(self, *args, timeout=600):
        return forge_build(self.foundry["forge"], self.bridge_srcdir, self.bridge_builddir,
                           *args, timeout=timeout)

    def bridge_forge_test(self, *args, timeout=1800):
        return forge_test(self.foundry["forge"], self.bridge_srcdir, self.bridge_builddir,
                          *args, timeout=timeout)

    def bridge_artifact(self, file, contract):
        return artifact(self.bridge_builddir, file, contract)

    def start_synthetic_beacon(self, anvil, slot_offset, base_block, name="beacon",
                               gvr=REGTEST_GVR, fork_version=REGTEST_FORK_VERSION):
        """A SyntheticBeacon over anvil whose server shutdown() stops.

        It is not serving yet; call serve() for the Beacon-API endpoints.
        """
        beacon = SyntheticBeacon(_anvil_url(anvil), self.bridge_tool("b3-bridge-regtest-beacon"),
                                 os.path.join(self.options.tmpdir, name), gvr, fork_version,
                                 slot_offset, base_block)
        return self.register_bridge_process(beacon)

    def run_bridge_relayer_once(self, node, beacon_url, anvil, trusted_root, *,
                                wallet=None, state=None, **kwargs):
        """run_relayer_once() with this build's ethcheck and a tmpdir state."""
        state = state or os.path.join(self.options.tmpdir, "relayer", "relayer.sqlite3")
        return run_relayer_once(
            node, wallet or self.default_wallet_name, state, beacon_url,
            ethereum_url=_anvil_url(anvil), payload_tool=self.bridge_tool("b3-bridge-ethcheck"),
            trusted_root=trusted_root, **kwargs)

    @property
    def evidence(self):
        if getattr(self, "_bridge_evidence", None) is None:
            self._bridge_evidence = BridgeEvidence(self.options.tmpdir, type(self).__name__)
        return self._bridge_evidence

    def evidence_step(self, name, anvil=None, **extra):
        """Record a step with n0's height and, if given, anvil's head."""
        return self.evidence.step(
            name, b3_height=self.nodes[0].getblockcount(),
            eth_block=anvil.block_number() if anvil is not None else None, **extra)

    # -- node set ----------------------------------------------------------
    def bridge_nodes(self):
        """Running nodes; helpers never touch a node that is not started."""
        return [node for node in self.nodes if node.running]

    def bridge_validators(self):
        return self.nodes[:VALIDATOR_COUNT]

    def start_bridge_validators(self, pins_path=None, extra_args=()):
        """Add every node with the bridge arguments; start and link n0-n3.

        The validators link only to each other, in the framework's default
        chain topology. Nodes past n3 are added but not started.
        """
        args = b3_bridge_args(pins_path) + list(extra_args)
        if not self.nodes:
            self.add_nodes(self.num_nodes, [list(args) for _ in range(self.num_nodes)])
        for i in range(VALIDATOR_COUNT):
            self.start_node(i, extra_args=args)
            if self.uses_wallet:
                self.init_wallet(node=i)
        for i in range(VALIDATOR_COUNT - 1):
            self.connect_nodes(i + 1, i)

    def sync_bridge_blocks(self, timeout=120):
        self.sync_blocks(self.bridge_nodes(), timeout=timeout)

    # -- copied from feature_flowmesh_release.py:79-199 ---------------------
    def set_chain_time(self, timestamp):
        self.mock_time = timestamp
        for node in self.bridge_nodes():
            node.setmocktime(timestamp)

    def mine_corridor(self, count):
        """Mine transition blocks without crossing the 120-second future bound."""
        address = self.nodes[0].get_deterministic_priv_key().address
        while count:
            chunk = min(count, 80)
            self.generatetoaddress(self.nodes[0], chunk, address,
                                   sync_fun=lambda: self.sync_bridge_blocks())
            count -= chunk
            if count:
                self.set_chain_time(self.mock_time + chunk)

    def synchronize_mempools(self):
        """Load wallet-created transactions into every running node.

        Mock time does not advance between transaction creation and block
        production, so ordinary wallet INV trickle timers do not fire. With
        live staking a transaction may be mined (or replaced) between the
        reads below; one that has left its source mempool is skipped
        instead of failing. Frozen, this is exactly the release helper.
        """
        nodes = self.bridge_nodes()
        pools = [set(node.getrawmempool()) for node in nodes]
        all_txids = set().union(*pools)
        transactions = {}
        dependencies = {}
        sources = {}
        for txid in all_txids:
            source = next(node for node, pool in zip(nodes, pools) if txid in pool)
            try:
                transactions[txid] = source.getrawtransaction(txid)
                dependencies[txid] = set(source.getmempoolentry(txid)["depends"])
            except JSONRPCException:
                if txid in source.getrawmempool():
                    raise
                continue
            sources[txid] = source

        ordered = []
        pending = set(transactions)
        while pending:
            ready = sorted(txid for txid in pending
                           if not (dependencies[txid] & pending))
            if not ready:
                raise AssertionError("Mempool dependency graph contains a cycle")
            ordered.extend(ready)
            pending.difference_update(ready)

        for txid in ordered:
            for node, pool in zip(nodes, pools):
                if txid in pool:
                    continue
                try:
                    assert_equal(node.sendrawtransaction(transactions[txid]), txid)
                except JSONRPCException:
                    if txid in sources[txid].getrawmempool():
                        raise
                    continue
                pool.add(txid)
        for node in nodes:
            node.syncwithvalidationinterfacequeue()
        if not getattr(self, "pos_running", False):
            assert all(pool == pools[0] for pool in pools)

    def start_pos(self):
        if getattr(self, "pos_running", False):
            return
        for node in self.bridge_validators():
            if node.getstakinginfo()["staking"]["running"]:
                continue
            result = node.startstaking()
            assert_equal(result["running"], True)
            assert_equal(result["finality_signing"], True)
        self.pos_running = True

    def stop_pos(self):
        for node in self.bridge_validators():
            if node.running:
                assert_equal(node.stopstaking()["running"], False)
        self.pos_running = False

    def mine_pos_blocks_mocktime(self, count, allow_overshoot=False):
        """Advance modern PoS under mock time with four live signers.

        The release pattern: advance the chain clock only to the earliest
        advertised slot, then freeze production before synchronising so a
        caller can target an exact height. Needs set_chain_time() first.
        """
        assert getattr(self, "mock_time", None), "set_chain_time() before mocktime production"
        target_height = self.nodes[0].getblockcount() + count
        while self.nodes[0].getblockcount() < target_height:
            self.start_pos()
            old_height = self.nodes[0].getblockcount()
            deadline = time.time() + 60
            while self.nodes[0].getblockcount() == old_height:
                if time.time() >= deadline:
                    raise AssertionError("Modern PoS did not produce a block")
                schedules = []
                for node in self.bridge_validators():
                    staking = node.getstakinginfo()["staking"]
                    if staking["running"] and "next_block_time" in staking:
                        schedules.append(staking["next_block_time"])
                future = [slot for slot in schedules if slot > self.mock_time]
                self.set_chain_time(min(future) if future else self.mock_time + 1)
                time.sleep(0.5)
            self.stop_pos()
            self.sync_bridge_blocks()
            new_height = self.nodes[0].getblockcount()
            if (new_height <= old_height or
                    (not allow_overshoot and new_height > target_height)):
                raise AssertionError(
                    f"Modern PoS crossed target height {target_height}: "
                    f"{old_height} -> {new_height}"
                )

    @staticmethod
    def transaction_height(node, txid, first_height):
        """Locate a just-mined transaction without relying on wallet metadata."""
        for height in range(first_height, node.getblockcount() + 1):
            block = node.getblock(node.getblockhash(height))
            if txid in block["tx"]:
                return height
        raise AssertionError(f"Transaction {txid} was not mined")

    @staticmethod
    def wallet_asset(wallet, asset_id):
        assets = wallet.getwalletassets(asset_id)["assets"]
        if not assets:
            return {"confirmed": 0, "unconfirmed": 0, "spendable": 0,
                    "utxos": []}
        assert_equal(len(assets), 1)
        return assets[0]

    # -- live-finality fixture (plan §3.6) ---------------------------------
    @staticmethod
    def finalized_height(node):
        status = node.getfinalitystatus()
        if not status.get("bootstrapped"):
            return -1
        return status.get("finalized", {}).get("height", -1)

    def wait_for_finalized(self, height, timeout=300):
        self.wait_until(lambda: all(self.finalized_height(node) >= height
                                    for node in self.bridge_validators()),
                        timeout=timeout)

    def bring_up_validators(self, target_height, stakes=VALIDATOR_STAKES, timeout=600):
        """Corridor, four stakes with finality keys, then live real-time PoS.

        The corridor runs under mock time from now-300 so its 1 s blocks stay
        behind the wall clock. From M-1 the nodes return to the real clock
        and stake continuously (feature_b3_finality_soak.py:120-152), so BLS
        finality is live. Returns once n0 is at or above target_height and
        every validator has finalized at least M+5.
        """
        validators = self.bridge_validators()
        assert_equal(len(validators), VALIDATOR_COUNT)
        assert all(node.running for node in validators)
        assert target_height >= MODERN_START
        n0 = validators[0]
        for node in validators:
            assert_regtest_genesis(node)
        self.pos_running = False
        self.set_chain_time(int(time.time()) - 300)

        self.log.info("Corridor funding and four independent finality stakes")
        self.mine_corridor(101)
        for node in validators[1:]:
            n0.sendtoaddress(node.get_deterministic_priv_key().address, 120)
        self.synchronize_mempools()
        self.mine_corridor(1)
        for node, amount in zip(validators, stakes):
            node.createstake(amount)
            node.bindfinalitykey()
        self.synchronize_mempools()
        self.mine_corridor(1)
        self.mine_corridor(CORRIDOR_END - n0.getblockcount())
        assert_equal(n0.getblockcount(), CORRIDOR_END)
        for node in validators:
            self.wait_until(lambda n=node: n.getstakinginfo()["active"] > Decimal("0"),
                            timeout=60)
            preview = node.getfinalitystatus()["set0_preview"]
            assert_equal(preview["ready"], True)
            assert_equal(preview["size"], VALIDATOR_COUNT)
        # The Python domain derivation must equal the node's own.
        assert_equal(n0.getfinalityset()["chain_domain"], regtest_chain_domain().hex())

        self.log.info("Real-time staking with live BLS finality")
        for node in self.bridge_nodes():
            node.setmocktime(0)
        self.mock_time = 0
        self.start_pos()
        self.wait_until(lambda: n0.getblockcount() >= target_height, timeout=timeout)
        self.wait_for_finalized(MODERN_START + 5, timeout=timeout)
        for node in validators:
            assert_equal(node.getfinalitystatus()["lineage_broken"], False)

    def freeze(self):
        """Stop every staker and agree on one tip.

        Wallet prevalidation and every negative check run frozen, so the tip
        cannot change underneath them (no TIP_CHANGED).
        """
        self.stop_pos()
        self.sync_bridge_blocks()
        for node in self.bridge_nodes():
            node.syncwithvalidationinterfacequeue()

    def resume(self):
        """Restart real-time staking on every validator."""
        self.pos_running = False
        self.start_pos()

    def wait_mined(self, txid, first_height=None, timeout=180):
        """Wait until txid is in a block that every running node agrees on.

        Needs a producer (resume() or mocktime production). Blocks are scanned
        from first_height (default: 20 below the tip at call time). Returns
        the containing height.
        """
        n0 = self.nodes[0]
        if first_height is None:
            first_height = max(0, n0.getblockcount() - 20)
        state = {"next": first_height, "hit": None}

        def mined():
            try:
                if state["hit"] is None:
                    tip = n0.getblockcount()
                    while state["next"] <= tip and state["hit"] is None:
                        block_hash = n0.getblockhash(state["next"])
                        if txid in n0.getblock(block_hash)["tx"]:
                            state["hit"] = (state["next"], block_hash)
                        state["next"] += 1
                    if state["hit"] is None:
                        return False
                height, block_hash = state["hit"]
                if n0.getblockhash(height) != block_hash:
                    # Reorganised away on n0: scan the window again.
                    state["next"], state["hit"] = first_height, None
                    return False
                return all(node.getblockhash(height) == block_hash
                           for node in self.bridge_nodes())
            except JSONRPCException:
                # A node is still below the height, or n0 reorganised mid-scan.
                return False

        self.wait_until(mined, timeout=timeout, check_interval=0.25)
        return state["hit"][0]

    def submit_and_mine(self, label, create):
        """Create a bridge transaction frozen, share it, then stake until mined.

        create() returns an RPC result with a txid. Returns (txid, containing
        height) and leaves the validators frozen on one tip.
        """
        self.freeze()
        first_height = self.nodes[0].getblockcount() + 1
        txid = create()["txid"]
        self.synchronize_mempools()
        self.resume()
        # Every running node has the containing block once this returns.
        height = self.wait_mined(txid, first_height=first_height)
        self.freeze()
        self.evidence_step(label, getattr(self, "anvil", None), txid=txid, mined_height=height)
        return txid, height


class TestFrameworkB3BridgeGuards(unittest.TestCase):
    """The harness refuses non-local endpoints and real Ethereum domains.

    Offline: every case is refused before any request is sent.
    """

    ADDRESSES = ("0x9fe46736679d2d9a65f0992f2272de9f3c7fa6e0",  # vault
                 "0x5fbdb2315678afecb367f032d93f642f64180aa3",  # token
                 "0xe7f1725e7734ce288f8367e1bb143e90bb3f0512")  # verifier
    DEPLOYMENT_BLOCK = 3
    BOOTSTRAP = {"checkpoint_block": 3, "checkpoint_slot": 20_480,
                 "checkpoint_root": "0x" + "c1" * 32,
                 "checkpoint_block_hash": "0x" + "d1" * 32}

    class FakeAnvil:
        """Answers only eth_chainId; any other request would fail."""
        url = "http://127.0.0.1:9"

        def __init__(self, chain_id):
            self.chain_id = chain_id

        def chain_id_rpc(self):
            return self.chain_id

    def assert_pins_refused(self, anvil, message, bootstrap=None, **kwargs):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "pins.json")
            with self.assertRaisesRegex(BridgePinError, message):
                write_regtest_bridge_pins(path, anvil, *self.ADDRESSES, self.DEPLOYMENT_BLOCK,
                                          bootstrap or self.BOOTSTRAP, **kwargs)
            self.assertFalse(os.path.exists(path))

    def test_pin_writer_refuses_real_networks(self):
        for chain_id in (1, 11_155_111, 17_000):
            self.assert_pins_refused(self.FakeAnvil(chain_id), "is not the regtest origin chain 31337")
        regtest = self.FakeAnvil(REGTEST_ETH_CHAIN_ID)
        for gvr in (ETHEREUM_MAINNET_GVR, bytes(32)):
            self.assert_pins_refused(regtest, "zero or the Ethereum mainnet root", gvr=gvr)
        for version in ("00000000", "05000000", "90000074"):
            self.assert_pins_refused(regtest, f"fork version 0x{version} is outside the synthetic 0xb3 namespace",
                                     fork_version=version)
        late = dict(self.BOOTSTRAP, checkpoint_block=self.DEPLOYMENT_BLOCK +
                    MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS + 1)
        self.assert_pins_refused(regtest, "must lie within 20000 blocks", bootstrap=late)

    def test_pin_writer_refuses_remote_endpoints(self):
        for url in ("https://ethereum-rpc.example.invalid", "http://10.0.0.1:8545"):
            with tempfile.TemporaryDirectory() as tmp, \
                    self.assertRaisesRegex(AssertionError, "Refusing non-local endpoint"):
                write_regtest_bridge_pins(os.path.join(tmp, "pins.json"), url, *self.ADDRESSES,
                                          self.DEPLOYMENT_BLOCK, self.BOOTSTRAP)

    def test_local_url_guard(self):
        for url in ("http://127.0.0.1:8545", "http://localhost:5052", "http://127.0.0.1"):
            self.assertEqual(assert_local_url(url), url)
        for url in ("https://127.0.0.1:8545", "http://10.0.0.1:8545",
                    "http://user:pass@127.0.0.1:8545", "http://user@localhost:8545",
                    "http://127.0.0.1.example.invalid:8545", "ws://localhost:8545",
                    "http://localhost:notaport", "127.0.0.1:8545", "", None):
            with self.assertRaises(AssertionError, msg=url):
                assert_local_url(url)
        with self.assertRaises(AssertionError):
            LocalEthRpc("https://ethereum-rpc.example.invalid")
