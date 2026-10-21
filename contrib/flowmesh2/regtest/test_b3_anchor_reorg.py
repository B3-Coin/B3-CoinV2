#!/usr/bin/env python3
"""Independent actual B3-anchor fork test; EVM backing stays canonical."""
import argparse
import json
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from custody import Q_ASSET, CustodyError, atomic_json, expect
from local_process import raw_hex, wait_until
from operator_wrapper import AGREEMENT_PROFILES, header_fields, private_json
from run_roundtrip import Roundtrip, canonical


class B3AnchorFork(Roundtrip):
    def run(self):
        self.build()
        self.start_b3()
        self.start_evm()
        self.bootstrap_operators()
        self.deploy_routes()
        self.open_operators()
        seed = os.urandom(32).hex()
        private_json(self.evidence / 'private-fork-client.json', {'seed': seed})
        q = self.routes['Q']['driver']
        action = self.client_action('fork-test-withdrawal', seed, 1, 'WITHDRAW',
                                    {'asset': raw_hex(Q_ASSET), 'amount': 1_000_000,
                                     'recipient': raw_hex(q.accounts[5], 20)}, ref_slot=1)
        q.mint(0, 10_000_000)
        q.approve(0, 10_000_000)
        receipt = q.deposit(0, '0x' + raw_hex(action['account']), 10_000_000)
        event = q.expected_deposit(receipt, '0x' + raw_hex(action['account']), 10_000_000, q.accounts[0])
        self.observe(self.mine_b3(), [{'kind': 'deposit', 'route': 'Q', 'transaction_hash': event['transaction_hash'],
                                      'account': event['account'], 'amount_raw': event['amount_raw'], 'sender': event['sender']}])
        self.commit([], label='actual Q deposit before independent B3 fork test')
        old_anchor = self.mine_b3()
        self.observe(old_anchor, [])
        self.commit([action['frame']], label='actual unpaid Q withdrawal anchored before B3 fork')
        proofs = [item['process'].request('withdrawal', withdrawal_id=action['id']) for item in self.operators]
        expect(all(value.get('found') and value.get('verified') is True and not value['record']['paid'] for value in proofs),
               'fork test requires a real committed unpaid obligation on all operators')
        expect(all(header_fields(value['header'])['anchor'] == old_anchor for value in proofs),
               'the exact soon-invalidated anchor must belong to the committed withdrawal header')
        before_evm = self.source_end()
        before_balances = q.balances(q.accounts[5])
        old_display = bytes.fromhex(old_anchor['hash'])[::-1].hex()
        for node in self.b3:
            node['rpc'].call('invalidateblock', old_display)
        wait_until(lambda: all(node['rpc'].call('getblockchaininfo')['blocks'] == old_anchor['height'] - 1 for node in self.b3),
                   60, 'owned B3 nodes did not invalidate their exact old committed anchor')
        hashes = self.b3[0]['rpc'].call('generatetodescriptor', 3, 'raw(52)', 1000000)
        expect(len(hashes) == 3, 'alternate payout script must produce actual different replacement corridor blocks')
        replacement = self.b3[0]['rpc'].anchor()
        wait_until(lambda: all(node['rpc'].anchor() == replacement for node in self.b3), 60,
                   'owned B3 nodes did not synchronize the actual taller alternate branch')
        expect(replacement['height'] > old_anchor['height'], 'fork must exercise a valid new higher anchor, not a height regression')
        expect(all(raw_hex(node['rpc'].call('getblockhash', old_anchor['height'])) != old_display for node in self.b3),
               'the old committed block must actually be absent from every current canonical B3 chain')
        expect(self.source_end() == before_evm and q.balances(q.accounts[5]) == before_balances,
               'this independent B3 test must not remove EVM backing or alter a custody payment')
        value = proofs[0]['record']
        timestamp = int(self.evm_rpc.call('eth_getBlockByNumber', ['latest', False])['timestamp'], 16)
        authorization = {'id': '0x' + raw_hex(value['id']), 'account': '0x' + raw_hex(value['account']), 'asset': Q_ASSET,
                         'amount': str(value['amount']), 'recipient': '0x' + raw_hex(value['recipient'], 20),
                         'slot': value['slot'], 'stateRoot': '0x' + header_fields(proofs[0]['header'])['state_root'],
                         'validUntil': timestamp + 240}
        refusals = []
        for item in self.operators:
            for command, fields in (('observe_receipts', {'anchor': replacement, 'events': [], 'evm_end': before_evm}),
                                    ('sign_withdrawal', {'withdrawal': authorization})):
                try:
                    item['process'].request(command, **fields)
                except CustodyError as error:
                    expect('anchor' in str(error), 'refusal must come from the removed committed B3 anchor')
                    refusals.append({'seat': item['open_config']['seat'], 'command': command, 'detail': str(error)})
                else:
                    raise CustodyError('operator silently reanchored/signed across the removed committed B3 block')
        recovered = self.operators[0]
        recovered['process'].close()
        self.launch_operator(recovered)
        expect(recovered['process'].request('bootstrap') == recovered['public'], 'B3-fork restart must preserve original operator identities')
        try:
            recovered['process'].request('open', config={**recovered['open_config'], 'create': False})
        except CustodyError as error:
            expect('anchor' in str(error), 'cold open must refuse the removed original B3 anchor')
            restart_refusal = str(error)
        else:
            raise CustodyError('cold open forgot the committed B3 anchor')
        report = {'passed': True, 'scope': 'isolated actual B3 canonical-anchor removal; no EVM reorg or PoS-finality claim',
                  'old_committed_anchor': old_anchor, 'replacement_anchor': replacement,
                  'canonical_evm_checkpoint_unchanged': before_evm, 'custody_balances_unchanged': before_balances,
                  'pending_obligation': proofs[0], 'refusals': refusals, 'restart_refusal': restart_refusal,
                  'no_further_happy_progress_after_fork': True}
        atomic_json(self.evidence / 'b3-anchor-reorg-report.json', report)
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True)
    parser.add_argument('--evidence-dir', required=True)
    parser.add_argument('--worker', default='/home/josh/B3-builds/fm2-regtest/bin/fm2-regtest-node')
    parser.add_argument('--b3coind', default='/home/josh/B3-builds/regtest-b3-node/bin/b3coind')
    parser.add_argument('--b3coin-cli', default='/home/josh/B3-builds/regtest-b3-node/bin/b3coin-cli')
    parser.add_argument('--tools', default='/home/josh/B3-tools/foundry-v1.8.5')
    parser.add_argument('--solc', default='/home/josh/B3-tools/solc-0.8.35')
    parser.add_argument('--agreement', choices=tuple(AGREEMENT_PROFILES), default='pbft-fixed-view')
    args = parser.parse_args()
    fixture = None
    try:
        fixture = B3AnchorFork(args)
        fixture.run()
        print(canonical({'passed': True, 'report': str(fixture.evidence / 'b3-anchor-reorg-report.json')}))
        return 0
    except Exception as error:
        if fixture is not None:
            atomic_json(fixture.evidence / 'b3-anchor-reorg-failure.json', {'passed': False, 'detail': str(error)})
        print('B3 anchor fork test failed: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if fixture is not None:
            fixture.close()


if __name__ == '__main__':
    sys.exit(main())
