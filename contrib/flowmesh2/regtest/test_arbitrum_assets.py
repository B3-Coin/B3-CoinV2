#!/usr/bin/env python3
"""Offline read-boundary tests, not real-chain compatibility evidence."""
import copy
import hashlib
import unittest

from custody import CustodyError
from arbitrum_assets import (ADMIN_SLOT, BEACON_SLOT, CHAIN_ID, CIRCLE_ADMIN_SLOT, CIRCLE_IMPLEMENTATION_SLOT,
    GENESIS_HASH, IMPLEMENTATION_SLOT, LOCAL_CHAIN_ID, asset_spec, capture_asset_pin, fund_local_fork_account,
    AssetComplianceBlocked, build_sponsored_deposit, validate_asset_pin_shape, verify_asset_pin)


def word(value):
    return '0x' + (int(value, 16) if isinstance(value, str) else value).to_bytes(32, 'big').hex()


class FakeCast:
    """Only deterministic test encodings; never represented as Ethereum crypto."""
    def __init__(self):
        self.arguments = {}

    def calldata(self, signature, *args):
        encoded = '0x' + hashlib.sha256(repr((signature, args)).encode()).hexdigest()
        self.arguments[encoded] = (signature, args)
        return encoded

    def keccak(self, value):
        return '0x' + hashlib.sha256(value.encode()).hexdigest()

    def abi_encode(self, signature, *args):
        return '0x' + hashlib.sha256(repr((signature, args)).encode()).hexdigest()


class FakeRpc:
    def __init__(self, cast, key='USDT0'):
        self.cast, self.key, self.spec = cast, key, asset_spec(key)
        self.origin = {'height': 17, 'hash': '0x' + '11' * 32}
        self.head = {'height': 20, 'hash': '0x' + '22' * 32}
        self.finalized = {'height': 18, 'hash': '0x' + '33' * 32}
        self.chain = CHAIN_ID
        self.genesis = GENESIS_HASH
        self.node = {'environment': {'chainId': LOCAL_CHAIN_ID}, 'forkConfig': {
            'forkUrl': 'https://public-read-only-fixture.invalid/rpc', 'forkBlockNumber': 17, 'forkChainId': CHAIN_ID}}
        self.implementation, self.admin = '0x' + '44' * 20, '0x' + '55' * 20
        self.admin_owner = '0x' + '66' * 20
        # Fix the fixture ABI independently of the registry under test. Deriving
        # these names from registry metadata would accept a nonexistent getter.
        abi_roles = ('owner()', 'oftContract()') if key == 'USDT0' else ('owner()', 'masterMinter()', 'pauser()', 'blacklister()')
        self.roles = {role: '0x' + ('%02x' % (100 + n)) * 20 for n, role in enumerate(abi_roles)}
        self.codes = {'token': '0x6001', 'implementation': '0x6002', 'admin': '0x6003'}
        self.decimals, self.paused, self.blocked, self.other_storage = 6, 0, {}, {}
        self.requests, self.attack = [], lambda calls, values: values

    def fork(self):
        self.chain = LOCAL_CHAIN_ID
        return self

    @staticmethod
    def block(point):
        return {'number': hex(point['height']), 'hash': point['hash']}

    def batch(self, calls):
        self.requests.append(copy.deepcopy(calls))
        values = []
        for method, params in calls:
            if method == 'eth_chainId': value = hex(self.chain)
            elif method == 'anvil_nodeInfo': value = copy.deepcopy(self.node)
            elif method == 'eth_getBlockByNumber':
                height = params[0]
                if height == '0x0': value = {'number': '0x0', 'hash': self.genesis}
                elif height == 'latest': value = self.block(self.head)
                elif height == 'finalized': value = self.block(self.finalized)
                else: value = self.block(self.origin if height == '0x11' else self.finalized if height == '0x12' else self.head)
            elif method == 'eth_getCode':
                target, block = params
                assert block in ('0x11', '0x12', '0x14')
                value = self.codes['token' if target == self.spec['address'] else
                                   'implementation' if target == self.implementation else 'admin']
            elif method == 'eth_getStorageAt':
                target, slot, block = params
                assert target == self.spec['address'] and block in ('0x11', '0x12', '0x14')
                selected = ((IMPLEMENTATION_SLOT, ADMIN_SLOT) if self.key == 'USDT0'
                            else (CIRCLE_IMPLEMENTATION_SLOT, CIRCLE_ADMIN_SLOT))
                value = self.other_storage.get(slot, word(self.implementation if slot == selected[0]
                    else self.admin if slot == selected[1] else 0))
            elif method == 'eth_call':
                query, block = params
                assert block in ('0x11', '0x12', '0x14')
                signature, args = self.cast.arguments[query['data']]
                if query['to'] == self.admin: value = word(self.admin_owner)
                elif signature == 'decimals()': value = word(self.decimals)
                elif signature == 'paused()': value = word(self.paused)
                elif signature in self.roles: value = word(self.roles[signature])
                elif signature == self.spec['blocklist_signature'] and len(args) == 1:
                    value = word(self.blocked.get(args[0], 0))
                else: raise AssertionError('token ABI has no getter: ' + signature)
            else: raise AssertionError('unexpected or write RPC method: ' + method)
            values.append(copy.deepcopy(value))
        return self.attack(calls, values)


