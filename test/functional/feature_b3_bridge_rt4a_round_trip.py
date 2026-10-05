#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""RT4a: the first bridge V2 round trip on regtest (node changes rev 11 §3-7).

REGTEST ONLY. Every process is local: four B3 regtest validators linked only
to each other, one anvil on 127.0.0.1 (chain 31337, never forked) and the
synthetic sync-committee tool. forge runs with --offline and writes its
build output, cache and broadcast records outside the source tree. Fresh
datadirs only; no external peer.

Fixture I with the fresh lineage (R_f = 262, GENESIS_HEIGHT 261) and a pin
file holding only a v2 section, written before the validators first start
(OI-3): the V2 contracts cannot exist yet because their constructors pin the
fresh lineage, so the pins name the CREATE addresses DeployV2.s.sol will use
(deployer = anvil account 2 at nonce 0) and carry no vault or verifier code
hash. The V1 registry stays the synthetic MANAGED_V1 bUSD. V = W2 = 281.

1. Bring-up to 267; the lineage id equals a Python derivation;
   getbridgev2info is identical on every node and equals the pins, its
   AssetId equals the Python bUSD V2 AssetId; getbridgeinfo is still the
   synthetic managed-v1 bUSD.
2. Fresh votes are shuttled until checkpoint 266 is finalized.
3. DeployV2.s.sol (START_CERTIFICATE, profile R windows with
   MIN_EPOCH_DURATION = REVIEW_WINDOW = 1 s) deploys at the predicted
   addresses; the vault's B3_ASSET_ID is the node's AssetId.
4. initialize with the epoch-0 certificate at 266 (below V): LIVE, deposits
   still closed.
5. Shuttling past V: every carried certificate signs withdrawalRoot 0 below
   V and the empty-tree root from V. All of them are relayed in order with
   prev_index = latestIndex(); the empty root is accepted and deposits open.
