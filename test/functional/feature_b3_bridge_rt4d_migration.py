#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""RT4d: a B-23 vault migration from v to its successor v' (node changes rev 11 §7.2, §7.6, §8.4).

REGTEST ONLY, the environment of RT4a (feature_b3_bridge_rt4a_round_trip.py):
four B3 regtest validators linked only to each other, one anvil on
127.0.0.1 (chain 31337, never forked, five dev accounts), the synthetic
sync-committee tool, forge --offline with its output outside the source
tree, fresh datadirs. Fixture I, START_CERTIFICATE.

The pin file is written before the validators first start and holds, with
v's v2 section, the successor v' (v2.successor): the CREATE address of anvil
account 4 at nonce 0, v as predecessor, v's bUSD V2 AssetId and the
migration height h_m = V + 60 = 341. Everything else v' inherits from v.

1. RT4a steps 1-5; getbridgev2info.successor equals the pins: inactive,
   its entry id differs from the launch id, retired false, deposit_vault v,
   unpinned also names successor_vault_runtime_code_hash.
2. B3VaultV2 is deployed in successor mode (the static Config tuple:
   verifier, its code hash, chain domain, token, token code hash, v's
   FORWARDER_FACTORY, MAX_DEPOSIT_RAW, predecessor v, v.B3_ASSET_ID()) at
   the pinned address: B3_ASSET_ID is the node's AssetId, PREDECESSOR is v.
   A Config naming another AssetId reverts BadConfig.
3. RT4a deposit, carriers and mint into v, below h_m. Before h_m
   buildbridgeobservationv2 18 and 19, the observer (dry run, subject 18)
   and submitbridgecarrier of successor.sweep.payload_hex are refused.
4. Shuttled to tip >= h_m - 1 within the C3 margin: v is retired,
   deposit_vault and getbridgedepositrecipient name v', the observation plan
   lists 18 and 19; with no launch RESERVE_E yet the sweep amount (v's
   reserve capped by its wind-down headroom) is not reported; with no
   observation both gates are NOT_READY at E1 and bridgewithdraw is refused.
5. Subjects 1-3 observed: v's gate but for retirement (successor.sweep.gate)
   is READY, while burn_gate (v' rows) is NOT_READY at E7 (no
   TOKEN_FLAGS_SUCCESSOR). The kind 16 record equals the Python encoding; it
   is mined at h1 and its leaf equals leaf_v2(asset, 0, 31337, v, token, v',
   DEPOSIT_RAW, h1) and v.withdrawalLeaf(w). v's reserve is 0 and burned
   DEPOSIT_RAW; v' transit DEPOSIT_RAW, credited 0. A second sweep has
   nothing to sweep.
6. A certificate covering h1 is relayed; getbridgewithdrawalproofv2(sweep,
   1, inclusion) is identical on every node; v.release pays v' once, the
   second release reverts AlreadyReleased; v locked = 0.
7. Subjects 1, 2, 3, 18 and 19 observed before absorb(): v' credited 0,
   transit unchanged, no conflict, both invariant R margins >= 0; burn_gate
   is READY on v' rows but bridgewithdraw is refused (check 2, v' reserve 0).
   v' has no storage yet (empty storage root): anvil proves its slots with
   the single node 0x80, which buildbridgeobservationv2 records as the
   canonical empty proof.
8. v'.absorb(): absorbedTotal = locked = DEPOSIT_RAW. 1, 18, 19 observed: v'
   credited DEPOSIT_RAW, transit 0, burn_gate READY on v' rows (closes_at
   from subjects 1, 18 and 19).
9. A late deposit into v (id 1) mints under the launch entry id after h_m;
   after 1-3 are observed, sweep #2 appends leaf 1 (v to v', LATE).
10. 1, 18, 19 observed again (credits nothing more before absorb);
    bridgewithdraw SUCCESSOR_BURN_RAW appends leaf 2 naming v' with the
    shared AssetId.
11. A certificate covering both is relayed; leaf 1 released on v and leaf 2
    on v' pay exactly once (second releases revert, each leaf reverts
    UnknownRoot on the other vault); v'.absorb, then 18 observed: v'
    credited DEPOSIT_RAW + LATE.
12. A deposit into v' (its id 0) is proven against v''s logs and claimed
    under the successor entry id: v' minted and proven_deposits 1,
    getbridgeproofstatus successor_deposit claimed, the replay refused; a
    RESERVE_E_SUCCESSOR at cut 1 counts it in P_pre and invariant R holds
    at margin 0.
13. Final ledgers, Ethereum balances, getbridgeproofstatus of both vaults,
    and wallet bUSD V2 supply == the sum of reserve + transit.
14. RT4a's pin guard; n3 restart (replay) and n0 -reindex: getbridgev2info,
    successor included, is identical on every node.

Evidence: bridge_evidence.json.
"""

import os

from feature_b3_bridge_rt4a_round_trip import (
    BRIDGE_V2_BURN_HEIGHT,
    BRIDGE_V2_HEIGHT,
    BRING_UP_HEIGHT,
    BURN_FINALITY_LAG_MARGIN,
    DEPOSIT_RAW,
    ETH_RECIPIENT,
    FIRST_DEPOSIT_ID,
    INIT_CHECKPOINT,
    MAX_DEPOSIT_RAW,
    MAX_TX_GAS,
    RPC_MISC_ERROR,
    RPC_VERIFY_REJECTED,
    TOKEN_SUPPLY,
    UNPINNED,
    B3BridgeRT4aRoundTripTest,
    error_selector,
    gas_used,
    revert_data,
)
from feature_b3_bridge_rt4c_conflict import B3BridgeRT4cConflictTest
from test_framework.anvil import AnvilRPCError, deploy
from test_framework.b3bridge import (
    EMPTY_TRIE_ROOT,
    ETH_OBSERVATION_KEYS,
    ETH_OBSERVATION_KEYS_SUCCESSOR,
    ETH_OBSERVATION_SUBJECTS,
    ETH_OBSERVATION_SUBJECTS_SUCCESSOR,
    GATE_R_MEMPOOL_CLOSE_MARGIN,
    GATE_R_OBSERVATION_MAX_AGE,
    GATE_R_ROTATION_AGE_LIMIT,
    REGTEST_ETH_CHAIN_ID,
    VAULT_V2_RELEASE_SIGNATURE,
    VAULT_V2_WITHDRAWAL_LEAF_SIGNATURE,
    ObserverRunError,
    abi_call,
    build_direct_mint_plan,
    call_address,
    call_bytes32,
    call_uint,
    enc_address,
    enc_bytes32,
    enc_uint,
    encode_vault_sweep_record,
    encode_vault_v2_config,
    encode_vault_v2_release,
    encode_withdrawal_v2_tuple,
    find_deposits,
    fold_withdrawal_path,
    gate_view,
    hx,
    keccak256,
    leaf_v2,
    leaf_v2_preimage,
    observation_plan_v2,
    predict_create_address,
    recipient_v1,
    rpc_refusal,
    run_observer_once,
    selector,
    to_0x,
    verify_account_storage_proof,
    withdrawal_root,
    withdrawal_root_and_path,
)
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
)

# -- the B-23 successor (node changes §7.6) -----------------------------------
MIGRATION_HEIGHT = BRIDGE_V2_HEIGHT + 60  # h_m, pinned before the first start
assert MIGRATION_HEIGHT > BRIDGE_V2_HEIGHT and MIGRATION_HEIGHT >= BRIDGE_V2_BURN_HEIGHT
LATE_DEPOSIT_RAW = 5_000_000
LATE_DEPOSIT_ID = FIRST_DEPOSIT_ID + 1
SUCCESSOR_BURN_RAW = 10_000_000
assert SUCCESSOR_BURN_RAW <= DEPOSIT_RAW  # paid from the migrated reserve
SUCCESSOR_DEPOSIT_RAW = 7_000_000
SUCCESSOR_DEPOSIT_ID = 0  # v' numbers its own deposits from 0
# anvil account 4 deploys v' at nonce 0; accounts 0-3 are RT4a's roles.
SUCCESSOR_DEPLOYER_ACCOUNT = 4
ANVIL_ACCOUNTS = SUCCESSOR_DEPLOYER_ACCOUNT + 1
# getbridgev2info lists the successor's vault code hash as unpinned too (OI-3).
SUCCESSOR_UNPINNED = UNPINNED[:-1] + ["successor_vault_runtime_code_hash"] + UNPINNED[-1:]

SUBJECT_NAMES = {**ETH_OBSERVATION_SUBJECTS, **ETH_OBSERVATION_SUBJECTS_SUCCESSOR}
SUBJECT_KEYS = {**ETH_OBSERVATION_KEYS, **ETH_OBSERVATION_KEYS_SUCCESSOR}
RESERVE_SUCCESSOR, TOKEN_FLAGS_SUCCESSOR = sorted(ETH_OBSERVATION_SUBJECTS_SUCCESSOR)  # 0x12, 0x13
ALL_SUBJECTS = tuple(sorted(SUBJECT_NAMES))
# The burn gate of v' reads VERIFIER_E and v''s own RESERVE_E and TOKEN_FLAGS.
SUCCESSOR_GATE_SUBJECTS = (1, RESERVE_SUCCESSOR, TOKEN_FLAGS_SUCCESSOR)
LAUNCH_GATE_SUBJECTS = (1, 2, 3)
LEDGER_KEYS = ("minted", "burned", "credited", "transit", "reserve", "proven_deposits")

SWEEPS_INACTIVE = "bridge V2 vault sweeps are not active"
SUCCESSOR_OBSERVATIONS_INACTIVE = "Successor observations are not active for the next block"
NOTHING_TO_SWEEP = "bridge V2 vault sweep has nothing to sweep"
NO_VERIFIER_E = "no VERIFIER_E observation"
NO_TOKEN_FLAGS = "no TOKEN_FLAGS observation"
BURN_EXCEEDS_RESERVE = "bridge V2 burn exceeds the vault entry's reserve"
ABSORBED_EVENT = "Absorbed(uint256,uint256)"
WITHDRAWN_EVENT = "Withdrawn(uint64,address,address,uint256,uint64)"


def ledger_view(info):
    """{launch, successor}: both entries' reserve ledgers from getbridgev2info."""
    return {name: {key: reserve.get(key) for key in LEDGER_KEYS}
            for name, reserve in (("launch", info["reserve"]), ("successor", info["successor"]["reserve"]))}


