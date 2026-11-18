#!/usr/bin/env python3
"""Offline adapter guard tests; actual fork end-to-end evidence is separate."""
import copy
import hashlib
import os
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import arbitrum_fork as fork
from arbitrum_assets import (ADMIN_SLOT, GENESIS_HASH, IMPLEMENTATION_SLOT, asset_spec)
import custody
import custody_policy as policy
from custody import CHAIN_ID, Q_ASSET, SYNTHETIC_ASSETS, CustodyError, LocalRpc

URL = 'https://arb1.arbitrum.io/rpc'
ORIGIN = {'height': 100, 'hash': '0x' + 'aa' * 32}


def context():
    spec = asset_spec('USDT0')
    pin = {'format': 'fm2-arbitrum-asset-pin/1', 'asset': 'USDT0', 'source_chain_id': 42161,
        'origin': ORIGIN.copy(), 'genesis_hash': GENESIS_HASH, 'mainnet_enabled': False,
        'identity': {'token': spec['address'], 'decimals': 6, 'token_code_hash': '0x' + 'bb' * 32,
            'proxy': {'kind': 'eip1967', 'implementation_slot': IMPLEMENTATION_SLOT,
                'implementation': '0x' + '11' * 20, 'implementation_code_hash': '0x' + 'cc' * 32,
                'admin_slot': ADMIN_SLOT, 'admin': '0x' + '22' * 20,
                'admin_code_hash': '0x' + 'dd' * 32, 'admin_owner': '0x' + '33' * 20},
            'issuer_roles': {role: '0x' + '44' * 20 for role in spec['issuer_role_signatures']}}}
    return {'format': fork.FORMAT, 'mainnet_enabled': False, 'origin': ORIGIN.copy(), 'asset_pin': pin,
        'provider_binding': {'operator_id': 'arbitrum-public', 'endpoint_id': 'archive-1',
            'endpoint_origin': 'https://arb1.arbitrum.io:443',
            'endpoint_commitment': hashlib.sha256(URL.encode()).hexdigest()}}


class Node:
    def __init__(self):
        self.chain = hex(CHAIN_ID)
        self.info = {'environment': {'chainId': CHAIN_ID}, 'forkConfig': {'forkUrl': URL, 'forkBlockNumber': 100}}
        self.origin, self.genesis = ORIGIN.copy(), GENESIS_HASH
        self.head = {'height': 110, 'hash': '0x' + 'ee' * 32}
        self.accounts = ['0x' + ('%02x' % (index + 1)) * 20 for index in range(6)]
        self.mutations, self.reads = [], []
        self.reserves = {'0x01': 0, '0x02': 0, '0x03': 0}
        self.after_state = lambda: None

    def exchange(self, payload):
        single = isinstance(payload, dict)
        replies = []
        for request in [payload] if single else payload:
            method, params = request['method'], request['params']
            self.reads.append((method, copy.deepcopy(params)))
            if method == 'eth_chainId': value = self.chain
            elif method == 'anvil_nodeInfo': value = self.info
            elif method == 'eth_getBlockByNumber':
                value = {'number': '0x0', 'hash': self.genesis} if params[0] == '0x0' else {
                    'number': hex(self.origin['height']), 'hash': self.origin['hash']} if params[0] == '0x64' else {
                    'number': hex(self.head['height']), 'hash': self.head['hash']}
            elif method == 'eth_getCode':
                value = '0x6001'
                self.after_state()
            elif method == 'eth_accounts': value = self.accounts
            elif method == 'eth_getBalance': value = hex(10 ** 18)
            elif method == 'eth_call': value = '0x' + self.reserves.get(params[0]['data'], 0).to_bytes(32, 'big').hex()
            elif method == 'eth_blockNumber': value = hex(self.head['height'])
            elif method in fork.ForkRpc.MUTATIONS:
                self.mutations.append((method, params))
                value = True
            else: raise AssertionError('unexpected RPC method ' + method)
            replies.append({'id': request['id'], 'jsonrpc': '2.0', 'result': copy.deepcopy(value)})
        return replies[0] if single else replies


class ProbeRpc(fork.ForkRpc):
    def __init__(self, node=None, config=None):
        self.node = node or Node()
        super().__init__('http://127.0.0.1:1234', config or context())

    def _exchange(self, payload):
        return self.node.exchange(payload)


