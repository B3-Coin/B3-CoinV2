#!/usr/bin/env python3
"""Four real operators and real-token code on an owned chain31337 Arbitrum fork.

This is a rehearsal of the existing synthetic-Q native profile against a real
token proxy on isolated Anvil state. It never enables mainnet, signs a chain42161
transaction or claims live protocol/finality or production-speed qualification.
Upstream URLs stay private; an explicit finalized origin is always required.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import uuid

from arbitrum_assets import build_sponsored_deposit, fund_local_fork_account, verify_asset_pin
from arbitrum_fork import ForkRpc
from arbitrum_rehearsal import custody_proxy_pin, rehearsal_inputs
from arbitrum_rpc import strict_json
from custody import (CHAIN_ID, Q_ASSET, SYNTHETIC_ASSETS, TEST_MNEMONIC, CustodyDriver,
                     CustodyError, atomic_json, expect, hex_value, q_units)
from custody_policy import pin_custody
from local_process import free_ports, raw_hex, wait_until
from operator_wrapper import private_json
from run_network_roundtrip import NetworkRoundtrip
from run_roundtrip import canonical
from source_finality import SourceFinality
from qualify_arbitrum_prelaunch import forge_rehearsal_summary, load_public_report
from qualify_regtest import sha256


def _public_input(path):
    path = Path(path).absolute()
    SourceFinality._safe_path(path)
    if not path.is_file():
        raise CustodyError('rehearsal public input must be a regular file')
    with path.open('rb') as stream:
        raw = stream.read(262145)
    if len(raw) > 262144:
        raise CustodyError('rehearsal public input exceeds its bound')
    return strict_json(raw.decode('utf-8'))


class ArbitrumRoundtrip(NetworkRoundtrip):
    def start_evm(self):
        port = free_ports(1)[0]
        self.evm_url = 'http://127.0.0.1:' + str(port)
        command = [str(self.tools / 'anvil'), '--host', '127.0.0.1', '--port', str(port),
            '--chain-id', str(CHAIN_ID), '--mnemonic', TEST_MNEMONIC, '--accounts', '6', '--balance', '10000',
            '--fork-url', self._fork_url, '--fork-block-number', str(self.fork_context['origin']['height']),
            '--fork-header', 'User-Agent:B3-Arbitrum-ReadOnly-Qualification/1', '--fork-state-by-number',
            '--no-fork-node-info', '--no-cors', '--silent']
        process = self.start(command, self.evidence / 'anvil-fork-private.log')
        self.evm_rpc = ForkRpc(self.evm_url, self.fork_context)
        wait_until(lambda: process.process.poll() is None and self.evm_rpc.assert_fixture(), 45,
                   'owned chain31337 fork failed its exact Arbitrum genesis/origin/provider identity')
        verify_asset_pin(self.evm_rpc, self.cast, self.asset_pin, execution_chain_id=31337)
        self.instance = self.cast.keccak('TEST isolated Arbitrum state rehearsal ' + uuid.uuid4().hex)
        self.assertions.append('owned literal-loopback Anvil chain31337 fork pins exact Arbitrum genesis, origin and provider endpoint')

    def deploy_routes(self):
        operators = sorted(item['public']['evm_address'] for item in self.operators)
        for name, asset in SYNTHETIC_ASSETS.items():
            directory = self.evidence / 'custody' / name
            config = {'rpc_url': self.evm_url, 'cast': str(self.tools / 'cast'), 'q_asset': Q_ASSET,
                'custody_asset': asset, 'instance': self.cast.keccak('TEST rehearsal route ' + self.instance + ' ' + name),
                'account_count': 6, 'deployer_index': 0, 'operator_addresses': operators,
                'mainnet_enabled': False, 'regtest_arbitrum_fork': copy.deepcopy(self.fork_context)}
            if name == 'Q':
                config['token'] = self.asset_pin['identity']['token']
            driver = CustodyDriver(config, directory)
            if name == 'Q':
                token = config['token']
            else:
                token, _ = driver.deploy(self.evidence / 'contracts-out/RebirthRegtestToken.sol/RebirthRegtestToken.json')
                driver.token = config['token'] = token
            roster = '[' + ','.join(driver.operator_addresses) + ']'
            encoded = self.cast.abi_encode('constructor(address,bytes32,bytes32,address[4],(uint256,uint64,uint64,uint64))',
                token, asset, driver.instance, roster, '(31337,30,60,3600)')
            vault, _ = driver.deploy(self.evidence / 'contracts-out/RebirthCustodyVault.sol/RebirthCustodyVault.json', encoded)
            driver.vault = config['vault'] = vault
            pin_custody(driver, config, proxy_implementation=custody_proxy_pin(self.asset_pin) if name == 'Q' else None)
            config['operator_addresses'] = driver.operator_addresses
            atomic_json(directory / 'custody-config.json', config)
            self.routes[name] = {'driver': driver, 'config': config}
        self.assertions += ['generic custody-v2 vaults use unchanged local chain31337 policy with30second challenge delay',
                            'Q route uses exact real token proxy/implementation pin; both base tokens remain synthetic',
                            'all three routes share the same explicit disabled Arbitrum-fork rehearsal context']

    def run(self):
        if not re.fullmatch(r'[A-Z][A-Z0-9_]{0,127}', self.args.fork_url_env):
            raise CustodyError('fork URL must come from an explicitly named environment variable')
        self._fork_url = os.environ.get(self.args.fork_url_env)
        if not self._fork_url:
            raise CustodyError('missing private read-only fork-provider URL/credentials')
        inputs = rehearsal_inputs(_public_input(self.args.finality_report), _public_input(self.args.finality_policy),
            _public_input(self.args.asset_pin), self.args.fork_provider, self._fork_url)
        self.fork_context, self.asset_pin = inputs['context'], inputs['asset_pin']
        atomic_json(self.evidence / 'rehearsal-public-inputs.json', inputs)
        self.build()
        self.start_b3()
        self.start_evm()
        contract_rehearsal = None
        if getattr(self.args, 'contracts_rehearsal', False):
            self.evm_rpc.assert_fixture()
            local_height = int(self.evm_rpc.call('eth_blockNumber'), 16)
            local_block = self.evm_rpc.call('eth_getBlockByNumber', [hex(local_height), False])
            solc = Path(self.args.solc).resolve(strict=True)
            forge = (self.tools / 'forge').resolve(strict=True)
            env = dict(os.environ, FOUNDRY_PROFILE='default',
                FOUNDRY_TEST=str(self.source / 'contrib/flowmesh2/rehearsal'), FOUNDRY_SOLC=str(solc),
                FOUNDRY_VIA_IR='true', FOUNDRY_OUT=str(self.evidence / 'rehearsal-contracts-out'),
                FOUNDRY_CACHE_PATH=str(self.evidence / 'rehearsal-contracts-cache'))
            resolved = subprocess.run([str(forge), 'config', '--root', str(self.source / 'contracts'), '--json'],
                cwd=self.source, env=env, check=True, capture_output=True, timeout=30)
            if len(resolved.stdout) > 1024 * 1024:
                raise CustodyError('resolved compiler configuration exceeds its bound')
            resolved_hash = hashlib.sha256(resolved.stdout).hexdigest()
            resolved = strict_json(resolved.stdout.decode('utf-8'))
            keys = ('evm_version', 'via_ir', 'optimizer', 'optimizer_runs', 'solc', 'src', 'test',
                    'out', 'cache_path', 'bytecode_hash', 'cbor_metadata')
            if (not isinstance(resolved, dict) or resolved.get('via_ir') is not True or
                    not isinstance(resolved.get('evm_version'), str) or not resolved['evm_version']):
                raise CustodyError('resolved local compiler/EVM configuration is incomplete')
            compiler_provenance = {'forge_sha256': sha256(forge), 'solc_sha256': sha256(solc),
                'forge_version': subprocess.check_output([str(forge), '--version'], text=True, timeout=30).strip(),
                'solc_version': subprocess.check_output([str(solc), '--version'], text=True, timeout=30).strip(),
                'foundry_toml_sha256': sha256(self.source / 'contracts/foundry.toml'),
                'resolved_configuration_sha256': resolved_hash,
                'resolved_configuration': {key: resolved[key] for key in keys if key in resolved},
                'execution_scope': 'Foundry local EVM configuration; not ArbOS/Nitro validation'}
            command = [str(self.tools / 'forge'), 'test', '--root', str(self.source / 'contracts'),
                '--offline', '--use', str(Path(self.args.solc).resolve(strict=True)), '--via-ir',
                '--match-contract', '^RebirthArbitrumForkTest$', '--match-path', '**/RebirthArbitrumFork.t.sol',
                '--fork-url', self.evm_url, '--fork-block-number', str(local_height), '--chain-id', str(CHAIN_ID),
                '--json', '--out', str(self.evidence / 'rehearsal-contracts-out'),
                '--cache-path', str(self.evidence / 'rehearsal-contracts-cache')]
            with (self.evidence / 'rehearsal-contracts.json').open('xb') as output, \
                    (self.evidence / 'rehearsal-contracts-private.log').open('xb') as log:
                result = subprocess.run(command, cwd=self.source, env=env, stdout=output, stderr=log, timeout=1800)
            if result.returncode:
                raise CustodyError('opt-in real-token contract rehearsal failed')
            contract_rehearsal = forge_rehearsal_summary(
                load_public_report(self.evidence / 'rehearsal-contracts.json'), local_height, local_block['hash'])
            compiler_provenance['artifacts'] = {}
            for name, artifact in (
                ('test_suite', self.evidence / 'rehearsal-contracts-out/RebirthArbitrumFork.t.sol/RebirthArbitrumForkTest.json'),
                ('deployed_vault', self.evidence / 'contracts-out/RebirthCustodyVault.sol/RebirthCustodyVault.json')):
                encoded = load_public_report(artifact)
                metadata = encoded.get('metadata')
                if isinstance(metadata, str):
                    metadata = strict_json(metadata)
                if not isinstance(metadata, dict) and isinstance(encoded.get('rawMetadata'), str):
                    metadata = strict_json(encoded['rawMetadata'])
                if (not isinstance(metadata, dict) or not isinstance(metadata.get('compiler'), dict) or
                        not isinstance(metadata['compiler'].get('version'), str) or
                        not isinstance(metadata.get('settings'), dict) or
                        not isinstance(metadata['settings'].get('evmVersion'), str)):
                    raise CustodyError('actual compiler artifact metadata is incomplete')
                settings = metadata['settings']
                compiler_provenance['artifacts'][name] = {'artifact_sha256': sha256(artifact),
                    'compiler_version': metadata['compiler']['version'],
                    'settings': {key: settings[key] for key in ('evmVersion', 'viaIR', 'optimizer', 'metadata') if key in settings}}
            contract_rehearsal['compiler_provenance'] = compiler_provenance
            self.evm_rpc.assert_fixture()
            expect(self.evm_rpc.call('eth_getBlockByNumber', [hex(local_height), False])['hash'] == local_block['hash'],
                   'local contract VM rehearsal must retain the exact owned fork block')
            atomic_json(self.evidence / 'rehearsal-contract-summary.json', contract_rehearsal)
        self.bootstrap_operators()
        self.deploy_routes()
        self.open_operators()
        self.enable_automatic()
        q = self.routes['Q']['driver']
        seeds = {'alice': os.urandom(32).hex(), 'bob': os.urandom(32).hex()}
        private_json(self.evidence / 'private-client-identities.json', seeds)
        # Pure signed identity probes are never submitted. Fresh order frames
        # below bind the head after all independently observed deposits settle.
        probe = {'market': self.markets['TOKENA_Q']['id'], 'side': 'SELL', 'tif': 'GTC', 'price_ticks': 200, 'lots': 3}
        alice = self.client_action('alice-identity', seeds['alice'], 1, 'PLACE', probe)['account']
        bob = self.client_action('bob-identity', seeds['bob'], 1, 'PLACE', {**probe, 'side': 'BUY', 'tif': 'IOC'})['account']
        deposits, sponsored = [], []
        for name in ('TOKENA', 'TOKENB'):
            driver = self.routes[name]['driver']
            amount = q_units('10')
            driver.mint(0, amount)
            driver.approve(0, amount)
            receipt = driver.deposit(0, '0x' + raw_hex(alice), amount)
            deposits.append(driver.expected_deposit(receipt, '0x' + raw_hex(alice), amount, driver.accounts[0]))
        # Fixed public fixture keys, outside the funded account set. Familiar
        # low mnemonic indices can already have real EIP7702 delegation code
        # on the fork; never clear that code or impersonate wallet validity.
        for index, account, amount in ((10008, alice, q_units('10')), (10009, bob, q_units('100'))):
            sender = self.cast.test_address(index)
            expect(sender not in q.accounts, 'source signer must be outside all initially funded fixture accounts')
            expect(hex_value(q.rpc.call('eth_getCode', [sender, 'latest'])) == '0x',
                   'sponsored fixture requires a code-empty EOA; delegated/contract wallets remain unqualified')
            funding = fund_local_fork_account(q.rpc, self.cast, self.asset_pin, sender, amount)
            # Artificial issuer funding may require gas for a local USDC minter.
            # The actual user-facing deposit starts and ends with exactly0 ETH.
            q.rpc.call('anvil_setBalance', [sender, '0x0'])
            before_eth = int(q.rpc.call('eth_getBalance', [sender, 'latest']), 16)
            expect(before_eth == 0, 'sponsored source user must hold no ETH before authorizing deposit')
            authorization = build_sponsored_deposit(q.rpc, self.cast, self.asset_pin, q.vault, sender, amount,
                '0x' + raw_hex(account), '0x' + os.urandom(32).hex())
            signature = bytes.fromhex(self.cast.sign_fixture_digest(index, authorization['digest'])[2:])
            r, s, v = '0x' + signature[:32].hex(), '0x' + signature[32:64].hex(), signature[64]
            if v < 27:
                v += 27
            expect(v in (27, 28), 'source authorization signature must have canonical recovery identifier')
            calldata = self.cast.calldata('depositWithAuthorization(address,uint256,bytes32,bytes32,uint256,uint256,uint8,bytes32,bytes32)',
                sender, amount, authorization['account'], authorization['user_salt'], authorization['valid_after'],
                authorization['valid_before'], v, r, s)
            receipt = q.transact(q.vault, calldata, sender_index=0)
            event = q.expected_deposit(receipt, '0x' + raw_hex(account), amount, sender)
            after_eth = int(q.rpc.call('eth_getBalance', [sender, 'latest']), 16)
            expect(after_eth == before_eth == 0, 'sponsor-paid real-token deposit must never debit user ETH')
            expect(hex_value(q.rpc.call('eth_getCode', [sender, 'latest'])) == '0x',
                   'sponsored source must retain its code-empty EOA profile after deposit')
            deposits.append(event)
            sponsored.append({'source': sender, 'amount': amount, 'account': raw_hex(account), 'funding': funding,
                'authorization': authorization, 'transaction_hash': receipt['transactionHash'],
                'source_wallet_profile': 'code-empty-eoa-vrs-only',
                'sponsor': q.accounts[0], 'user_eth_before': str(before_eth), 'user_eth_after': str(after_eth)})
        q.mine(blocks=2)
        wait_until(lambda: self.balances(alice).get(raw_hex(Q_ASSET)) == (10_000_000, 0) and
                   self.balances(bob).get(raw_hex(Q_ASSET)) == (100_000_000, 0), 60,
                   'real-token deposits were not independently observed/certified through automatic public runtime')
        self.network_assertions.append('zero-ETH source users authorize real-token deposits; independent sponsor pays local native gas')
        maker_frames, taker_frames = [], []
        parent = self.infos()[0]['height']
        for index, (market, price, lots) in enumerate((('TOKENA_Q', 200, 3), ('TOKENB_Q', 300, 2))):
            payload = {'market': self.markets[market]['id'], 'side': 'SELL', 'tif': 'GTC', 'price_ticks': price, 'lots': lots}
            maker_frames.append(self.client_action('alice-maker-' + str(index), seeds['alice'], index + 1,
                                                   'PLACE', payload, ref_slot=parent)['frame'])
        self.commit_public(maker_frames, label='real-token fork public maker orders')
        parent = self.infos()[0]['height']
        for index, (market, price, lots) in enumerate((('TOKENA_Q', 200, 3), ('TOKENB_Q', 300, 2))):
            payload = {'market': self.markets[market]['id'], 'side': 'BUY', 'tif': 'IOC', 'price_ticks': price, 'lots': lots}
            taker_frames.append(self.client_action('bob-taker-' + str(index), seeds['bob'], index + 1,
                                                   'PLACE', payload, ref_slot=parent)['frame'])
        self.commit_public(taker_frames, label='real-token fork public taker fills')
        expect(self.balances(alice)[raw_hex(Q_ASSET)] == (21_998_800, 0), 'seller must receive12Q less1200 stable trading atoms')
        expect(self.balances(bob)[raw_hex(Q_ASSET)] == (88_000_000, 0), 'buyer must spend exactly12Q across both markets')
        self.network_assertions.append('two markets settle exact12Q with1200 stable fee atoms over actual authenticated peer consensus')
        recipient = q.accounts[5]
        recipient_before = q.call_uint(q.token, 'balanceOf(address)', recipient)
        withdrawal = self.client_action('alice-public-withdrawal', seeds['alice'], 3, 'WITHDRAW',
            {'asset': raw_hex(Q_ASSET), 'amount': 5_000_000, 'recipient': raw_hex(recipient, 20)},
            ref_slot=self.infos()[0]['height'])
        self.commit_public([withdrawal['frame']], label='real-token fork public withdrawal')
        claim_id = '0x' + raw_hex(withdrawal['id'], 32)
        wait_until(lambda: int(raw_hex(q.withdrawal_status(claim_id)['digest']), 16) != 0, 60,
                   'independent custody operators failed automatic q3 authorization/request of public withdrawal')
        pending = [item['https'].request('GET', '/v1/withdrawal/' + withdrawal['id']) for item in self.operators]
        expect(all(value.get('verified') is True and value.get('record', {}).get('paid') is False for value in pending),
               'every public withdrawal proof must independently verify the same unpaid claim')
        restarted = self.operators[0]
        retained_identity = copy.deepcopy(restarted['public'])
        restarted['process'].close()
        self.restart_operator(restarted)
        restarted['process'].request('network_automatic', enabled=True)
        expect(restarted['public'] == retained_identity, 'accepted-claim recovery must retain the exact original operator identity')
        q.mine(blocks=2, advance_seconds=30)
        wait_until(lambda: q.withdrawal_status(claim_id)['paid'], 60,
                   'node-owned relayers did not automatically finalize real-token payout after the local challenge')
        q.mine(blocks=2)
        wait_until(lambda: all(item['https'].request('GET', '/v1/withdrawal/' + withdrawal['id']).get('record', {}).get('paid')
                              for item in self.operators), 60,
                   'complete source observers did not automatically certify the paid real-token obligation')
        expect(q.call_uint(q.token, 'balanceOf(address)', recipient) - recipient_before == 5_000_000,
               'exact withdrawal principal must reach the selected recipient once')
        duplicate = self.operators[2]['https'].request('POST', '/v1/submit', {'frame': withdrawal['frame']})
        expect(duplicate['code'] == 'DUPLICATE' and duplicate.get('included') is True,
               'exact public withdrawal retry must preserve the same nonce, fee and claim')
        totals = [item['https'].request('GET', '/v1/totals') for item in self.operators]
        expect(all(value.get('conservation') is True and value.get('certificate') is not None for value in totals),
               'all four public totals snapshots must pass native conservation and q3 certificate verification')
        canonical_totals = [{key: value for key, value in total.items() if key != 'certificate'} for total in totals]
        expect(len({canonical(value) for value in canonical_totals}) == 1, 'all four operators must reconcile identical ledger roots')
        reconciliation = {}
        for name, route in self.routes.items():
            driver = route['driver']
            actual = driver.call_uint(driver.token, 'balanceOf(address)', driver.vault)
            locked = driver.call_uint(driver.vault, 'locked()')
            entry = next(value for value in totals[0]['assets'] if raw_hex(value['asset']) == raw_hex(SYNTHETIC_ASSETS[name]))
            expect(actual == locked == int(entry['total_liabilities']) and int(entry['pending']) == 0,
                   name + ' real reserves, vault locked total and native liabilities must reconcile with no pending claims')
            expect(int(entry['protocol_available']) == (2200 if name == 'Q' else 0), 'all2200 fee atoms must remain stable Q')
            reconciliation[name] = {'token': driver.token, 'vault': driver.vault,
                'actual_vault_raw': str(actual), 'locked_raw': str(locked), 'ledger': entry}
        final_proofs = {name: [item['https'].request('GET', '/v1/account/' + raw_hex(account)) for item in self.operators]
                        for name, account in (('alice', alice), ('bob', bob))}
        expect(all(value.get('verified') is True for values in final_proofs.values() for value in values),
               'final account proofs must verify on all four operators after custody restart')
        self.evm_rpc.assert_fixture()
        expect(all(info['mainnet_enabled'] is False for info in self.infos()), 'rehearsal must retain every disabled-mainnet gate')
        report = {'format': 'fm2-arbitrum-four-operator-rehearsal/1', 'passed': True,
            'mainnet_enabled': False, 'mainnet_ready': False, 'production_activation': False,
            'scope': 'owned Anvil31337 fork of finalized Arbitrum state; actual real-token code with local artificial issuer funding',
            'source_origin': self.fork_context['origin'], 'source_chain_id': 42161, 'execution_chain_id': 31337,
            'asset': self.asset_pin['asset'], 'token': q.token, 'custody_profile': 'fm2-custody-v2-regtest/1',
            'profile_name': self.profile_name, 'internal_quote': 'unchanged synthetic regtest Q AssetId',
            'input_hashes': inputs['input_hashes'], 'provider_binding': self.fork_context['provider_binding'],
            'live_protocol_end_to_end_qualified': False, 'live_source_finality_qualified_by_this_run': False,
            'production_latency_qualified': False, 'user_deposit_native_gas_sponsored': True,
            'production_contract_or_delegated_wallets_qualified': False,
            'funding': 'artificial local issuer-role mint; no token storage overwrite or code replacement',
            'fees': {'trade_atoms': 1200, 'withdrawal_atoms': 1000, 'total_stable_atoms': 2200},
            'assertions': self.assertions + self.network_assertions + [
                'public withdrawal automatically reaches q3 custody authorization, delayed payout and source-paid confirmation',
                'accepted claim survives one operator/relayer restart under retained identity',
                'exact signed retry returns DUPLICATE; all reserves equal liabilities with zero pending claims'],
            'sponsored_deposits': sponsored, 'deposits': deposits, 'withdrawal_id': withdrawal['id'],
            'withdrawal_retry': duplicate, 'reconciliation': reconciliation,
            'final_account_proofs': final_proofs, 'final_totals_proofs': totals,
            'dispute_seconds_on_owned_fork': 30}
        report['contracts_rehearsal'] = contract_rehearsal
        atomic_json(self.evidence / 'arbitrum-roundtrip-report.json', report)
        return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('source-root', 'evidence-dir', 'worker', 'b3coind', 'b3coin-cli', 'tools', 'solc',
                 'finality-report', 'finality-policy', 'asset-pin', 'fork-provider', 'fork-url-env'):
        parser.add_argument('--' + flag, required=True)
    parser.add_argument('--contracts-rehearsal', action='store_true', help='opt-in14 real-token Forge tests against this owned fork')
    args, fixture = parser.parse_args(argv), None
    try:
        fixture = ArbitrumRoundtrip(args)
        fixture.run()
        print(canonical({'passed': True, 'mainnet_enabled': False,
                         'report': str(fixture.evidence / 'arbitrum-roundtrip-report.json')}))
        return 0
    except Exception:
        # Upstream RPC/process errors can include the private fork URL. Public
        # failure reports intentionally contain no exception text or argv.
        if fixture is not None:
            atomic_json(fixture.evidence / 'arbitrum-roundtrip-failure.json',
                {'passed': False, 'mainnet_enabled': False, 'detail': 'isolated real-token roundtrip refused; inspect private local process logs'})
        print('isolated Arbitrum real-token roundtrip refused; inspect private local process logs', file=sys.stderr)
        return 1
    finally:
        if fixture is not None:
            fixture.close()


if __name__ == '__main__':
    raise SystemExit(main())
