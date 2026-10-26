#!/usr/bin/env python3
"""The regtest gates name no machine-specific path and resolve tools from arguments or FM2_* variables."""

import argparse
import contextlib
import io
from pathlib import Path
import re
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tool_paths  # noqa: E402

HERE = Path(__file__).resolve().parent
# Every directory of harness code that a gate runs.
HARNESS_DIRS = (HERE, HERE.parent / 'nitro_regtest', HERE.parent / 'rehearsal')
HOME_PATH = re.compile(r"""['"]/(?:home|Users)/[^'"]*['"]""")


def parser_for(*names):
    parser = argparse.ArgumentParser(prog='gate')
    tool_paths.add(parser, *names)
    return parser


class ToolPathTests(unittest.TestCase):
    def test_no_harness_file_has_a_home_directory_literal(self):
        found = []
        for directory in HARNESS_DIRS:
            for path in sorted(directory.rglob('*.py')) if directory.is_dir() else ():
                for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
                    if HOME_PATH.search(line):
                        found.append(f'{path.relative_to(HERE.parent)}:{number}: {line.strip()}')
        self.assertEqual(found, [])

    def test_argument_wins_over_environment(self):
        parser = parser_for('tools')
        args = parser.parse_args(['--tools', '/opt/foundry'])
        tool_paths.resolve(parser, args, 'tools', environ={'FM2_TOOLS': '/elsewhere'})
        self.assertEqual(args.tools, '/opt/foundry')

    def test_environment_fills_a_missing_argument(self):
        parser = parser_for('solc', 'b3coind', 'b3coin_cli', 'worker')
        args = parser.parse_args([])
        tool_paths.resolve(parser, args, 'solc', 'b3coind', 'b3coin_cli', 'worker',
                           environ={'FM2_SOLC': '/t/solc', 'FM2_B3_NODE_BUILD': '/b/node', 'FM2_REGTEST_BUILD': '/b/fm2'})
        self.assertEqual(args.solc, '/t/solc')
        self.assertEqual(Path(args.b3coind), Path('/b/node/bin/b3coind'))
        self.assertEqual(Path(args.b3coin_cli), Path('/b/node/bin/b3coin-cli'))
        self.assertEqual(Path(args.worker), Path('/b/fm2/bin/fm2-regtest-node'))

    def test_missing_path_names_flag_and_variable(self):
        for name, flag, variable in (('tools', '--tools', 'FM2_TOOLS'), ('asan_build', '--asan-build', 'FM2_ASAN_BUILD'),
                                     ('tsan_build', '--tsan-build', 'FM2_TSAN_BUILD'),
                                     ('b3_node_build', '--b3-node-build', 'FM2_B3_NODE_BUILD')):
            parser = parser_for(name)
            args = parser.parse_args([])
            stderr = io.StringIO()
            with self.assertRaises(SystemExit) as stop, contextlib.redirect_stderr(stderr):
                tool_paths.resolve(parser, args, name, environ={variable: ''})
            self.assertEqual(stop.exception.code, 2)
            self.assertIn(flag + ' is required: pass it or set ' + variable, stderr.getvalue())


if __name__ == '__main__':
    unittest.main()
