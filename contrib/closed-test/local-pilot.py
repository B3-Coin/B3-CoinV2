#!/usr/bin/env python3
"""One fresh, loopback-only attended pilot. No inherited qualification workload."""
import argparse
import configparser
import hashlib
import secrets
import http.client
import json
import os
from pathlib import Path
import re
import signal
import socket
import ssl
import stat
import subprocess
import sys
sys.dont_write_bytecode = True
import time
from datetime import datetime, timezone
from decimal import Decimal

CODE = Path(__file__).resolve().parent
SOURCE = CODE.parents[1]
HERE = None
BUILD = None
sys.path.insert(0, str(SOURCE / 'test/functional'))
from feature_flowmesh_independent import FlowMeshIndependentTest
from feature_flowmesh_release import CORRIDOR_END
from test_framework.flowmesh_client_tls import FlowMeshTLSFaultRelay, create_test_pki
from test_framework.test_framework import TestStatus
from test_framework.util import p2p_port, rpc_port, assert_equal, PortSeed

DEADLINE = None
DEADLINE_UTC = None
DAEMON = None
DAEMON_HASH = None
PROFILE_ID = None
STOP = False

def record(path, value):
    raw = (json.dumps(value, sort_keys=True, default=str) + '\n').encode()
    if len(raw) > 16 * 1024 * 1024:
        raise ValueError('Evidence bound exceeded')
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'ab') as output:
        output.write(raw); output.flush(); os.fsync(output.fileno())

def save(path, value):
    raw = (json.dumps(value, sort_keys=True, indent=2, default=str) + '\n').encode()
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as output:
        output.write(raw); output.flush(); os.fsync(output.fileno())

def owners(path):
    listing = subprocess.check_output(['ps', '-axo', 'pid=,command='], text=True, timeout=10)
    result = []
    needle = '-datadir=' + str(path)
    for line in listing.splitlines():
        if needle in line:
            result.append({'pid': int(line.strip().split(None, 1)[0]), 'command': line.strip().split(None, 1)[1]})
    return result

def check_window():
    if STOP or time.time() >= DEADLINE - 120:
        raise InterruptedError('Pilot stop requested or cleanup reserve reached')

