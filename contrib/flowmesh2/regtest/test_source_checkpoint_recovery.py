#!/usr/bin/env python3
"""Actual SIGKILL after checkpoint fsync and before adapter index ACK."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tool_paths
from custody import Q_ASSET, CustodyError, atomic_json, expect
from local_process import raw_hex, wait_until
from operator_wrapper import private_json
from run_roundtrip import canonical
from test_operator_recovery import RecoveryFixture, retained_files


class CheckpointCrash(RecoveryFixture):
    def launch_operator(self, item):
        launcher = self.evidence / 'checkpoint-fault-wrapper.py'
        if not launcher.exists():
            # The test alone wraps the real immutable writer. Source/runtime
            # files are unchanged; the stop occurs only after its actual fsync.
            script = """import json,os,signal,sys
from pathlib import Path
sys.path.insert(0, SOURCE_PARENT)
import operator_wrapper as actual
configuration=Path(sys.argv[sys.argv.index('--config')+1])
directory=Path(json.loads(configuration.read_text())['datadir']).resolve()
arm=directory/'checkpoint-fault-arm.json'
reached=directory/'checkpoint-fault-reached.json'
original=actual.immutable_json
def stop_after_exact_file(path,value):
    original(path,value)
    path=Path(path).resolve()
    if path.parent != directory/'source-checkpoints' or not arm.exists() or reached.exists(): return
    target=json.loads(arm.read_text())
    if value.get('slot_height') != target['slot_height'] or value.get('head_hash') != target['head_hash']: return
    actual.private_json(reached,{'file':path.name,'slot_height':value['slot_height'],'head_hash':value['head_hash']})
    os.kill(os.getpid(),signal.SIGSTOP)
