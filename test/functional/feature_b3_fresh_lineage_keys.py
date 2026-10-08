#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Fresh-lineage per-epoch committee keys (B-37) on regtest: bridge V2 stage S3.

REGTEST ONLY. Every process is local: five B3 regtest validators linked in a
line n0-n1-n2-n3-n4 and a sixth node n5 that holds a restored copy of n0's
wallet backup, never stakes and links to n0 only. No anvil, no Ethereum
endpoint and no external peer is used; no fresh vote is moved by RPC.

With -b3freshlineageheight=262 (R_f) and -b3freshkeyheight=241 (R_key) the
fresh lineage seats only independent per-epoch BLS keys bound under their
own binding kind (FINALITY_KEY policy-version-2 cell plus MPA type 12;
finality dependency section 6.4), generated from fresh randomness into each
node's fresh key store outside the wallet file. V1 finality keeps its own
FINALITY_KEY bindings and keys throughout. Stakes are 40/30/20/10/10 with
-b3minfinalityset=4; -b3reorghorizon=40 sets the erasure depth. The test
checks:

1. n5 refuses -b3freshkeyheight without -b3flowmeshtest, without
   -b3freshlineageheight, at R_f - 1 and at 0, and negated.
2. Corridor, five stakes with V1 finality keys and live V1 BLS finality.
3. Below R_key bindfreshfinalitykey is refused (window not-active) and
   getfreshfinalitykeys / getfinalitylineage report the configuration only.
4. Between R_key and R_f - 1 every validator binds its genesis key (epoch 0,
   serving Set'_0 and Set'_1): a wrong epoch and a second key for the epoch
   are refused, the store records the key and its binding transaction, and
   a drill transaction rebinding n0's V1 key as a fresh key is refused by the
   mempool (fresh-key-ever-bound). n0's wallet is backed up after its
   binding; n5 restores that backup.
5. At R_f Set'_0 seats the five validators on their fresh keys, disjoint
   from getfinalityset's V1 keys, with Set'_1 = Set'_0's keys; V2
   certificates finalize with votes moved by finsigv2 gossip only.
6. In fresh epoch 0 n5 (the old backup) binds an epoch-2 key under n0's
   identity first, then n0 to n4 bind theirs. After the rotation into epoch
   1 Set'_2 has four members without n0, the only void (validator, epoch) is
   (n0, 2), and n0's getfreshfinalitykeys and staging signer raise the
   foreign-binding and void-seat alerts; certificates continue.
7. In epoch 1 all five bind epoch-3 keys (rebinding n0's stored genesis key
   is refused by the mempool); after the rotation into epoch 2 Set'_2 (four
   members) certifies, Set'_3 seats all five on new keys, and n0's voided
   epoch-2 key, never seated, is erased once its committee is fixed and
   HORIZON deep.
8. The restored backup: n5's store and getfreshfinalitykeys hold no secret
   of any seated key of epoch >= 2 (only its own voided key), it reports
   n0's epoch-3 seat as missing, and the V1 key it can still derive matches
   no fresh committee key.
