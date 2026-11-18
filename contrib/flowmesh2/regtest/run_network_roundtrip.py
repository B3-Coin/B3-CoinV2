#!/usr/bin/env python3
"""Actual encrypted peer-network and generic-custody qualification on REGTEST.

Client/admin pipes start the isolated deployment and submit authenticated actions.
They never receive/forward votes, invent peer identities, aggregate certificates,
or replay a retained final message to repair a returning peer.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import socket
import signal
import struct
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from custody import (CHAIN_ID, Q_ASSET, SYNTHETIC_ASSETS, CustodyError, atomic_json,
                     expect, hex_value, q_units)
from local_process import free_ports, raw_hex, wait_until
from operator_wrapper import header_fields, private_json
from run_roundtrip import Roundtrip, canonical
from https_api import HttpsApiError, HttpsClient, create_local_tls


class NetworkRoundtrip(Roundtrip):
    def __init__(self, args):
        args.agreement = 'pbft-view-change'
        args.custody = 'production'
        super().__init__(args)
        self.profile_name = 'fm2-regtest-stable-q/3'
        self.next_observation = None
        self.network_assertions = []
        self.automatic_enabled = False

    def bootstrap_operators(self):
        # Network identity is independent of the custody ECDSA and consensus BLS
        # keys and persists in each operator's private identity before bootstrap.
        manifest_file = self.evidence / 'deployment.json'
        for index in range(4):
            directory = self.evidence / ('operator-' + str(index))
            directory.mkdir()
            config = {'format': 'fm2-regtest-wrapper/1', 'worker': str(self.worker_path), 'datadir': str(directory),
                      'cast': str(self.tools / 'cast'), 'operator_index': index + 1, 'evm_rpc_url': self.evm_url,
                      'b3_rpc_url': self.b3[index]['rpc'].url, 'b3_cookie_file': str(self.b3[index]['rpc'].cookie_file),
                      'deployment_file': str(manifest_file), 'transport_mode': 'authenticated-tcp', 'automatic_batches': False}
            config['public_api'] = create_local_tls(directory / 'public-api')
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
        ports = free_ports(4)
        manifest = {'format': 'fm2-regtest-deployment/1', 'instance': self.instance, 'chain_id': CHAIN_ID,
                    'agreement': self.agreement, 'profile_name': self.profile_name,
                    'custody_profile': 'fm2-custody-v2-regtest/1', 'mainnet_enabled': False,
                    'evm_initial_checkpoint': self.source_end(mine_confirmations=True),
                    'b3_genesis': self.b3[0]['rpc'].call('getblockhash', 0), 'initial_anchor': anchor,
                    'routes': {name: value['config'] for name, value in self.routes.items()}, 'roster': roster,
                    'operators': [item['public'] for item in self.operators],
                    'transport': {'format': 'fm2-seat-tcp/1', 'network_magic': 'fabfb5da',
                                  'identity_keys': [item['public']['network_public_key'] for item in self.operators],
                                  'endpoints': [{'seat': seat, 'address': '127.0.0.1', 'port': port,
                                                 'epoch': 0, 'counter': 1} for seat, port in enumerate(ports)]},
                    'scope': 'isolated regtest release qualification; mainnet activation disabled'}
        atomic_json(manifest_file, manifest)
        self.network_manifest = manifest
        for seat, item in enumerate(self.operators):
            public = {'format': 'fm2-regtest-operator/1', 'seat': seat, 'roster': roster,
                      'agreement': self.agreement, 'datadir': str(item['directory'] / 'state'), 'create': True,
                      'signer_instance': 100 + seat, 'commit_instance': 200 + seat, 'control_instance': 300 + seat,
                      'deployment_hex': canonical(manifest).encode().hex(), 'genesis_anchor': anchor,
                      'facts_start': {'height': anchor['height'], 'index': 0}}
            item['open_config'] = public
            item['process'].request('open', config=public)
        self.endpoint_signatures = [item['process'].request('network_endpoint')['signature'] for item in self.operators]
        for item in self.operators:
            item['process'].request('network_start', signatures=self.endpoint_signatures)
        wait_until(lambda: all(item['process'].request('network_status')['authenticated_frames'] >= 3 for item in self.operators),
                   30, 'independent encrypted peer lanes failed mutual pinned-key authentication')
        infos = self.infos()
        expect(len({info['head_hash'] for info in infos}) == 1 and len({info['domain'] for info in infos}) == 1,
               'independent network operators must agree on the deployment-bound genesis')
        expect(all(info['transport'] == 'authenticated-bip324-tcp' and info['network_active'] and
                   info['mainnet_enabled'] is False and info['quorum'] == 3 for info in infos),
               'all peers must use authenticated sockets at unchanged quorum with mainnet disabled')
        self.domain, self.markets = infos[0]['domain'], infos[0]['markets']
        for item, info in zip(self.operators, infos):
            api = info['public_api']
            item['https'] = HttpsClient(api['url'], item['config']['public_api']['certificate'], api['certificate_sha256'])
            expect(item['https'].request('GET', '/v1/info')['head_hash'] == info['head_hash'],
                   'TLS1.3 client must read the same pinned certified operator head')
            self.routes['Q']['driver'].transact(info['custody_runtime']['gas_address'], '0x', native_value=10 ** 18)
        atomic_json(self.evidence / 'operator-genesis.json', infos)
        self.client = self.start([str(self.worker_path)], self.evidence / 'client-helper.stderr.log', True)
        self.network_assertions.append('mutual BIP324 session encryption and pinned BIP340 seat identities on real independent sockets')
        self.network_assertions.append('public client submits signed actions and reads proofs over pinned TLS1.3 HTTPS')

    def restart_operator(self, item):
        self.launch_operator(item)
        expect(item['process'].request('bootstrap') == item['public'], 'restart must retain all original private/public identities')
        item['process'].request('open', config={**item['open_config'], 'create': False})
        info = item['process'].request('info')
        item['https'] = HttpsClient(info['public_api']['url'], item['config']['public_api']['certificate'],
                                    info['public_api']['certificate_sha256'])
        item['process'].request('network_start', signatures=self.endpoint_signatures)

    def observe(self, anchor, events=None, operators=None, evm_end=None, mine_confirmations=True):
        # The hints select a source cutoff. Every peer independently scans the
        # complete confirmed range when it receives the authenticated batch.
        end = self.source_end(mine_confirmations=mine_confirmations) if evm_end is None else evm_end
        self.next_observation = {'anchor': anchor, 'evm_end': end}
        self.last_source_observation = {'source_range': {'to': end}, 'source_wait_and_validation_ms': 0}
        return self.last_source_observation

    def commit(self, frames, operators=None, reject_two=False, label='slot'):
        if self.automatic_enabled and frames:
            return self.commit_public(frames, operators=operators, label=label)
        if reject_two:
            raise CustodyError('network quorum-loss test must stop actual peer processes; no coordinator frame filter exists')
        live = self.operators if operators is None else operators
        before = [item['process'].request('info') for item in live]
        expect(len({info['head_hash'] for info in before}) == 1, 'live peers must share one contiguous parent before client seal')
        height = before[0]['height'] + 1
        started = time.monotonic_ns()
        hints = self.next_observation or {}
        for frame in frames:
            live[0]['https'].request('POST', '/v1/submit', {'frame': frame})
        live[0]['process'].request('network_seal', actions=frames, **hints)
        progress = {}
        def applied():
            infos = [item['process'].request('info') for item in live]
            statuses = [item['process'].request('network_status') for item in live]
            progress.update(infos=infos, statuses=statuses)
            if any(status['fenced'] for status in statuses):
                raise CustodyError('autonomous network runtime fenced: ' + canonical(statuses))
            return infos if all(info['height'] == height for info in infos) and len({info['head_hash'] for info in infos}) == 1 else None
        try:
            infos = wait_until(applied, 45, 'authenticated peers did not autonomously decide/apply the client batch')
        except CustodyError as error:
            raise CustodyError(str(error) + '; last=' + canonical(progress)) from error
        elapsed = (time.monotonic_ns() - started) / 1_000_000
        self.latencies.append(elapsed)
        record = live[0]['process'].request('network_committed', height=height)
        expect(record['found'], 'client must read a retained real certified decision after application')
        result = {key: record[key] for key in ('header', 'body', 'certificate')}
        result.update({'head_hash': infos[0]['head_hash'], 'latency_ms': elapsed})
        self.event('network-committed', label=label, height=height,
                   client_actions=len(frames), operator_heads=infos, **result)
        self.next_observation = None
        return result

    def enable_automatic(self):
        for item in self.operators:
            item['process'].request('network_automatic', enabled=True)
        self.automatic_enabled = True

    def commit_public(self, frames, operators=None, label='public-slot'):
        live = self.operators if operators is None else operators
        before = [item['https'].request('GET', '/v1/info') for item in live]
        parent_height = min(info['height'] for info in before)
        started = time.monotonic_ns()
        origin = live[0]
        accepted = [origin['https'].request('POST', '/v1/submit', {'frame': frame}) for frame in frames]
        expect(all(reply['code'] in ('ADMITTED', 'DUPLICATE') for reply in accepted),
               'all timed public actions must authenticate and enter native admission')
        included, proof, confirmed = None, None, False
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            replies = [origin['https'].request('POST', '/v1/submit', {'frame': frame}) for frame in frames]
            if all(reply['code'] == 'DUPLICATE' and reply.get('included') and reply.get('outcome') for reply in replies):
                included = replies
                proof = origin['https'].request('GET', '/v1/totals')
                if proof['certificate'] is not None and header_fields(proof['header'])['height'] > parent_height:
                    confirmed = True
                    break
            time.sleep(.002)
        if not confirmed or included is None or proof is None:
            raise CustodyError('public signed actions did not return certified included outcomes within the autonomous deadline')
        elapsed = (time.monotonic_ns() - started) / 1_000_000
        # Publication above follows the native verified q3 certificate and the
        # source ACK. All-peer convergence is checked outside the timed window.
        final_height = header_fields(proof['header'])['height']
        wait_until(lambda: all(item['https'].request('GET', '/v1/info')['height'] >= final_height for item in live),
                   30, 'live operators did not converge after public certified inclusion')
        infos = [item['https'].request('GET', '/v1/info') for item in live]
        expect(len({info['head_hash'] for info in infos}) == 1, 'public consensus must converge every live certified head')
        record = origin['process'].request('network_committed', height=final_height)
        self.latencies.append(elapsed)
        result = {key: record[key] for key in ('header', 'body', 'certificate')}
        result.update({'head_hash': infos[0]['head_hash'], 'latency_ms': elapsed,
                       'latency_scope': 'HTTPS signed submission through certified included outcomes and q3-backed published proof; all-peer convergence checked afterward'})
        self.event('public-network-committed', label=label, included=included, client_actions=len(frames), **result)
        self.next_observation = None
        return result

    def wait_caught_up(self, item, target_height):
        wait_until(lambda: item['process'].request('info')['height'] == target_height, 45,
                   'returning peer did not fetch and durably reexecute the authenticated certified prefix')
        expect(len({info['head_hash'] for info in self.infos()}) == 1, 'historical peer catchup must match every committed root')

    def balances(self, account):
        values = [item['https'].request('GET', '/v1/account/' + raw_hex(account)) for item in self.operators]
        expect(all(value.get('found') and value.get('verified') is True for value in values),
               'every HTTPS account response must contain a real verified certificate/inclusion proof')
        expect(len({canonical(value['balances']) for value in values}) == 1, 'independent HTTPS proofs must agree on balances')
        return {raw_hex(value['asset']): (int(value['available']), int(value['reserved'])) for value in values[0]['balances']}

    def client_action(self, name, seed, nonce, kind, payload, ref_slot=0, max_delay=32):
        result = self.client.request('client_action', seed=seed, domain=self.domain, nonce=nonce,
                                     ref_slot=ref_slot, max_delay=max_delay, kind=kind, payload=payload,
                                     profile_name=self.profile_name)
        atomic_json(self.evidence / 'client-actions' / (name + '.json'), result)
        return result

    def source_equivocation_test(self):
        parent = self.infos()[0]
        anchor = self.mine_b3()
        end_a = self.source_end(mine_confirmations=True)
        self.routes['Q']['driver'].mine(blocks=2)
        end_b = self.source_end()
        expect(end_a != end_b, 'source split fixture requires two different real canonical confirmed ranges')
        for item in self.operators:
            item['process'].request('network_pause', paused=True)
        candidates = []
        for index, item in enumerate(self.operators):
            candidates.append(item['process'].request('network_seal', actions=[], anchor=anchor,
                                                      evm_end=end_a if index < 2 else end_b))
        expect(len({value['candidate_hash'] for value in candidates}) == 2,
               'versioned source-range facts must commit different confirmed endpoints to different candidate headers')
        receiver, sender = self.operators[2], self.operators[0]
        generation = receiver['process'].request('info')['network_controller_generation']
        duplicates = receiver['process'].request('network_status')['history_duplicates']
        receiver['process'].request('network_request_history', peer=0, height=1)
        for item in (sender, receiver):
            item['process'].request('network_pause', paused=False)
        wait_until(lambda: receiver['process'].request('network_status')['history_duplicates'] > duplicates, 20,
                   'authenticated peer did not receive and ACK the valid old certified record')
        expect(receiver['process'].request('info')['network_controller_generation'] == generation,
               'valid old-history replay must preserve the live PBFT controller and timers')
        expect(sender['process'].request('info')['head_hash'] == parent['head_hash'] and
               receiver['process'].request('info')['head_hash'] == parent['head_hash'],
               'two disagreeing responsive peers cannot certify either source range')
        receiver['process'].request('network_handover_barrier', enabled=True)
        for item in self.operators:
            item['process'].request('network_pause', paused=False)
        target = parent['height'] + 1
        marker = receiver['directory'] / 'regtest-handover-ready.json'
        wait_until(marker.exists, 45, 'conflicting observer never reached the verified durable handover crash barrier')
        fault = json.loads(marker.read_text(encoding='utf-8'))
        expect(fault['phase'] == 'authority-durable-before-pending-pointer' and
               fault['wrapper_pid'] == receiver['process'].process.pid,
               'fault barrier must identify the exact owned wrapper and durable-authority phase')
        child_status = Path('/proc/' + str(fault['worker_pid']) + '/stat').read_text()
        expect(int(child_status.rsplit(')', 1)[1].split()[1]) == fault['wrapper_pid'],
               'qualification may kill only the actual private worker owned by this wrapper')
        os.kill(fault['worker_pid'], signal.SIGKILL)
        os.kill(fault['wrapper_pid'], signal.SIGKILL)
        receiver['process'].process.wait(timeout=10)
        responsive = [item for item in self.operators if item is not receiver]
        wait_until(lambda: all(item['process'].request('info')['height'] == target for item in responsive),
                   45, 'authenticated q3 new-view proof did not reconcile independently frozen source ranges')
        self.restart_operator(receiver)
        self.wait_caught_up(receiver, target)
        expect(len({info['head_hash'] for info in self.infos()}) == 1,
               'authorized source handover must converge every certified ledger head')
        statuses = [item['process'].request('network_status') for item in self.operators]
        expect(sum(value['source_handovers'] for value in statuses) >= 1,
               'live conflicting source observer must complete a proof-authorized handover')
        for item in self.operators[2:]:
            expect(list((item['directory'] / 'source-handovers').glob('*.json')),
                   'authorized source replacement must retain immutable old/new observation and NV evidence')
        self.network_assertions += ['different valid confirmed source ranges produce different /3 certified candidate hashes',
                                    'naked peer batches never thaw already published source observations',
                                    'quorum-proven higher NEW_VIEW authorizes independently revalidated durable source handover',
                                    'SIGKILL between durable NV authority and pending-pointer publication restores the exact handover after restart',
                                    'valid old certified history replay preserves live controller votes and timers']
        self.event('source-equivocation-reconciled', candidates=candidates, statuses=statuses, operator_info=self.infos())

    def highest_prepared_source_test(self):
        parent = self.infos()[0]
        slot = parent['height'] + 1
        next_leader = self.operators[(slot + 1) % 4]
        anchor = self.mine_b3()
        end_a = self.source_end(mine_confirmations=True)
        self.routes['Q']['driver'].mine(blocks=2)
        end_b = self.source_end()
        prepared = [item for item in self.operators if item is not next_leader]
        for item in self.operators:
            item['process'].request('network_pause', paused=True)
            item['process'].request('network_fault_drop_commits', enabled=True)
        candidates = []
        for item in self.operators:
            candidates.append(item['process'].request('network_seal', actions=[], anchor=anchor,
                                                      evm_end=end_b if item is next_leader else end_a))
        expect(len({value['candidate_hash'] for value in candidates}) == 2,
               'highest-prepared fixture requires two source-bound candidate values')
        selected_hash = next(candidates[index]['candidate_hash'] for index, item in enumerate(self.operators) if item in prepared)
        for item in self.operators:
            item['process'].request('network_pause', paused=False)
        wait_until(lambda: all(item['process'].request('info').get('pending_prepared_view') == 0 for item in prepared),
                   20, 'three actual peers did not persist the original-view highest prepared QC before commit loss')
        expect(all(item['process'].request('info')['head_hash'] == parent['head_hash'] for item in self.operators),
               'typed COMMIT packet loss must leave the prepared value undecided')
        wait_until(lambda: next_leader['process'].request('network_status')['own_source_handovers'] > 0,
                   45, 'replacement leader did not independently revalidate foreign highest-prepared source under its q-report proof')
        own_records = list((next_leader['directory'] / 'source-handovers').glob('*.json'))
        expect(own_records, 'replacement leader must durably retain exact unsigned q-report selection authority')
        for item in self.operators:
            item['process'].request('network_fault_drop_commits', enabled=False)
        wait_until(lambda: all(item['process'].request('info')['height'] == slot for item in self.operators),
                   45, 'restored COMMIT delivery did not finalize the exact highest-prepared source value')
        infos = self.infos()
        expect(len({info['head_hash'] for info in infos}) == 1 and infos[0]['head_hash'] == selected_hash,
               'foreign-source replacement leader must preserve the exact prior prepared header')
        self.network_assertions += ['actual q3 PREPARE certificates persist while typed COMMIT packets are lost',
                                    'replacement leader with different source facts handovers only under verified highest-prepared q-report evidence',
                                    'restored COMMIT delivery finalizes the exact original prepared value without a new economic action']
        self.event('foreign-highest-prepared-source-reconciled', candidates=candidates,
                   next_leader=next_leader['open_config']['seat'], selected_hash=selected_hash, operators=infos)

    def certified_history_source_test(self):
        parent = self.infos()[0]
        slot = parent['height'] + 1
        receiver = self.operators[slot % 4]
        responsive = [item for item in self.operators if item is not receiver]
        anchor = self.mine_b3()
        end_a = self.source_end(mine_confirmations=True)
        self.routes['Q']['driver'].mine(blocks=2)
        end_b = self.source_end()
        for item in self.operators:
            item['process'].request('network_pause', paused=True)
        # The actual initial leader durably PREPAREs B, then disappears before
        # any higher NEW_VIEW can reach it. The other three certify A.
        alternate = receiver['process'].request('network_seal', actions=[], anchor=anchor, evm_end=end_b)
        expect(receiver['process'].request('info')['observation_frozen'] is True,
               'history fixture must retain a genuinely published native PREPARE for the alternate source')
        candidates = [item['process'].request('network_seal', actions=[], anchor=anchor, evm_end=end_a)
                      for item in responsive]
        expect(all(value['candidate_hash'] != alternate['candidate_hash'] for value in candidates),
               'offline published source and the later certified source must differ')
        receiver['process'].close()
        for item in responsive:
            item['process'].request('network_pause', paused=False)
        wait_until(lambda: all(item['process'].request('info')['height'] == slot for item in responsive),
                   45, 'three actual peers failed to certify A after the B leader went offline')
        marker = receiver['directory'] / 'regtest-handover-ready.json'
        if marker.exists():
            expect(json.loads(marker.read_text())['phase'] == 'authority-durable-before-pending-pointer',
                   'only the known prior owned qualification marker may be cleared before the next fault')
            marker.unlink()
        # Drop all typed authenticated NEW_VIEW input before authority creation,
        # including already queued retries, so this gate tests history alone.
        receiver['config']['regtest_drop_new_views'] = True
        receiver['config']['regtest_handover_barrier'] = True
        atomic_json(receiver['config_path'], receiver['config'])
        self.restart_operator(receiver)
        wait_until(marker.exists, 45, 'certified historical A did not authorize the frozen B source handover')
        fault = json.loads(marker.read_text(encoding='utf-8'))
        expect(fault['wrapper_pid'] == receiver['process'].process.pid,
               'certified-history crash must name the exact owned wrapper')
        child_status = Path('/proc/' + str(fault['worker_pid']) + '/stat').read_text()
        expect(int(child_status.rsplit(')', 1)[1].split()[1]) == fault['wrapper_pid'],
               'certified-history fault may kill only its actual owned private worker')
        new_view_fault = fault['regtest_new_view_fault']
        expect(new_view_fault['enabled'] is True and
               0 <= new_view_fault['received'] == new_view_fault['dropped'] <= 0xffffffff,
               'every actual typed NEW_VIEW received before the history barrier must be dropped before authority')
        if new_view_fault['received']:
            expect(new_view_fault['dropped'] > 0, 'real NEW_VIEW retransmissions must exercise the typed receive loss')
        proof = json.loads((receiver['directory'] / 'source-handovers' / (fault['sha256'] + '.json')).read_text())
        expect(proof['format'] == 'fm2-source-handover/2' and proof['authority_kind'] == 'certified-history' and
               proof['old']['source']['to'] == end_b and proof['new']['source']['to'] == end_a,
               'history source replacement must retain the exact terminal certificate and both original ranges')
        os.kill(fault['worker_pid'], signal.SIGKILL)
        os.kill(fault['wrapper_pid'], signal.SIGKILL)
        receiver['process'].process.wait(timeout=10)
        receiver['config']['regtest_handover_barrier'] = False
        atomic_json(receiver['config_path'], receiver['config'])
        self.restart_operator(receiver)
        self.wait_caught_up(receiver, slot)
        wait_until(lambda: any(item['process'].request('network_status')['acknowledged'].get(str(slot % 4), {}).get('height', 0) >= slot
                               for item in responsive), 30,
                   'history sender did not receive the recovering frozen peer durable source/application ACK')
        post_recovery_fault = receiver['process'].request('network_status')
        expect(post_recovery_fault['regtest_drop_new_views'] is True and
               post_recovery_fault['new_view_frames'] == post_recovery_fault['regtest_dropped_new_views'],
               'typed NEW_VIEW receive loss must remain armed through exact certified recovery and durable ACK')
        receiver['config']['regtest_drop_new_views'] = False
        atomic_json(receiver['config_path'], receiver['config'])
        receiver['process'].close()
        self.restart_operator(receiver)
        self.wait_caught_up(receiver, slot)
        expect(receiver['process'].request('network_status')['regtest_drop_new_views'] is False,
               'typed NEW_VIEW receive loss must clear after the dedicated history fixture')
        self.network_assertions += ['offline leader with a durable alternate PREPARE catches up from exact authenticated q3 certified history while every arriving NEW_VIEW is dropped',
                                    'certified-history source authority grants no fresh signing permission and preserves immutable alternate evidence',
                                    'SIGKILL after terminal history proof fsync but before source-pointer publication recovers and ACKs the exact certified prefix']
        self.event('certified-history-frozen-source-reconciled', slot=slot, alternate=alternate,
                   certificate_authority=proof, new_view_fault_before_crash=new_view_fault,
                   new_view_fault_after_recovery={key: post_recovery_fault[key] for key in
                       ('regtest_drop_new_views', 'new_view_frames', 'regtest_dropped_new_views')}, operators=self.infos())

    def public_automatic_roundtrip(self, seeds, alice, bob):
        self.enable_automatic()
        # No pending work: ordinary order arrival after a full idle timeout
        # must still use the initial leader rather than an elapsed stale clock.
        time.sleep(3.25)
        initial = self.infos()[0]
        maker = self.client_action('public-automatic-maker', seeds['alice'], 4, 'PLACE',
                                   {'market': self.markets['TOKENA_Q']['id'], 'side': 'SELL', 'tif': 'GTC',
                                    'price_ticks': 200, 'lots': 1}, ref_slot=initial['height'])
        response = self.operators[2]['https'].request('POST', '/v1/submit', {'frame': maker['frame']})
        expect(response['code'] == 'ADMITTED', 'public signed maker must enter native admission')
        wait_until(lambda: all(item['https'].request('GET', '/v1/info')['height'] == initial['height'] + 1 for item in self.operators),
                   30, 'public HTTPS maker did not reach autonomous network consensus without an admin seal')
        idle_proofs = [item['https'].request('GET', '/v1/totals') for item in self.operators]
        expect(all(struct.unpack('<Q', bytes.fromhex(value['certificate'])[-8:])[0] == 0 for value in idle_proofs),
               'public order after idle must retain the scheduled initial leader with no needless view change')
        self.network_assertions.append('public order after more than3seconds idle uses the scheduled initial leader without a stale-clock view change')
        maker_head = self.infos()[0]
        stopped = self.operators[(maker_head['height'] + 1) % 4]
        stopped['process'].close()
        live = [item for item in self.operators if item is not stopped]
        taker = self.client_action('public-automatic-taker', seeds['bob'], 3, 'PLACE',
                                   {'market': self.markets['TOKENA_Q']['id'], 'side': 'BUY', 'tif': 'IOC',
                                    'price_ticks': 200, 'lots': 1}, ref_slot=maker_head['height'])
        response = live[-1]['https'].request('POST', '/v1/submit', {'frame': taker['frame']})
        expect(response['code'] == 'ADMITTED', 'public signed taker must enter through any live HTTPS operator')
        wait_until(lambda: all(item['https'].request('GET', '/v1/info')['height'] == maker_head['height'] + 1 for item in live),
                   45, 'public taker did not autonomously settle after the scheduled leader stopped')
        self.restart_operator(stopped)
        stopped['process'].request('network_automatic', enabled=True)
        self.wait_caught_up(stopped, maker_head['height'] + 1)
        height = self.infos()[0]['height']
        duplicate = self.operators[0]['https'].request('POST', '/v1/submit', {'frame': taker['frame']})
        expect(duplicate['code'] == 'DUPLICATE' and duplicate['included'] is True,
               'public exact retry must return its included outcome instead of a second order')
        time.sleep(.3)
        expect(all(info['height'] == height for info in self.infos()), 'included public retry must not create another economic batch')
        q = self.routes['Q']['driver']
        before = self.balances(alice)[raw_hex(Q_ASSET)][0]
        q.mint(0, 1_000_000); q.approve(0, 1_000_000); q.deposit(0, '0x' + raw_hex(alice), 1_000_000)
        q.mine(blocks=2)
        wait_until(lambda: self.operators[0]['https'].request('GET', '/v1/account/' + raw_hex(alice))['balances'] and
                   self.balances(alice)[raw_hex(Q_ASSET)][0] == before + 1_000_000,
                   30, 'confirmed real deposit was not automatically scanned/credited without an admin observe or seal')
        current = self.infos()[0]
        withdrawal = self.client_action('public-automatic-withdrawal', seeds['alice'], 5, 'WITHDRAW',
                                       {'asset': raw_hex(Q_ASSET), 'amount': 1_000_000,
                                        'recipient': raw_hex(q.accounts[4], 20)}, ref_slot=current['height'])
        origin = self.operators[1]
        for item in self.operators:
            item['process'].request('network_automatic', enabled=item is origin)
        origin['https'].request('POST', '/v1/submit', {'frame': withdrawal['frame']})
        wait_until(lambda: origin['https'].request('GET', '/v1/withdrawal/' + withdrawal['id']).get('found'),
                   30, 'public withdrawal was not committed by the independent network')
        signature_file = origin['directory'] / 'withdrawal-signatures' / (withdrawal['id'] + '.json')
        wait_until(signature_file.exists, 30, 'single enabled custody operator did not retain its own proven authorization')
        original_signature = json.loads(signature_file.read_text(encoding='utf-8'))
        time.sleep(.3)
        expect(int(raw_hex(q.withdrawal_status('0x' + withdrawal['id'])['digest']), 16) == 0,
               'one actual independent custody signer cannot create a q3 vault request')
        # This later real action changes the certified state while the original
        # withdrawal remains pending. Authorization keeps its immutable claim
        # slot/root and rechecks the current unpaid economic record.
        transfer_parent = origin['https'].request('GET', '/v1/info')
        transfer = self.client_action('public-trade-after-pending-withdrawal', seeds['bob'], 4, 'TRANSFER',
                                      {'asset': raw_hex(Q_ASSET), 'amount': 1, 'to': raw_hex(alice)},
                                      ref_slot=transfer_parent['height'])
        origin['https'].request('POST', '/v1/submit', {'frame': transfer['frame']})
        wait_until(lambda: origin['https'].request('GET', '/v1/info')['height'] > transfer_parent['height'],
                   30, 'newer public action did not advance the certified head after the original withdrawal')
        origin['process'].request('network_automatic', enabled=False)
        q.mine(advance_seconds=241)
        origin['process'].close()
        self.restart_operator(origin)
        origin['process'].request('network_automatic', enabled=True)
        wait_until(lambda: json.loads(signature_file.read_text(encoding='utf-8'))['withdrawal']['validUntil'] >
                   original_signature['withdrawal']['validUntil'], 30,
                   'expired unaccepted authorization did not renew the same original obligation after operator/relayer restart')
        for item in self.operators:
            item['process'].request('network_automatic', enabled=True)
        wait_until(lambda: int(raw_hex(q.withdrawal_status('0x' + withdrawal['id'])['digest']), 16) != 0,
                   45, 'public withdrawal did not obtain automatic independently verified q3 custody authorization and raw-transaction relay')
        q.mine(advance_seconds=30)
        wait_until(lambda: q.withdrawal_status('0x' + withdrawal['id'])['paid'], 30,
                   'node-owned funded relayers did not automatically finalize the challenged payout')
        q.mine(blocks=2)
        wait_until(lambda: all(item['https'].request('GET', '/v1/withdrawal/' + withdrawal['id']).get('record', {}).get('paid')
                              for item in self.operators), 30,
                   'autonomous complete source observer did not retire the actual paid obligation')
        expect(q.call_uint(q.token, 'balanceOf(address)', q.accounts[4]) == 1_000_000,
               'automatic withdrawal principal must reach the exact user recipient once')
        retry = self.operators[0]['https'].request('POST', '/v1/submit', {'frame': withdrawal['frame']})
        expect(retry['code'] == 'DUPLICATE' and retry['included'], 'public withdrawal retry must preserve the same nonce/fee/obligation')
        self.network_assertions += ['public HTTPS maker/taker orders automatically reach consensus without private seals',
                                    'public signed order accepted at any live operator survives scheduled-leader loss',
                                    'exact public retries return included outcomes without a second nonce/fee',
                                    'real confirmed deposit credits automatically without private observer/seal commands',
                                    'one enabled custody signer cannot create a q3 request; expired unaccepted authorization renews the same obligation after restart',
                                    'original certified withdrawal authority remains valid after a newer public action changes the current state root',
                                    'public signed withdrawal automatically gathers independently verified custody quorum and funded raw-transaction relayers',
                                    'challenge-complete token payout and paid liability confirmation finish without coordinator signatures or finalize commands']
        self.event('public-automatic-roundtrip', maker=maker['id'], taker=taker['id'], withdrawal=withdrawal['id'],
                   operator_info=self.infos(), network=[item['process'].request('network_status') for item in self.operators])

    def run(self):
        self.build(); self.start_b3(); self.start_evm(); self.bootstrap_operators(); self.deploy_routes(); self.open_operators()
        self.evm_snapshot = self.evm_rpc.call('evm_snapshot')
        seeds = {'alice': os.urandom(32).hex(), 'bob': os.urandom(32).hex()}
        private_json(self.evidence / 'private-client-identities.json', seeds)
        asks, bids = [], []
        for index, (market_name, price, lots) in enumerate((('TOKENA_Q', 200, 3), ('TOKENB_Q', 300, 2))):
            payload = {'market': self.markets[market_name]['id'], 'side': 'SELL', 'tif': 'GTC', 'price_ticks': price, 'lots': lots}
            asks.append(self.client_action('alice-ask-' + str(index), seeds['alice'], index + 1, 'PLACE', payload))
            bids.append(self.client_action('bob-bid-' + str(index), seeds['bob'], index + 1, 'PLACE', {**payload, 'side': 'BUY', 'tif': 'IOC'}))
        alice, bob = asks[0]['account'], bids[0]['account']
        deposits = []
        for name, account, quantity in (('TOKENA', alice, '10'), ('TOKENB', alice, '10'), ('Q', alice, '10'), ('Q', bob, '100')):
            driver = self.routes[name]['driver']; amount = q_units(quantity)
            driver.mint(0, amount); driver.approve(0, amount)
            receipt = driver.deposit(0, '0x' + raw_hex(account), amount)
            deposits.append(driver.expected_deposit(receipt, '0x' + raw_hex(account), amount, driver.accounts[0]))
        self.observe(self.mine_b3()); self.commit([], label='four real deposits over authenticated peers')
        self.network_assertions.append('four real deposits independently scanned, retained, certified, applied and source-ACKed')

        # The scheduled leader is a real stopped process. Responsive operators
        # self-drive monotone timers and exchange durable reports/new-view votes.
        next_height = self.infos()[0]['height'] + 1
        offline = self.operators[next_height % 4]
        offline['process'].close()
        active = [item for item in self.operators if item is not offline]
        self.observe(self.mine_b3()); maker = self.commit([value['frame'] for value in asks], active,
                                                         label='silent scheduled leader; automatic authenticated failover')
        certificate_view = struct.unpack('<Q', bytes.fromhex(maker['certificate'])[-8:])[0]
        expect(certificate_view > 0, 'missing scheduled leader must require a genuine view-bound replacement decision')
        self.observe(self.mine_b3()); self.commit([value['frame'] for value in bids], active,
                                                label='second missing slot while one peer remains stopped')
        self.restart_operator(offline)
        self.wait_caught_up(offline, 3)
        wait_until(lambda: any(item['process'].request('network_status')['history_acks'] >= 2 for item in active), 20,
                   'certified historical sender did not receive durable receiver/source ACKs')
        self.network_assertions += ['silent leader replaced through autonomous durable q3 view change',
                                    'returning peer fetched two certified historical slots directly from authenticated peers',
                                    'receiver reexecutes and fsyncs DECISION/APPLIED/source index before authenticated ACK']
        before = self.balances(alice)
        expect(before[raw_hex(Q_ASSET)] == (21_998_800, 0), 'two six-Q fills must charge exactly1200 stable fee atoms')

        # Wrong public from_seat and raw unauthenticated TCP bytes cannot enter
        # the private agreement receiver or alter an existing session identity.
        try:
            self.operators[0]['process'].request('pbft_receive', from_seat=1, message='0101')
        except CustodyError:
            pass
        else:
            raise CustodyError('public client injected a fabricated peer identity')
        port = self.network_manifest['transport']['endpoints'][0]['port']
        with socket.create_connection(('127.0.0.1', port), timeout=2) as attack:
            attack.sendall(b'forged peer hello and vote\n')
        api_info = self.operators[0]['process'].request('info')['public_api']
        bad_pin = HttpsClient(api_info['url'], self.operators[0]['config']['public_api']['certificate'], '00' * 32)
        try:
            bad_pin.request('GET', '/v1/info')
        except HttpsApiError as error:
            expect(error.status == 495, 'wrong HTTPS identity pin must fail before sending a request')
        else:
            raise CustodyError('HTTPS client accepted a substituted certificate identity')
        for payload in ({'frame': '00'}, {'frame': asks[0]['frame'], 'from_seat': 1}, {'frame': '00' * 20_000}):
            try:
                self.operators[0]['https'].request('POST', '/v1/submit', payload)
            except HttpsApiError:
                pass
            else:
                raise CustodyError('public HTTPS endpoint accepted malformed, peer-origin or oversized action input')
        expect(len({info['head_hash'] for info in self.infos()}) == 1, 'unauthenticated socket bytes must not mutate any certified head')
        self.network_assertions.append('client-supplied peer identity and unauthenticated plaintext TCP injection refused')
        self.network_assertions.append('HTTPS certificate substitution, malformed signature, peer-origin and oversized requests refused')

        self.source_equivocation_test()
        self.highest_prepared_source_test()
        self.certified_history_source_test()

        # Two live processes cannot reduce the configured quorum to make progress.
        stopped = self.operators[2:]
        for item in stopped:
            item['process'].close()
        live = self.operators[:2]
        parent = live[0]['process'].request('info')
        hints = {'anchor': self.mine_b3(), 'evm_end': self.source_end(mine_confirmations=True)}
        live[0]['process'].request('network_seal', actions=[], **hints)
        time.sleep(1.25)
        expect(all(item['process'].request('info')['head_hash'] == parent['head_hash'] for item in live),
               'two actual responsive peers must never certify or apply a slot')
        self.restart_operator(stopped[0])
        quorum_recovery_height = parent['height'] + 1
        wait_until(lambda: all(item['process'].request('info')['height'] == quorum_recovery_height for item in live + [stopped[0]]),
                   45, 'third returning peer did not restore progress at unchanged quorum3')
        self.restart_operator(stopped[1]); self.wait_caught_up(stopped[1], quorum_recovery_height)
        self.network_assertions.append('real two-peer outage stops commits; reconnecting third restores unchanged quorum3')

        q = self.routes['Q']['driver']
        withdrawal = self.client_action('alice-withdraw', seeds['alice'], 3, 'WITHDRAW',
                                       {'asset': raw_hex(Q_ASSET), 'amount': 5_000_000,
                                        'recipient': raw_hex(q.accounts[5], 20)}, ref_slot=quorum_recovery_height)
        self.observe(self.mine_b3()); self.commit([withdrawal['frame']], label='stable Q withdrawal debit to pending')
        proofs = [item['process'].request('withdrawal', withdrawal_id=withdrawal['id']) for item in self.operators]
        expect(all(proof.get('found') and proof.get('verified') is True and not proof['record']['paid'] for proof in proofs),
               'all independent operators must prove the same pending withdrawal')
        record, header = proofs[0]['record'], header_fields(proofs[0]['header'])
        timestamp = int(self.evm_rpc.call('eth_getBlockByNumber', ['latest', False])['timestamp'], 16)
        authorization = {'id': '0x' + raw_hex(record['id']), 'account': '0x' + raw_hex(record['account']),
                         'asset': Q_ASSET, 'amount': str(record['amount']), 'recipient': '0x' + raw_hex(record['recipient'], 20),
                         'slot': record['slot'], 'stateRoot': '0x' + header['state_root'], 'validUntil': timestamp + 240}
        signatures = [item['process'].request('sign_withdrawal', withdrawal=authorization) for item in self.operators]
        signatures.sort(key=lambda value: value['operator'])
        expect(len({value['digest'] for value in signatures}) == 1, 'production custody policy must bind identical q3 authorization')
        q.request_withdrawal(authorization, [value['signature'] for value in signatures[:3]])
        q.mine(advance_seconds=30)
        q.transact(q.token, self.cast.calldata('setTransfersEnabled(bool)', 'false'))
        failed = q.finalize(authorization['id'], require_success=False, gas=200_000)
        expect(int(failed['status'], 16) == 0 and not q.withdrawal_status(authorization['id'])['paid'],
               'failed external token transfer must leave the same obligation retryable')
        q.transact(q.token, self.cast.calldata('setTransfersEnabled(bool)', 'true'))
        payout = q.finalize(authorization['id'])
        paid = q.expected_paid(payout, authorization['id'], 5_000_000, q.accounts[5])
        replay = q.finalize(authorization['id'], require_success=False, gas=200_000)
        expect(int(replay['status'], 16) == 0, 'production custody payout nullifier must reject replay')
        self.observe(self.mine_b3()); self.commit([], label='independent complete-source confirmation of real payout')
        self.network_assertions += ['production custody policy q3 authorization and delayed real token payout',
                                    'failed token payout retries the same claim; replay stays refused',
                                    'confirmed payout retires pending liability exactly once']
        self.public_automatic_roundtrip(seeds, alice, bob)
        a, b = self.balances(alice), self.balances(bob)
        expect(a[raw_hex(Q_ASSET)] == (18_996_601, 0) and b[raw_hex(Q_ASSET)] == (85_999_999, 0),
               'final balances must include exact stable trading1400 and withdrawal2000 fees')
        totals = [item['process'].request('totals') for item in self.operators]
        compare = [{key: value for key, value in total.items() if key != 'certificate'} for total in totals]
        expect(len({canonical(value) for value in compare}) == 1, 'all authenticated peers must reconcile identical complete ledger roots')
        reconciliation = {}
        for name, route in self.routes.items():
            driver = route['driver']
            actual = driver.call_uint(driver.token, 'balanceOf(address)', driver.vault)
            locked = driver.call_uint(driver.vault, 'locked()')
            entry = next(value for value in totals[0]['assets'] if raw_hex(value['asset']) == raw_hex(SYNTHETIC_ASSETS[name]))
            expect(actual == locked == int(entry['total_liabilities']) and int(entry['pending']) == 0,
                   name + ' actual reserves must equal all user/protocol liabilities')
            expect(int(entry['protocol_available']) == (3400 if name == 'Q' else 0), 'fees must remain entirely stable Q')
            reconciliation[name] = {'actual_vault_raw': str(actual), 'locked_raw': str(locked), 'ledger': entry}
        before_fault = self.infos()
        statuses = [item['process'].request('network_status') for item in self.operators]
        expect(all(info['mainnet_enabled'] is False for info in before_fault), 'qualification must never enable mainnet')
        self.evm_rpc.assert_fixture()
        expect(self.evm_rpc.call('evm_revert', [self.evm_snapshot]) is True, 'actual owned EVM rollback must remove credited source history')
        wait_until(lambda: all(item['process'].request('network_status')['fenced'] for item in self.operators),
                   20, 'removed custody backing must fence every autonomous signing loop')
        self.network_assertions.append('actual credited-source rollback fences every autonomous network operator before new signing')
        samples = sorted(self.latencies)
        report = {'passed': True, 'mainnet_enabled': False, 'transport': 'authenticated-bip324-tcp',
                  'custody_profile': 'fm2-custody-v2-regtest/1', 'profile_name': self.profile_name,
                  'assertions': self.assertions + self.network_assertions,
                  'scope': 'actual isolated four-process static epoch0 regtest; synthetic assets and owned source RPCs',
                  'production_activation': False, 'source_finality': 'local canonical confirmed ranges; native B3 checkpoint finality remains separate',
                  'before_terminal_source_fault': {'operators': before_fault, 'network': statuses, 'reconciliation': reconciliation},
                  'after_source_fault': [item['process'].request('network_status') for item in self.operators],
                  'withdrawal': {'authorization': authorization, 'paid_event': paid},
                  'latency': {'samples': len(samples), 'all_ms': self.latencies,
                              'p95_ms': samples[max(0, math.ceil(.95 * len(samples)) - 1)], 'dispute_seconds_separate': 30}}
        atomic_json(self.evidence / 'network-roundtrip-report.json', report)
        return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True); parser.add_argument('--evidence-dir', required=True)
    parser.add_argument('--worker', required=True); parser.add_argument('--b3coind', required=True); parser.add_argument('--b3coin-cli', required=True)
    parser.add_argument('--tools', required=True); parser.add_argument('--solc', required=True)
    args = parser.parse_args(); fixture = None
    try:
        fixture = NetworkRoundtrip(args); fixture.run()
        print(canonical({'passed': True, 'report': str(fixture.evidence / 'network-roundtrip-report.json')})); return 0
    except Exception as error:
        if fixture is not None:
            atomic_json(fixture.evidence / 'network-roundtrip-failure.json', {'passed': False, 'detail': str(error), 'mainnet_enabled': False})
        print('authenticated network roundtrip failed: ' + str(error), file=sys.stderr); return 1
    finally:
        if fixture is not None:
            fixture.close()


if __name__ == '__main__':
    sys.exit(main())
