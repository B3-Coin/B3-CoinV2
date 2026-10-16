#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Evidence bundles for the FlowMesh V2 rebirth outline (plan section 10.2).

Stdlib only. Run with `python3 -I -B`. CTest: flowmesh2_evidence_selftest
(test_evidence.py).

A bundle is one directory (for durable evidence:
$EV/<milestone>/<task>/<run-id>/, see src/flowmesh2/TODO.md) holding

  run.json          schema fm2-evidence-run/1: source revision, host and one
                    entry per recorded step (argv, cwd, environment overrides,
                    expectation, exit status or signal, timing, log hashes)
  logs/             <label>.stdout.log and <label>.stderr.log of every step
  MANIFEST.sha256   `<sha256>  <path>` for every other file, sorted bytewise
                    (the `shasum -a 256 -c` format)

Commands:

  run --dir DIR --label LABEL [--expect zero|nonzero|any] [--expect-output RE]
      [--env NAME=VALUE ...] [--cwd DIR] [--timeout S] -- COMMAND [ARG ...]
        Runs COMMAND once, captures its output into logs/, appends a step to
        run.json and rewrites MANIFEST.sha256. A label is used once per
        bundle; an existing label is refused and nothing is changed. Exit 0 if
        the expectation was met (default: exit status 0), 1 if not (the step
        is still recorded), 2 on a usage error.
        `--expect nonzero` is how a sanitizer canary is recorded: the step
        passes only if the canary was flagged (non-zero exit or a signal).

  verify --dir DIR [--require-expectations]
        Recomputes the manifest. Fails (exit 1) on a changed, deleted or
        extra file, a symbolic link, a malformed, unsorted, duplicated,
        absolute or escaping manifest entry, or a run.json whose recorded log
        hashes disagree with the logs. With --require-expectations every
        recorded step must also have met its expectation.

  perf-metadata
        PERF-7 performance metadata (fsync probe, RTT matrix, nr_throttled,
        B3-advanced flag, topology). Not implemented in the outline: exits 3
        and names task M0-EVD-01.

