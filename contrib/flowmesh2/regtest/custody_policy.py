#!/usr/bin/env python3
"""Pinned custody release policy. This release authorizes local regtest only.

The contract is generic, but no deployed address, remote provider or live token
is authorized by these tools. A future chain policy needs separately qualified
finality/provider rules, deployment approvals and independent security review.
"""
import json

PROFILE = 'fm2-custody-v2-regtest/1'
LEGACY_PROFILE = 'fm2-custody-fixture/1'
POLICY_FORMAT = 'fm2-custody-release-policy/1'
FIELDS = {'format', 'chain_id', 'genesis_hash', 'token_code_hash', 'vault_code_hash',
          'policy_hash', 'domain_separator', 'roster_hash', 'roster_epoch', 'dispute_seconds',
          'rotation_seconds', 'max_certificate_lifetime', 'minimum_confirmations',
          'finality_mode', 'mainnet_enabled', 'proxy_implementation'}


def fail(message):
    # Import lazily to avoid the CustodyDriver -> policy -> driver cycle.
    from custody import CustodyError
    raise CustodyError(message)


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'))


def _bytes(driver, signature):
    from custody import hex_value
    return hex_value(driver.call(driver.vault, signature), 32)


def _code_hash(driver, target):
    from custody import hex_value
    code = hex_value(driver.rpc.call('eth_getCode', [target, 'latest']))
    if code == '0x':
        fail('pinned custody/token has no runtime code')
    return driver.cast.keccak(code)


def _implementation_slots(driver, policy):
    implementation_slot = '0x360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc'
    beacon_slot = '0xa3f0ad74e5423aebfd80d3ef4346578335a9a72aeaee59ff6cb3582b35133d50'
    circle_slot = driver.cast.keccak('org.zeppelinos.proxy.implementation')
    slots = [implementation_slot, beacon_slot, circle_slot]
    proxy = policy['proxy_implementation']
    if proxy is not None and proxy['slot'] not in slots:
        slots.append(proxy['slot'])
    return slots, beacon_slot


def verify_token_implementation(driver, policy, slot_values, implementation_code=None):
    """Pin explicit proxy behavior; a proxy's own runtime hash is insufficient.

    Probe the standard EIP-1967 implementation/beacon slots and Circle's
    Zeppelin implementation slot. Other proxy families need an explicitly
    reviewed implementation slot and bytecode pin; this is not a universal
    proxy detector or a claim that arbitrary token bytecode is trustworthy.
    """
    from custody import hex_value, word_address, words
    slots, beacon_slot = _implementation_slots(driver, policy)
    if set(slot_values) != set(slots):
        fail('custody snapshot did not cover its exact required token implementation slots')
    selected = {}
    for slot in slots:
        value = hex_value(slot_values[slot], 32)
        if int(value, 16) != 0:
            selected[slot] = value
    if beacon_slot in selected:
        fail('beacon-proxy token is not supported by this custody release')
    proxy = policy['proxy_implementation']
    if proxy is None:
        if selected:
            fail('token proxy requires an explicit reviewed implementation slot/address/code pin')
        return
    if any(slot != proxy['slot'] for slot in selected):
        fail('token has conflicting/unrecognized proxy implementation metadata')
    implementation = word_address(words(slot_values[proxy['slot']], 1)[0])
    code = hex_value(implementation_code) if implementation_code is not None else '0x'
    if implementation != proxy['address'] or code == '0x' or driver.cast.keccak(code) != proxy['code_hash']:
        fail('token proxy implementation changed; custody observation/signing is fenced')


def _expected_hashes(driver, policy):
    # Recompute from the public manifest, not the contract's claimed policy.
    key = canonical([policy, driver.token, driver.asset, driver.instance, driver.operator_addresses])
    prior = getattr(driver, '_custody_expected_hashes', None)
    if prior is not None and prior[0] == key:
        return prior[1]
    roster = '[' + ','.join(driver.operator_addresses) + ']'
    roster_hash = driver.cast.keccak(driver.cast.abi_encode('x(address[4])', roster))
    policy_hash = driver.cast.keccak(driver.cast.abi_encode(
        'x(uint256,address,bytes32,bytes32,bytes32,uint64,uint64,uint64,bytes32)',
        31337, driver.token, policy['token_code_hash'], driver.asset, driver.instance,
        policy['dispute_seconds'], policy['rotation_seconds'], policy['max_certificate_lifetime'], roster_hash))
    salt = driver.cast.keccak(driver.cast.abi_encode('x(bytes32,address,bytes32,bytes32,bytes32)',
        driver.instance, driver.token, driver.asset, policy['token_code_hash'], policy_hash))
    domain = driver.cast.keccak(driver.cast.abi_encode('x(bytes32,bytes32,bytes32,uint256,address,bytes32)',
        driver.cast.keccak('EIP712Domain(string name,string version,uint256 chainId,address verifyingContract,bytes32 salt)'),
        driver.cast.keccak('B3 Rebirth Custody'), driver.cast.keccak('2'), 31337, driver.vault, salt))
    result = roster_hash, policy_hash, domain
    driver._custody_expected_hashes = key, result
    return result


