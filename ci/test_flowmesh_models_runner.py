"""Negative controls for model CI discovery; no model implementation changes."""

from contextlib import redirect_stderr, redirect_stdout
import io
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import patch

import run_flowmesh_models as runner


class DiscoveryGuardTests(unittest.TestCase):
    def setUp(self):
        self.temporary = TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.original_path = list(sys.path)
        self.original_modules = set(sys.modules)
        self.addCleanup(self.restore_imports)

    def restore_imports(self):
        sys.path[:] = self.original_path
        for name in sys.modules.keys() - self.original_modules:
            del sys.modules[name]

    def write_case(self, name="test_guard_example.py", suffix=""):
        (self.directory / name).write_text(
            "import unittest\n"
            "class Example(unittest.TestCase):\n"
            "    def test_first(self): pass\n"
            "    def test_second(self): pass\n" + suffix,
            encoding="utf-8")

    def discover(self, minimums=None):
        return runner.discover_checked(self.directory, "test_*.py", minimums or {})

    def test_empty_suite_fails(self):
        with self.assertRaisesRegex(ValueError, "No test files"):
            self.discover()

    def test_removed_required_file_fails(self):
        with self.assertRaisesRegex(ValueError, "Missing required test files"):
            self.discover({"test_required.py": 1})

    def test_empty_test_module_fails(self):
        (self.directory / "test_guard_empty.py").write_text("", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Incomplete discovery"):
            self.discover()

    def test_module_below_reviewed_floor_fails(self):
        self.write_case()
        with self.assertRaisesRegex(ValueError, "need >= 3"):
            self.discover({"test_guard_example.py": 3})

    def test_new_modules_are_included_automatically(self):
        self.write_case()
        self.write_case("test_guard_additional.py")
        suite, count = self.discover({"test_guard_example.py": 2})
        self.assertEqual(count, 4)
        self.assertEqual(suite.countTestCases(), 4)

    def test_load_tests_cannot_silently_remove_a_method(self):
        self.write_case(suffix="\ndef load_tests(loader, tests, pattern):\n"
                              "    return unittest.TestSuite([Example('test_first')])\n")
        with self.assertRaisesRegex(ValueError, "Incomplete test selection"):
            self.discover()

    def test_import_error_fails(self):
        (self.directory / "test_guard_import.py").write_text(
            "raise ImportError('synthetic discovery failure')\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Test discovery failed"):
            self.discover()

    def test_skipped_or_expected_failure_is_not_success(self):
        class Outcomes(unittest.TestCase):
            @unittest.skip("synthetic omitted test")
            def test_skip(self):
                pass

            @unittest.expectedFailure
            def test_xfail(self):
                self.fail("synthetic expected failure")

        for method in ("test_skip", "test_xfail"):
            with self.subTest(method=method):
                suite = unittest.TestSuite([Outcomes(method)])
                with patch.object(runner, "discover_checked", return_value=(suite, 1)):
                    with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                        self.assertEqual(runner.run_suite("runner"), 1)


if __name__ == "__main__":
    unittest.main()
