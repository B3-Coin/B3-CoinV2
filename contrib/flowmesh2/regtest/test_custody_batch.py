#!/usr/bin/env python3
"""RPC boundary tests: complete ID sets and one canonical mutable snapshot.

Transport replies are local test doubles; this file never opens a provider or
modifies a chain. Actual custody/proxy roundtrips separately exercise Anvil.
"""
import copy
import io
import json
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from custody import CHAIN_ID, CustodyError, LocalRpc, RpcError
from custody_policy import POLICY_FORMAT, verify_custody


def hash_value(number):
    return '0x' + number.to_bytes(32, 'big').hex()


class Reply(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *arguments):
        self.close()


class Transport:
    def __init__(self, answer):
        self.answer, self.requests = answer, []

    def open(self, request, timeout):
        payload = json.loads(request.data)
        self.requests.append(payload)
        result = self.answer(payload)
        return Reply(result if isinstance(result, bytes) else json.dumps(result).encode())


def success(request, value):
    return {'jsonrpc': '2.0', 'id': request['id'], 'result': value}


class RpcBatchTests(unittest.TestCase):
    def rpc(self, answer):
        rpc = LocalRpc('http://127.0.0.1:1')
        rpc.opener = Transport(answer)
        return rpc

    def test_order_independent_response_binds_exact_request_ids(self):
        rpc = self.rpc(lambda requests: [success(item, item['method']) for item in reversed(requests)])
        self.assertEqual(rpc.batch([('eth_chainId', []), ('eth_blockNumber', [])]),
                         ['eth_chainId', 'eth_blockNumber'])

    def test_missing_duplicate_unexpected_and_boolean_ids_refuse(self):
        def missing(requests):
            return [success(requests[0], 1)]
        def duplicate(requests):
            return [success(requests[0], 1), success(requests[0], 2)]
        def unexpected(requests):
            return [success(requests[0], 1), {'jsonrpc': '2.0', 'id': 999, 'result': 2}]
        def boolean(requests):
            return [{'jsonrpc': '2.0', 'id': True, 'result': 1}, success(requests[1], 2)]
        for answer in (missing, duplicate, unexpected, boolean):
            with self.subTest(answer=answer.__name__):
                with self.assertRaises(RpcError):
                    self.rpc(answer).batch([('eth_chainId', []), ('eth_blockNumber', [])])

    def test_any_error_partial_envelope_or_duplicate_json_field_refuses_entire_batch(self):
        def rpc_error(requests):
            return [success(requests[0], 1), {'jsonrpc': '2.0', 'id': requests[1]['id'], 'error': {'code': -1}}]
        def result_and_error(requests):
            return [success(requests[0], 1), {**success(requests[1], 2), 'error': {'code': -1}}]
        def absent_result(requests):
            return [success(requests[0], 1), {'jsonrpc': '2.0', 'id': requests[1]['id']}]
        def duplicate_field(requests):
            first, second = requests[0]['id'], requests[1]['id']
            return ('[{"jsonrpc":"2.0","id":%d,"result":1,"result":2},'
                    '{"jsonrpc":"2.0","id":%d,"result":3}]' % (first, second)).encode()
        for answer in (rpc_error, result_and_error, absent_result, duplicate_field):
            with self.subTest(answer=answer.__name__):
                with self.assertRaises(RpcError):
                    self.rpc(answer).batch([('eth_chainId', []), ('eth_blockNumber', [])])

    def test_non_array_unknown_response_fields_and_nonfinite_json_refuse(self):
        cases = [lambda requests: success(requests[0], 1),
                 lambda requests: [{**success(requests[0], 1), 'unrecognized': True}],
                 lambda requests: b'[{"jsonrpc":"2.0","id":1,"result":NaN}]']
        for answer in cases:
            with self.assertRaises(RpcError):
                self.rpc(answer).batch([('eth_chainId', [])])

    def test_write_methods_and_item_bounds_are_rejected_before_transport(self):
        rpc = self.rpc(lambda requests: self.fail('invalid batch reached transport'))
        for calls in ([], [('eth_chainId', [])] * 33, [('eth_sendTransaction', [{}])],
                      [('evm_revert', ['0x1'])], [('eth_getCode', None)]):
            with self.subTest(calls=calls):
                with self.assertRaises(CustodyError):
                    rpc.batch(calls)
        self.assertEqual(rpc.opener.requests, [])

    def test_request_and_response_byte_bounds_refuse(self):
        rpc = self.rpc(lambda requests: b' ' * (LocalRpc.MAX_RESPONSE_BYTES + 1))
        with self.assertRaises(RpcError):
            rpc.batch([('eth_chainId', [])])
        rpc = self.rpc(lambda requests: self.fail('oversized request reached transport'))
        with self.assertRaises(RpcError):
            rpc.batch([('eth_call', [{'to': '0x' + '11' * 20, 'data': '0x' + 'aa' * 600000}, 'latest'])])