def validate_policy_shape(policy):
    from custody import hex_value, uint
    if not isinstance(policy, dict) or set(policy) != FIELDS or policy['format'] != POLICY_FORMAT:
        fail('unknown/incomplete custody release policy; refusing inferred defaults')
    if (policy['mainnet_enabled'] is not False or policy['chain_id'] != 31337 or
            policy['finality_mode'] != 'isolated-confirmations' or policy['minimum_confirmations'] != 2 or
            policy['roster_epoch'] != 0):
        fail('this release permits only isolated chain31337, two confirmations and the pinned initial roster')
    for field in ('genesis_hash', 'token_code_hash', 'vault_code_hash', 'policy_hash', 'domain_separator', 'roster_hash'):
        if hex_value(policy[field], 32) != policy[field]:
            fail('custody policy contains noncanonical hashes')
    for field in ('dispute_seconds', 'rotation_seconds', 'max_certificate_lifetime'):
        uint(policy[field], 64)
    if (not 30 <= policy['dispute_seconds'] <= 604800 or not 60 <= policy['rotation_seconds'] <= 2592000 or
            not 0 < policy['max_certificate_lifetime'] <= 86400):
        fail('custody delays/lifetime violate immutable policy bounds')
    proxy = policy['proxy_implementation']
    if proxy is not None:
        from custody import address
        if (not isinstance(proxy, dict) or set(proxy) != {'slot', 'address', 'code_hash'} or
                hex_value(proxy['slot'], 32) != proxy['slot'] or address(proxy['address']) != proxy['address'] or
                proxy['address'] == '0x' + '00' * 20 or hex_value(proxy['code_hash'], 32) != proxy['code_hash']):
            fail('malformed explicit token proxy implementation pin')


def pin_custody(driver, config, proxy_implementation=None):
    """Seal a fresh deployment's public identity; no signing keys are returned.

    Never invoked automatically to repair/rebind an existing configured route.
    Operators receive this exact result in their authenticated deployment domain.
    """
    from custody import hex_value
    driver.rpc.assert_fixture()
    genesis = driver.rpc.call('eth_getBlockByNumber', ['0x0', False])
    policy = {'format': POLICY_FORMAT, 'chain_id': 31337, 'genesis_hash': hex_value(genesis['hash'], 32),
        'token_code_hash': _code_hash(driver, driver.token), 'vault_code_hash': _code_hash(driver, driver.vault),
        'policy_hash': _bytes(driver, 'POLICY_HASH()'), 'domain_separator': _bytes(driver, 'DOMAIN_SEPARATOR()'),
        'roster_hash': _bytes(driver, 'ROSTER_HASH()'), 'roster_epoch': driver.call_uint(driver.vault, 'rosterEpoch()'),
        'dispute_seconds': driver.call_uint(driver.vault, 'DISPUTE_SECONDS()'),
        'rotation_seconds': driver.call_uint(driver.vault, 'ROTATION_SECONDS()'),
        'max_certificate_lifetime': driver.call_uint(driver.vault, 'MAX_CERTIFICATE_LIFETIME()'),
        'minimum_confirmations': 2, 'finality_mode': 'isolated-confirmations', 'mainnet_enabled': False,
        'proxy_implementation': proxy_implementation}
    validate_policy_shape(policy)
    config['custody_profile'], config['custody_policy'] = PROFILE, policy
    config['domain_separator'] = policy['domain_separator']
    config['dispute_seconds'] = policy['dispute_seconds']
    driver.config = dict(config)
    driver.custody_profile = PROFILE
    verify_custody(driver)
    return policy


def _custody_reads(driver):
    policy = driver.config.get('custody_policy')
    validate_policy_shape(policy)
    slots, _ = _implementation_slots(driver, policy)
    calls = [('eth_getCode', [driver.token]), ('eth_getCode', [driver.vault]),
             ('eth_call', [{'to': driver.vault, 'data': driver.cast.calldata('custodyIdentity()')}])]
    calls.extend(('eth_getStorageAt', [driver.token, slot]) for slot in slots)
    proxy = policy['proxy_implementation']
    if proxy is not None:
        calls.append(('eth_getCode', [proxy['address']]))
    return policy, slots, calls


