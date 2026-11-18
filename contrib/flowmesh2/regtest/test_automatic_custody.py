#!/usr/bin/env python3
"""Bounded custody protocol tests; actual crypto/receipts run in network regtest."""
import copy
import hashlib
import json
from pathlib import Path
import struct
import tempfile
import time
import unittest
from unittest.mock import patch

from custody import CustodyError, Q_ASSET
import custody_runtime as runtime


ID = '42' * 31 + '00'
ADDRESSES = ['0x' + str(number).zfill(40) for number in range(1, 5)]
GAS_ADDRESS = '0x' + 'ab' * 20
CHECKPOINT = {'height': 10, 'hash': '0x' + '33' * 32, 'minimum_confirmations': 2}


def digest(value):
    return '0x' + hashlib.sha256(runtime.canonical(value).encode()).hexdigest()


def signature(seat, message):
    # Deliberately fake only the cryptographic primitive. Admission, exact
    # authenticated-seat binding, low-S framing, queues and transitions run the
    # real protocol code. Real secp signatures are covered by the owned-Anvil test.
    scalar = int(message[-16:], 16) + 1
    return '0x' + (seat + 1).to_bytes(32, 'big').hex() + scalar.to_bytes(32, 'big').hex() + '1b'


class FakeCast:
    def run(self, *args):
        if args[:3] != ('wallet', 'verify', '--address') or args[4] != '--no-hash':
            raise AssertionError('unexpected cryptographic API')
        seat = ADDRESSES.index(args[3])
        if args[6] != signature(seat, args[5]):
            raise CustodyError('local cast command failed: Validation failed.')
        return 'Validation succeeded. Address ' + args[3] + ' signed this message.'


class FakeRpc:
    def __init__(self):
        self.timestamp = 1010

    def call(self, method, params=()):
        if method != 'eth_getBlockByNumber':
            raise AssertionError('unexpected source RPC')
        if params[0] == 'latest':
            return {'timestamp': hex(self.timestamp), 'hash': '0x' + '77' * 32}
        if params[0] != '0xa':
            raise AssertionError('source authority must retain its original checkpoint')
        return {'timestamp': hex(1000), 'hash': CHECKPOINT['hash']}


class FakeRoute:
    custody_profile = runtime.PROFILE
    operator_addresses = ADDRESSES
    config = {'custody_profile': runtime.PROFILE, 'immutable_public_policy': 'fake-for-offline-boundary-test'}

    def __init__(self):
        self.rpc, self.cast = FakeRpc(), FakeCast()
        self.source_canonical, self.identity_valid = True, True
        self.external = {'digest': '0x' + '00' * 32, 'recipient': '0x' + '00' * 20,
                         'amount_raw': '0', 'release_at': 0, 'paid': False}

    def verify_contract_identity(self):
        if not self.identity_valid:
            raise CustodyError('custody deployment policy changed')

    def withdrawal_digest(self, value):
        self.verify_contract_identity()
        return digest(value)

    def withdrawal_status(self, identity):
        if identity != '0x' + ID:
            raise AssertionError('unexpected claim')
        return copy.deepcopy(self.external)


class FakeWorker:
    def __init__(self):
        self.record = {'id': ID, 'account': '11' * 32, 'asset': Q_ASSET[2:], 'amount': 1000000,
                       'fee': 10000, 'recipient': '99' * 20, 'slot': 3, 'paid': False,
                       'payout_event': {'txid': '00' * 32, 'vout': 0}}
        encoded = bytearray(366)
        struct.pack_into('<Q', encoded, 73, 3)
        struct.pack_into('<I', encoded, 145, 200)
        encoded[149:181] = bytes.fromhex('55' * 32)
        encoded[325:357] = bytes.fromhex('66' * 32)
        self.original_header = encoded.hex()
        self.authority_calls, self.enumeration_calls = 0, 0

    def request(self, command, **fields):
        if command == 'info':
            return {'seat': 0, 'height': 30, 'head_hash': '88' * 32, 'header': 'newer-current-head'}
        if command == 'withdrawal_pending':
            self.enumeration_calls += 1
            return {'withdrawal_ids': [] if self.record['paid'] else [ID], 'next_cursor': None, 'more': False}
        if fields != {'withdrawal_id': ID}:
            raise AssertionError('unexpected native withdrawal command fields')
        if command == 'withdrawal':
            return {'found': True, 'verified': True, 'record': copy.deepcopy(self.record)}
        if command == 'withdrawal_authority':
            self.authority_calls += 1
            if self.record['paid']:
                raise CustodyError('paid withdrawal cannot receive a fresh custody authorization')
            return {'found': True, 'verified': True, 'current_unpaid': True,
                    'record': copy.deepcopy(self.record), 'current_record': copy.deepcopy(self.record),
                    'header': self.original_header, 'certificate': 'original-native-verified-certificate',
                    'source_checkpoint': CHECKPOINT}
        raise AssertionError('unexpected native command')


