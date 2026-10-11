#!/usr/bin/env python3
"""Offline trigger/guard regressions, not a YAML parser or a GitHub CI run.

Only the intentionally simple push allowlist shape is parsed here. Its paths
use exact names or directory/** prefixes; unsupported syntax fails this check
rather than pretending to emulate every GitHub glob/YAML feature. No network,
wallet, node, third-party parser, or signing operation is used.
"""
from pathlib import Path
import re
import unittest


SOURCE = Path(__file__).resolve().parents[2]
WORKFLOW = SOURCE / ".github/workflows/flowmesh-regtest-build.yml"


def push_paths(text):
    section = re.search(r"(?ms)^  push:\n(.*?)(?=^\S)", text)
    if section is None:
        raise ValueError("Expected a push section below on")
    paths = re.search(r"(?ms)^    paths:\n(.*)\Z", section[1])
    if paths is None:
        raise ValueError("Expected explicit push paths")
    result = []
    for line in paths[1].splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        item = re.fullmatch(r"      - '([A-Za-z0-9_./-]+(?:/\*\*)?)'", line)
        if item is None:
            raise ValueError(f"Unsupported trigger shape: {line!r}")
        result.append(item[1])
    if not result or len(set(result)) != len(result):
        raise ValueError("Empty or duplicate push paths")
    return result


def selected(path, patterns):
    return any(path.startswith(pattern[:-2]) if pattern.endswith("/**")
               else path == pattern for pattern in patterns)


class WorkflowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.text = WORKFLOW.read_text(encoding="utf-8")
        cls.paths = push_paths(cls.text)

    def assert_selected(self, paths):
        for path in paths:
            with self.subTest(path=path):
                self.assertTrue(selected(path, self.paths), path)

    def test_native_and_qt_repair_dependencies_trigger(self):
        self.assert_selected((
            "src/node/staking.cpp", "src/node/staking.h",
            "src/node/regtest_staking_tip_sync.h", "src/net_processing.cpp",
            "src/validation.cpp", "src/chainparams.cpp",
            "src/modern/finality_signer.cpp", "src/wallet/wallet.cpp",
            "src/flowmesh/engine.cpp", "src/qt/flowmeshpage.cpp",
            "src/qt/test/flowmeshclosedtest_policytests.cpp",
            "src/qt/test/b3createwalletdialogtests.cpp",
            "src/test/modern_pos_tests.cpp", "src/qt/test/CMakeLists.txt",
        ))

    def test_build_and_dependency_inputs_trigger(self):
        self.assert_selected((
            "CMakeLists.txt", "cmake/module/CheckLinkerFlags.cmake",
            "depends/packages/qt_details.mk", "depends/patches/qt/fix.patch",
            "share/qt/Info.plist.in", "COPYING",
        ))

    def test_packaging_profile_and_guard_inputs_trigger(self):
        self.assert_selected((
            ".github/workflows/flowmesh-regtest-build.yml",
            "contrib/flowmesh-regtest/package.py",
            "contrib/flowmesh-regtest/test_package.py",
            "contrib/flowmesh-regtest/test_workflow.py",
            "contrib/flowmesh-regtest/profile-vps-20260928.json",
            "contrib/flowmesh-regtest/session3-ca.pem",
            "contrib/flowmesh-regtest/README.md",
            "contrib/closed-test/collect-notices.py",
            "contrib/windeploy/package_portable.py",
            "contrib/macdeploy/normalize_bundled_libraries.rb",
        ))

    def test_unrelated_documents_and_private_evidence_do_not_trigger(self):
        for path in ("doc/design/example.md", "operations/private/wallet.dat",
                     "README.md", "contracts/src/example.sol"):
            with self.subTest(path=path):
                self.assertFalse(selected(path, self.paths))

    def test_artifact_only_branch_and_read_only_permission_contract(self):
        self.assertRegex(self.text, r"(?m)^    branches: \[flowmeshV2-dev\]$")
        permission = re.search(r"(?ms)^permissions:\n(.*?)(?=^\S)", self.text)
        self.assertIsNotNone(permission)
        self.assertEqual(permission[1].strip(), "contents: read")
        self.assertNotIn("pull_request:", self.text)
        self.assertNotIn("tags:", self.text)
        self.assertNotIn("permissions: write-all", self.text)

    def test_all_jobs_are_timed_and_checkouts_do_not_persist_credentials(self):
        jobs = re.search(r"(?ms)^jobs:\n(.*)\Z", self.text)
        self.assertIsNotNone(jobs)
        job_blocks = re.split(r"(?m)^  [a-z][a-z0-9-]*:\n", jobs[1])[1:]
        self.assertEqual(len(job_blocks), 4)
        for block in job_blocks:
            timeout = re.search(r"(?m)^    timeout-minutes: (\d+)$", block)
            self.assertIsNotNone(timeout)
            self.assertGreater(int(timeout[1]), 0)
            self.assertLessEqual(int(timeout[1]), 240)
        self.assertEqual(self.text.count("uses: actions/checkout@v4"), 3)
        self.assertEqual(self.text.count("persist-credentials: false"), 3)

    def test_trigger_guard_runs_and_optional_policy_tests_remain(self):
        self.assertIn("run: python3 -B contrib/flowmesh-regtest/test_workflow.py -v", self.text)
        self.assertRegex(self.text, r"(?m)^        default: true$")
        self.assertIn("run: python3 -B contrib/flowmesh-regtest/test_package.py -v", self.text)
        self.assertEqual(self.text.count("-DBUILD_FLOWMESH_REGTEST_POLICY_TESTS=${{ env.RUN_POLICY_TESTS }}"), 2)
        self.assertIn("build/bin/test_b3_flowmeshclosed-policy", self.text)
        self.assertIn("test_b3_flowmeshclosed-policy.exe", self.text)

    def test_session_guards_are_unconditional_and_before_platform_jobs(self):
        profile = self.text.split("  profile:\n", 1)[1].split("  windows-cross:\n", 1)[0]
        for script in ("test_session_preflight.py", "session_preflight.py"):
            pattern = r"(?m)^      - name: [^\n]+\n        run: python3 -B contrib/flowmesh-regtest/" + script.replace(".", r"\.") + r"(?: -v)?$"
            self.assertRegex(profile, pattern)


if __name__ == "__main__":
    unittest.main()
