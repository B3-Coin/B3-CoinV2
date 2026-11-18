#!/usr/bin/env python3
"""Run the actual offline Nitro history CLI against genuine dev-chain bytes.

This is a process acceptance checker, not a portable crypto mock or an
Arbitrum One source qualification. Every native process runs inside a fresh
disconnected Linux network namespace. The separately supplied checkpoint
remains unauthenticated; all live authority and mainnet flags must stay false.
"""

import argparse
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import stat
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).absolute().parent))
from proof_bundle import decode_proof, encode_proof, wire_digest
from qualify_history import disconnected_namespace

FORMAT = 'b3-native-nitro-history-process-qualification/1'
FIXTURE_FILES = ('README.txt', 'expected-events.json', 'proof.bin', 'request.json', 'source-context.json')
FALSE_FLAGS = ('mainnet_enabled', 'arbitrum_authority', 'execution_verified', 'rollup_settlement_verified')
MAX_REPORT = 8 * 1024 * 1024
CHECKPOINT_TRUST = 'separately-supplied-L2-checkpoint-not-authenticated-here'


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def unique(pairs):
    value = {}
    for key, item in pairs:
        require(key not in value, 'duplicate JSON member')
        value[key] = item
    return value


def safe(path, *, new=False):
    path = Path(path).absolute()
    require('..' not in path.parts, 'parent traversal refused')
    for item in (path, *path.parents):
        if new and item == path and not item.exists():
            continue
        meta = item.lstat()
        require(not stat.S_ISLNK(meta.st_mode), 'aliased path refused')
        if item != path:
            require(stat.S_ISDIR(meta.st_mode), 'non-directory ancestor refused')
    return path


def read(path, maximum=MAX_REPORT):
    path = safe(path)
    meta = path.stat()
    require(stat.S_ISREG(meta.st_mode) and meta.st_nlink == 1 and meta.st_size <= maximum,
            'bounded plain file required')
    with path.open('rb') as stream:
        data = stream.read(maximum + 1)
    require(len(data) <= maximum, 'file grew beyond bound')
    return data


def json_read(path):
    return json.loads(read(path), object_pairs_hook=unique,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite JSON')))


def sha(path):
    path = safe(path)
    meta = path.stat()
    require(stat.S_ISREG(meta.st_mode) and meta.st_nlink == 1 and meta.st_size <= 1024 ** 3,
            'bounded plain pinned file required')
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def write(path, value):
    data = (json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False) + '\n').encode()
    require(len(data) <= MAX_REPORT, 'qualification report exceeds bound')
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())


def pins(native, fixture):
    require({item.name for item in fixture.iterdir()} == set(FIXTURE_FILES), 'fixture inventory differs')
    directory = Path(__file__).absolute().parent
    return {'native_sha256': sha(native),
            'fixture': {name: sha(fixture / name) for name in FIXTURE_FILES},
            'checker_sha256': sha(Path(__file__).absolute()),
            'proof_bundle_sha256': sha(directory / 'proof_bundle.py'),
            'namespace_helper_sha256': sha(directory / 'qualify_history.py')}


def limits():
    resource.setrlimit(resource.RLIMIT_FSIZE, (MAX_REPORT, MAX_REPORT))
    resource.setrlimit(resource.RLIMIT_AS, (512 * 1024 ** 2, 512 * 1024 ** 2))
    resource.setrlimit(resource.RLIMIT_CPU, (10, 10))


def flags(value, success):
    require(isinstance(value, dict) and value.get('format') == 'b3-arbitrum-history-check/1',
            'native result format differs')
    require(all(value.get(key) is False for key in FALSE_FLAGS), 'native authority flag changed')
    require(value.get('external_anchor_required') is True
            and value.get('checkpoint_trust') == CHECKPOINT_TRUST,
            'native checkpoint trust boundary changed')
    require(value.get('commitments_match') is success, 'native commitment result differs')
    require(isinstance(value.get('result'), str) and re.fullmatch('[A-Z_]{1,64}', value['result']),
            'native result code is malformed')
    require((value['result'] == 'OK') is success, 'native success code differs')


class Processes:
    def __init__(self, native, evidence, arguments):
        self.native, self.evidence, self.arguments = native, evidence, arguments
        self.cases = []
        self.environment = {'PATH': '/usr/bin:/bin', 'HOME': str(evidence),
                            'LANG': 'C', 'LC_ALL': 'C', 'TZ': 'UTC'}

    def run(self, name, data, *, arguments=None, success=False):
        require(name not in {row['name'] for row in self.cases}, 'duplicate process case')
        out = self.evidence / (name + '.stdout.json')
        err = self.evidence / (name + '.stderr.log')
        started = time.monotonic()
        with out.open('xb') as stdout, err.open('xb') as stderr:
            result = subprocess.run([str(self.native), *(self.arguments if arguments is None else arguments)],
                                    input=data, stdout=stdout, stderr=stderr, timeout=15,
                                    check=False, env=self.environment, close_fds=True, preexec_fn=limits)
        value = json_read(out)
        row = {'name': name, 'exit_code': result.returncode,
               'input_sha256': hashlib.sha256(data).hexdigest(),
               'stdout_sha256': sha(out), 'stderr_sha256': sha(err),
               'result': value.get('result'), 'expected_success': success,
               'seconds': round(time.monotonic() - started, 6)}
        self.cases.append(row)
        require((result.returncode == 0) is success, 'native process exit contradicts expected outcome')
        flags(value, success)
        require(not read(err), 'native emitted unexpected stderr')
        row['passed'] = True
        return value, read(out)