class FakeFundingRpc(FakeRpc):
    def __init__(self, cast, key):
        super().__init__(cast, key)
        self.url = 'http://127.0.0.1:18545/'
        self.calls, self.impersonated, self.balances, self.receipts = [], set(), {}, {}
        self.fail_send, self.reverted = False, False

    def call(self, method, params=()):
        self.calls.append((method, copy.deepcopy(params)))
        if method == 'eth_getBalance': return hex(10_000 * 10**18)  # Larger than uint64 is valid ETH.
        if method == 'anvil_setBalance': return None
        if method == 'anvil_impersonateAccount': self.impersonated.add(params[0]); return None
        if method == 'anvil_stopImpersonatingAccount': self.impersonated.remove(params[0]); return None
        if method == 'eth_call':
            signature, args = self.cast.arguments[params[0]['data']]
            assert signature == 'balanceOf(address)'
            return word(self.balances.get(args[0], 0))
        if method == 'eth_sendTransaction':
            tx = params[0]
            assert self.chain == LOCAL_CHAIN_ID and tx['from'] in self.impersonated and tx['value'] == '0x0'
            if self.fail_send: raise CustodyError('local synthetic send fault')
            signature, args = self.cast.arguments[tx['data']]
            if signature == 'mint(address,uint256)' and not self.reverted:
                self.balances[args[0]] = self.balances.get(args[0], 0) + args[1]
            identity = '0x' + ('%064x' % (len(self.receipts) + 1))
            self.receipts[identity] = {'transactionHash': identity, 'status': hex(0 if self.reverted else 1),
                'blockNumber': '0x14', 'blockHash': self.head['hash']}
            return identity
        if method == 'eth_getTransactionReceipt': return self.receipts[params[0]]
        if method == 'eth_getBlockByNumber': return self.block(self.head)
        raise AssertionError('unexpected funding method: ' + method)


