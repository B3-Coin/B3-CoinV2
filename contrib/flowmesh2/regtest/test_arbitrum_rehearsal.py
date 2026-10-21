#!/usr/bin/env python3
"""Offline provenance/origin/proxy negatives, not a fork or live-chain pass."""
import copy
import unittest

from arbitrum_assets import (GENESIS_HASH, IMPLEMENTATION_SLOT, ADMIN_SLOT, CIRCLE_IMPLEMENTATION_SLOT,
                            CIRCLE_ADMIN_SLOT, asset_spec)
from arbitrum_rehearsal import custody_proxy_pin, rehearsal_inputs
from arbitrum_rpc import HttpsReadOnlyRpc
from custody import CustodyError
from source_finality import canonical_hash


def fixture_inputs(asset='USDT0'):
    spec = asset_spec(asset)
    origin = {'height': 17, 'hash': '0x' + '11' * 32}
    impl, admin = (IMPLEMENTATION_SLOT, ADMIN_SLOT) if asset == 'USDT0' else (CIRCLE_IMPLEMENTATION_SLOT, CIRCLE_ADMIN_SLOT)
    pin = {'format': 'fm2-arbitrum-asset-pin/1', 'asset': asset, 'source_chain_id': 42161,
        'origin': origin, 'genesis_hash': GENESIS_HASH, 'mainnet_enabled': False,
        'identity': {'token': spec['address'], 'decimals': 6, 'token_code_hash': '0x' + '12' * 32,
            'proxy': {'kind': spec['proxy_kind'], 'implementation_slot': impl, 'implementation': '0x' + '44' * 20,
                'implementation_code_hash': '0x' + '13' * 32, 'admin_slot': admin, 'admin': '0x' + '55' * 20,
                'admin_code_hash': '0x' + '14' * 32, 'admin_owner': '0x' + '66' * 20 if asset == 'USDT0' else None},
            'issuer_roles': {role: '0x' + ('%02x' % (100 + n)) * 20 for n, role in enumerate(spec['issuer_role_signatures'])}}}
    providers = {label: HttpsReadOnlyRpc('https://' + label + '.example/rpc', label, label + '-operator',
        label + '-endpoint', chain_id=42161, genesis_hash=GENESIS_HASH) for label in ('first', 'second')}
    policy = {'format': 'fm2-source-finality-policy/2', 'chain_id': 42161, 'genesis_hash': GENESIS_HASH,
        'chain_kind': 'arbitrum-l2', 'parent_chain_id': 1, 'mode': 'finalized', 'minimum_confirmations': 0,
        'provider_ids': list(providers), 'provider_bindings': {key: rpc.binding for key, rpc in providers.items()},
        'mainnet_enabled': False}
    report = {'format': 'fm2-arbitrum-read-only-qualification/1', 'policy_sha256': canonical_hash(policy),
        'checkpoint': copy.deepcopy(origin), 'finalized_heads': {key: copy.deepcopy(origin) for key in providers},
        'provider_bindings': copy.deepcopy(policy['provider_bindings']), 'mainnet_enabled': False,
        'writes_enabled': False, 'trust': 'configured-independent-provider-rpc; no consensus light client'}
    return report, policy, pin


class RehearsalInputTests(unittest.TestCase):
    def call(self, report, policy, pin, url='https://first.example/rpc', provider='first'):
        return rehearsal_inputs(report, policy, pin, provider, url)

    def test_exact_finalized_origin_and_disabled_context_are_retained(self):
        report, policy, pin = fixture_inputs()
        result = self.call(report, policy, pin)
        self.assertEqual(result['context']['origin'], report['checkpoint'])
        self.assertEqual(result['context']['provider_binding'], policy['provider_bindings']['first'])
        self.assertIs(result['context']['mainnet_enabled'], False)
        self.assertEqual(result['input_hashes']['asset_pin_sha256'], canonical_hash(pin))
        pin['origin']['height'] = 18
        self.assertEqual(result['context']['origin']['height'], 17)

    def test_wrong_origin_hash_height_or_genesis_refuses(self):
        for key, value in (('height', 18), ('hash', '0x' + 'ff' * 32)):
            report, policy, pin = fixture_inputs()
            pin['origin'][key] = value
            with self.subTest(key=key), self.assertRaisesRegex(CustodyError, 'origin'):
                self.call(report, policy, pin)
        report, policy, pin = fixture_inputs()
        policy['genesis_hash'] = '0x' + 'aa' * 32
        report['policy_sha256'] = canonical_hash(policy)
        with self.assertRaises(CustodyError):
            self.call(report, policy, pin)

    def test_proxy_slot_kind_or_token_substitution_refuses(self):
        for key, value in (('implementation_slot', CIRCLE_IMPLEMENTATION_SLOT), ('kind', 'beacon')):
            report, policy, pin = fixture_inputs()
            pin['identity']['proxy'][key] = value
            with self.subTest(key=key), self.assertRaises(CustodyError):
                self.call(report, policy, pin)
        report, policy, pin = fixture_inputs()
        pin['identity']['token'] = '0x' + 'dd' * 20
        with self.assertRaises(CustodyError):
            self.call(report, policy, pin)

    def test_report_hash_provider_set_and_incomplete_finality_refuse(self):
        changes = (lambda x: x.update(policy_sha256='ff' * 32),
                   lambda x: x['finalized_heads'].pop('second'),
                   lambda x: x['finalized_heads']['second'].update(height=16),
                   lambda x: x['finalized_heads']['second'].update(hash='0x' + 'bb' * 32),
                   lambda x: x.update(mainnet_enabled=True), lambda x: x.update(writes_enabled=True))
        for change in changes:
            report, policy, pin = fixture_inputs()
            change(report)
            with self.subTest(change=change), self.assertRaises(CustodyError):
                self.call(report, policy, pin)

    def test_upstream_replacement_or_unknown_provider_refuses(self):
        report, policy, pin = fixture_inputs()
        with self.assertRaisesRegex(CustodyError, 'upstream'):
            self.call(report, policy, pin, url='https://replacement.example/rpc')
        with self.assertRaisesRegex(CustodyError, 'provider'):
            self.call(report, policy, pin, provider='third')

    def test_actual_proxy_pin_supports_each_exact_registered_proxy_family(self):
        for asset, slot in (('USDT0', IMPLEMENTATION_SLOT), ('USDC', CIRCLE_IMPLEMENTATION_SLOT)):
            report, policy, pin = fixture_inputs(asset)
            self.call(report, policy, pin)
            self.assertEqual(custody_proxy_pin(pin), {'slot': slot, 'address': pin['identity']['proxy']['implementation'],
                'code_hash': pin['identity']['proxy']['implementation_code_hash']})


if __name__ == '__main__':
    unittest.main(verbosity=2)
