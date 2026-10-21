#!/usr/bin/env python3
"""Pure input integrity for the isolated four-operator real-token rehearsal."""
import copy

from arbitrum_assets import GENESIS_HASH, validate_asset_pin_shape
from arbitrum_fork import validate_fork_context
from arbitrum_rpc import HttpsReadOnlyRpc
from custody import CustodyError
from source_finality import SourceFinality, canonical_hash


def rehearsal_inputs(report, policy, asset_pin, provider_id, provider_url):
    """Validate public provenance links; this does not prove live finality."""
    policy, pin = SourceFinality.validate_policy(policy), validate_asset_pin_shape(asset_pin)
    fields = {'format', 'policy_sha256', 'checkpoint', 'finalized_heads', 'provider_bindings',
              'mainnet_enabled', 'writes_enabled', 'trust'}
    if (policy['format'] != 'fm2-source-finality-policy/2' or policy['genesis_hash'] != GENESIS_HASH or
            not isinstance(report, dict) or set(report) != fields or
            report['format'] != 'fm2-arbitrum-read-only-qualification/1' or
            report['policy_sha256'] != canonical_hash(policy) or report['mainnet_enabled'] is not False or
            report['writes_enabled'] is not False or
            report['trust'] != 'configured-independent-provider-rpc; no consensus light client' or
            report['provider_bindings'] != policy['provider_bindings']):
        raise CustodyError('rehearsal requires exact disabled finalized read-only report/policy provenance')
    checkpoint = SourceFinality._point(report['checkpoint'])
    if checkpoint != pin['origin'] or checkpoint['height'] == 0:
        raise CustodyError('rehearsal asset origin differs from its exact finalized report checkpoint')
    heads = report['finalized_heads']
    if not isinstance(heads, dict) or set(heads) != set(policy['provider_ids']):
        raise CustodyError('rehearsal report omitted independent finalized provider observations')
    for value in heads.values():
        head = SourceFinality._point(value)
        if head['height'] < checkpoint['height'] or (head['height'] == checkpoint['height'] and head != checkpoint):
            raise CustodyError('rehearsal checkpoint is inconsistent with its recorded finalized provider heads')
    if provider_id not in policy['provider_ids']:
        raise CustodyError('rehearsal fork provider is not in the exact finalized policy')
    binding = policy['provider_bindings'][provider_id]
    provider = HttpsReadOnlyRpc(provider_url, provider_id, binding['operator_id'], binding['endpoint_id'],
                              chain_id=42161, genesis_hash=GENESIS_HASH)
    if provider.binding != binding:
        raise CustodyError('rehearsal upstream differs from its exact pinned provider endpoint')
    context = validate_fork_context({'format': 'fm2-regtest-arbitrum-fork/1', 'mainnet_enabled': False,
        'origin': checkpoint, 'provider_binding': binding, 'asset_pin': pin})
    return {'context': context, 'asset_pin': pin, 'policy': policy, 'finality_report': copy.deepcopy(report),
            'input_hashes': {'policy_sha256': canonical_hash(policy), 'asset_pin_sha256': canonical_hash(pin),
                             'finality_report_sha256': canonical_hash(report)}}


def custody_proxy_pin(asset_pin):
    pin = validate_asset_pin_shape(asset_pin)
    proxy = pin['identity']['proxy']
    return {'slot': proxy['implementation_slot'], 'address': proxy['implementation'],
            'code_hash': proxy['implementation_code_hash']}