class ForkBoundaryTests(unittest.TestCase):
    def test_explicit_context_is_deeply_pinned_and_original_unforked_guard_stays(self):
        value, node = context(), Node()
        rpc = ProbeRpc(node, value)
        self.assertEqual(rpc.assert_fixture(), node.info)
        value['origin']['hash'] = '0x' + '00' * 32
        rpc.fork_context['asset_pin']['identity']['token'] = '0x' + '99' * 20
        self.assertEqual(rpc.fork_context, context())
        with self.assertRaises(CustodyError):
            LocalRpc._fixture_identity(node.chain, node.info)

    def test_unknown_context_fields_activation_and_wrong_origin_refuse(self):
        for mutate in (lambda value: value.update(mainnet_enabled=True),
                       lambda value: value.update(allow_fork=True),
                       lambda value: value.update(origin={'height': 101, 'hash': ORIGIN['hash']}),
                       lambda value: value.update(provider_binding={}),
                       lambda value: value['asset_pin'].update(source_chain_id=31337)):
            value = context(); mutate(value)
            with self.assertRaises(CustodyError):
                ProbeRpc(config=value)
        for url in ('https://arb1.arbitrum.io/rpc', 'http://example.invalid:1234', 'http://localhost:1234'):
            with self.assertRaises(CustodyError):
                fork.ForkRpc(url, context())

    def test_source_and_provider_changes_block_every_mutation_before_send(self):
        for attack in ('chain', 'environment', 'height', 'url', 'genesis', 'origin', 'unforked', 'fork_chain'):
            node, rpc = Node(), None
            rpc = ProbeRpc(node)
            if attack == 'chain': node.chain = hex(42161)
            elif attack == 'environment': node.info['environment']['chainId'] = 42161
            elif attack == 'height': node.info['forkConfig']['forkBlockNumber'] = 101
            elif attack == 'url': node.info['forkConfig']['forkUrl'] = 'https://other.example/rpc'
            elif attack == 'genesis': node.genesis = '0x' + 'ff' * 32
            elif attack == 'origin': node.origin['hash'] = '0x' + 'ff' * 32
            elif attack == 'unforked': node.info['forkConfig'] = {'forkUrl': None, 'forkBlockNumber': None}
            elif attack == 'fork_chain': node.info['forkConfig']['forkChainId'] = 1
            with self.subTest(attack=attack), self.assertRaises(CustodyError):
                rpc.call('anvil_impersonateAccount', [node.accounts[0]])
            self.assertFalse(node.mutations)

    def test_read_only_batch_and_local_mutation_allowlists_remain_separate(self):
        rpc = ProbeRpc()
        self.assertTrue(rpc.call('anvil_setBalance', [rpc.node.accounts[0], hex(10 ** 18)]))
        self.assertTrue(rpc.call('eth_sendTransaction', [{'chainId': hex(CHAIN_ID)}]))
        for method, params in (('anvil_reset', []), ('anvil_setStorageAt', []),
                               ('eth_sendTransaction', [{}]), ('eth_sendTransaction', [{'chainId': hex(42161)}]),
                               ('anvil_setBalance', [rpc.node.accounts[0], hex(10 ** 23)])):
            with self.subTest(method=method, params=params), self.assertRaises(CustodyError):
                rpc.call(method, params)
        with self.assertRaises(CustodyError):
            rpc.batch([('anvil_impersonateAccount', [rpc.node.accounts[0]])])

    def test_state_batch_rechecks_origin_after_state_reads(self):
        rpc = ProbeRpc()
        state = rpc.state_batch([('eth_getCode', ['0x' + '12' * 20])])
        self.assertEqual(state['genesis']['hash'], GENESIS_HASH)
        self.assertEqual(state['results'], ['0x6001'])
        rpc.node.after_state = lambda: rpc.node.origin.update(hash='0x' + 'ff' * 32)
        with self.assertRaises(CustodyError):
            rpc.state_batch([('eth_getCode', ['0x' + '12' * 20])])

    def driver(self, rpc=None, asset=Q_ASSET):
        rpc = rpc or ProbeRpc()
        return SimpleNamespace(config={'regtest_arbitrum_fork': context(), 'custody_policy': {'minimum_confirmations': 2}},
            rpc=rpc, cast=SimpleNamespace(calldata=lambda signature, *_: {'balanceOf(address)': '0x01', 'locked()': '0x02',
                'pendingTotal()': '0x03'}[signature]), asset=asset, token=context()['asset_pin']['identity']['token'] if asset == Q_ASSET else '0x' + '77' * 20,
            vault='0x' + '88' * 20, custody_profile=policy.PROFILE)

    def test_q_route_checks_real_asset_each_time_but_base_route_checks_fork_only(self):
        driver = self.driver()
        with mock.patch.object(fork, 'verify_asset_pin', return_value={'identity': 'checked'}) as check:
            fork.verify_fork_driver(driver)
            fork.verify_fork_driver(driver, accounts=[driver.rpc.node.accounts[0]], check_compliance=True)
            self.assertEqual(check.call_count, 2)
            self.assertFalse(check.call_args_list[0].kwargs['check_compliance'])
            self.assertTrue(check.call_args_list[1].kwargs['check_compliance'])
            self.assertEqual(check.call_args.kwargs['accounts'], [driver.vault, driver.rpc.node.accounts[0]])
            fork.verify_fork_driver(self.driver(asset=SYNTHETIC_ASSETS['TOKENA']))
            self.assertEqual(check.call_count, 2)
        driver.token = '0x' + '99' * 20
        with self.assertRaises(CustodyError): fork.verify_fork_driver(driver)

    def test_context_removal_or_rebinding_cannot_turn_off_guard(self):
        driver = self.driver()
        del driver.config['regtest_arbitrum_fork']
        with self.assertRaises(CustodyError): policy.verify_custody(driver)
        driver.config['regtest_arbitrum_fork'] = context()
        driver.config['regtest_arbitrum_fork']['provider_binding']['endpoint_id'] = 'replacement'
        with self.assertRaises(CustodyError): fork.verify_fork_driver(driver)

    def test_real_reserve_deficit_is_fatal_before_any_soft_issuer_pause(self):
        driver = self.driver()
        for balance, locked, pending in ((9, 10, 1), (10, 10, 11), (custody.MAX_ATOMS + 1, custody.MAX_ATOMS + 1, 0)):
            driver.rpc.node.reserves = {'0x01': balance, '0x02': locked, '0x03': pending}
            with mock.patch.object(fork, 'verify_asset_pin', side_effect=CustodyError('would be a soft issuer pause')) as check:
                for compliance in (False, True):
                    with self.subTest(balance=balance, locked=locked, pending=pending, compliance=compliance), \
                            self.assertRaisesRegex(CustodyError, 'reserve deficit or pending/locked'):
                        fork.verify_fork_driver(driver, check_compliance=compliance)
                check.assert_not_called()

    def test_single_grouped_and_source_guards_each_verify_exact_fork_pin(self):
        driver = self.driver()
        calls = [('eth_getCode', [driver.token])]
        with mock.patch.object(fork, 'verify_asset_pin', return_value={}) as check, \
             mock.patch.object(policy, '_custody_reads', return_value=({'minimum_confirmations': 2}, [], calls)), \
             mock.patch.object(policy, '_verify_custody_snapshot', return_value='verified'), \
             mock.patch.object(policy, 'validate_policy_shape'):
            self.assertEqual(policy.verify_custody(driver), 'verified')
            self.assertEqual(policy.verify_custody_routes([driver]), ['verified'])
            point = dict(driver.rpc.node.origin, minimum_confirmations=2)
            self.assertTrue(policy.verify_source_point(driver, point))
            self.assertEqual(check.call_count, 3)
            altered = self.driver(); altered.config['regtest_arbitrum_fork']['provider_binding']['endpoint_id'] = 'other'
            with self.assertRaises(CustodyError): policy.verify_custody_routes([driver, altered])

    def test_factory_is_explicit_and_synthetic_and_real_funding_paths_stay_separate(self):
        rpc = ProbeRpc()
        cast = SimpleNamespace(test_address=lambda index: rpc.node.accounts[index])
        config = {'rpc_url': rpc.url, 'cast': 'unused', 'instance': '0x' + '99' * 32,
                  'regtest_arbitrum_fork': context()}
        with tempfile.TemporaryDirectory() as directory, mock.patch.object(custody, 'Cast', return_value=cast), \
             mock.patch.object(fork, 'ForkRpc', return_value=rpc) as factory:
            driver = custody.CustodyDriver(config, directory)
            factory.assert_called_once_with(rpc.url, config['regtest_arbitrum_fork'])
        driver.token, driver.vault = context()['asset_pin']['identity']['token'], '0x' + '88' * 20
        driver.cast = mock.Mock()
        with mock.patch.object(fork, 'verify_fork_driver') as guard, \
             mock.patch('arbitrum_assets.fund_local_fork_account', return_value={'funding': 'artificial'}) as fund:
            self.assertEqual(driver.mint(0, 123), {'funding': 'artificial'})
            guard.assert_called_once_with(driver)
            fund.assert_called_once_with(rpc, driver.cast, config['regtest_arbitrum_fork']['asset_pin'], rpc.node.accounts[0], 123)
        driver.asset = SYNTHETIC_ASSETS['TOKENA']
        driver.transact = mock.Mock(return_value={'synthetic': True})
        driver.cast.calldata.return_value = '0x1234'
        self.assertEqual(driver.mint(0, 456), {'synthetic': True})
        driver.cast.calldata.assert_called_once_with('mint(address,uint256)', rpc.node.accounts[0], 456)

    def test_actual_asset_guard_preserves_identity_checks_during_issuer_block(self):
        from arbitrum_assets import AssetComplianceBlocked, capture_asset_pin
        from test_arbitrum_assets import FakeCast, FakeRpc
        cast = FakeCast()
        source = FakeRpc(cast)
        original = capture_asset_pin(source, cast, 'USDT0', source.origin)
        source.fork()
        source.node['forkConfig']['forkUrl'] = URL
        value = context(); value['origin'], value['asset_pin'] = source.origin.copy(), original
        class Adapter(fork.ForkRpc):
            def _exchange(self, payload):
                single = isinstance(payload, dict)
                requests = [payload] if single else payload
                results = []
                for request in requests:
                    if request['method'] == 'eth_call':
                        signature, args = cast.arguments[request['params'][0]['data']]
                        if signature in ('balanceOf(address)', 'locked()', 'pendingTotal()'):
                            results.append('0x' + bytes(32).hex())
                            continue
                    results.extend(source.batch([(request['method'], request['params'])]))
                replies = [{'jsonrpc': '2.0', 'id': request['id'], 'result': result}
                           for request, result in zip(requests, results)]
                return replies[0] if single else replies
        rpc = Adapter('http://127.0.0.1:1234', value)
        driver = self.driver(rpc)
        driver.config['regtest_arbitrum_fork'], driver.cast = value, cast
        source.blocked[driver.vault] = 1
        observed = fork.verify_fork_driver(driver)
        self.assertTrue(observed['status']['blocked'][driver.vault])
        with self.assertRaises(AssetComplianceBlocked):
            fork.verify_fork_driver(driver, check_compliance=True)
        source.codes['implementation'] = '0x6004'
        try:
            fork.verify_fork_driver(driver, check_compliance=True)
        except AssetComplianceBlocked:
            self.fail('issuer pause concealed a changed implementation identity')
        except CustodyError:
            pass
        else:
            self.fail('changed implementation was accepted during issuer pause')


class ForkRelayerComplianceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # The production relayer is POSIX-only; these tests exercise its pure
        # admission/prepublication methods, never its file lock or key runtime.
        if os.name == 'nt':
            with mock.patch.dict(sys.modules, {'fcntl': SimpleNamespace()}):
                import signed_relayer
        else:
            import signed_relayer
        cls.Relayer = signed_relayer.DurableSignedRelayer

    def setUp(self):
        self.identity, self.recipient = '0x' + 'ab' * 32, '0x' + 'cd' * 20
        self.action = {'operation': 'request', 'withdrawal_id': self.identity}
        fields = [bytes.fromhex(self.identity[2:]), bytes(32), bytes(32), (1).to_bytes(32, 'big'),
                  bytes(12) + bytes.fromhex(self.recipient[2:]), bytes(32), bytes(32), bytes(32), bytes(32)]
        self.data = '0x12345678' + b''.join(fields).hex()
        self.finalize_data = '0x87654321' + self.identity[2:]
        self.subject = self.Relayer.__new__(self.Relayer)
        self.subject.fenced = False
        self.subject.driver = SimpleNamespace(config={'regtest_arbitrum_fork': context()},
            cast=SimpleNamespace(keccak=lambda _: '0x12345678' + '00' * 28,
                                 calldata=lambda *_: self.finalize_data),
            withdrawal_status=lambda _: {'recipient': self.recipient}, verify_contract_identity=mock.Mock())

    def test_exact_request_and_finalization_recipient_are_freshly_checked(self):
        with mock.patch.object(fork, 'verify_fork_driver') as guard:
            self.subject._fork_compliance(self.action, self.data)
            self.subject._fork_compliance(dict(self.action, operation='finalize'), self.finalize_data)
            self.assertEqual(guard.call_count, 2)
            for call in guard.call_args_list:
                self.assertEqual(call.args, (self.subject.driver,))
                self.assertEqual(call.kwargs, {'accounts': [self.recipient], 'check_compliance': True})

    def test_modified_recovery_calldata_or_action_cannot_choose_a_recipient(self):
        for action, data in ((dict(self.action, withdrawal_id='0x' + 'ff' * 32), self.data),
                             (self.action, '0x99999999' + self.data[10:]),
                             (self.action, self.data[:10]),
                             (dict(self.action, operation='other'), self.data),
                             (dict(self.action, operation='finalize'), self.data)):
            with self.subTest(action=action, data=data[:18]), self.assertRaises(CustodyError):
                self.subject._fork_compliance(action, data)
        malformed = self.data[:2 + (4 + 4 * 32) * 2] + '01' + self.data[2 + (4 + 4 * 32) * 2 + 2:]
        with self.assertRaises(CustodyError): self.subject._fork_compliance(self.action, malformed)

    def test_old_unforked_relayer_retains_exact_existing_path(self):
        self.subject.driver.config = {}
        with mock.patch.object(fork, 'verify_fork_driver') as guard:
            self.subject._fork_compliance({'operation': 'legacy'}, '0x')
            guard.assert_not_called()

    def test_blocked_recovery_never_rebroadcasts_or_allocates_new_transaction(self):
        self.subject.intents = {0: {'transaction_hash': '0x' + '12' * 32,
            'payload': {'data': self.data}, 'raw_transaction': '0xab', 'action': self.action}}
        self.subject.sender = '0x' + '34' * 20
        self.subject.driver._find_nonce = lambda _: None
        seen = []
        def rpc(method, params):
            seen.append(method)
            if method in ('eth_getTransactionByHash', 'eth_getTransactionReceipt'): return None
            if method == 'eth_getTransactionCount': return '0x0'
            raise AssertionError('unexpected publication or RPC: ' + method)
        self.subject.driver.rpc = SimpleNamespace(call=rpc)
        self.subject._fork_compliance = mock.Mock(side_effect=CustodyError('test recipient blocked'))
        with self.assertRaisesRegex(CustodyError, 'recipient blocked'):
            self.subject._resume(0)
        self.assertNotIn('eth_sendRawTransaction', seen)
        self.subject._fork_compliance.assert_called_once_with(self.action, self.data)

    def test_already_canonical_receipt_reconciles_without_a_new_payout(self):
        tx = '0x' + '12' * 32
        receipt = {'blockNumber': '0x64', 'blockHash': ORIGIN['hash'], 'status': '0x1'}
        payload = {'data': self.data}
        self.subject.intents = {0: {'transaction_hash': tx, 'payload': payload,
                                   'raw_transaction': '0xab', 'action': self.action}}
        self.subject.resolved, self.subject._append = {}, mock.Mock()
        self.subject.driver._same_transaction = lambda *_: True
        self.subject.driver.canonical_receipt = lambda *_, **__: receipt
        def rpc(method, params):
            if method == 'eth_getTransactionByHash': return {'hash': tx}
            if method == 'eth_getTransactionReceipt': return receipt
            if method == 'eth_getBlockByNumber': return {'hash': ORIGIN['hash']}
            raise AssertionError('unexpected publication or RPC: ' + method)
        self.subject.driver.rpc = SimpleNamespace(call=rpc)
        self.subject._fork_compliance = mock.Mock(side_effect=CustodyError('recipient now blocked'))
        self.assertEqual(self.subject._resume(0), receipt)
        self.subject._fork_compliance.assert_not_called()
        self.subject._append.assert_called_once_with('receipt', {'nonce': 0, 'transaction_hash': tx, 'receipt': receipt})

    def test_new_signature_rechecks_recipient_after_fee_and_nonce_reads(self):
        self.subject.intents, self.subject.resolved = {}, {}
        self.subject.sender, self.subject.seed = '0x' + '12' * 20, 'ab' * 32
        self.subject.worker = mock.Mock()
        self.subject.driver.vault = '0x' + '88' * 20
        self.subject.driver.withdrawal_tuple = lambda _: '()'
        self.subject.driver.cast.calldata = lambda *_: self.data
        def rpc(method, params=()):
            if method == 'eth_getTransactionCount': return '0x0'
            if method == 'eth_gasPrice': return '0x1'
            if method == 'eth_estimateGas': return hex(21000)
            raise AssertionError('unexpected mutation or RPC: ' + method)
        self.subject.driver.rpc = SimpleNamespace(call=rpc)
        self.subject._fork_compliance = mock.Mock(side_effect=[None, CustodyError('blocked before signing')])
        with self.assertRaisesRegex(CustodyError, 'blocked before signing'):
            self.subject.transact('request', {'withdrawal': {'id': self.identity}, 'signatures': ['0x' + '11' * 65] * 3})
        self.assertEqual(self.subject._fork_compliance.call_count, 2)
        self.subject.worker.request.assert_not_called()


if __name__ == '__main__':
    unittest.main()
