#!/usr/bin/env python3
"""Complete synthetic-EVM ranges, durably bound to local adapter slot heads.

This adapter checkpoint is not a B3 on-chain checkpoint or production finality
proof. The pinned unforked local RPC supplies canonical blocks and logs.
"""
import hashlib
import json
import os
import re
import struct
from functools import lru_cache
from pathlib import Path

from custody import CustodyError, address, atomic_json, hex_value, uint
from local_process import raw_hex


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'))


@lru_cache(maxsize=1)
def hash_domains():
    catalog = json.loads((Path(__file__).resolve().parents[3] /
                          'src/flowmesh2/core/HASH_DOMAINS.json').read_text(encoding='utf-8'))
    if catalog.get('format') != 'fm2-hash-domains/1':
        raise CustodyError('unknown core hash-domain catalog')
    rows = catalog['tags']
    if any(row['id'] != i + 1 or row['text'] != catalog['prefix'] + row['name']
           for i, row in enumerate(rows)) or len({row['name'] for row in rows}) != len(rows):
        raise CustodyError('invalid core hash-domain catalog')
    return {row['name']: row['text'] for row in rows}


def tagged(name, value):
    try:
        text = hash_domains()[name]
    except KeyError as error:
        raise CustodyError('unregistered core hash domain') from error
    tag = hashlib.sha256(text.encode()).digest()
    return hashlib.sha256(tag + tag + value).digest()


def fact_root(facts):
    nodes = [tagged('MERKLE_LEAF', tagged('FACT', bytes.fromhex(value))) for value in facts]
    if not nodes:
        tree = tagged('MERKLE_EMPTY', b'')
    else:
        while len(nodes) > 1:
            nodes = [tagged('MERKLE_NODE', nodes[i] + nodes[i + 1]) if i + 1 < len(nodes) else nodes[i]
                     for i in range(0, len(nodes), 2)]
        tree = nodes[0]
    return tagged('MERKLE_ROOT', struct.pack('<Q', len(facts)) + tree).hex()


def header_binding(encoded):
    value = bytes.fromhex(raw_hex(encoded, 366))
    return {'height': struct.unpack_from('<Q', value, 73)[0], 'parent': value[81:113].hex(),
            'anchor': {'height': struct.unpack_from('<I', value, 145)[0], 'hash': value[149:181].hex()},
            'facts_from': {'height': struct.unpack_from('<I', value, 181)[0], 'index': struct.unpack_from('<I', value, 185)[0]},
            'facts_to': {'height': struct.unpack_from('<I', value, 189)[0], 'index': struct.unpack_from('<I', value, 193)[0]},
            'facts_root': value[229:261].hex(), 'hash': tagged('SLOT', value).hex()}


def encode_source_facts(events, records, start, domain, source_range, range_commitment):
    # Pure encoding lets certified-history metadata be fully compared with this
    # node's own receipt scan before any native authority or source pointer moves.
    cursor = dict(start)
    facts = []
    for event, record in zip(events, records):
        prefix = struct.pack('<II', uint(cursor['height'], 32), uint(cursor['index'], 32))
        outpoint = bytes.fromhex(raw_hex(record['transaction_hash'])) + struct.pack('<I', uint(record['log_index'], 32))
        amount = struct.pack('<q', uint(record['amount_raw'], 63))
        account, asset = bytes.fromhex(raw_hex(record['account'])), bytes.fromhex(raw_hex(record['asset']))
        if event['kind'] == 'deposit':
            fact = b'\x01' + prefix + outpoint + account + asset + amount
        else:
            fact = (b'\x05' + prefix + bytes.fromhex(raw_hex(record['withdrawal_id'])) + account + asset + amount +
                    bytes.fromhex(raw_hex(record['recipient'], 20)) + outpoint + bytes.fromhex(raw_hex(domain)))
        facts.append(fact.hex())
        cursor['index'] = uint(cursor['index'] + 1, 32)
    if range_commitment:
        from source_checkpoint import encode_source_range
        facts.append(encode_source_range(cursor['height'], cursor['index'], domain, source_range).hex())
        cursor['index'] = uint(cursor['index'] + 1, 32)
    return facts, cursor


