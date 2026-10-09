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
  (TestFrameworkB3BridgeGuards, run by feature_framework_unit_tests.py);
- bridge V2 (first node slice, RT4a): CREATE address prediction, the bUSD
  V2 AssetId, leaf V2 and the V2 withdrawal tree, the -b3bridgepins v2
  writer, Verifier V2 / Vault V2 calldata encoders, a DeployV2.s.sol
  runner and FreshVoteShuttle (waits on fresh-lineage votes, which travel
  over the P2P message finsigv2; its RPC relay remains for drills);
- bridge V2 (second node slice, RT4): the OI-12 K_OF_N bootstrap message
  and K_OF_N initialize calldata, a runner of the REGTEST ONLY
  b3-bridge-regtest-certify tool, the kind 9 observation plan, a one-shot
  run of contrib/b3bridge/b3_bridge_observe_v2.py, a light-client update
  to the anvil head, mining under either clock and a fresh-finality lag
  shuttle for the V2 burn gate (row C3);
- bridge V2 (third node slice, RT4c): validators bound to the tool's public
  test keys (bring_up_validators(finality_key_label=...)), the
  FinalizedBlockV2 encoder, digest V2, history leaves, the kind 13
  FINALITY_CONFLICT record, Verifier V2 reportConflict calldata and a
  one-shot run of contrib/b3bridge/b3_bridge_deposit_guard_v2.py;
- bridge V2 (fourth node slice, RT4d): the B-23 successor vault pins, the
  kind 16 VAULT_SWEEP record, the successor kind 9 subjects 18 and 19 and
  their observation plan, and the B3VaultV2 constructor Config encoder;
- fresh-lineage per-epoch keys (B-37): a validator count other than four
  (B3BridgeChainMixin.bridge_validator_count), bind_fresh_keys() and an
  independent parser of a node's fresh key store file
  (parse_fresh_key_store, src/node/finality_fresh_key_store.h);
- bridge V2 forwarders (routes (b) and (d3), RT6; contracts spec sections
  7-9): the Arbitrum anvil's chain id and both LayerZero endpoint ids, the
  -b3bridgepins v2 deposit_forwarder / exit_forwarder pins, DeployV2 with
  its forwarder factories (deployer nonces 4 and 5), the init-code hash of
  a forge artifact, the EIP-191 refund EOA message signed by an anvil dev
  account (anvil personal_sign, recovered here independently and checked
  by `cast wallet verify` when cast is present), IOFT send calldata and
  one-shot runs of contrib/b3bridge/b3_bridge_forwarders_v2.py and
  contrib/b3bridge/regtest/mock_lz_relayer.py. Salts, CREATE2 addresses,
  the OI-2 fee cap and the parameter checks are the contrib module's,
  imported here, so Python has one implementation of them.