def exercise(native, fixture, evidence):
    original_pins = pins(native, fixture)
    data = read(fixture / 'proof.bin', 32 * 1024 ** 2)
    proof = decode_proof(data)
    request = json_read(fixture / 'request.json')
    expected = json_read(fixture / 'expected-events.json')
    context = json_read(fixture / 'source-context.json')
    require(set(request) == {'previous_height', 'previous_hash', 'to_height', 'to_hash'}, 'request shape differs')
    require(context['source']['chain_id'] == 412346 and context['source']['kind'] == 'owned-regtest'
            and context['mainnet_enabled'] is False and context['l1_settlement_verified'] is False
            and context['proof_sha256'] == hashlib.sha256(data).hexdigest(), 'fixture context differs')
    require(len(data) == 6557 and len(proof.blocks) == 2
            and [len(b.receipts) for b in proof.blocks] == [2, 2], 'genuine fixture shape differs')
    arguments = ['--previous-height', str(request['previous_height']), '--previous-hash', request['previous_hash'],
                 '--to-height', str(request['to_height']), '--to-hash', request['to_hash'],
                 '--vault', expected['vault']]
    process = Processes(native, evidence, arguments)
    baseline, raw = process.run('genuine_two_block_range', data, success=True)
    require(baseline['header_count'] == 2 and baseline['receipt_count'] == 4, 'native range count differs')
    require(baseline['candidate_events'] == expected['candidate_events'], 'native events differ from original Nitro receipts')
    require(baseline['proof_digest'] == '0x' + wire_digest(data), 'native exact proof digest differs')
    for name in ('previous_height', 'to_height'):
        require(baseline[name] == str(request[name]), 'native returned height differs')
    for name in ('previous_hash', 'to_hash'):
        require(baseline[name] == request[name], 'native returned endpoint differs')
    for name in ('newest', 'oldest'):
        accounts = baseline[name + '_accounts']
        require(len(accounts) == 1 and accounts[0]['address'] == expected['vault'], 'native account scope differs')
        actual = {slot['slot']: slot['value'] for slot in accounts[0]['storage']}
        require(actual == expected[name + '_storage'], 'native account/storage proof differs')
    _, repeated = process.run('deterministic_exact_replay', data, success=True)
    require(raw == repeated, 'native exact replay output is nondeterministic')

    def changed_arg(index, value):
        result = arguments.copy()
        result[index] = value
        return result

    def flip(raw):
        return raw[:-1] + bytes([raw[-1] ^ 1])

    def frame(name, changed):
        process.run(name, encode_proof(changed))

    altered_hash = '0x' + ('00' if request['to_hash'][2:4] != '00' else '01') + request['to_hash'][4:]
    process.run('wrong_requested_to_hash', data, arguments=changed_arg(7, altered_hash))
    process.run('wrong_requested_previous_hash', data, arguments=changed_arg(3, altered_hash))
    process.run('wrong_requested_to_height', data, arguments=changed_arg(5, str(request['to_height'] + 1)))
    process.run('wrong_requested_previous_height', data, arguments=changed_arg(1, str(request['previous_height'] - 1)))
    process.run('zero_requested_to_hash', data, arguments=changed_arg(7, '0x' + '00' * 32))
    process.run('truncated_frame', data[:-1])
    process.run('wrong_frame_magic', b'X' + data[1:])
    process.run('trailing_frame_byte', data + b'\x00')
    process.run('oversized_declared_frame', data[:8] + (32 * 1024 ** 2).to_bytes(4, 'big') + data[12:])
    process.run('oversized_actual_frame', data + bytes(32 * 1024 ** 2 + 1 - len(data)))
    first, second = proof.blocks
    frame('mutated_raw_header', replace(proof, blocks=(replace(first, header=flip(first.header)), second)))
    tx = replace(first.transactions[-1], encoded=flip(first.transactions[-1].encoded))
    frame('mutated_transaction_body', replace(proof, blocks=(replace(first, transactions=(*first.transactions[:-1], tx)), second)))
    receipt = replace(first.receipts[-1], encoded=flip(first.receipts[-1].encoded))
    frame('mutated_raw_receipt', replace(proof, blocks=(replace(first, receipts=(*first.receipts[:-1], receipt)), second)))
    reduced = replace(first, receipts=tuple(replace(v, index=i) for i, v in enumerate(first.receipts[:-1])),
                      transactions=tuple(replace(v, index=i) for i, v in enumerate(first.transactions[:-1])))
    frame('omitted_matched_body_receipt_pair', replace(proof, blocks=(reduced, second)))
    frame('reordered_blocks', replace(proof, blocks=(second, first)))
    frame('truncated_requested_range', replace(proof, blocks=(first,)))
    frame('swapped_oldest_newest_state', replace(proof, confirmed_accounts=proof.historical_accounts,
                                               historical_accounts=proof.confirmed_accounts))
    forged_assertion = replace(proof.assertion, block_hash=bytes.fromhex(request['to_hash'][2:]))
    frame('nonneutral_assertion_cannot_mint_authority', replace(proof, assertion=forged_assertion))
    process.run('duplicate_argument', data, arguments=arguments + ['--previous-height', str(request['previous_height'])])
    process.run('unknown_verified_argument', data, arguments=arguments + ['--verified', 'true'])
    process.run('noncanonical_height', data, arguments=changed_arg(1, '0' + str(request['previous_height'])))
    process.run('uppercase_requested_hash', data, arguments=changed_arg(7, request['to_hash'].upper()))
    process.run('missing_required_argument', data, arguments=arguments[:-4])
    process.run('duplicate_watched_vault', data, arguments=arguments + ['--vault', expected['vault']])
    require(original_pins == pins(native, fixture), 'native or fixture changed during process suite')
    return {'format': FORMAT, 'passed': True, 'cases_run': len(process.cases),
            'cases': process.cases, 'tests_skipped': 0, 'pins_before': original_pins,
            'pins_after': pins(native, fixture), 'baseline': baseline,
            'mainnet_enabled': False, 'live_arbitrum_authority': False,
            'arbitrum_one_rpc_independent_qualification': False,
            'nitro_execution_verified_by_checker': False,
            'rollup_settlement_verified': False,
            'scope': 'genuine-owned-Nitro-history-commitments-against-separately-supplied-checkpoint'}


