#!/usr/bin/env python3
"""Full frozen-source regtest gates plus read-only Arbitrum/fork rehearsals.

Every existing network release gate remains required. Real Arbitrum access is
read-only; all minting, sponsorship and payouts execute on owned Anvil31337
forks. Passing this suite cannot activate a live native profile or mainnet.
Private child state/logs stay separate from the curated public evidence.
"""
import argparse
import copy
from contextlib import contextmanager
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import re

from arbitrum_assets import capture_asset_pin
from arbitrum_plan import build_disabled_plan, validate_disabled_plan
from arbitrum_rpc import strict_json
from custody import Cast, CustodyError, atomic_json, hex_value, uint
from qualify_arbitrum_read_only import providers_from_environment
from qualify_regtest import sha256, source_manifest
from source_finality import SourceFinality, canonical_hash, qualify_read_only
from arbitrum_rpc import HttpsReadOnlyRpc


# Framework, loader, test-filter, proxy and CA overrides are intentionally absent.
# The same environment governs direct fixture.run() calls and every child they
# start; passing env= only to the outer qualification command is insufficient.
RUNTIME_ENV = frozenset(('PATH', 'HOME', 'USER', 'LOGNAME', 'LANG', 'LC_ALL', 'LC_CTYPE',
    'LC_MESSAGES', 'LC_NUMERIC', 'LC_COLLATE', 'LC_TIME', 'LC_MONETARY', 'TMPDIR', 'TMP',
    'TEMP', 'TZ', 'SYSTEMROOT', 'WINDIR', 'COMSPEC', 'PATHEXT', 'USERPROFILE', 'USERNAME',
    'HOMEDRIVE', 'HOMEPATH', 'APPDATA', 'LOCALAPPDATA', 'PROGRAMDATA', 'PROGRAMFILES',
    'PROGRAMFILES(X86)', 'COMMONPROGRAMFILES', 'COMMONPROGRAMFILES(X86)',
    'PROCESSOR_ARCHITECTURE', 'NUMBER_OF_PROCESSORS', 'OS'))
PROVIDER_OVERRIDE_PREFIXES = ('FOUNDRY', 'DAPP_', 'CMAKE', 'CTEST', 'BOOST_', 'GTEST_',
    'PYTHON', 'PYTEST', 'LD_', 'DYLD_', 'ASAN_', 'LSAN_', 'TSAN_', 'MSAN_', 'UBSAN_',
    'LLVM_', 'GCOV_', 'COVERAGE_', 'CARGO_', 'RUST', 'GIT_', 'SSL_', 'OPENSSL_',
    'HTTP_', 'HTTPS_', 'ALL_PROXY', 'NO_PROXY', 'LC_', 'FM2_')
PROVIDER_OVERRIDE_NAMES = frozenset(('CC', 'CXX', 'CPP', 'AR', 'AS', 'LD', 'RANLIB',
    'STRIP', 'OBJCOPY', 'OBJDUMP', 'MAKEFLAGS', 'MFLAGS', 'SHELL', 'BASH_ENV', 'ENV',
    'IFS', 'SHELLOPTS', 'BASHOPTS', 'NODE_OPTIONS', 'CURL_HOME', 'REQUESTS_CA_BUNDLE'))


def qualification_environment(config, environ=None):
    """Return only normal runtime variables and exact identity-pinned URL vars."""
    source = dict(os.environ if environ is None else environ)
    if (not isinstance(config, dict) or set(config) != {'format', 'policy', 'providers'} or
            config.get('format') != 'fm2-arbitrum-read-only-config/1'):
        raise CustodyError('unknown qualification provider configuration')
    policy = SourceFinality.validate_policy(config['policy'])
    configured = config['providers']
    if (policy['format'] != 'fm2-source-finality-policy/2' or not isinstance(configured, dict) or
            set(configured) != set(policy['provider_ids'])):
        raise CustodyError('qualification provider environment differs from policy')
    clean, seen = {}, set()
    for name, value in source.items():
        if name.upper() in RUNTIME_ENV:
            if name.upper() in seen:
                raise CustodyError('runtime environment has ambiguous variable aliases')
            seen.add(name.upper())
            clean[name] = value
    provider_names = set()
    for label in policy['provider_ids']:
        item = configured[label]
        if not isinstance(item, dict) or set(item) != {'url_env'}:
            raise CustodyError('qualification provider requires exactly one named URL variable')
        name = item['url_env']
        if (not isinstance(name, str) or not re.fullmatch(r'[A-Z][A-Z0-9_]{0,127}', name) or
                name in RUNTIME_ENV or name in PROVIDER_OVERRIDE_NAMES or
                name.startswith(PROVIDER_OVERRIDE_PREFIXES) or name in provider_names):
            raise CustodyError('provider URL variable collides with runtime/tool/test controls')
        url = source.get(name)
        if not isinstance(url, str) or not url:
            raise CustodyError('missing named provider URL/credentials')
        binding = policy['provider_bindings'][label]
        rpc = HttpsReadOnlyRpc(url, label, binding['operator_id'], binding['endpoint_id'],
                              chain_id=policy['chain_id'], genesis_hash=policy['genesis_hash'])
        if rpc.binding != binding:
            raise CustodyError('named provider URL differs from the exact policy endpoint')
        provider_names.add(name)
        clean[name] = url
    clean['PYTHONDONTWRITEBYTECODE'] = '1'
    return clean


