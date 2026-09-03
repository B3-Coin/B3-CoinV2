#!/usr/bin/env python3
# Copyright (c) 2026 The B3 Coin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.
"""End-to-end recovery of legacy dumpwallet private keys."""

from decimal import Decimal

from test_framework.address import key_to_p2pkh
from test_framework.descriptors import descsum_create
from test_framework.script import hash160
from test_framework.script_util import key_to_p2pk_script, key_to_p2pkh_script
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error
from test_framework.wallet import MiniWallet
from test_framework.wallet_util import generate_keypair

LEGACY_COIN = 1_000_000


class ImportLegacyWalletDumpTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    @staticmethod
    def record(wif, pubkey, timestamp, role):
        return f"{wif} {timestamp} {role} # addr={key_to_p2pkh(pubkey)}"

    def run_test(self):
        node = self.nodes[0]
        source = MiniWallet(node)
        compressed_wif, compressed_pubkey = generate_keypair(compressed=True, wif=True)
        uncompressed_wif, uncompressed_pubkey = generate_keypair(compressed=False, wif=True)
        blank_wif, blank_pubkey = generate_keypair(compressed=True, wif=True)

        source.send_to(
            from_node=node,
            scriptPubKey=key_to_p2pkh_script(compressed_pubkey),
            amount=1 * LEGACY_COIN,
        )
        source.send_to(
            from_node=node,
            scriptPubKey=key_to_p2pk_script(uncompressed_pubkey),
            amount=2 * LEGACY_COIN,
        )
        source.send_to(
            from_node=node,
            scriptPubKey=key_to_p2pkh_script(blank_pubkey),
            amount=4 * LEGACY_COIN,
        )
        self.generate(source, 1)

        first = self.record(compressed_wif, compressed_pubkey, "2017-09-18T12:34:56Z", "label=Cold%20Storage%25")
        second = self.record(uncompressed_wif, uncompressed_pubkey, "2018-01-02T03:04:05Z", "change=1")
        dump_path = node.datadir_path / "legacy-wallet.dump"
        dump_path.write_text(
            "# Wallet dump created by B3-Coin v3.1.2.2\r\n"
            "# authentic comments are ignored\r\n\r\n"
            f"{first}\r\n{second}\r\n{first}\r\n"
            "# End of dump\r\n",
            encoding="utf8",
        )

        node.createwallet(wallet_name="recovery", passphrase="recovery-passphrase")
        recovery = node.get_wallet_rpc("recovery")
        assert_raises_rpc_error(-13, "Please enter the wallet passphrase", recovery.importlegacywalletdump, str(dump_path))
        recovery.walletpassphrase("recovery-passphrase", 600)

        log_offset = node.debug_log_path.stat().st_size
        result = recovery.importlegacywalletdump(str(dump_path))
        assert_equal(result["keys_imported"], 2)
        assert_equal(result["keys_already_present"], 0)
        assert_equal(result["rescan_complete"], True)
        assert_equal(recovery.getbalances()["mine"]["trusted"], Decimal("0.003000000"))

        log_delta = node.debug_log_path.read_text(encoding="utf8")[log_offset:]
        assert compressed_wif not in log_delta
        assert uncompressed_wif not in log_delta

        address_info = recovery.getaddressinfo(key_to_p2pkh(compressed_pubkey))
        assert_equal(address_info["ismine"], True)
        assert_equal(address_info["labels"], ["Cold Storage%"])
        assert_equal(recovery.getaddressinfo(key_to_p2pkh(uncompressed_pubkey))["ismine"], True)

        descriptors = recovery.listdescriptors()["descriptors"]
        for pubkey, timestamp in (
            (compressed_pubkey, 1505738096),
            (uncompressed_pubkey, 1514862245),
        ):
            fingerprint = hash160(pubkey)[:4].hex()
            expected = descsum_create(f"combo([{fingerprint}]{pubkey.hex()})")
            matches = [entry for entry in descriptors if entry["desc"] == expected]
            assert_equal(len(matches), 1)
            assert_equal(matches[0]["timestamp"], timestamp)

        repeat = recovery.importlegacywalletdump(str(dump_path))
        assert_equal(repeat["keys_imported"], 0)
        assert_equal(repeat["keys_already_present"], 2)
        assert_equal(recovery.getbalances()["mine"]["trusted"], Decimal("0.003000000"))

        node.createwallet(wallet_name="sink")
        sink = node.get_wallet_rpc("sink")

        blank_dump_path = node.datadir_path / "blank-recovery.dump"
        blank_dump_path.write_text(
            "# Wallet dump created by B3-Coin v3.1.2.2\n"
            f"{self.record(blank_wif, blank_pubkey, '2018-02-03T04:05:06Z', 'label=Blank%20Recovery')}\n"
            "# End of dump\n",
            encoding="utf8",
        )
        node.createwallet(wallet_name="blank-recovery", blank=True, passphrase="blank-passphrase")
        blank_recovery = node.get_wallet_rpc("blank-recovery")
        blank_recovery.walletpassphrase("blank-passphrase", 600)
        assert_equal(blank_recovery.getwalletinfo()["blank"], True)
        assert_raises_rpc_error(-4, "This wallet has no available keys", blank_recovery.getnewaddress)

        blank_result = blank_recovery.importlegacywalletdump(str(blank_dump_path))
        assert_equal(blank_result["keys_imported"], 1)
        assert_equal(blank_recovery.getwalletinfo()["blank"], False)
        assert_equal(blank_recovery.getbalances()["mine"]["trusted"], Decimal("0.004000000"))
        blank_recovery.getnewaddress(address_type="legacy")
        blank_recovery.getrawchangeaddress(address_type="legacy")

        # Funding a transaction for less than the recovered UTXO must create
        # change. This is the same wallet capability bindfinalitykey needs.
        funded = blank_recovery.walletcreatefundedpsbt(
            inputs=[],
            outputs={sink.getnewaddress(address_type="legacy"): Decimal("0.001000000")},
            feeRate=Decimal("0.000100000"),
        )
        assert funded["changepos"] >= 0

        spend = recovery.sendall(recipients=[sink.getnewaddress()])
        assert_equal(spend["complete"], True)

        node.createwallet(wallet_name="malformed")
        malformed = node.get_wallet_rpc("malformed")
        malformed_path = node.datadir_path / "malformed-legacy-wallet.dump"
        malformed_path.write_text(
            "# Wallet dump created by B3-Coin v3.1.2.2\n"
            f"{first}\n"
            "this second record is broken\n"
            "# End of dump\n",
            encoding="utf8",
        )
        before = malformed.listdescriptors()["descriptors"]
        assert_raises_rpc_error(-8, "Malformed key record on line 3", malformed.importlegacywalletdump, str(malformed_path))
        assert_equal(malformed.listdescriptors()["descriptors"], before)
        assert_equal(malformed.getaddressinfo(key_to_p2pkh(compressed_pubkey))["ismine"], False)


if __name__ == "__main__":
    ImportLegacyWalletDumpTest(__file__).main()
