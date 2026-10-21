#!/usr/bin/env python3
"""Explicit local31337 Arbitrum state rehearsal, never a live source adapter.

All requests, including development-only funding, go to a literal loopback
Anvil. The fork origin, genesis and private upstream URL commitment stay pinned
on every source/signing boundary. Existing LocalRpc keeps rejecting all forks.
The native profile, internal synthetic Q and custody/EIP155 chain31337 remain
unchanged; a successful rehearsal cannot authorize mainnet operations.
"""
import copy
import re

from custody import CHAIN_ID, MAX_ATOMS, Q_ASSET, CustodyError, LocalRpc, address, hex_value, uint, words
from arbitrum_assets import GENESIS_HASH, validate_asset_pin_shape, verify_asset_pin
from arbitrum_rpc import HttpsReadOnlyRpc

FORMAT = 'fm2-regtest-arbitrum-fork/1'
FIELDS = {'format', 'mainnet_enabled', 'origin', 'provider_binding', 'asset_pin'}
BINDING_FIELDS = {'operator_id', 'endpoint_id', 'endpoint_origin', 'endpoint_commitment'}


def validate_fork_context(value):
    if (not isinstance(value, dict) or set(value) != FIELDS or value['format'] != FORMAT or
            value['mainnet_enabled'] is not False):
        raise CustodyError('local Arbitrum fork requires its exact disabled rehearsal context')
    pin = validate_asset_pin_shape(value['asset_pin'])
    if value['origin'] != pin['origin']:
        raise CustodyError('local fork origin differs from the exact original real-token pin')
    origin = value['origin']
    if (not isinstance(origin, dict) or set(origin) != {'height', 'hash'} or
            type(origin['height']) is not int or origin['height'] <= 0 or
            hex_value(origin['hash'], 32) != origin['hash']):
        raise CustodyError('local fork requires an exact canonical origin height/hash')
    binding = value['provider_binding']
    if not isinstance(binding, dict) or set(binding) != BINDING_FIELDS:
        raise CustodyError('local fork requires one exact public provider identity binding')
    for field in ('operator_id', 'endpoint_id'):
        if not isinstance(binding[field], str) or not re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9._-]{0,127}', binding[field]):
            raise CustodyError('local fork provider labels must be bounded public identifiers')
    if (not isinstance(binding['endpoint_origin'], str) or
            not re.fullmatch(r'https://[a-z0-9.-]+:443', binding['endpoint_origin']) or
            not isinstance(binding['endpoint_commitment'], str) or
            not re.fullmatch(r'[0-9a-f]{64}', binding['endpoint_commitment'])):
        raise CustodyError('local fork provider origin/commitment is not canonical')
    return copy.deepcopy(value)


def _height(value):
    if type(value) is int:
        return uint(value, 64)
    if not isinstance(value, str) or not re.fullmatch(r'0x(?:0|[1-9a-f][0-9a-f]*)', value):
        raise CustodyError('Anvil fork height is not a canonical unsigned quantity')
    return uint(int(value, 16), 64)


