#!/usr/bin/env python3
"""Actual isolated B3/EVM/operator roundtrip; explicit PBFT mode over local pipes."""
import argparse
from collections import deque
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from custody import (CHAIN_ID, Q_ASSET, SYNTHETIC_ASSETS, TEST_MNEMONIC, Cast, CustodyDriver,
                     CustodyError, LocalRpc, atomic_json, expect, hex_value, q_units)
from local_process import B3Rpc, JsonProcess, MODERN_GENESIS, OwnedProcess, free_ports, raw_hex, wait_until
from operator_wrapper import AGREEMENT_PROFILES, header_fields, private_json
from custody_policy import PROFILE as CUSTODY_V2_PROFILE, LEGACY_PROFILE as CUSTODY_FIXTURE_PROFILE, pin_custody


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'))


def require_canonical_source_refusal(error, command):
    """Accept only the worker's typed, nonfatal canonical-source safety fence."""
    prefix = 'worker refused ' + command + ': '
    refusal = None
    if str(error).startswith(prefix):
        try:
            refusal = json.loads(str(error)[len(prefix):])
        except (TypeError, ValueError):
            pass
    accepted = {
        'source checkpoint is replaced, unavailable or not sufficiently confirmed',
        'retained source range checkpoint removed or replaced; refusing new signing',
        'retained canonical validation checkpoint removed or replaced; refusing new signing',
    }
    if (not isinstance(refusal, dict) or set(refusal) != {'code', 'detail', 'fatal'} or
            refusal['code'] != 'REGTEST_REFUSAL' or refusal['fatal'] is not False or
            refusal['detail'] not in accepted):
        raise CustodyError('removed credited backing requires a typed canonical-source refusal; actual: ' + str(error))
    return refusal['detail']