- sponsored native-B3 fees (owner ruling R10, RT7s): a one-shot run of
  contrib/b3bridge/b3_bridge_sponsor.py (fee-coin pool refill, status and
  completion of a user's partial burn) against a relayer wallet.

The relayer module is imported only as a library; its main() is never
called in-process, only as an unchanged subprocess by run_relayer_once().
The forwarder and LayerZero tools likewise run only as subprocesses.
Nothing in this module contacts a non-local endpoint.
"""

from collections import Counter, namedtuple
import copy
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
    B3BRIDGE_REGTEST_DIR,
    SRCDIR,
    artifact,
    assert_local_url,
    find_foundry_tool,
    forge_build,
    forge_test,
    import_b3bridge_contrib,
    local_json_rpc,
    normalize_address,
    run_forge,
)
from test_framework.authproxy import JSONRPCException
from test_framework.crypto.secp256k1 import G, GE
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
# Library use only (salts, CREATE2 addresses, the OI-2 fee cap, its ABI
# codec): its main() runs only as a subprocess, in run_forwarder_tool().
forwarders_v2 = import_b3bridge_contrib("b3_bridge_forwarders_v2")
observe_v2 = import_b3bridge_contrib("b3_bridge_observe_v2")

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


# -- bridge V2 identities, leaf V2 and the withdrawal tree --------------------
# Derived here independently of src/modern/bridge_v2.h, withdrawal_v2.h and
# contracts/src/v2/B3TypesV2.sol; contracts/test/vectors/bridge_v2_vectors.json
# pins all three (TestFrameworkB3BridgeGuards below checks this module).
BRIDGE_V2_ASSET_TAG = "B3/BRIDGE/ASSET/V2"
BRIDGE_ASSET_IDENTITY_VERSION_V2 = 2  # Consensus::BRIDGE_ASSET_IDENTITY_VERSION_V2
BRIDGE_V2_DECIMALS = 6
WITHDRAWAL_LEAF_V2_TAG = keccak256(b"B3/BRIDGE/WITHDRAWAL/V2")
WITHDRAWAL_LEAF_V2_PREIMAGE_SIZE = 180
WITHDRAWAL_TREE_DEPTH = 32
MAX_WITHDRAWAL_ID = 1 << WITHDRAWAL_TREE_DEPTH  # B3TypesV2.MAX_INDEX
WITHDRAWAL_ZERO_HASHES = [bytes(32)]
for _depth in range(WITHDRAWAL_TREE_DEPTH):
    WITHDRAWAL_ZERO_HASHES.append(keccak256(WITHDRAWAL_ZERO_HASHES[-1] * 2))
del _depth
# Root of the empty depth-32 tree: non-zero, signed from V until the first burn.
EMPTY_WITHDRAWAL_ROOT = WITHDRAWAL_ZERO_HASHES[WITHDRAWAL_TREE_DEPTH]
BRIDGE_V2_DEFAULT_CAP_RAW = 10_000_000_000  # Consensus::BRIDGE_V2_DEFAULT_CAP_RAW (B-14)


def predict_create_address(sender, nonce):
    """The CREATE address of `sender` at `nonce`: keccak(rlp([sender, nonce]))[12:]."""
    assert isinstance(nonce, int) and nonce >= 0, nonce
    return "0x" + keccak256(rlp([hx(normalize_address(sender)), nonce]))[12:].hex()


def asset_id_v2(domain, chain_id, vault, token, origin_decimals=BRIDGE_V2_DECIMALS,
                asset_decimals=BRIDGE_V2_DECIMALS):
    """Raw bUSD V2 AssetId bytes (node changes §3.2; Vault V2 B3_ASSET_ID).

    TaggedHash("B3/BRIDGE/ASSET/V2") over chainDomain || u8 2 || u64 LE
    chain id || vault || token || u8 origin decimals || u8 asset decimals
    (an 83-byte message).
    """
    assert 0 < chain_id < 1 << 64, chain_id
    message = (enc_bytes32(domain) + bytes([BRIDGE_ASSET_IDENTITY_VERSION_V2]) +
               chain_id.to_bytes(8, "little") + hx(normalize_address(vault)) +
               hx(normalize_address(token)) + bytes([origin_decimals, asset_decimals]))
    assert_equal(len(message), 83)
    return TaggedHash(BRIDGE_V2_ASSET_TAG, message)


def leaf_v2_preimage(asset_id, withdrawal_id, chain_id, vault, token, recipient, amount, b3_height):
    """The 180-byte leaf V2 preimage (contracts spec; B3TypesV2.withdrawalLeaf).

    tag || AssetId || u64 id || u64 chain id || vault || token || recipient ||
    u256 amount || u64 b3Height, integers big-endian.
    """
    assert 0 <= withdrawal_id < MAX_WITHDRAWAL_ID, withdrawal_id
    assert 0 < chain_id < 1 << 64 and 0 < amount <= MAX_MONEY and 0 <= b3_height < 1 << 64
    preimage = (WITHDRAWAL_LEAF_V2_TAG + enc_bytes32(asset_id) +
                withdrawal_id.to_bytes(8, "big") + chain_id.to_bytes(8, "big") +
                hx(normalize_address(vault)) + hx(normalize_address(token)) +
                hx(normalize_address(recipient)) + amount.to_bytes(32, "big") +
                b3_height.to_bytes(8, "big"))
    assert_equal(len(preimage), WITHDRAWAL_LEAF_V2_PREIMAGE_SIZE)
    return preimage


def leaf_v2(*args, **kwargs):
    """keccak256 of leaf_v2_preimage(...)."""
    return keccak256(leaf_v2_preimage(*args, **kwargs))


def withdrawal_root_and_path(leaves, index=None):
    """Depth-32 ordered withdrawal tree with zero-subtree padding.

    Returns the root and, when index is given, the 32 siblings of that leaf
    from the leaf level up. An empty tree has EMPTY_WITHDRAWAL_ROOT.
    """
    leaves = [enc_bytes32(leaf) for leaf in leaves]
    assert index is None or 0 <= index < len(leaves)
    if not leaves:
        return EMPTY_WITHDRAWAL_ROOT, None
    level, path, position = leaves, [], index
    for depth in range(WITHDRAWAL_TREE_DEPTH):
        if len(level) % 2:
            level.append(WITHDRAWAL_ZERO_HASHES[depth])
        if position is not None:
            path.append(level[position ^ 1])
            position >>= 1
        level = [keccak256(level[i] + level[i + 1]) for i in range(0, len(level), 2)]
    assert_equal(len(level), 1)
    return level[0], (path if index is not None else None)


def withdrawal_root(leaves):
    return withdrawal_root_and_path(leaves)[0]


def fold_withdrawal_path(leaf, index, path):
    """The vault's walk: index bit d = 0 hashes (node || sibling), else (sibling || node)."""
    assert_equal(len(path), WITHDRAWAL_TREE_DEPTH)
    node = enc_bytes32(leaf)
    for depth, sibling in enumerate(path):
        sibling = enc_bytes32(sibling)
        node = keccak256(sibling + node) if (index >> depth) & 1 else keccak256(node + sibling)
    return node


# -- bridge V2 forwarders: chains, nonces, identities (contracts spec §7-§9) ----
# The second anvil stands in for Arbitrum One only as the USDT0 chain (RP-5).
ARBITRUM_REGTEST_CHAIN_ID = 31338
# LayerZero V2 endpoint ids of Ethereum and Arbitrum One. The regtest mock
# endpoints use the same ids; Arbitrum's is B3OftExitFactory's DST_EID.
LZ_EID_ETHEREUM = 30101
LZ_EID_ARBITRUM = 30110
# DeployV2 with B3V2_DEPLOY_FORWARDERS=1 continues its four CREATEs (deployer
# nonces n..n+3) with the F_d factory at n+4, the address the vault already
# names as FORWARDER_FACTORY, and the F_x factory at n+5. The pin writer
# requires n = 0, so these are absolute nonces there.
DEPLOY_V2_FORWARDER_CONTRACTS = ("B3DepositForwarderFactory", "B3OftExitFactory")
FORWARDER_FACTORY_NONCE = 4
EXIT_FACTORY_NONCE = 5
# forge artifact (file, contract) whose creation code both factories CREATE2.
FORWARDER_ARTIFACT = ("B3Forwarder.sol", "B3Forwarder")
FORWARDERS_V2_SCRIPT = str(B3BRIDGE_CONTRIB_DIR / "b3_bridge_forwarders_v2.py")
MOCK_LZ_RELAYER_SCRIPT = str(B3BRIDGE_REGTEST_DIR / "mock_lz_relayer.py")

# One Python implementation: the contrib module's derivations (spec §7.2).
DepositParams = forwarders_v2.DepositParams
ExitParams = forwarders_v2.ExitParams
address_word = forwarders_v2.address_word
deposit_forwarder_address = forwarders_v2.deposit_forwarder_address
exit_forwarder_address = forwarders_v2.exit_forwarder_address
deposit_fee_cap = forwarders_v2.deposit_fee_cap
assert forwarders_v2.REGTEST_ETH_CHAIN_ID == REGTEST_ETH_CHAIN_ID


def forwarder_init_code_hash(contract_artifact):
    """FORWARDER_INIT_CODE_HASH: keccak256 of B3Forwarder's creation code.

    contract_artifact is artifact()'s dict for FORWARDER_ARTIFACT, or the raw
    creation code. Never hard-code the result: any byte of B3Forwarder.sol,
    comments included, changes its metadata and so this hash. The pins must
    name the value the deployed factories report.
    """
    code = contract_artifact["bytecode"] if isinstance(contract_artifact, dict) else bytes(contract_artifact)
    assert code, "B3Forwarder creation code is empty"
    return keccak256(code)


def forwarder_pins_v2(deployer, init_code_hash, *, dst_eid=LZ_EID_ARBITRUM, roles=("deposit", "exit")):
    """write_regtest_bridge_pins_v2's `forwarders` for a DeployV2 run with
    B3V2_DEPLOY_FORWARDERS=1 from `deployer`'s nonce 0: the deposit factory
    at nonce FORWARDER_FACTORY_NONCE, the exit factory at EXIT_FACTORY_NONCE,
    both with init_code_hash, and the exit pin's dst_eid. roles selects the
    pins written (the node accepts either one alone)."""
    assert roles and set(roles) <= {"deposit", "exit"}, roles
    init_code_hash = to_0x(enc_bytes32(init_code_hash))
    pins = {}
    if "deposit" in roles:
        pins["deposit"] = {"factory": predict_create_address(deployer, FORWARDER_FACTORY_NONCE),
                           "init_code_hash": init_code_hash}
    if "exit" in roles:
        pins["exit"] = {"factory": predict_create_address(deployer, EXIT_FACTORY_NONCE),
                        "init_code_hash": init_code_hash, "dst_eid": dst_eid}
    return pins


# -- -b3bridgepins v2 section (node changes §3.1-3.4, OI-3) --------------------
def _light_client_pins_v2(bootstrap_output, gvr, fork_version, horizon_epochs):
    checkpoint_slot = bootstrap_output["checkpoint_slot"]
    return {
        "trusted_checkpoint_root": bootstrap_output["checkpoint_root"],
        "trusted_checkpoint_slot": checkpoint_slot,
        "checkpoint_execution_block": bootstrap_output["checkpoint_block"],
        "genesis_validators_root": to_0x(gvr),
        "fork_schedule": [{"activation_epoch": 0, "fork_version": to_0x(fork_version)}],
        "fork_schedule_valid_through_epoch": checkpoint_slot // SLOTS_PER_EPOCH + horizon_epochs,
        "electra_epoch": 0,
        "min_sync_committee_participants": SYNC_COMMITTEE_SUPERMAJORITY,
        "max_sync_lag_slots": MAX_SYNC_LAG_SLOTS,
    }


def write_regtest_bridge_pins_v2(path, anvil, *, vault, token, verifier, deployer,
                                 deployment_block, bootstrap_output, activation_height,
                                 max_deposit_raw, burn_height=None, caps=None,
                                 min_bridge_validators=4, max_bridge_validators=64,
                                 min_bridge_total_weight=4, horizon_epochs=1024,
                                 gvr=REGTEST_GVR, fork_version=REGTEST_FORK_VERSION,
                                 verifier_max_epoch_lag=None, successor=None, forwarders=None,
                                 sponsored_sighash_height=None, check=True):
    """Write a -b3bridgepins file holding only a v2 section; return its JSON.

    The file must exist before the validators first start (OI-3), when the
    V2 contracts cannot be deployed yet: their constructors pin the fresh
    lineage, which commits to blockHash(R_f - 1). So vault and verifier are
    the addresses DeployV2.s.sol will create from `deployer`'s nonces 2 and 1
    (prover 0, verifier 1, vault 2, relay batch 3), and the file carries no
    vault or verifier code hash, which the node refuses as unknown keys.
    deployment_block is a lower bound on the vault's first event block.

    With check (the default) the writer refuses, before writing: a chain id
    other than 31337 (also for a non-local endpoint, refused first); the
    mainnet genesis validators root or a fork version outside 0xb3; vault,
    token and verifier that are zero or not pairwise distinct; a checkpoint
    outside deployment..deployment+20,000; an activation height below 1 or a
    burn height below it; caps outside 0 < max_deposit_raw <= max_per_block
    <= max_per_epoch <= MAX_MONEY; validator bounds outside 4..64 or a zero
    weight; and, read from anvil, a checkpoint not on this chain, a codeless
    token, code already at the vault or verifier, a deployer nonce other than
    zero and vault/verifier other than the deployer's CREATE addresses at
    nonces 2/1. check=False writes the file anyway, for node-refusal tests.
    caps may set max_per_block and/or max_per_epoch (node defaults 1e10).
    verifier_max_epoch_lag is the Verifier V2 MAX_EPOCH_LAG DeployV2 will use
    (default: profile R's GATE_R_SET_EXPIRY, DeployV2's R_MAX_EPOCH_LAG); the
    node requires it to equal its burn gate's set expiry, and the writer
    refuses any other value first.

    successor, when given, pins a B-23 successor vault v' (node changes §7.6)
    in the same file, so it too exists before the validators first start and
    the record guard freezes it with v; its rules apply only from h_m. It is
    a dict with "vault" (v', the CREATE address of a dedicated "deployer" at
    nonce 0, since v' can only be deployed once v holds code), "asset_id" (v's
    bUSD V2 AssetId, raw bytes as asset_id_v2 returns them; the node refuses
    any other value), "migration_height" (h_m) and optionally
    "origin_deployment_block" (node default: v's). The writer emits v as the
    predecessor and every other field is inherited from v by the node. With
    check it also refuses: a successor deployer equal to `deployer` (whose
    nonce 0 DeployV2 uses), a v' other than predict_create_address(successor
    deployer, 0), a v' equal to vault, token or verifier, a zero or non-32-byte
    AssetId, h_m <= V or h_m < W2, a deployment bound below 1, and, read from
    anvil, a successor deployer nonce other than zero or code already at v'.

    forwarders, when given, pins the route (b) and (d3) forwarder factories
    (node changes §3.1; contracts spec §7-§8) as v2.deposit_forwarder and
    v2.exit_forwarder, so they too exist before the validators first start
    and the record guard freezes them with v. It is a dict with "deposit"
    ({"factory", "init_code_hash"}) and/or "exit" ({"factory",
    "init_code_hash", "dst_eid"}), as forwarder_pins_v2() builds it: the
    factories are DeployV2's CREATEs at deployer nonces 4 and 5
    (B3V2_DEPLOY_FORWARDERS=1) and the hash is forwarder_init_code_hash() of
    the forge artifact, all knowable before the deployment. With check it
    also refuses: a deposit factory other than
    predict_create_address(deployer, FORWARDER_FACTORY_NONCE), an exit
    factory other than the EXIT_FACTORY_NONCE address, an init-code hash
    that is zero or not 32 bytes, two different hashes (both factories
    CREATE2 the same B3Forwarder), a dst_eid other than LZ_EID_ARBITRUM and,
    read from anvil, code already at either factory.

    sponsored_sighash_height, when given, pins owner ruling R10's sponsored
    sighash rule (v2.sponsored_sighash_height, >= V): from that height a
    kind 6 burn's owner may sign SIGHASH_ALL|ANYONECANPAY. Without it the
    node keeps the rule off. With check the writer refuses a height below
    activation_height.
    """
    assert os.path.isabs(path), "-b3bridgepins needs an absolute path"
    caps = dict(caps or {})
    assert set(caps) <= {"max_per_block", "max_per_epoch"}, caps
    vault, token, verifier, deployer = (normalize_address(a) for a in (vault, token, verifier, deployer))
    if successor is not None:
        successor = dict(successor)
        assert {"vault", "deployer", "asset_id", "migration_height"} <= set(successor) <= {
            "vault", "deployer", "asset_id", "migration_height", "origin_deployment_block"}, successor
        successor["vault"] = normalize_address(successor["vault"])
        successor["deployer"] = normalize_address(successor["deployer"])
        successor["asset_id"] = hx(successor["asset_id"])
    if forwarders is not None:
        forwarder_keys = {"deposit": {"factory", "init_code_hash"},
                          "exit": {"factory", "init_code_hash", "dst_eid"}}
        assert forwarders and set(forwarders) <= set(forwarder_keys), forwarders
        forwarders = {role: dict(pin) for role, pin in forwarders.items()}
        for role, pin in forwarders.items():
            assert set(pin) == forwarder_keys[role], (role, pin)
            pin["factory"] = normalize_address(pin["factory"])
            pin["init_code_hash"] = hx(pin["init_code_hash"])
    gvr = hx(gvr)
    fork_version = hx(fork_version)
    checkpoint_block = bootstrap_output["checkpoint_block"]

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
    if bytes(20) in (hx(vault), hx(token), hx(verifier)) or len({vault, token, verifier}) != 3:
        refuse("vault, token and verifier addresses must be non-zero and pairwise distinct")
    if not (1 <= deployment_block <= checkpoint_block <=
            deployment_block + MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS):
        refuse(f"checkpoint execution block {checkpoint_block} must lie within "
               f"{MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS} blocks at or after the "
               f"deployment block {deployment_block} (check 4)")
    if activation_height < 1 or (burn_height is not None and burn_height < activation_height):
        refuse(f"need 1 <= activation_height ({activation_height}) <= burn_height ({burn_height})")
    if sponsored_sighash_height is not None and not (
            activation_height <= sponsored_sighash_height <= 2**31 - 1):
        refuse(f"the sponsored_sighash_height ({sponsored_sighash_height}) must be at or above "
               f"activation_height ({activation_height})")
    block_cap = caps.get("max_per_block", BRIDGE_V2_DEFAULT_CAP_RAW)
    epoch_cap = caps.get("max_per_epoch", BRIDGE_V2_DEFAULT_CAP_RAW)
    if not 0 < max_deposit_raw <= block_cap <= epoch_cap <= MAX_MONEY:
        refuse(f"caps must satisfy 0 < max_deposit_raw ({max_deposit_raw}) <= max_per_block "
               f"({block_cap}) <= max_per_epoch ({epoch_cap}) <= MAX_MONEY (check 1)")
    if not (4 <= min_bridge_validators <= max_bridge_validators <= 64 and min_bridge_total_weight > 0):
        refuse("need 4 <= min_bridge_validators <= max_bridge_validators <= 64 and a non-zero weight")
    if verifier_max_epoch_lag is None:
        verifier_max_epoch_lag = GATE_R_SET_EXPIRY
    if verifier_max_epoch_lag != GATE_R_SET_EXPIRY:
        refuse(f"verifier_max_epoch_lag {verifier_max_epoch_lag} is not the regtest gate's set expiry "
               f"{GATE_R_SET_EXPIRY} (DeployV2 R_MAX_EPOCH_LAG)")
    if successor is not None:
        successor_vault, successor_deployer = successor["vault"], successor["deployer"]
        effective_burn = activation_height if burn_height is None else burn_height
        migration_height = successor["migration_height"]
        if hx(successor_vault) == bytes(20) or successor_vault in (vault, token, verifier):
            refuse("the successor vault must be non-zero and differ from vault, token and verifier")
        if successor_deployer == deployer:
            refuse("the successor deployer must differ from the V2 deployer, whose nonce 0 DeployV2 uses")
        if successor_vault != predict_create_address(successor_deployer, 0):
            refuse(f"the successor vault {successor_vault} is not the CREATE address of its deployer "
                   f"{successor_deployer} at nonce 0")
        if len(successor["asset_id"]) != 32 or successor["asset_id"] == bytes(32):
            refuse("the successor asset_id must be v's non-zero 32-byte bUSD V2 AssetId")
        if not (isinstance(migration_height, int) and activation_height < migration_height and
                effective_burn <= migration_height <= 2**31 - 1):
            refuse(f"the successor migration_height ({migration_height}) must be above activation_height "
                   f"({activation_height}), at or above burn_height ({effective_burn}) and at most {2**31 - 1}")
        if successor.get("origin_deployment_block", 1) < 1:
            refuse("the successor origin_deployment_block must be at least 1")
    if forwarders is not None:
        for role, nonce in (("deposit", FORWARDER_FACTORY_NONCE), ("exit", EXIT_FACTORY_NONCE)):
            pin = forwarders.get(role)
            if pin is None:
                continue
            if pin["factory"] != predict_create_address(deployer, nonce):
                refuse(f"the {role} forwarder factory {pin['factory']} is not DeployV2's CREATE address at "
                       f"deployer nonce {nonce} (B3V2_DEPLOY_FORWARDERS=1)")
            if len(pin["init_code_hash"]) != 32 or pin["init_code_hash"] == bytes(32):
                refuse(f"the {role} forwarder init_code_hash must be B3Forwarder's non-zero 32-byte "
                       "creation code hash")
        if len({bytes(pin["init_code_hash"]) for pin in forwarders.values()}) != 1:
            refuse("the deposit and exit forwarder init_code_hash differ; both factories CREATE2 B3Forwarder")
        if "exit" in forwarders and forwarders["exit"]["dst_eid"] != LZ_EID_ARBITRUM:
            refuse(f"the exit forwarder dst_eid {forwarders['exit']['dst_eid']} is not Arbitrum's LayerZero "
                   f"endpoint id {LZ_EID_ARBITRUM}")

    def rpc(method, *params):
        return local_json_rpc(_anvil_url(anvil), method, list(params))

    def code(address):
        return hx(rpc("eth_getCode", address, "latest"))

    if check:
        anchor = rpc("eth_getBlockByNumber", hex(checkpoint_block), False)
        if (anchor is None or relayer.norm_hex(anchor["hash"], 32) !=
                relayer.norm_hex(bootstrap_output["checkpoint_block_hash"], 32)):
            refuse(f"checkpoint block {checkpoint_block} is not on this anvil chain")
        for name, address in (("vault", vault), ("verifier", verifier)):
            if code(address):
                refuse(f"the {name} address {address} already holds code; pins must predict a "
                       "deployment that has not happened")
        nonce = int(rpc("eth_getTransactionCount", deployer, "latest"), 16)
        if nonce != 0:
            refuse(f"deployer {deployer} has nonce {nonce}; DeployV2 must start at nonce 0")
        for name, address, deployer_nonce in (("verifier", verifier, 1), ("vault", vault, 2)):
            if address != predict_create_address(deployer, deployer_nonce):
                refuse(f"the {name} is not DeployV2's CREATE address at deployer nonce {deployer_nonce}")
        if successor is not None:
            if code(successor["vault"]):
                refuse(f"the successor vault address {successor['vault']} already holds code; pins must "
                       "predict a deployment that has not happened")
            successor_nonce = int(rpc("eth_getTransactionCount", successor["deployer"], "latest"), 16)
            if successor_nonce != 0:
                refuse(f"successor deployer {successor['deployer']} has nonce {successor_nonce}; "
                       "v' must be its first deployment")
        for role, pin in sorted((forwarders or {}).items()):
            if code(pin["factory"]):
                refuse(f"the {role} forwarder factory address {pin['factory']} already holds code; pins "
                       "must predict a deployment that has not happened")
    token_code = code(token)
    if not token_code:
        refuse(f"no runtime code at the token {token}")

    v2 = {
        "origin_chain_id": REGTEST_ETH_CHAIN_ID,
        "vault": vault,
        "token": token,
        "token_runtime_code_hash": to_0x(keccak256(token_code)),
        "origin_deployment_block": deployment_block,
        "max_deposit_raw": max_deposit_raw,
        "light_client": _light_client_pins_v2(bootstrap_output, gvr, fork_version, horizon_epochs),
        "verifier": verifier,
        "min_bridge_validators": min_bridge_validators,
        "max_bridge_validators": max_bridge_validators,
        "min_bridge_total_weight": min_bridge_total_weight,
        "activation_height": activation_height,
        "verifier_max_epoch_lag": verifier_max_epoch_lag,
    }
    if burn_height is not None:
        v2["burn_height"] = burn_height
    if sponsored_sighash_height is not None:
        v2["sponsored_sighash_height"] = sponsored_sighash_height
    v2.update(caps)
    if successor is not None:
        v2["successor"] = {
            "vault": successor["vault"],
            "predecessor": vault,
            "asset_id": to_0x(successor["asset_id"]),
            "migration_height": successor["migration_height"],
        }
        if "origin_deployment_block" in successor:
            v2["successor"]["origin_deployment_block"] = successor["origin_deployment_block"]
    for role, pin in sorted((forwarders or {}).items()):
        v2[f"{role}_forwarder"] = dict(pin, init_code_hash=to_0x(pin["init_code_hash"]))
    document = {"format": PINS_FORMAT, "v2": v2}
    with open(path, "w", encoding="utf8") as f:
        json.dump(document, f, indent=2, sort_keys=True)
        f.write("\n")
    return document


def pin_sponsored_sighash_v2(path, height):
    """Add owner ruling R10's v2.sponsored_sighash_height to the
    -b3bridgepins file at path, written as write_regtest_bridge_pins_v2
    writes it, and return the new JSON. Only before the nodes first start
    with the file: the record guard refuses a changed file afterwards. The
    height must be at or above the file's activation_height (V)."""
    with open(path, encoding="utf8") as f:
        document = json.load(f)
    v2 = document["v2"]
    if not (isinstance(height, int) and v2["activation_height"] <= height <= 2**31 - 1):
        raise BridgePinError(f"refusing to pin sponsored_sighash_height {height!r} below activation_height "
                             f"{v2['activation_height']}")
    v2["sponsored_sighash_height"] = height
    with open(path, "w", encoding="utf8") as f:
        json.dump(document, f, indent=2, sort_keys=True)
        f.write("\n")
    return document


# -- Verifier V2 and Vault V2 calldata (contracts spec §3.3, §5, §6.4) ---------
FINALIZED_BLOCK_V2_SIZE = 185
FB_V2_TUPLE = "(bytes32,uint64,bytes32,bytes32,bytes32,uint64,uint64,bytes32,uint8)"
SET_HEADER_TUPLE = "(uint64,uint16,uint32,uint64,uint64,bytes,bytes32)"
VERIFIER_V2_SUBMIT_SIGNATURE = (f"submitCertificate({FB_V2_TUPLE},{SET_HEADER_TUPLE},"
                                f"{SET_HEADER_TUPLE},bytes32[32],bytes)")
VERIFIER_V2_INITIALIZE_SIGNATURE = (f"initialize({FB_V2_TUPLE},{SET_HEADER_TUPLE},{SET_HEADER_TUPLE},"
                                    f"{SET_HEADER_TUPLE},bytes32[32],bytes)")
VAULT_V2_WITHDRAWAL_TUPLE = "(uint64,address,uint256,uint64)"
VAULT_V2_RELEASE_SIGNATURE = f"release({VAULT_V2_WITHDRAWAL_TUPLE},bytes32[32])"
VAULT_V2_WITHDRAWAL_LEAF_SIGNATURE = f"withdrawalLeaf({VAULT_V2_WITHDRAWAL_TUPLE})"
# B3TypesV2.STATUS_*.
VERIFIER_V2_STATUS_UNINITIALIZED = 0
VERIFIER_V2_STATUS_LIVE = 1
VERIFIER_V2_STATUS_HALTED_CONFLICT = 2
# reportConflict (contracts spec §3.4) and its earlier-index form
# (B3FinalityVerifierV2.CONFLICT_EARLIER_INDEX).
VERIFIER_V2_REPORT_CONFLICT_SIGNATURE = (f"reportConflict({FB_V2_TUPLE},{SET_HEADER_TUPLE},uint64,bytes32,"
                                         f"bytes32[32],bytes)")
VERIFIER_V2_CONFLICT_EARLIER_INDEX = 3
# FinalizedBlockV2.bridgeFlags bit 0 (B3TypesV2.FLAG_DEPOSITS_CLOSED): B3
# closed burns for good, so Verifier V2 closes deposits for good.
FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED = 0x01
FINALITY_V2_DIGEST_TAG = "B3/FINALITY/V2"
BLS_SIGNATURE_SIZE = 96
MAX_FINALITY_SET = 8192


def decode_finalized_block_v2(data):
    """The 185-byte FinalizedBlockV2 as a dict (contracts spec §2)."""
    data = hx(data)
    assert_equal(len(data), FINALIZED_BLOCK_V2_SIZE)
    return {
        "lineage_id": data[0:32],
        "height": int.from_bytes(data[32:40], "big"),
        "block_hash": data[40:72],
        "withdrawal_root": data[72:104],
        "validator_set_hash": data[104:136],
        "epoch": int.from_bytes(data[136:144], "big"),
        "checkpoint_index": int.from_bytes(data[144:152], "big"),
        "checkpoint_history_root": data[152:184],
        "bridge_flags": data[184],
        "encoded": data,
    }


def encode_finalized_block_v2(fb):
    """The 185-byte FinalizedBlockV2 of a decode_finalized_block_v2() dict
    (its inverse; "encoded" is ignored): lineageId || u64 height ||
    blockHash || withdrawalRoot || validatorSetHash || u64 epoch || u64
    checkpointIndex || checkpointHistoryRoot || u8 bridgeFlags."""
    for key in ("height", "epoch", "checkpoint_index"):
        assert isinstance(fb[key], int) and 0 <= fb[key] < 1 << 64, (key, fb[key])
    assert isinstance(fb["bridge_flags"], int) and 0 <= fb["bridge_flags"] <= 0xff
    encoded = (enc_bytes32(fb["lineage_id"]) + fb["height"].to_bytes(8, "big") + enc_bytes32(fb["block_hash"]) +
               enc_bytes32(fb["withdrawal_root"]) + enc_bytes32(fb["validator_set_hash"]) +
               fb["epoch"].to_bytes(8, "big") + fb["checkpoint_index"].to_bytes(8, "big") +
               enc_bytes32(fb["checkpoint_history_root"]) + bytes([fb["bridge_flags"]]))
    assert_equal(len(encoded), FINALIZED_BLOCK_V2_SIZE)
    return encoded


def finality_digest_v2(domain, fb):
    """The BLS message of a fresh-lineage certificate: TaggedHash("B3/FINALITY/V2",
    chainDomain || FinalizedBlockV2), derived independently of
    modern::FinalityDigestV2 and B3TypesV2.digest."""
    encoded = encode_finalized_block_v2(fb) if isinstance(fb, dict) else hx(fb)
    assert_equal(len(encoded), FINALIZED_BLOCK_V2_SIZE)
    return TaggedHash(FINALITY_V2_DIGEST_TAG, enc_bytes32(domain) + encoded)


def history_leaf_v2(height, block_hash):
    """Checkpoint-history leaf keccak256(u64 height || blockHash) (B3TypesV2.historyLeaf).

    The history tree is the depth-32 withdrawal tree, so withdrawal_root(),
    withdrawal_root_and_path() and fold_withdrawal_path() apply unchanged.
    """
    assert isinstance(height, int) and 0 <= height < 1 << 64, height
    return keccak256(height.to_bytes(8, "big") + enc_bytes32(block_hash))


def encode_certificate_payload_v2(fb, signer_bitmap, aggregate_signature):
    """Certificate payload V2 (the type-11 carrier body and the body of a
    kind 13 record): FinalizedBlockV2 || bitmap || 96-byte aggregate signature."""
    encoded = encode_finalized_block_v2(fb) if isinstance(fb, dict) else hx(fb)
    signature = hx(aggregate_signature)
    assert_equal(len(encoded), FINALIZED_BLOCK_V2_SIZE)
    assert_equal(len(signature), BLS_SIGNATURE_SIZE)
    return encoded + hx(signer_bitmap) + signature


def fb_v2_abi_words(fb):
    """The nine static ABI words of a FinalizedBlockV2 tuple."""
    if not isinstance(fb, dict):
        fb = decode_finalized_block_v2(fb)
    return b"".join([
        fb["lineage_id"], enc_uint(fb["height"]), fb["block_hash"], fb["withdrawal_root"],
        fb["validator_set_hash"], enc_uint(fb["epoch"]), enc_uint(fb["checkpoint_index"]),
        fb["checkpoint_history_root"], enc_uint(fb["bridge_flags"]),
    ])


def abi_bytes_tail(data):
    """A dynamic `bytes` value: length word, data, zero padding to 32."""
    data = hx(data)
    return enc_uint(len(data)) + data + bytes(-len(data) % 32)


def encode_set_header_tuple(header):
    """ABI tuple of a SetHeader from an RPC set object (one dynamic member)."""
    pubkey = hx(header["aggregate_pubkey"])
    head = [enc_uint(header["epoch"]), enc_uint(header["ruleset_version"]),
            enc_uint(header["validator_count"]), enc_uint(header["total_weight"]),
            enc_uint(header["quorum_weight"]), enc_uint(7 * 32), enc_bytes32(header["members_root"])]
    return b"".join(head) + abi_bytes_tail(pubkey)


def _abi_encode_args(args):
    """args: (dynamic, bytes) pairs; static bytes go inline, dynamic ones as tails."""
    head_size = sum(32 if dynamic else len(data) for dynamic, data in args)
    head, tail = b"", b""
    for dynamic, data in args:
        if dynamic:
            head += enc_uint(head_size + len(tail))
            tail += data
        else:
            assert len(data) % 32 == 0
            head += data
    return head + tail


def _path_words(path):
    assert_equal(len(path), WITHDRAWAL_TREE_DEPTH)
    return b"".join(enc_bytes32(word) for word in path)


def encode_submit_certificate(cert, ancestry_path=None):
    """B3FinalityVerifierV2.submitCertificate calldata from a
    getfinalitylineagecertificate result (ancestry_path defaults to its own)."""
    if ancestry_path is None:
        ancestry_path = cert["history"]["ancestry_path"]
    return selector(VERIFIER_V2_SUBMIT_SIGNATURE) + _abi_encode_args([
        (False, fb_v2_abi_words(cert["encoded"])),
        (True, encode_set_header_tuple(cert["signing_set"])),
        (True, encode_set_header_tuple(cert["successor_set"])),
        (False, _path_words(ancestry_path)),
        (True, abi_bytes_tail(cert["proof"])),
    ])


def _encode_verifier_v2_initialize(fb, signing_set, start_set, successor, leaf0_path, proof):
    return selector(VERIFIER_V2_INITIALIZE_SIGNATURE) + _abi_encode_args([
        (False, fb_v2_abi_words(fb)),
        (True, encode_set_header_tuple(signing_set)),
        (True, encode_set_header_tuple(start_set)),
        (True, encode_set_header_tuple(successor)),
        (False, _path_words(leaf0_path)),
        (True, abi_bytes_tail(proof)),
    ])


def encode_verifier_v2_initialize(cert):
    """START_CERTIFICATE initialize calldata from a getfinalitylineagecertificate
    result: signingSet = startSet = the certificate's signing set (the pinned
    start set signs its own statement), its successor and leaf0Path."""
    return _encode_verifier_v2_initialize(cert["encoded"], cert["signing_set"], cert["signing_set"],
                                          cert["successor_set"], cert["history"]["leaf0_path"], cert["proof"])


def encode_verifier_v2_initialize_kofn(fb, bootstrap_header, start_set, successor, leaf0_path, proof):
    """K_OF_N initialize calldata (contracts spec §5 step 3): signingSet is
    the pinned bootstrap header and proof the 3-of-4 attestation over
    bootstrap_attestation_digest_v2 (b3-bridge-regtest-certify bootstrap);
    startSet (Set'_k), successor and leaf0Path come from B3's epoch-k
    certificate (getfinalitylineagecertificate). Set headers are RPC or tool
    set objects."""
    return _encode_verifier_v2_initialize(fb, bootstrap_header, start_set, successor, leaf0_path, proof)


def encode_withdrawal_v2_tuple(withdrawal_id, recipient, amount, b3_height):
    return enc_uint(withdrawal_id) + enc_address(recipient) + enc_uint(amount) + enc_uint(b3_height)


def encode_vault_v2_release(withdrawal_id, recipient, amount, b3_height, path):
    """B3VaultV2.release(w, path) calldata (4 + 36 * 32 bytes)."""
    return (selector(VAULT_V2_RELEASE_SIGNATURE) +
            encode_withdrawal_v2_tuple(withdrawal_id, recipient, amount, b3_height) + _path_words(path))


def encode_report_conflict(fb, signing_set, leaf_height, leaf_hash, path, proof):
    """B3FinalityVerifierV2.reportConflict(fb, signingSet, leafHeight,
    leafHash, path, proof) calldata (contracts spec §3.4).

    fb is a FinalizedBlockV2 (185 bytes or a decoded dict), signing_set an
    RPC or b3-bridge-regtest-certify set object of the verifier's current or
    next set, (leaf_height, leaf_hash) the history leaf `path` places (at
    fb's index under latestHistoryRoot in the earlier-index form) and proof
    the BlsDigestProver witness over fb's digest V2.
    """
    encoded = encode_finalized_block_v2(fb) if isinstance(fb, dict) else hx(fb)
    return selector(VERIFIER_V2_REPORT_CONFLICT_SIGNATURE) + _abi_encode_args([
        (False, fb_v2_abi_words(encoded)),
        (True, encode_set_header_tuple(signing_set)),
        (False, enc_uint(leaf_height, 64)),
        (False, enc_bytes32(leaf_hash)),
        (False, _path_words(path)),
        (True, abi_bytes_tail(proof)),
    ])


# -- K_OF_N bootstrap attestation (OI-12, owner ruling R9; contracts spec §5) --
# Derived here independently of B3TypesV2.bootstrapMessage and
# src/bridge/bootstrap_v2.h; bridge_v2_vectors.json pins all three.
BRIDGE_BOOTSTRAP_V2_TAG = "B3/BRIDGE/BOOTSTRAP/V2"
BRIDGE_BOOTSTRAP_V2_MESSAGE_SIZE = 301
VERIFIER_V2_BOOTSTRAP_DIGEST_SIGNATURE = f"bootstrapDigestOf({FB_V2_TUPLE})"
REGTEST_CERTIFY_TOOL = "b3-bridge-regtest-certify"


def bootstrap_attestation_message_v2(domain, evm_chain_id, verifier, start_set_hash, fb):
    """The 301-byte K_OF_N bootstrap message: chainDomain || u256 evmChainId ||
    verifier || startSetHash || FinalizedBlockV2 (OI-12: a signature binds one
    deployment on one chain and its start set)."""
    encoded = fb["encoded"] if isinstance(fb, dict) else hx(fb)
    assert_equal(len(encoded), FINALIZED_BLOCK_V2_SIZE)
    message = (enc_bytes32(domain) + enc_uint(evm_chain_id) + hx(normalize_address(verifier)) +
               enc_bytes32(start_set_hash) + encoded)
    assert_equal(len(message), BRIDGE_BOOTSTRAP_V2_MESSAGE_SIZE)
    return message


def bootstrap_attestation_digest_v2(domain, evm_chain_id, verifier, start_set_hash, fb):
    """sha256(tag || tag || message), tag = sha256("B3/BRIDGE/BOOTSTRAP/V2")."""
    return TaggedHash(BRIDGE_BOOTSTRAP_V2_TAG,
                      bootstrap_attestation_message_v2(domain, evm_chain_id, verifier, start_set_hash, fb))


class RegtestCertifyError(AssertionError):
    """b3-bridge-regtest-certify refused (exit 1); .message is its error text."""

    def __init__(self, message):
        self.message = message
        super().__init__(f"b3-bridge-regtest-certify refused: {message}")


_REGTEST_CERTIFY_RUNS = Counter()


def regtest_certify(tool, mode, input=None, *, workdir=None, args=(), timeout=300):
    """Run the REGTEST ONLY certificate tool once and return its JSON output.

    mode is bootstrap-set, keys (args: --label L --count N), bootstrap or
    certify; input (a dict) is written to <workdir>/<mode>-<n>.json for the
    last two. The tool never takes a secret key and refuses every chain
    domain but the -b3modernregtest one and every EVM chain id but 31337. A
    refusal raises RegtestCertifyError carrying its message.
    """
    command = [tool, mode, *args]
    if input is not None:
        assert workdir is not None, "an input file needs a workdir"
        os.makedirs(workdir, exist_ok=True)
        _REGTEST_CERTIFY_RUNS[mode] += 1
        path = os.path.join(workdir, f"{mode}-{_REGTEST_CERTIFY_RUNS[mode]}.json")
        with open(path, "w", encoding="utf8") as f:
            json.dump(input, f, indent=2, sort_keys=True, default=_json_default)
            f.write("\n")
        command.append(path)
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                            stdin=subprocess.DEVNULL, env=_local_process_env())
    if result.returncode != 0:
        message = result.stderr.strip()
        assert_equal(result.returncode, 1)
        assert message.startswith("error: "), message
        raise RegtestCertifyError(message[len("error: "):])
    output = json.loads(result.stdout)
    assert_equal(output["test_keys_only"], True)
    return output