class FakeB3:
    def verify_anchor(self, anchor):
        if anchor != {'height': 200, 'hash': '55' * 32}:
            raise CustodyError('original B3 anchor changed')


class FakeRelayer:
    def __init__(self, operator):
        self.operator, self.commands, self.needs_resume = operator, [], None
        self.ambiguous_once, self.closed = False, False

    def close(self):
        self.closed = True

    def request(self, operation, **fields):
        self.commands.append((operation, copy.deepcopy(fields)))
        if operation == 'pending':
            return copy.deepcopy(self.needs_resume) if self.needs_resume else {'required': False}
        if operation == 'resume':
            if fields != {'nonce': 7, 'require_success': False}:
                raise AssertionError('recovery must reuse its exact durable nonce')
            self.needs_resume = None
            return {'status': '0x1'}
        if operation == 'request':
            value, signatures = fields['withdrawal'], fields['signatures']
            expected = [signature(seat, digest(value)) for seat in range(4) if signature(seat, digest(value)) in signatures]
            if len(expected) < 3 or expected != signatures:
                raise AssertionError('gas request requires sorted distinct independently verified seats')
            if self.ambiguous_once:
                self.ambiguous_once = False
                self.needs_resume = {'required': True, 'nonce': 7,
                                     'action': {'operation': 'request', 'withdrawal_id': '0x' + ID}}
                raise CustodyError('signed send remains ambiguous; resume the exact saved nonce/raw transaction')
            self.operator.route.external.update(digest=digest(value), recipient=value['recipient'],
                amount_raw=value['amount'], release_at=self.operator.route.rpc.timestamp + 30)
        elif operation == 'finalize':
            if fields != {'withdrawal_id': '0x' + ID, 'require_success': False}:
                raise AssertionError('finalize cannot change the original claim')
            if self.operator.route.rpc.timestamp < self.operator.route.external['release_at']:
                raise AssertionError('challenge cannot be bypassed')
            self.operator.route.external['paid'] = True
        else:
            raise AssertionError('unsupported private gas operation')
        return {'status': '0x1'}


class FakeOperator:
    def __init__(self, directory):
        self.directory, self.evm_address = Path(directory), ADDRESSES[0]
        self.worker, self.route, self.b3 = FakeWorker(), FakeRoute(), FakeB3()
        self.routes = {'Q': self.route}
        manifest = {'format': 'fm2-regtest-deployment/1', 'chain_id': 31337, 'mainnet_enabled': False,
                    'profile_name': 'fm2-regtest-stable-q/3', 'custody_profile': runtime.PROFILE,
                    'agreement': 'pbft-view-change', 'transport': {'format': 'fm2-seat-tcp/1'},
                    'operators': [{'evm_address': item} for item in ADDRESSES]}
        self.manifest_path = self.directory / 'deployment.json'
        self.manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
        self.config = {'deployment_file': str(self.manifest_path), 'worker': 'unused-private-native-worker',
                       'automatic_batches': True}
        self.signed, self.retained_valid = [], True
        self.accept_at_sign, self.sign_refusal = False, None

    def _verify_retained(self):
        if not self.retained_valid:
            raise CustodyError('retained source range checkpoint removed or replaced; refusing new signing')

    def _sign_withdrawal(self, value, renewal=False):
        self._verify_retained()
        if self.sign_refusal:
            raise CustodyError(self.sign_refusal)
        if self.accept_at_sign:
            self.route.external.update(digest=digest(value), recipient=value['recipient'],
                amount_raw=value['amount'], release_at=self.route.rpc.timestamp + 30)
        if int(self.route.external['digest'], 16) != 0:
            raise CustodyError('external claim is already accepted/paid; finalize its existing id instead of reauthorizing')
        path = self.directory / 'withdrawal-signatures' / (ID + '.json')
        if path.exists():
            prior = json.loads(path.read_text())['withdrawal']
            if prior != value and (not renewal or prior['validUntil'] >= self.route.rpc.timestamp or value['validUntil'] <= prior['validUntil']):
                raise CustodyError('durable authorization cannot change before monotone expired renewal')
        path.parent.mkdir(exist_ok=True)
        path.write_text(json.dumps({'withdrawal': value}), encoding='utf-8')
        self.signed.append(copy.deepcopy(value))
        return {'operator': self.evm_address, 'signature': signature(0, digest(value)), 'digest': digest(value)}


class AutomaticCustodyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.operator, self.sent = FakeOperator(self.temp.name), []
        self.start_patch = patch.object(runtime.AutomaticCustody, '_start_relayer', self.start_fake)
        self.source_patch = patch.object(runtime, 'verify_source_point', self.verify_source)
        self.start_patch.start()
        self.source_patch.start()
        self.custody = runtime.AutomaticCustody(self.operator, lambda seat, value: self.sent.append((seat, copy.deepcopy(value))))
        self.now = 10.0

    @staticmethod
    def start_fake(instance):
        instance.relayer, instance.gas_address = FakeRelayer(instance.operator), GAS_ADDRESS

    @staticmethod
    def verify_source(route, point):
        if point != CHECKPOINT or not route.source_canonical:
            raise CustodyError('source checkpoint is replaced, unavailable or not sufficiently confirmed')

    def tearDown(self):
        self.custody.close()
        self.source_patch.stop()
        self.start_patch.stop()
        self.temp.cleanup()

    def pump(self, rounds=20):
        for _ in range(rounds):
            self.now += .1
            self.custody.step(self.now, self.operator.worker.request('info'))
            time.sleep(.001)

    def peer_vote(self, seat, value=None, message_digest=None, signed=None):
        value = copy.deepcopy(value or self.custody.records[ID]['withdrawal'])
        message_digest = message_digest or digest(value)
        self.custody.on_application(seat, {'format': 'fm2-network-app/1', 'kind': 'custody_authorization',
            'withdrawal': value, 'digest': message_digest, 'signature': signed or signature(seat, message_digest)})

    def relay_commands(self):
        return [operation for operation, fields in self.custody.relayer.commands if operation in ('request', 'finalize', 'resume')]

    def test_mainnet_and_wrong_profile_are_refused(self):
        manifest = json.loads(self.operator.manifest_path.read_text())
        for field, bad in (('chain_id', 1), ('mainnet_enabled', True), ('profile_name', 'fm2-regtest-stable-q/2')):
            altered = {**manifest, field: bad}
            self.operator.manifest_path.write_text(json.dumps(altered))
            with self.assertRaises(CustodyError):
                runtime.AutomaticCustody(self.operator, lambda *args: None)

    def test_no_quorum_never_relays_or_debits(self):
        self.pump(80)
        self.assertEqual(self.relay_commands(), [])
        self.assertEqual(len(self.custody.records[ID]['signatures']), 1)
        self.assertFalse(self.operator.route.external['paid'])
        self.assertGreater(len(self.sent), 0)

    def test_three_distinct_verified_seats_request_once(self):
        self.pump()
        self.peer_vote(1)
        self.peer_vote(2)
        self.pump(50)
        self.assertEqual(self.relay_commands(), ['request'])
        self.assertEqual(self.custody.stats['peer_authorizations_verified'], 2)
        self.assertFalse(self.operator.route.external['paid'])

    def test_challenge_then_external_paid_then_native_paid_gc(self):
        self.pump()
        self.peer_vote(1)
        self.peer_vote(2)
        self.pump(30)
        self.assertEqual(self.relay_commands(), ['request'])
        self.operator.route.rpc.timestamp += 29
        self.pump(30)
        self.assertEqual(self.relay_commands(), ['request'])
        self.operator.route.rpc.timestamp += 1
        self.pump(30)
        self.assertEqual(self.relay_commands(), ['request', 'finalize'])
        self.assertIn(ID, self.custody.records)
        self.assertFalse(self.operator.worker.record['paid'])
        self.operator.worker.record['paid'] = True
        self.pump()
        self.assertNotIn(ID, self.custody.records)
        self.assertEqual(self.custody.stats['native_paid_observed'], 1)

    def test_original_authority_survives_newer_head(self):
        self.pump()
        value = self.custody.records[ID]['withdrawal']
        self.assertEqual(value['slot'], 3)
        self.assertEqual(value['stateRoot'], '0x' + '66' * 32)
        self.assertEqual(self.operator.worker.request('info')['height'], 30)
        self.assertEqual(self.operator.worker.authority_calls, 1)

    def test_catching_up_node_ignores_votes_after_external_acceptance(self):
        self.custody._discover(self.now)
        entry = self.custody.records[ID]
        current = self.custody._current(ID)
        authority = self.custody._authority(ID, entry, current)
        value = self.custody._value(ID, authority, self.operator.route.rpc.timestamp)
        self.operator.route.external.update(digest=digest(value), recipient=value['recipient'],
            amount_raw=value['amount'], release_at=self.operator.route.rpc.timestamp + 30)
        self.peer_vote(1, value=value)
        self.pump()
        self.assertEqual(self.operator.signed, [])
        self.assertEqual(entry['signatures'], {})

    def test_late_votes_after_payout_do_not_reauthorize_or_fence(self):
        self.pump()
        value = copy.deepcopy(self.custody.records[ID]['withdrawal'])
        self.operator.route.external.update(digest=digest(value), recipient=value['recipient'],
            amount_raw=value['amount'], release_at=self.operator.route.rpc.timestamp + 30, paid=True)
        self.pump()
        self.assertEqual(self.custody.records[ID]['signatures'], {})
        self.peer_vote(1, value=value)
        self.peer_vote(2, value=value)
        self.pump()
        self.assertEqual(len(self.operator.signed), 1)
        self.assertEqual(self.custody.records[ID]['signatures'], {})

    def test_precise_acceptance_race_reconciles_matched_claim(self):
        self.operator.accept_at_sign = True
        self.pump()
        self.assertEqual(self.operator.signed, [])
        self.assertNotEqual(self.operator.route.external['digest'], '0x' + '00' * 32)
        self.assertEqual(self.relay_commands(), [])

    def test_acceptance_race_never_hides_mismatched_external_claim(self):
        original = self.operator._sign_withdrawal
        def mismatched(*args, **kwargs):
            self.operator.route.external.update(digest='0x' + 'ff' * 32,
                recipient='0x' + self.operator.worker.record['recipient'], amount_raw='2', release_at=1040)
            return original(*args, **kwargs)
        self.operator._sign_withdrawal = mismatched
        with self.assertRaisesRegex(CustodyError, 'differs from the immutable native obligation'):
            self.pump()

    def test_unrelated_signer_refusal_is_not_treated_as_acceptance_race(self):
        self.operator.sign_refusal = 'private signer journal is corrupt'
        with self.assertRaisesRegex(CustodyError, 'private signer journal is corrupt'):
            self.pump()
        self.assertEqual(self.operator.signed, [])

    def test_spoofed_seat_bad_digest_and_economic_change_refuse(self):
        self.pump()
        self.peer_vote(1, signed=signature(2, self.custody.records[ID]['digest']))
        self.peer_vote(1, message_digest='0x' + 'ff' * 32)
        bad = {**self.custody.records[ID]['withdrawal'], 'recipient': '0x' + '88' * 20}
        self.peer_vote(2, value=bad)
        self.pump()
        self.assertEqual(len(self.custody.records[ID]['signatures']), 1)
        self.assertGreaterEqual(self.custody.stats['peer_refusals'], 3)
        self.assertEqual(self.relay_commands(), [])

    def test_duplicates_do_not_count_twice(self):
        self.pump()
        for _ in range(20):
            self.peer_vote(1)
        self.pump(40)
        self.assertEqual(len(self.custody.records[ID]['signatures']), 2)
        self.assertEqual(self.relay_commands(), [])
        self.assertGreater(self.custody.stats['peer_duplicates'], 0)

    def test_unknown_id_and_bounded_inbound_never_replay_native(self):
        self.pump()
        count = self.operator.worker.authority_calls
        unknown = {**self.custody.records[ID]['withdrawal'], 'id': '0x' + 'ef' * 32}
        self.peer_vote(1, value=unknown)
        self.assertEqual(len(self.custody.inbound), 0)
        for index in range(self.custody.MAX_INBOUND + 5):
            value = {**self.custody.records[ID]['withdrawal'], 'validUntil': 1200 + index}
            self.peer_vote(1, value=value)
        self.assertEqual(len(self.custody.inbound), self.custody.MAX_INBOUND)
        self.assertEqual(self.operator.worker.authority_calls, count)
        self.assertGreaterEqual(self.custody.stats['peer_refusals'], 5)

    def test_replaced_source_fences_before_private_signature(self):
        self.operator.route.source_canonical = False
        with self.assertRaisesRegex(CustodyError, 'source checkpoint'):
            self.pump()
        self.assertEqual(self.operator.signed, [])
        self.assertEqual(self.relay_commands(), [])

    def test_expired_authorization_renews_after_restart_without_changing_claim(self):
        self.pump()
        original = copy.deepcopy(self.custody.records[ID]['withdrawal'])
        self.custody.close()
        self.operator.route.rpc.timestamp = original['validUntil'] + 1
        self.custody = runtime.AutomaticCustody(self.operator, lambda seat, value: self.sent.append((seat, value)))
        self.pump()
        renewed = self.custody.records[ID]['withdrawal']
        self.assertGreater(renewed['validUntil'], original['validUntil'])
        self.assertLessEqual(renewed['validUntil'] - self.operator.route.rpc.timestamp, 240)
        self.assertEqual({key: value for key, value in original.items() if key != 'validUntil'},
                         {key: value for key, value in renewed.items() if key != 'validUntil'})

    def test_exact_inclusive_expiry_waits_then_renews_without_fencing(self):
        self.pump()
        original = copy.deepcopy(self.custody.records[ID]['withdrawal'])
        self.operator.route.rpc.timestamp = original['validUntil']
        self.pump()
        self.assertEqual(self.custody.records[ID]['withdrawal'], original)
        self.assertEqual(len(self.operator.signed), 1)
        self.operator.route.rpc.timestamp += 1
        self.pump()
        self.assertGreater(self.custody.records[ID]['withdrawal']['validUntil'], original['validUntil'])
        self.assertEqual(len(self.operator.signed), 2)

    def test_peer_cannot_force_renewal_at_inclusive_old_expiry(self):
        self.pump()
        original = copy.deepcopy(self.custody.records[ID]['withdrawal'])
        self.operator.route.rpc.timestamp = original['validUntil']
        later = {**original, 'validUntil': original['validUntil'] + 240}
        self.peer_vote(1, value=later)
        self.pump()
        self.assertEqual(self.custody.records[ID]['withdrawal'], original)
        self.assertEqual(len(self.operator.signed), 1)
        self.assertEqual(self.relay_commands(), [])

    def test_expiry_during_fresh_wrapper_check_retries_next_window(self):
        original_sign = self.operator._sign_withdrawal
        raced = []
        def sign(value, **fields):
            if not raced:
                raced.append(value['validUntil'])
                self.operator.route.rpc.timestamp = value['validUntil']
                raise CustodyError('withdrawal certificate expired or exceeds local acceptance bound')
            return original_sign(value, **fields)
        self.operator._sign_withdrawal = sign
        self.pump()
        self.assertEqual(len(self.operator.signed), 1)
        self.assertGreater(self.operator.signed[0]['validUntil'], raced[0])

    def test_expiry_refusal_without_confirmed_expiration_still_fences(self):
        self.operator.sign_refusal = 'withdrawal certificate expired or exceeds local acceptance bound'
        with self.assertRaisesRegex(CustodyError, 'expired or exceeds local acceptance bound'):
            self.pump()
        self.assertEqual(self.operator.signed, [])

    def test_existing_live_authorization_cannot_be_changed_by_peer(self):
        self.pump()
        original = copy.deepcopy(self.custody.records[ID]['withdrawal'])
        self.custody.close()
        self.custody = runtime.AutomaticCustody(self.operator, lambda *args: None)
        self.custody._discover(self.now)
        other = {**original, 'validUntil': original['validUntil'] + 1}
        self.peer_vote(1, value=other)
        self.pump()
        self.assertEqual(self.custody.records[ID]['withdrawal'], original)
        self.assertEqual(len(self.custody.records[ID]['signatures']), 1)

    def test_ambiguous_send_recovers_exact_nonce_before_new_work(self):
        self.pump()
        self.custody.relayer.ambiguous_once = True
        self.peer_vote(1)
        self.peer_vote(2)
        self.pump(80)
        operations = self.relay_commands()
        self.assertEqual(operations[:2], ['request', 'resume'])
        self.assertEqual(self.custody.stats['exact_recoveries'], 1)

    def test_native_paid_claim_can_reconcile_saved_raw_intent_without_new_signature(self):
        self.operator.worker.record['paid'] = True
        self.custody.relayer.needs_resume = {'required': True, 'nonce': 7,
            'action': {'operation': 'finalize', 'withdrawal_id': '0x' + ID}}
        self.pump()
        self.assertEqual(self.relay_commands(), ['resume'])
        self.assertEqual(self.operator.signed, [])

    def test_relayer_config_change_refuses_reset(self):
        path = Path(self.temp.name) / 'relayer-config.json'
        runtime._immutable_config(path, {'immutable': 'original-deployment'})
        runtime._immutable_config(path, {'immutable': 'original-deployment'})
        with self.assertRaises(CustodyError):
            runtime._immutable_config(path, {'immutable': 'different-deployment'})
        self.assertEqual(json.loads(path.read_text()), {'immutable': 'original-deployment'})


if __name__ == '__main__':
    unittest.main()
