#!/usr/bin/env python3
# Copyright (c) 2026 The B3 Coin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Test releasestake: unstaking STAKE outputs back into ordinary B3.

Releasing replaces the manual console recipe (getstakinginfo, then sendall
with the stake outpoint as an input). It spends only STAKE outputs of this
wallet, pays the fee from the released principal and returns it to an
address of the same wallet.
"""

import time
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error

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


class WalletReleaseStakeTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [B3_ARGS]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine(self, count):
        while count:
            chunk = min(count, 80)
            self.generatetoaddress(self.node, chunk, self.miner)
            count -= chunk
            self.mocktime += chunk
            self.node.setmocktime(self.mocktime)

    def stake_outpoints(self):
        return {(s["txid"], s["vout"]) for s in self.wallet.getstakinginfo()["stakes"]}

    def run_test(self):
        self.node = self.nodes[0]
        self.mocktime = int(time.time()) + 10
        self.node.setmocktime(self.mocktime)
        self.wallet = self.node.get_wallet_rpc(self.default_wallet_name)
        self.miner = self.wallet.getnewaddress()

        self.log.info("Fund the wallet and create two confirmed stakes and one unconfirmed")
        self.mine(110)
        first = self.wallet.createstake(1)
        second = self.wallet.createstake(2)
        self.mine(1)
        third = self.wallet.createstake(3)
        assert_equal(len(self.stake_outpoints()), 3)

        def outpoint(stake):
            return {"txid": stake["txid"], "vout": stake["vout"]}

        self.log.info("Refuse what is not a releasable stake of this wallet")
        assert_raises_rpc_error(-4, "is not confirmed yet",
                                self.wallet.releasestake, [outpoint(third)])
        stakes = self.stake_outpoints()
        ordinary = next(u for u in self.wallet.listunspent() if (u["txid"], u["vout"]) not in stakes)
        assert_raises_rpc_error(-8, "is not an unspent STAKE output of this wallet",
                                self.wallet.releasestake, [{"txid": ordinary["txid"], "vout": ordinary["vout"]}])
        assert_raises_rpc_error(-8, "is listed twice",
                                self.wallet.releasestake, [outpoint(first), outpoint(first)])
        self.wallet.lockunspent(False, [outpoint(first)])
        assert_raises_rpc_error(-4, "is locked",
                                self.wallet.releasestake, [outpoint(first)])
        self.wallet.lockunspent(True, [outpoint(first)])
        self.node.createwallet(wallet_name="other")
        foreign = self.node.get_wallet_rpc("other").getnewaddress()
        assert_raises_rpc_error(-5, "does not belong to this wallet",
                                self.wallet.releasestake, [outpoint(first)], foreign)
        assert_equal(self.stake_outpoints(), stakes)

        self.log.info("Release one chosen stake to a wallet address")
        before = self.wallet.getbalances()["mine"]["trusted"]
        released = self.wallet.releasestake([outpoint(first)])
        assert_equal(released["released_outputs"], 1)
        assert_equal(released["principal"], Decimal(str(first["amount"])))
        assert_equal(released["amount"], released["principal"] - released["fee"])
        assert self.wallet.getaddressinfo(released["address"])["ismine"]
        tx = self.wallet.gettransaction(released["txid"], verbose=True)["decoded"]
        assert_equal(len(tx["vin"]), 1)
        assert_equal((tx["vin"][0]["txid"], tx["vin"][0]["vout"]), (first["txid"], first["vout"]))
        assert_equal(len(tx["vout"]), 1)
        self.mine(1)
        remaining = self.stake_outpoints()
        assert (first["txid"], first["vout"]) not in remaining
        assert_equal(len(remaining), 2)
        after = self.wallet.getbalances()["mine"]["trusted"]
        # The block reward also lands in this wallet; the release adds the rest.
        assert after - before >= released["amount"]

        self.log.info("Release every confirmed stake at once, to a chosen address")
        self.mine(1)  # Confirm the third stake.
        target = self.wallet.getnewaddress()
        everything = self.wallet.releasestake(None, target)
        assert_equal(everything["released_outputs"], 2)
        assert_equal(everything["principal"], Decimal(str(second["amount"])) + Decimal(str(third["amount"])))
        assert_equal(everything["address"], target)
        self.mine(1)
        assert_equal(self.stake_outpoints(), set())
        assert_raises_rpc_error(-4, "no confirmed STAKE outputs to release", self.wallet.releasestake)


if __name__ == "__main__":
    WalletReleaseStakeTest(__file__).main()
