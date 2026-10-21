#!/usr/bin/env python3
"""Owned Nitro412346 / fresh genuine USDT0 / four direct-quorum B3 lab.

This coordinator fixture trusts its owned local full node for checkpoints. It
does not authenticate an ArbitrumOne assertion or enable mainnet/source credit.
"""
import argparse
from dataclasses import replace
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import time

from lab_common import (HERE, REPO, CHAIN, PROFILE, IMPL_SLOT, ADMIN_SLOT, WITHDRAWAL_ABI,
    NitroLabChain, deploy_usdt0, export_block, canonical, private_new, atomic_json,
    raw_hex, hex_value, expect, CustodyError, withdrawal_tuple, owned_b3_control)
from proof_bundle import (Assertion, Proof, DirectoryObjects, assemble_raw_block,
                          assemble_proof, encode_proof, decode_proof)
from operator_wrapper import header_fields
from qualify_regtest import sha256

spec = importlib.util.spec_from_file_location('legacy_regtest_roundtrip', HERE.parent / 'regtest/run_roundtrip.py')
legacy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(legacy)

ASSETS = {'TOKENA': '9f84daf3e5e62cba57a6cc6dc2e02a3ac8fbae94d39cf085a3884f87e1533265',
          'TOKENB': 'fc416025aa6b00dd141d6dd9911cbfa7f0978707da4bfc3f0a4c7ca0c1c19283'}

def source_manifest(source):
    # Root is needed only for the owned Docker fixture. Trust this one explicit
    # repository for this read command; do not change global Git configuration.
    names = subprocess.check_output(['git', '-c', 'safe.directory=' + str(source), 'ls-files',
        '--cached', '--others', '--exclude-standard', '-z'], cwd=source).split(b'\0')
    files = {os.fsdecode(name): sha256(source / os.fsdecode(name)) if (source / os.fsdecode(name)).is_file() else None
             for name in sorted(set(names)) if name}
    return {'sha256': hashlib.sha256(canonical(files).encode()).hexdigest(), 'files': files}