class SnapshotTransport:
    def __init__(self):
        self.state_blocks = []
        self.hash = hash_value(10)
        self.final_hash = self.hash
        self.chain = hex(CHAIN_ID)
        self.fork = None
        self.reads = 0

    def answer(self, payload):
        def result(item):
            method, params = item['method'], item['params']
            if method == 'eth_chainId':
                return self.chain
            if method == 'anvil_nodeInfo':
                return {'environment': {'chainId': CHAIN_ID},
                        'forkConfig': {'forkUrl': self.fork, 'forkBlockNumber': None}}
            if method == 'eth_getBlockByNumber':
                if params[0] == '0x0':
                    return {'number': '0x0', 'hash': hash_value(0)}
                return {'number': '0xa', 'hash': self.hash if params[0] == 'latest' else self.final_hash}
            self.state_blocks.append((method, params[-1]))
            self.reads += 1
            return '0x' + '00' * 32 if method == 'eth_getStorageAt' else '0x6001'
        if isinstance(payload, list):
            return [success(item, result(item)) for item in reversed(payload)]
        return success(payload, result(payload))


class CanonicalSnapshotTests(unittest.TestCase):
    def rpc(self, node):
        rpc = LocalRpc('http://127.0.0.1:1')
        rpc.opener = Transport(node.answer)
        return rpc

    @staticmethod
    def reads():
        return [('eth_getCode', ['0x' + '11' * 20]),
                ('eth_getStorageAt', ['0x' + '11' * 20, hash_value(1)]),
                ('eth_call', [{'to': '0x' + '22' * 20, 'data': '0x12345678'}])]

    def test_every_mutable_read_uses_one_pinned_number_then_separate_hash_recheck(self):
        node = SnapshotTransport(); rpc = self.rpc(node)
        snapshot = rpc.state_batch(self.reads())
        self.assertEqual(snapshot['head'], {'height': 10, 'hash': hash_value(10)})
        self.assertEqual(node.state_blocks, [('eth_call', '0xa'), ('eth_getStorageAt', '0xa'), ('eth_getCode', '0xa')])
        self.assertEqual(len(rpc.opener.requests), 3)
        self.assertEqual(rpc.opener.requests[-1]['params'], ['0xa', False])
        self.assertEqual(len(snapshot['results']), 3)

    def test_replaced_snapshot_hash_refuses_mixed_state(self):
        node = SnapshotTransport(); node.final_hash = hash_value(11)
        with self.assertRaisesRegex(RpcError, 'mixed source state'):
            self.rpc(node).state_batch(self.reads())

    def test_wrong_chain_or_fork_refuses_before_reading_mutable_state(self):
        for key, value in [('chain', '0x1'), ('fork', 'http://forbidden.invalid')]:
            node = SnapshotTransport(); setattr(node, key, value)
            with self.assertRaises(CustodyError):
                self.rpc(node).state_batch(self.reads())
            self.assertEqual(node.reads, 0)

    def test_state_reads_are_not_cached_between_guards(self):
        node = SnapshotTransport(); rpc = self.rpc(node)
        rpc.state_batch(self.reads())
        node.final_hash = hash_value(11)
        with self.assertRaises(RpcError):
            rpc.state_batch(self.reads())
        self.assertEqual(node.reads, 6)

    def test_caller_cannot_supply_latest_or_state_overrides(self):
        node = SnapshotTransport(); rpc = self.rpc(node)
        for calls in ([('eth_getCode', ['0x' + '11' * 20, 'latest'])],
                      [('eth_call', [{'to': '0x' + '11' * 20, 'data': '0x', 'from': '0x' + '33' * 20}])],
                      [('eth_getBalance', ['0x' + '11' * 20])]):
            with self.assertRaises(CustodyError):
                rpc.state_batch(calls)
        self.assertEqual(rpc.opener.requests, [])


