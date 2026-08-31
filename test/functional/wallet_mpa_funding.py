#!/usr/bin/env python3
# Copyright (c) 2026 The B3 Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that generic wallet funding cannot discard Modern Payload Area data."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class WalletMpaFundingTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # Enable the B3 Modern raw-transaction codec. No activation bypasses
        # are needed: this test exercises wallet decoding only.
        self.extra_args = [["-b3modernregtest"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.nodes[0].createwallet("mpa_funding")
        wallet = self.nodes[0].get_wallet_rpc("mpa_funding")

        # The query RPC is available on an ordinary wallet and starts with an
        # exact, empty asset inventory.
        assert_equal(wallet.getwalletassets(), {"assets": []})
        assert_raises_rpc_error(-8, "minconf must be non-negative",
                                wallet.getwalletassets, None, -1)
        assert_raises_rpc_error(-8, "asset_id must identify a non-native asset",
                                wallet.getwalletassets, "00" * 32)

        raw = bytes.fromhex(wallet.createrawtransaction(
            [], [{wallet.getnewaddress(): 1}]))
        self.log.debug("Base transaction: %s", raw.hex())

        # Convert the ordinary no-input encoding into the modern optional-data
        # form and append one structurally valid MPA record before nLockTime.
        mpa = bytes.fromhex("01040001000100")
        with_mpa = raw[:4] + bytes.fromhex("0002") + raw[4:-4] + mpa + raw[-4:]
        self.log.debug("MPA transaction: %s", with_mpa.hex())

        assert_raises_rpc_error(
            -8,
            "fundrawtransaction does not support transactions with Modern Payload Area records",
            wallet.fundrawtransaction,
            with_mpa.hex(),
        )


if __name__ == '__main__':
    WalletMpaFundingTest(__file__).main()