class FakeSponsoredRpc(FakeFundingRpc):
    def __init__(self, cast, key):
        super().__init__(cast, key)
        self.vault = '0x' + 'cc' * 20
        self.instance, self.asset = '0x' + 'ee' * 32, '0x' + 'ff' * 32
        self.builder_attack = None
        self.source_code, self.builder_reads = '0x', []

    @staticmethod
    def block(point):
        return {**FakeRpc.block(point), 'timestamp': hex(1000)}

    def call(self, method, params=()):
        self.builder_reads.append((method, copy.deepcopy(params)))
        if method == 'eth_getCode':
            self.calls.append((method, copy.deepcopy(params)))
            assert params[1] == '0x14'
            return self.source_code
        if method == 'eth_call':
            signature, args = self.cast.arguments[params[0]['data']]
            if signature == 'TOKEN()': return word(self.spec['address'])
            if signature == 'DEPLOYMENT_CHAIN_ID()': return word(LOCAL_CHAIN_ID)
            if signature == 'INSTANCE_ID()': return self.instance
            if signature == 'Q_ASSET_ID()': return self.asset
            if signature == 'depositAuthorizationNonce(address,uint256,bytes32,bytes32)':
                sender, amount, account, salt = args
                value = self.cast.keccak(self.cast.abi_encode('x(bytes32,uint256,address,bytes32,bytes32,address,uint256,bytes32,bytes32)',
                    self.cast.keccak('DepositAuthorization(uint256 chainId,address vault,bytes32 instance,bytes32 asset,address sender,uint256 amount,bytes32 account,bytes32 userSalt)'),
                    LOCAL_CHAIN_ID, self.vault, self.instance, self.asset, sender, amount, account, salt))
                return word(1) if self.builder_attack == 'nonce' else value
            name = 'USD\u20ae0' if self.key == 'USDT0' else 'USD Coin'
            if signature == 'name()':
                encoded = name.encode()
                return word(32) + word(len(encoded))[2:] + encoded.hex().ljust(((len(encoded) + 31) // 32) * 64, '0')
            if signature == 'DOMAIN_SEPARATOR()':
                value = self.cast.keccak(self.cast.abi_encode('x(bytes32,bytes32,bytes32,uint256,address)',
                    self.cast.keccak('EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)'),
                    self.cast.keccak('0x' + name.encode().hex()), self.cast.keccak(self.spec['authorization_domain_version']),
                    LOCAL_CHAIN_ID, self.spec['address']))
                return word(1) if self.builder_attack == 'domain' else value
            if signature == 'RECEIVE_WITH_AUTHORIZATION_TYPEHASH()':
                return word(1) if self.builder_attack == 'type' else self.cast.keccak('ReceiveWithAuthorization(address from,address to,uint256 value,uint256 validAfter,uint256 validBefore,bytes32 nonce)')
            if signature == 'authorizationState(address,bytes32)': return word(int(self.builder_attack == 'consumed'))
        return super().call(method, params)


class ArbitrumAssetsTests(unittest.TestCase):
    def setUp(self):
        self.cast = FakeCast()
        self.rpc = FakeRpc(self.cast)

    def pin(self, key='USDT0'):
        return capture_asset_pin(self.rpc, self.cast, key, self.rpc.origin)

    def test_registry_is_exact_and_returns_independent_metadata(self):
        self.assertEqual(asset_spec('USDT0')['address'], '0xfd086bc7cd5c481dcc9c85ebe478a1c0b69fcbb9')
        self.assertEqual(asset_spec('USDC')['address'], '0xaf88d065e77c8cc2239327c5edb3a432268e5831')
        first = asset_spec('USDT0'); first['issuer_role_signatures'].append('other()')
        self.assertNotIn('other()', asset_spec('USDT0')['issuer_role_signatures'])
        for key in ('USDT', 'USDC.e', 'usdt0', None, True, {'asset': 'USDT0'}):
            with self.subTest(key=key), self.assertRaises(CustodyError): asset_spec(key)

    def test_usdt0_pins_public_oft_getter_and_refuses_legacy_internal_getter_pin(self):
        self.assertEqual(asset_spec('USDT0')['issuer_role_signatures'], ['owner()', 'oftContract()'])
        pin = self.pin()
        self.assertEqual(set(pin['identity']['issuer_roles']), {'owner()', 'oftContract()'})
        token = self.rpc.spec['address']
        self.assertIn(('eth_call', [{'to': token, 'data': self.cast.calldata('oftContract()')}, '0x11']), self.rpc.requests[1])
        with self.assertRaisesRegex(AssertionError, 'token ABI has no getter: l2Gateway'):
            self.rpc.batch([('eth_call', [{'to': token, 'data': self.cast.calldata('l2Gateway()')}, '0x11'])])
        stale = copy.deepcopy(pin)
        stale['identity']['issuer_roles']['l2Gateway()'] = stale['identity']['issuer_roles'].pop('oftContract()')
        with self.assertRaisesRegex(CustodyError, 'proxy/issuer-role schema'):
            validate_asset_pin_shape(stale)

    def test_both_tokens_capture_complete_pins_at_one_explicit_finalized_checkpoint(self):
        for key in ('USDT0', 'USDC'):
            self.rpc = FakeRpc(self.cast, key)
            pin = self.pin(key)
            self.assertEqual(validate_asset_pin_shape(pin), pin)
            self.assertEqual(pin['identity']['token'], asset_spec(key)['address'])
            self.assertEqual(pin['identity']['decimals'], 6)
            self.assertIs(pin['mainnet_enabled'], False)
            self.assertEqual(len(self.rpc.requests), 4)
            for batch in self.rpc.requests[1:3]:
                self.assertTrue(all(params[-1] == '0x11' for _, params in batch))
            self.rpc.fork()
            snapshot = verify_asset_pin(self.rpc, self.cast, pin, accounts=['0x' + 'ab' * 20])
            self.assertEqual(snapshot['block'], self.rpc.head)
            self.assertEqual(snapshot['identity'], pin['identity'])
            self.assertEqual(snapshot['status']['blocked'], {'0x' + 'ab' * 20: False})

    def test_wrong_chain_genesis_origin_finality_or_fork_context_refuses(self):
        for attack in ('chain', 'genesis', 'origin', 'unfinalized', 'fork_chain', 'fork_height', 'unforked', 'fork_environment'):
            self.rpc = FakeRpc(self.cast)
            origin = copy.deepcopy(self.rpc.origin)
            execution = CHAIN_ID
            if attack == 'chain': self.rpc.chain = LOCAL_CHAIN_ID
            elif attack == 'genesis': self.rpc.genesis = '0x' + '00' * 32
            elif attack == 'origin': origin['hash'] = '0x' + '99' * 32
            elif attack == 'unfinalized': self.rpc.finalized['height'] = 16
            else:
                self.rpc.fork(); execution = LOCAL_CHAIN_ID
                if attack == 'fork_chain': self.rpc.node['forkConfig']['forkChainId'] = 1
                elif attack == 'fork_height': self.rpc.node['forkConfig']['forkBlockNumber'] = 16
                elif attack == 'unforked': self.rpc.node['forkConfig']['forkUrl'] = None
                elif attack == 'fork_environment': self.rpc.node['environment']['chainId'] = 42161
            with self.subTest(attack=attack), self.assertRaises(CustodyError):
                capture_asset_pin(self.rpc, self.cast, 'USDT0', origin, execution_chain_id=execution)

    def test_remote_verification_uses_fresh_finalized_then_explicit_state_reads(self):
        pin = self.pin()
        snapshot = verify_asset_pin(self.rpc, self.cast, pin, execution_chain_id=CHAIN_ID)
        self.assertEqual(snapshot['block'], self.rpc.finalized)
        self.assertEqual(self.rpc.requests[-4][3], ('eth_getBlockByNumber', ['finalized', False]))
        for batch in self.rpc.requests[-3:-1]:
            self.assertTrue(all(params[-1] == '0x12' for _, params in batch))
        self.assertFalse(any('latest' in params for batch in self.rpc.requests for _, params in batch))

    def test_same_proxy_code_implementation_admin_and_issuer_changes_fence_without_repinning(self):
        for attack in ('implementation', 'implementation_code', 'admin', 'admin_code', 'admin_owner', 'owner()', 'oftContract()'):
            self.rpc = FakeRpc(self.cast)
            pin = self.pin(); retained = copy.deepcopy(pin)
            self.rpc.fork()
            if attack == 'implementation': self.rpc.implementation = '0x' + '77' * 20
            elif attack == 'implementation_code': self.rpc.codes['implementation'] = '0x6004'
            elif attack == 'admin': self.rpc.admin = '0x' + '77' * 20
            elif attack == 'admin_code': self.rpc.codes['admin'] = '0x6004'
            elif attack == 'admin_owner': self.rpc.admin_owner = '0x' + '77' * 20
            else: self.rpc.roles[attack] = '0x' + '77' * 20
            with self.subTest(attack=attack), self.assertRaisesRegex(CustodyError, 'identity changed'):
                verify_asset_pin(self.rpc, self.cast, pin)
            self.assertEqual(pin, retained)
            self.assertEqual(self.rpc.codes['token'], '0x6001')

    def test_proxy_conflicts_beacon_zero_roles_and_wrong_decimals_refuse(self):
        for attack in ('beacon', 'other_impl', 'other_admin', 'zero_impl', 'zero_owner', 'zero_admin_owner', 'decimals', 'empty_admin'):
            self.rpc = FakeRpc(self.cast)
            if attack == 'beacon': self.rpc.other_storage[BEACON_SLOT] = word('0x' + '77' * 20)
            elif attack == 'other_impl': self.rpc.other_storage[CIRCLE_IMPLEMENTATION_SLOT] = word('0x' + '77' * 20)
            elif attack == 'other_admin': self.rpc.other_storage[CIRCLE_ADMIN_SLOT] = word('0x' + '77' * 20)
            elif attack == 'zero_impl': self.rpc.implementation = '0x' + '00' * 20
            elif attack == 'zero_owner': self.rpc.roles['owner()'] = '0x' + '00' * 20
            elif attack == 'zero_admin_owner': self.rpc.admin_owner = '0x' + '00' * 20
            elif attack == 'decimals': self.rpc.decimals = 18
            elif attack == 'empty_admin': self.rpc.codes['admin'] = '0x'
            with self.subTest(attack=attack), self.assertRaises(CustodyError): self.pin()

    def test_compliance_pause_blocklist_and_noncanonical_bool_fail_closed(self):
        account = '0x' + 'ab' * 20
        for key, attack in (('USDC', 'paused'), ('USDC', 'blocked'), ('USDT0', 'blocked'), ('USDT0', 'bad_bool')):
            self.rpc = FakeRpc(self.cast, key)
            pin = self.pin(key); self.rpc.fork()
            if attack == 'paused': self.rpc.paused = 1
            else: self.rpc.blocked[account] = 2 if attack == 'bad_bool' else 1
            with self.subTest(key=key, attack=attack), self.assertRaises(CustodyError):
                verify_asset_pin(self.rpc, self.cast, pin, accounts=[account])

    def test_canonical_origin_head_and_chain_are_rechecked_after_all_reads(self):
        pin = self.pin()
        for attack in ('origin', 'head', 'chain', 'partial'):
            self.rpc = FakeRpc(self.cast).fork()
            def changed(calls, values, attack=attack):
                if len(self.rpc.requests) == 4:
                    if attack == 'origin': values[1]['hash'] = '0x' + '99' * 32
                    elif attack == 'head': values[2]['hash'] = '0x' + '99' * 32
                    elif attack == 'chain': values[0] = hex(1)
                    elif attack == 'partial': values.pop()
                return values
            self.rpc.attack = changed
            with self.subTest(attack=attack), self.assertRaises(CustodyError): verify_asset_pin(self.rpc, self.cast, pin)

    def test_pin_schema_cannot_select_other_tokens_proxy_slots_or_enable_mainnet(self):
        original = self.pin()
        for attack in ('extra', 'mainnet', 'chain', 'asset', 'token', 'decimals', 'slot', 'role', 'code_hash', 'origin'):
            pin = copy.deepcopy(original)
            if attack == 'extra': pin['unknown'] = True
            elif attack == 'mainnet': pin['mainnet_enabled'] = True
            elif attack == 'chain': pin['source_chain_id'] = '42161'
            elif attack == 'asset': pin['asset'] = 'USDT'
            elif attack == 'token': pin['identity']['token'] = asset_spec('USDC')['address']
            elif attack == 'decimals': pin['identity']['decimals'] = True
            elif attack == 'slot': pin['identity']['proxy']['implementation_slot'] = BEACON_SLOT
            elif attack == 'role': pin['identity']['issuer_roles']['fake()'] = '0x' + '77' * 20
            elif attack == 'code_hash': pin['identity']['token_code_hash'] = '0x' + '00' * 32
            elif attack == 'origin': pin['origin']['height'] = '17'
            with self.subTest(attack=attack), self.assertRaises(CustodyError): validate_asset_pin_shape(pin)

    def test_compliance_and_checkpoint_bounds_reject_ambiguous_inputs(self):
        pin = self.pin(); self.rpc.fork()
        for accounts in (['0x' + '01' * 20] * 9, ['0x' + '01' * 20] * 2, ['0x' + '00' * 20], {}):
            with self.subTest(accounts=accounts), self.assertRaises(CustodyError):
                verify_asset_pin(self.rpc, self.cast, pin, accounts=accounts)
        for context in (1, True, '31337'):
            with self.subTest(context=context), self.assertRaises(CustodyError):
                verify_asset_pin(self.rpc, self.cast, pin, execution_chain_id=context)

    def test_artificial_local_funding_uses_real_authorized_calls_not_token_storage_writes(self):
        recipient = '0x' + 'ab' * 20
        for key in ('USDT0', 'USDC'):
            rpc = FakeFundingRpc(self.cast, key)
            pin = capture_asset_pin(rpc, self.cast, key, rpc.origin)
            rpc.fork()
            result = fund_local_fork_account(rpc, self.cast, pin, recipient, 1_000_000)
            self.assertEqual(result['funding'], 'artificial-authorized-role-mint')
            self.assertEqual(rpc.balances[recipient], 1_000_000)
            self.assertFalse(rpc.impersonated)
            self.assertEqual(len(result['transactions']), 1 if key == 'USDT0' else 2)
            sends = [params[0] for method, params in rpc.calls if method == 'eth_sendTransaction']
            self.assertEqual(sends[0]['from'], pin['identity']['issuer_roles']['oftContract()' if key == 'USDT0' else 'masterMinter()'])
            self.assertFalse(any(method in ('anvil_setCode', 'anvil_setStorageAt') for method, _ in rpc.calls))

    def test_artificial_funding_always_stops_impersonation_after_send_or_receipt_failure(self):
        for failure in ('send', 'receipt'):
            rpc = FakeFundingRpc(self.cast, 'USDT0')
            pin = capture_asset_pin(rpc, self.cast, 'USDT0', rpc.origin); rpc.fork()
            rpc.fail_send, rpc.reverted = failure == 'send', failure == 'receipt'
            with self.subTest(failure=failure), self.assertRaises(CustodyError):
                fund_local_fork_account(rpc, self.cast, pin, '0x' + 'ab' * 20, 1_000_000)
            self.assertFalse(rpc.impersonated)
            self.assertEqual(rpc.balances, {})

    def test_artificial_funding_rejects_live_url_or_live_chain_before_any_dev_call(self):
        for attack in ('url', 'chain'):
            rpc = FakeFundingRpc(self.cast, 'USDT0')
            pin = capture_asset_pin(rpc, self.cast, 'USDT0', rpc.origin)
            if attack == 'url': rpc.url = 'https://arb1.arbitrum.io/rpc'; rpc.fork()
            with self.subTest(attack=attack), self.assertRaises(CustodyError):
                fund_local_fork_account(rpc, self.cast, pin, '0x' + 'ab' * 20, 1_000_000)
            self.assertEqual(rpc.calls, [])

    def test_issuer_compliance_deferral_never_masks_proxy_or_source_identity_failure(self):
        rpc = FakeRpc(self.cast, 'USDC')
        pin = capture_asset_pin(rpc, self.cast, 'USDC', rpc.origin); rpc.fork(); rpc.paused = 1
        with self.assertRaises(AssetComplianceBlocked) as caught: verify_asset_pin(rpc, self.cast, pin)
        self.assertEqual(caught.exception.reason, 'issuer-paused')
        self.assertTrue(verify_asset_pin(rpc, self.cast, pin, check_compliance=False)['status']['paused'])
        rpc.codes['implementation'] = '0x6005'
        with self.assertRaises(CustodyError) as caught: verify_asset_pin(rpc, self.cast, pin)
        self.assertNotIsInstance(caught.exception, AssetComplianceBlocked)
        self.assertIn('identity changed', str(caught.exception))

    def test_unsigned_sponsored_payload_binds_account_salt_domain_and_actual_vault_nonce(self):
        for key in ('USDT0', 'USDC'):
            rpc = FakeSponsoredRpc(self.cast, key)
            pin = capture_asset_pin(rpc, self.cast, key, rpc.origin); rpc.fork()
            args = [rpc, self.cast, pin, rpc.vault, '0x' + 'ab' * 20, 1_000_000, '0x' + '12' * 32, '0x' + '13' * 32]
            first = build_sponsored_deposit(*args)
            self.assertEqual((first['valid_after'], first['valid_before']), (999, 1300))
            args[-2] = '0x' + '14' * 32
            redirected = build_sponsored_deposit(*args)
            self.assertNotEqual(first['nonce'], redirected['nonce'])
            self.assertNotEqual(first['digest'], redirected['digest'])
            self.assertTrue(all(method in ('eth_getCode', 'eth_getBlockByNumber') for method, _ in rpc.calls))
            self.assertIn(('eth_getCode', [args[4], '0x14']), rpc.builder_reads)

    def test_unsigned_sponsored_payload_rejects_delegated_and_contract_wallets_before_nonce_or_digest(self):
        # Literal observed EIP7702 designator, independent of registry/helper
        # metadata. Possession of the original EOA key is insufficient when
        # token SignatureChecker takes the ERC1271 branch for nonempty code.
        delegated = '0xef01008a67b5020ee254ef48e3b6a04927f39baf7e408a'
        for key in ('USDT0', 'USDC'):
            for code in (delegated, '0x60006000', '0x00'):
                rpc = FakeSponsoredRpc(self.cast, key)
                pin = capture_asset_pin(rpc, self.cast, key, rpc.origin); rpc.fork()
                sender = '0x' + 'ab' * 20
                rpc.source_code = code
                with self.subTest(asset=key, code=code), self.assertRaisesRegex(CustodyError, 'code-empty EOA'):
                    build_sponsored_deposit(rpc, self.cast, pin, rpc.vault, sender, 1_000_000,
                                            '0x' + '12' * 32, '0x' + '13' * 32)
                self.assertEqual(rpc.builder_reads, [('eth_getCode', [sender, '0x14'])])
                self.assertFalse(any(method.startswith('anvil_') or method == 'eth_sendTransaction'
                                     for method, _ in rpc.calls))

    def test_unsigned_sponsored_payload_refuses_malformed_source_code_without_authorization(self):
        for code in (None, '0x0', 'not-code'):
            rpc = FakeSponsoredRpc(self.cast, 'USDT0')
            pin = capture_asset_pin(rpc, self.cast, 'USDT0', rpc.origin); rpc.fork()
            rpc.source_code = code
            with self.subTest(code=code), self.assertRaises(CustodyError):
                build_sponsored_deposit(rpc, self.cast, pin, rpc.vault, '0x' + 'ab' * 20, 1_000_000,
                                        '0x' + '12' * 32, '0x' + '13' * 32)
            self.assertEqual(len(rpc.builder_reads), 1)

    def test_unsigned_sponsored_payload_rejects_nonce_domain_type_replay_and_bad_time(self):
        rpc = FakeSponsoredRpc(self.cast, 'USDT0')
        pin = capture_asset_pin(rpc, self.cast, 'USDT0', rpc.origin); rpc.fork()
        args = [rpc, self.cast, pin, rpc.vault, '0x' + 'ab' * 20, 1_000_000, '0x' + '12' * 32, '0x' + '13' * 32]
        for attack in ('nonce', 'domain', 'type', 'consumed'):
            rpc.builder_attack = attack
            with self.subTest(attack=attack), self.assertRaises(CustodyError): build_sponsored_deposit(*args)
        rpc.builder_attack = None
        for window in ({'valid_after': 1001}, {'valid_before': 999}, {'valid_before': 4601}):
            with self.subTest(window=window), self.assertRaises(CustodyError): build_sponsored_deposit(*args, **window)


if __name__ == '__main__':
    unittest.main()