class PolicyBatchTests(unittest.TestCase):
    """Check that batching retained every mutable custody authority fence."""
    def setUp(self):
        self.policy = {'format': POLICY_FORMAT, 'chain_id': 31337, 'genesis_hash': hash_value(0),
            'token_code_hash': hash_value(1), 'vault_code_hash': hash_value(2), 'policy_hash': hash_value(3),
            'domain_separator': hash_value(4), 'roster_hash': hash_value(5), 'roster_epoch': 0,
            'dispute_seconds': 30, 'rotation_seconds': 60, 'max_certificate_lifetime': 3600,
            'minimum_confirmations': 2, 'finality_mode': 'isolated-confirmations',
            'mainnet_enabled': False, 'proxy_implementation': None}
        self.token, self.vault = '0x' + '11' * 20, '0x' + '22' * 20
        self.identities = [31337, self.policy['token_code_hash'], self.policy['policy_hash'],
            self.policy['domain_separator'], self.policy['roster_hash'], 0, 30, 60, 3600,
            self.token, hash_value(6), hash_value(7), 0, hash_value(0)]
        def keccak(value):
            return {'0x6001': hash_value(1), '0x6002': hash_value(2), '0x6003': hash_value(8),
                    'org.zeppelinos.proxy.implementation': hash_value(99)}[value]
        self.cast = SimpleNamespace(keccak=keccak, calldata=lambda value: '0x12345678')
        self.calls = None
        self.code = '0x6001'
        self.genesis = hash_value(0)
        self.storage = {}
        self.driver = SimpleNamespace(config={'custody_policy': self.policy}, token=self.token, vault=self.vault,
            asset=hash_value(6), instance=hash_value(7), cast=self.cast, operator_addresses=[])
        self.driver.rpc = SimpleNamespace(state_batch=self.snapshot)

    def snapshot(self, calls):
        self.calls = calls
        identity = '0x' + ''.join((value.to_bytes(32, 'big') if isinstance(value, int) else
                                 bytes.fromhex(value[2:]).rjust(32, b'\0')).hex() for value in self.identities)
        result = []
        for method, params in calls:
            if method == 'eth_getCode':
                result.append(self.code if params[0] == self.token else '0x6002' if params[0] == self.vault else '0x6003')
            elif method == 'eth_call':
                result.append(identity)
            else:
                result.append(self.storage.get(params[1], hash_value(0)))
        return {'head': {'height': 10, 'hash': hash_value(10)},
                'genesis': {'height': 0, 'hash': self.genesis}, 'results': result}

    def verify(self):
        expected = (self.policy['roster_hash'], self.policy['policy_hash'], self.policy['domain_separator'])
        with patch('custody_policy._expected_hashes', return_value=expected):
            return verify_custody(self.driver)

    def test_batched_identity_retains_pause_epoch_code_and_genesis_fences(self):
        self.assertEqual(self.verify(), self.policy)
        for field, bad in [(12, 1), (5, 1), (6, 31), (4, hash_value(99))]:
            original = self.identities[field]; self.identities[field] = bad
            with self.assertRaises(CustodyError):
                self.verify()
            self.identities[field] = original
        self.code = '0x6003'
        with self.assertRaises(CustodyError):
            self.verify()
        self.code, self.genesis = '0x6001', hash_value(99)
        with self.assertRaises(CustodyError):
            self.verify()

    def test_unconfigured_proxy_and_same_runtime_implementation_change_still_refuse(self):
        slot = '0x360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc'
        implementation = '0x' + '33' * 20
        self.storage[slot] = '0x' + '00' * 12 + implementation[2:]
        with self.assertRaisesRegex(CustodyError, 'proxy requires'):
            self.verify()
        self.policy['proxy_implementation'] = {'slot': slot, 'address': implementation, 'code_hash': hash_value(8)}
        self.assertEqual(self.verify(), self.policy)
        self.assertIn(('eth_getCode', [implementation]), self.calls)
        self.storage[slot] = '0x' + '00' * 12 + '44' * 20
        with self.assertRaisesRegex(CustodyError, 'implementation changed'):
            self.verify()


if __name__ == '__main__':
    unittest.main()
