#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

"""Package the cross-built portable executables and their runtime DLL closure."""

import argparse
from pathlib import Path
import re
import subprocess
import zipfile


PROGRAMS = (
    "b3coin-qt.exe", "b3coind.exe", "b3coin-cli.exe", "b3coin-tx.exe",
    "b3coin-util.exe", "b3coin-wallet.exe",
)
# Windows 10 supplies these libraries. All other imports must be bundled.
SYSTEM_DLLS = set("""
advapi32.dll authz.dll avrt.dll bcrypt.dll cfgmgr32.dll comctl32.dll
comdlg32.dll credui.dll crypt32.dll cryptbase.dll d2d1.dll d3d9.dll
d3d11.dll dcomp.dll dnsapi.dll dwmapi.dll dwrite.dll dxgi.dll dxva2.dll
gdi32.dll hid.dll imm32.dll iphlpapi.dll kernel32.dll mf.dll mfplat.dll
mfreadwrite.dll mpr.dll msimg32.dll msvcrt.dll ncrypt.dll netapi32.dll
normaliz.dll ntdll.dll ole32.dll oleacc.dll oleaut32.dll opengl32.dll
powrprof.dll propsys.dll psapi.dll rpcrt4.dll secur32.dll setupapi.dll
shell32.dll shlwapi.dll synchronization.dll ucrtbase.dll user32.dll
userenv.dll usp10.dll uxtheme.dll version.dll winhttp.dll wininet.dll
winmm.dll winscard.dll winspool.drv wintrust.dll wldap32.dll ws2_32.dll
wtsapi32.dll
""".split())


def command(*args):
    return subprocess.check_output(args, text=True)


def collect_payload(binaries, depends, objdump, compiler):
    payload = {name: binaries / name for name in PROGRAMS}
    available = {}
    for directory in (binaries, depends / "bin", depends / "lib"):
        if directory.is_dir():
            for path in directory.rglob("*"):
                if path.is_file() and path.suffix.lower() == ".dll":
                    available.setdefault(path.name.lower(), path)
    pending = list(payload.values())
    while pending:
        binary = pending.pop()
        if not binary.is_file():
            raise RuntimeError(f"Missing portable binary: {binary}")
        headers = command(objdump, "-f", str(binary))
        if "file format pei-x86-64" not in headers:
            raise RuntimeError(f"Not an x86-64 Windows payload: {binary}")
        imports = re.findall(r"DLL Name:\s*(\S+)", command(objdump, "-p", str(binary)))
        for name in imports:
            key = name.lower()
            if key in SYSTEM_DLLS or key.startswith(("api-ms-win-", "ext-ms-win-")):
                continue
            if key in payload:
                continue
            dependency = available.get(key)
            if dependency is None:
                located = command(compiler, f"-print-file-name={name}").strip()
                candidate = Path(located)
                if located != name and candidate.is_file():
                    dependency = candidate
            if dependency is None:
                raise RuntimeError(f"Unbundled runtime dependency {name} imported by {binary}")
            payload[key] = dependency
            pending.append(dependency)
    return payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", type=Path, required=True)
    parser.add_argument("--depends", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--objdump", default="x86_64-w64-mingw32-objdump")
    parser.add_argument("--compiler", default="x86_64-w64-mingw32-g++-posix")
    args = parser.parse_args()
    payload = collect_payload(args.binaries, args.depends, args.objdump, args.compiler)
    for name in ("README.md", "COPYING"):
        path = args.source / name
        if not path.is_file():
            raise RuntimeError(f"Missing package documentation: {path}")
        payload[name] = path
    args.archive.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.archive, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, path in sorted(payload.items()):
            archive.write(path, arcname=name)
    with zipfile.ZipFile(args.archive) as archive:
        bad_member = archive.testzip()
        if bad_member is not None or set(archive.namelist()) != set(payload):
            raise RuntimeError(f"Portable archive verification failed: {bad_member}")
    dll_count = sum(name.lower().endswith(".dll") for name in payload)
    print(f"Packaged {len(PROGRAMS)} x86-64 programs and {dll_count} runtime DLLs: {args.archive}")


if __name__ == "__main__":
    main()