def expected_ledgers(launch, successor):
    """ledger_view's form from (minted, burned, credited, transit, reserve, proven) tuples."""
    return {"launch": dict(zip(LEDGER_KEYS, launch)), "successor": dict(zip(LEDGER_KEYS, successor))}


class B3BridgeRT4dMigrationTest(B3BridgeRT4aRoundTripTest):
    BOOTSTRAP_MODE = 1  # START_CERTIFICATE, as RT4a

    # RT4c's fresh-lineage settling (tip within the lag margin, the finalized
    # checkpoint in the current fresh epoch, no rotation at the next block),
    # so the gates' C3, E3 and E6 rows hold for the next block.
    fresh_lineage_settled = B3BridgeRT4cConflictTest.fresh_lineage_settled
    settle_fresh_lineage = B3BridgeRT4cConflictTest.settle_fresh_lineage

    def set_test_params(self):
        # Required: the framework's metaclass refuses a subclass that does not
        # define set_test_params itself. RT4a's parameters are RT4d's.
        super().set_test_params()

    # -- pins with the successor (before any node starts) ---------------------
    def start_anvil(self, name="anvil", chain_id=REGTEST_ETH_CHAIN_ID, accounts=ANVIL_ACCOUNTS):
        return super().start_anvil(name, chain_id, accounts=accounts)

    def bridge_pins_successor(self):
        """v2.successor: v' = CREATE(anvil account 4, nonce 0), v's AssetId, h_m."""
        anvil = self.anvil
        accounts = anvil.accounts()
        assert_equal(len(accounts), ANVIL_ACCOUNTS)
        self.successor_deployer = accounts[SUCCESSOR_DEPLOYER_ACCOUNT]
        assert self.successor_deployer not in (self.owner, self.depositor, self.deployer, self.relayer)
        self.successor_vault = predict_create_address(self.successor_deployer, 0)
        assert self.successor_vault not in self.predicted.values()
        assert_equal(anvil.code(self.successor_vault), b"")
        return {"vault": self.successor_vault, "deployer": self.successor_deployer,
                "asset_id": self.asset_id_raw, "migration_height": MIGRATION_HEIGHT}

    def expected_unpinned(self):
        return list(SUCCESSOR_UNPINNED)

    # -- test -------------------------------------------------------------------
    def run_test(self):
        self.withdrawal_leaves = []  # (b3 height, leaf V2) in id order
        self.relayed = []
        self.conflict_height = None  # no conflict in RT4d: bridgeFlags stay 0

        self.log.info("(1) RT4a bring-up; getbridgev2info.successor equals the pins and is inactive")
        self.bring_up_validators(BRING_UP_HEIGHT)
        self.check_lineage()
        self.v2info = self.check_v2_info_before_activation()
        self.resume()
        self.shuttle_until(lambda: all(self.fresh_finalized_height(node) >= INIT_CHECKPOINT
                                       for node in self.bridge_validators()))
        self.freeze()
        self.deploy_v2_stack()
        self.initialize_verifier()
        self.resume()
        self.shuttle_until(lambda: all(self.fresh_finalized_height(node) >= BRIDGE_V2_HEIGHT
                                       for node in self.bridge_validators()))
        self.freeze()
        self.check_carried_roots()
        self.relay_certificates("past V")
        self.check_deposits_open()

        self.deploy_successor()
        deposit = self.make_deposit()
        plan = self.build_plan(deposit)
        self.relay_carriers(plan, deposit)
        self.claim_mint(plan, deposit)
        self.check_successor_inactive()
        self.reach_migration_height()
        self.sweep_migration_leaf()
        self.release_migration_leaf()
        self.observe_before_absorb()
        self.absorb_and_credit()
        self.sweep_late_deposit()
        self.successor_burn()
        self.release_late_sweep_and_burn()
        self.successor_deposit()
        self.check_final_accounting()
        self.check_pin_guard_and_reindex()
        self.evidence_step("RT4d complete", self.anvil, fresh_votes=dict(self.fresh_vote_results),
                           relayed=len(self.relayed), withdrawals=len(self.withdrawal_leaves))

    # -- 1. the successor before activation ----------------------------------------
    def check_v2_info_before_activation(self):
        info = super().check_v2_info_before_activation()
        self.log.info("getbridgev2info.successor equals the pins: inactive, its own entry id")
        pin = self.pins["v2"]["successor"]
        assert_equal(sorted(pin), ["asset_id", "migration_height", "predecessor", "vault"])
        successor = info["successor"]
        assert_equal(hx(successor["vault"]), hx(self.successor_vault))
        assert_equal(hx(pin["vault"]), hx(self.successor_vault))
        assert "vault_runtime_code_hash" not in successor  # OI-3: unpinned off mainnet
        assert_equal(hx(successor["predecessor"]), hx(self.vault))
        assert_equal(hx(pin["predecessor"]), hx(self.vault))
        # B-23: v' carries v's AssetId (the node refuses any other at startup).
        assert_equal(hx(successor["asset_id_evm"]), self.asset_id_raw)
        assert_equal(hx(successor["asset_id_evm"]), hx(info["asset_id_evm"]))
        assert_equal(hx(pin["asset_id"]), self.asset_id_raw)
        assert_equal(successor["vault_entry_id"], hx(successor["vault_entry_id_evm"])[::-1].hex())
        assert hx(successor["vault_entry_id_evm"]) != hx(info["vault_entry_id_evm"])
        # Inherited from v: the deployment bound, the deposit cap.
        assert_equal(successor["origin_deployment_block"], self.origin_deployment_block)
        assert_equal(successor["max_deposit_raw"], MAX_DEPOSIT_RAW)
        assert_equal((successor["migration_height"], pin["migration_height"]), (MIGRATION_HEIGHT, MIGRATION_HEIGHT))
        assert_greater_than(MIGRATION_HEIGHT, info["next_height"])
        assert_equal(successor["active"], False)
        assert_equal(info["retired"], False)
        assert_equal(hx(info["deposit_vault"]), hx(self.vault))
        assert_equal(ledger_view(info)["successor"], dict.fromkeys(LEDGER_KEYS, 0))
        self.sweep_record = encode_vault_sweep_record(info["vault_entry_id_evm"])
        # Before h_m the sweep is only its canonical record.
        assert_equal(successor["sweep"], {"payload_hex": self.sweep_record.hex()})
        self.evidence.assertion("getbridgev2info.successor equals the pins (inactive)", expected=pin,
                                actual=successor, launch_entry_id=info["vault_entry_id"],
                                sweep_record=to_0x(self.sweep_record))
        return info

    # -- 2. deploy v' -------------------------------------------------------------------
    def creation_revert(self, sender, initcode):
        """The revert data of a contract creation that must revert (eth_call)."""
        try:
            self.anvil.rpc("eth_call", {"from": sender, "data": to_0x(initcode), "gas": hex(MAX_TX_GAS)}, "latest")
        except AnvilRPCError as e:
            assert isinstance(e.data, str), f"revert without data: {e}"
            return hx(e.data)
        raise AssertionError("the creation did not revert")

    def deploy_successor(self):
        self.log.info("(2) Deploy B3VaultV2 in successor mode at the pinned address of anvil account 4")
        anvil, vault, deployer = self.anvil, self.vault, self.successor_deployer
        assert_equal(int(anvil.rpc("eth_getTransactionCount", deployer, "latest"), 16), 0)
        contract = self.bridge_artifact("B3VaultV2.sol", "B3VaultV2")
        predecessor_asset_id = call_bytes32(anvil, vault, "B3_ASSET_ID()")
        assert_equal(predecessor_asset_id, hx(self.v2info["asset_id_evm"]))
        forwarder_factory = call_address(anvil, vault, "FORWARDER_FACTORY()")
        common = {"verifier": self.verifier, "verifier_code_hash": anvil.code_hash(self.verifier),
                  "chain_domain": self.domain, "token": self.token, "token_code_hash": self.token_code_hash,
                  "forwarder_factory": forwarder_factory, "max_deposit_raw": MAX_DEPOSIT_RAW}
        config = encode_vault_v2_config(**common, predecessor=vault, predecessor_asset_id=predecessor_asset_id)
        # A successor naming another AssetId than v's is refused at
        # construction; eth_call only, so the deployer's nonce stays 0.
        bad = encode_vault_v2_config(**common, predecessor=vault,
                                     predecessor_asset_id=keccak256(b"B3-REGTEST-RT4D/other-asset"))
        refused = self.creation_revert(deployer, contract["bytecode"] + bad)
        assert_equal(refused, error_selector("BadConfig"))
        estimate = int(anvil.rpc("eth_estimateGas", {"from": deployer, "data": to_0x(contract["bytecode"] + config)}),
                       16)
        address, block, code_hash = deploy(anvil, deployer, contract, config,
                                           gas=min(2 * estimate + 100_000, MAX_TX_GAS))
        receipt_gas = gas_used(anvil.receipt(anvil.block(block)["transactions"][-1]))
        self.evidence.gas("B3VaultV2 constructor (successor)", receipt_gas, estimate=estimate, block=block)
        assert_equal(address, self.successor_vault)
        assert_equal(int(anvil.rpc("eth_getTransactionCount", deployer, "latest"), 16), 1)
        successor = self.successor_vault
        node_asset = hx(self.v2info["asset_id_evm"])
        assert_equal(call_bytes32(anvil, successor, "B3_ASSET_ID()"), node_asset)
        assert_equal(call_address(anvil, successor, "PREDECESSOR()"), vault)
        assert_equal(call_address(anvil, vault, "PREDECESSOR()"), "0x" + "00" * 20)
        assert_equal(call_address(anvil, successor, "VERIFIER()"), self.verifier)
        assert_equal(call_bytes32(anvil, successor, "VERIFIER_CODE_HASH()"), anvil.code_hash(self.verifier))
        assert_equal(call_address(anvil, successor, "ORIGIN_TOKEN()"), self.token)
        assert_equal(call_bytes32(anvil, successor, "TOKEN_CODE_HASH()"), self.token_code_hash)
        assert_equal(call_bytes32(anvil, successor, "B3_CHAIN_DOMAIN()"), self.domain)
        assert_equal(call_uint(anvil, successor, "ORIGIN_CHAIN_ID()"), REGTEST_ETH_CHAIN_ID)
        assert_equal(call_uint(anvil, successor, "MAX_DEPOSIT_RAW()"), MAX_DEPOSIT_RAW)
        assert_equal(call_uint(anvil, successor, "BRIDGE_ACTIVATION_HEIGHT()"), BRIDGE_V2_HEIGHT)
        assert_equal(call_address(anvil, successor, "FORWARDER_FACTORY()"), forwarder_factory)
        for slot in ("nextDepositId()", "locked()", "absorbedTotal()"):
            assert_equal(call_uint(anvil, successor, slot), 0)
        # One verifier serves both vaults, so v''s deposit gate is v's.
        assert_equal(call_uint(anvil, successor, "depositGateOpen()"), call_uint(anvil, vault, "depositGateOpen()"))
        # The same withdrawal is another leaf on v': each vault names itself.
        w = encode_withdrawal_v2_tuple(0, ETH_RECIPIENT, 1, BRIDGE_V2_HEIGHT)
        leaf_selector = selector(VAULT_V2_WITHDRAWAL_LEAF_SIGNATURE)
        assert anvil.call(successor, leaf_selector + w) != anvil.call(vault, leaf_selector + w)
        assert_equal(hx(self.nodes[0].getbridgev2info()["successor"]["vault"]), hx(successor))
        self.evidence_step("successor vault deployed at the pinned address", anvil, successor=successor, block=block,
                           asset_id=to_0x(node_asset), predecessor=vault, forwarder_factory=forwarder_factory,
                           bad_asset_refusal=to_0x(refused),
                           # OI-3: observed only; the regtest pins carry no v' code hash.
                           successor_code_hash=to_0x(code_hash))

    # -- kind 9 observations including the successor subjects ----------------------------
    def successor_observed_values(self, block):
        """Subjects 18 and 19 as getbridgev2info reports them, read with
        eth_call and extcodehash of v' at `block`."""
        anvil, vault, token = self.anvil, self.successor_vault, self.token

        def uint(to, signature, *words):
            return call_uint(anvil, to, signature, *words, block=block)

        def code_hash(account):
            return to_0x(anvil.code_hash(account, block))

        return {
            RESERVE_SUCCESSOR: {"next_deposit_id": uint(vault, "nextDepositId()"), "locked": uint(vault, "locked()"),
                                "absorbed_total": uint(vault, "absorbedTotal()"),
                                "balance": uint(token, "balanceOf(address)", enc_address(vault)),
                                "vault_code_hash": code_hash(vault), "token_code_hash": code_hash(token)},
            TOKEN_FLAGS_SUCCESSOR: {"token_code_hash": code_hash(token),
                                    "owner": call_address(anvil, token, "owner()", block=block),
                                    "paused": bool(uint(token, "paused()")),
                                    "upgraded_address": call_address(anvil, token, "upgradedAddress()", block=block),
                                    "deprecated": bool(uint(token, "deprecated()")),
                                    "vault_blacklisted": bool(uint(token, "isBlackListed(address)",
                                                                   enc_address(vault)))},
        }

    def proven_successor_values(self, block):
        """The same values from eth_getProof at `block`, verified here against
        the block's stateRoot with the successor plan (vault slots 0, 1, 3,
        balances[v'], token slots 0, 10, isBlackListed[v'])."""
        anvil = self.anvil
        state_root = hx(anvil.block(block)["stateRoot"])
        plan = observation_plan_v2(self.verifier, self.vault, self.token, successor_vault=self.successor_vault)
        words, code_hashes = {}, {}
        for subject in (RESERVE_SUCCESSOR, TOKEN_FLAGS_SUCCESSOR):
            for account, slots in plan[subject]:
                proof = anvil.get_proof(account, slots, block)
                for slot in slots:
                    proven = verify_account_storage_proof(state_root, account, int.from_bytes(slot, "big"), proof)
                    words[(account, slot)] = proven["value"]
                    code_hashes[account] = to_0x(proven["code_hash"])
        vault, token = plan[RESERVE_SUCCESSOR][0][0], plan[TOKEN_FLAGS_SUCCESSOR][0][0]
        owner_paused, upgraded = words[(token, enc_uint(0))], words[(token, enc_uint(10))]

        def address(value):
            return to_0x((value & ((1 << 160) - 1)).to_bytes(20, "big"))

        return {
            RESERVE_SUCCESSOR: {"next_deposit_id": words[(vault, enc_uint(0))], "locked": words[(vault, enc_uint(1))],
                                "absorbed_total": words[(vault, enc_uint(3))],
                                "balance": words[(token, plan[RESERVE_SUCCESSOR][1][1][0])],
                                "vault_code_hash": code_hashes[vault], "token_code_hash": code_hashes[token]},
            TOKEN_FLAGS_SUCCESSOR: {"token_code_hash": code_hashes[token], "owner": address(owner_paused),
                                    "paused": bool((owner_paused >> 160) & 0xff), "upgraded_address": address(upgraded),
                                    "deprecated": bool((upgraded >> 160) & 0xff),
                                    "vault_blacklisted": bool(words[(token, plan[TOKEN_FLAGS_SUCCESSOR][0][1][2])])},
        }

    def check_observations(self, output, light_client):
        """The observations connected on every node equal eth_call reads and
        Python's own proof check at the anchor block, for v's subjects 1-3
        and v''s 18 and 19 alike (RT4a's check without its one-deposit
        expectations)."""
        anchor, block = output["anchor"], light_client["finalized_block"]
        assert_equal((anchor["number"], hx(anchor["hash_evm"]), anchor["timestamp"]),
                     (block, hx(light_client["finalized_hash"]), light_client["timestamp"]))
        subjects = [observation["subject"] for observation in output["observations"]]
        called = self.anvil_observed_values(block)
        proven = self.proven_observed_values(block)
        if any(subject in ETH_OBSERVATION_SUBJECTS_SUCCESSOR for subject in subjects):
            called.update(self.successor_observed_values(block))
            proven.update(self.proven_successor_values(block))
        assert_equal(proven, called)
        info = self.v2_infos()
        for observation in output["observations"]:
            subject = observation["subject"]
            record = info["observations"][SUBJECT_KEYS[subject]]
            assert_equal(observation["values"], called[subject])
            assert_equal(record["values"], called[subject])
            assert_equal((record["subject"], record["name"], record["anchor_block"], record["t_obs"],
                          record["verdict"], observation["verdict"]),
                         (subject, SUBJECT_NAMES[subject], block, light_client["timestamp"], 0, 0))
            assert_equal(hx(record["anchor_hash_evm"]), hx(anchor["hash_evm"]))
            assert_equal(record["connected_height"], observation["mined_height"])
            assert_equal(record["connected_block"], self.nodes[0].getblockhash(observation["mined_height"]))
        if 1 in subjects:
            verifier = called[1]
            assert_equal((verifier["status_name"], verifier["deposits_closed"]), ("LIVE", False))
            latest_cert = self.relayed[-1] if self.relayed else self.init_cert
            assert_equal(hx(verifier["latest_cert_id"]), hx(latest_cert["cert_id"]))
            lineage = self.nodes[0].getfinalitylineage()
            sets = {header["epoch"]: hx(header["set_hash"]) for header in
                    (lineage.get(key) for key in ("previous_set", "current_set", "next_set")) if header}
            epoch = verifier["current_epoch"]
            assert_equal((hx(verifier["current_set_hash"]), hx(verifier["next_set_hash"])),
                         (sets[epoch], sets[epoch + 1]))
        for subject in (3, TOKEN_FLAGS_SUCCESSOR):
            if subject in subjects:
                flags = called[subject]
                assert_equal((flags["paused"], flags["deprecated"], flags["vault_blacklisted"]), (False, False, False))
                assert_equal(flags["owner"], self.owner)
        for subject, vault in ((2, self.vault), (RESERVE_SUCCESSOR, self.successor_vault)):
            if subject in subjects:
                # A vault holds at least what it locks (absorb only raises locked to the balance).
                assert_greater_than_or_equal(called[subject]["balance"], called[subject]["locked"])
                assert_equal(called[subject]["vault_code_hash"], to_0x(self.anvil.code_hash(vault)))
        self.evidence.assertion("kind 9 observations equal anvil reads and Python's proof check",
                                expected={subject: called[subject] for subject in subjects},
                                actual={SUBJECT_KEYS[subject]: info["observations"][SUBJECT_KEYS[subject]]["values"]
                                        for subject in subjects}, anchor=anchor)
        return info

    def observe(self, label, subjects):
        """Relay new certificates, finalize the anvil head on the V2 light
        client and connect kind 9 observations of `subjects`."""
        self.relay_certificates(label, require_new=False)
        light_client = self.update_light_client_to_head(label=f"V2 light client at the anvil head ({label})")
        output = self.submit_observations_v2(subjects=subjects, label=f"kind 9 observations mined ({label})")
        return output, self.check_observations(output, light_client)

    def open_gate(self, label, subjects):
        """Settle the fresh lineage, observe `subjects`, settle again (so the
        next block judges C3, E3 and E6 on a settled lineage); returns
        (observer output, getbridgev2info)."""
        self.settle_fresh_lineage(BURN_FINALITY_LAG_MARGIN)
        output, _ = self.observe(label, subjects)
        self.settle_fresh_lineage(BURN_FINALITY_LAG_MARGIN)
        return output, self.v2_infos()

    def check_gate_ready_on(self, gate, info, subjects, label):
        """A READY gate whose closes_at comes from exactly `subjects`'
        observations (and the rotation age): the rows that gate reads."""
        assert_equal((gate["status"], gate["reason"]), ("READY", ""))
        assert "row" not in gate
        observations = info["observations"]
        expected = min(min(observations[SUBJECT_KEYS[subject]]["t_obs"] for subject in subjects) +
                       GATE_R_OBSERVATION_MAX_AGE + 1,
                       observations["verifier_e"]["values"]["last_rotation_time"] + GATE_R_ROTATION_AGE_LIMIT + 1)
        assert_equal(gate["closes_at"], expected)
        assert_greater_than(gate["closes_at"] - gate["now_eff"], GATE_R_MEMPOOL_CLOSE_MARGIN)
        self.evidence.assertion(f"gate READY on subjects {list(subjects)} ({label})", expected="READY", actual=gate,
                                fresh_finality_lags=self.fresh_finality_lags())

    def check_observation_plan_with_successor(self, info):
        """From h_m the plan also lists 18 and 19, planned against v'."""
        expected = observation_plan_v2(self.verifier, self.vault, self.token, successor_vault=self.successor_vault)
        plan = info["observation_plan"]
        assert_equal([(entry["subject"], entry["name"]) for entry in plan["subjects"]], sorted(SUBJECT_NAMES.items()))
        for entry in plan["subjects"]:
            assert_equal([(hx(account["address"]), [hx(slot) for slot in account["slots"]])
                          for account in entry["accounts"]],
                         [(hx(address), slots) for address, slots in expected[entry["subject"]]])

    # -- 3. before h_m -----------------------------------------------------------------------
    def check_successor_inactive(self):
        self.log.info("(3) Below h_m: the v mint is in; successor subjects and the sweep are refused")
        n0 = self.nodes[0]
        self.freeze()
        assert_greater_than(MIGRATION_HEIGHT, self.mint_height)
        info = self.v2_infos()
        assert_greater_than(MIGRATION_HEIGHT, info["next_height"])
        assert_equal((info["retired"], info["successor"]["active"]), (False, False))
        assert_equal(hx(info["deposit_vault"]), hx(self.vault))
        assert_equal(info["successor"]["sweep"], {"payload_hex": self.sweep_record.hex()})
        assert_equal(ledger_view(info), expected_ledgers((DEPOSIT_RAW, 0, 0, 0, DEPOSIT_RAW, 1), (0, 0, 0, 0, 0, 0)))
        # Only subjects 1-3 are planned below h_m.
        assert_equal([entry["subject"] for entry in info["observation_plan"]["subjects"]], [1, 2, 3])
        anchor = info["observation_plan"].get("anchor", {}).get("hash_evm", to_0x(bytes(32)))
        refusals = {}
        for subject in ETH_OBSERVATION_SUBJECTS_SUCCESSOR:
            refusals[f"buildbridgeobservationv2 {subject}"] = rpc_refusal(
                RPC_MISC_ERROR, SUCCESSOR_OBSERVATIONS_INACTIVE, n0.buildbridgeobservationv2, subject, anchor, [])
        try:
            run_observer_once(n0, self.default_wallet_name, self.anvil.url, subjects=(RESERVE_SUCCESSOR,),
                              dry_run=True)
        except ObserverRunError as e:
            assert f"no single entry for subject {RESERVE_SUCCESSOR}" in e.message, e.message
            refusals["observer --subjects 18 (dry run)"] = e.message
        else:
            raise AssertionError("the observer built a successor observation below h_m")
        refusals["submitbridgecarrier kind 16"] = rpc_refusal(
            RPC_VERIFY_REJECTED, SWEEPS_INACTIVE, n0.submitbridgecarrier, self.sweep_record.hex())
        assert_equal(self.v2_infos()["withdrawals"], 0)
        self.evidence.assertion("below h_m: successor observations and vault sweeps refused",
                                expected=[SUCCESSOR_OBSERVATIONS_INACTIVE, SWEEPS_INACTIVE], actual=refusals,
                                mint_height=self.mint_height, migration_height=MIGRATION_HEIGHT)

    # -- 4. h_m ----------------------------------------------------------------------------------
    def reach_migration_height(self):
        self.log.info("(4) Shuttle to h_m - 1: v retired, deposits target v', burns wait for the v' gate")
        n0 = self.nodes[0]
        self.resume()
        self.shuttle_until(lambda: all(node.getblockcount() >= MIGRATION_HEIGHT - 1 for node in self.bridge_validators()),
                           timeout=600)
        self.freeze()
        self.settle_fresh_lineage(BURN_FINALITY_LAG_MARGIN)
        info = self.v2_infos()
        assert_greater_than_or_equal(info["next_height"], MIGRATION_HEIGHT)
        successor = info["successor"]
        assert_equal((info["retired"], successor["active"]), (True, True))
        assert_equal(hx(info["deposit_vault"]), hx(self.successor_vault))
        sweep = successor["sweep"]
        assert_equal(hx(sweep["payload_hex"]), self.sweep_record)
        # The amount is v's reserve capped by the wind-down headroom of v's
        # latest RESERVE_E, of which there is none yet.
        assert "amount" not in sweep
        assert_equal(info["reserve"]["reserve"], DEPOSIT_RAW)
        # No observation yet: both gates stop at E1.
        for gate in (sweep["gate"], info["burn_gate"]):
            assert_equal((gate["status"], gate["row"], gate["reason"]), ("NOT_READY", "E1", NO_VERIFIER_E))
        self.check_observation_plan_with_successor(info)
        instructions = n0.getbridgedepositrecipient()
        assert_equal((hx(instructions["vault"]), hx(instructions["token"])), (hx(self.successor_vault), hx(self.token)))
        assert_equal(instructions["asset_id"], self.v2info["asset_id"])
        assert_equal(hx(instructions["recipient_v1"]), recipient_v1(instructions["address"]))
        refusal = rpc_refusal(RPC_VERIFY_REJECTED, f"bridge V2 burn gate is not ready: E1: {NO_VERIFIER_E}",
                              n0.bridgewithdraw, SUCCESSOR_BURN_RAW, ETH_RECIPIENT)
        self.evidence.assertion("from h_m: v retired, deposits and burns name v'",
                                expected={"retired": True, "deposit_vault": self.successor_vault},
                                actual={"retired": info["retired"], "deposit_vault": info["deposit_vault"],
                                        "sweep": sweep, "burn_gate": info["burn_gate"]},
                                deposit_instructions=instructions, bridgewithdraw=refusal,
                                fresh_finality_lags=self.fresh_finality_lags())

    # -- 5. the migration leaf ---------------------------------------------------------------------
    def submit_sweep(self, withdrawal_id, amount, label):
        """Submit successor.sweep.payload_hex (READY) and mine it; returns
        (txid, height, leaf), the leaf checked against Python and v."""
        n0 = self.nodes[0]
        info = self.v2_infos()
        sweep = info["successor"]["sweep"]
        assert_equal(hx(sweep["payload_hex"]), self.sweep_record)
        assert_equal(sweep["amount"], amount)

        def create():
            result = n0.submitbridgecarrier(self.sweep_record.hex())
            assert_equal((result["record_kind"], result["binding_vout"]), ("vault-sweep", 1))
            assert_equal(result["sweep_amount_raw"], amount)
            assert result["proof_rpc_after_confirmation"].startswith(
                f"getbridgewithdrawalproofv2 {result['txid']} 1 ")
            return result

        txid, height = self.submit_and_mine(label, create)
        assert_greater_than_or_equal(height, MIGRATION_HEIGHT)
        leaf = leaf_v2(self.asset_id_raw, withdrawal_id, REGTEST_ETH_CHAIN_ID, self.vault, self.token,
                       self.successor_vault, amount, height)
        # The retiring vault computes the same leaf: it names v and pays v'.
        assert_equal(self.anvil.call(self.vault, selector(VAULT_V2_WITHDRAWAL_LEAF_SIGNATURE) +
                                     encode_withdrawal_v2_tuple(withdrawal_id, self.successor_vault, amount, height)),
                     leaf)
        self.withdrawal_leaves.append((height, leaf))
        after = self.v2_infos()
        assert_equal(after["withdrawals"], len(self.withdrawal_leaves))
        assert_equal(hx(after["withdrawal_root_evm"]), withdrawal_root([l for _, l in self.withdrawal_leaves]))
        self.evidence.assertion(f"kind 16 leaf {withdrawal_id} equals Python and v.withdrawalLeaf", expected=to_0x(leaf),
                                actual=after["withdrawal_root_evm"], txid=txid, height=height, amount=amount,
                                sweep_gate=sweep["gate"])
        return txid, height, leaf

    def sweep_migration_leaf(self):
        self.log.info("(5) Subjects 1-3: v's gate but for retirement is READY; the kind 16 migration leaf pays v'")
        n0 = self.nodes[0]
        _, info = self.open_gate("before the migration leaf", LAUNCH_GATE_SUBJECTS)
        sweep = info["successor"]["sweep"]
        self.check_gate_ready_on(sweep["gate"], info, LAUNCH_GATE_SUBJECTS, "sweep gate, migration leaf")
        # v''s burn gate reads TOKEN_FLAGS_SUCCESSOR, which no record carries yet.
        gate = info["burn_gate"]
        assert_equal((gate["status"], gate["row"], gate["reason"]), ("NOT_READY", "E7", NO_TOKEN_FLAGS))
        self.sweep1_txid, self.sweep1_height, self.sweep1_leaf = self.submit_sweep(
            0, DEPOSIT_RAW, "kind 16 migration leaf mined")
        after = self.v2_infos()
        assert_equal(ledger_view(after), expected_ledgers((DEPOSIT_RAW, DEPOSIT_RAW, 0, 0, 0, 1),
                                                          (0, 0, 0, DEPOSIT_RAW, 0, 0)))
        assert_equal(after["successor"]["sweep"]["amount"], 0)
        refusal = rpc_refusal(RPC_VERIFY_REJECTED, NOTHING_TO_SWEEP, n0.submitbridgecarrier, self.sweep_record.hex())
        self.evidence.assertion("migration leaf: v burned its reserve, v' holds it in transit",
                                expected=expected_ledgers((DEPOSIT_RAW, DEPOSIT_RAW, 0, 0, 0, 1),
                                                          (0, 0, 0, DEPOSIT_RAW, 0, 0)),
                                actual=ledger_view(after), second_sweep=refusal, burn_gate_before=gate)

    # -- 6. release of the migration leaf --------------------------------------------------------
    def leaves_through(self, height):
        return [leaf for burned_at, leaf in self.withdrawal_leaves if burned_at <= height]

    def relay_covering(self, height, label):
        """Shuttle until a carried certificate covers `height`, relay every
        certificate in order; returns the first covering one."""
        n0 = self.nodes[0]
        self.resume()
        self.shuttle_until(lambda: any(carrier["fb"]["height"] >= height
                                       for carrier in self.fresh_certificates(n0, height + 1)), timeout=600)
        self.freeze()
        self.check_carried_roots()
        self.relay_certificates(label)
        covering = next(entry for entry in self.relayed if entry["height"] >= height)
        root = withdrawal_root(self.leaves_through(covering["height"]))
        accepted = call_uint(self.anvil, self.verifier, "acceptedRootHeight(bytes32)", enc_bytes32(root))
        assert_equal(accepted, covering["height"])
        self.evidence.assertion(f"root covering {height} accepted ({label})", expected=covering["height"],
                                actual=accepted, covering=covering, root=to_0x(root))
        return covering

    def check_withdrawal_proof_v2(self, txid, vout, withdrawal_id, vault, recipient, amount, height, sweep, covering):
        """getbridgewithdrawalproofv2, identical on every node and re-derived."""
        n0 = self.nodes[0]
        inclusion = covering["inclusion"]
        proofs = [node.getbridgewithdrawalproofv2(txid, vout, inclusion) for node in self.bridge_nodes()]
        for proof in proofs[1:]:
            assert_equal(proof, proofs[0])
        proof = proofs[0]
        assert_equal((proof["withdrawal_id"], proof["source_txid"], proof["burn_output_index"]),
                     (withdrawal_id, txid, vout))
        assert_equal((proof["b3_height"], proof["b3_block"]), (height, n0.getblockhash(height)))
        assert_equal(hx(proof["asset_id_evm"]), self.asset_id_raw)
        assert_equal(proof["destination_chain_id"], REGTEST_ETH_CHAIN_ID)
        for key, value in (("vault", vault), ("token", self.token), ("recipient", recipient)):
            assert_equal(hx(proof[key]), hx(value))
        assert_equal((proof["amount_raw"], proof["sweep"]), (amount, sweep))
        preimage = leaf_v2_preimage(self.asset_id_raw, withdrawal_id, REGTEST_ETH_CHAIN_ID, vault, self.token,
                                    recipient, amount, height)
        leaf = keccak256(preimage)
        assert_equal((hx(proof["leaf_preimage"]), hx(proof["withdrawal_leaf"])), (preimage, leaf))
        leaves = self.leaves_through(covering["height"])
        root, path = withdrawal_root_and_path(leaves, withdrawal_id)
        assert_equal(hx(proof["withdrawal_root"]), root)
        assert_equal([hx(sibling) for sibling in proof["path"]], path)
        assert_equal(fold_withdrawal_path(leaf, withdrawal_id, path), root)
        assert_equal((proof["certificate"]["inclusion_height"], proof["certificate"]["height"]),
                     (inclusion, covering["height"]))
        assert_equal(proof["function"], VAULT_V2_RELEASE_SIGNATURE)
        assert_equal(hx(proof["calldata"]), encode_vault_v2_release(withdrawal_id, recipient, amount, height, path))
        assert_equal(self.anvil.call(vault, selector(VAULT_V2_WITHDRAWAL_LEAF_SIGNATURE) +
                                     encode_withdrawal_v2_tuple(withdrawal_id, recipient, amount, height)), leaf)
        return proof

    def release_once(self, vault, proof, label, check_on_chain=True):
        """release(proof) on `vault` pays proof's recipient exactly once: one
        Withdrawn log, then AlreadyReleased (eth_call and, if asked, a
        reverting transaction that moves nothing). Returns the receipt."""
        anvil, token = self.anvil, self.token
        withdrawal_id, recipient = proof["withdrawal_id"], proof["recipient"]
        amount, height = proof["amount_raw"], proof["b3_height"]

        def balances():
            return {"recipient": call_uint(anvil, token, "balanceOf(address)", enc_address(recipient)),
                    "vault": call_uint(anvil, token, "balanceOf(address)", enc_address(vault)),
                    "locked": call_uint(anvil, vault, "locked()"),
                    "released": call_uint(anvil, vault, "released(uint64)", enc_uint(withdrawal_id))}

        before = balances()
        assert_equal(before["released"], 0)
        calldata = hx(proof["calldata"])
        receipt = self.transact(vault, calldata, label, withdrawal_id=withdrawal_id)
        logs = [log for log in receipt["logs"] if hx(log["address"]) == hx(vault)]
        assert_equal(len(logs), 1)
        assert_equal([hx(topic) for topic in logs[0]["topics"]],
                     [keccak256(WITHDRAWN_EVENT.encode()), enc_uint(withdrawal_id), enc_address(token),
                      enc_address(recipient)])
        assert_equal(hx(logs[0]["data"]), enc_uint(amount) + enc_uint(height))
        after = balances()
        assert_equal(after, {"recipient": before["recipient"] + amount, "vault": before["vault"] - amount,
                             "locked": before["locked"] - amount, "released": 1})
        assert_equal(revert_data(anvil, vault, calldata, self.relayer), error_selector("AlreadyReleased"))
        second_status = None
        if check_on_chain:
            tx_hash = anvil.rpc("eth_sendTransaction", {"from": self.relayer, "to": vault,
                                                        "data": to_0x(calldata), "gas": hex(1_000_000)})
            second_status = int(anvil.receipt(tx_hash)["status"], 16)
            assert_equal(second_status, 0)
            assert_equal(balances(), after)
        self.evidence.assertion(f"{label}: paid exactly once", expected=after,
                                actual={"before": before, "after": after, "second_status": second_status},
                                tx=receipt["transactionHash"])
        return receipt

    def release_migration_leaf(self):
        self.log.info("(6) Relay a certificate covering the migration leaf; v.release pays v' once")
        anvil = self.anvil
        covering = self.relay_covering(self.sweep1_height, "covering the migration leaf")
        proof = self.check_withdrawal_proof_v2(self.sweep1_txid, 1, 0, self.vault, self.successor_vault, DEPOSIT_RAW,
                                               self.sweep1_height, True, covering)
        assert_equal(hx(proof["withdrawal_leaf"]), self.sweep1_leaf)
        self.release_once(self.vault, proof, "B3VaultV2.release (migration leaf, v to v')")
        state = {"v_locked": call_uint(anvil, self.vault, "locked()"),
                 "v_balance": call_uint(anvil, self.token, "balanceOf(address)", enc_address(self.vault)),
                 "v'_balance": call_uint(anvil, self.token, "balanceOf(address)", enc_address(self.successor_vault)),
                 "v'_locked": call_uint(anvil, self.successor_vault, "locked()"),
                 "v'_absorbed": call_uint(anvil, self.successor_vault, "absorbedTotal()")}
        # v' received the reserve by transfer; nothing is absorbed yet.
        assert_equal(state, {"v_locked": 0, "v_balance": 0, "v'_balance": DEPOSIT_RAW, "v'_locked": 0,
                             "v'_absorbed": 0})
        self.evidence_step("migration leaf released to v'", anvil, **state)

    # -- 7. before absorb ------------------------------------------------------------------------
    def observe_before_absorb(self):
        self.log.info("(7) Subjects 1, 2, 3, 18, 19 before absorb(): nothing credited, no conflict")
        n0 = self.nodes[0]
        output, info = self.open_gate("before absorb", ALL_SUBJECTS)
        reserve = info["observations"][SUBJECT_KEYS[RESERVE_SUCCESSOR]]["values"]
        assert_equal({key: reserve[key] for key in ("next_deposit_id", "locked", "absorbed_total", "balance")},
                     {"next_deposit_id": 0, "locked": 0, "absorbed_total": 0, "balance": DEPOSIT_RAW})
        # v' had written no storage at the anchor: an empty storage trie,
        # whose slots anvil proves with the single node 0x80 and the record
        # carries as the canonical empty proof (buildbridgeobservationv2).
        empty = self.anvil.get_proof(self.successor_vault, [enc_uint(0)], output["anchor"]["number"])
        assert_equal(hx(empty["storageHash"]), EMPTY_TRIE_ROOT)
        assert_equal(ledger_view(info), expected_ledgers((DEPOSIT_RAW, DEPOSIT_RAW, 0, 0, 0, 1),
                                                         (0, 0, 0, DEPOSIT_RAW, 0, 0)))
        assert "conflict" not in info
        margins = {"launch": info["reserve"]["invariant_r_margin"],
                   "successor": info["successor"]["reserve"]["invariant_r_margin"]}
        assert_equal(margins, {"launch": 0, "successor": 0})
        # The gate of v' is READY on its own rows; check 2 (its reserve, 0
        # until a credit) refuses the burn.
        self.check_gate_ready_on(info["burn_gate"], info, SUCCESSOR_GATE_SUBJECTS, "v' gate before absorb")
        refusal = rpc_refusal(RPC_VERIFY_REJECTED, BURN_EXCEEDS_RESERVE, n0.bridgewithdraw, SUCCESSOR_BURN_RAW,
                              ETH_RECIPIENT)
        self.evidence.assertion("observation of v' before absorb credits nothing and sets no conflict",
                                expected={"credited": 0, "transit": DEPOSIT_RAW, "conflict": None},
                                actual={"successor": info["successor"]["reserve"], "conflict": info.get("conflict")},
                                observed=reserve, invariant_r_margins=margins, bridgewithdraw=refusal,
                                successor_storage_hash=empty["storageHash"],
                                successor_slot0_proof=empty["storageProof"][0]["proof"])

    # -- 8. absorb and credit ----------------------------------------------------------------------
    def absorb(self, expected_increase, label):
        anvil, successor = self.anvil, self.successor_vault
        absorbed_before = call_uint(anvil, successor, "absorbedTotal()")
        receipt = self.transact(successor, abi_call("absorb()"), label)
        logs = [log for log in receipt["logs"] if hx(log["address"]) == hx(successor)]
        assert_equal(len(logs), 1)
        assert_equal([hx(topic) for topic in logs[0]["topics"]], [keccak256(ABSORBED_EVENT.encode())])
        total = absorbed_before + expected_increase
        assert_equal(hx(logs[0]["data"]), enc_uint(expected_increase) + enc_uint(total))
        assert_equal(call_uint(anvil, successor, "absorbedTotal()"), total)
        assert_equal(call_uint(anvil, successor, "locked()"),
                     call_uint(anvil, self.token, "balanceOf(address)", enc_address(successor)))
        return total

    def absorb_and_credit(self):
        self.log.info("(8) v'.absorb(); RESERVE_E_SUCCESSOR credits v' from absorbedTotal")
        total = self.absorb(DEPOSIT_RAW, "B3VaultV2.absorb (v', migration leaf)")
        assert_equal((total, call_uint(self.anvil, self.successor_vault, "locked()")), (DEPOSIT_RAW, DEPOSIT_RAW))
        _, info = self.open_gate("after absorb", SUCCESSOR_GATE_SUBJECTS)
        reserve = info["observations"][SUBJECT_KEYS[RESERVE_SUCCESSOR]]["values"]
        assert_equal((reserve["locked"], reserve["absorbed_total"], reserve["balance"]),
                     (DEPOSIT_RAW, DEPOSIT_RAW, DEPOSIT_RAW))
        assert_equal(ledger_view(info), expected_ledgers((DEPOSIT_RAW, DEPOSIT_RAW, 0, 0, 0, 1),
                                                         (0, 0, DEPOSIT_RAW, 0, DEPOSIT_RAW, 0)))
        assert_equal(info["successor"]["reserve"]["invariant_r_margin"], 0)
        assert "conflict" not in info
        self.check_gate_ready_on(info["burn_gate"], info, SUCCESSOR_GATE_SUBJECTS, "v' gate after absorb")
        self.evidence.assertion("v' credited only after absorb", expected=DEPOSIT_RAW,
                                actual=info["successor"]["reserve"]["credited"], observed=reserve)

    # -- 9. late deposit into v -----------------------------------------------------------------
    def sweep_late_deposit(self):
        self.log.info("(9) A late deposit into the retired v mints after h_m and is swept to v'")
        anvil, n0 = self.anvil, self.nodes[0]
        locked_before = call_uint(anvil, self.vault, "locked()")
        receipt = self.transact(self.vault, abi_call("deposit(uint256,bytes32)", enc_uint(LATE_DEPOSIT_RAW),
                                                     recipient_v1(self.recipient)),
                                "B3VaultV2.deposit (late, into the retired vault)", sender=self.depositor)
        k = int(receipt["blockNumber"], 16)
        anvil.mine(1)  # The update's attested block k + 1.
        assert_equal(call_uint(anvil, self.vault, "locked()"), locked_before + LATE_DEPOSIT_RAW)
        assert_equal(call_uint(anvil, self.vault, "nextDepositId()"), LATE_DEPOSIT_ID + 1)
        deposits = find_deposits(anvil, self.vault, self.token, self.origin_deployment_block)
        assert_equal([d["deposit_id"] for d in deposits], [FIRST_DEPOSIT_ID, LATE_DEPOSIT_ID])
        deposit = deposits[-1]
        assert_equal((deposit["block_number"], deposit["amount"]), (k, LATE_DEPOSIT_RAW))
        # No deposit ever went to v' itself.
        assert_equal(find_deposits(anvil, self.successor_vault, self.token, self.origin_deployment_block), [])

        launch_id = self.v2info["vault_entry_id"]
        plan = build_direct_mint_plan(os.path.join(self.options.tmpdir, "late_mint_plan"), self.beacon,
                                      anvil.url, deposit, launch_id, [k], vault=self.vault, token=self.token,
                                      bridge_v2=True)
        mint = plan["mint"]
        assert_equal(hx(mint["registry_id"]), hx(launch_id))
        assert_equal((mint["deposit_id"], mint["amount"], mint["target_block_number"]),
                     (LATE_DEPOSIT_ID, LATE_DEPOSIT_RAW, k))
        assert_equal(plan["backfills"], [])

        def update():
            result = n0.submitbridgecarrier(plan["updates"][0]["payload_hex"])
            assert_equal(result["record_kind"], "update")
            return result

        update_txid, update_height = self.submit_and_mine("V2 update carrier (late deposit) mined", update)
        self.light_client_updates_v2.append({
            "finalized_block": k, "finalized_hash": deposit["block_hash"],
            "timestamp": int(anvil.block(k)["timestamp"], 16), "height": update_height, "txid": update_txid})

        def claim():
            # The retired launch entry still mints a proven deposit (node
            # changes §7.6): the vault cannot refund it.
            result = n0.claimbridgedeposit(mint["payload_hex"], LATE_DEPOSIT_RAW, self.recipient)
            assert_equal((result["asset_id"], result["amount"]), (self.v2info["asset_id"], LATE_DEPOSIT_RAW))
            return result

        self.late_mint_txid, self.late_mint_height = self.submit_and_mine("late bUSD V2 mint (retired v) mined", claim)
        assert_greater_than_or_equal(self.late_mint_height, MIGRATION_HEIGHT)
        info = self.v2_infos()
        assert_equal(info["nullifiers"], 2)
        assert_equal(ledger_view(info), expected_ledgers(
            (DEPOSIT_RAW + LATE_DEPOSIT_RAW, DEPOSIT_RAW, 0, 0, LATE_DEPOSIT_RAW, 2),
            (0, 0, DEPOSIT_RAW, 0, DEPOSIT_RAW, 0)))
        assert_equal(info["successor"]["sweep"]["amount"], LATE_DEPOSIT_RAW)
        assert_equal(self.wallet_asset(n0, self.v2info["asset_id"])["confirmed"], DEPOSIT_RAW + LATE_DEPOSIT_RAW)

        _, info = self.open_gate("before the late-deposit sweep", LAUNCH_GATE_SUBJECTS)
        reserve = info["observations"]["reserve_e"]["values"]
        assert_equal((reserve["next_deposit_id"], reserve["locked"], reserve["balance"]),
                     (LATE_DEPOSIT_ID + 1, LATE_DEPOSIT_RAW, LATE_DEPOSIT_RAW))
        # The launch ledger at the bound: the wind-down headroom is exactly the late deposit.
        assert_equal((info["reserve"]["invariant_r_margin"], info["reserve"]["wind_down_headroom"]),
                     (0, LATE_DEPOSIT_RAW))
        self.check_gate_ready_on(info["successor"]["sweep"]["gate"], info, LAUNCH_GATE_SUBJECTS,
                                 "sweep gate, late deposit")
        self.sweep2_txid, self.sweep2_height, self.sweep2_leaf = self.submit_sweep(
            1, LATE_DEPOSIT_RAW, "kind 16 sweep of the late deposit mined")
        after = self.v2_infos()
        assert_equal(ledger_view(after), expected_ledgers(
            (DEPOSIT_RAW + LATE_DEPOSIT_RAW, DEPOSIT_RAW + LATE_DEPOSIT_RAW, 0, 0, 0, 2),
            (0, 0, DEPOSIT_RAW, LATE_DEPOSIT_RAW, DEPOSIT_RAW, 0)))
        self.evidence.assertion("late deposit into v minted after h_m and swept to v'",
                                expected={"leaf_id": 1, "amount": LATE_DEPOSIT_RAW},
                                actual=ledger_view(after), late_mint_height=self.late_mint_height,
                                sweep_height=self.sweep2_height, deposit_block=k)

    # -- 10. a burn from v' --------------------------------------------------------------------------
    def successor_burn(self):
        self.log.info("(10) bridgewithdraw burns into a leaf naming v' (shared AssetId)")
        n0 = self.nodes[0]
        asset_id = self.v2info["asset_id"]
        _, info = self.open_gate("before the successor burn", SUCCESSOR_GATE_SUBJECTS)
        # v' still counts only what absorb() counted: the late sweep stays in transit.
        assert_equal(ledger_view(info)["successor"],
                     dict(zip(LEDGER_KEYS, (0, 0, DEPOSIT_RAW, LATE_DEPOSIT_RAW, DEPOSIT_RAW, 0))))
        assert_greater_than_or_equal(info["successor"]["reserve"]["wind_down_headroom"], SUCCESSOR_BURN_RAW)
        self.check_gate_ready_on(info["burn_gate"], info, SUCCESSOR_GATE_SUBJECTS, "v' gate before its burn")
        held = DEPOSIT_RAW + LATE_DEPOSIT_RAW

        def create():
            result = n0.bridgewithdraw(SUCCESSOR_BURN_RAW, ETH_RECIPIENT)
            assert_equal(result["withdrawal_mode"], "decentralized-verifier-v2")
            assert_equal((result["asset_id"], result["amount"]), (asset_id, SUCCESSOR_BURN_RAW))
            # Coin selection spends the 25,000,000 mint alone or both mints
            # (the 5,000,000 one cannot cover the burn by itself).
            assert result["asset_change"] + SUCCESSOR_BURN_RAW in (DEPOSIT_RAW, held), result["asset_change"]
            assert_equal((result["burn_vout"], result["binding_vout"]), (0, 1))
            return result

        self.burn_txid, self.burn_height = self.submit_and_mine("bUSD V2 burn naming v' mined", create)
        self.burn_leaf = leaf_v2(self.asset_id_raw, 2, REGTEST_ETH_CHAIN_ID, self.successor_vault, self.token,
                                 ETH_RECIPIENT, SUCCESSOR_BURN_RAW, self.burn_height)
        assert_equal(self.anvil.call(self.successor_vault, selector(VAULT_V2_WITHDRAWAL_LEAF_SIGNATURE) +
                                     encode_withdrawal_v2_tuple(2, ETH_RECIPIENT, SUCCESSOR_BURN_RAW,
                                                                self.burn_height)), self.burn_leaf)
        self.withdrawal_leaves.append((self.burn_height, self.burn_leaf))
        after = self.v2_infos()
        assert_equal(after["withdrawals"], 3)
        assert_equal(hx(after["withdrawal_root_evm"]), withdrawal_root([leaf for _, leaf in self.withdrawal_leaves]))
        assert_equal(ledger_view(after)["successor"], dict(zip(LEDGER_KEYS, (
            0, SUCCESSOR_BURN_RAW, DEPOSIT_RAW, LATE_DEPOSIT_RAW, DEPOSIT_RAW - SUCCESSOR_BURN_RAW, 0))))
        assert_equal(self.wallet_asset(n0, asset_id)["confirmed"], held - SUCCESSOR_BURN_RAW)
        self.evidence.assertion("leaf 2 names v' with the shared AssetId", expected=to_0x(self.burn_leaf),
                                actual=after["withdrawal_root_evm"], burn_txid=self.burn_txid,
                                burn_height=self.burn_height, burn_gate=info["burn_gate"],
                                successor_reserve=after["successor"]["reserve"])

    # -- 11. releases on both vaults ------------------------------------------------------------------
    def release_late_sweep_and_burn(self):
        self.log.info("(11) Relay a certificate covering both; release leaf 1 on v and leaf 2 on v' once each")
        anvil = self.anvil
        covering = self.relay_covering(max(self.sweep2_height, self.burn_height), "covering the sweep and the burn")
        sweep_proof = self.check_withdrawal_proof_v2(self.sweep2_txid, 1, 1, self.vault, self.successor_vault,
                                                     LATE_DEPOSIT_RAW, self.sweep2_height, True, covering)
        burn_proof = self.check_withdrawal_proof_v2(self.burn_txid, 0, 2, self.successor_vault, ETH_RECIPIENT,
                                                    SUCCESSOR_BURN_RAW, self.burn_height, False, covering)
        # Each leaf names its own vault: on the other vault it is another leaf.
        wrong_vault = {"leaf 1 on v'": revert_data(anvil, self.successor_vault, hx(sweep_proof["calldata"]),
                                                   self.relayer),
                       "leaf 2 on v": revert_data(anvil, self.vault, hx(burn_proof["calldata"]), self.relayer)}
        assert_equal(wrong_vault, {name: error_selector("UnknownRoot") for name in wrong_vault})
        self.release_once(self.vault, sweep_proof, "B3VaultV2.release (late sweep, v to v')", check_on_chain=False)
        self.release_once(self.successor_vault, burn_proof, "B3VaultV2.release (burn, v' to the recipient)")
        assert_equal(call_uint(anvil, self.token, "balanceOf(address)", enc_address(ETH_RECIPIENT)), SUCCESSOR_BURN_RAW)

        total = self.absorb(LATE_DEPOSIT_RAW, "B3VaultV2.absorb (v', late sweep)")
        assert_equal(total, DEPOSIT_RAW + LATE_DEPOSIT_RAW)
        self.settle_fresh_lineage(BURN_FINALITY_LAG_MARGIN)
        _, info = self.observe("after the second absorb", (RESERVE_SUCCESSOR,))
        reserve = info["observations"][SUBJECT_KEYS[RESERVE_SUCCESSOR]]["values"]
        assert_equal((reserve["locked"], reserve["absorbed_total"], reserve["balance"]),
                     (DEPOSIT_RAW + LATE_DEPOSIT_RAW - SUCCESSOR_BURN_RAW, DEPOSIT_RAW + LATE_DEPOSIT_RAW,
                      DEPOSIT_RAW + LATE_DEPOSIT_RAW - SUCCESSOR_BURN_RAW))
        assert_equal(info["successor"]["reserve"]["credited"], DEPOSIT_RAW + LATE_DEPOSIT_RAW)
        assert_equal(info["successor"]["reserve"]["transit"], 0)
        self.evidence.assertion("both leaves released once; v' credited after the second absorb",
                                expected=DEPOSIT_RAW + LATE_DEPOSIT_RAW,
                                actual=info["successor"]["reserve"], wrong_vault=wrong_vault, covering=covering)

    # -- 12. a deposit into v' ------------------------------------------------------------------------
    def successor_deposit(self):
        self.log.info("(12) A deposit into v' is proven against v''s logs and claimed under the successor entry id")
        anvil, n0 = self.anvil, self.nodes[0]
        successor, asset_id = self.successor_vault, self.v2info["asset_id"]
        instructions = n0.getbridgedepositrecipient()
        assert_equal(hx(instructions["vault"]), hx(successor))
        recipient = instructions["address"]
        locked_before = call_uint(anvil, successor, "locked()")
        held_before = self.wallet_asset(n0, asset_id)["confirmed"]
        anvil.send(self.depositor, self.token, abi_call(
            "approve(address,uint256)", enc_address(successor), enc_uint(TOKEN_SUPPLY)))
        receipt = self.transact(successor, abi_call("deposit(uint256,bytes32)", enc_uint(SUCCESSOR_DEPOSIT_RAW),
                                                    recipient_v1(recipient)),
                                "B3VaultV2.deposit (into the successor)", sender=self.depositor)
        k = int(receipt["blockNumber"], 16)
        anvil.mine(1)  # The update's attested block k + 1.
        assert_equal(call_uint(anvil, successor, "locked()"), locked_before + SUCCESSOR_DEPOSIT_RAW)
        assert_equal(call_uint(anvil, successor, "nextDepositId()"), SUCCESSOR_DEPOSIT_ID + 1)
        deposits = find_deposits(anvil, successor, self.token, self.origin_deployment_block)
        assert_equal([d["deposit_id"] for d in deposits], [SUCCESSOR_DEPOSIT_ID])
        deposit = deposits[0]
        assert_equal((deposit["block_number"], deposit["amount"]), (k, SUCCESSOR_DEPOSIT_RAW))
        assert_equal(deposit["recipient"], to_0x(recipient_v1(recipient)))

        successor_id = self.v2_infos()["successor"]["vault_entry_id"]
        assert successor_id != self.v2info["vault_entry_id"]
        plan = build_direct_mint_plan(os.path.join(self.options.tmpdir, "successor_mint_plan"), self.beacon,
                                      anvil.url, deposit, successor_id, [k], vault=successor, token=self.token,
                                      bridge_v2=True)
        mint = plan["mint"]
        assert_equal(hx(mint["registry_id"]), hx(successor_id))
        assert_equal((mint["deposit_id"], mint["amount"], mint["target_block_number"]),
                     (SUCCESSOR_DEPOSIT_ID, SUCCESSOR_DEPOSIT_RAW, k))
        assert_equal(plan["backfills"], [])

        def update():
            result = n0.submitbridgecarrier(plan["updates"][0]["payload_hex"])
            assert_equal(result["record_kind"], "update")
            return result

        update_txid, update_height = self.submit_and_mine("V2 update carrier (successor deposit) mined", update)
        self.light_client_updates_v2.append({
            "finalized_block": k, "finalized_hash": deposit["block_hash"],
            "timestamp": int(anvil.block(k)["timestamp"], 16), "height": update_height, "txid": update_txid})

        def claim():
            result = n0.claimbridgedeposit(mint["payload_hex"], SUCCESSOR_DEPOSIT_RAW, recipient)
            assert_equal((result["asset_id"], result["amount"]), (asset_id, SUCCESSOR_DEPOSIT_RAW))
            assert_equal((result["mint_vout"], result["binding_vout"]), (0, 1))
            return result

        mint_txid, mint_height = self.submit_and_mine("bUSD V2 mint (successor deposit) mined", claim)
        assert_greater_than_or_equal(mint_height, MIGRATION_HEIGHT)
        replay = rpc_refusal(RPC_VERIFY_REJECTED, "bridge deposit is already nullified", n0.claimbridgedeposit,
                             mint["payload_hex"], SUCCESSOR_DEPOSIT_RAW, recipient)
        info = self.v2_infos()
        assert_equal(info["nullifiers"], 3)
        launch_ledger = (DEPOSIT_RAW + LATE_DEPOSIT_RAW, DEPOSIT_RAW + LATE_DEPOSIT_RAW, 0, 0, 0, 2)
        successor_reserve = DEPOSIT_RAW + LATE_DEPOSIT_RAW - SUCCESSOR_BURN_RAW + SUCCESSOR_DEPOSIT_RAW
        assert_equal(ledger_view(info), expected_ledgers(
            launch_ledger,
            (SUCCESSOR_DEPOSIT_RAW, SUCCESSOR_BURN_RAW, DEPOSIT_RAW + LATE_DEPOSIT_RAW, 0, successor_reserve, 1)))
        assert_equal(self.wallet_asset(n0, asset_id)["confirmed"], held_before + SUCCESSOR_DEPOSIT_RAW)
        statuses = {}
        for node in self.bridge_nodes():
            status = node.getbridgeproofstatus(deposit_id=SUCCESSOR_DEPOSIT_ID)
            assert_equal((status["successor_deposit"]["deposit_id"], status["successor_deposit"]["claimed"]),
                         (SUCCESSOR_DEPOSIT_ID, True))
            # The launch vault's id 0 is its first deposit, claimed long ago.
            assert_equal((status["deposit"]["deposit_id"], status["deposit"]["claimed"]), (FIRST_DEPOSIT_ID, True))
            statuses[node.index] = status["successor_deposit"]

        # A RESERVE_E_SUCCESSOR at v''s cut 1 counts the deposit in P_pre:
        # invariant R on v' holds at margin 0 and supply still equals the
        # sum of reserve + transit over both entries.
        self.settle_fresh_lineage(BURN_FINALITY_LAG_MARGIN)
        _, info = self.observe("after the successor deposit", (RESERVE_SUCCESSOR,))
        reserve = info["observations"][SUBJECT_KEYS[RESERVE_SUCCESSOR]]["values"]
        assert_equal((reserve["next_deposit_id"], reserve["locked"], reserve["balance"]),
                     (SUCCESSOR_DEPOSIT_ID + 1, successor_reserve, successor_reserve))
        assert_equal(info["successor"]["reserve"]["invariant_r_margin"], 0)
        assert "conflict" not in info
        ledgers = ledger_view(info)
        backing = sum(ledger["reserve"] + ledger["transit"] for ledger in ledgers.values())
        wallets = sum(self.wallet_asset(node, asset_id)["confirmed"] for node in self.bridge_validators())
        assert_equal((wallets, backing), (successor_reserve, successor_reserve))
        self.evidence.assertion("deposit into v' minted under the successor entry id; v' P_pre counts it",
                                expected={"minted": SUCCESSOR_DEPOSIT_RAW, "proven_deposits": 1, "claimed": True,
                                          "invariant_r_margin": 0},
                                actual={"successor": info["successor"]["reserve"], "proof_status": statuses},
                                mint_txid=mint_txid, mint_height=mint_height, deposit_block=k, replay=replay,
                                observed=reserve)

    # -- 13. final accounting ------------------------------------------------------------------------
    def check_final_accounting(self):
        self.log.info("(13) Final ledgers, Ethereum balances and bUSD V2 supply == sum of reserve + transit")
        anvil = self.anvil
        info = self.v2_infos()
        supply = DEPOSIT_RAW + LATE_DEPOSIT_RAW - SUCCESSOR_BURN_RAW + SUCCESSOR_DEPOSIT_RAW
        self.final_ledgers = expected_ledgers(
            (DEPOSIT_RAW + LATE_DEPOSIT_RAW, DEPOSIT_RAW + LATE_DEPOSIT_RAW, 0, 0, 0, 2),
            (SUCCESSOR_DEPOSIT_RAW, SUCCESSOR_BURN_RAW, DEPOSIT_RAW + LATE_DEPOSIT_RAW, 0, supply, 1))
        ledgers = ledger_view(info)
        assert_equal(ledgers, self.final_ledgers)
        for ledger in ledgers.values():
            # reserve_v = minted + credited - burned on each entry.
            assert_equal(ledger["reserve"], ledger["minted"] + ledger["credited"] - ledger["burned"])
        backing = sum(ledger["reserve"] + ledger["transit"] for ledger in ledgers.values())
        # Every bUSD V2 unit is in a validator wallet (n0 received all three mints).
        wallets = sum(self.wallet_asset(node, self.v2info["asset_id"])["confirmed"] for node in self.bridge_validators())
        assert_equal((wallets, backing), (supply, supply))
        assert_equal((info["nullifiers"], info["withdrawals"]), (3, 3))
        assert_equal(hx(info["withdrawal_root_evm"]), withdrawal_root([leaf for _, leaf in self.withdrawal_leaves]))
        assert "conflict" not in info
        assert_greater_than_or_equal(info["reserve"]["invariant_r_margin"], 0)
        assert_greater_than_or_equal(info["successor"]["reserve"]["invariant_r_margin"], 0)
        ethereum = {
            "v": {"locked": call_uint(anvil, self.vault, "locked()"),
                  "balance": call_uint(anvil, self.token, "balanceOf(address)", enc_address(self.vault)),
                  "absorbed": call_uint(anvil, self.vault, "absorbedTotal()"),
                  "next_deposit_id": call_uint(anvil, self.vault, "nextDepositId()")},
            "v'": {"locked": call_uint(anvil, self.successor_vault, "locked()"),
                   "balance": call_uint(anvil, self.token, "balanceOf(address)", enc_address(self.successor_vault)),
                   "absorbed": call_uint(anvil, self.successor_vault, "absorbedTotal()"),
                   "next_deposit_id": call_uint(anvil, self.successor_vault, "nextDepositId()")},
            "recipient": call_uint(anvil, self.token, "balanceOf(address)", enc_address(ETH_RECIPIENT)),
        }
        assert_equal(ethereum, {
            "v": {"locked": 0, "balance": 0, "absorbed": 0, "next_deposit_id": LATE_DEPOSIT_ID + 1},
            "v'": {"locked": supply, "balance": supply, "absorbed": DEPOSIT_RAW + LATE_DEPOSIT_RAW,
                   "next_deposit_id": SUCCESSOR_DEPOSIT_ID + 1},
            "recipient": SUCCESSOR_BURN_RAW})
        # Relayer view: both v deposits are claimed; v' numbers its own ids,
        # and only its deposit 0 is claimed.
        statuses = {}
        for deposit_id in (FIRST_DEPOSIT_ID, LATE_DEPOSIT_ID):
            status = self.nodes[0].getbridgeproofstatus(deposit_id=deposit_id)
            assert_equal(status["store"], "v2")
            assert_equal((status["deposit"]["deposit_id"], status["deposit"]["claimed"]), (deposit_id, True))
            assert_equal((status["successor_deposit"]["deposit_id"], status["successor_deposit"]["claimed"]),
                         (deposit_id, deposit_id == SUCCESSOR_DEPOSIT_ID))
            statuses[deposit_id] = status
        self.evidence.assertion("final accounting: supply == sum of reserve + transit over both entries",
                                expected={"supply": supply, "ledgers": self.final_ledgers},
                                actual={"wallets": wallets, "backing": backing, "ledgers": ledgers},
                                ethereum=ethereum, proof_status=statuses)

    # -- 14. replay and reindex ------------------------------------------------------------------------
    def check_reindexed_v2_info(self, info):
        """RT4a's reindex check: the rebuilt state is RT4d's final state, and
        every node (n3 restarted from its datadir, n0 reindexed) agrees,
        successor included."""
        assert_equal(sorted(info["observations"]), sorted(SUBJECT_KEYS.values()))
        assert "conflict" not in info
        assert_equal((info["nullifiers"], info["withdrawals"]), (3, 3))
        assert_equal(hx(info["withdrawal_root_evm"]), withdrawal_root([leaf for _, leaf in self.withdrawal_leaves]))
        assert_equal(ledger_view(info), self.final_ledgers)
        assert_equal((info["retired"], info["successor"]["active"]), (True, True))
        reference = gate_view(info)
        for node in self.bridge_nodes():
            assert_equal(gate_view(node.getbridgev2info()), reference)
        self.evidence.assertion("n3 replay and n0 reindex agree with n1, successor included",
                                expected=self.final_ledgers, actual=ledger_view(info),
                                successor=info["successor"], nodes=[node.index for node in self.bridge_nodes()])


if __name__ == '__main__':
    B3BridgeRT4dMigrationTest(__file__).main()