actual.immutable_json=stop_after_exact_file
sys.exit(actual.main())
""".replace('SOURCE_PARENT', repr(str(Path(__file__).resolve().parent)))
            launcher.write_text(script, encoding='utf-8', newline='\n')
        item['process'] = self.start([sys.executable, str(launcher), '--config', str(item['config_path'])],
                                     item['directory'] / 'wrapper.stderr.log', True)
        return item['process']

    def reopen(self, item):
        self.launch_operator(item)
        expect(item['process'].request('bootstrap') == item['public'], 'checkpoint recovery must preserve original private identities')
        return item['process'].request('open', config={**item['open_config'], 'create': False})

    def run(self):
        expect(sys.platform == 'linux', 'actual SIGSTOP/SIGKILL index-ACK gate requires Linux')
        self.build()
        self.start_b3()
        self.start_evm()
        self.bootstrap_operators()
        self.deploy_routes()
        self.open_operators()
        seed = os.urandom(32).hex()
        private_json(self.evidence / 'private-checkpoint-client.json', {'seed': seed})
        q = self.routes['Q']['driver']
        action = self.client_action('checkpoint-recovery-withdrawal', seed, 1, 'WITHDRAW',
                                    {'asset': raw_hex(Q_ASSET), 'amount': 1_000_000,
                                     'recipient': raw_hex(q.accounts[5], 20)}, ref_slot=1)
        q.mint(0, 10_000_000)
        q.approve(0, 10_000_000)
        receipt = q.deposit(0, '0x' + raw_hex(action['account']), 10_000_000)
        event = q.expected_deposit(receipt, '0x' + raw_hex(action['account']), 10_000_000, q.accounts[0])
        self.observe(self.mine_b3(), [{'kind': 'deposit', 'route': 'Q', 'transaction_hash': event['transaction_hash'],
                                     'account': event['account'], 'amount_raw': event['amount_raw'], 'sender': event['sender']}])
        self.commit([], label='actual canonical Q deposit before adapter-index fault')
        self.observe(self.mine_b3(), [])
        candidate = self.operators[2]['process'].request('candidate', actions=[action['frame']])
        victim = self.operators[0]
        index_path = victim['directory'] / 'source-checkpoints/index.jsonl'
        previous_index = index_path.read_bytes()
        previous_lines = previous_index.splitlines()
        expect(len(previous_lines) == 1, 'fault must follow the actual indexed deposit slot')
        previous_tip = json.loads(previous_lines[-1])['hash']
        atomic_json(victim['directory'] / 'checkpoint-fault-arm.json', {'slot_height': 2, 'head_hash': candidate['hash']})
        result = {}

        def actual_commit():
            try:
                result['unexpected_success'] = self.commit([action['frame']], label='actual withdrawal interrupted only after native APPLIED')
            except Exception as error:
                result['interrupted_request'] = str(error)

        pending = threading.Thread(target=actual_commit, daemon=True)
        pending.start()
        marker = victim['directory'] / 'checkpoint-fault-reached.json'
        wait_until(lambda: (marker.is_file() and Path(f'/proc/{victim["process"].process.pid}/stat').read_text().split()[2] == 'T') or result,
                   90, 'actual worker never reached the exact checkpoint-fsync boundary')
        expect(marker.is_file(), 'real protocol must reach checkpoint fsync before the injected stop: ' + str(result))
        stopped = json.loads(marker.read_text(encoding='utf-8'))
        expect(stopped['head_hash'] == candidate['hash'] and stopped['slot_height'] == 2, 'fault must interrupt the expected actual APPLIED withdrawal')
        checkpoint = index_path.parent / stopped['file']
        original_checkpoint = checkpoint.read_bytes()
        expect(index_path.read_bytes() == previous_index, 'SIGSTOP must occur after checkpoint fsync and strictly before index append')
        frozen_native = retained_files(victim['directory'])
        self.kill_operator(victim)
        pending.join(timeout=30)
        expect(not pending.is_alive() and 'interrupted_request' in result and 'unexpected_success' not in result,
               'the actual interrupted wrapper ACK must fail, retaining every original journal')
        expect(retained_files(victim['directory']) == frozen_native and checkpoint.read_bytes() == original_checkpoint,
               'SIGKILL must preserve the exact native APPLIED prefix and fsynced checkpoint')
        restored = self.reopen(victim)
        expect(restored['height'] == 2 and restored['head_hash'] == candidate['hash'], 'native replay must prove the exact original APPLIED head')
        expect(retained_files(victim['directory']) == frozen_native, 'adapter recovery must never append native votes, debit again or reset keys')
        entry = {'file': checkpoint.name, 'sha256': hashlib.sha256(original_checkpoint).hexdigest(), 'previous': previous_tip}
        entry['hash'] = hashlib.sha256(canonical(entry).encode()).hexdigest()
        expected_index = previous_index + (canonical(entry) + '\n').encode()
        expect(index_path.read_bytes() == expected_index and checkpoint.read_bytes() == original_checkpoint,
               'recovery must complete exactly one expected index entry while preserving the fsynced file')
        account = victim['process'].request('account', account=action['account'])
        balance = next(value for value in account['balances'] if raw_hex(value['asset']) == raw_hex(Q_ASSET))
        withdrawal = victim['process'].request('withdrawal', withdrawal_id=action['id'])
        totals = victim['process'].request('totals')
        ledger = next(value for value in totals['assets'] if raw_hex(value['asset']) == raw_hex(Q_ASSET))
        expect(account.get('verified') is True and balance['available'] == 8_999_000 and balance['reserved'] == 0 and
               withdrawal.get('verified') is True and withdrawal['record']['amount'] == 1_000_000 and
               withdrawal['record']['fee'] == 1000 and not withdrawal['record']['paid'],
               'recovery must preserve one actual principal, one stable fee and a certified unpaid obligation')
        expect(totals['conservation'] is True and ledger['deposits'] == 10_000_000 and ledger['pending'] == 1_000_000 and
               ledger['protocol_available'] == 1000 and ledger['total_liabilities'] == 10_000_000,
               'full certified liabilities must reconcile to the actual canonical deposit once')
        victim['process'].close()
        again = self.reopen(victim)
        expect(again['height'] == 2 and again['head_hash'] == candidate['hash'] and index_path.read_bytes() == expected_index and
               retained_files(victim['directory']) == frozen_native and victim['process'].request('totals')['assets'] == totals['assets'],
               'a second cold open must be idempotent without another index append or economic effect')
        report = {'passed': True, 'agreement': self.agreement, 'profile_name': self.profile_name,
                  'scope': 'actual four-process agreement followed by one owned operator SIGKILL after native APPLIED/checkpoint fsync before adapter index ACK',
                  'fault_injection': 'evidence-only wrapper writer interception; no source/runtime fault hook',
                  'actual_deposit': event, 'original_action': action['id'], 'victim_seat': victim['open_config']['seat'],
                  'interrupted_request': result['interrupted_request'], 'checkpoint_file': checkpoint.name,
                  'checkpoint_sha256_unchanged': hashlib.sha256(original_checkpoint).hexdigest(),
                  'completed_exact_index_entry': entry, 'reopened_info': restored, 'second_open_info': again,
                  'certified_withdrawal': withdrawal, 'totals': totals,
                  'qualification': 'exact local adapter ACK crash boundary; no claim of production chain finality or network transport'}
        atomic_json(self.evidence / 'source-checkpoint-recovery-report.json', report)
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True)
    parser.add_argument('--evidence-dir', required=True)
    parser.add_argument('--agreement', choices=('pbft-fixed-view', 'pbft-view-change'), default='pbft-view-change')
    tool_paths.add(parser, 'worker', 'b3coind', 'b3coin_cli', 'tools', 'solc')
    args = parser.parse_args()
    tool_paths.resolve(parser, args, 'worker', 'b3coind', 'b3coin_cli', 'tools', 'solc')
    fixture = None
    try:
        fixture = CheckpointCrash(args)
        fixture.run()
        print(canonical({'passed': True, 'report': str(fixture.evidence / 'source-checkpoint-recovery-report.json')}))
        return 0
    except Exception as error:
        if fixture is not None:
            atomic_json(fixture.evidence / 'source-checkpoint-recovery-failure.json', {'passed': False, 'detail': str(error)})
        print('source checkpoint recovery test failed: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if fixture is not None:
            fixture.close()


if __name__ == '__main__':
    sys.exit(main())
