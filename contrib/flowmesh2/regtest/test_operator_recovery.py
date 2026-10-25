#!/usr/bin/env python3
"""Real local operator SIGKILL/reopen gate; no memory journal or key reset."""
import argparse
from collections import deque
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tool_paths
from custody import Q_ASSET, CustodyError, atomic_json, expect
from local_process import raw_hex, wait_until
from operator_wrapper import private_json
from run_roundtrip import Roundtrip, canonical


def digest_file(path):
    if Path(path).name == 'private-identity.json':
        # Preserve the coordinator boundary even for no-reset assertions: one
        # separate short-lived process reads one operator's private file and
        # returns its fingerprint only. No private bytes enter this process.
        code = 'import hashlib,sys;print(hashlib.sha256(open(sys.argv[1],"rb").read()).hexdigest())'
        result = subprocess.run([sys.executable, '-I', '-c', code, str(Path(path).resolve(strict=True))],
                                capture_output=True, text=True, check=True, timeout=10,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        fingerprint = result.stdout.strip()
        expect(len(fingerprint) == 64 and all(value in '0123456789abcdef' for value in fingerprint),
               'private identity fingerprint helper returned unexpected output')
        return fingerprint
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def retained_files(directory):
    """Hash persistent identities/state, excluding ordinary process logs."""
    directory = Path(directory)
    paths = [file for file in (directory / 'state').rglob('*') if file.is_file()]
    paths += [directory / name for name in ('private-identity.json', 'public-identity.json')
              if (directory / name).is_file()]
    return {str(path.relative_to(directory)): digest_file(path) for path in sorted(paths)}


def retained_observations(directory):
    """Hash the exact original adapter batch/checkpoints without changing them."""
    directory = Path(directory)
    paths = [directory / name for name in ('pending-observation.json', 'source-genesis.json')
             if (directory / name).is_file()]
    for name in ('source-observations', 'source-checkpoints', 'retained-receipts', 'receipt-validation-checkpoints'):
        paths += [path for path in (directory / name).rglob('*') if path.is_file()]
    return {str(path.relative_to(directory)): digest_file(path) for path in sorted(paths)}


def proposal_message(candidate, epoch=0, view=0, parent_certificate=None):
    """Canonical existing PROPOSAL codec; no signatures or digest invented."""
    header, body = bytes.fromhex(raw_hex(candidate['header'], 366)), bytes.fromhex(candidate['body'])
    parent = b'' if parent_certificate is None else bytes.fromhex(parent_certificate)
    if len(parent) < 253:
        length = bytes([len(parent)])
    elif len(parent) <= 0xffff:
        length = b'\xfd' + struct.pack('<H', len(parent))
    else:
        raise CustodyError('recovery fixture parent certificate exceeds its bounded codec')
    return (b'\x01\x01' + struct.pack('<QQ', epoch, view) + header + length + parent + body).hex()


def is_prepare(frame):
    message = bytes.fromhex(frame['message'])
    return len(message) >= 3 and message[:3] == b'\x01\x02\x01' and frame.get('durable_vote') is True


class RecoveryFixture(Roundtrip):
    def kill_operator(self, item):
        """Kill only the exact worker child and wrapper created by this fixture."""
        if sys.platform != 'linux':
            raise CustodyError('real SIGKILL recovery gate requires Linux /proc')
        owned = item['process']
        parent = owned.process.pid
        children = Path(f'/proc/{parent}/task/{parent}/children').read_text().split()
        matching = [int(child) for child in children
                    if Path(f'/proc/{child}/exe').resolve(strict=True) == self.worker_path]
        expect(len(matching) == 1 and len(children) == 1, 'must identify exactly the owned operator worker child')
        worker = matching[0]
        expect(os.getpgid(parent) == parent, 'wrapper must still own its dedicated process group')
        os.kill(worker, signal.SIGKILL)
        wait_until(lambda: not Path(f'/proc/{worker}').exists() or
                   Path(f'/proc/{worker}/stat').read_text().split()[2] == 'Z', 10,
                   'owned worker did not stop after SIGKILL')
        os.killpg(parent, signal.SIGKILL)
        owned.process.wait(timeout=10)
        owned.close()
        self.event('operator-sigkill', seat=item['open_config']['seat'], worker_pid=worker,
                   wrapper_pid=parent, signal='SIGKILL')

    def refused_copy(self, original, name, corrupt=False, missing_identity=False):
        """Copies never join agreement or sign a vote; originals remain untouched."""
        directory = self.evidence / 'refusal-copies' / name
        ignore = shutil.ignore_patterns('private-identity.json') if missing_identity else None
        shutil.copytree(original['directory'], directory, ignore=ignore)
        config = {**original['config'], 'datadir': str(directory)}
        config_path = directory / 'copy-wrapper-config.json'
        atomic_json(config_path, config)
        item = {'directory': directory, 'config': config, 'config_path': config_path}
        if corrupt:
            segment = directory / 'state/signer/00000000.seg'
            with segment.open('r+b') as stream:
                stream.seek(64 + 9)  # after the 64-byte segment header: first record payload, CRC/hash chain must fail
                old = stream.read(1)
                expect(len(old) == 1, 'corruption fixture needs an actual framed signer record')
                stream.seek(64 + 9)
                stream.write(bytes([old[0] ^ 1]))
                stream.flush()
                os.fsync(stream.fileno())
        before = retained_files(directory)
        self.launch_operator(item)
        refusal = None
        try:
            identity = item['process'].request('bootstrap')
            expect(not missing_identity and identity == original['public'], 'copy must retain the exact public identity')
            item['process'].request('open', config={**original['open_config'], 'datadir': str(directory / 'state'), 'create': False})
        except CustodyError as error:
            refusal = str(error)
        finally:
            item['process'].close()
        expect(refusal is not None, 'damaged or identity-missing copied store must refuse to open')
        expect(retained_files(directory) == before, 'refusal must not truncate/repair journals or reset identities')
        if missing_identity:
            expect(not (directory / 'private-identity.json').exists(), 'missing private identity must never be regenerated')
            log = (directory / 'wrapper.stderr.log').read_text(errors='replace')
            expect('private identity missing' in log, 'missing-identity refusal must come from the explicit no-reset guard')
        self.event('copied-store-refused', fixture=name, detail=refusal, retained_hashes=before)

    def run(self):
        self.build()
        self.start_b3()
        self.start_evm()
        self.bootstrap_operators()
        self.deploy_routes()
        self.open_operators()
        secret = os.urandom(32).hex()
        private_json(self.evidence / 'private-recovery-client.json', {'seed': secret})
        q = self.routes['Q']['driver']
        action = self.client_action('recovery-withdrawal', secret, 1, 'WITHDRAW',
                                   {'asset': raw_hex(Q_ASSET), 'amount': 1_000_000,
                                    'recipient': raw_hex(q.accounts[5], 20)}, ref_slot=1)
        account = action['account']
        q.mint(0, 10_000_000)
        q.approve(0, 10_000_000)
        receipt = q.deposit(0, '0x' + raw_hex(account), 10_000_000)
        deposit = q.expected_deposit(receipt, '0x' + raw_hex(account), 10_000_000, q.accounts[0])
        self.observe(self.mine_b3(), [{'kind': 'deposit', 'route': 'Q', 'transaction_hash': deposit['transaction_hash'],
                                     'account': deposit['account'], 'amount_raw': deposit['amount_raw'], 'sender': deposit['sender']}])
        self.commit([], label='actual recovery-test Q deposit')
        baseline = self.infos()
        expect(all(info['height'] == 1 for info in baseline), 'recovery fault must start from the real certified deposit')
        self.observe(self.mine_b3(), [])
        for item in self.operators:
            expect(item['process'].request('submit', frame=action['frame'])['code'] == 'ADMITTED', 'original withdrawal must enter each actual pool')
        leader_seat = 2
        leader = self.operators[leader_seat]
        victim = self.operators[0]
        candidate = leader['process'].request('candidate', actions=[action['frame']])
        alternate = self.client_action('conflicting-withdrawal', secret, 1, 'WITHDRAW',
                                      {'asset': raw_hex(Q_ASSET), 'amount': 2_000_000,
                                       'recipient': raw_hex(q.accounts[5], 20)}, ref_slot=1)
        conflict = leader['process'].request('candidate', actions=[alternate['frame']])
        expect(candidate['hash'] != conflict['hash'], 'conflicting proposal must be a distinct executable value')
        started = leader['process'].request('pbft_start', header=candidate['header'], body=candidate['body'])
        expect(not started['applied'], 'one leader cannot decide alone')
        proposal = next(frame for frame in started['frames']
                        if frame['to_seat'] == 0 and frame['message'].startswith('0101'))
        prepared = victim['process'].request('pbft_receive', from_seat=leader_seat, message=proposal['message'])
        expect(not prepared['applied'] and prepared['frames'] and all(is_prepare(frame) for frame in prepared['frames']),
               'pause must occur after durable PREPARE and before any COMMIT/DECISION')
        paused_info = victim['process'].request('info')
        expect(paused_info['height'] == 1 and paused_info['signer_seq'] > baseline[0]['signer_seq'] and
               paused_info['commit_seq'] == baseline[0]['commit_seq'], 'only the signer journal may advance before the fault')
        paused_hashes = retained_files(victim['directory'])
        atomic_json(self.evidence / 'durable-prepare-before-sigkill.json',
                    {'info': paused_info, 'proposal': proposal, 'frames': prepared['frames'], 'retained_hashes': paused_hashes})
        self.kill_operator(victim)
        expect(retained_files(victim['directory']) == paused_hashes, 'SIGKILL must retain the durable signer and applied prefix')
        self.refused_copy(victim, 'corrupt-signer-journal', corrupt=True)
        self.refused_copy(victim, 'missing-private-identity', missing_identity=True)
        self.launch_operator(victim)
        expect(victim['process'].request('bootstrap') == victim['public'], 'reopened operator must retain its own exact keys')
        restored = victim['process'].request('open', config={**victim['open_config'], 'create': False})
        expect(restored['height'] == 1 and restored['head_hash'] == baseline[0]['head_hash'],
               'durable PREPARE alone must never publish the pending withdrawal')
        expect(retained_files(victim['directory']) == paused_hashes, 'open must preserve durable bytes, including the pending PREPARE')
        expect(restored.get('observation_frozen') is True, 'recovered durable PREPARE must freeze the original source batch')
        original_observation = json.loads((victim['directory'] / 'pending-observation.json').read_text(encoding='utf-8'))
        original_adapter_files = retained_observations(victim['directory'])
        frozen_info = victim['process'].request('info')
        changed_anchor = self.mine_b3()
        changed_end = self.source_end(mine_confirmations=True)
        expect(changed_anchor != original_observation['anchor'] and changed_end != original_observation['source']['to'],
               'frozen-observation probes must use actual valid different canonical chain endpoints')
        frozen_refusals = []
        for anchor, endpoint in ((changed_anchor, original_observation['source']['to']),
                                 (original_observation['anchor'], changed_end)):
            try:
                victim['process'].request('observe_receipts', anchor=anchor, events=original_observation['events'], evm_end=endpoint)
            except CustodyError as error:
                expect('frozen' in str(error), 'changed observation must refuse at the recovered durable freeze preflight')
                frozen_refusals.append(str(error))
            else:
                raise CustodyError('reopened prepared operator replaced the original source observation')
            expect(retained_observations(victim['directory']) == original_adapter_files and
                   retained_files(victim['directory']) == paused_hashes and victim['process'].request('info') == frozen_info,
                   'rejected changed anchor/source end must not rewrite the pending pointer, immutable checkpoints or journals')
        refusal_before = victim['process'].request('info')
        conflict_message = proposal_message(conflict, parent_certificate=baseline[leader_seat]['certificate'])
        try:
            victim['process'].request('pbft_receive', from_seat=leader_seat, message=conflict_message)
        except CustodyError as error:
            expect('EQUIVOCATION_REFUSED' in str(error), 'conflict must be the durable same-view refusal')
        else:
            raise CustodyError('reopened operator accepted a conflicting same-height/view proposal')
        expect(victim['process'].request('info') == refusal_before and retained_files(victim['directory']) == paused_hashes,
               'conflicting proposal must not sign, write, publish or poison the recovered original')
        resumed = victim['process'].request('pbft_receive', from_seat=leader_seat, message=proposal['message'])
        expect(resumed['frames'] == prepared['frames'] and not resumed['applied'],
               'exact pending proposal must retransmit byte-identical real durable PREPARE votes')
        expect(retained_files(victim['directory']) == paused_hashes, 'retransmission must not append duplicate signing obligations')
        pending = deque(frame for frame in started['frames'] if frame != proposal)
        pending.extend(resumed['frames'])
        applied, delivered, final_message = {}, [], None
        steps = 0
        while pending:
            steps += 1
            expect(steps <= 2048, 'recovery exchange must remain bounded')
            frame = pending.popleft()
            recipient = frame['to_seat']
            expect(frame['from_seat'] != recipient, 'only actual inter-operator frames may be delivered')
            if frame['message'].startswith('0106'):
                final_message = frame['message']
            reply = self.operators[recipient]['process'].request('pbft_receive', from_seat=frame['from_seat'], message=frame['message'])
            delivered.append(frame)
            pending.extend(reply['frames'])
            for decision in reply['applied']:
                expect(recipient not in applied and decision['head_hash'] == candidate['hash'], 'each real operator applies exactly the original value once')
                applied[recipient] = decision
        for seat, item in enumerate(self.operators):
            reply = item['process'].request('pbft_drain')
            expect(not reply['frames'], 'all actual recovery messages must have been drained')
            for decision in reply['applied']:
                expect(seat not in applied and decision['head_hash'] == candidate['hash'], 'one durable application per operator')
                applied[seat] = decision
        expect(set(applied) == {0, 1, 2, 3} and final_message is not None, 'all four operators must finish actual PBFT after recovery')
        final_infos = self.infos()
        expect(all(info['height'] == 2 and info['head_hash'] == candidate['hash'] for info in final_infos), 'all recovered heads must agree')
        before_accounts = [item['process'].request('account', account=account) for item in self.operators]
        expect(all(value.get('verified') is True and value['balances'] == before_accounts[0]['balances'] for value in before_accounts), 'each operator must prove the same recovered account')
        balance = next(value for value in before_accounts[0]['balances'] if raw_hex(value['asset']) == raw_hex(Q_ASSET))
        expect(balance['available'] == 8_999_000 and balance['reserved'] == 0, 'one principal and one1000rawQ service fee, no duplicate debit')
        before_totals = [item['process'].request('totals') for item in self.operators]
        assets = [value['assets'] for value in before_totals]
        expect(all(value == assets[0] for value in assets), 'all independent ledgers must reconcile after recovery')
        totals = next(value for value in assets[0] if raw_hex(value['asset']) == raw_hex(Q_ASSET))
        expect(totals['deposits'] == 10_000_000 and totals['effects'] == 0 and totals['pending'] == 1_000_000 and
               totals['protocol_available'] == 1000 and totals['fees_held'] == 0 and totals['total_liabilities'] == 10_000_000,
               'deposit replay, pending principal and stable fee must be counted exactly once')
        records = [item['process'].request('withdrawal', withdrawal_id=action['id']) for item in self.operators]
        expect(all(value.get('verified') is True and value['record'] == records[0]['record'] and
                   not value['record']['paid'] and value['record']['amount'] == 1_000_000 and value['record']['fee'] == 1000
                   for value in records), 'one proven original unpaid obligation must survive the fault')
        stable_hashes = [retained_files(item['directory']) for item in self.operators]
        for seat, item in enumerate(self.operators):
            peer = leader_seat if seat != leader_seat else 0  # any configured peer may relay a real proof
            reply = item['process'].request('pbft_receive', from_seat=peer, message=final_message)
            expect(not reply['applied'] and not reply['frames'], 'duplicate final proof must not reapply or sign')
        expect(self.infos() == final_infos and
               [retained_files(item['directory']) for item in self.operators] == stable_hashes,
               'certificate replay must leave all durable bytes and heads unchanged')
        self.observe(self.mine_b3(), [])
        for item in self.operators:
            reply = item['process'].request('submit', frame=action['frame'])
            expect(reply['code'] in ('DUPLICATE', 'NONCE_USED', 'NONCE_TOO_LOW'), 'already executed client nonce must never be admitted anew')
            try:
                item['process'].request('candidate', actions=[action['frame']])
            except CustodyError as error:
                expect('nonce excluded' in str(error), 'the replayed action must fail the actual parent nonce window')
            else:
                raise CustodyError('actual engine accepted the already consumed withdrawal nonce')
            current = item['process'].request('account', account=account)
            expect(current['leaf'] == before_accounts[0]['leaf'], 'nonce/balances must not change on request replay')
            expect(item['process'].request('totals')['assets'] == assets[0], 'replayed request must not create another credit/fee/liability')
        next_action = self.client_action('next-valid-recovery-nonce', secret, 2, 'WITHDRAW',
                                        {'asset': raw_hex(Q_ASSET), 'amount': 500_000,
                                         'recipient': raw_hex(q.accounts[5], 20)}, ref_slot=2)
        next_candidate = self.operators[3]['process'].request('candidate', actions=[next_action['frame']])
        expect(next_candidate['hash'] != candidate['hash'], 'a fresh valid next nonce must still build after rejecting replay')
        self.event('recovery-pbft-complete', delivered=delivered, applied=applied)
        report = {'passed': True, 'scope': 'actual Linux SIGKILL after durable PREPARE; ' + getattr(self.args, 'agreement', 'pbft-fixed-view') + ' over trusted local pipes',
                  'assertions': ['real deposit and withdrawal; no synthetic credit', 'durable PREPARE survives SIGKILL without publication',
                                 'changed observations after restart refuse without changing pending/checkpoint bytes',
                                 'conflicting first proposal after restart refuses before signing', 'exact original resumes with byte-identical durable votes',
                                 'all four operators apply one value once', 'no duplicate deposit, principal, nonce or stable fee',
                                 'damaged copied journal refuses without repair', 'missing copied private identity refuses without key reset'],
                  'retained_original_keys': True, 'original_withdrawal': action['id'], 'account': account,
                  'frozen_observation_refusals': frozen_refusals,
                  'final_operator_info': self.infos(), 'totals': assets[0], 'fixed_view_only': getattr(self.args, 'agreement', 'pbft-fixed-view') == 'pbft-fixed-view', 'production_transport_qualified': False}
        atomic_json(self.evidence / 'operator-recovery-report.json', report)
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True)
    parser.add_argument('--evidence-dir', required=True)
    parser.add_argument('--agreement', choices=('pbft-fixed-view', 'pbft-view-change'), default='pbft-fixed-view')
    tool_paths.add(parser, 'worker', 'b3coind', 'b3coin_cli', 'tools', 'solc')
    args = parser.parse_args()
    tool_paths.resolve(parser, args, 'worker', 'b3coind', 'b3coin_cli', 'tools', 'solc')
    fixture = None
    try:
        fixture = RecoveryFixture(args)
        fixture.run()
        print(canonical({'passed': True, 'report': str(fixture.evidence / 'operator-recovery-report.json')}))
        return 0
    except Exception as error:
        if fixture is not None:
            atomic_json(fixture.evidence / 'operator-recovery-failure.json', {'passed': False, 'detail': str(error),
                        'scope': 'recovery incomplete; all original keys, journals and diagnostic copies retained'})
        print('operator recovery failed: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if fixture is not None:
            fixture.close()


if __name__ == '__main__':
    sys.exit(main())
