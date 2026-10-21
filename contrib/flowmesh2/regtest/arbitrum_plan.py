#!/usr/bin/env python3
"""Public, disabled Arbitrum planning artifact; never a runtime configuration.

This module has no RPC client, signer, deployment or broadcast operation. It
does not reinterpret the synthetic Q profiles or authorize a live deployment.
Recorded pins/policy hashes link planning inputs; they do not prove source
state, provider independence, security review or permission to operate.
"""
import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import sys

from custody import CustodyError
from arbitrum_assets import GENESIS_HASH, asset_spec, validate_asset_pin_shape
from source_finality import SourceFinality

FORMAT = 'fm2-arbitrum-disabled-plan/1'
MAX_BYTES = 262144
PRESERVED_PROFILES = ('fm2-regtest-stable-q/1', 'fm2-regtest-stable-q/2', 'fm2-regtest-stable-q/3')

# These are implementation/operational blockers, not a checklist that this
# artifact permits a caller to mark complete. A new qualified activation path
# needs its own reviewed version and cannot be enabled by editing this plan.
BLOCKERS = (
    ('native-profile-and-quote-binding',
     'Implement a separately versioned native profile and domain binding the exact real quote asset, '
     'source42161 policy, markets and deployment; preserve every existing regtest profile and store.',
     ['src/flowmesh2/core/params.cpp:Validate', 'src/flowmesh2/dev/regtest_operator.cpp:Impl']),
    ('source-finality-runtime-integration',
     'Integrate the reviewed Arbitrum finalized/provider policy into each operator observation, '
     'durable source marker, catchup and withdrawal authority; independent provider RPC is an explicit trust assumption.',
     ['contrib/flowmesh2/regtest/source_finality.py:SourceFinality',
      'contrib/flowmesh2/regtest/source_stream.py:SourceStream']),
    ('real-token-and-custody-release-policy',
     'Qualify exact token proxy/admin/issuer controls and a separately versioned vault policy, '
     'source genesis, deployment, roster epoch, pause and rotation lifecycle; a six-decimal token is not synthetic Q.',
     ['contrib/flowmesh2/regtest/custody_policy.py:validate_policy_shape',
      'contrib/flowmesh2/regtest/operator_wrapper.py:Operator._open']),
    ('arbitrum-gas-relayer',
     'Implement and qualify a separate Arbitrum gas policy and chain-bound signer/nonce recovery, '
     'fee bounds, funding and replacement handling; the current raw transaction journal permits chain31337 only.',
     ['contrib/flowmesh2/regtest/signed_relayer.py:DurableSignedRelayer']),
    ('operator-network-and-client-provenance',
     'Qualify production B3/network identity, independent operator hosts, signed endpoint distribution, '
     'TLS certificate and key rotation, client pin distribution and internet failure/load behavior.',
     ['contrib/flowmesh2/regtest/operator_wrapper.py:Operator._open',
      'contrib/flowmesh2/regtest/https_api.py:HttpsGateway']),
    ('signer-disaster-recovery-and-fencing',
     'Qualify host-loss backups, independent antirollback/fencing, original signer/control/commit/source/gas '
     'journal recovery, key custody and epoch transition; local checksums cannot detect a coherent old backup.',
     ['contrib/flowmesh2/regtest/README.txt:Limits and remaining activation work',
      'contrib/flowmesh2/regtest/signed_relayer.py:DurableSignedRelayer']),
    ('committee-custody-and-exit-model',
     'Review and accept the three-of-four custody trust model and its failure/recovery policy. The vault '
     'checks quorum signatures, not a native state proof; its delay and pause are not a permissionless fraud proof or exit.',
     ['contracts/custody/RebirthCustodyVault.sol:requestWithdrawal',
      'contracts/custody/RebirthCustodyVault.sol:finalizeWithdrawal']),
    ('stable-fees-and-source-gas-product-flow',
     'Qualify real stablecoin fee economics and gas sponsorship/product behavior. Current trading/service fees '
     'are synthetic Q; local EIP3009 deposits can use a native-gas sponsor, while production sponsorship '
     'pricing/funding and public wallet submission remain unqualified.',
     ['src/flowmesh2/core/params.cpp:StableQRegtestProfile',
      'contrib/flowmesh2/regtest/README.txt:Fees and timing']),
    ('independent-review-and-operational-activation',
     'Complete independent protocol/cryptographic/contract review, sustained adversarial and internet load tests, '
     'reconciliation/monitoring/incident drills and an explicitly approved separately qualified activation release.',
     ['contrib/flowmesh2/regtest/README.txt:Limits and remaining activation work']),
)


