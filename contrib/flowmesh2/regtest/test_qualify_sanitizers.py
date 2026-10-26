#!/usr/bin/env python3
"""The sanitizer gate's canary rule against recorded GCC 15 and Clang 18 output.

testdata/sanitizer/ holds the output of contrib/flowmesh2/canaries/asan_canary.cpp
built with -fsanitize=address,undefined and run with the gate's options:
canary-asan.clang18.log (Clang 18), canary-asan.gcc15.log (GCC 15 with
-fno-sanitize=object-size, as src/flowmesh2/CMakeLists.txt builds it) and
canary-asan.gcc15-with-object-size.log (GCC 15 without that flag, where UBSan
halts first). Recorded 2026-10-25; evidence in ~/B3-evidence/M0/M0-QUAL-01/sanitizer-logs/.
"""

from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qualify_sanitizers import CANARIES, check_passed, compiler_of  # noqa: E402

LOGS = Path(__file__).resolve().parent / 'testdata' / 'sanitizer'
ASAN = dict((label, expected) for label, _, expected in CANARIES['asan-ubsan'])['canary-asan']


def log(name):
    return (LOGS / name).read_text(errors='replace')


class CanaryRuleTests(unittest.TestCase):
    def test_clang_asan_canary_passes(self):
        self.assertTrue(check_passed(1, log('canary-asan.clang18.log'), ASAN))

    def test_gcc_asan_canary_passes_as_built(self):
        self.assertTrue(check_passed(1, log('canary-asan.gcc15.log'), ASAN))

    def test_gcc_ubsan_object_size_report_is_the_wrong_diagnostic(self):
        text = log('canary-asan.gcc15-with-object-size.log')
        self.assertIn('runtime error:', text)
        self.assertFalse(check_passed(1, text, ASAN))

    def test_a_canary_that_exits_zero_fails(self):
        self.assertFalse(check_passed(0, log('canary-asan.clang18.log'), ASAN))

    def test_a_clean_step_fails_on_any_sanitizer_report(self):
        for name in ('canary-asan.clang18.log', 'canary-asan.gcc15.log', 'canary-asan.gcc15-with-object-size.log'):
            with self.subTest(name=name):
                self.assertFalse(check_passed(0, log(name)))
        self.assertTrue(check_passed(0, '*** No errors detected\n'))

    def test_every_mode_has_its_canaries(self):
        self.assertEqual([label for label, _, _ in CANARIES['asan-ubsan']], ['canary-asan', 'canary-ubsan'])
        self.assertEqual([label for label, _, _ in CANARIES['tsan']], ['canary-tsan'])

    def test_compiler_is_read_from_the_cache(self):
        self.assertEqual(compiler_of('X:BOOL=ON\nCMAKE_CXX_COMPILER:FILEPATH=/usr/bin/clang++-18\n'), '/usr/bin/clang++-18')
        self.assertEqual(compiler_of('X:BOOL=ON\n'), 'unknown')


if __name__ == '__main__':
    unittest.main()
