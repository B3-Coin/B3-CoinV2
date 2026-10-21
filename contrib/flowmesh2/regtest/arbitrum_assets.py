#!/usr/bin/env python3
"""Read-only real-token pins and isolated-fork rehearsal; never a release gate.

An RPC finalized tag is a provider assertion, not an Arbitrum/L1 light-client
proof. Capture the same checkpoint from independent providers before a local
fork. These pins never authorize mainnet writes or change the regtest runtime.
"""
import copy
import time

from custody import CustodyError, MAX_ATOMS, address, hex_value, local_rpc_url, uint, word_address, words

CHAIN_ID = 42161
LOCAL_CHAIN_ID = 31337
GENESIS_HASH = '0x7ee576b35482195fc49205cec9af72ce14f003b9ae69f6ba0faef4514be8b442'
PIN_FORMAT = 'fm2-arbitrum-asset-pin/1'
IMPLEMENTATION_SLOT = '0x360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc'
ADMIN_SLOT = '0xb53127684a568b3173ae13b9f8a6016e243e63b6e8ee1178d6a717850b5d6103'
BEACON_SLOT = '0xa3f0ad74e5423aebfd80d3ef4346578335a9a72aeaee59ff6cb3582b35133d50'
CIRCLE_IMPLEMENTATION_SLOT = '0x7050c9e0f4ca769c69bd3a8ef740bc37934f8e2c036e5a723fd8ee048ed3f8c3'
CIRCLE_ADMIN_SLOT = '0x10d6a54a4754c8869d6886b5f5d7fbfa5b4522237ea5c60d11bc4e7a1ff9390b'
ZERO_ADDRESS = '0x' + '00' * 20
_ASSETS = {
    'USDT0': {'key': 'USDT0', 'chain_id': CHAIN_ID,
        'address': '0xfd086bc7cd5c481dcc9c85ebe478a1c0b69fcbb9', 'decimals': 6,
        'proxy_kind': 'eip1967', 'blocklist_signature': 'isBlocked(address)',
        'authorization_domain_version': '1',
        # ArbitrumExtensionV2 exposes the internal l2Gateway role through this
        # public getter. There is no external l2Gateway() ABI on the token.
        'issuer_role_signatures': ['owner()', 'oftContract()'],
        'sources': ['https://docs.usdt0.to/technical-documentation/deployments',
                    'https://docs.usdt0.to/technical-documentation/token-features/additional-behavior']},
    'USDC': {'key': 'USDC', 'chain_id': CHAIN_ID,
        'address': '0xaf88d065e77c8cc2239327c5edb3a432268e5831', 'decimals': 6,
        'proxy_kind': 'circle-zeppelin', 'blocklist_signature': 'isBlacklisted(address)',
        'authorization_domain_version': '2',
        'issuer_role_signatures': ['owner()', 'masterMinter()', 'pauser()', 'blacklister()'],
        'sources': ['https://developers.circle.com/stablecoins/usdc-contract-addresses',
                    'https://github.com/circlefin/stablecoin-evm']},
}
_PIN_FIELDS = {'format', 'asset', 'source_chain_id', 'origin', 'genesis_hash', 'identity', 'mainnet_enabled'}


class AssetComplianceBlocked(CustodyError):
    """An actual issuer pause/blocklist defers transfers, never identity checks."""
    def __init__(self, reason):
        if reason not in ('issuer-paused', 'issuer-blocked'):
            raise CustodyError('unknown issuer compliance refusal')
        self.reason = reason
        super().__init__('issuer pause/blocklist currently prevents this custody operation: ' + reason)


def asset_spec(key):
    """Select an exact asset; USDT aliases, USDC.e and arbitrary tokens refuse."""
    if not isinstance(key, str) or key not in _ASSETS:
        raise CustodyError('select exactly USDT0 or native USDC from the Arbitrum registry')
    return copy.deepcopy(_ASSETS[key])