def _canonical(value):
    try:
        raw = json.dumps(value, sort_keys=True, separators=(',', ':'), allow_nan=False).encode('utf-8')
    except (TypeError, ValueError, RecursionError) as error:
        raise CustodyError('disabled plan requires bounded finite JSON') from error
    if len(raw) > MAX_BYTES:
        raise CustodyError('disabled plan exceeds its public artifact bound')
    return raw


def _hash(value):
    return hashlib.sha256(_canonical(value)).hexdigest()


def _public(value):
    """Refuse accidental credentials before including an input in a public plan."""
    stack = [(value, 0)]
    forbidden = {'seed', 'ikm', 'private_key', 'privatekey', 'secret', 'mnemonic', 'authorization',
                 'password', 'cookie', 'cookie_file', 'rpc_url', 'fork_url', 'forkurl', 'api_key'}
    while stack:
        item, depth = stack.pop()
        if depth > 16:
            raise CustodyError('disabled plan input nesting exceeds its bound')
        if isinstance(item, dict):
            for key, child in item.items():
                if not isinstance(key, str) or key.lower() in forbidden:
                    raise CustodyError('disabled plan accepts public input fields only')
                stack.append((child, depth + 1))
        elif isinstance(item, list):
            stack.extend((child, depth + 1) for child in item)
        elif not isinstance(item, (str, int, float, bool, type(None))):
            raise CustodyError('disabled plan input is not JSON')
    _canonical(value)


def _finality(policy):
    if policy is None:
        return None
    _public(policy)
    normalized = SourceFinality.validate_policy(policy)
    if (normalized['format'] != 'fm2-source-finality-policy/2' or normalized['chain_id'] != 42161 or
            normalized['genesis_hash'] != GENESIS_HASH or normalized['chain_kind'] != 'arbitrum-l2' or
            type(normalized['parent_chain_id']) is not int or normalized['parent_chain_id'] != 1 or
            normalized['mode'] != 'finalized' or type(normalized['minimum_confirmations']) is not int or
            normalized['minimum_confirmations'] != 0):
        raise CustodyError('Arbitrum planning requires the explicit disabled finalized HTTPS provider policy')
    return normalized


