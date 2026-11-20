#!/usr/bin/env python3
"""Actual four-operator view-change, prepared carryover and control recovery gate."""
import argparse
from collections import deque
import os
from pathlib import Path
import signal
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from custody import Q_ASSET, CustodyError, atomic_json, expect
from local_process import raw_hex, wait_until
from operator_wrapper import private_json
from run_roundtrip import Roundtrip, canonical
from test_operator_recovery import digest_file


def wire(frame):
    text = frame['message'].removeprefix('0x')
    return bytes.fromhex(raw_hex(text, len(text) // 2))


def wire_kind(frame):
    encoded = wire(frame)
    expect(len(encoded) >= 2 and encoded[0] == 1, 'controller must emit the actual versioned message envelope')
    return encoded[1]


def wire_view(frame):
    encoded, kind = wire(frame), wire_kind(frame)
    offset = 11 if kind == 2 else 10 if kind in (1, 9) else 150 if kind in (7, 8) else None
    return None if offset is None else struct.unpack_from('<Q', encoded, offset)[0]


def compact(encoded, offset):
    expect(offset < len(encoded), 'fixture codec reached a missing CompactSize')
    first = encoded[offset]
    size = 1 if first < 253 else 3 if first == 253 else 5 if first == 254 else 9
    expect(offset + size <= len(encoded), 'fixture codec reached a truncated CompactSize')
    value = first if size == 1 else int.from_bytes(encoded[offset + 1:offset + size], 'little')
    expect(value <= 16_777_216 and (size == 1 or value >= (253 if size == 3 else 65536 if size == 5 else 4294967296)),
           'fixture codec requires a bounded canonical CompactSize')
    return value, offset + size


def certificate_parts(frame):
    encoded = wire(frame)
    expect(wire_kind(frame) == 6, 'negative certificate input must originate from a real certified proposal')
    length, start = compact(encoded, 2)
    end = start + length
    expect(end + 16 + 366 < len(encoded), 'actual final frame must contain its certificate and proposal body')
    certificate = encoded[start:end]
    bitmap_length, bitmap_start = compact(certificate, 32)
    expect(bitmap_length == 1 and len(certificate) == bitmap_start + bitmap_length + 96 + 8,
           'this fixture requires the explicit /2 view-bound four-seat certificate format')
    return encoded, start, end


def remove_last_report(frame):
    """Negative only: remove one real report; keep all signatures unchanged."""
    encoded = wire(frame)
    expect(wire_kind(frame) == 8, 'missing-report mutation needs an actual controller NEW_VIEW')
    count_offset = 2 + 157 + 32
    count, position = compact(encoded, count_offset)
    expect(count in (3, 4), 'actual NEW_VIEW must contain a real quorum')
    reports = []
    for _ in range(count):
        begin = position
        length, payload = compact(encoded, position)
        position = payload + length
        expect(position <= len(encoded), 'actual nested report is truncated')
        reports.append(encoded[begin:position])
    return encoded[:count_offset] + bytes([2]) + b''.join(reports[:2]) + encoded[position:]


def retained_state(directory):
    """Only hashes persistent bytes; private identity material is never returned."""
    root = Path(directory)
    files = [path for path in (root / 'state').rglob('*') if path.is_file()]
    files += [root / name for name in ('private-identity.json', 'public-identity.json') if (root / name).is_file()]
    return {str(path.relative_to(root)): digest_file(path) for path in sorted(files)}


class Exchange:
    """Deliver genuine worker frames only; no coordinator vote aggregation."""
    def __init__(self, fixture, active, expected_hash):
        self.fixture, self.active, self.expected_hash = fixture, set(active), expected_hash
        self.queue, self.held, self.frames, self.delivered, self.certified = deque(), [], [], [], []
        self.applied, self.steps = {}, 0

    def collect(self, seat, reply):
        for decision in reply['applied']:
            expect(seat not in self.applied and decision['head_hash'] == self.expected_hash,
                   'each actual operator may apply the selected economic value only once')
            self.applied[seat] = decision
        for frame in reply['frames']:
            expect(frame['from_seat'] == seat and 0 <= frame['to_seat'] < 4 and frame['to_seat'] != seat,
                   'frame must belong to its real private operator pipe')
            kind = wire_kind(frame)
            if kind == 2:
                expect(frame.get('durable_vote') is True, 'actual vote must have durable signer publication evidence')
            if kind in (7, 8):
                expect(frame.get('durable_control') is True, 'actual control signature must have durable control publication evidence')
            if kind == 6:
                self.certified.append(frame)
            self.frames.append(frame)
            self.queue.append(frame)

    def pump(self, allowed=None, hold_commits=False, minimum_view=None, before_delivery=None):
        allowed = self.active if allowed is None else set(allowed)
        while self.queue:
            self.steps += 1
            expect(self.steps <= 8192, 'actual message exchange exceeded its bounded fixture budget')
            frame = self.queue.popleft()
            kind = wire_kind(frame)
            if (frame['to_seat'] not in allowed or frame['from_seat'] not in allowed or
                    (hold_commits and kind in (2, 6) and (kind == 6 or wire(frame)[2] == 2)) or
                    (minimum_view is not None and kind in (1, 2) and wire_view(frame) < minimum_view)):
                self.held.append(frame)
                continue
            if before_delivery is not None:
                before_delivery(frame)
            seat = frame['to_seat']
            reply = self.fixture.operators[seat]['process'].request('pbft_receive', from_seat=frame['from_seat'], message=frame['message'])
            self.delivered.append(frame)
            self.collect(seat, reply)

    def release(self):
        self.queue.extend(self.held)
        self.held.clear()

    def public_evidence(self):
        return {'active': sorted(self.active), 'frames': self.frames, 'delivered': self.delivered,
                'held': self.held, 'applied': self.applied}


class LeaderFailover(Roundtrip):
    def kill_operator(self, item):
        expect(sys.platform == 'linux', 'real SIGKILL control recovery fixture requires Linux /proc')
        owned = item['process']
        parent = owned.process.pid
        children = Path(f'/proc/{parent}/task/{parent}/children').read_text().split()
        matching = [int(child) for child in children if Path(f'/proc/{child}/exe').resolve(strict=True) == self.worker_path]
        expect(len(matching) == 1 and len(children) == 1 and os.getpgid(parent) == parent,
               'fixture must identify exactly its owned wrapper group and worker child')
        worker = matching[0]
        os.kill(worker, signal.SIGKILL)
        wait_until(lambda: not Path(f'/proc/{worker}').exists() or Path(f'/proc/{worker}/stat').read_text().split()[2] == 'Z',
                   10, 'owned worker did not stop after actual SIGKILL')
        os.killpg(parent, signal.SIGKILL)
        owned.process.wait(timeout=10)
        owned.close()
        self.event('failover-operator-sigkill', seat=item['open_config']['seat'], worker_pid=worker, wrapper_pid=parent)

    def reopen(self, item):
        before = retained_state(item['directory'])
        self.launch_operator(item)
        expect(item['process'].request('bootstrap') == item['public'], 'recovery must retain the exact original independent identities')
        result = item['process'].request('open', config={**item['open_config'], 'create': False})
        expect(retained_state(item['directory']) == before, 'cold open must retain original durable journal bytes')
        return result

    def reject(self, seat, frame, label, expected_text=None):
        item = self.operators[seat]
        before, files = item['process'].request('info'), retained_state(item['directory'])
        try:
            item['process'].request('pbft_receive', from_seat=frame['from_seat'], message=frame['message'])
        except CustodyError as error:
            detail = str(error)
            if expected_text is not None:
                expect(expected_text in detail, label + ' must reach its intended verification boundary')
        else:
            raise CustodyError(label + ' was accepted')
        expect(item['process'].request('info') == before and retained_state(item['directory']) == files,
               label + ' must not sign, publish, apply, append or reset durable state')
        self.negatives.append({'label': label, 'seat': seat, 'detail': detail})

    def changed(self, frame, encoded, destination=None):
        return {**frame, 'message': encoded.hex(), 'to_seat': frame['to_seat'] if destination is None else destination}

    def assert_obligation(self, action, amount, available, pending, fees, absent=None):
        proofs = [item['process'].request('withdrawal', withdrawal_id=action['id']) for item in self.operators]
        expect(all(value.get('found') and value.get('verified') is True and not value['record']['paid'] and
                   value['record']['amount'] == amount and value['record']['fee'] == 1000 for value in proofs),
               'all real certificate/proof paths must preserve exactly the original unpaid principal and fee')
        expect(self.balances(action['account'])[raw_hex(Q_ASSET)] == (available, 0), 'withdrawal/nonce must debit exactly once')
        totals = [item['process'].request('totals') for item in self.operators]
        expect(all(value['conservation'] is True and value['assets'] == totals[0]['assets'] for value in totals),
               'all independently applied ledgers must conserve the same actual deposited assets')
        q = next(value for value in totals[0]['assets'] if raw_hex(value['asset']) == raw_hex(Q_ASSET))
        expect(q['deposits'] == 10_000_000 and q['pending'] == pending and q['protocol_available'] == fees and
               q['total_liabilities'] == 10_000_000 and q['effects'] == 0 and q['fees_held'] == 0,
               'pending obligations, stable fees and complete liabilities must reconcile exactly once')
        if absent is not None:
            expect(all(not item['process'].request('withdrawal', withdrawal_id=absent['id'])['found'] for item in self.operators),
                   'the fresh competing nonce instruction must never replace the prepared obligation')
        for item in self.operators:
            reply = item['process'].request('submit', frame=action['frame'])
            expect(reply['code'] in ('DUPLICATE', 'NONCE_USED', 'NONCE_TOO_LOW'), 'same signed nonce must remain consumed after failover')
        return {'proof': proofs[0], 'totals': totals[0], 'available_raw': available}

    def absent_leader(self, action):
        source = self.observe(self.mine_b3(), [])
        old_leader, new_leader, active = 2, 3, {0, 1, 3}
        for item in self.operators:
            expect(item['process'].request('submit', frame=action['frame'])['code'] == 'ADMITTED', 'unchanged real action must be admitted')
        self.kill_operator(self.operators[old_leader])
        candidate = self.operators[0]['process'].request('candidate', actions=[action['frame']])
        exchange = Exchange(self, active, candidate['hash'])
        exchange.collect(0, self.operators[0]['process'].request('pbft_start', header=candidate['header'], body=candidate['body']))
        expect(all(wire_kind(frame) == 9 for frame in exchange.frames), 'responsive nonleader must only offer before a valid new view')
        exchange.pump()
        expect(not exchange.applied, 'silenced scheduled leader must prevent the old view from deciding')
        for now in (0, 500):
            for seat in sorted(active):
                exchange.collect(seat, self.operators[seat]['process'].request('pbft_timeout', now_ms=now))
        reports = [frame for frame in exchange.frames if wire_kind(frame) == 7]
        expect({frame['from_seat'] for frame in reports} == active and all(wire_view(frame) == 1 for frame in reports),
               'each responsive operator must durably issue an authenticated same-target report')
        exchange.pump(allowed={0, new_leader})
        expect(not exchange.applied and not any(wire_kind(frame) == 8 for frame in exchange.frames),
               'two authenticated participants/reports must not form a NEW_VIEW or decision')
        expect(all(self.operators[seat]['process'].request('info')['changing_view'] for seat in active),
               'missing the third report must leave all responsive operators changing')
        checked = set()

        def negatives(frame):
            kind, recipient = wire_kind(frame), frame['to_seat']
            if kind == 7 and 'report' not in checked:
                encoded = bytearray(wire(frame)); encoded[-1] ^= 1
                self.reject(recipient, self.changed(frame, encoded), 'malformed authenticated control report')
                checked.add('report')
            if kind == 8 and 'newview' not in checked:
                self.reject(recipient, self.changed(frame, remove_last_report(frame)), 'NEW_VIEW missing quorum report', 'CONTROL_REPORT_QUORUM')
                self.reject(recipient, self.changed(frame, wire(frame)[:-1]), 'truncated NEW_VIEW')
                checked.add('newview')

        exchange.release()
        exchange.pump(before_delivery=negatives)
        expect(set(exchange.applied) == active and checked == {'report', 'newview'}, 'three unchanged participants must complete the actual authenticated new view')
        expect(all(self.operators[seat]['process'].request('info')['height'] == 2 for seat in active), 'responsive operators must durably apply the real withdrawal')
        self.reopen(self.operators[old_leader])
        final = next(frame for frame in exchange.certified if frame['to_seat'] == old_leader)
        encoded, begin, end = certificate_parts(final)
        certificate_view = struct.unpack_from('<Q', encoded, end - 8)[0]
        expect(certificate_view == 1 and struct.unpack_from('<Q', encoded, end + 8)[0] == 1, 'actual failover certificate and proposal must both bind view1')
        mixed = bytearray(encoded); struct.pack_into('<Q', mixed, end - 8, 0)
        self.reject(old_leader, self.changed(final, mixed), 'mixed proposal/certificate views')
        struct.pack_into('<Q', mixed, end + 8, 0)
        self.reject(old_leader, self.changed(final, mixed), 'relabelled quorum certificate with unchanged real aggregate')
        learned = self.operators[old_leader]['process'].request('pbft_receive', from_seat=final['from_seat'], message=final['message'])
        expect(len(learned['applied']) == 1 and learned['applied'][0]['head_hash'] == candidate['hash'], 'old leader must catch up from the actual transferable certificate')
        infos = self.infos()
        expect(len({info['head_hash'] for info in infos}) == 1 and all(info['quorum'] == 3 and info['seats'] == 4 for info in infos),
               'returning old leader must learn the exact same value at unchanged quorum')
        reconciliation = self.assert_obligation(action, 1_000_000, 8_999_000, 1_000_000, 1000)
        self.event('absent-leader-complete', source=source, operator_info=infos, **exchange.public_evidence())
        return {'old_leader': old_leader, 'new_leader': new_leader, 'certificate_view': certificate_view,
                'source': source, 'reconciliation': reconciliation, **exchange.public_evidence()}

    def prepared_carryover(self, action, alternate):
        source = self.observe(self.mine_b3(), [])
        before = self.infos()
        old_leader, new_leader, prepared_seats, active = 3, 0, {1, 2, 3}, {0, 1, 2}
        for item in self.operators:
            expect(item['process'].request('submit', frame=action['frame'])['code'] == 'ADMITTED', 'prepared original must be a genuine admitted signed action')
        fresh = self.operators[new_leader]['process'].request('candidate', actions=[alternate['frame']])
        competing_offer = self.operators[new_leader]['process'].request('pbft_start', header=fresh['header'], body=fresh['body'])
        expect(not competing_offer['applied'] and all(wire_kind(frame) == 9 for frame in competing_offer['frames']),
               'the next leader must have a different executable fresh OFFER before learning preparation A')
        candidate = self.operators[old_leader]['process'].request('candidate', actions=[action['frame']])
        expect(fresh['hash'] != candidate['hash'], 'prepared A and fresh B must be distinct valid economic candidates')
        prepared = Exchange(self, prepared_seats, candidate['hash'])
        prepared.collect(old_leader, self.operators[old_leader]['process'].request('pbft_start', header=candidate['header'], body=candidate['body']))
        prepared.pump(allowed=prepared_seats, hold_commits=True)
        old_commits = [frame for frame in prepared.frames if wire_kind(frame) == 2 and wire(frame)[2] == 2]
        expect({frame['from_seat'] for frame in old_commits} == prepared_seats and not prepared.applied,
               'three actual durable COMMIT publications prove qPREPARE while withheld COMMIT delivery prevents qCOMMIT')
        paused = self.infos()
        expect(all(paused[seat]['height'] == 2 and paused[seat]['commit_seq'] > before[seat]['commit_seq'] and
                   paused[seat]['signer_seq'] > before[seat]['signer_seq'] for seat in prepared_seats),
               'each prepared operator must persist its verified PreparedQC and own COMMIT without applying')
        expect(paused[new_leader]['height'] == 2 and paused[new_leader]['signer_seq'] == before[new_leader]['signer_seq'],
               'fresh-B operator must not have signed or accepted A before view change')
        self.kill_operator(self.operators[old_leader])
        exchange = Exchange(self, active, candidate['hash'])
        exchange.collect(new_leader, competing_offer)
        for now in (1000, 1500):
            for seat in sorted(active):
                exchange.collect(seat, self.operators[seat]['process'].request('pbft_timeout', now_ms=now))
        changing = {seat: self.operators[seat]['process'].request('info') for seat in active}
        expect(all(info['current_view'] == 1 and info['changing_view'] is True and info['height'] == 2 for info in changing.values()),
               'responsive operators must durably enter CHANGING before the controller restart')
        reports = [frame for frame in exchange.frames if wire_kind(frame) == 7]
        expect({frame['from_seat'] for frame in reports} == active, 'view change requires three actual independently signed reports')
        victim_seat = 1
        victim = self.operators[victim_seat]
        original_report = next(frame for frame in reports if frame['from_seat'] == victim_seat)
        expect(wire(original_report)[2 + 157 + 8] == 1, 'restarted operator must carry its real durable preparation A')
        durable_before = retained_state(victim['directory'])
        self.kill_operator(victim)
        expect(retained_state(victim['directory']) == durable_before, 'SIGKILL must retain all original signer/commit/control bytes')
        restored = self.reopen(victim)
        expect(restored['current_view'] == 1 and restored['changing_view'] is True and
               restored['control_seq'] == changing[victim_seat]['control_seq'] and restored['observation_frozen'] is True,
               'reopen must restore the same CHANGING view, immutable report prefix and frozen source batch')
        replay = victim['process'].request('pbft_timeout', now_ms=1500)
        replay_reports = [frame for frame in replay['frames'] if wire_kind(frame) == 7]
        expect(replay_reports and all(frame['message'] == original_report['message'] for frame in replay_reports),
               'control recovery must retransmit its exact original signed report')
        expect(not replay['applied'] and not any(wire_kind(frame) == 2 and wire_view(frame) == 0 for frame in replay['frames']) and
               replay['info']['current_view'] == 1 and replay['info']['changing_view'] is True,
               'restart alone must neither invent a higher view nor emit abandoned-view votes')
        exchange.collect(victim_seat, replay)
        commit = next(frame for frame in old_commits if frame['from_seat'] == 1 and frame['to_seat'] == 2)
        relabelled = bytearray(wire(commit)); struct.pack_into('<Q', relabelled, 11, 1)
        self.reject(2, self.changed(commit, relabelled), 'old COMMIT relabelled as new view with original signature', 'PBFT_VOTE_SIGNATURE')
        report_to_two = next(frame for frame in reports if frame['from_seat'] == 1 and frame['to_seat'] == 2)
        bad_preparation = bytearray(wire(report_to_two))
        certificate_length, certificate_start = compact(bad_preparation, 2 + 157 + 8 + 1 + 8 + 366)
        struct.pack_into('<Q', bad_preparation, certificate_start + certificate_length - 8, 1)
        self.reject(2, self.changed(report_to_two, bad_preparation), 'prepared proof carrying a different certificate view', 'CONTROL_PREPARED_VIEW')
        exchange.pump(minimum_view=1)
        expect(set(exchange.applied) == active, 'responsive quorum must complete the actual view1 preparation and COMMIT path')
        new_views = [frame for frame in exchange.frames if wire_kind(frame) == 8]
        expect(new_views and all(wire(frame)[2 + 157:2 + 157 + 32].hex() == candidate['hash'] for frame in new_views),
               'authenticated NEW_VIEW must select highest prepared A over the new leader\'s fresh B')
        expect(not any(wire_kind(frame) == 2 and wire_view(frame) == 0 for frame in exchange.delivered),
               'abandoned-view COMMIT shares must not contribute to the new-view quorum')
        self.reopen(self.operators[old_leader])
        final = next(frame for frame in exchange.certified if frame['to_seat'] == old_leader)
        learned = self.operators[old_leader]['process'].request('pbft_receive', from_seat=final['from_seat'], message=final['message'])
        expect(len(learned['applied']) == 1 and learned['applied'][0]['head_hash'] == candidate['hash'],
               'returned old leader must learn the real new-view certificate for original A')
        infos = self.infos()
        expect(all(info['height'] == 3 and info['head_hash'] == candidate['hash'] and info['quorum'] == 3 for info in infos),
               'all four actual journal owners must converge on A without reducing quorum')
        reconciliation = self.assert_obligation(action, 2_000_000, 6_998_000, 3_000_000, 2000, absent=alternate)
        self.event('prepared-carryover-complete', source=source, prepared=prepared.public_evidence(),
                   controller_restart={'seat': victim_seat, 'before': changing[victim_seat], 'after': restored},
                   operator_info=infos, **exchange.public_evidence())
        return {'old_leader': old_leader, 'new_leader': new_leader, 'prepared_hash': candidate['hash'],
                'rejected_fresh_hash': fresh['hash'], 'prepared': prepared.public_evidence(),
                'restarted_changing_seat': victim_seat, 'original_report': original_report,
                'restored_control_info': restored, 'source': source, 'reconciliation': reconciliation,
                **exchange.public_evidence()}

    def run(self):
        expect(self.agreement == 'pbft-view-change', 'failover gate requires explicit new versioned view-change mode')
        self.negatives = []
        self.build()
        self.start_b3()
        self.start_evm()
        self.bootstrap_operators()
        self.deploy_routes()
        self.open_operators()
        seed = os.urandom(32).hex()
        private_json(self.evidence / 'private-failover-client.json', {'seed': seed})
        q = self.routes['Q']['driver']
        first = self.client_action('absent-leader-withdrawal', seed, 1, 'WITHDRAW',
                                   {'asset': raw_hex(Q_ASSET), 'amount': 1_000_000, 'recipient': raw_hex(q.accounts[5], 20)}, ref_slot=1)
        second = self.client_action('prepared-withdrawal-A', seed, 2, 'WITHDRAW',
                                    {'asset': raw_hex(Q_ASSET), 'amount': 2_000_000, 'recipient': raw_hex(q.accounts[5], 20)}, ref_slot=2)
        alternate = self.client_action('fresh-competing-withdrawal-B', seed, 2, 'WITHDRAW',
                                       {'asset': raw_hex(Q_ASSET), 'amount': 3_000_000, 'recipient': raw_hex(q.accounts[5], 20)}, ref_slot=2)
        q.mint(0, 10_000_000)
        q.approve(0, 10_000_000)
        receipt = q.deposit(0, '0x' + raw_hex(first['account']), 10_000_000)
        deposit = q.expected_deposit(receipt, '0x' + raw_hex(first['account']), 10_000_000, q.accounts[0])
        self.observe(self.mine_b3(), [{'kind': 'deposit', 'route': 'Q', 'transaction_hash': deposit['transaction_hash'],
                                     'account': deposit['account'], 'amount_raw': deposit['amount_raw'], 'sender': deposit['sender']}])
        self.commit([], label='actual canonical Q deposit before independent leader failover gate')
        first_result = self.absent_leader(first)
        carryover = self.prepared_carryover(second, alternate)
        infos = self.infos()
        before_replay = [retained_state(item['directory']) for item in self.operators]
        final = next(frame for frame in carryover['frames'] if wire_kind(frame) == 6)
        for seat, item in enumerate(self.operators):
            sender = final['from_seat'] if final['from_seat'] != seat else (seat + 1) % 4
            duplicate = item['process'].request('pbft_receive', from_seat=sender, message=final['message'])
            expect(not duplicate['applied'] and not duplicate['frames'], 'duplicate real certificate must neither reapply nor sign')
        expect(self.infos() == infos and [retained_state(item['directory']) for item in self.operators] == before_replay,
               'certificate replay must preserve every original persistent prefix')
        actual, locked = q.call_uint(q.token, 'balanceOf(address)', q.vault), q.call_uint(q.vault, 'locked()')
        expect(actual == locked == 10_000_000, 'actual canonical synthetic custody must back complete user/protocol/pending liabilities')
        report = {'passed': True, 'agreement': self.agreement, 'profile_name': self.profile_name,
                  'scope': 'actual four-process BLS/file-journal view-change and recovery; trusted local pipes and virtual timers; no production transport/finality/latency qualification',
                  'seats': 4, 'quorum': 3, 'no_coordinator_certificate_aggregation': True,
                  'chain_id': 31337, 'actual_deposit': deposit, 'actual_q_reserves_raw': actual,
                  'missing_leader': first_result, 'highest_prepared_carryover': carryover,
                  'negative_checks': self.negatives, 'final_operator_info': infos,
                  'economic_result': {'deposit_raw': 10_000_000, 'pending_raw': 3_000_000, 'available_raw': 6_998_000,
                                      'q_fee_raw': 2000, 'withdrawal_actions': [first['id'], second['id']], 'unselected_action': alternate['id']},
                  'custody_payout_qualification': 'this gate preserves real backed pending obligations; full payout is exercised by run_roundtrip.py'}
        atomic_json(self.evidence / 'leader-failover-report.json', report)
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
    parser.add_argument('--agreement', choices=('pbft-view-change',), default='pbft-view-change')
    args = parser.parse_args()
    fixture = None
    try:
        fixture = LeaderFailover(args)
        fixture.run()
        print(canonical({'passed': True, 'report': str(fixture.evidence / 'leader-failover-report.json')}))
        return 0
    except Exception as error:
        if fixture is not None:
            atomic_json(fixture.evidence / 'leader-failover-failure.json', {'passed': False, 'detail': str(error)})
        print('leader failover test failed: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if fixture is not None:
            fixture.close()


if __name__ == '__main__':
    sys.exit(main())
