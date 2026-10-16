#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Self-test of evidence.py (black box: every call is a subprocess).

Run with `python3 -I -B test_evidence.py --workdir DIR` (stdlib only).
CTest: flowmesh2_evidence_selftest.

Covers: `run` writes run.json, logs and MANIFEST.sha256 and `verify` accepts
the bundle; expectations zero / nonzero / output regex / timeout / signal;
a duplicate label changes nothing; `verify` fails on a tampered, a deleted
and an extra file, on a symbolic link, on malformed, unsorted, duplicated,
absolute and escaping manifest entries, on a manifest that lists itself,
on a missing manifest and on run.json log hashes that disagree; and the
outline stub `perf-metadata` exits 3 naming M0-EVD-01 (the Python
stub_inventory case test_stub_M0_EVD_01, recognized by check_guards.py
rule 5).
"""

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
EVIDENCE = os.path.join(HERE, "evidence.py")
PY = sys.executable
WORKDIR = None


def evidence(*args, timeout=120):
    proc = subprocess.run([PY, "-I", "-B", EVIDENCE] + list(args), capture_output=True, text=True, timeout=timeout)
    return proc.returncode, proc.stdout + proc.stderr


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.dir = os.path.join(WORKDIR, self.id().rsplit(".", 1)[-1])
        shutil.rmtree(self.dir, ignore_errors=True)

    def run_step(self, label, code, *extra, expect_rc=0):
        rc, out = evidence("run", "--dir", self.dir, "--label", label, *extra, "--", PY, "-I", "-B", "-c", code)
        self.assertEqual(rc, expect_rc, out)
        return out

    def verify(self, *extra):
        return evidence("verify", "--dir", self.dir, *extra)

    def make_bundle(self):
        self.run_step("hello", "import sys; print('hello'); print('warn', file=sys.stderr)")
        self.run_step("canary", "import sys; sys.exit(7)", "--expect", "nonzero")
        rc, out = self.verify("--require-expectations")
        self.assertEqual(rc, 0, out)

    def manifest_lines(self):
        with open(os.path.join(self.dir, "MANIFEST.sha256"), encoding="utf-8") as f:
            return f.read().splitlines()

    def write_manifest_lines(self, lines):
        with open(os.path.join(self.dir, "MANIFEST.sha256"), "w", encoding="utf-8") as f:
            f.write("".join(line + "\n" for line in lines))

    def assert_verify_fails(self, needle):
        rc, out = self.verify()
        self.assertEqual(rc, 1, out)
        self.assertIn(needle, out)

    # -- run ---------------------------------------------------------------

    def test_run_writes_bundle(self):
        self.make_bundle()
        for rel in ("run.json", "MANIFEST.sha256", "logs/hello.stdout.log", "logs/hello.stderr.log",
                    "logs/canary.stdout.log", "logs/canary.stderr.log"):
            self.assertTrue(os.path.isfile(os.path.join(self.dir, rel)), rel)
        listed = [line.split("  ", 1)[1] for line in self.manifest_lines()]
        self.assertEqual(listed, sorted(listed))
        self.assertEqual(set(listed), {"run.json", "logs/hello.stdout.log", "logs/hello.stderr.log",
                                       "logs/canary.stdout.log", "logs/canary.stderr.log"})
        with open(os.path.join(self.dir, "run.json"), encoding="utf-8") as f:
            run = json.load(f)
        self.assertEqual(run["schema"], "fm2-evidence-run/1")
        self.assertIn("git_head", run["source"])
        self.assertIn("system", run["host"])
        hello, canary = run["steps"]
        self.assertEqual((hello["exit_code"], hello["expect"], hello["expectation_met"]), (0, "zero", True))
        self.assertEqual((canary["exit_code"], canary["expect"], canary["expectation_met"]), (7, "nonzero", True))
        with open(os.path.join(self.dir, "logs/hello.stdout.log"), encoding="utf-8") as f:
            self.assertEqual(f.read(), "hello\n")

    def test_unmet_expectation_is_recorded_and_fails(self):
        out = self.run_step("canary_not_flagged", "pass", "--expect", "nonzero", expect_rc=1)
        self.assertIn("NOT MET", out)
        rc, out = self.verify()
        self.assertEqual(rc, 0, out)  # integrity is fine ...
        rc, out = self.verify("--require-expectations")
        self.assertEqual(rc, 1, out)  # ... but the expectation was not met
        self.assertIn("canary_not_flagged", out)

    def test_expect_output(self):
        self.run_step("match", "print('ThreadSanitizer: data race')", "--expect-output", "ThreadSanitizer: data race")
        self.run_step("mismatch", "print('clean')", "--expect-output", "ThreadSanitizer", expect_rc=1)

    def test_signal_counts_as_nonzero(self):
        if not hasattr(signal, "SIGKILL"):
            self.skipTest("no POSIX signals")
        self.run_step("killed", "import os, signal; os.kill(os.getpid(), signal.SIGKILL)", "--expect", "nonzero")
        with open(os.path.join(self.dir, "run.json"), encoding="utf-8") as f:
            step = json.load(f)["steps"][0]
        self.assertEqual(step["signal"], signal.SIGKILL)

    def test_timeout_never_meets_expectation(self):
        self.run_step("slow", "import time; time.sleep(30)", "--expect", "any", "--timeout", "1", expect_rc=1)
        with open(os.path.join(self.dir, "run.json"), encoding="utf-8") as f:
            step = json.load(f)["steps"][0]
        self.assertTrue(step["timed_out"])
        self.assertFalse(step["expectation_met"])

    def test_env_override_reaches_child_and_is_recorded(self):
        self.run_step("env", "import os, sys; sys.exit(0 if os.environ.get('FM2_PROBE') == 'on' else 1)",
                      "--env", "FM2_PROBE=on")
        with open(os.path.join(self.dir, "run.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f)["steps"][0]["env"], {"FM2_PROBE": "on"})

    def test_duplicate_label_is_refused_and_changes_nothing(self):
        self.make_bundle()
        before = self.manifest_lines()
        out = self.run_step("hello", "print('again')", expect_rc=2)
        self.assertIn("already recorded", out)
        self.assertEqual(self.manifest_lines(), before)
        rc, out = self.verify("--require-expectations")
        self.assertEqual(rc, 0, out)

    def test_bad_usage(self):
        rc, out = evidence("run", "--dir", self.dir, "--label", "../escape", "--", PY, "-c", "pass")
        self.assertEqual(rc, 2, out)
        rc, out = evidence("run", "--dir", self.dir, "--label", "nocmd", "--")
        self.assertEqual(rc, 2, out)
        rc, out = evidence("run", "--dir", self.dir, "--label", "missing", "--", os.path.join(self.dir, "no-such-binary"))
        self.assertEqual(rc, 2, out)
        self.assertFalse(os.path.exists(os.path.join(self.dir, "logs", "missing.stdout.log")))

    # -- verify ------------------------------------------------------------

    def test_verify_fails_on_tampered_file(self):
        self.make_bundle()
        with open(os.path.join(self.dir, "logs/hello.stdout.log"), "a", encoding="utf-8") as f:
            f.write("x")
        self.assert_verify_fails("changed file (SHA-256 differs from the manifest): logs/hello.stdout.log")

    def test_verify_fails_on_tampered_run_json(self):
        self.make_bundle()
        path = os.path.join(self.dir, "run.json")
        with open(path, encoding="utf-8") as f:
            text = f.read()
        with open(path, "w", encoding="utf-8") as f:
            f.write(text.replace('"exit_code": 7', '"exit_code": 0'))
        self.assert_verify_fails("changed file (SHA-256 differs from the manifest): run.json")

    def test_verify_fails_on_deleted_file(self):
        self.make_bundle()
        os.remove(os.path.join(self.dir, "logs/canary.stderr.log"))
        self.assert_verify_fails("deleted file listed in the manifest: logs/canary.stderr.log")

    def test_verify_fails_on_extra_file(self):
        self.make_bundle()
        with open(os.path.join(self.dir, "logs/smuggled.log"), "w", encoding="utf-8") as f:
            f.write("not recorded\n")
        self.assert_verify_fails("extra file not in the manifest: logs/smuggled.log")

    def test_verify_fails_on_symlink(self):
        self.make_bundle()
        try:
            os.symlink(os.path.join(self.dir, "run.json"), os.path.join(self.dir, "link.json"))
        except (OSError, NotImplementedError):
            self.skipTest("cannot create a symbolic link here")
        self.assert_verify_fails("symbolic link in bundle: link.json")

    def test_verify_fails_on_missing_manifest(self):
        self.make_bundle()
        os.remove(os.path.join(self.dir, "MANIFEST.sha256"))
        self.assert_verify_fails("MANIFEST.sha256 is missing")

    def test_verify_fails_on_bad_manifest_entries(self):
        self.make_bundle()
        good = self.manifest_lines()
        digest = good[0].split("  ", 1)[0]
        cases = {
            "malformed line": good + ["not a manifest line"],
            "not sorted bytewise": list(reversed(good)),
            "is listed twice": good + [good[-1]],
            "is not a plain relative path": good + [f"{digest}  /etc/passwd"],
            "is not a plain relative path inside": good + [f"{digest}  ../outside.log"],
            "must not list itself": good + [f"{digest}  MANIFEST.sha256"],
        }
        for needle, lines in cases.items():
            with self.subTest(needle=needle):
                self.write_manifest_lines(lines)
                self.assert_verify_fails(needle)
        self.write_manifest_lines(good)
        rc, out = self.verify()
        self.assertEqual(rc, 0, out)

    def test_verify_fails_when_run_json_disagrees_with_logs(self):
        self.make_bundle()
        path = os.path.join(self.dir, "run.json")
        with open(path, encoding="utf-8") as f:
            run = json.load(f)
        run["steps"][0]["stdout_sha256"] = "0" * 64
        with open(path, "w", encoding="utf-8") as f:
            json.dump(run, f, indent=2, sort_keys=True)
        # Re-seal the manifest so only the run.json/log disagreement remains.
        rc, out = evidence("run", "--dir", self.dir, "--label", "reseal", "--", PY, "-c", "pass")
        self.assertEqual(rc, 0, out)
        self.assert_verify_fails("stdout log hash in run.json disagrees with the manifest")

    # -- stub --------------------------------------------------------------

    def test_stub_M0_EVD_01(self):
        rc, out = evidence("perf-metadata")
        self.assertEqual(rc, 3, out)
        self.assertIn("NOT_IMPLEMENTED M0-EVD-01", out)


def main():
    global WORKDIR
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--workdir", required=True)
    args, rest = parser.parse_known_args()
    WORKDIR = os.path.abspath(args.workdir)
    os.makedirs(WORKDIR, exist_ok=True)
    program = unittest.main(argv=[sys.argv[0], "-v"] + rest, exit=False)
    result = program.result
    print(f"evidence self-test: run={result.testsRun} failures={len(result.failures)} "
          f"errors={len(result.errors)} skipped={len(result.skipped)}")
    return 0 if result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