class NitroRoundtrip(legacy.Roundtrip):
    def stage(self, name):
        print(canonical({'stage': name, 'mainnet_enabled': False}), flush=True)
        self.stage_name = name

    def build(self):
        self.stage('compile separate chain412346 lab contracts')
        command = [str(self.tools / 'forge'), 'build', '--root', str(self.source / 'contracts'), '--offline',
                   '--use', str(Path(self.args.solc).resolve(strict=True)), '--evm-version', 'cancun',
                   '--out', str(self.evidence / 'contracts-out'), '--cache-path', str(self.evidence / 'contracts-cache'),
                   'labnitro/RebirthNitroLabToken.sol', 'labnitro/RebirthNitroCustodyVaultLab.sol']
        with (self.evidence / 'forge-build.log').open('w') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, timeout=600, check=True)

    def artifact(self, name):
        value = json.loads((self.evidence / 'contracts-out' / (name + '.sol') / (name + '.json')).read_text())
        return hex_value(value['bytecode']['object'])

    def launch(self, item):
        item['process'] = self.start([sys.executable, str(HERE / 'lab_operator.py'), '--config', str(item['config_path'])],
                                     item['directory'] / 'wrapper.stderr.log', json_pipe=True)
        return item['process']

    def setup(self):
        self.stage('start four real B3 regtest peers')
        self.start_b3()
        self.client = self.start([str(self.worker_path)], self.evidence / 'client-helper.stderr.log', json_pipe=True)
        self.chain = NitroLabChain(self.args.runtime, self.tools, self.evidence / 'fixture-transactions', self.source)
        self.stage('bootstrap four private lab seats')
        manifest_path = self.evidence / 'deployment.json'
        for index in range(4):
            directory = self.evidence / ('operator-' + str(index)); directory.mkdir(mode=0o700)
            config = {'directory': str(directory), 'worker': str(self.worker_path), 'checker': str(Path(self.args.native_checker).resolve(strict=True)),
                'runtime': str(Path(self.args.runtime).resolve(strict=True)), 'tools': str(self.tools), 'source_root': str(self.source),
                'manifest': str(manifest_path), 'b3_rpc_url': self.b3[index]['rpc'].url,
                'b3_cookie': str(self.b3[index]['rpc'].cookie_file)}
            config_path = directory / 'private-config.json'; private_new(config_path, config)
            item = {'directory': directory, 'config_path': config_path}
            self.launch(item); item['public'] = item['process'].request('bootstrap'); self.operators.append(item)
        self.operators.sort(key=lambda item: item['public']['public_key'])
        self.stage('deploy byte-identical fresh USDT0 with normal constructor initialization')
        self.token, self.implementation, self.binding = deploy_usdt0(self.chain)
        profile = self.client.request('nitro_lab_profile', nitro_lab=self.binding)
        self.quote = raw_hex(profile['quote_asset'])
        expect(self.quote not in ASSETS.values(), 'derived quote collides with synthetic inventory')
        assets = {**ASSETS, 'Q': self.quote}
        roster = [{'public_key': item['public']['public_key'], 'pop': item['public']['pop']}
                  for item in self.operators]
        addresses = sorted('0x' + raw_hex(item['public']['evm_address'], 20) for item in self.operators)
        for name, asset in assets.items():
            token = self.token if name == 'Q' else self.chain.deploy(self.artifact('RebirthNitroLabToken'),
                self.cast.abi_encode('f(string,string)', 'Owned Nitro synthetic ' + name, 'LAB' + name[-1]))
            instance = self.cast.keccak('Owned Nitro lab ' + os.urandom(32).hex() + ' ' + name)
            constructor = self.cast.abi_encode('f(address,bytes32,bytes32,address[4],(uint256,uint64,uint64,uint64))',
                token, '0x' + asset, instance, '[' + ','.join(addresses) + ']', '(412346,300,604800,3600)')
            vault = self.chain.deploy(self.artifact('RebirthNitroCustodyVaultLab'), constructor)
            expect(self.chain.number(vault, 'LAB_ONLY()') == 1 and self.chain.number(vault, 'LAB_CHAIN_ID()') == CHAIN,
                   'wrong lab vault')
            self.routes[name] = {'token': token, 'vault': vault, 'custody_asset': '0x' + asset,
                'token_code_hash': self.chain.code_hash(token), 'vault_code_hash': self.chain.code_hash(vault), 'instance': instance}
        self.source_point = self.chain.point()
        manifest = {'format': 'fm2-owned-nitro-lab-deployment/1', 'chain_id': CHAIN, 'mainnet_enabled': False,
            'profile_name': PROFILE, 'nitro_lab': self.binding, 'roster': roster, 'routes': self.routes,
            'custody_addresses': addresses,
            'source_initial': self.source_point, 'initial_anchor': self.b3[0]['rpc'].anchor()}
        private_new(manifest_path, manifest)
        for item in self.operators:
            item['info'] = item['process'].request('open', create=True)
        infos = self.infos()
        expect(len({value['head_hash'] for value in infos}) == 1 and
               all(value['assets']['Q'] == self.quote for value in infos), 'lab profile/quote differs across private seats')
        self.domain, self.markets = infos[0]['domain'], infos[0]['markets']
        self.stage('create private client identities')
        self.clients = {name: self.client.request('evm_keygen') for name in ('alice', 'bob')}
        identities = [self.chain.dev_address, *addresses, *('0x' + raw_hex(value['address'], 20) for value in self.clients.values())]
        expect(len(set(identities)) == len(identities), 'sponsor/client/operator custody identities overlap')
        private_new(self.evidence / 'private-clients.json', self.clients)

    def infos(self):
        return [item['process'].request('info') for item in self.operators]

    def action(self, name, who, nonce, kind, payload, ref_slot=0):
        value = self.client.request('client_action', seed=self.clients[who]['seed'], domain=self.domain, nonce=nonce,
            ref_slot=ref_slot, max_delay=32, kind=kind, payload=payload, profile_name=PROFILE, nitro_lab=self.binding)
        private_new(self.evidence / 'client-actions' / (name + '.json'), value)
        return value

    def sponsor(self, who, account, amount):
        wallet = '0x' + raw_hex(self.clients[who]['address'], 20)
        q = self.routes['Q']; token, vault = q['token'], q['vault']
        artifact = json.loads((HERE / 'usdt0_bytecode.json').read_text())
        self.chain.bind_usdt0(token, self.implementation, artifact)
        expect(self.chain.read.call('eth_getCode', [wallet, 'latest']) == '0x' and
               int(self.chain.read.call('eth_getBalance', [wallet, 'latest']), 16) == 0, 'source must be fresh code-empty zero-ETH EOA')
        self.chain.invoke(token, 'mint(address,uint256)', wallet, amount)
        expect(self.chain.number(token, 'allowance(address,address)', wallet, vault) == 0, 'sponsored source approved token')
        salt = '0x' + os.urandom(32).hex()
        nonce = self.chain.call(vault, 'depositAuthorizationNonce(address,uint256,bytes32,bytes32)', wallet, amount,
                                '0x' + raw_hex(account), salt)
        now = int(self.chain.read.call('eth_getBlockByNumber', ['latest', False])['timestamp'], 16)
        after, before = now - 1, now + 600
        domain = self.cast.keccak(self.cast.abi_encode('f(bytes32,bytes32,bytes32,uint256,address)',
            self.cast.keccak('EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)'),
            self.cast.keccak('USD₮0'), self.cast.keccak('1'), CHAIN, token))
        expect(self.chain.call(token, 'DOMAIN_SEPARATOR()') == domain, 'actual USDT0 EIP712 domain differs')
        struct_hash = self.cast.keccak(self.cast.abi_encode('f(bytes32,address,address,uint256,uint256,uint256,bytes32)',
            self.cast.keccak('ReceiveWithAuthorization(address from,address to,uint256 value,uint256 validAfter,uint256 validBefore,bytes32 nonce)'),
            wallet, vault, amount, after, before, nonce))
        digest = self.cast.keccak('0x1901' + domain[2:] + struct_hash[2:])
        signature = raw_hex(self.client.request('evm_sign', seed=self.clients[who]['seed'], digest=raw_hex(digest))['signature'], 65)
        receipt = self.chain.invoke(vault, 'depositWithAuthorization(address,uint256,bytes32,bytes32,uint256,uint256,uint8,bytes32,bytes32)',
            wallet, amount, '0x' + raw_hex(account), salt, after, before, int(signature[128:130], 16), '0x' + signature[:64], '0x' + signature[64:128])
        expect(self.chain.number(token, 'allowance(address,address)', wallet, vault) == 0 and
            self.chain.read.call('eth_getCode', [wallet, 'latest']) == '0x' and
            int(self.chain.read.call('eth_getBalance', [wallet, 'latest']), 16) == 0, 'sponsored deposit altered source ETH/code/allowance')
        return {'source': wallet, 'amount_raw': str(amount), 'source_wallet_profile': 'code-empty-eoa-vrs-only',
                'source_eth_before': '0', 'source_eth_after': '0', 'approve_transactions': 0,
                'sponsor': self.chain.dev_address, 'transaction_hash': receipt['transactionHash'],
                'sponsor_gas_used': receipt['gasUsed'], 'sponsor_effective_gas_price': receipt.get('effectiveGasPrice')}

    def proof(self, point):
        if point == self.source_point:
            return self.last_proof
        expect(0 < point['height'] - self.source_point['height'] <= 128, 'proof acquisition range exceeds lab bound')
        blocks, newest_accounts = [], ()
        targets = {route['vault']: [] for route in self.routes.values()}
        targets.update({route['token']: [] for route in self.routes.values()})
        targets[self.token] = [IMPL_SLOT, ADMIN_SLOT]; targets[self.implementation] = []
        for height in range(point['height'], self.source_point['height'], -1):
            block = self.chain.read.call('eth_getBlockByNumber', [hex(height), False])
            directory = self.evidence / 'exports' / ('block-' + str(height))
            accounts = [{'address': target, 'storage_keys': slots} for target, slots in targets.items()] if height == point['height'] else []
            manifest = export_block(self.source, self.args.runtime, directory, block['hash'], accounts)
            source = manifest['source']; objects = DirectoryObjects(directory / 'objects', source)
            data = {key: value for key, value in manifest['block'].items() if key != 'raw_block_sha256'}
            blocks.append(assemble_raw_block(objects, data))
            if height == point['height']:
                envelope = {'format': 'b3-arbitrum-proof-manifest/1', 'mainnet_enabled': False, 'writes_enabled': False,
                    'source': source, 'assertion': {'parent_assertion': '0x' + '00' * 32, 'block_hash': '0x' + '00' * 32,
                    'send_root': '0x' + '00' * 32, 'inbox_position': 0, 'position_in_message': 0, 'machine_status': 1,
                    'end_history_root': '0x' + '00' * 32, 'inbox_accumulator': '0x' + '00' * 32}, 'l1_accounts': [],
                    'blocks': [data], 'confirmed_accounts': manifest['accounts'], 'historical_accounts': []}
                newest_accounts = assemble_proof(objects, envelope).confirmed_accounts
        wire = encode_proof(Proof(Assertion(), (), tuple(blocks), newest_accounts, ()))
        path = self.evidence / ('proof-' + str(point['height']) + '.bin')
        path.write_bytes(wire); path.chmod(0o600)
        self.last_proof = path
        return path

    def observe(self, point=None, *, negatives=False):
        if point is None:
            # An ordinary owned-chain transaction creates a real successor.
            # This does not replace the real 300-second custody waiting period.
            self.chain.send(self.chain.dev_address)
        point = self.chain.point() if point is None else point
        proof = self.proof(point)
        anchor = self.mine_b3()
        if negatives:
            original = decode_proof(proof.read_bytes()); block = original.blocks[0]
            changed = replace(block.receipts[0], encoded=block.receipts[0].encoded + b'\x00')
            bad = replace(original, blocks=(replace(block, receipts=(changed,) + block.receipts[1:]),) + original.blocks[1:])
            bad_path = self.evidence / 'tampered-proof.bin'; bad_path.write_bytes(encode_proof(bad)); bad_path.chmod(0o600)
            before = [(info['height'], info['signer_seq']) for info in self.infos()]
            for item in self.operators:
                for fields in ({'proof': str(bad_path)}, {'proof': str(proof), 'expected_events': []}):
                    try:
                        item['process'].request('observe_bundle', to=point, anchor=anchor, **fields)
                    except CustodyError:
                        pass
                    else:
                        raise CustodyError('tampered proof/omitted events accepted by private seat')
                latest = self.chain.read.call('eth_getBlockByNumber', ['latest', False])
                try:
                    item['process'].request('observe_bundle', proof=str(proof), anchor=anchor,
                        to={'height': int(latest['number'], 16), 'hash': latest['hash'], 'minimum_confirmations': 1})
                except CustodyError:
                    pass
                else:
                    raise CustodyError('zero-successor tip accepted as confirmed source')
            expect(before == [(info['height'], info['signer_seq']) for info in self.infos()], 'refused proof moved native balances/signing')
            self.assertions.append('all four private seats reject altered proof and omitted real events before observation/voting')
        replies = [item['process'].request('observe_bundle', proof=str(proof), to=point, anchor=anchor) for item in self.operators]
        expect(len({canonical(value) for value in replies}) == 1, 'independent proof-derived observations differ')
        self.pending_point = point
        if negatives:
            candidate = self.operators[0]['process'].request('candidate', actions=[])
            affected = next(item for item in self.operators if item['directory'].name == 'operator-0')
            before = affected['process'].request('info')['signer_seq']
            block_hash = bytes.fromhex(anchor['hash'])[::-1].hex()
            owned_b3_control(self.cli_path, self.b3[0], 'invalidateblock', block_hash)
            try:
                try:
                    affected['process'].request('vote', header=candidate['header'], body=candidate['body'])
                except CustodyError:
                    pass
                else:
                    raise CustodyError('removed pending B3 anchor permitted a fresh native vote')
                expect(affected['process'].request('info')['signer_seq'] == before, 'B3 guard refusal advanced signing journal')
            finally:
                owned_b3_control(self.cli_path, self.b3[0], 'reconsiderblock', block_hash)
            self.b3[0]['rpc'].verify_anchor(anchor)
            self.assertions.append('removed pending B3 anchor and unconfirmed Nitro tip refuse without signing progress')
        return replies[0]

    def commit(self, frames, label, reject_two=False):
        begin = time.monotonic_ns()
        for item in self.operators:
            for frame in frames:
                item['process'].request('submit', frame=frame)
        candidate = self.operators[0]['process'].request('candidate', actions=frames)
        proposal = {key: candidate[key] for key in ('header', 'body')}
        votes = [item['process'].request('vote', **proposal) for item in self.operators]
        if reject_two:
            try:
                self.operators[0]['process'].request('cert_from_votes', header=candidate['header'], votes=votes[:2])
            except CustodyError:
                pass
            else:
                raise CustodyError('two-seat certificate admitted')
        certificate = self.operators[0]['process'].request('cert_from_votes', header=candidate['header'], votes=votes[:3])['certificate']
        request = {**proposal, 'certificate': certificate}
        replies = [item['process'].request('commit', **request) for item in self.operators]
        expect(len({value['head_hash'] for value in replies}) == 1, 'four independent native states diverged')
        elapsed = (time.monotonic_ns() - begin) / 1_000_000
        self.latencies.append({'label': label, 'milliseconds': elapsed, 'client_actions': len(frames)})
        self.source_point = self.pending_point
        private_new(self.evidence / 'certified-slots' / (str(replies[0]['height']) + '.json'), request)
        return request

    def balances(self, account):
        results = [item['process'].request('account', account=raw_hex(account)) for item in self.operators]
        expect(all(value.get('found') and value.get('verified') is True for value in results), 'native account inclusion proof refused')
        expect(len({canonical(value['balances']) for value in results}) == 1, 'native balances differ')
        return {raw_hex(value['asset']): (int(value['available']), int(value['reserved'])) for value in results[0]['balances']}

    def run(self):
        before_hashes = {'source': source_manifest(self.source)['sha256'], 'worker': sha256(self.worker_path),
                         'checker': sha256(Path(self.args.native_checker))}
        self.build(); self.setup()
        asks, bids = [], []
        for index, (market_name, price, lots) in enumerate((('TOKENA_Q', 200, 3), ('TOKENB_Q', 300, 2))):
            market = self.markets[market_name]
            expect(int(market['base_atoms_per_lot']) == 1_000_000 and int(market['quote_atoms_per_tick']) == 10_000, 'wrong lot/tick economics')
            payload = {'market': market['id'], 'side': 'SELL', 'tif': 'GTC', 'price_ticks': price, 'lots': lots}
            asks.append(self.action('alice-ask-' + str(index), 'alice', index + 1, 'PLACE', payload))
            bids.append(self.action('bob-bid-' + str(index), 'bob', index + 1, 'PLACE', {**payload, 'side': 'BUY', 'tif': 'IOC'}))
        alice, bob = asks[0]['account'], bids[0]['account']
        self.stage('deposit synthetic inventory and sponsor genuine USDT0 zero-ETH deposits')
        for name in ('TOKENA', 'TOKENB'):
            route = self.routes[name]
            self.chain.invoke(route['token'], 'mint(address,uint256)', self.chain.dev_address, 10_000_000)
            self.chain.invoke(route['token'], 'approve(address,uint256)', route['vault'], 10_000_000)
            self.chain.invoke(route['vault'], 'deposit(uint256,bytes32)', 10_000_000, '0x' + raw_hex(alice))
        sponsored = [self.sponsor('alice', alice, 10_000_000), self.sponsor('bob', bob, 100_000_000)]
        self.observe(negatives=True); self.commit([], 'four deposits', reject_two=True)
        self.stage('settle two native maker/taker markets')
        self.observe(); self.commit([value['frame'] for value in asks], 'two maker orders')
        self.observe(); self.commit([value['frame'] for value in bids], 'two taker fills')
        expect(self.balances(alice)[self.quote] == (21_998_800, 0) and self.balances(bob)[self.quote] == (88_000_000, 0), 'native stablecoin trade balances differ')
        self.stage('commit stablecoin withdrawal and restart original private seat')
        recipient = self.cast.test_address(20002)
        withdrawal = self.action('alice-withdraw', 'alice', 3, 'WITHDRAW', {'asset': self.quote, 'amount': 5_000_000,
            'recipient': raw_hex(recipient, 20)}, self.infos()[0]['height'])
        self.observe(); self.commit([withdrawal['frame']], 'withdrawal obligation')
        item = self.operators[3]; original = item['public']; item['process'].close(); self.launch(item)
        expect(item['process'].request('bootstrap') == original, 'restart changed private identity')
        item['process'].request('open', create=False)
        proofs = [entry['process'].request('withdrawal_authority', withdrawal_id=withdrawal['id']) for entry in self.operators]
        expect(all(value.get('verified') is True and value.get('current_unpaid') is True for value in proofs), 'independent original claim proof refused')
        record = proofs[0]['record']; head = header_fields(proofs[0]['header'])
        now = int(self.chain.read.call('eth_getBlockByNumber', ['latest', False])['timestamp'], 16)
        value = {'id': '0x' + raw_hex(record['id']), 'account': '0x' + raw_hex(record['account']), 'asset': '0x' + self.quote,
            'amount': str(record['amount']), 'recipient': '0x' + raw_hex(record['recipient'], 20), 'slot': record['slot'],
            'stateRoot': '0x' + head['state_root'], 'validUntil': now + 600}
        for entry in self.operators:
            try:
                entry['process'].request('sign_claim', value={**value, 'amount': '5000001'})
            except CustodyError:
                pass
            else:
                raise CustodyError('changed native principal received custody signature')
        signatures = [entry['process'].request('sign_claim', value=value) for entry in self.operators]
        signatures.sort(key=lambda signature: signature['address'])
        expect(len({signature['address'] for signature in signatures}) == 4, 'custody signer identity duplicated')
        q = self.routes['Q']; before = self.chain.number(self.token, 'balanceOf(address)', recipient)
        queued = self.chain.invoke(q['vault'], 'requestWithdrawal(' + WITHDRAWAL_ABI + ',bytes[])', withdrawal_tuple(value),
                                   '[' + ','.join('0x' + raw_hex(signature['signature'], 65) for signature in signatures[:3]) + ']')
        status = self.chain.call(q['vault'], 'withdrawals(bytes32)', value['id'])
        words = [int(status[i:i+64], 16) for i in range(2, len(status), 64)]
        digest = self.chain.call(q['vault'], 'withdrawalDigest(' + WITHDRAWAL_ABI + ')', withdrawal_tuple(value))
        expect(len(words) == 5 and words[0] == int(digest, 16) and words[1] == int(value['recipient'], 16) and
               words[2] == 5_000_000 and words[4] == 0, 'accepted external claim differs or is already paid')
        release = words[3]
        expect(release >= int(self.chain.read.call('eth_getBlockByNumber', [queued['blockNumber'], False])['timestamp'], 16) + 300,
               'real lab challenge interval shortened')
        self.stage('wait real 300-second custody challenge interval; no time override')
        waited = time.monotonic()
        while time.time() <= release + 1 or time.monotonic() - waited < 300:
            time.sleep(min(15, max(1, release + 2 - time.time())))
            print(canonical({'stage': 'real custody wait', 'remaining_seconds': max(0, int(release + 2 - time.time()))}), flush=True)
        payout = self.chain.invoke(q['vault'], 'finalizeWithdrawal(bytes32)', value['id'])
        wait_elapsed = time.monotonic() - waited
        expect(self.chain.number(self.token, 'balanceOf(address)', recipient) - before == 5_000_000, 'actual USDT0 recipient delta differs')
        self.stage('independently prove payout and reconcile every native liability')
        paid_observation = self.observe(); final_slot = self.commit([], 'canonical paid confirmation')
        expect(any(event['kind'] == 'paid' for event in paid_observation['events']), 'native complete receipts omit actual payout')
        expect(self.balances(alice)[self.quote] == (16_997_800, 0), 'withdrawal principal/service fee differs')
        totals = [entry['process'].request('totals') for entry in self.operators]
        reconciliation = {}
        for name, route in self.routes.items():
            assets = [next(asset for asset in total['assets'] if raw_hex(asset['asset']) == raw_hex(route['custody_asset'])) for total in totals]
            expect(len({canonical(asset) for asset in assets}) == 1, 'operators disagree on complete liabilities')
            asset = assets[0]; actual = self.chain.number(route['token'], 'balanceOf(address)', route['vault'])
            locked = self.chain.number(route['vault'], 'locked()')
            expect(actual == locked == int(asset['total_liabilities']) and int(asset['pending']) == 0 and
                int(asset['protocol_available']) == (2200 if name == 'Q' else 0), 'reserve/liability/stable fee conservation failed')
            reconciliation[name] = {'actual_vault_raw': str(actual), 'locked_raw': str(locked), 'ledger': asset}
        for entry in self.operators:
            retry = entry['process'].request('submit', frame=withdrawal['frame'])
            expect(retry['code'] == 'DUPLICATE' and retry.get('included') is True, 'exact client retry changed obligation')
            expect(entry['process'].request('commit', **final_slot)['duplicate'] is True, 'exact certificate retry reapplied source')
            proof = entry['process'].request('withdrawal', withdrawal_id=withdrawal['id'])
            expect(proof.get('verified') is True and proof['record']['paid'] is True, 'native paid proof failed')
        after_hashes = {'source': source_manifest(self.source)['sha256'], 'worker': sha256(self.worker_path),
                        'checker': sha256(Path(self.args.native_checker))}
        expect(before_hashes == after_hashes, 'source or native binaries changed during lab qualification')
        proof_checks = []
        for seat, entry in enumerate(self.operators):
            checks = []
            for path in sorted((entry['directory'] / 'native-proof-checks').glob('*.json')):
                checked = json.loads(path.read_text())
                expect(checked['result'] == 'OK' and checked['commitments_match'] is True, 'retained native proof report changed')
                checks.append({'wire_sha256': path.stem, 'proof_digest': checked['proof_digest'],
                    'header_count': checked['header_count'], 'receipt_count': checked['receipt_count']})
            expect(checks, 'private seat lacks genuine native proof evidence')
            proof_checks.append({'seat': seat, 'unique_checked_proofs': checks})
        report = {'format': 'b3-owned-nitro-usdt0-roundtrip/1', 'passed': True, 'mainnet_enabled': False,
            'before_sha256': before_hashes, 'after_sha256': after_hashes,
            'independent_native_proof_checks': proof_checks,
            'live_arbitrum_one_qualified': False, 'no_rpc_end_to_end_qualified': False, 'l1_settlement_verified': False,
            'execution_verified': False, 'arbitrum_authority': False, 'external_anchor_required': True,
            'external_rpc_dependency_removed_for_owned_lab': True, 'source_checkpoint_trust': 'owned-local-Nitro-full-node',
            'coordinator_fixture': 'four independent private native workers; direct static-roster quorum3-of4; local coordinator',
            'profile_name': PROFILE, 'chain_id': CHAIN, 'genesis_hash': self.chain.genesis, 'nitro_lab': self.binding,
            'local_artificial_issuer_funding': True, 'external_stablecoin_backing_qualified': False,
            'production_contract_or_delegated_wallets_qualified': False, 'sponsored_deposits': sponsored,
            'stablecoin_fees_raw': {'trading': '1200', 'withdrawal_service': '1000', 'total': '2200'},
            'native_gas': 'public local fixture sponsor pays native gas separately; source users retain zero ETH',
            'real_challenge_seconds': 300, 'observed_wait_seconds': wait_elapsed,
            'withdrawal': {'id': value['id'], 'recipient': recipient, 'amount_raw': '5000000',
                'request_transaction': queued['transactionHash'], 'payout_transaction': payout['transactionHash'],
                'signer_addresses': [signature['address'] for signature in signatures[:3]], 'recipient_delta_raw': '5000000'},
            'reconciliation': reconciliation, 'b3_anchors': [entry['rpc'].anchor() for entry in self.b3],
            'latency': {'scope': 'local direct coordinator client-submit through four durable native replies; no WAN or L1 finality claim',
                'sample_count': len(self.latencies), 'samples': self.latencies}, 'assertions': self.assertions}
        atomic_json(self.evidence / 'roundtrip-report.json', report)
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for field in ('source-root', 'worker', 'native-checker', 'runtime', 'tools', 'solc', 'evidence-dir', 'b3coind', 'b3coin-cli'):
        parser.add_argument('--' + field, required=True)
    args = parser.parse_args(); args.agreement = 'pbft-fixed-view'; args.custody = 'fixture'
    fixture = None
    try:
        fixture = NitroRoundtrip(args); fixture.run()
        print(canonical({'passed': True, 'mainnet_enabled': False, 'report': str(fixture.evidence / 'roundtrip-report.json')}))
        return 0
    except Exception as error:
        if fixture is not None:
            atomic_json(fixture.evidence / 'roundtrip-failure.json', {'passed': False, 'mainnet_enabled': False,
                'stage': getattr(fixture, 'stage_name', 'initialization'), 'detail': str(error)[:2000]})
        print('owned Nitro lab failed: ' + str(error)[:2000], file=sys.stderr)
        return 1
    finally:
        if fixture is not None:
            fixture.close()


if __name__ == '__main__':
    sys.exit(main())
