#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Test fast wallet rescans across the deterministic FN Genesis block.

The BASIC filter commits to each complete B3A1 carrier, not to the embedded
owner-script suffix. A descriptor wallet restored from a pre-genesis backup
must therefore inspect the post-transition block even when the filter itself
does not match the bare owner script.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


FN_PER_TEST_RECIPIENT = 875
FN_WALLET_MATURITY_DEPTH = 31


class WalletFnGenesisRescanTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-b3modernregtest",
            "-b3flowmeshtest",
            "-blockfilterindex=1",
        ]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    @staticmethod
    def only_fn_asset(wallet, asset_id):
        assets = wallet.getwalletassets(asset_id)["assets"]
        assert_equal(len(assets), 1)
        return assets[0]

    @staticmethod
    def assert_fn_state(asset, *, depth, mature, spendable):
        assert_equal(asset["kind"], "fn")
        assert_equal(asset["confirmed"], FN_PER_TEST_RECIPIENT)
        assert_equal(asset["unconfirmed"], 0)
        assert_equal(asset["spendable"], spendable)
        assert_equal(len(asset["utxos"]), FN_PER_TEST_RECIPIENT)
        for utxo in asset["utxos"]:
            assert_equal(utxo["amount"], 1)
            assert_equal(utxo["confirmations"], depth)
            assert_equal(utxo["coinbase"], True)
            assert_equal(utxo["mature"], mature)
            assert_equal(utxo["spendable"], mature)

    def run_test(self):
        node = self.nodes[0]
        recipient = node.get_deterministic_priv_key().address
        backup = node.datadir_path / "fn-before-genesis.bak"
        original = node.get_wallet_rpc(self.default_wallet_name)

        self.log.info("Back up the deterministic FN recipient before block 1")
        original.backupwallet(backup)
        original.unloadwallet()

        self.log.info("Mine the mandatory FN Genesis block without the wallet loaded")
        self.generatetoaddress(node, 1, recipient)
        assert_equal(node.getblockcount(), 1)
        state = node.getassetstate()["fn"]
        assert_equal(state["historical_issued"], 3500)
        asset_id = state["asset_id"]

        self.wait_until(
            lambda: node.getindexinfo()["basic block filter index"]["synced"]
        )

        self.log.info("Restore the pre-genesis backup through the fast rescan path")
        with node.assert_debug_log(["fast variant using block filters"]):
            node.restorewallet("fn_rescan", backup)
        restored = node.get_wallet_rpc("fn_rescan")

        immature = self.only_fn_asset(restored, asset_id)
        self.assert_fn_state(
            immature,
            depth=1,
            mature=False,
            spendable=0,
        )

        self.log.info("At depth 30 FN is next-block eligible, but not wallet-selectable")
        self.generatetoaddress(node, FN_WALLET_MATURITY_DEPTH - 2, recipient)
        assert_equal(node.getblockcount(), FN_WALLET_MATURITY_DEPTH - 1)
        depth_30 = self.only_fn_asset(restored, asset_id)
        self.assert_fn_state(
            depth_30,
            depth=FN_WALLET_MATURITY_DEPTH - 1,
            mature=False,
            spendable=0,
        )

        self.log.info("At depth 31 FN enters the wallet's selectable balance")
        self.generatetoaddress(node, 1, recipient)
        assert_equal(node.getblockcount(), FN_WALLET_MATURITY_DEPTH)
        mature = self.only_fn_asset(restored, asset_id)
        self.assert_fn_state(
            mature,
            depth=FN_WALLET_MATURITY_DEPTH,
            mature=True,
            spendable=FN_PER_TEST_RECIPIENT,
        )


if __name__ == "__main__":
    WalletFnGenesisRescanTest(__file__).main()