class Roundtrip:
    def __init__(self, args):
        self.args = args
        self.agreement = getattr(args, 'agreement', 'pbft-fixed-view')
        if self.agreement not in AGREEMENT_PROFILES:
            raise CustodyError('unknown explicitly selected regtest agreement mode')
        self.profile_name = AGREEMENT_PROFILES[self.agreement]
        self.custody_mode = getattr(args, 'custody', 'fixture')
        if self.custody_mode not in ('fixture', 'production'):
            raise CustodyError('unknown explicitly selected custody mode')
        self.custody_profile = CUSTODY_V2_PROFILE if self.custody_mode == 'production' else CUSTODY_FIXTURE_PROFILE
        self.source = Path(args.source_root).resolve(strict=True)
        self.evidence = Path(args.evidence_dir).resolve()
        if self.evidence == self.source or self.source in self.evidence.parents:
            raise CustodyError('all datadirs/evidence/build outputs must be outside source')
        if self.evidence.exists() and any(self.evidence.iterdir()):
            raise CustodyError('roundtrip requires a fresh empty evidence directory; existing signer state is never erased')
        self.evidence.mkdir(parents=True, exist_ok=True)
        self.tools = Path(args.tools).resolve(strict=True)
        self.worker_path = Path(args.worker).resolve(strict=True)
        self.b3_path = Path(args.b3coind).resolve(strict=True)
        self.cli_path = Path(args.b3coin_cli).resolve(strict=True)
        self.cast = Cast(self.tools / 'cast')
        self.processes = []
        self.operators = []
        self.b3 = []
        self.routes = {}
        self.latencies = []
        self.assertions = []

    def event(self, kind, **fields):
        with (self.evidence / 'coordinator-events.jsonl').open('a', encoding='utf-8') as stream:
            stream.write(canonical({'kind': kind, 'monotonic_ns': time.monotonic_ns(), **fields}) + '\n')
            stream.flush()
            os.fsync(stream.fileno())

    def start(self, command, log, json_pipe=False):
        process = JsonProcess(command, log) if json_pipe else OwnedProcess(command, log)
        self.processes.append(process)
        return process

    def build(self):
        vault_source = 'custody/RebirthCustodyVault.sol' if self.custody_mode == 'production' else 'regtest/RebirthRegtestVault.sol'
        command = [str(self.tools / 'forge'), 'build', '--root', str(self.source / 'contracts'), '--offline',
                   '--use', str(Path(self.args.solc).resolve(strict=True)), '--out', str(self.evidence / 'contracts-out'),
                   '--cache-path', str(self.evidence / 'contracts-cache'),
                   'regtest/RebirthRegtestToken.sol', vault_source]
        with (self.evidence / 'forge-build.log').open('w', encoding='utf-8') as log:
            subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=600)

    def start_b3(self):
        ports = free_ports(8)
        for index in range(4):
            directory = self.evidence / ('b3-' + str(index))
            directory.mkdir()
            rpc_port, peer_port = ports[index], ports[index + 4]
            cookie = directory / 'rpc.cookie'
            command = [str(self.b3_path), '-regtest', '-b3modernregtest', '-server=1', '-daemon=0',
                       '-datadir=' + str(directory), '-disablewallet=1', '-listen=1', '-bind=127.0.0.1:' + str(peer_port),
                       '-port=' + str(peer_port), '-rpcbind=127.0.0.1:' + str(rpc_port), '-rpcport=' + str(rpc_port),
                       '-rpcallowip=127.0.0.1', '-rpccookiefile=' + str(cookie), '-rpccookieperms=owner',
                       '-dnsseed=0', '-fixedseeds=0', '-discover=0', '-listenonion=0', '-onion=0',
                       '-natpmp=0', '-i2pacceptincoming=0', '-onlynet=ipv4', '-maxconnections=16',
                       '-dbcache=32', '-maxmempool=10', '-par=1']
            command += ['-connect=127.0.0.1:' + str(ports[peer + 4]) for peer in range(4) if peer != index]
            owned = self.start(command, directory / 'process.log')
            rpc = B3Rpc('http://127.0.0.1:' + str(rpc_port), cookie)
            self.b3.append({'rpc': rpc, 'process': owned, 'directory': directory, 'command': command})
        for item in self.b3:
            wait_until(lambda item=item: item['process'].process.poll() is None and item['rpc'].assert_regtest(),
                       90, 'B3 modern regtest node failed to become ready')
        wait_until(lambda: all(len(item['rpc'].call('getpeerinfo')) >= 1 for item in self.b3), 60,
                   'isolated B3 nodes did not connect to their local peers')
        self.mine_b3(3)
        # Exercise the actual CLI against these owned fresh datadirs too.
        cli = subprocess.run([str(self.cli_path), '-regtest', '-datadir=' + str(self.b3[0]['directory']),
                              '-rpccookiefile=' + str(self.b3[0]['rpc'].cookie_file),
                              '-rpcport=' + self.b3[0]['rpc'].url.rsplit(':', 1)[1], 'getblockchaininfo'],
                             check=True, capture_output=True, text=True, timeout=20)
        info = json.loads(cli.stdout)
        expect(info['chain'] == 'regtest' and info['blocks'] == 3, 'real B3 CLI must match owned chain')
        atomic_json(self.evidence / 'b3-cli-readiness.json', info)
        self.assertions.append('four actual modern B3 regtest peers; canonical corridor anchors; no B3 PoS finality claim')

    def mine_b3(self, count=1):
        hashes = self.b3[0]['rpc'].call('generatetodescriptor', count, 'raw(51)', 1000000)
        expect(len(hashes) == count, 'B3 must mine every requested actual corridor block')
        anchor = self.b3[0]['rpc'].anchor()
        wait_until(lambda: all(item['rpc'].anchor() == anchor for item in self.b3), 60,
                   'B3 peers did not synchronize the actual canonical anchor')
        return anchor

    def start_evm(self):
        port = free_ports(1)[0]
        self.evm_url = 'http://127.0.0.1:' + str(port)
        command = [str(self.tools / 'anvil'), '--host', '127.0.0.1', '--port', str(port), '--chain-id', str(CHAIN_ID),
                   '--mnemonic', TEST_MNEMONIC, '--accounts', '6', '--balance', '10000', '--silent']
        process = self.start(command, self.evidence / 'anvil.log')
        self.evm_rpc = LocalRpc(self.evm_url)
        wait_until(lambda: process.process.poll() is None and self.evm_rpc.assert_fixture(), 20,
                   'owned unforked Anvil failed readiness')
        self.instance = self.cast.keccak('TEST actual FlowMesh regtest deployment ' + uuid.uuid4().hex)

    def deploy_routes(self):
        operator_addresses = sorted(item['public']['evm_address'] for item in self.operators)
        for name, asset in SYNTHETIC_ASSETS.items():
            directory = self.evidence / 'custody' / name
            config = {'rpc_url': self.evm_url, 'cast': str(self.tools / 'cast'), 'q_asset': Q_ASSET,
                      'custody_asset': asset, 'instance': self.cast.keccak('TEST custody route ' + self.instance + ' ' + name),
                      'account_count': 6, 'deployer_index': 0, 'operator_addresses': operator_addresses}
            driver = CustodyDriver(config, directory)
            token, _ = driver.deploy(self.evidence / 'contracts-out/RebirthRegtestToken.sol/RebirthRegtestToken.json')
            driver.token = config['token'] = token
            roster = '[' + ','.join(driver.operator_addresses) + ']'
            if self.custody_mode == 'production':
                encoded = self.cast.abi_encode('constructor(address,bytes32,bytes32,address[4],(uint256,uint64,uint64,uint64))',
                    token, asset, driver.instance, roster, '(31337,30,60,3600)')
                artifact = 'RebirthCustodyVault.sol/RebirthCustodyVault.json'
            else:
                encoded = self.cast.abi_encode('constructor(address,bytes32,bytes32,address[4],uint64)', token, asset,
                                              driver.instance, roster, 30)
                artifact = 'RebirthRegtestVault.sol/RebirthRegtestVault.json'
            vault, _ = driver.deploy(self.evidence / 'contracts-out' / artifact, encoded)
            driver.vault = config['vault'] = vault
            if self.custody_mode == 'production':
                pin_custody(driver, config)
            else:
                driver.verify_contract_identity()
            config['operator_addresses'] = driver.operator_addresses
            config['domain_separator'] = hex_value(driver.call(vault, 'DOMAIN_SEPARATOR()'), 32)
            config['dispute_seconds'] = 30
            atomic_json(directory / 'custody-config.json', config)
            self.routes[name] = {'driver': driver, 'config': config}
        self.assertions.append('three actually deployed synthetic ERC20/vault routes; Q-only withdrawal and fee policy')
        if self.custody_mode == 'production':
            self.assertions.append('generic custody-v2 vault exercised only on chain31337; independently pinned code/genesis/domain/policy/roster')
            driver = self.routes['Q']['driver']
            relay_config = {'format': 'fm2-signed-relayer/1', 'worker': str(self.worker_path),
                'datadir': str(self.evidence / 'private-gas-relayer'), 'custody_config': dict(self.routes['Q']['config'])}
            self.relayer_config_path = self.evidence / 'signed-relayer-config.json'
            atomic_json(self.relayer_config_path, relay_config)
            self.relayer_process = self.start([sys.executable, str(Path(__file__).with_name('signed_relayer.py')),
                '--config', str(self.relayer_config_path)], self.evidence / 'signed-relayer.stderr.log', True)
            public = self.relayer_process.request('info')
            self.relayer_address = public['address']
            # Gas funding is public Anvil fixture setup. All custody request and
            # payout transactions use the distinct private relayer key after it.
            driver.transact(self.relayer_address, '0x', native_value=10 ** 18)
            driver.relayer = self.relayer_process
            self.routes['Q']['config']['relayer_public_address'] = self.relayer_address
            atomic_json(self.evidence / 'signed-relayer-public.json', public)
            self.assertions.append('dedicated private gas relayer fsyncs exact EIP-155 raw transactions before broadcast; gas key never reaches coordinator')

    def restart_signed_relayer(self):
        if self.custody_mode != 'production':
            raise CustodyError('signed relayer restart applies only to custody-v2')
        self.relayer_process.close()
        self.relayer_process = self.start([sys.executable, str(Path(__file__).with_name('signed_relayer.py')),
            '--config', str(self.relayer_config_path)], self.evidence / 'signed-relayer-restarted.stderr.log', True)
        public = self.relayer_process.request('info')
        expect(public['address'] == self.relayer_address, 'gas relayer restart must retain its private identity and nonce history')
        self.routes['Q']['driver'].relayer = self.relayer_process
        return public

    def launch_operator(self, item):
        item['process'] = self.start([sys.executable, str(Path(__file__).with_name('operator_wrapper.py')),
                                      '--config', str(item['config_path'])], item['directory'] / 'wrapper.stderr.log', True)
        return item['process']

    def bootstrap_operators(self):
        manifest_file = self.evidence / 'deployment.json'
        for index in range(4):
            directory = self.evidence / ('operator-' + str(index))
            directory.mkdir()
            config = {'format': 'fm2-regtest-wrapper/1', 'worker': str(self.worker_path), 'datadir': str(directory),
                      'cast': str(self.tools / 'cast'), 'operator_index': index + 1, 'evm_rpc_url': self.evm_url,
                      'b3_rpc_url': self.b3[index]['rpc'].url, 'b3_cookie_file': str(self.b3[index]['rpc'].cookie_file),
                      'deployment_file': str(manifest_file)}
            path = directory / 'wrapper-config.json'
            atomic_json(path, config)
            item = {'directory': directory, 'config_path': path, 'config': config}
            self.launch_operator(item)
            item['public'] = item['process'].request('bootstrap')
            self.operators.append(item)
        self.operators.sort(key=lambda item: raw_hex(item['public']['public_key'], 48))

    def open_operators(self):
        manifest_file = self.evidence / 'deployment.json'
        roster = [{'public_key': raw_hex(item['public']['public_key'], 48), 'pop': raw_hex(item['public']['pop'], 96)}
                  for item in self.operators]
        anchor = self.b3[0]['rpc'].anchor()
        initial_source = self.source_end(mine_confirmations=True)
        manifest = {'format': 'fm2-regtest-deployment/1', 'instance': self.instance, 'chain_id': CHAIN_ID,
                    'custody_profile': self.custody_profile, 'mainnet_enabled': False,
                    'agreement': self.agreement,
                    'evm_initial_checkpoint': initial_source,
                    'b3_genesis': MODERN_GENESIS, 'initial_anchor': anchor, 'profile_name': self.profile_name,
                    'routes': {name: value['config'] for name, value in self.routes.items()}, 'roster': roster,
                    'operators': [item['public'] for item in self.operators],
                    'scope': ('four fixed operators, PBFT fixed-view0 over trusted local wrapper pipes; no full view change'
                              if self.agreement == 'pbft-fixed-view' else
                              'four fixed operators, PBFT controller with view changes over trusted local wrapper pipes')}
        atomic_json(manifest_file, manifest)
        for seat, item in enumerate(self.operators):
            public = {'format': 'fm2-regtest-operator/1', 'seat': seat, 'roster': roster,
                      'agreement': self.agreement,
                      'datadir': str(item['directory'] / 'state'), 'create': True,
                      'signer_instance': 100 + seat, 'commit_instance': 200 + seat,
                      'deployment_hex': canonical(manifest).encode().hex(), 'genesis_anchor': anchor,
                      'facts_start': {'height': anchor['height'], 'index': 0}}
            if self.agreement == 'pbft-view-change':
                public['control_instance'] = 300 + seat
            item['open_config'] = public
            item['process'].request('open', config=public)
        infos = self.infos()
        expect(len({info['head_hash'] for info in infos}) == 1 and len({info['domain'] for info in infos}) == 1,
               'all independently initialized operators must share one bound genesis/domain')
        expect(all(info.get('mode') == 'regtest-' + self.agreement and info.get('pbft_complete') is False for info in infos),
               'all operators must select the explicit versioned regtest PBFT integration')
        if self.agreement == 'pbft-view-change':
            expect(all(info.get('pbft_view_change') is True and info.get('pbft_fixed_view') is False for info in infos),
                   'the selected view-change profile must use the durable view-change controller')
        self.domain, self.markets = infos[0]['domain'], infos[0]['markets']
        atomic_json(self.evidence / 'operator-genesis.json', infos)
        self.client = self.start([str(self.worker_path)], self.evidence / 'client-helper.stderr.log', True)

    def infos(self):
        return [item['process'].request('info') for item in self.operators]

    def source_end(self, mine_confirmations=False):
        if mine_confirmations:
            self.routes['Q']['driver'].mine(blocks=2)
        height = int(self.evm_rpc.call('eth_blockNumber'), 16) - 1
        block = self.evm_rpc.call('eth_getBlockByNumber', [hex(height), False])
        return {'height': height, 'hash': hex_value(block['hash'], 32), 'minimum_confirmations': 2}

    def observe(self, anchor, events=None, operators=None, evm_end=None, mine_confirmations=True):
        started = time.monotonic_ns()
        operators = self.operators if operators is None else operators
        source_end = self.source_end(mine_confirmations=mine_confirmations) if evm_end is None else evm_end
        records = [item['process'].request('observe_receipts', anchor=anchor, events=events, evm_end=source_end) for item in operators]
        expect(len({canonical(record) for record in records}) == 1, 'independent operators must observe identical canonical facts')
        self.event('observed', anchor=anchor, records=records[0])
        self.last_source_observation = {**records[0], 'source_wait_and_validation_ms': (time.monotonic_ns() - started) / 1_000_000}
        return self.last_source_observation

    def commit(self, frames, operators=None, reject_two=False, label='slot'):
        operators = self.operators if operators is None else operators
        started = time.monotonic_ns()
        before = {item['open_config']['seat']: item['process'].request('info') for item in operators}
        next_height = next(iter(before.values()))['height'] + 1
        leader_seat = next_height % 4
        by_seat = {item['open_config']['seat']: item for item in operators}
        if leader_seat not in by_seat:
            raise CustodyError('fixed-view leader is unavailable; refusing automatic leadership/quorum changes')
        leader = by_seat[leader_seat]
        for frame in frames:
            for item in operators:
                item['process'].request('submit', frame=frame)
        candidate = leader['process'].request('candidate', actions=frames)
        proposal = {'header': candidate['header'], 'body': candidate['body']}
        pending, held, decisions, delivered = deque(), [], {}, []
        catchup = None
        processed_frames = 0

        def collect(sender, reply):
            nonlocal catchup
            seat = sender['open_config']['seat']
            for decision in reply['applied']:
                if decision['head_hash'] != candidate['hash'] or decision['header'] != candidate['header'] or decision['body'] != candidate['body']:
                    raise CustodyError('PBFT worker applied a different economic proposal')
                if seat in decisions and decisions[seat] != decision:
                    raise CustodyError('PBFT worker published conflicting application evidence')
                decisions[seat] = decision
            for frame in reply['frames']:
                if frame['from_seat'] != seat or not 0 <= frame['to_seat'] < 4:
                    raise CustodyError('worker frame does not belong to its actual private pipe identity')
                encoded = raw_hex(frame['message'], len(frame['message'].removeprefix('0x')) // 2)
                if encoded.startswith('0106') and seat == leader_seat:
                    catchup = dict(frame)
                pending.append(frame)

        def drain(allowed):
            nonlocal processed_frames
            # Independent operators execute concurrently, as peers on a network
            # would. Each round sends at most one frame to any one private
            # pipe, preserving that destination's FIFO and single writer.
            with ThreadPoolExecutor(max_workers=4) as transport:
                while pending:
                    batch, deferred = {}, deque()
                    while pending:
                        frame = pending.popleft()
                        recipient = frame['to_seat']
                        if recipient not in by_seat:
                            processed_frames += 1
                            continue  # Offline peer needs the actual final proof.
                        if recipient not in allowed:
                            processed_frames += 1
                            held.append(frame)
                            continue
                        if recipient in batch:
                            deferred.append(frame)
                        else:
                            batch[recipient] = frame
                            processed_frames += 1
                    pending.extend(deferred)
                    if processed_frames > 2048:
                        raise CustodyError('PBFT exchange exceeded the bounded local frame budget')
                    jobs = []
                    for recipient, frame in sorted(batch.items()):
                        future = transport.submit(by_seat[recipient]['process'].request, 'pbft_receive',
                                                  from_seat=frame['from_seat'], message=frame['message'])
                        jobs.append((recipient, frame, future))
                    # Join every submitted operation before collecting new
                    # rounds or touching a pipe again. Refusals still abort.
                    replies, failures = [], []
                    for recipient, frame, future in jobs:
                        try:
                            replies.append((recipient, frame, future.result()))
                        except Exception as error:
                            failures.append(error)
                    if failures:
                        raise failures[0]
                    for recipient, frame, reply in replies:
                        delivered.append(frame)
                        collect(by_seat[recipient], reply)

        collect(leader, leader['process'].request('pbft_start', **proposal))
        if reject_two:
            other = next(seat for seat in by_seat if seat != leader_seat)
            pair = {leader_seat, other}
            drain(pair)
            expect(not decisions and all(by_seat[seat]['process'].request('info')['head_hash'] == before[seat]['head_hash'] for seat in pair),
                   'two actual PBFT participants must never decide or apply a slot')
            self.assertions.append('two actual PBFT participants cannot decide; third resumes the same proposal at unchanged quorum3')
            pending.extend(held)
            held.clear()
        drain(set(by_seat))
        for item in operators:
            collect(item, item['process'].request('pbft_drain'))
        drain(set(by_seat))
        expect(set(decisions) == set(by_seat), 'every live PBFT worker must durably decide and apply')
        infos = [item['process'].request('info') for item in operators]
        expect(len({info['head_hash'] for info in infos}) == 1 and infos[0]['head_hash'] == candidate['hash'],
               'durably applied independent operators must agree with certified candidate')
        elapsed = (time.monotonic_ns() - started) / 1_000_000
        self.latencies.append(elapsed)
        if catchup is None:
            raise CustodyError('leader did not publish a real certified proposal for recovery')
        result = {**proposal, 'certificate': decisions[leader_seat]['certificate'], 'head_hash': candidate['hash'],
                  'latency_ms': elapsed, 'catchup_frame': catchup}
        self.event('committed', label=label, frames=delivered, applied=decisions, **result)
        return result

    def client_action(self, name, seed, nonce, kind, payload, ref_slot=0, max_delay=32):
        result = self.client.request('client_action', seed=seed, domain=self.domain, nonce=nonce,
                                     ref_slot=ref_slot, max_delay=max_delay, kind=kind, payload=payload)
        # Exact signed frame is durable before any submission. Never regenerate
        # it with a new nonce/expiry to disguise an ambiguous earlier result.
        atomic_json(self.evidence / 'client-actions' / (name + '.json'), result)
        return result

    def balances(self, account):
        results = [item['process'].request('account', account=raw_hex(account)) for item in self.operators]
        expect(all(value.get('found') and value.get('verified') is True for value in results),
               'every operator must provide a verified account inclusion proof')
        expect(len({canonical(value['balances']) for value in results}) == 1, 'independent account balances must agree')
        return {raw_hex(value['asset']): (int(value['available']), int(value['reserved'])) for value in results[0]['balances']}

    def end_faults(self, alice, seed):
        anchor = self.b3[0]['rpc'].anchor()
        wrong = {**anchor, 'hash': 'ff' * 32}
        operator = self.operators[0]
        try:
            operator['process'].request('observe_receipts', anchor=wrong, events=[])
        except CustodyError as error:
            expect('anchor' in str(error), 'wrong B3 anchor must refuse for the expected chain check')
        else:
            raise CustodyError('wrong canonical B3 anchor was accepted')
        q = self.routes['Q']['driver']
        minted = q.mint(0, 1)
        fake = {'kind': 'deposit', 'route': 'Q', 'transaction_hash': minted['transactionHash'],
                'account': '0x' + raw_hex(alice), 'amount_raw': '1', 'sender': q.accounts[0]}
        try:
            operator['process'].request('observe_receipts', anchor=anchor, events=[fake])
        except CustodyError as error:
            expect('event' in str(error), 'a token mint must refuse as a forged vault deposit event')
        else:
            raise CustodyError('a token mint was incorrectly credited as a vault deposit')

        head = self.infos()[0]
        request = self.client_action('fault-only-pending-withdrawal', seed, 4, 'WITHDRAW',
                                      {'asset': raw_hex(Q_ASSET), 'amount': 1_000_000, 'recipient': raw_hex(q.accounts[5], 20)},
                                      ref_slot=head['height'])
        self.observe(self.mine_b3(), [])
        self.commit([request['frame']], label='fault-only pending obligation before deliberate EVM rollback')
        proof = operator['process'].request('withdrawal', withdrawal_id=request['id'])
        expect(proof.get('found') and proof.get('verified') is True and not proof['record']['paid'],
               'rollback negative test must start with a genuine unpaid committed obligation')
        self.evm_rpc.assert_fixture()
        expect(self.evm_rpc.call('evm_revert', [self.evm_snapshot]) is True, 'owned Anvil must perform the actual snapshot rollback')
        self.evm_rpc.assert_fixture()
        timestamp = int(self.evm_rpc.call('eth_getBlockByNumber', ['latest', False])['timestamp'], 16)
        record, header = proof['record'], header_fields(proof['header'])
        value = {'id': '0x' + raw_hex(record['id']), 'account': '0x' + raw_hex(record['account']), 'asset': Q_ASSET,
                 'amount': str(record['amount']), 'recipient': '0x' + raw_hex(record['recipient'], 20),
                 'slot': record['slot'], 'stateRoot': '0x' + header['state_root'], 'validUntil': timestamp + 240}
        refusals = []
        for item in self.operators:
            for command, fields in (('candidate', {'actions': []}), ('sign_withdrawal', {'withdrawal': value})):
                try:
                    item['process'].request(command, **fields)
                except CustodyError as error:
                    require_canonical_source_refusal(error, command)
                    refusals.append({'seat': item['open_config']['seat'], 'command': command, 'detail': str(error)})
                else:
                    raise CustodyError('operator continued agreement/signing after credited backing disappeared')
        operator['process'].close()
        self.launch_operator(operator)
        expect(operator['process'].request('bootstrap') == operator['public'], 'rollback restart must preserve original operator identity')
        try:
            operator['process'].request('open', config={**operator['open_config'], 'create': False})
        except CustodyError as error:
            require_canonical_source_refusal(error, 'open')
            restart_refusal = str(error)
        else:
            raise CustodyError('restart forgot the removed original deposit receipts')
        result = {'expected_terminal_fault': True, 'no_happy_progress_after_rollback': True,
                  'snapshot': self.evm_snapshot, 'wrong_b3_anchor_refused': True, 'forged_mint_receipt_refused': True,
                  'genuine_pending_withdrawal': proof, 'removed_receipt_refusals': refusals, 'restart_refusal': restart_refusal,
                  'qualification': 'Anvil synthetic canonicality only; no production finality/chain-proof claim'}
        self.assertions += ['wrong B3 anchor refuses', 'ERC20 mint cannot impersonate a vault deposit',
                            'actual removed credited deposit blocks stop every operator agreement/withdrawal signer',
                            'restart rechecks immutable retained receipt history before acknowledging readiness']
        self.event('terminal-evm-rollback-refused', **result)
        return result

    def run(self):
        self.build()
        self.start_b3()
        self.start_evm()
        self.bootstrap_operators()
        self.deploy_routes()
        self.open_operators()
        self.evm_snapshot = self.evm_rpc.call('evm_snapshot')
        client_seeds = {'alice': os.urandom(32).hex(), 'bob': os.urandom(32).hex()}
        private_json(self.evidence / 'private-client-identities.json', client_seeds)
        asks, bids = [], []
        for index, (market_name, price, lots) in enumerate((('TOKENA_Q', 200, 3), ('TOKENB_Q', 300, 2))):
            market = self.markets[market_name]
            expect(int(market['base_atoms_per_lot']) == 1_000_000 and int(market['quote_atoms_per_tick']) == 10_000,
                   'test economics require the explicitly pinned synthetic lot/tick sizes')
            payload = {'market': market['id'], 'side': 'SELL', 'tif': 'GTC', 'price_ticks': price, 'lots': lots}
            asks.append(self.client_action('alice-ask-' + str(index), client_seeds['alice'], index + 1, 'PLACE', payload))
            payload = {**payload, 'side': 'BUY', 'tif': 'IOC'}
            bids.append(self.client_action('bob-bid-' + str(index), client_seeds['bob'], index + 1, 'PLACE', payload))
        alice, bob = asks[0]['account'], bids[0]['account']
        events = []
        for route_name, account, quantity in (('TOKENA', alice, '10'), ('TOKENB', alice, '10'),
                                               ('Q', alice, '10'), ('Q', bob, '100')):
            driver = self.routes[route_name]['driver']
            amount = q_units(quantity)
            driver.mint(0, amount)
            driver.approve(0, amount)
            receipt = driver.deposit(0, '0x' + raw_hex(account), amount)
            parsed = driver.expected_deposit(receipt, '0x' + raw_hex(account), amount, driver.accounts[0])
            events.append({'kind': 'deposit', 'route': route_name, 'transaction_hash': parsed['transaction_hash'],
                           'account': parsed['account'], 'amount_raw': parsed['amount_raw'], 'sender': parsed['sender']})
        deposit_anchor = self.mine_b3()
        deposit_end = self.source_end(mine_confirmations=True)
        try:
            self.operators[0]['process'].request('observe_receipts', anchor=deposit_anchor, events=events[:-1], evm_end=deposit_end)
        except CustodyError as error:
            expect('event' in str(error), 'omitted real deposit must refuse the complete source stream')
        else:
            raise CustodyError('coordinator omitted a real deposit from its declared source range')
        self.observe(deposit_anchor, events)
        self.commit([], label='four actual deposit credits')
        self.assertions.append('four independently observed actual deposits create authenticated credits')

        offline = self.operators[-1]
        offline['process'].close()
        active = self.operators[:-1]
        anchor = self.mine_b3()
        maker_source = self.observe(anchor, [], active)
        maker = self.commit([value['frame'] for value in asks], active, reject_two=True, label='two maker orders with one operator stopped')
        self.launch_operator(offline)
        expect(offline['process'].request('bootstrap') == offline['public'], 'restart must retain both operator identities')
        recovery_config = {**offline['open_config'], 'create': False}
        offline['process'].request('open', config=recovery_config)
        self.observe(anchor, [], [offline], evm_end=maker_source['source_range']['to'], mine_confirmations=False)
        offline['process'].request('pbft_receive', from_seat=maker['catchup_frame']['from_seat'], message=maker['catchup_frame']['message'])
        expect(len({info['head_hash'] for info in self.infos()}) == 1, 'restarted operator must catch up the exact certified slot')
        self.assertions.append('one operator restart preserves keys/journals and catches up an actual missing certificate')

        self.observe(self.mine_b3(), [])
        self.commit([value['frame'] for value in bids], label='two shared-book market fills')
        before = self.balances(alice)
        expect(before[raw_hex(Q_ASSET)] == (21_998_800, 0), 'two six-Q trades must charge seller exactly1200rawQ')
        expect(before[raw_hex(SYNTHETIC_ASSETS['TOKENA'])] == (7_000_000, 0) and
               before[raw_hex(SYNTHETIC_ASSETS['TOKENB'])] == (8_000_000, 0), 'actual maker inventory must settle')
        self.assertions.append('two real shared-book fills settle inventory and charge trading fees only in Q')

        q = self.routes['Q']['driver']
        head = self.infos()[0]
        withdrawal = self.client_action('alice-withdraw', client_seeds['alice'], 3, 'WITHDRAW',
                                        {'asset': raw_hex(Q_ASSET), 'amount': 5_000_000,
                                         'recipient': raw_hex(q.accounts[5], 20)}, ref_slot=head['height'])
        self.observe(self.mine_b3(), [])
        self.commit([withdrawal['frame']], label='Q withdrawal available to pending')
        proofs = [item['process'].request('withdrawal', withdrawal_id=withdrawal['id']) for item in self.operators]
        expect(all(proof.get('found') and proof.get('verified') is True and not proof['record']['paid'] for proof in proofs),
               'all operators must independently prove the pending withdrawal')
        record = proofs[0]['record']
        header = header_fields(proofs[0]['header'])
        timestamp = int(self.evm_rpc.call('eth_getBlockByNumber', ['latest', False])['timestamp'], 16)
        authorization = {'id': '0x' + raw_hex(record['id']), 'account': '0x' + raw_hex(record['account']),
                         'asset': Q_ASSET, 'amount': str(record['amount']), 'recipient': '0x' + raw_hex(record['recipient'], 20),
                         'slot': record['slot'], 'stateRoot': '0x' + header['state_root'], 'validUntil': timestamp + 240}
        signed = [item['process'].request('sign_withdrawal', withdrawal=authorization) for item in self.operators]
        signed.sort(key=lambda value: value['operator'])
        expect(len({value['digest'] for value in signed}) == 1, 'independent EVM authorizations must bind one identical digest')
        for item in self.operators:
            try:
                item['process'].request('renew_withdrawal', withdrawal={**authorization, 'amount': '5000001'})
            except CustodyError:
                pass
            else:
                raise CustodyError('coordinator changed principal and obtained an operator signature')
        forged = sorted((self.cast.test_address(index), self.cast.sign_fixture_digest(index, signed[0]['digest']))
                        for index in (1, 2, 3))

        def request_data(value, signatures):
            return self.cast.calldata('requestWithdrawal((bytes32,bytes32,bytes32,uint256,address,uint64,bytes32,uint64),bytes[])',
                                      q.withdrawal_tuple(value), '[' + ','.join(signatures) + ']')

        forged_receipt = q.transact(q.vault, request_data(authorization, [value[1] for value in forged]),
                                    gas=500_000, require_success=False)
        expect(int(forged_receipt['status'], 16) == 0, 'public Anvil fixture keys cannot impersonate random custody operators')
        original_authorization = dict(authorization)
        q.mine(advance_seconds=241)
        expired = q.transact(q.vault, request_data(authorization, [value['signature'] for value in signed[:3]]),
                             gas=500_000, require_success=False)
        expect(int(expired['status'], 16) == 0 and int(raw_hex(q.withdrawal_status(authorization['id'])['digest']), 16) == 0,
               'expired authorization must leave the original obligation unaccepted and recoverable')
        recovered_signer = self.operators[0]
        recovered_signer['process'].close()
        self.launch_operator(recovered_signer)
        expect(recovered_signer['process'].request('bootstrap') == recovered_signer['public'],
               'authorization renewal restart must retain each original signing identity')
        recovered_signer['process'].request('open', config={**recovered_signer['open_config'], 'create': False})
        timestamp = int(self.evm_rpc.call('eth_getBlockByNumber', ['latest', False])['timestamp'], 16)
        authorization = {**authorization, 'validUntil': timestamp + 240}
        signed = [item['process'].request('renew_withdrawal', withdrawal=authorization) for item in self.operators]
        signed.sort(key=lambda value: value['operator'])
        expect(len({value['digest'] for value in signed}) == 1, 'renewed authorization must be identical across operators')
        queued = q.request_withdrawal(authorization, [value['signature'] for value in signed[:3]])
        if self.custody_mode == 'production':
            relay_info = self.restart_signed_relayer()
            expect(relay_info['signed_intents'] == 1 and relay_info['resolved_intents'] == 1,
                   'custody request must be an exact durable signed gas transaction')
            repeated = self.relayer_process.request('resume', nonce=0)
            expect(repeated['transactionHash'] == queued['transactionHash'] and q.withdrawal_status(authorization['id'])['amount_raw'] == '5000000',
                   'restarted gas relayer must return the same accepted request without assigning another nonce')
        q.mine(advance_seconds=30)
        q.transact(q.token, self.cast.calldata('setTransfersEnabled(bool)', 'false'))
        failed = q.finalize(authorization['id'], require_success=False, gas=200_000)
        expect(int(failed['status'], 16) == 0 and not q.withdrawal_status(authorization['id'])['paid'],
               'failed external transfer must retain the same unpaid claim')
        q.transact(q.token, self.cast.calldata('setTransfersEnabled(bool)', 'true'))
        payout_snapshot = self.evm_rpc.call('evm_snapshot') if self.custody_mode == 'production' else None
        payout = q.finalize(authorization['id'])
        if self.custody_mode == 'production':
            original_payout_hash = payout['transactionHash']
            expect(self.evm_rpc.call('evm_revert', [payout_snapshot]) is True,
                   'owned source fixture must revert the not-yet-credited payout block')
            self.restart_signed_relayer()
            try:
                self.relayer_process.request('finalize', withdrawal_id='0x' + '99' * 32, gas=200_000)
            except CustodyError as error:
                expect('earlier signed relayer receipt' in str(error),
                       'source reorg must fence new nonce allocation until exact prior transaction recovery')
            else:
                raise CustodyError('gas relayer assigned new work before reconciling a removed receipt')
            payout = self.relayer_process.request('resume', nonce=2)
            expect(payout['transactionHash'] == original_payout_hash and q.withdrawal_status(authorization['id'])['paid'],
                   'source reorg must rebroadcast the identical durable raw payout and preserve one canonical payment')
            self.assertions.append('actual signed relayer restart/request replay and source-reorg exact-raw payout rebroadcast; no new gas nonce or double principal')
        paid = q.expected_paid(payout, authorization['id'], 5_000_000, q.accounts[5])
        replay = q.finalize(authorization['id'], require_success=False, gas=200_000)
        expect(int(replay['status'], 16) == 0, 'actual EVM payout replay must fail')
        payout_anchor = self.mine_b3()
        payout_end = self.source_end(mine_confirmations=True)
        try:
            self.operators[0]['process'].request('observe_receipts', anchor=payout_anchor, events=[], evm_end=payout_end)
        except CustodyError as error:
            expect('event' in str(error), 'omitted real payout must refuse the complete source stream')
        else:
            raise CustodyError('coordinator omitted a real payout from its declared source range')
        self.observe(payout_anchor, [{'kind': 'paid', 'route': 'Q', 'transaction_hash': paid['transaction_hash'],
                                    'withdrawal_id': authorization['id']}])
        self.commit([], label='actual canonical payout confirmation')
        self.assertions += ['actual q3-authorized five-Q payout', 'failed token transfer recovers the same claim',
                            'EVM payout replay refuses', 'confirmed payout retires committed pending liability',
                            'changed principal refuses operator authorization', 'public Anvil keys cannot forge custody quorum',
                            'expired unaccepted authorization renews same obligation after signer restart with append-only history',
                            'omitted deposits/payouts refuse independently scanned complete source ranges']

        alice_balances, bob_balances = self.balances(alice), self.balances(bob)
        expect(alice_balances[raw_hex(Q_ASSET)] == (16_997_800, 0) and bob_balances[raw_hex(Q_ASSET)] == (88_000_000, 0),
               'client Q balances must include exact trading fee1200 and withdrawal service fee1000')
        expect(bob_balances[raw_hex(SYNTHETIC_ASSETS['TOKENA'])] == (3_000_000, 0) and
               bob_balances[raw_hex(SYNTHETIC_ASSETS['TOKENB'])] == (2_000_000, 0), 'taker asset balances must settle')
        totals = [item['process'].request('totals') for item in self.operators]
        # Each native response verifies its own real quorum certificate.
        # Different q3 subsets may certify the same exact state/header.
        comparable = [{key: value for key, value in item.items() if key != 'certificate'} for item in totals]
        expect(len({canonical(value) for value in comparable}) == 1,
               'every independent operator must reconcile the same full ledger, header and state roots')
        per_asset = totals[0]['assets']
        reconciliation = {}
        for name, route in self.routes.items():
            driver = route['driver']
            actual = driver.call_uint(driver.token, 'balanceOf(address)', driver.vault)
            locked = driver.call_uint(driver.vault, 'locked()')
            item = next(value for value in per_asset if raw_hex(value['asset']) == raw_hex(SYNTHETIC_ASSETS[name]))
            expect(actual == locked == int(item['total_liabilities']) and int(item['pending']) == 0,
                   name + ' actual EVM reserves must equal complete committed ledger liabilities')
            expect(int(item['protocol_available']) == (2200 if name == 'Q' else 0), 'all protocol fees/rewards must remain Q')
            reconciliation[name] = {'actual_vault_raw': str(actual), 'locked_raw': str(locked), 'ledger': item}
        self.assertions.append('per-asset actual reserves equal all user/protocol liabilities; all2200fee atoms in Q')
        final_infos = self.infos()
        expect(len({value['head_hash'] for value in final_infos}) == 1, 'all four durable final heads must agree')
        end_fault = self.end_faults(alice, client_seeds['alice'])
        samples = sorted(self.latencies)
        percentiles = {name: samples[max(0, math.ceil(percentile * len(samples)) - 1)]
                       for name, percentile in (('p50_ms', .50), ('p95_ms', .95), ('p99_ms', .99))}
        report = {'passed': True, 'agreement': self.agreement, 'profile_name': self.profile_name,
                  'custody_profile': self.custody_profile, 'mainnet_enabled': False,
                  'scope': ('actual isolated EVM/B3/' + self.agreement +
                            ' roundtrip over trusted local pipes; fixture evidence only, no production or latency qualification'),
                  'assertions': self.assertions, 'b3_anchors': [item['rpc'].anchor() for item in self.b3],
                  'chain_id': CHAIN_ID, 'deployment': str(self.evidence / 'deployment.json'),
                  'source_adapter': {'complete_ranges': True, 'minimum_confirmations': 2,
                                     'qualification': 'durable local synthetic-EVM range checkpoints; not native B3 checkpoint proofs'},
                  'accounts': {'alice': alice, 'bob': bob}, 'roundtrip_operator_info_before_end_fault': final_infos,
                  'reconciliation_before_end_fault': reconciliation, 'end_fault': end_fault,
                  'withdrawal': {'authorization': authorization,
                  'original_authorization': original_authorization, 'forged_request_transaction': forged_receipt['transactionHash'],
                  'expired_request_transaction': expired['transactionHash'],
                  'request_transaction': queued['transactionHash'], 'paid_event': paid},
                  'latency': {'description': 'client submit through actual ' + self.agreement + ' frames and all live operators durable apply; fixture samples only',
                  'samples': len(samples), 'all_ms': self.latencies, **percentiles, 'dispute_seconds_separate': 30}}
        atomic_json(self.evidence / 'roundtrip-report.json', report)
        return report

    def close(self):
        for item in reversed(self.processes):
            item.close()


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
    parser.add_argument('--custody', choices=('fixture', 'production'), default='fixture')
    args = parser.parse_args()
    fixture = None
    try:
        fixture = Roundtrip(args)
        fixture.run()
        print(canonical({'passed': True, 'report': str(fixture.evidence / 'roundtrip-report.json')}))
        return 0
    except Exception as error:
        if fixture is not None:
            atomic_json(fixture.evidence / 'roundtrip-failure.json', {'passed': False, 'detail': str(error),
                        'scope': 'local roundtrip incomplete; retained evidence and signer state must not be erased'})
        print('local roundtrip failed: ' + str(error), file=sys.stderr)
        return 1
    finally:
        if fixture is not None:
            fixture.close()


if __name__ == '__main__':
    sys.exit(main())
