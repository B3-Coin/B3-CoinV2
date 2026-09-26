#!/usr/bin/env python3
"""Pure packaging invariants; no network, GUI, wallet, or signing operation."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest
import tempfile

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("regtest_package", HERE / "package.py")
PACKAGE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PACKAGE)

class PackageTests(unittest.TestCase):
    def setUp(self):
        self.raw = (HERE / "profile-vps-20260926.json").read_bytes()
        self.ca = (HERE / "regtest-ca.pem").read_bytes()
        self.profile = json.loads(self.raw)

    def test_approved_profile_and_ca(self):
        self.assertEqual(PACKAGE.validate_profile(self.raw, self.ca)["profile_id"], PACKAGE.PROFILE)

    def test_network_version_identity_and_endpoints_are_fixed(self):
        mutations = [("schema", 1), ("ready", False), ("network", "main"),
            ("profile_id", "other"), ("public_session_approval", "other"),
            ("regtest_profile", "other"), ("b3_peer", "88.216.63.161:5647"),
            ("https_endpoints", ["http://88.216.63.161:18580/flowmesh/v1"]),
            ("https_endpoints", ["https://88.216.63.161:5650/flowmesh/v1"])]
        for key, value in mutations:
            with self.subTest(key=key, value=value):
                changed = copy.deepcopy(self.profile)
                changed[key] = value
                with self.assertRaises(RuntimeError):
                    PACKAGE.validate_profile(json.dumps(changed).encode(), self.ca)

    def test_changed_ca_refused(self):
        with self.assertRaises(RuntimeError):
            PACKAGE.validate_profile(self.raw, self.ca + b"\n")

    def test_extra_profile_fields_refused(self):
        changed = copy.deepcopy(self.profile)
        changed["wallet"] = "mainnet"
        with self.assertRaises(RuntimeError):
            PACKAGE.validate_profile(json.dumps(changed).encode(), self.ca)

    def test_stale_compiled_identity_refused(self):
        with tempfile.TemporaryDirectory() as path:
            build = Path(path)
            (build / "src").mkdir()
            commit = "1" * 40
            header = build / "src/bitcoin-build-info.h"
            binary = build / "gui"
            header.write_text('#define BUILD_GIT_COMMIT "' + commit[:12] + '"\n')
            binary.write_bytes(PACKAGE.VERSION.encode() + b" " + commit[:12].encode())
            PACKAGE.validate_compiled_identity(build, binary, commit)
            binary.write_bytes(PACKAGE.VERSION.encode() + b" oldcommit")
            with self.assertRaises(RuntimeError):
                PACKAGE.validate_compiled_identity(build, binary, commit)
            header.write_text('#define BUILD_GIT_COMMIT "' + commit[:12] + '-dirty"\n')
            with self.assertRaises(RuntimeError):
                PACKAGE.validate_compiled_identity(build, binary, commit)

    def test_matching_but_unapproved_ca_refused(self):
        changed = copy.deepcopy(self.profile)
        changed["ca_pem"] = self.ca.decode() + "\n"
        with self.assertRaises(RuntimeError):
            PACKAGE.validate_profile(json.dumps(changed).encode(), self.ca + b"\n")

    def test_private_key_marker_refused(self):
        changed = copy.deepcopy(self.profile)
        changed["operator"] = "PRIVATE KEY"
        with self.assertRaises(RuntimeError):
            PACKAGE.validate_profile(json.dumps(changed).encode(), self.ca)

    def test_oversized_profile_refused(self):
        with self.assertRaises(RuntimeError):
            PACKAGE.validate_profile(self.raw + b" " * 32769, self.ca)

    def test_minimum_comparison_not_lexical(self):
        self.assertGreater(PACKAGE.version_tuple("26.0"), PACKAGE.version_tuple("15.0"))
        self.assertGreater(PACKAGE.version_tuple("15.10"), PACKAGE.version_tuple("15.9"))
        self.assertEqual(PACKAGE.version_tuple("15"), PACKAGE.version_tuple("15.0"))

    def test_bad_minimum_refused(self):
        for value in ("", "15; true", "15.beta", "-1", "15.0.0.0.1"):
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                PACKAGE.version_tuple(value)

    def test_public_docs_contain_no_operator_path(self):
        for name in ("README.md", "profile-vps-20260926.json"):
            raw = (HERE / name).read_bytes()
            self.assertNotIn(b"/Users/", raw)
            self.assertNotIn(b"-----BEGIN PRIVATE KEY", raw)

if __name__ == "__main__":
    unittest.main()
