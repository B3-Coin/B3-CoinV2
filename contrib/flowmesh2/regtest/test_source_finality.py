#!/usr/bin/env python3
"""Offline source-policy negatives; no URL/RPC client, fork or live chain."""
import copy
import tempfile
from pathlib import Path
import unittest

from custody import CustodyError
from source_finality import SourceFinality
from signed_relayer import decode_transaction, rlp


def digest(number, branch=0):
    return '0x' + (number + branch * 1000).to_bytes(32, 'big').hex()


class Node:
    def __init__(self):
        self.chain, self.latest, self.finalized = 42161, 100, 80
        self.changed = {}
        self.missing_finalized = False
        self.finalized_hash = None

    def block(self, number):
        return {'number': hex(number), 'hash': self.changed.get(number, digest(number))}

    def call(self, method, params=()):
        if method == 'eth_chainId':
            return hex(self.chain)
        if method == 'eth_blockNumber':
            return hex(self.latest)
        if method == 'eth_getBlockByNumber':
            selected = params[0]
            if selected == 'finalized':
                if self.missing_finalized:
                    return None
                result = self.block(self.finalized)
                if self.finalized_hash is not None:
                    result['hash'] = self.finalized_hash
                return result
            return self.block(int(selected, 16))
        if method == 'eth_getBlockByHash':
            for number in range(self.latest + 1):
                block = self.block(number)
                if block['hash'] == params[0]:
                    return block
            return None
        raise AssertionError('unexpected source policy RPC method')


class FinalityTests(unittest.TestCase):
    def setUp(self):
        self.policy = {'format': 'fm2-source-finality-policy/1', 'chain_id': 42161,
            'genesis_hash': digest(0), 'chain_kind': 'arbitrum-l2', 'parent_chain_id': 1,
            'mode': 'finalized', 'minimum_confirmations': 0, 'provider_ids': ['own-node', 'independent-node'],
            'mainnet_enabled': False}
        self.nodes = {'own-node': Node(), 'independent-node': Node()}
        self.point = {'height': 70, 'hash': digest(70)}

    def verifier(self, policy=None, path=None):
        return SourceFinality(self.policy if policy is None else policy, self.nodes, path)

    def test_arbitrum_parent_finalized_from_both_nodes(self):
        self.assertEqual(self.verifier().verify(self.point), self.point)

    def test_provider_quorum_is_frozen_against_caller_policy_mutation(self):
        verifier = self.verifier()
        self.policy['provider_ids'].pop()
        self.nodes['independent-node'].finalized = 69
        with self.assertRaises(CustodyError):
            verifier.verify(self.point)

    def test_sequencer_head_is_never_substituted_for_parent_finality(self):
        self.nodes['independent-node'].finalized = 69
        with self.assertRaises(CustodyError):
            self.verifier().verify(self.point)

    def test_missing_finalized_tag_refuses_without_latest_fallback(self):
        self.nodes['own-node'].missing_finalized = True
        with self.assertRaises(CustodyError):
            self.verifier().verify(self.point)

    def test_finalized_tag_must_match_same_nodes_canonical_chain(self):
        self.nodes['own-node'].finalized_hash = digest(80, 1)
        with self.assertRaises(CustodyError):
            self.verifier().verify(self.point)

    def test_provider_fork_disagreement_refuses(self):
        self.nodes['independent-node'].changed[70] = digest(70, 1)
        with self.assertRaises(CustodyError):
            self.verifier().verify(self.point)

    def test_wrong_chain_and_genesis_refuse(self):
        self.nodes['own-node'].chain = 1
        with self.assertRaises(CustodyError):
            self.verifier().verify(self.point)
        self.nodes['own-node'].chain = 42161
        self.nodes['own-node'].changed[0] = digest(0, 1)
        with self.assertRaises(CustodyError):
            self.verifier().verify(self.point)

    def test_durable_high_water_reopen_regression_and_reorg_refuse(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'finality.json'
            self.verifier(path=path).verify(self.point, persist=True)
            reopened = self.verifier(path=path)
            self.assertEqual(reopened.verify(self.point), self.point)
            with self.assertRaises(CustodyError):
                reopened.verify({'height': 69, 'hash': digest(69)})
            self.nodes['own-node'].changed[70] = digest(70, 1)
            with self.assertRaises(CustodyError):
                reopened.verify({'height': 71, 'hash': digest(71)})

    def test_durable_policy_change_cannot_reset_checkpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'finality.json'
            self.verifier(path=path).verify(self.point, persist=True)
            changed = copy.deepcopy(self.policy)
            changed['provider_ids'] = ['own-node', 'replacement-node']
            nodes = {'own-node': Node(), 'replacement-node': Node()}
            with self.assertRaises(CustodyError):
                SourceFinality(changed, nodes, path)

    def test_block_counts_cannot_be_used_for_arbitrum(self):
        changed = copy.deepcopy(self.policy)
        changed.update(mode='confirmations', minimum_confirmations=2)
        with self.assertRaises(CustodyError):
            self.verifier(changed)

    def test_unknown_parent_single_node_duplicate_and_mainnet_enable_refuse(self):
        for key, value in [('parent_chain_id', None), ('provider_ids', ['own-node']),
                           ('provider_ids', ['own-node', 'own-node']), ('mainnet_enabled', True), ('mode', 'safe')]:
            changed = copy.deepcopy(self.policy); changed[key] = value
            with self.assertRaises(CustodyError):
                self.verifier(changed)

    def test_confirmations_are_explicitly_isolated_regtest_only(self):
        changed = copy.deepcopy(self.policy)
        changed.update(chain_id=31337, chain_kind='isolated-regtest', parent_chain_id=None,
                       mode='confirmations', minimum_confirmations=2)
        for node in self.nodes.values():
            node.chain, node.latest = 31337, 71
        self.assertEqual(self.verifier(changed).verify(self.point), self.point)
        self.nodes['own-node'].latest = 70
        with self.assertRaises(CustodyError):
            self.verifier(changed).verify(self.point)


class SignedEnvelopeTests(unittest.TestCase):
    def test_eip155_rlp_roundtrip_and_canonical_integer_encoding(self):
        body = [0, 1_000_000_000, 21000, bytes.fromhex('11' * 20), 0, b'', 31337 * 2 + 35, 123, 456]
        encoded = rlp(body)
        fields = decode_transaction('0x' + encoded.hex())
        self.assertEqual(rlp(fields), encoded)
        self.assertEqual(int.from_bytes(fields[6], 'big'), 31337 * 2 + 35)

    def test_signed_rlp_noncanonical_trailing_nested_and_truncated_refuse(self):
        body = [0, 1, 21000, bytes.fromhex('11' * 20), 0, b'', 31337 * 2 + 35, 123, 456]
        good = rlp(body)
        cases = [good + b'\0', good[:-1], rlp(body[:8]), rlp(body[:8] + [[456]]), rlp([b'\0'] + body[1:])]
        for encoded in cases:
            with self.subTest(encoded=encoded.hex()):
                with self.assertRaises(CustodyError):
                    decode_transaction('0x' + encoded.hex())


if __name__ == '__main__':
    unittest.main()
