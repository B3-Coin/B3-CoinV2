#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Fresh finality lineage (digest V2) on regtest: bridge V2 stage S3, fixture I.

REGTEST ONLY. Every process is local: four B3 regtest validators linked only
to each other and a fifth node that syncs from them. No anvil, no Ethereum
endpoint and no external peer is used. The fixture is regtest-plan §3.6
fixture I (V1 intact) without its pin file: the pins need an anvil
deployment, so the bridge regime stays inactive and withdrawalRoot is zero.

With -b3freshlineageheight=262 (R_f; GENESIS_HEIGHT 261 = 131 + 26 * 5 is on
the V1 grid) the V1 single-vote signer also signs B3/FINALITY/V2 for the
fresh lineage (staging signer, B-7, V1 FINALITY_KEY binding keys; per-epoch
keys B-37 are out of this slice). The test checks:

1. n4 refuses the knob without -b3flowmeshtest, off the checkpoint grid,
   the extension without the height and the negated knob.
2. Before R_f getfinalitylineage reports the configuration only.
3. Corridor, four stakes with finality keys and live V1 BLS finality.
4. Set'_0 = Snapshot(261) under ruleset 2 and the lineage id are identical
   on every validator and equal an independent Python derivation
   (TaggedHash over chainDomain || u64 261 || blockHash(261) || setHash).
