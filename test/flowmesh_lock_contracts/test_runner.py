#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Runner rejection controls; not node, sanitizer, or runtime-lock tests."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
from pathlib import Path
import signal
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("flowmesh_lock_contract_runner", ROOT / "ci" / "check_flowmesh_lock_contracts.py")
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


class DiscoveryTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="flowmesh-lock-contracts-")
        self.addCleanup(self.temporary.cleanup)
        self.fixtures = Path(self.temporary.name)
        (self.fixtures / "lock_contract_fixture.h").touch()
        for case in RUNNER.CASES:
            (self.fixtures / case.filename).touch()

    def test_exact_manifest(self):
        RUNNER.discover(self.fixtures)

    def test_missing_fixture(self):
        (self.fixtures / RUNNER.CASES[0].filename).unlink()
        with self.assertRaisesRegex(RUNNER.RunnerError, "missing="):
            RUNNER.discover(self.fixtures)

    def test_extra_fixture(self):
        (self.fixtures / "unlisted.cpp").touch()
        with self.assertRaisesRegex(RUNNER.RunnerError, "unexpected=.*unlisted.cpp"):
            RUNNER.discover(self.fixtures)

    def test_nested_extra_fixture(self):
        (self.fixtures / "nested").mkdir()
        (self.fixtures / "nested" / "unlisted.cpp").touch()
        with self.assertRaisesRegex(RUNNER.RunnerError, "unexpected=.*nested/unlisted.cpp"):
            RUNNER.discover(self.fixtures)

    def test_empty_manifest(self):
        with self.assertRaisesRegex(RUNNER.RunnerError, "nonempty"):
            RUNNER.discover(self.fixtures, ())

    def test_duplicate_manifest(self):
        with self.assertRaisesRegex(RUNNER.RunnerError, "duplicates"):
            RUNNER.discover(self.fixtures, (RUNNER.CASES[0], RUNNER.CASES[0]))

    def test_invalid_manifest_path(self):
        with self.assertRaisesRegex(RUNNER.RunnerError, "invalid filename"):
            RUNNER.discover(self.fixtures, (RUNNER.Case("../outside.cpp"),))

    def test_missing_common_header(self):
        (self.fixtures / "lock_contract_fixture.h").unlink()
        with self.assertRaisesRegex(RUNNER.RunnerError, "common fixture header"):
            RUNNER.discover(self.fixtures)


class DiagnosticTests(unittest.TestCase):
    source = Path("/fixture/excluded_entry_rejected.cpp")
    case = next(case for case in RUNNER.CASES if case.filename == "excluded_entry_rejected.cpp")
    intended = f"{source}:10:5: error: cannot call function 'ChainOperation' while mutex 'chain' is held [-Werror,-Wthread-safety-analysis]\n1 error generated.\n"

    @staticmethod
    def result(exit_code=1, stderr="", stdout="", timed_out=False):
        return RUNNER.ChildResult(exit_code, stdout, stderr, timed_out, 0.01)

    def test_exact_intended_error(self):
        self.assertIsNone(RUNNER.check_result(self.case, self.result(stderr=self.intended), self.source))

    def test_repository_empty_capability_label(self):
        text = self.intended.replace("while mutex ", "while  ")
        self.assertIsNone(RUNNER.check_result(self.case, self.result(stderr=text), self.source))

    def test_clean_positive(self):
        self.assertIsNone(RUNNER.check_result(RUNNER.CASES[0], self.result(0), self.source))

    def test_warning_not_a_positive_pass(self):
        self.assertIsNotNone(RUNNER.check_result(RUNNER.CASES[0], self.result(0, "warning: ignored attribute\n"), self.source))

    def test_missing_header_not_a_negative_pass(self):
        text = f"{self.source}:1:10: fatal error: 'threadsafety.h' file not found\n1 error generated.\n"
        self.assertIsNotNone(RUNNER.check_result(self.case, self.result(stderr=text), self.source))

    def test_syntax_error_not_a_negative_pass(self):
        text = f"{self.source}:10:5: error: expected ';'\n1 error generated.\n"
        self.assertIsNotNone(RUNNER.check_result(self.case, self.result(stderr=text), self.source))

    def test_wrong_capability_not_a_pass(self):
        text = self.intended.replace("mutex 'chain'", "mutex 'market'")
        self.assertIsNotNone(RUNNER.check_result(self.case, self.result(stderr=text), self.source))

    def test_wrong_function_not_a_pass(self):
        text = self.intended.replace("'ChainOperation'", "'Unrelated'")
        self.assertIsNotNone(RUNNER.check_result(self.case, self.result(stderr=text), self.source))

    def test_wrong_source_not_a_pass(self):
        text = self.intended.replace(str(self.source), "/different/file.cpp")
        self.assertIsNotNone(RUNNER.check_result(self.case, self.result(stderr=text), self.source))

    def test_extra_diagnostic_not_a_pass(self):
        text = self.intended + f"{self.source}:1:1: warning: unknown warning option\n"
        self.assertIsNotNone(RUNNER.check_result(self.case, self.result(stderr=text), self.source))

    def test_duplicate_intended_errors_not_a_pass(self):
        self.assertIsNotNone(RUNNER.check_result(self.case, self.result(stderr=self.intended * 2), self.source))

    def test_wrong_exit_not_a_pass(self):
        for exit_code in (0, 2, -signal.SIGKILL):
            with self.subTest(exit_code=exit_code):
                self.assertIsNotNone(RUNNER.check_result(self.case, self.result(exit_code, self.intended), self.source))

    def test_timeout_not_a_pass(self):
        self.assertIsNotNone(RUNNER.check_result(self.case, self.result(1, self.intended, timed_out=True), self.source))

    def test_unexpected_stdout_not_a_pass(self):
        self.assertIsNotNone(RUNNER.check_result(self.case, self.result(1, self.intended, "unexpected\n"), self.source))

    def test_source_echo_not_a_pass(self):
        text = f"{self.source}:10:5: error: expected expression\n{self.intended}"
        self.assertIsNotNone(RUNNER.check_result(self.case, self.result(stderr=text), self.source))

    def test_missing_warning_category_not_a_pass(self):
        text = self.intended.replace(" [-Werror,-Wthread-safety-analysis]", "")
        self.assertIsNotNone(RUNNER.check_result(self.case, self.result(stderr=text), self.source))