def _point(value):
    if not isinstance(value, dict) or set(value) != {'height', 'hash'} or type(value['height']) is not int:
        raise CustodyError('asset pin requires an exact explicit checkpoint height/hash')
    point = {'height': uint(value['height'], 64), 'hash': hex_value(value['hash'], 32)}
    if point['height'] == 0 or int(point['hash'], 16) == 0 or point != value:
        raise CustodyError('asset pin checkpoint must be canonical and nonzero')
    return point


def _quantity(value, bits=64):
    if type(value) is int:
        return uint(value, bits)
    if not isinstance(value, str):
        raise CustodyError('asset RPC quantity is not a canonical integer')
    try:
        parsed = uint(int(value, 16), bits)
    except (ValueError, TypeError) as error:
        raise CustodyError('asset RPC quantity is malformed') from error
    if value != hex(parsed):
        raise CustodyError('asset RPC quantity is not canonical hexadecimal')
    return parsed


def _block(value):
    if not isinstance(value, dict) or 'number' not in value or 'hash' not in value:
        raise CustodyError('asset checkpoint is missing from its canonical source')
    return _point({'height': _quantity(value['number']), 'hash': value['hash']})


def _execution(chain, node, execution_chain_id, origin):
    if type(execution_chain_id) is not int or execution_chain_id not in (CHAIN_ID, LOCAL_CHAIN_ID):
        raise CustodyError('asset rehearsal permits only Arbitrum reads or an isolated local chain31337 fork')
    if _quantity(chain) != execution_chain_id:
        raise CustodyError('asset RPC chain differs from its explicit execution context')
    if execution_chain_id == LOCAL_CHAIN_ID:
        if (not isinstance(node, dict) or not isinstance(node.get('environment'), dict) or
                node['environment'].get('chainId') != LOCAL_CHAIN_ID or not isinstance(node.get('forkConfig'), dict)):
            raise CustodyError('asset rehearsal requires explicit Anvil chain31337 fork metadata')
        fork = node['forkConfig']
        if (not isinstance(fork.get('forkUrl'), str) or not fork['forkUrl'] or
                _quantity(fork.get('forkBlockNumber')) != origin['height'] or
                ('forkChainId' in fork and _quantity(fork['forkChainId']) != CHAIN_ID)):
            raise CustodyError('asset rehearsal fork differs from its exact finalized Arbitrum origin')


def _word(encoded):
    return words(encoded, 1)[0]


def _bool(encoded):
    value = int.from_bytes(_word(encoded), 'big')
    if value not in (0, 1):
        raise CustodyError('issuer compliance read returned a noncanonical ABI boolean')
    return bool(value)


def _code_hash(cast, encoded, *, allow_empty=False):
    code = hex_value(encoded)
    if (code == '0x' and not allow_empty) or len(code) > 196610:
        raise CustodyError('asset identity code is absent or exceeds its reviewed bound')
    return hex_value(cast.keccak(code), 32)


def _read_batch(rpc, calls):
    if not 1 <= len(calls) <= 32:
        raise CustodyError('asset read batch exceeds its bound')
    result = rpc.batch(calls)
    if not isinstance(result, list) or len(result) != len(calls):
        raise CustodyError('asset read batch omitted required results')
    return result