# -- kind 13 FINALITY_CONFLICT (node changes §8.5) ------------------------------
FINALITY_CONFLICT_RECORD_KIND = 13
FINALITY_CONFLICT_RECORD_HEADER_SIZE = 4


def encode_finality_conflict_record(validator_count, payload):
    """The kind 13 FINALITY_CONFLICT bridge record submitbridgecarrier takes:
    0x0d || 0x00 || u16 n (big-endian) || certificate payload V2
    (FinalizedBlockV2 || bitmap(ceil(n/8)) || aggSig(96)), 286 bytes at
    n = 4. Derived independently of src/bridge/proof.h; B3 accepts it only
    from the fresh lineage's current or next committee (n is that size)."""
    payload = hx(payload)
    assert isinstance(validator_count, int) and 1 <= validator_count <= MAX_FINALITY_SET, validator_count
    assert_equal(len(payload), FINALIZED_BLOCK_V2_SIZE + (validator_count + 7) // 8 + BLS_SIGNATURE_SIZE)
    return bytes([FINALITY_CONFLICT_RECORD_KIND, 0]) + validator_count.to_bytes(2, "big") + payload


# -- B-23 vault migration (node changes §7.6, contracts spec §6.4) --------------
VAULT_SWEEP_RECORD_KIND = 16


def encode_vault_sweep_record(entry_id_evm):
    """The kind 16 VAULT_SWEEP bridge record submitbridgecarrier takes:
    exactly 34 bytes, 0x10 || 0x00 || the launch vault entry id in raw
    order (getbridgev2info vault_entry_id_evm, as kinds 3 and 6 carry a
    registry id). Derived independently of src/bridge/proof.h; consensus
    derives the amount, the leaf id and both vaults, so every sweep of one
    migration is this same record."""
    entry_id = hx(entry_id_evm)
    assert_equal(len(entry_id), 32)
    assert entry_id != bytes(32), "the vault entry id is null"
    return bytes([VAULT_SWEEP_RECORD_KIND, 0]) + entry_id


def encode_vault_v2_config(*, verifier, verifier_code_hash, chain_domain, token, token_code_hash,
                           forwarder_factory, max_deposit_raw, predecessor=None, predecessor_asset_id=None):
    """B3VaultV2's constructor argument, the static Config tuple
    (contracts/src/v2/B3VaultV2.sol): nine words in declaration order. A
    B-23 successor names the vault it replaces and that vault's
    B3_ASSET_ID; the launch vault leaves both zero."""
    assert (predecessor is None) == (predecessor_asset_id is None), "a successor needs both fields"
    words = [enc_address(verifier), enc_bytes32(verifier_code_hash), enc_bytes32(chain_domain),
             enc_address(token), enc_bytes32(token_code_hash), enc_address(forwarder_factory),
             enc_uint(max_deposit_raw),
             enc_address(predecessor or "0x" + "00" * 20),
             enc_bytes32(predecessor_asset_id if predecessor_asset_id is not None else bytes(32))]
    return b"".join(words)


# -- relayer deposit guard (node changes §8.5) ------------------------------------
DEPOSIT_GUARD_V2_SCRIPT = str(B3BRIDGE_CONTRIB_DIR / "b3_bridge_deposit_guard_v2.py")


class DepositGuardError(AssertionError):
    """b3_bridge_deposit_guard_v2.py refused (exit 1); .message is its error text."""

    def __init__(self, message):
        self.message = message
        super().__init__(f"b3_bridge_deposit_guard_v2.py refused: {message}")


def run_deposit_guard(node, timeout=120):
    """Run contrib/b3bridge/b3_bridge_deposit_guard_v2.py once as a subprocess.

    It asks node's getbridgev2info whether B3 has closed bridge V2 deposits
    for good. Returns its stdout JSON ({"deposits": "OPEN", "next_height"})
    on exit 0; a refusal (exit 1) raises DepositGuardError with its message.
    """
    b3_url, auth = b3_rpc_endpoint(node)
    command = [sys.executable, "-B", DEPOSIT_GUARD_V2_SCRIPT, "--b3-rpc-url", b3_url, *auth]
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                            stdin=subprocess.DEVNULL, env=_local_process_env())
    if result.returncode != 0:
        message = result.stderr.strip()
        assert_equal(result.returncode, 1)
        assert message.startswith("error: "), message
        raise DepositGuardError(message[len("error: "):])
    return json.loads(result.stdout)


# -- kind 9 ETH_STATE_OBSERVATION and the V2 burn gate (node changes §8) ------
OBSERVE_V2_SCRIPT = str(B3BRIDGE_CONTRIB_DIR / "b3_bridge_observe_v2.py")
ETH_OBSERVATION_SUBJECTS = {1: "VERIFIER_E", 2: "RESERVE_E", 3: "TOKEN_FLAGS"}
ETH_OBSERVATION_KEYS = {1: "verifier_e", 2: "reserve_e", 3: "token_flags"}
# B-23 successor subjects (node changes §7.6): the base subject | (vault entry
# index 1 << 4), RESERVE_E and TOKEN_FLAGS read from the successor vault v'.
# The next block may carry them only from the migration height.
ETH_OBSERVATION_SUBJECTS_SUCCESSOR = {0x12: "RESERVE_E_SUCCESSOR", 0x13: "TOKEN_FLAGS_SUCCESSOR"}
ETH_OBSERVATION_KEYS_SUCCESSOR = {0x12: "reserve_e_successor", 0x13: "token_flags_successor"}
# Byte of the verdict in a kind 9 payload: kind, reserved, subject, anchor hash.
ETH_OBSERVATION_VERDICT_OFFSET = 35
ETH_OBSERVATION_VERDICT_SETS_MISMATCH = 0x01
# Consensus::BridgeV2GateParams::RegtestProfileR() (mainnet / 36). The set
# expiry is also DeployV2's profile R MAX_EPOCH_LAG (R_MAX_EPOCH_LAG).
GATE_R_SET_EXPIRY = 72_000
GATE_R_RELEASE_MARGIN = 16_800
GATE_R_BURN_MARGIN = 2_400
GATE_R_ROTATION_AGE_LIMIT = GATE_R_SET_EXPIRY - GATE_R_RELEASE_MARGIN - GATE_R_BURN_MARGIN
GATE_R_OBSERVATION_MAX_AGE = 4_800
GATE_R_MAX_FINALITY_LAG = 30
GATE_R_MEMPOOL_CLOSE_MARGIN = 100
# Verifier V2 slots 0, 1, 2, 4, 6; Vault V2 slots 0, 1, 3; Tether slots 0, 10
# and the balances (2) and isBlackListed (6) mappings (contracts spec §3.2,
# §6.6; storage_layout_v2 in bridge_v2_vectors.json).
VERIFIER_V2_OBSERVED_SLOTS = (0, 1, 2, 4, 6)
VAULT_V2_OBSERVED_SLOTS = (0, 1, 3)
TOKEN_SLOT_OWNER_PAUSED = 0
TOKEN_SLOT_BALANCES = 2
TOKEN_SLOT_IS_BLACK_LISTED = 6
TOKEN_SLOT_UPGRADED_DEPRECATED = 10


def _reserve_and_flags_plan(vault, token):
    """(RESERVE_E plan, TOKEN_FLAGS plan) of one vault entry."""
    vault_word = enc_address(vault)
    reserve = [(normalize_address(vault), [enc_uint(s) for s in VAULT_V2_OBSERVED_SLOTS]),
               (normalize_address(token), [enc_uint(mapping_slot(vault_word, TOKEN_SLOT_BALANCES))])]
    flags = [(normalize_address(token), [enc_uint(TOKEN_SLOT_OWNER_PAUSED), enc_uint(TOKEN_SLOT_UPGRADED_DEPRECATED),
                                         enc_uint(mapping_slot(vault_word, TOKEN_SLOT_IS_BLACK_LISTED))])]
    return reserve, flags


def observation_plan_v2(verifier, vault, token, successor_vault=None):
    """{subject: [(address, [32-byte storage keys])]}: getbridgev2info's
    observation_plan derived independently (src/bridge/state_observation.h).

    With successor_vault (a B-23 v', node changes §7.6) it also holds
    subjects 0x12 and 0x13: the RESERVE_E and TOKEN_FLAGS reads of v' (vault
    slots 0, 1, 3 and balances[v']; token slots 0, 10 and isBlackListed[v'])."""
    reserve, flags = _reserve_and_flags_plan(vault, token)
    plan = {
        1: [(normalize_address(verifier), [enc_uint(s) for s in VERIFIER_V2_OBSERVED_SLOTS])],
        2: reserve,
        3: flags,
    }
    if successor_vault is not None:
        plan[0x12], plan[0x13] = _reserve_and_flags_plan(successor_vault, token)
    return plan


def tamper_observation_verdict(payload_hex, flip=ETH_OBSERVATION_VERDICT_SETS_MISMATCH):
    """A kind 9 payload with verdict bits flipped (still canonical and decodable)."""
    payload = bytearray(hx(payload_hex))
    assert_equal(payload[0], 9)
    payload[ETH_OBSERVATION_VERDICT_OFFSET] ^= flip
    return bytes(payload).hex()


class ObserverRunError(AssertionError):
    """b3_bridge_observe_v2.py refused (exit 1); .message is its error text."""

    def __init__(self, message):
        self.message = message
        super().__init__(f"b3_bridge_observe_v2.py refused: {message}")


def run_observer_once(node, wallet, ethereum_url, subjects=(1, 2, 3), *, dry_run=False, timeout=600):
    """Run contrib/b3bridge/b3_bridge_observe_v2.py once as a subprocess.

    It reads getbridgev2info.observation_plan from node, calls eth_getProof
    on the local anvil at the plan's anchor, builds each subject's kind 9
    record with buildbridgeobservationv2 and, unless dry_run, submits it
    with the wallet's submitbridgecarrier. Returns its stdout JSON; a refusal
    raises ObserverRunError.
    """
    b3_url, auth = b3_rpc_endpoint(node)
    command = [sys.executable, "-B", OBSERVE_V2_SCRIPT,
               "--ethereum-rpc", assert_local_url(ethereum_url),
               "--b3-rpc-url", b3_url, *auth,
               "--subjects", ",".join(str(subject) for subject in subjects)]
    if dry_run:
        command.append("--dry-run")
    else:
        command += ["--wallet", wallet]
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                            stdin=subprocess.DEVNULL, env=_local_process_env())
    if result.returncode != 0:
        message = result.stderr.strip()
        assert message.startswith("error: "), message
        raise ObserverRunError(message[len("error: "):])
    output = json.loads(result.stdout)
    assert_equal([o["subject"] for o in output["observations"]], sorted(set(subjects)))
    assert_equal(output["dry_run"], dry_run)
    return output


