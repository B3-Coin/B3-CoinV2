#!/usr/bin/env python3
"""Untrusted raw settlement/history availability from the owned local rollup.

No Python result grants finality or execution authority. Native code must bind
the exact owned genesis, checkpoint, deployment policy and requested range.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

from runtime import Rpc, assets_info, expect, load, read_json, safe, status, write_json, MODULE_ROOT, OWNER, SEQUENCER

BRIDGE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BRIDGE))
sys.path.insert(0, str(BRIDGE / 'arbitrum_history'))
sys.path.insert(0, str(BRIDGE / 'nitro_regtest'))
from capture_eth_receipts_fixture import keccak256
_spec = importlib.util.spec_from_file_location('b3_owned_raw_export', BRIDGE / 'nitro_regtest/export_witness.py')
_raw_export = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_raw_export)
Objects, account_request = _raw_export.Objects, _raw_export.account_request
from proof_bundle import Assertion, AccountWitness, StorageWitness, IndexedRecord, BlockWitness, Proof, encode_proof

PRIMARY = '0x360894a13ba1a3210667c828492db98dca3e2076cc3735a920a3ca505d382bbc'
SECONDARY = '0x2b1dbce74324248c222f0ec2d5ed7bd323cfc425b336f0253c5ccfda7265546d'
ADMIN = '0x' + (int.from_bytes(keccak256(b'eip1967.proxy.admin'), 'big') - 1).to_bytes(32, 'big').hex()
ZERO = bytes(32)


def raw(value, size=None):
    expect(type(value) is str and value.startswith('0x') and len(value) % 2 == 0, 'raw hex required')
    data = bytes.fromhex(value[2:])
    expect(size is None or len(data) == size, 'raw field size refused')
    return data


def hex32(value):
    expect(type(value) is int and 0 <= value < 2 ** 256, 'uint256 required')
    return '0x' + value.to_bytes(32, 'big').hex()


def abi_type(item):
    value = item['type']
    return '(' + ','.join(abi_type(c) for c in item['components']) + ')' + value[5:] if value.startswith('tuple') else value


def layout(assets):
    found = []
    layouts = {}
    for path in sorted((assets / 'contracts/build/contracts/build-info').glob('*.json')):
        data = read_json(path, 64 * 1024 * 1024)
        for contracts in data['output']['contracts'].values():
            for name, compiled in contracts.items():
                if name in ('RollupUserLogic', 'UpgradeExecutorMock', 'EdgeChallengeManager', 'Bridge', 'Inbox', 'Outbox', 'SequencerInbox', 'ProxyAdmin'):
                    layouts[name] = compiled['storageLayout']
        entry = data['output']['contracts'].get('src/rollup/RollupUserLogic.sol', {}).get('RollupUserLogic')
        if entry is not None:
            found.append((path, entry))
    expect(len(found) == 1, 'one exact compiled RollupUserLogic layout required')
    path, entry = found[0]
    sl = entry['storageLayout']
    labels = {x['label']: x for x in sl['storage']}
    for name, slot in (('_latestConfirmed', 116), ('_assertions', 117), ('wasmModuleRoot', 104)):
        expect(int(labels[name]['slot']) == slot and labels[name]['offset'] == 0, 'compiled settlement layout changed')
    event = next(x for x in entry['abi'] if x.get('type') == 'event' and x.get('name') == 'AssertionCreated')
    signature = 'AssertionCreated(' + ','.join(abi_type(x) for x in event['inputs']) + ')'
    assertion_type = sl['types'][sl['types'][labels['_assertions']['type']]['value']]
    members = {x['label']: x for x in assertion_type['members']}
    expect(members['createdAtBlock']['offset'] == 16 and members['status']['offset'] == 25,
           'compiled assertion packing changed')
    return {'storage_layout': sl, 'compiled_component_storage_layouts': layouts,
            'governance_layout_note': 'UpgradeExecutorMock has the same inherited storage declarations as the real dependency UpgradeExecutor; actual governance runtime code comes from the pinned upgrade-executor package artifact, not the mock.',
            'build_info_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'assertion_event_abi': event, 'assertion_event_signature': signature,
            'assertion_event_topic': '0x' + keccak256(signature.encode()).hex()}


def witness(rpc, address, slots, block_hash, objects):
    request = account_request({'address': address.lower(), 'storage_keys': list(slots)})
    response = rpc.call('eth_getProof', [request['address'], request['storage_keys'],
                                      {'blockHash': block_hash, 'requireCanonical': True}])
    expect(response['address'].lower() == address.lower(), 'proof account changed')
    expected = {int(x, 16) for x in slots}
    actual = [int(x['key'], 16) for x in response['storageProof']]
    expect(len(actual) == len(expected) and set(actual) == expected, 'proof keys changed')
    storage = tuple(StorageWitness(int(x['key'], 16).to_bytes(32, 'big'), tuple(raw(n) for n in x['proof']))
                    for x in response['storageProof'])
    result = AccountWitness(raw(address, 20), tuple(raw(n) for n in response['accountProof']), storage)
    record = {'address': address.lower(), 'nodes': [objects.put(n) for n in result.nodes],
              'storage': [{'slot': '0x' + s.slot.hex(), 'nodes': [objects.put(n) for n in s.nodes]} for s in storage]}
    return result, record


def block(rpc, block_hash, objects, complete=False):
    info = rpc.call('eth_getBlockByHash', [block_hash, False])
    expect(info is not None and info['hash'].lower() == block_hash.lower(), 'block unavailable')
    header = raw(rpc.call('debug_getRawHeader', [block_hash]))
    expect(keccak256(header) == raw(block_hash, 32), 'raw header preimage changed')
    receipts, transactions = (), ()
    if complete:
        expect(len(info['transactions']) <= 8192, 'block record bound exceeded')
        receipt_data = rpc.call('debug_getRawReceipts', [block_hash])
        expect(len(receipt_data) == len(info['transactions']), 'incomplete receipt set')
        receipts = tuple(IndexedRecord(i, raw(x)) for i, x in enumerate(receipt_data))
        transactions = tuple(IndexedRecord(i, raw(rpc.call('eth_getRawTransactionByBlockHashAndIndex', [block_hash, hex(i)])))
                             for i in range(len(receipts)))
    result = BlockWitness(header, receipts, transactions)
    record = {'number': int(info['number'], 16), 'hash': info['hash'].lower(), 'parent_hash': info['parentHash'].lower(),
              'header_sha256': objects.put(header), 'receipts': [objects.put(x.encoded) for x in receipts],
              'transactions': [objects.put(x.encoded) for x in transactions]}
    return result, record, info


def write_raw(path, data):
    expect(type(data) is bytes and len(data) <= 32 * 1024 * 1024, 'bounded proof bytes required')
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(fd, 'wb') as stream:
        stream.write(data); stream.flush(); os.fsync(stream.fileno())
    return hashlib.sha256(data).hexdigest()


def deployment_graph(l1, rollup, deployment, block_number, policy, objects, l1_hash, assertion_slots):
    def word(address, slot):
        return raw(l1.call('eth_getStorageAt', [address, slot, block_number]), 32)
    def address_at(address, slot):
        return '0x' + word(address, slot)[-20:].hex()
    roles = [('rollup', rollup, 'RollupUserLogic'),
             ('rollup-admin', address_at(rollup, PRIMARY), None),
             ('rollup-user', address_at(rollup, SECONDARY), None)]
    proxies = [('governance', address_at(rollup, ADMIN), 'UpgradeExecutorMock'),
               ('challenge', address_at(rollup, hex32(105)), 'EdgeChallengeManager'),
               ('bridge', address_at(rollup, hex32(107)), 'Bridge'),
               ('inbox', address_at(rollup, hex32(106)), 'Inbox'),
               ('outbox', address_at(rollup, hex32(108)), 'Outbox'),
               ('sequencer', deployment['sequencer-inbox'].lower(), 'SequencerInbox')]
    for name, address, contract in proxies:
        roles.extend([(name + '-proxy', address, contract), (name + '-logic', address_at(address, PRIMARY), None)])
    roles.extend([('core-proxy-admin', address_at(proxies[1][1], ADMIN), 'ProxyAdmin'),
                  ('governance-proxy-admin', address_at(proxies[0][1], ADMIN), 'ProxyAdmin')])
    witnesses, records, graph, aliases, seen = [], [], [], [], {}
    for role, address, contract in roles:
        expect(address != '0x' + '00' * 20, 'zero deployment graph member')
        if address in seen:
            aliases.append({'role': role, 'address': address, 'same_account_as': seen[address]})
            continue
        seen[address] = role
        slots = set()
        if contract:
            sl = policy['compiled_component_storage_layouts'][contract]
            for item in sl['storage']:
                if 'gap' in item['label']:
                    continue
                start = int(item['slot']); kind = sl['types'][item['type']]
                # Static structs can occupy several words. Mapping/array heads
                # are included, with selected public membership leaves below.
                count = (int(kind['numberOfBytes']) + 31) // 32 if kind['encoding'] == 'inplace' else 1
                for index in range(count):
                    slots.add(hex32(start + index))
            if role.endswith('-proxy') or role == 'rollup':
                slots.update((PRIMARY, ADMIN))
        if role == 'rollup':
            slots.add(SECONDARY)
            # Runtime staker lists/funds are not policy. Keep exact reviewed
            # configuration and required current confirmed assertion witnesses.
            slots.difference_update(hex32(x) for x in range(116, 122))
            slots.discard(hex32(124))
            slots.update(assertion_slots)
            count = int.from_bytes(word(address, hex32(114)), 'big')
            expect(count <= 16, 'validator registry exceeds owned export budget')
            start = int.from_bytes(keccak256((114).to_bytes(32, 'big')), 'big')
            for index in range(count):
                slot = hex32(start + index); slots.add(slot)
                validator_address = word(address, slot)
                slots.add('0x' + keccak256(validator_address + (115).to_bytes(32, 'big')).hex())
        if role == 'governance-proxy':
            role_slot = next(x for x in policy['compiled_component_storage_layouts'][contract]['storage'] if x['label'] == '_roles')
            for name, member in ((b'ADMIN_ROLE', address), (b'EXECUTOR_ROLE', OWNER)):
                base = int.from_bytes(keccak256(keccak256(name) + int(role_slot['slot']).to_bytes(32, 'big')), 'big')
                slots.add(hex32(base + 1))
                slots.add('0x' + keccak256(bytes(12) + raw(member, 20) + base.to_bytes(32, 'big')).hex())
        if role == 'challenge-proxy':
            array_base = int.from_bytes(keccak256((7).to_bytes(32, 'big')), 'big')
            count = int.from_bytes(word(address, hex32(7)), 'big')
            expect(count <= 8, 'challenge stake levels exceed bounded reviewed graph')
            slots.update(hex32(array_base + x) for x in range(count))
        if role == 'bridge-proxy':
            # Accumulator lengths and the sequencer's reported message count
            # advance on normal batches/deposits; they are not configuration.
            slots.difference_update(hex32(x) for x in (6, 7, 10))
            for label, member in (('allowedDelayedInboxesMap', deployment['inbox']),
                                  ('allowedOutboxesMap', address_at(rollup, hex32(108)))):
                field = next(x for x in sl['storage'] if x['label'] == label)
                value_type = sl['types'][sl['types'][field['type']]['value']]
                base = int.from_bytes(keccak256(bytes(12) + raw(member, 20) + int(field['slot']).to_bytes(32, 'big')), 'big')
                for item in value_type['members']:
                    slots.add(hex32(base + int(item['slot'])))
        if role == 'sequencer-proxy':
            slots.discard(hex32(0))  # totalDelayedMessagesRead is routine progress
            for key in (3, 9):
                slots.add('0x' + keccak256(bytes(12) + raw(SEQUENCER, 20) + key.to_bytes(32, 'big')).hex())
        expect(len(slots) <= 64, 'component policy exceeds storage witness budget')
        ordered = sorted(slots, key=lambda s: int(s, 16))
        w, rec = witness(l1, address, ordered, l1_hash, objects)
        code = raw(l1.call('eth_getCode', [address, block_number]))
        expect(code, 'deployment graph contract has no runtime code')
        witnesses.append(w); records.append(rec)
        graph.append({'role': role, 'address': address, 'code_keccak256': '0x' + keccak256(code).hex(),
                      'code_sha256': objects.put(code), 'storage': [{'slot': s, 'value': '0x' + word(address, s).hex()} for s in ordered]})
    expect(len(witnesses) <= 17, 'full deployment graph exceeds two bounded account groups')
    return witnesses, records, graph, aliases


def export_settled(runtime, output, *, previous_height, previous_hash, to_height, to_hash, accounts=()):
    runtime, config = load(runtime)
    expect(config.get('ready') is True and all(status(runtime)['running'].values()), 'complete owned runtime required')
    expect(type(previous_height) is int and type(to_height) is int and 0 <= previous_height < to_height
           and to_height - previous_height <= 128, 'bounded nonempty requested range required')
    raw(previous_hash, 32); raw(to_hash, 32)
    expect(len(accounts) <= 16, 'account request bound exceeded')
    accounts = [account_request(x) for x in accounts]
    expect(len({x['address'] for x in accounts}) == len(accounts), 'duplicate historical account')
    assets, av = assets_info(Path(config['assets']))
    policy = layout(assets)
    l1, l2, validator = (Rpc(config['ports'][r]) for r in ('geth', 'sequencer', 'validator'))
    for rpc, chain, genesis in ((l1, 1337, config['l1_genesis']), (l2, 412346, config['l2_genesis'])):
        expect(int(rpc.call('eth_chainId'), 16) == chain and rpc.call('eth_getBlockByNumber', ['0x0', False])['hash'] == genesis,
               'owned chain identity changed')
    output = safe(output, exists=False); output.mkdir(mode=0o700)
    objects = Objects(output / 'objects')
    deployment = read_json(runtime / 'config/deployment.json')
    rollup = deployment['rollup'].lower()
    l1_info = l1.call('eth_getBlockByNumber', ['latest', False])
    l1_hash = l1_info['hash']
    l1_header = raw(l1.call('debug_getRawHeader', [l1_hash]))
    expect(keccak256(l1_header) == raw(l1_hash, 32), 'L1 raw header preimage mismatch')
    latest = l1.call('eth_getStorageAt', [rollup, hex32(116), l1_info['number']])
    base = int.from_bytes(keccak256(raw(latest, 32) + (117).to_bytes(32, 'big')), 'big')
    assertion_word = l1.call('eth_getStorageAt', [rollup, hex32(base), l1_info['number']])
    packed = int.from_bytes(raw(assertion_word, 32), 'big')
    created = (packed >> (16 * 8)) & (2 ** 64 - 1)
    expect((packed >> (25 * 8)) & 255 == 2 and deployment['deployed-at'] <= created <= int(l1_info['number'], 16),
           'latest assertion is not confirmed')
    logs = l1.call('eth_getLogs', [{'address': rollup, 'fromBlock': hex(created), 'toBlock': hex(created),
                                   'topics': [policy['assertion_event_topic'], latest]}])
    expect(len(logs) == 1 and logs[0].get('removed') is False and len(logs[0]['topics']) == 3, 'exact assertion creation unavailable')
    event = logs[0]; data = raw(event['data']); expect(len(data) == 800, 'assertion ABI fixed layout changed')
    words = [data[i:i + 32] for i in range(0, len(data), 32)]
    a = Assertion(raw(event['topics'][2], 32), words[13], words[14], int.from_bytes(words[15], 'big'),
                  int.from_bytes(words[16], 'big'), int.from_bytes(words[17], 'big'), words[18], words[19])
    expect(keccak256(a.parent_assertion + keccak256(b''.join(words[13:19])) + a.inbox_accumulator) == raw(latest, 32),
           'assertion event does not open latestConfirmed')
    head_hash = '0x' + a.block_hash.hex()
    settled, settled_record, settled_info = block(l2, head_hash, objects)
    settled_height = int(settled_info['number'], 16)
    expect(settled_height >= to_height, 'requested history not yet contract-settled')
    validated = validator.call('arb_latestValidated')
    expect(validated is not None and MODULE_ROOT in validated.get('WasmRoots', []), 'actual WASM validation not yet available')
    validated_info = l2.call('eth_getBlockByHash', [validated['GlobalState']['BlockHash'], False])
    expect(validated_info is not None and int(validated_info['number'], 16) >= settled_height, 'WASM validator has not covered settled head')
    canonical_validation = l2.call('eth_getBlockByNumber', [validated_info['number'], False])
    expect(canonical_validation['hash'] == validated_info['hash'], 'validated result is not canonical')
    accounts_l1, records_l1, graph, aliases = deployment_graph(l1, rollup, deployment, l1_info['number'], policy,
                                                             objects, l1_hash, (hex32(116), hex32(base), hex32(base + 1)))
    policy.update({'format': 'b3-owned-nitro-deployment-policy/1', 'contracts_source_commit': av['upstream']['contracts']['commit'],
                   'nitro_module_root': MODULE_ROOT, 'rollup': rollup, 'deployment': deployment, 'code_accounts': graph,
                   'account_role_aliases': aliases,
                   'latest_confirmed_slot': hex32(116), 'assertions_mapping_slot': hex32(117),
                   'assertion_node_slot': hex32(base), 'primary_implementation_slot': PRIMARY, 'secondary_implementation_slot': SECONDARY,
                   'native_policy_accepted': False, 'mainnet_enabled': False})
    confirmed, historical, confirmed_records, historical_records = [], [], [], []
    for request in accounts:
        w, rec = witness(l2, request['address'], request['storage_keys'], to_hash, objects)
        historical.append(w); historical_records.append(rec)
    pages, cursor_hash, cursor_height = [], settled_info['parentHash'], settled_height - 1
    while cursor_height >= to_height:
        page = []
        for _ in range(min(128, cursor_height - to_height + 1)):
            _, rec, info = block(l2, cursor_hash, objects)
            page.append(rec); cursor_hash = info['parentHash']; cursor_height -= 1
        expect(len(pages) < 64, 'ancestor traversal exceeds bounded 8192-header export')
        pages.append(page)
    expect((pages[-1][-1]['hash'] if pages else head_hash).lower() == to_hash.lower(), 'requested endpoint is not settled ancestor')
    cursor_hash = to_hash
    blocks, block_records = [], []
    for height in range(to_height, previous_height, -1):
        bw, rec, info = block(l2, cursor_hash, objects, complete=True)
        expect(int(info['number'], 16) == height, 'range block height changed')
        blocks.append(bw); block_records.append(rec); cursor_hash = info['parentHash']
    expect(cursor_hash.lower() == previous_hash.lower(), 'requested range predecessor changed')
    settlement_sha = write_raw(output / 'settlement-proof.bin', encode_proof(Proof(a, tuple(accounts_l1[:16]), (settled,), tuple(confirmed))))
    second_sha = None
    if len(accounts_l1) > 16:
        second_sha = write_raw(output / 'settlement-group-1.bin', encode_proof(Proof(a, tuple(accounts_l1[16:]), (settled,))))
    # Standalone complete history deliberately has a neutral assertion. Its
    # confirmed_accounts field means the NEWEST selected block, not the newer
    # contract-settled head; historical_accounts would mean the OLDEST block.
    history_sha = write_raw(output / 'history-proof.bin', encode_proof(Proof(Assertion(), (), tuple(blocks), tuple(historical), ())))
    l1_header_sha = write_raw(output / 'l1-header.rlp', l1_header)
    l2_header_sha = write_raw(output / 'l2-settled-header.rlp', settled.header)
    write_json(output / 'deployment-policy.json', policy)
    write_json(output / 'ancestor-pages.json', {'format': 'b3-owned-nitro-ancestor-pages/1', 'pages': pages})
    expect(l1.call('eth_getBlockByNumber', [l1_info['number'], False])['hash'] == l1_hash, 'L1 canonical source changed during export')
    expect(l2.call('eth_getBlockByNumber', [hex(to_height), False])['hash'].lower() == to_hash.lower(), 'L2 source changed during export')
    manifest = {'format': 'b3-owned-nitro-settled-export/1', 'source': {'kind': 'owned-regtest', 'chain_id': 412346,
                 'genesis_hash': config['l2_genesis']}, 'l1': {'chain_id': 1337, 'genesis_hash': config['l1_genesis'],
                 'height': int(l1_info['number'], 16), 'hash': l1_hash, 'header_sha256': l1_header_sha},
                 'request': {'previous_height': previous_height, 'previous_hash': previous_hash, 'to_height': to_height, 'to_hash': to_hash},
                 'settled_head': settled_record, 'assertion_hash': latest, 'assertion_event': event,
                 'l1_accounts': records_l1, 'confirmed_accounts': confirmed_records, 'historical_accounts': historical_records,
                 'history_blocks': block_records, 'settlement_proof_sha256': settlement_sha, 'history_proof_sha256': history_sha,
                 'settlement_group_1_sha256': second_sha, 'l1_account_group_sizes': [len(accounts_l1[:16]), len(accounts_l1[16:])],
                 'l2_settled_header_sha256': l2_header_sha, 'ancestor_page_count': len(pages),
                 'observed_validator_result': validated, 'raw_object_bytes': objects.total, 'raw_object_count': objects.count,
                 'observed_validator_feed_input': read_json(runtime / 'config/validator_config.json')['node']['feed']['input']['url'],
                 'availability_transport': 'owned-loopback-node-apis', 'external_rpc_used': False,
                 'native_verified': False, 'l1_settlement_verified': False, 'ethereum_pos_verified': False,
                 'source_authority': False, 'mainnet_enabled': False}
    write_json(output / 'manifest.json', manifest)
    return manifest


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--runtime', type=Path, required=True); p.add_argument('--output', type=Path, required=True)
    p.add_argument('--previous-height', type=int, required=True); p.add_argument('--previous-hash', required=True)
    p.add_argument('--to-height', type=int, required=True); p.add_argument('--to-hash', required=True)
    p.add_argument('--accounts', type=Path)
    a = p.parse_args(); accounts = read_json(a.accounts) if a.accounts else []
    print(json.dumps(export_settled(a.runtime, a.output, previous_height=a.previous_height, previous_hash=a.previous_hash,
                                  to_height=a.to_height, to_hash=a.to_hash, accounts=accounts), sort_keys=True))


if __name__ == '__main__':
    main()
