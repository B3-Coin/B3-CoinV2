#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Focused offline regression tests for portable Windows packaging."""

from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import package_portable as package


class PortablePackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="b3-win-package-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.binaries = self.root / "bin"
        self.depends = self.root / "depends"
        self.binaries.mkdir()
        for name in package.PROGRAMS:
            (self.binaries / name).touch()
        self.imports = {}
        self.formats = {}
        self.compiler_files = {}
        self.commands = []
        self.mock = patch.object(package, "command", side_effect=self.command)
        self.mock.start()
        self.addCleanup(self.mock.stop)

    def command(self, *args):
        self.commands.append(args)
        if args[0] == "compiler":
            name = args[1].removeprefix("-print-file-name=")
            return str(self.compiler_files.get(name, name))
        self.assertEqual(args[0], "objdump")
        name = Path(args[2]).name
        if args[1] == "-f":
            return "file format " + self.formats.get(name, "pei-x86-64")
        self.assertEqual(args[1], "-p")
        return "\n".join("DLL Name: " + dll for dll in self.imports.get(name, []))

    def collect(self):
        return package.collect_payload(self.binaries, self.depends,
                                       "objdump", "compiler")

    def test_shcore_and_api_sets_are_windows_supplied_case_insensitively(self):
        # Qt imports SHCORE for Windows DPI support. It is an OS component,
        # not a missing redistributable to search for or ship beside the EXE.
        self.imports["b3coin-qt.exe"] = ["SHCORE.dll", "ShCoRe.DlL", "KERNEL32.dll",
                                         "api-ms-win-core-synch-l1-2-0.dll"]
        self.assertEqual(set(self.collect()), set(package.PROGRAMS))
        self.assertFalse(any(args[0] == "compiler" for args in self.commands))

    def test_unknown_dependency_still_fails_closed(self):
        self.imports["b3coin-qt.exe"] = ["not-a-windows-library.dll"]
        with self.assertRaisesRegex(RuntimeError, "Unbundled runtime dependency"):
            self.collect()

    def test_observed_windows_build_imports_are_all_accounted_for(self):
        # Union inspected in all six EXEs from Actions run 34478422827
        # (a90a40c). New builds still verify their own actual PE imports.
        self.imports["b3coin-qt.exe"] = """
            ADVAPI32.dll AUTHZ.dll bcrypt.dll comdlg32.dll d3d11.dll d3d9.dll
            dwmapi.dll DWrite.dll dxgi.dll GDI32.dll IMM32.dll IPHLPAPI.DLL
            KERNEL32.dll msvcrt.dll NETAPI32.dll ole32.dll OLEAUT32.dll
            Secur32.dll SETUPAPI.dll SHCORE.dll SHELL32.dll SHLWAPI.dll
            USER32.dll USERENV.dll UxTheme.dll VERSION.dll WINMM.dll
            WS2_32.dll WTSAPI32.dll api-ms-win-core-synch-l1-2-0.dll
            api-ms-win-core-winrt-l1-1-0.dll
            api-ms-win-core-winrt-string-l1-1-0.dll
        """.split()
        self.assertEqual(set(self.collect()), set(package.PROGRAMS))
        self.assertFalse(any(args[0] == "compiler" for args in self.commands))

    def test_non_system_dlls_and_their_dependencies_are_bundled(self):
        first = self.binaries / "libwinpthread-1.dll"
        first.touch()
        second = self.root / "libgcc_s_seh-1.dll"
        second.touch()
        self.imports["b3coin-qt.exe"] = [first.name]
        self.imports[first.name] = [second.name, "KERNEL32.dll"]
        self.imports[second.name] = [first.name.upper()]
        self.compiler_files[second.name] = second
        payload = self.collect()
        self.assertEqual(payload[first.name], first)
        self.assertEqual(payload[second.name], second)
        self.assertEqual(len(payload), len(package.PROGRAMS) + 2)

    def test_wrong_architecture_is_rejected(self):
        self.formats["b3coin-qt.exe"] = "pei-i386"
        with self.assertRaisesRegex(RuntimeError, "Not an x86-64 Windows payload"):
            self.collect()

    def test_missing_program_is_rejected(self):
        (self.binaries / "b3coin-qt.exe").unlink()
        with self.assertRaisesRegex(RuntimeError, "Missing portable binary"):
            self.collect()

    def test_archive_contains_portable_programs_and_no_installer(self):
        for name in ("README.md", "COPYING"):
            (self.root / name).touch()
        archive = self.root / "out" / "portable.zip"
        self.imports["b3coin-qt.exe"] = ["SHCORE.dll"]
        argv = ["package_portable.py", "--binaries", str(self.binaries),
                "--depends", str(self.depends), "--source", str(self.root),
                "--archive", str(archive), "--objdump", "objdump",
                "--compiler", "compiler"]
        with patch("sys.argv", argv):
            package.main()
        with zipfile.ZipFile(archive) as result:
            self.assertEqual(set(result.namelist()),
                             set(package.PROGRAMS) | {"README.md", "COPYING"})
            self.assertIsNone(result.testzip())


if __name__ == "__main__":
    unittest.main()
