#!/usr/bin/env python3
"""Offline process boundaries for the read-only native peer proof consumer.

Run against an explicitly built b3-bridge-peercheck executable. Cryptographic
positive vectors live in bridge_peer_light_client_tests; these tests exercise
the real JSON reader, owned-file checks, lock and journal corruption behavior.
No RPC, peer connection, signing key or chain transaction is used.
"""
import argparse
import json
import os
from pathlib import Path
import select
import subprocess
import tempfile
import time
import unittest

VERIFIER = None
ENVIRONMENT = {"PATH": "/usr/bin:/bin", "LANG": "C", "LC_ALL": "C", "TZ": "UTC"}


@unittest.skipUnless(os.name == "posix", "native durable peer journal is Linux/macOS only; use WSL")
class PeercheckProcess(unittest.TestCase):
    def setUp(self):
        if VERIFIER is None:
            self.skipTest("requires explicit --peercheck binary")
        self.temporary = tempfile.TemporaryDirectory(prefix="b3-peercheck-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.policy_path = self.directory / "policy.json"
        self.journal_path = self.directory / "proofs.journal"
        now = (int(time.time()) - 1606824023) // 12
        self.policy = {
            "format": "b3-ethereum-peer-policy/1", "mainnet_enabled": False, "chain_id": 1,
            "genesis_validators_root": "0x4b363db94e286120d76eb905340fdd4e54bfe9f06bf33ff6cf5ad27f511bfe95",
            "forks": [{"epoch": epoch, "version": f"0x{version:02x}000000"} for epoch, version in
                      ((0, 0), (74240, 1), (144896, 2), (194048, 3), (269568, 4), (364032, 5), (411392, 6))],
            "electra_epoch": 364032, "min_participants": 342,
            # This test has no real bootstrap; an arbitrary nonzero trust root
            # cannot yield authority without its exact committee/header proof.
            "trusted_root": "0x" + "01" * 32, "trusted_slot": now - 64,
            "fork_schedule_valid_through_epoch": now // 32 + 64,
            "max_sync_lag_slots": 8192, "max_recovery_lag_slots": 8192,
            "genesis_time": 1606824023, "seconds_per_slot": 12,
        }
        self.write_policy()

    def write_policy(self):
        self.policy_path.write_text(json.dumps(self.policy), encoding="utf-8")
        self.policy_path.chmod(0o600)

    def command(self, policy=None, journal=None):
        return [str(VERIFIER), "--policy", str(policy or self.policy_path),
                "--journal", str(journal or self.journal_path)]

    def run_tool(self, text="", **paths):
        return subprocess.run(self.command(**paths), input=text, text=True, capture_output=True,
                              timeout=10, env=ENVIRONMENT, check=False)

    def rows(self, completed):
        return [json.loads(line) for line in completed.stdout.splitlines()]

    def assert_disabled(self, row):
        self.assertEqual(row["format"], "b3-ethereum-peer-verification/1")
        self.assertIs(row["mainnet_enabled"], False)
        self.assertIs(row["arbitrum_authority"], False)
        self.assertIs(row["writes_enabled"], False)
        self.assertIs(row["fresh_finalized_execution"], False)
        self.assertNotIn("ethereum_finalized_execution", row)
        self.assertNotIn("facts", row)
        self.assertEqual(row["sequence"], 0)

    def test_clean_eof_rechecks_disabled_final_status(self):
        completed = self.run_tool()
        self.assertEqual(completed.returncode, 0, completed.stderr)
        rows = self.rows(completed)
        self.assertEqual(len(rows), 2)
        self.assertEqual([row["result"] for row in rows], ["READY", "READY"])
        for row in rows:
            self.assert_disabled(row)
        self.assertEqual(self.journal_path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.journal_path.read_bytes(), b"")

    def test_duplicate_extra_and_claimed_verified_fields_do_not_enter_journal(self):
        inputs = [
            '{"kind":"bootstrap","kind":"update","data":{}}',
            '{"kind":"bootstrap","data":{},"verified":true}',
            '{"kind":"optimistic_update","data":{"verified":true}}',
            '{"kind":"bootstrap","data":{"header":{},"header":{}}}',
        ]
        completed = self.run_tool("\n".join(inputs) + "\n")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        rows = self.rows(completed)
        self.assertEqual([row["result"] for row in rows[1:-1]], ["MALFORMED_PEER_PROOF"] * len(inputs))
        for row in rows:
            self.assert_disabled(row)
        self.assertEqual(self.journal_path.read_bytes(), b"")

    def test_size_depth_and_incomplete_eof_frames_are_bounded(self):
        completed = self.run_tool(" " * 262145 + "\n" + "[" * 20 + "]" * 20 + "\n" + '{"kind":')
        self.assertEqual(completed.returncode, 0, completed.stderr)
        rows = self.rows(completed)
        self.assertEqual([row["result"] for row in rows[1:-1]],
                         ["JSON_SIZE_LIMIT", "MALFORMED_PEER_PROOF", "MALFORMED_PEER_PROOF"])
        self.assertEqual(rows[-1]["result"], "READY")
        self.assertLess(len(completed.stdout), 8192)
        self.assertEqual(self.journal_path.read_bytes(), b"")

    def test_wrong_policy_cannot_create_a_journal(self):
        for key, value in (("mainnet_enabled", True), ("chain_id", 42161),
                           ("min_participants", 341), ("max_recovery_lag_slots", 8193),
                           ("max_recovery_lag_slots", 0), ("seconds_per_slot", 1),
                           ("format", "b3-ethereum-peer-policy/2"), ("verified", True)):
            with self.subTest(field=key, value=value):
                original = dict(self.policy)
                self.policy[key] = value
                self.write_policy()
                completed = self.run_tool()
                self.assertNotEqual(completed.returncode, 0)
                self.assertEqual(completed.stdout, "")
                self.assertFalse(self.journal_path.exists())
                self.policy = original

    def test_duplicate_policy_field_is_not_silently_overwritten(self):
        text = self.policy_path.read_text(encoding="utf-8")
        self.policy_path.write_text(text[:-1] + ',"mainnet_enabled":false}', encoding="utf-8")
        completed = self.run_tool()
        self.assertNotEqual(completed.returncode, 0)
        self.assertFalse(self.journal_path.exists())

    def test_policy_symlink_hardlink_and_mutable_file_are_refused(self):
        alias = self.directory / "policy-alias"
        alias.symlink_to(self.policy_path)
        completed = self.run_tool(policy=alias)
        self.assertNotEqual(completed.returncode, 0)
        alias.unlink()
        os.link(self.policy_path, alias)
        completed = self.run_tool()
        self.assertNotEqual(completed.returncode, 0)
        alias.unlink()
        self.policy_path.chmod(0o666)
        completed = self.run_tool()
        self.assertNotEqual(completed.returncode, 0)
        self.assertFalse(self.journal_path.exists())

    def test_journal_symlink_hardlink_and_broad_permissions_are_refused(self):
        target = self.directory / "unrelated"
        target.write_bytes(b"retained-data")
        target.chmod(0o600)
        self.journal_path.symlink_to(target)
        self.assertNotEqual(self.run_tool().returncode, 0)
        self.journal_path.unlink()
        os.link(target, self.journal_path)
        self.assertNotEqual(self.run_tool().returncode, 0)
        self.journal_path.unlink()
        self.journal_path.write_bytes(b"retained-data")
        self.journal_path.chmod(0o644)
        self.assertNotEqual(self.run_tool().returncode, 0)
        self.assertEqual(target.read_bytes(), b"retained-data")
        self.assertEqual(self.journal_path.read_bytes(), b"retained-data")

    def test_empty_and_torn_existing_journals_are_preserved_and_refused(self):
        for content in (b"", b"B3ETHP01", b"B3ETHP01" + b"\0" * 100):
            with self.subTest(length=len(content)):
                self.journal_path.write_bytes(content)
                self.journal_path.chmod(0o600)
                completed = self.run_tool()
                self.assertNotEqual(completed.returncode, 0)
                self.assertEqual(self.journal_path.read_bytes(), content)
                self.assertEqual(completed.stdout, "")

    def test_second_writer_cannot_open_the_same_journal(self):
        first = subprocess.Popen(self.command(), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True, env=ENVIRONMENT)
        try:
            readable, _, _ = select.select([first.stdout], [], [], 10)
            self.assertTrue(readable, "native consumer did not report ready")
            ready = json.loads(first.stdout.readline())
            self.assertEqual(ready["result"], "READY")
            second = self.run_tool()
            self.assertNotEqual(second.returncode, 0)
            self.assertIn("UNSAFE_OR_LOCKED_FILE", second.stderr)
            tail, errors = first.communicate(input="", timeout=10)
            self.assertEqual(first.returncode, 0, errors)
            self.assertEqual(json.loads(tail.splitlines()[-1])["result"], "READY")
        finally:
            if first.poll() is None:
                first.kill()
                first.communicate(timeout=10)

    def test_writable_ancestor_cannot_redirect_the_journal(self):
        outer = self.directory / "outer"
        inner = outer / "inner"
        inner.mkdir(parents=True, mode=0o700)
        outer.chmod(0o777)
        completed = self.run_tool(journal=inner / "proofs.journal")
        self.assertNotEqual(completed.returncode, 0)
        self.assertFalse((inner / "proofs.journal").exists())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--peercheck", type=Path, required=True)
    args, remainder = parser.parse_known_args()
    VERIFIER = args.peercheck.absolute()
    if not VERIFIER.is_file():
        parser.error("--peercheck must name the built native consumer")
    unittest.main(argv=[__file__, *remainder])
