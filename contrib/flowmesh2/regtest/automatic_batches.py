#!/usr/bin/env python3
"""Durable signed-client queue and automatic one-parent batch scheduling.

All actions pass native admission before staging. The native pool chooses the
bounded fair batch, and the ordinary authenticated PBFT path owns every vote.
Source polling proposes actual deposits/payouts without an admin seal request.
"""
from collections import OrderedDict
import json
import time

from custody import CustodyError, atomic_json
from local_process import raw_hex


class AutomaticBatches:
    CAPACITY = 4096
    BYTE_CAP = 12 * 1024 * 1024
    SERIALIZED_CAP = BYTE_CAP * 2 + CAPACITY * 512
    RETRYABLE = {'BUDGET', 'BUSY', 'POOL_FULL', 'NONCE_PENDING'}
    TERMINAL = {'EXPIRED', 'NONCE_TOO_LOW', 'NONCE_USED'}
    COALESCE_SECONDS = .002

    def __init__(self, runtime):
        self.runtime, self.operator = runtime, runtime.operator
        self.enabled = self.operator.config.get('automatic_batches', True)
        if type(self.enabled) is not bool:
            raise CustodyError('automatic batch policy must be an explicit boolean')
        self.path = self.operator.directory / 'pending-client-actions.json'
        self.identity_path = self.operator.directory / 'pending-client-queue-identity.json'
        identity = self.operator.worker.request('info')
        self.identity = {'format': 'fm2-pending-client-queue/1', 'domain': identity['domain'], 'seat': identity['seat']}
        if self.identity_path.exists() and not self.path.exists():
            raise CustodyError('original durable pending-client pointer is missing; refusing a fresh queue')
        if self.identity_path.exists():
            self.operator.handovers.write_once(self.identity_path, self.identity)
        self.pending = OrderedDict()
        self.deadline = None
        self.last_source_poll = 0
        self.last_gossip = 0
        self.last_refresh = 0
        self.refresh_cursor = 0
        self.stats = {'public_staged': 0, 'peer_staged': 0, 'automatic_batches': 0, 'automatic_source_batches': 0}
        if self.path.exists():
            encoded = self.path.read_bytes()
            if len(encoded) > self.SERIALIZED_CAP:
                raise CustodyError('original pending client queue exceeds its bound')
            value = json.loads(encoded)
            if set(value) != {'format', 'actions'} or value['format'] != 'fm2-pending-client-actions/1' or len(value['actions']) > self.CAPACITY:
                raise CustodyError('original pending client queue has an unknown format')
            for action in value['actions']:
                if set(action) != {'id', 'account', 'frame'} or action['id'] in self.pending:
                    raise CustodyError('original client queue has duplicate or changed identity')
                reply = self.operator.worker.request('submit', frame=action['frame'])
                if self._retained(action, reply):
                    self.pending[action['id']] = action
            if sum(len(item['frame']) // 2 for item in self.pending.values()) > self.BYTE_CAP:
                raise CustodyError('original pending signed-client bytes exceed their bound')
            self._save()

    def _retained(self, action, reply):
        if ((reply.get('id') is not None and raw_hex(reply['id']) != raw_hex(action['id'])) or
                (reply.get('account') is not None and raw_hex(reply['account']) != raw_hex(action['account']))):
            raise CustodyError('saved client action differs from its native authenticated identity')
        if reply['code'] == 'ADMITTED' or reply['code'] == 'DUPLICATE' and reply.get('pending'):
            return True
        if reply['code'] in self.RETRYABLE:
            return True  # A transient local admission budget cannot erase an acknowledged action.
        if reply['code'] in self.TERMINAL or reply['code'] == 'DUPLICATE' and reply.get('included'):
            return False
        raise CustodyError('previously admitted durable client action no longer authenticates: ' + reply['code'])

    def _refresh(self):
        keys = list(self.pending)
        if not keys:
            return
        changed = False
        start = self.refresh_cursor % len(keys)
        chosen = (keys[start:] + keys[:start])[:64]
        self.refresh_cursor = (start + len(chosen)) % len(keys)
        for identity in chosen:
            action = self.pending[identity]
            result = self.operator.worker.request('submit', frame=action['frame'])
            if not self._retained(action, result):
                del self.pending[identity]
                changed = True
        if changed:
            self._save()
        if not self.pending:
            self.deadline = None

    def status(self):
        return {**self.stats, 'enabled': self.enabled, 'durable_pending': len(self.pending)}

    def _save(self):
        atomic_json(self.path, {'format': 'fm2-pending-client-actions/1', 'actions': list(self.pending.values())})
        self.operator.handovers.write_once(self.identity_path, self.identity)

    def enqueue(self, frame, reply, public=False):
        if reply['code'] != 'ADMITTED' and not (reply['code'] == 'DUPLICATE' and reply.get('pending')):
            return
        identity, account = raw_hex(reply['id']), raw_hex(reply['account'])
        action = {'id': identity, 'account': account, 'frame': frame}
        previous = self.pending.get(identity)
        if previous is not None:
            if previous['account'] != account:
                raise CustodyError('duplicate signed action changed its native account')
            return
        size = sum(len(item['frame']) // 2 for item in self.pending.values()) + len(frame) // 2
        if len(self.pending) >= self.CAPACITY or size > self.BYTE_CAP:
            raise CustodyError('bounded signed-client queue is full; retry the exact signed action unchanged')
        self.pending[identity] = action
        self._save()  # Durability precedes the public ADMITTED response.
        self.stats['public_staged' if public else 'peer_staged'] += 1
        if self.deadline is None:
            self.deadline = time.monotonic() + self.COALESCE_SECONDS

    def on_application(self, value):
        if value.get('kind') != 'client_actions':
            return False
        frames = value.get('actions')
        if not isinstance(frames, list) or len(frames) > 64:
            raise CustodyError('peer signed-client batch exceeds its bound')
        for frame in frames:
            reply = self.operator.worker.request('submit', frame=frame)
            self.enqueue(frame, reply)
        return True

    def on_applied(self, reply):
        if not reply.get('applied') or not self.pending:
            return
        self._refresh()

    def _gossip(self):
        frames, accounts = [], {}
        for action in self.pending.values():
            account = action['account']
            if accounts.get(account, 0) >= 8:
                continue
            accounts[account] = accounts.get(account, 0) + 1
            frames.append(action['frame'])
            if len(frames) >= 64:
                break
        if frames:
            self.runtime._broadcast({'kind': 'client_actions', 'actions': frames})

    def step(self, now, info):
        if not self.enabled:
            return
        if self.pending and now - self.last_refresh >= .100:
            self._refresh()
            self.last_refresh = now
        if self.pending and now - self.last_gossip >= .100:
            self._gossip()
            self.last_gossip = now
        pending = self.operator.pending
        source_locked = (info['observation_frozen'] or self.operator.pending_frozen) and pending is not None and raw_hex(pending['parent_head_hash']) == raw_hex(info['head_hash'])
        if info['changing_view'] or source_locked:
            return
        if info['pending_actions']:
            if self.deadline is None:
                self.deadline = now + self.COALESCE_SECONDS
            if now >= self.deadline:
                self.runtime.seal(None)
                self.stats['automatic_batches'] += 1
            return
        if now - self.last_source_poll >= .250:
            self.last_source_poll = now
            events, records, source_range = self.operator.source.scan(None, None)
            if events:
                self.runtime.seal(None, evm_end=source_range['to'])
                self.stats['automatic_source_batches'] += 1
