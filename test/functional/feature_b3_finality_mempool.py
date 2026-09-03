#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""FINALITY_KEY conflicts in the mempool must not poison block assembly.

Two restored copies of one wallet can create distinct sequence-0 binding
transactions for the same validator while disconnected.  Each transaction is
valid against the confirmed binding index and can therefore reach one miner's
mempool.  The block builder must select at most one of them instead of building
a candidate that fails the in-block FINALITY_KEY overlay check.
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


CORRIDOR_LENGTH = 160
B3_ARGS = [
    "-b3modernregtest",
    f"-b3corridorlength={CORRIDOR_LENGTH}",
    "-fallbackfee=0.00001",
    "-minrelaytxfee=0.00001",
    "-maxtxfee=0.001",
    "-addresstype=legacy",
    "-changetype=legacy",
]


class B3FinalityMempoolTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [B3_ARGS] * self.num_nodes

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def set_mocktime(self, timestamp):
        for node in self.nodes:
            node.setmocktime(timestamp)

    def mine_corridor(self, node, count, address):
        """Advance mocktime with the chain so corridor future-time bounds hold."""
        while count:
            chunk = min(count, 80)
            self.generatetoaddress(node, chunk, address)
            count -= chunk
            self.mocktime += chunk
            self.set_mocktime(self.mocktime)

    @staticmethod
    def lock_all_but(wallet, selected):
        locked = [
            {"txid": coin["txid"], "vout": coin["vout"]}
            for coin in wallet.listunspent()
            if (coin["txid"], coin["vout"]) != selected
        ]
        if locked:
            assert wallet.lockunspent(False, locked)

    def run_test(self):
        miner, clone_node = self.nodes
        self.mocktime = int(time.time()) + 10
        self.set_mocktime(self.mocktime)

        mining_address = miner.getnewaddress()
        self.log.info("Fund one wallet and create its persistent validator identity")
        self.mine_corridor(miner, 110, mining_address)
        stake = miner.createstake(1)
        self.mine_corridor(miner, 1, mining_address)
        assert stake["txid"] in miner.getblock(miner.getbestblockhash())["tx"]
        self.sync_blocks()

        backup = miner.datadir_path / "shared-validator-wallet.bak"
        miner.backupwallet(backup)
        clone_node.restorewallet("validator_clone", backup)
        clone = clone_node.get_wallet_rpc("validator_clone")

        # Both restored copies know the same mature coins. Lock complementary
        # coins so wallet funding produces two distinct base transactions.
        miner_coins = sorted(
            miner.listunspent(), key=lambda coin: (coin["txid"], coin["vout"])
        )
        clone_coins = sorted(
            clone.listunspent(), key=lambda coin: (coin["txid"], coin["vout"])
        )
        assert_got = min(len(miner_coins), len(clone_coins))
        assert assert_got >= 2
        first = (miner_coins[0]["txid"], miner_coins[0]["vout"])
        second = (clone_coins[1]["txid"], clone_coins[1]["vout"])
        assert first != second
        self.lock_all_but(miner, first)
        self.lock_all_but(clone, second)

        self.disconnect_nodes(0, 1)
        self.log.info("Create two independently funded seq-0 binds for one validator")
        bind_a = miner.bindfinalitykey()
        bind_b = clone.bindfinalitykey()
        assert_equal(bind_a["seq"], 0)
        assert_equal(bind_b["seq"], 0)
        assert_equal(bind_a["validator_key"], bind_b["validator_key"])
        assert_equal(bind_a["bls_pubkey"], bind_b["bls_pubkey"])
        assert bind_a["txid"] != bind_b["txid"]

        raw_b = clone.getrawtransaction(bind_b["txid"])
        decoded_b = miner.decoderawtransaction(raw_b)
        assert_equal(decoded_b["ptxid"], bind_b["ptxid"])
        assert_equal(miner.sendrawtransaction(raw_b), bind_b["txid"])
        assert bind_a["txid"] in miner.getrawmempool()
        assert bind_b["txid"] in miner.getrawmempool()

        self.log.info("The toxic mempool must not prevent mining a valid block")
        block_hash = self.generatetoaddress(
            miner, 1, mining_address, sync_fun=self.no_op
        )[0]
        block = miner.getblock(block_hash)
        mined_binds = {
            txid for txid in (bind_a["txid"], bind_b["txid"])
            if txid in block["tx"]
        }
        assert_equal(len(mined_binds), 1)
        assert_equal(miner.getfinalityinfo()["binding"]["bound"], True)

        # The losing transaction spends a different input, so ordinary UTXO
        # conflict removal does not evict it. It is now stale against the
        # confirmed sequence and must remain harmless on every later template.
        loser = ({bind_a["txid"], bind_b["txid"]} - mined_binds).pop()
        assert loser in miner.getrawmempool()
        next_hash = self.generatetoaddress(
            miner, 1, mining_address, sync_fun=self.no_op
        )[0]
        assert loser not in miner.getblock(next_hash)["tx"]


if __name__ == "__main__":
    B3FinalityMempoolTest(__file__).main()
