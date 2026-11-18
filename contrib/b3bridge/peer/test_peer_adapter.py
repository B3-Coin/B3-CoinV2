#!/usr/bin/env python3
"""Availability/process boundaries; cryptographic acceptance is tested natively."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import build_peer_collector as build
import run_peer_light_client as peer


class PeerAdapterTests(unittest.TestCase):
    def test_no_framework_or_credentials_in_child_environment(self):
        with patch.dict(os.environ, {"PATH": "known-tools", "HOME": "known-home",
                                    "LD_PRELOAD": "injected", "PYTHONPATH": "injected",
                                    "HTTPS_PROXY": "private", "ETH_RPC_URL": "private",
                                    "NIMFLAGS": "injected", "FOUNDRY_MATCH_TEST": "skip"}, clear=True):
            self.assertEqual(build.clean_environment(), {"PATH": "known-tools", "HOME": "known-home"})

    def test_content_pin_rejects_changed_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "collector"
            binary.write_bytes(b"first")
            pin = hashlib.sha256(b"first").hexdigest()
            self.assertEqual(peer.pinned_binary(binary, pin), binary.resolve())
            binary.write_bytes(b"changed")
            with self.assertRaises(ValueError):
                peer.pinned_binary(binary, pin)
            with self.assertRaises(ValueError):
                peer.pinned_binary(binary, "ab")

    @unittest.skipIf(os.name != "posix", "POSIX executable/symlink boundary")
    def test_symlink_binary_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            binary, link = Path(directory) / "collector", Path(directory) / "link"
            binary.write_bytes(b"first")
            link.symlink_to(binary)
            with self.assertRaises(ValueError):
                peer.pinned_binary(link, hashlib.sha256(b"first").hexdigest())

    @unittest.skipIf(os.name != "posix", "Bounded owned POSIX version process")
    def test_oversized_backend_version_is_stopped(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "collector"
            binary.write_text("#!/usr/bin/env python3\nimport sys,time\nsys.stdout.write('x'*1048576)\nsys.stdout.flush()\ntime.sleep(20)\n")
            binary.chmod(0o700)
            with self.assertRaisesRegex(ValueError, "exceeds its bound"):
                build.bounded_version(binary)

    def test_policy_requires_disabled_ethereum_and_explicit_checkpoint(self):
        policy = {"format": "b3-ethereum-peer-policy/1", "mainnet_enabled": False,
                  "chain_id": 1, "trusted_root": "0x" + "12" * 32}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "policy.json"
            path.write_text(json.dumps(policy))
            self.assertEqual(peer.policy_input(path)[1], policy)
            for field, replacement in (("mainnet_enabled", True), ("chain_id", 42161),
                                       ("trusted_root", "latest"), ("format", "rpc-response")):
                changed = dict(policy, **{field: replacement})
                path.write_text(json.dumps(changed))
                with self.subTest(field=field), self.assertRaises(ValueError):
                    peer.policy_input(path)
            path.write_bytes(b" " * 32769)
            with self.assertRaises(ValueError):
                peer.policy_input(path)

    def test_backend_arguments_are_peer_only_and_bounded(self):
        args = peer.backend_arguments(Path("collector"), {"trusted_root": "0x" + "12" * 32}, "private-dir", 19000)
        self.assertIn("--no-el", args)
        self.assertIn("--network=mainnet", args)
        self.assertIn("--nat=none", args)
        self.assertIn("--max-peers=8", args)
        self.assertNotIn("rpc", " ".join(args).lower())
        for port in (0, 1023, 65534, True):
            with self.subTest(port=port), self.assertRaises(ValueError):
                peer.backend_arguments(Path("collector"), {"trusted_root": "root"}, "dir", port)

    def test_native_diagnostic_cannot_enable_bridge(self):
        good = {"format": "b3-ethereum-peer-verification/1", "mainnet_enabled": False,
                "arbitrum_authority": False, "writes_enabled": False}
        self.assertEqual(peer.valid_result(good), good)
        for key in ("mainnet_enabled", "arbitrum_authority", "writes_enabled"):
            with self.subTest(key=key), self.assertRaises(ValueError):
                peer.valid_result(dict(good, **{key: True}))
            missing = dict(good)
            missing.pop(key)
            with self.assertRaises(ValueError):
                peer.valid_result(missing)
        with self.assertRaises(ValueError):
            peer.valid_result({"verified": True})

    def test_historical_fresh_row_cannot_override_final_fence(self):
        fresh = {"result": "READY", "fresh_finalized_execution": True, "fenced": False}
        self.assertTrue(peer.final_anchor_verified(fresh))
        for changed in (dict(fresh, fenced=True), dict(fresh, fresh_finalized_execution=False),
                        dict(fresh, result="BAD_PROOF"), None):
            with self.subTest(changed=changed):
                self.assertFalse(peer.final_anchor_verified(changed))

    def test_native_policy_and_journal_digests_are_consistent(self):
        row = {"sequence": 1, "policy_sha256": "11" * 32, "journal_sha256": "22" * 32}
        peer.consistent_result(None, row)
        peer.consistent_result(row, dict(row))
        peer.consistent_result(row, dict(row, sequence=2, journal_sha256="33" * 32))
        for changed in (dict(row, sequence=0), dict(row, policy_sha256="44" * 32),
                        dict(row, journal_sha256="55" * 32), dict(row, sequence=True),
                        dict(row, policy_sha256="raw-file-hash")):
            with self.subTest(changed=changed), self.assertRaises(ValueError):
                peer.consistent_result(row, changed)

    def test_wrong_or_dirty_nimbus_tree_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(subprocess, "check_output", return_value="0" * 40):
                with self.assertRaises(ValueError):
                    build.checked_source(Path(directory))
            with patch.object(subprocess, "check_output", side_effect=[build.NIMBUS_COMMIT, " M beacon_chain/light_client.nim"]):
                with self.assertRaises(ValueError):
                    build.checked_source(Path(directory))
            for line in ("-" + "0" * 40 + " vendor/nim-eth", "+" + "0" * 40 + " vendor/nim-eth"):
                with patch.object(subprocess, "check_output", side_effect=[build.NIMBUS_COMMIT, "", line]):
                    with self.subTest(line=line), self.assertRaises(ValueError):
                        build.checked_source(Path(directory))

    def test_matching_nimbus_tree_accepted(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(subprocess, "check_output", side_effect=[build.NIMBUS_COMMIT, "", " " + "a" * 40 + " vendor/nim-eth\n"]):
                self.assertEqual(build.checked_source(Path(directory)), Path(directory).resolve())

    @unittest.skipIf(os.name != "posix", "Real owned POSIX pipe process test")
    def test_real_pipe_remains_ethereum_only(self):
        # These small processes test forwarding and cleanup, never BLS/SSZ.
        # Native cryptographic tests exercise actual proof acceptance.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            backend, native = root / "backend", root / "native"
            version = {"format": build.FORMAT, "upstream_commit": build.NIMBUS_COMMIT,
                       "authority": "availability-only"}
            backend.write_text("#!/usr/bin/env python3\nimport sys,json,time\n"
                               "if '--b3-version' in sys.argv: print(" + repr(json.dumps(version)) + ");sys.exit(0)\n"
                               "print('{\"kind\":\"bootstrap\",\"data\":{}}',flush=True)\n"
                               "time.sleep(20)\n")
            result = {"format": "b3-ethereum-peer-verification/1", "mainnet_enabled": False,
                      "arbitrum_authority": False, "writes_enabled": False,
                      "sequence": 0, "policy_sha256": "11" * 32, "journal_sha256": "00" * 32,
                      "fresh_finalized_execution": False, "result": "BAD_PROOF"}
            native.write_text("#!/usr/bin/env python3\nimport sys,time\n"
                              "line=sys.stdin.readline()\nassert 'bootstrap' in line\n"
                              "print(" + repr(json.dumps(result)) + ",flush=True)\n"
                              "sys.stdin.read()\n"
                              "print(" + repr(json.dumps(dict(result, result="READY", fenced=False))) + ",flush=True)\n")
            backend.chmod(0o700)
            native.chmod(0o700)
            report = peer.run_session(backend, native, root / "policy", {"trusted_root": "0x" + "12" * 32}, root / "journal", 1, 19000)
            self.assertEqual(report["result"], "NO_FRESH_ANCHOR")
            self.assertFalse(report["mainnet_enabled"])
            self.assertFalse(report["arbitrum_authority"])
            self.assertEqual(report["native_results"][0]["result"], "BAD_PROOF")
            self.assertEqual(report["final_native_result"]["result"], "READY")
            self.assertLess(report["duration_seconds"], 3)
            self.assertEqual(set(report["logs"]), {"native", "backend"})


if __name__ == "__main__":
    unittest.main()