def _snapshot(rpc, cast, key, origin, execution_chain_id, *, latest=False, accounts=()):
    spec, origin = asset_spec(key), _point(origin)
    if not isinstance(accounts, (list, tuple)) or len(accounts) > 8:
        raise CustodyError('issuer compliance account set exceeds its bound')
    checked = [address(item) for item in accounts]
    if len(set(checked)) != len(checked) or ZERO_ADDRESS in checked:
        raise CustodyError('issuer compliance accounts must be distinct and nonzero')
    local = execution_chain_id == LOCAL_CHAIN_ID
    context = [('eth_chainId', []), ('eth_getBlockByNumber', ['0x0', False]),
               ('eth_getBlockByNumber', [hex(origin['height']), False]),
               ('eth_getBlockByNumber', [('latest' if local else 'finalized') if latest else hex(origin['height']), False])]
    context.append(('anvil_nodeInfo', []) if local else ('eth_getBlockByNumber', ['finalized', False]))
    chain, genesis, original, selected, extra = _read_batch(rpc, context)
    _execution(chain, extra if local else None, execution_chain_id, origin)
    if (not isinstance(genesis, dict) or genesis.get('number') != '0x0' or
            hex_value(genesis.get('hash'), 32) != GENESIS_HASH or _block(original) != origin):
        raise CustodyError('asset source genesis/origin differs from pinned Arbitrum')
    selected = _block(selected)
    if selected['height'] < origin['height'] or (not local and _block(extra)['height'] < origin['height']):
        raise CustodyError('asset origin is beyond the provider asserted finalized checkpoint')
    block = hex(selected['height'])
    circle_impl, circle_admin = CIRCLE_IMPLEMENTATION_SLOT, CIRCLE_ADMIN_SLOT
    slots = [IMPLEMENTATION_SLOT, ADMIN_SLOT, BEACON_SLOT, circle_impl, circle_admin]
    token = spec['address']
    roles = spec['issuer_role_signatures']
    calls = [('eth_getCode', [token, block])]
    calls.extend(('eth_getStorageAt', [token, slot, block]) for slot in slots)
    signatures = [('decimals()', ())] + [(role, ()) for role in roles]
    if key == 'USDC':
        signatures.append(('paused()', ()))
    signatures.extend((spec['blocklist_signature'], (account,)) for account in checked)
    calls.extend(('eth_call', [{'to': token, 'data': cast.calldata(signature, *args)}, block])
                 for signature, args in signatures)
    raw = _read_batch(rpc, calls)
    if len(raw) != len(calls):
        raise CustodyError('asset snapshot omitted required code/storage/issuer reads')
    token_hash = _code_hash(cast, raw[0])
    storage = {slot: hex_value(value, 32) for slot, value in zip(slots, raw[1:6])}
    chosen_impl, chosen_admin = ((IMPLEMENTATION_SLOT, ADMIN_SLOT) if key == 'USDT0' else (circle_impl, circle_admin))
    if any(int(value, 16) for slot, value in storage.items() if slot not in (chosen_impl, chosen_admin)):
        raise CustodyError('asset proxy has conflicting implementation/admin or unsupported beacon metadata')
    implementation, admin = (word_address(_word(storage[slot])) for slot in (chosen_impl, chosen_admin))
    if implementation == ZERO_ADDRESS or admin == ZERO_ADDRESS:
        raise CustodyError('asset proxy implementation/admin is zero or missing')
    fixed = raw[6:]
    decimals = int.from_bytes(_word(fixed[0]), 'big')
    if decimals != spec['decimals']:
        raise CustodyError('registered stable asset must retain exactly six decimals')
    issuer_roles = {role: word_address(_word(value)) for role, value in zip(roles, fixed[1:1 + len(roles)])}
    if ZERO_ADDRESS in issuer_roles.values():
        raise CustodyError('asset issuer authority is zero or unavailable')
    offset = 1 + len(roles)
    paused = _bool(fixed[offset]) if key == 'USDC' else None
    offset += int(key == 'USDC')
    blocked = {account: _bool(value) for account, value in zip(checked, fixed[offset:])}
    targets = [('eth_getCode', [implementation, block]), ('eth_getCode', [admin, block])]
    if key == 'USDT0':
        targets.append(('eth_call', [{'to': admin, 'data': cast.calldata('owner()')}, block]))
    identity_raw = _read_batch(rpc, targets)
    if len(identity_raw) != len(targets):
        raise CustodyError('asset snapshot omitted implementation/admin identity')
    admin_hash = _code_hash(cast, identity_raw[1], allow_empty=key == 'USDC')
    admin_owner = word_address(_word(identity_raw[2])) if key == 'USDT0' else None
    if admin_owner == ZERO_ADDRESS:
        raise CustodyError('asset proxy admin owner is zero')
    identity = {'token': token, 'decimals': decimals, 'token_code_hash': token_hash,
        'proxy': {'kind': spec['proxy_kind'], 'implementation_slot': chosen_impl,
                  'implementation': implementation, 'implementation_code_hash': _code_hash(cast, identity_raw[0]),
                  'admin_slot': chosen_admin, 'admin': admin, 'admin_code_hash': admin_hash, 'admin_owner': admin_owner},
        'issuer_roles': issuer_roles}
    final = [('eth_chainId', []), ('eth_getBlockByNumber', [hex(origin['height']), False]),
             ('eth_getBlockByNumber', [block, False])]
    final.append(('anvil_nodeInfo', []) if local else ('eth_getBlockByNumber', ['finalized', False]))
    final_chain, final_origin, final_selected, final_extra = _read_batch(rpc, final)
    _execution(final_chain, final_extra if local else None, execution_chain_id, origin)
    if (_block(final_origin) != origin or _block(final_selected) != selected or
            (not local and _block(final_extra)['height'] < origin['height'])):
        raise CustodyError('asset canonical snapshot changed during its complete identity read')
    return {'block': selected, 'identity': identity, 'status': {'paused': paused, 'blocked': blocked}}


