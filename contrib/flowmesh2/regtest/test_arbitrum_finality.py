#!/usr/bin/env python3
"""Offline live-boundary finality/recovery negatives; no live-chain pass."""
import copy
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from arbitrum_rpc import HttpsReadOnlyRpc, ProviderIdentityError
from custody import CustodyError
from qualify_arbitrum_read_only import providers_from_environment
from source_finality import SourceFinality, canonical_hash, qualify_read_only


def digest(number, branch=0):
    return '0x' + (number + branch * 1000).to_bytes(32, 'big').hex()


class Node:
    """Deterministic public test blocks, not a provider or generated identity."""
    def __init__(self):
        self.chain, self.latest, self.finalized = 42161, 100, 80
        self.changed, self.missing_finalized = {}, False

    def block(self, number):
        return {'number': hex(number), 'hash': self.changed.get(number, digest(number))}

    def call(self, method, params=()):
        if method == 'eth_chainId':
            return hex(self.chain)
        if method == 'eth_getBlockByNumber':
            if params[0] == 'finalized':
                return None if self.missing_finalized else self.block(self.finalized)
            return self.block(int(params[0], 16))
        if method == 'eth_getBlockByHash':
            return next((self.block(n) for n in range(self.latest + 1) if self.block(n)['hash'] == params[0]), None)
        raise AssertionError('unexpected fake provider call')


