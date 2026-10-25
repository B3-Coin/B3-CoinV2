#!/usr/bin/env python3
"""Private owned L1-settled operator: native raw proof gate before every vote."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys

from settled_common import (SettledLabChain, SOURCE_MODEL, retain_bundle, policy_binding, REPO,
                            private_new, raw_file, canonical, expect)
from lab_common import CHAIN, PROFILE, IMPL_SLOT, JsonProcess, raw_hex
from lab_operator import LabOperator
from local_process import B3Rpc


class SettledLabOperator(LabOperator):
    def __init__(self, config):
        self.config = config; self.directory = Path(config['directory']).resolve(strict=True)
        self.worker = JsonProcess([config['worker']], self.directory / 'native.stderr.log')
        self.chain = SettledLabChain(config['runtime'], config['tools'], self.directory / 'chain-io', config['source_root'])
        self.b3 = B3Rpc(config['b3_rpc_url'], config['b3_cookie'])
        path = self.directory / 'private-identity.json'
        if path.exists():
            self.secret = json.loads(raw_file(path, 16384))
        else:
            self.secret = {'ikm': os.urandom(32).hex(), 'ecdsa': self.worker.request('evm_keygen')}
            private_new(path, self.secret)
        key = self.worker.request('key_info', ikm=self.secret['ikm'])
        self.public = {**key, 'evm_address': self.secret['ecdsa']['address']}
        self.manifest = self.current = self.pending = None; self.opened = False

    def open(self, create):
        manifest = json.loads(raw_file(self.config['manifest'], 262144))
        expect(manifest.get('source_model') == SOURCE_MODEL, 'full settlement source model required')
        expect(manifest['owned_settlement'] == policy_binding(self.config['baseline_policy'], self.chain),
               'immutable owned policy/genesis binding changed')
        # Keep an independent private copy before the manifest enters the native
        # deployment domain. Never replace this policy on subsequent exports.
        policy = raw_file(self.config['baseline_policy'], 4 * 1024 * 1024)
        pinned = self.directory / 'owned-baseline-policy.json'
        if pinned.exists():
            expect(raw_file(pinned, 4 * 1024 * 1024) == policy, 'private baseline policy changed')
        else:
            fd = os.open(pinned, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
            with os.fdopen(fd, 'wb') as stream:
                stream.write(policy); stream.flush(); os.fsync(stream.fileno())
        return super().open(create)

    def observe(self, fields):
        expect(set(fields) <= {'proof', 'to', 'anchor', 'expected_events'}, 'unknown full settlement source request fields')
        if fields['to'] != self.current['point']:
            wire = raw_file(fields['proof'], 32 * 1024 * 1024)
            digest = hashlib.sha256(wire).hexdigest()
            original = Path(fields['proof']).parent
            manifest = json.loads(raw_file(original / 'manifest.json', 4 * 1024 * 1024))
            expect(manifest.get('format') == 'b3-owned-nitro-settled-export/1' and
                   manifest['history_proof_sha256'] == digest and manifest['mainnet_enabled'] is False,
                   'full raw settlement bundle required')
            checkpoint = self.chain.checkpoint(manifest['l1']['height'])
            expect(checkpoint['hash'] == manifest['l1']['hash'], 'export L1 header differs from independent owned checkpoint')
            bundle = self.directory / 'source-bundles' / digest
            retain_bundle(original, bundle)
            binding = {'bundle': str(bundle), 'checkpoint': checkpoint,
                       'manifest_sha256': hashlib.sha256(raw_file(bundle / 'manifest.json', 4 * 1024 * 1024)).hexdigest(),
                       'policy_sha256': self.manifest['owned_settlement']['baseline_policy_sha256']}
            private_new(self.directory / 'bundle-index' / (digest + '.json'), binding)
        return super().observe(fields)

    def guard(self):
        info = self.worker.request('info')
        if info['height'] > 0:
            expect(info.get('source_checkpoint') == self.current['point'],
                   'source cursor differs from native committed SourceRangeFact')
        else:
            expect(self.current['point'] == self.manifest['source_initial'],
                   'initial cursor differs from native domain-bound deployment')
        return super().guard()

    def recheck(self, context):
        self.identity(); self.chain.check_point(context['previous']); self.chain.check_point(context['to'])
        wire = raw_file(context['proof'], 32 * 1024 * 1024)
        digest = hashlib.sha256(wire).hexdigest()
        expect(digest == context['sha256'], 'retained history proof changed')
        binding = json.loads(raw_file(self.directory / 'bundle-index' / (digest + '.json'), 65536))
        bundle = Path(binding['bundle'])
        expect(hashlib.sha256(raw_file(bundle / 'manifest.json', 4 * 1024 * 1024)).hexdigest() == binding['manifest_sha256'],
               'retained full bundle manifest changed')
        expect(raw_file(bundle / 'history-proof.bin', 32 * 1024 * 1024) == wire, 'retained bundle history differs')
        policy = self.directory / 'owned-baseline-policy.json'
        expect(hashlib.sha256(raw_file(policy, 4 * 1024 * 1024)).hexdigest() == binding['policy_sha256'] ==
               self.manifest['owned_settlement']['baseline_policy_sha256'], 'immutable baseline policy digest changed')
        checkpoint = binding['checkpoint']
        expect(self.chain.checkpoint(checkpoint['height']) == checkpoint, 'owned L1 checkpoint no longer canonical')
        command = [self.config['checker'], '--regtest', '--export', str(bundle), '--trusted-policy', str(policy),
                   '--trusted-policy-sha256', '0x' + binding['policy_sha256'],
                   '--trusted-l1-head', checkpoint['hash'], '--l1-genesis', self.chain.l1_genesis,
                   '--l2-genesis', self.chain.genesis, '--previous-height', str(context['previous']['height']),
                   '--previous-hash', context['previous']['hash'], '--to-height', str(context['to']['height']),
                   '--to-hash', context['to']['hash']]
        for route in self.manifest['routes'].values():
            command.extend(['--vault', route['vault']])
        checked = subprocess.run(command, capture_output=True, timeout=30)
        expect(checked.returncode == 0 and len(checked.stdout) <= 8 * 1024 * 1024, 'native owned settlement/history gate refused: ' + checked.stderr.decode(errors='replace')[-500:])
        reply = json.loads(checked.stdout)
        expect(reply['result'] == 'OK' and reply['commitments_match'] is True and
               reply['owned_settlement_verified'] is True and reply['mainnet_enabled'] is False and
               reply['public_arbitrum_authority'] is False and reply['execution_verified'] is False,
               'native owned settlement gate scope/result mismatch')
        expect(reply['trusted_policy_sha256'] == '0x' + binding['policy_sha256'], 'native policy digest binding differs')
        accounts = {x['address'].lower(): x for x in reply['newest_accounts']}
        for route in self.manifest['routes'].values():
            for target, code in ((route['vault'], route['vault_code_hash']), (route['token'], route['token_code_hash'])):
                expect(target in accounts and accounts[target]['code_hash'] == code, 'native endpoint proof omits route code')
        lab = self.manifest['nitro_lab']; proxy = accounts.get('0x' + lab['quote_token'])
        implementation = accounts.get('0x' + lab['quote_implementation'])
        expect(proxy is not None and implementation is not None and
               implementation['code_hash'] == '0x' + lab['quote_implementation_code_hash'], 'native endpoint proof omits USDT0 implementation')
        slots = {x['slot']: x['value'] for x in proxy['storage']}
        expect(slots.get(IMPL_SLOT) == '0x' + '00' * 12 + lab['quote_implementation'], 'native endpoint proof omits exact proxy implementation')
        private_new(self.directory / 'native-proof-checks' / (digest + '.json'), reply)
        self.chain.check_point(context['to'])
        return reply


def main():
    p = argparse.ArgumentParser(description=__doc__); p.add_argument('--config', required=True); a = p.parse_args()
    operator = SettledLabOperator(json.loads(raw_file(a.config, 65536)))
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
                fields = {k: v for k, v in request.items() if k not in ('id', 'cmd')}
                result = operator.handle(request['cmd'], fields)
                reply = {'id': request['id'], 'ok': True, 'result': result}
            except Exception as error:
                reply = {'id': request['id'], 'ok': False, 'error': {'code': 'OWNED_SETTLEMENT_REFUSAL', 'detail': str(error)[:1500], 'fatal': False}}
            print(canonical(reply), flush=True)
    finally:
        operator.worker.close()


if __name__ == '__main__':
    sys.exit(main())