class FreshPilot(FlowMeshIndependentTest):
    def set_test_params(self):
        super().set_test_params()
        self.rpc_timeout = 20
        self.options.nocleanup = True
        self.funding_addresses = {}
        self.pki = None
        self.api_ports = [rpc_port(8), rpc_port(9)]
        self.relay_ports = [rpc_port(10), rpc_port(11)]
        self.tls_relays = []
        self.child_processes = {}
        self.last_command = 0
        self.rejected_commands = set()
        self.client_address = None
        self.checkpoints = 0
        self.asset = None
        self.last_market = None
        self.next_status = 0
        self.client_ready = False
        for i, args in enumerate(self.extra_args):
            args.extend(['-enableflowmeshvalidator=1', '-unsafesqlitesync=0',
                         '-rpcbind=127.0.0.1', '-rpcallowip=127.0.0.1',
                         '-rpcservertimeout=30', '-listen=1',
                         '-listenonion=0',
                         '-dnsseed=0', '-fixedseeds=0', '-discover=0', '-natpmp=0',
                         '-server=1', '-noincludeconf'])

    def setup_network(self):
        check_window()
        if Path(self.options.tmpdir).resolve() != HERE / 'regtest':
            raise ValueError('Only the fresh approved pilot root is permitted')
        if hashlib.sha256(DAEMON.read_bytes()).hexdigest() != DAEMON_HASH:
            raise ValueError('Daemon identity differs')
        ports = [p2p_port(i) for i in range(12)] + [rpc_port(i) for i in (0, 1, 2, 3, 8, 9, 10, 11)]
        sockets = []
        try:
            for port in ports:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.bind(('127.0.0.1', port)); sockets.append(sock)
        finally:
            for sock in sockets: sock.close()
        save(HERE / 'preflight.json', {'deadline_utc': DEADLINE_UTC,
             'ports_available_at_check': ports, 'bind': '127.0.0.1',
             'daemon': str(DAEMON), 'daemon_sha256': DAEMON_HASH,
             'generated_keys_only': True, 'normal_sqlite_sync': True,
             'no_mocktime_or_system_clock_change': True})
        self.pki = create_test_pki(HERE / 'regtest/tls')
        for i, port in enumerate(self.api_ports):
            self.extra_args[i].extend(['-flowmeshapi=1', '-flowmeshapibind=127.0.0.1',
                f'-flowmeshapiport={port}', f'-flowmeshapicert={self.pki["certificate"]}',
                f'-flowmeshapikey={self.pki["key"]}'])
        super().setup_network()

    def setup_nodes(self):
        # Do not enter framework import_deterministic_coinbase_privkeys().
        self.add_nodes(self.num_nodes, self.extra_args)
        for i, node in enumerate(self.nodes):
            check_window()
            if Path(node.args[0]).resolve() != DAEMON.resolve():
                raise ValueError('Actual node command does not use the pinned daemon')
            if owners(node.datadir_path):
                raise ValueError('Fresh pilot datadir already has an owner')
            if (node.chain_path / 'wallets').exists():
                raise ValueError('Unexpected preexisting wallet directory')
            self.start_node(i)
            self.child_processes[i] = node.process
            save(HERE / f'node{i}-launch.json', {'pid': node.process.pid,
                'datadir': str(node.datadir_path), 'command': node.args + node.extra_args,
                'daemon_sha256': DAEMON_HASH, 'wallet_source': 'new createwallet, no imported keys'})
            self.init_wallet(node=i)

    def init_wallet(self, *, node):
        n = self.nodes[node]
        n.createwallet(wallet_name=self.default_wallet_name, load_on_startup=True)
        self.funding_addresses[node] = n.getnewaddress('pilot-generated-funding', 'legacy')

    def set_chain_time(self, timestamp):
        raise RuntimeError('This pilot must not set mock or system time')

    def synchronize_mempools(self):
        if self.client_ready:
            # After readiness use ordinary relay only. No client transaction
            # is manually injected into a producer by this helper.
            self.sync_mempools(timeout=60)
            return
        pools = [n.getrawmempool() for n in self.nodes]
        record(HERE / 'bootstrap-manual-relay.jsonl', {
            'method': 'sendrawtransaction of exact existing transactions',
            'txids': sorted(set().union(*(set(p) for p in pools)))})
        super().synchronize_mempools()

    def mine_corridor(self, count):
        for _ in range(count):
            check_window()
            # Respect the existing exact timestamp/future bound with wall time.
            while self.nodes[0].getblock(self.nodes[0].getbestblockhash())['time'] >= time.time() + 100:
                check_window(); time.sleep(.5)
            self.generatetoaddress(self.nodes[0], 1, self.funding_addresses[0])
        self.sync_blocks(timeout=120)

    def mine_pos_blocks(self, count, allow_overshoot=False):
        if not 1 <= count <= 64:
            raise ValueError('Block progression bound is1..64')
        target = self.nodes[0].getblockcount() + count
        self.start_pos()
        local_deadline = min(DEADLINE - 120, time.time() + 240)
        while min(n.getblockcount() for n in self.nodes) < target:
            check_window()
            if time.time() > local_deadline:
                save(HERE / f'progress-stall-{int(time.time())}.json', self.snapshot())
                raise TimeoutError('Real-time B3 progress stalled')
            time.sleep(.25)
        self.stop_pos()
        self.sync_blocks(timeout=90)

    def publish_checkpoint(self, market_id, prepare=False):
        if market_id != self.last_market or self.checkpoints >= 128:
            raise ValueError('Checkpoint is outside the pilot market/bound')
        status = self.market_status(self.nodes[0], market_id)
        if not status or not status['checkpoint_pending']:
            return {'published': False}
        transaction = self.nodes[0].createflowmeshcheckpoint(market_id)
        self.checkpoints += 1
        record(HERE / 'transactions.jsonl', {'kind': 'checkpoint', 'txid': transaction['txid'],
               'sequence': transaction.get('sequence'), 'at_height': self.nodes[0].getblockcount()})
        # Bootstrap/control injection is explicit, never called a propagation test.
        self.synchronize_mempools()
        self.mine_pos_blocks(1, allow_overshoot=True)
        self.wait_for_market_convergence(market_id)
        return {'published': True, 'txid': transaction['txid'], 'sequence': transaction.get('sequence')}

    def snapshot(self):
        return {'observed_utc': datetime.now(timezone.utc).isoformat(),
                'operators': [{'node': i, 'pid': self.child_processes.get(i).pid if self.child_processes.get(i) else None,
                    'height': n.getblockcount(), 'tip': n.getbestblockhash(),
                    'staking': n.getstakinginfo()['staking'],
                    'validator': n.getflowmeshvalidatorinfo(),
                    'network': n.getflowmeshnetworkinfo(),
                    'market': self.market_status(n, self.last_market) if self.last_market else None}
                    for i, n in enumerate(self.nodes)]}

    def https(self, port, method, params, *, trusted=True):
        context = ssl.create_default_context(cafile=str(self.pki['ca']) if trusted else None)
        connection = http.client.HTTPSConnection('127.0.0.1', port, context=context, timeout=10)
        body = json.dumps({'method': method, 'params': params}).encode()
        try:
            connection.request('POST', '/flowmesh/v1', body, {'Content-Type': 'application/json'})
            response = connection.getresponse(); data = response.read(32 * 1024 * 1024 + 1)
            if len(data) > 32 * 1024 * 1024: raise ValueError('HTTPS reply bound')
            return response.status, json.loads(data)
        finally:
            connection.close()

    def run_test(self):
        # Deliberately no super().run_test(): no reorg, reindex, performance,
        # withdrawal or broad qualification campaign is run by this pilot.
        n0 = self.nodes[0]
        self.mine_corridor(101)
        for i, node in enumerate(self.nodes[1:], 1):
            txid = n0.sendtoaddress(self.funding_addresses[i], Decimal('16000'))
            record(HERE / 'transactions.jsonl', {'kind': 'generated_operator_funding', 'node': i, 'txid': txid, 'amount_B3': '16000'})
        self.synchronize_mempools(); self.mine_corridor(1)
        for node, amount in zip(self.nodes, (40, 30, 20, 10)):
            node.createstake(amount); node.bindfinalitykey()
        self.synchronize_mempools(); self.mine_corridor(CORRIDOR_END - n0.getblockcount())
        self.mine_pos_blocks(1)
        fn = []
        for i, node in enumerate(self.nodes):
            created = node.createfncoin()
            assert_equal(created['amount'], 1)
            fn.append({'node': i, 'txid': created['txid'], 'owner_address': created['owner_address'],
                       'disintegration': created['disintegration'], 'created_before': created['created_before']})
            self.synchronize_mempools(); self.mine_pos_blocks(1, allow_overshoot=True)
        seats = []
        for i, node in enumerate(self.nodes):
            seat = node.bindflowmeshseat()
            seats.append({k: seat[k] for k in ('seat_id', 'seat_txid', 'seat_vout', 'bls_pubkey', 'owner_address')})
        assert_equal(len({s['bls_pubkey'] for s in seats}), 4)
        self.synchronize_mempools(); self.mine_pos_blocks(1, allow_overshoot=True)
        for node in self.nodes:
            assert_equal(node.startflowmeshvalidator()['armed_keys'], 1)
        issued = n0.issueasset(100_000_000, 6)
        self.asset = issued['asset_id']
        self.synchronize_mempools(); self.mine_pos_blocks(1, allow_overshoot=True)
        for node in self.nodes:
            node.setassetmetadata(self.asset, 'cUSD Pilot Unbacked Test', 'cUSD', issued['hex'])
        deposit = n0.flowmeshdeposit(self.asset, self.asset, 20_000_000, {'market_bootstrap': True})
        self.last_market = deposit['market_id']
        self.synchronize_mempools(); self.mine_pos_blocks(31, allow_overshoot=True)
        self.wait_for_market_convergence(self.last_market, require_unpaused=False)
        self.wait_until(lambda: self.market_status(n0, self.last_market)['checkpoint_pending'], timeout=90)
        genesis = self.publish_checkpoint(self.last_market)
        assert_equal(genesis['sequence'], 0)
        for port, upstream in zip(self.relay_ports, self.api_ports):
            self.tls_relays.append(FlowMeshTLSFaultRelay(port, upstream, self.pki))
        tls_results = []
        for port in self.api_ports + self.relay_ports:
            code, data = self.https(port, 'markets', {})
            assert code == 200 and data.get('ok'), (code, data)
            tls_results.append({'port': port, 'http_status': code, 'trusted_response': data})
            try:
                self.https(port, 'markets', {}, trusted=False)
            except ssl.SSLCertVerificationError:
                tls_results[-1]['untrusted_ca_rejected'] = True
            else:
                raise AssertionError('Untrusted TLS was not rejected')
        cert = subprocess.check_output(['openssl', 'x509', '-in', str(self.pki['certificate']),
            '-noout', '-text'], text=True, timeout=10)
        save(HERE / 'tls-verification.json', {'connections': tls_results, 'server_certificate_details': cert,
            'certificate_sha256': hashlib.sha256(self.pki['certificate'].read_bytes()).hexdigest(),
            'ca_sha256': hashlib.sha256(self.pki['ca'].read_bytes()).hexdigest()})
        tip = n0.getblock(n0.getbestblockhash())
        while tip['time'] > time.time():
            check_window(); time.sleep(.5)
        profile = {'schema': 1, 'ready': True, 'profile_id': PROFILE_ID, 'network': 'regtest',
            'regtest_profile': 'flowmesh-client-v1', 'operator': 'Local pilot operator; this machine only; generated test identities',
            'availability': f'Local session only; ends {DEADLINE_UTC}; no ongoing service',
            'b3_peer': f'127.0.0.1:{p2p_port(0)}',
            'https_endpoints': [r.url for r in self.tls_relays], 'ca_pem': self.pki['ca'].read_text()}
        save(HERE / 'enabled-profile.json', profile)
        ready = {'ready': True, 'profile_id': PROFILE_ID, 'deadline_utc': DEADLINE_UTC,
            'parent_pid': os.getpid(), 'operators': [{'index': i, 'pid': p.pid, 'datadir': str(self.nodes[i].datadir_path),
                'B3_port': p2p_port(i), 'rpc_port': rpc_port(i), 'FMN2_port': self.fm_ports[i]} for i, p in self.child_processes.items()],
            'daemon': str(DAEMON), 'daemon_sha256': DAEMON_HASH, 'profile_file': str(HERE / 'enabled-profile.json'),
            'ca_path': str(self.pki['ca']), 'HTTPS_endpoints': [r.url for r in self.tls_relays],
            'market_id': self.last_market, 'base_asset_id': self.asset, 'canonical_quote_asset': 'B3',
            'asset_decimals': 6, 'issuance': issued, 'genesis_checkpoint': genesis, 'generated_FN_creations': fn,
            'seats': seats, 'market_data': n0.getflowmeshmarketdata(self.last_market, {'limit': 1}),
            'command_directory': str(HERE / 'commands'), 'response_directory': str(HERE / 'responses'),
            'no_client_wallet_created': True, 'no_deterministic_funding_keys_imported': True,
            'setup_B3_transactions_manually_relayed': True, 'wall_clock_only': True}
        save(HERE / 'ready.json', ready)
        self.client_ready = True
        print(json.dumps({'ready': str(HERE / 'ready.json'), 'market_id': self.last_market}), flush=True)
        self.service_loop()

    def service_loop(self):
        self.start_pos()
        while not STOP and time.time() < DEADLINE - 120:
            self.process_commands()
            if time.time() >= self.next_status:
                record(HERE / 'observations.jsonl', self.snapshot())
                self.next_status = time.time() + 10
            status = self.market_status(self.nodes[0], self.last_market)
            if status and status['checkpoint_pending']:
                self.publish_checkpoint(self.last_market)
                self.start_pos()
            time.sleep(.2)

    def process_commands(self):
        for path in sorted((HERE / 'commands').glob('*.json')):
            if path.name in self.rejected_commands: continue
            try:
                st = path.lstat()
                if not stat.S_ISREG(st.st_mode) or st.st_nlink != 1 or st.st_uid != os.getuid() or st.st_size > 4096:
                    raise ValueError('Unsafe control file')
                value = json.loads(path.read_text())
                if not isinstance(value, dict): raise ValueError('Command must be an object')
                identifier = value.get('id')
                if type(identifier) is not int or not 1 <= identifier <= 128: raise ValueError('Command id bound')
                if path.name != f'{identifier:03d}.json': raise ValueError('Command filename/id mismatch')
            except (OSError, ValueError, TypeError) as error:
                # Terminal files are immutable. Refuse once, preserve the
                # evidence and continue servicing later valid commands.
                self.rejected_commands.add(path.name)
                record(HERE / 'commands-refused.jsonl', {'event': 'command_refused', 'file': path.name,
                       'error_type': type(error).__name__, 'reason': str(error)[:500]})
                continue
            if identifier <= self.last_command: continue
            self.last_command = identifier
            record(HERE / 'commands-consumed.jsonl', {'id': identifier, 'command': value.get('command')})
            try:
                result = self.command(value)
                response = {'id': identifier, 'ok': True, 'result': result}
            except Exception as error:
                response = {'id': identifier, 'ok': False, 'error_type': type(error).__name__, 'error': str(error)[:500]}
            save(HERE / 'responses' / f'{identifier:03d}.json', response)

    def command(self, value):
        global STOP
        command = value.get('command')
        allowed = {'snapshot': {'id', 'command'}, 'fund': {'id', 'command', 'address'},
                   'mine': {'id', 'command', 'blocks'}, 'checkpoint': {'id', 'command'},
                   'hold_read': {'id', 'command', 'milliseconds'}, 'clear_holds': {'id', 'command'},
                   'requests': {'id', 'command'}, 'stop': {'id', 'command'}}
        if command not in allowed or set(value) != allowed[command]: raise ValueError('Unsupported control shape')
        if command == 'snapshot': return self.snapshot()
        if command == 'fund':
            address = value['address']
            if not isinstance(address, str) or not re.fullmatch('[A-Za-z0-9]{26,90}', address): raise ValueError('Public address required')
            if self.client_address is not None: raise ValueError('Client funding already consumed; no automatic duplicate transaction')
            if not self.nodes[0].validateaddress(address)['isvalid']: raise ValueError('Invalid test address')
            if any(n.getaddressinfo(address).get('ismine') for n in self.nodes): raise ValueError('Address belongs to an operator, not the fresh tester')
            self.client_address = address
            record(HERE / 'client-funding-consumed.jsonl', {'address': address, 'amount_B3': '3', 'before_send': True})
            txid = self.nodes[0].sendtoaddress(address, Decimal('3'))
            self.synchronize_mempools(); self.mine_pos_blocks(1, allow_overshoot=True); self.start_pos()
            return {'txid': txid, 'address': address, 'amount_B3': '3'}
        if command == 'mine':
            count = value['blocks']
            if type(count) is not int or not 1 <= count <= 40: raise ValueError('Block bound1..40')
            before = self.nodes[0].getblockcount()
            self.mine_pos_blocks(count, allow_overshoot=True); self.start_pos()
            return {'before': before, 'after': self.nodes[0].getblockcount()}
        if command == 'checkpoint':
            result = self.publish_checkpoint(self.last_market); self.start_pos(); return result
        if command in ('hold_read', 'clear_holds'):
            delay = value.get('milliseconds', 0)
            if type(delay) is not int or not 0 <= delay <= 3500: raise ValueError('Read hold bound0..3500ms')
            holds = {method: delay for method in ('markets', 'updates', 'snapshot', 'action')} if delay else {}
            for relay in self.tls_relays: relay.configure(response_hold_ms=holds)
            return {'response_hold_ms': holds, 'submission_faults': False,
                    'note': 'Each actual upstream read is held separately; cached status need not issue HTTP'}
        if command == 'requests':
            result = {'endpoints': [r.snapshot() for r in self.tls_relays]}
            save(HERE / f'requests-{value["id"]:03d}.json', result)
            return {'file': str(HERE / f'requests-{value["id"]:03d}.json')}
        if command == 'stop': STOP = True; return {'stopping': True}

    def shutdown(self):
        # The generic framework intentionally leaves failed nodes alive. This
        # attended service must instead reap every owned child even on failure.
        cleanup = []
        for relay in self.tls_relays:
            relay.configure()
        for i, node in enumerate(self.nodes):
            child = self.child_processes.get(i) or node.process
            if child is None: continue
            error = None
            try:
                if child.poll() is None:
                    if node.rpc_connected: node.stop_node(wait_until_stopped=False)
                    else: child.terminate()
                    child.wait(timeout=60)
            except Exception as exc:
                error = str(exc)[:300]
                if child.poll() is None:
                    child.terminate()
                    try: child.wait(timeout=30)
                    except subprocess.TimeoutExpired: error += '; remains running; no forced kill'
            log = node.chain_path / 'debug.log'
            tail = b''
            if log.exists():
                with log.open('rb') as source:
                    source.seek(max(0, log.stat().st_size - 4*1024*1024))
                    tail = source.read(4*1024*1024)
            done = bool(re.search(rb'Shutdown:?\s+done\s*$', tail, re.M))
            cleanup.append({'node': i, 'pid': child.pid, 'exit_code': child.poll(), 'shutdown_done': done,
                            'remaining_owners': owners(node.datadir_path), 'cleanup_error': error})
            if child.poll() is not None:
                node.running = False; node.process = None
                for stream in (getattr(node, 'stdout', None), getattr(node, 'stderr', None)):
                    if stream and not stream.closed: stream.close()
        for relay in self.tls_relays:
            relay.stop()
        for proxy in self.proxies: proxy.stop()
        if self.network_thread: self.network_thread.close(timeout=10)
        if self.tls_relays:
            save(HERE / 'final-requests.json', {'endpoints': [r.snapshot() for r in self.tls_relays]})
        report = {'framework_status': str(getattr(self, 'success', None)), 'operators': cleanup,
                  'all_data_preserved': True, 'no_mocktime': True,
                  'stopped_utc': datetime.now(timezone.utc).isoformat()}
        report['clean'] = bool(cleanup) and all(r['exit_code'] == 0 and r['shutdown_done'] and not r['remaining_owners'] for r in cleanup)
        save(HERE / 'exit.json', report)
        print(json.dumps(report), flush=True)
        return 0 if report['clean'] and getattr(self, 'success', None) == TestStatus.PASSED else 1

