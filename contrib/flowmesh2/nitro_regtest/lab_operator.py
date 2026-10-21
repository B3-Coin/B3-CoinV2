#!/usr/bin/env python3
"""Private isolated lab seat: native proofs precede observations and votes.

The owned Nitro full node authenticates the separately supplied checkpoint.
The native history checker verifies commitments, NOT L1 settlement/execution.
There is no command accepting a caller's verified flag, facts, or signing seed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import signal
import subprocess
import sys

from lab_common import (CHAIN, PROFILE, HERE, IMPL_SLOT, WITHDRAWAL_ABI, NitroLabChain, JsonProcess, atomic_json,
                        canonical, expect, private_new, raw_file, raw_hex, withdrawal_tuple)
from source_stream import encode_source_facts, header_binding, fact_root
from local_process import B3Rpc


def check_custody_roster(chain, vault, expected):
    # The unchanged generic constructor leaves rosterEpoch at Solidity's zero
    # default. Epoch one is reached only after an actual accepted rotation.
    expect(chain.number(vault, 'rosterEpoch()') == 0 and
           ['0x' + chain.call(vault, 'operators(uint256)', i)[-40:] for i in range(4)] == expected,
           'lab custody roster or epoch changed')


class LabOperator:
    def __init__(self, config):
        self.config = config
        self.directory = Path(config['directory']).resolve(strict=True)
        self.worker = JsonProcess([config['worker']], self.directory / 'native.stderr.log')
        self.chain = NitroLabChain(config['runtime'], config['tools'], self.directory / 'chain-io', config['source_root'])
        self.b3 = B3Rpc(config['b3_rpc_url'], config['b3_cookie'])
        path = self.directory / 'private-identity.json'
        if path.exists():
            self.secret = json.loads(raw_file(path, 16384))
        else:
            self.secret = {'ikm': os.urandom(32).hex(), 'ecdsa': self.worker.request('evm_keygen')}
            private_new(path, self.secret)
        key = self.worker.request('key_info', ikm=self.secret['ikm'])
        self.public = {**key, 'evm_address': self.secret['ecdsa']['address']}
        self.manifest = self.current = self.pending = None
        self.opened = False

    def open(self, create):
        manifest = json.loads(raw_file(self.config['manifest'], 262144))
        expect(manifest['format'] == 'fm2-owned-nitro-lab-deployment/1' and manifest['profile_name'] == PROFILE and
               manifest['chain_id'] == CHAIN and manifest['mainnet_enabled'] is False, 'lab deployment mode mismatch')
        expect(manifest['nitro_lab']['source_genesis_hash'] == raw_hex(self.chain.genesis), 'lab source genesis differs')
        self.manifest = manifest
        seat = next(i for i, value in enumerate(manifest['roster']) if value['public_key'] == self.public['public_key'])
        self.b3.verify_anchor(manifest['initial_anchor'])
        config = {'format': 'fm2-regtest-operator/1', 'seat': seat, 'roster': manifest['roster'],
            'agreement': 'direct-commit', 'datadir': str(self.directory / 'native-state'), 'create': create,
            'ikm': self.secret['ikm'], 'signer_instance': 100 + seat, 'commit_instance': 200 + seat,
            'deployment_hex': canonical(manifest).encode().hex(), 'genesis_anchor': manifest['initial_anchor'],
            'facts_start': {'height': manifest['initial_anchor']['height'], 'index': 0}, 'nitro_lab': manifest['nitro_lab']}
        info = self.worker.request('open', config=config)
        expect(info['profile_name'] == PROFILE and info['mainnet_enabled'] is False and info['quorum'] == 3,
               'native lab worker selected another mode')
        source_file = self.directory / 'source-current.json'
        if source_file.exists():
            self.current = json.loads(raw_file(source_file, 65536))
        else:
            expect(info['height'] == 0, 'existing native state lacks original source cursor')
            self.current = {'head_hash': info['head_hash'], 'height': 0, 'point': manifest['source_initial'], 'check': None}
            private_new(source_file, self.current)
        pending_file = self.directory / 'source-pending.json'
        self.pending = json.loads(raw_file(pending_file, 4 * 1024 * 1024)) if pending_file.exists() else None
        if self.current['head_hash'] != info['head_hash']:
            expect(self.pending is not None and self.pending['parent'] == self.current['head_hash'],
                   'native applied without exact pre-vote source evidence')
            self.bind_header(info['header'])
            self.applied(info)
        self.guard()
        if self.pending is not None and self.pending['parent'] == info['head_hash']:
            self.recheck(self.pending['check'])
            self.b3.verify_anchor(self.pending['observation']['anchor'])
            self.worker.request('observe', **self.pending['observation'])
        self.opened = True
        return info

    def identity(self):
        self.chain.identity()
        binding = self.manifest['nitro_lab']
        token, implementation = ('0x' + binding[name] for name in ('quote_token', 'quote_implementation'))
        expect(self.chain.code_hash(token) == '0x' + binding['quote_token_code_hash'] and
               self.chain.code_hash(implementation) == '0x' + binding['quote_implementation_code_hash'],
               'lab USDT0 runtime changed')
        slot = self.chain.extra_read('eth_getStorageAt', [token, IMPL_SLOT, 'latest'])
        expect(raw_hex(slot) == '00' * 12 + binding['quote_implementation'], 'lab USDT0 implementation replaced')
        for route in self.manifest['routes'].values():
            expect(self.chain.code_hash(route['vault']) == route['vault_code_hash'] and
                   self.chain.code_hash(route['token']) == route['token_code_hash'], 'lab custody runtime changed')
            check_custody_roster(self.chain, route['vault'], self.manifest['custody_addresses'])

    def guard(self):
        self.identity()
        self.chain.check_point(self.current['point'])
        if self.current['check'] is not None:
            self.recheck(self.current['check'])

    def recheck(self, context):
        self.identity()
        self.chain.check_point(context['previous'])
        self.chain.check_point(context['to'])
        wire = raw_file(context['proof'], 32 * 1024 * 1024)
        expect(hashlib.sha256(wire).hexdigest() == context['sha256'], 'retained proof bytes changed')
        command = [self.config['checker'], '--previous-height', str(context['previous']['height']),
                   '--previous-hash', context['previous']['hash'], '--to-height', str(context['to']['height']),
                   '--to-hash', context['to']['hash']]
        for route in self.manifest['routes'].values():
            command += ['--vault', route['vault']]
        checked = subprocess.run(command, input=wire, capture_output=True, timeout=30)
        expect(checked.returncode == 0 and len(checked.stdout) <= 8 * 1024 * 1024, 'native source proof refused')
        reply = json.loads(checked.stdout)
        expect(reply['result'] == 'OK' and reply['commitments_match'] is True and reply['arbitrum_authority'] is False and
               reply['external_anchor_required'] is True and reply['mainnet_enabled'] is False and
               reply['execution_verified'] is False and reply['rollup_settlement_verified'] is False,
               'native checker scope/result mismatch')
        accounts = {entry['address'].lower(): entry for entry in reply['newest_accounts']}
        for route in self.manifest['routes'].values():
            for target, expected in ((route['vault'], route['vault_code_hash']), (route['token'], route['token_code_hash'])):
                expect(target in accounts and accounts[target]['code_hash'] == expected, 'proof omits exact route runtime')
        binding = self.manifest['nitro_lab']
        proxy = accounts.get('0x' + binding['quote_token'])
        expect(proxy is not None, 'proof omits USDT0 proxy')
        implementation = accounts.get('0x' + binding['quote_implementation'])
        expect(implementation is not None and implementation['code_hash'] == '0x' + binding['quote_implementation_code_hash'],
               'proof omits exact USDT0 implementation runtime')
        slots = {entry['slot']: entry['value'] for entry in proxy['storage']}
        expect(IMPL_SLOT in slots and slots[IMPL_SLOT] == '0x' + '00' * 12 + binding['quote_implementation'], 'proof omits exact proxy implementation')
        private_new(self.directory / 'native-proof-checks' / (context['sha256'] + '.json'), reply)
        self.chain.check_point(context['to'])
        return reply

    def observe(self, fields):
        expect(set(fields) <= {'proof', 'to', 'anchor', 'expected_events'}, 'unknown source request fields')
        self.guard()
        info = self.worker.request('info')
        self.b3.verify_anchor(fields['anchor'])
        point = fields['to']
        expect(set(point) == {'height', 'hash', 'minimum_confirmations'} and point['minimum_confirmations'] == 1,
               'wrong local checkpoint policy')
        expect(self.current['point']['height'] <= point['height'] <= self.current['point']['height'] + 128,
               'source cursor regresses/skips bounded range')
        if point == self.current['point']:
            expect(self.current['check'] is not None, 'empty source range needs its original checked proof')
            context = self.current['check']
            checked = self.recheck(context)
            records = []
        else:
            wire = raw_file(fields['proof'], 32 * 1024 * 1024)
            digest = hashlib.sha256(wire).hexdigest()
            path = self.directory / 'source-proofs' / (digest + '.bin')
            path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            if path.exists():
                expect(raw_file(path, 32 * 1024 * 1024) == wire, 'proof CAS conflict')
            else:
                fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
                with os.fdopen(fd, 'wb') as stream:
                    stream.write(wire); stream.flush(); os.fsync(stream.fileno())
                directory = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    os.fsync(directory)
                finally:
                    os.close(directory)
            context = {'proof': str(path), 'sha256': digest, 'previous': self.current['point'], 'to': point}
            checked = self.recheck(context)
            records = sorted(checked['candidate_events'], key=lambda value:
                (int(value['block_number']), int(value['transaction_index']), int(value['log_index'])))
        if 'expected_events' in fields:
            expect(fields['expected_events'] == records, 'caller omitted or changed independently native-derived events')
        routes = {route['vault']: name for name, route in self.manifest['routes'].items()}
        events, normalized = [], []
        for record in records:
            name = routes[record['vault']]
            value = dict(record)
            expect(0 < int(value['amount_raw']) < 1 << 62, 'candidate amount exceeds native atom bound')
            if value['kind'] == 'deposit':
                expect(value['asset'] == self.manifest['routes'][name]['custody_asset'], 'deposit asset differs from pinned route')
            else:
                expect(name == 'Q', 'only lab quote payout is supported')
                proof = self.worker.request('withdrawal', withdrawal_id=raw_hex(value['withdrawal_id']))
                expect(proof.get('verified') is True and not proof['record']['paid'] and
                       int(value['amount_raw']) == proof['record']['amount'] and
                       raw_hex(value['recipient'], 20) == proof['record']['recipient'], 'paid event differs from native obligation')
                value['account'], value['asset'] = '0x' + proof['record']['account'], '0x' + proof['record']['asset']
            events.append({'kind': value['kind'], 'route': name})
            normalized.append(value)
        start = dict(info['facts_to'])
        facts, cursor = encode_source_facts(events, normalized, start, info['domain'], {}, False)
        previous = self.current['point']
        range_fact = (struct.pack('<BII', 7, cursor['height'], cursor['index']) + bytes.fromhex(info['domain']) +
            struct.pack('<QQ', CHAIN, previous['height']) + bytes.fromhex(raw_hex(previous['hash'])) +
            struct.pack('<Q', point['height']) + bytes.fromhex(raw_hex(point['hash'])) + struct.pack('<I', 1))
        facts.append(range_fact.hex()); cursor['index'] += 1
        observation = {'anchor': fields['anchor'], 'facts_from': start, 'facts_to': cursor, 'facts': facts}
        self.worker.request('check_observation', **observation)
        pending = {'parent': info['head_hash'], 'point': point, 'check': context, 'observation': observation,
                   'records': records, 'fact_root': fact_root(facts)}
        private_new(self.directory / 'source-observations' / (hashlib.sha256(canonical(pending).encode()).hexdigest() + '.json'), pending)
        atomic_json(self.directory / 'source-pending.json', pending)
        self.worker.request('observe', **observation)
        self.pending = pending
        return {'events': records, 'facts': len(facts), 'facts_to': cursor, 'source_to': point,
                'proof_digest': checked['proof_digest'], 'arbitrum_authority': False}

    def bind_header(self, header):
        expect(self.pending is not None, 'no independently checked pending source')
        binding = header_binding(header)
        o = self.pending['observation']
        expect(binding['parent'] == self.pending['parent'] and binding['anchor'] == o['anchor'] and
               binding['facts_from'] == o['facts_from'] and binding['facts_to'] == o['facts_to'] and
               binding['facts_root'] == self.pending['fact_root'], 'candidate differs from own complete source observation')

    def applied(self, info):
        self.bind_header(info['header'])
        next_state = {'head_hash': info['head_hash'], 'height': info['height'], 'point': self.pending['point'],
                      'check': self.pending['check']}
        private_new(self.directory / 'source-applied' / (str(info['height']).zfill(20) + '-' + info['head_hash'] + '.json'),
                    {'native': info, 'source': self.pending})
        atomic_json(self.directory / 'source-current.json', next_state)
        self.current = next_state

    def sign_claim(self, value):
        self.guard()
        proof = self.worker.request('withdrawal_authority', withdrawal_id=raw_hex(value['id']))
        expect(proof.get('verified') is True and proof.get('current_unpaid') is True and
               proof['current_record'] == proof['record'], 'native historical withdrawal authority missing')
        record = proof['record']
        header = header_binding(proof['header'])
        self.b3.verify_anchor(header['anchor'])
        expect(all(raw_hex(value[name]) == record[name] for name in ('id', 'account', 'asset')) and
               int(value['amount']) == record['amount'] and raw_hex(value['recipient'], 20) == record['recipient'] and
               int(value['slot']) == record['slot'] and raw_hex(value['stateRoot']) == raw_hex(proof['header'], 366)[650:714],
               'authorization changes native obligation')
        q = self.manifest['routes']['Q']['vault']
        status = self.chain.call(q, 'withdrawals(bytes32)', value['id'])
        expect(int(status[2:66], 16) == 0, 'external claim already accepted; never reauthorize it')
        now = int(self.chain.read.call('eth_getBlockByNumber', ['latest', False])['timestamp'], 16)
        expect(now < int(value['validUntil']) <= now + 900, 'claim expiry exceeds lab policy')
        digest = self.chain.call(q, 'withdrawalDigest(' + WITHDRAWAL_ABI + ')', withdrawal_tuple(value))
        intent = {'value': value, 'digest': digest, 'head_hash': proof['head_hash']}
        path = self.directory / 'withdrawal-authorizations' / (raw_hex(value['id']) + '.json')
        private_new(path.with_suffix('.intent.json'), intent)
        if path.exists():
            prior = json.loads(raw_file(path, 16384))
            expect(prior['intent'] == intent, 'claim signing intent changed')
            return prior['signature']
        signature = self.worker.request('evm_sign', seed=self.secret['ecdsa']['seed'], digest=raw_hex(digest))
        expect(signature['address'] == self.public['evm_address'], 'wrong independent custody key')
        private_new(path, {'intent': intent, 'signature': signature})
        return signature

    def handle(self, command, fields):
        if command == 'bootstrap':
            expect(not fields, 'bootstrap has no caller identity fields')
            return self.public
        if command == 'open':
            expect(set(fields) == {'create'} and type(fields['create']) is bool, 'exact open mode required')
            return self.open(fields['create'])
        expect(self.opened, 'open private lab seat first')
        if command == 'observe_bundle':
            return self.observe(fields)
        if command == 'sign_claim':
            expect(set(fields) == {'value'}, 'exact claim only')
            return self.sign_claim(fields['value'])
        expect(command in ('info', 'candidate', 'vote', 'cert_from_votes', 'commit', 'submit', 'account',
                           'withdrawal', 'withdrawal_authority', 'totals'), 'private lab command denied')
        if command in ('candidate', 'vote'):
            expect(self.pending is not None, 'complete proof observation required before signing')
            self.recheck(self.pending['check'])
            self.b3.verify_anchor(self.pending['observation']['anchor'])
            if command == 'vote':
                self.bind_header(fields['header'])
        if command == 'commit':
            self.guard()
            before = self.worker.request('info')
            if header_binding(fields['header'])['height'] > before['height']:
                self.bind_header(fields['header'])
                self.b3.verify_anchor(self.pending['observation']['anchor'])
            reply = self.worker.request(command, **fields)
            if not reply['duplicate']:
                self.applied(reply)
            return reply
        return self.worker.request(command, **fields)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True)
    args = parser.parse_args()
    operator = LabOperator(json.loads(raw_file(args.config, 65536)))
    def stop(_signal, _frame):
        raise SystemExit(0)
    signal.signal(signal.SIGTERM, stop)
    try:
        while True:
            line = sys.stdin.buffer.readline(33_554_433)
            if not line:
                return 0
            expect(len(line) <= 33_554_432 and line.endswith(b'\n'), 'bounded JSONL required')
            request = json.loads(line)
            try:
                fields = {key: value for key, value in request.items() if key not in ('cmd', 'id')}
                result = operator.handle(request['cmd'], fields)
                reply = {'id': request['id'], 'ok': True, 'result': result}
            except Exception as error:
                reply = {'id': request['id'], 'ok': False, 'error': {'code': 'NITRO_LAB_REFUSAL',
                    'detail': str(error)[:1000], 'fatal': False}}
            print(canonical(reply), flush=True)
    finally:
        operator.worker.close()


if __name__ == '__main__':
    sys.exit(main())