def _verify_custody_snapshot(driver, policy, slots, calls, snapshot):
    from custody import hex_value, word_address, words
    proxy = policy['proxy_implementation']
    if snapshot['genesis']['hash'] != policy['genesis_hash']:
        fail('custody RPC genesis differs from the pinned source deployment')
    results = snapshot['results']
    if len(results) != len(calls):
        fail('custody snapshot result count differs from its exact required identity reads')
    token_code, vault_code = hex_value(results[0]), hex_value(results[1])
    if (token_code == '0x' or vault_code == '0x' or driver.cast.keccak(token_code) != policy['token_code_hash'] or
            driver.cast.keccak(vault_code) != policy['vault_code_hash']):
        fail('custody/token runtime differs from the pinned code identity')
    expected = _expected_hashes(driver, policy)
    if expected != (policy['roster_hash'], policy['policy_hash'], policy['domain_separator']):
        fail('custody public policy/domain does not recompute from the pinned token/roster/deployment')
    identity = words(results[2], 14)
    expected_identity = [31337, policy['token_code_hash'], policy['policy_hash'], policy['domain_separator'],
        policy['roster_hash'], 0, policy['dispute_seconds'], policy['rotation_seconds'],
        policy['max_certificate_lifetime'], driver.token, driver.asset, driver.instance, False, '0x' + '00' * 32]
    actual_identity = [int.from_bytes(identity[0], 'big'), *('0x' + value.hex() for value in identity[1:5]),
        *(int.from_bytes(value, 'big') for value in identity[5:9]), word_address(identity[9]),
        '0x' + identity[10].hex(), '0x' + identity[11].hex(), bool(int.from_bytes(identity[12], 'big')),
        '0x' + identity[13].hex()]
    if actual_identity != expected_identity:
        fail('custody identity/roster/active policy changed or paused; explicit operator migration is required')
    verify_token_implementation(driver, policy, dict(zip(slots, results[3:3 + len(slots)])),
                                results[-1] if proxy is not None else None)
    return policy


def verify_custody(driver):
    if 'regtest_arbitrum_fork' in driver.config or hasattr(driver.rpc, 'fork_context'):
        from arbitrum_fork import verify_fork_driver
        verify_fork_driver(driver)
    policy, slots, calls = _custody_reads(driver)
    return _verify_custody_snapshot(driver, policy, slots, calls, driver.rpc.state_batch(calls))


def verify_custody_routes(drivers, *, source_point=None):
    """Check every route at ONE fresh canonical source head, with no state cache.

    All code, proxy implementation/storage, domain and mutable roster/epoch/
    pause/rotation fields remain present in the shared read. Each route retains
    its independent deployment/genesis/policy comparisons. state_batch pins all
    reads to one explicit number and rechecks its canonical hash after replies.
    """
    if not isinstance(drivers, (list, tuple)) or not 1 <= len(drivers) <= 4:
        fail('shared custody identity snapshot requires one to four explicit routes')
    rpc = drivers[0].rpc
    fork_contexts = [driver.config.get('regtest_arbitrum_fork') for driver in drivers]
    if any(value is not None for value in fork_contexts) and any(value != fork_contexts[0] for value in fork_contexts):
        fail('shared custody snapshot cannot mix local fork origins or token/provider pins')
    prepared, calls = [], []
    for driver in drivers:
        if driver.custody_profile != PROFILE or driver.rpc.url != rpc.url:
            fail('shared custody identity snapshot requires the same pinned custody profile/source RPC')
        if 'regtest_arbitrum_fork' in driver.config or hasattr(driver.rpc, 'fork_context'):
            from arbitrum_fork import verify_fork_driver
            verify_fork_driver(driver)
        policy, slots, own = _custody_reads(driver)
        prepared.append((driver, policy, slots, own))
        calls.extend(own)
    if len(calls) > rpc.MAX_BATCH_ITEMS:
        fail('shared custody identity snapshot exceeds the bounded complete read set')
    point = None
    if source_point is not None:
        from custody import hex_value, uint
        if not isinstance(source_point, dict) or set(source_point) != {'height', 'hash', 'minimum_confirmations'}:
            fail('source checkpoint has unknown fields')
        point = {'height': uint(source_point['height'], 64), 'hash': hex_value(source_point['hash'], 32),
                 'minimum_confirmations': uint(source_point['minimum_confirmations'], 32)}
        if (source_point['minimum_confirmations'] != point['minimum_confirmations'] or
                any(point['minimum_confirmations'] != pinned['minimum_confirmations'] for _, pinned, _, _ in prepared)):
            fail('source checkpoint attempts to lower the pinned finality requirement')
    snapshot = rpc.state_batch(calls, block_heights=[point['height']]) if point is not None else rpc.state_batch(calls)
    if len(snapshot['results']) != len(calls):
        fail('shared custody identity snapshot omitted required route state')
    verified, offset = [], 0
    for driver, policy, slots, own in prepared:
        part = {'genesis': snapshot['genesis'], 'head': snapshot['head'],
                'results': snapshot['results'][offset:offset + len(own)]}
        verified.append(_verify_custody_snapshot(driver, policy, slots, own, part))
        offset += len(own)
    if point is not None:
        from custody import RpcError
        try:
            blocks = snapshot['blocks']
            if not isinstance(blocks, list) or len(blocks) != 1:
                fail('source checkpoint read was omitted from the shared canonical snapshot')
            actual = rpc._block_point(blocks[0])
            if (actual != {'height': point['height'], 'hash': point['hash']} or
                    point['height'] + point['minimum_confirmations'] - 1 > snapshot['head']['height']):
                fail('source checkpoint is replaced, unavailable or not sufficiently confirmed')
        except (KeyError, TypeError, ValueError, RpcError) as error:
            fail('source checkpoint is replaced, unavailable or not sufficiently confirmed')
        return {'head': snapshot['head'], 'source_point': point}
    return verified


