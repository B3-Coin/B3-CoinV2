#!/usr/bin/env python3
"""Strict native regtest sanitizer suite with positive detection controls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

from qualify_regtest import sha256, source_manifest

# Each canary must exit non-zero with its own sanitizer's report before any
# test result of that build counts. Under GCC the ASan canary is built without
# UBSan's object-size check (src/flowmesh2/CMakeLists.txt), so GCC and Clang
# both print the AddressSanitizer report; a UBSan report from the ASan canary
# is the wrong diagnostic and fails the gate.
CANARIES = {
    'asan-ubsan': (('canary-asan', 'fm2_canary_asan', r'AddressSanitizer: heap-buffer-overflow'),
                   ('canary-ubsan', 'fm2_canary_ubsan', r'runtime error: signed integer overflow')),
    'tsan': (('canary-tsan', 'fm2_canary_tsan', r'ThreadSanitizer: data race'),),
}
UNEXPECTED_REPORT = re.compile(r'ERROR: AddressSanitizer|WARNING: ThreadSanitizer|runtime error:')


def check_passed(returncode, text, expected=None):
    """A canary passes only by failing with its expected report; anything else only by a clean exit."""
    if expected:
        return returncode != 0 and re.search(expected, text) is not None
    return returncode == 0 and UNEXPECTED_REPORT.search(text) is None


def compiler_of(cache):
    """The C++ compiler a CMake cache names, for the report."""
    found = re.search(r'^CMAKE_CXX_COMPILER:\w+=(.*)$', cache, re.M)
    return found.group(1).strip() if found else 'unknown'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True)
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--evidence-dir', required=True)
    parser.add_argument('--mode', choices=('asan-ubsan', 'tsan'), required=True)
    args = parser.parse_args()
    source, build = Path(args.source_root).resolve(strict=True), Path(args.build_dir).resolve(strict=True)
    evidence = Path(args.evidence_dir).resolve()
    if evidence.exists() or evidence == source or source in evidence.parents:
        parser.error('sanitizer evidence must be a fresh directory outside source')
    evidence.mkdir(parents=True, mode=0o700)
    environment = {**os.environ, 'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                   'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1',
                   'TSAN_OPTIONS': 'halt_on_error=1:second_deadlock_stack=1'}
    report = {'passed': False, 'mode': args.mode, 'source_tree_sha256': source_manifest(source)['sha256'],
              'checks': [], 'strict_options': {key: environment[key] for key in
                                             ('ASAN_OPTIONS', 'UBSAN_OPTIONS', 'TSAN_OPTIONS')}}

    def run(label, command, expected=None, timeout=3600):
        log = evidence/(label+'.log')
        started = time.monotonic_ns()
        with log.open('wb') as stream:
            result = subprocess.run([str(value) for value in command], cwd=source, env=environment,
                                    stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
        passed = check_passed(result.returncode, log.read_text(errors='replace'), expected)
        report['checks'].append({'label': label, 'passed': passed, 'exit_code': result.returncode,
                                 'expected_diagnostic': expected, 'log': log.name, 'log_sha256': sha256(log),
                                 'elapsed_ms': (time.monotonic_ns()-started)/1_000_000})
        if not passed:
            raise RuntimeError(label+' failed; original evidence: '+str(log))
        print(args.mode+' '+label+': PASS', flush=True)

    try:
        cache = (build/'CMakeCache.txt').read_text()
        if 'CMAKE_HOME_DIRECTORY:INTERNAL='+str(source) not in cache:
            raise RuntimeError('sanitizer build belongs to another source tree')
        report['compiler'] = compiler_of(cache)
        run('build', ['cmake', '--build', build, '--target', 'fm2_all', '--parallel', '2'])
        for label, binary, expected in CANARIES[args.mode]:
            run(label, [build/'bin'/binary], expected)
        run('ctest', ['ctest', '--test-dir', build, '-R', '^flowmesh2_', '--no-tests=error', '--output-on-failure', '-j1'])
        if source_manifest(source)['sha256'] != report['source_tree_sha256']:
            raise RuntimeError('source changed during sanitizer qualification')
        report['test_binary_sha256'] = sha256(build/'bin/test_flowmesh2')
        report['passed'] = True
        return 0
    except Exception as error:
        report['error'] = str(error)
        print(str(error), flush=True)
        return 1
    finally:
        with (evidence/'qualification-report.json').open('x') as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())


if __name__ == '__main__':
    raise SystemExit(main())