@contextmanager
def isolated_environment(clean):
    """Cover nested in-process fixtures as well as direct subprocess calls."""
    previous = dict(os.environ)
    os.environ.clear()
    os.environ.update(clean)
    try:
        yield
    finally:
        os.environ.clear()
        os.environ.update(previous)


LEGACY_GATES = ('asan-ubsan', 'tsan', 'legacy-regressions', 'all-custody-contracts',
    'tls-boundary', 'rpc-batch-boundary', 'private-identity-update', 'automatic-custody-protocol',
    'automatic-batching-protocol', 'certified-history-protocol', 'relayer-capacity', 'runtime-guard-order',
    'custody-grouped-snapshot', 'b3-rpc-batch-boundary', 'finality-and-relayer-codec', 'custody-proxy-upgrade',
    'signed-relayer-process-recovery', 'signed-custody-roundtrip', 'network-roundtrip-and-recovery',
    'network-trading-latency')
OFFLINE_TESTS = ('test_arbitrum_rpc', 'test_arbitrum_finality', 'test_arbitrum_assets', 'test_custody_compliance',
    'test_arbitrum_plan', 'test_arbitrum_fork', 'test_arbitrum_rehearsal', 'test_arbitrum_prelaunch',
    'test_atomic_json_failure')
REQUIRED_SOURCE = tuple('contrib/flowmesh2/regtest/' + name for name in (
    'arbitrum_rpc.py', 'source_finality.py', 'qualify_arbitrum_read_only.py', 'arbitrum_assets.py',
    'arbitrum_fork.py', 'arbitrum_plan.py', 'arbitrum_rehearsal.py', 'run_arbitrum_roundtrip.py',
    'qualify_arbitrum_prelaunch.py', *(name + '.py' for name in OFFLINE_TESTS))) + (
    'contracts/custody/RebirthCustodyVault.sol', 'contrib/flowmesh2/rehearsal/RebirthArbitrumFork.t.sol',
    'contrib/flowmesh2/rehearsal/README.txt',
    'src/bridge/peer_light_client.h', 'src/bridge/peercheck.cpp',
    'src/test/bridge_peer_light_client_tests.cpp', 'src/CMakeLists.txt', 'src/test/CMakeLists.txt',
    *( 'contrib/b3bridge/peer/' + name for name in (
        'README.txt', 'b3_peer_collector.nim', 'build_peer_collector.py',
        'run_peer_light_client.py', 'test_peer_adapter.py', 'test_native_boundary.py')))
FORGE_TESTS = frozenset(('test_usdt0_exact_deposit_donation_claim_payout_and_replay',
    'test_usdc_exact_deposit_donation_claim_payout_and_replay',
    'test_usdt0_issuer_blocklist_retains_claim_and_same_id_retries',
    'test_usdc_issuer_blocklist_retains_claim_and_same_id_retries',
    'test_usdc_issuer_pause_retains_claim_and_same_id_retries',
    'test_usdt0_issuer_destroyed_reserves_fail_closed_preserving_obligation',
    'test_usdt0_sponsored_zero_eth_source_no_approve_and_token_nonce_replay_refused',
    'test_usdc_sponsored_zero_eth_source_no_approve_and_token_nonce_replay_refused',
    'test_usdt0_sponsored_account_amount_salt_source_time_vault_substitution_refuses',
    'test_usdc_sponsored_account_amount_salt_source_time_vault_substitution_refuses',
    'test_usdt0_sponsored_expired_future_wrong_chain_refuses',
    'test_usdc_sponsored_expired_future_wrong_chain_refuses',
    'test_usdt0_sponsored_issuer_blocklist_refuses_without_nonce_or_credit_loss',
    'test_usdc_sponsored_issuer_blocklist_refuses_without_nonce_or_credit_loss'))