Exit status: 0 success, 1 check failed, 2 usage error, 3 not implemented.
"""

import argparse
import datetime
import hashlib
import json
import os
import platform
import re
import stat
import subprocess
import sys
import time

SCHEMA = "fm2-evidence-run/1"
MANIFEST = "MANIFEST.sha256"
RUN_JSON = "run.json"
LOGS = "logs"
LABEL = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
MANIFEST_LINE = re.compile(r"^([0-9a-f]{64})  (\S(?:.*\S)?)$")
EXIT_OK, EXIT_FAILED, EXIT_USAGE, EXIT_NOT_IMPLEMENTED = 0, 1, 2, 3
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


class UsageError(Exception):
    pass


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 16), b""):
            digest.update(block)
    return digest.hexdigest()


def git(*args):
    try:
        proc = subprocess.run(["git", "-C", REPO] + list(args), capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return proc.stdout.strip() if proc.returncode == 0 else None


def source_info():
    status = git("status", "--porcelain")
    return {
        "repository": REPO,
        "git_head": git("rev-parse", "HEAD"),
        "git_branch": git("branch", "--show-current"),
        "git_dirty": None if status is None else bool(status),
    }


def host_info():
    return {
        "system": platform.system(),
        "release": platform.release(),
        "machine": platform.machine(),
        "python": platform.python_version(),
    }


def bundle_files(directory):
    """(relative POSIX paths of regular files except the manifest, problems)."""
    files, problems = [], []
    for dirpath, dirnames, filenames in os.walk(directory, followlinks=False):
        for name in sorted(dirnames):
            if os.path.islink(os.path.join(dirpath, name)):
                problems.append(f"symbolic link in bundle: {os.path.relpath(os.path.join(dirpath, name), directory)}")
        for name in sorted(filenames):
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, directory).replace(os.sep, "/")
            mode = os.lstat(path).st_mode
            if stat.S_ISLNK(mode):
                problems.append(f"symbolic link in bundle: {rel}")
            elif not stat.S_ISREG(mode):
                problems.append(f"not a regular file: {rel}")
            elif rel != MANIFEST:
                files.append(rel)
    return sorted(files, key=lambda p: p.encode("utf-8")), problems


def write_manifest(directory):
    files, problems = bundle_files(directory)
    if problems:
        raise UsageError("; ".join(problems))
    lines = "".join(f"{sha256_file(os.path.join(directory, rel))}  {rel}\n" for rel in files)
    tmp = os.path.join(directory, MANIFEST + ".tmp")
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(lines)
    os.replace(tmp, os.path.join(directory, MANIFEST))


def load_run(directory):
    path = os.path.join(directory, RUN_JSON)
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def save_run(directory, run):
    tmp = os.path.join(directory, RUN_JSON + ".tmp")
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(run, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp, os.path.join(directory, RUN_JSON))


def cmd_run(args):
    if not LABEL.match(args.label):
        raise UsageError(f"label {args.label!r} must match {LABEL.pattern}")
    command = list(args.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        raise UsageError("no command given after --")
    env_overrides = {}
    for item in args.env:
        name, sep, value = item.partition("=")
        if not sep or not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
            raise UsageError(f"--env needs NAME=VALUE, got {item!r}")
        env_overrides[name] = value
    expect_output = re.compile(args.expect_output) if args.expect_output else None

    directory = os.path.abspath(args.dir)
    os.makedirs(os.path.join(directory, LOGS), exist_ok=True)
    run = load_run(directory)
    if run is None:
        run = {"schema": SCHEMA, "created_utc": utc_now(), "tool": "contrib/flowmesh2/evidence.py",
               "source": source_info(), "host": host_info(), "steps": []}
    elif run.get("schema") != SCHEMA:
        raise UsageError(f"{RUN_JSON} has schema {run.get('schema')!r}, expected {SCHEMA}")
    if any(step.get("label") == args.label for step in run["steps"]):
        raise UsageError(f"label {args.label!r} is already recorded in this bundle")
    stdout_rel = f"{LOGS}/{args.label}.stdout.log"
    stderr_rel = f"{LOGS}/{args.label}.stderr.log"
    for rel in (stdout_rel, stderr_rel):
        if os.path.exists(os.path.join(directory, rel)):
            raise UsageError(f"{rel} already exists")

    env = dict(os.environ)
    env.update(env_overrides)
    cwd = os.path.abspath(args.cwd) if args.cwd else os.getcwd()
    started = utc_now()
    t0 = time.monotonic()
    timed_out = False
    with open(os.path.join(directory, stdout_rel), "wb") as out, open(os.path.join(directory, stderr_rel), "wb") as err:
        try:
            proc = subprocess.Popen(command, cwd=cwd, env=env, stdout=out, stderr=err)
        except OSError as e:
            start_error = e
        else:
            start_error = None
    if start_error is not None:
        for rel in (stdout_rel, stderr_rel):
            os.remove(os.path.join(directory, rel))
        raise UsageError(f"cannot start {command[0]!r}: {start_error}")
    # The child holds its own descriptors of both logs.
    try:
        returncode = proc.wait(timeout=args.timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.kill()
        returncode = proc.wait()
    duration_ms = int((time.monotonic() - t0) * 1000)

    signal_number = -returncode if returncode < 0 else None
    if timed_out:
        met = False
    elif args.expect == "zero":
        met = returncode == 0
    elif args.expect == "nonzero":
        met = returncode != 0
    else:
        met = True
    if met and expect_output is not None:
        with open(os.path.join(directory, stdout_rel), encoding="utf-8", errors="replace") as f:
            text = f.read()
        with open(os.path.join(directory, stderr_rel), encoding="utf-8", errors="replace") as f:
            text += f.read()
        met = expect_output.search(text) is not None

    run["steps"].append({
        "label": args.label,
        "argv": command,
        "cwd": cwd,
        "env": env_overrides,
        "expect": args.expect,
        "expect_output": args.expect_output,
        "started_utc": started,
        "duration_ms": duration_ms,
        "timed_out": timed_out,
        "exit_code": returncode,
        "signal": signal_number,
        "expectation_met": met,
        "stdout": stdout_rel,
        "stderr": stderr_rel,
        "stdout_sha256": sha256_file(os.path.join(directory, stdout_rel)),
        "stderr_sha256": sha256_file(os.path.join(directory, stderr_rel)),
    })
    save_run(directory, run)
    write_manifest(directory)
    outcome = f"signal {signal_number}" if signal_number else f"exit {returncode}"
    if timed_out:
        outcome = f"timed out after {args.timeout} s"
    print(f"evidence: step {args.label}: {outcome}; expected {args.expect}"
          f"{' with output /' + args.expect_output + '/' if args.expect_output else ''}: "
          f"{'met' if met else 'NOT MET'}")
    return EXIT_OK if met else EXIT_FAILED


def verify_bundle(directory, require_expectations):
    """Return a list of problems; empty means the bundle verifies."""
    problems = []
    manifest_path = os.path.join(directory, MANIFEST)
    if not os.path.isdir(directory):
        return [f"{directory} is not a directory"]
    if not os.path.exists(manifest_path):
        return [f"{MANIFEST} is missing"]
    listed = {}
    order = []
    with open(manifest_path, encoding="utf-8", errors="replace", newline="") as f:
        content = f.read()
    if content and not content.endswith("\n"):
        problems.append(f"{MANIFEST} does not end with a newline")
    for number, line in enumerate(content.split("\n")[:-1] if content else [], start=1):
        m = MANIFEST_LINE.match(line)
        if not m:
            problems.append(f"{MANIFEST}:{number}: malformed line {line!r}")
            continue
        digest, rel = m.group(1), m.group(2)
        parts = rel.split("/")
        if rel.startswith("/") or "\\" in rel or any(p in ("", ".", "..") for p in parts) or re.match(r"^[A-Za-z]:", rel):
            problems.append(f"{MANIFEST}:{number}: path {rel!r} is not a plain relative path inside the bundle")
            continue
        if rel == MANIFEST:
            problems.append(f"{MANIFEST}:{number}: the manifest must not list itself")
            continue
        if rel in listed:
            problems.append(f"{MANIFEST}:{number}: {rel} is listed twice")
            continue
        listed[rel] = digest
        order.append(rel)
    if order != sorted(order, key=lambda p: p.encode("utf-8")):
        problems.append(f"{MANIFEST} is not sorted bytewise by path")
    present, walk_problems = bundle_files(directory)
    problems += walk_problems
    for rel in present:
        if rel not in listed:
            problems.append(f"extra file not in the manifest: {rel}")
    for rel, digest in listed.items():
        path = os.path.join(directory, rel)
        if not os.path.lexists(path):
            problems.append(f"deleted file listed in the manifest: {rel}")
        elif os.path.islink(path) or not os.path.isfile(path):
            continue  # reported by the walk
        elif sha256_file(path) != digest:
            problems.append(f"changed file (SHA-256 differs from the manifest): {rel}")
    try:
        run = load_run(directory)
    except (OSError, ValueError) as e:
        return problems + [f"{RUN_JSON} is not valid JSON: {e}"]
    if run is None:
        problems.append(f"{RUN_JSON} is missing")
        return problems
    if run.get("schema") != SCHEMA:
        problems.append(f"{RUN_JSON} schema {run.get('schema')!r} is not {SCHEMA}")
    steps = run.get("steps")
    if not isinstance(steps, list) or not steps:
        problems.append(f"{RUN_JSON} records no step")
        return problems
    for step in steps:
        label = step.get("label")
        for key in ("stdout", "stderr"):
            rel = step.get(key)
            if not isinstance(rel, str) or rel not in listed:
                problems.append(f"step {label}: {key} log {rel!r} is not in the manifest")
            elif listed[rel] != step.get(f"{key}_sha256"):
                problems.append(f"step {label}: {key} log hash in {RUN_JSON} disagrees with the manifest")
        if require_expectations and step.get("expectation_met") is not True:
            problems.append(f"step {label}: expectation {step.get('expect')} was not met "
                            f"(exit {step.get('exit_code')}, signal {step.get('signal')}, timed out {step.get('timed_out')})")
    return problems


def cmd_verify(args):
    directory = os.path.abspath(args.dir)
    problems = verify_bundle(directory, args.require_expectations)
    for problem in problems:
        print(f"VERIFY FAILED: {problem}")
    if problems:
        print(f"evidence: {directory}: {len(problems)} problem(s)")
        return EXIT_FAILED
    run = load_run(directory)
    print(f"evidence: {directory}: manifest OK, {len(run['steps'])} step(s)")
    for step in run["steps"]:
        print(f"  {step['label']}: exit {step['exit_code']} signal {step['signal']} "
              f"expect {step['expect']} -> {'met' if step['expectation_met'] else 'NOT MET'}")
    return EXIT_OK


def tag_text(task_id):
    """The source tag of task_id, spelled so this file holds no second tag."""
    return "TODO(" + "rebirth:" + task_id + ")"


def not_implemented(task_id):
    """The Python form of a typed outline stub: names the task and exits 3."""
    print(f"evidence.py: NOT_IMPLEMENTED {task_id} ({tag_text(task_id)}, see src/flowmesh2/TODO.md)", file=sys.stderr)
    return EXIT_NOT_IMPLEMENTED


def cmd_perf_metadata(_args):
    # PERF-7 metadata: fsync probe, RTT matrix, nr_throttled, B3-advanced flag
    # and topology; `verify` must then fail when a field is missing.
    return not_implemented("M0-EVD-01")  # TODO(rebirth:M0-EVD-01)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="action", required=True)
    p_run = sub.add_parser("run", help="run one command and record it in a bundle")
    p_run.add_argument("--dir", required=True)
    p_run.add_argument("--label", required=True)
    p_run.add_argument("--expect", choices=("zero", "nonzero", "any"), default="zero")
    p_run.add_argument("--expect-output", help="regex the combined stdout and stderr must match")
    p_run.add_argument("--env", action="append", default=[], metavar="NAME=VALUE")
    p_run.add_argument("--cwd")
    p_run.add_argument("--timeout", type=float, default=None, help="seconds; a timeout never meets the expectation")
    p_run.add_argument("command", nargs=argparse.REMAINDER)
    p_verify = sub.add_parser("verify", help="recompute the manifest of a bundle")
    p_verify.add_argument("--dir", required=True)
    p_verify.add_argument("--require-expectations", action="store_true")
    sub.add_parser("perf-metadata", help="PERF-7 metadata (not implemented: M0-EVD-01)")
    args = parser.parse_args(argv)
    try:
        if args.action == "run":
            return cmd_run(args)
        if args.action == "verify":
            return cmd_verify(args)
        return cmd_perf_metadata(args)
    except UsageError as e:
        print(f"evidence.py: error: {e}", file=sys.stderr)
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())
