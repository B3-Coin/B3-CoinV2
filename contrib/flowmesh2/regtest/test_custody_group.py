#!/usr/bin/env python3
"""Shared route snapshots keep every mutable identity read and canonical fence."""
import copy
import hashlib
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from custody import CustodyError, LocalRpc, RpcError, SYNTHETIC_ASSETS
import custody_policy as policy


GENESIS, HEAD = '0x' + '11' * 32, '0x' + '22' * 32
SOURCE = {'height': 9, 'hash': '0x' + '44' * 32, 'minimum_confirmations': 2}


def hash_value(value):
    return '0x' + hashlib.sha256(value.encode()).hexdigest()


def word(value):
    return value.to_bytes(32, 'big').hex() if isinstance(value, int) else value[2:].zfill(64)


class FakeCast:
    def calldata(self, signature):
        if signature != 'custodyIdentity()':
            raise AssertionError('unexpected identity ABI')
        return '0xaaaaaaaa'

    def keccak(self, value):
        return hash_value(value)


class SnapshotRpc(LocalRpc):
    def __init__(self, url='http://127.0.0.1:31337'):
        super().__init__(url)
        self.exchanges, self.state_reads = 0, []
        self.height, self.digest = 10, HEAD
        self.blocks = {9: {'number': '0x9', 'hash': SOURCE['hash']}}
        self.forked = False
        self.code, self.storage, self.identities = {}, {}, {}
        self.fault = None

    def advance(self):
        self.blocks[self.height] = {'number': hex(self.height), 'hash': self.digest}
        self.height += 1
        self.digest = hash_value('head ' + str(self.height))

    def _exchange(self, payload):
        self.exchanges += 1
        batch = isinstance(payload, list)
        requests = payload if batch else [payload]
        replies = []
        state = False
        for request in requests:
            method, params = request['method'], request['params']
            if method == 'eth_chainId':
                result = hex(31337)
            elif method == 'anvil_nodeInfo':
                result = {'environment': {'chainId': 31337}, 'forkConfig': {'forkUrl': None, 'forkBlockNumber': None},
                          'currentBlockNumber': hex(self.height)}
                if self.forked:
                    result['forkConfig'] = {'forkUrl': 'https://not-authorized.invalid', 'forkBlockNumber': 1}
            elif method == 'eth_getBlockByNumber':
                if params[0] == '0x0':
                    result = {'number': '0x0', 'hash': GENESIS}
                elif params[0] in ('latest', hex(self.height)):
                    result = {'number': hex(self.height), 'hash': self.digest}
                else:
                    result = self.blocks.get(int(params[0], 16))
            elif method in ('eth_getCode', 'eth_getStorageAt', 'eth_call'):
                state = True
                self.state_reads.append((method, copy.deepcopy(params)))
                if params[-1] != hex(self.height):
                    raise AssertionError('every route state read must use the same explicit canonical head')
                if method == 'eth_getCode':
                    result = self.code[params[0]]
                elif method == 'eth_getStorageAt':
                    result = self.storage.get((params[0], params[1]), '0x' + '00' * 32)
                else:
                    result = self.identities[params[0]['to']]
            else:
                raise AssertionError('unexpected snapshot RPC method')
            replies.append({'id': request['id'], 'jsonrpc': '2.0', 'result': result})
        if state and self.fault == 'removed-result':
            replies.pop()
        if state and self.fault == 'reorg':
            self.digest = hash_value('replacement at same height')
        if state and self.fault == 'final-fork':
            self.forked = True
        return list(reversed(replies)) if batch else replies[0]


class Driver:
    custody_profile = policy.PROFILE
    operator_addresses = ['0x' + str(value).zfill(40) for value in range(1, 5)]

    def __init__(self, number, asset, rpc):
        self.rpc, self.cast = rpc, FakeCast()
        self.token, self.vault = '0x' + str(10 + number).zfill(40), '0x' + str(20 + number).zfill(40)
        self.asset, self.instance = asset, hash_value('instance ' + str(number))
        rpc.code[self.token], rpc.code[self.vault] = '0x6001', '0x6002'
        pinned = {'format': policy.POLICY_FORMAT, 'chain_id': 31337, 'genesis_hash': GENESIS,
            'token_code_hash': self.cast.keccak(rpc.code[self.token]), 'vault_code_hash': self.cast.keccak(rpc.code[self.vault]),
            'policy_hash': hash_value('policy ' + str(number)), 'domain_separator': hash_value('domain ' + str(number)),
            'roster_hash': hash_value('roster'), 'roster_epoch': 0, 'dispute_seconds': 30, 'rotation_seconds': 60,
            'max_certificate_lifetime': 3600, 'minimum_confirmations': 2, 'finality_mode': 'isolated-confirmations',
            'mainnet_enabled': False, 'proxy_implementation': None}
        self.config = {'custody_policy': pinned}
        self.expected = pinned['roster_hash'], pinned['policy_hash'], pinned['domain_separator']
        self.identity = [31337, pinned['token_code_hash'], pinned['policy_hash'], pinned['domain_separator'], pinned['roster_hash'],
                         0, 30, 60, 3600, self.token, asset, self.instance, 0, '0x' + '00' * 32]
        self.publish_identity()

    def publish_identity(self):
        self.rpc.identities[self.vault] = '0x' + ''.join(word(value) for value in self.identity)


