#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""RT6: bridge V2 routes (b) and (d3) through the forwarders (contracts spec §7-§9).

REGTEST ONLY, the environment of RT4c (feature_b3_bridge_rt4c_conflict.py)
plus a second chain: four B3 regtest validators linked only to each other
and bound to the public rt4c-committee test keys, the Ethereum anvil
(chain 31337) and an Arbitrum stand-in anvil (chain 31338), both on
127.0.0.1 and never forked, the synthetic sync-committee tool, forge
--offline with its output outside the source tree and fresh datadirs.
LayerZero is the regtest mock endpoint pair (Ethereum eid 30101, Arbitrum
eid 30110) with MockOFTAdapter over MockTetherToken on 31337 (seeded with
locked USDT) and MockUSDT0 on 31338; contrib/b3bridge/regtest/
mock_lz_relayer.py moves the packets. Forwarders are flushed and forwarded
by contrib/b3bridge/b3_bridge_forwarders_v2.py, run as a subprocess.

The pin file, written before the validators first start (OI-3), carries the
v2 deposit_forwarder and exit_forwarder pins: DeployV2 with
B3V2_DEPLOY_FORWARDERS=1 CREATEs B3DepositForwarderFactory and
B3OftExitFactory at deployer nonces 4 and 5, and both CREATE2 B3Forwarder,
whose init-code hash comes from this build's forge artifact.

1. RT4c bring-up; getbridgev2info.forwarders equals the pins (never
   unpinned); bridgeforwarderaddress and bridgewithdraw options.arbitrum
   are refused below V.
2. Fresh checkpoint 266; DeployV2 with the forwarder factories (nonces 4 and
   5, the vault's FORWARDER_FACTORY, init-code hash = pin, F_x DST_EID
   30110); initialize; past V, every certificate relayed, deposits open.
3. Route (b): bridgeforwarderaddress (refund EOA = anvil account 1, proven by
   its personal_sign signature) derives two forwarders, F_d1 != F_d2, each
   the Python derivation and factory.accountOf(p), each with fee cap
   4 x fee_per_tx (OI-2); a bad signature, the vault as refund EOA (OI-6),
   a not_after out of range and an amount above MAX_DEPOSIT_RAW (OI-2) are
   refused. On
   31338 MockUSDT0.send moves DEPOSIT_RAW + FLUSH_FEE to F_d1 before it
   exists; the LayerZero relayer delivers once (a second pass delivers
   nothing, a direct re-delivery reverts AlreadyDelivered). A flush called
   from a contract origin reverts BadCaller (PR-2). The contrib flush (deposit
   guard OPEN) deploys F_d1 and flushes it: exactly the token Transfer and
   the vault Deposit (PR-1), receipt leaf within 7,168 bytes (PR-3), Deposit
   id 0 of DEPOSIT_RAW for F_d1's B3 address. collectFee pays the relayer
   FLUSH_FEE; a second flush finds nothing to flush and sends nothing.
   b3-bridge-ethcheck proves the flush and claimbridgedeposit mints bUSD V2
   (RT4a's steps); the replay is refused. With the V2 light client
   bootstrapped, getbridgev2info reports its horizon (the pin file's
   valid-through epoch is 4,096 epochs past the checkpoint, about 18 days)
   and bridgeforwarderaddress ends not_after 24 hours before it.
4. Route (d3): with the full V2 burn gate READY, bridgewithdraw with
   options.arbitrum (recipient = account 1 on 31338, the refund EOA,
   fee_cap 2,000,000) builds, unbroadcast, a burn whose exit block names p
   and whose recipient is F_x = exitFactory.accountOf(p), re-derived here;
   the wallet refuses the vault as ethereum_recipient, the vault or the exit
   factory as the Arbitrum recipient and the vault as refund EOA (OI-6), a
   fee_cap not below the amount, both recipients, an unknown or missing
   arbitrum key and max_bridge_fee_bps 51; a positional "" recipient with
   options.arbitrum builds the same burn.
   The user publishes p on the exit factory (Published) before the burn is
   broadcast and mined; its leaf V2 pays F_x. A certificate covering it is
   relayed; getbridgewithdrawalproofv2 and getbridgewithdrawals are
   identical on every node and return the exit block p stored with the leaf
   and recipient F_x. The contrib forward sends one
   B3RelayBatch.relayWithForwards: the leaf is released to F_x and
   forwarded through the OFT adapter, the relayer takes FORWARD_FEE, the
   ETH margin returns and the batch, F_x and the factory hold no wei. The
   LayerZero relayer delivers BURN_RAW - FORWARD_FEE USDT0 to account 1 on
   31338. A second forward is refused before any transaction (released, F_x
   empty); sent directly, the batch reports ReleaseSkipped and
   ForwardFailed(NothingToForward) and moves nothing; no packet is
   delivered twice.
5. Closure: RT4c's kind 13 conflict closes bridge V2 deposits for good on
   every node; bridgeforwarderaddress is refused (closed for good). USDT0
   reaches F_d2 through LayerZero, and the contrib flush exits 1 with the
   deposit guard's message, sending nothing. Relayed flagged certificates
   close Verifier V2 deposits: F_d2 deploys, but its flush reverts
   DepositClosed (eth_call); refundEOA refunds the delivery, while the
   vault's balance and locked do not move.
6. Accounting: Vault V2 locked = DEPOSIT_RAW - BURN_RAW = its balance = B3's
   reserve = the wallets' bUSD V2; USDT is conserved across every holder;
   the adapter's locked USDT equals the USDT0 supply on 31338 (minted -
   sent + delivered). n3 restarts (replay) and n0 reindexes, replaying the
   exit-block burn; every node agrees, getbridgewithdrawals included.
   Evidence: bridge_evidence.json.
