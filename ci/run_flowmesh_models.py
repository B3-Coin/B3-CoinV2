#!/usr/bin/env python3
"""Run the complete, bounded isolated-model CI locally or on GitHub Actions.

From the repository root: python3.14 -B ci/run_flowmesh_models.py
Python 3.14 is the tested baseline. Only the standard library is required.
The accounting and agreement suites retain their predetermined campaign seeds;
this runner additionally fixes Python's hash seed and isolates their imports.
Every test_*.py below each model or storage directory is discovered automatically.
"""

from collections import Counter
import os
from pathlib import Path
import subprocess
import sys
import unittest


ROOT = Path(__file__).resolve().parents[1]
# Floors preserve the reviewed suites, including independent checker negative
# controls, hidden/late certificates, restart cases and bounded campaigns.
# Deliberate test removals require an explicit review of these floors as well.
SUITES = {
    "runner": ("ci", "test_flowmesh_models_runner.py", 30, {
        "test_flowmesh_models_runner.py": 8,
    }),
    "accounting": ("test/flowmesh_v2_model", "test_*.py", 300, {
        "test_boundaries.py": 6,
        "test_curves.py": 17,
        "test_decimal_fees.py": 6,
        "test_milestone1_randomized.py": 5,
        "test_milestone1_regressions.py": 11,
        "test_milestone1_snapshot.py": 9,
        "test_model.py": 50,
        "test_validation.py": 9,
    }),
    "agreement": ("test/flowmesh_v2_agreement", "test_*.py", 300, {
        "test_admission.py": 16,
        "test_agreement.py": 13,
        "test_application.py": 15,
        "test_checker.py": 35,
        "test_exploration.py": 2,
        "test_publication_recovery.py": 4,
        "test_recovery.py": 13,
        "test_retry_bounds.py": 13,
        "test_review.py": 5,
    }),
    "storage": ("test/flowmesh_v2_storage", "test_*.py", 300, {
        "test_disk_store.py": 34,
        "test_process_recovery.py": 14,
        "test_storage_availability.py": 5,
        "test_storage_profile.py": 2,
    }),
}


def test_cases(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from test_cases(item)
        else:
            yield item


def discover_checked(directory, pattern, minimums):
    """Fail closed on missing/empty modules and silently filtered test methods."""
    directory = directory.resolve()
    files = {path.relative_to(directory).as_posix(): path
             for path in directory.rglob(pattern)}
    missing = minimums.keys() - files.keys()
    if missing:
        raise ValueError(f"Missing required test files: {sorted(missing)}")
    if not files:
        raise ValueError(f"No test files found in {directory}")
    loader = unittest.TestLoader()
    suite = loader.discover(str(directory), pattern=pattern)
    if loader.errors:
        raise ValueError("Test discovery failed:\n" + "\n".join(loader.errors))
    cases = list(test_cases(suite))
    ids = [case.id() for case in cases]
    if len(ids) != len(set(ids)):
        raise ValueError("Duplicate discovered test IDs")
    counts = Counter()
    for case in cases:
        module = sys.modules[type(case).__module__]
        source = Path(module.__file__).resolve()
        if not source.is_relative_to(directory):
            raise ValueError(f"Test imported from outside suite: {case.id()}")
        counts[source.relative_to(directory).as_posix()] += 1
    for name in sorted(files):
        floor = minimums.get(name, 1)
        if counts[name] < floor:
            raise ValueError(f"Incomplete discovery: {name}: {counts[name]} tests, need >= {floor}")
    # Check the methods declared by each loaded TestCase independently of
    # load_tests hooks, which can otherwise silently trim unittest discovery.
    expected_ids = set()
    for module in tuple(sys.modules.values()):
        source = getattr(module, "__file__", None)
        if not source or Path(source).resolve() not in files.values():
            continue
        for cls in vars(module).values():
            if (isinstance(cls, type) and issubclass(cls, unittest.TestCase)
                    and cls.__module__ == module.__name__):
                expected_ids.update(f"{module.__name__}.{cls.__qualname__}.{method}"
                                    for method in loader.getTestCaseNames(cls))
    if set(ids) != expected_ids:
        raise ValueError(f"Incomplete test selection: missing={sorted(expected_ids - set(ids))}, "
                         f"unexpected={sorted(set(ids) - expected_ids)}")
    return suite, len(cases)


def run_suite(name):
    relative, pattern, _, minimums = SUITES[name]
    try:
        suite, expected = discover_checked(ROOT / relative, pattern, minimums)
    except (ImportError, ValueError) as error:
        print(f"{name}: {error}", file=sys.stderr, flush=True)
        return 1
    print(f"{name}: discovered {expected} tests", flush=True)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    # A skipped campaign, xfail, or custom suite that omits cases is not a pass.
    complete = (result.testsRun == expected and not result.skipped
                and not result.expectedFailures)
    if not complete:
        print(f"{name}: incomplete execution (ran={result.testsRun}, expected={expected}, "
              f"skipped={len(result.skipped)}, expected failures={len(result.expectedFailures)})",
              file=sys.stderr, flush=True)
    return 0 if complete and result.wasSuccessful() else 1


def main():
    if sys.version_info[:2] != (3, 14):
        print("Use the tested Python 3.14 baseline: python3.14 -B ci/run_flowmesh_models.py",
              file=sys.stderr)
        return 1
    if len(sys.argv) == 3 and sys.argv[1] == "--suite" and sys.argv[2] in SUITES:
        return run_suite(sys.argv[2])
    if len(sys.argv) != 1:
        print(__doc__, file=sys.stderr)
        return 1
    env = dict(os.environ, PYTHONHASHSEED="0", PYTHONDONTWRITEBYTECODE="1")
    print(f"FlowMesh isolated models: Python {sys.version.split()[0]}; PYTHONHASHSEED=0", flush=True)
    failed = False
    for name, (_, _, timeout, _) in SUITES.items():
        try:
            result = subprocess.run([sys.executable, "-B", str(Path(__file__).resolve()),
                                     "--suite", name], cwd=ROOT, env=env, timeout=timeout,
                                    check=False)
            failed |= result.returncode != 0
        except subprocess.TimeoutExpired:
            print(f"{name}: exceeded {timeout}-second timeout", file=sys.stderr, flush=True)
            failed = True
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
