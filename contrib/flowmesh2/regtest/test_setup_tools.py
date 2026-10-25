#!/usr/bin/env python3
"""setup_tools.sh verifies every download, refuses to overwrite and leaves nothing behind on failure.

A stub curl on PATH serves local bytes, so no network is used. Set
FM2_SETUP_TOOLS_REAL=1 to also run the real download into a temporary directory.
"""

import hashlib
import os
from pathlib import Path
import platform
import subprocess
import sys
import tarfile
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / 'setup_tools.sh'
LINUX_X86 = sys.platform.startswith('linux') and platform.machine() == 'x86_64'

STUB_CURL = """#!/bin/sh
# Writes $STUB_DIR/<last path component of the URL> to the file after -o.
out=""; url=""
while [ "$#" -gt 0 ]; do
  case "$1" in -o) out=$2; shift 2 ;; --) url=$2; shift 2 ;; *) shift ;; esac
done
[ -f "$STUB_DIR/${url##*/}" ] || exit 22
cp "$STUB_DIR/${url##*/}" "$out"
"""


@unittest.skipUnless(LINUX_X86, 'setup_tools.sh installs Linux x86-64 tools')
class SetupToolsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.stub, self.bin, self.dest = root / 'served', root / 'bin', root / 'tools'
        self.stub.mkdir()
        self.bin.mkdir()
        curl = self.bin / 'curl'
        curl.write_text(STUB_CURL)
        curl.chmod(0o755)

    def tearDown(self):
        self.tmp.cleanup()

    def run_setup(self, dest=None):
        env = dict(os.environ, PATH=f'{self.bin}{os.pathsep}{os.environ["PATH"]}', STUB_DIR=str(self.stub))
        return subprocess.run(['bash', str(SCRIPT), str(dest or self.dest)], capture_output=True, text=True, env=env)

    def serve_wrong_bytes(self):
        archive = self.stub / 'foundry_v1.8.5_linux_amd64.tar.gz'
        with tarfile.open(archive, 'w:gz') as tar:
            for name in ('forge', 'anvil', 'cast', 'chisel', 'solar'):
                item = self.stub / name
                item.write_bytes(b'not foundry')
                tar.add(item, arcname=name)
        (self.stub / 'solc-static-linux').write_bytes(b'not solc')

    def assert_nothing_installed(self):
        self.assertEqual(sorted(p.name for p in self.dest.iterdir()) if self.dest.exists() else [], [])

    def test_wrong_hash_is_refused_and_installs_nothing(self):
        self.serve_wrong_bytes()
        result = self.run_setup()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('SHA-256 mismatch', result.stderr)
        self.assertIn('6c66ffcc55fa4249197721baa3098bc208014ea1d8aa04b2ed50ac6bccffb226', result.stderr)
        self.assert_nothing_installed()

    def test_failed_download_installs_nothing(self):
        result = self.run_setup()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('download failed', result.stderr)
        self.assert_nothing_installed()

    def test_existing_target_is_never_overwritten(self):
        for existing in ('foundry-v1.8.5', 'solc-0.8.35'):
            with self.subTest(existing=existing):
                dest = Path(self.tmp.name) / ('dest-' + existing)
                dest.mkdir()
                (dest / existing).write_bytes(b'keep me')
                self.serve_wrong_bytes()
                result = self.run_setup(dest)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('refusing to overwrite', result.stderr)
                self.assertEqual((dest / existing).read_bytes(), b'keep me')
                self.assertEqual(sorted(p.name for p in dest.iterdir()), [existing])

    @unittest.skipUnless(os.environ.get('FM2_SETUP_TOOLS_REAL') == '1', 'set FM2_SETUP_TOOLS_REAL=1 to download')
    def test_real_download_installs_the_pinned_tools(self):
        result = subprocess.run(['bash', str(SCRIPT), str(self.dest)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        solc = self.dest / 'solc-0.8.35'
        self.assertEqual(hashlib.sha256(solc.read_bytes()).hexdigest(),
                         'fa8ac9a32d301ad023a36ee5a29f8e291fe3200c60244e43c142539e82a617f4')
        self.assertIn('0.8.35', subprocess.run([solc, '--version'], capture_output=True, text=True).stdout)
        forge = subprocess.run([self.dest / 'foundry-v1.8.5' / 'forge', '--version'], capture_output=True, text=True)
        self.assertIn('1.8.5', forge.stdout)
        self.assertIn('export FM2_TOOLS=' + str(self.dest / 'foundry-v1.8.5'), result.stdout)


if __name__ == '__main__':
    unittest.main()