class SourceStream:
    def __init__(self, operator, manifest, write_once):
        self.owner, self.write_once = operator, write_once
        self.range_commitment = manifest.get('profile_name') == 'fm2-regtest-stable-q/3'
        self.rpc = operator.routes['Q'].rpc
        self.root = operator.directory / 'source-checkpoints'
        self.genesis_path = operator.directory / 'source-genesis.json'
        self.manifest_point = self.point(manifest['evm_initial_checkpoint'])
        self.records, self.index_tip = self.load_index()
        self.pending = None
        self.committed = None
        self.head_hash = None
        self.slot_height = None

    @staticmethod
    def point(value):
        if (not isinstance(value, dict) or set(value) != {'height', 'hash', 'minimum_confirmations'} or
                value['minimum_confirmations'] != 2):
            raise CustodyError('source checkpoint differs from the pinned two-confirmation regtest policy')
        return {'height': uint(value['height'], 64), 'hash': hex_value(value['hash'], 32), 'minimum_confirmations': 2}

    def check_point(self, point):
        from custody_policy import PROFILE, verify_source_point
        route = self.owner.routes['Q']
        if route.custody_profile == PROFILE:
            verify_source_point(route, point)
            return
        block = self.rpc.call('eth_getBlockByNumber', [hex(point['height']), False])
        if not isinstance(block, dict) or hex_value(block['hash'], 32) != point['hash']:
            raise CustodyError('retained source range checkpoint removed or replaced; refusing new signing')

    def guard(self, history_tip):
        selected = self.pending if self.pending is not None else self.committed
        if selected is None or selected['history_tip'] != history_tip:
            return False
        from custody_policy import PROFILE, verify_custody_routes
        production = [route for route in self.owner.routes.values() if route.custody_profile == PROFILE]
        point = selected['source']['to']
        if production:
            if len(production) == len(self.owner.routes):
                verify_custody_routes(production, source_point=point)
                return True
            verify_custody_routes(production)
        identity = self.rpc.assert_fixture()
        self.check_point(point)
        if int(identity['currentBlockNumber'], 16) < point['height'] + 1:
            raise CustodyError('source range awaiting at least two local confirmations')
        return True

    def load_index(self):
        path = self.root / 'index.jsonl'
        encoded = path.read_bytes() if path.exists() else b''
        if len(encoded) > 16_777_216 or (encoded and not encoded.endswith(b'\n')):
            raise CustodyError('source checkpoint index is oversized or torn')
        tip, values, seen = '00' * 32, [], set()
        for line in encoded.splitlines():
            entry = json.loads(line)
            digest = entry.pop('hash')
            if (set(entry) != {'file', 'sha256', 'previous'} or entry['previous'] != tip or
                    hashlib.sha256(canonical(entry).encode()).hexdigest() != digest or
                    re.fullmatch(r'[0-9]{20}-[0-9a-f]{64}\.json', entry['file']) is None or entry['file'] in seen):
                raise CustodyError('source checkpoint index is corrupt or conflicting')
            file = self.root / entry['file']
            if not file.exists() or hashlib.sha256(file.read_bytes()).hexdigest() != entry['sha256']:
                raise CustodyError('source checkpoint missing or changed')
            value = json.loads(file.read_text(encoding='utf-8'))
            if value.get('format') != 'fm2-regtest-source-commit/1':
                raise CustodyError('unknown source checkpoint format')
            if entry['file'] != str(value['slot_height']).zfill(20) + '-' + raw_hex(value['head_hash']) + '.json':
                raise CustodyError('source checkpoint filename differs from authenticated slot')
            values.append(value)
            seen.add(entry['file'])
            tip = digest
        unindexed = ({file.name for file in self.root.glob('*.json')} - seen) if self.root.exists() else set()
        if len(unindexed) > 1 or any(re.fullmatch(r'[0-9]{20}-[0-9a-f]{64}\.json', name) is None for name in unindexed):
            raise CustodyError('ambiguous/unrecognized unindexed source checkpoints refuse recovery')
        # One exact next APPLIED checkpoint can precede its index append during
        # a crash. Do not adopt it here: recover() must first bind it to the
        # authenticated worker head and original pre-signing observation.
        self.unindexed = next(iter(unindexed), None)
        return values, tip

    def verify_binding(self, saved, header, head_hash):
        binding = header_binding(header)
        if (binding['hash'] != raw_hex(head_hash) or binding['parent'] != raw_hex(saved['parent_head_hash']) or
                binding['anchor'] != {'height': uint(saved['anchor']['height'], 32), 'hash': raw_hex(saved['anchor']['hash'])} or
                binding['facts_from'] != saved['facts_from'] or binding['facts_to'] != saved['facts_to'] or
                binding['facts_root'] != fact_root(saved['facts'])):
            raise CustodyError('source checkpoint does not bind this exact committed header/fact range')
        source = saved['source']
        previous, end = self.point(source['previous']), self.point(source['to'])
        if (source.get('complete') is not True or source['from_height'] != previous['height'] + 1 or
                end['height'] < previous['height'] or end['height'] - previous['height'] > 4096 or
                source['empty_range'] != (end['height'] == previous['height']) or
                (end['height'] == previous['height'] and end != previous) or
                len(saved['facts']) != len(saved['events']) + int(self.range_commitment) or len(saved['events']) != len(saved['records'])):
            raise CustodyError('source checkpoint contains a noncontiguous/incomplete event range')
        if self.range_commitment:
            from source_checkpoint import verify_source_range
            raw_header = bytes.fromhex(raw_hex(header, 366))
            verify_source_range(bytes.fromhex(saved['facts'][-1]), saved['facts_from']['height'],
                                uint(saved['facts_from']['index'] + len(saved['events']), 32),
                                raw_header[1:33].hex(), source)
        return binding

    def recover(self, info):
        if not self.genesis_path.exists():
            if info['height'] != 0 or self.records or getattr(self, 'unindexed', None) is not None:
                raise CustodyError('existing worker lacks its original source genesis checkpoint')
            self.write_once(self.genesis_path, {'format': 'fm2-regtest-source-genesis/1', 'head_hash': info['head_hash'],
                            'header': info['header'], 'source': self.manifest_point})
        genesis = json.loads(self.genesis_path.read_text(encoding='utf-8'))
        if (genesis.get('format') != 'fm2-regtest-source-genesis/1' or genesis['source'] != self.manifest_point or
                header_binding(genesis['header'])['height'] != 0 or header_binding(genesis['header'])['hash'] != raw_hex(genesis['head_hash'])):
            raise CustodyError('source genesis checkpoint differs from pinned deployment')
        cursor = header_binding(genesis['header'])['facts_to']
        self.head_hash, self.slot_height = raw_hex(genesis['head_hash']), 0
        self.committed = {'history_tip': '00' * 32, 'source': {'to': self.manifest_point}, 'facts_to': cursor}
        for value in self.records:
            saved = value['observation']
            binding = self.verify_binding(saved, value['header'], value['head_hash'])
            if (binding['height'] != self.slot_height + 1 or binding['parent'] != self.head_hash or
                    saved['facts_from'] != cursor or saved['source']['previous'] != self.committed['source']['to'] or
                    saved['history_tip'] not in self.owner.retained_tips):
                raise CustodyError('source checkpoint chain has a gap, changed parent or unknown receipt history')
            self.head_hash, self.slot_height, cursor = binding['hash'], binding['height'], binding['facts_to']
            self.committed = saved
        pending_path = self.owner.directory / 'pending-observation.json'
        saved = json.loads(pending_path.read_text(encoding='utf-8')) if pending_path.exists() else None
        if saved is None and any(raw_hex(json.loads(file.read_text(encoding='utf-8'))['parent_head_hash']) == raw_hex(info['head_hash'])
                                 for file in (self.owner.directory / 'source-observations').glob('*.json')):
            raise CustodyError('original pending source pointer missing; refusing to choose another prepared observation')
        if saved is not None:
            self.validate_observation_file(saved)
        unindexed = getattr(self, 'unindexed', None)
        if unindexed is not None:
            binding = header_binding(info['header'])
            expected = {'format': 'fm2-regtest-source-commit/1', 'slot_height': binding['height'],
                        'head_hash': raw_hex(info['head_hash']), 'header': info['header'],
                        'certificate': info['certificate'], 'observation': saved}
            expected_name = str(binding['height']).zfill(20) + '-' + raw_hex(info['head_hash']) + '.json'
            if (saved is None or not info['certificate'] or binding['height'] != self.slot_height + 1 or
                    raw_hex(info['head_hash']) == self.head_hash or unindexed != expected_name or
                    json.loads((self.root / unindexed).read_text(encoding='utf-8')) != expected):
                raise CustodyError('unindexed checkpoint is not the exact authenticated APPLIED successor/original observation')
        if raw_hex(info['head_hash']) != self.head_hash:
            if saved is None:
                raise CustodyError('worker applied a slot without its pre-signing source checkpoint')
            self.pending = saved
            self.applied(info['header'], info['head_hash'], info['certificate'])
        self.check_point(self.committed['source']['to'])
        if saved is not None:
            if raw_hex(saved['parent_head_hash']) == raw_hex(info['head_hash']):
                self.pending = saved
                self.owner._observe(saved['anchor'], saved['events'], expected=saved, evm_end=saved['source']['to'])
            elif self.records and self.records[-1]['observation'] == saved:
                self.owner.pending = saved  # Retain consumed context for harmless duplicate certified messages.
            else:
                raise CustodyError('pending source checkpoint does not match the recovered worker head')

    def validate_observation_file(self, saved):
        digest = hashlib.sha256(canonical(saved).encode()).hexdigest()
        file = self.owner.directory / 'source-observations' / (digest + '.json')
        if not file.exists() or json.loads(file.read_text(encoding='utf-8')) != saved:
            raise CustodyError('pre-signing source observation missing or changed')

    def save_pending(self, saved, before_publish=None):
        self.check_point(saved['source']['previous'])
        self.check_point(saved['source']['to'])
        if (raw_hex(saved['parent_head_hash']) != self.head_hash or saved['facts_from'] != self.committed['facts_to'] or
                saved['source']['previous'] != self.committed['source']['to'] or saved['history_tip'] != self.owner.history_tip):
            raise CustodyError('new source observation skips its exact committed parent/range/history')
        digest = hashlib.sha256(canonical(saved).encode()).hexdigest()
        self.write_once(self.owner.directory / 'source-observations' / (digest + '.json'), saved)
        if before_publish is not None:
            before_publish()
        atomic_json(self.owner.directory / 'pending-observation.json', saved)
        self.pending = saved

    def applied(self, header, head_hash, certificate):
        if raw_hex(head_hash) == self.head_hash:
            return
        if self.pending is None or not certificate:
            raise CustodyError('applied slot lacks its durable complete-source observation/certificate')
        saved = self.pending
        binding = self.verify_binding(saved, header, head_hash)
        if (binding['parent'] != self.head_hash or binding['height'] != self.slot_height + 1 or
                saved['facts_from'] != self.committed['facts_to'] or
                saved['source']['previous'] != self.committed['source']['to'] or
                saved['history_tip'] not in self.owner.retained_tips):
            raise CustodyError('source application skips the exact previous checkpoint head')
        self.check_point(saved['source']['to'])
        value = {'format': 'fm2-regtest-source-commit/1', 'slot_height': binding['height'], 'head_hash': raw_hex(head_hash),
                 'header': header, 'certificate': certificate, 'observation': saved}
        name = str(binding['height']).zfill(20) + '-' + raw_hex(head_hash) + '.json'
        self.write_once(self.root / name, value)
        entry = {'file': name, 'sha256': hashlib.sha256((self.root / name).read_bytes()).hexdigest(), 'previous': self.index_tip}
        digest = hashlib.sha256(canonical(entry).encode()).hexdigest()
        entry['hash'] = digest
        with (self.root / 'index.jsonl').open('ab') as stream:
            stream.write(canonical(entry).encode() + b'\n')
            stream.flush()
            os.fsync(stream.fileno())
        if hasattr(os, 'O_DIRECTORY'):
            fd = os.open(self.root, os.O_RDONLY | os.O_DIRECTORY)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        self.records.append(value)
        self.unindexed = None
        self.index_tip, self.head_hash, self.slot_height = digest, binding['hash'], binding['height']
        self.committed, self.pending = saved, None

    def finish(self, reply):
        try:
            for decision in reply.get('applied', []):
                self.applied(decision['header'], decision['head_hash'], decision['certificate'])
                self.owner.pending_frozen = False
        except Exception:
            # The native APPLIED receipt already exists. No later request may
            # sign through an ambiguous adapter index ACK in this process.
            self.owner.opened = False
            raise
        return reply

    def normalize_reference(self, event, discovered):
        result = {'kind': event['kind'], 'route': event['route'], 'transaction_hash': hex_value(event['transaction_hash'], 32)}
        matches = [value for value in discovered if all(value[key] == result[key] for key in result)]
        if 'log_index' in event:
            result['log_index'] = uint(event['log_index'], 32)
        elif len(matches) == 1:
            result['log_index'] = matches[0]['log_index']
        else:
            raise CustodyError('event reference needs its exact unambiguous log index')
        if event['kind'] == 'deposit':
            result.update({'account': '0x' + raw_hex(event['account']), 'amount_raw': str(uint(event['amount_raw'], 63)),
                           'sender': address(event['sender'])})
        elif event['kind'] == 'paid':
            result['withdrawal_id'] = '0x' + raw_hex(event['withdrawal_id'])
        else:
            raise CustodyError('event reference has an unsupported kind')
        return result

    def scan(self, end, expected_events):
        self.owner._verify_retained()
        previous = self.committed['source']['to']
        self.check_point(previous)
        identity = self.rpc.assert_fixture()
        head = int(identity['currentBlockNumber'], 16)
        height = head - 1 if end is None else uint(end['height'], 64)
        if not previous['height'] <= height <= head - 1 or height - previous['height'] > 4096:
            raise CustodyError('source event range skips confirmations, regresses or exceeds the local bound')
        block = self.rpc.call('eth_getBlockByNumber', [hex(height), False])
        point = {'height': height, 'hash': hex_value(block['hash'], 32), 'minimum_confirmations': 2}
        if end is not None and point != self.point(end):
            raise CustodyError('declared source end block differs from this operator\'s canonical chain')
        deposit_topic = self.owner.cast.keccak('Deposit(uint64,bytes32,bytes32,uint256,address)')
        paid_topic = self.owner.cast.keccak('Withdrawn(bytes32,address,uint256)')
        routes = {route.vault: name for name, route in self.owner.routes.items()}
        logs = []
        if height > previous['height']:
            logs = self.rpc.call('eth_getLogs', [{'fromBlock': hex(previous['height'] + 1), 'toBlock': hex(height),
                                                 'address': sorted(routes), 'topics': [[deposit_topic, paid_topic]]}])
        if not isinstance(logs, list) or len(logs) > 128:
            raise CustodyError('source logs exceed the bounded complete local event batch')
        logs.sort(key=lambda value: (int(value['blockNumber'], 16), int(value['transactionIndex'], 16), int(value['logIndex'], 16)))
        events, records, seen = [], [], set()
        for log in logs:
            route_name = routes.get(address(log['address']))
            block_height, index = int(log['blockNumber'], 16), int(log['logIndex'], 16)
            key = (hex_value(log['transactionHash'], 32), index)
            if (route_name is None or log.get('removed') is not False or not previous['height'] < block_height <= height or key in seen):
                raise CustodyError('source log has a duplicate, wrong route or noncanonical range position')
            seen.add(key)
            route = self.owner.routes[route_name]
            receipt = route.canonical_receipt(key[0])
            if int(receipt['transactionIndex'], 16) != int(log['transactionIndex'], 16):
                raise CustodyError('source log transaction position differs from its canonical receipt')
            topic = hex_value(log['topics'][0], 32)
            if topic == deposit_topic:
                record = route.expected_deposit(receipt, log_index=index)
                event = {'kind': 'deposit', 'route': route_name, 'transaction_hash': key[0], 'log_index': index,
                         'account': record['account'], 'amount_raw': record['amount_raw'], 'sender': record['sender']}
            elif topic == paid_topic and route_name == 'Q':
                record = route.expected_paid(receipt, log_index=index)
                event = {'kind': 'paid', 'route': route_name, 'transaction_hash': key[0], 'log_index': index,
                         'withdrawal_id': record['withdrawal_id']}
                obligation = self.owner.worker.request('withdrawal', withdrawal_id=raw_hex(record['withdrawal_id']))
                if (not obligation.get('found') or obligation.get('verified') is not True or
                        uint(obligation['record']['amount']) != uint(record['amount_raw']) or
                        raw_hex(obligation['record']['recipient'], 20) != raw_hex(record['recipient'], 20)):
                    raise CustodyError('complete source payout lacks this operator\'s exact committed obligation')
                record['account'] = '0x' + raw_hex(obligation['record']['account'])
            else:
                raise CustodyError('complete source stream contains an unsupported payout/asset event')
            if record['block_number'] != block_height or record['block_hash'] != hex_value(log['blockHash'], 32):
                raise CustodyError('source log block differs from its independently checked full receipt')
            events.append(event)
            records.append(record)
        if expected_events is not None and [self.normalize_reference(value, events) for value in expected_events] != events:
            raise CustodyError('coordinator event references omit/change the independently scanned complete source range')
        self.check_point(previous)
        self.check_point(point)
        return events, records, {'previous': previous, 'from_height': previous['height'] + 1, 'to': point,
                                 'empty_range': height == previous['height'], 'complete': True,
                                 'qualification': 'adapter-local synthetic checkpoint; not a B3 on-chain proof'}