class GroupedCustodyTests(unittest.TestCase):
    def setUp(self):
        self.rpc = SnapshotRpc()
        self.drivers = [Driver(index, asset, self.rpc) for index, asset in enumerate(SYNTHETIC_ASSETS.values())]
        self.hashes = patch.object(policy, '_expected_hashes', lambda driver, pinned: driver.expected)
        self.hashes.start()

    def tearDown(self):
        self.hashes.stop()

    def test_all_three_routes_read_once_at_one_fresh_head_in_three_exchanges(self):
        result = policy.verify_custody_routes(self.drivers)
        self.assertEqual(len(result), 3)
        self.assertEqual(self.rpc.exchanges, 3)
        self.assertEqual(len(self.rpc.state_reads), 18)
        for driver in self.drivers:
            self.assertIn(('eth_getCode', [driver.token, '0xa']), self.rpc.state_reads)
            self.assertIn(('eth_getCode', [driver.vault, '0xa']), self.rpc.state_reads)
            self.assertIn(('eth_call', [{'to': driver.vault, 'data': '0xaaaaaaaa'}, '0xa']), self.rpc.state_reads)

    def test_repeated_guard_never_reuses_mutable_results(self):
        policy.verify_custody_routes(self.drivers)
        policy.verify_custody_routes(self.drivers)
        self.assertEqual(self.rpc.exchanges, 6)
        self.assertEqual(len(self.rpc.state_reads), 36)

    def test_any_route_code_change_is_refused(self):
        policy.verify_custody_routes(self.drivers)
        self.rpc.advance()
        self.rpc.code[self.drivers[1].token] = '0x6003'
        with self.assertRaisesRegex(CustodyError, 'runtime differs'):
            policy.verify_custody_routes(self.drivers)

    def test_mutable_epoch_and_pause_change_are_refused_on_each_route(self):
        for index, field in ((0, 5), (1, 12), (2, 13)):
            original = self.drivers[index].identity[field]
            self.drivers[index].identity[field] = '0x' + 'ab' * 32 if field == 13 else 1
            self.drivers[index].publish_identity()
            with self.assertRaisesRegex(CustodyError, 'identity/roster/active policy changed'):
                policy.verify_custody_routes(self.drivers)
            self.drivers[index].identity[field] = original
            self.drivers[index].publish_identity()

    def test_proxy_upgrade_is_refused_even_with_identical_token_and_vault_code(self):
        driver = self.drivers[2]
        original, replacement = '0x' + '88' * 20, '0x' + '99' * 20
        slot = '0x360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc'
        self.rpc.code[original], self.rpc.code[replacement] = '0x6042', '0x6043'
        driver.config['custody_policy']['proxy_implementation'] = {
            'slot': slot, 'address': original, 'code_hash': driver.cast.keccak(self.rpc.code[original])}
        self.rpc.storage[(driver.token, slot)] = '0x' + word(original)
        policy.verify_custody_routes(self.drivers)
        code = self.rpc.code[driver.token], self.rpc.code[driver.vault]
        self.rpc.advance()
        self.rpc.storage[(driver.token, slot)] = '0x' + word(replacement)
        with self.assertRaisesRegex(CustodyError, 'implementation changed'):
            policy.verify_custody_routes(self.drivers)
        self.assertEqual((self.rpc.code[driver.token], self.rpc.code[driver.vault]), code)

    def test_each_route_independently_checks_its_genesis_pin(self):
        self.drivers[1].config['custody_policy']['genesis_hash'] = hash_value('wrong genesis')
        with self.assertRaisesRegex(CustodyError, 'genesis differs'):
            policy.verify_custody_routes(self.drivers)

    def test_different_rpc_or_profile_refuses_before_any_snapshot(self):
        self.drivers[1].rpc = SnapshotRpc('http://127.0.0.1:31338')
        with self.assertRaisesRegex(CustodyError, 'same pinned custody profile/source RPC'):
            policy.verify_custody_routes(self.drivers)
        self.assertEqual(self.rpc.exchanges, 0)

    def test_shared_snapshot_cannot_accept_missing_route_reply(self):
        self.rpc.fault = 'removed-result'
        with self.assertRaises(RpcError):
            policy.verify_custody_routes(self.drivers)

    def test_replaced_head_during_shared_reads_refuses_mixed_state(self):
        self.rpc.fault = 'reorg'
        with self.assertRaisesRegex(RpcError, 'snapshot changed'):
            policy.verify_custody_routes(self.drivers)

    def test_source_checkpoint_is_inside_same_three_exchange_snapshot(self):
        result = policy.verify_custody_routes(self.drivers, source_point=SOURCE)
        self.assertEqual(result, {'head': {'height': 10, 'hash': HEAD}, 'source_point': SOURCE})
        self.assertEqual(self.rpc.exchanges, 3)
        self.assertEqual(len(self.rpc.state_reads), 18)

    def test_altered_or_missing_source_block_is_refused(self):
        for block in (None, {'number': '0x9', 'hash': hash_value('replacement')},
                      {'number': '0x8', 'hash': SOURCE['hash']}):
            self.rpc.blocks[9] = block
            with self.assertRaisesRegex(CustodyError, 'source checkpoint is replaced, unavailable or not sufficiently confirmed'):
                policy.verify_custody_routes(self.drivers, source_point=SOURCE)

    def test_source_checkpoint_requires_two_confirmations_at_the_same_head(self):
        too_recent = {'height': 10, 'hash': HEAD, 'minimum_confirmations': 2}
        with self.assertRaisesRegex(CustodyError, 'not sufficiently confirmed'):
            policy.verify_custody_routes(self.drivers, source_point=too_recent)

    def test_source_policy_cannot_lower_or_reencode_confirmations(self):
        for bad in (1, 0, '2'):
            with self.assertRaisesRegex(CustodyError, 'lower the pinned finality'):
                policy.verify_custody_routes(self.drivers, source_point={**SOURCE, 'minimum_confirmations': bad})
        self.assertEqual(self.rpc.exchanges, 0)

    def test_source_combined_snapshot_rechecks_final_head_and_fork_identity(self):
        for fault in ('reorg', 'final-fork'):
            self.rpc.fault = fault
            self.rpc.digest, self.rpc.forked = HEAD, False
            with self.assertRaises((RpcError, CustodyError)):
                policy.verify_custody_routes(self.drivers, source_point=SOURCE)

    def test_source_lookup_preserves_complete_batch_bounds_and_forbids_latest(self):
        call = ('eth_getCode', [self.drivers[0].token])
        for heights in (['latest'], ['pending'], [True], [9, 9]):
            with self.assertRaises(CustodyError):
                self.rpc.state_batch([call], block_heights=heights)
        with self.assertRaises(CustodyError):
            self.rpc.state_batch([call] * 32, block_heights=[9])
        self.assertEqual(self.rpc.exchanges, 0)

    def test_production_source_guard_uses_only_the_shared_three_exchanges(self):
        from source_stream import SourceStream
        guard = SourceStream.__new__(SourceStream)
        guard.owner = SimpleNamespace(routes=dict(zip(('TOKENA', 'TOKENB', 'Q'), self.drivers)))
        guard.rpc, guard.committed = self.rpc, None
        guard.pending = {'history_tip': 'retained', 'source': {'to': SOURCE}}
        self.assertTrue(guard.guard('retained'))
        self.assertEqual(self.rpc.exchanges, 3)
        self.assertEqual(len(self.rpc.state_reads), 18)

    def test_legacy_source_guard_keeps_its_existing_fixture_and_checkpoint_reads(self):
        from source_stream import SourceStream
        for driver in self.drivers:
            driver.custody_profile = policy.LEGACY_PROFILE
        guard = SourceStream.__new__(SourceStream)
        guard.owner = SimpleNamespace(routes=dict(zip(('TOKENA', 'TOKENB', 'Q'), self.drivers)))
        guard.rpc, guard.committed = self.rpc, None
        guard.pending = {'history_tip': 'retained', 'source': {'to': SOURCE}}
        self.assertTrue(guard.guard('retained'))
        self.assertEqual(self.rpc.exchanges, 2)
        self.assertEqual(self.rpc.state_reads, [])


if __name__ == '__main__':
    unittest.main()