6. approve + deposit of 25,000,000 raw units to n0's RECIPIENT_V1.
7. b3-bridge-ethcheck builds the bootstrap, update and mint against the V2
   vault entry with the V2 receipt rules; the carriers fill the V2
   light-client store only and claimbridgedeposit mints bUSD V2 (nullifier
   namespace 2; getbridgeinfo's V1 nullifier count stays 0); a replay is
   refused. A plain burnasset of bUSD V2 is refused without
   allow_unredeemable_burn (node changes §7.5).
8. bridgewithdraw burns 10,000,000 (decentralized-verifier-v2; the burn gate
   is TEMPORARY: fresh lineage live only, kind 9 pending); the leaf V2 and
   the one-leaf root equal the Python derivation.
9. Shuttling until a certificate covers the burn and fresh epoch 1 has
   certified, relayed in order (so the sequence includes a Verifier V2
   handover to Set'_1); acceptedRootHeight(root) is the first covering
   certificate's height.
10. getbridgewithdrawalproofv2 is identical on every node and re-derived in
    Python; certificates below the burn or below V are refused.
11. release pays the recipient exactly once; the second release reverts
    (AlreadyReleased) on chain and changes no balance. The relayer RPCs
    getbridgeproofstatus, getbridgeanchorforblock and
    getbridgelightclientstore serve the V2 store and Vault V2 deposits.
12. n3 restarted with a modified v2 section is refused by the pin-file
    guard, then restarts unchanged; n0 -reindex reproduces n1's
    getbridgev2info and fresh lineage. Evidence: bridge_evidence.json.
"""

import json
import os

from test_framework.anvil import AnvilRPCError, deploy
from test_framework.b3bridge import (
    BRIDGE_ACTIVATION_HEIGHT,
    BRIDGE_V2_DEFAULT_CAP_RAW,
    CHECKPOINT_INTERVAL,
    DEPLOY_V2_CONTRACTS,
    EMPTY_WITHDRAWAL_ROOT,
    EPOCH_LENGTH,
    PERIOD_SLOTS,
    REGTEST_ETH_CHAIN_ID,
    REGTEST_GVR,
    REGTEST_MODERN_GENESIS_HASH,
    VALIDATOR_COUNT,
    VAULT_V2_RELEASE_SIGNATURE,
    VAULT_V2_WITHDRAWAL_LEAF_SIGNATURE,
    VERIFIER_V2_INITIALIZE_SIGNATURE,
    VERIFIER_V2_STATUS_LIVE,
    VERIFIER_V2_STATUS_UNINITIALIZED,
    VERIFIER_V2_SUBMIT_SIGNATURE,
    B3BridgeChainMixin,
    FreshVoteShuttle,
    abi_call,
    asset_id_v2,
    build_direct_mint_plan,
    call_address,
    call_bytes32,
    call_uint,
    decode_finalized_block_v2,
    enc_address,
    enc_bytes32,
    enc_uint,
    encode_submit_certificate,
    encode_vault_v2_release,
    encode_verifier_v2_initialize,
    encode_withdrawal_v2_tuple,
    file_sha256,
    find_deposits,
    fold_withdrawal_path,
    hx,
    keccak256,
    leaf_v2,
    leaf_v2_preimage,
    predict_create_address,
    recipient_v1,
    regtest_chain_domain,
    rpc_refusal,
    selector,
    start_refused,
    to_0x,
    withdrawal_root,
    withdrawal_root_and_path,
    write_regtest_bridge_pins_v2,
)
from test_framework.key import TaggedHash
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
)

# -- fresh lineage (regtest plan §3.3, §3.6) ---------------------------------
FRESH_ACTIVATION = 262  # R_f
FRESH_GENESIS = FRESH_ACTIVATION - 1  # GENESIS_HEIGHT, history leaf 0
FRESH_EXTENSION = 1280  # profile R
FRESH_ARGS = [
    f"-b3freshlineageheight={FRESH_ACTIVATION}",
    f"-b3freshmaxepochextension={FRESH_EXTENSION}",
]
LINEAGE_V2_TAG = "B3/FINALITY/LINEAGE/V2"
INIT_CHECKPOINT = FRESH_GENESIS + CHECKPOINT_INTERVAL  # 266, checkpoint index 1
BRING_UP_HEIGHT = INIT_CHECKPOINT + 1

# -- bridge V2 schedule and amounts -------------------------------------------
BRIDGE_V2_HEIGHT = 281  # V: the Verifier V2 BRIDGE_ACTIVATION_HEIGHT
BRIDGE_V2_BURN_HEIGHT = BRIDGE_V2_HEIGHT  # W2
assert (BRIDGE_V2_HEIGHT - FRESH_GENESIS) % CHECKPOINT_INTERVAL == 0
assert BRIDGE_V2_HEIGHT >= max(FRESH_ACTIVATION, BRIDGE_ACTIVATION_HEIGHT)
assert FRESH_GENESIS < INIT_CHECKPOINT < BRIDGE_V2_HEIGHT
DEPOSIT_RAW = 25_000_000
BURN_RAW = 10_000_000
MAX_DEPOSIT_RAW = BRIDGE_V2_DEFAULT_CAP_RAW  # Vault V2 default; also both node caps
MIN_BRIDGE_TOTAL_WEIGHT = 4
TOKEN_SUPPLY = 10**12
FIRST_DEPOSIT_ID = 0
ETH_RECIPIENT = "0x" + keccak256(b"B3-REGTEST-RT4A/ethereum-recipient")[12:].hex()
# DeployV2's CREATEs from the deployer, nonces 0..3 (contracts/script/DeployV2.s.sol).
DEPLOY_ROLES = ("prover", "verifier", "vault", "batch")
UNPINNED = ["vault_runtime_code_hash", "verifier_runtime_code_hash", "lineage_id"]

# Explicit gas limits stay below EIP-7825's 2^24 per-transaction cap.
MAX_TX_GAS = 16_000_000

RPC_MISC_ERROR = -1
RPC_INVALID_ADDRESS_OR_KEY = -5
RPC_INVALID_PARAMETER = -8
RPC_VERIFY_REJECTED = -26

NODE_PINS_CHANGED_ERROR = ("-b3bridgepins differs from the pin file recorded for this datadir, which already "
                           "stores blocks at or above bridge activation; start from a fresh -datadir")

# getfinalitylineage fields every node must agree on (node-local signer and
# vote-pool fields left out), as in feature_b3_fresh_lineage.py.
LINEAGE_VIEW_KEYS = (
    "configured", "staging_signer", "activation_height", "genesis_height",
    "max_epoch_extension", "checkpoint_interval", "chain_domain", "tip_height",
    "state_available", "bootstrapped", "genesis_block_hash", "genesis_block_hash_evm",
    "genesis_leaf", "lineage_id", "genesis_set_hash", "epoch", "epoch_starts",
    "handover_certified", "lineage_broken", "break_height", "genesis_set",
    "previous_set", "current_set", "next_set", "finalized", "history",
)


def mid_period_slot_offset(checkpoint_block):
    """slot_offset putting slot(checkpoint_block) mid sync-committee period.

    The checkpoint and every block this test finalizes stay in one period.
    """
    return PERIOD_SLOTS * (checkpoint_block // PERIOD_SLOTS + 2) + PERIOD_SLOTS // 2 - checkpoint_block


def gas_used(receipt):
    return int(receipt["gasUsed"], 16)


def raw_block_hash(node, height):
    """A block hash as B3 signs it (internal byte order)."""
    return bytes.fromhex(node.getblockhash(height))[::-1]


def revert_data(anvil, to, data, sender):
    """The revert data of an eth_call that must revert."""
    try:
        anvil.call(to, data, sender=sender)
    except AnvilRPCError as e:
        assert isinstance(e.data, str), f"revert without data: {e}"
        return hx(e.data)
    raise AssertionError("the call did not revert")


def error_selector(name):
    return selector(f"{name}()")


class B3BridgeRT4aRoundTripTest(FreshVoteShuttle, B3BridgeChainMixin, BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_foundry(("anvil", "forge"))
        self.skip_if_no_bridge_tools()

    # -- Ethereum side and pins (before any node starts) --------------------
    def setup_network(self):
        self.log.info("anvil: MockTetherToken by account 0, depositor funded")
        anvil = self.anvil = self.start_anvil()
        self.owner, self.depositor, self.deployer, self.relayer = anvil.accounts()[:4]
        self.bridge_forge_build()
        self.token, token_block, self.token_code_hash = deploy(
            anvil, self.owner, self.bridge_artifact("MockTetherToken.sol", "MockTetherToken"))
        anvil.send(self.owner, self.token, abi_call("issue(uint256)", enc_uint(TOKEN_SUPPLY)))
        anvil.send(self.owner, self.token, abi_call(
            "transfer(address,uint256)", enc_address(self.depositor), enc_uint(TOKEN_SUPPLY)))

        self.log.info("Predict DeployV2's addresses; checkpoint at the anvil head; v2-only pin file")
        assert_equal(int(anvil.rpc("eth_getTransactionCount", self.deployer, "latest"), 16), 0)
        self.predicted = {role: predict_create_address(self.deployer, nonce)
                          for nonce, role in enumerate(DEPLOY_ROLES)}
        self.verifier, self.vault = self.predicted["verifier"], self.predicted["vault"]
        for address in self.predicted.values():
            assert_equal(anvil.code(address), b"")
        checkpoint_block = anvil.block_number()
        # The vault deploys later; the checkpoint block is a safe lower bound
        # for its first event (node changes §3.1 origin_deployment_block).
        self.origin_deployment_block = checkpoint_block
        self.slot_offset = mid_period_slot_offset(checkpoint_block)
        self.beacon = self.start_synthetic_beacon(anvil, self.slot_offset, checkpoint_block)
        self.bootstrap = self.beacon.bootstrap()
        assert_equal(self.bootstrap["checkpoint_block"], checkpoint_block)
        assert_equal(self.bootstrap["checkpoint_slot"] % PERIOD_SLOTS, PERIOD_SLOTS // 2)
        self.pins_path = os.path.join(self.options.tmpdir, "b3bridgepins-v2.json")
        self.pins = write_regtest_bridge_pins_v2(
            self.pins_path, anvil, vault=self.vault, token=self.token, verifier=self.verifier,
            deployer=self.deployer, deployment_block=self.origin_deployment_block,
            bootstrap_output=self.bootstrap, activation_height=BRIDGE_V2_HEIGHT,
            burn_height=BRIDGE_V2_BURN_HEIGHT, max_deposit_raw=MAX_DEPOSIT_RAW,
            min_bridge_total_weight=MIN_BRIDGE_TOTAL_WEIGHT)
        assert_equal(sorted(self.pins), ["format", "v2"])
        assert_equal(hx(self.pins["v2"]["token_runtime_code_hash"]), self.token_code_hash)
        self.domain = regtest_chain_domain()
        self.asset_id_raw = asset_id_v2(self.domain, REGTEST_ETH_CHAIN_ID, self.vault, self.token)
        self.evidence.step("ethereum token deployed and v2 pins written", eth_block=anvil.block_number(),
                           token=self.token, token_block=token_block, predicted=self.predicted,
                           checkpoint_block=checkpoint_block,
                           checkpoint_slot=self.bootstrap["checkpoint_slot"],
                           checkpoint_root=self.bootstrap["checkpoint_root"],
                           pins_sha256=file_sha256(self.pins_path),
                           asset_id_v2=to_0x(self.asset_id_raw))
        self.start_bridge_validators(self.pins_path, extra_args=FRESH_ARGS)

    # -- test ---------------------------------------------------------------
    def run_test(self):
        self.withdrawal_leaves = []  # (b3 height, leaf V2) in id order
        self.relayed = []

        self.log.info("(1) Corridor, stakes and live V1 finality past the fresh genesis")
        self.bring_up_validators(BRING_UP_HEIGHT)
        self.check_lineage()
        self.v2info = self.check_v2_info_before_activation()

        self.log.info("(2) Shuttle fresh votes until checkpoint 266 is finalized")
        self.resume()
        self.shuttle_until(lambda: all(self.fresh_finalized_height(node) >= INIT_CHECKPOINT
                                       for node in self.bridge_validators()))
        self.freeze()
        self.evidence_step("fresh checkpoint 266 finalized", self.anvil,
                           fresh_finalized=self.nodes[0].getfinalitylineage()["finalized"])

        self.deploy_v2_stack()
        self.initialize_verifier()

        self.log.info("(5) Shuttle past V and relay every certificate in order")
        self.resume()
        self.shuttle_until(lambda: all(self.fresh_finalized_height(node) >= BRIDGE_V2_HEIGHT
                                       for node in self.bridge_validators()))
        self.freeze()
        self.check_carried_roots()
        self.relay_certificates("past V")
        self.check_deposits_open()

        deposit = self.make_deposit()
        plan = self.build_plan(deposit)
        self.relay_carriers(plan, deposit)
        self.claim_mint(plan, deposit)
        self.burn()
        covering = self.relay_withdrawal_root()
        proof = self.check_withdrawal_proof(covering)
        self.release(proof)
        self.check_v2_relayer_rpcs(deposit)
        self.check_pin_guard_and_reindex()
        self.evidence_step("RT4a complete", self.anvil, fresh_votes=dict(self.fresh_vote_results),
                           relayed=len(self.relayed))

    # -- 1. bring-up --------------------------------------------------------
    def check_lineage(self):
        self.log.info("The fresh lineage id equals an independent Python derivation")
        validators = self.bridge_validators()
        self.wait_until(lambda: all(node.getfinalitylineage().get("bootstrapped") for node in validators),
                        timeout=120)
        infos = [node.getfinalitylineage() for node in validators]
        for info in infos[1:]:
            for key in ("lineage_id", "chain_domain", "genesis_block_hash_evm", "genesis_set_hash", "genesis_set"):
                assert_equal(info[key], infos[0][key])
        info = infos[0]
        genesis_hash = raw_block_hash(self.nodes[0], FRESH_GENESIS)
        assert_equal(info["genesis_height"], FRESH_GENESIS)
        assert_equal(hx(info["genesis_block_hash_evm"]), genesis_hash)
        assert_equal(hx(info["chain_domain"]), self.domain)
        assert_equal(hx(info["genesis_set"]["set_hash"]), hx(info["genesis_set_hash"]))
        assert_equal(info["genesis_set"]["validator_count"], VALIDATOR_COUNT)
        expected = TaggedHash(LINEAGE_V2_TAG, self.domain + FRESH_GENESIS.to_bytes(8, "big") + genesis_hash +
                              hx(info["genesis_set_hash"]))
        assert_equal(hx(info["lineage_id"]), expected)
        self.lineage_id = info["lineage_id"]
        self.lineage = info
        self.evidence.assertion("fresh lineage id derived independently", expected=to_0x(expected),
                                actual=info["lineage_id"], genesis_set_hash=info["genesis_set_hash"])

    def v2_infos(self):
        """getbridgev2info of every running node; all must be identical (frozen)."""
        infos = [node.getbridgev2info() for node in self.bridge_nodes()]
        for info in infos[1:]:
            assert_equal(info, infos[0])
        assert_equal(infos[0]["state_available"], True)
        return infos[0]

    def check_v2_info_before_activation(self):
        self.log.info("getbridgev2info equals the pins on every node; getbridgeinfo is still managed-v1")
        self.freeze()
        info = self.v2_infos()
        v2 = self.pins["v2"]
        light = v2["light_client"]
        assert_equal(info["configured"], True)
        assert_equal((info["activation_height"], info["burn_height"]), (BRIDGE_V2_HEIGHT, BRIDGE_V2_BURN_HEIGHT))
        assert_greater_than(BRIDGE_V2_HEIGHT, info["next_height"])
        assert_equal((info["active"], info["burns_active"]), (False, False))
        assert_equal(info["origin_chain_id"], REGTEST_ETH_CHAIN_ID)
        for key in ("vault", "token", "verifier", "token_runtime_code_hash"):
            assert_equal(hx(info[key]), hx(v2[key]))
        for key in ("origin_deployment_block", "max_deposit_raw", "min_bridge_validators",
                    "max_bridge_validators", "min_bridge_total_weight"):
            assert_equal(info[key], v2[key])
        assert_equal((info["max_per_block"], info["max_per_epoch"]),
                     (BRIDGE_V2_DEFAULT_CAP_RAW, BRIDGE_V2_DEFAULT_CAP_RAW))
        assert_equal(info["epoch_length_blocks"], EPOCH_LENGTH)
        assert "vault_runtime_code_hash" not in info and "verifier_runtime_code_hash" not in info
        assert_equal(info["unpinned"], UNPINNED)
        assert_equal(hx(info["asset_id_evm"]), self.asset_id_raw)
        assert_equal(info["asset_id"], self.asset_id_raw[::-1].hex())
        assert_equal(info["vault_entry_id"], hx(info["vault_entry_id_evm"])[::-1].hex())
        lc = info["light_client"]
        assert_equal(hx(lc["trusted_checkpoint_root"]), hx(light["trusted_checkpoint_root"]))
        assert_equal(lc["trusted_checkpoint_slot"], light["trusted_checkpoint_slot"])
        assert_equal(hx(lc["genesis_validators_root"]), REGTEST_GVR)
        assert_equal(lc["fork_schedule_valid_through_epoch"], light["fork_schedule_valid_through_epoch"])
        assert_equal((lc["bootstrapped"], lc["anchors"]), (False, 0))
        assert_equal((info["nullifiers"], info["withdrawals"]), (0, 0))
        assert_equal(hx(info["withdrawal_root_evm"]), EMPTY_WITHDRAWAL_ROOT)
        assert_equal(hx(info["empty_withdrawal_root_evm"]), EMPTY_WITHDRAWAL_ROOT)
        assert_equal(info["burn_gate"]["temporary"], True)

        for node in self.bridge_nodes():
            assert_equal(node.getblockhash(0), REGTEST_MODERN_GENESIS_HASH)
            v1 = node.getbridgeinfo()
            assert_equal(v1["withdrawal_mode"], "managed-v1")
            assert hx(v1["vault"]) != hx(self.vault)
        self.evidence.assertion("getbridgev2info equals the v2 pins on every node", expected=v2,
                                actual=info)
        return info

    # -- 3. DeployV2 --------------------------------------------------------
    def deploy_v2_stack(self):
        self.log.info("(3) DeployV2.s.sol, START_CERTIFICATE, at the predicted addresses")
        anvil, lineage = self.anvil, self.lineage
        genesis_set_hash = to_0x(hx(lineage["genesis_set_hash"]))
        env = {
            "B3V2_CHAIN_DOMAIN": to_0x(self.domain),
            "B3V2_GENESIS_HEIGHT": FRESH_GENESIS,
            "B3V2_GENESIS_BLOCK_HASH": to_0x(hx(lineage["genesis_block_hash_evm"])),
            "B3V2_GENESIS_SET_HASH": genesis_set_hash,
            "B3V2_LINEAGE_ID": to_0x(hx(lineage["lineage_id"])),
            "B3V2_START_EPOCH": 0,
            "B3V2_START_SET_HASH": genesis_set_hash,
            "B3V2_BOOTSTRAP_MODE": 1,  # START_CERTIFICATE (K_OF_N is the next slice)
            "B3V2_BRIDGE_ACTIVATION_HEIGHT": BRIDGE_V2_HEIGHT,
            "B3V2_ORIGIN_TOKEN": self.token,
            "B3V2_MIN_BRIDGE_TOTAL_WEIGHT": MIN_BRIDGE_TOTAL_WEIGHT,
            "B3V2_MIN_EPOCH_DURATION": 1,
            "B3V2_REVIEW_WINDOW": 1,
            "B3V2_MAX_DEPOSIT_RAW": MAX_DEPOSIT_RAW,
        }
        created = self.bridge_deploy_v2(anvil, self.deployer, env)
        for name, role in zip(DEPLOY_V2_CONTRACTS, DEPLOY_ROLES):
            assert_equal(created[name]["address"], self.predicted[role])
            assert anvil.code(self.predicted[role]), name
            self.evidence.gas(f"DeployV2 {name}", created[name]["gas_used"], block=created[name]["block"])
        assert_equal(int(anvil.rpc("eth_getTransactionCount", self.deployer, "latest"), 16), 4)

        verifier, vault = self.verifier, self.vault
        node_asset = hx(self.v2info["asset_id_evm"])
        assert_equal(call_bytes32(anvil, vault, "B3_ASSET_ID()"), node_asset)
        assert_equal(call_address(anvil, vault, "VERIFIER()"), verifier)
        assert_equal(call_bytes32(anvil, vault, "VERIFIER_CODE_HASH()"), anvil.code_hash(verifier))
        assert_equal(call_address(anvil, vault, "ORIGIN_TOKEN()"), self.token)
        assert_equal(call_bytes32(anvil, vault, "TOKEN_CODE_HASH()"), hx(self.pins["v2"]["token_runtime_code_hash"]))
        assert_equal(call_bytes32(anvil, vault, "B3_CHAIN_DOMAIN()"), self.domain)
        assert_equal(call_uint(anvil, vault, "ORIGIN_CHAIN_ID()"), REGTEST_ETH_CHAIN_ID)
        assert_equal(call_uint(anvil, vault, "MAX_DEPOSIT_RAW()"), self.pins["v2"]["max_deposit_raw"])
        assert_equal(call_uint(anvil, vault, "BRIDGE_ACTIVATION_HEIGHT()"), BRIDGE_V2_HEIGHT)
        assert_equal(call_address(anvil, verifier, "PROVER()"), self.predicted["prover"])
        assert_equal(call_uint(anvil, verifier, "BRIDGE_ACTIVATION_HEIGHT()"), BRIDGE_V2_HEIGHT)
        assert_equal(call_bytes32(anvil, verifier, "LINEAGE_ID()"), hx(lineage["lineage_id"]))
        assert_equal(call_bytes32(anvil, verifier, "CHAIN_DOMAIN()"), self.domain)
        assert_equal(call_uint(anvil, verifier, "GENESIS_HEIGHT()"), FRESH_GENESIS)
        assert_equal(call_bytes32(anvil, verifier, "START_SET_HASH()"), hx(genesis_set_hash))
        assert_equal(call_uint(anvil, verifier, "BOOTSTRAP_MODE()"), 1)
        assert_equal(call_uint(anvil, verifier, "MIN_BRIDGE_VALIDATORS()"), self.pins["v2"]["min_bridge_validators"])
        assert_equal(call_uint(anvil, verifier, "MAX_BRIDGE_VALIDATORS()"), self.pins["v2"]["max_bridge_validators"])
        assert_equal(call_uint(anvil, verifier, "MIN_BRIDGE_TOTAL_WEIGHT()"), MIN_BRIDGE_TOTAL_WEIGHT)
        assert_equal(call_uint(anvil, verifier, "status()"), VERIFIER_V2_STATUS_UNINITIALIZED)
        # The ABI encoders below must target the deployed functions.
        verifier_ids = self.bridge_method_ids("B3FinalityVerifierV2.sol", "B3FinalityVerifierV2")
        vault_ids = self.bridge_method_ids("B3VaultV2.sol", "B3VaultV2")
        for ids, signature in ((verifier_ids, VERIFIER_V2_INITIALIZE_SIGNATURE),
                               (verifier_ids, VERIFIER_V2_SUBMIT_SIGNATURE),
                               (vault_ids, VAULT_V2_RELEASE_SIGNATURE),
                               (vault_ids, VAULT_V2_WITHDRAWAL_LEAF_SIGNATURE)):
            assert_equal(ids[signature], selector(signature).hex())
        self.evidence_step("DeployV2 deployed at the predicted addresses", anvil,
                           addresses={name: created[name]["address"] for name in created},
                           asset_id=to_0x(node_asset),
                           # OI-3: observed only; B3 pins neither in this slice.
                           vault_code_hash=to_0x(anvil.code_hash(vault)),
                           verifier_code_hash=to_0x(anvil.code_hash(verifier)))

    # -- Ethereum transactions ----------------------------------------------
    def contract_errors(self):
        """{selector: signature} of every custom error of the V2 contracts."""
        if getattr(self, "_contract_errors", None) is None:
            self._contract_errors = {}
            for file, contract in (("B3FinalityVerifierV2.sol", "B3FinalityVerifierV2"),
                                   ("B3VaultV2.sol", "B3VaultV2"),
                                   ("BlsDigestProver.sol", "BlsDigestProver")):
                for entry in self.bridge_artifact(file, contract)["abi"]:
                    if entry["type"] == "error":
                        signature = f"{entry['name']}({','.join(i['type'] for i in entry['inputs'])})"
                        self._contract_errors[selector(signature)] = signature
        return self._contract_errors

    def transact(self, to, data, label, sender=None, **extra):
        """Send a transaction that must succeed, with an explicit gas limit.

        In one RT4a run a submitCertificate sent without a gas limit (anvil
        fills in its own estimate) ran out of gas on chain, so the limit is
        twice eth_estimateGas plus 100,000 (capped below 2^24); evidence
        records the estimate and the gas used. A simulated revert is
        reported by its error name.
        """
        sender = sender or self.relayer
        tx = {"from": sender, "to": to, "data": to_0x(data)}
        try:
            estimate = int(self.anvil.rpc("eth_estimateGas", tx), 16)
        except AnvilRPCError as e:
            revert = hx(e.data)[:4] if isinstance(e.data, str) else b""
            name = self.contract_errors().get(revert, revert.hex() or "no data")
            raise AssertionError(f"{label} reverts in simulation: {name} ({e.message})") from None
        receipt = self.anvil.send(sender, to, data, gas=min(2 * estimate + 100_000, MAX_TX_GAS))
        self.evidence.gas(label, gas_used(receipt), estimate=estimate, block=int(receipt["blockNumber"], 16),
                          **extra)
        return receipt

    # -- certificates -------------------------------------------------------
    def export_certificate(self, inclusion, prev_index=None):
        """getfinalitylineagecertificate, identical on every node and self-checked."""
        args = (inclusion,) if prev_index is None else (inclusion, prev_index)
        exports = [node.getfinalitylineagecertificate(*args) for node in self.bridge_nodes()]
        for export in exports[1:]:
            assert_equal(export, exports[0])
        cert = exports[0]
        assert_equal(cert["inclusion_height"], inclusion)
        assert_equal(hx(cert["cert_id"]), keccak256(hx(cert["encoded"])))
        assert_equal(cert["function"], VERIFIER_V2_SUBMIT_SIGNATURE)
        # The harness's encoder reproduces the node's calldata byte for byte.
        assert_equal(encode_submit_certificate(cert), hx(cert["calldata"]))
        return cert

    def expected_withdrawal_root(self, height):
        """Zero below V, else the V2 tree over the leaves burned at or below height."""
        if height < BRIDGE_V2_HEIGHT:
            return bytes(32)
        return withdrawal_root([leaf for burned_at, leaf in self.withdrawal_leaves if burned_at <= height])

    def check_carried_roots(self):
        carriers = self.fresh_certificates(self.nodes[0], FRESH_ACTIVATION)
        observed = []
        for carrier in carriers:
            fb = carrier["fb"]
            assert_equal(fb["lineage_id"], hx(self.lineage_id))
            assert_equal(fb["withdrawal_root"], self.expected_withdrawal_root(fb["height"]))
            assert_equal(fb["bridge_flags"], 0)
            observed.append((carrier["inclusion"], fb["height"], fb["epoch"], to_0x(fb["withdrawal_root"])))
        assert any(fb_height >= BRIDGE_V2_HEIGHT for _, fb_height, _, _ in observed)
        self.evidence.assertion("carried certificates: root zero below V, V2 tree root from V",
                                expected={"below_v": to_0x(bytes(32)), "from_v": to_0x(EMPTY_WITHDRAWAL_ROOT)},
                                actual=observed)
        return carriers

    def initialize_verifier(self):
        self.log.info("(4) initialize with the epoch-0 certificate at 266 (START_CERTIFICATE)")
        anvil, n0 = self.anvil, self.nodes[0]
        carriers = [c for c in self.fresh_certificates(n0, FRESH_ACTIVATION)
                    if c["fb"]["height"] < BRIDGE_V2_HEIGHT]
        assert carriers, "no fresh certificate below V was carried"
        chosen = next((c for c in carriers if c["fb"]["height"] == INIT_CHECKPOINT), carriers[0])
        cert = self.export_certificate(chosen["inclusion"])
        fb = decode_finalized_block_v2(cert["encoded"])
        assert_equal(fb["encoded"], chosen["fb"]["encoded"])
        assert_equal((fb["epoch"], fb["withdrawal_root"], fb["bridge_flags"]), (0, bytes(32), 0))
        assert FRESH_GENESIS < fb["height"] < BRIDGE_V2_HEIGHT
        # No certificate can land before initialize.
        assert_equal(revert_data(anvil, self.verifier, hx(cert["calldata"]), self.relayer),
                     error_selector("NotLive"))
        self.transact(self.verifier, encode_verifier_v2_initialize(cert),
                      "B3FinalityVerifierV2.initialize (START_CERTIFICATE)", height=fb["height"],
                      signers=cert["signer_count"], absent=cert["absent_indices"])
        assert_equal(call_uint(anvil, self.verifier, "status()"), VERIFIER_V2_STATUS_LIVE)
        assert_equal(call_uint(anvil, self.verifier, "latestIndex()"), fb["checkpoint_index"])
        assert_equal(call_uint(anvil, self.verifier, "latestHeight()"), fb["height"])
        assert_equal(call_uint(anvil, self.verifier, "currentEpoch()"), 0)
        assert_equal(call_bytes32(anvil, self.verifier, "currentSetHash()"), hx(cert["signing_set"]["set_hash"]))
        assert_equal(call_bytes32(anvil, self.verifier, "nextSetHash()"), fb["validator_set_hash"])
        assert_equal(call_uint(anvil, self.verifier, "latestBridgeHeight()"), 0)
        assert_equal(call_uint(anvil, self.verifier, "depositsOpen()"), 0)
        assert_equal(call_uint(anvil, self.vault, "depositGateOpen()"), 0)
        # Deposits wait for an ordinary certificate at or above V.
        closed = revert_data(anvil, self.vault, abi_call(
            "deposit(uint256,bytes32)", enc_uint(DEPOSIT_RAW), bytes(11) + b"\x01" + b"\x22" * 20),
            self.depositor)
        assert_equal(closed, error_selector("DepositClosed"))
        self.init_cert = {"inclusion": chosen["inclusion"], "height": fb["height"],
                          "checkpoint_index": fb["checkpoint_index"],
                          "successor_set_hash": fb["validator_set_hash"]}
        self.evidence_step("verifier initialized (LIVE, deposits closed)", anvil, **self.init_cert,
                           cert_id=cert["cert_id"])

    def relay_certificates(self, label):
        """Relay every carried certificate above latestIndex(), in order."""
        anvil, n0 = self.anvil, self.nodes[0]
        latest_index = call_uint(anvil, self.verifier, "latestIndex()")
        relayed = []
        for carrier in self.fresh_certificates(n0, FRESH_ACTIVATION):
            fb = carrier["fb"]
            if fb["checkpoint_index"] <= latest_index:
                continue
            cert = self.export_certificate(carrier["inclusion"], latest_index)
            assert_equal(hx(cert["encoded"]), fb["encoded"])
            assert_equal(cert["history"]["prev_index"], latest_index)
            assert_equal(fb["withdrawal_root"], self.expected_withdrawal_root(fb["height"]))
            epoch_before = call_uint(anvil, self.verifier, "currentEpoch()")
            transition = fb["epoch"] == epoch_before + 1
            receipt = self.transact(self.verifier, hx(cert["calldata"]), "B3FinalityVerifierV2.submitCertificate",
                                    height=fb["height"], epoch=fb["epoch"], transition=transition,
                                    signers=cert["signer_count"], absent=cert["absent_indices"])
            latest_index = call_uint(anvil, self.verifier, "latestIndex()")
            assert_equal(latest_index, fb["checkpoint_index"])
            assert_equal(call_uint(anvil, self.verifier, "latestHeight()"), fb["height"])
            assert_equal(call_uint(anvil, self.verifier, "currentEpoch()"), fb["epoch"])
            relayed.append({"inclusion": carrier["inclusion"], "height": fb["height"], "epoch": fb["epoch"],
                            "checkpoint_index": fb["checkpoint_index"],
                            "withdrawal_root": to_0x(fb["withdrawal_root"]), "transition": transition,
                            "gas_used": gas_used(receipt), "signers": cert["signer_count"]})
        assert relayed, f"no new certificate to relay ({label})"
        self.relayed.extend(relayed)
        self.evidence_step(f"certificates relayed ({label})", anvil, relayed=relayed,
                           recorded_inclusions=sorted(self.fresh_inclusions))
        return relayed

    def check_deposits_open(self):
        anvil = self.anvil
        from_v = [entry for entry in self.relayed if entry["height"] >= BRIDGE_V2_HEIGHT]
        assert from_v, "no certificate at or above V was relayed"
        assert_equal(call_uint(anvil, self.verifier, "acceptedRootHeight(bytes32)",
                               enc_bytes32(EMPTY_WITHDRAWAL_ROOT)), from_v[0]["height"])
        assert_equal(call_uint(anvil, self.verifier, "latestBridgeHeight()"), self.relayed[-1]["height"])
        assert_equal(call_bytes32(anvil, self.verifier, "latestBridgeRoot()"), EMPTY_WITHDRAWAL_ROOT)
        assert_equal(call_uint(anvil, self.verifier, "depositsOpen()"), 1)
        assert_equal(call_uint(anvil, self.vault, "depositGateOpen()"), 1)
        self.evidence.assertion("empty V2 root accepted from V; deposits open",
                                expected=from_v[0]["height"],
                                actual=call_uint(anvil, self.verifier, "acceptedRootHeight(bytes32)",
                                                 enc_bytes32(EMPTY_WITHDRAWAL_ROOT)))

    # -- 6-7. deposit and mint ------------------------------------------------
    def make_deposit(self):
        self.log.info("(6) anvil: approve and deposit 25,000,000 raw units to n0's RECIPIENT_V1")
        anvil = self.anvil
        self.recipient = self.nodes[0].getnewaddress()
        anvil.send(self.depositor, self.token, abi_call(
            "approve(address,uint256)", enc_address(self.vault), enc_uint(TOKEN_SUPPLY)))
        receipt = self.transact(self.vault, abi_call("deposit(uint256,bytes32)", enc_uint(DEPOSIT_RAW),
                                                     recipient_v1(self.recipient)),
                                "B3VaultV2.deposit", sender=self.depositor)
        k = int(receipt["blockNumber"], 16)
        anvil.mine(1)  # The update's attested block k + 1.
        assert_equal(call_uint(anvil, self.vault, "locked()"), DEPOSIT_RAW)
        assert_equal(call_uint(anvil, self.vault, "nextDepositId()"), FIRST_DEPOSIT_ID + 1)
        assert_equal(call_uint(anvil, self.token, "balanceOf(address)", enc_address(self.vault)), DEPOSIT_RAW)

        deposits = find_deposits(anvil, self.vault, self.token, self.origin_deployment_block)
        assert_equal(len(deposits), 1)
        deposit = deposits[0]
        assert_equal((deposit["deposit_id"], deposit["block_number"], deposit["amount"]),
                     (FIRST_DEPOSIT_ID, k, DEPOSIT_RAW))
        assert_equal(deposit["recipient"], to_0x(recipient_v1(self.recipient)))
        assert_equal(deposit["block_hash"], anvil.block(k)["hash"])
        assert_equal(self.beacon.period_of_block(k + 1), self.bootstrap["period"])
        self.evidence_step("ethereum V2 deposit", anvil, deposit_block=k, deposit_id=FIRST_DEPOSIT_ID,
                           amount=DEPOSIT_RAW, recipient=self.recipient)
        return deposit

    def build_plan(self, deposit):
        self.log.info("(7) b3-bridge-ethcheck: bootstrap, update(k) and a mint naming the V2 vault entry")
        k = deposit["block_number"]
        entry_id = self.v2info["vault_entry_id"]
        plan = build_direct_mint_plan(os.path.join(self.options.tmpdir, "mint_plan"), self.beacon,
                                      self.anvil.url, deposit, entry_id, [k], vault=self.vault, token=self.token,
                                      bridge_v2=True)
        update = self.beacon.update(k)
        assert_equal(plan["bootstrap"]["payload_hex"], self.bootstrap["payload_hex"])
        assert_equal([u["kind"] for u in plan["updates"]], ["update"])
        assert_equal(plan["updates"][0]["payload_hex"], update["payload_hex"])
        assert_equal(plan["backfills"], [])
        assert_equal((update["finalized_block"], update["finalized_block_hash"]), (k, deposit["block_hash"]))
        mint = plan["mint"]
        assert_equal(mint["kind"], "mint")
        assert_equal(mint["bridge_v2"], True)  # ethcheck applied the V2 receipt rules
        assert_equal(hx(mint["registry_id"]), hx(entry_id))
        assert_equal((mint["deposit_id"], mint["origin_amount"], mint["amount"]),
                     (FIRST_DEPOSIT_ID, DEPOSIT_RAW, DEPOSIT_RAW))
        assert_equal((mint["target_block_number"], mint["target_block_hash"]), (k, deposit["block_hash"]))
        assert_equal(mint["b3_recipient"], deposit["recipient"])
        assert_equal(mint["origin_token"], self.token)
        self.evidence_step("mint plan verified by b3-bridge-ethcheck", self.anvil,
                           mint_bytes=len(mint["payload_hex"]) // 2, registry_id=entry_id)
        return plan

    def relay_carriers(self, plan, deposit):
        self.log.info("Bootstrap and update carriers fill the V2 light-client store only")
        n0 = self.nodes[0]
        self.freeze()
        assert_greater_than_or_equal(n0.getblockcount() + 1, BRIDGE_V2_HEIGHT)
        refusals = {
            "mint before bootstrap": rpc_refusal(
                RPC_VERIFY_REJECTED, "bridge mint precedes light-client bootstrap",
                n0.claimbridgedeposit, plan["mint"]["payload_hex"], DEPOSIT_RAW, self.recipient),
            "update before bootstrap": rpc_refusal(
                RPC_VERIFY_REJECTED, "bridge light-client update precedes bootstrap",
                n0.submitbridgecarrier, plan["updates"][0]["payload_hex"]),
        }

        def bootstrap():
            result = n0.submitbridgecarrier(plan["bootstrap"]["payload_hex"])
            assert_equal(result["record_kind"], "bootstrap")
            return result

        _, height = self.submit_and_mine("V2 bootstrap carrier mined", bootstrap)
        for node in self.bridge_nodes():
            lc = node.getbridgev2info()["light_client"]
            assert_equal(lc["bootstrapped"], True)
            assert_equal(lc["finalized_execution_block"], self.origin_deployment_block)
            assert_equal(hx(lc["finalized_execution_hash"]), hx(self.bootstrap["checkpoint_block_hash"]))
            assert_equal(lc["finalized_beacon_slot"], self.bootstrap["checkpoint_slot"])
            assert_greater_than_or_equal(lc["connected_height"], height)
            # The V1 store stays untouched from V (node changes §3.4).
            assert not node.getbridgeinfo().get("light_client_bootstrapped", False)
        refusals["bootstrap replay"] = rpc_refusal(
            RPC_VERIFY_REJECTED, "bridge light client is already bootstrapped",
            n0.submitbridgecarrier, plan["bootstrap"]["payload_hex"])

        def update():
            result = n0.submitbridgecarrier(plan["updates"][0]["payload_hex"])
            assert_equal(result["record_kind"], "update")
            return result

        _, self.update_height = self.submit_and_mine("V2 update carrier mined", update)
        k = deposit["block_number"]
        for node in self.bridge_nodes():
            lc = node.getbridgev2info()["light_client"]
            assert_equal(lc["finalized_execution_block"], k)
            assert_equal(hx(lc["finalized_execution_hash"]), hx(deposit["block_hash"]))
            assert_equal(lc["connected_height"], self.update_height)
            assert not node.getbridgeinfo().get("light_client_bootstrapped", False)
        self.evidence.assertion("V2 carrier ordering enforced", actual=refusals)

    def claim_mint(self, plan, deposit):
        self.log.info("claimbridgedeposit mints bUSD V2 on every node; the replay is refused")
        n0, n1 = self.nodes[0], self.nodes[1]
        mint = plan["mint"]["payload_hex"]
        asset_id = self.v2info["asset_id"]
        self.freeze()
        v1_nullifiers = [node.getbridgeinfo()["nullifiers"] for node in self.bridge_nodes()]
        assert_equal(v1_nullifiers, [0] * len(v1_nullifiers))

        def create():
            result = n0.claimbridgedeposit(mint, DEPOSIT_RAW, self.recipient)
            assert_equal(result["asset_id"], asset_id)
            assert_equal(result["amount"], DEPOSIT_RAW)
            assert_equal(result["owner_address"], self.recipient)
            assert_equal((result["mint_vout"], result["binding_vout"]), (0, 1))
            return result

        self.mint_txid, self.mint_height = self.submit_and_mine("bUSD V2 mint mined", create)
        assert_greater_than_or_equal(self.mint_height, BRIDGE_V2_HEIGHT)
        info = self.v2_infos()
        assert_equal(info["nullifiers"], 1)
        assert_equal(info["light_client"]["finalized_execution_block"], deposit["block_number"])
        held = self.wallet_asset(n0, asset_id)
        assert_equal(held["confirmed"], DEPOSIT_RAW)
        assert_equal([(u["txid"], u["vout"], u["amount"]) for u in held["utxos"]],
                     [(self.mint_txid, 0, DEPOSIT_RAW)])
        nullified = "bridge deposit is already nullified"
        replays = {
            "mint": rpc_refusal(RPC_VERIFY_REJECTED, nullified, n0.claimbridgedeposit, mint,
                                DEPOSIT_RAW, self.recipient),
            "mint relayed by n1": rpc_refusal(RPC_VERIFY_REJECTED, nullified, n1.claimbridgedeposit, mint,
                                              DEPOSIT_RAW, self.recipient),
        }
        # getbridgeinfo counts V1 keys only: the V2 mint is namespace 2.
        for node in self.bridge_nodes():
            assert_equal(node.getbridgeinfo()["nullifiers"], 0)
        self.evidence.assertion("bUSD V2 minted once (nullifier namespace 2); replays refused",
                                expected=DEPOSIT_RAW, actual=held["confirmed"], mint_txid=self.mint_txid,
                                mint_height=self.mint_height, replays=replays,
                                v1_getbridgeinfo_nullifiers=n0.getbridgeinfo()["nullifiers"])

        self.log.info("A plain burnasset of bUSD V2 is refused unless allow_unredeemable_burn is set")
        refusal = rpc_refusal(RPC_INVALID_PARAMETER, "burnasset would destroy bridge V2 bUSD without a withdrawal leaf",
                              n0.burnasset, asset_id, 1)
        # The opt-in builds an ordinary plain burn (consensus allows it); it
        # is not broadcast, so the wallet keeps every unit.
        unbroadcast = n0.burnasset(asset_id, 1, {"allow_unredeemable_burn": True, "broadcast": False})
        assert_equal(unbroadcast["broadcast"], False)
        assert_equal(n0.testmempoolaccept([unbroadcast["hex"]])[0]["allowed"], True)
        assert unbroadcast["txid"] not in n0.getrawmempool()
        assert_equal(self.wallet_asset(n0, asset_id)["confirmed"], DEPOSIT_RAW)
        self.evidence.assertion("plain bUSD V2 burnasset refused without the opt-in", actual=refusal,
                                opt_in_txid=unbroadcast["txid"])

    # -- 8-9. burn and its root ------------------------------------------------
    def burn(self):
        self.log.info("(8) bridgewithdraw burns bUSD V2 into a leaf V2 (TEMPORARY gate: fresh lineage live)")
        n0 = self.nodes[0]
        self.freeze()
        info = self.v2_infos()
        assert_equal((info["active"], info["burns_active"]), (True, True))
        assert_equal(info["burn_gate"]["status"], "READY")
        asset_id = self.v2info["asset_id"]

        def create():
            result = n0.bridgewithdraw(BURN_RAW, ETH_RECIPIENT)
            assert_equal(result["withdrawal_mode"], "decentralized-verifier-v2")
            assert_equal(result["withdrawal_status"], "pending-confirmation")
            assert_equal((result["asset_id"], result["amount"], result["asset_change"]),
                         (asset_id, BURN_RAW, DEPOSIT_RAW - BURN_RAW))
            assert_equal(hx(result["ethereum_recipient"]), hx(ETH_RECIPIENT))
            assert_equal((result["burn_vout"], result["binding_vout"]), (0, 1))
            assert result["proof_rpc_after_confirmation"].startswith(
                f"getbridgewithdrawalproofv2 {result['txid']} 0 ")
            return result

        self.burn_txid, self.burn_height = self.submit_and_mine("bUSD V2 burn mined", create)
        assert_greater_than_or_equal(self.burn_height, BRIDGE_V2_BURN_HEIGHT)
        self.leaf = leaf_v2(self.asset_id_raw, 0, REGTEST_ETH_CHAIN_ID, self.vault, self.token,
                            ETH_RECIPIENT, BURN_RAW, self.burn_height)
        self.withdrawal_leaves.append((self.burn_height, self.leaf))
        self.root = withdrawal_root([self.leaf])
        info = self.v2_infos()
        assert_equal(info["withdrawals"], 1)
        assert_equal(hx(info["withdrawal_root_evm"]), self.root)
        assert_equal(self.wallet_asset(n0, asset_id)["confirmed"], DEPOSIT_RAW - BURN_RAW)
        self.evidence.assertion("leaf V2 appended; one-leaf root equals Python", expected=to_0x(self.root),
                                actual=info["withdrawal_root_evm"], burn_txid=self.burn_txid,
                                burn_height=self.burn_height, leaf=to_0x(self.leaf),
                                burn_gate=info["burn_gate"])

    @staticmethod
    def fresh_finalized_epoch(node):
        return (node.getfinalitylineage().get("finalized") or {}).get("epoch", -1)

    def relay_withdrawal_root(self):
        self.log.info("(9) Shuttle until a certificate covers the burn and Set'_1 certifies; relay in order")
        self.resume()
        # Waiting for fresh epoch 1 as well puts a Verifier V2 handover
        # (signed by the pinned next set) into the relayed sequence.
        self.shuttle_until(lambda: all(self.fresh_finalized_height(node) >= self.burn_height and
                                       self.fresh_finalized_epoch(node) >= 1
                                       for node in self.bridge_validators()), timeout=600)
        self.freeze()
        self.check_carried_roots()
        self.relay_certificates("covering the burn")
        covering = next(entry for entry in self.relayed if entry["height"] >= self.burn_height)
        anvil = self.anvil
        assert_equal(call_uint(anvil, self.verifier, "acceptedRootHeight(bytes32)", enc_bytes32(self.root)),
                     covering["height"])
        assert_equal(call_bytes32(anvil, self.verifier, "latestBridgeRoot()"), self.root)
        transitions = [entry for entry in self.relayed if entry["transition"]]
        assert transitions, "no Verifier V2 epoch handover was relayed"
        epoch = call_uint(anvil, self.verifier, "currentEpoch()")
        assert_greater_than_or_equal(epoch, 1)
        assert_equal(call_bytes32(anvil, self.verifier, "setHashByEpoch(uint64)", enc_uint(1)),
                     self.init_cert["successor_set_hash"])
        assert_equal(call_bytes32(anvil, self.verifier, "currentSetHash()"),
                     call_bytes32(anvil, self.verifier, "setHashByEpoch(uint64)", enc_uint(epoch)))
        self.evidence.assertion("the burn's V2 root accepted at the covering certificate; handover relayed",
                                expected=covering["height"],
                                actual=call_uint(anvil, self.verifier, "acceptedRootHeight(bytes32)",
                                                 enc_bytes32(self.root)),
                                covering=covering, epoch_transitions=transitions, verifier_epoch=epoch)
        return covering

    # -- 10-11. proof and release ------------------------------------------------
    def check_withdrawal_proof(self, covering):
        self.log.info("(10) getbridgewithdrawalproofv2 is identical on every node and re-derived")
        n0 = self.nodes[0]
        inclusion = covering["inclusion"]
        proofs = [node.getbridgewithdrawalproofv2(self.burn_txid, 0, inclusion) for node in self.bridge_nodes()]
        for proof in proofs[1:]:
            assert_equal(proof, proofs[0])
        proof = proofs[0]
        assert_equal((proof["withdrawal_id"], proof["source_txid"], proof["burn_output_index"]),
                     (0, self.burn_txid, 0))
        assert_equal(proof["b3_height"], self.burn_height)
        assert_equal(proof["b3_block"], n0.getblockhash(self.burn_height))
        assert_equal(hx(proof["asset_id_evm"]), self.asset_id_raw)
        assert_equal(proof["destination_chain_id"], REGTEST_ETH_CHAIN_ID)
        for key, value in (("vault", self.vault), ("token", self.token), ("recipient", ETH_RECIPIENT)):
            assert_equal(hx(proof[key]), hx(value))
        assert_equal(proof["amount_raw"], BURN_RAW)
        preimage = leaf_v2_preimage(self.asset_id_raw, 0, REGTEST_ETH_CHAIN_ID, self.vault, self.token,
                                    ETH_RECIPIENT, BURN_RAW, self.burn_height)
        assert_equal(hx(proof["leaf_preimage"]), preimage)
        assert_equal(hx(proof["withdrawal_leaf"]), self.leaf)
        assert_equal(hx(proof["withdrawal_root"]), self.root)
        root, path = withdrawal_root_and_path([self.leaf], 0)
        assert_equal([hx(sibling) for sibling in proof["path"]], path)
        assert_equal(fold_withdrawal_path(self.leaf, 0, path), root)
        certificate = proof["certificate"]
        assert_equal((certificate["inclusion_height"], certificate["height"]), (inclusion, covering["height"]))
        assert_equal(certificate["signing_validator_count"], VALIDATOR_COUNT)
        assert_greater_than_or_equal(certificate["signing_total_weight"], MIN_BRIDGE_TOTAL_WEIGHT)
        assert_equal(proof["function"], VAULT_V2_RELEASE_SIGNATURE)
        assert_equal(hx(proof["calldata"]),
                     encode_vault_v2_release(0, ETH_RECIPIENT, BURN_RAW, self.burn_height, path))
        assert_equal(proof["private_key_required_by_rpc"], False)
        # The deployed vault computes the same leaf.
        assert_equal(self.anvil.call(self.vault, selector(VAULT_V2_WITHDRAWAL_LEAF_SIGNATURE) +
                                     encode_withdrawal_v2_tuple(0, ETH_RECIPIENT, BURN_RAW, self.burn_height)),
                     self.leaf)

        refusals = {}
        uncovered = [entry for entry in self.relayed
                     if BRIDGE_V2_HEIGHT <= entry["height"] < self.burn_height]
        if uncovered:
            refusals["certificate below the burn"] = rpc_refusal(
                RPC_MISC_ERROR, "does not cover the withdrawal",
                n0.getbridgewithdrawalproofv2, self.burn_txid, 0, uncovered[-1]["inclusion"])
        refusals["certificate below V"] = rpc_refusal(
            RPC_MISC_ERROR, "below the bridge V2 activation height",
            n0.getbridgewithdrawalproofv2, self.burn_txid, 0, self.init_cert["inclusion"])
        refusals["mint is not a burn"] = rpc_refusal(
            RPC_INVALID_ADDRESS_OR_KEY, "Burn source is not a confirmed V2 withdrawal",
            n0.getbridgewithdrawalproofv2, self.mint_txid, 0, inclusion)
        self.evidence.assertion("withdrawal proof re-derived; uncovering certificates refused",
                                expected=to_0x(self.root), actual=proof["withdrawal_root"],
                                certificate=certificate, refusals=refusals)
        return proof

    def release(self, proof):
        self.log.info("(11) release pays the recipient exactly once")
        anvil, vault, token = self.anvil, self.vault, self.token

        def balances():
            return {"recipient": call_uint(anvil, token, "balanceOf(address)", enc_address(ETH_RECIPIENT)),
                    "vault": call_uint(anvil, token, "balanceOf(address)", enc_address(vault)),
                    "locked": call_uint(anvil, vault, "locked()"),
                    "released_0": call_uint(anvil, vault, "released(uint64)", enc_uint(0))}

        before = balances()
        assert_equal(before, {"recipient": 0, "vault": DEPOSIT_RAW, "locked": DEPOSIT_RAW, "released_0": 0})
        calldata = hx(proof["calldata"])
        receipt = self.transact(vault, calldata, "B3VaultV2.release")
        withdrawn = keccak256(b"Withdrawn(uint64,address,address,uint256,uint64)")
        logs = [log for log in receipt["logs"] if hx(log["address"]) == hx(vault)]
        assert_equal(len(logs), 1)
        assert_equal([hx(topic) for topic in logs[0]["topics"]],
                     [withdrawn, enc_uint(0), enc_address(token), enc_address(ETH_RECIPIENT)])
        assert_equal(hx(logs[0]["data"]), enc_uint(BURN_RAW) + enc_uint(self.burn_height))
        after = balances()
        assert_equal(after, {"recipient": BURN_RAW, "vault": DEPOSIT_RAW - BURN_RAW,
                             "locked": DEPOSIT_RAW - BURN_RAW, "released_0": 1})

        self.log.info("A second release reverts (AlreadyReleased) and moves nothing")
        assert_equal(revert_data(anvil, vault, calldata, self.relayer), error_selector("AlreadyReleased"))
        tx_hash = anvil.rpc("eth_sendTransaction", {"from": self.relayer, "to": vault,
                                                    "data": to_0x(calldata), "gas": hex(1_000_000)})
        second = anvil.receipt(tx_hash)
        assert_equal(int(second["status"], 16), 0)
        assert_equal(balances(), after)
        self.evidence.assertion("release paid exactly once; second release reverted on chain",
                                expected={"first": after, "second_status": 0},
                                actual={"before": before, "after_first": after, "after_second": balances(),
                                        "second_status": int(second["status"], 16),
                                        "second_tx": tx_hash})
        self.evidence_step("released on Ethereum", anvil, release_tx=receipt["transactionHash"])

    def check_v2_relayer_rpcs(self, deposit):
        self.log.info("Relayer RPCs serve the V2 store from V: proof status, anchor, light-client store")
        k = deposit["block_number"]
        # getbridgeanchorforblock and getbridgelightclientstore answer only
        # from B3-finalized bridge state.
        if not all(self.finalized_height(node) >= self.update_height for node in self.bridge_validators()):
            self.resume()
            self.wait_until(lambda: all(self.finalized_height(node) >= self.update_height
                                        for node in self.bridge_validators()), timeout=300)
        self.freeze()
        observed = {}
        for node in self.bridge_nodes():
            status = node.getbridgeproofstatus(deposit["block_hash"], FIRST_DEPOSIT_ID)
            assert_equal(status["store"], "v2")
            assert_equal((status["anchor"]["known"], status["anchor"]["block_number"]), (True, k))
            assert_equal(status["anchor"]["connected_height"], self.update_height)
            assert_equal((status["deposit"]["claimed"], status["deposit"]["claimed_height"]),
                         (True, self.mint_height))
            assert_equal(node.getbridgeproofstatus(deposit_id=FIRST_DEPOSIT_ID + 1)["deposit"]["claimed"], False)
            anchor = node.getbridgeanchorforblock(k)
            assert_equal((anchor["store"], anchor["found"], anchor["block_number"]), ("v2", True, k))
            assert_equal(hx(anchor["block_hash"]), hx(deposit["block_hash"]))
            assert_equal(anchor["connected_height"], self.update_height)
            # The V1 store was never bootstrapped in RT4a; the V2 one is.
            store = node.getbridgelightclientstore()
            assert_equal(store["connection"]["height"], self.update_height)
            assert_equal(int(store["store"]["finalized_header"]["execution"]["block_number"]), k)
            observed[node.index] = {"status": status, "anchor": anchor, "connection": store["connection"]}
        self.evidence.assertion("relayer RPCs serve the V2 store and Vault V2 deposits from V",
                                expected={"anchor_block": k, "mint_height": self.mint_height,
                                          "update_height": self.update_height},
                                actual=observed)

    # -- 12. pin guard and reindex -------------------------------------------------
    def check_pin_guard_and_reindex(self):
        self.log.info("(12) n3 refuses a modified v2 section, then restarts with the original")
        n0, n1, n3 = self.nodes[0], self.nodes[1], self.nodes[3]
        self.freeze()
        with open(self.pins_path, encoding="utf8") as f:
            altered = json.load(f)
        altered["v2"]["max_per_epoch"] = 2 * BRIDGE_V2_DEFAULT_CAP_RAW
        altered_path = os.path.join(self.options.tmpdir, "b3bridgepins-v2-altered.json")
        with open(altered_path, "w", encoding="utf8") as f:
            json.dump(altered, f, indent=2, sort_keys=True)
            f.write("\n")
        assert file_sha256(altered_path) != file_sha256(self.pins_path)
        height, tip = n3.getblockcount(), n3.getbestblockhash()
        self.stop_node(3)
        altered_args = [arg for arg in n3.extra_args if not arg.startswith("-b3bridgepins=")]
        refusal = start_refused(n3, altered_args + [f"-b3bridgepins={altered_path}"], NODE_PINS_CHANGED_ERROR)
        self.start_node(3)
        assert_equal((n3.getblockcount(), n3.getbestblockhash()), (height, tip))
        self.connect_nodes(3, 2)
        self.sync_bridge_blocks()
        self.evidence.assertion("modified v2 section refused after activation; original accepted",
                                expected=NODE_PINS_CHANGED_ERROR, actual=refusal)

        self.log.info("n0 reindexes and reproduces n1's bridge V2 state and fresh lineage")
        self.restart_node(0, extra_args=list(n0.extra_args) + ["-reindex"])
        self.wait_until(lambda: n0.getbestblockhash() == n1.getbestblockhash(), timeout=300)
        self.connect_nodes(1, 0)
        n0.syncwithvalidationinterfacequeue()
        info0, info1 = n0.getbridgev2info(), n1.getbridgev2info()
        assert_equal(info0, info1)
        assert_equal((info0["nullifiers"], info0["withdrawals"]), (1, 1))
        assert_equal(hx(info0["withdrawal_root_evm"]), self.root)
        lineage0, lineage1 = n0.getfinalitylineage(), n1.getfinalitylineage()
        for key in LINEAGE_VIEW_KEYS:
            assert_equal(lineage0.get(key), lineage1.get(key))
        self.evidence_step("n0 reindexed and agrees with n1", self.anvil,
                           tip=n0.getbestblockhash(), fresh_finalized=lineage0.get("finalized"))


if __name__ == '__main__':
    B3BridgeRT4aRoundTripTest(__file__).main()