def main():
    global STOP, HERE, BUILD, DEADLINE, DEADLINE_UTC, DAEMON, DAEMON_HASH, PROFILE_ID
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', required=True, type=Path)
    parser.add_argument('--session-dir', required=True, type=Path,
                        help='A NEW private directory outside the source checkout; never reused or deleted')
    parser.add_argument('--profile-id', required=True,
                        help='Unused lowercase/digit/dash identifier, at most 48 characters')
    parser.add_argument('--daemon-sha256', required=True)
    parser.add_argument('--duration', type=int, default=7200, help='Session seconds including bootstrap; 900..14400')
    parser.add_argument('--portseed', type=int, help='Optional test-framework port seed; unavailable ports refuse safely')
    args = parser.parse_args()
    if not 900 <= args.duration <= 14400:
        parser.error('Duration must be900..14400 seconds, including a120-second cleanup reserve')
    if not re.fullmatch(r'[a-z0-9][a-z0-9-]{0,47}', args.profile_id):
        parser.error('Unsafe profile identity')
    if not re.fullmatch(r'[0-9a-f]{64}', args.daemon_sha256):
        parser.error('Expected lowercase daemon SHA-256')
    BUILD = args.build_dir.resolve(strict=True)
    requested = args.session_dir.expanduser().absolute()
    if requested.exists() or requested.is_symlink():
        parser.error('Session directory must not already exist; preserved data is never reset/adopted')
    if requested.parent.resolve(strict=True) != requested.parent:
        parser.error('Session parent must not redirect through a symlink')
    HERE = requested
    if HERE.is_relative_to(SOURCE) or SOURCE.is_relative_to(HERE):
        parser.error('Runtime state must be outside the source checkout')
    config_path = BUILD / 'test/config.ini'
    configuration = configparser.ConfigParser()
    with config_path.open() as stream:
        configuration.read_file(stream)
    if Path(configuration['environment']['SRCDIR']).resolve() != SOURCE:
        parser.error('Framework source does not match this clean checkout')
    if Path(configuration['environment']['BUILDDIR']).resolve() != BUILD:
        parser.error('Framework build path does not match')
    DAEMON = BUILD / 'bin/b3coind'
    DAEMON_HASH = args.daemon_sha256
    if not DAEMON.is_file() or hashlib.sha256(DAEMON.read_bytes()).hexdigest() != DAEMON_HASH:
        parser.error('Built daemon identity differs')
    for name in ('BITCOIND', 'BITCOINCLI', 'BITCOINUTIL', 'BITCOINTX', 'BITCOINWALLET',
                 'TEST_RUNNER_PORT_MIN'):
        if name in os.environ:
            parser.error('Remove test binary/port environment override: ' + name)
    PROFILE_ID = args.profile_id
    DEADLINE = time.time() + args.duration
    DEADLINE_UTC = datetime.fromtimestamp(DEADLINE, timezone.utc).isoformat()
    # Bind-and-release probes precede all state/key creation. Actual bind
    # failure still refuses safely; this does not promise reservation.
    chosen = None
    candidates = [args.portseed] if args.portseed is not None else [secrets.randbelow(4000) for _ in range(32)]
    for seed in candidates:
        if not 0 <= seed <= 1000000:
            parser.error('Port seed outside bound0..1000000')
        PortSeed.n = seed
        sockets = []
        try:
            for port in [p2p_port(i) for i in range(12)] + [rpc_port(i) for i in (0,1,2,3,8,9,10,11)]:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sockets.append(sock)
                sock.bind(('127.0.0.1', port))
            chosen = seed
            break
        except OSError:
            pass
        finally:
            for sock in sockets:
                sock.close()
    if chosen is None:
        parser.error('No available bounded loopback port range; no services started')
    sys.argv = [sys.argv[0], '--configfile=' + str(config_path),
                '--tmpdir=' + str(HERE / 'regtest'), '--cachedir=' + str(HERE / 'cache'),
                '--portseed=' + str(chosen), '--nocleanup', '--timeout-factor=1', '--loglevel=INFO']
    os.umask(0o077)
    HERE.mkdir(mode=0o700, exist_ok=False)
    save(HERE / 'session.json', {'profile_id': PROFILE_ID, 'source': str(SOURCE),
        'build': str(BUILD), 'daemon_sha256': DAEMON_HASH, 'deadline_utc': DEADLINE_UTC,
        'portseed': chosen, 'controller_pid': os.getpid(), 'mode': 'loopback-only-generated'})
    for name in ('commands', 'responses'):
        (HERE / name).mkdir(mode=0o700, exist_ok=False)
    for number in (signal.SIGTERM, signal.SIGINT):
        signal.signal(number, lambda *_: globals().__setitem__('STOP', True))
    check_window()
    FreshPilot(__file__).main()

if __name__ == '__main__': main()