def capture_asset_pin(rpc, cast, key, checkpoint, *, execution_chain_id=CHAIN_ID):
    """Explicit one-time snapshot; never used to auto-repair an existing pin."""
    origin = _point(checkpoint)
    snapshot = _snapshot(rpc, cast, key, origin, execution_chain_id)
    _check_compliance(snapshot)
    return validate_asset_pin_shape({'format': PIN_FORMAT, 'asset': key, 'source_chain_id': CHAIN_ID, 'origin': origin,
            'genesis_hash': GENESIS_HASH, 'identity': snapshot['identity'], 'mainnet_enabled': False})


def validate_asset_pin_shape(pin):
    """Pure schema/registry validation; this never asserts live state or readiness."""
    if (not isinstance(pin, dict) or set(pin) != _PIN_FIELDS or pin['format'] != PIN_FORMAT or
            type(pin['source_chain_id']) is not int or pin['source_chain_id'] != CHAIN_ID or
            pin['mainnet_enabled'] is not False or pin['genesis_hash'] != GENESIS_HASH):
        raise CustodyError('unknown asset pin or attempted mainnet activation')
    spec, origin = asset_spec(pin['asset']), _point(pin['origin'])
    identity = pin['identity']
    if (not isinstance(identity, dict) or set(identity) != {'token', 'decimals', 'token_code_hash', 'proxy', 'issuer_roles'} or
            identity['token'] != spec['address'] or type(identity['decimals']) is not int or identity['decimals'] != 6):
        raise CustodyError('asset pin token/decimals differs from the exact registry')
    proxy, roles = identity['proxy'], identity['issuer_roles']
    impl_slot, admin_slot = ((IMPLEMENTATION_SLOT, ADMIN_SLOT) if pin['asset'] == 'USDT0'
                            else (CIRCLE_IMPLEMENTATION_SLOT, CIRCLE_ADMIN_SLOT))
    if (not isinstance(proxy, dict) or set(proxy) != {'kind', 'implementation_slot', 'implementation',
            'implementation_code_hash', 'admin_slot', 'admin', 'admin_code_hash', 'admin_owner'} or
            proxy['kind'] != spec['proxy_kind'] or proxy['implementation_slot'] != impl_slot or proxy['admin_slot'] != admin_slot or
            not isinstance(roles, dict) or set(roles) != set(spec['issuer_role_signatures'])):
        raise CustodyError('asset pin proxy/issuer-role schema differs from the registry')
    addresses = [proxy['implementation'], proxy['admin'], *roles.values()]
    if pin['asset'] == 'USDT0':
        addresses.append(proxy['admin_owner'])
    elif proxy['admin_owner'] is not None:
        raise CustodyError('Circle proxy admin governance requires separately reviewed authority evidence')
    for value in addresses:
        if address(value) != value or value == ZERO_ADDRESS:
            raise CustodyError('asset pin contains a noncanonical/zero authority address')
    for value in (identity['token_code_hash'], proxy['implementation_code_hash'], proxy['admin_code_hash']):
        if hex_value(value, 32) != value or int(value, 16) == 0:
            raise CustodyError('asset pin contains a noncanonical/zero code hash')
    result = copy.deepcopy(pin)
    result['origin'] = origin
    return result


