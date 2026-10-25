#!/usr/bin/env python3
"""Disconnected process controls over genuine owned-rollup settlement bytes.

This qualifies the explicitly trusted local L1 checkpoint boundary. It does
not qualify public Ethereum PoS, public Arbitrum, or independent Nitro STF.
"""
import argparse
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import sys
import time


def need(value, reason):
    if not value:
        raise ValueError(reason)


def unique(pairs):
    out = {}
    for key, value in pairs:
        need(key not in out, 'duplicate JSON member')
        out[key] = value
    return out


def load(path):
    return json.loads(Path(path).read_bytes(), object_pairs_hook=unique)


def save(path, value):
    with Path(path).open('x', encoding='utf8') as stream:
        json.dump(value, stream, sort_keys=True, indent=2, allow_nan=False)
        stream.write('\n')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def limits():
    resource.setrlimit(resource.RLIMIT_FSIZE, (8 * 1024**2, 8 * 1024**2))
    resource.setrlimit(resource.RLIMIT_AS, (768 * 1024**2, 768 * 1024**2))
    resource.setrlimit(resource.RLIMIT_CPU, (20, 20))


def exercise(args):
    sys.path.insert(0, str(args.source / 'contrib/b3bridge/arbitrum_history'))
    from proof_bundle import decode_proof, encode_proof
    from qualify_history import disconnected_namespace
    namespace = disconnected_namespace(args.parent_namespace)
    manifest = load(args.export / 'manifest.json')
    base = args.export
    settlement = decode_proof((base / 'settlement-proof.bin').read_bytes())
    history = decode_proof((base / 'history-proof.bin').read_bytes())
    request = manifest['request']
    need(len(settlement.l1_accounts) == 16, 'genuine complete16-account owned closure required')
    need(history.blocks and history.blocks[0].transactions, 'genuine complete transactions required')
    need(load(base / 'ancestor-pages.json')['pages'], 'genuine nontrivial ancestry required')
    rows = []
    frozen = sha(args.native.read_bytes())

    def command(export, policy=args.trusted_policy):
        out = [str(args.native), '--regtest', '--export', str(export),
               '--trusted-policy', str(policy), '--trusted-policy-sha256', '0x'+sha(policy.read_bytes()),
               '--trusted-l1-head', manifest['l1']['hash'],
               '--l1-genesis', manifest['l1']['genesis_hash'],
               '--l2-genesis', manifest['source']['genesis_hash']]
        for key in ('previous_height', 'previous_hash', 'to_height', 'to_hash'):
            out.extend(['--' + key.replace('_', '-'), str(request[key])])
        for vault in args.vault:
            out.extend(['--vault', vault])
        return out

    def run(name, *, mutate=None, change=None, success=False, policy=None):
        directory = args.evidence / name
        directory.mkdir(mode=0o700)
        export = base
        if mutate:
            export = directory / 'export'
            shutil.copytree(base, export, symlinks=False)
            mutate(export)
        argv = command(export, policy or args.trusted_policy)
        if change:
            argv = change(argv)
        started = time.monotonic()
        result = subprocess.run(argv, capture_output=True, timeout=30, stdin=subprocess.DEVNULL,
            env={'PATH': '/usr/bin:/bin', 'HOME': str(args.evidence), 'LANG': 'C', 'TZ': 'UTC'},
            preexec_fn=limits)
        need(len(result.stdout) <= 8 * 1024**2 and len(result.stderr) <= 65536, 'bounded native diagnostics required')
        (directory / 'stdout.json').write_bytes(result.stdout)
        (directory / 'stderr.log').write_bytes(result.stderr)
        need(result.returncode in (0, 2), name + ': native process crashed')
        need(not result.stderr, name + ': unexpected native diagnostic')
        value = load(directory / 'stdout.json')
        need(bool(value), name + ': structured native result required')
        need((result.returncode == 0) is success, name + ': unexpected native exit')
        for flag in ('mainnet_enabled', 'arbitrum_authority', 'ethereum_pos_verified', 'execution_verified'):
            if value:
                need(value.get(flag) is False, name + ': authority scope crossed: ' + flag)
        if value:
            need(value.get('owned_settlement_verified') is success, name + ': incorrect settlement result')
            need((value.get('result') == 'OK') is success, name + ': incorrect result code')
        if success:
            need(value.get('format') == 'b3-owned-arbitrum-settlement-check/1', 'positive format differs')
            need(value.get('commitments_match') is True, 'range commitments not checked')
        row = {'name': name, 'passed': True, 'expected_success': success, 'exit_code': result.returncode,
               'result': value.get('result'), 'stdout_sha256': sha(result.stdout),
               'stderr_sha256': sha(result.stderr), 'seconds': round(time.monotonic()-started, 4)}
        rows.append(row)
        return value

    def argument(name, value):
        def change(argv):
            argv = argv.copy()
            argv[argv.index(name)+1] = str(value)
            return argv
        return change

    def write_proof(name, proof):
        def mutate(directory):
            (directory / name).write_bytes(encode_proof(proof))
            # Update untrusted producer digest too: hashes are not authority.
            v = load(directory / 'manifest.json')
            key = {'settlement-proof.bin': 'settlement_proof_sha256',
                   'history-proof.bin': 'history_proof_sha256'}[name]
            v[key] = sha((directory / name).read_bytes())
            (directory / 'manifest.json').write_text(json.dumps(v, sort_keys=True) + '\n')
        return mutate

    positive = run('genuine-settlement', success=True)
    repeated = run('genuine-repeat', success=True)
    need(positive == repeated, 'same bounded proof did not verify deterministically')
    run('explicit-regtest-required', change=lambda argv: [s for s in argv if s != '--regtest'])
    zero = '0x' + '00'*32
    for label, option in (('wrong-l1-head', '--trusted-l1-head'), ('wrong-l1-genesis', '--l1-genesis'),
                          ('wrong-l2-genesis', '--l2-genesis'), ('wrong-range-parent', '--previous-hash'),
                          ('wrong-range-tip', '--to-hash'), ('wrong-policy-pin', '--trusted-policy-sha256')):
        run(label, change=argument(option, zero))
    run('omit-closure-account', mutate=write_proof('settlement-proof.bin',
        replace(settlement, l1_accounts=settlement.l1_accounts[:-1])))
    run('duplicate-closure-account', mutate=write_proof('settlement-proof.bin',
        replace(settlement, l1_accounts=settlement.l1_accounts[:-1] + (settlement.l1_accounts[0],))))
    account = settlement.l1_accounts[0]
    node = account.nodes[-1]
    bad_account = replace(account, nodes=account.nodes[:-1] + (node[:-1]+bytes([node[-1]^1]),))
    run('corrupt-l1-mpt-node', mutate=write_proof('settlement-proof.bin',
        replace(settlement, l1_accounts=(bad_account,) + settlement.l1_accounts[1:])))
    bad_account = replace(account, storage=account.storage[:-1])
    run('omit-rollup-storage-word', mutate=write_proof('settlement-proof.bin',
        replace(settlement, l1_accounts=(bad_account,) + settlement.l1_accounts[1:])))
    run('alter-assertion-preimage', mutate=write_proof('settlement-proof.bin',
        replace(settlement, assertion=replace(settlement.assertion, block_hash=bytes(32)))))
    def omit_pages(directory):
        v = load(directory / 'ancestor-pages.json')
        v['pages'] = []
        (directory / 'ancestor-pages.json').write_text(json.dumps(v)+'\n')
    run('omit-ancestor-spine', mutate=omit_pages)
    def corrupt_l1(directory):
        p = directory / 'l1-header.rlp'
        raw = p.read_bytes()
        p.write_bytes(raw[:-1] + bytes([raw[-1]^1]))
    run('alter-l1-header-preimage', mutate=corrupt_l1)
    def relabel(directory):
        p = directory / 'manifest.json'
        v = load(p)
        v['source']['chain_id'] = 42161
        p.write_text(json.dumps(v)+'\n')
    run('public-network-relabel', mutate=relabel)
    block = history.blocks[0]
    rec = block.receipts[-1]
    bad_rec = replace(rec, encoded=rec.encoded[:-1] + bytes([rec.encoded[-1]^1]))
    bad_block = replace(block, receipts=block.receipts[:-1]+(bad_rec,))
    run('corrupt-receipt', mutate=write_proof('history-proof.bin',
        replace(history, blocks=(bad_block,)+history.blocks[1:])))
    rec = block.transactions[-1]
    bad_rec = replace(rec, encoded=rec.encoded[:-1] + bytes([rec.encoded[-1]^1]))
    bad_block = replace(block, transactions=block.transactions[:-1]+(bad_rec,))
    run('corrupt-transaction', mutate=write_proof('history-proof.bin',
        replace(history, blocks=(bad_block,)+history.blocks[1:])))
    bad_block = replace(block, receipts=block.receipts[:-1], transactions=block.transactions[:-1])
    run('omit-transaction-and-receipt', mutate=write_proof('history-proof.bin',
        replace(history, blocks=(bad_block,)+history.blocks[1:])))
    run('omit-oldest-range-block', mutate=write_proof('history-proof.bin',
        replace(history, blocks=history.blocks[:-1])))
    # Change an exporter-observed allowlist while trusted baseline stays fixed.
    def poison_policy(directory):
        p = directory / 'deployment-policy.json'
        v = load(p)
        v['code_accounts'][0]['code_keccak256'] = zero
        p.write_text(json.dumps(v)+'\n')
    # An unused producer annotation may safely be ignored; it must never make
    # corrupted raw proofs pass. Combine it with an actually omitted account.
    def poisoned_omission(directory):
        poison_policy(directory)
        write_proof('settlement-proof.bin', replace(settlement, l1_accounts=settlement.l1_accounts[:-1]))(directory)
    run('producer-policy-cannot-authorize-omission', mutate=poisoned_omission)
    need(sha(args.native.read_bytes()) == frozen, 'native executable changed during qualification')
    report = {'format': 'b3-owned-settlement-process-qualification/1', 'passed': True,
              'mainnet_enabled': False, 'public_arbitrum_qualified': False,
              'ethereum_pos_verified': False, 'independent_nitro_stf_verified': False,
              'checkpoint_trust': 'explicit-owned-L1-fullnode-checkpoint',
              'native_sha256': frozen, 'trusted_policy_sha256': sha(args.trusted_policy.read_bytes()),
              'network_namespace': namespace, 'cases': rows, 'cases_run': len(rows),
              'positive_candidate_events': positive.get('candidate_events', [])}
    save(args.evidence / 'report.json', report)
    print(json.dumps({key: report[key] for key in ('passed', 'mainnet_enabled', 'cases_run')}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('source', 'native', 'export', 'trusted-policy', 'evidence'):
        parser.add_argument('--'+key, type=Path, required=True)
    parser.add_argument('--vault', action='append', default=[])
    parser.add_argument('--child', action='store_true')
    parser.add_argument('--parent-namespace')
    args = parser.parse_args()
    for key in ('source', 'native', 'export', 'trusted_policy'):
        value = getattr(args, key)
        need(value.is_absolute() and '..' not in value.parts and not value.is_symlink(), 'normal absolute inputs required')
    if args.child:
        exercise(args)
    else:
        args.evidence.mkdir(mode=0o700, parents=False, exist_ok=False)
        command = ['/usr/bin/unshare', '--net', '--', sys.executable, '-I', '-B', str(Path(__file__).absolute()),
                   *sys.argv[1:], '--child', '--parent-namespace', os.readlink('/proc/self/ns/net')]
        raise SystemExit(subprocess.run(command, check=False).returncode)


if __name__ == '__main__':
    main()
