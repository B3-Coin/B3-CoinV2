#!/usr/bin/env python3
"""Run the local release gates. This tool has no mainnet mode."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import tool_paths


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def source_manifest(source):
    names = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=source).split(b'\0')
    files = {}
    for raw in sorted(set(names)):
        if not raw:
            continue
        name = os.fsdecode(raw)
        path = source / name
        files[name] = sha256(path) if path.is_file() else None
    encoded = json.dumps(files, sort_keys=True, separators=(',', ':')).encode()
    return {'sha256': hashlib.sha256(encoded).hexdigest(), 'files': files}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True)
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--evidence-dir', required=True)
    tool_paths.add(parser, 'b3_node_build', 'tools', 'solc')
    parser.add_argument('--agreement', choices=('pbft-fixed-view', 'pbft-view-change'), default='pbft-view-change')
    args = parser.parse_args()
    tool_paths.resolve(parser, args, 'b3_node_build', 'tools', 'solc')
    source = Path(args.source_root).resolve(strict=True)
    build = Path(args.build_dir).resolve(strict=True)
    evidence = Path(args.evidence_dir).resolve()
    if evidence == source or source in evidence.parents or evidence.exists():
        parser.error('evidence must be a fresh directory outside source; original signer state is never erased')
    evidence.mkdir(parents=True, mode=0o700)
    worker = build / 'bin/fm2-regtest-node'
    driver = source / 'contrib/flowmesh2/regtest'
    gates = []

    def run(label, command, timeout=2400):
        print('Checking ' + label + '...', flush=True)
        started = time.monotonic_ns()
        log = evidence / (label + '.log')
        with log.open('wb') as stream:
            result = subprocess.run([str(value) for value in command], cwd=source, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=timeout)
        gates.append({'label': label, 'passed': result.returncode == 0, 'exit_code': result.returncode,
                      'elapsed_ms': (time.monotonic_ns() - started) / 1_000_000, 'log': log.name,
                      'log_sha256': sha256(log)})
        if result.returncode:
            raise RuntimeError(label + ' failed; retained evidence: ' + str(log))

    report = {'format': 'fm2-regtest-qualification/1', 'agreement': args.agreement,
              'mainnet_ready': False, 'mainnet_actions': [], 'gates': gates, 'passed': False}
    try:
        cache = (build / 'CMakeCache.txt').read_text()
        if 'CMAKE_HOME_DIRECTORY:INTERNAL=' + str(source) not in cache:
            raise RuntimeError('build directory belongs to another source tree')
        required = ['run_roundtrip.py', 'test_operator_recovery.py', 'test_b3_anchor_reorg.py', 'test_source_checkpoint_recovery.py', 'benchmark.py']
        if args.agreement == 'pbft-view-change':
            required.append('test_leader_failover.py')
        for name in required:
            if not (driver / name).is_file():
                raise RuntimeError('release gate is not implemented: ' + name)
        report['base_commit'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip()
        report['source_dirty'] = bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=source))
        initial_source = source_manifest(source)
        (evidence / 'source-manifest.json').write_text(json.dumps(initial_source, indent=2, sort_keys=True) + '\n')
        report['source_tree_sha256'] = initial_source['sha256']
        run('build', ['cmake', '--build', build, '--target', 'fm2_all', '--parallel', '2'])
        report['worker_sha256'] = sha256(worker)
        run('core', ['ctest', '--test-dir', build, '-R', '^flowmesh2_', '--no-tests=error', '--output-on-failure', '-j', '2'])
        run('custody-contracts', [Path(args.tools) / 'forge', 'test', '--root', source / 'contracts', '--offline',
                                  '--use', Path(args.solc), '--out', evidence / 'contracts-out',
                                  '--cache-path', evidence / 'contracts-cache', '--match-contract', 'RebirthRegtestVaultTest', '-vv'])
        common = ['--source-root', str(source), '--worker', str(worker), '--tools', args.tools, '--solc', args.solc,
                  '--b3coind', str(Path(args.b3_node_build) / 'bin/b3coind'),
                  '--b3coin-cli', str(Path(args.b3_node_build) / 'bin/b3coin-cli')]
        mode = ['--agreement', args.agreement]
        run('roundtrip', [sys.executable, driver / 'run_roundtrip.py', *common, *mode,
                           '--evidence-dir', evidence / 'roundtrip'])
        # The crash gate preserves the same original files and keys. Its
        # explicit mode argument is required for the version2 release gate.
        run('operator-recovery', [sys.executable, driver / 'test_operator_recovery.py', *common, *mode,
                                   '--evidence-dir', evidence / 'operator-recovery'])
        run('source-checkpoint-recovery', [sys.executable, driver / 'test_source_checkpoint_recovery.py', *common, *mode,
                                           '--evidence-dir', evidence / 'source-checkpoint-recovery'])
        run('b3-anchor-reorg', [sys.executable, driver / 'test_b3_anchor_reorg.py', *common, *mode,
                                '--evidence-dir', evidence / 'b3-anchor-reorg'])
        if args.agreement == 'pbft-view-change':
            run('leader-failover', [sys.executable, driver / 'test_leader_failover.py', *common, *mode,
                                    '--evidence-dir', evidence / 'leader-failover'])
        # No competing compiler or fault fixture runs during the benchmark.
        run('trading-latency', [sys.executable, driver / 'benchmark.py', *common, *mode,
                                '--samples', '100', '--warmup', '20', '--evidence-dir', evidence / 'trading-latency'])
        if source_manifest(source)['sha256'] != report['source_tree_sha256'] or sha256(worker) != report['worker_sha256']:
            raise RuntimeError('source or worker changed during qualification; results are not one release candidate')
        report['passed'] = all(gate['passed'] for gate in gates)
        report['scope'] = 'synthetic custody assets, actual isolated EVM/B3 and local operator processes; no production qualification'
        return 0
    except Exception as error:
        report['error'] = str(error)
        print(str(error), file=sys.stderr)
        return 1
    finally:
        path = evidence / 'qualification-report.json'
        encoded = json.dumps(report, indent=2, sort_keys=True) + '\n'
        with path.open('x', encoding='utf-8') as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        print('Report: ' + str(path), flush=True)


if __name__ == '__main__':
    raise SystemExit(main())
