#!/usr/bin/env python3
"""Private local operator boundary: one durable BLS/ECDSA identity per process."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import re
import struct
import sys
import time
import threading

sys.path.insert(0, str(Path(__file__).resolve().parent))
from custody import (CHAIN_ID, Q_ASSET, SYNTHETIC_ASSETS, Cast, CustodyDriver,
                     CustodyError, atomic_json, hex_value, uint)
from local_process import B3Rpc, JsonProcess, raw_hex
from source_stream import SourceStream, header_binding, encode_source_facts

NETWORK_PROFILE = 'fm2-regtest-stable-q/3'

AGREEMENT_PROFILES = {'pbft-fixed-view': 'fm2-regtest-stable-q/1',
                      'pbft-view-change': 'fm2-regtest-stable-q/2'}


def header_fields(encoded):
    value = bytes.fromhex(raw_hex(encoded, 366))
    return {'height': struct.unpack_from('<Q', value, 73)[0],
            'anchor': {'height': struct.unpack_from('<I', value, 145)[0], 'hash': value[149:181].hex()},
            'facts_from': {'height': struct.unpack_from('<I', value, 181)[0], 'index': struct.unpack_from('<I', value, 185)[0]},
            'facts_to': {'height': struct.unpack_from('<I', value, 189)[0], 'index': struct.unpack_from('<I', value, 193)[0]},
            'state_root': value[325:357].hex()}


def cursor_bytes(cursor):
    return struct.pack('<II', uint(cursor['height'], 32), uint(cursor['index'], 32))


def private_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8', newline='\n') as stream:
        json.dump(value, stream, sort_keys=True)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    if hasattr(os, 'O_DIRECTORY'):
        fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def immutable_json(path, value):
    path = Path(path)
    if path.exists():
        if json.loads(path.read_text(encoding='utf-8')) != value:
            raise CustodyError('conflicting append-only authorization history')
    else:
        private_json(path, value)


class Operator:
    PUBLIC = {'info', 'submit', 'account', 'withdrawal', 'totals'}

    def __init__(self, config):
        if config.get('format') != 'fm2-regtest-wrapper/1':
            raise CustodyError('unknown operator wrapper configuration')
        self.config = config
        self.state_lock = threading.RLock()
        self.network = None
        self.public_api = None
        self.directory = Path(config['datadir']).resolve()
        self.directory.mkdir(parents=True, exist_ok=True)
        self.b3 = B3Rpc(config['b3_rpc_url'], config['b3_cookie_file'])
        self.cast = Cast(config['cast'])
        self.key_path = self.directory / 'private-identity.json'
        operator_index = uint(config['operator_index'], 8)
        if operator_index not in (1, 2, 3, 4):
            raise CustodyError('operator fixture index must be one of labels1..4')
        self.worker = JsonProcess([str(Path(config['worker']).resolve(strict=True))], self.directory / 'worker.stderr.log')
        if not self.key_path.exists():
            if (self.directory / 'state').exists() or (self.directory / 'public-identity.json').exists():
                raise CustodyError('operator private identity missing from an existing signer; refusing key reset')
            generated = self.worker.request('evm_keygen')
            seed = raw_hex(generated['seed'])
            private_json(self.key_path, {'ikm': os.urandom(32).hex(), 'ecdsa_seed': seed, 'ecdsa_policy': 'random-local-v1',
                                         'operator_index': operator_index})
        if os.name != 'nt' and self.key_path.stat().st_mode & 0o077:
            raise CustodyError('operator private identity permissions are too broad')
        self.secret = json.loads(self.key_path.read_text(encoding='utf-8'))
        if self.secret.get('ecdsa_policy') != 'random-local-v1':
            raise CustodyError('old fixture ECDSA policy requires a fresh deployment; existing key/journals retained')
        if self.secret['operator_index'] != operator_index:
            raise CustodyError('operator identity file belongs to another fixture account')
        network_requested = config.get('transport_mode') == 'authenticated-tcp'
        if network_requested:
            if 'network_seed' not in self.secret:
                if (self.directory / 'state').exists() or (self.directory / 'public-identity.json').exists():
                    raise CustodyError('transport identity missing from existing operator; refusing key reset')
                self.secret['network_seed'] = raw_hex(self.worker.request('evm_keygen')['seed'])
                atomic_json(self.key_path, self.secret)
            self.network_public_key = self.worker.request('network_key_info', seed=raw_hex(self.secret['network_seed']))['public_key']
        self.public_key = self.worker.request('key_info', ikm=raw_hex(self.secret['ikm']))
        self.evm_address = hex_value(self.worker.request('evm_key_info', seed=raw_hex(self.secret['ecdsa_seed']))['address'], 20)
        public = {**self.public_key, 'evm_address': self.evm_address, 'operator_index': operator_index}
        if network_requested:
            public['network_public_key'] = self.network_public_key
            if config.get('public_api') is not None:
                public['public_api_certificate_sha256'] = raw_hex(config['public_api']['certificate_sha256'])
        public_path = self.directory / 'public-identity.json'
        if public_path.exists() and json.loads(public_path.read_text(encoding='utf-8')) != public:
            raise CustodyError('operator public identity changed; refusing to reset existing signer')
        atomic_json(public_path, public)
        self.opened = False
        self.routes = {}
        self.pending = None
        self.pending_frozen = False
        self.source = None
        self._load_retained()
        self._load_validation_checkpoints()
        from source_handover import SourceHandover
        self.handovers = SourceHandover(self, immutable_json)

    def close(self):
        # Never wait for HTTP request threads while holding the state-owner lock.
        if self.public_api is not None:
            self.public_api.close()
        if self.network is not None:
            self.network.close()
        self.worker.close()

    def _load_retained(self):
        self.retained = {}
        self.history_tip = '00' * 32
        self.retained_tips = {self.history_tip}
        root = self.directory / 'retained-receipts'
        index = root / 'index.jsonl'
        encoded = index.read_bytes() if index.exists() else b''
        if len(encoded) > 16_777_216 or (encoded and not encoded.endswith(b'\n')):
            raise CustodyError('retained receipt index is oversized or torn; refusing truncation')
        for line in encoded.splitlines():
            entry = json.loads(line)
            digest = entry.pop('hash')
            computed = hashlib.sha256(json.dumps(entry, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
            name = entry['file']
            if (set(entry) != {'file', 'sha256', 'previous'} or entry['previous'] != self.history_tip or digest != computed or
                    re.fullmatch(r'[0-9a-f]{64}-[0-9]{10}\.json', name) is None or name in self.retained):
                raise CustodyError('retained receipt index is corrupt/conflicting')
            path = root / name
            if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
                raise CustodyError('retained receipt record missing or changed')
            value = json.loads(path.read_text(encoding='utf-8'))
            if value.get('format') != 'fm2-regtest-retained-receipt/1':
                raise CustodyError('unknown retained receipt format')
            self.retained[name] = value
            self.history_tip = digest
            self.retained_tips.add(digest)
        if root.exists() and {item.name for item in root.glob('*.json')} != set(self.retained):
            raise CustodyError('unindexed retained receipt history; refusing automatic repair/discard')

    def _retain_receipt(self, route_name, record, anchor):
        name = raw_hex(record['transaction_hash']) + '-' + str(uint(record['log_index'], 32)).zfill(10) + '.json'
        event = {'kind': record['kind'], 'route': route_name, 'transaction_hash': record['transaction_hash']}
        event['log_index'] = record['log_index']
        if record['kind'] == 'deposit':
            event.update({key: record[key] for key in ('account', 'amount_raw', 'sender')})
        else:
            event['withdrawal_id'] = record['withdrawal_id']
        if name in self.retained:
            previous = self.retained[name]
            if previous['event'] != event or previous['record'] != record:
                raise CustodyError('retained receipt identity cannot be rebound to another event/block')
            return
        receipt = self.routes[route_name].canonical_receipt(record['transaction_hash'])
        value = {'format': 'fm2-regtest-retained-receipt/1', 'event': event, 'record': record,
                 'anchor': self.b3.verify_anchor(anchor), 'receipt': receipt}
        path = self.directory / 'retained-receipts' / name
        immutable_json(path, value)
        entry = {'file': name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'previous': self.history_tip}
        digest = hashlib.sha256(json.dumps(entry, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        entry['hash'] = digest
        with (path.parent / 'index.jsonl').open('ab') as stream:
            stream.write(json.dumps(entry, sort_keys=True, separators=(',', ':')).encode() + b'\n')
            stream.flush()
            os.fsync(stream.fileno())
        if hasattr(os, 'O_DIRECTORY'):
            fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        self.retained[name] = value
        self.history_tip = digest
        self.retained_tips.add(digest)

    def _verify_retained(self):
        if getattr(self, 'source', None) is not None and self.source.guard(self.history_tip):
            return
        if not self.retained:
            return
        rpc = self.routes['Q'].rpc
        identity = rpc.assert_fixture()
        if self.receipt_checkpoint is not None:
            self._check_receipt_checkpoint(self.receipt_checkpoint)
            if int(identity['currentBlockNumber'], 16) < self.receipt_checkpoint['height'] + 1:
                raise CustodyError('retained receipt checkpoint is awaiting two local confirmations again')
            if self.receipt_checkpoint['history_tip'] == self.history_tip:
                return
        head = int(rpc.call('eth_blockNumber'), 16)
        height = head - 1
        if height < max(value['record']['block_number'] for value in self.retained.values()):
            raise CustodyError('retained receipt is awaiting at least two local confirmations')
        block = rpc.call('eth_getBlockByNumber', [hex(height), False])
        if not isinstance(block, dict):
            raise CustodyError('retained receipt validation checkpoint unavailable')
        checkpoint = {'format': 'fm2-regtest-receipt-checkpoint/1', 'history_tip': self.history_tip,
                      'height': height, 'hash': hex_value(block['hash'], 32), 'minimum_confirmations': 2}
        for original in self.retained.values():
            self.b3.verify_anchor(original['anchor'])
            event = original['event']
            if self.routes[event['route']].rpc.call('eth_getTransactionReceipt', [event['transaction_hash']]) is None:
                raise CustodyError('retained previously observed receipt disappeared; immutable history refuses new signing')
            if self._event(event) != original['record']:
                raise CustodyError('retained receipt no longer matches its original canonical block/event')
        # Same canonical descendant before and after all receipt reads prevents
        # a changing source chain from getting certified as a new checkpoint.
        self._check_receipt_checkpoint(checkpoint)
        root = self.directory / 'receipt-validation-checkpoints'
        name = self.history_tip + '.json'
        immutable_json(root / name, checkpoint)
        entry = {'file': name, 'sha256': hashlib.sha256((root / name).read_bytes()).hexdigest(),
                 'previous': self.validation_tip}
        digest = hashlib.sha256(json.dumps(entry, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        entry['hash'] = digest
        with (root / 'index.jsonl').open('ab') as stream:
            stream.write(json.dumps(entry, sort_keys=True, separators=(',', ':')).encode() + b'\n')
            stream.flush()
            os.fsync(stream.fileno())
        if hasattr(os, 'O_DIRECTORY'):
            fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        self.validation_tip, self.receipt_checkpoint = digest, checkpoint

    def _check_receipt_checkpoint(self, checkpoint):
        rpc = self.routes['Q'].rpc
        block = rpc.call('eth_getBlockByNumber', [hex(uint(checkpoint['height'], 64)), False])
        if not isinstance(block, dict) or hex_value(block['hash'], 32) != checkpoint['hash']:
            raise CustodyError('retained canonical validation checkpoint removed or replaced; refusing new signing')

    def _load_validation_checkpoints(self):
        self.validation_tip, self.receipt_checkpoint = '00' * 32, None
        root = self.directory / 'receipt-validation-checkpoints'
        encoded = (root / 'index.jsonl').read_bytes() if (root / 'index.jsonl').exists() else b''
        if len(encoded) > 16_777_216 or (encoded and not encoded.endswith(b'\n')):
            raise CustodyError('retained checkpoint index is oversized or torn')
        seen = set()
        for line in encoded.splitlines():
            entry = json.loads(line)
            digest = entry.pop('hash')
            computed = hashlib.sha256(json.dumps(entry, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
            name = entry['file']
            if (set(entry) != {'file', 'sha256', 'previous'} or entry['previous'] != self.validation_tip or digest != computed or
                    re.fullmatch(r'[0-9a-f]{64}\.json', name) is None or name in seen):
                raise CustodyError('retained checkpoint index is corrupt/conflicting')
            path = root / name
            if not path.exists() or hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
                raise CustodyError('retained checkpoint missing or changed')
            value = json.loads(path.read_text(encoding='utf-8'))
            if (value.get('format') != 'fm2-regtest-receipt-checkpoint/1' or value['history_tip'] + '.json' != name or
                    value['history_tip'] not in self.retained_tips or value.get('minimum_confirmations') != 2):
                raise CustodyError('unknown or mismatched retained checkpoint')
            hex_value(value['hash'], 32)
            uint(value['height'], 64)
            seen.add(name)
            self.validation_tip, self.receipt_checkpoint = digest, value
        if root.exists() and {item.name for item in root.glob('*.json')} != seen:
            raise CustodyError('unindexed retained checkpoint refuses automatic repair/discard')

    def _open(self, public):
        if self.opened or 'ikm' in public:
            raise CustodyError('operator is already open or caller supplied a private identity')
        manifest = json.loads(Path(self.config['deployment_file']).read_text(encoding='utf-8'))
        canonical = json.dumps(manifest, sort_keys=True, separators=(',', ':')).encode().hex()
        if public['deployment_hex'] != canonical or public['datadir'] != str(self.directory / 'state'):
            raise CustodyError('worker open differs from the operator\'s pinned public deployment/path')
        seat = uint(public['seat'], 8)
        agreement = public.get('agreement')
        if (agreement not in AGREEMENT_PROFILES or manifest.get('agreement') != agreement or
                manifest.get('profile_name') != (NETWORK_PROFILE if manifest.get('transport') is not None else AGREEMENT_PROFILES[agreement]) or
                (agreement == 'pbft-view-change' and public.get('control_instance') != 300 + seat) or
                (agreement == 'pbft-fixed-view' and 'control_instance' in public)):
            raise CustodyError('agreement/profile/control journal differs from the pinned versioned deployment')
        if (public['format'] != 'fm2-regtest-operator/1' or public['roster'] != manifest['roster'] or
                seat >= 4 or raw_hex(public['roster'][seat]['public_key'], 48) != raw_hex(self.public_key['public_key'], 48) or
                public['genesis_anchor'] != manifest['initial_anchor'] or
                public['facts_start'] != {'height': manifest['initial_anchor']['height'], 'index': 0} or
                public['signer_instance'] != 100 + seat or public['commit_instance'] != 200 + seat):
            raise CustodyError('worker identity/authority/anchor differs from the pinned deployment')
        self.b3.verify_anchor(public['genesis_anchor'])
        if manifest['b3_genesis'] != self.b3.call('getblockhash', 0):
            raise CustodyError('deployment belongs to another B3 genesis')
        if len(manifest['routes']) != 3 or set(manifest['routes']) != set(SYNTHETIC_ASSETS):
            raise CustodyError('deployment must contain exactly the three synthetic custody routes')
        if manifest.get('custody_profile') == 'fm2-custody-v2-regtest/1':
            if manifest.get('mainnet_enabled') is not False or any(
                    route.get('custody_profile') != manifest['custody_profile'] for route in manifest['routes'].values()):
                raise CustodyError('custody policy/profile or disabled-mainnet release gate differs from deployment')
        for name, route in manifest['routes'].items():
            if route['custody_asset'] != SYNTHETIC_ASSETS[name] or route['rpc_url'] != self.config['evm_rpc_url']:
                raise CustodyError('route asset or RPC differs from pinned synthetic deployment')
            self.routes[name] = CustodyDriver(route, self.directory / 'evm-observation' / name)
        supplied = dict(public)
        supplied['ikm'] = raw_hex(self.secret['ikm'])
        if manifest.get('transport') is not None:
            if (self.config.get('transport_mode') != 'authenticated-tcp' or
                    manifest['transport']['identity_keys'][seat] != self.network_public_key):
                raise CustodyError('deployment transport identity differs from the original private operator key')
            supplied['network_identity'] = {'seed': raw_hex(self.secret['network_seed'])}
            recovery_authority = self.handovers.open_authority()
            if recovery_authority is not None:
                supplied['observer_handover'] = recovery_authority
            if self.config.get('public_api') is not None and manifest['operators'][seat].get('public_api_certificate_sha256') != raw_hex(self.config['public_api']['certificate_sha256']):
                raise CustodyError('public TLS identity differs from the deployment-bound original certificate pin')
        elif self.config.get('transport_mode') is not None:
            raise CustodyError('wrapper transport opt-in differs from the pinned deployment')
        result = self.worker.request('open', **{'config': supplied})
        self.opened = True
        try:
            info = self.worker.request('info')
            if not isinstance(info.get('observation_frozen'), bool):
                raise CustodyError('worker lacks its required durable observation-freeze preflight')
            self.b3.verify_anchor(header_fields(info['header'])['anchor'])
            self.source = SourceStream(self, manifest, immutable_json)
            self.handovers.reconcile(info, self.source)
            self.source.recover(info)
            if self.handovers.latest is not None and self.handovers.latest['parent'] == raw_hex(info['head_hash']):
                self.source.finish(self.worker.request('network_resume_observation'))
            self._verify_retained()
            if manifest.get('transport') is not None:
                from network_runtime import NetworkRuntime
                self.network = NetworkRuntime(self, manifest, immutable_json)
                if self.config.get('public_api') is not None:
                    from https_api import HttpsGateway
                    self.public_api = HttpsGateway(self.config['public_api'], self.api_dispatch).start()
            elif self.config.get('public_api') is not None:
                raise CustodyError('public HTTPS activation requires the authenticated network deployment')
        except Exception:
            self.opened = False  # Failed adapter readiness can never leave a signing endpoint armed.
            raise
        return result

    def _event(self, event):
        route = self.routes[event['route']]
        receipt = route.canonical_receipt(event['transaction_hash'])
        if event['kind'] == 'deposit':
            return route.expected_deposit(receipt, event['account'], event['amount_raw'], event['sender'], log_index=event.get('log_index'))
        if event['kind'] != 'paid' or event['route'] != 'Q':
            raise CustodyError('only Q payouts are supported by this profile')
        withdrawal = self.worker.request('withdrawal', withdrawal_id=raw_hex(event['withdrawal_id']))
        if not withdrawal.get('found') or withdrawal.get('verified') is not True:
            raise CustodyError('payout lacks this operator\'s committed withdrawal proof')
        record = withdrawal['record']
        parsed = route.expected_paid(receipt, '0x' + raw_hex(record['id']), record['amount'], '0x' + raw_hex(record['recipient'], 20),
                                    log_index=event.get('log_index'))
        parsed['account'] = '0x' + raw_hex(record['account'])
        return parsed

    def _observe(self, anchor, events, expected=None, evm_end=None, handover=None):
        info = self.worker.request('info')
        if self.source is None or raw_hex(info['head_hash']) != self.source.head_hash:
            raise CustodyError('worker and source checkpoint head differ; restart without discarding state')
        if not isinstance(info.get('observation_frozen'), bool):
            raise CustodyError('worker lacks its required durable observation-freeze preflight')
        pending_current = self.pending is not None and raw_hex(self.pending['parent_head_hash']) == raw_hex(info['head_hash'])
        frozen = info['observation_frozen'] or (self.pending_frozen and pending_current)
        if frozen and expected is None and handover is None:
            raise CustodyError('source observation is frozen by a published vote; complete the original proposal')
        if expected is not None:
            if (anchor != expected['anchor'] or events != expected['events'] or evm_end != expected['source']['to'] or
                    self.source.pending != expected):
                raise CustodyError('frozen recovery must restore the exact original complete-source batch')
            self.source.validate_observation_file(expected)
        self._verify_retained()
        self.b3.verify_anchor(anchor)
        current = header_fields(info['header'])
        self.b3.verify_anchor(current['anchor'])
        if uint(anchor['height'], 32) < current['anchor']['height']:
            raise CustodyError('observation anchor regresses')
        cursor = dict(current['facts_to'])
        start = dict(cursor)
        discovered, records, source_range = self.source.scan(evm_end, events)
        facts, cursor = encode_source_facts(discovered, records, start, info['domain'], source_range, self.source.range_commitment)
        for event, record in zip(discovered, records):
            self._retain_receipt(event['route'], record, anchor)
        saved = {'format': 'fm2-regtest-source-observation/1', 'parent_head_hash': raw_hex(info['head_hash']),
                 'anchor': anchor, 'events': discovered, 'records': records, 'facts_from': start, 'facts_to': cursor,
                 'facts': facts, 'source': source_range, 'history_tip': self.history_tip}
        if expected is not None and saved != expected:
            raise CustodyError('previously observed facts changed before voting/recovery')
        observation_fields = {'anchor': anchor, 'facts_from': start, 'facts_to': cursor, 'facts': facts}
        if handover is not None:
            if expected is not None or handover.get('header') is None:
                raise CustodyError('source handover requires exact native certified authority')
            observation_fields['observation_token'] = handover['token']
        preflight = self.worker.request('check_observation', **observation_fields)
        if preflight.get('acceptable') is not True or preflight.get('observation_frozen') != info['observation_frozen']:
            raise CustodyError('worker did not preflight the exact observation under its unchanged durable freeze')
        publish = None
        if handover is not None:
            initial = self.pending is None or raw_hex(self.pending['parent_head_hash']) != raw_hex(info['head_hash'])
            pointer = self.directory / 'pending-observation.json'
            prior = (json.loads(pointer.read_text(encoding='utf-8')) if pointer.exists() else None) if initial else self.pending
            if initial:
                # No old observation exists for this parent. Persist the exact
                # verified authority first, so a crash before either new source
                # file/pointer can recover only this embedded selected value.
                self.handovers.record(prior, saved, handover, initial=True)
            else:
                publish = lambda: self.handovers.record(prior, saved, handover)
        self.source.save_pending(saved, before_publish=publish)
        self._verify_retained()
        result = self.worker.request('observe', **observation_fields)
        self.pending = saved
        self.pending_frozen = frozen
        return {'observed': result, 'facts_from': start, 'facts_to': cursor, 'records': records,
                'source_validation_checkpoint': source_range['to'], 'source_range': source_range}

    def _validate_proposal(self, fields):
        self._verify_retained()
        self.b3.verify_anchor(header_fields(fields['header'])['anchor'])
        self._check_proposal_binding(fields)

    def _check_proposal_binding(self, fields):
        # Pure immutable-value comparison. Network callers still run a fresh
        # canonical guard immediately before each native call that can sign.
        proposal = header_fields(fields['header'])
        if self.pending is None:
            raise CustodyError('operator has not independently observed this next batch')
        if (proposal['anchor'] != self.pending['anchor'] or proposal['facts_from'] != self.pending['facts_from'] or
                proposal['facts_to'] != self.pending['facts_to']):
            raise CustodyError('proposal does not consume this operator\'s exact independent observation range')
        self.source.verify_binding(self.pending, fields['header'], header_binding(fields['header'])['hash'])

    def _validate_pending(self):
        self._verify_retained()
        if self.pending is None:
            raise CustodyError('PBFT requires this operator\'s independently observed next batch')
        self.b3.verify_anchor(self.pending['anchor'])

    def _sign_withdrawal(self, value, renewal=False):
        self._verify_retained()
        if self.routes['Q'].withdrawal_tuple(value) is None:
            raise CustodyError('noncanonical withdrawal')
        own = self.worker.request('withdrawal_authority', withdrawal_id=raw_hex(value['id']))
        info = self.worker.request('info')
        if (not own.get('found') or own.get('verified') is not True or own.get('current_unpaid') is not True or
                own.get('current_record') != own.get('record')):
            raise CustodyError('withdrawal lacks original committed authority and a current identical unpaid claim')
        record, header = own['record'], header_fields(own['header'])
        self.b3.verify_anchor(header['anchor'])
        if 'source_checkpoint' in own:
            from custody_policy import verify_source_point
            verify_source_point(self.routes['Q'], own['source_checkpoint'])
        if record['paid'] or raw_hex(record['asset']) != raw_hex(Q_ASSET):
            raise CustodyError('withdrawal must be unpaid and Q-denominated')
        external = self.routes['Q'].withdrawal_status(value['id'])
        if external['paid'] or int(raw_hex(external['digest']), 16) != 0:
            raise CustodyError('external claim is already accepted/paid; finalize its existing id instead of reauthorizing')
        for name in ('id', 'account', 'asset'):
            if raw_hex(value[name]) != raw_hex(record[name]):
                raise CustodyError('withdrawal certificate differs from committed ' + name)
        if (uint(value['amount']) != uint(record['amount']) or raw_hex(value['recipient'], 20) != raw_hex(record['recipient'], 20) or
                uint(value['slot'], 64) != uint(record['slot'], 64) or raw_hex(value['stateRoot']) != header['state_root']):
            raise CustodyError('withdrawal principal/recipient/slot/state differs from committed obligation')
        timestamp = int(self.routes['Q'].rpc.call('eth_getBlockByNumber', ['latest', False])['timestamp'], 16)
        expiry = uint(value['validUntil'], 64)
        if not timestamp < expiry <= timestamp + 300:
            raise CustodyError('withdrawal certificate expired or exceeds local acceptance bound')
        digest = self.routes['Q'].withdrawal_digest(value)
        path = self.directory / 'withdrawal-signatures' / (raw_hex(value['id']) + '.json')
        intent = {'withdrawal': value, 'digest': digest, 'head_hash': info['head_hash'], 'evm_address': self.evm_address}
        prior = None
        if path.exists():
            prior = json.loads(path.read_text(encoding='utf-8'))
            previous = prior['withdrawal']
            for name in ('id', 'account', 'asset', 'recipient'):
                size = 20 if name == 'recipient' else 32
                if raw_hex(previous[name], size) != raw_hex(value[name], size):
                    raise CustodyError('renewal changes an immutable economic withdrawal field')
            if uint(previous['amount']) != uint(value['amount']) or uint(previous['slot'], 64) != uint(value['slot'], 64):
                raise CustodyError('renewal changes principal or original action slot')
            if prior['digest'] == digest and previous == value:
                if prior.get('signature'):
                    return {'operator': self.evm_address, 'signature': prior['signature'], 'digest': digest}
                intent = dict(prior)  # Resume the exact pre-signing intent, including its original head evidence.
            elif (not renewal or uint(previous['validUntil'], 64) >= timestamp or
                  uint(value['validUntil'], 64) <= uint(previous['validUntil'], 64)):
                raise CustodyError('changed authorization requires explicit monotone renewal after previous expiry')
        elif renewal:
            raise CustodyError('renewal requires this operator\'s durable previous authorization')

        def archive(entry):
            root = self.directory / 'withdrawal-signature-history' / raw_hex(value['id'])
            name = str(uint(entry['withdrawal']['validUntil'], 64)).zfill(20) + '-' + raw_hex(entry['digest'])
            unsigned = {key: item for key, item in entry.items() if key != 'signature'}
            immutable_json(root / (name + '.intent.json'), unsigned)
            if entry.get('signature'):
                immutable_json(root / (name + '.signed.json'), entry)

        if prior is not None:
            archive(prior)
        archive(intent)
        atomic_json(path, intent)
        signed = self.worker.request('evm_sign', seed=raw_hex(self.secret['ecdsa_seed']), digest=raw_hex(digest))
        if signed['address'] != self.evm_address:
            raise CustodyError('private ECDSA helper did not use this operator\'s exact pinned key')
        intent['signature'] = hex_value('0x' + raw_hex(signed['signature'], 65), 65)
        archive(intent)
        atomic_json(path, intent)
        return {'operator': self.evm_address, 'signature': intent['signature'], 'digest': digest}

    def api_dispatch(self, command, fields):
        if not self.state_lock.acquire(timeout=2):
            raise CustodyError('operator state owner is busy; retry the signed action unchanged')
        try:
            return self._request(command, fields)
        finally:
            self.state_lock.release()

    def request(self, command, fields):
        # The source adapter, worker, peer loop and public request share one
        # serialized state owner. A pipe lock alone does not protect checkpoints.
        with self.state_lock:
            return self._request(command, fields)

    def _request(self, command, fields):
        if command == 'bootstrap':
            value = {**self.public_key, 'evm_address': self.evm_address, 'operator_index': self.config['operator_index']}
            if self.config.get('transport_mode') == 'authenticated-tcp':
                value['network_public_key'] = self.network_public_key
                if self.config.get('public_api') is not None:
                    value['public_api_certificate_sha256'] = raw_hex(self.config['public_api']['certificate_sha256'])
            return value
        if command == 'open':
            return self._open(fields['config'])
        if not self.opened:
            raise CustodyError('operator is not open')
        if self.network is not None:
            if command == 'network_committed':
                if set(fields) != {'height'}:
                    raise CustodyError('certified history query requires one exact height')
                return self.worker.request('network_history', height=uint(fields['height'], 64))
            if command == 'network_automatic':
                if set(fields) != {'enabled'} or type(fields['enabled']) is not bool:
                    raise CustodyError('automatic batch policy requires an explicit boolean')
                self.network.automatic.enabled = fields['enabled']
                self.config['automatic_batches'] = fields['enabled']
                return self.network.status()
            if command == 'network_fault_drop_commits':
                if set(fields) != {'enabled'} or type(fields['enabled']) is not bool:
                    raise CustodyError('regtest commit-loss fault requires an explicit boolean')
                return self.worker.request(command, enabled=fields['enabled'])
            if command == 'network_handover_barrier':
                if set(fields) != {'enabled'} or type(fields['enabled']) is not bool:
                    raise CustodyError('regtest handover fault barrier requires an explicit boolean')
                self.config['regtest_handover_barrier'] = fields['enabled']
                return {'enabled': fields['enabled']}
            if command == 'network_pause':
                if set(fields) != {'paused'} or type(fields['paused']) is not bool:
                    raise CustodyError('regtest peer-loop pause requires an explicit boolean')
                self.network.paused = fields['paused']
                return self.network.status()
            if command == 'network_request_history':
                if set(fields) != {'peer', 'height'}:
                    raise CustodyError('historical replay probe requires exact pinned peer/retained height')
                return self.network.probe_history(fields['peer'], fields['height'])
            if command == 'network_status':
                return self.network.status()
            if command == 'network_endpoint':
                if fields:
                    raise CustodyError('endpoint signing accepts no caller-supplied record')
                return self.worker.request(command)
            if command == 'network_start':
                if set(fields) != {'signatures'}:
                    raise CustodyError('network activation accepts only pinned endpoint signatures')
                return self.network.start(fields['signatures'])
            if command == 'network_seal':
                if not {'actions'} <= set(fields) or not set(fields) <= {'actions', 'anchor', 'evm_end'}:
                    raise CustodyError('client seal accepts only signed actions and bounded source hints')
                return self.network.seal(**fields)
            if command in ('pbft_start', 'pbft_receive', 'pbft_drain', 'pbft_timeout', 'vote', 'commit', 'cert_from_votes'):
                raise CustodyError('peer identity and agreement routing belong only to authenticated network sockets')
            if self.network.fenced and command not in ('info', 'account', 'withdrawal', 'totals'):
                raise CustodyError('authenticated runtime is fenced: ' + self.network.fenced)
        if command == 'observe_receipts':
            if (set(fields) not in ({'anchor', 'events'}, {'anchor', 'events', 'evm_end'}) or
                    (fields['events'] is not None and len(fields['events']) > 128)):
                raise CustodyError('observation accepts only a bounded set of receipt references')
            return self._observe(fields['anchor'], fields['events'], evm_end=fields.get('evm_end'))
        if command in ('vote', 'commit', 'cert_from_votes'):
            self._validate_proposal(fields)
            return self.worker.request(command, **fields)
        if command == 'pbft_start':
            self._validate_proposal(fields)
            self.pending_frozen = True
            return self.source.finish(self.worker.request(command, **fields))
        if command == 'pbft_receive':
            self._validate_pending()
            if set(fields) != {'from_seat', 'message'} or uint(fields['from_seat'], 8) >= 4:
                raise CustodyError('PBFT pipe frame has an invalid peer identity')
            raw_hex(fields['message'], len(fields['message'].removeprefix('0x')) // 2)
            self.pending_frozen = True
            return self.source.finish(self.worker.request(command, **fields))
        if command in ('pbft_drain', 'pbft_timeout'):
            self._validate_pending()
            return self.source.finish(self.worker.request(command, **fields))
        if command == 'candidate':
            self._verify_retained()
            if self.pending is None:
                raise CustodyError('candidate requires independent observation first')
            if self.source is None or raw_hex(self.pending['parent_head_hash']) != self.source.head_hash:
                raise CustodyError('candidate needs a fresh complete-source observation for its current parent')
            self.b3.verify_anchor(self.pending['anchor'])
            return self.worker.request(command, **fields)
        if command == 'sign_withdrawal':
            return self._sign_withdrawal(fields['withdrawal'])
        if command == 'renew_withdrawal':
            return self._sign_withdrawal(fields['withdrawal'], renewal=True)
        if command in self.PUBLIC:
            result = self.worker.request(command, **fields)
            if command == 'submit' and self.network is not None:
                self.network.automatic.enqueue(fields['frame'], result, public=True)
            if command == 'info' and self.network is not None:
                result['automatic'] = self.network.automatic.status()
                result['custody_runtime'] = self.network.custody.info()
            if command == 'info' and self.public_api is not None:
                result['public_api'] = self.public_api.info()
            return result
        raise CustodyError('command is outside the operator wrapper boundary')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True)
    args = parser.parse_args()
    import stage_trace
    stage_trace.install('operator')  # no-op unless FM2_STAGE_TRACE_DIR is set
    operator = None
    try:
        def stop_owned(signum, frame):
            raise SystemExit(128 + signum)
        signal.signal(signal.SIGTERM, stop_owned)
        signal.signal(signal.SIGINT, stop_owned)
        operator = Operator.__new__(Operator)
        operator.__init__(json.loads(Path(args.config).read_text(encoding='utf-8')))
        while True:
            line = sys.stdin.buffer.readline(16_777_217)
            if not line:
                break
            if len(line) > 16_777_216 or not line.endswith(b'\n'):
                raise CustodyError('wrapper request exceeds framing bound')
            identity = None
            try:
                request = json.loads(line)
                identity = request.pop('id')
                command = request.pop('cmd')
                result = operator.request(command, request)
                reply = {'id': identity, 'ok': True, 'result': result}
            except Exception as error:
                reply = {'id': identity, 'ok': False, 'error': {'code': 'REGTEST_REFUSAL', 'detail': str(error), 'fatal': False}}
            print(json.dumps(reply, sort_keys=True, separators=(',', ':')), flush=True)
    finally:
        if operator is not None and hasattr(operator, 'worker'):
            operator.close()


if __name__ == '__main__':
    main()
