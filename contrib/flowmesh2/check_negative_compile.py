#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Run the fm2 compile-fail controls (src/flowmesh2/test/compile_fail/).

Run with `python3 -I -B` (stdlib only). CTest: flowmesh2_negative_compile
(Clang only, because the lock control needs Clang's thread-safety analysis).

src/flowmesh2/CMakeLists.txt defines, for every negative control <case>.cpp,
two EXCLUDE_FROM_ALL object libraries with the same flags as the fm2 code
(core_interface, fm2_warnings, fm2_types):

- fm2_cf_<case>: the file as is. Building it must FAIL, and the build output
  must match the regex on the file's `// FM2_EXPECT_ERROR: <regex>` line.
- fm2_cf_<case>_fixed: the file with FM2_COMPILE_FAIL_FIXED, which removes
  exactly the planted violation. Building it must SUCCEED, so the failure
  above is the violation and nothing else.

and fm2_cf_positive_control (positive_control.cpp), which must compile; it is
built first, because if it fails the harness itself is broken.

Required negative controls (owner-protected; removing one fails this check):
tsa_missing_lock (missing LOCK), ignored_result (ignored Result),
forge_token (forged Durable token), since slice R8 sign_without_intent
(SeatSigner::Sign without a DurableIntent), and since the review fix O-92
forge_receipt (forged ApplyReceipt) and apply_without_receipt
(CommitLog::AppendApplied without the receipt of a successful Apply). Any
other *.cpp in the directory must follow the same pattern and is run as well.

Stale objects cannot hide a result: before every build the runner deletes the
target's object file (passed by CMake as --object TARGET=PATH from
$<TARGET_OBJECTS>), so each control is compiled afresh. A successful build
must leave the object; a failed one must not.

Every build log is kept under --workdir. Exit status 0 only if every control
behaved as required.
"""

import argparse
import os
import re
import subprocess
import sys

REQUIRED = ("tsa_missing_lock", "ignored_result", "forge_token", "sign_without_intent", "forge_receipt", "apply_without_receipt")
POSITIVE = "positive_control"
EXPECT = re.compile(r"^//\s*FM2_EXPECT_ERROR:\s*(.+?)\s*$", re.M)
BUILD_TIMEOUT_S = 600


def build(args, target):
    """Delete the target's object, build it; return (exit status, output, object exists)."""
    obj = args.objects.get(target)
    if obj is None:
        return 1, f"no --object given for {target}", False
    if os.path.exists(obj):
        os.remove(obj)
    cmd = [args.cmake, "--build", args.build_dir, "--target", target]
    if args.config:
        cmd += ["--config", args.config]
    proc = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=BUILD_TIMEOUT_S)
    output = proc.stdout + proc.stderr
    with open(os.path.join(args.workdir, f"{target}.log"), "w", encoding="utf-8") as f:
        f.write(f"$ {' '.join(cmd)}\n{output}\nexit status: {proc.returncode}\nobject: {obj} exists={os.path.exists(obj)}\n")
    return proc.returncode, output, os.path.exists(obj)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cmake", required=True, help="cmake executable (CMAKE_COMMAND)")
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--config", default="", help="build configuration (multi-config generators)")
    parser.add_argument("--cases-dir", required=True, help="src/flowmesh2/test/compile_fail")
    parser.add_argument("--workdir", required=True, help="directory for the build logs")
    parser.add_argument("--object", action="append", default=[], metavar="TARGET=PATH",
                        help="object file of a control target (repeatable)")
    args = parser.parse_args()
    args.objects = {}
    for item in args.object:
        target, sep, path = item.partition("=")
        if not sep or not path:
            parser.error(f"--object needs TARGET=PATH, got {item!r}")
        args.objects[target] = path
    os.makedirs(args.workdir, exist_ok=True)

    failures = []
    stems = sorted(name[:-4] for name in os.listdir(args.cases_dir) if name.endswith(".cpp"))
    for required in REQUIRED + (POSITIVE,):
        if required not in stems:
            failures.append(f"required control {required}.cpp is missing")
    negatives = [stem for stem in stems if stem != POSITIVE]

    rc, output, made = build(args, f"fm2_cf_{POSITIVE}")
    ok = rc == 0 and made
    print(f"{'ok  ' if ok else 'FAIL'} positive control compiles: exit {rc}")
    if not ok:
        failures.append(f"positive control fm2_cf_{POSITIVE} does not compile; the harness is broken:\n{output[-4000:]}")

    for stem in negatives:
        with open(os.path.join(args.cases_dir, f"{stem}.cpp"), encoding="utf-8") as f:
            expectations = EXPECT.findall(f.read())
        if len(expectations) != 1:
            failures.append(f"{stem}.cpp must have exactly one '// FM2_EXPECT_ERROR: <regex>' line")
            continue
        expected = re.compile(expectations[0])

        rc, output, made = build(args, f"fm2_cf_{stem}_fixed")
        ok = rc == 0 and made
        print(f"{'ok  ' if ok else 'FAIL'} {stem} with FM2_COMPILE_FAIL_FIXED compiles: exit {rc}")
        if not ok:
            failures.append(f"fm2_cf_{stem}_fixed does not compile, so the control fails for another reason:\n{output[-4000:]}")

        rc, output, made = build(args, f"fm2_cf_{stem}")
        matched = expected.search(output)
        ok = rc != 0 and matched is not None and not made
        print(f"{'ok  ' if ok else 'FAIL'} {stem} fails to compile with the expected diagnostic: exit {rc}")
        if matched:
            print(f"      {matched.group(0)}")
        if not ok:
            failures.append(f"fm2_cf_{stem}: expected a failed build matching {expectations[0]!r}, got exit {rc}:\n{output[-4000:]}")

    print(f"controls: positive=1 negative={len(negatives)} failures={len(failures)}")
    for failure in failures:
        print(f"FAILURE: {failure}")
    if failures:
        return 1
    print("OK: every negative control fails with its diagnostic, its fixed form and the positive control compile")
    return 0


if __name__ == "__main__":
    sys.exit(main())
