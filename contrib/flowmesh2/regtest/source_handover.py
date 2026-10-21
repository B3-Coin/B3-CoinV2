#!/usr/bin/env python3
"""Durable source-observation handover authorized by a verified PBFT NEW_VIEW.

No OFFER, client hint, timeout or receipt can create this authority. The private
worker first validates the exact authenticated NEW_VIEW and its quorum reports.
Old observations remain immutable; a one-record interrupted pointer publication
is recovered only after the native proof and selected source have been checked.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import time

from custody import CustodyError, atomic_json, uint
from local_process import raw_hex
from source_stream import canonical, header_binding


class SourceHandover:
    LIMIT = 64 * 1024 * 1024

    def __init__(self, operator, write_once):
        self.operator, self.write_once = operator, write_once
        self.root = operator.directory / 'source-handovers'
        self.pointer = operator.directory / 'source-handover-pointer.json'
        self.tip = '00' * 32
        self.latest = None
        files = list(self.root.glob('*.json')) if self.root.exists() else []
        if len(files) > 1024 or sum(file.stat().st_size for file in files) > self.LIMIT:
            raise CustodyError('durable source-handover history exceeds its bound')
        values = {}
        for file in files:
            if file.is_symlink() or re.fullmatch(r'[0-9a-f]{64}\.json', file.name) is None:
                raise CustodyError('unknown source-handover history entry')
            value = json.loads(file.read_text(encoding='utf-8'))
            digest = hashlib.sha256(canonical(value).encode()).hexdigest()
            fields = {'format', 'previous', 'parent', 'view', 'message', 'header', 'old', 'new'}
            certified = value.get('format') == 'fm2-source-handover/2' and value.get('authority_kind') == 'certified-history'
            initial = (value.get('format') == 'fm2-source-handover/3' and value.get('initial') is True and
                       value.get('authority_kind') in ('signed-new-view', 'own-quorum'))
            if certified or initial:
                fields.add('authority_kind')
            if initial:
                fields.add('initial')
            if file.stem != digest or set(value) != fields:
                raise CustodyError('source-handover history changed or has unknown fields')
            if ((value['format'] != 'fm2-source-handover/1' and not certified and not initial) or raw_hex(value['parent']) != value['parent'] or
                    (uint(value['view'], 64) == 0 and not certified) or raw_hex(value['previous']) != value['previous']):
                raise CustodyError('unknown source-handover format or authority')
            binding = header_binding(value['header'])
            old = value['old']
            if (binding['parent'] != value['parent'] or raw_hex(value['new']['parent_head_hash']) != value['parent'] or
                    (not initial and (old is None or raw_hex(old['parent_head_hash']) != value['parent'])) or
                    (initial and old is not None and raw_hex(old['parent_head_hash']) == value['parent'])):
                raise CustodyError('source-handover changes its committed parent or original observation mode')
            values[digest] = value
        if self.pointer.exists():
            pointer = json.loads(self.pointer.read_text(encoding='utf-8'))
            if set(pointer) != {'format', 'sha256'} or pointer['format'] != 'fm2-source-handover-pointer/1' or pointer['sha256'] not in values:
                raise CustodyError('original source-handover pointer is missing or corrupt')
            self.tip = pointer['sha256']
        reached = set()
        cursor = self.tip
        while cursor != '00' * 32:
            if cursor in reached or cursor not in values:
                raise CustodyError('source-handover chain is cyclic or missing its immutable history')
            reached.add(cursor)
            previous = values[cursor]['previous']
            if previous != '00' * 32 and values.get(previous, {}).get('parent') == values[cursor]['parent']:
                prior, current = values[previous], values[cursor]
                if (prior.get('authority_kind') == 'certified-history' or
                        current.get('authority_kind') != 'certified-history' and prior['view'] >= current['view']):
                    raise CustodyError('source-handover authority is not monotone for its parent')
            cursor = previous
        extra = set(values) - reached
        if len(extra) > 1 or (extra and values[next(iter(extra))]['previous'] != self.tip):
            raise CustodyError('ambiguous unindexed source-handover history refuses automatic repair')
        if extra:
            self.tip = next(iter(extra))
        self.latest = values.get(self.tip)
        self.needs_publish = bool(extra)

    def open_authority(self):
        if self.latest is None:
            return None
        result = {'message': self.latest['message']}
        if self.latest.get('authority_kind') == 'certified-history':
            result['authority_kind'] = 'certified-history'
        return result

    def reconcile(self, info, source):
        if self.latest is None:
            return
        value = self.latest
        if value['old'] is not None:
            source.validate_observation_file(value['old'])
        source.verify_binding(value['new'], value['header'], header_binding(value['header'])['hash'])
        if value.get('initial') is True:
            # Native open has already revalidated this exact full quorum proof.
            # Initial authority precedes its first observation file so there is
            # never an ambiguous orphan before a missing original pointer.
            digest = hashlib.sha256(canonical(value['new']).encode()).hexdigest()
            self.write_once(self.operator.directory / 'source-observations' / (digest + '.json'), value['new'])
        source.validate_observation_file(value['new'])
        if raw_hex(info['head_hash']) == value['parent']:
            path = self.operator.directory / 'pending-observation.json'
            if not path.exists() and value['old'] is not None:
                raise CustodyError('source-handover lost the original pending observation pointer')
            pending = json.loads(path.read_text(encoding='utf-8')) if path.exists() else None
            if pending not in (value['old'], value['new']):
                raise CustodyError('pending source pointer is not either side of its exact durable handover')
            if pending == value['old']:
                atomic_json(path, value['new'])
        if self.needs_publish:
            atomic_json(self.pointer, {'format': 'fm2-source-handover-pointer/1', 'sha256': self.tip})
            self.needs_publish = False

    def record(self, old, new, authority, initial=False):
        parent = raw_hex(new['parent_head_hash'])
        if initial:
            pointer = self.operator.directory / 'pending-observation.json'
            actual = json.loads(pointer.read_text(encoding='utf-8')) if pointer.exists() else None
            if (actual != old or (old is not None and (raw_hex(old['parent_head_hash']) == parent or
                    self.operator.source.committed != old))):
                raise CustodyError('initial observation authority differs from the actual prior consumed pointer')
            if authority.get('authority_kind') not in ('signed-new-view', 'own-quorum'):
                raise CustodyError('initial observation requires exact verified new-view authority')
        elif old is None or raw_hex(old['parent_head_hash']) != parent:
            raise CustodyError('source-handover requires its original immutable pending observation')
        if old is not None:
            self.operator.source.validate_observation_file(old)
        if not initial:
            self.operator.source.validate_observation_file(new)
        header = authority['header']
        self.operator.source.verify_binding(new, header, header_binding(header)['hash'])
        view = uint(authority['view'], 64)
        parent = raw_hex(new['parent_head_hash'])
        certified = authority.get('authority_kind') == 'certified-history'
        if (self.latest is not None and self.latest['parent'] == parent and
                self.latest['view'] == view and (self.latest.get('authority_kind') == 'certified-history') == certified and
                self.latest.get('initial', False) == initial):
            if (self.latest['message'] == authority['message'] and self.latest['header'] == header and
                    self.latest['old'] == old and self.latest['new'] == new):
                return  # exact interrupted handover retry; immutable evidence already durable
            raise CustodyError('source-handover conflicts with its exact durable new-view authority')
        if self.latest is not None and self.latest['parent'] == parent and (
                self.latest.get('authority_kind') == 'certified-history' or not certified and self.latest['view'] > view):
            raise CustodyError('source-handover cannot roll back its verified committed or new-view authority')
        value = {'format': 'fm2-source-handover/1', 'previous': self.tip, 'parent': parent, 'view': view,
                 'message': authority['message'], 'header': header, 'old': old, 'new': new}
        if certified:
            value.update(format='fm2-source-handover/2', authority_kind='certified-history')
        elif initial:
            value.update(format='fm2-source-handover/3', authority_kind=authority['authority_kind'], initial=True)
        digest = hashlib.sha256(canonical(value).encode()).hexdigest()
        files = list(self.root.glob('*.json')) if self.root.exists() else []
        if len(files) >= 1024 or sum(path.stat().st_size for path in files) + len(canonical(value).encode()) > self.LIMIT:
            raise CustodyError('durable source-handover history exceeds its bound')
        self.write_once(self.root / (digest + '.json'), value)
        # This publication precedes replacing pending-observation.json. Recovery
        # can finish exactly this authority/observation pair after any crash.
        atomic_json(self.pointer, {'format': 'fm2-source-handover-pointer/1', 'sha256': digest})
        self.tip, self.latest = digest, value
        if self.operator.config.get('regtest_handover_barrier') is True:
            # Explicit local qualification fault only. No HTTPS command can arm
            # this barrier, and the runtime constructor hard-pins REGTEST /3.
            marker = self.operator.directory / 'regtest-handover-ready.json'
            runtime = getattr(self.operator, 'network', None)
            fault_stats = getattr(runtime, 'stats', {})
            new_view_fault = {'enabled': getattr(runtime, 'regtest_drop_new_views', False),
                             'received': fault_stats.get('new_view_frames', 0),
                             'dropped': fault_stats.get('regtest_dropped_new_views', 0)}
            self.write_once(marker, {'phase': 'authority-durable-before-pending-pointer', 'sha256': digest,
                                     'wrapper_pid': os.getpid(), 'worker_pid': self.operator.worker.process.pid,
                                     'regtest_new_view_fault': new_view_fault})
            deadline = time.monotonic() + 30
            while not (self.operator.directory / 'regtest-handover-release').exists():
                if time.monotonic() >= deadline:
                    raise CustodyError('bounded regtest handover fault barrier expired; state remains recoverable')
                time.sleep(.01)
