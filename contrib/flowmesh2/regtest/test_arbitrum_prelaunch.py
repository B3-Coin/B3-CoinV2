#!/usr/bin/env python3
"""Offline gate-integrity tests; no live RPC, fork or qualification pass."""
import copy
import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
import subprocess
import sys
from unittest import mock

from custody import CustodyError
from qualify_arbitrum_prelaunch import (FORGE_TESTS, LEGACY_GATES, REQUIRED_SOURCE, forge_rehearsal_summary,
    fresh_evidence_path, require_frozen_hashes, require_source_coverage, validate_legacy_report,
    validate_roundtrip_report, main, qualification_environment, isolated_environment, RUNTIME_ENV)
from test_arbitrum_rehearsal import fixture_inputs
from source_finality import canonical_hash


def legacy():
    return {'format': 'fm2-network-regtest-qualification/1', 'passed': True, 'mainnet_ready': False,
        'mainnet_actions': [], 'source_tree_sha256': '11' * 32, 'worker_sha256': '22' * 32,
        'gates': [{'label': label, 'passed': True, 'exit_code': 0} for label in LEGACY_GATES]}


def forge():
    return {'contrib/flowmesh2/rehearsal/RebirthArbitrumFork.t.sol:RebirthArbitrumForkTest': {
        'test_results': {name + '()': {'status': 'Success'} for name in FORGE_TESTS}}}


def roundtrip():
    report, policy, pin = fixture_inputs()
    value = {'format': 'fm2-arbitrum-four-operator-rehearsal/1', 'passed': True,
        'mainnet_enabled': False, 'mainnet_ready': False, 'production_activation': False,
        'live_protocol_end_to_end_qualified': False, 'live_source_finality_qualified_by_this_run': False,
        'production_latency_qualified': False, 'production_contract_or_delegated_wallets_qualified': False,
        'source_chain_id': 42161, 'execution_chain_id': 31337,
        'asset': 'USDT0', 'token': pin['identity']['token'], 'source_origin': report['checkpoint'],
        'profile_name': 'fm2-regtest-stable-q/3', 'custody_profile': 'fm2-custody-v2-regtest/1',
        'input_hashes': {'policy_sha256': canonical_hash(policy), 'asset_pin_sha256': canonical_hash(pin),
                         'finality_report_sha256': canonical_hash(report)},
        'fees': {'trade_atoms': 1200, 'withdrawal_atoms': 1000, 'total_stable_atoms': 2200},
        'contracts_rehearsal': forge_rehearsal_summary(forge(), 17, pin['origin']['hash']),
        'sponsored_deposits': [{'user_eth_before': '0', 'user_eth_after': '0',
                               'source_wallet_profile': 'code-empty-eoa-vrs-only'} for _ in range(2)],
        'withdrawal_retry': {'code': 'DUPLICATE', 'included': True},
        'reconciliation': {name: {'actual_vault_raw': '10000000', 'locked_raw': '10000000',
            'ledger': {'total_liabilities': 10000000, 'pending': 0, 'protocol_available': 2200 if name == 'Q' else 0}}
            for name in ('Q', 'TOKENA', 'TOKENB')}}
    value['contracts_rehearsal']['compiler_provenance'] = {
        **{field: '11' * 32 for field in ('forge_sha256', 'solc_sha256', 'foundry_toml_sha256', 'resolved_configuration_sha256')},
        'resolved_configuration': {'via_ir': True, 'evm_version': 'osaka'},
        'artifacts': {name: {'artifact_sha256': '22' * 32, 'compiler_version': 'test-only',
                            'settings': {'evmVersion': 'osaka'}} for name in ('test_suite', 'deployed_vault')}}
    return value, report, policy, pin


