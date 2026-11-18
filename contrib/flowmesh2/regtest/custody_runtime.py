#!/usr/bin/env python3
"""Operator-owned withdrawal completion on the authenticated REGTEST mesh.

All native/source checks run under the operator's state lock. The separate gas
child owns its own private identity and durable exact-transaction journal. Only
public obligation fields and verified custody signatures cross the peer mesh.
No user, client gateway or coordinator signs custody votes or moves gas keys.
"""
from collections import OrderedDict, deque
import json
import os
from pathlib import Path
import queue
import struct
import sys
import threading

from custody import CustodyError, Q_ASSET, address, hex_value, uint
from custody_policy import PROFILE, verify_source_point
from arbitrum_assets import AssetComplianceBlocked
from local_process import JsonProcess, raw_hex


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'))


def _immutable_config(path, value):
    encoded = (canonical(value) + '\n').encode()
    if path.exists():
        if path.read_bytes() != encoded:
            raise CustodyError('automatic custody relayer configuration changed; explicit migration is required')
        return
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(encoded)
        stream.flush()
        os.fsync(stream.fileno())
    if hasattr(os, 'O_DIRECTORY'):
        fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def _header(encoded):
    data = bytes.fromhex(raw_hex(encoded, 366))
    return {'height': struct.unpack_from('<Q', data, 73)[0],
            'anchor': {'height': struct.unpack_from('<I', data, 145)[0], 'hash': data[149:181].hex()},
            'state_root': '0x' + data[325:357].hex()}