def _check_compliance(snapshot):
    if snapshot['status']['paused']:
        raise AssetComplianceBlocked('issuer-paused')
    if any(snapshot['status']['blocked'].values()):
        raise AssetComplianceBlocked('issuer-blocked')


def verify_asset_pin(rpc, cast, pin, *, execution_chain_id=LOCAL_CHAIN_ID, accounts=(), check_compliance=True):
    """Re-read all mutable identity fields at one explicit canonical checkpoint.

    Local fork mode observes latest owned state. Remote read-only verification
    uses a fresh provider-finalized checkpoint, matching the strict live client.
    """
    pin = validate_asset_pin_shape(pin)
    if type(check_compliance) is not bool:
        raise CustodyError('issuer compliance policy must be an explicit boolean')
    snapshot = _snapshot(rpc, cast, pin['asset'], pin['origin'], execution_chain_id, latest=True, accounts=accounts)
    if snapshot['identity'] != pin['identity']:
        raise CustodyError('registered asset code/proxy/admin/issuer identity changed; explicit review required')
    if check_compliance:
        _check_compliance(snapshot)
    return snapshot


def fund_local_fork_account(rpc, cast, pin, recipient, amount):
    """ARTIFICIAL local issuer funding through actual token code; never mint live.

    No token bytecode/storage/balances are overwritten. Public issuer roles are
    impersonated on an owned loopback Anvil31337 fork only, then always stopped.
    Gas ETH is artificial fixture funding, not stablecoin fee reimbursement.
    """
    pin, recipient, amount = validate_asset_pin_shape(pin), address(recipient), uint(amount, 256)
    if amount == 0 or amount > MAX_ATOMS or recipient == ZERO_ADDRESS:
        raise CustodyError('local issuer funding amount/recipient is outside its bound')
    local_rpc_url(rpc.url)  # Refuse DNS, remote hosts, credentials and redirects.
    verify_asset_pin(rpc, cast, pin, accounts=[recipient])
    spec = asset_spec(pin['asset'])
    token, transactions = spec['address'], []

    def mutation(method, params):
        chain, node, genesis, origin = _read_batch(rpc, [('eth_chainId', []), ('anvil_nodeInfo', []),
            ('eth_getBlockByNumber', ['0x0', False]), ('eth_getBlockByNumber', [hex(pin['origin']['height']), False])])
        _execution(chain, node, LOCAL_CHAIN_ID, pin['origin'])
        if not isinstance(genesis, dict) or genesis.get('hash') != GENESIS_HASH or _block(origin) != pin['origin']:
            raise CustodyError('local funding fork origin changed before mutation')
        return rpc.call(method, params)

    def send_as(sender, signature, *args):
        verify_asset_pin(rpc, cast, pin, accounts=[recipient])
        balance = _quantity(rpc.call('eth_getBalance', [sender, 'latest']), 256)
        if balance < 10**18:
            mutation('anvil_setBalance', [sender, hex(10**18)])
        mutation('anvil_impersonateAccount', [sender])
        try:
            tx = hex_value(mutation('eth_sendTransaction', [{'from': sender, 'to': token,
                'data': cast.calldata(signature, *args), 'gas': hex(800_000), 'value': '0x0', 'chainId': hex(LOCAL_CHAIN_ID)}]), 32)
            transactions.append(tx)
            deadline = time.monotonic() + 20
            receipt = None
            while time.monotonic() < deadline:
                receipt = rpc.call('eth_getTransactionReceipt', [tx])
                if receipt is not None:
                    break
                time.sleep(.05)
            if (not isinstance(receipt, dict) or hex_value(receipt.get('transactionHash'), 32) != tx or
                    _quantity(receipt.get('status')) != 1):
                raise CustodyError('local artificial issuer funding did not produce an exact successful receipt')
            block = rpc.call('eth_getBlockByNumber', [receipt['blockNumber'], False])
            if _block(block) != {'height': _quantity(receipt['blockNumber']), 'hash': hex_value(receipt['blockHash'], 32)}:
                raise CustodyError('local issuer funding receipt lost its canonical block')
        finally:
            mutation('anvil_stopImpersonatingAccount', [sender])

    before = int.from_bytes(_word(rpc.call('eth_call', [{'to': token,
        'data': cast.calldata('balanceOf(address)', recipient)}, 'latest'])), 'big')
    if pin['asset'] == 'USDT0':
        send_as(pin['identity']['issuer_roles']['oftContract()'], 'mint(address,uint256)', recipient, amount)
    else:
        send_as(pin['identity']['issuer_roles']['masterMinter()'], 'configureMinter(address,uint256)', recipient, amount)
        send_as(recipient, 'mint(address,uint256)', recipient, amount)
    after = int.from_bytes(_word(rpc.call('eth_call', [{'to': token,
        'data': cast.calldata('balanceOf(address)', recipient)}, 'latest'])), 'big')
    if after - before != amount:
        raise CustodyError('local issuer mint did not increase the exact recipient token balance')
    return {'funding': 'artificial-authorized-role-mint', 'transactions': transactions, 'amount': amount, 'recipient': recipient}


