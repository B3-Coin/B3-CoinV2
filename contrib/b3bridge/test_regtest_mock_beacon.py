"""Offline guards of the REGTEST ONLY mock beacon; no anvil, tool or network."""
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent / "regtest"))
import mock_beacon  # noqa: E402


GVR = mock_beacon.DEFAULT_GENESIS_VALIDATORS_ROOT
FORK = mock_beacon.DEFAULT_FORK_VERSION
MAINNET_GVR = mock_beacon.ETHEREUM_MAINNET_GENESIS_VALIDATORS_ROOT
ANVIL = "http://127.0.0.1:9"
# Any executable passes the constructor's tool check; it is never run here.
TOOL = sys.executable


class DomainGuardTests(unittest.TestCase):
    def test_synthetic_domain_is_accepted(self):
        mock_beacon.check_domain(GVR, FORK, 0)
        mock_beacon.check_domain(GVR, bytes.fromhex("b3000002"), 0)

    def test_real_domains_are_refused(self):
        self.assertEqual(MAINNET_GVR.hex(),
                         "4b363db94e286120d76eb905340fdd4e54bfe9f06bf33ff6cf5ad27f511bfe95")
        with self.assertRaisesRegex(mock_beacon.MockBeaconError, "Ethereum mainnet genesis validators root"):
            mock_beacon.check_domain(MAINNET_GVR, FORK, 0)
        with self.assertRaisesRegex(mock_beacon.MockBeaconError, "must be non-zero"):
            mock_beacon.check_domain(bytes(32), FORK, 0)
        for version in ("00000000", "05000000", "90000074"):
            with self.assertRaisesRegex(mock_beacon.MockBeaconError, "outside the synthetic 0xb3 namespace"):
                mock_beacon.check_domain(GVR, bytes.fromhex(version), 0)
        for electra in (1, 364_032, None, True, 0.0):
            with self.assertRaisesRegex(mock_beacon.MockBeaconError, "electra_epoch must be 0"):
                mock_beacon.check_domain(GVR, FORK, electra)

    def test_constructor_applies_the_guard(self):
        with tempfile.TemporaryDirectory() as tmp:
            beacon = mock_beacon.SyntheticBeacon(ANVIL, TOOL, tmp, GVR, FORK, 20_477, 3)
            self.assertEqual(beacon.slot(3), 20_480)
            for gvr, fork, electra, message in (
                    (MAINNET_GVR, FORK, 0, "Ethereum mainnet genesis validators root"),
                    (GVR, "0x05000000", 0, "outside the synthetic 0xb3 namespace"),
                    (GVR, FORK, 1, "electra_epoch must be 0")):
                with self.assertRaisesRegex(mock_beacon.MockBeaconError, message):
                    mock_beacon.SyntheticBeacon(ANVIL, TOOL, tmp, gvr, fork, 20_477, 3,
                                                electra_epoch=electra)
            for url in ("https://ethereum-rpc.example.invalid", "http://10.0.0.1:8545"):
                with self.assertRaisesRegex(mock_beacon.MockBeaconError, "refusing non-local endpoint"):
                    mock_beacon.SyntheticBeacon(url, TOOL, tmp, GVR, FORK, 20_477, 3)


class LocalUrlGuardTests(unittest.TestCase):
    def test_only_plain_local_http_is_accepted(self):
        for url in ("http://127.0.0.1:8545", "http://localhost:5052"):
            self.assertEqual(mock_beacon.check_local_url(url), url)
        for url in ("https://127.0.0.1:8545", "http://10.0.0.1:8545",
                    "http://user:pass@127.0.0.1:8545", "http://user@localhost:8545",
                    "http://127.0.0.1.example.invalid:8545", "ws://localhost:8545",
                    "http://localhost:notaport", "", None):
            with self.assertRaises(mock_beacon.MockBeaconError, msg=url):
                mock_beacon.check_local_url(url)

    def test_eth_rpc_refuses_before_sending(self):
        with self.assertRaisesRegex(mock_beacon.MockBeaconError, "refusing non-local endpoint"):
            mock_beacon.eth_rpc("https://ethereum-rpc.example.invalid", "eth_blockNumber", [])


class PinFileTests(unittest.TestCase):
    LIGHT = {
        "trusted_checkpoint_root": "0x" + "c1" * 32,
        "trusted_checkpoint_slot": 20_480,
        "checkpoint_execution_block": 3,
        "genesis_validators_root": "0x" + GVR.hex(),
        "fork_schedule": [{"activation_epoch": 0, "fork_version": "0x" + FORK.hex()}],
        "fork_schedule_valid_through_epoch": 1_664,
        "electra_epoch": 0,
    }

    def beacon_from(self, drop=(), **changes):
        light = {key: value for key, value in {**self.LIGHT, **changes}.items() if key not in drop}
        pins = {"format": "b3-regtest-bridge-pins/1", "v1_decentralized": {"light_client": light}}
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "pins.json")
            with open(path, "w", encoding="utf8") as f:
                json.dump(pins, f)
            return mock_beacon.beacon_from_pins(path, ANVIL, TOOL, tmp)

    def test_pin_file_domain_is_checked_before_anvil_is_read(self):
        with self.assertRaisesRegex(mock_beacon.MockBeaconError, "Ethereum mainnet genesis validators root"):
            self.beacon_from(genesis_validators_root="0x" + MAINNET_GVR.hex())
        with self.assertRaisesRegex(mock_beacon.MockBeaconError, "outside the synthetic 0xb3 namespace"):
            self.beacon_from(fork_schedule=[{"activation_epoch": 0, "fork_version": "0x05000000"}])
        with self.assertRaisesRegex(mock_beacon.MockBeaconError, "electra_epoch must be 0"):
            self.beacon_from(electra_epoch=1)
        with self.assertRaisesRegex(mock_beacon.MockBeaconError, "malformed pin file: missing 'electra_epoch'"):
            self.beacon_from(drop=("electra_epoch",))


if __name__ == "__main__":
    unittest.main()
