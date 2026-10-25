#!/usr/bin/env python3
"""Real owned L1 settlement + actual Nitro WASM + USDT0/B3 regtest roundtrip."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import time

from settled_common import (HERE, REPO, SettledLabChain, export_settled, policy_binding,
                            SOURCE_MODEL, canonical, private_new, raw_file, expect)
from lab_common import (CHAIN, PROFILE, IMPL_SLOT, ADMIN_SLOT, deploy_usdt0, raw_hex, atomic_json)

_spec = importlib.util.spec_from_file_location('b3_existing_nitro_roundtrip', REPO / 'contrib/flowmesh2/nitro_regtest/run_roundtrip.py')
existing = importlib.util.module_from_spec(_spec); _spec.loader.exec_module(existing)


class SettledRoundtrip(existing.NitroRoundtrip):
    def launch(self, item):
        item['process'] = self.start([sys.executable, str(HERE / 'settled_operator.py'), '--config', str(item['config_path'])],
                                     item['directory'] / 'wrapper.stderr.log', json_pipe=True)
        return item['process']

    def setup(self):
        self.stage('start four B3 regtest peers and private settlement-gated seats')
        self.start_b3()
        self.client = self.start([str(self.worker_path)], self.evidence / 'client-helper.stderr.log', json_pipe=True)
        self.chain = SettledLabChain(self.args.runtime, self.tools, self.evidence / 'fixture-transactions', self.source)
        manifest_path = self.evidence / 'deployment.json'
        baseline_path = self.evidence / 'owned-baseline-policy.json'
        for index in range(4):
            directory = self.evidence / ('operator-' + str(index)); directory.mkdir(mode=0o700)
            config = {'directory': str(directory), 'worker': str(self.worker_path),
                      'checker': str(Path(self.args.native_checker).resolve(strict=True)),
                      'runtime': str(Path(self.args.runtime).resolve(strict=True)), 'tools': str(self.tools),
                      'source_root': str(self.source), 'manifest': str(manifest_path), 'baseline_policy': str(baseline_path),
                      'b3_rpc_url': self.b3[index]['rpc'].url, 'b3_cookie': str(self.b3[index]['rpc'].cookie_file)}
            config_path = directory / 'private-config.json'; private_new(config_path, config)
            item = {'directory': directory, 'config_path': config_path}
            self.launch(item); item['public'] = item['process'].request('bootstrap'); self.operators.append(item)
        self.operators.sort(key=lambda item: item['public']['public_key'])
        self.stage('deploy genuine fresh USDT0 and local custody contracts on full rollup')
        self.token, self.implementation, self.binding = deploy_usdt0(self.chain)
        profile = self.client.request('nitro_lab_profile', nitro_lab=self.binding)
        self.quote = raw_hex(profile['quote_asset']); expect(self.quote not in existing.ASSETS.values(), 'quote asset collision')
        assets = {**existing.ASSETS, 'Q': self.quote}
        roster = [{'public_key': item['public']['public_key'], 'pop': item['public']['pop']} for item in self.operators]
        addresses = sorted('0x' + raw_hex(item['public']['evm_address'], 20) for item in self.operators)
        for name, asset in assets.items():
            token = self.token if name == 'Q' else self.chain.deploy(self.artifact('RebirthNitroLabToken'),
                self.cast.abi_encode('f(string,string)', 'Owned settled Nitro synthetic ' + name, 'LAB' + name[-1]))
            instance = self.cast.keccak('Owned settled Nitro lab ' + os.urandom(32).hex() + ' ' + name)
            constructor = self.cast.abi_encode('f(address,bytes32,bytes32,address[4],(uint256,uint64,uint64,uint64))',
                token, '0x' + asset, instance, '[' + ','.join(addresses) + ']', '(412346,300,604800,3600)')
            vault = self.chain.deploy(self.artifact('RebirthNitroCustodyVaultLab'), constructor)
            expect(self.chain.number(vault, 'LAB_ONLY()') == 1 and self.chain.number(vault, 'LAB_CHAIN_ID()') == CHAIN, 'wrong local custody vault')
            self.routes[name] = {'token': token, 'vault': vault, 'custody_asset': '0x' + asset,
                                'token_code_hash': self.chain.code_hash(token), 'vault_code_hash': self.chain.code_hash(vault), 'instance': instance}
        self.source_point = self.chain.point()
        self.stage('pin full immutable owned deployment policy before test deposits')
        previous = self.chain.read.call('eth_getBlockByNumber', [hex(self.source_point['height'] - 1), False])
        baseline_export = self.evidence / 'baseline-export'
        export_settled(self.args.runtime, baseline_export, previous_height=self.source_point['height'] - 1,
                       previous_hash=previous['hash'], to_height=self.source_point['height'], to_hash=self.source_point['hash'])
        policy = raw_file(baseline_export / 'deployment-policy.json', 4 * 1024 * 1024)
        fd = os.open(baseline_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'wb') as stream:
            stream.write(policy); stream.flush(); os.fsync(stream.fileno())
        self.owned_binding = policy_binding(baseline_path, self.chain)
        manifest = {'format': 'fm2-owned-nitro-lab-deployment/1', 'chain_id': CHAIN, 'mainnet_enabled': False,
                    'profile_name': PROFILE, 'source_model': SOURCE_MODEL, 'owned_settlement': self.owned_binding,
                    'nitro_lab': self.binding, 'roster': roster, 'routes': self.routes, 'custody_addresses': addresses,
                    'source_initial': self.source_point, 'initial_anchor': self.b3[0]['rpc'].anchor()}
        private_new(manifest_path, manifest)
        for item in self.operators:
            item['info'] = item['process'].request('open', create=True)
        infos = self.infos()
        expect(len({x['head_hash'] for x in infos}) == 1 and all(x['assets']['Q'] == self.quote for x in infos), 'private profile states differ')
        self.domain, self.markets = infos[0]['domain'], infos[0]['markets']
        self.clients = {name: self.client.request('evm_keygen') for name in ('alice', 'bob')}
        identities = [self.chain.dev_address, *addresses, *('0x' + raw_hex(x['address'], 20) for x in self.clients.values())]
        expect(len(set(identities)) == len(identities), 'sponsor/client/operator identities overlap')
        private_new(self.evidence / 'private-clients.json', self.clients)

    def proof(self, point):
        if point == self.source_point:
            return self.last_proof
        expect(0 < point['height'] - self.source_point['height'] <= 128, 'bounded source range required')
        targets = {route['vault']: [] for route in self.routes.values()}
        targets.update({route['token']: [] for route in self.routes.values()})
        targets[self.token] = [IMPL_SLOT, ADMIN_SLOT]; targets[self.implementation] = []
        directory = self.evidence / 'exports' / ('range-' + str(self.source_point['height']) + '-' + str(point['height']))
        directory.parent.mkdir(mode=0o700, exist_ok=True)
        manifest = export_settled(self.args.runtime, directory,
            previous_height=self.source_point['height'], previous_hash=self.source_point['hash'],
            to_height=point['height'], to_hash=point['hash'],
            accounts=[{'address': target, 'storage_keys': slots} for target, slots in targets.items()])
        expect(manifest['observed_validator_feed_input'] == [], 'qualification requires L1-only input after recorded cutover')
        self.last_proof = directory / 'history-proof.bin'
        return self.last_proof

    def commit(self, frames, label, reject_two=False):
        result = super().commit(frames, label, reject_two)
        if label == 'four deposits':
            # A previously canonical, settled endpoint still cannot replace the
            # cursor decoded from the original worker's committed range fact.
            item = self.operators[0]
            path = item['directory'] / 'source-current.json'
            original = json.loads(raw_file(path, 65536))
            manifest = json.loads(raw_file(self.evidence / 'deployment.json', 262144))
            item['process'].close()
            atomic_json(path, {**original, 'point': manifest['source_initial']})
            self.launch(item)
            try:
                try:
                    item['process'].request('open', create=False)
                except existing.CustodyError:
                    pass
                else:
                    raise existing.CustodyError('tampered source-current endpoint survived original-journal reopen')
            finally:
                item['process'].close()
                atomic_json(path, original)
            self.launch(item); item['process'].request('open', create=False)
            self.assertions.append('original native journal rejects a tampered canonical source cursor on restart; restoring exact evidence resumes same state')
        return result

    def run(self):
        report = super().run()
        checks = []
        for seat, item in enumerate(self.operators):
            for path in sorted((item['directory'] / 'native-proof-checks').glob('*.json')):
                value = json.loads(raw_file(path, 8 * 1024 * 1024))
                expect(value['owned_settlement_verified'] is True and value['public_arbitrum_authority'] is False,
                       'retained native owned settlement qualification missing')
                checks.append({'seat': seat, 'wire_sha256': path.stem, 'check': value})
        report.update({'format': 'b3-owned-l1-settled-nitro-usdt0-roundtrip/1', 'source_model': SOURCE_MODEL,
                       'owned_settlement': self.owned_binding, 'owned_l1_settlement_verified': True,
                       'source_checkpoint_trust': 'separately pinned owned Geth-dev canonical L1 checkpoints',
                       'owned_validator_l1_only_after_cutover': True, 'owned_validator_from_genesis_replay': False,
                       'native_owned_settlement_checks': checks, 'public_arbitrum_authority': False,
                       'public_ethereum_pos_verified': False, 'public_arbitrum_settlement_verified': False,
                       'provider_free_owned_end_to_end_qualified': True,
                       'rpc_usage': 'owned loopback node APIs only; no external RPC provider',
                       'owned_validator_wasm_coverage_observed': True,
                       'execution_verification_scope': 'native checker authenticates owned contract settlement and complete history; actual maintained WASM validator coverage is separately observed on the owned runtime',
                       'mainnet_enabled': False})
        # The inherited standalone lab field has no network scope. The new
        # format reports owned settlement and public PoS settlement separately.
        report.pop('l1_settlement_verified', None)
        atomic_json(self.evidence / 'roundtrip-report.json', report)
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ('source-root', 'worker', 'native-checker', 'runtime', 'tools', 'solc', 'evidence-dir', 'b3coind', 'b3coin-cli'):
        parser.add_argument('--' + field, required=True)
    args = parser.parse_args(); args.agreement = 'pbft-fixed-view'; args.custody = 'fixture'
    fixture = None
    try:
        fixture = SettledRoundtrip(args); fixture.run()
        print(canonical({'passed': True, 'mainnet_enabled': False, 'report': str(fixture.evidence / 'roundtrip-report.json')}))
        return 0
    except Exception as error:
        if fixture is not None:
            atomic_json(fixture.evidence / 'roundtrip-failure.json', {'passed': False, 'mainnet_enabled': False,
                'stage': getattr(fixture, 'stage_name', 'initialization'), 'detail': str(error)[:2000]})
        print('owned settlement roundtrip failed: ' + str(error)[:2000], file=sys.stderr)
        return 1
    finally:
        if fixture is not None:
            fixture.close()


if __name__ == '__main__':
    sys.exit(main())