class AutomaticCustody:
    MAX_PENDING = 256
    MAX_INBOUND = 64
    MAX_MESSAGE_BYTES = 2048
    CERTIFICATE_SECONDS = 240
    FALLBACK_SECONDS = 2.0
    SECP256K1_ORDER = 0xfffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141

    def __init__(self, operator, send_callback):
        self.operator, self.worker, self.send = operator, operator.worker, send_callback
        manifest = json.loads(Path(operator.config['deployment_file']).read_text(encoding='utf-8'))
        self.route = operator.routes['Q']
        if (manifest.get('format') != 'fm2-regtest-deployment/1' or manifest.get('chain_id') != 31337 or
                manifest.get('mainnet_enabled') is not False or manifest.get('profile_name') != 'fm2-regtest-stable-q/3' or
                manifest.get('custody_profile') != PROFILE or self.route.custody_profile != PROFILE or
                manifest.get('agreement') != 'pbft-view-change' or
                manifest.get('transport', {}).get('format') != 'fm2-seat-tcp/1'):
            raise CustodyError('automatic custody requires the exact mainnet-disabled authenticated REGTEST deployment')
        self.seat = uint(self.worker.request('info')['seat'], 8)
        self.addresses = [address(item['evm_address']) for item in manifest['operators']]
        if (len(self.addresses) != 4 or len(set(self.addresses)) != 4 or self.seat >= 4 or
                self.addresses[self.seat] != address(operator.evm_address) or
                sorted(self.addresses) != self.route.operator_addresses):
            raise CustodyError('automatic custody seat keys differ from the pinned vault custody roster')
        self.directory = operator.directory / 'automatic-custody'
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        self.records, self.inbound = OrderedDict(), deque()
        self.cursor, self.next_discovery, self.next_step = None, 0.0, 0.0
        self.next_recovery, self.recovery = 0.0, None
        self.job, self.job_thread, self.completions = None, None, queue.Queue(maxsize=1)
        self.closed, self.last_error = False, None
        self.issuer_compliance = None
        self.stats = {'native_claims_discovered': 0, 'authorizations_signed': 0, 'peer_authorizations_verified': 0,
                      'peer_refusals': 0, 'peer_duplicates': 0, 'requests_relayed': 0,
                      'finalizations_relayed': 0, 'exact_recoveries': 0, 'relay_refusals': 0,
                      'native_paid_observed': 0}
        self._start_relayer()

    def _start_relayer(self):
        config = {'format': 'fm2-signed-relayer/1', 'worker': self.operator.config['worker'],
                  'datadir': str(self.directory / 'private-gas-relayer'), 'custody_config': dict(self.route.config)}
        path = self.directory / 'relayer-config.json'
        _immutable_config(path, config)
        self.relayer = JsonProcess([sys.executable, str(Path(__file__).with_name('signed_relayer.py')),
                                    '--config', str(path)], self.directory / 'relayer.stderr.log')
        public = self.relayer.request('info')
        if public.get('chain_id') != 31337 or public.get('mainnet_enabled') is not False:
            self.relayer.close()
            raise CustodyError('private gas relayer did not retain the REGTEST release gate')
        self.gas_address = address(public['address'])
        if self.gas_address in self.addresses:
            self.relayer.close()
            raise CustodyError('gas and custody signing identities must be separate')

    def info(self):
        return {**self.stats, 'enabled': self.operator.config.get('automatic_batches', True) is True,
                'gas_address': self.gas_address, 'pending_claims': len(self.records),
                'queued_peer_authorizations': len(self.inbound), 'relay_inflight': self.job is not None,
                'last_relay_error': self.last_error, 'issuer_compliance': getattr(self, 'issuer_compliance', None),
                'mainnet_enabled': False}

    def close(self):
        if not self.closed:
            self.closed = True
            self.relayer.close()
            if self.job_thread is not None:
                self.job_thread.join(timeout=2)

    def on_applied(self, reply):
        # Native certified decisions reconstruct the original obligation after a
        # restart. This is only a prompt to rescan, never authority from Python.
        self.next_discovery = 0.0

    def on_application(self, from_seat, value):
        if (not isinstance(value, dict) or not isinstance(value.get('kind'), str) or
                not value['kind'].startswith('custody_')):
            return False
        try:
            seat = uint(from_seat, 8)
            if (seat >= 4 or seat == self.seat or value.get('kind') != 'custody_authorization' or
                    set(value) != {'format', 'kind', 'withdrawal', 'digest', 'signature'} or
                    value['format'] != 'fm2-network-app/1'):
                raise CustodyError('unknown custody application schema or authenticated seat')
            withdrawal = self._canonical_value(value['withdrawal'])
            identity = raw_hex(withdrawal['id'])
            digest, signature = hex_value(value['digest'], 32), hex_value(value['signature'], 65)
            if len(canonical(value).encode()) > self.MAX_MESSAGE_BYTES:
                raise CustodyError('custody authorization exceeds its public wire bound')
            raw = bytes.fromhex(signature[2:])
            if (not 0 < int.from_bytes(raw[:32], 'big') < self.SECP256K1_ORDER or
                    not 0 < int.from_bytes(raw[32:64], 'big') <= self.SECP256K1_ORDER // 2 or raw[64] not in (27, 28)):
                raise CustodyError('custody signature is not canonical low-S ECDSA')
            # An authenticated peer cannot allocate obligations or force native
            # replay for arbitrary IDs. Independently enumerated pending claims
            # are the only admission authority; senders retry public votes.
            if identity not in self.records:
                return True
            entry = self.records[identity]
            if (entry.get('withdrawal') == withdrawal and entry.get('digest') == digest and
                    entry.get('signatures', {}).get(seat) == signature):
                self.stats['peer_duplicates'] += 1
                return True
            item = (seat, identity, withdrawal, digest, signature)
            if item in self.inbound:
                self.stats['peer_duplicates'] += 1
            elif len(self.inbound) < self.MAX_INBOUND:
                self.inbound.append(item)
            else:
                raise CustodyError('bounded custody peer queue is full')
        except (CustodyError, TypeError, ValueError, KeyError, AttributeError):
            self.stats['peer_refusals'] += 1
        return True

    @staticmethod
    def _canonical_value(value):
        fields = {'id', 'account', 'asset', 'amount', 'recipient', 'slot', 'stateRoot', 'validUntil'}
        if not isinstance(value, dict) or set(value) != fields:
            raise CustodyError('custody vote has unknown or missing withdrawal fields')
        result = {name: hex_value(value[name], 32) for name in ('id', 'account', 'asset', 'stateRoot')}
        result.update(amount=str(uint(value['amount'])), recipient=address(value['recipient']),
                      slot=uint(value['slot'], 64), validUntil=uint(value['validUntil'], 64))
        if result['asset'] != Q_ASSET or int(result['amount']) == 0 or result != value:
            raise CustodyError('custody vote must use exact canonical stable-Q field encodings')
        return result

    def _discover(self, now):
        if now < self.next_discovery:
            return
        request = {'limit': 16}
        if self.cursor is not None:
            request['cursor'] = self.cursor
        page = self.worker.request('withdrawal_pending', **request)
        if (set(page) != {'withdrawal_ids', 'next_cursor', 'more'} or not isinstance(page['withdrawal_ids'], list) or
                len(page['withdrawal_ids']) > 16 or type(page['more']) is not bool):
            raise CustodyError('native pending-withdrawal enumeration violated its bound')
        for value in page['withdrawal_ids']:
            identity = raw_hex(value)
            if identity not in self.records and len(self.records) < self.MAX_PENDING:
                self.records[identity] = {'signatures': {}, 'next_send': 0.0, 'ready_at': None,
                                          'next_relay': 0.0, 'authority': None}
                self.stats['native_claims_discovered'] += 1
        cursor = page['next_cursor']
        if cursor is not None:
            cursor = raw_hex(cursor)
        if page['more'] != (cursor is not None) or (page['more'] and cursor == self.cursor):
            raise CustodyError('native pending-withdrawal cursor made no progress')
        self.cursor = cursor
        self.next_discovery = now + (.05 if page['more'] else .5)

    def _current(self, identity):
        current = self.worker.request('withdrawal', withdrawal_id=identity)
        if (not current.get('found') or current.get('verified') is not True or
                raw_hex(current['record']['id']) != identity or raw_hex(current['record']['asset']) != raw_hex(Q_ASSET)):
            raise CustodyError('automatic custody lacks a native verified current stable-Q claim')
        return current['record']

    def _authority(self, identity, entry, current):
        if entry['authority'] is None:
            proof = self.worker.request('withdrawal_authority', withdrawal_id=identity)
            if (not proof.get('found') or proof.get('verified') is not True or proof.get('current_unpaid') is not True or
                    proof.get('current_record') != proof.get('record') or proof['record'] != current or current['paid']):
                raise CustodyError('native original withdrawal authority/current unpaid claim mismatch')
            header = _header(proof['header'])
            if header['height'] != uint(current['slot'], 64) or not proof.get('certificate'):
                raise CustodyError('withdrawal authority is not its original certified action slot')
            self.operator.b3.verify_anchor(header['anchor'])
            point = proof['source_checkpoint']
            verify_source_point(self.route, point)
            block = self.route.rpc.call('eth_getBlockByNumber', [hex(uint(point['height'], 64)), False])
            if not isinstance(block, dict) or hex_value(block['hash'], 32) != hex_value(point['hash'], 32):
                raise CustodyError('original withdrawal source checkpoint changed while deriving its clock')
            entry['authority'] = {'record': current, 'header': header, 'source_checkpoint': point,
                                  'timestamp': int(block['timestamp'], 16)}
        authority = entry['authority']
        if authority['record'] != current:
            raise CustodyError('original committed withdrawal economic fields changed')
        return authority

    def _guard(self, authority=None):
        self.operator._verify_retained()
        self.route.verify_contract_identity()
        if authority is not None:
            self.operator.b3.verify_anchor(authority['header']['anchor'])
            verify_source_point(self.route, authority['source_checkpoint'])
            if self.route.config.get('regtest_arbitrum_fork') is not None:
                from arbitrum_fork import verify_fork_driver
                verify_fork_driver(self.route, accounts=['0x' + raw_hex(authority['record']['recipient'], 20)], check_compliance=True)
            self.issuer_compliance = None

    @staticmethod
    def _matches(value, authority):
        record = authority['record']
        return (all(raw_hex(value[name]) == raw_hex(record[name]) for name in ('id', 'account', 'asset')) and
                uint(value['amount']) == uint(record['amount']) and
                raw_hex(value['recipient'], 20) == raw_hex(record['recipient'], 20) and
                uint(value['slot'], 64) == uint(record['slot'], 64) and
                value['stateRoot'] == authority['header']['state_root'])

    def _value(self, identity, authority, timestamp):
        path = self.operator.directory / 'withdrawal-signatures' / (identity + '.json')
        if path.exists():
            prior = self._canonical_value(json.loads(path.read_text(encoding='utf-8'))['withdrawal'])
            if not self._matches(prior, authority):
                raise CustodyError('durable custody authorization differs from original committed obligation')
            if timestamp <= prior['validUntil']:
                return prior
        base = authority['timestamp']
        if timestamp < base:
            raise CustodyError('current custody time precedes original canonical source block')
        # The common original source block, not a local wall clock or latest
        # state root, anchors the acceptance window for every honest operator.
        expiry = base + ((timestamp - base) // self.CERTIFICATE_SECONDS + 1) * self.CERTIFICATE_SECONDS
        record = authority['record']
        return {'id': '0x' + identity, 'account': hex_value('0x' + raw_hex(record['account']), 32),
                'asset': Q_ASSET, 'amount': str(uint(record['amount'])),
                'recipient': address('0x' + raw_hex(record['recipient'], 20)), 'slot': uint(record['slot'], 64),
                'stateRoot': authority['header']['state_root'], 'validUntil': expiry}

    def _authorize(self, identity, entry, value, authority, timestamp):
        value = self._canonical_value(value)
        if not self._matches(value, authority) or not timestamp < value['validUntil'] <= timestamp + 300:
            raise CustodyError('peer custody authority differs from native claim or bounded acceptance window')
        self._guard(authority)
        if entry.get('withdrawal') == value and self.seat in entry['signatures']:
            return True
        prior = self.operator.directory / 'withdrawal-signatures' / (identity + '.json')
        try:
            signed = self.operator._sign_withdrawal(value, renewal=prior.exists())
        except CustodyError as error:
            # Another independent relayer may accept the exact claim between
            # our latest read and the wrapper's mandatory fresh pre-sign check.
            # Accept only that precise race, with a new matching onchain read.
            if str(error) == 'external claim is already accepted/paid; finalize its existing id instead of reauthorizing':
                _, accepted = self._external(identity, authority['record'])
                if accepted:
                    return False
            elif str(error) == 'withdrawal certificate expired or exceeds local acceptance bound':
                current_time = int(self.route.rpc.call('eth_getBlockByNumber', ['latest', False])['timestamp'], 16)
                if current_time >= value['validUntil']:
                    return False  # A fresh wrapper check observed the window close; retry with fresh time.
            raise
        digest = hex_value(signed['digest'], 32)
        if address(signed['operator']) != self.addresses[self.seat]:
            raise CustodyError('private custody signer differs from the pinned authenticated seat')
        if entry.get('withdrawal') != value:
            entry['signatures'], entry['ready_at'] = {}, None
        entry.update(withdrawal=value, digest=digest, next_send=0.0)
        entry['signatures'][self.seat] = hex_value(signed['signature'], 65)
        self.stats['authorizations_signed'] += 1
        return True

    def _external(self, identity, current):
        external = self.route.withdrawal_status('0x' + identity)
        accepted = int(raw_hex(external['digest']), 16) != 0
        if accepted:
            if (uint(external['amount_raw']) != uint(current['amount']) or
                    address(external['recipient']) != address('0x' + raw_hex(current['recipient'], 20)) or
                    uint(external['release_at'], 64) == 0):
                raise CustodyError('accepted external custody claim differs from the immutable native obligation')
        elif external['paid']:
            raise CustodyError('external custody paid flag lacks its permanent accepted claim digest')
        return external, accepted

    def _receive(self, now, timestamp):
        if not self.inbound:
            return
        seat, identity, value, digest, signature = self.inbound.popleft()
        entry = self.records.get(identity)
        if entry is None:
            return
        current = self._current(identity)
        if current['paid']:
            return
        authority = self._authority(identity, entry, current)
        _, accepted = self._external(identity, current)
        if accepted:
            return  # A delayed vote cannot request a fresh signature after acceptance.
        if not self._matches(value, authority) or not timestamp < value['validUntil'] <= timestamp + 300:
            self.stats['peer_refusals'] += 1
            return
        if entry.get('withdrawal') != value and entry.get('withdrawal', {}).get('validUntil', 0) >= timestamp:
            # A still-live durable authorization is immutable. A later peer can
            # receive that exact vote; it cannot make this operator equivocate.
            self.stats['peer_refusals'] += 1
            return
        own_path = self.operator.directory / 'withdrawal-signatures' / (identity + '.json')
        if own_path.exists():
            own = self._canonical_value(json.loads(own_path.read_text(encoding='utf-8'))['withdrawal'])
            if not self._matches(own, authority):
                raise CustodyError('durable custody authorization differs from original committed obligation')
            if own['validUntil'] >= timestamp and own != value:
                self.stats['peer_refusals'] += 1
                return
        expected = entry.get('digest') if entry.get('withdrawal') == value else self.route.withdrawal_digest(value)
        if expected != digest:
            self.stats['peer_refusals'] += 1
            return
        try:
            verified = self.route.cast.run('wallet', 'verify', '--address', self.addresses[seat], '--no-hash', digest, signature)
        except CustodyError:
            self.stats['peer_refusals'] += 1
            return
        if not verified.startswith('Validation succeeded.') or self.addresses[seat].lower() not in verified.lower():
            self.stats['peer_refusals'] += 1
            return
        # Only a verified independently pinned peer vote can ask for our vote.
        # The wrapper repeats native/current/source checks and fsyncs its own
        # intent before the private ECDSA helper signs the same digest.
        if not self._authorize(identity, entry, value, authority, timestamp):
            return
        if entry['digest'] != digest:
            raise CustodyError('independently computed custody digest differs after private authorization')
        entry['signatures'][seat] = signature
        self.stats['peer_authorizations_verified'] += 1

    def _launch(self, operation, identity=None, **fields):
        if self.job is not None or self.closed:
            raise CustodyError('automatic custody attempted overlapping private gas operations')
        self.job = {'operation': operation, 'identity': identity}
        def run():
            try:
                result, error = self.relayer.request(operation, **fields), None
            except Exception as failure:
                result, error = None, str(failure)
            self.completions.put((result, error))
        self.job_thread = threading.Thread(target=run, name='fm2-private-custody-relay', daemon=True)
        self.job_thread.start()

    def _completed(self, now):
        if self.job is None:
            return
        try:
            result, error = self.completions.get_nowait()
        except queue.Empty:
            return
        job, self.job = self.job, None
        if error is not None:
            self.last_error = error[-4096:]
            self.stats['relay_refusals'] += 1
            self.next_recovery = now + 1.0
            if job['identity'] in self.records:
                self.records[job['identity']]['next_relay'] = now + 2.0
            return
        self.last_error = None
        if job['operation'] == 'pending':
            if not isinstance(result, dict) or type(result.get('required')) is not bool:
                raise CustodyError('private signed-relayer recovery status is malformed')
            if result['required']:
                if set(result) != {'required', 'nonce', 'action'} or set(result['action']) != {'operation', 'withdrawal_id'}:
                    raise CustodyError('private signed-relayer recovery action is malformed')
                if result['action']['operation'] not in ('request', 'finalize'):
                    raise CustodyError('private signed-relayer journal contains an unsupported economic operation')
                self.recovery = {'nonce': uint(result['nonce'], 64),
                                 'identity': raw_hex(result['action']['withdrawal_id'])}
                self.next_recovery = now
            else:
                if set(result) != {'required'}:
                    raise CustodyError('private signed-relayer idle status has extra fields')
                self.next_recovery = now + 5.0
        elif job['operation'] == 'resume':
            self.stats['exact_recoveries'] += 1
            self.next_recovery = now
        elif job['operation'] in ('request', 'finalize'):
            if not isinstance(result, dict) or int(result['status'], 16) not in (0, 1):
                raise CustodyError('private signed-relayer returned no canonical execution receipt')
            if int(result['status'], 16) == 1:
                self.stats['requests_relayed' if job['operation'] == 'request' else 'finalizations_relayed'] += 1
            self.next_recovery = now

    def _eligible(self, identity, entry, now):
        if entry['ready_at'] is None:
            entry['ready_at'] = now
        primary = int(identity[-8:], 16) % 4
        priority = (self.seat - primary) % 4
        return now >= entry['ready_at'] + priority * self.FALLBACK_SECONDS and now >= entry['next_relay']

    def step(self, now, info):
        try:
            self._step(now, info)
        except AssetComplianceBlocked as error:
            # Issuer restrictions preserve the exact pending obligation and
            # existing signatures. Source/code/proxy/native failures still
            # propagate to the ordinary fatal network guard.
            self.issuer_compliance = error.reason
            self.stats['compliance_deferrals'] = self.stats.get('compliance_deferrals', 0) + 1
            self.next_step = now + .5

    def _step(self, now, info):
        if self.closed:
            return
        self._completed(now)
        if self.operator.config.get('automatic_batches', True) is not True or now < self.next_step:
            return
        self.next_step = now + .05
        self._discover(now)
        if not self.records and self.recovery is None and self.job is None and now < self.next_recovery:
            return
        self._guard()
        if self.recovery is not None and self.job is None and now >= self.next_recovery:
            # Previously signed exact intent, not a new authorization/nonce.
            # A paid native claim is allowed here only to reconcile its already
            # canonical nullifier; no changed recipient or fresh claim is made.
            recovery, self.recovery = self.recovery, None
            self._current(recovery['identity'])
            self._launch('resume', recovery['identity'], nonce=recovery['nonce'], require_success=False)
            return
        if self.job is None and now >= self.next_recovery:
            self._launch('pending')
            return
        if not self.records:
            return
        timestamp = int(self.route.rpc.call('eth_getBlockByNumber', ['latest', False])['timestamp'], 16)
        self._receive(now, timestamp)
        identity, entry = next(iter(self.records.items()))
        self.records.move_to_end(identity)
        current = self._current(identity)
        if current['paid']:
            del self.records[identity]
            self.stats['native_paid_observed'] += 1
            return
        authority = self._authority(identity, entry, current)
        external, accepted = self._external(identity, current)
        if accepted:
            if external['paid']:
                entry['signatures'].clear()
                return  # The independent complete source scanner must commit the paid fact.
            if timestamp >= uint(external['release_at'], 64) and self.job is None and self._eligible(identity, entry, now):
                self._guard(authority)
                entry['next_relay'] = now + 2.0
                self._launch('finalize', identity, withdrawal_id='0x' + identity, require_success=False)
            return
        value = self._value(identity, authority, timestamp)
        if value['validUntil'] <= timestamp:
            return  # The contract accepts the old window at equality; renewal must wait until strictly after it.
        if not self._authorize(identity, entry, value, authority, timestamp):
            return
        if now >= entry['next_send']:
            message = {'kind': 'custody_authorization', 'withdrawal': entry['withdrawal'],
                       'digest': entry['digest'], 'signature': entry['signatures'][self.seat]}
            for seat in range(4):
                if seat != self.seat:
                    self.send(seat, message)
            entry['next_send'] = now + .5
        if len(entry['signatures']) >= 3 and self.job is None and self._eligible(identity, entry, now):
            self._guard(authority)
            signatures = [signature for seat, signature in sorted(entry['signatures'].items(), key=lambda item: self.addresses[item[0]])]
            entry['next_relay'] = now + 2.0
            self._launch('request', identity, withdrawal=entry['withdrawal'], signatures=signatures, require_success=False)
