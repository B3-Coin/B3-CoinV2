#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check that test_flowmesh2 runs exactly the cases listed in INVENTORY.txt.

Run with `python3 -I -B` (stdlib only). CTest: flowmesh2_inventory.

Three sets must be identical, and their counts are printed and compared:

- inventory: the `suite/case` lines of src/flowmesh2/test/INVENTORY.txt
  (unique, sorted bytewise, comments start with '#');
- listed: the cases the binary reports with --list_content (all enabled);
- executed: the cases that actually ran in a full run, read from the Boost XML
  log (a TestCase with a TestingTime element and no skipped attribute).

The full run must also exit 0. With --selftest the script first proves that
it fails on doctored inventories (a case removed, a phantom case added, a
duplicated line, an unsorted file) and on a run in which a listed case did not
execute (--run_test=!<case>). If any doctored scenario passes, the self-test
fails and so does the CTest.
"""

import argparse
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

CASE_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(/[A-Za-z_][A-Za-z0-9_]*)+$")
RUN_TIMEOUT_S = 120


def read_inventory(path):
    """Return (cases, problems) for an inventory file."""
    problems = []
    cases = []
    with open(path, encoding="utf-8") as f:
        for number, raw in enumerate(f, start=1):
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if line != line.strip() or not CASE_RE.match(line):
                problems.append(f"{path}:{number}: malformed inventory line {line!r}")
                continue
            cases.append(line)
    seen = set()
    for case in cases:
        if case in seen:
            problems.append(f"inventory lists {case} more than once")
        seen.add(case)
    if cases != sorted(cases):
        problems.append("inventory is not sorted bytewise (use `LC_ALL=C sort`)")
    return cases, problems


def list_cases(binary):
    """Return (cases, disabled) from `binary --list_content`."""
    proc = subprocess.run([binary, "--list_content"], capture_output=True, text=True, timeout=RUN_TIMEOUT_S)
    if proc.returncode != 0:
        raise RuntimeError(f"--list_content exited {proc.returncode}: {proc.stderr}")
    # Boost prints the tree to stderr; accept stdout too for other versions.
    text = proc.stderr if proc.stderr.strip() else proc.stdout
    entries = []  # (depth, name, enabled)
    for raw in text.splitlines():
        if not raw.strip():
            continue
        indent = len(raw) - len(raw.lstrip(" "))
        if indent % 4:
            raise RuntimeError(f"unexpected --list_content indentation: {raw!r}")
        name = raw.strip()
        enabled = name.endswith("*")
        entries.append((indent // 4, name.rstrip("*"), enabled))
    cases = []
    disabled = set()
    stack = []
    for i, (depth, name, enabled) in enumerate(entries):
        del stack[depth:]
        stack.append(name)
        is_leaf = i + 1 == len(entries) or entries[i + 1][0] <= depth
        if is_leaf:
            path = "/".join(stack)
            cases.append(path)
            if not enabled:
                disabled.add(path)
    return cases, disabled


def run_cases(binary, log_path, extra_args=()):
    """Run the binary with an XML log; return (returncode, executed, skipped)."""
    if os.path.exists(log_path):
        os.remove(log_path)
    args = [binary, *extra_args, "--log_format=XML", "--log_level=test_suite",
            f"--log_sink={log_path}", "--report_level=no"]
    proc = subprocess.run(args, capture_output=True, text=True, timeout=RUN_TIMEOUT_S)
    executed = []
    skipped = []
    if os.path.exists(log_path) and os.path.getsize(log_path) > 0:
        root = ET.parse(log_path).getroot()

        def walk(node, prefix):
            for child in node:
                if child.tag == "TestSuite":
                    walk(child, prefix + [child.get("name")])
                elif child.tag == "TestCase":
                    path = "/".join(prefix + [child.get("name")])
                    if child.get("skipped") == "yes":
                        skipped.append(path)
                    elif child.find("TestingTime") is not None:
                        executed.append(path)
                    else:
                        skipped.append(path)

        # The outermost TestSuite is the master suite; its name is not part of
        # a case path.
        for master in root.findall("TestSuite"):
            walk(master, [])
    return proc.returncode, executed, skipped


def compare(inventory, inventory_problems, listed, disabled, run):
    """Return the list of problems; empty means consistent."""
    returncode, executed, skipped = run
    problems = list(inventory_problems)
    inv, lst, exe = set(inventory), set(listed), set(executed)
    for case in sorted(lst - inv):
        problems.append(f"{case}: in the binary but not in INVENTORY.txt")
    for case in sorted(inv - lst):
        problems.append(f"{case}: in INVENTORY.txt but not in the binary")
    for case in sorted(disabled):
        problems.append(f"{case}: disabled in the binary")
    for case in sorted(inv - exe):
        problems.append(f"{case}: listed but did not execute")
    for case in sorted(exe - inv):
        problems.append(f"{case}: executed but not in INVENTORY.txt")
    if len(executed) != len(exe):
        problems.append("a case executed more than once")
    if skipped:
        problems.append(f"skipped cases: {', '.join(sorted(skipped))}")
    if not (len(inventory) == len(listed) == len(executed)):
        problems.append(f"case counts differ: inventory={len(inventory)} listed={len(listed)} executed={len(executed)}")
    if returncode != 0:
        problems.append(f"test binary exited with {returncode}")
    return problems


def write_lines(path, header, cases):
    with open(path, "w", encoding="utf-8") as f:
        f.write(header)
        for case in cases:
            f.write(case + "\n")


def selftest(binary, inventory_path, workdir, listed, disabled, full_run):
    """Every doctored scenario must produce at least one problem."""
    cases, problems = read_inventory(inventory_path)
    if problems or len(cases) < 2:
        return [f"selftest needs a clean inventory with at least two cases: {problems}"]
    header = "# doctored copy written by check_inventory.py --selftest\n"
    # name -> (doctored inventory, text the expected problem must contain)
    scenarios = {
        "case_removed": (cases[:-1], "in the binary but not in INVENTORY.txt"),
        "phantom_case_added": (sorted(cases + ["result_tests/fm2_selftest_phantom_case"]),
                               "in INVENTORY.txt but not in the binary"),
        "duplicated_line": (sorted(cases + [cases[0]]), "more than once"),
        "unsorted": (list(reversed(cases)), "not sorted"),
    }
    failures = []

    def expect(name, found, needle):
        ok = any(needle in problem for problem in found)
        print(f"selftest {name}: {'fails as required' if ok else 'NOT DETECTED'} ({len(found)} problem(s))")
        if not ok:
            failures.append(f"selftest: scenario '{name}' did not report '{needle}'")

    for name, (doctored, needle) in scenarios.items():
        path = os.path.join(workdir, f"INVENTORY.doctored.{name}.txt")
        write_lines(path, header, doctored)
        inv, inv_problems = read_inventory(path)
        expect(name, compare(inv, inv_problems, listed, disabled, full_run), needle)
    # A run in which one listed case does not execute.
    first = cases[0]
    partial = run_cases(binary, os.path.join(workdir, "run-without-first-case.xml"), [f"--run_test=!{first}"])
    expect(f"case_not_executed ({first})", compare(cases, [], listed, disabled, partial),
           f"{first}: listed but did not execute")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--inventory", required=True)
    parser.add_argument("--workdir", required=True)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    os.makedirs(args.workdir, exist_ok=True)
    listed, disabled = list_cases(args.binary)
    full_run = run_cases(args.binary, os.path.join(args.workdir, "run-full.xml"))

    failures = []
    if args.selftest:
        failures += selftest(args.binary, args.inventory, args.workdir, listed, disabled, full_run)

    inventory, inventory_problems = read_inventory(args.inventory)
    problems = compare(inventory, inventory_problems, listed, disabled, full_run)
    print(f"inventory={len(inventory)} listed={len(listed)} executed={len(full_run[1])} "
          f"skipped={len(full_run[2])} exit={full_run[0]}")
    for problem in problems:
        print(f"PROBLEM: {problem}")
    for failure in failures:
        print(f"SELFTEST FAILURE: {failure}")
    if problems or failures:
        return 1
    print("OK: every listed case is in the binary and executed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