def load_public_report(path, maximum=8 * 1024 * 1024):
    path = Path(path).absolute()
    SourceFinality._safe_path(path)
    with path.open('rb') as stream:
        raw = stream.read(maximum + 1)
    if len(raw) > maximum:
        raise CustodyError('qualification artifact exceeds its explicit bound')
    return strict_json(raw.decode('utf-8'))


def validate_legacy_report(value, source_hash, worker_hash):
    if (not isinstance(value, dict) or value.get('format') != 'fm2-network-regtest-qualification/1' or
            value.get('passed') is not True or value.get('mainnet_ready') is not False or
            value.get('mainnet_actions') != [] or value.get('source_tree_sha256') != source_hash or
            value.get('worker_sha256') != worker_hash):
        raise CustodyError('original frozen-source network qualification did not pass under disabled mainnet')
    gates = value.get('gates')
    if (not isinstance(gates, list) or len(gates) != len(LEGACY_GATES) or
            tuple(item.get('label') for item in gates if isinstance(item, dict)) != LEGACY_GATES or
            any(item.get('passed') is not True or type(item.get('exit_code')) is not int or item['exit_code'] != 0 for item in gates)):
        raise CustodyError('all original20 network qualification gates must run and pass in their original order')
    return {'format': value['format'], 'passed': True, 'gate_count': len(gates),
            'gate_labels': list(LEGACY_GATES), 'source_tree_sha256': source_hash, 'worker_sha256': worker_hash}


def forge_rehearsal_summary(value, local_height, local_hash):
    """Require actual explicit14 Forge successes; an exit0/empty match is insufficient."""
    uint(local_height, 64)
    hash_ = hex_value(local_hash, 32)
    if not isinstance(value, dict) or len(value) != 1:
        raise CustodyError('opt-in Forge rehearsal must return exactly its selected suite')
    suite_name, suite = next(iter(value.items()))
    if (not isinstance(suite_name, str) or not suite_name.endswith(':RebirthArbitrumForkTest') or
            'RebirthArbitrumFork.t.sol' not in suite_name or not isinstance(suite, dict) or
            not isinstance(suite.get('test_results'), dict)):
        raise CustodyError('opt-in Forge rehearsal returned a different contract suite')
    tests = suite['test_results']
    normalized = {name.removesuffix('()'): item for name, item in tests.items() if isinstance(name, str)}
    if (len(normalized) != len(tests) or set(normalized) != FORGE_TESTS or
            any(not isinstance(item, dict) or item.get('status') != 'Success' for item in normalized.values())):
        raise CustodyError('all14 real-token Forge tests must pass with no missing/failed/skipped cases')
    return {'format': 'fm2-arbitrum-contract-rehearsal/1', 'passed': True, 'execution_chain_id': 31337,
        'mainnet_enabled': False, 'local_fork_block': {'height': local_height, 'hash': hash_},
        'tests': [{'name': name, 'status': 'Success'} for name in sorted(normalized)], 'test_count': 14,
        'skipped': 0, 'scope': 'ephemeral Forge VM on the owned local31337 fork; no live chain execution'}


