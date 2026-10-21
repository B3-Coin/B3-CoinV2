#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""RT7s: sponsored native-B3 fees for bridge V2 burns (owner ruling R10, OI-8).

REGTEST ONLY, the environment of RT6 (feature_b3_bridge_rt6_route_b_d3.py):
four B3 regtest validators linked only to each other and bound to the
public rt4c-committee test keys, the Ethereum anvil (chain 31337) and the
Arbitrum stand-in anvil (chain 31338) on 127.0.0.1, never forked, the mock
LayerZero endpoints and relayer, forge --offline with its output outside the
source tree and fresh datadirs. A fifth node, n4, is not a validator: it
joins n0 only after the bUSD V2 mint and hosts two wallets, "user" (bUSD V2
only, never any native B3) and "relayer" (a pool of pre-sized native fee
coins). contrib/b3bridge/b3_bridge_sponsor.py runs as a subprocess against
the relayer wallet.

A bUSD-only wallet cannot pay a burn's native network fee. With
bridgewithdraw options.sponsor it returns a partial transaction: bUSD inputs
only, each signed SIGHASH_ALL|ANYONECANPAY, the BURN and BRIDGE_RECORD
binding outputs, a bUSD tip to the relayer and bUSD change, no native
output. The relayer's bridgesponsorburn adds one native fee input and
broadcasts. From the envelope's own sponsored sighash pin
(v2.sponsored_sighash_height, here V, added to RT6's pin file before the
first start) consensus checks a bridge V2 kind 6 burn under
SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL_OUTPUTS (regtest only: mainnet and testnet
set neither the envelope nor the pin), which admits ALL|ANYONECANPAY
because it still commits every output. bridge_v2_gate_tests'
sponsored_sighash_in_mempool_and_blocks shows the same rule refused without
the pin, in the mempool and in blocks.

1. RT6 bring-up. Below V, bridgewithdraw options.sponsor is refused on n0
   (owner ruling R10 needs bridge V2 burns and the sponsored sighash rule),
   as are an unknown sponsor key and a missing tip_amount.
2. RT6 step 2 (fresh checkpoint 266, DeployV2 with forwarder factories,
   initialize, past V, deposits open), then route (b) to n0: deposit, plan,
   carriers and the bUSD V2 mint of DEPOSIT_RAW.
3. n4 starts with the same pins, syncs from n0 and reports the same
   getbridgev2info. n0 sends the user two bUSD V2 coins (DEPOSIT_RAW in
   all) and the relayer native B3; the contrib refill creates four pool
   coins of POOL_VALUE in one sendmany (a second refill is a no-op). The
   user holds DEPOSIT_RAW bUSD V2 and zero native B3 (trusted,
   untrusted_pending and immature).
4. With the full burn gate READY on every node, n4 included: without a
   sponsor the user's bridgewithdraw fails for lack of fee (insufficient
   funds), as before; options.sponsor with broadcast true or fee_rate is
   refused. Route (d1): a sponsored bridgewithdraw to ETH_RECIPIENT returns
   the partial: tip at vout 2, the bUSD inputs locked, every input the
   user's bUSD signed ALL|ANYONECANPAY, no native output; alone it is
   refused only for its zero fee (min relay fee not met). The relayer's
   bridgesponsorburn (hex, min_tip, max_fee, {fee_rate, fee_label})
   refuses min_tip TIP + 1 (below minimum), a partial tipping another
   address (tip missing; its inputs are unlocked again), the partial with
   the tip raised by one unit and the change lowered by one (the user's
   signature on input 0 no longer verifies), the partial with the tip
   lowered by one and the change raised by one (below MIN_TIP; with
   min_tip TIP - 1, the signature check refuses it), the partial with the
   change owner redirected (likewise) and a max_fee above -maxtxfee.
5. Route (d3): a sponsored bridgewithdraw with options.arbitrum, on the
   other bUSD coin, names F_x = exitFactory.accountOf(p); the user publishes
   p. The relayer completes it without broadcasting (fee input = one pool
   coin, network fee = POOL_VALUE). Raising the tip in the completed
   transaction and re-signing the relayer's own input with
   signrawtransactionwithwallet is refused by the mempool
   (script-verify-flag-failed on input 0): the relayer cannot raise its tip
   even with its own input re-signed. The contrib release unlocks the fee
   coin of that unsent completion (unspent, not in the mempool). The
   untampered completion is accepted; a second release finds nothing.
6. The contrib complete refuses a minimum tip above the partial's tip and
   then completes and broadcasts d1 with another pool coin. Both burns are
   mined on every node: getbridgewithdrawals lists the two leaves (d1 pays
   ETH_RECIPIENT; d3 pays F_x with exit block p), each source txid the
   completed one; the relayer holds 2 x TIP bUSD V2 and two pool coins
   fewer; the user holds DEPOSIT_RAW - BURN_D1 - BURN_D3 - 2 x TIP bUSD V2
   and still no native B3; the reserve has burned BURN_D1 + BURN_D3.
7. A certificate covering both burns is relayed; getbridgewithdrawalproofv2
   is identical on every node for both final txids. Vault V2 releases d1 to
   ETH_RECIPIENT (+BURN_D1 USDT); the contrib forward releases d3 to F_x
   and forwards it, and LayerZero delivers BURN_D3 - FORWARD_FEE USDT0 to
   the user on 31338.
8. The contrib status reports the 2 x TIP tips and the R10 note that buying
   native B3 with them needs a bridge V2 B3/bUSD market, which does not
   exist yet. n4 reindexes and replays both sponsored burns: the same
   getbridgewithdrawals, bridge V2 state and wallet balances.
   Evidence: bridge_evidence.json.