def verify_source_point(driver, point):
    """Check the pinned source-finality policy before accepting a source range."""
    from custody import hex_value, uint
    if 'regtest_arbitrum_fork' in driver.config or hasattr(driver.rpc, 'fork_context'):
        from arbitrum_fork import verify_fork_driver
        verify_fork_driver(driver)
    policy = driver.config.get('custody_policy')
    validate_policy_shape(policy)
    if not isinstance(point, dict) or set(point) != {'height', 'hash', 'minimum_confirmations'}:
        fail('source checkpoint has unknown fields')
    height, digest = uint(point['height'], 64), hex_value(point['hash'], 32)
    if point['minimum_confirmations'] != policy['minimum_confirmations']:
        fail('source checkpoint attempts to lower the pinned finality requirement')
    driver.rpc.assert_fixture()
    latest = int(driver.rpc.call('eth_blockNumber'), 16)
    block = driver.rpc.call('eth_getBlockByNumber', [hex(height), False])
    if (height + policy['minimum_confirmations'] - 1 > latest or not isinstance(block, dict) or
            int(block['number'], 16) != height or hex_value(block['hash'], 32) != digest):
        fail('source checkpoint is replaced, unavailable or not sufficiently confirmed')
    return True


def withdrawal_digest(driver, withdrawal):
    """Compute typed custody authority locally, and compare the chain view.

    A provider cannot turn an otherwise valid withdrawal proof into a signature
    on unrelated bytes by substituting withdrawalDigest's eth_call response.
    """
    policy = verify_custody(driver)
    if 'regtest_arbitrum_fork' in driver.config or hasattr(driver.rpc, 'fork_context'):
        from arbitrum_fork import verify_fork_driver
        verify_fork_driver(driver, accounts=[withdrawal['recipient']], check_compliance=True)
    fields = driver.withdrawal_tuple(withdrawal)
    type_hash = driver.cast.keccak('Withdraw(bytes32 id,bytes32 account,bytes32 asset,uint256 amount,address recipient,uint64 slot,bytes32 stateRoot,uint64 validUntil,uint64 rosterEpoch)')
    message = driver.cast.keccak(driver.cast.abi_encode(
        'x(bytes32,bytes32,bytes32,bytes32,uint256,address,uint64,bytes32,uint64,uint64)', type_hash,
        withdrawal['id'], withdrawal['account'], withdrawal['asset'], withdrawal['amount'], withdrawal['recipient'],
        withdrawal['slot'], withdrawal['stateRoot'], withdrawal['validUntil'], policy['roster_epoch']))
    digest = driver.cast.keccak('0x1901' + policy['domain_separator'][2:] + message[2:])
    from custody import WITHDRAWAL_ABI, hex_value
    reported = hex_value(driver.call(driver.vault, 'withdrawalDigest(' + WITHDRAWAL_ABI + ')', fields), 32)
    if reported != digest:
        fail('custody withdrawal digest differs from independently computed committed authority')
    return digest