def validate_roundtrip_report(value, asset, pin, finality_report, policy):
    if (not isinstance(value, dict) or value.get('format') != 'fm2-arbitrum-four-operator-rehearsal/1' or
            value.get('passed') is not True or any(value.get(field) is not False for field in
                ('mainnet_enabled', 'mainnet_ready', 'production_activation', 'live_protocol_end_to_end_qualified',
                 'live_source_finality_qualified_by_this_run', 'production_latency_qualified',
                 'production_contract_or_delegated_wallets_qualified')) or
            value.get('source_chain_id') != 42161 or value.get('execution_chain_id') != 31337 or
            value.get('asset') != asset or value.get('token') != pin['identity']['token'] or
            value.get('source_origin') != finality_report['checkpoint'] or
            value.get('profile_name') != 'fm2-regtest-stable-q/3' or
            value.get('custody_profile') != 'fm2-custody-v2-regtest/1' or
            value.get('input_hashes') != {'policy_sha256': canonical_hash(policy), 'asset_pin_sha256': canonical_hash(pin),
                                        'finality_report_sha256': canonical_hash(finality_report)} or
            value.get('fees') != {'trade_atoms': 1200, 'withdrawal_atoms': 1000, 'total_stable_atoms': 2200}):
        raise CustodyError('real-token roundtrip did not retain its exact disabled fork/profile/provenance/fee scope')
    contract = value.get('contracts_rehearsal')
    if (not isinstance(contract, dict) or contract.get('passed') is not True or contract.get('test_count') != 14 or
            contract.get('skipped') != 0 or contract.get('execution_chain_id') != 31337 or
            contract.get('mainnet_enabled') is not False):
        raise CustodyError('four-operator fork rehearsal requires its actual14-test contract gate')
    compiler = contract.get('compiler_provenance')
    if not isinstance(compiler, dict):
        raise CustodyError('actual Forge/compiler/configuration provenance is required')
    for field in ('forge_sha256', 'solc_sha256', 'foundry_toml_sha256', 'resolved_configuration_sha256'):
        value_ = compiler.get(field)
        if not isinstance(value_, str) or len(value_) != 64 or any(c not in '0123456789abcdef' for c in value_):
            raise CustodyError('actual Forge/compiler/configuration hashes are incomplete')
    settings = compiler.get('resolved_configuration')
    artifacts = compiler.get('artifacts')
    if (not isinstance(settings, dict) or settings.get('via_ir') is not True or
            not isinstance(settings.get('evm_version'), str) or not settings['evm_version'] or
            not isinstance(artifacts, dict) or set(artifacts) != {'test_suite', 'deployed_vault'}):
        raise CustodyError('actual resolved local EVM and both compiler artifact settings are required')
    for artifact in artifacts.values():
        if (not isinstance(artifact, dict) or not isinstance(artifact.get('artifact_sha256'), str) or
                len(artifact['artifact_sha256']) != 64 or not isinstance(artifact.get('compiler_version'), str) or
                not isinstance(artifact.get('settings'), dict) or
                not isinstance(artifact['settings'].get('evmVersion'), str)):
            raise CustodyError('actual compiler artifact metadata is incomplete')
    deposits = value.get('sponsored_deposits')
    if (not isinstance(deposits, list) or len(deposits) != 2 or
            any(not isinstance(item, dict) or item.get('user_eth_before') != '0' or item.get('user_eth_after') != '0' or
                item.get('source_wallet_profile') != 'code-empty-eoa-vrs-only'
                for item in deposits)):
        raise CustodyError('both source users must complete actual sponsor-paid deposits with zero ETH under the code-empty EOA wallet profile')
    retry = value.get('withdrawal_retry')
    if not isinstance(retry, dict) or retry.get('code') != 'DUPLICATE' or retry.get('included') is not True:
        raise CustodyError('withdrawal retry must preserve the exact included claim')
    reconciliation = value.get('reconciliation')
    if not isinstance(reconciliation, dict) or set(reconciliation) != {'Q', 'TOKENA', 'TOKENB'}:
        raise CustodyError('all three reserve routes must be reconciled')
    for name, item in reconciliation.items():
        try:
            ledger = item['ledger']
            if (uint(item['actual_vault_raw']) != uint(item['locked_raw']) or
                    uint(item['locked_raw']) != uint(ledger['total_liabilities']) or uint(ledger['pending']) != 0 or
                    uint(ledger['protocol_available']) != (2200 if name == 'Q' else 0)):
                raise CustodyError('fork reserves/fees differ from total native liabilities')
        except (KeyError, TypeError):
            raise CustodyError('fork reserve reconciliation is incomplete') from None
    return {'asset': asset, 'passed': True, 'execution_chain_id': 31337, 'source_origin': pin['origin'],
            'real_token': pin['identity']['token'], 'contract_tests': 14, 'stable_fee_atoms': 2200,
            'mainnet_enabled': False, 'mainnet_ready': False}


def fresh_evidence_path(source, evidence):
    source, evidence = Path(source).resolve(strict=True), Path(evidence).absolute()
    if evidence == source or source in evidence.parents or evidence.exists():
        raise CustodyError('qualification evidence must be a fresh directory outside source; retained state is never erased')
    for parent in evidence.parents:
        if parent.exists() and (parent.is_symlink() or getattr(parent.lstat(), 'st_file_attributes', 0) & 0x400):
            raise CustodyError('qualification evidence cannot traverse symlinks/reparse points')
    return evidence