def child(parent_namespace, native, fixture, evidence):
    before = disconnected_namespace(parent_namespace)
    report = exercise(Path(native), Path(fixture), Path(evidence))
    after = disconnected_namespace(parent_namespace)
    require(before['namespace'] == after['namespace'], 'network namespace changed during suite')
    before['same_after_native'] = True
    before['post_run_canaries'] = after['canaries']
    report['network_namespace'] = before
    write(Path(evidence) / 'report.json', report)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ['--child']:
        require(len(argv) == 5, 'invalid internal checker invocation')
        child(*argv[1:])
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, required=True)
    parser.add_argument('--fixture', type=Path, required=True)
    parser.add_argument('--evidence-dir', type=Path, required=True)
    args = parser.parse_args(argv)
    require(sys.platform == 'linux', 'actual process suite requires Linux network namespaces')
    native, fixture, evidence = safe(args.native), safe(args.fixture), safe(args.evidence_dir, new=True)
    source = Path(__file__).absolute().parents[3]
    require(not evidence.is_relative_to(source) and not evidence.is_relative_to(fixture),
            'fresh evidence must be outside source and fixture')
    require(not evidence.exists(), 'existing evidence is never overwritten')
    original = pins(native, fixture)
    evidence.mkdir(mode=0o700)
    environment = {'PATH': '/usr/bin:/bin', 'HOME': str(evidence), 'LANG': 'C', 'LC_ALL': 'C', 'TZ': 'UTC'}
    command = ['/usr/bin/unshare', '--user', '--map-root-user', '--net', sys.executable, '-I', '-B',
               str(Path(__file__).absolute()), '--child', os.readlink('/proc/self/ns/net'),
               str(native), str(fixture), str(evidence)]
    with (evidence / 'checker.stdout.log').open('xb') as stdout, (evidence / 'checker.stderr.log').open('xb') as stderr:
        result = subprocess.run(command, env=environment, stdout=stdout, stderr=stderr, timeout=180, check=False)
    require(original == pins(native, fixture), 'native or fixture changed around namespace run')
    require(result.returncode == 0, 'actual disconnected process suite failed; retained fresh evidence')
    report = json_read(evidence / 'report.json')
    require(report.get('passed') is True and report.get('cases_run') == 26
            and len(report.get('cases', [])) == 26 and report.get('tests_skipped') == 0,
            'native process suite inventory is incomplete')
    namespace = report.get('network_namespace', {})
    require(namespace.get('different_from_parent') is True and namespace.get('loopback_up') is False
            and namespace.get('same_after_native') is True and len(namespace.get('canaries', [])) == 4
            and len(namespace.get('post_run_canaries', [])) == 4, 'network denial receipt is incomplete')
    print(json.dumps({'passed': True, 'cases_run': report['cases_run'], 'evidence_dir': str(evidence),
                      'mainnet_enabled': False, 'live_arbitrum_authority': False}, sort_keys=True))


if __name__ == '__main__':
    main()
