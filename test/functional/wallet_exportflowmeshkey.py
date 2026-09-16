#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""Selected FN BLS key export using disposable wallets and fresh test keys only."""

import secrets

from feature_flowmesh_release import B3_ARGS
from test_framework.authproxy import JSONRPCException, log as rpc_log
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


BLS_ORDER = int("73eda753299d7d483339d80809a1d80553bda402fffe5bfeffffffff00000001", 16)


class ExportFlowMeshKeyTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.supports_cli = False
        self.wallet_names = []
        self.extra_args = [[*B3_ARGS, "-enableflowmeshvalidator=1", "-flowmeshtransport=legacy"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        # Even generated fixture secrets should not enter the RPC trace.
        previous_logging = rpc_log.disabled
        rpc_log.disabled = True
        try:
            self.exercise_export()
        finally:
            rpc_log.disabled = previous_logging

    def exercise_export(self):
        node = self.nodes[0]
        node.createwallet(wallet_name="source", blank=True, passphrase="isolated-export-passphrase")
        node.createwallet(wallet_name="other", blank=True)
        node.createwallet(wallet_name="destination", blank=True)
        node.createwallet(wallet_name="watch", disable_private_keys=True, blank=True)
        source = node.get_wallet_rpc("source")
        other = node.get_wallet_rpc("other")
        destination = node.get_wallet_rpc("destination")
        watch = node.get_wallet_rpc("watch")
        source.walletpassphrase("isolated-export-passphrase", 600)

        first_secret = (secrets.randbelow(BLS_ORDER - 1) + 1).to_bytes(32, "big").hex()
        second_secret = (secrets.randbelow(BLS_ORDER - 1) + 1).to_bytes(32, "big").hex()
        malformed_secret = first_secret[:-1] + "!"
        try:
            source.importflowmeshkey(malformed_secret)
        except JSONRPCException as error:
            assert_equal(error.error["code"], -8)
            assert malformed_secret not in error.error["message"], "Malformed secret was echoed in an error"
        else:
            raise AssertionError("Malformed key import unexpectedly succeeded")
        first = source.importflowmeshkey(first_secret)
        second = other.importflowmeshkey(second_secret)
        pubkey = first["bls_pubkey"]
        assert pubkey != second["bls_pubkey"]
        assert_equal(source.getflowmeshvalidatorinfo()["wallet_bls_pubkeys"], [pubkey])

        self.log.info("Export requires explicit acknowledgement and one canonical public key")
        assert_raises_rpc_error(-1, "exportflowmeshkey", source.exportflowmeshkey, pubkey)
        assert_raises_rpc_error(-8, "ack_risk=true", source.exportflowmeshkey, pubkey, False)
        for wrong_type in (None, "true", 1, []):
            assert_raises_rpc_error(-3, "", source.exportflowmeshkey, pubkey, wrong_type)
        for invalid in ("", first_secret, pubkey[:-2], pubkey + "00", "g" * 96):
            assert_raises_rpc_error(-8, "exactly 96 hex characters", source.exportflowmeshkey, invalid, True)
        for invalid in ("00" * 48, "c0" + "00" * 47, "ff" * 48):
            assert_raises_rpc_error(-8, "canonical compressed BLS", source.exportflowmeshkey, invalid, True)

        self.log.info("Export is isolated to the selected wallet and rejects watch-only wallets")
        assert_raises_rpc_error(-4, "no matching FlowMesh BLS key", source.exportflowmeshkey, second["bls_pubkey"], True)
        assert_raises_rpc_error(-4, "no matching FlowMesh BLS key", other.exportflowmeshkey, pubkey, True)
        assert_raises_rpc_error(-4, "no matching FlowMesh BLS key", destination.exportflowmeshkey, pubkey, True)
        assert_raises_rpc_error(-4, "Private keys are disabled", watch.exportflowmeshkey, pubkey, True)
        assert_raises_rpc_error(-19, "Multiple wallets", node.exportflowmeshkey, pubkey, True)

        # Match the core state after the GUI's staking-only unlock completes:
        # keys remain armed, while the actual encrypted wallet is relocked.
        armed = source.startflowmeshvalidator()
        assert_equal(armed["armed_bls_pubkeys"], [pubkey])
        before = source.getflowmeshvalidatorinfo()
        source.walletlock()
        assert_raises_rpc_error(-13, "Please enter the wallet passphrase", source.exportflowmeshkey, pubkey, True)
        assert_equal(source.getflowmeshvalidatorinfo(), before)

        self.log.info("Full unlock exports the exact key without changing key ownership or arming")
        source.walletpassphrase("isolated-export-passphrase", 600)
        unlock_deadline = source.getwalletinfo()["unlocked_until"]
        exported = source.exportflowmeshkey(pubkey.upper(), True)
        assert_equal(set(exported), {"bls_pubkey", "blssecret", "warning"})
        assert_equal(exported["bls_pubkey"], pubkey)
        assert exported["blssecret"] == first_secret, "Export did not preserve the selected key"
        assert "FN votes and historical FN rewards" in exported["warning"]
        assert "signer journal" in exported["warning"]
        assert "duplicate signer" in exported["warning"]
        assert_equal(source.getflowmeshvalidatorinfo(), before)
        assert_equal(source.getwalletinfo()["unlocked_until"], unlock_deadline)

        # Exercise positional and named CLI boolean conversion without putting
        # any private key in the command line.
        cli = node.cli("-rpcwallet=source")
        assert cli.exportflowmeshkey(pubkey, True)["blssecret"] == first_secret
        assert cli.exportflowmeshkey(bls_pubkey=pubkey, ack_risk=True)["blssecret"] == first_secret

        imported = destination.importflowmeshkey(exported["blssecret"])
        assert_equal(imported, first)
        assert destination.exportflowmeshkey(pubkey, True)["blssecret"] == first_secret
        assert_equal(destination.getflowmeshvalidatorinfo()["wallet_bls_pubkeys"], [pubkey])
        assert_equal(other.getflowmeshvalidatorinfo()["wallet_bls_pubkeys"], [second["bls_pubkey"]])
        assert_equal(source.getflowmeshvalidatorinfo(), before)

        source.walletlock()
        assert_equal(source.getwalletinfo()["unlocked_until"], 0)
        assert_raises_rpc_error(-13, "Please enter the wallet passphrase", source.exportflowmeshkey, pubkey, True)
        assert_equal(source.getflowmeshvalidatorinfo(), before)
        debug_log = node.debug_log_path.read_text(encoding="utf8")
        assert first_secret not in debug_log, "Exported fixture key entered the node log"
        assert second_secret not in debug_log, "Fixture key entered the node log"


if __name__ == "__main__":
    ExportFlowMeshKeyTest(__file__).main()
