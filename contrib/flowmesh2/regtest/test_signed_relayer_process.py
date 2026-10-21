#!/usr/bin/env python3
"""Actual private gas-child fault qualification on a fresh unforked Anvil.

The only successfully submitted test call targets an unknown withdrawal and
reverts on our empty synthetic vault. No fixture custody key or gas seed is read
by this coordinator. Faulted histories and the original private identities are
retained in separate diagnostic datadirs; none is reset or silently repaired.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

from custody import (CHAIN_ID, Q_ASSET, TEST_MNEMONIC, CustodyDriver, CustodyError,
                     LocalRpc, atomic_json, expect)
from custody_policy import canonical, pin_custody
from local_process import JsonProcess, OwnedProcess, free_ports, wait_until
from signed_relayer import MAX_GAS_LIMIT, decode_transaction, rlp


UNKNOWN = '0x' + '12' * 32


def require_refusal(child, command, fields, detail):
    try:
        child.request(command, **fields)
    except CustodyError as error:
        prefix = 'worker refused ' + command + ': '
        expect(str(error).startswith(prefix), 'private child must return a typed command refusal: ' + str(error))
        value = json.loads(str(error)[len(prefix):])
        expect(set(value) == {'code', 'detail', 'fatal'} and value['code'] == 'SIGNED_RELAYER_REFUSAL' and
               value['fatal'] is False and detail in value['detail'], 'private child refusal reason differs: ' + str(error))
        return value['detail']
    raise CustodyError('private gas child accepted a forbidden command: ' + command)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True)
    parser.add_argument('--evidence-dir', required=True)
    parser.add_argument('--worker', required=True)
    parser.add_argument('--tools', default='/home/josh/B3-tools/foundry-v1.8.5')
    parser.add_argument('--solc', default='/home/josh/B3-tools/solc-0.8.35')
    args = parser.parse_args()
    source, evidence = Path(args.source_root).resolve(strict=True), Path(args.evidence_dir).resolve()
    if evidence == source or source in evidence.parents or (evidence.exists() and any(evidence.iterdir())):
        raise CustodyError('signed-relayer process test requires fresh evidence outside source; no history is erased')
    evidence.mkdir(parents=True, exist_ok=True)
    tools, worker = Path(args.tools).resolve(strict=True), Path(args.worker).resolve(strict=True)
    command = [str(tools / 'forge'), 'build', '--root', str(source / 'contracts'), '--offline', '--use', args.solc,
        '--out', str(evidence / 'contracts-out'), '--cache-path', str(evidence / 'contracts-cache'),
        'regtest/RebirthRegtestToken.sol', 'custody/RebirthCustodyVault.sol']
    with (evidence / 'forge-build.log').open('w', encoding='utf-8') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)
    url = 'http://127.0.0.1:' + str(free_ports(1)[0])
    anvil = OwnedProcess([str(tools / 'anvil'), '--host', '127.0.0.1', '--port', url.rsplit(':', 1)[1],
        '--chain-id', str(CHAIN_ID), '--mnemonic', TEST_MNEMONIC, '--accounts', '6', '--silent'], evidence / 'anvil.log')
    children = []
    try:
        rpc = LocalRpc(url)
        wait_until(lambda: anvil.process.poll() is None and rpc.assert_fixture(), 20, 'owned Anvil fixture not ready')
        config = {'rpc_url': url, 'cast': str(tools / 'cast'), 'q_asset': Q_ASSET, 'custody_asset': Q_ASSET,
                  'instance': '0x' + 'bc' * 32, 'account_count': 6, 'deployer_index': 0}
        driver = CustodyDriver(config, evidence / 'deployment-driver')
        token, _ = driver.deploy(evidence / 'contracts-out/RebirthRegtestToken.sol/RebirthRegtestToken.json')
        driver.token = config['token'] = token
        roster = '[' + ','.join(driver.operator_addresses) + ']'
        vault, _ = driver.deploy(evidence / 'contracts-out/RebirthCustodyVault.sol/RebirthCustodyVault.json',
            driver.cast.abi_encode('constructor(address,bytes32,bytes32,address[4],(uint256,uint64,uint64,uint64))',
                token, Q_ASSET, driver.instance, roster, '(31337,30,60,3600)'))
        driver.vault = config['vault'] = vault
        pin_custody(driver, config)
        script = str(Path(__file__).with_name('signed_relayer.py'))

        def launch(label, fund=False):
            directory = evidence / label
            directory.mkdir(mode=0o700)
            child_config = {'format': 'fm2-signed-relayer/1', 'worker': str(worker),
                            'datadir': str(directory / 'private-gas'), 'custody_config': config}
            path = directory / 'config.json'
            atomic_json(path, child_config)
            child = JsonProcess([sys.executable, script, '--config', str(path)], directory / 'child.stderr.log')
            children.append(child)
            public = child.request('info')
            expect(public['mainnet_enabled'] is False and public['chain_id'] == 31337 and
                   public['address'] not in driver.operator_addresses, 'gas child identity/release policy differs')
            if fund:
                driver.transact(public['address'], '0x', native_value=10 ** 17)
            return child, public, directory, path

        def restart(path, directory, label):
            child = JsonProcess([sys.executable, script, '--config', str(path)], directory / (label + '.stderr.log'))
            children.append(child)
            return child

        child, public, directory, path = launch('normal', fund=True)
        withdrawal = {'id': UNKNOWN, 'account': '0x' + '45' * 32, 'asset': Q_ASSET, 'amount': '1',
                      'recipient': driver.accounts[0], 'slot': 1, 'stateRoot': '0x' + '67' * 32,
                      'validUntil': int(rpc.call('eth_getBlockByNumber', ['latest', False])['timestamp'], 16) + 240}
        cases = [
            ('send', {'to': driver.accounts[0], 'value': '1'}, 'cannot sign arbitrary'),
            ('request', {'withdrawal': withdrawal, 'signatures': []}, 'custody quorum'),
            ('request', {'withdrawal': withdrawal, 'signatures': ['0x01'] * 3}, 'wrong byte length'),
            ('finalize', {'withdrawal_id': UNKNOWN, 'gas': 0}, 'gas limit is outside'),
            ('finalize', {'withdrawal_id': UNKNOWN, 'gas': MAX_GAS_LIMIT + 1}, 'gas limit is outside'),
            ('finalize', {'withdrawal_id': UNKNOWN, 'gas': True}, 'boolean is not'),
            ('finalize', {'withdrawal_id': UNKNOWN, 'require_success': 'yes'}, 'explicit boolean'),
            ('finalize', {'withdrawal_id': UNKNOWN, 'to': driver.accounts[0]}, 'unexpected signed finalization'),
            ('resume', {'nonce': 0}, 'unknown signed relayer nonce'),
        ]
        refusals = []
        for command, fields, detail in cases:
            refused = require_refusal(child, command, fields, detail)
            after = child.request('info')
            expect(after['signed_intents'] == 0 and int(rpc.call('eth_getTransactionCount', [public['address'], 'pending']), 16) == 0,
                   'denied gas command must not sign an intent or consume any nonce')
            refusals.append({'command': command, 'detail': refused})

        # A duplicate process cannot obtain this identity lock, even for info.
        duplicate = subprocess.run([sys.executable, script, '--config', str(path)], input=b'{"id":1,"cmd":"info"}\n',
                                   capture_output=True, timeout=30)
        expect(duplicate.returncode != 0 and b'another relayer process owns' in duplicate.stderr,
               'second child must not share an active private gas identity')

        snapshot = rpc.call('evm_snapshot')
        receipt = child.request('finalize', withdrawal_id=UNKNOWN, gas=100000, require_success=False)
        expect(int(receipt['status'], 16) == 0, 'unknown synthetic claim must revert without moving reserves')
        raw_hash = receipt['transactionHash']
        expect(child.request('pending') == {'required': False}, 'canonical resolved intent must not be pending')
        child.close()
        child = restart(path, directory, 'restarted')
        again = child.request('info')
        expect(again['address'] == public['address'] and again['signed_intents'] == 1 and again['resolved_intents'] == 1,
               'restart must retain exact private identity and signed nonce history')
        repeated = child.request('resume', nonce=0, require_success=False)
        expect(repeated['transactionHash'] == raw_hash and int(rpc.call('eth_getTransactionCount', [public['address'], 'latest']), 16) == 1,
               'exact replay must not allocate a new nonce or change signed bytes')
        expect(rpc.call('evm_revert', [snapshot]) is True, 'owned snapshot rollback must succeed')
        pending = child.request('pending')
        expect(pending == {'required': True, 'nonce': 0, 'action': {'operation': 'finalize', 'withdrawal_id': UNKNOWN}},
               'reorg must expose the original exact operation and nonce')
        recovered = child.request('resume', nonce=pending['nonce'], require_success=False)
        expect(recovered['transactionHash'] == raw_hash and int(rpc.call('eth_getTransactionCount', [public['address'], 'latest']), 16) == 1,
               'reorg recovery must rebroadcast its original signed transaction, once')
        child.close()

        startup_faults = []
        def refuse_start(path, directory, expected):
            proc = subprocess.run([sys.executable, script, '--config', str(path)], input=b'{"id":1,"cmd":"info"}\n',
                                  capture_output=True, timeout=30)
            (directory / 'expected-startup-refusal.log').write_bytes(proc.stderr)
            expect(proc.returncode != 0 and expected.encode() in proc.stderr,
                   'faulted child must refuse startup for the precise durable fault: ' + expected)
            startup_faults.append(expected)

        missing, _, missing_dir, missing_path = launch('missing-key')
        missing.close()
        key = missing_dir / 'private-gas/private-gas-identity.json'
        held = key.with_name('fixture-held-original-private-gas-identity.json')
        key.rename(held)  # Keep the original secret; do not replace or print it.
        refuse_start(missing_path, missing_dir, 'private gas identity missing; refusing replacement')
        expect(not key.exists() and held.exists(), 'missing identity refusal must never generate a replacement')

        lost, _, lost_dir, lost_path = launch('missing-journal', fund=True)
        lost.request('finalize', withdrawal_id=UNKNOWN, gas=100000, require_success=False)
        lost.close()
        journal = lost_dir / 'private-gas/transactions.jsonl'
        held_journal = journal.with_name('fixture-held-original-transactions.jsonl')
        journal.rename(held_journal)
        refuse_start(lost_path, lost_dir, 'original journal missing; refusing empty-history recovery')
        expect(not journal.exists() and held_journal.exists(), 'missing signed history must never be recreated as empty')

        torn, _, torn_dir, torn_path = launch('torn-journal', fund=True)
        torn.request('finalize', withdrawal_id=UNKNOWN, gas=100000, require_success=False)
        torn.close()
        journal = torn_dir / 'private-gas/transactions.jsonl'
        with journal.open('ab') as stream:
            stream.write(b'{"torn":')
            stream.flush()
            os.fsync(stream.fileno())
        damaged = hashlib.sha256(journal.read_bytes()).hexdigest()
        refuse_start(torn_path, torn_dir, 'journal is torn/oversized; refusing repair/truncation')
        expect(hashlib.sha256(journal.read_bytes()).hexdigest() == damaged, 'torn history must remain untouched for diagnosis')

        corrupt, _, corrupt_dir, corrupt_path = launch('invalid-raw-signature', fund=True)
        corrupt.request('finalize', withdrawal_id=UNKNOWN, gas=100000, require_success=False)
        corrupt.close()
        journal = corrupt_dir / 'private-gas/transactions.jsonl'
        records = [json.loads(line) for line in journal.read_bytes().splitlines()]
        first = records[0]['value']
        fields = decode_transaction(first['raw_transaction'])
        r = 1 if int.from_bytes(fields[7], 'big') != 1 else 2
        fields[7] = r.to_bytes((r.bit_length() + 7) // 8, 'big')
        first['raw_transaction'] = '0x' + rlp(fields).hex()
        first['transaction_hash'] = driver.cast.keccak(first['raw_transaction'])
        # Recompute the public hash chain, so refusal must come from actual
        # private-gas signature verification rather than an easy hash mismatch.
        previous = '00' * 32
        for record in records:
            record.pop('hash')
            record['previous'] = previous
            previous = hashlib.sha256(canonical(record).encode()).hexdigest()
            record['hash'] = previous
        journal.rename(journal.with_name('fixture-held-original-transactions.jsonl'))
        journal.write_text(''.join(canonical(record) + '\n' for record in records), encoding='utf-8')
        damaged = hashlib.sha256(journal.read_bytes()).hexdigest()
        refuse_start(corrupt_path, corrupt_dir, 'Validation failed.')
        expect(hashlib.sha256(journal.read_bytes()).hexdigest() == damaged,
               'invalid signed history must never be truncated or automatically replaced')
        expect(driver.call_uint(vault, 'locked()') == 0 and driver.call_uint(vault, 'pendingTotal()') == 0,
               'process faults must leave the empty synthetic custody reserve unchanged')
        report = {'passed': True, 'chain_id': 31337, 'mainnet_enabled': False,
                  'denied_before_signing': refusals, 'exclusive_identity_lock_refused': True,
                  'real_reverted_transaction': raw_hash, 'restart_exact_nonce_replay': True,
                  'reorg_exact_raw_recovery': True, 'retained_startup_faults': startup_faults,
                  'coordinator_read_private_seed': False, 'reserves_unchanged': True}
        atomic_json(evidence / 'signed-relayer-process-report.json', report)
        print(json.dumps({'passed': True, 'report': str(evidence / 'signed-relayer-process-report.json')}))
        return 0
    finally:
        for child in reversed(children):
            child.close()
        anvil.close()


if __name__ == '__main__':
    sys.exit(main())