class ArbitrumFinalityTests(unittest.TestCase):
    def setUp(self):
        self.nodes = {'first': Node(), 'second': Node()}
        self.providers = {
            key: HttpsReadOnlyRpc('https://' + key + '.example/private-key', key, key + '-operator', key + '-rpc',
                                chain_id=42161, genesis_hash=digest(0))
            for key in self.nodes}
        self.patches = []
        for key, rpc in self.providers.items():
            patch = mock.patch.object(rpc, 'call', side_effect=self.nodes[key].call)
            self.patches.append(patch)
            patch.start()
        self.addCleanup(lambda: [patch.stop() for patch in self.patches])
        self.policy = {'format': 'fm2-source-finality-policy/2', 'chain_id': 42161,
            'genesis_hash': digest(0), 'chain_kind': 'arbitrum-l2', 'parent_chain_id': 1,
            'mode': 'finalized', 'minimum_confirmations': 0, 'provider_ids': ['first', 'second'],
            'provider_bindings': {key: rpc.binding for key, rpc in self.providers.items()}, 'mainnet_enabled': False}
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / 'finality.json'

    def test_selection_uses_common_canonical_finalized_height_not_latest(self):
        self.nodes['first'].latest, self.nodes['first'].finalized = 1000, 80
        self.nodes['second'].latest, self.nodes['second'].finalized = 2000, 79
        result = qualify_read_only(self.policy, self.providers, self.path)
        self.assertEqual(result['checkpoint'], {'height': 79, 'hash': digest(79)})
        self.assertEqual(result['finalized_heads']['first']['height'], 80)
        self.assertEqual(result['policy_sha256'], canonical_hash(self.policy))
        self.assertIs(result['mainnet_enabled'], False)
        self.assertIs(result['writes_enabled'], False)
        self.assertNotIn('private-key', json.dumps(result))
        self.assertEqual(json.loads(self.path.read_text())['checkpoint'], result['checkpoint'])

    def test_organizations_and_endpoint_aliases_cannot_count_twice(self):
        for field in SourceFinality.BINDING_FIELDS:
            changed = copy.deepcopy(self.policy)
            changed['provider_bindings']['second'][field] = changed['provider_bindings']['first'][field]
            with self.subTest(field=field), self.assertRaises(CustodyError):
                SourceFinality.validate_policy(changed)
        swapped = dict(self.providers)
        swapped['second'] = swapped['first']
        with self.assertRaises(CustodyError):
            SourceFinality(self.policy, swapped)
        wrong = copy.deepcopy(self.policy)
        wrong['provider_bindings']['first']['endpoint_commitment'] = 'ff' * 32
        with self.assertRaises(CustodyError):
            SourceFinality(wrong, self.providers)

    def test_same_checkpoint_replaced_is_sticky_after_endpoint_restored_and_reopen(self):
        with SourceFinality(self.policy, self.providers, self.path) as verifier:
            verifier.verify({'height': 70, 'hash': digest(70)}, persist=True)
            self.nodes['second'].changed[70] = digest(70, 1)
            with self.assertRaisesRegex(CustodyError, 'durably fenced'):
                verifier.verify({'height': 71, 'hash': digest(71)}, persist=True)
            self.nodes['second'].changed.clear()
            with self.assertRaisesRegex(CustodyError, 'remains durably fenced'):
                verifier.verify({'height': 71, 'hash': digest(71)})
        with SourceFinality(self.policy, self.providers, self.path) as reopened:
            with self.assertRaisesRegex(CustodyError, 'remains durably fenced'):
                reopened.verify({'height': 71, 'hash': digest(71)})
        self.assertEqual(json.loads(self.path.read_text())['fence'], 'retained-checkpoint-replaced')

    def test_provider_finalized_head_rollback_or_replaced_old_head_fences(self):
        for mode in ('rollback', 'replacement'):
            path = self.path.with_name(mode + '.json')
            with SourceFinality(self.policy, self.providers, path) as verifier:
                verifier.verify({'height': 70, 'hash': digest(70)}, persist=True)
                if mode == 'rollback':
                    self.nodes['first'].finalized = 79
                else:
                    self.nodes['first'].finalized = 81
                    self.nodes['first'].changed[80] = digest(80, 1)
                with self.assertRaisesRegex(CustodyError, 'durably fenced'):
                    verifier.finalized_heads()
            self.nodes['first'].finalized, self.nodes['first'].changed = 80, {}
            with self.assertRaisesRegex(CustodyError, 'remains durably fenced'):
                qualify_read_only(self.policy, self.providers, path)

    def test_identity_pin_exception_from_transport_durably_fences(self):
        with SourceFinality(self.policy, self.providers, self.path) as verifier:
            verifier.verify({'height': 70, 'hash': digest(70)}, persist=True)
            with mock.patch.object(self.providers['first'], 'call', side_effect=ProviderIdentityError('pin differs')):
                with self.assertRaisesRegex(CustodyError, 'chain-identity-changed'):
                    verifier.verify({'height': 71, 'hash': digest(71)})
        self.assertEqual(json.loads(self.path.read_text())['fence'], 'chain-identity-changed')

    def test_missing_finalized_or_disagreement_cannot_advance_checkpoint(self):
        qualify_read_only(self.policy, self.providers, self.path)
        before = self.path.read_bytes()
        self.nodes['second'].missing_finalized = True
        with self.assertRaises(CustodyError):
            qualify_read_only(self.policy, self.providers, self.path)
        self.assertEqual(self.path.read_bytes(), before)
        self.nodes['second'].missing_finalized = False
        self.nodes['first'].finalized = self.nodes['second'].finalized = 81
        self.nodes['second'].changed[81] = digest(81, 1)
        with self.assertRaises(CustodyError):
            qualify_read_only(self.policy, self.providers, self.path)
        self.assertEqual(self.path.read_bytes(), before)

    def test_durable_owner_policy_and_endpoint_replacement_refuse(self):
        with SourceFinality(self.policy, self.providers, self.path) as verifier:
            verifier.verify({'height': 70, 'hash': digest(70)}, persist=True)
            with self.assertRaisesRegex(CustodyError, 'already has an owner'):
                SourceFinality(self.policy, self.providers, self.path)
        replacement = copy.deepcopy(self.policy)
        replacement['provider_bindings']['first']['operator_id'] = 'replacement-operator'
        replaced = dict(self.providers)
        replaced['first'] = HttpsReadOnlyRpc('https://first.example/private-key', 'first', 'replacement-operator',
                                          'first-rpc', chain_id=42161, genesis_hash=digest(0))
        with self.assertRaisesRegex(CustodyError, 'no reset'):
            SourceFinality(replacement, replaced, self.path)

    def test_state_duplicate_fields_corruption_oversize_and_links_refuse(self):
        for content in ('{"policy":{},"policy":{}}', '[]', '{', ' ' * (SourceFinality.MAX_STATE_BYTES + 1)):
            self.path.write_text(content)
            with self.subTest(content=content[:30]), self.assertRaises(CustodyError):
                SourceFinality(self.policy, self.providers, self.path)
        self.path.unlink()
        if os.name == 'posix':
            target = self.path.with_name('target')
            target.write_text('unchanged')
            self.path.symlink_to(target)
            with self.assertRaises(CustodyError):
                SourceFinality(self.policy, self.providers, self.path)
            self.assertEqual(target.read_text(), 'unchanged')

    def test_strict_point_and_false_numeric_policy_inputs_refuse(self):
        for change in ({'minimum_confirmations': False}, {'parent_chain_id': True}, {'chain_id': '42161'}):
            with self.subTest(change=change), self.assertRaises(CustodyError):
                SourceFinality.validate_policy(dict(self.policy, **change))
        for point in ({'height': '70', 'hash': digest(70)}, {'height': True, 'hash': digest(70)},
                      {'height': 70, 'hash': '0x' + 'AA' * 32}):
            with self.subTest(point=point), self.assertRaises(CustodyError):
                SourceFinality(self.policy, self.providers).verify(point)

    def test_private_environment_credentials_missing_or_changed_are_not_qualified(self):
        config = {'format': 'fm2-arbitrum-read-only-config/1', 'policy': self.policy,
                  'providers': {key: {'url_env': 'B3_' + key.upper()} for key in self.nodes}}
        with mock.patch.dict(os.environ, {}, clear=True):
            with self.assertRaisesRegex(CustodyError, 'missing read-only provider'):
                providers_from_environment(config)
        urls = {'B3_FIRST': 'https://first.example/private-key', 'B3_SECOND': 'https://second.example/private-key'}
        with mock.patch.dict(os.environ, urls, clear=True):
            policy, providers = providers_from_environment(config)
            self.assertEqual(policy, self.policy)
            self.assertEqual(providers['first'].binding, self.providers['first'].binding)
        urls['B3_FIRST'] = 'https://replacement.example/different-private-key'
        with mock.patch.dict(os.environ, urls, clear=True):
            with self.assertRaisesRegex(CustodyError, 'preapproved'):
                providers_from_environment(config)


if __name__ == '__main__':
    unittest.main(verbosity=2)