5. Fresh votes travel only over the P2P message finsigv2 (flood on first
   acceptance, replay of each node's verified pool) along the line
   n0-n1-n2-n3, without any vote RPC, until a V2 certificate is included
   and finalized (and, since V1 and V2 votes gossip unordered, until V1 has
   finalized the same checkpoint). Every validator received finsigv2
   traffic. getfinalitylineage's node-local local_signer shows every staging
   signer healthy. Hidden-RPC drill, frozen with votes pending: once gossip
   settles getfinalitylineagevotes exports the same votes on every
   validator, each field equal to its 148 serialized bytes, and
   submitfinalitylineagevote of any of them is a duplicate everywhere
   (gossip already pooled it); a vote with a flipped signature byte is
   bad-signature, one naming another lineage id lineage-mismatch, and
   neither changes any export.
6. getfinalitylineagecertificate is identical on every node, and Python
   re-derives every byte: the 185-byte FinalizedBlockV2, the history root
   from getblockhash leaves, both history paths, digestV2, certId, both
   ruleset-2 set headers, the coinbase record and the Verifier V2 calldata.
7. V1 keeps certifying above R_f and its epoch advances.
8. The fresh lineage rotates to epoch 1 at 302 after its handover and keeps
   certifying with Set'_1; the second export places the previous checkpoint
   with a non-genesis ancestry path.
9. A restart and a -reindex restart of n3, and n4 syncing from genesis,
   reproduce the lineage id, epoch, sets and finalized checkpoint.
10. Every validator holds a V1 journal and a separate V2 lineage journal.
    With staking resumed, n4 (linked to n0 only, never a validator) and every
    validator, n3 after its restarts included, receive finsigv2 traffic on
    their current connections.
"""

from collections import Counter
import hashlib
import os

from test_framework.b3bridge import (
    B3_BRIDGE_ARGS,
    CHECKPOINT_INTERVAL,
    EPOCH_LENGTH,
    MODERN_START,
    VALIDATOR_COUNT,
    B3BridgeChainMixin,
    FreshVoteShuttle,
    hx,
    keccak256,
    regtest_chain_domain,
    rpc_refusal,
    selector,
    start_refused,
    to_0x,
)
from test_framework.key import TaggedHash
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
)

# -- fresh lineage schedule (regtest plan §3.3, §3.6) -----------------------
FRESH_ACTIVATION = 262  # R_f
FRESH_GENESIS = FRESH_ACTIVATION - 1  # GENESIS_HEIGHT, history leaf 0
FRESH_EXTENSION = 1280  # profile R
FRESH_ARGS = [
    f"-b3freshlineageheight={FRESH_ACTIVATION}",
    f"-b3freshmaxepochextension={FRESH_EXTENSION}",
]
CHECKPOINT_DEPTH = 3  # -b3checkpointdepth in B3_BRIDGE_ARGS
FIRST_FRESH_CHECKPOINT = FRESH_GENESIS + CHECKPOINT_INTERVAL  # index 1
FRESH_EPOCH1_START = FRESH_ACTIVATION + EPOCH_LENGTH
assert (FRESH_GENESIS - MODERN_START) % CHECKPOINT_INTERVAL == 0
assert "-b3checkpointdepth=3" in B3_BRIDGE_ARGS

# -- bytes (contracts spec §2) -----------------------------------------------
FINALITY_V2_TAG = "B3/FINALITY/V2"
LINEAGE_V2_TAG = "B3/FINALITY/LINEAGE/V2"
RULESET_V1 = 1
RULESET_V2 = 2
SET_HEADER_SIZE = 110
FB_V2_SIZE = 185
BLS_SIGNATURE_SIZE = 96
VOTE_SIZE = 148
HISTORY_DEPTH = 32
MPA_TYPE_FINALITY_CERTIFICATE = 4
MPA_TYPE_FINALITY_CERTIFICATE_V2 = 11
# B3FinalityVerifierV2.submitCertificate (contracts spec §3.3,
# contracts/src/v2/IB3FinalityVerifierV2.sol).
SUBMIT_CERTIFICATE_V2 = (
    "submitCertificate((bytes32,uint64,bytes32,bytes32,bytes32,uint64,uint64,bytes32,uint8),"
    "(uint64,uint16,uint32,uint64,uint64,bytes,bytes32),"
    "(uint64,uint16,uint32,uint64,uint64,bytes,bytes32),bytes32[32],bytes)")
CALLDATA_HEAD_WORDS = 9 + 2 + HISTORY_DEPTH + 1

# Journals (src/node/finality_signer_store.cpp).
V1_JOURNAL_MAGIC = b"B3FSLOCK"
V2_JOURNAL_MAGIC = b"B3FSLIN2"
JOURNAL_SIZE = 8 + 1 + 32 + 32 + 4 + 32 + 32 + 4 + 32 + 32 + 8 + 32 + 32 + 32

RPC_MISC_ERROR = -1
RPC_DESERIALIZATION_ERROR = -22

# -- refusal messages (src/chainparams.cpp, src/kernel/chainparams.cpp) ------
REQUIRES_FLOWMESH = "-b3freshlineageheight requires -b3modernregtest and -b3flowmeshtest (regtest-only)"
OFF_GRID = (f"-b3freshlineageheight={FRESH_ACTIVATION + 1}: genesis checkpoint {FRESH_ACTIVATION} "
            f"must be a checkpoint height >= M={MODERN_START} on the {CHECKPOINT_INTERVAL}-block grid")
EXTENSION_ALONE = "-b3freshmaxepochextension requires -b3freshlineageheight"
NEGATED = "Negating of -b3freshlineageheight is meaningless and therefore forbidden"

# Fields of getfinalitylineage that every node must agree on (the local
# vote pool size is node-local and left out).
LINEAGE_VIEW_KEYS = (
    "configured", "staging_signer", "activation_height", "genesis_height",
    "max_epoch_extension", "checkpoint_interval", "chain_domain", "tip_height",
    "state_available", "bootstrapped", "genesis_block_hash", "genesis_block_hash_evm",
    "genesis_leaf", "lineage_id", "genesis_set_hash", "epoch", "epoch_starts",
    "handover_certified", "lineage_broken", "break_height", "genesis_set",
    "previous_set", "current_set", "next_set", "finalized", "history",
)


# -- independent byte derivations ---------------------------------------------
def be64(value):
    return value.to_bytes(8, "big")


def word_uint(value):
    return value.to_bytes(32, "big")


def raw_block_hash(node, height):
    """The block hash as B3 signs it (internal byte order)."""
    return bytes.fromhex(node.getblockhash(height))[::-1]


def history_leaf(height, raw_hash):
    return keccak256(be64(height) + raw_hash)


ZERO_HASHES = [bytes(32)]
for _ in range(HISTORY_DEPTH):
    ZERO_HASHES.append(keccak256(ZERO_HASHES[-1] + ZERO_HASHES[-1]))


def history_root_and_path(leaves, index=None):
    """Depth-32 ordered tree with zero-subtree padding (as withdrawals).

    Returns the root and, when index is given, the 32 siblings of that leaf
    from the leaf level up.
    """
    assert leaves, "the history tree always holds leaf 0"
    assert index is None or 0 <= index < len(leaves)
    level, path, position = list(leaves), [], index
    for depth in range(HISTORY_DEPTH):
        if len(level) % 2:
            level.append(ZERO_HASHES[depth])
        if position is not None:
            path.append(level[position ^ 1])
            position >>= 1
        level = [keccak256(level[i] + level[i + 1]) for i in range(0, len(level), 2)]
    assert_equal(len(level), 1)
    return level[0], path


def fold_history_path(leaf, index, path):
    """Index bit d = 0 hashes (node || sibling), else (sibling || node)."""
    assert_equal(len(path), HISTORY_DEPTH)
    node = leaf
    for depth, sibling in enumerate(path):
        if (index >> depth) & 1:
            node = keccak256(sibling + node)
        else:
            node = keccak256(node + sibling)
    return node


def decode_fb_v2(data):
    assert_equal(len(data), FB_V2_SIZE)
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
    }


def decode_set_header(data):
    assert_equal(len(data), SET_HEADER_SIZE)
    return {
        "epoch": int.from_bytes(data[0:8], "big"),
        "ruleset_version": int.from_bytes(data[8:10], "big"),
        "validator_count": int.from_bytes(data[10:14], "big"),
        "total_weight": int.from_bytes(data[14:22], "big"),
        "quorum_weight": int.from_bytes(data[22:30], "big"),
        "aggregate_pubkey": data[30:78],
        "members_root": data[78:110],
    }


def check_set_header(header, epoch, ruleset=RULESET_V2):
    """Decode encoded_header, check it against the JSON fields and its hash."""
    encoded = hx(header["encoded_header"])
    decoded = decode_set_header(encoded)
    assert_equal(decoded["epoch"], epoch)
    assert_equal(decoded["ruleset_version"], ruleset)
    for key in ("epoch", "ruleset_version", "validator_count", "total_weight", "quorum_weight"):
        assert_equal(decoded[key], header[key])
    assert_equal(decoded["aggregate_pubkey"], hx(header["aggregate_pubkey"]))
    assert_equal(decoded["members_root"], hx(header["members_root"]))
    assert_equal(decoded["quorum_weight"], decoded["total_weight"] * 2 // 3 + 1)
    assert_equal(hx(header["set_hash"]), keccak256(encoded))
    return decoded


def abi_uint(data, offset):
    word = data[offset:offset + 32]
    assert_equal(len(word), 32)
    return int.from_bytes(word, "big")


def abi_bytes(data, offset):
    """A dynamic bytes value at offset: (payload, end of its padded tail)."""
    length = abi_uint(data, offset)
    start = offset + 32
    end = start + (length + 31) // 32 * 32
    assert len(data) >= end, "ABI bytes run past the calldata"
    assert_equal(data[start + length:end], bytes(end - start - length))
    return data[start:start + length], end


def abi_set_header(data, offset):
    pubkey_offset = abi_uint(data, offset + 5 * 32)
    assert_equal(pubkey_offset, 7 * 32)  # The one dynamic member follows the head.
    pubkey, end = abi_bytes(data, offset + pubkey_offset)
    return {
        "epoch": abi_uint(data, offset),
        "ruleset_version": abi_uint(data, offset + 32),
        "validator_count": abi_uint(data, offset + 64),
        "total_weight": abi_uint(data, offset + 96),
        "quorum_weight": abi_uint(data, offset + 128),
        "aggregate_pubkey": pubkey,
        "members_root": data[offset + 192:offset + 224],
    }, end


def decode_submit_certificate(calldata):
    """Strict decode of the canonical submitCertificate encoding."""
    assert_equal(calldata[:4], selector(SUBMIT_CERTIFICATE_V2))
    args = calldata[4:]
    fb_words = [args[32 * i:32 * i + 32] for i in range(9)]
    signing_offset = abi_uint(args, 9 * 32)
    successor_offset = abi_uint(args, 10 * 32)
    path = [args[32 * (11 + i):32 * (12 + i)] for i in range(HISTORY_DEPTH)]
    proof_offset = abi_uint(args, 43 * 32)
    assert_equal(signing_offset, CALLDATA_HEAD_WORDS * 32)
    signing, end = abi_set_header(args, signing_offset)
    assert_equal(successor_offset, end)
    successor, end = abi_set_header(args, successor_offset)
    assert_equal(proof_offset, end)
    proof, end = abi_bytes(args, proof_offset)
    assert_equal(end, len(args))
    return {"fb_words": fb_words, "signing": signing, "successor": successor,
            "path": path, "proof": proof}


def fb_v2_words(fb):
    return [
        fb["lineage_id"], word_uint(fb["height"]), fb["block_hash"], fb["withdrawal_root"],
        fb["validator_set_hash"], word_uint(fb["epoch"]), word_uint(fb["checkpoint_index"]),
        fb["checkpoint_history_root"], word_uint(fb["bridge_flags"]),
    ]


def sha256d(data):
    return hashlib.sha256(hashlib.sha256(data).digest()).digest()


def decode_journal(path, magic):
    """Parse one finality signer journal (magic, version, DiskState, sha256d)."""
    with open(path, "rb") as f:
        data = f.read()
    assert_equal(len(data), JOURNAL_SIZE)
    assert_equal(data[:8], magic)
    assert_equal(data[8], 1)
    assert_equal(data[-32:], sha256d(data[:-32]))
    pos = 9

    def take(size):
        nonlocal pos
        out = data[pos:pos + size]
        pos += size
        return out

    out = {"scope": take(32), "validator_key": take(32)}
    out["last_signed_height"] = int.from_bytes(take(4), "little", signed=True)
    out["last_signed_block_hash"] = take(32)
    out["last_signed_digest"] = take(32)
    out["lock_height"] = int.from_bytes(take(4), "little", signed=True)
    out["lock_block_hash"] = take(32)
    out["lock_digest"] = take(32)
    out["lock_epoch"] = int.from_bytes(take(8), "little")
    out["lock_signing_set_hash"] = take(32)
    out["lock_successor_set_hash"] = take(32)
    assert_equal(pos, JOURNAL_SIZE - 32)
    return out


def lineage_view(node):
    info = node.getfinalitylineage()
    view = {key: info.get(key) for key in LINEAGE_VIEW_KEYS}
    view["tip"] = node.getbestblockhash()
    return view


def sets_by_epoch(view):
    out = {}
    for key in ("genesis_set", "previous_set", "current_set", "next_set"):
        header = view.get(key)
        if header is not None:
            assert out.get(header["epoch"], header) == header
            out[header["epoch"]] = header
    return out


class B3FreshLineageTest(FreshVoteShuttle, B3BridgeChainMixin, BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 5
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        args = list(B3_BRIDGE_ARGS) + FRESH_ARGS
        # n0-n3 are the validators; n4 only checks refusals, then syncs.
        self.add_nodes(self.num_nodes, [list(args) for _ in range(self.num_nodes)])
        self.check_knob_refusals()
        self.start_bridge_validators(extra_args=FRESH_ARGS)

    # -- 1. startup refusals ------------------------------------------------
    def check_knob_refusals(self):
        self.log.info("n4 refuses the fresh-lineage knobs outside their exact configuration")
        n4 = self.nodes[4]
        without_flowmesh = [arg for arg in B3_BRIDGE_ARGS if arg != "-b3flowmeshtest"] + FRESH_ARGS
        refusals = {
            "without -b3flowmeshtest": start_refused(n4, without_flowmesh, REQUIRES_FLOWMESH),
            "off the checkpoint grid": start_refused(
                n4, list(B3_BRIDGE_ARGS) + [f"-b3freshlineageheight={FRESH_ACTIVATION + 1}"], OFF_GRID),
            "extension without height": start_refused(
                n4, list(B3_BRIDGE_ARGS) + [f"-b3freshmaxepochextension={FRESH_EXTENSION}"], EXTENSION_ALONE),
            "negated knob": start_refused(n4, list(B3_BRIDGE_ARGS) + ["-nob3freshlineageheight"], NEGATED),
        }
        self.evidence.assertion("fresh-lineage knobs refused on n4", expected={
            "without -b3flowmeshtest": REQUIRES_FLOWMESH, "off the checkpoint grid": OFF_GRID,
            "extension without height": EXTENSION_ALONE, "negated knob": NEGATED}, actual=refusals)

    # -- test ---------------------------------------------------------------
    def run_test(self):
        # Hidden-RPC drill results only: the fresh votes themselves gossip
        # (fresh_vote_transport "p2p", so fresh_vote_results stays empty).
        self.vote_results = Counter()
        self.domain = regtest_chain_domain()

        self.check_before_activation()
        self.log.info("Corridor, four stakes and live V1 BLS finality past the fresh genesis")
        self.bring_up_validators(FIRST_FRESH_CHECKPOINT + 1)
        self.v1_start = self.nodes[0].getfinalitystatus()
        self.evidence_step("validators live", v1_finalized=self.v1_start["finalized"],
                           v1_epoch=self.v1_start["epoch"])
        self.check_bootstrap()

        self.log.info("Fresh votes gossip (finsigv2) until a V2 certificate is finalized on every node")
        # V2 votes (finsigv2) and V1 votes (finsig) gossip with no ordering
        # between them: wait for both lineages (and for every staging signer
        # to have signed the checkpoint) before freezing.
        self.shuttle_until(lambda: all(self.first_checkpoint_settled(node) for node in self.bridge_validators()))
        self.freeze()
        view = self.agreed_view()
        self.check_local_signers(min_signed=FIRST_FRESH_CHECKPOINT)
        self.first = self.check_certificate_export(view)
        self.check_export_refusals(view, self.first)
        self.check_v1_status(min_finalized=FIRST_FRESH_CHECKPOINT)
        gossip = self.assert_fresh_gossip(self.bridge_validators())
        self.evidence_step("first fresh certificate exported", fresh_finalized=view["finalized"],
                           finsigv2_bytes_received=gossip, vote_rpc_calls=dict(self.fresh_vote_results))
        self.hidden_rpc_drill()

        self.log.info("Fresh votes keep gossiping until the lineage rotates and Set'_1 certifies")
        self.resume()
        self.shuttle_until(lambda: all(self.epoch1_settled(node) for node in self.bridge_validators()),
                           timeout=600)
        self.freeze()
        view = self.agreed_view()
        self.check_local_signers(min_signed=view["finalized"]["height"])
        self.check_rotation(view)
        second = self.check_certificate_export(view)
        assert_greater_than(second["fb"]["checkpoint_index"], self.first["fb"]["checkpoint_index"])
        assert_greater_than_or_equal(second["history"]["prev_index"], 1)
        self.check_carriers(view)
        self.check_v1_status(min_finalized=FIRST_FRESH_CHECKPOINT + 1, epoch_advanced=True)
        self.evidence_step("fresh epoch 1 certified", fresh_finalized=view["finalized"],
                           epoch_starts=view["epoch_starts"], vote_rpc_calls=dict(self.fresh_vote_results))

        self.check_restart_and_reindex(view)
        self.check_sync_from_genesis(view)
        self.check_signer_journals(view)
        self.check_gossip_reaches_every_node()
        # Only the drill submitted votes by RPC: its duplicates and its two
        # tampered votes.
        assert_equal(set(self.vote_results), {"duplicate", "bad-signature", "lineage-mismatch"})
        assert_equal((self.vote_results["bad-signature"], self.vote_results["lineage-mismatch"]), (1, 1))
        self.evidence_step("fresh lineage checks complete", drill_results=dict(self.vote_results),
                           vote_rpc_calls=dict(self.fresh_vote_results))

    # -- 2. before R_f --------------------------------------------------------
    def check_before_activation(self):
        self.log.info("Before R_f: getfinalitylineage reports the configuration only")
        for node in self.bridge_validators():
            info = node.getfinalitylineage()
            assert_equal(info["configured"], True)
            assert_equal(info["staging_signer"], True)
            assert_equal(info["activation_height"], FRESH_ACTIVATION)
            assert_equal(info["genesis_height"], FRESH_GENESIS)
            assert_equal(info["max_epoch_extension"], FRESH_EXTENSION)
            assert_equal(info["checkpoint_interval"], CHECKPOINT_INTERVAL)
            assert_equal(hx(info["chain_domain"]), self.domain)
            assert_greater_than(FRESH_GENESIS, info["tip_height"])
            assert_equal(info.get("bootstrapped", False), False)
            for key in ("lineage_id", "genesis_set", "current_set", "finalized"):
                assert key not in info, key
        self.evidence.assertion("lineage configured, not bootstrapped before R_f",
                                actual=self.nodes[0].getfinalitylineage())

    # -- 4. bootstrap ---------------------------------------------------------
    def check_bootstrap(self):
        self.log.info("Set'_0 = Snapshot(261) and the lineage id agree on every node and in Python")
        validators = self.bridge_validators()
        self.wait_until(lambda: all(node.getfinalitylineage().get("bootstrapped") for node in validators),
                        timeout=120)
        infos = [node.getfinalitylineage() for node in validators]
        for info in infos[1:]:
            for key in ("lineage_id", "genesis_block_hash", "genesis_set_hash", "genesis_set",
                        "genesis_leaf", "epoch_starts"):
                assert_equal(info[key], infos[0][key])
        info = infos[0]
        n0 = self.nodes[0]
        genesis_hash = raw_block_hash(n0, FRESH_GENESIS)
        assert_equal(info["genesis_block_hash"], n0.getblockhash(FRESH_GENESIS))
        assert_equal(hx(info["genesis_block_hash_evm"]), genesis_hash)
        assert_equal(hx(info["genesis_leaf"]), history_leaf(FRESH_GENESIS, genesis_hash))

        genesis_set = check_set_header(info["genesis_set"], epoch=0)
        genesis_set_hash = hx(info["genesis_set_hash"])
        assert_equal(hx(info["genesis_set"]["set_hash"]), genesis_set_hash)
        assert_equal(genesis_set["validator_count"], VALIDATOR_COUNT)
        # Still fresh epoch 0 (302 is the first rotation): Set'_1 has the
        # same members stamped with epoch 1.
        assert_equal(info["epoch"], 0)
        assert_equal(info["epoch_starts"], [FRESH_ACTIVATION])
        assert_equal(info["lineage_broken"], False)
        assert_equal(info["break_height"], FRESH_ACTIVATION + EPOCH_LENGTH + FRESH_EXTENSION)
        assert_equal(info["current_set"], info["genesis_set"])
        next_set = check_set_header(info["next_set"], epoch=1)
        for key in ("validator_count", "total_weight", "aggregate_pubkey", "members_root"):
            assert_equal(next_set[key], genesis_set[key])
        assert hx(info["next_set"]["set_hash"]) != genesis_set_hash

        # Same members as the V1 set (ruleset 1), different set hash.
        v1_set = n0.getfinalityset()
        assert_equal(v1_set["ruleset_version"], RULESET_V1)
        assert_equal(genesis_set["members_root"], hx(v1_set["members_root"]))
        assert_equal(genesis_set["aggregate_pubkey"], hx(v1_set["aggregate_pubkey"]))
        assert_equal(genesis_set["validator_count"], v1_set["validator_count"])
        assert_equal(genesis_set["total_weight"], v1_set["total_weight"])
        assert hx(v1_set["set_hash"]) != genesis_set_hash

        expected = TaggedHash(LINEAGE_V2_TAG, self.domain + be64(FRESH_GENESIS) + genesis_hash +
                              genesis_set_hash)
        assert_equal(hx(info["lineage_id"]), expected)
        self.lineage_id = info["lineage_id"]
        self.genesis_set_hash = genesis_set_hash
        self.evidence.assertion("lineage id derived independently", expected=to_0x(expected),
                                actual=self.lineage_id, genesis_block_hash=info["genesis_block_hash"],
                                genesis_set_hash=info["genesis_set_hash"])

    # -- 5. gossip and the hidden-RPC drill ------------------------------------
    @staticmethod
    def decode_vote(vote):
        """A getfinalitylineagevotes entry checked against its 148 bytes:
        lineage id || u64 epoch || u64 height || u32 index (little-endian,
        like finsig) || 96-byte signature."""
        raw = hx(vote["hex"])
        assert_equal(len(raw), VOTE_SIZE)
        assert_equal(raw[0:32], hx(vote["lineage_id"]))
        assert_equal(int.from_bytes(raw[32:40], "little"), vote["epoch"])
        assert_equal(int.from_bytes(raw[40:48], "little"), vote["height"])
        assert_equal(int.from_bytes(raw[48:52], "little"), vote["index"])
        assert_equal(raw[52:], hx(vote["signature"]))
        assert_equal(len(raw[52:]), BLS_SIGNATURE_SIZE)
        return raw

    def settled_exports(self, validators, timeout=60):
        """Frozen: wait until gossip gives every validator the same verified
        votes; return them as {hex: vote}."""
        exports = []

        def agreed():
            exports[:] = [{vote["hex"]: vote for vote in node.getfinalitylineagevotes()} for node in validators]
            return all(set(export) == set(exports[0]) for export in exports)

        self.wait_until(agreed, timeout=timeout)
        return exports[0]

    def hidden_rpc_drill(self, attempts=20):
        """The hidden vote RPCs as a drill: export, re-submission, tampering."""
        self.log.info("Hidden-RPC drill: the gossiped pools export the same votes; tampered votes are refused")
        validators = self.bridge_validators()
        n0 = self.nodes[0]
        for attempt in range(1, attempts + 1):
            # Votes are pending only between a checkpoint becoming signable
            # and the block that carries its certificate: freeze inside that
            # window (a block in flight can still close it, so retry).
            self.resume()
            self.wait_until(lambda: len(n0.getfinalitylineagevotes()) > 0, timeout=120, check_interval=0.05)
            self.freeze()
            pooled = self.settled_exports(validators)
            if pooled:
                break
        else:
            raise AssertionError(f"no fresh vote was pending at a freeze in {attempts} attempts")
        for vote in pooled.values():
            assert_equal(vote["lineage_id"], self.lineage_id)
            self.decode_vote(vote)
        # Gossip already pooled every vote everywhere: the RPC adds nothing.
        resubmitted = Counter()
        for target in validators:
            for vote_hex in pooled:
                result = target.submitfinalitylineagevote(vote_hex)["result"]
                assert_equal(result, "duplicate")
                resubmitted[result] += 1
        self.vote_results.update(resubmitted)

        sample = next(iter(pooled.values()))
        raw = bytearray(hx(sample["hex"]))
        raw[-1] ^= 0x01  # A signature byte: other bytes for a pooled member.
        bad_signature = validators[1].submitfinalitylineagevote(raw.hex())["result"]
        raw = bytearray(hx(sample["hex"]))
        raw[0] ^= 0x01  # lineage_id is the first serialized field.
        lineage_mismatch = validators[0].submitfinalitylineagevote(raw.hex())["result"]
        assert_equal((bad_signature, lineage_mismatch), ("bad-signature", "lineage-mismatch"))
        self.vote_results.update([bad_signature, lineage_mismatch])
        assert_equal(self.settled_exports(validators, timeout=10), pooled)
        self.evidence.assertion(
            "hidden-RPC drill: one gossiped export on every validator; tampered votes refused",
            expected={"resubmitted": "duplicate", "flipped signature byte": "bad-signature",
                      "flipped lineage id byte": "lineage-mismatch"},
            actual={"resubmitted": dict(resubmitted), "flipped signature byte": bad_signature,
                    "flipped lineage id byte": lineage_mismatch},
            attempts=attempt, tip=n0.getblockcount(),
            votes=sorted((vote["epoch"], vote["height"], vote["index"]) for vote in pooled.values()))

    @staticmethod
    def fresh_epoch1_certified(node):
        info = node.getfinalitylineage()
        return info.get("epoch", 0) >= 1 and info.get("finalized", {}).get("epoch", 0) >= 1

    @staticmethod
    def v1_progress(node):
        """(V1 finalized height or -1, V1 epoch) of one node."""
        status = node.getfinalitystatus()
        return (status.get("finalized") or {}).get("height", -1), status.get("epoch", 0)

    @staticmethod
    def local_signed_height(node):
        return (node.getfinalitylineage().get("local_signer") or {}).get("last_signed_height", -1)

    def first_checkpoint_settled(self, node):
        """V2 and V1 both finalized the first fresh checkpoint; its signer signed it."""
        return (self.fresh_finalized_height(node) >= FIRST_FRESH_CHECKPOINT and
                self.v1_progress(node)[0] >= FIRST_FRESH_CHECKPOINT and
                self.local_signed_height(node) >= FIRST_FRESH_CHECKPOINT)

    def epoch1_settled(self, node):
        """Set'_1 certified; V1 moved past both the checkpoint and its start epoch."""
        if not self.fresh_epoch1_certified(node):
            return False
        v1_finalized, v1_epoch = self.v1_progress(node)
        return (v1_finalized > FIRST_FRESH_CHECKPOINT and v1_epoch > self.v1_start["epoch"] and
                self.local_signed_height(node) >= self.fresh_finalized_height(node))

    def check_local_signers(self, min_signed):
        """Every validator's staging signer reports no refusal and has signed."""
        observed = {}
        for node in self.bridge_validators():
            local = node.getfinalitylineage()["local_signer"]
            assert_equal(local["last_error"], "")
            assert_greater_than_or_equal(local["last_signed_height"], min_signed)
            assert_equal((local["last_signed_height"] - FRESH_GENESIS) % CHECKPOINT_INTERVAL, 0)
            observed[f"node{node.index}"] = local
        self.evidence.assertion("staging signers healthy (node-local status)", actual=observed)

    def agreed_view(self):
        """Every running node exports the same fresh-lineage state."""
        views = [lineage_view(node) for node in self.bridge_nodes()]
        for view in views[1:]:
            assert_equal(view, views[0])
        view = views[0]
        assert_equal(view["bootstrapped"], True)
        assert_equal(view["lineage_id"], self.lineage_id)
        assert_equal(view["lineage_broken"], False)
        self.check_history_frontier(view)
        return view

    def check_history_frontier(self, view):
        """The tip's history root over every grid checkpoint, from getblockhash."""
        n0 = self.nodes[0]
        history = view["history"]
        count = (view["tip_height"] - FRESH_GENESIS) // CHECKPOINT_INTERVAL + 1
        assert_equal(history["checkpoint_count"], count)
        assert_equal(history["last_checkpoint_height"], FRESH_GENESIS + (count - 1) * CHECKPOINT_INTERVAL)
        leaves = [history_leaf(h, raw_block_hash(n0, h))
                  for h in range(FRESH_GENESIS, history["last_checkpoint_height"] + 1, CHECKPOINT_INTERVAL)]
        root, _ = history_root_and_path(leaves)
        assert_equal(hx(history["root"]), root)
        finalized = view.get("finalized")
        expected_held = finalized["checkpoint_index"] + 1 if finalized else 1
        assert_equal(history["finalized_leaf_count"], expected_held)

    # -- 6. certificate export ------------------------------------------------
    def check_certificate_export(self, view):
        n0 = self.nodes[0]
        finalized = view["finalized"]
        inclusion = finalized["certified_at"]
        self.log.info(f"Export the V2 certificate for {finalized['height']} carried at {inclusion}")
        exports = [node.getfinalitylineagecertificate(inclusion) for node in self.bridge_nodes()]
        for export in exports[1:]:
            assert_equal(export, exports[0])
        cert = exports[0]
        assert_equal(cert["inclusion_height"], inclusion)
        assert_equal(cert["inclusion_block"], n0.getblockhash(inclusion))
        assert_equal(hx(cert["inclusion_block_evm"]), raw_block_hash(n0, inclusion))
        assert_equal(cert["private_key_required_by_rpc"], False)

        # The 185-byte FinalizedBlockV2.
        encoded = hx(cert["encoded"])
        fb = decode_fb_v2(encoded)
        height = fb["height"]
        assert_equal(fb["lineage_id"], hx(self.lineage_id))
        assert_equal(height, finalized["height"])
        assert_equal((height - FRESH_GENESIS) % CHECKPOINT_INTERVAL, 0)
        assert_greater_than_or_equal(inclusion, height + CHECKPOINT_DEPTH)
        assert_equal(fb["block_hash"], raw_block_hash(n0, height))
        assert_equal(cert["block_hash"], n0.getblockhash(height))
        assert_equal(fb["withdrawal_root"], bytes(32))
        assert_equal(fb["bridge_flags"], 0)
        index = fb["checkpoint_index"]
        assert_equal(index, (height - FRESH_GENESIS) // CHECKPOINT_INTERVAL)
        assert_equal(fb["epoch"], finalized["epoch"])
        for key in ("height", "epoch", "checkpoint_index", "bridge_flags"):
            assert_equal(cert[key], fb[key])
        for key in ("lineage_id", "withdrawal_root", "validator_set_hash", "checkpoint_history_root"):
            assert_equal(hx(cert[key]), fb[key])
        assert_equal(hx(cert["block_hash_evm"]), fb["block_hash"])
        assert_equal(finalized["checkpoint_index"], index)
        assert_equal(finalized["block_hash"], cert["block_hash"])

        # History leaves 0..index-1 from getblockhash.
        leaves = [history_leaf(FRESH_GENESIS + i * CHECKPOINT_INTERVAL,
                               raw_block_hash(n0, FRESH_GENESIS + i * CHECKPOINT_INTERVAL))
                  for i in range(index)]
        root, leaf0_path = history_root_and_path(leaves, 0)
        assert_equal(fb["checkpoint_history_root"], root)
        assert_equal(hx(finalized["checkpoint_history_root"]), root)
        history = cert["history"]
        prev_index = history["prev_index"]
        assert 0 <= prev_index < index
        prev_height = FRESH_GENESIS + prev_index * CHECKPOINT_INTERVAL
        assert_equal(history["prev_height"], prev_height)
        assert_equal(history["prev_block_hash"], n0.getblockhash(prev_height))
        assert_equal(hx(history["prev_block_hash_evm"]), raw_block_hash(n0, prev_height))
        assert_equal(hx(history["prev_leaf"]), leaves[prev_index])
        ancestry_path = [hx(sibling) for sibling in history["ancestry_path"]]
        assert_equal(ancestry_path, history_root_and_path(leaves, prev_index)[1])
        assert_equal(fold_history_path(leaves[prev_index], prev_index, ancestry_path), root)
        assert_equal(history["genesis_height"], FRESH_GENESIS)
        assert_equal(hx(history["genesis_leaf"]), leaves[0])
        assert_equal([hx(sibling) for sibling in history["leaf0_path"]], leaf0_path)
        assert_equal(fold_history_path(leaves[0], 0, leaf0_path), root)

        # Hashes: digestV2 (sha256 tagged) and certId (keccak).
        assert_equal(hx(cert["digest_v2"]), TaggedHash(FINALITY_V2_TAG, self.domain + encoded))
        assert_equal(hx(cert["cert_id"]), keccak256(encoded))
        assert_equal(hx(finalized["cert_id"]), keccak256(encoded))

        # Signing and successor ruleset-2 headers.
        signing = check_set_header(cert["signing_set"], epoch=fb["epoch"])
        successor = check_set_header(cert["successor_set"], epoch=fb["epoch"] + 1)
        assert_equal(hx(cert["successor_set"]["set_hash"]), fb["validator_set_hash"])
        lineage_sets = sets_by_epoch(view)
        assert_equal(cert["signing_set"], lineage_sets[fb["epoch"]])
        if fb["epoch"] + 1 in lineage_sets:
            assert_equal(cert["successor_set"], lineage_sets[fb["epoch"] + 1])

        # Signers: LSB-first bitmap, both quorums.
        n = signing["validator_count"]
        assert_equal(cert["validator_count"], n)
        bitmap = hx(cert["signer_bitmap"])
        assert_equal(len(bitmap), (n + 7) // 8)
        signers = [i for i in range(n) if bitmap[i // 8] >> (i % 8) & 1]
        assert not any(bitmap[i // 8] >> (i % 8) & 1 for i in range(n, 8 * len(bitmap)))
        assert_equal(cert["signer_count"], len(signers))
        assert_equal(cert["absent_indices"], [i for i in range(n) if i not in signers])
        assert_greater_than_or_equal(len(signers), 2 * n // 3 + 1)
        assert_greater_than_or_equal(cert["signed_weight"], signing["quorum_weight"])

        # The coinbase carrier: exactly one type-11 record, fb || bitmap || sig.
        coinbase = n0.getblock(cert["inclusion_block"], 2)["tx"][0]
        records = [r for r in coinbase.get("mpa", []) if r["type"] == MPA_TYPE_FINALITY_CERTIFICATE_V2]
        assert_equal(len(records), 1)
        assert_equal(records[0]["version"], 1)
        payload = hx(records[0]["payload"])
        assert_equal(len(payload), FB_V2_SIZE + len(bitmap) + BLS_SIGNATURE_SIZE)
        assert_equal(payload[:FB_V2_SIZE], encoded)
        assert_equal(payload[FB_V2_SIZE:FB_V2_SIZE + len(bitmap)], bitmap)

        # Verifier V2 calldata.
        assert_equal(cert["function"], SUBMIT_CERTIFICATE_V2)
        call = decode_submit_certificate(hx(cert["calldata"]))
        assert_equal(call["fb_words"], fb_v2_words(fb))
        for decoded, header in ((call["signing"], signing), (call["successor"], successor)):
            assert_equal(decoded, header)
        assert_equal(call["path"], ancestry_path)
        assert_equal(call["proof"], hx(cert["proof"]))

        self.evidence.assertion(
            f"V2 certificate at {height} re-derived from chain data", expected=to_0x(keccak256(encoded)),
            actual=cert["cert_id"], inclusion=inclusion, height=height, epoch=fb["epoch"],
            checkpoint_index=index, prev_index=prev_index, signer_count=len(signers),
            signed_weight=cert["signed_weight"], calldata_bytes=len(hx(cert["calldata"])))
        return {"cert": cert, "fb": fb, "history": history, "leaves": leaves, "leaf0_path": leaf0_path}

    def check_export_refusals(self, view, exported):
        self.log.info("Explicit prev_index, and refusals of blocks that carry no V2 certificate")
        n0 = self.nodes[0]
        cert, fb = exported["cert"], exported["fb"]
        inclusion = cert["inclusion_height"]
        # prev_index 0 places the genesis leaf: the ancestry path is leaf0Path.
        genesis = n0.getfinalitylineagecertificate(inclusion, 0)
        assert_equal(genesis["history"]["prev_index"], 0)
        assert_equal(genesis["history"]["ancestry_path"], cert["history"]["leaf0_path"])
        call = decode_submit_certificate(hx(genesis["calldata"]))
        assert_equal(call["path"], exported["leaf0_path"])
        assert_equal(call["fb_words"], fb_v2_words(fb))
        refusals = {
            "prev_index = index": rpc_refusal(
                RPC_MISC_ERROR, "must be below the certificate's checkpoint index",
                n0.getfinalitylineagecertificate, inclusion, fb["checkpoint_index"]),
            "GENESIS_HEIGHT block": rpc_refusal(
                RPC_MISC_ERROR, "below the fresh lineage activation height",
                n0.getfinalitylineagecertificate, FRESH_GENESIS),
            "R_f block": rpc_refusal(
                RPC_MISC_ERROR, "contains no fresh-lineage certificate",
                n0.getfinalitylineagecertificate, FRESH_ACTIVATION),
            "short vote": rpc_refusal(
                RPC_DESERIALIZATION_ERROR, f"exactly {VOTE_SIZE} bytes",
                n0.submitfinalitylineagevote, "00" * (VOTE_SIZE - 1)),
        }
        self.evidence.assertion("export and relay refusals", actual=refusals)

    # -- 7. V1 continues ------------------------------------------------------
    def check_v1_status(self, min_finalized, epoch_advanced=False):
        statuses = [node.getfinalitystatus() for node in self.bridge_validators()]
        for status in statuses:
            assert_equal(status["bootstrapped"], True)
            assert_equal(status["lineage_broken"], False)
            assert_greater_than_or_equal(status["finalized"]["height"], min_finalized)
            assert_equal(status["finalized"], statuses[0]["finalized"])
            if epoch_advanced:
                assert_greater_than(status["epoch"], self.v1_start["epoch"])
        # A V1 certificate for a checkpoint at or above min_finalized
        # (>= R_f + 4) is included at least checkpoint_depth blocks later,
        # so it was carried above R_f; check_carriers() reads the records.
        return statuses[0]

    def check_carriers(self, view):
        """Scan coinbases: V1 (type 4) and V2 (type 11) certificates coexist."""
        self.log.info("Coinbase records: V1 and V2 certificates on one chain")
        n0 = self.nodes[0]
        v1_records, v2_records, both = [], [], 0
        for height in range(MODERN_START, view["tip_height"] + 1):
            coinbase = n0.getblock(n0.getblockhash(height), 2)["tx"][0]
            records = coinbase.get("mpa", [])
            v1 = [r for r in records if r["type"] == MPA_TYPE_FINALITY_CERTIFICATE]
            v2 = [r for r in records if r["type"] == MPA_TYPE_FINALITY_CERTIFICATE_V2]
            assert len(v1) <= 1 and len(v2) <= 1
            for record in v1:
                v1_records.append((height, int.from_bytes(hx(record["payload"])[:8], "big")))
            for record in v2:
                assert_greater_than_or_equal(height, FRESH_ACTIVATION)
                fb = decode_fb_v2(hx(record["payload"])[:FB_V2_SIZE])
                assert_equal(fb["lineage_id"], hx(self.lineage_id))
                assert_equal(fb["checkpoint_index"], (fb["height"] - FRESH_GENESIS) // CHECKPOINT_INTERVAL)
                assert_greater_than_or_equal(height, fb["height"] + CHECKPOINT_DEPTH)
                v2_records.append((height, fb["height"], fb["epoch"]))
            both += bool(v1 and v2)
        assert_greater_than_or_equal(len(v2_records), 2)
        certified = [record[1] for record in v2_records]
        assert_equal(certified, sorted(set(certified)))  # Strictly increasing.
        assert_equal((v2_records[-1][0], v2_records[-1][1]),
                     (view["finalized"]["certified_at"], view["finalized"]["height"]))
        assert {record[2] for record in v2_records} >= {0, 1}
        v1_after = [record for record in v1_records if record[0] > FRESH_ACTIVATION]
        assert v1_after, "no V1 certificate was included above R_f"
        assert_equal(v1_records[-1][1], n0.getfinalitystatus()["finalized"]["height"])
        self.evidence.assertion("V1 and V2 certificate carriers", actual={
            "v1_certificates_above_r_f": len(v1_after), "v2_certificates": len(v2_records),
            "blocks_with_both": both, "v2": v2_records, "last_v1": v1_records[-1]})

    # -- 8. rotation ----------------------------------------------------------
    def check_rotation(self, view):
        self.log.info("The fresh lineage rotated after its handover; V2 epochs are independent of V1")
        first_fb = self.first["fb"]
        assert_greater_than_or_equal(view["epoch"], 1)
        starts = view["epoch_starts"]
        assert_equal(starts[0], FRESH_ACTIVATION)
        assert_greater_than_or_equal(starts[1], FRESH_EPOCH1_START)
        if self.first["cert"]["inclusion_height"] < FRESH_EPOCH1_START:
            # The epoch-0 handover was certified before 302: rotation is exact.
            assert_equal(starts[1], FRESH_EPOCH1_START)
        sets = sets_by_epoch(view)
        assert_equal(hx(view["genesis_set"]["set_hash"]), self.genesis_set_hash)
        if view["epoch"] == 1:
            assert_equal(hx(view["previous_set"]["set_hash"]), self.genesis_set_hash)
        if first_fb["epoch"] == 0 and 1 in sets:
            assert_equal(hx(sets[1]["set_hash"]), first_fb["validator_set_hash"])
        for epoch, header in sets.items():
            check_set_header(header, epoch=epoch)
        assert_greater_than_or_equal(view["finalized"]["epoch"], 1)
        assert_greater_than_or_equal(view["finalized"]["height"], starts[1])
        assert_equal(view["break_height"], starts[-1] + EPOCH_LENGTH + FRESH_EXTENSION)

    # -- 9. restart, reindex, sync from genesis -------------------------------
    def node_state(self, node):
        return {"lineage": lineage_view(node), "v1_finalized": node.getfinalitystatus()["finalized"]}

    def check_restart_and_reindex(self, view):
        n3 = self.nodes[3]
        before = self.node_state(n3)
        assert_equal(before["lineage"], view)
        self.log.info("n3 restarts and reproduces the fresh lineage")
        self.restart_node(3)
        self.connect_nodes(3, 2)
        assert_equal(self.node_state(n3), before)
        self.log.info("n3 reindexes and reproduces the fresh lineage")
        self.restart_node(3, extra_args=list(n3.extra_args) + ["-reindex"])
        self.wait_until(lambda: n3.getbestblockhash() == view["tip"], timeout=300)
        self.connect_nodes(3, 2)
        n3.syncwithvalidationinterfacequeue()
        assert_equal(self.node_state(n3), before)
        self.evidence_step("n3 restart and reindex agree", lineage_id=view["lineage_id"],
                           epoch=view["epoch"], fresh_finalized=view["finalized"])

    def check_sync_from_genesis(self, view):
        self.log.info("n4 syncs from genesis, validating every V2 carrier, and agrees")
        n4 = self.nodes[4]
        self.start_node(4)
        self.connect_nodes(4, 0)
        self.sync_blocks(self.bridge_nodes(), timeout=300)
        n4.syncwithvalidationinterfacequeue()
        assert_equal(self.node_state(n4), self.node_state(self.nodes[0]))
        assert_equal(lineage_view(n4), view)
        self.evidence_step("n4 synced from genesis and agrees", fresh_finalized=view["finalized"])

    # -- 10. gossip on every current connection -------------------------------
    def check_gossip_reaches_every_node(self):
        """n4 (linked to n0 only, no vote RPC ever) and every validator,
        restarted n3 included, receive finsigv2; n4 pools verified votes."""
        self.log.info("Staking resumed: finsigv2 reaches n4 and every validator on their current connections")
        nodes = self.bridge_nodes()
        n4 = self.nodes[4]
        assert_equal([node.index for node in nodes], list(range(self.num_nodes)))
        before = {f"node{node.index}": self.fresh_gossip_bytes(node) for node in nodes}
        n4_pooled = []

        def reached():
            if not n4_pooled:
                n4_pooled.extend(n4.getfinalitylineagevotes())
            return n4_pooled and all(self.fresh_gossip_bytes(node) > before[f"node{node.index}"] for node in nodes)

        self.resume()
        self.wait_until(reached, timeout=300)
        self.freeze()
        after = self.assert_fresh_gossip(nodes)
        for vote in n4_pooled:
            assert_equal(vote["lineage_id"], self.lineage_id)
            self.decode_vote(vote)
        self.evidence.assertion("finsigv2 reaches n4 and every validator", expected="more bytes on every node",
                                actual={"before": before, "after": after},
                                n4_pooled=sorted((vote["epoch"], vote["height"], vote["index"]) for vote in n4_pooled))

    # -- 10. journals ---------------------------------------------------------
    def check_signer_journals(self, view):
        self.log.info("Every validator keeps separate V1 and V2 lineage signer journals")
        lineage = hx(self.lineage_id)
        observed = {}
        for node in self.bridge_validators():
            directory = node.chain_path / "finality_signer"
            names = sorted(name for name in os.listdir(directory) if name.endswith(".dat"))
            validator_key = node.getstakinginfo()["validator_key"]
            v1_name = f"finality_signer_v1_{self.domain[::-1].hex()}_{validator_key}.dat"
            v2_name = f"finality_signer_v2_lineage_{lineage[::-1].hex()}_{validator_key}.dat"
            assert_equal(names, sorted([v1_name, v2_name]))
            v1 = decode_journal(directory / v1_name, V1_JOURNAL_MAGIC)
            v2 = decode_journal(directory / v2_name, V2_JOURNAL_MAGIC)
            assert_equal(v1["scope"], self.domain)
            assert_equal(v2["scope"], lineage)
            for journal in (v1, v2):
                assert_equal(journal["validator_key"].hex(), validator_key)
                assert_greater_than_or_equal(journal["lock_height"], journal["last_signed_height"])
            # The fresh journal only ever records fresh-grid checkpoints
            # above GENESIS_HEIGHT, with its own (V2) digests.
            assert_greater_than(v2["last_signed_height"], FRESH_GENESIS)
            assert_equal((v2["last_signed_height"] - FRESH_GENESIS) % CHECKPOINT_INTERVAL, 0)
            assert_equal(v2["last_signed_block_hash"], raw_block_hash(node, v2["last_signed_height"]))
            assert_greater_than_or_equal(v2["lock_epoch"], 1)
            if v1["last_signed_height"] == v2["last_signed_height"]:
                assert v1["last_signed_digest"] != v2["last_signed_digest"]
            observed[f"node{node.index}"] = {
                "files": names, "v1_last_signed": v1["last_signed_height"],
                "v2_last_signed": v2["last_signed_height"], "v2_lock_epoch": v2["lock_epoch"]}
        self.evidence.assertion("separate V1 and V2 signer journals", actual=observed)


if __name__ == '__main__':
    B3FreshLineageTest(__file__).main()