"""

from decimal import Decimal
import io

from feature_b3_bridge_rt4a_round_trip import (
    BRIDGE_V2_BURN_HEIGHT,
    BRIDGE_V2_HEIGHT,
    BRING_UP_HEIGHT,
    DEPOSIT_RAW,
    ETH_RECIPIENT,
    INIT_CHECKPOINT,
    RPC_INVALID_PARAMETER,
    RPC_MISC_ERROR,
    RPC_VERIFY_REJECTED,
)
from feature_b3_bridge_rt4c_conflict import COMMITTEE_LABEL
from feature_b3_bridge_rt6_route_b_d3 import (
    EXIT_FEE_CAP,
    FORWARD_FEE,
    NATIVE_MARGIN,
    SIG_X_PUBLISH,
    B3BridgeRT6RouteBD3Test,
    word,
)
from test_framework.b3bridge import (
    LZ_EID_ARBITRUM,
    REGTEST_ETH_CHAIN_ID,
    VALIDATOR_COUNT,
    VAULT_V2_RELEASE_SIGNATURE,
    ExitParams,
    SponsorToolError,
    b3_bridge_args,
    call_uint,
    encode_vault_v2_release,
    enc_uint,
    exit_forwarder_address,
    file_sha256,
    forwarders_v2,
    gate_view,
    hx,
    leaf_v2,
    pin_sponsored_sighash_v2,
    rpc_refusal,
    run_forwarder_tool,
    run_sponsor_tool,
    to_0x,
    withdrawal_root,
    withdrawal_root_and_path,
)
from test_framework.messages import deser_compact_size, ser_compact_size
from test_framework.script import OP_DROP
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
)

RPC_WALLET_INSUFFICIENT_FUNDS = -6

# -- n4 and its wallets ----------------------------------------------------------
SPONSOR_NODE = 4
USER_WALLET = "user"
RELAYER_WALLET = "relayer"
TIP_LABEL = "b3-sponsor-tip"
# The contrib refill's pool label (b3_bridge_sponsor.DEFAULT_LABEL).
POOL_LABEL = "b3-sponsor-fee"

# -- amounts --------------------------------------------------------------------------
# Raw six-decimal bUSD V2 units.
BURN_D1 = 8_000_000
BURN_D3 = 8_000_000
TIP = MIN_TIP = 50_000
# The user's two bUSD V2 coins: each sponsored burn spends one of them, so
# both partials exist side by side without sharing an input.
USER_COINS = (DEPOSIT_RAW // 2, DEPOSIT_RAW - DEPOSIT_RAW // 2)
assert all(coin >= max(BURN_D1, BURN_D3) + TIP for coin in USER_COINS)
assert BURN_D3 > EXIT_FEE_CAP >= FORWARD_FEE
# Native B3 (1 B3 = 1e9 atomic units). The fee rate is atomic units per vB.
FEE_RATE = Decimal(25)
# A bridge record declares the whole per-transaction payload cost
# (modern::BRIDGE_VERIFY_COST = MAX_TX_PAYLOAD_COST = 12,000 units at
# PAYLOAD_COST_TO_VBYTES = 1), so every bridge V2 burn relays at a virtual
# size of at least 12,000 vB whatever its byte weight (policy.h).
BRIDGE_BURN_VSIZE = 12_000
# About twice FEE_RATE x BRIDGE_BURN_VSIZE: a pool coin covers a completed
# burn with room, and stays far below -maxtxfee (0.1 B3).
POOL_VALUE = Decimal("0.0006")
assert POOL_VALUE * 10**9 >= 2 * FEE_RATE * BRIDGE_BURN_VSIZE
POOL_COUNT = 4
RELAYER_FUNDING = Decimal(1)

# -- R10 refusals ------------------------------------------------------------------------
SPONSOR_BELOW_V = ("options.sponsor needs bridge V2 burns and the sponsored sighash rule for the next block "
                   "(owner ruling R10)")
# Owner ruling R10's own pin (v2.sponsored_sighash_height): at V here, added
# to RT6's pin file before the validators first start.
SPONSORED_SIGHASH_HEIGHT = BRIDGE_V2_HEIGHT
SPONSOR_BROADCAST = ("options.sponsor returns a partial transaction for the relayer to complete; "
                     "broadcast must be false or omitted")
SPONSOR_FEE_RATE = "options.fee_rate does not apply with options.sponsor: the relayer's fee input pays the fee"
SPONSOR_SIGNATURE_INVALID = ("an output changed after the user signed, or the input is not signed "
                             "SIGHASH_ALL|ANYONECANPAY")
# The wallet's coin selection (no native coin can pay the fee): the burn
# without a sponsor fails exactly as it did before R10.
NO_NATIVE_FEE = "The total exceeds your balance when the"
# The partial alone pays no native fee (MemPoolAccept::CheckFeeRate).
PARTIAL_ALONE_REFUSAL = "min relay fee not met"
MARKET_NOTE_FRAGMENT = "bridge V2 B3/bUSD FlowMesh market, which does not exist yet"

# -- B3A1 asset outputs (src/modern/asset_output.h) ---------------------------------------
# PUSH(payload) OP_DROP <owner script>, payload = "B3A1" || asset[32] ||
# amount_u64_be || policy_type_u16_be || policy_version_u16_be.
ASSET_PAYLOAD_SIZE = 48
ASSET_AMOUNT_OFFSET = 1 + 4 + 32  # after the push opcode, the magic and the asset
ASSET_OWNER_OFFSET = 1 + ASSET_PAYLOAD_SIZE + 1  # after the payload push and OP_DROP


def asset_output_fields(script):
    """(asset bytes as stored, amount, owner script) of an OWNER asset output."""
    assert_equal(script[0], ASSET_PAYLOAD_SIZE)
    assert_equal(script[1:5], b"B3A1")
    assert_equal(script[ASSET_OWNER_OFFSET - 1], int(OP_DROP))
    amount = int.from_bytes(script[ASSET_AMOUNT_OFFSET:ASSET_AMOUNT_OFFSET + 8], "big")
    return script[5:ASSET_AMOUNT_OFFSET], amount, script[ASSET_OWNER_OFFSET:]


def asset_output_script(script, amount=None, owner=None):
    """The same OWNER asset output with its payload amount and/or its owner
    script replaced; the length is unchanged."""
    asset_output_fields(script)
    out = bytearray(script)
    if amount is not None:
        out[ASSET_AMOUNT_OFFSET:ASSET_AMOUNT_OFFSET + 8] = amount.to_bytes(8, "big")
    if owner is not None:
        assert_equal(len(owner), len(script) - ASSET_OWNER_OFFSET)
        out[ASSET_OWNER_OFFSET:] = owner
    return bytes(out)


def replace_once(tx_hex, replacements):
    """tx_hex with each {old bytes: new bytes} replaced; each old must occur
    exactly once and keep its length, so every length prefix stays valid."""
    raw = bytes.fromhex(tx_hex)
    for old, new in replacements.items():
        assert_equal(len(new), len(old))
        assert_equal(raw.count(old), 1)
        raw = raw.replace(old, new)
    return raw.hex()


def with_script_sig(tx_hex, txid, vout, script_sig):
    """tx_hex with the scriptSig of the input spending txid:vout replaced."""
    raw = bytes.fromhex(tx_hex)
    outpoint = bytes.fromhex(txid)[::-1] + vout.to_bytes(4, "little")
    assert_equal(raw.count(outpoint), 1)
    start = raw.index(outpoint) + len(outpoint)
    stream = io.BytesIO(raw[start:])
    length = deser_compact_size(stream)
    end = start + stream.tell() + length
    return (raw[:start] + ser_compact_size(len(script_sig)) + script_sig + raw[end:]).hex()


class B3BridgeRT7sSponsoredFeesTest(B3BridgeRT6RouteBD3Test):
    BOOTSTRAP_MODE = 1  # START_CERTIFICATE, as RT4a, RT4c and RT6

    def set_test_params(self):
        # RT6's parameters plus n4: added (not started) by
        # start_bridge_validators, never a validator, so the staking and
        # finality helpers leave it alone.
        super().set_test_params()
        self.num_nodes = VALIDATOR_COUNT + 1
        assert_equal(SPONSOR_NODE, VALIDATOR_COUNT)

    def start_bridge_validators(self, pins_path=None, extra_args=()):
        # RT6 writes its pin file just before this first start. R10's
        # sponsored sighash rule has a pin of its own; without it the node
        # keeps every V2 burn at SIGHASH_ALL.
        if pins_path is not None:
            self.pins = pin_sponsored_sighash_v2(pins_path, SPONSORED_SIGHASH_HEIGHT)
            assert_equal(self.pins["v2"]["sponsored_sighash_height"], SPONSORED_SIGHASH_HEIGHT)
            self.evidence.step("R10: v2.sponsored_sighash_height pinned before the first start",
                               sponsored_sighash_height=SPONSORED_SIGHASH_HEIGHT,
                               pins_sha256=file_sha256(pins_path))
        super().start_bridge_validators(pins_path, extra_args)

    # -- run ----------------------------------------------------------------------------
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
        deposit = self.make_deposit()  # route (b), minted to n0
        plan = self.build_plan(deposit)
        self.relay_carriers(plan, deposit)
        self.claim_mint(plan, deposit)

        self.start_sponsor_node()
        self.fund_user_and_relayer()

        d1 = self.sponsored_burn_d1()
        d3 = self.sponsored_burn_d3()
        burns = self.complete_and_mine(d1, d3)

        covering = self.relay_withdrawal_root()
        self.release_and_forward_sponsored(burns, covering)
        self.check_status_and_reindex(burns)
        gossip = self.assert_fresh_gossip(self.bridge_validators())
        self.evidence_step("RT7s complete", self.anvil, fresh_votes=dict(self.fresh_vote_results),
                           fresh_vote_transport=self.fresh_vote_transport, finsigv2_bytes_received=gossip,
                           relayed=len(self.relayed), arbitrum_block=self.arb.block_number(),
                           burns={route: {key: burn[key] for key in ("txid", "height", "withdrawal_id")}
                                  for route, burn in burns.items()})

    # -- 1. below V -------------------------------------------------------------------------
    def check_v2_info_before_activation(self):
        info = super().check_v2_info_before_activation()
        self.log.info("(1) R10: bridgewithdraw options.sponsor is refused below V")
        n0 = self.nodes[0]
        sponsor = {"tip_address": n0.getnewaddress(), "tip_amount": TIP}
        refusals = {
            "options.sponsor below V": rpc_refusal(
                RPC_MISC_ERROR, SPONSOR_BELOW_V, n0.bridgewithdraw, BURN_D1, ETH_RECIPIENT, {"sponsor": sponsor}),
            "unknown sponsor option": rpc_refusal(
                RPC_INVALID_PARAMETER, "Unknown sponsor option 'memo'", n0.bridgewithdraw, BURN_D1, ETH_RECIPIENT,
                {"sponsor": {**sponsor, "memo": "x"}}),
            "missing tip_amount": rpc_refusal(
                RPC_INVALID_PARAMETER, "options.sponsor.tip_amount is required", n0.bridgewithdraw, BURN_D1,
                ETH_RECIPIENT, {"sponsor": {"tip_address": sponsor["tip_address"]}}),
        }
        assert_equal(refusals["options.sponsor below V"], SPONSOR_BELOW_V)
        self.evidence.assertion("R10: options.sponsor refused below V", expected=SPONSOR_BELOW_V,
                                actual=refusals["options.sponsor below V"], refusals=refusals,
                                next_height=n0.getblockcount() + 1, bridge_v2_height=BRIDGE_V2_HEIGHT)
        return info

    # -- 3. n4: user and relayer -----------------------------------------------------------
    def start_sponsor_node(self):
        self.log.info("(3) n4 (not a validator) starts with the same pins and syncs from n0")
        self.freeze()
        n0, n4 = self.nodes[0], self.nodes[SPONSOR_NODE]
        assert not n4.running
        self.start_node(SPONSOR_NODE, extra_args=b3_bridge_args(self.pins_path) + self.fresh_args())
        if getattr(self, "mock_time", 0):
            n4.setmocktime(self.mock_time)
        self.connect_nodes(SPONSOR_NODE, 0)
        self.sync_bridge_blocks(timeout=300)
        n4.syncwithvalidationinterfacequeue()
        assert n4 in self.bridge_nodes() and n4 not in self.bridge_validators()
        # v2_infos() compares every running node, n4 included.
        info = self.v2_infos()
        assert_equal(gate_view(n4.getbridgev2info()), gate_view(n0.getbridgev2info()))
        n4.createwallet(USER_WALLET)
        n4.createwallet(RELAYER_WALLET)
        self.user_wallet = n4.get_wallet_rpc(USER_WALLET)
        self.relayer_wallet = n4.get_wallet_rpc(RELAYER_WALLET)
        self.evidence_step("n4 synced: same bridge V2 state as n0; wallets user and relayer", self.anvil,
                           n4_height=n4.getblockcount(), asset_id=info["asset_id"], wallets=n4.listwallets())

    def native_balances(self, wallet):
        mine = wallet.getbalances()["mine"]
        return {key: mine[key] for key in ("trusted", "untrusted_pending", "immature")}

    def pool_status(self):
        return run_sponsor_tool("status", self.nodes[SPONSOR_NODE], RELAYER_WALLET, value=POOL_VALUE)

    def fund_user_and_relayer(self):
        self.log.info("(3) n0 sends the user two bUSD V2 coins and the relayer native B3; contrib refill")
        n0, n4 = self.nodes[0], self.nodes[SPONSOR_NODE]
        user, relayer = self.user_wallet, self.relayer_wallet
        asset_id = self.v2info["asset_id"]
        self.freeze()
        user_addresses = [user.getnewaddress("bridge-user") for _ in USER_COINS]
        funding_address = relayer.getnewaddress("relayer-funding")
        self.tip_address = relayer.getnewaddress(TIP_LABEL)
        first_height = n0.getblockcount() + 1
        txids = []
        for index, (address, amount) in enumerate(zip(user_addresses, USER_COINS)):
            # The second send spends n0's own unconfirmed bUSD change.
            sent = n0.sendasset(asset_id, amount, address, {"minconf": 0} if index else {})
            assert_equal((sent["asset_id"], sent["amount"], sent["owner_address"]), (asset_id, amount, address))
            txids.append(sent["txid"])
        txids.append(n0.sendtoaddress(funding_address, RELAYER_FUNDING))
        heights = self.mine_txids(txids, first_height)

        refill = run_sponsor_tool("refill", n4, RELAYER_WALLET, count=POOL_COUNT, value=POOL_VALUE)
        assert_equal((refill["pool_before"], refill["created"], refill["remaining_deficit"]), (0, POOL_COUNT, 0))
        assert_equal((Decimal(refill["value"]), len(refill["addresses"])), (POOL_VALUE, POOL_COUNT))
        for address in refill["addresses"]:
            info = relayer.getaddressinfo(address)
            assert_equal((info["labels"], info["ismine"]), ([POOL_LABEL], True))
            assert hx(info["scriptPubKey"]).startswith(bytes.fromhex("76a914")), info  # P2PKH
        # The pending refill counts (minconf 0): a second run sends nothing.
        again = run_sponsor_tool("refill", n4, RELAYER_WALLET, count=POOL_COUNT, value=POOL_VALUE)
        assert_equal((again["pool_before"], again["created"], again["txid"]), (POOL_COUNT, 0, None))
        heights += self.mine_txids([refill["txid"]], n0.getblockcount() + 1)

        pool = self.pool_status()["pool"]
        assert_equal((pool["coins"], pool["confirmed"], Decimal(pool["total"])),
                     (POOL_COUNT, POOL_COUNT, POOL_COUNT * POOL_VALUE))
        held = self.wallet_asset(user, asset_id)
        assert_equal(held["confirmed"], DEPOSIT_RAW)
        assert_equal(sorted(utxo["amount"] for utxo in held["utxos"]), sorted(USER_COINS))
        assert_equal(self.native_balances(user), {"trusted": 0, "untrusted_pending": 0, "immature": 0})
        assert_equal(user.listunspent(0), [])
        assert_equal(self.wallet_asset(relayer, asset_id)["confirmed"], 0)
        # Each completion spends exactly one pool coin of the relayer's native B3.
        self.relayer_native_funded = self.native_balances(relayer)
        self.evidence_step("user holds only bUSD V2; relayer fee pool refilled", self.anvil, funding=txids,
                           mined_heights=heights, refill=refill, refill_again=again, pool=pool,
                           user_busd=held["confirmed"], user_native=self.native_balances(user),
                           tip_address=self.tip_address)

    # -- 4. route (d1) and the relayer's refusals ----------------------------------------------
    def sponsor(self, tip_address=None, tip_amount=TIP):
        return {"tip_address": tip_address or self.tip_address, "tip_amount": tip_amount}

    def check_partial(self, built, amount, route, coins):
        """A sponsored partial: bUSD inputs of `coins` only, all ALL|ANYONECANPAY
        and locked, outputs [burn, binding, tip, change] with no native value."""
        n4, user = self.nodes[SPONSOR_NODE], self.user_wallet
        asset_id = self.v2info["asset_id"]
        assert_equal((built["sponsored"], built["route"], built["amount"]), (True, route, amount))
        assert_equal((built["withdrawal_mode"], built["withdrawal_status"]),
                     ("decentralized-verifier-v2", "awaiting-sponsor"))
        assert_equal((built["burn_vout"], built["binding_vout"]), (0, 1))
        assert_equal(built["sponsor"], {"tip_address": self.tip_address, "tip_amount": TIP, "tip_vout": 2,
                                        "sighash": "ALL|ANYONECANPAY"})
        assert "txid" not in built and "proof_rpc_after_confirmation" not in built, built
        decoded = n4.decoderawtransaction(built["hex"])
        inputs = [(vin["txid"], vin["vout"]) for vin in decoded["vin"]]
        assert_equal(sorted(inputs), sorted((entry["txid"], entry["vout"]) for entry in built["locked_inputs"]))
        for vin in decoded["vin"]:
            assert (vin["txid"], vin["vout"]) in coins, (vin, coins)
            assert "[ALL|ANYONECANPAY]" in vin["scriptSig"]["asm"], vin["scriptSig"]
        selected = sum(coins[outpoint] for outpoint in inputs)
        assert_equal(built["asset_change"], selected - amount - TIP)
        assert_greater_than(built["asset_change"], 0)
        assert_equal(len(decoded["vout"]), 4)
        assert all(Decimal(vout["value"]) == 0 for vout in decoded["vout"]), decoded["vout"]
        scripts = [bytes.fromhex(vout["scriptPubKey"]["hex"]) for vout in decoded["vout"]]
        tip_asset, tip_amount, tip_owner = asset_output_fields(scripts[2])
        change_asset, change_amount, change_owner = asset_output_fields(scripts[3])
        assert_equal(tip_asset, bytes.fromhex(asset_id)[::-1])
        assert_equal(change_asset, tip_asset)
        assert_equal((tip_amount, change_amount), (TIP, built["asset_change"]))
        assert_equal(tip_owner.hex(), self.relayer_wallet.getaddressinfo(self.tip_address)["scriptPubKey"])
        assert user.getaddressinfo(user.decodescript(change_owner.hex())["address"])["ismine"]
        locked = {(entry["txid"], entry["vout"]) for entry in user.listlockunspent()}
        assert set(inputs) <= locked, (inputs, locked)
        return {"decoded": decoded, "inputs": inputs, "tip_script": scripts[2], "change_script": scripts[3]}

    def user_coins(self):
        held = self.wallet_asset(self.user_wallet, self.v2info["asset_id"])
        return {(utxo["txid"], utxo["vout"]): utxo["amount"] for utxo in held["utxos"]}

    def sponsored_burn_d1(self):
        info = self.prepare_burn_gate()
        self.log.info("(4) Gate READY on n4 too; without a sponsor the bUSD-only burn fails for lack of fee")
        n0, n4 = self.nodes[0], self.nodes[SPONSOR_NODE]
        user, relayer = self.user_wallet, self.relayer_wallet
        self.freeze()
        assert_equal((info["active"], info["burns_active"]), (True, True))
        assert_equal(gate_view(n4.getbridgev2info())["burn_gate"]["status"], "READY")
        reserve = info["reserve"]
        assert_equal({key: reserve[key] for key in ("minted", "burned", "reserve")},
                     {"minted": DEPOSIT_RAW, "burned": 0, "reserve": DEPOSIT_RAW})
        coins = self.user_coins()
        refusals = {
            "no sponsor: no native fee": rpc_refusal(
                RPC_WALLET_INSUFFICIENT_FUNDS, NO_NATIVE_FEE, user.bridgewithdraw, BURN_D1, ETH_RECIPIENT),
            "sponsor with broadcast true": rpc_refusal(
                RPC_INVALID_PARAMETER, SPONSOR_BROADCAST, user.bridgewithdraw, BURN_D1, ETH_RECIPIENT,
                {"sponsor": self.sponsor(), "broadcast": True}),
            "sponsor with fee_rate": rpc_refusal(
                RPC_INVALID_PARAMETER, SPONSOR_FEE_RATE, user.bridgewithdraw, BURN_D1, ETH_RECIPIENT,
                {"sponsor": self.sponsor(), "fee_rate": 10}),
        }
        assert_equal(user.listlockunspent(), [])
        assert_equal(self.user_coins(), coins)

        self.log.info("(4) Route (d1): the sponsored bridgewithdraw returns a signed partial, unbroadcast")
        d1 = user.bridgewithdraw(BURN_D1, ETH_RECIPIENT, {"sponsor": self.sponsor()})
        assert_equal(hx(d1["ethereum_recipient"]), hx(ETH_RECIPIENT))
        assert "exit" not in d1
        partial = self.check_partial(d1, BURN_D1, "ethereum", coins)
        mempool = n4.getrawmempool()
        alone = n4.testmempoolaccept([d1["hex"]])[0]
        # Refused for its zero fee and nothing else: every check before the
        # fee rate (structure, bridge V2 entry, gate and reserve, assets)
        # admitted it.
        assert_equal((alone["allowed"], alone["reject-reason"]), (False, PARTIAL_ALONE_REFUSAL))
        assert_equal(n4.getrawmempool(), mempool)

        self.log.info("(4) bridgesponsorburn refuses a low minimum, a foreign tip and changed outputs")
        options = {"fee_rate": FEE_RATE, "fee_label": POOL_LABEL}
        other = user.bridgewithdraw(BURN_D1, ETH_RECIPIENT, {"sponsor": self.sponsor(n0.getnewaddress())})
        other_inputs = [(entry["txid"], entry["vout"]) for entry in other["locked_inputs"]]
        assert not set(other_inputs) & set(partial["inputs"]), (other_inputs, partial["inputs"])
        tip_script, change_script = partial["tip_script"], partial["change_script"]
        raised = replace_once(d1["hex"], {
            tip_script: asset_output_script(tip_script, amount=TIP + 1),
            change_script: asset_output_script(change_script, amount=d1["asset_change"] - 1),
        })
        lowered = replace_once(d1["hex"], {
            tip_script: asset_output_script(tip_script, amount=TIP - 1),
            change_script: asset_output_script(change_script, amount=d1["asset_change"] + 1),
        })
        foreign_owner = bytes.fromhex(n0.getaddressinfo(n0.getnewaddress())["scriptPubKey"])
        redirected = replace_once(d1["hex"], {
            change_script: asset_output_script(change_script, owner=foreign_owner),
        })
        relayer_refusals = {
            "min_tip above the tip": rpc_refusal(
                RPC_VERIFY_REJECTED, f"sponsor-tip-below-minimum: tip {TIP} < {TIP + 1}",
                relayer.bridgesponsorburn, d1["hex"], TIP + 1, POOL_VALUE, options),
            "tip to another address": rpc_refusal(
                RPC_VERIFY_REJECTED, "sponsor-tip-missing", relayer.bridgesponsorburn, other["hex"], MIN_TIP,
                POOL_VALUE, options),
            "tip raised, change lowered": rpc_refusal(
                RPC_VERIFY_REJECTED, "sponsor-signature-invalid: input 0", relayer.bridgesponsorburn, raised, MIN_TIP,
                POOL_VALUE, options),
            "tip lowered below the minimum": rpc_refusal(
                RPC_VERIFY_REJECTED, f"sponsor-tip-below-minimum: tip {TIP - 1} < {MIN_TIP}",
                relayer.bridgesponsorburn, lowered, MIN_TIP, POOL_VALUE, options),
            # With a minimum the lowered tip meets, the signature check is
            # what refuses it.
            "tip lowered, change raised": rpc_refusal(
                RPC_VERIFY_REJECTED, "sponsor-signature-invalid: input 0", relayer.bridgesponsorburn, lowered,
                TIP - 1, POOL_VALUE, options),
            "change owner redirected": rpc_refusal(
                RPC_VERIFY_REJECTED, "sponsor-signature-invalid: input 0", relayer.bridgesponsorburn, redirected,
                MIN_TIP, POOL_VALUE, options),
            # max_fee is required and bounded by -maxtxfee.
            "max_fee above -maxtxfee": rpc_refusal(
                RPC_INVALID_PARAMETER, "max_fee 0.20 exceeds -maxtxfee 0.10",
                relayer.bridgesponsorburn, d1["hex"], MIN_TIP, Decimal("0.2"), options),
        }
        for key in ("tip raised, change lowered", "tip lowered, change raised", "change owner redirected"):
            assert SPONSOR_SIGNATURE_INVALID in relayer_refusals[key], relayer_refusals[key]
        # Refused completions lock and send nothing.
        assert_equal(relayer.listlockunspent(), [])
        assert_equal(self.pool_status()["pool"]["coins"], POOL_COUNT)
        assert_equal(n4.getrawmempool(), mempool)
        user.lockunspent(True, other["locked_inputs"])
        locked = {(entry["txid"], entry["vout"]) for entry in user.listlockunspent()}
        assert_equal(locked, set(partial["inputs"]))
        self.evidence.assertion("route (d1) sponsored partial; no-sponsor burn and tampered partials refused",
                                expected={"tip_vout": 2, "tip": TIP, "native_outputs": 0},
                                actual={"tip_vout": d1["sponsor"]["tip_vout"], "tip": d1["sponsor"]["tip_amount"],
                                        "native_outputs": sum(1 for vout in partial["decoded"]["vout"]
                                                              if Decimal(vout["value"]) != 0)},
                                partial_inputs=partial["inputs"], partial_alone=alone, refusals=refusals,
                                relayer_refusals=relayer_refusals, burn_gate=info["burn_gate"])
        d1["partial"] = partial
        self.d1_partial_inputs = partial["inputs"]
        return d1

    # -- 5. route (d3): completion without broadcast, tamper after re-signing ------------------
    def sponsored_burn_d3(self):
        self.log.info("(5) Route (d3): a sponsored bridgewithdraw with options.arbitrum on the other bUSD coin")
        anvil, n4 = self.anvil, self.nodes[SPONSOR_NODE]
        user, relayer = self.user_wallet, self.relayer_wallet
        self.freeze()
        coins = self.user_coins()
        d3 = user.bridgewithdraw(BURN_D3, None, {"arbitrum": self.arbitrum_route(), "sponsor": self.sponsor()})
        exit_block = d3["exit"]
        p = ExitParams.from_json(exit_block["params"])
        expected_p = ExitParams(LZ_EID_ARBITRUM, word(self.user_eoa()), 0, EXIT_FEE_CAP, self.refund_eoa)
        assert p == expected_p, (p.to_json(), expected_p.to_json())
        account = to_0x(exit_forwarder_address(self.exit_factory, self.init_code_hash, p))
        assert_equal(forwarders_v2.view(self.eth, self.exit_factory, forwarders_v2.SIG_X_ACCOUNT_OF, p.abi(),
                                        returns="address"), account)
        assert_equal((hx(exit_block["account"]), hx(d3["ethereum_recipient"])), (hx(account), hx(account)))
        assert_equal(anvil.code(account), b"")
        partial = self.check_partial(d3, BURN_D3, "arbitrum", coins)
        assert not set(partial["inputs"]) & set(self.d1_partial_inputs), "the two partials share an input"
        published = anvil.send(self.user_eoa(), self.exit_factory, forwarders_v2.encode_call(SIG_X_PUBLISH, p.abi()))
        logs = [log for log in published["logs"] if hx(log["address"]) == hx(self.exit_factory)]
        assert_equal(len(logs), 1)
        kind, logged_account, logged_p = forwarders_v2.decode_published(logs[0])
        assert_equal((kind, to_0x(logged_account)), ("exit", account))
        assert logged_p == p

        self.log.info("(5) The relayer completes d3 without broadcasting: one pool coin is the whole fee")
        mempool = n4.getrawmempool()
        completed = relayer.bridgesponsorburn(d3["hex"], MIN_TIP, POOL_VALUE,
                                              {"fee_rate": FEE_RATE, "fee_label": POOL_LABEL, "broadcast": False})
        self.check_completion(completed, d3, BURN_D3, "arbitrum", account, broadcast=False)
        assert_equal(n4.getrawmempool(), mempool)
        fee_input = completed["fee_input"]
        assert_equal(relayer.listlockunspent(), [{"txid": fee_input["txid"], "vout": fee_input["vout"]}])

        self.log.info("(5) Tip raised in the completed burn, relayer input re-signed: the mempool refuses it")
        decoded = n4.decoderawtransaction(completed["hex"])
        scripts = [bytes.fromhex(vout["scriptPubKey"]["hex"]) for vout in decoded["vout"]]
        assert_equal((scripts[2], scripts[3]), (partial["tip_script"], partial["change_script"]))
        tampered = replace_once(completed["hex"], {
            scripts[2]: asset_output_script(scripts[2], amount=TIP + 1),
            scripts[3]: asset_output_script(scripts[3], amount=d3["asset_change"] - 1),
        })
        resigned = relayer.signrawtransactionwithwallet(tampered)
        assert_equal(resigned["complete"], False)
        # Only the user's inputs fail: the relayer's own input is signed anew.
        user_outpoints = set(partial["inputs"])
        assert_equal({(error["txid"], error["vout"]) for error in resigned["errors"]}, user_outpoints)
        fee_index = len(decoded["vin"]) - 1
        assert_equal((decoded["vin"][fee_index]["txid"], decoded["vin"][fee_index]["vout"]),
                     (fee_input["txid"], fee_input["vout"]))
        fee_sig = bytes.fromhex(n4.decoderawtransaction(resigned["hex"])["vin"][fee_index]["scriptSig"]["hex"])
        assert fee_sig != bytes.fromhex(decoded["vin"][fee_index]["scriptSig"]["hex"])
        # The user's signatures as they were, the relayer's fresh one: only
        # the tip and change amounts differ from the completion.
        attempt = with_script_sig(tampered, fee_input["txid"], fee_input["vout"], fee_sig)
        refused = rpc_refusal(RPC_VERIFY_REJECTED, "script-verify-flag-failed", n4.sendrawtransaction, attempt)
        assert "input 0 of" in refused, refused
        assert_equal(n4.testmempoolaccept([attempt])[0]["allowed"], False)
        assert_equal(n4.getrawmempool(), mempool)

        self.log.info("(5) contrib release unlocks the fee coin of the unsent completion only")
        # The fee coin is unspent and not in the mempool: release unlocks it.
        # The user's locked bUSD inputs live in another wallet.
        released = run_sponsor_tool("release", n4, RELAYER_WALLET)
        assert_equal(released["released"], [{"txid": fee_input["txid"], "vout": fee_input["vout"]}])
        assert_equal(released["kept"], [])
        assert_equal(relayer.listlockunspent(), [])

        self.log.info("(5) The untampered completion is accepted")
        txid = n4.sendrawtransaction(completed["hex"])
        assert_equal(txid, completed["txid"])
        assert txid in n4.getrawmempool()
        # Its fee coin is now spent in the mempool: nothing is stale.
        again = run_sponsor_tool("release", n4, RELAYER_WALLET)
        assert_equal((again["released"], again["kept"]), ([], []))
        self.evidence.assertion("route (d3) sponsored: completed without broadcast; raised tip refused in the "
                                "mempool after the relayer re-signed its input; release unlocks the unsent "
                                "completion's fee coin",
                                expected={"network_fee": POOL_VALUE, "refusal": "script-verify-flag-failed"},
                                actual={"network_fee": completed["network_fee"], "refusal": refused},
                                completed=completed, exit=exit_block, f_x=account, released=released,
                                published_tx=published["transactionHash"], resign_errors=resigned["errors"])
        d3.update({"partial": partial, "completed": completed, "exit_p": p, "account": account})
        return d3

    def user_eoa(self):
        """Anvil account 1: the user on both chains and the refund EOA (RT6)."""
        return self.depositor

    def check_completion(self, completed, built, amount, route, recipient, broadcast):
        """bridgesponsorburn's result for the partial `built`."""
        assert_equal(completed["broadcast"], broadcast)
        assert_equal(Decimal(completed["network_fee"]), POOL_VALUE)
        assert_equal(Decimal(completed["fee_input"]["amount"]), POOL_VALUE)
        assert_equal(completed["tip"], {"vout": 2, "amount": TIP})
        burn = completed["burn"]
        assert_equal((burn["amount"], burn["route"], burn["burn_vout"], burn["binding_vout"]), (amount, route, 0, 1))
        assert_equal(hx(burn["ethereum_recipient"]), hx(recipient))
        assert_equal(completed["proof_rpc_after_confirmation"],
                     f"getbridgewithdrawalproofv2 {completed['txid']} 0 <certificate_block>")
        n4 = self.nodes[SPONSOR_NODE]
        partial = n4.decoderawtransaction(built["hex"])
        final = n4.decoderawtransaction(completed["hex"])
        assert_equal(final["txid"], completed["txid"])
        assert_equal(final["vout"], partial["vout"])
        assert_equal(final["vin"][:-1], partial["vin"])
        fee_vin = final["vin"][-1]
        assert_equal((fee_vin["txid"], fee_vin["vout"]),
                     (completed["fee_input"]["txid"], completed["fee_input"]["vout"]))
        assert "[ALL]" in fee_vin["scriptSig"]["asm"], fee_vin["scriptSig"]
        # The relay vsize is the bridge record's payload-cost floor, far
        # above the byte weight decoderawtransaction reports.
        assert_equal(completed["vsize"], BRIDGE_BURN_VSIZE)
        assert_greater_than(completed["vsize"], final["vsize"])
        assert_greater_than_or_equal(POOL_VALUE * 10**9, FEE_RATE * completed["vsize"])

    # -- 6. completion of d1 by the contrib tool; both mined --------------------------------------
    def complete_and_mine(self, d1, d3):
        self.log.info("(6) contrib complete refuses a minimum above the tip, then completes and broadcasts d1")
        n0, n4 = self.nodes[0], self.nodes[SPONSOR_NODE]
        user, relayer = self.user_wallet, self.relayer_wallet
        asset_id = self.v2info["asset_id"]
        try:
            run_sponsor_tool("complete", n4, RELAYER_WALLET, hex=d1["hex"], min_tip=TIP + 1, fee_rate=FEE_RATE,
                             max_fee=POOL_VALUE)
        except SponsorToolError as e:
            tool_refusal = e.message
        else:
            raise AssertionError("the contrib complete accepted a tip below its minimum")
        assert tool_refusal.startswith("bridgesponsorburn refused the partial: sponsor-tip-below-minimum"), \
            tool_refusal
        completed = run_sponsor_tool("complete", n4, RELAYER_WALLET, hex=d1["hex"], min_tip=MIN_TIP,
                                     fee_rate=FEE_RATE, max_fee=POOL_VALUE)
        assert_equal(completed["wallet"], RELAYER_WALLET)
        self.check_completion(completed, d1, BURN_D1, "ethereum", ETH_RECIPIENT, broadcast=True)
        assert completed["fee_input"]["txid"] != d3["completed"]["fee_input"]["txid"] or \
            completed["fee_input"]["vout"] != d3["completed"]["fee_input"]["vout"]
        assert completed["txid"] in n4.getrawmempool()
        d1["completed"] = completed

        self.log.info("(6) Both sponsored burns are mined on every node, n4 included")
        reserve_before = self.v2_infos()["reserve"]
        first_height = n0.getblockcount() + 1
        txids = [d1["completed"]["txid"], d3["completed"]["txid"]]
        heights = self.mine_txids(txids, first_height)
        for height in heights:
            assert_greater_than_or_equal(height, BRIDGE_V2_BURN_HEIGHT)
        for node in self.bridge_nodes():
            for txid, height in zip(txids, heights):
                assert txid in node.getblock(node.getblockhash(height))["tx"], (node.index, txid)

        listings = [node.getbridgewithdrawals() for node in self.bridge_nodes()]
        for listing in listings[1:]:
            assert_equal(listing, listings[0])
        listing = listings[0]
        assert_equal((listing["withdrawals_total"], len(listing["withdrawals"])), (2, 2))
        by_txid = {entry["source_txid"]: entry for entry in listing["withdrawals"]}
        assert_equal(sorted(by_txid), sorted(txids))
        burns = {}
        for route, built, txid, height, recipient, amount in (
                ("d1", d1, txids[0], heights[0], ETH_RECIPIENT, BURN_D1),
                ("d3", d3, txids[1], heights[1], d3["account"], BURN_D3)):
            entry = by_txid[txid]
            assert_equal((entry["burn_output_index"], entry["b3_height"], entry["entry"], entry["sweep"],
                          entry["amount_raw"]), (0, height, "launch", False, amount))
            assert_equal(entry["b3_block"], n0.getblockhash(height))
            for key, value in (("vault", self.vault), ("token", self.token), ("recipient", recipient)):
                assert_equal(hx(entry[key]), hx(value))
            leaf = leaf_v2(self.asset_id_raw, entry["withdrawal_id"], REGTEST_ETH_CHAIN_ID, self.vault, self.token,
                           recipient, amount, height)
            assert_equal(hx(entry["withdrawal_leaf"]), leaf)
            burns[route] = {"txid": txid, "height": height, "withdrawal_id": entry["withdrawal_id"],
                            "recipient": recipient, "amount": amount, "leaf": leaf, "listed": entry}
        assert "exit" not in burns["d1"]["listed"], burns["d1"]["listed"]
        exit_listed = burns["d3"]["listed"]["exit"]
        assert ExitParams.from_json(exit_listed) == d3["exit_p"], exit_listed
        assert_equal((hx(exit_listed["factory"]), hx(exit_listed["account"])),
                     (hx(self.exit_factory), hx(d3["account"])))
        assert_equal(sorted(burn["withdrawal_id"] for burn in burns.values()), [0, 1])

        ordered = sorted(burns.values(), key=lambda burn: burn["withdrawal_id"])
        self.withdrawal_leaves.extend((burn["height"], burn["leaf"]) for burn in ordered)
        self.root = withdrawal_root([burn["leaf"] for burn in ordered])
        self.burn_height = max(heights)
        self.leaves_in_order = [burn["leaf"] for burn in ordered]

        info = self.v2_infos()
        assert_equal(info["withdrawals"], 2)
        assert_equal(hx(info["withdrawal_root_evm"]), self.root)
        assert_equal({key: info["reserve"][key] for key in ("minted", "burned", "reserve")},
                     {"minted": DEPOSIT_RAW, "burned": BURN_D1 + BURN_D3,
                      "reserve": DEPOSIT_RAW - BURN_D1 - BURN_D3})
        user_busd = self.wallet_asset(user, asset_id)
        relayer_busd = self.wallet_asset(relayer, asset_id)
        assert_equal(user_busd["confirmed"], DEPOSIT_RAW - BURN_D1 - BURN_D3 - 2 * TIP)
        assert_equal(relayer_busd["confirmed"], 2 * TIP)
        assert_equal(sorted(utxo["amount"] for utxo in relayer_busd["utxos"]), [TIP, TIP])
        assert_equal(self.native_balances(user), {"trusted": 0, "untrusted_pending": 0, "immature": 0})
        assert_equal(user.listunspent(0), [])
        pool = self.pool_status()["pool"]
        assert_equal(pool["coins"], POOL_COUNT - 2)
        spent_fee_inputs = {(burn["completed"]["fee_input"]["txid"], burn["completed"]["fee_input"]["vout"])
                            for burn in (d1, d3)}
        assert not spent_fee_inputs & {(coin["txid"], coin["vout"]) for coin in pool["outpoints"]}
        relayer_native = self.native_balances(relayer)
        assert_equal(relayer_native["trusted"], self.relayer_native_funded["trusted"] - 2 * POOL_VALUE)
        assert_equal(relayer_native["untrusted_pending"], 0)
        for burn in burns.values():
            assert_equal(user.gettransaction(burn["txid"])["confirmations"] >= 1, True)
        self.evidence.assertion("both sponsored burns mined: two leaves, tips to the relayer, user without B3",
                                expected={"user_busd": DEPOSIT_RAW - BURN_D1 - BURN_D3 - 2 * TIP,
                                          "relayer_busd": 2 * TIP, "user_native": 0,
                                          "burned": BURN_D1 + BURN_D3, "pool": POOL_COUNT - 2},
                                actual={"user_busd": user_busd["confirmed"], "relayer_busd": relayer_busd["confirmed"],
                                        "user_native": self.native_balances(user)["trusted"],
                                        "burned": info["reserve"]["burned"], "pool": pool["coins"]},
                                burns={route: {key: burn[key] for key in ("txid", "height", "withdrawal_id")}
                                       for route, burn in burns.items()},
                                network_fees={"d1": completed["network_fee"],
                                              "d3": d3["completed"]["network_fee"]},
                                tool_refusal=tool_refusal, listing=listing, reserve_before=reserve_before,
                                reserve_after=info["reserve"], relayer_native=relayer_native)
        self.sponsored_listing = listing
        return burns

    # -- 7. proofs, release (d1) and forward (d3) -----------------------------------------------
    def release_and_forward_sponsored(self, burns, covering):
        self.log.info("(7) getbridgewithdrawalproofv2 for both completed txids, identical on every node")
        anvil = self.anvil
        inclusion = covering["inclusion"]
        proofs = {}
        for route, burn in burns.items():
            found = [node.getbridgewithdrawalproofv2(burn["txid"], 0, inclusion) for node in self.bridge_nodes()]
            for proof in found[1:]:
                assert_equal(proof, found[0])
            proof = found[0]
            index = burn["withdrawal_id"]
            assert_equal((proof["withdrawal_id"], proof["source_txid"], proof["burn_output_index"]),
                         (index, burn["txid"], 0))
            assert_equal((proof["b3_height"], proof["amount_raw"]), (burn["height"], burn["amount"]))
            assert_equal(hx(proof["recipient"]), hx(burn["recipient"]))
            assert_equal((hx(proof["withdrawal_leaf"]), hx(proof["withdrawal_root"])), (burn["leaf"], self.root))
            root, path = withdrawal_root_and_path(self.leaves_in_order, index)
            assert_equal(root, self.root)
            assert_equal([hx(sibling) for sibling in proof["path"]], path)
            assert_equal(proof["function"], VAULT_V2_RELEASE_SIGNATURE)
            assert_equal(hx(proof["calldata"]), encode_vault_v2_release(index, burn["recipient"], burn["amount"],
                                                                        burn["height"], path))
            assert_equal(proof["certificate"]["inclusion_height"], inclusion)
            proofs[route] = proof
        assert_equal(ExitParams.from_json(proofs["d3"]["exit"]), ExitParams.from_json(burns["d3"]["listed"]["exit"]))
        assert "exit" not in proofs["d1"], proofs["d1"]

        self.log.info("(7) Route (d1): Vault V2 releases BURN_D1 USDT to ETH_RECIPIENT")
        d1, d3 = burns["d1"], burns["d3"]
        recipient_before = self.usdt(ETH_RECIPIENT)
        receipt = self.transact(self.vault, hx(proofs["d1"]["calldata"]), "B3VaultV2.release (sponsored d1)")
        assert_equal(self.usdt(ETH_RECIPIENT), recipient_before + BURN_D1)
        assert_equal(call_uint(anvil, self.vault, "released(uint64)", enc_uint(d1["withdrawal_id"])), 1)

        self.log.info("(7) Route (d3): contrib forward releases to F_x and forwards; USDT0 reaches the user")
        user_usdt0_before = self.usdt0_balance(self.user_eoa())
        out = run_forwarder_tool("forward", anvil, node=self.nodes[0], sender=self.relayer, batch=self.batch,
                                 txid=d3["txid"], vout=0, certificate_block=inclusion, fee=FORWARD_FEE,
                                 native_margin=NATIVE_MARGIN)
        assert_equal((out["status"], out["release_included"], out["released_by_this_tx"]), ("forwarded", True, True))
        assert_equal((hx(out["account"]), out["withdrawal_id"]), (hx(d3["recipient"]), d3["withdrawal_id"]))
        assert_equal((out["amount_sent"], out["fee"]), (BURN_D3 - FORWARD_FEE, FORWARD_FEE))
        assert_equal(call_uint(anvil, self.vault, "released(uint64)", enc_uint(d3["withdrawal_id"])), 1)
        self.deliver_lz(anvil, self.ep_eth, self.arb, self.ep_arb, "sponsored route (d3) to the user", delivered=1)
        assert_equal(self.usdt0_balance(self.user_eoa()), user_usdt0_before + BURN_D3 - FORWARD_FEE)
        locked = call_uint(anvil, self.vault, "locked()")
        assert_equal((locked, self.usdt(self.vault)), (DEPOSIT_RAW - BURN_D1 - BURN_D3,) * 2)
        self.evidence_step("sponsored burns paid out: d1 released to ETH_RECIPIENT, d3 forwarded to Arbitrum",
                           anvil, release_tx=receipt["transactionHash"], forward=out, proofs=proofs,
                           eth_recipient_usdt={"before": recipient_before, "after": self.usdt(ETH_RECIPIENT)},
                           user_usdt0={"before": user_usdt0_before, "after": self.usdt0_balance(self.user_eoa())},
                           vault_locked=locked, arbitrum_block=self.arb.block_number())

    # -- 8. status and reindex ------------------------------------------------------------------
    def check_status_and_reindex(self, burns):
        self.log.info("(8) contrib status: tips 2 x TIP and the R10 market note")
        n0, n4 = self.nodes[0], self.nodes[SPONSOR_NODE]
        asset_id = self.v2info["asset_id"]
        self.freeze()
        status = self.pool_status()
        assert_equal(status["tips"]["asset_id"], asset_id)
        assert_equal((status["tips"]["confirmed"], status["tips"]["utxos"]), (2 * TIP, 2))
        assert_equal(status["pool"]["coins"], POOL_COUNT - 2)
        assert MARKET_NOTE_FRAGMENT in status["note"], status["note"]
        assert_equal(status["bridge_v2"]["configured"], True)

        self.log.info("(8) n4 reindexes and replays both sponsored burns")
        reference = {
            "listing": n4.getbridgewithdrawals(),
            "user_busd": self.wallet_asset(self.user_wallet, asset_id)["confirmed"],
            "relayer_busd": self.wallet_asset(self.relayer_wallet, asset_id)["confirmed"],
        }
        assert_equal(reference["listing"], n0.getbridgewithdrawals())
        tip = n4.getbestblockhash()
        self.restart_node(SPONSOR_NODE, extra_args=list(n4.extra_args) + ["-reindex"])
        self.wait_until(lambda: n4.getbestblockhash() == tip, timeout=300)
        self.connect_nodes(SPONSOR_NODE, 0)
        for name in (USER_WALLET, RELAYER_WALLET):
            n4.loadwallet(name)
        self.user_wallet = n4.get_wallet_rpc(USER_WALLET)
        self.relayer_wallet = n4.get_wallet_rpc(RELAYER_WALLET)
        self.sync_bridge_blocks()
        n4.syncwithvalidationinterfacequeue()
        replayed = {
            "listing": n4.getbridgewithdrawals(),
            "user_busd": self.wallet_asset(self.user_wallet, asset_id)["confirmed"],
            "relayer_busd": self.wallet_asset(self.relayer_wallet, asset_id)["confirmed"],
        }
        assert_equal(replayed, reference)
        assert_equal(replayed["listing"], self.sponsored_listing)
        # The replayed bridge V2 state equals every validator's (v2_infos
        # compares all running nodes, n4 included, at one time).
        info = self.v2_infos()
        assert_equal((info["withdrawals"], hx(info["withdrawal_root_evm"])), (2, self.root))
        assert_equal(info["reserve"]["burned"], BURN_D1 + BURN_D3)
        for burn in burns.values():
            assert_equal(n4.getrawtransaction(burn["txid"], 1, n4.getblockhash(burn["height"]))["txid"], burn["txid"])
        assert_equal(self.native_balances(self.user_wallet), {"trusted": 0, "untrusted_pending": 0, "immature": 0})
        self.evidence.assertion("contrib status reports the tips and the R10 market note; n4 reindexed",
                                expected={"tips": 2 * TIP, "listing": reference["listing"]},
                                actual={"tips": status["tips"]["confirmed"], "listing": replayed["listing"]},
                                note=status["note"], pool=status["pool"], tip=tip)


if __name__ == '__main__':
    B3BridgeRT7sSponsoredFeesTest(__file__).main()
