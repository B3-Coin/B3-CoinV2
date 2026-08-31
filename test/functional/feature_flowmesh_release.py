#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Minimal FlowMesh transition-release gate.

Four independent regtest wallets pre-bind one historical FN seat each at A2,
bootstrap a fixed-supply TEST_ASSET/B3 market, and certify the sequence-zero
genesis over the existing B3 P2P network at A3. The test then drives both
assets through deposit, checkpoint, keyless sweep, one exact-fee spot trade,
and an asset withdrawal whose connected type-9 payout is retired by the
anchor-derived settlement entry.

The chain clock is advanced explicitly. This keeps activation-height and
30-block anchor assertions exact without weakening any consensus depth.
"""

import time
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


CORRIDOR_END = 130
MODERN_START = CORRIDOR_END + 1
A1 = MODERN_START + 1
A2 = MODERN_START + 2
A3 = A2 + 30

TEST_ASSET_SUPPLY = 1_000_000
TEST_ASSET_DEPOSIT = 1_000
B3_DEPOSIT = Decimal("20")
TRADE_PRICE = 100_000_000  # 0.1 B3 in native atomic units per base unit.
TRADE_QUANTITY = 100
TRADE_NOTIONAL = Decimal("10")
TRADE_FEE = Decimal("0.001")  # floor(10 B3 / 10,000), charged once to seller.
TREASURY_FEE = Decimal("0.0002")  # FlowMesh v1 pins 20% of the trade fee.
WITHDRAW_ASSET = 40

B3_ARGS = [
    "-b3modernregtest",
    "-b3flowmeshtest",
    f"-b3corridorlength={CORRIDOR_END}",
    "-b3corridorreward=2000000000000",  # 2,000 B3 test-only funding blocks.
    "-b3blockinterval=1",
    "-b3roundseconds=1",
    "-b3epochlength=200",
    "-b3checkpointinterval=5",
    "-b3checkpointdepth=3",
    "-b3maxepochextension=200",
    "-b3minfinalityset=4",
    "-fallbackfee=0.00001",
    "-addresstype=legacy",
    "-changetype=legacy",
    # This is a release gate: every successful wallet and FlowMesh response
    # must agree with its public RPC result schema as well as its fund effects.
    "-rpcdoccheck=1",
]


class FlowMeshReleaseTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        self.extra_args = [B3_ARGS] * self.num_nodes
        self.mock_time = int(time.time()) - 300
        self.pos_running = False

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_chain_time(self, timestamp):
        self.mock_time = timestamp
        for node in self.nodes:
            node.setmocktime(timestamp)

    def mine_corridor(self, count):
        """Mine transition blocks without crossing the 120-second future bound."""
        address = self.nodes[0].get_deterministic_priv_key().address
        while count:
            chunk = min(count, 80)
            self.generatetoaddress(self.nodes[0], chunk, address)
            self.sync_blocks(timeout=120)
            count -= chunk
            if count:
                self.set_chain_time(self.mock_time + chunk)

    def synchronize_mempools(self):
        """Load wallet-created transactions into every node deterministically.

        Mock time deliberately does not advance between transaction creation
        and block production, so ordinary wallet INV trickle timers do not
        fire. FlowMesh traffic is not touched by this helper.
        """
        pools = [set(node.getrawmempool()) for node in self.nodes]
        all_txids = set().union(*pools)
        transactions = {}
        dependencies = {}
        for txid in all_txids:
            source = next(node for node, pool in zip(self.nodes, pools)
                          if txid in pool)
            transactions[txid] = source.getrawtransaction(txid)
            dependencies[txid] = set(source.getmempoolentry(txid)["depends"])

        ordered = []
        pending = set(all_txids)
        while pending:
            ready = sorted(txid for txid in pending
                           if not (dependencies[txid] & pending))
            if not ready:
                raise AssertionError("Mempool dependency graph contains a cycle")
            ordered.extend(ready)
            pending.difference_update(ready)

        for txid in ordered:
            for node, pool in zip(self.nodes, pools):
                if txid not in pool:
                    assert_equal(node.sendrawtransaction(transactions[txid]),
                                 txid)
                    pool.add(txid)
        for node in self.nodes:
            node.syncwithvalidationinterfacequeue()
        assert all(pool == pools[0] for pool in pools)

    def start_pos(self):
        if self.pos_running:
            return
        for node in self.nodes:
            result = node.startstaking()
            assert_equal(result["running"], True)
            assert_equal(result["finality_signing"], True)
        self.pos_running = True

    def stop_pos(self):
        if not self.pos_running:
            return
        for node in self.nodes:
            assert_equal(node.stopstaking()["running"], False)
        self.pos_running = False

    def mine_pos_blocks(self, count, allow_overshoot=False):
        """Advance modern PoS with four live signers.

        Activation-boundary callers require an exact stop. Later custody
        callers may allow a concurrently completed extra block and derive the
        actual transaction height from the connected transaction.
        """
        target_height = self.nodes[0].getblockcount() + count
        while self.nodes[0].getblockcount() < target_height:
            self.start_pos()
            old_height = self.nodes[0].getblockcount()
            deadline = time.time() + 60
            while self.nodes[0].getblockcount() == old_height:
                if time.time() >= deadline:
                    raise AssertionError("Modern PoS did not produce a block")
                schedules = []
                for node in self.nodes:
                    staking = node.getstakinginfo()["staking"]
                    if staking["running"] and "next_block_time" in staking:
                        schedules.append(staking["next_block_time"])
                future = [slot for slot in schedules if slot > self.mock_time]
                # Advance only to the earliest still-advertised slot. If that
                # producer rebuilds and withdraws it, re-read all schedules on
                # the next pass instead of jumping across multiple rounds.
                self.set_chain_time(min(future) if future else self.mock_time + 1)
                # The staking worker polls at a coarser cadence than the RPC
                # harness. Give the selected round time a chance to produce
                # before advancing again, or several rounds can become due at
                # once and cross an activation boundary.
                time.sleep(0.5)
            # Freeze production before synchronization so activation-boundary
            # setup can always target the next block exactly.
            self.stop_pos()
            self.sync_blocks(timeout=120)
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

    def market_status(self, node, market_id):
        markets = node.listflowmeshmarkets()
        matches = [m for m in markets if m["market_id"] == market_id]
        assert len(matches) <= 1
        return matches[0] if matches else None

    def wait_for_market_convergence(self, market_id, require_unpaused=True):
        def converged():
            statuses = [self.market_status(n, market_id) for n in self.nodes]
            if any(s is None for s in statuses):
                return False
            if any(not s["running"] or s["observer_only"] or
                   s["halt"] != "none" for s in statuses):
                return False
            if require_unpaused and any(s["paused"] for s in statuses):
                return False
            return len({s["state_root"] for s in statuses}) == 1

        self.wait_until(converged, timeout=120)

    def publish_checkpoint(self, market_id):
        self.wait_until(
            lambda: bool(self.market_status(self.nodes[0], market_id) or {}) and
                    self.market_status(self.nodes[0], market_id)["checkpoint_pending"],
            timeout=120,
        )
        checkpoint = self.nodes[0].createflowmeshcheckpoint(market_id)
        self.synchronize_mempools()
        self.mine_pos_blocks(1, allow_overshoot=True)
        self.wait_for_market_convergence(market_id)
        return checkpoint

    def publish_pending_checkpoints(self, market_id, maximum=16):
        published = []
        for _ in range(maximum):
            status = self.market_status(self.nodes[0], market_id)
            if not status["checkpoint_pending"]:
                return published
            published.append(self.publish_checkpoint(market_id))
        raise AssertionError("FlowMesh checkpoint queue did not drain")

    def publish_until_vault_operations(self, market_id, count):
        for _ in range(16):
            operations = self.nodes[0].listflowmeshvaultoperations(market_id)
            if len(operations) >= count:
                return operations
            status = self.market_status(self.nodes[0], market_id)
            if status["checkpoint_pending"]:
                self.publish_checkpoint(market_id)
            else:
                time.sleep(0.1)
        raise AssertionError("Certified FlowMesh vault operations did not appear")

    @staticmethod
    def parse_vault_script(script_hex):
        script = bytes.fromhex(script_hex)
        if not script:
            return None
        opcode = script[0]
        if 1 <= opcode <= 75:
            size, offset = opcode, 1
        elif opcode == 0x4C and len(script) >= 2:
            size, offset = script[1], 2
        elif opcode == 0x4D and len(script) >= 3:
            size = int.from_bytes(script[1:3], "little")
            offset = 3
        else:
            return None
        payload = script[offset:offset + size]
        if len(payload) != size or len(payload) < 83 or payload[:4] != b"B3A1":
            return None
        if int.from_bytes(payload[44:46], "big") != 3:
            return None
        if int.from_bytes(payload[46:48], "big") != 2:
            return None
        params = payload[48:]
        if len(params) not in (35, 67):
            return None
        return {
            "asset": payload[4:36],
            "amount": int.from_bytes(payload[36:44], "big"),
            "vault": params[:32],
            "kind": params[32],
        }

    def custody(self, records, asset_id, vault_id):
        """Sum currently-unspent pool outputs among the known custody txs."""
        asset_wire = bytes.fromhex(asset_id)[::-1]
        vault_wire = bytes.fromhex(vault_id)[::-1]
        total = 0
        for record in records:
            decoded = self.nodes[0].decoderawtransaction(record["hex"])
            assert_equal(decoded["txid"], record["txid"])
            for output in decoded["vout"]:
                parsed = self.parse_vault_script(output["scriptPubKey"]["hex"])
                if (parsed is None or parsed["kind"] != 2 or
                        parsed["asset"] != asset_wire or
                        parsed["vault"] != vault_wire):
                    continue
                if self.nodes[0].gettxout(record["txid"], output["n"]) is not None:
                    total += parsed["amount"]
        return total

    def run_test(self):
        n0, n1, n2, n3 = self.nodes
        self.set_chain_time(self.mock_time)

        self.log.info("Corridor funding and four independent finality stakes")
        self.mine_corridor(101)
        for node in (n1, n2, n3):
            n0.sendtoaddress(node.get_deterministic_priv_key().address, 120)
        self.synchronize_mempools()
        self.mine_corridor(1)

        for node, amount in zip(self.nodes, (40, 30, 20, 10)):
            node.createstake(amount)
            node.bindfinalitykey()
        self.synchronize_mempools()
        self.mine_corridor(1)
        self.mine_corridor(CORRIDOR_END - n0.getblockcount())
        assert_equal(n0.getblockcount(), CORRIDOR_END)
        for node in self.nodes:
            self.wait_until(
                lambda n=node: n.getstakinginfo()["active"] > Decimal("0"),
                timeout=60,
            )

        self.log.info("Modern PoS reaches A2-1 under all four validators")
        self.mine_pos_blocks(A2 - 1 - n0.getblockcount())
        assert_equal(n0.getblockcount(), A2 - 1)
        assert_equal(A1, A2 - 1)

        self.log.info("A2: bind four FN seats and bootstrap TEST_ASSET first")
        issued = n0.issueasset(TEST_ASSET_SUPPLY, 2)
        asset_id = issued["asset_id"]
        seats = [node.bindflowmeshseat() for node in self.nodes]
        assert_equal(len({seat["seat_id"] for seat in seats}), 4)
        assert_equal(len({seat["bls_pubkey"] for seat in seats}), 4)

        asset_deposit = n0.flowmeshdeposit(
            asset_id,
            asset_id,
            TEST_ASSET_DEPOSIT,
            {"minconf": 0, "include_unsafe": True},
        )
        market_id = asset_deposit["market_id"]
        vault_id = asset_deposit["vault_id"]
        self.synchronize_mempools()
        self.mine_pos_blocks(1)
        assert_equal(n0.getblockcount(), A2)
        assert_equal(self.wallet_asset(n0, asset_id)["confirmed"],
                     TEST_ASSET_SUPPLY - TEST_ASSET_DEPOSIT)

        for node in self.nodes:
            started = node.startflowmeshvalidator()
            assert_equal(started["running"], True)
            assert_equal(started["armed_keys"], 1)

        self.log.info("Native B3 joins only after the colored-first market fact")
        native_deposit = n1.flowmeshdeposit(asset_id, "B3", B3_DEPOSIT)
        assert_equal(native_deposit["market_id"], market_id)
        assert_equal(native_deposit["vault_id"], vault_id)
        self.synchronize_mempools()
        self.mine_pos_blocks(1)

        self.log.info("A3: four-seat P2P quorum certifies and anchors sequence zero")
        self.mine_pos_blocks(A3 - n0.getblockcount())
        assert_equal(n0.getblockcount(), A3)
        # Sequence zero intentionally pauses the market until its checkpoint
        # connects, so require consistent/runnable peers here but not resumed
        # execution yet.
        self.wait_for_market_convergence(market_id, require_unpaused=False)
        self.wait_until(
            lambda: self.market_status(n0, market_id)["checkpoint_pending"] and
                    self.market_status(n0, market_id)["pending_checkpoint_sequence"] == 0,
            timeout=120,
        )
        genesis_checkpoint = self.publish_checkpoint(market_id)
        assert_equal(genesis_checkpoint["sequence"], 0)
        assert_equal(genesis_checkpoint["effect_count"], 0)

        self.log.info("Both 30-deep deposits are certified and checkpointed")
        deposit_sequence = self.market_status(
            n0, market_id,
        )["next_microblock_sequence"]
        n0.submitflowmeshdeposit(
            market_id, asset_deposit["deposit_txid"],
            asset_deposit["deposit_vout"],
        )
        n1.submitflowmeshdeposit(
            market_id, native_deposit["deposit_txid"],
            native_deposit["deposit_vout"],
        )

        def deposits_credited():
            status = self.market_status(n0, market_id)
            if status["next_microblock_sequence"] > deposit_sequence + 2:
                raise AssertionError(
                    "Deposit actions produced repeated zero-effect entries"
                )
            seller = n0.getflowmeshbalance(market_id)["account"]
            buyer = n1.getflowmeshbalance(market_id)["account"]
            return (seller["base_available"] == TEST_ASSET_DEPOSIT and
                    seller["b3_available"] == Decimal("0") and
                    buyer["base_available"] == 0 and
                    buyer["b3_available"] == B3_DEPOSIT)

        self.wait_until(deposits_credited, timeout=120)
        self.wait_for_market_convergence(market_id)
        credited = self.market_status(n0, market_id)
        assert_equal(credited["next_effect_index"], 2)
        assert credited["next_microblock_sequence"] <= deposit_sequence + 2
        deposit_operations = self.publish_until_vault_operations(market_id, 2)
        assert_equal({op["kind"] for op in deposit_operations}, {"deposit-sweep"})
        assert_equal({op["asset"] for op in deposit_operations}, {asset_id, "B3"})

        self.log.info("Certified type-9 sweeps move both deposits into pool custody")
        custody_records = [asset_deposit, native_deposit]
        for operation in deposit_operations:
            sweep = n0.createflowmeshvaulttx(operation["effect_id"])
            assert_equal(sweep["operation"], "deposit-sweep")
            custody_records.append(sweep)
        self.synchronize_mempools()
        sweep_first_height = n0.getblockcount() + 1
        self.mine_pos_blocks(1, allow_overshoot=True)
        sweep_height = max(
            self.transaction_height(n0, record["txid"], sweep_first_height)
            for record in custody_records[-2:]
        )
        self.wait_until(
            lambda: not n0.listflowmeshvaultoperations(market_id),
            timeout=120,
        )
        assert_equal(self.custody(custody_records, asset_id, vault_id),
                     TEST_ASSET_DEPOSIT)
        assert_equal(self.custody(custody_records, "00" * 32, vault_id),
                     int(B3_DEPOSIT * 1_000_000_000))

        self.log.info("One matched TEST_ASSET/B3 trade charges exactly 0.01% once")
        n1.submitflowmeshorder(
            market_id, "bid", TRADE_PRICE, TRADE_QUANTITY,
        )
        n0.submitflowmeshorder(
            market_id, "ask", TRADE_PRICE, TRADE_QUANTITY,
        )

        def trade_settled():
            seller = n0.getflowmeshbalance(market_id)["account"]
            buyer = n1.getflowmeshbalance(market_id)["account"]
            return (seller["base_available"] ==
                        TEST_ASSET_DEPOSIT - TRADE_QUANTITY and
                    seller["base_reserved"] == 0 and
                    seller["b3_available"] == TRADE_NOTIONAL - TRADE_FEE and
                    buyer["base_available"] == TRADE_QUANTITY and
                    buyer["b3_available"] == B3_DEPOSIT - TRADE_NOTIONAL and
                    buyer["b3_reserved"] == Decimal("0"))

        self.wait_until(trade_settled, timeout=120)
        self.wait_for_market_convergence(market_id)
        self.publish_pending_checkpoints(market_id)
        seller = n0.getflowmeshbalance(market_id)["account"]
        buyer = n1.getflowmeshbalance(market_id)["account"]
        assert_equal(
            B3_DEPOSIT - seller["b3_available"] - buyer["b3_available"],
            TRADE_FEE,
        )
        assert_equal(self.custody(custody_records, asset_id, vault_id),
                     TEST_ASSET_DEPOSIT)
        assert_equal(self.custody(custody_records, "00" * 32, vault_id),
                     int(B3_DEPOSIT * 1_000_000_000))

        # The sweep outputs are not yet visible at the 30-deep production
        # anchor. The trade must still succeed: its treasury share remains in
        # the fixed internal account and no unpayable receipt is certified.
        assert_equal(n0.listflowmeshvaultoperations(market_id), [])

        self.log.info("Pool custody matures; treasury and user withdrawals become payable")
        self.mine_pos_blocks(
            sweep_height + 30 - n0.getblockcount(), allow_overshoot=True,
        )
        payout_address = n1.getnewaddress()
        n1.requestflowmeshwithdrawal(
            market_id, asset_id, WITHDRAW_ASSET, payout_address,
        )
        self.wait_until(
            lambda: n1.getflowmeshbalance(market_id)["account"]["base_available"] ==
                    TRADE_QUANTITY - WITHDRAW_ASSET,
            timeout=120,
        )
        # This ordinary slot also retries the deferred treasury balance. Both
        # assets have anchored pool capacity, so its exact fee receipt and the
        # user's asset receipt are certified together.
        withdrawal_ops = self.publish_until_vault_operations(market_id, 2)
        withdrawals = [op for op in withdrawal_ops if op["kind"] == "withdrawal"]
        assert_equal(len(withdrawals), 2)
        treasury_matches = [op for op in withdrawals if op["asset"] == "B3"]
        user_matches = [op for op in withdrawals if op["asset"] == asset_id]
        assert_equal(len(treasury_matches), 1)
        assert_equal(len(user_matches), 1)
        treasury_receipt = treasury_matches[0]
        assert_equal(treasury_receipt["amount"], TREASURY_FEE)
        receipt = user_matches[0]
        assert_equal(receipt["asset"], asset_id)
        assert_equal(receipt["amount"], WITHDRAW_ASSET)

        treasury_payout = n0.createflowmeshvaulttx(
            treasury_receipt["effect_id"],
            n0.get_deterministic_priv_key().address,
        )
        assert_equal(treasury_payout["operation"], "withdrawal")
        custody_records.append(treasury_payout)
        payout = n0.createflowmeshvaulttx(receipt["effect_id"], payout_address)
        assert_equal(payout["operation"], "withdrawal")
        custody_records.append(payout)
        self.synchronize_mempools()
        payout_first_height = n0.getblockcount() + 1
        self.mine_pos_blocks(1, allow_overshoot=True)
        payout_height = max(
            self.transaction_height(
                n0, treasury_payout["txid"], payout_first_height,
            ),
            self.transaction_height(n0, payout["txid"], payout_first_height),
        )
        self.wait_until(
            lambda: self.wallet_asset(n1, asset_id)["confirmed"] == WITHDRAW_ASSET,
            timeout=120,
        )
        assert_equal(self.custody(custody_records, asset_id, vault_id),
                     TEST_ASSET_DEPOSIT - WITHDRAW_ASSET)
        assert_equal(self.custody(custody_records, "00" * 32, vault_id),
                     int((B3_DEPOSIT - TREASURY_FEE) * 1_000_000_000))

        # The type-9 fact enters production only when its block is the
        # canonical 30-deep anchor. The resulting actionless settlement entry
        # must itself receive a type-8 checkpoint before execution resumes.
        self.mine_pos_blocks(
            payout_height + 30 - n0.getblockcount(), allow_overshoot=True,
        )
        self.wait_until(
            lambda: self.market_status(n0, market_id)["checkpoint_pending"],
            timeout=120,
        )
        settlement_checkpoint = self.publish_checkpoint(market_id)
        assert_equal(settlement_checkpoint["effect_count"], 0)
        self.wait_until(
            lambda: not self.market_status(n0, market_id)["checkpoint_pending"],
            timeout=120,
        )

        seller = n0.getflowmeshbalance(market_id)["account"]
        buyer = n1.getflowmeshbalance(market_id)["account"]
        assert_equal(seller["base_available"] + buyer["base_available"],
                     TEST_ASSET_DEPOSIT - WITHDRAW_ASSET)
        assert_equal(self.custody(custody_records, asset_id, vault_id),
                     seller["base_available"] + buyer["base_available"])
        assert_equal(self.custody(custody_records, "00" * 32, vault_id),
                     int((B3_DEPOSIT - TREASURY_FEE) * 1_000_000_000))
        assert_equal(
            self.wallet_asset(n0, asset_id)["confirmed"] +
            self.wallet_asset(n1, asset_id)["confirmed"] +
            self.custody(custody_records, asset_id, vault_id),
            TEST_ASSET_SUPPLY,
        )
        assert_equal(n0.listflowmeshvaultoperations(market_id), [])
        self.wait_for_market_convergence(market_id)

        final_status = self.market_status(n0, market_id)
        final_sequence = final_status["next_microblock_sequence"]
        final_state_root = final_status["state_root"]
        final_seller = seller
        final_buyer = buyer

        self.log.info("Four-node restart preserves certified state and custody")
        self.stop_nodes()
        self.start_nodes()
        self.set_chain_time(self.mock_time)
        for i in range(self.num_nodes - 1):
            self.connect_nodes(i + 1, i)
        self.sync_all()
        for node in self.nodes:
            started = node.startflowmeshvalidator()
            assert_equal(started["running"], True)
            assert_equal(started["armed_keys"], 1)
        self.wait_for_market_convergence(market_id)
        for node in self.nodes:
            status = self.market_status(node, market_id)
            assert_equal(status["next_microblock_sequence"], final_sequence)
            assert_equal(status["state_root"], final_state_root)
        assert_equal(n0.getflowmeshbalance(market_id)["account"], final_seller)
        assert_equal(n1.getflowmeshbalance(market_id)["account"], final_buyer)
        assert_equal(n0.listflowmeshvaultoperations(market_id), [])
        assert_equal(self.custody(custody_records, asset_id, vault_id),
                     TEST_ASSET_DEPOSIT - WITHDRAW_ASSET)

        self.log.info(
            f"FlowMesh release gate complete: market {market_id}, "
            f"sequence {final_sequence}"
        )


if __name__ == "__main__":
    FlowMeshReleaseTest(__file__).main()
