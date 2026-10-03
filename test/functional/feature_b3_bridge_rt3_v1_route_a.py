#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""RT3a: V1 route (a) deposit to bUSD mint (doc/design/b3-bridge-v2-regtest-plan.md §5).

REGTEST ONLY. Every process is local: four B3 regtest validators linked only
to each other, one anvil on 127.0.0.1 (chain 31337, never forked) and the
synthetic sync-committee tool. forge runs with --offline.

--deposit-only (RT3a, the only mode until RT3):

1. anvil gets MockTetherToken (block 1), MockDepositVerifier (block 2) and the
   production B3StakerBridge against it (block 3 = d). The pinned light-client
   checkpoint is H(d) itself (the +0 window case), mid sync-committee period.
2. n0-n3 start from height 0 with one regtest -b3bridgepins file and bring
   up live BLS finality to A3 - 1; getbridgeinfo matches the pins and anvil
   on every node.
3. An Ethereum deposit of 25,000,000 raw units to n0's RECIPIENT_V1 is
   proven by b3-bridge-ethcheck from the synthetic bootstrap and one update
   finalizing the deposit block.
4. The bootstrap and update carriers, then the mint, are validated by B3
   consensus on all four nodes; n0's wallet holds the bUSD.
5. Ordering is enforced (mint before bootstrap, update before bootstrap, mint
   before its anchor) and every replay is refused: bootstrap, update (no-op)
   and the mint (nullified, also with amount + 1).