class ControlTests(unittest.TestCase):
    def test_expected_semantic_case_inventory(self):
        self.assertEqual({case.filename for case in RUNNER.CASES}, {
            "exclusion_body_allowed.cpp", "unannotated_helper_allowed.cpp",
            "unannotated_virtual_allowed.cpp", "excluded_entry_rejected.cpp",
            "blocking_boundary_rejected.cpp", "guarded_access_allowed.cpp",
            "guarded_access_rejected.cpp", "required_lock_allowed.cpp",
            "required_lock_rejected.cpp", "annotated_virtual_rejected.cpp",
        })
        self.assertEqual(sum(case.expected_exit == 0 for case in RUNNER.CASES), 5)
        self.assertEqual(sum(case.expected_exit == 1 for case in RUNNER.CASES), 5)

    def test_missing_compiler_is_nonzero_not_skip(self):
        with patch.object(RUNNER.shutil, "which", return_value=None), redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertEqual(RUNNER.main([]), 2)

    def test_nonclang_refused(self):
        version = RUNNER.ChildResult(0, "g++ (GCC) 14\n", "", False, 0.01)
        with patch.object(RUNNER, "run_child", return_value=version), redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RUNNER.RunnerError, "Clang"):
                RUNNER.compiler_prerequisites("not-clang")

    def test_clang_predefines_required(self):
        version = RUNNER.ChildResult(0, "clang version 17\n", "", False, 0.01)
        macros = RUNNER.ChildResult(0, "#define __GNUC__ 4\n", "", False, 0.01)
        with patch.object(RUNNER, "run_child", side_effect=[version, macros]), redirect_stdout(io.StringIO()):
            with self.assertRaisesRegex(RUNNER.RunnerError, "__clang__"):
                RUNNER.compiler_prerequisites("clang++")

    def test_control_failure_exit_refused(self):
        version = RUNNER.ChildResult(3, "clang version 17\n", "", False, 0.01)
        with patch.object(RUNNER, "run_child", return_value=version), redirect_stdout(io.StringIO()):
            with self.assertRaises(RUNNER.RunnerError):
                RUNNER.compiler_prerequisites("clang++")

    def test_actual_child_exit_retained(self):
        result = RUNNER.run_child([sys.executable, "-c", "raise SystemExit(7)"], 2)
        self.assertEqual(result.returncode, 7)
        self.assertFalse(result.timed_out)

    def test_child_timeout_is_killed_not_success(self):
        result = RUNNER.run_child([sys.executable, "-c", "import time; time.sleep(30)"], 0.05)
        self.assertEqual(result.returncode, -signal.SIGKILL)
        self.assertTrue(result.timed_out)


if __name__ == "__main__":
    unittest.main()