class PrelaunchGateTests(unittest.TestCase):
    def test_all_original_twenty_gates_required_and_source_worker_bound(self):
        self.assertEqual(validate_legacy_report(legacy(), '11' * 32, '22' * 32)['gate_count'], 20)
        changes = (lambda x: x['gates'].pop(), lambda x: x['gates'].reverse(),
            lambda x: x['gates'][0].update(passed=False), lambda x: x['gates'][0].update(exit_code=True),
            lambda x: x['gates'][0].update(label=x['gates'][1]['label']),
            lambda x: x.update(mainnet_ready=True), lambda x: x.update(mainnet_actions=['broadcast']),
            lambda x: x.update(worker_sha256='33' * 32), lambda x: x.update(source_tree_sha256='44' * 32))
        for change in changes:
            value = legacy()
            change(value)
            with self.subTest(change=change), self.assertRaises(CustodyError):
                validate_legacy_report(value, '11' * 32, '22' * 32)

    def test_forge_requires_fourteen_actual_successes_no_empty_match_or_skip(self):
        self.assertEqual(forge_rehearsal_summary(forge(), 17, '0x' + 'ab' * 32)['test_count'], 14)
        for mode in ('empty', 'missing', 'skipped', 'failed', 'duplicate-suite', 'wrong-suite'):
            value = forge()
            suite = next(iter(value.values()))
            if mode == 'empty': value = {}
            elif mode == 'missing': suite['test_results'].pop(next(iter(suite['test_results'])))
            elif mode in ('skipped', 'failed'): next(iter(suite['test_results'].values()))['status'] = mode
            elif mode == 'duplicate-suite': value['extra:AnotherTest'] = copy.deepcopy(suite)
            else: value = {'Wrong.sol:WrongContract': suite}
            with self.subTest(mode=mode), self.assertRaises(CustodyError):
                forge_rehearsal_summary(value, 17, '0x' + 'ab' * 32)

    def test_roundtrip_never_upgrades_local_fork_to_live_profile_or_production_claim(self):
        value, report, policy, pin = roundtrip()
        self.assertIs(validate_roundtrip_report(value, 'USDT0', pin, report, policy)['mainnet_ready'], False)
        for field in ('mainnet_enabled', 'mainnet_ready', 'production_activation', 'live_protocol_end_to_end_qualified',
                      'live_source_finality_qualified_by_this_run', 'production_latency_qualified',
                      'production_contract_or_delegated_wallets_qualified'):
            changed = copy.deepcopy(value)
            changed[field] = True
            with self.subTest(field=field), self.assertRaises(CustodyError):
                validate_roundtrip_report(changed, 'USDT0', pin, report, policy)
        changed = copy.deepcopy(value)
        changed['execution_chain_id'] = 42161
        with self.assertRaises(CustodyError):
            validate_roundtrip_report(changed, 'USDT0', pin, report, policy)

    def test_fork_claim_requires_sponsored_zero_eth_and_exact_retry_reserves_fee_totals(self):
        changes = (lambda x: x['sponsored_deposits'].pop(),
            lambda x: x['sponsored_deposits'][0].update(user_eth_before='1'),
            lambda x: x['contracts_rehearsal'].update(skipped=1),
            lambda x: x['contracts_rehearsal'].pop('compiler_provenance'),
            lambda x: x['withdrawal_retry'].update(code='ADMITTED'),
            lambda x: x['reconciliation']['Q'].update(actual_vault_raw='9000000'),
            lambda x: x['reconciliation']['Q']['ledger'].update(pending=1),
            lambda x: x['reconciliation']['TOKENA']['ledger'].update(protocol_available=1),
            lambda x: x['fees'].update(total_stable_atoms=0))
        for change in changes:
            value, report, policy, pin = roundtrip()
            change(value)
            with self.subTest(change=change), self.assertRaises(CustodyError):
                validate_roundtrip_report(value, 'USDT0', pin, report, policy)

    def test_sponsored_wallet_scope_requires_explicit_eoa_profile_and_refuses_broader_claims(self):
        value, report, policy, pin = roundtrip()
        validate_roundtrip_report(value, 'USDT0', pin, report, policy)
        changes = (
            lambda x: x.pop('production_contract_or_delegated_wallets_qualified'),
            lambda x: x.update(production_contract_or_delegated_wallets_qualified=True),
            lambda x: x.update(production_contract_or_delegated_wallets_qualified=0),
            lambda x: x['sponsored_deposits'][0].pop('source_wallet_profile'),
            lambda x: x['sponsored_deposits'][1].pop('source_wallet_profile'),
            lambda x: x['sponsored_deposits'][0].update(source_wallet_profile='eip7702-delegated'),
            lambda x: x['sponsored_deposits'][1].update(source_wallet_profile='erc1271-contract'),
            lambda x: x['sponsored_deposits'][1].update(source_wallet_profile=None))
        for change in changes:
            changed = copy.deepcopy(value)
            change(changed)
            with self.subTest(change=change), self.assertRaises(CustodyError):
                validate_roundtrip_report(changed, 'USDT0', pin, report, policy)

    def test_source_manifest_explicitly_covers_opt_in_solidity_and_readme(self):
        value = {'files': {name: '11' * 32 for name in REQUIRED_SOURCE}}
        require_source_coverage(value)
        for name in ('contrib/flowmesh2/rehearsal/RebirthArbitrumFork.t.sol', 'contrib/flowmesh2/rehearsal/README.txt',
                     'contrib/b3bridge/peer/b3_peer_collector.nim', 'src/bridge/peercheck.cpp',
                     'contrib/b3bridge/peer/test_native_boundary.py'):
            changed = copy.deepcopy(value)
            changed['files'].pop(name)
            with self.assertRaises(CustodyError):
                require_source_coverage(changed)

    def test_frozen_source_or_worker_change_cannot_pass(self):
        require_frozen_hashes('11' * 32, '11' * 32, '22' * 32, '22' * 32)
        for hashes in (('11' * 32, '33' * 32, '22' * 32, '22' * 32),
                       ('11' * 32, '11' * 32, '22' * 32, '44' * 32), ('x' * 64,) * 4):
            with self.assertRaises(CustodyError):
                require_frozen_hashes(*hashes)

    def test_evidence_must_be_fresh_and_outside_source(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'source'
            source.mkdir()
            fresh_evidence_path(source, Path(directory) / 'fresh')
            for target in (source, source / 'nested', Path(directory)):
                with self.subTest(target=target), self.assertRaises(CustodyError):
                    fresh_evidence_path(source, target)

    def test_preflight_errors_redact_private_urls_and_do_not_start_gates(self):
        with tempfile.TemporaryDirectory() as directory:
            source, build = Path(directory) / 'source', Path(directory) / 'build'
            source.mkdir()
            build.mkdir()
            args = ['--source-root', str(source), '--build-dir', str(build), '--evidence-dir', str(Path(directory) / 'evidence'),
                '--provider-config', 'private-config.json', '--finality-state', str(Path(directory) / 'state.json'),
                '--fork-provider', 'first', '--asan-build', str(build), '--tsan-build', str(build)]
            error = io.StringIO()
            with mock.patch('qualify_arbitrum_prelaunch.load_public_report', return_value={}), \
                    mock.patch('qualify_arbitrum_prelaunch.qualification_environment',
                               side_effect=CustodyError('https://provider.example/private-secret')), \
                    mock.patch('qualify_arbitrum_prelaunch.subprocess.run') as execute, \
                    contextlib.redirect_stderr(error):
                self.assertEqual(main(args), 1)
            self.assertNotIn('private-secret', error.getvalue())
            execute.assert_not_called()
            self.assertFalse((Path(directory) / 'evidence').exists())

    @staticmethod
    def environment_config():
        _report, policy, _pin = fixture_inputs()
        return {'format': 'fm2-arbitrum-read-only-config/1', 'policy': policy,
                'providers': {'first': {'url_env': 'B3_ARB_FIRST_RPC'}, 'second': {'url_env': 'B3_ARB_SECOND_RPC'}}}

    @staticmethod
    def provider_environment():
        return {'B3_ARB_FIRST_RPC': 'https://first.example/rpc', 'B3_ARB_SECOND_RPC': 'https://second.example/rpc'}

    def test_minimal_environment_excludes_inherited_suite_filters_and_loader_overrides(self):
        inherited = dict(os.environ, **self.provider_environment(), FOUNDRY_PROFILE='narrow', FOUNDRY_TEST='empty',
            FOUNDRY_MATCH_TEST='one_case', CMAKE_BUILD_PARALLEL_LEVEL='99', CTEST_TEST_ARGS='-R nothing',
            BOOST_TEST_RUN_FILTERS='one_case', PYTHONPATH='untrusted_modules', LD_PRELOAD='untrusted_library',
            SSL_CERT_FILE='untrusted_ca', HTTPS_PROXY='https://untrusted.example', UNRELATED_SECRET='never_export')
        clean = qualification_environment(self.environment_config(), inherited)
        for name in ('FOUNDRY_PROFILE', 'FOUNDRY_TEST', 'FOUNDRY_MATCH_TEST', 'CMAKE_BUILD_PARALLEL_LEVEL',
                     'CTEST_TEST_ARGS', 'BOOST_TEST_RUN_FILTERS', 'PYTHONPATH', 'LD_PRELOAD', 'SSL_CERT_FILE',
                     'HTTPS_PROXY', 'UNRELATED_SECRET'):
            self.assertNotIn(name, clean)
        self.assertEqual(clean['PATH'], inherited['PATH'])
        self.assertEqual(clean['B3_ARB_FIRST_RPC'], inherited['B3_ARB_FIRST_RPC'])
        self.assertEqual(clean['PYTHONDONTWRITEBYTECODE'], '1')
        self.assertTrue(all(name.upper() in RUNTIME_ENV or name in
                            ('B3_ARB_FIRST_RPC', 'B3_ARB_SECOND_RPC', 'PYTHONDONTWRITEBYTECODE') for name in clean))

    def test_provider_variable_cannot_smuggle_tool_loader_or_runtime_controls(self):
        for name in ('FOUNDRY_PROFILE', 'FOUNDRY_TEST', 'FOUNDRY_MATCH_TEST', 'CMAKE_GENERATOR', 'CTEST_TEST_ARGS',
                     'BOOST_TEST_RUN_FILTERS', 'PYTHONPATH', 'LD_PRELOAD', 'SSL_CERT_FILE', 'BASH_ENV', 'HOME', 'PATH'):
            config = self.environment_config()
            config['providers']['first']['url_env'] = name
            inherited = dict(self.provider_environment(), **{name: 'https://first.example/rpc'})
            with self.subTest(name=name), self.assertRaises(CustodyError):
                qualification_environment(config, inherited)

    def test_isolation_covers_actual_nested_child_and_restores_after_failure(self):
        inherited = dict(os.environ, **self.provider_environment(), FOUNDRY_MATCH_TEST='one_case',
                         CTEST_TEST_ARGS='-R nothing', UNRELATED_SECRET='never_export')
        clean = qualification_environment(self.environment_config(), inherited)
        with mock.patch.dict(os.environ, inherited, clear=True):
            before = dict(os.environ)
            with self.assertRaisesRegex(RuntimeError, 'fixture failed'):
                with isolated_environment(clean):
                    # Like fixture.run -> build/Forge, this child deliberately
                    # has no env= argument and must still inherit the clean scope.
                    result = subprocess.check_output([sys.executable, '-c',
                        'import json,os; print(json.dumps({k:(k in os.environ) for k in '
                        '["FOUNDRY_MATCH_TEST","CTEST_TEST_ARGS","UNRELATED_SECRET","B3_ARB_FIRST_RPC"]}))'],
                        text=True, timeout=10)
                    self.assertEqual(json.loads(result), {'FOUNDRY_MATCH_TEST': False, 'CTEST_TEST_ARGS': False,
                                                          'UNRELATED_SECRET': False, 'B3_ARB_FIRST_RPC': True})
                    raise RuntimeError('fixture failed')
            self.assertEqual(dict(os.environ), before)


if __name__ == '__main__':
    unittest.main(verbosity=2)
