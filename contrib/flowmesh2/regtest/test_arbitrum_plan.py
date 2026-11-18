#!/usr/bin/env python3
"""Disabled planning integrity and scope, without RPC, keys or transactions."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import arbitrum_assets as assets
from arbitrum_plan import (BLOCKERS, MAX_BYTES, PRESERVED_PROFILES, build_disabled_plan,
                           load_public_json, validate_disabled_plan)
from custody import CustodyError


def pin(key='USDT0'):
    spec = assets.asset_spec(key)
    return {'format': 'fm2-arbitrum-asset-pin/1', 'asset': key, 'source_chain_id': 42161,
        'origin': {'height': 100, 'hash': '0x' + 'aa' * 32}, 'genesis_hash': assets.GENESIS_HASH,
        'identity': {'token': spec['address'], 'decimals': 6, 'token_code_hash': '0x' + 'bb' * 32,
            'proxy': {'kind': spec['proxy_kind'],
                'implementation_slot': assets.IMPLEMENTATION_SLOT if key == 'USDT0' else assets.CIRCLE_IMPLEMENTATION_SLOT,
                'implementation': '0x' + '11' * 20, 'implementation_code_hash': '0x' + 'cc' * 32,
                'admin_slot': assets.ADMIN_SLOT if key == 'USDT0' else assets.CIRCLE_ADMIN_SLOT,
                'admin': '0x' + '22' * 20, 'admin_code_hash': '0x' + 'dd' * 32,
                'admin_owner': '0x' + '33' * 20 if key == 'USDT0' else None},
            'issuer_roles': {role: '0x' + '44' * 20 for role in spec['issuer_role_signatures']}},
        'mainnet_enabled': False}


def policy():
    return {'format': 'fm2-source-finality-policy/2', 'chain_id': 42161, 'genesis_hash': assets.GENESIS_HASH,
        'chain_kind': 'arbitrum-l2', 'parent_chain_id': 1, 'mode': 'finalized', 'minimum_confirmations': 0,
        'provider_ids': ['a', 'b'], 'mainnet_enabled': False,
        'provider_bindings': {key: {'operator_id': 'operator-' + key, 'endpoint_id': 'endpoint-' + key,
            'endpoint_origin': 'https://' + key + '.invalid:443', 'endpoint_commitment': byte * 64}
            for key, byte in (('a', '1'), ('b', '2'))}}


class DisabledPlanTests(unittest.TestCase):
    def test_each_exact_asset_builds_without_any_network_configuration(self):
        for key in ('USDT0', 'USDC'):
            value = build_disabled_plan(key)
            self.assertEqual(validate_disabled_plan(value), value)
            self.assertEqual(value['target']['asset'], assets.asset_spec(key))
            self.assertFalse(value['mainnet_ready'])
            self.assertIsNone(value['inputs']['asset_pin'])
            self.assertIsNone(value['inputs']['finality_policy'])
            self.assertEqual(value['preserved_profiles'], list(PRESERVED_PROFILES))
            self.assertEqual(value['native_protocol']['available_profile'], 'fm2-regtest-stable-q/3')
            self.assertFalse(value['native_protocol']['target_compatible'])

    def test_valid_input_pins_are_linked_but_never_clear_a_blocker(self):
        for key in ('USDT0', 'USDC'):
            value = build_disabled_plan(key, asset_pin=pin(key), finality_policy=policy())
            self.assertEqual(validate_disabled_plan(value), value)
            self.assertEqual(len(value['unresolved_blockers']), len(BLOCKERS))
            self.assertTrue(all(item['status'] == 'unresolved' for item in value['unresolved_blockers']))
            self.assertEqual(value['local_rehearsal_scope']['execution_chain_id'], 31337)
            self.assertFalse(value['local_rehearsal_scope']['live_protocol_end_to_end_qualified'])
            self.assertFalse(value['local_rehearsal_scope']['live_finality_qualified'])
            self.assertFalse(value['custody']['native_proof_verified_on_chain'])

    def test_readiness_flags_unknown_fields_and_self_cleared_blockers_refuse(self):
        original = build_disabled_plan('USDT0', asset_pin=pin(), finality_policy=policy())
        for field in ('mainnet_ready', 'mainnet_enabled', 'signing_enabled', 'deployment_enabled', 'broadcast_enabled'):
            value = copy.deepcopy(original)
            value[field] = True
            with self.subTest(field=field), self.assertRaises(CustodyError):
                validate_disabled_plan(value)
        for change in (lambda value: value.update(status='ready'),
                       lambda value: value.update(mainnet_rpc_url='https://example.invalid'),
                       lambda value: value['unresolved_blockers'].clear(),
                       lambda value: value['unresolved_blockers'][0].update(status='passed'),
                       lambda value: value['local_rehearsal_scope'].update(live_protocol_end_to_end_qualified=True)):
            value = copy.deepcopy(original)
            change(value)
            with self.assertRaises(CustodyError):
                validate_disabled_plan(value)

    def test_boolean_fields_cannot_be_replaced_by_equal_python_integers(self):
        value = build_disabled_plan('USDC')
        value['mainnet_enabled'] = 0
        with self.assertRaises(CustodyError):
            validate_disabled_plan(value)

    def test_exact_token_selection_refuses_ambiguous_or_bridged_alias(self):
        for key in ('USDT', 'usdt0', 'USDC.e', 'Q', 'TOKENA', 0, None):
            with self.subTest(asset=key), self.assertRaises(CustodyError):
                build_disabled_plan(key)
        with self.assertRaises(CustodyError):
            build_disabled_plan('USDC', asset_pin=pin('USDT0'))
        altered = pin()
        altered['identity']['token'] = assets.asset_spec('USDC')['address']
        with self.assertRaises(CustodyError):
            build_disabled_plan('USDT0', asset_pin=altered)

    def test_source_or_execution_chain_substitution_refuses(self):
        for mutate in (lambda value: value.update(source_chain_id=31337),
                       lambda value: value.update(mainnet_enabled=True),
                       lambda value: value.update(genesis_hash='0x' + 'ee' * 32),
                       lambda value: value['origin'].update(height=0),
                       lambda value: value['identity'].update(decimals=18)):
            altered = pin()
            mutate(altered)
            with self.assertRaises(CustodyError):
                build_disabled_plan('USDT0', asset_pin=altered)
        altered_policy = policy()
        altered_policy['genesis_hash'] = '0x' + 'ee' * 32
        with self.assertRaises(CustodyError):
            build_disabled_plan('USDT0', asset_pin=pin(), finality_policy=altered_policy)

    def test_finality_cannot_downgrade_to_old_policy_or_confirmation_count(self):
        for mutate in (lambda value: value.update(mode='confirmations', minimum_confirmations=2),
                       lambda value: value.update(format='fm2-source-finality-policy/1'),
                       lambda value: value.update(chain_id=31337),
                       lambda value: value.update(genesis_hash='0x' + 'ee' * 32),
                       lambda value: value.update(parent_chain_id=True),
                       lambda value: value.update(minimum_confirmations=False),
                       lambda value: value.update(mainnet_enabled=True)):
            altered = policy()
            mutate(altered)
            with self.assertRaises(CustodyError):
                build_disabled_plan('USDT0', finality_policy=altered)

    def test_outputs_and_inputs_are_independent_deep_copies(self):
        original_pin, original_policy = pin(), policy()
        value = build_disabled_plan('USDT0', asset_pin=original_pin, finality_policy=original_policy)
        original_pin['identity']['token_code_hash'] = '0x' + 'ff' * 32
        original_policy['provider_ids'].clear()
        self.assertEqual(validate_disabled_plan(value), value)
        value['unresolved_blockers'][0]['source_references'].clear()
        self.assertTrue(build_disabled_plan('USDT0')['unresolved_blockers'][0]['source_references'])
        validated = validate_disabled_plan(build_disabled_plan('USDC'))
        validated['target']['asset']['sources'].clear()
        self.assertTrue(build_disabled_plan('USDC')['target']['asset']['sources'])

    def test_duplicate_oversized_nonfinite_and_private_json_inputs_refuse(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'input.json'
            for encoded in (b'{"x":1,"x":2}', b'{"x":NaN}', b' ' * (MAX_BYTES + 1),
                            b'{"nested":{"private_key":"secret"}}', b'{"RPC_URL":"https://secret.invalid"}'):
                path.write_bytes(encoded)
                with self.subTest(encoded=encoded[:48]), self.assertRaises(CustodyError):
                    load_public_json(path)

    def test_hash_or_registry_alteration_is_not_an_activation_authority(self):
        value = build_disabled_plan('USDT0')
        value['target']['asset']['address'] = assets.asset_spec('USDC')['address']
        # Even replacing the integrity hash with another well-formed hash cannot
        # make a modified registry or readiness claim an accepted planning input.
        value['content_sha256'] = 'ab' * 32
        with self.assertRaises(CustodyError):
            validate_disabled_plan(value)

    def test_cli_writes_only_a_fresh_disabled_public_plan_and_preserves_it(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'plan.json'
            command = [sys.executable, '-B', str(Path(__file__).with_name('arbitrum_plan.py')),
                       '--asset', 'USDT0', '--out', str(path)]
            first = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(first.returncode, 0, first.stderr)
            original = path.read_bytes()
            self.assertEqual(validate_disabled_plan(json.loads(original))['status'], 'blocked')
            second = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(second.returncode, 0)
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual([item.name for item in Path(directory).iterdir()], ['plan.json'])


if __name__ == '__main__':
    unittest.main()