def require_source_coverage(manifest):
    if (not isinstance(manifest, dict) or not isinstance(manifest.get('files'), dict) or
            any(not isinstance(manifest['files'].get(name), str) or len(manifest['files'][name]) != 64
                for name in REQUIRED_SOURCE)):
        raise CustodyError('frozen source manifest must hash every new adapter, test, opt-in Solidity source and README')
    return manifest


def require_frozen_hashes(source_before, source_after, worker_before, worker_after):
    hashes = (source_before, source_after, worker_before, worker_after)
    if (any(not isinstance(value, str) or len(value) != 64 or any(c not in '0123456789abcdef' for c in value)
            for value in hashes) or source_before != source_after or worker_before != worker_after):
        raise CustodyError('source/worker changed during prelaunch qualification; require one frozen release')


UNIT_RUNNER = '''import json,sys,unittest
from custody import atomic_json
names=sys.argv[2:]
suites=[unittest.defaultTestLoader.loadTestsFromName(name) for name in names]
counts={name:suite.countTestCases() for name,suite in zip(names,suites)}
suite=unittest.TestSuite(suites)
result=unittest.TextTestRunner(verbosity=2).run(suite)
passed=result.wasSuccessful() and all(counts.values()) and result.testsRun==sum(counts.values()) and not result.skipped
atomic_json(sys.argv[1], {'format':'fm2-arbitrum-offline-tests/1','passed':passed,'tests':result.testsRun,
 'skipped':len(result.skipped),'failures':len(result.failures),'errors':len(result.errors),'modules':names,'module_counts':counts})
raise SystemExit(0 if passed else 1)
'''


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ('source-root', 'build-dir', 'evidence-dir', 'provider-config', 'finality-state', 'fork-provider'):
        parser.add_argument('--' + flag, required=True)
    parser.add_argument('--b3-node-build', default='/home/josh/B3-builds/regtest-b3-node')
    parser.add_argument('--tools', default='/home/josh/B3-tools/foundry-v1.8.5')
    parser.add_argument('--solc', default='/home/josh/B3-tools/solc-0.8.35')
    parser.add_argument('--asan-build', required=True)
    parser.add_argument('--tsan-build', required=True)
    args = parser.parse_args(argv)
    try:
        config = load_public_report(args.provider_config, 65536)
        env = qualification_environment(config)
    except Exception:
        print('prelaunch configuration/environment refused; check private provider configuration and named URL variables', file=sys.stderr)
        return 1
    with isolated_environment(env):
        return _qualify(args, config, env)