def gate_view(info):
    """getbridgev2info without burn_gate.now_eff, light_client.fresh and,
    with a B-23 successor, successor.sweep.gate.now_eff, which follow each
    node's clock (max(now, MTP + 1)) and so may differ between two calls."""
    view = copy.deepcopy(info)
    view.get("burn_gate", {}).pop("now_eff", None)
    view.get("light_client", {}).pop("fresh", None)
    view.get("successor", {}).get("sweep", {}).get("gate", {}).pop("now_eff", None)
    return view


# -- DeployV2.s.sol (contracts/script, regtest only) ---------------------------
DEPLOY_V2_CONTRACTS = ("BlsDigestProver", "B3FinalityVerifierV2", "B3VaultV2", "B3RelayBatch")


def deploy_v2_contracts(effective_env):
    """The contract names one DeployV2 run CREATEs, in deployer nonce order:
    DEPLOY_V2_CONTRACTS, plus DEPLOY_V2_FORWARDER_CONTRACTS when the
    environment forge sees sets B3V2_DEPLOY_FORWARDERS to 1 (the script's
    default 0 keeps the four)."""
    flag = str(effective_env.get("B3V2_DEPLOY_FORWARDERS", "0")).strip()
    assert flag in ("0", "1"), f"B3V2_DEPLOY_FORWARDERS must be 0 or 1, not {flag!r}"
    return DEPLOY_V2_CONTRACTS + (DEPLOY_V2_FORWARDER_CONTRACTS if flag == "1" else ())