def _abi_text(encoded):
    data = bytes.fromhex(hex_value(encoded)[2:])
    if len(data) < 64 or int.from_bytes(data[:32], 'big') != 32:
        raise CustodyError('token signing name has malformed ABI encoding')
    size = int.from_bytes(data[32:64], 'big')
    if not 1 <= size <= 128 or len(data) != 64 + ((size + 31) // 32) * 32 or any(data[64 + size:]):
        raise CustodyError('token signing name exceeds its canonical bound')
    try:
        return data[64:64 + size].decode('utf-8')
    except UnicodeError as error:
        raise CustodyError('token signing name is not UTF-8') from error


def build_sponsored_deposit(rpc, cast, pin, vault, sender, amount, account, user_salt,
                            *, valid_after=None, valid_before=None):
    """Unsigned local-fork EIP-3009 payload; wallet signing stays with the source.

    This v/r/s helper supports code-empty EOAs only. Contract wallets and EIP7702
    delegations need separately qualified ERC1271/bytes-signature handling;
    recovering an EOA key does not establish those wallets' token authority.
    The full fixture must separately show the source holds zero ETH and that
    the gas sponsor broadcasts only on loopback31337. No fee quote is invented.
    """
    local_rpc_url(rpc.url)
    pin = validate_asset_pin_shape(pin)
    vault, sender, amount = address(vault), address(sender), uint(amount)
    account, salt = hex_value(account, 32), hex_value(user_salt, 32)
    if sender in (ZERO_ADDRESS, vault) or vault == ZERO_ADDRESS or amount == 0 or amount > MAX_ATOMS or int(account, 16) == 0:
        raise CustodyError('sponsored deposit economics are outside their exact bounds')
    snapshot = verify_asset_pin(rpc, cast, pin, accounts=[vault, sender])
    block = hex(snapshot['block']['height'])
    if hex_value(rpc.call('eth_getCode', [sender, block])) != '0x':
        raise CustodyError('sponsored v/r/s deposit requires a code-empty EOA; EIP7702/ERC1271 wallets remain unqualified')
    token = pin['identity']['token']
    def call(target, signature, *args):
        return rpc.call('eth_call', [{'to': target, 'data': cast.calldata(signature, *args)}, block])
    if (word_address(_word(call(vault, 'TOKEN()'))) != token or
            int.from_bytes(_word(call(vault, 'DEPLOYMENT_CHAIN_ID()')), 'big') != LOCAL_CHAIN_ID):
        raise CustodyError('sponsored deposit vault differs from its exact local token/chain')
    instance, asset = hex_value(call(vault, 'INSTANCE_ID()'), 32), hex_value(call(vault, 'Q_ASSET_ID()'), 32)
    if int(instance, 16) == 0 or int(asset, 16) == 0:
        raise CustodyError('sponsored deposit vault has no explicit instance/asset binding')
    type_hash = cast.keccak('DepositAuthorization(uint256 chainId,address vault,bytes32 instance,bytes32 asset,address sender,uint256 amount,bytes32 account,bytes32 userSalt)')
    nonce = cast.keccak(cast.abi_encode('x(bytes32,uint256,address,bytes32,bytes32,address,uint256,bytes32,bytes32)',
        type_hash, LOCAL_CHAIN_ID, vault, instance, asset, sender, amount, account, salt))
    if hex_value(call(vault, 'depositAuthorizationNonce(address,uint256,bytes32,bytes32)', sender, amount, account, salt), 32) != nonce:
        raise CustodyError('sponsored deposit nonce differs from independently bound B3 account/economics')
    spec = asset_spec(pin['asset'])
    name = _abi_text(call(token, 'name()'))
    domain = cast.keccak(cast.abi_encode('x(bytes32,bytes32,bytes32,uint256,address)',
        cast.keccak('EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)'),
        cast.keccak('0x' + name.encode('utf-8').hex()), cast.keccak(spec['authorization_domain_version']), LOCAL_CHAIN_ID, token))
    if hex_value(call(token, 'DOMAIN_SEPARATOR()'), 32) != domain:
        raise CustodyError('token EIP-3009 domain differs from its independently computed local chain/name/version')
    receive_type = cast.keccak('ReceiveWithAuthorization(address from,address to,uint256 value,uint256 validAfter,uint256 validBefore,bytes32 nonce)')
    if hex_value(call(token, 'RECEIVE_WITH_AUTHORIZATION_TYPEHASH()'), 32) != receive_type:
        raise CustodyError('token receive authorization type differs from reviewed EIP-3009')
    selected = rpc.call('eth_getBlockByNumber', [block, False])
    if _block(selected) != snapshot['block']:
        raise CustodyError('sponsored deposit source block changed during authorization construction')
    timestamp = _quantity(selected.get('timestamp'))
    after = max(0, timestamp - 1) if valid_after is None else uint(valid_after)
    before = timestamp + 300 if valid_before is None else uint(valid_before)
    if not after < timestamp < before <= timestamp + 3600:
        raise CustodyError('sponsored deposit requires an active bounded authorization window')
    if _bool(call(token, 'authorizationState(address,bytes32)', sender, nonce)):
        raise CustodyError('source token authorization nonce is already consumed/cancelled')
    message = cast.keccak(cast.abi_encode('x(bytes32,address,address,uint256,uint256,uint256,bytes32)',
        receive_type, sender, vault, amount, after, before, nonce))
    digest = cast.keccak('0x1901' + domain[2:] + message[2:])
    return {'sender': sender, 'amount': amount, 'account': account, 'user_salt': salt,
            'valid_after': after, 'valid_before': before, 'nonce': nonce, 'digest': digest, 'token_domain': domain}
