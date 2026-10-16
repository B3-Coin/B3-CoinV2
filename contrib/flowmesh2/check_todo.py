#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Lint the rebirth task ledger src/flowmesh2/TODO.md.

Run with `python3 -I -B` (stdlib only). CTest: flowmesh2_todo_lint.

Every task table has exactly this header:

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |

Rules for each row:

- exactly ten cells;
- ID unique over the whole file and in the task ID format (the same format
  the C++ TaskId accepts at compile time: R<n>, M<n>-<AREA>-<nn>,
  MB1-<AREA>-<nn>, B0-<AREA>-<nn>);
- Ms equals the ID's milestone (`R` for outline slices);
- Status in {todo, outline, done};
- Skeleton in {STUB, REAL-REPLACE, NONE, —}; outline slice rows use `—` and
  name themselves in Slice; no other row uses `—`;
- STUB and REAL-REPLACE rows name the replace target: one or more existing
  outline slices in Slice (`R5` or `R5, R11`) and a location;
- NONE rows have Slice `—` and are never `outline`;
- an `outline` STUB or REAL-REPLACE row names at least one landed slice
  (an R row with status `outline` or `done`);
- Task, Acceptance and Evidence are not empty.

The outline slice IDs must be R1..Rn without gaps. With --selftest the script
first proves that it rejects doctored copies (duplicate ID, bad status, bad
skeleton, STUB without location or slice, wrong cell count, bad ID).
"""

import argparse
import os
import re
import sys

HEADER = ("| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location "
          "| Acceptance (failing-first) | Evidence | Refs |")
COLUMNS = 10
ID_RE = re.compile(r"^(R[1-9][0-9]*|(?:M(?:0|[1-9][0-9]*)|MB1|B0)-[A-Z][A-Z0-9]*-[0-9]{2})$")
SLICE_RE = re.compile(r"^R[1-9][0-9]*(, R[1-9][0-9]*)*$")
STATUSES = {"todo", "outline", "done"}
SKELETONS = {"STUB", "REAL-REPLACE", "NONE", "—"}
EMPTY_LOCATIONS = {"", "—", "-", "new"}


def split_row(line):
    """Split a Markdown table row into cells; '|' inside `code` or after '\\' is text."""
    if not (line.startswith("|") and line.endswith("|")):
        return None
    cells = []
    current = []
    in_code = False
    i = 1
    while i < len(line):
        c = line[i]
        if c == "\\" and i + 1 < len(line) and line[i + 1] == "|":
            current.append("|")
            i += 2
            continue
        if c == "`":
            in_code = not in_code
        if c == "|" and not in_code:
            cells.append("".join(current).strip())
            current = []
        else:
            current.append(c)
        i += 1
    if in_code or "".join(current).strip():
        return None
    return cells


def parse(lines):
    """Return (rows, problems). Each row is (line_number, cells)."""
    rows = []
    problems = []
    i = 0
    tables = 0
    while i < len(lines):
        line = lines[i].rstrip("\n")
        if line.startswith("| ID | Ms |") and line != HEADER:
            problems.append(f"line {i + 1}: task table header differs from the canonical header")
        if line != HEADER:
            i += 1
            continue
        tables += 1
        if i + 1 >= len(lines):
            problems.append(f"line {i + 1}: task table without separator")
            break
        separator = split_row(lines[i + 1].rstrip("\n"))
        if separator is None or len(separator) != COLUMNS or not all(re.fullmatch(r":?-{3,}:?", c) for c in separator):
            problems.append(f"line {i + 2}: malformed table separator")
        i += 2
        while i < len(lines) and lines[i].startswith("|"):
            text = lines[i].rstrip("\n")
            cells = split_row(text)
            if cells is None:
                problems.append(f"line {i + 1}: malformed table row")
            elif len(cells) != COLUMNS:
                problems.append(f"line {i + 1}: {len(cells)} cells instead of {COLUMNS}")
            else:
                rows.append((i + 1, cells))
            i += 1
    if tables == 0:
        problems.append("no task table found")
    return rows, problems


def milestone_of(task_id):
    return "R" if task_id.startswith("R") else task_id.split("-", 1)[0]


def lint(text):
    """Return (problems, summary) for the ledger text."""
    rows, problems = parse(text.splitlines())
    by_id = {}
    for number, cells in rows:
        task_id = cells[0]
        if task_id in by_id:
            problems.append(f"line {number}: duplicate ID {task_id} (first at line {by_id[task_id][0]})")
        else:
            by_id[task_id] = (number, cells)

    slice_ids = {task_id for task_id in by_id if task_id.startswith("R") and ID_RE.match(task_id)}
    landed = {task_id for task_id in slice_ids if by_id[task_id][1][2] in ("outline", "done")}
    if slice_ids:
        highest = max(int(task_id[1:]) for task_id in slice_ids)
        for n in range(1, highest + 1):
            if f"R{n}" not in slice_ids:
                problems.append(f"outline slices have a gap: R{n} is missing")
    else:
        problems.append("no outline slice rows (R<n>) found")

    for number, cells in rows:
        task_id, ms, status, skeleton, slices, task, location, acceptance, evidence, _refs = cells
        where = f"line {number} ({task_id})"
        if not ID_RE.match(task_id):
            problems.append(f"{where}: malformed ID")
            continue
        if ms != milestone_of(task_id):
            problems.append(f"{where}: Ms '{ms}' should be '{milestone_of(task_id)}'")
        if status not in STATUSES:
            problems.append(f"{where}: Status '{status}' not in {sorted(STATUSES)}")
        if skeleton not in SKELETONS:
            problems.append(f"{where}: Skeleton '{skeleton}' not in {sorted(SKELETONS)}")
        for name, value in (("Task", task), ("Acceptance", acceptance), ("Evidence", evidence)):
            if not value:
                problems.append(f"{where}: empty {name}")
        is_slice = task_id.startswith("R")
        if is_slice:
            if skeleton != "—":
                problems.append(f"{where}: an outline slice row must have Skeleton '—'")
            if slices != task_id:
                problems.append(f"{where}: an outline slice row must name itself in Slice")
            continue
        if skeleton == "—":
            problems.append(f"{where}: only outline slice rows may have Skeleton '—'")
        elif skeleton in ("STUB", "REAL-REPLACE"):
            if not SLICE_RE.match(slices):
                problems.append(f"{where}: {skeleton} must name its outline slice(s) in Slice, got '{slices}'")
            else:
                named = slices.split(", ")
                for name in named:
                    if name not in slice_ids:
                        problems.append(f"{where}: Slice names unknown outline slice {name}")
                if status == "outline" and not any(name in landed for name in named):
                    problems.append(f"{where}: status 'outline' but none of {slices} has landed")
            if location in EMPTY_LOCATIONS:
                problems.append(f"{where}: {skeleton} must name the location it replaces")
        elif skeleton == "NONE":
            if slices != "—":
                problems.append(f"{where}: a NONE row has no outline slice; Slice must be '—'")
            if status == "outline":
                problems.append(f"{where}: a NONE row cannot be 'outline'")

    counts = {}
    for _number, cells in rows:
        counts[cells[2]] = counts.get(cells[2], 0) + 1
    summary = (f"rows={len(rows)} unique_ids={len(by_id)} slices={len(slice_ids)} landed={sorted(landed, key=lambda s: int(s[1:]))} "
               + " ".join(f"{k}={counts[k]}" for k in sorted(counts)))
    return problems, summary


def doctor(text, mutate):
    """Apply mutate(cells) -> cells or None (drop) or list of rows to the first matching row."""
    lines = text.splitlines(keepends=True)
    rows, _ = parse([l for l in lines])
    for number, cells in rows:
        result = mutate(cells)
        if result is None:
            continue
        index = number - 1
        replacement = "".join("| " + " | ".join(r) + " |\n" for r in result)
        lines[index] = replacement
        return "".join(lines)
    raise RuntimeError("doctor: no row matched")


def selftest(text, workdir):
    def first_row(change):
        return lambda cells: change(list(cells))

    def first_with_skeleton(kind, change):
        return lambda cells: change(list(cells)) if cells[3] == kind else None

    def set_cell(index, value):
        def change(cells):
            cells[index] = value
            return [cells]
        return change

    scenarios = {
        # name: (mutation, text the expected problem must contain)
        "duplicate_id": (first_row(lambda cells: [cells, cells]), "duplicate ID"),
        "bad_status": (first_row(set_cell(2, "wip")), "Status 'wip'"),
        "bad_skeleton": (first_with_skeleton("NONE", set_cell(3, "MAYBE")), "Skeleton 'MAYBE'"),
        "stub_without_location": (first_with_skeleton("STUB", set_cell(6, "")), "must name the location"),
        "stub_without_slice": (first_with_skeleton("STUB", set_cell(4, "—")), "must name its outline slice"),
        "stub_unknown_slice": (first_with_skeleton("STUB", set_cell(4, "R999")), "unknown outline slice"),
        "wrong_cell_count": (first_row(lambda cells: [cells[:-1]]), "cells instead of 10"),
        "bad_id": (first_with_skeleton("NONE", set_cell(0, "M2-led-1")), "malformed ID"),
        "none_marked_outline": (first_with_skeleton("NONE", set_cell(2, "outline")), "cannot be 'outline'"),
    }
    failures = []
    for name, (mutation, needle) in scenarios.items():
        doctored = doctor(text, mutation)
        path = os.path.join(workdir, f"TODO.doctored.{name}.md")
        with open(path, "w", encoding="utf-8") as f:
            f.write(doctored)
        with open(path, encoding="utf-8") as f:
            found, _ = lint(f.read())
        ok = any(needle in problem for problem in found)
        print(f"selftest {name}: {'fails as required' if ok else 'NOT DETECTED'} ({len(found)} problem(s))")
        if not ok:
            failures.append(f"selftest: scenario '{name}' did not report '{needle}'")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--todo", required=True)
    parser.add_argument("--workdir", help="directory for --selftest copies")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    with open(args.todo, encoding="utf-8") as f:
        text = f.read()
    failures = []
    if args.selftest:
        if not args.workdir:
            parser.error("--selftest needs --workdir")
        os.makedirs(args.workdir, exist_ok=True)
        failures = selftest(text, args.workdir)
    problems, summary = lint(text)
    print(summary)
    for problem in problems:
        print(f"PROBLEM: {problem}")
    for failure in failures:
        print(f"SELFTEST FAILURE: {failure}")
    if problems or failures:
        return 1
    print("OK: TODO.md ledger is well formed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