"""

import os

from feature_b3_bridge_rt4a_round_trip import (
    BRIDGE_V2_BURN_HEIGHT,
    BRIDGE_V2_HEIGHT,
    BRING_UP_HEIGHT,
    BURN_FINALITY_LAG_MARGIN,
    BURN_RAW,
    DEPOSIT_RAW,
    ETH_RECIPIENT,
    FIRST_DEPOSIT_ID,
    FRESH_GENESIS,
    INIT_CHECKPOINT,
    MAX_DEPOSIT_RAW,
    MIN_BRIDGE_TOTAL_WEIGHT,
    RPC_INVALID_ADDRESS_OR_KEY,
    RPC_INVALID_PARAMETER,
    RPC_MISC_ERROR,
    RPC_VERIFY_REJECTED,
    TOKEN_SUPPLY,
    error_selector,
    gas_used,
    mid_period_slot_offset,
    revert_data,
)
from feature_b3_bridge_rt4c_conflict import (
    COMMITTEE_LABEL,
    B3BridgeRT4cConflictTest,
)
from test_framework.anvil import deploy
from test_framework.b3bridge import (
    DEPLOY_V2_CONTRACTS,
    DEPLOY_V2_FORWARDER_CONTRACTS,
    EXIT_FACTORY_NONCE,
    FORWARDER_FACTORY_NONCE,
    LZ_EID_ARBITRUM,
    LZ_EID_ETHEREUM,
    OFT_QUOTE_SEND_SIGNATURE,
    OFT_SEND_SIGNATURE,
    REGTEST_ETH_CHAIN_ID,
    VAULT_V2_RELEASE_SIGNATURE,
    VAULT_V2_WITHDRAWAL_LEAF_SIGNATURE,
    VERIFIER_V2_INITIALIZE_SIGNATURE,
    VERIFIER_V2_STATUS_UNINITIALIZED,
    VERIFIER_V2_SUBMIT_SIGNATURE,
    VALIDATOR_COUNT,
    DepositGuardError,
    DepositParams,
    ExitParams,
    ForwarderToolError,
    abi_call,
    address_word,
    asset_id_v2,
    call_address,
    call_bytes32,
    call_uint,
    decode_oft_message,
    deposit_fee_cap,
    deposit_forwarder_address,
    enc_address,
    enc_uint,
    encode_oft_quote_send,
    encode_oft_send,
    encode_vault_v2_release,
    encode_withdrawal_v2_tuple,
    exit_forwarder_address,
    file_sha256,
    find_deposits,
    fold_withdrawal_path,
    forwarder_eth_rpc,
    forwarder_pins_v2,
    forwarders_v2,
    hx,
    keccak256,
    leaf_v2,
    leaf_v2_preimage,
    oft_send_param,
    personal_sign,
    predict_create_address,
    recipient_v1,
    refund_eoa_message,
    regtest_chain_domain,
    rpc_refusal,
    run_deposit_guard,
    run_forwarder_tool,
    run_mock_lz_relayer,
    selector,
    to_0x,
    withdrawal_root,
    withdrawal_root_and_path,
    write_regtest_bridge_pins_v2,
)
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
)

# -- deployment ---------------------------------------------------------------
# DeployV2's CREATEs from the deployer with B3V2_DEPLOY_FORWARDERS=1, nonces 0..5.
DEPLOY_ROLES = ("prover", "verifier", "vault", "batch", "deposit_factory", "exit_factory")
assert DEPLOY_ROLES.index("deposit_factory") == FORWARDER_FACTORY_NONCE
assert DEPLOY_ROLES.index("exit_factory") == EXIT_FACTORY_NONCE
DEPLOY_NAMES = DEPLOY_V2_CONTRACTS + DEPLOY_V2_FORWARDER_CONTRACTS
UNPINNED = ["vault_runtime_code_hash", "verifier_runtime_code_hash", "lineage_id"]
FORWARDER_MIN_FLUSH_RAW = forwarders_v2.MIN_FLUSH_RAW

# -- amounts (raw six-decimal units) -------------------------------------------
# The adapter's locked USDT backs every USDT0 on 31338: seed it with the
# USDT0 minted there, so locked == USDT0 supply holds throughout.
ARB_USDT0_MINT = 100_000_000
ADAPTER_SEED = ARB_USDT0_MINT
FLUSH_FEE = 500_000  # per F_d transaction (bridgeforwarderaddress fee_per_tx)
DELIVERY_1 = DEPOSIT_RAW + FLUSH_FEE  # F_d1: one flush of DEPOSIT_RAW after its fee
DELIVERY_2 = 3_000_000  # F_d2: delivered after closure, refunded
EXIT_FEE_CAP = 2_000_000  # F_x p.feeCap
FORWARD_FEE = 1_000_000  # the relayer's F_x fee, within EXIT_FEE_CAP
NATIVE_MARGIN = 10**15  # wei above the OFT quote; the factory returns it
assert BURN_RAW > EXIT_FEE_CAP >= FORWARD_FEE
assert DEPOSIT_RAW >= FORWARDER_MIN_FLUSH_RAW and DEPOSIT_RAW <= MAX_DEPOSIT_RAW
assert DELIVERY_2 >= FORWARDER_MIN_FLUSH_RAW + FLUSH_FEE
RECEIPT_LEAF_LIMIT = forwarders_v2.PR3_LEAF_LIMIT  # 7,168 bytes (PR-3)
DAY = 24 * 60 * 60
NOT_AFTER_MAX_LIFETIME = 30 * DAY  # bridgeforwarderaddress: now + 30 days
# The pin file's light-client horizon: 4,096 epochs of 12 s slots past the
# checkpoint, about 18.2 days. That keeps the wallets' 7-day refusal clear and
# brings their 24 h not_after cap below 30 days (node changes section 12.3).
HORIZON_EPOCHS = 4096

# -- ABI ------------------------------------------------------------------------
DEPOSIT_PARAMS_TYPE = forwarders_v2.DEPOSIT_PARAMS_TYPE
EXIT_PARAMS_TYPE = forwarders_v2.EXIT_PARAMS_TYPE
SIG_D_PUBLISH = f"publish({DEPOSIT_PARAMS_TYPE})"
SIG_D_COLLECT_FEE = "collectFee(address,address)"
SIG_D_REFUND = f"refund({DEPOSIT_PARAMS_TYPE},address,address,uint256)"
SIG_D_OWED = "owed(address,address)"
SIG_D_RESERVED = "reserved(address)"
SIG_X_PUBLISH = f"publish({EXIT_PARAMS_TYPE})"
SIG_LZ_DELIVER = "deliver(uint32,bytes32,uint64,address,bytes)"
EV_PACKET_SENT = "PacketSent(uint64,uint32,address,bytes32,bytes)"
EV_FORWARD_FAILED = "ForwardFailed(uint256,address,bytes)"
EV_RELEASE_SKIPPED = "ReleaseSkipped(uint256,uint64)"
EV_WITHDRAWN = "Withdrawn(uint64,address,address,uint256,uint64)"
EV_TRANSFER = "Transfer(address,address,uint256)"
EV_DEPOSIT = "Deposit(uint64,address,uint256,bytes32)"

DEPOSITS_CLOSED_TOOL_SUFFIX = "this relayer refuses deposits and flushes"
FORWARDER_CLOSED_SUFFIX = "a forwarder deposit made now mints bUSD V2 that can never be redeemed"
OI6_SUFFIX = "funds sent there are lost, the bridge V2 contracts give no claim back (OI-6)"


def word(address):
    """The 32-byte address word of a 0x address string or 20 bytes."""
    return address_word(address)


class B3BridgeRT6RouteBD3Test(B3BridgeRT4cConflictTest):
    BOOTSTRAP_MODE = 1  # START_CERTIFICATE, as RT4a and RT4c

    def set_test_params(self):
        # Required: the framework's metaclass refuses a subclass that does not
        # define set_test_params itself. RT4a's parameters are RT6's.
        super().set_test_params()

    def skip_test_if_missing_module(self):
        # RT4c's: the wallet, anvil and forge, ethcheck, the regtest beacon and
        # the regtest certify tool. The Arbitrum stand-in is a second process
        # of the same anvil binary (skip_if_no_foundry found it).
        super().skip_test_if_missing_module()

    # -- Ethereum and Arbitrum anvils, mocks and pins (before any node starts) --
    def setup_network(self):
        self.log.info("anvil 31337: MockTetherToken, the Ethereum LayerZero endpoint and the USDT0 OFT adapter")
        anvil = self.anvil = self.start_anvil()
        self.owner, self.depositor, self.deployer, self.relayer = anvil.accounts()[:4]
        # Account 1 is the user on both chains: the depositor on Ethereum,
        # the USDT0 holder on Arbitrum and the refund EOA of every forwarder.
        self.user = self.refund_eoa = self.depositor
        self.bridge_forge_build()
        self.token, token_block, self.token_code_hash = deploy(
            anvil, self.owner, self.bridge_artifact("MockTetherToken.sol", "MockTetherToken"))
        anvil.send(self.owner, self.token, abi_call("issue(uint256)", enc_uint(TOKEN_SUPPLY)))
        anvil.send(self.owner, self.token, abi_call(
            "transfer(address,uint256)", enc_address(self.depositor), enc_uint(TOKEN_SUPPLY)))
        self.ep_eth, _, _ = deploy(anvil, self.owner, self.bridge_artifact("MockLzEndpoint.sol", "MockLzEndpoint"),
                                   enc_uint(LZ_EID_ETHEREUM))
        self.adapter, _, _ = deploy(anvil, self.owner, self.bridge_artifact("MockOFTAdapter.sol", "MockOFTAdapter"),
                                    enc_address(self.token) + enc_address(self.ep_eth))
        # The real adapter holds the USDT behind every USDT0: the depositor locks it.
        anvil.send(self.depositor, self.token, abi_call(
            "transfer(address,uint256)", enc_address(self.adapter), enc_uint(ADAPTER_SEED)))
        assert_equal(call_address(anvil, self.adapter, "token()"), self.token)

        self.log.info("anvil 31338 (Arbitrum stand-in): its LayerZero endpoint and MockUSDT0; peers set")
        arb = self.arb = self.start_arbitrum_anvil()
        assert_equal(arb.accounts()[:4], [self.owner, self.depositor, self.deployer, self.relayer])
        self.ep_arb, _, _ = deploy(arb, self.owner, self.bridge_artifact("MockLzEndpoint.sol", "MockLzEndpoint"),
                                   enc_uint(LZ_EID_ARBITRUM))
        self.usdt0, _, _ = deploy(arb, self.owner, self.bridge_artifact("MockUSDT0.sol", "MockUSDT0"),
                                  enc_address(self.ep_arb))
        anvil.send(self.owner, self.adapter, abi_call("setPeer(uint32,bytes32)", enc_uint(LZ_EID_ARBITRUM),
                                                      word(self.usdt0)))
        arb.send(self.owner, self.usdt0, abi_call("setPeer(uint32,bytes32)", enc_uint(LZ_EID_ETHEREUM),
                                                  word(self.adapter)))
        arb.send(self.owner, self.usdt0, abi_call("mint(address,uint256)", enc_address(self.user),
                                                  enc_uint(ARB_USDT0_MINT)))
        assert_equal(call_uint(arb, self.usdt0, "totalSupply()"), ARB_USDT0_MINT)
        assert_equal(call_uint(arb, self.ep_arb, "eid()"), LZ_EID_ARBITRUM)
        assert_equal(call_uint(anvil, self.ep_eth, "eid()"), LZ_EID_ETHEREUM)
        self.check_mock_selectors()

        self.log.info("Predict DeployV2's six CREATEs; checkpoint at the anvil head; v2 pins with forwarders")
        assert_equal(int(anvil.rpc("eth_getTransactionCount", self.deployer, "latest"), 16), 0)
        self.predicted = {role: predict_create_address(self.deployer, nonce)
                          for nonce, role in enumerate(DEPLOY_ROLES)}
        self.verifier, self.vault = self.predicted["verifier"], self.predicted["vault"]
        self.batch = self.predicted["batch"]
        self.deposit_factory = self.predicted["deposit_factory"]
        self.exit_factory = self.predicted["exit_factory"]
        for address in self.predicted.values():
            assert_equal(anvil.code(address), b"")
        self.init_code_hash = self.bridge_forwarder_init_code_hash()
        self.forwarder_pins = forwarder_pins_v2(self.deployer, self.init_code_hash)
        assert_equal((self.forwarder_pins["deposit"]["factory"], self.forwarder_pins["exit"]["factory"]),
                     (self.deposit_factory, self.exit_factory))
        checkpoint_block = anvil.block_number()
        self.origin_deployment_block = checkpoint_block
        self.slot_offset = mid_period_slot_offset(checkpoint_block)
        self.beacon = self.start_synthetic_beacon(anvil, self.slot_offset, checkpoint_block)
        self.bootstrap = self.beacon.bootstrap()
        assert_equal(self.bootstrap["checkpoint_block"], checkpoint_block)
        self.domain = regtest_chain_domain()
        self.asset_id_raw = asset_id_v2(self.domain, REGTEST_ETH_CHAIN_ID, self.vault, self.token)
        self.pins_path = os.path.join(self.options.tmpdir, "b3bridgepins-v2.json")
        self.pins = write_regtest_bridge_pins_v2(
            self.pins_path, anvil, vault=self.vault, token=self.token, verifier=self.verifier,
            deployer=self.deployer, deployment_block=self.origin_deployment_block,
            bootstrap_output=self.bootstrap, activation_height=BRIDGE_V2_HEIGHT,
            burn_height=BRIDGE_V2_BURN_HEIGHT, max_deposit_raw=MAX_DEPOSIT_RAW,
            min_bridge_total_weight=MIN_BRIDGE_TOTAL_WEIGHT, successor=self.bridge_pins_successor(),
            forwarders=self.forwarder_pins, horizon_epochs=HORIZON_EPOCHS)
        assert_equal(sorted(self.pins), ["format", "v2"])
        assert_equal(hx(self.pins["v2"]["token_runtime_code_hash"]), self.token_code_hash)
        assert_equal(self.pins["v2"]["deposit_forwarder"], self.forwarder_pins["deposit"])
        assert_equal(self.pins["v2"]["exit_forwarder"], self.forwarder_pins["exit"])
        self.eth = forwarder_eth_rpc(anvil)
        self.evidence.step("ethereum and arbitrum mocks deployed; v2 pins with forwarders written",
                           eth_block=anvil.block_number(), arbitrum_block=arb.block_number(),
                           token=self.token, token_block=token_block, predicted=self.predicted,
                           lz_endpoints={"ethereum": self.ep_eth, "arbitrum": self.ep_arb},
                           oft_adapter=self.adapter, usdt0=self.usdt0, adapter_seed=ADAPTER_SEED,
                           usdt0_minted=ARB_USDT0_MINT, init_code_hash=to_0x(self.init_code_hash),
                           checkpoint_block=checkpoint_block, pins_sha256=file_sha256(self.pins_path),
                           forwarder_pins=self.forwarder_pins, asset_id_v2=to_0x(self.asset_id_raw))
        self.start_bridge_validators(self.pins_path, extra_args=self.fresh_args())

    def check_mock_selectors(self):
        """The calldata this test and the contrib tools build targets the
        deployed mocks' functions (forge methodIdentifiers)."""
        for file, contract, signatures in (
                ("MockLzEndpoint.sol", "MockLzEndpoint", (SIG_LZ_DELIVER, "isDelivered(uint32,bytes32,uint64)")),
                ("MockOFTAdapter.sol", "MockOFTAdapter", (OFT_SEND_SIGNATURE, OFT_QUOTE_SEND_SIGNATURE)),
                ("MockUSDT0.sol", "MockUSDT0", (OFT_SEND_SIGNATURE, OFT_QUOTE_SEND_SIGNATURE)),
        ):
            ids = self.bridge_method_ids(file, contract)
            for signature in signatures:
                assert_equal(ids[signature], selector(signature).hex())

    # -- 1. bring-up ------------------------------------------------------------
    def run_test(self):
        self.withdrawal_leaves = []  # (b3 height, leaf V2) in id order
        self.relayed = []
        self.conflict_height = None

        self.log.info(f"(1) Corridor and stakes bound to the {COMMITTEE_LABEL} test keys; live V1 finality")
        self.bring_up_validators(BRING_UP_HEIGHT, finality_key_label=COMMITTEE_LABEL)
        self.check_committee()
        self.check_lineage()
        self.v2info = self.check_v2_info_before_activation()

        self.log.info("(2) Fresh checkpoint 266, DeployV2 with forwarder factories, initialize, past V")
        self.resume()
        self.shuttle_until(lambda: all(self.fresh_finalized_height(node) >= INIT_CHECKPOINT
                                       for node in self.bridge_validators()))
        self.freeze()
        self.evidence_step("fresh checkpoint 266 finalized by gossip", self.anvil,
                           finsigv2_bytes_received=self.assert_fresh_gossip(self.bridge_validators()))
        self.deploy_v2_stack()
        self.initialize_verifier()
        self.resume()
        self.shuttle_until(lambda: all(self.fresh_finalized_height(node) >= BRIDGE_V2_HEIGHT
                                       for node in self.bridge_validators()))
        self.freeze()
        self.check_carried_roots()
        self.relay_certificates("past V")
        self.check_deposits_open()

        deposit = self.make_deposit()  # route (b)
        plan = self.build_plan(deposit)
        self.relay_carriers(plan, deposit)
        self.claim_mint(plan, deposit)
        self.check_forwarder_horizon()

        self.burn()  # route (d3)
        covering = self.relay_withdrawal_root()
        proof = self.check_withdrawal_proof(covering)
        self.release_and_forward(proof, covering)

        conflict = self.close_deposits_for_good()
        self.flush_refused_after_closure(conflict)
        self.relay_flagged_certificates(conflict)
        self.refund_after_closure()

        self.check_final_accounting()
        gossip = self.assert_fresh_gossip(self.bridge_validators())
        self.check_replay_and_reindex(conflict)
        self.evidence_step("RT6 complete", self.anvil, fresh_votes=dict(self.fresh_vote_results),
                           fresh_vote_transport=self.fresh_vote_transport, finsigv2_bytes_received=gossip,
                           relayed=len(self.relayed), conflict_height=conflict["height"],
                           arbitrum_block=self.arb.block_number())

    def expected_forwarders(self):
        """getbridgev2info.forwarders for the pins."""
        deposit, exit_pin = self.forwarder_pins["deposit"], self.forwarder_pins["exit"]
        return {"deposit": {"factory": deposit["factory"], "init_code_hash": to_0x(self.init_code_hash)},
                "exit": {"factory": exit_pin["factory"], "init_code_hash": to_0x(self.init_code_hash),
                         "dst_eid": LZ_EID_ARBITRUM}}

    def check_v2_info_before_activation(self):
        info = super().check_v2_info_before_activation()
        self.log.info("getbridgev2info.forwarders equals the pins; bridgeforwarderaddress waits for V")
        forwarders = info["forwarders"]
        assert_equal({role: {key: (hx(value) if isinstance(value, str) else value) for key, value in pin.items()}
                      for role, pin in forwarders.items()},
                     {role: {key: (hx(value) if isinstance(value, str) else value) for key, value in pin.items()}
                      for role, pin in self.expected_forwarders().items()})
        # Knowable before the first start: never an unpinned entry (OI-3).
        assert_equal(info["unpinned"], UNPINNED)
        n0 = self.nodes[0]
        signature = personal_sign(self.anvil, self.refund_eoa, refund_eoa_message(self.domain, self.refund_eoa))
        refusal = rpc_refusal(RPC_MISC_ERROR, "Bridge V2 rules are not active for the next block",
                              n0.bridgeforwarderaddress, self.refund_eoa, to_0x(signature))
        route = {"recipient": self.user, "refund_eoa": self.refund_eoa, "refund_eoa_signature": to_0x(signature)}
        exit_refusal = rpc_refusal(RPC_MISC_ERROR,
                                   "options.arbitrum (route d3) needs bridge V2 burn rules for the next block",
                                   n0.bridgewithdraw, BURN_RAW, None, {"arbitrum": route})
        self.evidence.assertion("getbridgev2info.forwarders equals the pins; no forwarder below V",
                                expected=self.expected_forwarders(), actual=forwarders, below_v=refusal,
                                route_d3_below_v=exit_refusal)
        return info

    # -- 2. DeployV2 with the forwarder factories -------------------------------
    def deploy_v2_stack(self):
        """RT4a's deployment with B3V2_DEPLOY_FORWARDERS=1. RT4a's own
        deploy_v2_stack asserts the deployer's nonce is 4 afterwards, so this
        repeats its checks for the six CREATEs instead of calling it."""
        self.log.info("(2) DeployV2.s.sol with B3V2_DEPLOY_FORWARDERS=1 at the six predicted addresses")
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
            "B3V2_BRIDGE_ACTIVATION_HEIGHT": BRIDGE_V2_HEIGHT,
            "B3V2_ORIGIN_TOKEN": self.token,
            "B3V2_MIN_BRIDGE_TOTAL_WEIGHT": MIN_BRIDGE_TOTAL_WEIGHT,
            "B3V2_MIN_EPOCH_DURATION": 1,
            "B3V2_REVIEW_WINDOW": 1,
            "B3V2_MAX_DEPOSIT_RAW": MAX_DEPOSIT_RAW,
            # Route (b) and (d3) factories at deployer nonces 4 and 5; the
            # exit factory's DST_EID stays DeployV2's default (Arbitrum One).
            "B3V2_DEPLOY_FORWARDERS": 1,
            "B3V2_OFT_ADAPTER": self.adapter,
        }
        env.update(self.bootstrap_deploy_env())
        assert_equal(env["B3V2_BOOTSTRAP_MODE"], self.BOOTSTRAP_MODE)
        created = self.bridge_deploy_v2(anvil, self.deployer, env)
        assert_equal(sorted(created), sorted(DEPLOY_NAMES))
        for name, role in zip(DEPLOY_NAMES, DEPLOY_ROLES):
            assert_equal(created[name]["address"], self.predicted[role])
            assert anvil.code(self.predicted[role]), name
            self.evidence.gas(f"DeployV2 {name}", created[name]["gas_used"], block=created[name]["block"])
        assert_equal(int(anvil.rpc("eth_getTransactionCount", self.deployer, "latest"), 16), len(DEPLOY_ROLES))

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
        assert_equal(call_uint(anvil, verifier, "BOOTSTRAP_MODE()"), self.BOOTSTRAP_MODE)
        assert_equal(call_uint(anvil, verifier, "status()"), VERIFIER_V2_STATUS_UNINITIALIZED)

        self.log.info("The vault names the F_d factory; both factories CREATE2 the pinned B3Forwarder")
        fd, fx = self.deposit_factory, self.exit_factory
        assert_equal(call_address(anvil, vault, "FORWARDER_FACTORY()"), fd)
        assert_equal((call_address(anvil, fd, "VAULT()"), call_address(anvil, fd, "TOKEN()")), (vault, self.token))
        assert_equal(call_uint(anvil, fd, "MAX_DEPOSIT_RAW()"), MAX_DEPOSIT_RAW)
        assert_equal(call_uint(anvil, fd, "MIN_FLUSH_RAW()"), FORWARDER_MIN_FLUSH_RAW)
        assert_equal((call_address(anvil, fx, "TOKEN()"), call_address(anvil, fx, "ADAPTER()")),
                     (self.token, self.adapter))
        assert_equal(call_uint(anvil, fx, "DST_EID()"), LZ_EID_ARBITRUM)
        for factory in (fd, fx):
            assert_equal(call_bytes32(anvil, factory, "FORWARDER_INIT_CODE_HASH()"), self.init_code_hash)
        # The ABI encoders below and in the contrib tools target the deployed functions.
        for file, contract, signatures in (
                ("B3FinalityVerifierV2.sol", "B3FinalityVerifierV2",
                 (VERIFIER_V2_INITIALIZE_SIGNATURE, VERIFIER_V2_SUBMIT_SIGNATURE)),
                ("B3VaultV2.sol", "B3VaultV2", (VAULT_V2_RELEASE_SIGNATURE, VAULT_V2_WITHDRAWAL_LEAF_SIGNATURE)),
                ("B3DepositForwarderFactory.sol", "B3DepositForwarderFactory",
                 (forwarders_v2.SIG_D_ACCOUNT_OF, forwarders_v2.SIG_D_DEPLOY, forwarders_v2.SIG_D_FLUSH,
                  SIG_D_PUBLISH, SIG_D_COLLECT_FEE, SIG_D_REFUND)),
                ("B3OftExitFactory.sol", "B3OftExitFactory",
                 (forwarders_v2.SIG_X_ACCOUNT_OF, forwarders_v2.SIG_X_FORWARD, forwarders_v2.SIG_X_QUOTE_FORWARD,
                  SIG_X_PUBLISH)),
                ("B3RelayBatch.sol", "B3RelayBatch", (forwarders_v2.SIG_RELAY_WITH_FORWARDS,)),
        ):
            ids = self.bridge_method_ids(file, contract)
            for signature in signatures:
                assert_equal(ids[signature], selector(signature).hex())
        self.evidence_step("DeployV2 deployed six contracts at the predicted addresses", anvil,
                           addresses={name: created[name]["address"] for name in created},
                           asset_id=to_0x(node_asset), init_code_hash=to_0x(self.init_code_hash),
                           vault_code_hash=to_0x(anvil.code_hash(vault)),
                           verifier_code_hash=to_0x(anvil.code_hash(verifier)),
                           deposit_factory_code_hash=to_0x(anvil.code_hash(fd)),
                           exit_factory_code_hash=to_0x(anvil.code_hash(fx)))

    # -- balances ------------------------------------------------------------------
    def usdt(self, address):
        return call_uint(self.anvil, self.token, "balanceOf(address)", enc_address(address))

    def usdt0_balance(self, address):
        return call_uint(self.arb, self.usdt0, "balanceOf(address)", enc_address(address))

    @staticmethod
    def wei(anvil, address):
        return int(anvil.rpc("eth_getBalance", address, "latest"), 16)

    @staticmethod
    def nonce(anvil, address):
        return int(anvil.rpc("eth_getTransactionCount", address, "latest"), 16)

    def send_usdt0(self, account, amount):
        """MockUSDT0.send on 31338 from the user to `account` on Ethereum,
        with the quoted native fee; returns (receipt, LayerZero nonce)."""
        arb = self.arb
        sp = oft_send_param(LZ_EID_ETHEREUM, account, amount)
        quote = int.from_bytes(arb.call(self.usdt0, encode_oft_quote_send(sp))[:32], "big")
        assert_equal(quote, call_uint(arb, self.ep_arb, "quote()"))
        before = self.usdt0_balance(self.user)
        receipt = arb.send(self.user, self.usdt0, encode_oft_send(sp, quote, self.user), value=quote)
        assert_equal(self.usdt0_balance(self.user), before - amount)
        sent = [log for log in receipt["logs"] if hx(log["address"]) == hx(self.ep_arb)]
        assert_equal(len(sent), 1)
        topics = [hx(topic) for topic in sent[0]["topics"]]
        assert_equal(topics[0], keccak256(EV_PACKET_SENT.encode()))
        assert_equal(topics[2:], [enc_uint(LZ_EID_ETHEREUM), enc_address(self.usdt0)])
        return receipt, int.from_bytes(topics[1], "big")

    def deliver_lz(self, src, src_endpoint, dst, dst_endpoint, label, delivered):
        """One mock_lz_relayer.py pass; exactly `delivered` new packets, none failed."""
        result = run_mock_lz_relayer(src, src_endpoint, dst, dst_endpoint, self.relayer)
        assert_equal((result["delivered"], result["failed"]), (delivered, 0))
        self.evidence.assertion(f"LayerZero relayer pass ({label})", expected={"delivered": delivered},
                                actual={key: result[key] for key in ("delivered", "already_delivered", "failed")},
                                packets=result["packets"])
        return result

    # -- 3. route (b) ------------------------------------------------------------------
    def derive_forwarder(self, signature, amount):
        """bridgeforwarderaddress for a delivery of `amount` at FLUSH_FEE per
        transaction, checked against the Python derivation and the factory."""
        n0 = self.nodes[0]
        result = n0.bridgeforwarderaddress(self.refund_eoa, to_0x(signature),
                                           {"amount": amount, "fee_per_tx": FLUSH_FEE})
        p = DepositParams.from_json(result["params"])
        account = to_0x(deposit_forwarder_address(self.deposit_factory, self.init_code_hash, p))
        assert_equal(hx(result["forwarder"]), hx(account))
        assert_equal(forwarders_v2.view(self.eth, self.deposit_factory, forwarders_v2.SIG_D_ACCOUNT_OF, p.abi(),
                                        returns="address"), account)
        assert_equal((hx(result["factory"]), hx(result["init_code_hash"])),
                     (hx(self.deposit_factory), self.init_code_hash))
        assert_equal((hx(result["vault"]), hx(result["token"])), (hx(self.vault), hx(self.token)))
        assert_equal((hx(p.vault), hx(p.refund_eoa)), (hx(self.vault), hx(self.refund_eoa)))
        assert_equal(p.b3_recipient, recipient_v1(result["address"]))
        assert_equal(hx(result["recipient_v1"]), p.b3_recipient)
        assert_equal(n0.getaddressinfo(result["address"])["labels"], ["b3-busd-bridge-forwarder"])
        # OI-2: one delivery of at most MAX_DEPOSIT_RAW; the lifetime cap pays
        # deploy, two flushes and returnToOwner.
        assert_equal(p.fee_cap, deposit_fee_cap(FLUSH_FEE))
        assert_equal(p.fee_cap, 4 * FLUSH_FEE)
        assert_equal((result["amount_raw"], result["fee_per_tx"], result["max_deposit_raw"], result["min_flush_raw"]),
                     (amount, FLUSH_FEE, MAX_DEPOSIT_RAW, FORWARDER_MIN_FLUSH_RAW))
        now = self.anvil_time()
        assert now < p.not_after <= now + NOT_AFTER_MAX_LIFETIME + 600, (now, p.not_after)
        # Before the V2 light client's bootstrap no horizon caps not_after.
        assert "horizon_time" not in result, result
        assert any("not bootstrapped" in warning for warning in result["warnings"]), result["warnings"]
        assert_equal(hx(result["salt"]), forwarders_v2.deposit_salt(p))
        assert_equal(result["oft_send"], {"dst_eid": LZ_EID_ETHEREUM, "to": to_0x(word(account)),
                                          "compose_msg": "", "oft_cmd": ""})
        assert_equal(result["refund_eoa_message"], refund_eoa_message(self.domain, self.refund_eoa))
        assert account not in (self.vault, self.token, self.deposit_factory, self.exit_factory)
        return result, p, account

    def anvil_time(self):
        return int(self.anvil.block()["timestamp"], 16)

    def make_deposit(self):
        """Route (b): Arbitrum USDT0 -> LayerZero -> F_d1 -> flush -> Vault V2 Deposit."""
        self.log.info("(3) Route (b): bridgeforwarderaddress derives two fresh deposit forwarders")
        anvil, n0 = self.anvil, self.nodes[0]
        self.freeze()
        guards = {}
        for node in self.bridge_nodes():
            guards[node.index] = run_deposit_guard(node)
            assert_equal(guards[node.index], {"deposits": "OPEN", "next_height": node.getblockcount() + 1})
        message = refund_eoa_message(self.domain, self.refund_eoa)
        self.refund_signature = personal_sign(anvil, self.refund_eoa, message)
        first, p1, fd1 = self.derive_forwarder(self.refund_signature, DELIVERY_1)
        second, p2, fd2 = self.derive_forwarder(self.refund_signature, DELIVERY_2)
        assert fd1 != fd2 and first["address"] != second["address"], (fd1, fd2)
        self.forwarder_1 = {"result": first, "p": p1, "account": fd1}
        self.forwarder_2 = {"result": second, "p": p2, "account": fd2}
        self.recipient = first["address"]

        self.log.info("Refused: another key's signature, the vault as refund EOA (OI-6), not_after out of range, "
                      "an amount above MAX_DEPOSIT_RAW (OI-2)")
        wrong = personal_sign(anvil, self.relayer, message)
        now = self.anvil_time()
        refusals = {
            "signature by another key": rpc_refusal(
                RPC_INVALID_ADDRESS_OR_KEY,
                "refund_eoa_signature is not the refund EOA's EIP-191 signature of the refund EOA message.",
                n0.bridgeforwarderaddress, self.refund_eoa, to_0x(wrong)),
            "refund EOA = Vault V2": rpc_refusal(
                RPC_INVALID_ADDRESS_OR_KEY, f"refund_eoa is the bridge V2 launch vault: {OI6_SUFFIX}",
                n0.bridgeforwarderaddress, self.vault, to_0x(self.refund_signature)),
            "refund EOA = deposit factory": rpc_refusal(
                RPC_INVALID_ADDRESS_OR_KEY, f"refund_eoa is the bridge V2 deposit forwarder factory: {OI6_SUFFIX}",
                n0.bridgeforwarderaddress, self.deposit_factory, to_0x(self.refund_signature)),
            "not_after beyond 30 days": rpc_refusal(
                RPC_INVALID_PARAMETER, "not_after must be in the range [",
                n0.bridgeforwarderaddress, self.refund_eoa, to_0x(self.refund_signature),
                {"not_after": now + NOT_AFTER_MAX_LIFETIME + 86_400}),
            "not_after in the past": rpc_refusal(
                RPC_INVALID_PARAMETER, "not_after must be in the range [",
                n0.bridgeforwarderaddress, self.refund_eoa, to_0x(self.refund_signature), {"not_after": 1}),
            "amount above MAX_DEPOSIT_RAW (OI-2)": rpc_refusal(
                RPC_INVALID_PARAMETER, f"amount exceeds max_deposit_raw ({MAX_DEPOSIT_RAW})",
                n0.bridgeforwarderaddress, self.refund_eoa, to_0x(self.refund_signature),
                {"amount": MAX_DEPOSIT_RAW + 1}),
        }
        assert message in refusals["signature by another key"]
        self.evidence.assertion("bridgeforwarderaddress: two fresh F_d, each derived independently; refusals",
                                expected={"f_d1": fd1, "f_d2": fd2},
                                actual={"f_d1": first["forwarder"], "f_d2": second["forwarder"]},
                                params={"f_d1": p1.to_json(), "f_d2": p2.to_json()}, guards=guards,
                                refund_eoa=self.refund_eoa, refusals=refusals)

        self.log.info("(3) 31338: MockUSDT0.send of DEPOSIT_RAW + FLUSH_FEE to F_d1, which has no code yet")
        assert_equal(anvil.code(fd1), b"")
        adapter_before = self.usdt(self.adapter)
        receipt, lz_nonce = self.send_usdt0(fd1, DELIVERY_1)
        relayed = self.deliver_lz(self.arb, self.ep_arb, anvil, self.ep_eth, "route (b) to F_d1", delivered=1)
        packet = relayed["packets"][0]
        assert_equal((packet["nonce"], hx(packet["receiver"]), hx(packet["sender"])),
                     (lz_nonce, word(self.adapter), hx(self.usdt0)))
        assert_equal(decode_oft_message(packet["message"]), (word(fd1), DELIVERY_1))
        again = self.deliver_lz(self.arb, self.ep_arb, anvil, self.ep_eth, "route (b) again", delivered=0)
        assert_equal(again["already_delivered"], 1)
        redeliver = revert_data(anvil, self.ep_eth, forwarders_v2.encode_call(
            SIG_LZ_DELIVER, LZ_EID_ARBITRUM, word(self.usdt0), lz_nonce, self.adapter, hx(packet["message"])),
            self.relayer)
        assert_equal(redeliver[:4], selector("AlreadyDelivered(uint32,bytes32,uint64)"))
        assert_equal((self.usdt(fd1), anvil.code(fd1)), (DELIVERY_1, b""))
        assert_equal(self.usdt(self.adapter), adapter_before - DELIVERY_1)

        self.log.info("PR-2: a flush whose transaction origin has code reverts BadCaller")
        flush_data = forwarders_v2.encode_call(forwarders_v2.SIG_D_FLUSH, p1.abi(), FLUSH_FEE)
        bad_caller = revert_data(anvil, self.deposit_factory, flush_data, self.vault)
        assert_equal(bad_caller, error_selector("BadCaller"))
        # From the codeless relayer the same call only lacks the deployment.
        assert_equal(revert_data(anvil, self.deposit_factory, flush_data, self.relayer), error_selector("NotDeployed"))

        self.log.info("contrib flush (deposit guard OPEN) deploys F_d1 and flushes DEPOSIT_RAW")
        relayer_before = self.usdt(self.relayer)
        nonce_before = self.nonce(anvil, self.relayer)
        out = run_forwarder_tool("flush", anvil, node=n0, sender=self.relayer, factory=self.deposit_factory,
                                 params=first["params"], fee=FLUSH_FEE)
        assert_equal((out["status"], out["deposits"], out["deposit_id"], out["amount"]),
                     ("flushed", "OPEN", FIRST_DEPOSIT_ID, DEPOSIT_RAW))
        assert_equal((hx(out["account"]), out["free"], out["remaining_free"]), (hx(fd1), DELIVERY_1, 0))
        assert out["deploy_tx"], out
        assert out["leaf_size"] <= RECEIPT_LEAF_LIMIT and out["leaf_size_worst_case"] <= RECEIPT_LEAF_LIMIT, out
        assert_equal(out["leaf_size_worst_case"], forwarders_v2.predicted_flush_leaf_size())
        assert_equal(self.nonce(anvil, self.relayer), nonce_before + 2)  # deploy, flush
        assert anvil.code(fd1)
        deploy_receipt, flush_receipt = anvil.receipt(out["deploy_tx"]), anvil.receipt(out["flush_tx"])
        self.evidence.gas("B3DepositForwarderFactory.deploy", gas_used(deploy_receipt),
                          block=int(deploy_receipt["blockNumber"], 16))
        self.evidence.gas("B3DepositForwarderFactory.flush", gas_used(flush_receipt),
                          block=int(flush_receipt["blockNumber"], 16))
        # PR-1 / D1, read back here: the token Transfer and the vault Deposit, nothing else.
        logs = flush_receipt["logs"]
        assert_equal([(hx(log["address"]), [hx(topic) for topic in log["topics"]], hx(log["data"])) for log in logs],
                     [(hx(self.token), [keccak256(EV_TRANSFER.encode()), word(fd1), word(self.vault)],
                       enc_uint(DEPOSIT_RAW)),
                      (hx(self.vault), [keccak256(EV_DEPOSIT.encode()), enc_uint(FIRST_DEPOSIT_ID), word(self.token)],
                       enc_uint(DEPOSIT_RAW) + p1.b3_recipient)])
        k = int(flush_receipt["blockNumber"], 16)
        anvil.mine(1)  # The update's attested block k + 1.
        assert_equal(call_uint(anvil, self.vault, "locked()"), DEPOSIT_RAW)
        assert_equal(call_uint(anvil, self.vault, "nextDepositId()"), FIRST_DEPOSIT_ID + 1)
        assert_equal(self.usdt(self.vault), DEPOSIT_RAW)
        assert_equal(call_uint(anvil, self.deposit_factory, "feesCharged(address)", enc_address(fd1)), FLUSH_FEE)

        self.log.info("collectFee pays the relayer FLUSH_FEE; a second flush finds nothing and sends nothing")
        collect = anvil.send(self.relayer, self.deposit_factory,
                             forwarders_v2.encode_call(SIG_D_COLLECT_FEE, fd1, self.relayer))
        self.evidence.gas("B3DepositForwarderFactory.collectFee", gas_used(collect))
        assert_equal(self.usdt(self.relayer), relayer_before + FLUSH_FEE)
        assert_equal(self.usdt(fd1), 0)
        for signature in (SIG_D_OWED, SIG_D_RESERVED):
            args = (fd1, self.relayer) if signature == SIG_D_OWED else (fd1,)
            assert_equal(forwarders_v2.view(self.eth, self.deposit_factory, signature, *args), 0)
        nonce_before = self.nonce(anvil, self.relayer)
        second_flush = run_forwarder_tool("flush", anvil, node=n0, sender=self.relayer, factory=self.deposit_factory,
                                          params=first["params"], fee=FLUSH_FEE)
        assert_equal((second_flush["status"], second_flush["free"]), ("nothing-to-flush", 0))
        assert_equal(self.nonce(anvil, self.relayer), nonce_before)

        deposits = find_deposits(anvil, self.vault, self.token, self.origin_deployment_block)
        assert_equal(len(deposits), 1)
        deposit = deposits[0]
        assert_equal((deposit["deposit_id"], deposit["block_number"], deposit["amount"]),
                     (FIRST_DEPOSIT_ID, k, DEPOSIT_RAW))
        assert_equal(deposit["recipient"], to_0x(recipient_v1(self.recipient)))
        assert_equal(deposit["block_hash"], anvil.block(k)["hash"])
        assert_equal(self.beacon.period_of_block(k + 1), self.bootstrap["period"])
        self.evidence_step("route (b): F_d1 flushed into Vault V2", anvil, deposit_block=k,
                           deposit_id=FIRST_DEPOSIT_ID, amount=DEPOSIT_RAW, recipient=self.recipient,
                           forwarder=fd1, usdt0_send_tx=receipt["transactionHash"], lz_nonce=lz_nonce,
                           flush=out, second_flush=second_flush, bad_caller=to_0x(bad_caller),
                           redeliver=to_0x(redeliver))
        return deposit

    def check_forwarder_horizon(self):
        """Node changes section 12.3 once the V2 light client is bootstrapped:
        getbridgev2info reports its freshness and horizon, and
        bridgeforwarderaddress ends not_after 24 hours before the horizon."""
        self.log.info("(3) Light client bootstrapped: horizon reported; not_after ends 24 h before it")
        n0 = self.nodes[0]
        info = self.v2_infos()
        lc = info["light_client"]
        assert_equal(lc["bootstrapped"], True)
        assert isinstance(lc["fresh"], bool), lc
        assert_equal(lc["fork_schedule_valid_through_epoch"],
                     self.bootstrap["checkpoint_slot"] // 32 + HORIZON_EPOCHS)
        end_slot = (lc["fork_schedule_valid_through_epoch"] + 1) * 32
        assert_equal(lc["horizon_time"],
                     lc["finalized_execution_timestamp"] + (end_slot - lc["finalized_beacon_slot"]) * 12)
        assert_equal(lc["stale_after"], lc["finalized_execution_timestamp"] +
                     self.pins["v2"]["light_client"]["max_sync_lag_slots"] * 12)
        assert_equal(info["epoch_mint_headroom"], info["max_per_epoch"] - info["epoch_minted"])
        horizon = lc["horizon_time"]
        now = self.anvil_time()
        assert now + 7 * DAY < horizon - DAY < now + NOT_AFTER_MAX_LIFETIME, (now, horizon)

        result = n0.bridgeforwarderaddress(self.refund_eoa, to_0x(self.refund_signature), {"fee_per_tx": FLUSH_FEE})
        p = DepositParams.from_json(result["params"])
        assert_equal((result["horizon_time"], p.not_after), (horizon, horizon - DAY))
        assert_equal(hx(result["forwarder"]),
                     hx(to_0x(deposit_forwarder_address(self.deposit_factory, self.init_code_hash, p))))
        assert not any("not bootstrapped" in warning for warning in result["warnings"]), result["warnings"]
        refusal = rpc_refusal(RPC_INVALID_PARAMETER, "not_after must be in the range [",
                              n0.bridgeforwarderaddress, self.refund_eoa, to_0x(self.refund_signature),
                              {"not_after": horizon - DAY + 1})
        assert refusal.endswith(f", {horizon - DAY}]"), refusal
        self.evidence.assertion("light-client horizon caps a forwarder's not_after (node changes 12.3)",
                                expected={"not_after": horizon - DAY}, actual={"not_after": p.not_after},
                                light_client=lc, epoch_mint_headroom=info["epoch_mint_headroom"],
                                forwarder=result["forwarder"], refusal=refusal)

    # -- 4. route (d3) ---------------------------------------------------------------------
    def arbitrum_route(self, **changes):
        route = {"recipient": self.user, "refund_eoa": self.refund_eoa,
                 "refund_eoa_signature": to_0x(self.refund_signature), "fee_cap": EXIT_FEE_CAP}
        route.update(changes)
        return route

    def burn(self):
        info = self.prepare_burn_gate()
        self.log.info("(4) Route (d3): bridgewithdraw options.arbitrum names F_x; the user publishes p first")
        anvil, n0 = self.anvil, self.nodes[0]
        assert_equal((info["active"], info["burns_active"]), (True, True))
        reserve = info["reserve"]
        assert_equal({key: reserve[key] for key in ("minted", "burned", "reserve", "proven_deposits")},
                     {"minted": DEPOSIT_RAW, "burned": 0, "reserve": DEPOSIT_RAW, "proven_deposits": 1})
        asset_id = self.v2info["asset_id"]

        refusals = {
            "ethereum_recipient = Vault V2": rpc_refusal(
                RPC_INVALID_ADDRESS_OR_KEY, f"ethereum_recipient is the bridge V2 launch vault: {OI6_SUFFIX}",
                n0.bridgewithdraw, BURN_RAW, self.vault),
            "arbitrum recipient = Vault V2": rpc_refusal(
                RPC_INVALID_ADDRESS_OR_KEY, f"arbitrum.recipient is the bridge V2 launch vault: {OI6_SUFFIX}",
                n0.bridgewithdraw, BURN_RAW, None, {"arbitrum": self.arbitrum_route(recipient=self.vault)}),
            "arbitrum recipient = exit factory": rpc_refusal(
                RPC_INVALID_ADDRESS_OR_KEY,
                f"arbitrum.recipient is the bridge V2 exit forwarder factory: {OI6_SUFFIX}",
                n0.bridgewithdraw, BURN_RAW, None, {"arbitrum": self.arbitrum_route(recipient=self.exit_factory)}),
            "fee_cap = amount": rpc_refusal(
                RPC_INVALID_PARAMETER, "amount must exceed arbitrum.fee_cap",
                n0.bridgewithdraw, BURN_RAW, None, {"arbitrum": self.arbitrum_route(fee_cap=BURN_RAW)}),
            "both recipients": rpc_refusal(
                RPC_INVALID_PARAMETER, "ethereum_recipient and options.arbitrum are mutually exclusive",
                n0.bridgewithdraw, BURN_RAW, ETH_RECIPIENT, {"arbitrum": self.arbitrum_route()}),
            "refund EOA = Vault V2": rpc_refusal(
                RPC_INVALID_ADDRESS_OR_KEY, f"arbitrum.refund_eoa is the bridge V2 launch vault: {OI6_SUFFIX}",
                n0.bridgewithdraw, BURN_RAW, None, {"arbitrum": self.arbitrum_route(refund_eoa=self.vault)}),
            "unknown arbitrum option": rpc_refusal(
                RPC_INVALID_PARAMETER, "Unknown arbitrum option 'memo'",
                n0.bridgewithdraw, BURN_RAW, None, {"arbitrum": self.arbitrum_route(memo="x")}),
            "missing arbitrum recipient": rpc_refusal(
                RPC_INVALID_PARAMETER, "options.arbitrum.recipient is required",
                n0.bridgewithdraw, BURN_RAW, None,
                {"arbitrum": {key: value for key, value in self.arbitrum_route().items() if key != "recipient"}}),
            "max_bridge_fee_bps 51": rpc_refusal(
                RPC_INVALID_PARAMETER, "arbitrum.max_bridge_fee_bps must be in the range [0, 50]",
                n0.bridgewithdraw, BURN_RAW, None, {"arbitrum": self.arbitrum_route(max_bridge_fee_bps=51)}),
            "refund EOA signature by another key": rpc_refusal(
                RPC_INVALID_ADDRESS_OR_KEY, "arbitrum.refund_eoa_signature is not the refund EOA's EIP-191 signature",
                n0.bridgewithdraw, BURN_RAW, None, {"arbitrum": self.arbitrum_route(
                    refund_eoa_signature=to_0x(personal_sign(anvil, self.relayer, refund_eoa_message(
                        self.domain, self.refund_eoa))))}),
        }
        assert_equal(self.wallet_asset(n0, asset_id)["confirmed"], DEPOSIT_RAW)

        built = n0.bridgewithdraw(BURN_RAW, None, {"arbitrum": self.arbitrum_route(), "broadcast": False})
        assert_equal((built["broadcast"], built["route"], built["withdrawal_mode"]),
                     (False, "arbitrum", "decentralized-verifier-v2"))
        assert_equal((built["asset_id"], built["amount"], built["asset_change"]),
                     (asset_id, BURN_RAW, DEPOSIT_RAW - BURN_RAW))
        assert_equal((built["burn_vout"], built["binding_vout"]), (0, 1))
        exit_block = built["exit"]
        p = ExitParams.from_json(exit_block["params"])
        expected_p = ExitParams(LZ_EID_ARBITRUM, word(self.user), 0, EXIT_FEE_CAP, self.refund_eoa)
        assert p == expected_p, (p.to_json(), expected_p.to_json())
        account = to_0x(exit_forwarder_address(self.exit_factory, self.init_code_hash, p))
        assert_equal(forwarders_v2.view(self.eth, self.exit_factory, forwarders_v2.SIG_X_ACCOUNT_OF, p.abi(),
                                        returns="address"), account)
        assert_equal(forwarders_v2.view(self.eth, self.exit_factory, f"isValid({EXIT_PARAMS_TYPE})", p.abi(),
                                        returns="bool"), True)
        assert_equal((hx(exit_block["account"]), hx(built["ethereum_recipient"])), (hx(account), hx(account)))
        assert_equal((hx(exit_block["recipient"]), hx(exit_block["factory"]), hx(exit_block["salt"])),
                     (hx(self.user), hx(self.exit_factory), forwarders_v2.exit_salt(p)))
        assert account not in (self.vault, self.token, self.deposit_factory, self.exit_factory, self.user)
        assert_equal(anvil.code(account), b"")
        assert built["txid"] not in n0.getrawmempool()
        self.exit_p, self.exit_account = p, account
        # Positionally the CLI passes ethereum_recipient as a string: with
        # options.arbitrum an empty one counts as omitted.
        positional = n0.bridgewithdraw(BURN_RAW, "", {"arbitrum": self.arbitrum_route(), "broadcast": False})
        assert_equal((positional["route"], hx(positional["ethereum_recipient"])), ("arbitrum", hx(account)))
        assert ExitParams.from_json(positional["exit"]["params"]) == p
        assert positional["txid"] not in n0.getrawmempool()

        # The user publishes p before the burn exists, so any relayer can
        # forward from the factory's Published event alone.
        published = anvil.send(self.user, self.exit_factory, forwarders_v2.encode_call(SIG_X_PUBLISH, p.abi()))
        logs = [log for log in published["logs"] if hx(log["address"]) == hx(self.exit_factory)]
        assert_equal(len(logs), 1)
        kind, logged_account, logged_p = forwarders_v2.decode_published(logs[0])
        assert_equal((kind, to_0x(logged_account)), ("exit", account))
        assert logged_p == p
        listed = run_forwarder_tool("published", anvil, factory=self.exit_factory)
        assert_equal([(entry["kind"], hx(entry["account"]), entry["derivation_ok"]) for entry in listed["published"]],
                     [("exit", hx(account), True)])
        assert_equal(self.v2_infos()["burn_gate"]["status"], "READY")

        def create():
            txid = n0.sendrawtransaction(built["hex"])
            assert_equal(txid, built["txid"])
            return {"txid": txid}

        self.burn_txid, self.burn_height = self.submit_and_mine("bUSD V2 route (d3) burn mined", create)
        assert_greater_than_or_equal(self.burn_height, BRIDGE_V2_BURN_HEIGHT)
        self.burn_recipient = account
        self.leaf = leaf_v2(self.asset_id_raw, 0, REGTEST_ETH_CHAIN_ID, self.vault, self.token, account, BURN_RAW,
                            self.burn_height)
        self.withdrawal_leaves.append((self.burn_height, self.leaf))
        self.root = withdrawal_root([self.leaf])
        after = self.v2_infos()
        assert_equal(after["withdrawals"], 1)
        assert_equal(hx(after["withdrawal_root_evm"]), self.root)
        assert_equal(self.wallet_asset(n0, asset_id)["confirmed"], DEPOSIT_RAW - BURN_RAW)
        assert_equal({key: after["reserve"][key] for key in ("minted", "burned", "reserve")},
                     {"minted": DEPOSIT_RAW, "burned": BURN_RAW, "reserve": DEPOSIT_RAW - BURN_RAW})
        self.evidence.assertion("route (d3) burn: exit block p, leaf V2 paying F_x; wallet refusals",
                                expected={"f_x": account, "p": expected_p.to_json(), "root": to_0x(self.root)},
                                actual={"f_x": built["ethereum_recipient"], "exit": exit_block,
                                        "root": after["withdrawal_root_evm"]},
                                burn_txid=self.burn_txid, burn_height=self.burn_height, leaf=to_0x(self.leaf),
                                published_tx=published["transactionHash"], burn_gate=info["burn_gate"],
                                refusals=refusals, reserve_after=after["reserve"])

    def check_withdrawal_proof(self, covering):
        self.log.info("(4) getbridgewithdrawalproofv2 is identical on every node: exit = p, recipient = F_x")
        n0 = self.nodes[0]
        inclusion, account, p = covering["inclusion"], self.exit_account, self.exit_p
        proofs = [node.getbridgewithdrawalproofv2(self.burn_txid, 0, inclusion) for node in self.bridge_nodes()]
        for proof in proofs[1:]:
            assert_equal(proof, proofs[0])
        proof = proofs[0]
        assert_equal((proof["withdrawal_id"], proof["source_txid"], proof["burn_output_index"]),
                     (0, self.burn_txid, 0))
        assert_equal((proof["b3_height"], proof["b3_block"]), (self.burn_height, n0.getblockhash(self.burn_height)))
        assert_equal(hx(proof["asset_id_evm"]), self.asset_id_raw)
        assert_equal(proof["destination_chain_id"], REGTEST_ETH_CHAIN_ID)
        for key, value in (("vault", self.vault), ("token", self.token), ("recipient", account)):
            assert_equal(hx(proof[key]), hx(value))
        assert_equal(proof["amount_raw"], BURN_RAW)
        exit_block = proof["exit"]
        assert_equal(sorted(exit_block), sorted(["version", "factory", "account", "salt", *ExitParams.FIELDS]))
        assert_equal(exit_block["version"], 1)
        assert ExitParams.from_json(exit_block) == p, exit_block
        assert_equal((hx(exit_block["factory"]), hx(exit_block["account"]), hx(exit_block["salt"])),
                     (hx(self.exit_factory), hx(account), forwarders_v2.exit_salt(p)))
        preimage = leaf_v2_preimage(self.asset_id_raw, 0, REGTEST_ETH_CHAIN_ID, self.vault, self.token, account,
                                    BURN_RAW, self.burn_height)
        assert_equal(hx(proof["leaf_preimage"]), preimage)
        assert_equal((hx(proof["withdrawal_leaf"]), hx(proof["withdrawal_root"])), (self.leaf, self.root))
        root, path = withdrawal_root_and_path([self.leaf], 0)
        assert_equal([hx(sibling) for sibling in proof["path"]], path)
        assert_equal(fold_withdrawal_path(self.leaf, 0, path), root)
        certificate = proof["certificate"]
        assert_equal((certificate["inclusion_height"], certificate["height"]), (inclusion, covering["height"]))
        assert_equal(certificate["signing_validator_count"], VALIDATOR_COUNT)
        assert_equal(proof["function"], VAULT_V2_RELEASE_SIGNATURE)
        assert_equal(hx(proof["calldata"]), encode_vault_v2_release(0, account, BURN_RAW, self.burn_height, path))
        assert_equal(self.anvil.call(self.vault, selector(VAULT_V2_WITHDRAWAL_LEAF_SIGNATURE) +
                                     encode_withdrawal_v2_tuple(0, account, BURN_RAW, self.burn_height)), self.leaf)
        self.log.info("(4) getbridgewithdrawals lists the leaf with the same stored exit block (C-7)")
        listings = [node.getbridgewithdrawals() for node in self.bridge_nodes()]
        for listing in listings[1:]:
            assert_equal(listing, listings[0])
        listing = listings[0]
        assert_equal(listing["withdrawals_total"], 1)
        assert_equal(len(listing["withdrawals"]), 1)
        listed = listing["withdrawals"][0]
        assert_equal((listed["withdrawal_id"], listed["source_txid"], listed["burn_output_index"]),
                     (0, self.burn_txid, 0))
        assert_equal((listed["b3_height"], listed["b3_block"]), (self.burn_height, n0.getblockhash(self.burn_height)))
        assert_equal((listed["entry"], listed["sweep"], listed["amount_raw"]), ("launch", False, BURN_RAW))
        for key, value in (("vault", self.vault), ("token", self.token), ("recipient", account)):
            assert_equal(hx(listed[key]), hx(value))
        assert_equal(hx(listed["withdrawal_leaf"]), self.leaf)
        assert_equal(listed["exit"], exit_block)
        assert_equal(n0.getbridgewithdrawals(1), {"withdrawals_total": 1, "withdrawals": []})
        assert_equal(n0.getbridgewithdrawals(0, 1), listing)
        refusals = {
            "certificate below V": rpc_refusal(
                RPC_MISC_ERROR, "below the bridge V2 activation height",
                n0.getbridgewithdrawalproofv2, self.burn_txid, 0, self.init_cert["inclusion"]),
            "mint is not a burn": rpc_refusal(
                RPC_INVALID_ADDRESS_OR_KEY, "Burn source is not a confirmed V2 withdrawal",
                n0.getbridgewithdrawalproofv2, self.mint_txid, 0, inclusion),
            "getbridgewithdrawals count 0": rpc_refusal(
                RPC_INVALID_PARAMETER, "count must be in the range [1, 1000]", n0.getbridgewithdrawals, 0, 0),
        }
        self.withdrawal_listing = listing
        self.evidence.assertion("withdrawal proof and listing: exit block stored with the leaf; recipient F_x",
                                expected={"exit": p.to_json(), "recipient": account, "root": to_0x(self.root)},
                                actual={"exit": exit_block, "recipient": proof["recipient"],
                                        "root": proof["withdrawal_root"]},
                                certificate=certificate, listing=listing, refusals=refusals)
        return proof

    def forward_balances(self):
        anvil, account = self.anvil, self.exit_account
        return {
            "vault": self.usdt(self.vault), "locked": call_uint(anvil, self.vault, "locked()"),
            "released_0": call_uint(anvil, self.vault, "released(uint64)", enc_uint(0)),
            "f_x": self.usdt(account), "relayer": self.usdt(self.relayer), "adapter": self.usdt(self.adapter),
            "batch_wei": self.wei(anvil, self.batch), "f_x_wei": self.wei(anvil, account),
            "exit_factory_wei": self.wei(anvil, self.exit_factory),
            "user_usdt0": self.usdt0_balance(self.user), "usdt0_supply": call_uint(self.arb, self.usdt0,
                                                                                  "totalSupply()"),
        }

    def release_and_forward(self, proof, covering):
        self.log.info("(4) contrib forward: one relayWithForwards releases to F_x and forwards through the adapter")
        anvil, n0, account, p = self.anvil, self.nodes[0], self.exit_account, self.exit_p
        before = self.forward_balances()
        assert_equal({key: before[key] for key in ("vault", "locked", "released_0", "f_x", "batch_wei")},
                     {"vault": DEPOSIT_RAW, "locked": DEPOSIT_RAW, "released_0": 0, "f_x": 0, "batch_wei": 0})
        quote = forwarders_v2.view(self.eth, self.exit_factory, forwarders_v2.SIG_X_QUOTE_FORWARD, p.abi(),
                                   BURN_RAW - FORWARD_FEE)
        assert_equal(quote, call_uint(anvil, self.ep_eth, "quote()"))
        relayer_wei, nonce_before = self.wei(anvil, self.relayer), self.nonce(anvil, self.relayer)
        eth_head = anvil.block_number()
        out = run_forwarder_tool("forward", anvil, node=n0, sender=self.relayer, batch=self.batch,
                                 txid=self.burn_txid, vout=0, certificate_block=covering["inclusion"],
                                 fee=FORWARD_FEE, native_margin=NATIVE_MARGIN)
        assert_equal((out["status"], out["release_included"], out["released_by_this_tx"]), ("forwarded", True, True))
        assert_equal((hx(out["account"]), hx(out["batch"]), out["withdrawal_id"]), (hx(account), hx(self.batch), 0))
        assert_equal((out["amount_sent"], out["amount_received"], out["fee"]),
                     (BURN_RAW - FORWARD_FEE, BURN_RAW - FORWARD_FEE, FORWARD_FEE))
        assert_equal((hx(out["fee_recipient"]), out["native_fee"], out["native_value"]),
                     (hx(self.relayer), quote, quote + NATIVE_MARGIN))
        assert ExitParams.from_json(out["exit"]) == p
        assert_equal(self.nonce(anvil, self.relayer), nonce_before + 1)  # one transaction
        receipt = anvil.receipt(out["tx"])
        self.evidence.gas("B3RelayBatch.relayWithForwards (release + first forward)", gas_used(receipt),
                          block=int(receipt["blockNumber"], 16), gas_limit=out["gas_limit"])
        withdrawn = [log for log in receipt["logs"] if hx(log["address"]) == hx(self.vault)]
        assert_equal(len(withdrawn), 1)
        assert_equal([hx(topic) for topic in withdrawn[0]["topics"]],
                     [keccak256(EV_WITHDRAWN.encode()), enc_uint(0), word(self.token), word(account)])
        packets = anvil.get_logs(self.ep_eth, [keccak256(EV_PACKET_SENT.encode())], from_block=eth_head + 1)
        assert_equal(len(packets), 1)
        topics = [hx(topic) for topic in packets[0]["topics"]]
        assert_equal(topics[2:], [enc_uint(LZ_EID_ARBITRUM), word(self.adapter)])
        receiver, message = forwarders_v2.decode_abi(["bytes32", "bytes"], hx(packets[0]["data"]))
        assert_equal((receiver, decode_oft_message(message)), (word(self.usdt0), (word(self.user),
                                                                                  BURN_RAW - FORWARD_FEE)))

        after = self.forward_balances()
        gas_cost = gas_used(receipt) * int(receipt["effectiveGasPrice"], 16)
        assert_equal({key: after[key] for key in ("vault", "locked", "released_0", "f_x", "relayer", "adapter",
                                                  "batch_wei", "f_x_wei", "exit_factory_wei")},
                     {"vault": DEPOSIT_RAW - BURN_RAW, "locked": DEPOSIT_RAW - BURN_RAW, "released_0": 1, "f_x": 0,
                      "relayer": before["relayer"] + FORWARD_FEE,
                      "adapter": before["adapter"] + BURN_RAW - FORWARD_FEE,
                      "batch_wei": 0, "f_x_wei": 0, "exit_factory_wei": 0})
        # The OFT quote is all the ETH the relayer spent beyond gas: the margin came back.
        assert_equal(relayer_wei - self.wei(anvil, self.relayer), gas_cost + quote)
        assert anvil.code(account), "forward deploys F_x"
        assert_equal(call_uint(anvil, self.token, "allowance(address,address)", enc_address(account),
                               enc_address(self.adapter)), 0)

        self.log.info("31337 -> 31338: the LayerZero relayer delivers BURN_RAW - FORWARD_FEE USDT0 to the user")
        self.deliver_lz(anvil, self.ep_eth, self.arb, self.ep_arb, "route (d3) to the user", delivered=1)
        delivered = self.forward_balances()
        assert_equal(delivered["user_usdt0"], before["user_usdt0"] + BURN_RAW - FORWARD_FEE)
        assert_equal(delivered["usdt0_supply"], before["usdt0_supply"] + BURN_RAW - FORWARD_FEE)

        self.log.info("Exactly once: a second forward is refused before any transaction")
        nonce_before = self.nonce(anvil, self.relayer)
        try:
            run_forwarder_tool("forward", anvil, node=n0, sender=self.relayer, batch=self.batch,
                               txid=self.burn_txid, vout=0, certificate_block=covering["inclusion"],
                               fee=FORWARD_FEE, native_margin=NATIVE_MARGIN)
        except ForwarderToolError as e:
            second = e.message
        else:
            raise AssertionError("a second forward was sent")
        assert "is already released and its exit forwarder" in second and "nothing to forward" in second, second
        assert_equal(self.nonce(anvil, self.relayer), nonce_before)

        self.log.info("Sent directly, the batch skips the release and reports ForwardFailed(NothingToForward)")
        withdrawal = (0, account, BURN_RAW, self.burn_height)
        path = [hx(sibling) for sibling in proof["path"]]
        calldata = forwarders_v2.encode_call(
            forwarders_v2.SIG_RELAY_WITH_FORWARDS, self.verifier, [], 0, self.vault, [(withdrawal, path)],
            forwarders_v2.DEFAULT_RELEASE_GAS, self.exit_factory, [(p.abi(), FORWARD_FEE, self.relayer, quote)],
            forwarders_v2.DEFAULT_FORWARD_GAS, self.relayer)
        gas = forwarders_v2.batch_gas_limit(calldata, 1, forwarders_v2.DEFAULT_RELEASE_GAS, 1,
                                            forwarders_v2.DEFAULT_FORWARD_GAS)
        relayer_wei = self.wei(anvil, self.relayer)
        direct = anvil.send(self.relayer, self.batch, calldata, value=quote, gas=gas)
        batch_logs = [(hx(log["topics"][0]), [hx(topic) for topic in log["topics"][1:]], hx(log["data"]))
                      for log in direct["logs"]]
        nothing = error_selector("NothingToForward")
        assert_equal(batch_logs, [
            (keccak256(EV_RELEASE_SKIPPED.encode()), [enc_uint(0), enc_uint(0)], b""),
            (keccak256(EV_FORWARD_FAILED.encode()), [enc_uint(0), word(account)],
             forwarders_v2.encode_abi(["bytes"], [nothing])),
        ])
        assert all(hx(log["address"]) == hx(self.batch) for log in direct["logs"])
        assert_equal(relayer_wei - self.wei(anvil, self.relayer),
                     gas_used(direct) * int(direct["effectiveGasPrice"], 16))
        no_packet = self.deliver_lz(anvil, self.ep_eth, self.arb, self.ep_arb, "route (d3) again", delivered=0)
        assert_equal(no_packet["already_delivered"], 1)
        # Nothing moved: no token, no wei, no USDT0.
        assert_equal(self.forward_balances(), delivered)
        self.evidence_step("route (d3): released and forwarded once; USDT0 delivered on 31338", anvil,
                           forward=out, second_forward=second, direct_batch_tx=direct["transactionHash"],
                           balances={"before": before, "after": after, "delivered": delivered},
                           arbitrum_block=self.arb.block_number())

    # -- 5. closure ---------------------------------------------------------------------------
    def close_deposits_for_good(self):
        self.log.info("(5) RT4c's kind 13 conflict closes bridge V2 deposits for good on every node")
        n0 = self.nodes[0]
        self.settle_fresh_lineage(BURN_FINALITY_LAG_MARGIN)
        self.relay_certificates("before the conflict", require_new=False)
        frozen = self.read_conflict_inputs()
        conflict = self.submit_conflict(frozen)
        reason = conflict["deposits_reason"]
        refusals = {}
        for node in self.bridge_nodes():
            info = node.getbridgev2info()
            assert_equal(info["conflict"], conflict["expected"])
            assert_equal(info["deposits"], {"status": "CLOSED_FOR_GOOD", "reason": reason})
            try:
                run_deposit_guard(node)
            except DepositGuardError as e:
                assert_equal(e.message, f"bridge V2 deposits are closed for good: {reason}; "
                                        f"{DEPOSITS_CLOSED_TOOL_SUFFIX}")
                refusals[f"node{node.index} deposit guard"] = e.message
            else:
                raise AssertionError(f"the deposit guard reported OPEN on node{node.index}")
        refusals["bridgeforwarderaddress"] = rpc_refusal(
            RPC_VERIFY_REJECTED, f"bridge V2 deposits are closed for good: {reason}; {FORWARDER_CLOSED_SUFFIX}",
            n0.bridgeforwarderaddress, self.refund_eoa, to_0x(self.refund_signature))
        self.evidence.assertion("B3 closed bridge V2 deposits for good; forwarder derivation refused",
                                expected={"status": "CLOSED_FOR_GOOD", "reason": reason},
                                actual=n0.getbridgev2info()["deposits"], refusals=refusals)
        return conflict

    def flush_refused_after_closure(self, conflict):
        self.log.info("(5) USDT0 reaches F_d2 through LayerZero; the contrib flush refuses and sends nothing")
        anvil, n0 = self.anvil, self.nodes[0]
        fd2, p2 = self.forwarder_2["account"], self.forwarder_2["p"]
        assert_equal(anvil.code(fd2), b"")
        _, lz_nonce = self.send_usdt0(fd2, DELIVERY_2)
        relayed = self.deliver_lz(self.arb, self.ep_arb, anvil, self.ep_eth, "route (b) to F_d2", delivered=1)
        assert_equal([packet["nonce"] for packet in relayed["packets"] if packet["status"] == "delivered"], [lz_nonce])
        assert_equal(self.usdt(fd2), DELIVERY_2)
        nonce_before, head = self.nonce(anvil, self.relayer), anvil.block_number()
        try:
            run_forwarder_tool("flush", anvil, node=n0, sender=self.relayer, factory=self.deposit_factory,
                               params=self.forwarder_2["result"]["params"], fee=FLUSH_FEE)
        except ForwarderToolError as e:
            refused = e.message
        else:
            raise AssertionError("the contrib flush ran after deposits closed for good")
        assert_equal(refused, f"bridge V2 deposits are closed for good: {conflict['deposits_reason']}; "
                              f"{DEPOSITS_CLOSED_TOOL_SUFFIX}")
        assert_equal((self.nonce(anvil, self.relayer), anvil.block_number()), (nonce_before, head))
        assert_equal((self.usdt(fd2), anvil.code(fd2)), (DELIVERY_2, b""))
        assert_equal(DepositParams.from_json(self.forwarder_2["result"]["params"]), p2)
        self.evidence.assertion("contrib flush refused after closure; no Ethereum transaction",
                                expected="closed for good", actual=refused, forwarder=fd2,
                                relayer_nonce=nonce_before, eth_head=head)

    def refund_after_closure(self):
        self.log.info("(5) Verifier V2 closed: F_d2 deploys, its flush reverts DepositClosed, refundEOA refunds it")
        anvil = self.anvil
        fd2, p2 = self.forwarder_2["account"], self.forwarder_2["p"]
        vault_before = {"balance": self.usdt(self.vault), "locked": call_uint(anvil, self.vault, "locked()"),
                        "next_deposit_id": call_uint(anvil, self.vault, "nextDepositId()")}
        deployed = anvil.send(self.relayer, self.deposit_factory,
                              forwarders_v2.encode_call(forwarders_v2.SIG_D_DEPLOY, p2.abi()))
        assert anvil.code(fd2)
        published = [forwarders_v2.decode_published(log) for log in deployed["logs"]
                     if hx(log["address"]) == hx(self.deposit_factory)]
        assert_equal([(kind, to_0x(account)) for kind, account, _ in published], [("deposit", fd2)])
        assert published[0][2] == p2
        flush = forwarders_v2.encode_call(forwarders_v2.SIG_D_FLUSH, p2.abi(), FLUSH_FEE)
        closed = revert_data(anvil, self.deposit_factory, flush, self.relayer)
        assert_equal(closed, error_selector("DepositClosed"))
        refund_before = self.usdt(self.refund_eoa)
        refund = anvil.send(self.refund_eoa, self.deposit_factory, forwarders_v2.encode_call(
            SIG_D_REFUND, p2.abi(), self.token, self.refund_eoa, DELIVERY_2))
        self.evidence.gas("B3DepositForwarderFactory.refund", gas_used(refund))
        assert_equal(self.usdt(self.refund_eoa), refund_before + DELIVERY_2)
        assert_equal(self.usdt(fd2), 0)
        vault_after = {"balance": self.usdt(self.vault), "locked": call_uint(anvil, self.vault, "locked()"),
                       "next_deposit_id": call_uint(anvil, self.vault, "nextDepositId()")}
        assert_equal(vault_after, vault_before)
        self.evidence.assertion("after closure: F_d2 flush reverts DepositClosed; refundEOA refunds the delivery",
                                expected={"flush": to_0x(error_selector("DepositClosed")), "refunded": DELIVERY_2},
                                actual={"flush": to_0x(closed), "refunded": self.usdt(self.refund_eoa) - refund_before},
                                vault_before=vault_before, vault_after=vault_after, forwarder=fd2,
                                deploy_tx=deployed["transactionHash"], refund_tx=refund["transactionHash"])

    def check_replay_and_reindex(self, conflict):
        """RT4c's replay and reindex; the bridge index rebuilds the stored exit
        block, so getbridgewithdrawals is unchanged on every node."""
        super().check_replay_and_reindex(conflict)
        for node in self.bridge_nodes():
            assert_equal(node.getbridgewithdrawals(), self.withdrawal_listing)
        self.evidence.assertion("getbridgewithdrawals after replay and reindex",
                                expected=self.withdrawal_listing, actual=self.nodes[0].getbridgewithdrawals())

    # -- 6. accounting ----------------------------------------------------------------------------
    def check_final_accounting(self):
        self.log.info("(6) Vault, B3 reserve and wallets agree; USDT conserved; USDT0 backed by the adapter")
        anvil, arb = self.anvil, self.arb
        self.freeze()
        locked = call_uint(anvil, self.vault, "locked()")
        assert_equal((locked, self.usdt(self.vault)), (DEPOSIT_RAW - BURN_RAW, DEPOSIT_RAW - BURN_RAW))
        info = self.v2_infos()
        reserve = info["reserve"]
        assert_equal({key: reserve[key] for key in ("minted", "burned", "reserve", "proven_deposits")},
                     {"minted": DEPOSIT_RAW, "burned": BURN_RAW, "reserve": locked, "proven_deposits": 1})
        held = {node.index: self.wallet_asset(node, self.v2info["asset_id"])["confirmed"]
                for node in self.bridge_nodes()}
        assert_equal(sum(held.values()), reserve["reserve"])
        assert_equal((info["nullifiers"], info["withdrawals"]), (1, 1))

        holders = {
            "owner": self.owner, "depositor (user, refund EOA)": self.depositor, "deployer": self.deployer,
            "relayer": self.relayer, "oft adapter": self.adapter, "vault": self.vault,
            "f_d1": self.forwarder_1["account"], "f_d2": self.forwarder_2["account"], "f_x": self.exit_account,
            "deposit factory": self.deposit_factory, "exit factory": self.exit_factory, "batch": self.batch,
            "eth recipient": ETH_RECIPIENT,
        }
        balances = {name: self.usdt(address) for name, address in holders.items()}
        expected = dict.fromkeys(holders, 0)
        expected.update({
            "depositor (user, refund EOA)": TOKEN_SUPPLY - ADAPTER_SEED + DELIVERY_2,
            "relayer": FLUSH_FEE + FORWARD_FEE,
            "oft adapter": ADAPTER_SEED - DELIVERY_1 - DELIVERY_2 + BURN_RAW - FORWARD_FEE,
            "vault": DEPOSIT_RAW - BURN_RAW,
        })
        assert_equal(balances, expected)
        supply = call_uint(anvil, self.token, "totalSupply()")
        assert_equal((sum(balances.values()), supply), (TOKEN_SUPPLY, TOKEN_SUPPLY))

        usdt0_supply = call_uint(arb, self.usdt0, "totalSupply()")
        expected_usdt0 = ARB_USDT0_MINT - DELIVERY_1 - DELIVERY_2 + BURN_RAW - FORWARD_FEE
        assert_equal((usdt0_supply, self.usdt0_balance(self.user)), (expected_usdt0, expected_usdt0))
        # Every USDT0 on 31338 is backed by USDT the adapter holds on 31337.
        assert_equal(balances["oft adapter"], usdt0_supply)
        assert_equal(call_uint(anvil, self.adapter, "lockedBalance()"), usdt0_supply)
        assert_greater_than(locked, 0)
        self.evidence.assertion("final accounting: vault = B3 reserve = wallets; USDT conserved; USDT0 backed",
                                expected={"locked": DEPOSIT_RAW - BURN_RAW, "usdt": expected,
                                          "usdt0_supply": expected_usdt0},
                                actual={"locked": locked, "reserve": reserve, "wallets": held, "usdt": balances,
                                        "usdt_supply": supply, "usdt0_supply": usdt0_supply})

if __name__ == '__main__':
    B3BridgeRT6RouteBD3Test(__file__).main()
