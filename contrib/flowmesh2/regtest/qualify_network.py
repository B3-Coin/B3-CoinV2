#!/usr/bin/env python3
"""Full regtest release gates for authenticated transport and generic custody.

This program has no mainnet mode. Synthetic assets and owned local chains only.
"""
# TODO(rebirth:M0-QUAL-01) the gates are qualified on one host only; they must
# pass on a second machine from the tools setup_tools.sh documents.
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import tool_paths
from qualify_regtest import sha256, source_manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True)
    parser.add_argument('--build-dir', required=True)
    parser.add_argument('--evidence-dir', required=True)
    tool_paths.add(parser, 'b3_node_build', 'tools', 'solc', 'asan_build', 'tsan_build')
    args = parser.parse_args()
    tool_paths.resolve(parser, args, 'b3_node_build', 'tools', 'solc', 'asan_build', 'tsan_build')
    source, build = Path(args.source_root).resolve(strict=True), Path(args.build_dir).resolve(strict=True)
    evidence = Path(args.evidence_dir).resolve()
    if evidence == source or source in evidence.parents or evidence.exists():
        parser.error('evidence must be a fresh directory outside source; original state is retained')
    evidence.mkdir(mode=0o700, parents=True)
    driver, worker = source / 'contrib/flowmesh2/regtest', build / 'bin/fm2-regtest-node'
    report = {'format': 'fm2-network-regtest-qualification/1', 'passed': False, 'mainnet_ready': False,
              'mainnet_actions': [], 'transport': 'authenticated-bip324-tcp', 'client_api': 'pinned-TLS-1.3',
              'profile': 'fm2-regtest-stable-q/3', 'custody': 'fm2-custody-v2-regtest/1', 'gates': []}

    def run(label, command, timeout=3600):
        print('Checking ' + label + '...', flush=True)
        log = evidence / (label + '.log')
        started = time.monotonic_ns()
        with log.open('wb') as stream:
            result = subprocess.run([str(item) for item in command], cwd=source, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=timeout)
        report['gates'].append({'label': label, 'passed': result.returncode == 0, 'exit_code': result.returncode,
                               'elapsed_ms': (time.monotonic_ns() - started) / 1_000_000,
                               'log': log.name, 'log_sha256': sha256(log)})
        if result.returncode:
            raise RuntimeError(label + ' failed; original evidence retained: ' + str(log))

    try:
        manifest = source_manifest(source)
        report['source_tree_sha256'] = manifest['sha256']
        report['base_commit'] = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip()
        (evidence / 'source-manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
        common = ['--source-root', source, '--worker', worker, '--tools', args.tools, '--solc', args.solc,
                  '--b3coind', Path(args.b3_node_build) / 'bin/b3coind',
                  '--b3coin-cli', Path(args.b3_node_build) / 'bin/b3coin-cli']
        for mode, sanitized_build in (('asan-ubsan', args.asan_build), ('tsan', args.tsan_build)):
            run(mode, [sys.executable, '-B', driver/'qualify_sanitizers.py', '--source-root', source,
                '--build-dir', sanitized_build, '--mode', mode, '--evidence-dir', evidence/'sanitizers'/mode], timeout=7200)
        # The prior profiles, full core inventory, guards, old custody fixture,
        # SIGKILL recovery, source/anchor forks and view-change regressions stay required.
        run('legacy-regressions', [sys.executable, driver / 'qualify_regtest.py', '--source-root', source,
            '--build-dir', build, '--evidence-dir', evidence / 'legacy-regressions',
            '--b3-node-build', args.b3_node_build, '--tools', args.tools, '--solc', args.solc,
            '--agreement', 'pbft-view-change'], timeout=7200)
        report['worker_sha256'] = sha256(worker)
        run('all-custody-contracts', [Path(args.tools) / 'forge', 'test', '--root', source / 'contracts',
            '--offline', '--use', args.solc, '--out', evidence / 'contracts-out',
            '--cache-path', evidence / 'contracts-cache', '-vv'])
        for label, name in (('tls-boundary', 'test_https_api.py'), ('rpc-batch-boundary', 'test_custody_batch.py'),
                            ('private-identity-update', 'test_private_atomic_json.py'),
                            ('automatic-custody-protocol', 'test_automatic_custody.py'),
                            ('automatic-batching-protocol', 'test_automatic_batches.py'),
                            ('certified-history-protocol', 'test_certified_history.py'),
                            ('relayer-capacity', 'test_signed_relayer_capacity.py'),
                            ('runtime-guard-order', 'test_runtime_guards.py'),
                            ('custody-grouped-snapshot', 'test_custody_group.py'),
                            ('b3-rpc-batch-boundary', 'test_b3_rpc_batch.py'),
                            ('rpc-keepalive-boundary', 'test_keepalive.py'),
                            ('finality-and-relayer-codec', 'test_source_finality.py')):
            run(label, [sys.executable, '-B', driver / name])
        run('custody-proxy-upgrade', [sys.executable, '-B', driver / 'test_custody_proxy.py',
            '--source-root', source, '--tools', args.tools, '--solc', args.solc,
            '--evidence-dir', evidence / 'custody-proxy-upgrade'])
        run('signed-relayer-process-recovery', [sys.executable, '-B', driver / 'test_signed_relayer_process.py',
            '--source-root', source, '--worker', worker, '--tools', args.tools, '--solc', args.solc,
            '--evidence-dir', evidence / 'signed-relayer-process-recovery'])
        run('signed-custody-roundtrip', [sys.executable, '-B', driver / 'run_roundtrip.py', *common,
            '--agreement', 'pbft-view-change', '--custody', 'production',
            '--evidence-dir', evidence / 'signed-custody-roundtrip'])
        run('network-roundtrip-and-recovery', [sys.executable, '-B', driver / 'run_network_roundtrip.py',
            *common, '--evidence-dir', evidence / 'network-roundtrip-and-recovery'])
        # No competing compilation, sanitizer or fault fixture during the latency gate.
        run('network-trading-latency', [sys.executable, '-B', driver / 'benchmark.py', *common,
            '--transport', 'authenticated-tcp', '--custody', 'production', '--agreement', 'pbft-view-change',
            '--samples', '100', '--warmup', '20', '--evidence-dir', evidence / 'network-trading-latency'])
        if source_manifest(source)['sha256'] != report['source_tree_sha256'] or sha256(worker) != report['worker_sha256']:
            raise RuntimeError('source or worker changed during qualification; rerun one frozen release candidate')
        report['passed'] = True
        report['scope'] = ('actual isolated B3/Anvil/four independent operators, encrypted authenticated sockets, '
                           'TLS signed actions and synthetic custody reserves; external/mainnet qualification remains disabled')
        return 0
    except Exception as error:
        report['error'] = str(error)
        print(str(error), file=sys.stderr)
        return 1
    finally:
        with (evidence / 'qualification-report.json').open('x') as stream:
            json.dump(report, stream, indent=2, sort_keys=True)
            stream.write('\n')
            stream.flush()
            os.fsync(stream.fileno())
        print('Report: ' + str(evidence / 'qualification-report.json'), flush=True)


if __name__ == '__main__':
    raise SystemExit(main())