def _qualify(args, config, env):
    try:
        source, build = Path(args.source_root).resolve(strict=True), Path(args.build_dir).resolve(strict=True)
        evidence = fresh_evidence_path(source, args.evidence_dir)
        state = Path(args.finality_state).absolute()
        if state == source or source in state.parents or state == evidence or evidence in state.parents:
            raise CustodyError('retain finality state outside source and fresh evidence')
        SourceFinality._safe_path(state)
        policy, providers = providers_from_environment(config)
        if args.fork_provider not in policy['provider_ids']:
            raise CustodyError('fork provider must be in the exact independent read-only policy')
        source_hash = require_source_coverage(source_manifest(source))
        worker = build / 'bin/fm2-regtest-node'
        worker_hash = sha256(worker)
        peer_binaries = {name: build / 'bin' / name for name in ('b3-bridge-peercheck', 'test_bitcoin')}
        peer_hashes = {name: sha256(path) for name, path in peer_binaries.items()}
        evidence.mkdir(mode=0o700, parents=True)
        private, public = evidence / 'private', evidence / 'public'
        private.mkdir(mode=0o700)
        public.mkdir(mode=0o700)
    except Exception:
        print('prelaunch configuration/source/evidence refused; check private paths and named provider environment variables', file=sys.stderr)
        return 1
    driver = source / 'contrib/flowmesh2/regtest'
    report = {'format': 'fm2-arbitrum-prelaunch-qualification/1', 'passed': False,
        'mainnet_ready': False, 'mainnet_enabled': False, 'mainnet_actions': [],
        'started_unix_seconds': int(time.time()), 'source_tree_sha256_before': source_hash['sha256'],
        'worker_sha256_before': worker_hash, 'gates': [], 'artifacts': {},
        'peer_binaries_sha256_before': peer_hashes,
        'ethereum_peer_scope': 'native BLS/SSZ and process-boundary tests only; Ethereum finality is not Arbitrum source authority',
        'remaining_blockers': [{'asset': asset, 'blockers': build_disabled_plan(asset, finality_policy=policy)['unresolved_blockers']}
                               for asset in ('USDT0', 'USDC')],
        'provider_prerequisites': {'independent_https_operators': 'two or more required',
            'historical_state': 'every provider must serve code, storage and eth_call at the exact finalized checkpoint',
            'archive_credentials': 'configure named environment URLs as needed; unavailable archive data never falls back to one provider'},
        'scope': 'unchanged20-gate regtest release plus independent Arbitrum HTTPS reads and isolated real-token forks',
        'live_native_profile_qualified': False, 'live_end_to_end_qualified': False,
        'production_latency_claimed': False, 'source_genesis_policy': policy['genesis_hash']}
    report['environment_policy'] = 'minimal-runtime-plus-exact-pinned-provider-urls/1'

    def command_gate(label, command, timeout):
        print('Checking ' + label + '...', flush=True)
        started = time.monotonic_ns()
        log = private / (label + '.log')
        with log.open('xb') as stream:
            completed = subprocess.run([str(item) for item in command], cwd=driver, env=env,
                stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
        gate = {'label': label, 'passed': completed.returncode == 0, 'exit_code': completed.returncode,
            'elapsed_ms': (time.monotonic_ns() - started) / 1_000_000, 'private_log_sha256': sha256(log)}
        report['gates'].append(gate)
        if completed.returncode:
            raise CustodyError(label + ' failed')

    def function_gate(label, function):
        print('Checking ' + label + '...', flush=True)
        started = time.monotonic_ns()
        gate = {'label': label, 'passed': False}
        report['gates'].append(gate)
        try:
            result = function()
            gate['passed'] = True
            return result
        finally:
            gate['elapsed_ms'] = (time.monotonic_ns() - started) / 1_000_000

    def publish(name, value):
        path = public / name
        if path.exists():
            raise CustodyError('public qualification artifact already exists')
        atomic_json(path, value)
        report['artifacts'][name] = sha256(path)
        return path

    try:
        publish('source-manifest.json', source_hash)
        policy_path = publish('source-finality-policy.json', policy)
        peer_driver = source / 'contrib/b3bridge/peer'
        command_gate('ethereum-native-peer-proofs', [peer_binaries['test_bitcoin'],
            '--run_test=bridge_peer_light_client_tests', '--report_level=detailed', '--log_level=test_suite'], 600)
        peer_runner = 'import sys; sys.path.insert(0, ' + repr(str(peer_driver)) + ')\n' + UNIT_RUNNER
        command_gate('ethereum-peer-adapter-boundaries', [sys.executable, '-B', '-c', peer_runner,
            private / 'peer-offline-result.json', 'test_peer_adapter'], 600)
        peer_offline = load_public_report(private / 'peer-offline-result.json')
        if (peer_offline.get('passed') is not True or peer_offline.get('skipped') != 0 or
                peer_offline.get('modules') != ['test_peer_adapter']):
            raise CustodyError('all Ethereum peer adapter boundaries must execute with no skips')
        publish('peer-offline-summary.json', peer_offline)
        command_gate('ethereum-native-cli-boundaries', [sys.executable, '-B', peer_driver / 'test_native_boundary.py',
            '--peercheck', peer_binaries['b3-bridge-peercheck']], 600)
        command_gate('original-network20-gates', [sys.executable, '-B', driver / 'qualify_network.py',
            '--source-root', source, '--build-dir', build, '--evidence-dir', private / 'network-regtest',
            '--b3-node-build', args.b3_node_build, '--tools', args.tools, '--solc', args.solc,
            '--asan-build', args.asan_build, '--tsan-build', args.tsan_build], 21600)
        legacy = validate_legacy_report(load_public_report(private / 'network-regtest/qualification-report.json'),
                                        source_hash['sha256'], worker_hash)
        publish('original-network-summary.json', legacy)
        command_gate('all-new-offline-boundaries', [sys.executable, '-B', '-c', UNIT_RUNNER,
            private / 'offline-test-result.json', *OFFLINE_TESTS], 600)
        offline = load_public_report(private / 'offline-test-result.json')
        if (offline.get('passed') is not True or offline.get('skipped') != 0 or
                offline.get('modules') != list(OFFLINE_TESTS)):
            raise CustodyError('all new offline boundaries must execute with no skips')
        publish('offline-test-summary.json', offline)
        finality = function_gate('independent-arbitrum-finalized-reads', lambda:
                                 qualify_read_only(policy, providers, state))
        finality_path = publish('arbitrum-read-only-finality.json', finality)
        cast = Cast(Path(args.tools) / 'cast')
        for asset in ('USDT0', 'USDC'):
            def capture(asset=asset):
                pins = [capture_asset_pin(providers[label], cast, asset, finality['checkpoint'], execution_chain_id=42161)
                        for label in policy['provider_ids']]
                if len({canonical_hash(pin) for pin in pins}) != 1:
                    raise CustodyError('independent providers disagree on exact real-token proxy/admin/issuer identity')
                with SourceFinality(policy, providers, state) as verifier:
                    verifier.verify(finality['checkpoint'], persist=True)
                return pins[0]
            pin = function_gate('independent-' + asset.lower() + '-identity-pins', capture)
            pin_path = publish(asset.lower() + '-asset-pin.json', pin)
            plan = validate_disabled_plan(build_disabled_plan(asset, asset_pin=pin, finality_policy=policy))
            publish(asset.lower() + '-disabled-plan.json', plan)
            next(item for item in report['remaining_blockers'] if item['asset'] == asset)['blockers'] = plan['unresolved_blockers']
            def roundtrip(asset=asset, pin=pin, pin_path=pin_path):
                from run_arbitrum_roundtrip import ArbitrumRoundtrip
                url_env = config['providers'][args.fork_provider]['url_env']
                fixture_args = argparse.Namespace(source_root=str(source), evidence_dir=str(private / (asset.lower() + '-four-operator')),
                    worker=str(worker), b3coind=str(Path(args.b3_node_build) / 'bin/b3coind'),
                    b3coin_cli=str(Path(args.b3_node_build) / 'bin/b3coin-cli'), tools=args.tools, solc=args.solc,
                    finality_report=str(finality_path), finality_policy=str(policy_path), asset_pin=str(pin_path),
                    fork_provider=args.fork_provider, fork_url_env=url_env, contracts_rehearsal=True)
                fixture = ArbitrumRoundtrip(fixture_args)
                try:
                    result = fixture.run()
                    stored = load_public_report(fixture.evidence / 'arbitrum-roundtrip-report.json', 32 * 1024 * 1024)
                    if stored != result:
                        raise CustodyError('stored actual fork report differs from returned execution result')
                    summary = validate_roundtrip_report(result, asset, pin, finality, policy)
                    publish(asset.lower() + '-four-operator-report.json', result)
                    return summary
                finally:
                    fixture.close()
            summary = function_gate(asset.lower() + '-contract14-and-four-operator-roundtrip', roundtrip)
            publish(asset.lower() + '-roundtrip-summary.json', summary)
        report['source_tree_sha256_after'] = source_manifest(source)['sha256']
        report['worker_sha256_after'] = sha256(worker)
        require_frozen_hashes(report['source_tree_sha256_before'], report['source_tree_sha256_after'],
                             report['worker_sha256_before'], report['worker_sha256_after'])
        report['peer_binaries_sha256_after'] = {name: sha256(path) for name, path in peer_binaries.items()}
        if report['peer_binaries_sha256_after'] != peer_hashes:
            raise CustodyError('native peer binaries changed during qualification')
        report['passed'] = True
        return 0
    except Exception as error:
        # Provider/process exception strings can contain private upstream URLs.
        report['failure_class'] = type(error).__name__
        report['failure_gate'] = report['gates'][-1]['label'] if report['gates'] else 'preflight'
        report['error'] = 'qualification refused; inspect retained private gate/process evidence'
        print(report['error'], file=sys.stderr)
        return 1
    finally:
        report['finished_unix_seconds'] = int(time.time())
        if 'source_tree_sha256_after' not in report:
            try:
                report['source_tree_sha256_after'] = source_manifest(source)['sha256']
                report['worker_sha256_after'] = sha256(worker)
            except Exception:
                report['post_run_hashes_unavailable'] = True
        atomic_json(public / 'qualification-report.json', report)
        print('Public report: ' + str(public / 'qualification-report.json'), flush=True)


if __name__ == '__main__':
    raise SystemExit(main())