def deploy_v2(forge, srcdir, builddir, anvil_url, deployer, env, broadcast_dir, timeout=600):
    """Run contracts/script/DeployV2.s.sol:DeployV2 once against a local anvil.

    env carries the script's B3V2_* inputs (B3V2_DEPLOYER is set from
    deployer). forge runs --offline with its build output and cache below
    <BUILDDIR>/test/forge (run_forge) and the broadcast records in
    broadcast_dir; no manifest is written unless env names one. The script
    itself refuses every chain id but 31337. Returns {contract name:
    {"address", "block", "gas_used", "tx_hash"}} for the four CREATEs: names
    and addresses from the broadcast record, receipts from anvil itself,
    matched by contractAddress (in one run the record's transaction hashes
    and receipts disagreed on which hash created which contract when
    several CREATEs shared a block, so its hash pairing is not trusted).

    With B3V2_DEPLOY_FORWARDERS=1 (env, B3V2_OFT_ADAPTER required) the run
    also CREATEs B3DepositForwarderFactory and B3OftExitFactory, returned
    the same way and checked here to be the deployer's CREATE addresses at
    its starting nonce + FORWARDER_FACTORY_NONCE and + EXIT_FACTORY_NONCE.
    The default path is unchanged: four contracts and no extra request.
    """
    assert_local_url(anvil_url)
    deployer = normalize_address(deployer)
    script = os.path.join(os.path.abspath(srcdir), "contracts", "script", "DeployV2.s.sol")
    full_env = {key: str(value) for key, value in env.items()}
    full_env["B3V2_DEPLOYER"] = deployer
    full_env["FOUNDRY_BROADCAST"] = os.path.abspath(broadcast_dir)
    # run_forge lays env over os.environ; judge the flag as forge will see it.
    expected = deploy_v2_contracts({**os.environ, **full_env})
    with_forwarders = len(expected) > len(DEPLOY_V2_CONTRACTS)
    if with_forwarders:
        start_nonce = int(local_json_rpc(anvil_url, "eth_getTransactionCount", [deployer, "latest"]), 16)
    run_forge(forge, srcdir, builddir, "script", f"{script}:DeployV2", "--rpc-url", anvil_url,
              "--broadcast", "--unlocked", "--sender", deployer, env=full_env, timeout=timeout)
    record_path = os.path.join(full_env["FOUNDRY_BROADCAST"], "DeployV2.s.sol",
                               str(REGTEST_ETH_CHAIN_ID), "run-latest.json")
    with open(record_path, encoding="utf8") as f:
        record = json.load(f)
    hashes = {relayer.norm_hex(tx["hash"], 32) for tx in record["transactions"]}
    hashes |= {relayer.norm_hex(r["transactionHash"], 32) for r in record["receipts"]}
    by_address = {}
    for tx_hash in sorted(hashes):
        receipt = local_json_rpc(anvil_url, "eth_getTransactionReceipt", [tx_hash])
        assert receipt is not None, f"anvil has no receipt for DeployV2 transaction {tx_hash}"
        assert_equal(normalize_address(receipt["from"]), deployer)
        assert_equal(int(receipt["status"], 16), 1)
        if receipt.get("contractAddress"):
            by_address[normalize_address(receipt["contractAddress"])] = receipt
    created = {}
    for tx in record["transactions"]:
        if tx.get("transactionType") != "CREATE":
            continue
        address = normalize_address(tx["contractAddress"])
        receipt = by_address[address]
        created[tx["contractName"]] = {
            "address": address,
            "block": int(receipt["blockNumber"], 16),
            "gas_used": int(receipt["gasUsed"], 16),
            "tx_hash": relayer.norm_hex(receipt["transactionHash"], 32),
        }
    assert_equal(sorted(created), sorted(expected))
    if with_forwarders:
        for name, offset in zip(DEPLOY_V2_FORWARDER_CONTRACTS, (FORWARDER_FACTORY_NONCE, EXIT_FACTORY_NONCE)):
            assert_equal(created[name]["address"], predict_create_address(deployer, start_nonce + offset))
    return created


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
                           max_ancestry=relayer.MAX_ANCESTRY_DISTANCE, bridge_v2=False):
    """Bootstrap, updates and one mint for `deposit`, verified by ethcheck.

    updates lists finalized execution blocks (ints), tool update specs
    (dicts) or ready tool outputs; the last one's finalized block is the
    receipt proof's source. The workdir gets the relayer's own config.json
    (from the beacon domain: electra_epoch 0, min_participants 342),
    bootstrap.json and updates.json, then relayer.capture_receipt() (no
    retained B3 anchor) and relayer.emit_plan() run b3-bridge-ethcheck
    --emit-payloads (default: next to the beacon tool). Returns the plan:
    bootstrap, updates, backfills and mint records with payload_hex.
    bridge_v2=True marks receipt_proof.json for a bridge V2 vault entry, so
    ethcheck applies the V2 receipt rules (types up to 0x7f, 8,192-byte
    terminal node, 16 KiB proof) when it builds the mint.
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
    if bridge_v2:
        proof_path = workdir / "receipt_proof.json"
        proof = json.loads(proof_path.read_text(encoding="utf8"))
        proof["bridge_v2"] = True
        proof_path.write_text(json.dumps(proof, indent=2, sort_keys=True) + "\n", encoding="utf8")
    plan = relayer.emit_plan(payload_tool, workdir)
    assert plan.get("bootstrap") is not None, "ethcheck emitted no bootstrap"
    assert plan.get("mint") is not None, "ethcheck emitted no mint"
    assert plan["mint"].get("bridge_v2", False) == bridge_v2, "ethcheck applied the wrong receipt rules"
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


# -- bridge V2 forwarders: refund EOA proof, OFT calldata, contrib tools -------
# The refund EOA proof (contracts spec §7.3; src/bridge/eip191.h): a route (b)
# forwarder pays refundEOA on refund and returnToOwner, so the wallet derives
# F_d (and bridgewithdraw's arbitrum route F_x) only for an EOA whose key
# personal_signed refund_eoa_message() under this chain's domain.
REFUND_EOA_MESSAGE_PREFIX_V2 = "B3 bridge V2 refund EOA"
EIP191_PREFIX = b"\x19Ethereum Signed Message:\n"
EIP191_SIGNATURE_SIZE = 65


def refund_eoa_message(domain, eoa):
    """The exact text a refund EOA signs (bridge::RefundEoaMessageV2): three
    lines, no trailing newline, with the raw 32-byte chain domain (wire order,
    as regtest_chain_domain() returns it) and the lower-case EOA."""
    return (f"{REFUND_EOA_MESSAGE_PREFIX_V2}\nchain domain: {to_0x(enc_bytes32(domain))}\n"
            f"refund EOA: {normalize_address(eoa)}")


def _message_bytes(message):
    return message.encode() if isinstance(message, str) else bytes(message)


def eip191_digest(message):
    """keccak256("\\x19Ethereum Signed Message:\\n" || decimal(len) || message)."""
    data = _message_bytes(message)
    return keccak256(EIP191_PREFIX + str(len(data)).encode() + data)


def recover_eip191_signer(message, signature):
    """ecrecover of a 65-byte r || s || v personal_sign signature (v 27 or 28,
    or the raw recovery id 0 or 1) over `message`, computed here with
    test_framework.crypto.secp256k1, independently of the node
    (bridge::RecoverEip191SignerV2) and of cast. Returns the lower-case 0x
    address, or None for a malformed signature or a failed recovery."""
    sig = hx(signature)
    if len(sig) != EIP191_SIGNATURE_SIZE:
        return None
    r, s, v = int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:64], "big"), sig[64]
    recid = v - 27 if v in (27, 28) else v if v in (0, 1) else None
    if recid is None or not (0 < r < GE.ORDER and 0 < s < GE.ORDER):
        return None
    # R's x is r (r < ORDER < p; recovery ids 2 and 3, x = r + ORDER, are not
    # produced by personal_sign); the recovery id's low bit is R's y parity.
    point = GE.lift_x(r)
    if point is None:
        return None
    if recid & 1:
        point = -point
    e = int.from_bytes(eip191_digest(message), "big") % GE.ORDER
    r_inv = pow(r, -1, GE.ORDER)
    public = GE.mul((s * r_inv, point), (-e * r_inv, G))
    if public.infinity:
        return None
    return normalize_address(keccak256(public.x.to_bytes() + public.y.to_bytes())[12:])


def personal_sign(anvil, account, message, *, check_with_cast=True):
    """EIP-191 personal_sign of `message` (text or bytes) by an unlocked anvil
    dev account, through anvil's own personal_sign: anvil holds the key and
    none is handled here. Returns the 65-byte r || s || v signature after two
    checks: recover_eip191_signer() must give `account` and, when cast is
    found (find_foundry_tool; never installed here) and check_with_cast,
    `cast wallet verify` (offline) must accept it too."""
    account = normalize_address(account)
    data = _message_bytes(message)
    signature = hx(local_json_rpc(_anvil_url(anvil), "personal_sign", [to_0x(data), account]))
    assert_equal(len(signature), EIP191_SIGNATURE_SIZE)
    assert_equal(recover_eip191_signer(data, signature), account)
    cast = find_foundry_tool("cast") if check_with_cast else None
    if cast is not None:
        # A 0x-prefixed message is read as raw bytes, so the text passes exactly.
        result = subprocess.run([cast, "wallet", "verify", "--address", account, to_0x(data), to_0x(signature)],
                                capture_output=True, text=True, timeout=120, stdin=subprocess.DEVNULL,
                                env=_local_process_env())
        if result.returncode != 0:
            raise AssertionError(f"cast wallet verify refused {account}'s personal_sign signature: "
                                 f"{(result.stderr or result.stdout).strip()}")
    return signature


# IOFT (contracts/src/v2/IOFTV2.sol; LayerZero's struct layouts and selectors).
OFT_SEND_PARAM_TUPLE = "(uint32,bytes32,uint256,uint256,bytes,bytes,bytes)"
OFT_MESSAGING_FEE_TUPLE = "(uint256,uint256)"
OFT_SEND_SIGNATURE = f"send({OFT_SEND_PARAM_TUPLE},{OFT_MESSAGING_FEE_TUPLE},address)"
OFT_QUOTE_SEND_SIGNATURE = f"quoteSend({OFT_SEND_PARAM_TUPLE},bool)"
# The mocks' packet: abi.encodePacked(bytes32 to, uint64 amount) (MockOFTBase).
OFT_MESSAGE_SIZE = 40


def _evm_word(to):
    """A 32-byte recipient word from an EVM address (left-padded) or a word."""
    data = bytes(to) if isinstance(to, (bytes, bytearray)) else hx(to)
    if len(data) == 20:
        return bytes(12) + data
    assert len(data) == 32, "an OFT recipient is a 20-byte address or a 32-byte word"
    return data


def oft_send_param(dst_eid, to, amount_ld, min_amount_ld=None, extra_options=b"", compose_msg=b"", oft_cmd=b""):
    """IOFTV2.SendParam as the tuple encode_oft_send() takes. `to` is an EVM
    address (left-padded to its word) or a 32-byte word; min_amount_ld
    defaults to amount_ld (no bridge fee accepted)."""
    return (dst_eid, _evm_word(to), amount_ld, amount_ld if min_amount_ld is None else min_amount_ld,
            hx(extra_options), hx(compose_msg), hx(oft_cmd))


def encode_oft_send(send_param, fee, refund):
    """Calldata of IOFT send(SendParam, MessagingFee, refundAddress), the
    dynamic tuple LayerZero's OFT and OFT adapter take. send_param is
    oft_send_param()'s tuple or a dict of its keyword arguments; fee is the
    native fee (lzTokenFee 0) or a (nativeFee, lzTokenFee) pair. The sender
    attaches msg.value == nativeFee (LayerZero's exact-fee rule)."""
    if isinstance(send_param, dict):
        send_param = oft_send_param(**send_param)
    fee = (fee, 0) if isinstance(fee, int) else tuple(fee)
    return forwarders_v2.encode_call(OFT_SEND_SIGNATURE, tuple(send_param), fee, normalize_address(refund))


def encode_oft_quote_send(send_param, pay_in_lz_token=False):
    """Calldata of IOFT quoteSend(SendParam, bool); it returns (nativeFee, lzTokenFee)."""
    if isinstance(send_param, dict):
        send_param = oft_send_param(**send_param)
    return forwarders_v2.encode_call(OFT_QUOTE_SEND_SIGNATURE, tuple(send_param), pay_in_lz_token)


def decode_oft_message(message):
    """(recipient word, amount) of a mock OFT packet message."""
    data = hx(message)
    assert_equal(len(data), OFT_MESSAGE_SIZE)
    return data[:32], int.from_bytes(data[32:], "big")


def forwarder_eth_rpc(anvil):
    """The contrib forwarder library's JSON-RPC client over a local anvil,
    for in-process use of its helpers (forwarders_v2.view and eth_call,
    whose revert raises CallReverted with a decoded reason, get_logs and
    decode_published)."""
    return observe_v2.JsonRpc(_anvil_url(anvil))


class ForwarderToolError(AssertionError):
    """b3_bridge_forwarders_v2.py refused (exit 1); .message is its error text."""

    def __init__(self, message):
        self.message = message
        super().__init__(f"b3_bridge_forwarders_v2.py refused: {message}")


FORWARDER_TOOL_COMMANDS = ("flush", "forward", "published")
_TOOL_FLAGS = {"sender": "--from", "tool_timeout": "--timeout"}


def _tool_arguments(options):
    args = []
    for name, value in options.items():
        if value is None:
            continue
        if isinstance(value, (dict, list)):
            text = json.dumps(value, sort_keys=True)
        elif isinstance(value, (bytes, bytearray)):
            text = to_0x(value)
        else:
            text = str(value)
        args += [_TOOL_FLAGS.get(name, "--" + name.replace("_", "-")), text]
    return args


def run_forwarder_tool(command, ethereum, *, node=None, timeout=300, **options):
    """Run contrib/b3bridge/b3_bridge_forwarders_v2.py <command> once as a subprocess.

    command is "flush" or "forward", which need node (its RPC endpoint and
    auth are passed), or "published", which reads Ethereum only. ethereum
    is the Ethereum anvil or its local URL. options become the subcommand's
    flags: sender -> --from, tool_timeout -> --timeout (the tool's receipt
    wait), any other name with "_" as "-" (fee_recipient -> --fee-recipient,
    certificate_block -> --certificate-block); a dict or list value goes as
    JSON (flush's params: the wallet's bridgeforwarderaddress result or its
    params object), bytes as 0x hex, and None omits the flag. timeout
    bounds the subprocess. Returns its stdout JSON on exit 0; a refusal
    (exit 1, "error: ..." on stderr) raises ForwarderToolError.
    """
    assert command in FORWARDER_TOOL_COMMANDS, command
    args = [sys.executable, "-B", FORWARDERS_V2_SCRIPT, command, "--ethereum-rpc", _anvil_url(ethereum)]
    if command == "published":
        assert node is None, "published reads Ethereum only"
    else:
        assert node is not None, f"{command} needs the B3 node"
        b3_url, auth = b3_rpc_endpoint(node)
        args += ["--b3-rpc-url", b3_url, *auth]
    args += _tool_arguments(options)
    result = subprocess.run(args, capture_output=True, text=True, timeout=timeout,
                            stdin=subprocess.DEVNULL, env=_local_process_env())
    if result.returncode != 0:
        message = result.stderr.strip()
        if result.returncode != 1 or not message.startswith("error: "):
            raise AssertionError(f"b3_bridge_forwarders_v2.py {command} exited {result.returncode}: {message}")
        raise ForwarderToolError(message[len("error: "):])
    return json.loads(result.stdout)


class MockLzRelayerError(AssertionError):
    """mock_lz_relayer.py refused (exit 1); .message is its error text."""

    def __init__(self, message):
        self.message = message
        super().__init__(f"mock_lz_relayer.py refused: {message}")


def run_mock_lz_relayer(src, src_endpoint, dst, dst_endpoint, sender, *, from_block=0, timeout=300):
    """Run contrib/b3bridge/regtest/mock_lz_relayer.py deliver once as a subprocess.

    It delivers every packet src_endpoint (a MockLzEndpoint on anvil src)
    sent to dst_endpoint's eid and dst_endpoint has not delivered yet,
    sending deliver() on anvil dst from `sender`, an unlocked dev account
    there. src and dst are anvils or local URLs; the tool itself refuses
    chain ids outside {31337, 31338}. Returns its stdout JSON on exit 0
    (nothing failed) and on exit 2 (a packet failed and stays undelivered
    for a later run: "failed" > 0, each such packet with its "error"); a
    refusal (exit 1) raises MockLzRelayerError.
    """
    command = [sys.executable, "-B", MOCK_LZ_RELAYER_SCRIPT, "deliver",
               "--src-url", _anvil_url(src), "--src-endpoint", normalize_address(src_endpoint),
               "--dst-url", _anvil_url(dst), "--dst-endpoint", normalize_address(dst_endpoint),
               "--from", normalize_address(sender), "--from-block", str(from_block)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout,
                            stdin=subprocess.DEVNULL, env=_local_process_env())
    if result.returncode in (0, 2):
        output = json.loads(result.stdout)
        assert_equal(result.returncode == 2, output["failed"] > 0)
        return output
    message = result.stderr.strip()
    if result.returncode != 1 or not message.startswith("error: "):
        raise AssertionError(f"mock_lz_relayer.py exited {result.returncode}: {message}")
    raise MockLzRelayerError(message[len("error: "):])


# -- R10 sponsored native-B3 fees: the relayer's contrib tool -----------------
SPONSOR_SCRIPT = str(B3BRIDGE_CONTRIB_DIR / "b3_bridge_sponsor.py")
SPONSOR_TOOL_COMMANDS = ("refill", "status", "complete", "release")


class SponsorToolError(AssertionError):
    """b3_bridge_sponsor.py refused (exit 1); .message is its error text."""

    def __init__(self, message):
        self.message = message
        super().__init__(f"b3_bridge_sponsor.py refused: {message}")


def run_sponsor_tool(command, node, wallet, *, timeout=300, **options):
    """Run contrib/b3bridge/b3_bridge_sponsor.py <command> once as a subprocess.

    command is "refill", "status", "complete" or "release"; node is the running B3
    node hosting the relayer's `wallet` (its RPC endpoint and auth are
    passed with --wallet). options become the subcommand's flags as in
    run_forwarder_tool (tool_timeout -> --timeout, any other name with "_"
    as "-"; min_tip -> --min-tip, max_fee -> --max-fee): True passes a bare
    flag (no_broadcast -> --no-broadcast), False and None omit it, and a
    Decimal goes as its exact decimal text. timeout bounds the subprocess.
    Returns its stdout JSON on exit 0, with fractional numbers as Decimal
    (the tool prints B3 amounts as exact decimal strings); a refusal (exit
    1, "error: ..." on stderr) raises SponsorToolError.
    """
    assert command in SPONSOR_TOOL_COMMANDS, command
    b3_url, auth = b3_rpc_endpoint(node)
    args = [sys.executable, "-B", SPONSOR_SCRIPT, command, "--b3-rpc-url", b3_url, *auth, "--wallet", wallet]
    for name, value in options.items():
        if value is None or value is False:
            continue
        flag = _TOOL_FLAGS.get(name, "--" + name.replace("_", "-"))
        if value is True:
            args.append(flag)
        elif isinstance(value, Decimal):
            args += [flag, format(value, "f")]
        else:
            args += [flag, str(value)]
    result = subprocess.run(args, capture_output=True, text=True, timeout=timeout,
                            stdin=subprocess.DEVNULL, env=_local_process_env())
    if result.returncode != 0:
        message = result.stderr.strip()
        if result.returncode != 1 or not message.startswith("error: "):
            raise AssertionError(f"b3_bridge_sponsor.py {command} exited {result.returncode}: {message}")
        raise SponsorToolError(message[len("error: "):])
    return json.loads(result.stdout, parse_float=Decimal)


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
    are the validators (nodes 0 to bridge_validator_count - 1 when a test
    overrides that class attribute, with as many stakes). Any further node
    (e.g. a sync checker that is not started) is ignored by the helpers
    below, which act only on running nodes. The corridor/stake helpers are
    copied from feature_flowmesh_release.py:79-199, which is left unchanged.
    """

    #: Nodes 0 .. bridge_validator_count - 1 are the validators.
    bridge_validator_count = VALIDATOR_COUNT

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

    def start_anvil(self, name="anvil", chain_id=REGTEST_ETH_CHAIN_ID, accounts=4):
        """Start a local anvil that shutdown() stops even on failure.

        accounts: the number of unlocked dev accounts (anvil's deterministic
        mnemonic, so account i is the same address in every run)."""
        anvil = Anvil(self.foundry["anvil"], os.path.join(self.options.tmpdir, name),
                      chain_id=chain_id, accounts=accounts)
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

    def bridge_method_ids(self, file, contract):
        """{signature: 4-byte selector hex} from a forge artifact (methodIdentifiers)."""
        with open(os.path.join(self.bridge_builddir, "test", "forge", "out", file, f"{contract}.json"),
                  encoding="utf8") as f:
            return json.load(f)["methodIdentifiers"]

    def bridge_deploy_v2(self, anvil, deployer, env, name="deploy_v2"):
        """deploy_v2() with this build and the broadcast records in the tmpdir."""
        return deploy_v2(self.foundry["forge"], self.bridge_srcdir, self.bridge_builddir,
                         _anvil_url(anvil), deployer, env,
                         os.path.join(self.options.tmpdir, name, "broadcast"))

    def start_arbitrum_anvil(self, name="anvil-arbitrum", accounts=4):
        """start_anvil() for the USDT0 chain standing in for Arbitrum (31338)."""
        return self.start_anvil(name, chain_id=ARBITRUM_REGTEST_CHAIN_ID, accounts=accounts)

    def bridge_forwarder_init_code_hash(self):
        """forwarder_init_code_hash() of this build's B3Forwarder artifact
        (needs a prior bridge_forge_build())."""
        return forwarder_init_code_hash(self.bridge_artifact(*FORWARDER_ARTIFACT))

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
        return self.nodes[:self.bridge_validator_count]

    def start_bridge_validators(self, pins_path=None, extra_args=()):
        """Add every node with the bridge arguments; start and link n0-n3.

        The validators link only to each other, in the framework's default
        chain topology (a line). Nodes past n3 (past the last validator with
        bridge_validator_count) are added but not started.
        """
        args = b3_bridge_args(pins_path) + list(extra_args)
        if not self.nodes:
            self.add_nodes(self.num_nodes, [list(args) for _ in range(self.num_nodes)])
        for i in range(self.bridge_validator_count):
            self.start_node(i, extra_args=args)
            if self.uses_wallet:
                self.init_wallet(node=i)
        for i in range(self.bridge_validator_count - 1):
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
                raw = source.getrawtransaction(txid)
                depends = set(source.getmempoolentry(txid)["depends"])
            except JSONRPCException:
                if txid in source.getrawmempool():
                    raise
                continue
            # Record a transaction only once both reads succeeded: one mined
            # between them must not be left without its dependencies.
            transactions[txid] = raw
            dependencies[txid] = depends
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

    def bring_up_validators(self, target_height, stakes=VALIDATOR_STAKES, timeout=600, finality_key_label=None):
        """Corridor, four stakes with finality keys, then live real-time PoS.

        The corridor runs under mock time from now-300 so its 1 s blocks stay
        behind the wall clock. From M-1 the nodes return to the real clock
        and stake continuously (feature_b3_finality_soak.py:120-152), so BLS
        finality is live. Returns once n0 is at or above target_height and
        every validator has finalized at least M+5. A test that overrides
        bridge_validator_count passes one stake per validator (len(stakes)
        validators); -b3minfinalityset stays its own choice.

        With finality_key_label the validators bind the public test keys of
        that label instead of their derived keys (import_test_finality_keys),
        so b3-bridge-regtest-certify can sign as B3's own committees.
        """
        validators = self.bridge_validators()
        assert_equal(len(validators), len(stakes))
        assert all(node.running for node in validators)
        assert target_height >= MODERN_START
        n0 = validators[0]
        for node in validators:
            assert_regtest_genesis(node)
        self.pos_running = False
        self.set_chain_time(int(time.time()) - 300)

        self.log.info(f"Corridor funding and {len(stakes)} independent finality stakes")
        self.mine_corridor(101)
        for node in validators[1:]:
            n0.sendtoaddress(node.get_deterministic_priv_key().address, 120)
        self.synchronize_mempools()
        self.mine_corridor(1)
        if finality_key_label is None:
            for node, amount in zip(validators, stakes):
                node.createstake(amount)
                node.bindfinalitykey()
        else:
            self.import_test_finality_keys(validators, stakes, finality_key_label)
        self.synchronize_mempools()
        self.mine_corridor(1)
        self.mine_corridor(CORRIDOR_END - n0.getblockcount())
        assert_equal(n0.getblockcount(), CORRIDOR_END)
        for node in validators:
            self.wait_until(lambda n=node: n.getstakinginfo()["active"] > Decimal("0"),
                            timeout=60)
            preview = node.getfinalitystatus()["set0_preview"]
            assert_equal(preview["ready"], True)
            assert_equal(preview["size"], len(stakes))
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

    def import_test_finality_keys(self, validators, stakes, label):
        """Stake on every validator, then bind test keys 0..n-1 of `label`.

        REGTEST ONLY: the keys are the public deterministic test keys of
        b3-bridge-regtest-certify (keys --with-secrets), worthless anywhere
        else. B3 orders set members by ascending validator_key bytes
        (node/validator_set.h), so the validator of rank i imports test key i
        (importfinalitykey) before every validator binds (bindfinalitykey):
        member i of every later B3 set is then the tool's member i. The
        transactions are left in the wallets' mempools. Returns and keeps in
        self.finality_test_keys {"label", "members": [{"index", "node",
        "validator_key", "bls_pubkey"}]} in member order.
        """
        staked = [node.createstake(amount) for node, amount in zip(validators, stakes)]
        keys = self.regtest_certify("keys", args=("--label", label, "--count", str(len(validators)),
                                                  "--with-secrets"))["keys"]
        assert_equal([key["index"] for key in keys], list(range(len(validators))))
        ranked = sorted(zip(validators, staked), key=lambda pair: bytes.fromhex(pair[1]["validator_key"]))
        members = []
        for rank, (node, stake) in enumerate(ranked):
            imported = node.importfinalitykey(keys[rank]["blssecret"])
            assert_equal((imported["imported"], hx(imported["bls_pubkey"])), (True, hx(keys[rank]["pubkey"])))
            members.append({"index": rank, "node": node.index, "validator_key": stake["validator_key"],
                            "bls_pubkey": to_0x(hx(keys[rank]["pubkey"]))})
        for member in members:
            bound = self.nodes[member["node"]].bindfinalitykey()
            assert_equal((bound["action"], bound["validator_key"], hx(bound["bls_pubkey"])),
                         ("bind", member["validator_key"], hx(member["bls_pubkey"])))
        self.finality_test_keys = {"label": label, "members": members}
        return self.finality_test_keys

    def bind_fresh_keys(self, wallets, epoch=None):
        """bindfreshfinalitykey on each wallet (a node or a wallet RPC) for
        `epoch` (default: the next block's window) while frozen; REGTEST ONLY
        (-b3freshkeyheight). Each key is generated into that node's fresh key
        store and its binding broadcast from the wallet; the transactions are
        left in the mempools (mine_txids). Returns the results in order.
        """
        results = []
        for wallet in wallets:
            result = wallet.bindfreshfinalitykey() if epoch is None else wallet.bindfreshfinalitykey(epoch)
            if epoch is not None:
                assert_equal(result["epoch"], epoch)
            assert_equal((result["key_source"], result["status"]), ("generated", "UNCONFIRMED"))
            assert "warnings" not in result, result["warnings"]
            assert_equal(len(hx(result["bls_pubkey"])), 48)
            results.append(result)
        return results

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

    # -- bridge V2: certificate tool, light-client head, kind 9 ----------------
    def regtest_certify(self, mode, input=None, args=()):
        """regtest_certify() with this build's tool and a tmpdir workdir."""
        return regtest_certify(self.bridge_tool(REGTEST_CERTIFY_TOOL), mode, input,
                               workdir=os.path.join(self.options.tmpdir, "regtest_certify"), args=args)

    @staticmethod
    def mined_height(node, txid, first_height):
        """Height of txid in node's active chain at or above first_height, or None."""
        for height in range(first_height, node.getblockcount() + 1):
            if txid in node.getblock(node.getblockhash(height))["tx"]:
                return height
        return None

    def mine_txids(self, txids, first_height, timeout=300):
        """Mine txids (already in a mempool) and leave the validators frozen.

        With the real clock (mock_time 0) the validators stake until each
        txid is in a block every running node agrees on. Under mock time
        (set_chain_time) one block at a time is produced with
        mine_pos_blocks_mocktime(), calling the FreshVoteShuttle round in
        between when the test has one (fresh votes gossip meanwhile).
        Returns the containing heights in txid order.
        """
        self.synchronize_mempools()
        if not getattr(self, "mock_time", 0):
            self.resume()
            heights = [self.wait_mined(txid, first_height=first_height, timeout=timeout) for txid in txids]
            self.freeze()
            return heights
        n0, deadline = self.nodes[0], time.time() + timeout
        shuttle = getattr(self, "shuttle_fresh_votes", None)
        while True:
            heights = [self.mined_height(n0, txid, first_height) for txid in txids]
            if all(height is not None for height in heights):
                self.freeze()
                return heights
            if time.time() >= deadline:
                raise AssertionError(f"not mined under mock time within {timeout} s: {txids}")
            self.mine_pos_blocks_mocktime(1, allow_overshoot=True)
            if shuttle is not None:
                shuttle()

    def update_light_client_to_head(self, node=None, label="V2 light client finalizes the anvil head"):
        """Finalize anvil's newest block on the bridge V2 light client.

        Mines one anvil block (the attested successor of the block finalized),
        builds the synthetic sync-committee update with the mock beacon
        (self.beacon over self.anvil) and mines it through node's
        submitbridgecarrier, so its execution anchor, with that block's state
        root, is the newest one a kind 9 observation can name. The update must
        stay in the store's sync-committee period: crossing one needs rotation
        updates, which no bridge V2 test makes yet. Appends {"finalized_block",
        "finalized_hash", "timestamp", "height", "txid"} to
        self.light_client_updates_v2 and returns it; the validators stay frozen.
        """
        node = node or self.nodes[0]
        anvil, beacon = self.anvil, self.beacon
        self.freeze()
        light = node.getbridgev2info()["light_client"]
        assert light["bootstrapped"], "the V2 light client is not bootstrapped"
        anvil.mine(1)
        finalized = anvil.block_number() - 1
        assert finalized > light["finalized_execution_block"], "no new anvil block to finalize"
        assert beacon.slot(finalized) % PERIOD_SLOTS <= PERIOD_SLOTS - 3, "the update would leave its period"
        assert_equal(beacon.period_of_block(finalized), light["period"])
        header = anvil.block(finalized)
        update = beacon.update(finalized)
        assert_equal((update["finalized_block"], hx(update["finalized_block_hash"])),
                     (finalized, hx(header["hash"])))
        first_height = node.getblockcount() + 1
        submitted = node.submitbridgecarrier(update["payload_hex"])
        assert_equal(submitted["record_kind"], "update")
        height, = self.mine_txids([submitted["txid"]], first_height)
        for running in self.bridge_nodes():
            lc = running.getbridgev2info()["light_client"]
            assert_equal((lc["finalized_execution_block"], hx(lc["finalized_execution_hash"])),
                         (finalized, hx(header["hash"])))
            assert_equal(lc["connected_height"], height)
        record = {"finalized_block": finalized, "finalized_hash": header["hash"],
                  "timestamp": int(header["timestamp"], 16), "height": height, "txid": submitted["txid"]}
        if not hasattr(self, "light_client_updates_v2"):
            self.light_client_updates_v2 = []
        self.light_client_updates_v2.append(record)
        self.evidence_step(label, anvil, **record)
        return record

    def submit_observations_v2(self, node=None, anvil=None, subjects=(1, 2, 3), wallet=None,
                               label="kind 9 observations mined"):
        """Run contrib/b3bridge/b3_bridge_observe_v2.py once and mine its carriers.

        The observer proves each subject at observation_plan's anchor (the
        newest retained V2 anchor with a state root) against anvil and
        submits the records from node's wallet. Returns its output with each
        observation's mined_height; the validators stay frozen.
        """
        node = node or self.nodes[0]
        anvil = anvil or self.anvil
        self.freeze()
        first_height = node.getblockcount() + 1
        output = run_observer_once(node, wallet or self.default_wallet_name, _anvil_url(anvil), subjects)
        txids = [observation["txid"] for observation in output["observations"]]
        for observation, height in zip(output["observations"], self.mine_txids(txids, first_height)):
            observation["mined_height"] = height
        self.evidence_step(label, anvil, anchor=output["anchor"],
                           observations=[{key: observation[key] for key in
                                          ("name", "verdict", "txid", "mined_height", "payload_bytes")}
                                         for observation in output["observations"]])
        return output


# -- B-37 fresh key store (src/node/finality_fresh_key_store.h) ---------------
FRESH_KEY_STORE_MAGIC = b"B3FKEYS1"
FRESH_KEY_STORE_VERSION = 1
FRESH_KEY_STORE_HEADER_SIZE = 8 + 1 + 32 + 32 + 4
FRESH_KEY_STORE_RECORD_SIZE = 8 + 1 + 48 + 32 + 32
FRESH_KEY_STORE_CHECKSUM_SIZE = 32
FRESH_KEY_STORE_MAX_RECORDS = 4096
FRESH_KEY_STATES = {1: "held", 2: "erased"}


class FreshKeyStoreError(AssertionError):
    pass


def parse_fresh_key_store(data):
    """Decode one fresh key store file independently of the node.

    Layout (FreshKeyStore, integers little-endian): magic "B3FKEYS1",
    version 1, chain domain (uint256 bytes), x-only validator key, u32 count,
    count x {u64 target epoch, u8 state (1 held, 2 erased), 48-byte BLS
    public key, 32-byte secret (all zero once erased), bind txid (uint256
    bytes, null until recorded)}, then SHA256 of every preceding byte.
    Returns {"chain_domain": bytes, "validator_key": hex, "records": [...]}
    with each record {"epoch", "state", "bls_pubkey" (hex), "secret"
    (bytes), "bind_txid" (display hex or None)}; raises FreshKeyStoreError
    on anything that does not verify.
    """
    def refuse(reason):
        raise FreshKeyStoreError(f"fresh key store: {reason}")

    minimum = FRESH_KEY_STORE_HEADER_SIZE + FRESH_KEY_STORE_CHECKSUM_SIZE
    if len(data) < minimum:
        refuse(f"{len(data)} bytes, at least {minimum} expected")
    if data[:8] != FRESH_KEY_STORE_MAGIC:
        refuse("bad magic")
    if data[8] != FRESH_KEY_STORE_VERSION:
        refuse(f"unknown version {data[8]}")
    if hashlib.sha256(data[:-FRESH_KEY_STORE_CHECKSUM_SIZE]).digest() != data[-FRESH_KEY_STORE_CHECKSUM_SIZE:]:
        refuse("checksum mismatch")
    count = int.from_bytes(data[73:77], "little")
    if count > FRESH_KEY_STORE_MAX_RECORDS:
        refuse(f"{count} records, maximum {FRESH_KEY_STORE_MAX_RECORDS}")
    if len(data) != minimum + count * FRESH_KEY_STORE_RECORD_SIZE:
        refuse(f"{len(data)} bytes do not hold {count} records")
    records, seen = [], set()
    for i in range(count):
        offset = FRESH_KEY_STORE_HEADER_SIZE + i * FRESH_KEY_STORE_RECORD_SIZE
        record = data[offset:offset + FRESH_KEY_STORE_RECORD_SIZE]
        state = FRESH_KEY_STATES.get(record[8])
        pubkey, secret, txid = record[9:57], record[57:89], record[89:121]
        if state is None:
            refuse(f"record {i}: unknown state {record[8]}")
        if (state == "erased") == any(secret):
            refuse(f"record {i}: {state} with {'a' if any(secret) else 'no'} secret")
        if pubkey in seen:
            refuse(f"record {i}: duplicate public key")
        seen.add(pubkey)
        records.append({"epoch": int.from_bytes(record[0:8], "little"), "state": state,
                        "bls_pubkey": pubkey.hex(), "secret": secret,
                        "bind_txid": txid[::-1].hex() if any(txid) else None})
    return {"chain_domain": data[9:41], "validator_key": data[41:73].hex(), "records": records}


def read_fresh_key_store(path):
    """parse_fresh_key_store() of the file at path, plus its raw bytes."""
    with open(path, "rb") as f:
        data = f.read()
    store = parse_fresh_key_store(data)
    store["raw"] = data
    return store


class FreshVoteShuttle:
    """Fresh-lineage (digest V2) vote transport for a B3BridgeChainMixin test.

    Fresh votes travel over the lineage-naming P2P message finsigv2
    (net_processing.cpp, only on a chain with a fresh lineage): a node floods
    a vote once on first acceptance and replays its verified pool to every
    connection, so the line n0-n1-n2-n3 of start_bridge_validators() reaches
    every validator. With fresh_vote_transport "p2p" (the default) a round
    of shuttle_fresh_votes() calls no vote RPC: shuttle_until() and
    shuttle_fresh_until_lag() only wait while the nodes gossip. "rpc" adds
    the former relay through the hidden vote RPCs (getfinalitylineagevotes /
    submitfinalitylineagevote) for drills or a phase whose validators are
    not linked; set self.lineage_id (getfinalitylineage) first. Every
    submission is counted in self.fresh_vote_results, which stays empty
    with gossip (assert_fresh_gossip). Each round also records n0's
    getfinalitylineage().finalized.certified_at in self.fresh_inclusions;
    fresh_certificates() reads every type-11 carrier from the coinbases, so
    no certificate included between two polls is missed when relaying.
    """

    FRESH_VOTE_SIZE = 148
    FRESH_VOTE_MESSAGE = "finsigv2"
    FRESH_VOTE_TRANSPORTS = ("p2p", "rpc")
    MPA_TYPE_FINALITY_CERTIFICATE_V2 = 11
    fresh_vote_transport = "p2p"

    def _fresh_shuttle_state(self):
        if not hasattr(self, "_fresh_delivered"):
            self._fresh_delivered = set()
            self.fresh_vote_results = Counter()
            self.fresh_inclusions = set()
            self._fresh_carriers = {}
        return self._fresh_delivered

    def shuttle_fresh_votes(self):
        """One round: relay votes by RPC if fresh_vote_transport is "rpc"
        (gossip needs nothing), then record n0's newest fresh inclusion."""
        self._fresh_shuttle_state()
        assert self.fresh_vote_transport in self.FRESH_VOTE_TRANSPORTS, self.fresh_vote_transport
        if self.fresh_vote_transport == "rpc":
            self.relay_fresh_votes_by_rpc()
        try:
            certified_at = (self.nodes[0].getfinalitylineage().get("finalized") or {}).get("certified_at")
        except JSONRPCException:
            certified_at = None
        if certified_at is not None:
            self.fresh_inclusions.add(certified_at)
        return True

    def relay_fresh_votes_by_rpc(self):
        """Relay every verified fresh vote of each validator to the others
        with the hidden vote RPCs (drills only; gossip does this unasked)."""
        delivered = self._fresh_shuttle_state()
        assert getattr(self, "lineage_id", None), "set self.lineage_id before shuttling"
        validators = [node for node in self.bridge_validators() if node.running]
        for source in validators:
            try:
                votes = source.getfinalitylineagevotes()
            except JSONRPCException as e:
                self.log.debug(f"getfinalitylineagevotes on node{source.index}: {e.error}")
                continue
            for vote in votes:
                assert_equal(vote["lineage_id"], self.lineage_id)
                assert_equal(len(hx(vote["hex"])), self.FRESH_VOTE_SIZE)
                for target in validators:
                    key = (target.index, vote["hex"])
                    if target is source or key in delivered:
                        continue
                    try:
                        result = target.submitfinalitylineagevote(vote["hex"])["result"]
                    except JSONRPCException as e:
                        self.log.debug(f"submitfinalitylineagevote on node{target.index}: {e.error}")
                        result = "rpc-error"
                    self.fresh_vote_results[result] += 1
                    if result in ("accepted", "duplicate", "stale"):
                        delivered.add(key)

    @classmethod
    def fresh_gossip_bytes(cls, node):
        """finsigv2 bytes node received, summed over its current connections
        (getpeerinfo counters restart with a node or a connection)."""
        return sum(peer["bytesrecv_per_msg"].get(cls.FRESH_VOTE_MESSAGE, 0) for peer in node.getpeerinfo())

    def assert_fresh_gossip(self, nodes):
        """Every node in `nodes` received finsigv2 traffic from its peers and,
        with the default "p2p" transport, the shuttle submitted no vote by RPC.
        Returns {"nodeN": bytes received}."""
        self._fresh_shuttle_state()
        observed = {f"node{node.index}": self.fresh_gossip_bytes(node) for node in nodes}
        silent = [name for name, received in observed.items() if received <= 0]
        assert not silent, f"no {self.FRESH_VOTE_MESSAGE} received by {silent}: {observed}"
        if self.fresh_vote_transport == "p2p":
            assert_equal(dict(self.fresh_vote_results), {})
        return observed

    def shuttle_until(self, predicate, timeout=300):
        """Poll predicate, one shuttle_fresh_votes() round before each check
        (with gossip only n0's newest inclusion is recorded)."""
        self.wait_until(lambda: self.shuttle_fresh_votes() and predicate(), timeout=timeout,
                        check_interval=0.25)

    @staticmethod
    def fresh_finalized_height(node):
        return (node.getfinalitylineage().get("finalized") or {}).get("height", -1)

    def fresh_finality_lags(self):
        """tip - fresh finalized height of every validator (None: nothing finalized)."""
        lags = []
        for node in self.bridge_validators():
            finalized = self.fresh_finalized_height(node)
            lags.append(node.getblockcount() - finalized if finalized >= 0 else None)
        return lags

    def shuttle_fresh_until_lag(self, max_lag, timeout=300):
        """Let fresh votes gossip (or relay them, transport "rpc") until every
        validator's tip is at most max_lag blocks above its fresh finalized
        checkpoint; leave them frozen.

        A V2 burn is judged with candidate height - finalized <=
        max_finality_lag_blocks (gate row C3; 30 on profile R), so callers
        leave a margin for the blocks until it is mined. With the real clock
        the validators stake meanwhile; under mock time (set_chain_time) one
        block at a time is produced. Returns the lags.
        """
        def within():
            return all(lag is not None and lag <= max_lag for lag in self.fresh_finality_lags())

        deadline = time.time() + timeout
        while True:
            if getattr(self, "mock_time", 0):
                self.shuttle_fresh_votes()
                if within():
                    self.freeze()
                    break
                self.mine_pos_blocks_mocktime(1, allow_overshoot=True)
            else:
                self.resume()
                self.shuttle_until(within, timeout=max(1, deadline - time.time()))
                self.freeze()
                # A block staked after the last check can raise the lag again.
                if within():
                    break
            if time.time() >= deadline:
                raise AssertionError(f"fresh finality lag above {max_lag}: {self.fresh_finality_lags()}")
        return self.fresh_finality_lags()

    def fresh_certificates(self, node, first, last=None):
        """Every type-11 carrier in blocks first..last (default tip) of node.

        Returns [{"inclusion": height, "inclusion_block": hash, "fb": dict}]
        in height order. Blocks already read are cached by hash, so a
        reorganized block is read again.
        """
        self._fresh_shuttle_state()
        if last is None:
            last = node.getblockcount()
        found = []
        for height in range(first, last + 1):
            block_hash = node.getblockhash(height)
            carriers = self._fresh_carriers.get(block_hash)
            if carriers is None:
                coinbase = node.getblock(block_hash, 2)["tx"][0]
                carriers = [decode_finalized_block_v2(hx(record["payload"])[:FINALIZED_BLOCK_V2_SIZE])
                            for record in coinbase.get("mpa", [])
                            if record["type"] == self.MPA_TYPE_FINALITY_CERTIFICATE_V2]
                assert len(carriers) <= 1, f"block {height} carries {len(carriers)} fresh certificates"
                self._fresh_carriers[block_hash] = carriers
            for fb in carriers:
                found.append({"inclusion": height, "inclusion_block": block_hash, "fb": fb})
        return found


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

    # -- bridge V2 ------------------------------------------------------------
    V2_DEPLOYER = "0x3c44cdddb6a900fa2b585dd299e03d12fa4293bc"  # anvil account 2

    def v2_pin_kwargs(self, **overrides):
        kwargs = {
            "vault": predict_create_address(self.V2_DEPLOYER, 2),
            "token": self.ADDRESSES[1],
            "verifier": predict_create_address(self.V2_DEPLOYER, 1),
            "deployer": self.V2_DEPLOYER,
            "deployment_block": self.DEPLOYMENT_BLOCK,
            "bootstrap_output": self.BOOTSTRAP,
            "activation_height": 281,
            "max_deposit_raw": BRIDGE_V2_DEFAULT_CAP_RAW,
        }
        kwargs.update(overrides)
        return kwargs

    def assert_v2_pins_refused(self, anvil, message, **overrides):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "pins-v2.json")
            with self.assertRaisesRegex(BridgePinError, message):
                write_regtest_bridge_pins_v2(path, anvil, **self.v2_pin_kwargs(**overrides))
            self.assertFalse(os.path.exists(path))

    def test_v2_pin_writer_refusals(self):
        for chain_id in (1, 11_155_111, 17_000):
            self.assert_v2_pins_refused(self.FakeAnvil(chain_id), "is not the regtest origin chain 31337")
        regtest = self.FakeAnvil(REGTEST_ETH_CHAIN_ID)
        self.assert_v2_pins_refused(regtest, "zero or the Ethereum mainnet root", gvr=ETHEREUM_MAINNET_GVR)
        self.assert_v2_pins_refused(regtest, "outside the synthetic 0xb3 namespace", fork_version="05000000")
        self.assert_v2_pins_refused(regtest, "non-zero and pairwise distinct", verifier=self.ADDRESSES[1])
        self.assert_v2_pins_refused(regtest, "non-zero and pairwise distinct", vault="0x" + "00" * 20)
        late = dict(self.BOOTSTRAP, checkpoint_block=self.DEPLOYMENT_BLOCK +
                    MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS + 1)
        self.assert_v2_pins_refused(regtest, "must lie within 20000 blocks", bootstrap_output=late)
        self.assert_v2_pins_refused(regtest, "activation_height", activation_height=0)
        self.assert_v2_pins_refused(regtest, "activation_height", burn_height=280)
        self.assert_v2_pins_refused(regtest, "caps must satisfy", max_deposit_raw=BRIDGE_V2_DEFAULT_CAP_RAW + 1)
        self.assert_v2_pins_refused(regtest, "caps must satisfy", caps={"max_per_epoch": BRIDGE_V2_DEFAULT_CAP_RAW - 1})
        self.assert_v2_pins_refused(regtest, "min_bridge_validators", min_bridge_validators=3)
        self.assert_v2_pins_refused(regtest, "min_bridge_validators", max_bridge_validators=65)
        self.assert_v2_pins_refused(regtest, "non-zero weight", min_bridge_total_weight=0)
        self.assert_v2_pins_refused(regtest, "is not the regtest gate's set expiry",
                                    verifier_max_epoch_lag=GATE_R_SET_EXPIRY - 1)
        for url in ("https://ethereum-rpc.example.invalid", "http://10.0.0.1:8545"):
            with tempfile.TemporaryDirectory() as tmp, \
                    self.assertRaisesRegex(AssertionError, "Refusing non-local endpoint"):
                write_regtest_bridge_pins_v2(os.path.join(tmp, "pins-v2.json"), url, **self.v2_pin_kwargs())

    V2_SUCCESSOR_DEPLOYER = "0x15d34aaf54267db7d7c367839aaf71a00a2c6a65"  # anvil account 4

    def v2_successor(self, **overrides):
        successor = {
            "vault": predict_create_address(self.V2_SUCCESSOR_DEPLOYER, 0),
            "deployer": self.V2_SUCCESSOR_DEPLOYER,
            "asset_id": "0x" + "11" * 32,
            "migration_height": 341,
        }
        successor.update(overrides)
        return successor

    def test_v2_pin_writer_successor_refusals(self):
        regtest = self.FakeAnvil(REGTEST_ETH_CHAIN_ID)

        def refused(message, pin_overrides=None, **successor_overrides):
            self.assert_v2_pins_refused(regtest, message, **(pin_overrides or {}),
                                        successor=self.v2_successor(**successor_overrides))

        vault, token, verifier = (self.v2_pin_kwargs()[key] for key in ("vault", "token", "verifier"))
        for address in (vault, token, verifier, "0x" + "00" * 20):
            refused("successor vault must be non-zero and differ from vault, token and verifier", vault=address)
        refused("must differ from the V2 deployer", deployer=self.V2_DEPLOYER,
                vault=predict_create_address(self.V2_DEPLOYER, 0))
        refused("is not the CREATE address of its deployer", vault=predict_create_address(self.V2_SUCCESSOR_DEPLOYER, 1))
        refused("is not the CREATE address of its deployer", deployer=self.ADDRESSES[0])
        for asset_id in ("0x" + "00" * 32, "0x" + "11" * 20, b"\x01" * 31):
            refused("asset_id must be v's non-zero 32-byte bUSD V2 AssetId", asset_id=asset_id)
        for height in (0, 280, 281, 2**31):
            refused(f"migration_height \\({height}\\) must be above activation_height \\(281\\)",
                    migration_height=height)
        refused("at or above burn_height \\(300\\)", pin_overrides={"burn_height": 300}, migration_height=299)
        refused("origin_deployment_block must be at least 1", origin_deployment_block=0)
        # Keys the node inherits from v are not successor inputs.
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(AssertionError):
            write_regtest_bridge_pins_v2(os.path.join(tmp, "pins-v2.json"), regtest,
                                         **self.v2_pin_kwargs(successor=self.v2_successor(token=token)))

    def test_predict_create_address(self):
        # anvil account 0 deploys at the well-known addresses of nonces 0, 1 and 2.
        sender = "0xf39fd6e51aad88f6f4ce6ab8827279cfffb92266"
        token, vault = self.ADDRESSES[1], self.ADDRESSES[0]
        verifier = self.ADDRESSES[2]
        self.assertEqual([predict_create_address(sender, n) for n in range(3)], [token, verifier, vault])

    def test_v2_helpers_match_shared_vectors(self):
        with open(SRCDIR / "contracts" / "test" / "vectors" / "bridge_v2_vectors.json", encoding="utf8") as f:
            vectors = json.load(f)
        self.assertEqual(WITHDRAWAL_LEAF_V2_TAG, hx(vectors["tags"]["leaf_v2"]))
        self.assertEqual(hashlib.sha256(BRIDGE_V2_ASSET_TAG.encode()).digest(), hx(vectors["tags"]["asset_v2"]))
        for v in vectors["asset_id_v2"]:
            self.assertEqual(asset_id_v2(v["chain_domain"], v["origin_chain_id"], v["vault"], v["token"]),
                             hx(v["asset_id"]))
        for v in vectors["leaf_v2"]:
            args = (v["asset_id"], v["withdrawal_id"], v["chain_id"], v["vault"], v["token"],
                    v["recipient"], v["amount"], v["b3_height"])
            self.assertEqual(leaf_v2_preimage(*args), hx(v["preimage"]))
            self.assertEqual(leaf_v2(*args), hx(v["leaf"]))
        tree = vectors["withdrawal_tree_v2"]
        leaves = [hx(leaf["leaf"]) if isinstance(leaf, dict) else hx(leaf) for leaf in tree["leaves"]]
        self.assertEqual(len(leaves), tree["count"])
        for i, leaf in enumerate(leaves):
            self.assertEqual(leaf, leaf_v2(tree["asset_id"], i, tree["chain_id"], tree["vault"], tree["token"],
                                           tree["recipient"], tree["amount"], tree["b3_height"]))
        self.assertEqual(withdrawal_root(leaves), hx(tree["root"]))
        for path in tree["paths"]:
            root, siblings = withdrawal_root_and_path(leaves, path["index"])
            self.assertEqual(siblings, [hx(s) for s in path["siblings"]])
            self.assertEqual(fold_withdrawal_path(leaves[path["index"]], path["index"], siblings), root)
        self.assertEqual(withdrawal_root([]), EMPTY_WITHDRAWAL_ROOT)
        self.assertNotEqual(EMPTY_WITHDRAWAL_ROOT, bytes(32))

    def test_v2_calldata_shapes(self):
        header = {"epoch": 0, "ruleset_version": 2, "validator_count": 4, "total_weight": 100,
                  "quorum_weight": 67, "aggregate_pubkey": "0x" + "a1" * 48, "members_root": "0x" + "b2" * 32}
        encoded = encode_set_header_tuple(header)
        self.assertEqual(len(encoded), 7 * 32 + 32 + 64)
        self.assertEqual(encoded[5 * 32:6 * 32], enc_uint(7 * 32))
        fb = bytes(range(32)) + (266).to_bytes(8, "big") + b"\x11" * 96 + bytes(8) + (1).to_bytes(8, "big") + \
            b"\x22" * 32 + b"\x00"
        cert = {"encoded": to_0x(fb), "signing_set": header, "successor_set": dict(header, epoch=1),
                "history": {"ancestry_path": ["0x" + "33" * 32] * 32, "leaf0_path": ["0x" + "44" * 32] * 32},
                "proof": "0x" + "55" * 70}
        submit = encode_submit_certificate(cert)
        self.assertEqual(submit[:4], selector(VERIFIER_V2_SUBMIT_SIGNATURE))
        self.assertEqual(submit[4 + 9 * 32:4 + 10 * 32], enc_uint(44 * 32))  # signingSet follows the 44-word head
        init = encode_verifier_v2_initialize(cert)
        self.assertEqual(init[4 + 9 * 32:4 + 10 * 32], enc_uint(45 * 32))
        self.assertEqual(init[4 + 12 * 32:4 + 13 * 32], b"\x44" * 32)  # leaf0Path after three offsets
        release = encode_vault_v2_release(0, self.ADDRESSES[0], 10, 281, [bytes(32)] * 32)
        self.assertEqual(len(release), 4 + 36 * 32)

    def test_v2_bootstrap_attestation_matches_shared_vectors(self):
        """OI-12: the harness's 301-byte K_OF_N message and digest equal the
        contracts' and the C++ tool's (bootstrap_attestation_v2 vectors)."""
        with open(SRCDIR / "contracts" / "test" / "vectors" / "bridge_v2_vectors.json", encoding="utf8") as f:
            vectors = json.load(f)
        self.assertEqual(hashlib.sha256(BRIDGE_BOOTSTRAP_V2_TAG.encode()).digest(),
                         hx(vectors["tags"]["bootstrap_v2"]))
        digests = set()
        for v in vectors["bootstrap_attestation_v2"]:
            args = (v["chain_domain"], v["evm_chain_id"], v["verifier"], v["start_set_hash"], v["fb_encoded"])
            self.assertEqual(bootstrap_attestation_message_v2(*args), hx(v["message"]))
            self.assertEqual(bootstrap_attestation_digest_v2(*args), hx(v["digest"]))
            digests.add(v["digest"])
            # Each bound field changes the digest (no replay across them).
            for i, other in ((1, v["evm_chain_id"] + 1), (2, "0x" + "99" * 20), (3, "0x" + "98" * 32)):
                changed = list(args)
                changed[i] = other
                self.assertNotEqual(bootstrap_attestation_digest_v2(*changed), hx(v["digest"]))
        self.assertEqual(len(digests), len(vectors["bootstrap_attestation_v2"]))
        # The bootstrap tag differs from digest V2's, so neither statement
        # can stand in for the other.
        self.assertNotEqual(vectors["tags"]["bootstrap_v2"], vectors["tags"]["finality_v2"])

    def test_v2_observation_plan_matches_shared_vectors(self):
        with open(SRCDIR / "contracts" / "test" / "vectors" / "bridge_v2_vectors.json", encoding="utf8") as f:
            layout = json.load(f)["storage_layout_v2"]
        self.assertEqual(VERIFIER_V2_OBSERVED_SLOTS, tuple(sorted(layout["verifier"]["slots"].values())))
        self.assertEqual(VAULT_V2_OBSERVED_SLOTS, tuple(sorted(layout["vault"]["slots"].values())))
        token = layout["token"]["slots"]
        self.assertEqual((token["owner_paused"], token["balances"], token["is_black_listed"],
                          token["upgraded_deprecated"]),
                         (TOKEN_SLOT_OWNER_PAUSED, TOKEN_SLOT_BALANCES, TOKEN_SLOT_IS_BLACK_LISTED,
                          TOKEN_SLOT_UPGRADED_DEPRECATED))
        keys = {m["slot"]: (m["key"], hx(m["storage_key"])) for m in layout["mapping_slots"]}
        vault = keys[TOKEN_SLOT_BALANCES][0]
        self.assertEqual(keys[TOKEN_SLOT_IS_BLACK_LISTED][0], vault)
        plan = observation_plan_v2(self.ADDRESSES[2], vault, self.ADDRESSES[1])
        self.assertEqual([len(slots) for _, slots in plan[1]], [5])
        self.assertEqual([len(slots) for _, slots in plan[2]], [3, 1])
        self.assertEqual([len(slots) for _, slots in plan[3]], [3])
        self.assertEqual(plan[2][1][1][0], keys[TOKEN_SLOT_BALANCES][1])
        self.assertEqual(plan[3][0][1][2], keys[TOKEN_SLOT_IS_BLACK_LISTED][1])
        self.assertEqual(GATE_R_ROTATION_AGE_LIMIT, 52_800)

    def test_v2_kofn_initialize_and_verdict_tamper(self):
        header = {"epoch": 0, "ruleset_version": 2, "validator_count": 4, "total_weight": 4,
                  "quorum_weight": 3, "aggregate_pubkey": "0x" + "a1" * 48, "members_root": "0x" + "b2" * 32}
        start = dict(header, total_weight=100, quorum_weight=67, members_root="0x" + "c3" * 32)
        fb = bytes(32) + (266).to_bytes(8, "big") + b"\x11" * 96 + bytes(8) + (1).to_bytes(8, "big") + \
            b"\x22" * 32 + b"\x00"
        leaf0 = ["0x" + "44" * 32] * 32
        cert = {"encoded": to_0x(fb), "signing_set": start, "successor_set": dict(start, epoch=1),
                "history": {"leaf0_path": leaf0}, "proof": "0x" + "55" * 70}
        kofn = encode_verifier_v2_initialize_kofn(fb, header, start, cert["successor_set"], leaf0, "0x" + "66" * 90)
        self.assertEqual(kofn[:4], selector(VERIFIER_V2_INITIALIZE_SIGNATURE))
        head = 4 + 9 * 32
        signing_offset = int.from_bytes(kofn[head:head + 32], "big")
        start_offset = int.from_bytes(kofn[head + 32:head + 64], "big")
        self.assertEqual(kofn[4 + signing_offset:4 + start_offset], encode_set_header_tuple(header))
        # With the certificate's own set as signer it is exactly START_CERTIFICATE's calldata.
        self.assertEqual(encode_verifier_v2_initialize_kofn(fb, start, start, cert["successor_set"], leaf0,
                                                            cert["proof"]),
                         encode_verifier_v2_initialize(cert))
        payload = (bytes([9, 0, 1]) + b"\x77" * 32 + bytes([0, 1]) + b"\x88" * 10).hex()
        tampered = hx(tamper_observation_verdict(payload))
        self.assertEqual(tampered[ETH_OBSERVATION_VERDICT_OFFSET], ETH_OBSERVATION_VERDICT_SETS_MISMATCH)
        self.assertEqual(tampered[:ETH_OBSERVATION_VERDICT_OFFSET], hx(payload)[:ETH_OBSERVATION_VERDICT_OFFSET])
        self.assertEqual(tampered[ETH_OBSERVATION_VERDICT_OFFSET + 1:], hx(payload)[ETH_OBSERVATION_VERDICT_OFFSET + 1:])
        view = gate_view({"burn_gate": {"status": "READY", "now_eff": 5, "closes_at": 9}, "x": [1]})
        self.assertEqual(view, {"burn_gate": {"status": "READY", "closes_at": 9}, "x": [1]})

    def test_v2_conflict_encoders_match_shared_vectors(self):
        """RT4c: FinalizedBlockV2 (bridgeFlags bit 0 included), digest V2,
        history leaves and paths equal bridge_v2_vectors.json; the kind 13
        record and reportConflict calldata have their normative shapes."""
        with open(SRCDIR / "contracts" / "test" / "vectors" / "bridge_v2_vectors.json", encoding="utf8") as f:
            vectors = json.load(f)
        self.assertEqual(hashlib.sha256(FINALITY_V2_DIGEST_TAG.encode()).digest(), hx(vectors["tags"]["finality_v2"]))
        flagged = []
        for v in vectors["finalized_block_v2"]:
            fb = decode_finalized_block_v2(v["encoded"])
            self.assertEqual(encode_finalized_block_v2(fb), hx(v["encoded"]))
            self.assertEqual((fb["height"], fb["epoch"], fb["checkpoint_index"], fb["bridge_flags"]),
                             (v["height"], v["epoch"], v["checkpoint_index"], v["bridge_flags"]))
            self.assertEqual(finality_digest_v2(v["chain_domain"], fb), hx(v["digest_v2"]))
            self.assertEqual(keccak256(encode_finalized_block_v2(fb)), hx(v["cert_id"]))
            if fb["bridge_flags"]:
                flagged.append(fb)
        self.assertEqual([fb["encoded"][184] for fb in flagged], [FINALITY_BRIDGE_FLAG_DEPOSITS_CLOSED])
        # The same statement without the flag is another digest and certId.
        unflagged = dict(flagged[0], bridge_flags=0)
        self.assertNotEqual(keccak256(encode_finalized_block_v2(unflagged)), keccak256(flagged[0]["encoded"]))

        history = vectors["history"]
        leaves = []
        for entry in history["leaves"]:
            self.assertEqual(entry["height"], history["genesis_height"] + entry["index"] * history["interval"])
            leaves.append(history_leaf_v2(entry["height"], entry["block_hash"]))
            self.assertEqual(leaves[-1], hx(entry["leaf"]))
        for count, root in enumerate(history["roots"]):
            self.assertEqual(withdrawal_root(leaves[:count]), hx(root))
        for path in history["paths"]:
            root, siblings = withdrawal_root_and_path(leaves[:path["count"]], path["index"])
            self.assertEqual((root, siblings), (hx(path["root"]), [hx(s) for s in path["siblings"]]))

        # Kind 13: 0d 00 || u16 n || FinalizedBlockV2 || bitmap || aggSig.
        payload = encode_certificate_payload_v2(flagged[0], "0x0f", "0x" + "a5" * 96)
        record = encode_finality_conflict_record(4, payload)
        self.assertEqual((record[:4], len(record)), (bytes.fromhex("0d000004"), 286))
        self.assertEqual(record[4:4 + FINALIZED_BLOCK_V2_SIZE], flagged[0]["encoded"])
        wide = encode_certificate_payload_v2(flagged[0], bytes(8), bytes(96))
        self.assertEqual(len(encode_finality_conflict_record(64, wide)), 293)
        for count, body in ((0, payload), (9, payload), (MAX_FINALITY_SET + 1, payload), (4, payload + b"\x00")):
            with self.assertRaises(AssertionError):
                encode_finality_conflict_record(count, body)

        # reportConflict: fb (9 words), signingSet offset, leafHeight,
        # leafHash, 32 path words, proof offset; then the two tails.
        header = {"epoch": 2, "ruleset_version": 2, "validator_count": 4, "total_weight": 100,
                  "quorum_weight": 67, "aggregate_pubkey": "0x" + "a1" * 48, "members_root": "0x" + "b2" * 32}
        path = ["0x" + "33" * 32] * 32
        report = encode_report_conflict(flagged[0], header, 316, "0x" + "44" * 32, path, "0x" + "55" * 70)
        self.assertEqual(report[:4], selector(VERIFIER_V2_REPORT_CONFLICT_SIGNATURE))
        words = [report[4 + 32 * i:4 + 32 * (i + 1)] for i in range((len(report) - 4) // 32)]
        self.assertEqual(b"".join(words[:9]), fb_v2_abi_words(flagged[0]["encoded"]))
        head_words = 9 + 1 + 1 + 1 + 32 + 1
        self.assertEqual(words[9], enc_uint(head_words * 32))
        self.assertEqual((words[10], words[11]), (enc_uint(316), b"\x44" * 32))
        self.assertEqual(words[12:44], [b"\x33" * 32] * 32)
        set_tuple = encode_set_header_tuple(header)
        self.assertEqual(words[44], enc_uint(head_words * 32 + len(set_tuple)))
        self.assertEqual(report[4 + head_words * 32:4 + head_words * 32 + len(set_tuple)], set_tuple)
        self.assertEqual(report[4 + head_words * 32 + len(set_tuple):], abi_bytes_tail("0x" + "55" * 70))

    def test_v2_migration_helpers(self):
        """RT4d: the kind 16 VAULT_SWEEP record, the successor subjects' plan,
        the B3VaultV2 Config words and gate_view's successor sweep gate."""
        entry_id = b"\x5a" * 31 + b"\x01"
        record = encode_vault_sweep_record("0x" + entry_id.hex())
        self.assertEqual((record[:2], record[2:], len(record)), (bytes([16, 0]), entry_id, 34))
        for bad in ("0x" + "00" * 32, "0x" + "11" * 31, b"\x22" * 33):
            with self.assertRaises(AssertionError):
                encode_vault_sweep_record(bad)

        vault, token, verifier = self.ADDRESSES
        successor = predict_create_address(self.V2_SUCCESSOR_DEPLOYER, 0)
        plain = observation_plan_v2(verifier, vault, token)
        plan = observation_plan_v2(verifier, vault, token, successor_vault=successor)
        self.assertEqual(sorted(plain), [1, 2, 3])
        self.assertEqual(sorted(plan), [1, 2, 3, 0x12, 0x13])
        self.assertEqual({subject: plan[subject] for subject in (1, 2, 3)}, plain)
        # 0x12 / 0x13 are RESERVE_E / TOKEN_FLAGS with v' in place of v.
        self.assertEqual((plan[0x12], plan[0x13]),
                         (observation_plan_v2(verifier, successor, token)[2],
                          observation_plan_v2(verifier, successor, token)[3]))
        self.assertEqual([account for account, _ in plan[0x12]], [successor, normalize_address(token)])
        self.assertEqual(plan[0x13][0][1][2], enc_uint(mapping_slot(enc_address(successor), TOKEN_SLOT_IS_BLACK_LISTED)))
        self.assertEqual(sorted(ETH_OBSERVATION_SUBJECTS_SUCCESSOR), [18, 19])
        self.assertEqual({subject & 0x0f for subject in ETH_OBSERVATION_SUBJECTS_SUCCESSOR}, {2, 3})
        self.assertEqual(sorted(ETH_OBSERVATION_KEYS_SUCCESSOR), sorted(ETH_OBSERVATION_SUBJECTS_SUCCESSOR))

        common = {"verifier": verifier, "verifier_code_hash": b"\x01" * 32, "chain_domain": b"\x02" * 32,
                  "token": token, "token_code_hash": b"\x03" * 32, "forwarder_factory": self.ADDRESSES[0],
                  "max_deposit_raw": BRIDGE_V2_DEFAULT_CAP_RAW}
        launch = encode_vault_v2_config(**common)
        config = encode_vault_v2_config(**common, predecessor=vault, predecessor_asset_id=b"\x04" * 32)
        words = [config[32 * i:32 * (i + 1)] for i in range(len(config) // 32)]
        self.assertEqual(len(config), 9 * 32)
        self.assertEqual(words[:7], [launch[32 * i:32 * (i + 1)] for i in range(7)])
        self.assertEqual(words, [enc_address(verifier), b"\x01" * 32, b"\x02" * 32, enc_address(token), b"\x03" * 32,
                                 enc_address(self.ADDRESSES[0]), enc_uint(BRIDGE_V2_DEFAULT_CAP_RAW),
                                 enc_address(vault), b"\x04" * 32])
        self.assertEqual(launch[7 * 32:], bytes(64))
        with self.assertRaises(AssertionError):
            encode_vault_v2_config(**common, predecessor=vault)

        info = {"burn_gate": {"status": "READY", "now_eff": 5},
                "successor": {"sweep": {"payload_hex": "1000", "gate": {"status": "READY", "now_eff": 7,
                                                                         "closes_at": 9}}}}
        self.assertEqual(gate_view(info), {"burn_gate": {"status": "READY"},
                                           "successor": {"sweep": {"payload_hex": "1000",
                                                                   "gate": {"status": "READY", "closes_at": 9}}}})
        # Before the migration height the sweep carries no gate.
        self.assertEqual(gate_view({"successor": {"sweep": {"payload_hex": "1000"}}}),
                         {"successor": {"sweep": {"payload_hex": "1000"}}})

    def test_fresh_key_store_parser(self):
        """parse_fresh_key_store against a hand-built file and its refusals."""
        domain, validator = bytes(range(32)), bytes(range(32, 64))

        def record(epoch, state, pubkey, secret=bytes(32), txid=bytes(32)):
            return epoch.to_bytes(8, "little") + bytes([state]) + pubkey + secret + txid

        def store(records, count=None, magic=FRESH_KEY_STORE_MAGIC, version=FRESH_KEY_STORE_VERSION):
            body = (magic + bytes([version]) + domain + validator +
                    (len(records) if count is None else count).to_bytes(4, "little") + b"".join(records))
            return body + hashlib.sha256(body).digest()

        held = record(2, 1, b"\xa1" * 48, b"\x5a" * 32, bytes(range(1, 33)))
        erased = record(0, 2, b"\xa2" * 48)
        parsed = parse_fresh_key_store(store([erased, held]))
        self.assertEqual(parsed["chain_domain"], domain)
        self.assertEqual(parsed["validator_key"], validator.hex())
        self.assertEqual(parsed["records"], [
            {"epoch": 0, "state": "erased", "bls_pubkey": "a2" * 48, "secret": bytes(32), "bind_txid": None},
            {"epoch": 2, "state": "held", "bls_pubkey": "a1" * 48, "secret": b"\x5a" * 32,
             "bind_txid": bytes(range(1, 33))[::-1].hex()},
        ])
        self.assertEqual(len(store([held])), FRESH_KEY_STORE_HEADER_SIZE + FRESH_KEY_STORE_RECORD_SIZE + 32)
        self.assertEqual(parse_fresh_key_store(store([]))["records"], [])

        good = store([held])
        for data, reason in (
            (good[:-1], "checksum"),
            (good[:40], "at least"),
            (good[:100] + bytes([good[100] ^ 1]) + good[101:], "checksum"),
            (store([held], magic=b"B3FKEYS2"), "bad magic"),
            (store([held], version=2), "unknown version"),
            (store([held], count=2), "do not hold"),
            (store([], count=FRESH_KEY_STORE_MAX_RECORDS + 1), "maximum"),
            (store([record(1, 3, b"\xa3" * 48, b"\x01" * 32)]), "unknown state"),
            (store([record(1, 2, b"\xa3" * 48, b"\x01" * 32)]), "erased with a secret"),
            (store([record(1, 1, b"\xa3" * 48)]), "held with no secret"),
            (store([held, held]), "duplicate public key"),
        ):
            with self.assertRaisesRegex(FreshKeyStoreError, reason):
                parse_fresh_key_store(data)

    # -- bridge V2 forwarders (routes (b) and (d3)) ---------------------------
    V2_INIT_CODE_HASH = "0x" + "e1" * 32

    def test_v2_forwarder_pin_writer_refusals(self):
        regtest = self.FakeAnvil(REGTEST_ETH_CHAIN_ID)
        pins = forwarder_pins_v2(self.V2_DEPLOYER, self.V2_INIT_CODE_HASH)
        self.assertEqual(pins, {
            "deposit": {"factory": predict_create_address(self.V2_DEPLOYER, 4),
                        "init_code_hash": self.V2_INIT_CODE_HASH},
            "exit": {"factory": predict_create_address(self.V2_DEPLOYER, 5),
                     "init_code_hash": self.V2_INIT_CODE_HASH, "dst_eid": 30110}})
        self.assertEqual(list(forwarder_pins_v2(self.V2_DEPLOYER, self.V2_INIT_CODE_HASH, roles=("exit",))),
                         ["exit"])

        def refused(message, **changes):
            changed = copy.deepcopy(pins)
            for role, fields in changes.items():
                changed[role].update(fields)
            self.assert_v2_pins_refused(regtest, message, forwarders=changed)

        nonce = "is not DeployV2's CREATE address at deployer nonce"
        refused(f"deposit forwarder factory 0x[0-9a-f]{{40}} {nonce} 4",
                deposit={"factory": predict_create_address(self.V2_DEPLOYER, 3)})
        refused(f"exit forwarder factory 0x[0-9a-f]{{40}} {nonce} 5", exit={"factory": pins["deposit"]["factory"]})
        refused(f"deposit forwarder factory 0x[0-9a-f]{{40}} {nonce} 4",
                deposit={"factory": predict_create_address(self.V2_SUCCESSOR_DEPLOYER, 4)})
        refused("deposit forwarder init_code_hash must be", deposit={"init_code_hash": "0x" + "00" * 32})
        refused("exit forwarder init_code_hash must be", exit={"init_code_hash": "0x" + "e1" * 31})
        refused("init_code_hash differ", exit={"init_code_hash": "0x" + "e2" * 32})
        for dst_eid in (LZ_EID_ETHEREUM, 0, 30111):
            refused(f"dst_eid {dst_eid} is not Arbitrum's LayerZero endpoint id 30110", exit={"dst_eid": dst_eid})
        # Missing or unknown keys and roles are harness bugs, refused even without check.
        for bad in ({}, {"other": {}}, {"deposit": {"factory": pins["deposit"]["factory"]}},
                    {"exit": dict(pins["exit"], extra=1)}):
            with tempfile.TemporaryDirectory() as tmp, self.assertRaises(AssertionError):
                write_regtest_bridge_pins_v2(os.path.join(tmp, "pins-v2.json"), regtest,
                                             **self.v2_pin_kwargs(forwarders=bad, check=False))

    def test_v2_forwarder_pins_written(self):
        """The v2.deposit_forwarder / v2.exit_forwarder keys the node parses,
        the anvil code checks, and no forwarder key without forwarders.
        Every request goes to a fake local_json_rpc."""
        from unittest import mock
        kwargs = self.v2_pin_kwargs()
        token = kwargs["token"]
        coded = {token}

        def fake_rpc(url, method, params=None, timeout=120):
            if method == "eth_getBlockByNumber":
                return {"hash": self.BOOTSTRAP["checkpoint_block_hash"]}
            if method == "eth_getTransactionCount":
                return "0x0"
            if method == "eth_getCode":
                return "0x6001" if normalize_address(params[0]) in coded else "0x"
            raise AssertionError(f"unexpected request {method}")

        regtest = self.FakeAnvil(REGTEST_ETH_CHAIN_ID)
        pins = forwarder_pins_v2(self.V2_DEPLOYER, self.V2_INIT_CODE_HASH)
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch("test_framework.b3bridge.local_json_rpc", side_effect=fake_rpc):
            path = os.path.join(tmp, "pins-v2.json")
            plain = write_regtest_bridge_pins_v2(path, regtest, **kwargs)["v2"]
            self.assertFalse({"deposit_forwarder", "exit_forwarder"} & set(plain))
            document = write_regtest_bridge_pins_v2(path, regtest, **kwargs, forwarders=pins)
            with open(path, encoding="utf8") as f:
                self.assertEqual(json.load(f), document)
            v2 = document["v2"]
            self.assertEqual(v2["deposit_forwarder"], pins["deposit"])
            self.assertEqual(v2["exit_forwarder"], pins["exit"])
            self.assertEqual({key: value for key, value in v2.items() if not key.endswith("_forwarder")}, plain)
            only_exit = write_regtest_bridge_pins_v2(path, regtest, **kwargs, forwarders={"exit": pins["exit"]})
            self.assertEqual(sorted(k for k in only_exit["v2"] if k.endswith("_forwarder")), ["exit_forwarder"])
            for role in ("deposit", "exit"):
                coded.add(pins[role]["factory"])
                with self.assertRaisesRegex(BridgePinError, f"the {role} forwarder factory address .* already "
                                                            "holds code"):
                    write_regtest_bridge_pins_v2(path, regtest, **kwargs, forwarders={role: pins[role]})
                coded.discard(pins[role]["factory"])

    def test_v2_sponsored_sighash_pin_written(self):
        """Owner ruling R10's v2.sponsored_sighash_height: absent by
        default, written by the writer or added by pin_sponsored_sighash_v2
        (the same bytes either way), never below V."""
        from unittest import mock
        kwargs = self.v2_pin_kwargs()
        token = kwargs["token"]

        def fake_rpc(url, method, params=None, timeout=120):
            if method == "eth_getBlockByNumber":
                return {"hash": self.BOOTSTRAP["checkpoint_block_hash"]}
            if method == "eth_getTransactionCount":
                return "0x0"
            if method == "eth_getCode":
                return "0x6001" if normalize_address(params[0]) == token else "0x"
            raise AssertionError(f"unexpected request {method}")

        regtest = self.FakeAnvil(REGTEST_ETH_CHAIN_ID)
        activation = kwargs["activation_height"]
        with tempfile.TemporaryDirectory() as tmp, \
                mock.patch("test_framework.b3bridge.local_json_rpc", side_effect=fake_rpc):
            path = os.path.join(tmp, "pins-v2.json")
            plain = write_regtest_bridge_pins_v2(path, regtest, **kwargs)
            self.assertNotIn("sponsored_sighash_height", plain["v2"])
            added = pin_sponsored_sighash_v2(path, activation + 3)
            with open(path, encoding="utf8") as f:
                added_bytes = f.read()
            self.assertEqual(added["v2"], {**plain["v2"], "sponsored_sighash_height": activation + 3})
            written = write_regtest_bridge_pins_v2(path, regtest, **kwargs, sponsored_sighash_height=activation + 3)
            with open(path, encoding="utf8") as f:
                self.assertEqual(f.read(), added_bytes)
            self.assertEqual(written, added)
            with self.assertRaisesRegex(BridgePinError, "sponsored_sighash_height"):
                write_regtest_bridge_pins_v2(path, regtest, **kwargs, sponsored_sighash_height=activation - 1)
            with self.assertRaisesRegex(BridgePinError, "below activation_height"):
                pin_sponsored_sighash_v2(path, activation - 1)

    def test_v2_forwarder_helpers_match_shared_vectors(self):
        """The contrib derivations the harness imports, its refund EOA
        message, EIP-191 digest and independent ecrecover reproduce the
        forwarder_v2 vectors (the C++ and Foundry suites read the same file)."""
        with open(SRCDIR / "contracts" / "test" / "vectors" / "bridge_v2_vectors.json", encoding="utf8") as f:
            vectors = json.load(f)["forwarder_v2"]
        self.assertEqual(forwarders_v2.DEPOSIT_TAG, hx(vectors["tags"]["deposit"]))
        self.assertEqual(forwarders_v2.EXIT_TAG, hx(vectors["tags"]["exit"]))
        self.assertEqual(vectors["pinned_dst_eid"], LZ_EID_ARBITRUM)
        init_code_hash = vectors["init_code_hash"]
        for v in vectors["deposit"]:
            p = DepositParams(v["vault"], v["b3_recipient"], v["refund_eoa"], v["fee_cap"], v["not_after"])
            self.assertEqual(forwarders_v2.deposit_salt_preimage(p), hx(v["salt_preimage"]))
            self.assertEqual(forwarders_v2.deposit_salt(p), hx(v["salt"]))
            self.assertEqual(deposit_forwarder_address(v["factory"], init_code_hash, p), hx(v["account"]))
        for v in vectors["exit"]:
            p = ExitParams(v["dst_eid"], v["to"], v["max_bridge_fee_bps"], v["fee_cap"], v["refund_eoa"])
            self.assertEqual(forwarders_v2.exit_salt_preimage(p), hx(v["salt_preimage"]))
            self.assertEqual(forwarders_v2.exit_salt(p), hx(v["salt"]))
            self.assertEqual(exit_forwarder_address(v["factory"], init_code_hash, p), hx(v["account"]))
            self.assertEqual(forwarders_v2.exit_params_valid(p, vectors["pinned_dst_eid"]), v["is_valid"])
        for v in vectors["deposit_fee_cap"]:
            self.assertEqual(deposit_fee_cap(v["fee_per_tx"]), v["fee_cap"])

        names = []
        for v in vectors["refund_eoa_message"]:
            names.append(v["name"])
            message = refund_eoa_message(v["chain_domain"], v["refund_eoa"])
            self.assertEqual(message, v["message"])
            self.assertEqual(eip191_digest(message), hx(v["eip191_digest"]))
            self.assertEqual(recover_eip191_signer(message, v["signature"]), normalize_address(v["refund_eoa"]))
            sig = hx(v["signature"])
            self.assertEqual(recover_eip191_signer(message, sig[:64] + bytes([sig[64] - 27])),
                             normalize_address(v["refund_eoa"]))
            # Another message, the other parity or another chain recovers someone else.
            other_parity = sig[:64] + bytes([55 - sig[64]])
            for text, signature in ((message + "\n", sig), (message, other_parity),
                                    (refund_eoa_message(bytes(32), v["refund_eoa"]), sig)):
                self.assertNotEqual(recover_eip191_signer(text, signature), normalize_address(v["refund_eoa"]))
            for bad in (sig[:64], sig + b"\x00", sig[:64] + b"\x1d", bytes(32) + sig[32:], sig[:32] + bytes(32) +
                        sig[64:], b"\xff" * 32 + sig[32:]):
                self.assertIsNone(recover_eip191_signer(message, bad))
            if v["name"] == "regtest":
                self.assertEqual(hx(v["chain_domain"]), regtest_chain_domain())
        self.assertIn("regtest", names)
        self.assertEqual(refund_eoa_message("0x" + "ab" * 32, "0x" + "CD" * 20),
                         "B3 bridge V2 refund EOA\nchain domain: 0x" + "ab" * 32 + "\nrefund EOA: 0x" + "cd" * 20)

    def test_v2_oft_calldata(self):
        to = "0x" + "ab" * 20
        sp = oft_send_param(LZ_EID_ETHEREUM, to, 25_000_000, extra_options=b"\x00\x03")
        self.assertEqual(sp, (30101, bytes(12) + b"\xab" * 20, 25_000_000, 25_000_000, b"\x00\x03", b"", b""))
        data = encode_oft_send(sp, 10**14, "0x" + "cd" * 20)
        self.assertEqual(data[:4].hex(), "c7c7f5b3")  # LayerZero IOFT send
        self.assertEqual(data, encode_oft_send({"dst_eid": LZ_EID_ETHEREUM, "to": bytes(12) + b"\xab" * 20,
                                                "amount_ld": 25_000_000, "extra_options": "0x0003"},
                                               (10**14, 0), "0x" + "cd" * 20))
        # Head: SendParam offset, fee (2 words), refund; then SendParam's 7 head
        # words and its three bytes tails (1 + 1 words, 1 word, 1 word).
        self.assertEqual(len(data), 4 + 32 * (4 + 7 + 2 + 1 + 1))
        self.assertEqual(data[4:36], enc_uint(4 * 32))
        decoded = forwarders_v2.decode_abi([OFT_SEND_PARAM_TUPLE, OFT_MESSAGING_FEE_TUPLE, "address"], data[4:])
        self.assertEqual(decoded, [sp, (10**14, 0), "0x" + "cd" * 20])
        slippage = oft_send_param(LZ_EID_ARBITRUM, "0x" + "11" * 32, 1_000, min_amount_ld=995)
        self.assertEqual(slippage[3], forwarders_v2.min_amount_after_fee(1_000, 50))
        self.assertEqual(encode_oft_quote_send(sp)[:4].hex(), "3b6f743b")  # quoteSend
        self.assertEqual(decode_oft_message(b"\x22" * 32 + (7).to_bytes(8, "big")), (b"\x22" * 32, 7))
        for bad_to in ("0x" + "ab" * 19, b"\x01" * 31):
            with self.assertRaises(AssertionError):
                oft_send_param(LZ_EID_ETHEREUM, bad_to, 1)

    def test_v2_forwarder_deploy_and_tool_guards(self):
        """DeployV2's forwarder flag and nonces, and the tool wrappers'
        refusals, all before any process starts."""
        self.assertEqual(deploy_v2_contracts({}), DEPLOY_V2_CONTRACTS)
        self.assertEqual(deploy_v2_contracts({"B3V2_DEPLOY_FORWARDERS": "0"}), DEPLOY_V2_CONTRACTS)
        self.assertEqual(deploy_v2_contracts({"B3V2_DEPLOY_FORWARDERS": 1}),
                         DEPLOY_V2_CONTRACTS + ("B3DepositForwarderFactory", "B3OftExitFactory"))
        for flag in ("2", "yes", ""):
            with self.assertRaises(AssertionError):
                deploy_v2_contracts({"B3V2_DEPLOY_FORWARDERS": flag})
        # The harness's nonces and endpoint id are DeployV2's.
        self.assertEqual((FORWARDER_FACTORY_NONCE, EXIT_FACTORY_NONCE, LZ_EID_ARBITRUM), (4, 5, 30110))
        with open(SRCDIR / "contracts" / "script" / "DeployV2.s.sol", encoding="utf8") as f:
            script = f.read()
        for constant in ("FORWARDER_FACTORY_NONCE_OFFSET = 4;", "EXIT_FORWARDER_FACTORY_NONCE_OFFSET = 5;",
                         "DEFAULT_EXIT_DST_EID = 30_110;"):
            self.assertIn(constant, script)
        self.assertEqual(forwarder_init_code_hash({"bytecode": b"\x60\x00"}), keccak256(b"\x60\x00"))
        self.assertEqual(forwarder_init_code_hash(b"\x60\x00"), keccak256(b"\x60\x00"))

        self.assertEqual(_tool_arguments({"sender": "0x" + "aa" * 20, "fee_recipient": "0x" + "bb" * 20,
                                          "params": {"vault": "0x1", "fee_cap": 2}, "fee": 5, "account": None,
                                          "certificate_block": 300, "tool_timeout": 30, "salt": b"\x01\x02"}),
                         ["--from", "0x" + "aa" * 20, "--fee-recipient", "0x" + "bb" * 20,
                          "--params", '{"fee_cap": 2, "vault": "0x1"}', "--fee", "5",
                          "--certificate-block", "300", "--timeout", "30", "--salt", "0x0102"])
        local = "http://127.0.0.1:9"
        for command, kwargs in (("published", {"node": object()}), ("flush", {}), ("forward", {}),
                                ("deploy", {})):
            with self.assertRaises(AssertionError):
                run_forwarder_tool(command, local, **kwargs)
        for url in ("https://ethereum-rpc.example.invalid", "http://10.0.0.1:8545"):
            with self.assertRaisesRegex(AssertionError, "Refusing non-local endpoint"):
                run_forwarder_tool("published", url, factory="0x" + "aa" * 20)
            with self.assertRaisesRegex(AssertionError, "Refusing non-local endpoint"):
                run_mock_lz_relayer(url, "0x" + "aa" * 20, local, "0x" + "bb" * 20, "0x" + "cc" * 20)
            with self.assertRaisesRegex(AssertionError, "Refusing non-local endpoint"):
                run_mock_lz_relayer(local, "0x" + "aa" * 20, url, "0x" + "bb" * 20, "0x" + "cc" * 20)
            with self.assertRaisesRegex(AssertionError, "Refusing non-local endpoint"):
                personal_sign(url, "0x" + "aa" * 20, "m")
            with self.assertRaisesRegex(AssertionError, "Refusing non-local endpoint"):
                forwarder_eth_rpc(url)
        self.assertEqual((ARBITRUM_REGTEST_CHAIN_ID, LZ_EID_ETHEREUM, LZ_EID_ARBITRUM), (31338, 30101, 30110))
