#!/usr/bin/env python3
"""No-service regression for private command publication and rejection."""
import ast
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

HELPERS = Path(os.environ.get('PILOT_CONTROL_HELPERS', Path(__file__).resolve().parent))


def controller_method():
    # Extract only the real method: importing the full functional fixture is
    # unnecessary and must never start a node in this no-service regression.
    tree = ast.parse((HELPERS / 'local-pilot.py').read_text())
    cls = next(node for node in tree.body if isinstance(node, ast.ClassDef) and node.name == 'FreshPilot')
    method = next(node for node in cls.body if isinstance(node, ast.FunctionDef) and node.name == 'process_commands')
    return ast.Module(body=[method], type_ignores=[])


class PilotControlTests(unittest.TestCase):
    def test_poll_during_write_never_sees_partial_command(self):
        spec = importlib.util.spec_from_file_location('pilot_control_under_test', HELPERS / 'local-pilot-control.py')
        writer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(writer)
        with tempfile.TemporaryDirectory(prefix='pilot-control-regression-') as temporary:
            root = Path(temporary)
            (root / 'commands').mkdir()
            (root / 'session.json').write_text(json.dumps({'controller_pid': os.getpid()}))
            (root / 'ready.json').write_text('{}')
            observed = []
            def interrupted_dump(value, output):
                raw = json.dumps(value)
                output.write(raw[:4]); output.flush()
                observed.append(list((root / 'commands').glob('*.json')))
                output.write(raw[4:])
            with patch.object(sys, 'argv', ['control', '--session-dir', str(root), 'snapshot']), \
                    patch.object(writer.json, 'dump', interrupted_dump), contextlib.redirect_stdout(io.StringIO()):
                writer.main()
            self.assertEqual(observed, [[]], 'Controller can observe an unpublished partial command')
            final = root / 'commands/001.json'
            self.assertEqual(json.loads(final.read_text()), {'id': 1, 'command': 'snapshot'})
            self.assertEqual(sorted(path.name for path in (root / 'commands').iterdir()), ['001.json'])
            self.assertEqual(stat.S_IMODE(final.stat().st_mode), 0o600)

    def test_malformed_terminal_command_is_refused_once_and_later_work_survives(self):
        with tempfile.TemporaryDirectory(prefix='pilot-control-regression-') as temporary:
            root = Path(temporary); (root / 'commands').mkdir()
            bad = root / 'commands/001.json'; bad.write_text('{"id')
            good = root / 'commands/002.json'; good.write_text('{"id":2,"command":"snapshot"}')
            records, responses, dispatched = [], [], []
            namespace = {'HERE': root, 'os': os, 'stat': stat, 'json': json,
                         'record': lambda path, value: records.append((path.name, value)),
                         'save': lambda path, value: responses.append((path.name, value))}
            exec(compile(controller_method(), str(HELPERS / 'local-pilot.py'), 'exec'), namespace)
            node = SimpleNamespace(last_command=0, rejected_commands=set(),
                                   command=lambda value: dispatched.append(value) or {'read_only': True})
            namespace['process_commands'](node)
            namespace['process_commands'](node)
            self.assertEqual(dispatched, [{'id': 2, 'command': 'snapshot'}])
            self.assertEqual(len(responses), 1)
            refusals = [value for name, value in records if name == 'commands-refused.jsonl']
            self.assertEqual(len(refusals), 1)
            self.assertEqual(refusals[0]['file'], '001.json')
            self.assertEqual(refusals[0]['error_type'], 'JSONDecodeError')
            self.assertEqual(bad.read_text(), '{"id')
            self.assertEqual(json.loads(good.read_text())['id'], 2)

    def test_mismatched_identity_is_not_dispatched(self):
        with tempfile.TemporaryDirectory(prefix='pilot-control-regression-') as temporary:
            root = Path(temporary); (root / 'commands').mkdir()
            (root / 'commands/001.json').write_text('{"id":2,"command":"snapshot"}')
            records, dispatched = [], []
            namespace = {'HERE': root, 'os': os, 'stat': stat, 'json': json,
                         'record': lambda path, value: records.append((path.name, value)),
                         'save': lambda *_: None}
            exec(compile(controller_method(), str(HELPERS / 'local-pilot.py'), 'exec'), namespace)
            node = SimpleNamespace(last_command=0, rejected_commands=set(), command=dispatched.append)
            namespace['process_commands'](node)
            self.assertEqual(dispatched, [])
            self.assertEqual(node.last_command, 0)
            self.assertEqual(records[0][0], 'commands-refused.jsonl')


if __name__ == '__main__':
    unittest.main(verbosity=2)