9. Erasure: every validator holds its genesis key at a tip in
   [start_2 + HORIZON - 5, start_2 + HORIZON - 1] and erases it at
   start_2 + HORIZON (to a few blocks, from the signer's log): the record is
   erased, its secret bytes are gone from the file, current and next keys
   (and Set'_2's keys, retired only at start_3) stay held. Set'_3 certifies
   with n0 seated again (its fresh journal lock moved past the epoch it sat
   out to an included epoch-2 certificate); without epoch-4 bindings Set'_4
   carries Set'_3 over.
10. V1's finalized height advances between every phase.
11. A restart and a -reindex restart of n4 reproduce the lineage, its
    committees, bindings and voids, and n4's key report; afterwards V1 and
    the fresh lineage both keep finalizing, with gossip on every validator.
"""

import os
import re

from test_framework.b3bridge import (
    B3_BRIDGE_ARGS,
    CHECKPOINT_INTERVAL,
    EPOCH_LENGTH,
    B3BridgeChainMixin,
    FreshVoteShuttle,
    hx,
    read_fresh_key_store,
    regtest_chain_domain,
    rpc_refusal,
    start_refused,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
)

# -- schedule (regtest plan §3.3; finality dependency §6.4) ------------------
FRESH_ACTIVATION = 262  # R_f
FRESH_GENESIS = FRESH_ACTIVATION - 1  # Set'_0 = Committee(Snapshot(R_f - 1))
KEY_HEIGHT = 241  # R_key: genesis bindings count in [R_key, R_f - 1]
HORIZON = 40  # -b3reorghorizon, the erasure depth (FreshKeyErasureHorizon)
STAKES = (40, 30, 20, 10, 10)  # n0 .. n4
MIN_FINALITY_SET = 4
FRESH_ARGS = [
    f"-b3freshlineageheight={FRESH_ACTIVATION}",
    f"-b3freshkeyheight={KEY_HEIGHT}",
    f"-b3reorghorizon={HORIZON}",
]
FIRST_FRESH_CHECKPOINT = FRESH_GENESIS + CHECKPOINT_INTERVAL
BACKUP_NODE = len(STAKES)  # n5
BACKUP_WALLET = "n0_backup"
# The signer may run its pass a few blocks after the erasure height.
ERASURE_SLACK = 5
assert f"-b3minfinalityset={MIN_FINALITY_SET}" in B3_BRIDGE_ARGS
assert KEY_HEIGHT < FRESH_GENESIS

RPC_MISC_ERROR = -1
RPC_WALLET_ERROR = -4
RPC_INVALID_PARAMETER = -8

# -- refusal messages (src/chainparams.cpp, src/kernel/chainparams.cpp) ------
REQUIRES_FLOWMESH = "-b3freshkeyheight requires -b3modernregtest and -b3flowmeshtest (regtest-only)"
REQUIRES_LINEAGE = "-b3freshkeyheight requires -b3freshlineageheight"
OUT_OF_RANGE = (f"Invalid value for -b3freshkeyheight: {FRESH_GENESIS} must be in [1, {FRESH_ACTIVATION - 2}] "
                f"(above the legacy final height 0 and below R_f - 1 = {FRESH_GENESIS})")
ZERO = "Invalid value for -b3freshkeyheight: must be in [1, 2147483647]"
NEGATED = "Negating of -b3freshkeyheight is meaningless and therefore forbidden"

# Fields of getfinalitylineage every node must agree on (local_signer and the
# vote pool size are node-local).
LINEAGE_VIEW_KEYS = (
    "configured", "staging_signer", "activation_height", "genesis_height", "chain_domain", "tip_height",
    "state_available", "bootstrapped", "lineage_id", "genesis_set_hash", "epoch", "epoch_starts",
    "handover_certified", "lineage_broken", "genesis_set", "previous_set", "current_set", "next_set",
    "finalized", "history", "fresh_keys",
)

ERASED_LOG = re.compile(r"erased fresh key ([0-9a-f]{96}) of fresh epoch (\d+) at height (\d+)")
CATCH_UP_LOG = re.compile(r"moved the ancestry lock from fresh checkpoint (\d+) \(epoch (\d+)\) to the included "
                          r"certificate for fresh checkpoint (\d+) \(epoch (\d+)\)")


def lineage_view(node):
    info = node.getfinalitylineage()
    view = {key: info.get(key) for key in LINEAGE_VIEW_KEYS}
    view["tip"] = node.getbestblockhash()
    return view


def committees_of(view):
    """{epoch: {validator_key: (bls_pubkey, weight)}} from fresh_keys.committees."""
    out = {}
    for committee in view["fresh_keys"]["committees"]:
        members = {m["validator_key"]: (m["bls_pubkey"], m["weight"]) for m in committee["members"]}
        assert_equal(len(members), len(committee["members"]))
        assert committee["epoch"] not in out
        out[committee["epoch"]] = members
    return out


def committee_keys(members):
    return {pubkey for pubkey, _ in members.values()}


def voids_of(view):
    return {(b["validator_key"], b["epoch"]) for b in view["fresh_keys"]["bindings"] if b["void"]}


def bindings_for(view, epoch):
    """{validator_key: [(bls_pubkey, height, counted, void), ...]} naming epoch."""
    out = {}
    for b in view["fresh_keys"]["bindings"]:
        if b["epoch"] == epoch:
            out.setdefault(b["validator_key"], []).append((b["bls_pubkey"], b["height"], b["counted"], b["void"]))
    return out


def key_entry(report, pubkey):
    """The getfreshfinalitykeys keys[] entry of pubkey."""
    entries = [key for key in report["keys"] if key["bls_pubkey"] == pubkey]
    assert_equal(len(entries), 1)
    return entries[0]


def store_record(store, pubkey):
    records = [record for record in store["records"] if record["bls_pubkey"] == pubkey]
    assert_equal(len(records), 1)
    return records[0]


class B3FreshLineageKeysTest(FreshVoteShuttle, B3BridgeChainMixin, BitcoinTestFramework):
    bridge_validator_count = len(STAKES)

    def set_test_params(self):
        self.num_nodes = len(STAKES) + 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        args = list(B3_BRIDGE_ARGS) + FRESH_ARGS
        # n0-n4 are the validators; n5 checks refusals, then holds the backup.
        self.add_nodes(self.num_nodes, [list(args) for _ in range(self.num_nodes)])
        self.check_knob_refusals()
        self.start_bridge_validators(extra_args=FRESH_ARGS)

    # -- 1. startup refusals ------------------------------------------------
    def check_knob_refusals(self):
        self.log.info("n5 refuses -b3freshkeyheight outside its exact configuration")
        n5 = self.nodes[BACKUP_NODE]
        lineage = [f"-b3freshlineageheight={FRESH_ACTIVATION}"]
        key = [f"-b3freshkeyheight={KEY_HEIGHT}"]
        without_flowmesh = [arg for arg in B3_BRIDGE_ARGS if arg != "-b3flowmeshtest"] + key
        refusals = {
            "without -b3flowmeshtest": start_refused(n5, without_flowmesh, REQUIRES_FLOWMESH),
            "without -b3freshlineageheight": start_refused(n5, list(B3_BRIDGE_ARGS) + key, REQUIRES_LINEAGE),
            "R_key = R_f - 1": start_refused(
                n5, list(B3_BRIDGE_ARGS) + lineage + [f"-b3freshkeyheight={FRESH_GENESIS}"], OUT_OF_RANGE),
            "R_key = 0": start_refused(n5, list(B3_BRIDGE_ARGS) + lineage + ["-b3freshkeyheight=0"], ZERO),
            "negated knob": start_refused(n5, list(B3_BRIDGE_ARGS) + lineage + ["-nob3freshkeyheight"], NEGATED),
        }
        self.evidence.assertion("-b3freshkeyheight refused on n5", expected={
            "without -b3flowmeshtest": REQUIRES_FLOWMESH, "without -b3freshlineageheight": REQUIRES_LINEAGE,
            "R_key = R_f - 1": OUT_OF_RANGE, "R_key = 0": ZERO, "negated knob": NEGATED}, actual=refusals)

    # -- test ---------------------------------------------------------------
    def run_test(self):
        self.domain = regtest_chain_domain()
        self.v1_progress = []
        validators = self.bridge_validators()
        assert_equal([node.index for node in validators], list(range(len(STAKES))))

        self.log.info("Corridor, five stakes and live V1 BLS finality below R_key")
        self.bring_up_validators(KEY_HEIGHT - 30, stakes=STAKES)
        self.freeze()
        self.vk = {node.index: node.getstakinginfo()["validator_key"] for node in validators}
        self.stake = {self.vk[i]: STAKES[i] for i in range(len(STAKES))}
        self.v1_keys = self.v1_set_keys()
        assert_equal(set(self.v1_keys), set(self.vk.values()))
        self.record_v1("validators live")

        self.check_before_key_height()
        self.bind_genesis_keys()
        self.start_backup_holder()
        self.check_fresh_genesis()
        self.bind_epoch2_keys_with_backup_first()
        self.check_void_seat_after_rotation()
        self.bind_epoch3_keys()
        self.check_epoch2_committee()
        self.check_restored_backup()
        self.check_genesis_key_erasure()
        self.check_restart_and_reindex()
        self.check_lineages_continue()

    # -- helpers --------------------------------------------------------------
    def v1_set_keys(self):
        info = self.nodes[0].getfinalityset()
        assert "next_offset" not in info
        assert_equal(info["validator_count"], len(STAKES))
        return {member["validator_key"]: member["bls_pubkey"] for member in info["members"]}

    def record_v1(self, label):
        """V1 finality is live and advanced since the previous phase."""
        statuses = [node.getfinalitystatus() for node in self.bridge_validators()]
        for status in statuses:
            assert_equal(status["bootstrapped"], True)
            assert_equal(status["lineage_broken"], False)
            assert_equal(status["finalized"], statuses[0]["finalized"])
        finalized = statuses[0]["finalized"]["height"]
        if self.v1_progress:
            assert_greater_than(finalized, self.v1_progress[-1][1])
        self.v1_progress.append((label, finalized, self.nodes[0].getblockcount()))
        self.evidence_step(f"V1 finalized: {label}", v1_finalized=finalized)

    def agreed_view(self):
        """Every running node (n5 included) exports the same lineage state."""
        views = [lineage_view(node) for node in self.bridge_nodes()]
        for view in views[1:]:
            assert_equal(view, views[0])
        view = views[0]
        assert_equal(view["bootstrapped"], True)
        assert_equal(view["lineage_broken"], False)
        assert_equal(view["fresh_keys"]["state_available"], True)
        assert_equal(view["fresh_keys"]["key_height"], KEY_HEIGHT)
        return view

    @staticmethod
    def lineage_epoch(node):
        info = node.getfinalitylineage()
        return info["epoch"] if info.get("bootstrapped") else -1

    @staticmethod
    def fresh_finalized(node):
        return node.getfinalitylineage().get("finalized") or {"height": -1, "epoch": -1}

    def epoch_settled(self, epoch):
        """Every validator is in fresh epoch >= epoch and finalized a
        checkpoint of that epoch."""
        return all(self.lineage_epoch(node) >= epoch and self.fresh_finalized(node)["epoch"] >= epoch
                   for node in self.bridge_validators())

    def fresh_store(self, wallet):
        report = wallet.getfreshfinalitykeys()
        store = read_fresh_key_store(report["store_path"])
        assert_equal(store["chain_domain"], self.domain)
        assert_equal(store["validator_key"], report["validator_key"])
        return store

    def check_open_window(self, epoch, window_start):
        for node in self.bridge_nodes():
            fresh = node.getfinalitylineage()["fresh_keys"]
            assert_equal((fresh["next_binding_window"], fresh["next_binding_epoch"], fresh["window_start"]),
                         ("open", epoch, window_start))

    def erasure_heights(self, node):
        """{(bls_pubkey, target epoch): height} from node's signer log."""
        with open(node.debug_log_path, encoding="utf-8") as f:
            return {(m.group(1), int(m.group(2))): int(m.group(3)) for m in ERASED_LOG.finditer(f.read())}

    def first_checkpoint_settled(self, node):
        local = node.getfinalitylineage().get("local_signer") or {}
        return (self.fresh_finalized(node)["height"] >= FIRST_FRESH_CHECKPOINT and
                self.finalized_height(node) >= FIRST_FRESH_CHECKPOINT and
                local.get("last_signed_height", -1) >= FIRST_FRESH_CHECKPOINT)

    # -- 3. below R_key -------------------------------------------------------
    def check_before_key_height(self):
        self.log.info("Below R_key: no binding window, configuration only")
        n0 = self.nodes[0]
        tip = n0.getblockcount()
        assert_greater_than(KEY_HEIGHT, tip + 1)
        refusal = rpc_refusal(RPC_MISC_ERROR, f"fresh bindings start at height {KEY_HEIGHT} (R_key)",
                              n0.bindfreshfinalitykey)
        assert f"(height {tip + 1}, not-active)" in refusal, refusal
        for node in self.bridge_validators():
            report = node.getfreshfinalitykeys()
            assert_equal(report["configured"], True)
            assert_equal(report["validator_key"], self.vk[node.index])
            assert_equal((report["key_height"], report["activation_height"]), (KEY_HEIGHT, FRESH_ACTIVATION))
            assert_equal(hx(report["chain_domain"])[::-1], self.domain)
            assert_equal((report["state_available"], report["next_binding_window"], report["bootstrapped"]),
                         (True, "not-active", False))
            assert "next_binding_epoch" not in report
            for key in ("keys", "foreign_bindings", "missing_seats", "alerts"):
                assert_equal(report[key], [])
            # Nothing is created before the first key.
            assert not os.path.exists(report["store_path"]), report["store_path"]
            info = node.getfinalitylineage()
            assert_equal((info["configured"], info["staging_signer"], info.get("bootstrapped", False)),
                         (True, True, False))
            assert_equal(info["fresh_keys"], {"key_height": KEY_HEIGHT, "state_available": True,
                                              "next_binding_window": "not-active", "bindings": [],
                                              "committees": []})
        self.evidence.assertion("bindfreshfinalitykey refused below R_key", actual=refusal, tip=tip)

    # -- 4. genesis bindings --------------------------------------------------
    def bind_genesis_keys(self):
        self.log.info("Between R_key and R_f - 1 every validator binds its genesis key")
        validators = self.bridge_validators()
        n0, n1 = self.nodes[0], self.nodes[1]
        self.resume()
        self.wait_until(lambda: n0.getblockcount() + 1 >= KEY_HEIGHT, timeout=300, check_interval=0.25)
        self.freeze()
        tip = n0.getblockcount()
        assert KEY_HEIGHT <= tip + 1 <= FRESH_GENESIS - 10, tip
        self.record_v1("R_key window open")
        self.check_open_window(0, KEY_HEIGHT)

        refusals = {
            "wrong epoch": rpc_refusal(RPC_INVALID_PARAMETER, "epoch 1 is not the next binding window",
                                       n0.bindfreshfinalitykey, 1),
            "reuse_key broadcast": rpc_refusal(RPC_INVALID_PARAMETER, "reuse_key requires broadcast=false",
                                               n0.bindfreshfinalitykey, reuse_key="v1"),
        }
        genesis = self.bind_fresh_keys(validators, 0)
        self.genesis_keys = {}
        for node, result in zip(validators, genesis):
            assert_equal((result["validator_key"], result["window_start"]), (self.vk[node.index], KEY_HEIGHT))
            self.genesis_keys[result["validator_key"]] = result["bls_pubkey"]
        assert_equal(len(set(self.genesis_keys.values())), len(STAKES))
        assert not set(self.genesis_keys.values()) & set(self.v1_keys.values())
        # A second key for one epoch would void the seat: refused.
        refusals["second key for epoch 0"] = rpc_refusal(RPC_WALLET_ERROR, "already holds key",
                                                         n1.bindfreshfinalitykey)
        # Never bound twice: n0's V1 key as a fresh key (a drill transaction).
        drill = n0.bindfreshfinalitykey(broadcast=False, reuse_key="v1")
        assert_equal((drill["key_source"], drill["status"], drill["epoch"]), ("v1", "NOT_BROADCAST", 0))
        assert_equal(drill["bls_pubkey"], self.v1_keys[self.vk[0]])
        accept = n0.testmempoolaccept([drill["hex"]])[0]
        assert_equal((accept["allowed"], accept["reject-reason"]), (False, "fresh-key-ever-bound"))
        refusals["V1 key as fresh key"] = accept["reject-reason"]

        # The backup is taken after n0's genesis binding: it holds n0's
        # identity and that transaction, never a fresh secret.
        self.backup_path = os.path.join(self.options.tmpdir, "n0_wallet.bak")
        n0.backupwallet(self.backup_path)

        heights = self.mine_txids([result["txid"] for result in genesis], tip + 1)
        assert max(heights) <= FRESH_GENESIS, heights
        view = self.agreed_view_unbootstrapped()
        bound = bindings_for(view, 0)
        assert_equal(bound, {result["validator_key"]: [(result["bls_pubkey"], height, True, False)]
                             for result, height in zip(genesis, heights)})
        for node, result in zip(validators, genesis):
            store = self.fresh_store(node)
            assert_equal(len(store["records"]), 1)
            record = store["records"][0]
            assert_equal((record["epoch"], record["state"], record["bls_pubkey"], record["bind_txid"]),
                         (0, "held", result["bls_pubkey"], result["txid"]))
            assert any(record["secret"])
            # The wallet backup holds no fresh secret.
            with open(self.backup_path, "rb") as f:
                assert record["secret"] not in f.read()
        self.evidence.assertion("genesis bindings and refusals", actual={
            "refusals": refusals, "heights": heights, "keys": self.genesis_keys})

    def agreed_view_unbootstrapped(self):
        views = [lineage_view(node) for node in self.bridge_nodes()]
        for view in views[1:]:
            assert_equal(view, views[0])
        return views[0]

    # -- n5: the restored backup ------------------------------------------------
    def start_backup_holder(self):
        self.log.info("n5 restores n0's wallet backup; it never stakes")
        n5 = self.nodes[BACKUP_NODE]
        self.start_node(BACKUP_NODE)
        self.connect_nodes(BACKUP_NODE, 0)
        self.sync_bridge_blocks(timeout=300)
        n5.restorewallet(BACKUP_WALLET, self.backup_path)
        self.w5 = n5.get_wallet_rpc(BACKUP_WALLET)
        n5.syncwithvalidationinterfacequeue()
        report = self.w5.getfreshfinalitykeys()
        vk0, g0 = self.vk[0], self.genesis_keys[self.vk[0]]
        assert_equal(report["validator_key"], vk0)
        assert_equal(report["keys"], [])
        assert_equal([(b["epoch"], b["bls_pubkey"]) for b in report["foreign_bindings"]], [(0, g0)])
        assert any(g0 in alert and "not in this node's fresh key store" in alert for alert in report["alerts"])
        assert not os.path.exists(report["store_path"])
        assert_equal(self.w5.getstakinginfo()["staking"]["running"], False)

    # -- 5. Set'_0 on fresh keys ------------------------------------------------
    def check_fresh_genesis(self):
        self.log.info("Set'_0 seats the fresh keys; V2 certificates finalize by gossip")
        validators = self.bridge_validators()
        n0 = self.nodes[0]
        self.resume()
        self.shuttle_until(lambda: all(self.first_checkpoint_settled(node) for node in validators), timeout=600)
        self.freeze()
        view = self.agreed_view()
        self.lineage_id = view["lineage_id"]
        committees = committees_of(view)
        expected = {vk: (pubkey, self.stake[vk]) for vk, pubkey in self.genesis_keys.items()}
        assert_equal(sorted(committees), [0, 1])
        assert_equal(committees[0], expected)
        assert_equal(committees[1], expected)
        v1 = n0.getfinalityset()
        genesis_set = view["genesis_set"]
        assert_equal((genesis_set["epoch"], genesis_set["ruleset_version"], genesis_set["validator_count"],
                      genesis_set["total_weight"]), (0, 2, len(STAKES), sum(STAKES)))
        # Same validators as the V1 set, other keys: another aggregate key and
        # another member tree.
        assert hx(genesis_set["aggregate_pubkey"]) != hx(v1["aggregate_pubkey"])
        assert hx(genesis_set["members_root"]) != hx(v1["members_root"])
        assert_equal(view["next_set"]["aggregate_pubkey"], genesis_set["aggregate_pubkey"])
        assert_equal(view["next_set"]["members_root"], genesis_set["members_root"])

        finalized = view["finalized"]
        cert = n0.getfinalitylineagecertificate(finalized["certified_at"])
        assert_equal(cert["signing_set"], genesis_set)
        assert_greater_than_or_equal(cert["signed_weight"], genesis_set["quorum_weight"])
        gossip = self.assert_fresh_gossip(validators)
        for node in validators:
            local = node.getfinalitylineage()["local_signer"]
            assert_equal(local["last_error"], "")
            assert_equal(local["fresh_keys"], [{"epoch": 0, "bls_pubkey": self.genesis_keys[self.vk[node.index]],
                                                "held": True, "seated_epochs": [0, 1]}])
            assert_equal((local["missing_seats"], local["alerts"]), ([], []))
        self.first_fresh = finalized
        self.record_v1("Set'_0 certified")
        self.evidence.assertion("Set'_0 on fresh keys, disjoint from V1", actual={
            "set0": committees[0], "v1_keys": self.v1_keys, "finalized": finalized,
            "signer_count": cert["signer_count"], "finsigv2_bytes_received": gossip})

    # -- 6. a double binding voids the seat -------------------------------------
    def bind_epoch2_keys_with_backup_first(self):
        self.log.info("Fresh epoch 0: the old backup binds n0's epoch-2 seat first, then every validator binds")
        validators = self.bridge_validators()
        n0 = self.nodes[0]
        tip = n0.getblockcount()
        view = self.agreed_view()
        assert_equal(view["epoch"], 0)
        assert tip + 1 <= FRESH_ACTIVATION + EPOCH_LENGTH - 10, tip
        self.check_open_window(2, FRESH_ACTIVATION)

        stolen, = self.bind_fresh_keys([self.w5], 2)
        assert_equal(stolen["validator_key"], self.vk[0])
        self.stolen_key = stolen["bls_pubkey"]
        # n0's wallet must see the backup's spend before it funds its own.
        self.synchronize_mempools()
        own = self.bind_fresh_keys(validators, 2)
        self.epoch2_keys = {result["validator_key"]: result["bls_pubkey"] for result in own}
        heights = self.mine_txids([stolen["txid"]] + [result["txid"] for result in own], tip + 1)
        assert max(heights) < FRESH_ACTIVATION + EPOCH_LENGTH, heights
        view = self.agreed_view()
        assert_equal(view["epoch"], 0)
        bound = bindings_for(view, 2)
        vk0 = self.vk[0]
        assert_equal(sorted(bound[vk0]), sorted([(self.stolen_key, heights[0], True, True),
                                                 (self.epoch2_keys[vk0], heights[1], True, True)]))
        for i, node in enumerate(validators[1:], start=1):
            assert_equal(bound[self.vk[i]], [(self.epoch2_keys[self.vk[i]], heights[1 + i], True, False)])
        assert_equal(voids_of(view), {(vk0, 2)})

        report = n0.getfreshfinalitykeys()
        assert_equal([(b["epoch"], b["bls_pubkey"], b["counted"], b["void"]) for b in report["foreign_bindings"]],
                     [(2, self.stolen_key, True, True)])
        assert any(self.stolen_key in alert and "not in this node's fresh key store" in alert
                   for alert in report["alerts"])
        assert any("seat for fresh epoch 2 is void" in alert for alert in report["alerts"])
        own_key = key_entry(report, self.epoch2_keys[vk0])
        assert_equal((own_key["state"], own_key["counted"], own_key["void"], own_key["seated_epochs"]),
                     ("held", True, True, []))
        backup = self.w5.getfreshfinalitykeys()
        assert_equal([(k["epoch"], k["bls_pubkey"], k["state"], k["void"]) for k in backup["keys"]],
                     [(2, self.stolen_key, "held", True)])
        assert_equal(sorted((b["epoch"], b["bls_pubkey"]) for b in backup["foreign_bindings"]),
                     sorted([(0, self.genesis_keys[vk0]), (2, self.epoch2_keys[vk0])]))
        self.evidence.assertion("double binding of n0 for epoch 2", actual={
            "backup_key": self.stolen_key, "own_key": self.epoch2_keys[vk0], "heights": heights,
            "n0_alerts": report["alerts"]})

    def check_void_seat_after_rotation(self):
        self.log.info("After the rotation into epoch 1 Set'_2 seats four members without n0")
        validators = self.bridge_validators()
        n0 = self.nodes[0]
        vk0 = self.vk[0]
        self.resume()
        self.shuttle_until(lambda: self.epoch_settled(1), timeout=600)
        self.freeze()
        view = self.agreed_view()
        committees = committees_of(view)
        assert_equal(sorted(committees), [0, 1, 2])
        assert_equal(committees[1], committees[0])
        assert_equal(committee_keys(committees[1]), set(self.genesis_keys.values()))
        expected2 = {vk: (pubkey, self.stake[vk]) for vk, pubkey in self.epoch2_keys.items() if vk != vk0}
        assert_equal(committees[2], expected2)
        assert_equal(len(committees[2]), MIN_FINALITY_SET)
        assert not committee_keys(committees[2]) & (set(self.genesis_keys.values()) | set(self.v1_keys.values()))
        assert_equal((view["next_set"]["epoch"], view["next_set"]["validator_count"],
                      view["next_set"]["total_weight"]), (2, len(STAKES) - 1, sum(STAKES[1:])))
        assert_equal(voids_of(view), {(vk0, 2)})
        assert_greater_than(view["finalized"]["height"], self.first_fresh["height"])
        alerts = n0.getfinalitylineage()["local_signer"]["alerts"]
        assert any(self.stolen_key in alert for alert in alerts), alerts
        assert any("seat for fresh epoch 2 is void" in alert for alert in alerts), alerts
        for node in validators[1:]:
            assert_equal(node.getfinalitylineage()["local_signer"]["alerts"], [])
        self.record_v1("epoch 1 certified")
        self.evidence.assertion("Set'_2 without n0 (void seat)", actual={
            "set2": committees[2], "voids": sorted(voids_of(view)), "n0_signer_alerts": alerts,
            "epoch_starts": view["epoch_starts"], "fresh_finalized": view["finalized"]})

    # -- 7. epoch-3 keys ----------------------------------------------------------
    def bind_epoch3_keys(self):
        self.log.info("Fresh epoch 1: every validator binds a new epoch-3 key")
        validators = self.bridge_validators()
        n0 = self.nodes[0]
        vk0 = self.vk[0]
        view = self.agreed_view()
        assert_equal(view["epoch"], 1)
        start1 = view["epoch_starts"][1]
        tip = n0.getblockcount()
        assert tip + 1 <= start1 + EPOCH_LENGTH - 10, (tip, start1)
        self.check_open_window(3, start1)
        drill = n0.bindfreshfinalitykey(broadcast=False, reuse_key="epoch:0")
        assert_equal((drill["key_source"], drill["epoch"], drill["bls_pubkey"]),
                     ("stored", 3, self.genesis_keys[vk0]))
        accept = n0.testmempoolaccept([drill["hex"]])[0]
        assert_equal((accept["allowed"], accept["reject-reason"]), (False, "fresh-key-ever-bound"))
        own = self.bind_fresh_keys(validators, 3)
        self.epoch3_keys = {result["validator_key"]: result["bls_pubkey"] for result in own}
        heights = self.mine_txids([result["txid"] for result in own], tip + 1)
        assert max(heights) < start1 + EPOCH_LENGTH, heights
        view = self.agreed_view()
        bound = bindings_for(view, 3)
        assert_equal(bound, {vk: [(pubkey, height, True, False)]
                             for (vk, pubkey), height in zip(self.epoch3_keys.items(), heights)})
        self.evidence.assertion("epoch-3 bindings; stored genesis key refused", actual={
            "stored genesis key": accept["reject-reason"], "heights": heights})

    def check_epoch2_committee(self):
        self.log.info("Set'_2 (four members) certifies; Set'_3 seats all five on new keys")
        validators = self.bridge_validators()
        n0 = self.nodes[0]
        vk0 = self.vk[0]
        self.resume()
        self.shuttle_until(lambda: self.epoch_settled(2), timeout=600)
        self.freeze()
        view = self.agreed_view()
        committees = committees_of(view)
        assert_equal(sorted(committees), [1, 2, 3])
        assert_equal(committees[3], {vk: (pubkey, self.stake[vk]) for vk, pubkey in self.epoch3_keys.items()})
        earlier = (set(self.genesis_keys.values()) | set(self.epoch2_keys.values()) |
                   set(self.v1_keys.values()) | {self.stolen_key})
        assert not committee_keys(committees[3]) & earlier
        finalized = view["finalized"]
        assert_equal(finalized["epoch"], 2)
        cert = n0.getfinalitylineagecertificate(finalized["certified_at"])
        assert_equal(cert["signing_set"], view["current_set"])
        assert_equal(cert["validator_count"], len(STAKES) - 1)

        # n0's voided epoch-2 key was never seated: erased once Set'_2 was
        # fixed (start_1) and HORIZON deep.
        start1 = view["epoch_starts"][1]
        store = self.fresh_store(n0)
        voided = store_record(store, self.epoch2_keys[vk0])
        assert_equal((voided["state"], voided["secret"]), ("erased", bytes(32)))
        height = self.erasure_heights(n0)[(self.epoch2_keys[vk0], 2)]
        assert start1 + HORIZON <= height <= start1 + HORIZON + ERASURE_SLACK, (height, start1)
        assert_equal(key_entry(n0.getfreshfinalitykeys(), self.epoch2_keys[vk0])["state"], "erased")
        # Every other key of every validator is still held.
        for node in validators:
            for record in self.fresh_store(node)["records"]:
                if record["bls_pubkey"] != self.epoch2_keys[vk0]:
                    assert_equal(record["state"], "held")
        self.committees_epoch2 = committees
        self.record_v1("epoch 2 certified")
        self.evidence.assertion("Set'_3 on new keys; n0's void key erased", actual={
            "set3": committees[3], "fresh_finalized": finalized, "void_key_erased_at": height,
            "epoch_starts": view["epoch_starts"]})

    # -- 8. the restored backup ----------------------------------------------------
    def check_restored_backup(self):
        self.log.info("The restored backup holds no secret of a seated key of epoch >= 2")
        vk0 = self.vk[0]
        committees = self.committees_epoch2
        seated = set()
        for epoch, members in committees.items():
            if epoch >= 2:
                seated |= committee_keys(members)
        store = self.fresh_store(self.w5)
        assert_equal([(r["epoch"], r["bls_pubkey"], r["state"]) for r in store["records"]],
                     [(2, self.stolen_key, "held")])
        assert self.stolen_key not in seated
        report = self.w5.getfreshfinalitykeys()
        for key in report["keys"]:
            assert_equal(key["seated_epochs"], [])
        assert {"epoch": 3, "bls_pubkey": self.epoch3_keys[vk0]} in report["missing_seats"], report["missing_seats"]
        # The V1 key the backup can still derive seats nothing fresh.
        derived = self.w5.bindfreshfinalitykey(broadcast=False, reuse_key="v1")
        assert_equal((derived["key_source"], derived["bls_pubkey"]), ("v1", self.v1_keys[vk0]))
        every_fresh_key = set()
        for members in committees.values():
            every_fresh_key |= committee_keys(members)
        every_fresh_key |= set(self.genesis_keys.values())
        assert derived["bls_pubkey"] not in every_fresh_key
        assert_equal(self.w5.getstakinginfo()["staking"]["running"], False)
        self.evidence.assertion("restored backup cannot sign for epoch >= 2", actual={
            "backup_store": [(r["epoch"], r["bls_pubkey"], r["state"]) for r in store["records"]],
            "missing_seats": report["missing_seats"], "derivable_v1_key": derived["bls_pubkey"]})

    # -- 9. erasure of the genesis keys ---------------------------------------------
    def check_genesis_key_erasure(self):
        self.log.info("The genesis keys are held through start_2 + HORIZON - 1 and erased at start_2 + HORIZON")
        validators = self.bridge_validators()
        n0 = self.nodes[0]
        start2 = self.agreed_view()["epoch_starts"][2]
        self.resume()
        self.wait_until(lambda: n0.getblockcount() >= start2 + HORIZON - 5, timeout=300, check_interval=0.25)
        self.freeze()
        held_tip = n0.getblockcount()
        assert held_tip <= start2 + HORIZON - 1, (held_tip, start2)
        secrets = {}
        for node in validators:
            genesis_key = self.genesis_keys[self.vk[node.index]]
            record = store_record(self.fresh_store(node), genesis_key)
            assert_equal(record["state"], "held")
            secrets[node.index] = record["secret"]
            assert_equal(key_entry(node.getfreshfinalitykeys(), genesis_key)["state"], "held")

        def erased_everywhere():
            return (all(key_entry(node.getfreshfinalitykeys(), self.genesis_keys[self.vk[node.index]])["state"] ==
                        "erased" for node in validators) and self.epoch_settled(3))

        self.resume()
        self.wait_until(erased_everywhere, timeout=600, check_interval=1)
        self.freeze()
        view = self.agreed_view()
        tip = view["tip_height"]
        assert_greater_than_or_equal(tip, start2 + HORIZON)
        committees = committees_of(view)
        epoch = view["epoch"]
        assert_greater_than_or_equal(epoch, 3)
        heights = {}
        for node in validators:
            vk = self.vk[node.index]
            genesis_key = self.genesis_keys[vk]
            store = self.fresh_store(node)
            record = store_record(store, genesis_key)
            assert_equal((record["state"], record["secret"]), ("erased", bytes(32)))
            assert secrets[node.index] not in store["raw"]
            heights[f"node{node.index}"] = self.erasure_heights(node)[(genesis_key, 0)]
            assert start2 + HORIZON <= heights[f"node{node.index}"] <= start2 + HORIZON + ERASURE_SLACK, heights
            # Current and next keys stay held, and so do Set'_2's keys:
            # retired only at start_3, they go HORIZON blocks later.
            for k in (epoch, epoch + 1):
                if vk in committees[k]:
                    assert_equal(store_record(store, committees[k][vk][0])["state"], "held")
            if epoch == 3 and vk in committees[2]:
                assert_equal(store_record(store, committees[2][vk][0])["state"], "held")
        if epoch == 3:
            # No epoch-4 binding was made: Set'_4 carries Set'_3 over.
            assert_equal(committees[4], committees[3])
            assert_equal(committees[3], {vk: (pubkey, self.stake[vk]) for vk, pubkey in self.epoch3_keys.items()})
        cert = n0.getfinalitylineagecertificate(view["finalized"]["certified_at"])
        assert_equal(cert["validator_count"], len(STAKES))
        assert_greater_than_or_equal(view["finalized"]["epoch"], 3)
        # n0 sat out epoch 2 (void seat): its fresh journal lock moved past
        # that epoch to an included epoch-2 certificate, so it signs again
        # (Set'_3's quorum needs its weight).
        with open(n0.debug_log_path, encoding="utf-8") as f:
            caught_up = [tuple(int(v) for v in m) for m in CATCH_UP_LOG.findall(f.read())]
        assert caught_up, "n0's fresh-lineage journal never moved past the epoch it sat out"
        assert_equal((caught_up[0][1], caught_up[0][3]), (1, 2))
        local = n0.getfinalitylineage()["local_signer"]
        assert_equal(local["last_error"], "")
        assert_greater_than_or_equal(local["last_signed_height"], view["epoch_starts"][3])
        self.record_v1("genesis keys erased")
        self.evidence.assertion("genesis keys erased at start_2 + HORIZON", expected=start2 + HORIZON,
                                actual=heights, held_at=held_tip, tip=tip, epoch=epoch,
                                epoch_starts=view["epoch_starts"], n0_journal_catch_up=caught_up)

    # -- 11. restart and reindex ------------------------------------------------------
    def node_state(self, node):
        return {"lineage": lineage_view(node), "v1_finalized": node.getfinalitystatus()["finalized"],
                "keys": node.getfreshfinalitykeys()}

    def check_restart_and_reindex(self):
        n4 = self.nodes[4]
        before = self.node_state(n4)
        assert_equal(before["lineage"], self.agreed_view())
        self.n4_signed_before = n4.getfinalitylineage()["local_signer"]["last_signed_height"]
        assert_equal(voids_of(before["lineage"]), {(self.vk[0], 2)})
        self.log.info("n4 restarts and reproduces committees, bindings and voids")
        self.restart_node(4)
        self.connect_nodes(4, 3)
        n4.syncwithvalidationinterfacequeue()
        assert_equal(self.node_state(n4), before)
        self.log.info("n4 reindexes and reproduces committees, bindings and voids")
        self.restart_node(4, extra_args=list(n4.extra_args) + ["-reindex"])
        self.wait_until(lambda: n4.getbestblockhash() == before["lineage"]["tip"], timeout=300)
        self.connect_nodes(4, 3)
        n4.syncwithvalidationinterfacequeue()
        assert_equal(self.node_state(n4), before)
        self.evidence_step("n4 restart and reindex agree", lineage_id=self.lineage_id,
                           epoch=before["lineage"]["epoch"], voids=sorted(voids_of(before["lineage"])))

    def check_lineages_continue(self):
        self.log.info("Both lineages keep finalizing with n4 back; gossip only")
        validators = self.bridge_validators()
        n4 = self.nodes[4]
        fresh_before = self.fresh_finalized(self.nodes[0])["height"]
        signed_before = self.n4_signed_before
        self.resume()
        self.shuttle_until(lambda: all(self.fresh_finalized(node)["height"] > fresh_before for node in validators) and
                           (n4.getfinalitylineage().get("local_signer") or {}).get(
                               "last_signed_height", -1) > signed_before and
                           self.finalized_height(self.nodes[0]) > self.v1_progress[-1][1], timeout=300)
        self.freeze()
        self.agreed_view()
        self.record_v1("after n4 restart")
        gossip = self.assert_fresh_gossip(validators)
        for node in validators:
            assert_equal(node.getfinalitylineage()["local_signer"]["last_error"], "")
        assert_equal(self.fresh_vote_results, {})
        self.evidence.assertion("V1 finalized advanced between every phase", actual=self.v1_progress)
        self.evidence_step("fresh key checks complete", finsigv2_bytes_received=gossip,
                           vote_rpc_calls=dict(self.fresh_vote_results))


if __name__ == '__main__':
    B3FreshLineageKeysTest(__file__).main()