class ForkRpc(LocalRpc):
    FUNDING_METHODS = frozenset(('anvil_setBalance', 'anvil_impersonateAccount', 'anvil_stopImpersonatingAccount'))
    ALLOWED = LocalRpc.ALLOWED | FUNDING_METHODS
    MUTATIONS = frozenset(('eth_sendTransaction', 'eth_sendRawTransaction', 'evm_increaseTime',
                          'evm_mine', 'evm_snapshot', 'evm_revert')) | FUNDING_METHODS

    def __init__(self, url, context, timeout=10):
        self._fork_context = validate_fork_context(context)
        super().__init__(url, timeout=timeout)

    @property
    def fork_context(self):
        return copy.deepcopy(self._fork_context)

    def _fixture_identity(self, chain, info):
        if chain != hex(CHAIN_ID) or (not isinstance(info, dict) or not isinstance(info.get('environment'), dict) or
                type(info['environment'].get('chainId')) is not int or info['environment']['chainId'] != CHAIN_ID):
            raise CustodyError('local Arbitrum rehearsal permits only explicit Anvil chain31337')
        fork = info.get('forkConfig')
        if not isinstance(fork, dict) or _height(fork.get('forkBlockNumber')) != self._fork_context['origin']['height']:
            raise CustodyError('local Arbitrum fork height changed or fork metadata is missing')
        if 'forkChainId' in fork and _height(fork['forkChainId']) != 42161:
            raise CustodyError('local fork source is not the pinned Arbitrum chain42161')
        binding = self._fork_context['provider_binding']
        # Construction performs bounded HTTPS/public-host syntax validation
        # only. This object is never used to contact or write to the upstream.
        provider = HttpsReadOnlyRpc(fork.get('forkUrl'), binding['endpoint_id'], binding['operator_id'],
            binding['endpoint_id'], chain_id=42161, genesis_hash=GENESIS_HASH)
        if provider.binding != binding:
            raise CustodyError('local fork upstream differs from its pinned public provider commitment')
        genesis, origin = super().batch([('eth_getBlockByNumber', ['0x0', False]),
            ('eth_getBlockByNumber', [hex(self._fork_context['origin']['height']), False])])
        if self._block_point(genesis) != {'height': 0, 'hash': GENESIS_HASH} or self._block_point(origin) != self._fork_context['origin']:
            raise CustodyError('local fork genesis or original Arbitrum block was removed/replaced')
        return info

    def state_batch(self, calls, *, block_heights=()):
        result = super().state_batch(calls, block_heights=block_heights)
        # The inherited batch rechecks the chosen head. Retain an independent
        # final fork/origin check even for state-only batches without heights.
        self.assert_fixture()
        return result

    def call(self, method, params=()):
        if method in self.MUTATIONS:
            self.assert_fixture()
            if method == 'eth_sendTransaction':
                if (not isinstance(params, (list, tuple)) or len(params) != 1 or not isinstance(params[0], dict) or
                        params[0].get('chainId') != hex(CHAIN_ID)):
                    raise CustodyError('local fork transaction must explicitly bind EIP155 chain31337')
            elif method in self.FUNDING_METHODS:
                expected = 2 if method == 'anvil_setBalance' else 1
                if not isinstance(params, (list, tuple)) or len(params) != expected or address(params[0]) != params[0]:
                    raise CustodyError('local fork funding method requires an exact canonical account')
                if expected == 2 and (not isinstance(params[1], str) or
                        not re.fullmatch(r'0x(?:0|[1-9a-f][0-9a-f]*)', params[1]) or int(params[1], 16) > 10 ** 22):
                    raise CustodyError('local fork gas funding exceeds its explicit test-only bound')
        return super().call(method, params)


def verify_fork_driver(driver, *, accounts=(), check_compliance=False):
    """Fresh, fixed adapter guard; no caller-supplied guard or RPC injection."""
    configured = driver.config.get('regtest_arbitrum_fork')
    if configured is None:
        if isinstance(driver.rpc, ForkRpc):
            raise CustodyError('local fork driver lost its original explicit rehearsal context')
        return None
    context = validate_fork_context(configured)
    if not isinstance(driver.rpc, ForkRpc) or driver.rpc.fork_context != context:
        raise CustodyError('local fork driver differs from its original pinned RPC context')
    driver.rpc.assert_fixture()
    real_token = context['asset_pin']['identity']['token']
    if driver.asset == Q_ASSET:
        if driver.token != real_token:
            raise CustodyError('local fork Q route must use the exact pinned real-token contract')
        if not isinstance(accounts, (list, tuple)) or len(accounts) > 7:
            raise CustodyError('local fork compliance account set exceeds its bound')
        accounts = list(dict.fromkeys(([driver.vault] if driver.vault is not None else []) +
                                     [address(item) for item in accounts]))
        if driver.vault is not None:
            # Token issuers may confiscate reserves without changing code or
            # roles. A soft pause must never conceal an actual backing deficit.
            calls = [('eth_call', [{'to': real_token, 'data': driver.cast.calldata('balanceOf(address)', driver.vault)}]),
                     ('eth_call', [{'to': driver.vault, 'data': driver.cast.calldata('locked()')}]),
                     ('eth_call', [{'to': driver.vault, 'data': driver.cast.calldata('pendingTotal()')}])]
            backing = driver.rpc.state_batch(calls)
            if not isinstance(backing, dict) or len(backing.get('results', [])) != 3:
                raise CustodyError('local fork custody backing snapshot omitted exact reserve reads')
            balance, locked, pending = [int.from_bytes(words(value, 1)[0], 'big') for value in backing['results']]
            if not pending <= locked <= MAX_ATOMS or balance < locked:
                raise CustodyError('local fork real-token custody reserve deficit or pending/locked accounting mismatch')
        return verify_asset_pin(driver.rpc, driver.cast, context['asset_pin'],
                                execution_chain_id=31337, accounts=accounts, check_compliance=check_compliance)
    if driver.token == real_token:
        raise CustodyError('real fork quote token cannot be substituted for a synthetic base route')
    return None
