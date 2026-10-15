#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Compile-only annotation contracts, not runtime or liveness qualification.

Run from any directory: python3 ci/check_flowmesh_lock_contracts.py
Runner checks: python3 -m unittest discover -s test/flowmesh_lock_contracts -p 'test_*.py'

The accepted loophole fixtures deliberately compile: they document what Clang
does NOT prove. They are never linked or executed. Linux/macOS process groups
bound compiler children; this runner refuses unsupported platforms or compilers
instead of reporting a skipped check as success.
"""

import argparse
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import time


@dataclass(frozen=True)
class Case:
    filename: str
    diagnostic: str | None = None

    @property
    def expected_exit(self) -> int:
        return 0 if self.diagnostic is None else 1


CASES = (
    Case("exclusion_body_allowed.cpp"),
    Case("unannotated_helper_allowed.cpp"),
    Case("unannotated_virtual_allowed.cpp"),
    Case("excluded_entry_rejected.cpp", r"cannot call function 'ChainOperation' while\s+(?:mutex\s+)?'chain' is held"),
    Case("blocking_boundary_rejected.cpp", r"cannot call function 'BlockingChain' while\s+(?:mutex\s+)?'market' is held"),
    Case("guarded_access_allowed.cpp"),
    Case("guarded_access_rejected.cpp", r"reading variable 'value' requires holding\s+(?:mutex\s+)?'mutex'(?: exclusively)?"),
    Case("required_lock_allowed.cpp"),
    Case("required_lock_rejected.cpp", r"calling function 'ReadLocked' requires holding\s+(?:mutex\s+)?'mutex' exclusively"),
    Case("annotated_virtual_rejected.cpp", r"cannot call function 'Touch' while\s+(?:mutex\s+)?'chain' is held"),
)
COMMAND_TIMEOUT_SECONDS = 10
CONTROL_TIMEOUT_SECONDS = 5


class RunnerError(Exception):
    """A prerequisite or discovery failure; never a successful skipped run."""


@dataclass(frozen=True)
class ChildResult:
    returncode: int
    stdout: str
    stderr: str
    timed_out: bool
    seconds: float


def run_child(command: list[str], timeout: float, source: str | None = None) -> ChildResult:
    """Bound the whole compiler process group and retain its actual exit code."""
    if os.name != "posix":
        raise RunnerError("This bounded compiler runner requires Linux/macOS process groups")
    started = time.monotonic()
    try:
        child = subprocess.Popen(
            command,
            stdin=subprocess.PIPE if source is not None else subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
    except OSError as error:
        raise RunnerError(f"Cannot execute compiler: {error}") from error
    timed_out = False
    try:
        stdout, stderr = child.communicate(source, timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            stdout, stderr = child.communicate(timeout=2)
        except subprocess.TimeoutExpired as error:
            # A pipe retained outside our child group must not hang this runner.
            child.stdout.close()
            child.stderr.close()
            raise RunnerError("Compiler pipes remained open after killing its process group") from error
    except BaseException:
        # Do not leave a compiler group running when the caller is interrupted.
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            child.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            child.stdout.close()
            child.stderr.close()
        raise
    return ChildResult(child.returncode, stdout, stderr, timed_out, time.monotonic() - started)


def discover(fixtures: Path, cases: tuple[Case, ...] = CASES) -> None:
    names = [case.filename for case in cases]
    if not names or len(names) != len(set(names)):
        raise RunnerError("Fixture manifest must be nonempty and contain no duplicates")
    if any(Path(name).name != name or not name.endswith(".cpp") for name in names):
        raise RunnerError("Fixture manifest contains an invalid filename")
    actual = {path.relative_to(fixtures).as_posix() for path in fixtures.rglob("*.cpp")}
    if actual != set(names):
        missing = sorted(set(names) - actual)
        extra = sorted(actual - set(names))
        raise RunnerError(f"Fixture discovery mismatch: missing={missing}, unexpected={extra}")
    if not (fixtures / "lock_contract_fixture.h").is_file():
        raise RunnerError("Missing common fixture header")
    for name in names:
        if not (fixtures / name).is_file():
            raise RunnerError(f"Fixture is not a regular file: {name}")


def check_result(case: Case, result: ChildResult, source: Path) -> str | None:
    if result.timed_out:
        return f"compiler timed out; actual exit={result.returncode}"
    if result.returncode != case.expected_exit:
        return f"expected exit={case.expected_exit}, actual exit={result.returncode}"
    if result.stdout.strip():
        return "unexpected compiler stdout"
    if case.diagnostic is None:
        return None if not result.stderr.strip() else "unexpected compiler diagnostics"
    # One exact error, at this fixture, bearing the thread-safety diagnostic
    # category. Missing headers, syntax failures, other warnings/errors, signals,
    # and diagnostic text merely echoed in a source line cannot satisfy this.
    # LOCKABLE uses capability("") here; Clang can omit the noun "mutex".
    lines = [line for line in result.stderr.splitlines() if line.strip()]
    expected = (
        rf"{re.escape(str(source))}:\d+:\d+: error: {case.diagnostic} "
        r"\[-Werror,-Wthread-safety-analysis\]"
    )
    errors = [line for line in lines if re.fullmatch(expected, line)]
    others = [line for line in lines if not re.fullmatch(expected, line)]
    # Clang may append the diagnostic total. No other diagnostics are accepted.
    others = [line for line in others if line != "1 error generated."]
    if len(errors) != 1 or others:
        return "missing exact intended thread-safety error or additional diagnostics"
    return None


def compiler_prerequisites(compiler: str) -> str:
    version = run_child([compiler, "--version"], CONTROL_TIMEOUT_SECONDS)
    print(f"CONTROL compiler-version: actual exit={version.returncode}, timed_out={version.timed_out}")
    if version.timed_out or version.returncode != 0 or version.stderr.strip() or "clang" not in version.stdout.lower():
        raise RunnerError("A working Clang C++ compiler is required; unavailable is not a pass")
    macros = run_child([compiler, "-dM", "-E", "-x", "c++", "-"], CONTROL_TIMEOUT_SECONDS, "")
    print(f"CONTROL clang-predefines: actual exit={macros.returncode}, timed_out={macros.timed_out}")
    if macros.timed_out or macros.returncode != 0 or macros.stderr.strip() or not re.search(r"^#define __clang__ 1$", macros.stdout, re.MULTILINE):
        raise RunnerError("Compiler does not expose the required __clang__ annotations")
    return version.stdout.splitlines()[0]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default="clang++", help="Clang C++ executable (not a shell command)")
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    fixtures = root / "test" / "flowmesh_lock_contracts"
    header = root / "src" / "threadsafety.h"
    try:
        discover(fixtures)
        if not header.is_file():
            raise RunnerError("Missing actual repository src/threadsafety.h")
        compiler = shutil.which(args.compiler)
        if compiler is None:
            raise RunnerError(f"Clang executable not found: {args.compiler}; no checks ran")
        print(f"Compiler: {compiler_prerequisites(compiler)}")
        print(f"Repository threadsafety.h SHA-256: {hashlib.sha256(header.read_bytes()).hexdigest()}")
        failures = 0
        for case in CASES:
            source = fixtures / case.filename
            command = [
                compiler, "-std=c++20", "-fsyntax-only", "-Wthread-safety",
                "-Werror=thread-safety", "-Werror=unknown-warning-option",
                "-ferror-limit=1", "-fno-color-diagnostics", "-fno-caret-diagnostics",
                "-fdiagnostics-show-option", "-I", str(root / "src"),
                "-I", str(fixtures), str(source),
            ]
            result = run_child(command, COMMAND_TIMEOUT_SECONDS)
            error = check_result(case, result, source)
            print(f"{'FAIL' if error else 'PASS'} {case.filename}: expected exit={case.expected_exit}, actual exit={result.returncode}, seconds={result.seconds:.3f}")
            if error:
                failures += 1
                print(f"  {error}")
                print((result.stdout + result.stderr)[:5000])
        print(f"{len(CASES) - failures}/{len(CASES)} compile contracts passed; compile-only, no runtime/liveness claim")
        return 1 if failures else 0
    except RunnerError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