"""

import os

from test_framework.anvil import deploy
from test_framework.b3bridge import (
    BRIDGE_ACTIVATION_HEIGHT,
    PERIOD_SLOTS,
    REGTEST_ETH_CHAIN_ID,
    REGTEST_FORK_VERSION,
    REGTEST_GVR,
    REGTEST_MODERN_GENESIS_HASH,
    B3BridgeChainMixin,
    SyntheticBeacon,
    abi_call,
    build_direct_mint_plan,
    call_address,
    call_bytes32,
    call_uint,
    enc_address,
    enc_bytes32,
    enc_uint,
    file_sha256,
    find_deposits,
    recipient_v1,
    regtest_chain_domain,
    rpc_refusal,
    to_0x,
    write_regtest_bridge_pins,
)
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import (
    assert_equal,
    assert_greater_than_or_equal,
    assert_raises_rpc_error,
)

# B3StakerBridge.MAX_DEPOSIT_RAW; equal to the node's default per-block cap.
MAX_DEPOSIT_RAW = 1_000_000_000
TOKEN_SUPPLY = 10**12
DEPOSIT_RAW = 25_000_000
FIRST_DEPOSIT_ID = 0  # B3StakerBridge.nextDepositId starts at zero.
RPC_VERIFY_REJECTED = -26
RPC_INVALID_PARAMETER = -8


def mid_period_slot_offset(checkpoint_block):
    """slot_offset putting slot(checkpoint_block) mid-period (the +0 case).

    slot(c) = 8192 * (c // 8192 + 2) + 4096, so the bootstrap and every update
    of this test stay in one sync-committee period.
    """
    return PERIOD_SLOTS * (checkpoint_block // PERIOD_SLOTS + 2) + PERIOD_SLOTS // 2 - checkpoint_block


def gas_used(receipt):
    return int(receipt["gasUsed"], 16)


class B3BridgeRT3V1RouteATest(B3BridgeChainMixin, BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument(
            "--deposit-only", dest="deposit_only", default=False, action="store_true",
            help="RT3a: mock deposit verifier, deposit, mint, replay refused "
                 "(the only mode until RT3)")

    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        if not self.options.deposit_only:
            raise SkipTest("only --deposit-only (RT3a) is implemented; full RT3 "
                           "(bootstrap signing, certificates, release) is not")
        self.skip_if_no_wallet()
        self.skip_if_no_foundry(("anvil", "forge"))
        self.skip_if_no_bridge_tools()

    # -- Ethereum side and pins (before any node starts) --------------------
    def setup_network(self):
        self.log.info("anvil: MockTetherToken, MockDepositVerifier and B3StakerBridge")
        anvil = self.anvil = self.start_anvil()
        self.owner, self.depositor = anvil.accounts()[:2]
        self.bridge_forge_build()
        domain = regtest_chain_domain()
        self.token, token_block, self.token_code_hash = self.deploy_contract(
            "MockTetherToken.sol", "MockTetherToken")
        self.verifier, verifier_block, self.verifier_code_hash = self.deploy_contract(
            "MockDepositVerifier.sol", "MockDepositVerifier",
            enc_bytes32(domain) + enc_uint(BRIDGE_ACTIVATION_HEIGHT))
        self.vault, self.deployment_block, self.vault_code_hash = self.deploy_contract(
            "B3StakerBridge.sol", "B3StakerBridge",
            enc_address(self.verifier) + self.verifier_code_hash + domain +
            enc_address(self.token) + enc_uint(MAX_DEPOSIT_RAW))
        assert_equal((token_block, verifier_block, self.deployment_block), (1, 2, 3))
        assert_equal(call_address(anvil, self.vault, "ORIGIN_TOKEN()"), self.token)
        assert_equal(call_address(anvil, self.vault, "verifier()"), self.verifier)
        assert_equal(call_uint(anvil, self.vault, "BRIDGE_ACTIVATION_HEIGHT()"),
                     BRIDGE_ACTIVATION_HEIGHT)
        assert_equal(call_bytes32(anvil, self.vault, "B3_CHAIN_DOMAIN()"), domain)

        self.log.info("anvil: issue, fund the depositor and approve the vault once")
        anvil.send(self.owner, self.token, abi_call("issue(uint256)", enc_uint(TOKEN_SUPPLY)))
        anvil.send(self.owner, self.token, abi_call(
            "transfer(address,uint256)", enc_address(self.depositor), enc_uint(TOKEN_SUPPLY)))
        anvil.send(self.depositor, self.token, abi_call(
            "approve(address,uint256)", enc_address(self.vault), enc_uint(TOKEN_SUPPLY)))

        self.log.info("Synthetic bootstrap at the deployment block (+0) and the pin file")
        checkpoint_block = self.deployment_block
        self.slot_offset = mid_period_slot_offset(checkpoint_block)
        self.beacon = self.start_synthetic_beacon(anvil, self.slot_offset, checkpoint_block)
        self.bootstrap = self.beacon.bootstrap()
        assert_equal(self.bootstrap["checkpoint_block"], checkpoint_block)
        assert_equal(self.bootstrap["checkpoint_slot"], self.slot_offset + checkpoint_block)
        assert_equal(self.bootstrap["checkpoint_slot"] % PERIOD_SLOTS, PERIOD_SLOTS // 2)
        assert_equal(self.bootstrap["checkpoint_block_hash"], anvil.block(checkpoint_block)["hash"])
        self.pins_path = os.path.join(self.options.tmpdir, "b3bridgepins.json")
        self.pins = write_regtest_bridge_pins(
            self.pins_path, anvil, self.vault, self.token, self.verifier,
            self.deployment_block, self.bootstrap)
        v1 = self.pins["v1_decentralized"]
        assert_equal(v1["light_client"]["checkpoint_execution_block"], self.deployment_block)
        assert_equal(v1["origin_deployment_block"], self.deployment_block)
        # No node runs yet: record the Ethereum head only.
        self.evidence.step("ethereum deployed and pins written", eth_block=anvil.block_number(),
                           token=self.token, verifier=self.verifier, vault=self.vault,
                           deployment_block=self.deployment_block,
                           checkpoint_block=checkpoint_block,
                           checkpoint_slot=self.bootstrap["checkpoint_slot"],
                           checkpoint_root=self.bootstrap["checkpoint_root"],
                           pins_sha256=file_sha256(self.pins_path))

        self.start_bridge_validators(self.pins_path)

    def deploy_contract(self, file, contract, ctor_bytes=b""):
        address, block, code_hash = deploy(
            self.anvil, self.owner, self.bridge_artifact(file, contract), ctor_bytes)
        for tx_hash in self.anvil.block(block)["transactions"]:
            receipt = self.anvil.receipt(tx_hash)
            if (receipt.get("contractAddress") or "").lower() == address:
                self.evidence.gas(f"deploy {contract}", gas_used(receipt), block=block)
        return address, block, code_hash

    # -- test ---------------------------------------------------------------
    def run_test(self):
        self.log.info("B3: four validators with live BLS finality up to A3 - 1")
        self.bring_up_validators(BRIDGE_ACTIVATION_HEIGHT - 1)
        self.evidence_step("B3 validators live", self.anvil,
                           finalized=self.finalized_height(self.nodes[0]))
        info = self.check_bridge_info()

        deposit = self.make_deposit()
        plan = self.build_plan(deposit, info["registry_id"])
        self.relay_bootstrap(plan)
        self.relay_update(plan, deposit)
        self.claim_mint(plan, deposit, info)
        self.check_replays(plan)

    def check_bridge_info(self):
        self.log.info("getbridgeinfo matches the pins and anvil on every node")
        anvil = self.anvil
        v1 = self.pins["v1_decentralized"]
        light = v1["light_client"]
        asset_id_evm = to_0x(call_bytes32(anvil, self.vault, "B3_ASSET_ID()"))
        # bring_up_validators() waits for n0 only; every node must evaluate A3.
        self.wait_until(lambda: all(node.getblockcount() >= BRIDGE_ACTIVATION_HEIGHT - 1
                                    for node in self.bridge_nodes()))
        infos = []
        for node in self.bridge_nodes():
            assert_equal(node.getblockhash(0), REGTEST_MODERN_GENESIS_HASH)
            info = node.getbridgeinfo()
            assert_equal(info["status"], "active")
            assert_equal(info["configured"], True)
            assert_equal(info["ready"], True)
            assert_equal(info["active"], True)
            assert_equal(info["mint_approval_open"], True)
            assert_equal(info["state_available"], True)
            assert_greater_than_or_equal(info["next_height"], BRIDGE_ACTIVATION_HEIGHT)
            assert_equal(info["activation_height"], BRIDGE_ACTIVATION_HEIGHT)
            assert_equal(info["withdrawal_mode"], "decentralized-verifier-v1")
            assert_equal(info["ethereum_chain_id"], REGTEST_ETH_CHAIN_ID)
            assert_equal(info["origin_deployment_block"], self.deployment_block)
            assert_equal("0x" + info["vault"], self.vault)
            assert_equal("0x" + info["token"], self.token)
            assert_equal("0x" + info["decentralized_verifier"], self.verifier)
            assert_equal("0x" + info["vault_runtime_code_hash"], to_0x(anvil.code_hash(self.vault)))
            assert_equal("0x" + info["implementation_or_adapter"], to_0x(anvil.code_hash(self.token)))
            assert_equal("0x" + info["decentralized_verifier_code_hash"],
                         to_0x(anvil.code_hash(self.verifier)))
            assert_equal("0x" + info["vault_runtime_code_hash"], v1["vault_runtime_code_hash"])
            assert_equal("0x" + info["implementation_or_adapter"], v1["token_runtime_code_hash"])
            assert_equal("0x" + info["decentralized_verifier_code_hash"],
                         v1["verifier_runtime_code_hash"])
            assert_equal("0x" + info["trusted_checkpoint_root"], light["trusted_checkpoint_root"])
            assert_equal("0x" + info["trusted_checkpoint_root"], self.bootstrap["checkpoint_root"])
            assert_equal(info["trusted_checkpoint_slot"], light["trusted_checkpoint_slot"])
            assert_equal("0x" + info["genesis_validators_root"], to_0x(REGTEST_GVR))
            assert_equal(info["ethereum_fork_schedule"],
                         [{"activation_epoch": 0, "fork_version": REGTEST_FORK_VERSION.hex()}])
            assert_equal(info["electra_epoch"], 0)
            assert_equal(info["min_sync_committee_participants"], 342)
            assert_equal(info["asset_id_evm"], asset_id_evm)
            assert_equal(info["light_client_bootstrapped"], False)
            assert_equal(info["nullifiers"], 0)
            infos.append(info)
        # Every node derives the same registry and asset from the same pins.
        for key in ("registry_id", "asset_id", "max_per_block", "max_per_epoch"):
            assert_equal({info[key] for info in infos}, {infos[0][key]})
        self.evidence.assertion("getbridgeinfo matches pins and anvil on every node",
                                expected=v1["vault"], actual="0x" + infos[0]["vault"],
                                registry_id=infos[0]["registry_id"],
                                asset_id=infos[0]["asset_id"], asset_id_evm=asset_id_evm)
        return infos[0]

    def make_deposit(self):
        self.log.info("anvil: deposit 25,000,000 raw units to n0's RECIPIENT_V1")
        anvil = self.anvil
        self.recipient = self.nodes[0].getnewaddress()
        receipt = anvil.send(self.depositor, self.vault, abi_call(
            "deposit(uint256,bytes32)", enc_uint(DEPOSIT_RAW), recipient_v1(self.recipient)))
        self.evidence.gas("B3StakerBridge.deposit", gas_used(receipt))
        k = int(receipt["blockNumber"], 16)
        anvil.mine(1)  # The update's attested block k + 1.
        assert_equal(call_uint(anvil, self.vault, "locked()"), DEPOSIT_RAW)
        assert_equal(call_uint(anvil, self.vault, "nextDepositId()"), FIRST_DEPOSIT_ID + 1)
        assert_equal(call_uint(anvil, self.token, "balanceOf(address)", enc_address(self.vault)),
                     DEPOSIT_RAW)

        deposits = find_deposits(anvil, self.vault, self.token, self.deployment_block)
        assert_equal(len(deposits), 1)
        deposit = deposits[0]
        assert_equal(deposit["deposit_id"], FIRST_DEPOSIT_ID)
        assert_equal(deposit["block_number"], k)
        assert_equal(deposit["amount"], DEPOSIT_RAW)
        assert_equal(deposit["recipient"], to_0x(recipient_v1(self.recipient)))
        assert_equal(deposit["block_hash"], anvil.block(k)["hash"])
        # One period: the deposit block and its attested successor share the
        # bootstrap's sync-committee period.
        assert_equal(self.beacon.period_of_block(k + 1), self.bootstrap["period"])
        self.evidence_step("ethereum deposit", anvil, deposit_block=k,
                           deposit_id=FIRST_DEPOSIT_ID, amount=DEPOSIT_RAW,
                           recipient=self.recipient)
        return deposit

    def build_plan(self, deposit, registry_id):
        self.log.info("b3-bridge-ethcheck: bootstrap, update(k) and the mint")
        k = deposit["block_number"]
        plan = build_direct_mint_plan(os.path.join(self.options.tmpdir, "mint_plan"),
                                      self.beacon, self.anvil.url, deposit, registry_id, [k])
        update = self.beacon.update(k)
        assert_equal(plan["bootstrap"]["kind"], "bootstrap")
        assert_equal(plan["bootstrap"]["payload_hex"], self.bootstrap["payload_hex"])
        assert_equal([u["kind"] for u in plan["updates"]], ["update"])
        assert_equal(plan["updates"][0]["payload_hex"], update["payload_hex"])
        assert_equal(plan["backfills"], [])
        assert_equal((update["finalized_block"], update["finalized_block_hash"]),
                     (k, deposit["block_hash"]))
        assert_equal((update["participants"], update["has_next_committee"]), (512, True))
        mint = plan["mint"]
        assert_equal(mint["kind"], "mint")
        assert_equal(mint["registry_id"], registry_id)
        assert_equal((mint["deposit_id"], mint["origin_amount"], mint["amount"]),
                     (FIRST_DEPOSIT_ID, DEPOSIT_RAW, DEPOSIT_RAW))
        assert_equal((mint["target_block_number"], mint["target_block_hash"]),
                     (k, deposit["block_hash"]))
        assert_equal(mint["source_anchor_hash"], deposit["block_hash"])
        assert_equal(mint["b3_recipient"], deposit["recipient"])
        assert_equal(mint["origin_token"], self.token)

        # Determinism: a fresh instance over the same anvil headers runs the
        # tool again and reproduces its stdout byte for byte.
        again = SyntheticBeacon(self.anvil.url, self.beacon.tool,
                                os.path.join(self.options.tmpdir, "beacon_rerun"),
                                REGTEST_GVR, REGTEST_FORK_VERSION, self.slot_offset,
                                self.deployment_block)
        assert_equal(again.bootstrap(), self.bootstrap)
        assert_equal(again.update(k), update)
        outputs = self.tool_outputs(self.beacon.workdir)
        rerun = self.tool_outputs(again.workdir)
        assert_equal(sorted(rerun), sorted(set(rerun) & set(outputs)))
        assert_greater_than_or_equal(len(rerun), 2)
        for name, data in rerun.items():
            assert_equal(data, outputs[name])
        self.evidence.assertion("synthetic beacon tool output is byte-identical on rerun",
                                expected=sorted(outputs), actual=sorted(rerun))
        self.evidence_step("mint plan verified by b3-bridge-ethcheck", self.anvil,
                           bootstrap_bytes=len(plan["bootstrap"]["payload_hex"]) // 2,
                           update_bytes=len(plan["updates"][0]["payload_hex"]) // 2,
                           mint_bytes=len(mint["payload_hex"]) // 2,
                           mint_header_count=mint["header_count"],
                           finalized_slot=update["finalized_slot"],
                           attested_slot=update["attested_slot"],
                           signature_slot=update["signature_slot"])
        return plan

    @staticmethod
    def tool_outputs(workdir):
        outputs = {}
        for name in os.listdir(workdir):
            if name.endswith(".output.json"):
                with open(os.path.join(workdir, name), "rb") as f:
                    outputs[name] = f.read()
        return outputs

    # -- B3 side (submit_and_mine() is B3BridgeChainMixin's) ------------------
    def relay_bootstrap(self, plan):
        self.log.info("Bootstrap carrier: ordering is enforced before it")
        n0 = self.nodes[0]
        self.freeze()
        assert_raises_rpc_error(RPC_VERIFY_REJECTED, "bridge mint precedes light-client bootstrap",
                                n0.claimbridgedeposit, plan["mint"]["payload_hex"], DEPOSIT_RAW,
                                self.recipient)
        assert_raises_rpc_error(RPC_VERIFY_REJECTED, "bridge light-client update precedes bootstrap",
                                n0.submitbridgecarrier, plan["updates"][0]["payload_hex"])
        assert_raises_rpc_error(RPC_INVALID_PARAMETER,
                                "submitbridgecarrier accepts only bootstrap, update, or execution-backfill records",
                                n0.submitbridgecarrier, plan["mint"]["payload_hex"])

        def create():
            result = n0.submitbridgecarrier(plan["bootstrap"]["payload_hex"])
            assert_equal(result["record_kind"], "bootstrap")
            return result

        _, height = self.submit_and_mine("bootstrap carrier mined", create)
        for node in self.bridge_nodes():
            info = node.getbridgeinfo()
            assert_equal(info["light_client_bootstrapped"], True)
            assert_equal(info["light_client_period"], self.bootstrap["period"])
            assert_equal(info["finalized_beacon_slot"], self.bootstrap["checkpoint_slot"])
            assert_equal("0x" + info["finalized_beacon_root"], self.bootstrap["checkpoint_root"])
            assert_equal(info["finalized_execution_block"], self.deployment_block)
            assert_equal("0x" + info["finalized_execution_hash"],
                         self.bootstrap["checkpoint_block_hash"])
            assert_greater_than_or_equal(info["light_client_connected_height"], height)

    def relay_update(self, plan, deposit):
        self.log.info("Update carrier finalizing the deposit block")
        n0 = self.nodes[0]
        self.freeze()
        assert_raises_rpc_error(RPC_VERIFY_REJECTED, "bridge light client is already bootstrapped",
                                n0.submitbridgecarrier, plan["bootstrap"]["payload_hex"])
        assert_raises_rpc_error(RPC_VERIFY_REJECTED, "bridge mint names an unretained execution anchor",
                                n0.claimbridgedeposit, plan["mint"]["payload_hex"], DEPOSIT_RAW,
                                self.recipient)

        def create():
            result = n0.submitbridgecarrier(plan["updates"][0]["payload_hex"])
            assert_equal(result["record_kind"], "update")
            return result

        _, self.update_height = self.submit_and_mine("update carrier mined", create)
        k = deposit["block_number"]
        for node in self.bridge_nodes():
            info = node.getbridgeinfo()
            assert_equal(info["finalized_execution_block"], k)
            assert_equal("0x" + info["finalized_execution_hash"], deposit["block_hash"])
            assert "next_sync_committee_root" in info, "the same-period update carries next"
            anchor = node.getbridgeproofstatus(deposit["block_hash"])["anchor"]
            assert_equal(anchor["known"], True)
            assert_equal(anchor["block_number"], k)
            assert_equal(anchor["connected_height"], self.update_height)

    def claim_mint(self, plan, deposit, info):
        self.log.info("Mint: claimbridgedeposit is validated by B3 consensus on every node")
        n0 = self.nodes[0]
        mint = plan["mint"]["payload_hex"]
        asset_id = info["asset_id"]
        self.freeze()
        for node in self.bridge_nodes():
            assert_equal(self.wallet_asset(node, asset_id)["confirmed"], 0)
        assert_raises_rpc_error(RPC_VERIFY_REJECTED,
                                "bridge mint is not paid to the exact authorized OWNER output",
                                n0.claimbridgedeposit, mint, DEPOSIT_RAW + 1, self.recipient)
        assert_raises_rpc_error(RPC_VERIFY_REJECTED,
                                "bridge mint is not paid to the exact authorized OWNER output",
                                n0.claimbridgedeposit, mint, DEPOSIT_RAW, n0.getnewaddress())

        def create():
            result = n0.claimbridgedeposit(mint, DEPOSIT_RAW, self.recipient)
            assert_equal(result["asset_id"], asset_id)
            assert_equal(result["amount"], DEPOSIT_RAW)
            assert_equal(result["owner_address"], self.recipient)
            assert_equal((result["mint_vout"], result["binding_vout"]), (0, 1))
            return result

        self.mint_txid, self.mint_height = self.submit_and_mine("mint mined", create)

        self.log.info("Every node agrees on the light client, the anchor and the nullifier")
        self.freeze()
        k = deposit["block_number"]
        fields = ("light_client_period", "finalized_beacon_slot", "finalized_beacon_root",
                  "finalized_execution_block", "finalized_execution_hash",
                  "current_sync_committee_root", "next_sync_committee_root",
                  "light_client_connected_height", "light_client_connected_block",
                  "registry_id", "asset_id", "nullifiers")
        views = []
        for node in self.bridge_nodes():
            node_info = node.getbridgeinfo()
            assert_equal(node_info["state_available"], True)
            views.append({key: node_info[key] for key in fields})
            status = node.getbridgeproofstatus(deposit["block_hash"], FIRST_DEPOSIT_ID)
            assert_equal(status["state_available"], True)
            assert_equal(status["anchor"]["known"], True)
            assert_equal(status["anchor"]["block_number"], k)
            assert_equal(status["deposit"]["deposit_id"], FIRST_DEPOSIT_ID)
            assert_equal(status["deposit"]["claimed"], True)
            assert_equal(status["deposit"]["claimed_height"], self.mint_height)
            assert_equal(status["deposit"]["claimed_block"], node.getblockhash(self.mint_height))
            assert_equal(node.getbridgeproofstatus(deposit_id=FIRST_DEPOSIT_ID + 1)["deposit"]["claimed"],
                         False)
        for view in views:
            assert_equal(view, views[0])
        view = views[0]
        assert_equal(view["finalized_execution_block"], k)
        assert_equal("0x" + view["finalized_execution_hash"], deposit["block_hash"])
        assert_equal(view["light_client_period"], self.bootstrap["period"])
        assert_equal(view["light_client_connected_height"], self.update_height)
        assert_equal(view["nullifiers"], 1)

        held = self.wallet_asset(n0, asset_id)
        assert_equal(held["confirmed"], DEPOSIT_RAW)
        assert_equal(held["kind"], "bridge")
        assert_equal([(u["txid"], u["vout"], u["amount"], u["owner_address"]) for u in held["utxos"]],
                     [(self.mint_txid, 0, DEPOSIT_RAW, self.recipient)])
        for node in self.nodes[1:]:
            assert_equal(self.wallet_asset(node, asset_id)["confirmed"], 0)
        self.evidence.assertion("deposit minted on every node", expected=DEPOSIT_RAW,
                                actual=held["confirmed"], mint_txid=self.mint_txid,
                                mint_height=self.mint_height, light_client=view)

    def check_replays(self, plan):
        self.log.info("Replays are refused: mint, mint + 1, update, bootstrap")
        n0, n1 = self.nodes[0], self.nodes[1]
        mint = plan["mint"]["payload_hex"]
        nullified = "bridge deposit is already nullified"
        # Still frozen from claim_mint: the tip cannot change underneath.
        observed = {
            "mint": rpc_refusal(RPC_VERIFY_REJECTED, nullified,
                                n0.claimbridgedeposit, mint, DEPOSIT_RAW, self.recipient),
            # The nullifier is checked before the output, so amount + 1
            # cannot reach a second mint either.
            "mint amount + 1": rpc_refusal(RPC_VERIFY_REJECTED, nullified,
                                           n0.claimbridgedeposit, mint, DEPOSIT_RAW + 1, self.recipient),
            # Another wallet relaying the same proof is refused the same way.
            "mint relayed by n1": rpc_refusal(RPC_VERIFY_REJECTED, nullified,
                                              n1.claimbridgedeposit, mint, DEPOSIT_RAW, self.recipient),
            "update replay": rpc_refusal(RPC_VERIFY_REJECTED, "bridge light-client update is a no-op",
                                         n0.submitbridgecarrier, plan["updates"][0]["payload_hex"]),
            "bootstrap replay": rpc_refusal(RPC_VERIFY_REJECTED, "bridge light client is already bootstrapped",
                                            n0.submitbridgecarrier, plan["bootstrap"]["payload_hex"]),
        }
        for node in self.bridge_nodes():
            assert_equal(node.getbridgeinfo()["nullifiers"], 1)
            assert self.mint_txid not in node.getrawmempool()
        self.evidence.assertion("replays refused",
                                expected={"mint": nullified, "mint amount + 1": nullified,
                                          "mint relayed by n1": nullified,
                                          "update replay": "bridge light-client update is a no-op",
                                          "bootstrap replay": "bridge light client is already bootstrapped"},
                                actual=observed)
        self.evidence_step("RT3a complete", self.anvil)


if __name__ == '__main__':
    B3BridgeRT3V1RouteATest(__file__).main()
