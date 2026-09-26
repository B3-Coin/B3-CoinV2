#!/usr/bin/env python3
"""Pure packaging invariants; no network, GUI, wallet, or signing operation."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest
import tempfile
from unittest import mock

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

    def test_split_qt_umbrella_paths_preserved_for_plugin_dependencies(self):
        with tempfile.TemporaryDirectory() as path:
            prefix = Path(path)
            umbrella = prefix / "qt"
            base = prefix / "qtbase"
            extra = prefix / "lib"
            (base / "lib/cmake/Qt6").mkdir(parents=True)
            (umbrella / "lib/cmake").mkdir(parents=True)
            (umbrella / "lib/cmake/Qt6").symlink_to(base / "lib/cmake/Qt6", target_is_directory=True)
            (umbrella / "lib/QtPdf.framework").mkdir()
            extra.mkdir()
            roots = PACKAGE.macos_library_paths(umbrella / "bin/macdeployqt", umbrella / "lib/cmake/Qt6", [extra])
            self.assertEqual(roots, [umbrella / "lib", extra])
            self.assertTrue((roots[0] / "QtPdf.framework").is_dir())
            with mock.patch.object(PACKAGE, "run", return_value="") as run:
                PACKAGE.deploy_macos(umbrella / "bin/macdeployqt", prefix / "App.app", roots)
            args = run.call_args.args
            self.assertIn("-libpath=" + str(umbrella / "lib"), args)
            self.assertIn("-libpath=" + str(extra), args)
            self.assertNotIn("-no-plugins", args)
            # -libpath alone does not reach Qt's @rpath resolver. Roots must
            # be visible in the staged executable before deploying plugins.
            calls = run.call_args_list
            for root in roots:
                self.assertIn(mock.call("install_name_tool", "-add_rpath", root,
                    prefix / "App.app/Contents/MacOS" / PACKAGE.TARGET), calls[:-1])

    def test_existing_staged_rpath_not_added_twice(self):
        root = Path("/declared/qt/lib")
        load = "cmd LC_RPATH\n cmdsize 48\n path /declared/qt/lib (offset 12)"
        with mock.patch.object(PACKAGE, "run", return_value=load) as run:
            PACKAGE.deploy_macos(Path("/qt/bin/macdeployqt"), Path("/stage/Test.app"), [root])
        self.assertFalse(any(call.args[0] == "install_name_tool" for call in run.call_args_list))

    def test_macos_dependency_root_typo_refused(self):
        with tempfile.TemporaryDirectory() as path:
            prefix = Path(path)
            (prefix / "lib/cmake/Qt6").mkdir(parents=True)
            with self.assertRaisesRegex(RuntimeError, "Missing declared"):
                PACKAGE.macos_library_paths(prefix / "bin/macdeployqt", prefix / "lib/cmake/Qt6", [prefix / "missing"])

if __name__ == "__main__":
    unittest.main()