def build_disabled_plan(asset_key, *, asset_pin=None, finality_policy=None):
    """Build public planning data, without contacting a provider or touching keys."""
    spec = asset_spec(asset_key)
    pin = None
    if asset_pin is not None:
        _public(asset_pin)
        pin = validate_asset_pin_shape(asset_pin)
        if pin['asset'] != asset_key or pin['identity']['token'] != spec['address']:
            raise CustodyError('asset pin differs from the exact selected Arbitrum token')
    policy = _finality(finality_policy)
    if pin is not None and policy is not None and pin['genesis_hash'] != policy['genesis_hash']:
        raise CustodyError('asset pin and finalized provider policy belong to different source genesis blocks')
    result = {
        'format': FORMAT, 'status': 'blocked', 'mainnet_ready': False, 'mainnet_enabled': False,
        'signing_enabled': False, 'deployment_enabled': False, 'broadcast_enabled': False,
        'target': {'chain_id': 42161, 'chain_kind': 'arbitrum-l2', 'settlement_parent_chain_id': 1,
                   'asset': spec},
        'preserved_profiles': list(PRESERVED_PROFILES),
        'native_protocol': {'available_profile': PRESERVED_PROFILES[-1], 'available_chain': 'regtest',
            'available_quote_asset': 'synthetic-rUSD', 'source_chain_id': 31337, 'source_confirmations': 2,
            'target_profile': None, 'target_quote_asset_id': None, 'target_compatible': False},
        'custody': {'model': 'three-of-four-committee', 'native_proof_verified_on_chain': False,
                    'permissionless_exit_implemented': False, 'target_vault': None, 'target_policy': None},
        'fee_scope': {'requested_stable_unit': asset_key, 'qualified_stable_unit': 'synthetic-regtest-Q',
                     'source_approval_deposit_gas': 'local EIP3009 sponsor pays native gas; live sponsorship service unqualified',
                     'withdrawal_gas': 'operator-funded-native-asset; regtest-only'},
        'inputs': {'asset_pin': pin, 'asset_pin_sha256': _hash(pin) if pin is not None else None,
                   'finality_policy': policy, 'finality_policy_sha256': _hash(policy) if policy is not None else None,
                   'meaning': 'integrity links only; no live-state, finality, approval or activation evidence'},
        'local_rehearsal_scope': {'execution_chain_id': 31337, 'origin_chain_id': 42161,
            'actual_token_custody_compatibility_only': True, 'protocol_quote_remains_synthetic': True,
            'live_protocol_end_to_end_qualified': False, 'live_finality_qualified': False,
            'mainnet_operations_authorized': False},
        'unresolved_blockers': [{'id': key, 'status': 'unresolved', 'required_work': detail,
                                 'source_references': list(references)} for key, detail, references in BLOCKERS],
    }
    result['content_sha256'] = _hash(result)
    return result


def validate_disabled_plan(value):
    """Reconstruct all fields: editing a checksum or readiness cannot enable it."""
    _public(value)
    try:
        expected = build_disabled_plan(value['target']['asset']['key'],
                                      asset_pin=value['inputs']['asset_pin'],
                                      finality_policy=value['inputs']['finality_policy'])
    except (KeyError, TypeError) as error:
        raise CustodyError('disabled plan is incomplete or malformed') from error
    if _canonical(value) != _canonical(expected):
        raise CustodyError('disabled plan differs from exact disabled registry, policy, scope or blockers')
    return copy.deepcopy(expected)


def load_public_json(path):
    with Path(path).open('rb') as stream:
        raw = stream.read(MAX_BYTES + 1)
    if len(raw) > MAX_BYTES:
        raise CustodyError('public planning input exceeds its bound')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise CustodyError('public planning input repeats a JSON field')
            result[key] = value
        return result
    try:
        value = json.loads(raw, object_pairs_hook=unique,
                           parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite JSON')))
    except (ValueError, UnicodeError, RecursionError) as error:
        raise CustodyError('public planning input must be bounded finite JSON') from error
    _public(value)
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--asset', required=True, choices=('USDT0', 'USDC'))
    parser.add_argument('--asset-pin')
    parser.add_argument('--finality-policy')
    parser.add_argument('--out', required=True, help='fresh public JSON plan file; existing files are preserved')
    args = parser.parse_args()
    try:
        plan = build_disabled_plan(args.asset,
            asset_pin=load_public_json(args.asset_pin) if args.asset_pin else None,
            finality_policy=load_public_json(args.finality_policy) if args.finality_policy else None)
        # The only write is the requested public artifact, never a deployment,
        # worker configuration, key or transaction. Never replace prior input.
        with Path(args.out).open('x', encoding='utf-8', newline='\n') as stream:
            stream.write(json.dumps(plan, indent=2, sort_keys=True, allow_nan=False) + '\n')
            stream.flush()
            os.fsync(stream.fileno())
        print('Disabled plan: ' + str(Path(args.out).absolute()))
        return 0
    except (CustodyError, OSError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
