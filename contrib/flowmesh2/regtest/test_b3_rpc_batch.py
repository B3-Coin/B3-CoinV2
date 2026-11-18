#!/usr/bin/env python3
"""Strict authenticated RPC batches retain every canonical anchor guard."""
import base64
import copy
import json
from pathlib import Path
import tempfile
import unittest

from custody import CustodyError
from local_process import B3Rpc, MODERN_GENESIS


class Reply:
    def __init__(self, data):
        self.data = data

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return False

    def read(self, maximum):
        return self.data[:maximum]


class B3RpcBatchTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        cookie = Path(self.directory.name) / 'rpc.cookie'
        cookie.write_text('test-user:test-password', encoding='utf-8')
        self.rpc = B3Rpc('http://127.0.0.1:18443/', cookie)
        self.anchor = {'height': 17, 'hash': bytes(range(32)).hex()}
        self.block_hash = bytes(range(32))[::-1].hex()
        self.requests = []
        self.transform = lambda replies: replies
        test = self

        class Opener:
            def open(self, request, timeout):
                test.assertEqual(timeout, 10)
                test.assertEqual(request.full_url, 'http://127.0.0.1:18443/')
                test.assertEqual(request.get_header('Authorization'),
                                 'Basic ' + base64.b64encode(b'test-user:test-password').decode())
                payload = json.loads(request.data)
                test.requests.append(payload)
                items = payload if isinstance(payload, list) else [payload]
                replies = [{'jsonrpc': '2.0', 'id': item['id'], 'result': test.value(item)} for item in items]
                value = test.transform(replies if isinstance(payload, list) else replies[0])
                return Reply(value if isinstance(value, bytes) else json.dumps(value).encode())

        self.rpc.opener = Opener()

    def value(self, request):
        method, params = request['method'], request['params']
        if method == 'getblockchaininfo':
            return {'chain': 'regtest', 'blocks': 20}
        if method == 'getblockhash':
            return MODERN_GENESIS if params == [0] else self.block_hash
        if method == 'getpeerinfo':
            return [{'addr': '127.0.0.1:18444'}, {'addr': '[::1]:18445'}]
        if method == 'getblockheader':
            return {'height': 17, 'confirmations': 3, 'hash': self.block_hash}
        return {}

    def test_one_authenticated_transaction_verifies_all_five_known_queries(self):
        self.transform = lambda replies: list(reversed(replies))
        self.assertEqual(self.rpc.verify_anchor(self.anchor), self.anchor)
        self.assertEqual(len(self.requests), 1)
        calls = self.requests[0]
        self.assertEqual([(item['method'], item['params']) for item in calls], [
            ('getblockchaininfo', []), ('getblockhash', [0]), ('getpeerinfo', []),
            ('getblockhash', [17]), ('getblockheader', [self.block_hash])])
        self.assertEqual(len({item['id'] for item in calls}), 5)

    def test_response_order_and_identity_allocation_do_not_alias(self):
        self.transform = lambda replies: list(reversed(replies))
        values = self.rpc.batch([('getblockchaininfo', []), ('getblockhash', [0])])
        self.assertEqual(values, [{'chain': 'regtest', 'blocks': 20}, MODERN_GENESIS])
        self.transform = lambda reply: reply
        self.assertEqual(self.rpc.call('getblockhash', 0), MODERN_GENESIS)
        self.assertEqual(self.requests[1]['id'], 3)

    def test_unknown_duplicate_missing_and_partial_error_replies_fail(self):
        def changed(replies, kind):
            replies = copy.deepcopy(replies)
            if kind == 'unknown':
                replies[0]['id'] = 9999
            elif kind == 'duplicate':
                replies[0]['id'] = replies[1]['id']
            elif kind == 'missing':
                replies.pop()
            elif kind == 'extra':
                replies.append(replies[0])
            elif kind == 'error':
                replies[0]['error'] = {'code': -1, 'message': 'refused'}
            elif kind == 'boolean_id':
                replies[0]['id'] = True
            elif kind == 'protocol':
                replies[0]['jsonrpc'] = '1.0'
            elif kind == 'no_result':
                del replies[0]['result']
            elif kind == 'unknown_field':
                replies[0]['untrusted'] = True
            return replies
        for kind in ('unknown', 'duplicate', 'missing', 'extra', 'error', 'boolean_id', 'protocol', 'no_result', 'unknown_field'):
            with self.subTest(kind=kind):
                self.transform = lambda replies, kind=kind: changed(replies, kind)
                with self.assertRaises(CustodyError):
                    self.rpc.verify_anchor(self.anchor)

    @staticmethod
    def rpc_error(replies, position, code, omit_result=False):
        replies[position]['error'] = {'code': code, 'message': 'untrusted RPC diagnostic'}
        if omit_result:
            replies[position].pop('result', None)
        else:
            replies[position]['result'] = None

    def test_missing_anchor_header_or_height_has_a_precise_anchor_diagnostic(self):
        for failures in (((4, -5),), ((3, -8),), ((3, -8), (4, -5))):
            for omit_result in (False, True):
                def changed(replies, failures=failures, omit_result=omit_result):
                    for position, code in failures:
                        self.rpc_error(replies, position, code, omit_result)
                    return list(reversed(replies))
                self.transform = changed
                with self.subTest(failures=failures, omit_result=omit_result):
                    with self.assertRaisesRegex(CustodyError, '^B3 anchor is unavailable or noncanonical'):
                        self.rpc.verify_anchor(self.anchor)

    def test_missing_anchor_never_masks_malformed_batch_or_error_envelopes(self):
        for attack in ('wrong_id', 'duplicate_id', 'missing_reply', 'protocol', 'unknown_field',
                       'missing_result', 'error_boolean_code', 'error_float_code', 'error_no_message',
                       'error_unknown_field', 'error_with_result', 'error_not_object'):
            def changed(replies, attack=attack):
                self.rpc_error(replies, 4, -5)
                if attack == 'wrong_id': replies[0]['id'] = 9999
                elif attack == 'duplicate_id': replies[0]['id'] = replies[1]['id']
                elif attack == 'missing_reply': replies.pop(0)
                elif attack == 'protocol': replies[0]['jsonrpc'] = '1.0'
                elif attack == 'unknown_field': replies[0]['extra'] = True
                elif attack == 'missing_result': replies[0].pop('result')
                elif attack == 'error_boolean_code': replies[4]['error']['code'] = True
                elif attack == 'error_float_code': replies[4]['error']['code'] = -5.0
                elif attack == 'error_no_message': replies[4]['error'].pop('message')
                elif attack == 'error_unknown_field': replies[4]['error']['extra'] = True
                elif attack == 'error_with_result': replies[4]['result'] = {'hash': self.block_hash}
                elif attack == 'error_not_object': replies[4]['error'] = 'Block not found'
                return list(reversed(replies))  # Valid missing-anchor error is encountered first.
            self.transform = changed
            with self.subTest(attack=attack), self.assertRaises(CustodyError) as caught:
                self.rpc.verify_anchor(self.anchor)
            self.assertNotIn('anchor', str(caught.exception))

    def test_only_the_expected_anchor_query_and_error_code_are_classified(self):
        for failures in (((0, -5),), ((1, -8),), ((2, -5),), ((3, -5),), ((4, -8),),
                         ((4, -1),), ((4, -5), (0, -1))):
            def changed(replies, failures=failures):
                for position, code in failures:
                    self.rpc_error(replies, position, code)
                return list(reversed(replies))
            self.transform = changed
            with self.subTest(failures=failures), self.assertRaises(CustodyError) as caught:
                self.rpc.verify_anchor(self.anchor)
            self.assertEqual(str(caught.exception), 'local B3 RPC refused or returned a mismatched response')
        def single_error(reply):
            self.rpc_error([reply], 0, -5)
            return reply
        self.transform = single_error
        with self.assertRaises(CustodyError) as caught:
            self.rpc.call('getblockheader', self.block_hash)
        self.assertNotIn('anchor', str(caught.exception))

    def test_missing_anchor_preserves_fixture_identity_refusals(self):
        for attack in ('chain', 'genesis', 'peer'):
            def changed(replies, attack=attack):
                self.rpc_error(replies, 4, -5)
                if attack == 'chain': replies[0]['result']['chain'] = 'main'
                elif attack == 'genesis': replies[1]['result'] = '00' * 32
                elif attack == 'peer': replies[2]['result'] = [{'addr': '192.0.2.1:18444'}]
                return replies
            self.transform = changed
            with self.subTest(attack=attack), self.assertRaises(CustodyError) as caught:
                self.rpc.verify_anchor(self.anchor)
            self.assertNotIn('anchor', str(caught.exception))

    def test_duplicate_fields_non_json_nonfinite_and_oversize_fail(self):
        for payload in (b'[{"id":1,"id":1,"result":0}]', b'not-json', b'\xff',
                        b'[{"id":1,"result":NaN}]', b' ' * 4_194_305):
            with self.subTest(prefix=payload[:30]):
                self.transform = lambda replies, payload=payload: payload
                with self.assertRaises(CustodyError):
                    self.rpc.batch([('getblockhash', [0])])

    def test_batch_rejects_mutations_unbounded_requests_and_unknown_shapes(self):
        for calls in ([], [('getblockhash', [0])] * 17, [('stop', [])], [('invalidateblock', ['00' * 32])],
                      [('generatetodescriptor', [1, 'raw(51)'])], [('getblockhash', 0)], [('getblockhash',)],
                      [('getblockheader', ['x' * 65536])]):
            with self.subTest(shape=str(calls)[:100]):
                with self.assertRaises(CustodyError):
                    self.rpc.batch(calls)
        self.assertEqual(self.requests, [])

    def test_chain_genesis_and_peer_loopback_guards_are_not_cached(self):
        for attack in ('chain', 'genesis', 'peer', 'invalid_peer'):
            def changed(replies, attack=attack):
                if attack == 'chain':
                    replies[0]['result']['chain'] = 'main'
                elif attack == 'genesis':
                    replies[1]['result'] = '00' * 32
                elif attack == 'peer':
                    replies[2]['result'] = [{'addr': '192.0.2.1:18444'}]
                elif attack == 'invalid_peer':
                    replies[2]['result'] = [{'addr': '[invalid]:18444'}]
                return replies
            self.transform = changed
            with self.subTest(attack=attack), self.assertRaises(CustodyError):
                self.rpc.verify_anchor(self.anchor)
        self.assertEqual(len(self.requests), 4)

    def test_anchor_canonical_height_hash_and_confirmation_guards_remain(self):
        for attack in ('height', 'boolean_height', 'hash_at_height', 'header_hash', 'confirmations'):
            def changed(replies, attack=attack):
                if attack == 'height':
                    replies[4]['result']['height'] = 18
                elif attack == 'boolean_height':
                    replies[4]['result']['height'] = True
                elif attack == 'hash_at_height':
                    replies[3]['result'] = '00' * 32
                elif attack == 'header_hash':
                    replies[4]['result']['hash'] = '00' * 32
                elif attack == 'confirmations':
                    replies[4]['result']['confirmations'] = 0
                return replies
            self.transform = changed
            with self.subTest(attack=attack), self.assertRaises(CustodyError):
                self.rpc.verify_anchor(self.anchor)

    def test_assert_regtest_still_precedes_each_local_chain_mutation(self):
        self.assertEqual(self.rpc.call('generatetodescriptor', 1, 'raw(51)'), {})
        self.assertEqual(len(self.requests), 2)
        self.assertEqual([item['method'] for item in self.requests[0]],
                         ['getblockchaininfo', 'getblockhash', 'getpeerinfo'])
        self.assertEqual(self.requests[1]['method'], 'generatetodescriptor')


if __name__ == '__main__':
    unittest.main()
