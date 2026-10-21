#!/usr/bin/env python3
"""One operator's authenticated peer loop; the client never routes peer frames.

The C++ worker owns encrypted sockets and immutable receive tokens. This module
owns independent source RPC checks and the durable source ACK. Its caller holds
the operator's single state lock across each operation, including client calls.
Only the synthetic REGTEST deployment profile can instantiate this runtime.
"""
from collections import OrderedDict, deque
import hashlib
import json
import threading
import time

from custody import CustodyError, uint
from local_process import raw_hex
from source_stream import header_binding, encode_source_facts


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'))


class NetworkRuntime:
    MAX_APP_BYTES = 0xfff000
    MAX_PENDING = 48

    def __init__(self, operator, manifest, write_once):
        if (manifest.get('chain_id') != 31337 or manifest.get('agreement') != 'pbft-view-change' or
                manifest.get('profile_name') != 'fm2-regtest-stable-q/3' or
                manifest.get('transport', {}).get('format') != 'fm2-seat-tcp/1'):
            raise CustodyError('authenticated runtime requires the exact pinned REGTEST deployment')
        self.regtest_drop_new_views = operator.config.get('regtest_drop_new_views', False)
        if type(self.regtest_drop_new_views) is not bool:
            raise CustodyError('regtest typed NEW_VIEW loss requires an explicit private boolean')
        self.operator, self.worker, self.write_once = operator, operator.worker, write_once
        self.genesis_head = raw_hex(json.loads(operator.source.genesis_path.read_text(encoding='utf-8'))['head_hash'])
        self.seat = uint(operator.worker.request('info')['seat'], 8)
        self.stop_event = threading.Event()
        self.thread = None
        self.active = False
        self.paused = False
        self.busy = False
        self.fenced = None
        self.frames = deque()
        self.outbox = OrderedDict()
        self.batches = {}
        self.candidates = OrderedDict()
        self.last_hello = 0
        self.last_guard = 0
        self.last_timer = 0
        self.last_request = {}
        self.sent_history = {}
        self.acknowledged = {}
        self.started_batches = set()
        self.last_peer_refusal = None
        self.stats = {'authenticated_frames': 0, 'agreement_frames': 0, 'history_applied': 0,
                      'history_acks': 0, 'history_duplicates': 0, 'source_handovers': 0, 'own_source_handovers': 0,
                      'peer_refusals': 0, 'timer_ticks': 0, 'new_view_frames': 0, 'regtest_dropped_new_views': 0}
        from automatic_batches import AutomaticBatches
        self.automatic = AutomaticBatches(self)
        from custody_runtime import AutomaticCustody
        self.custody = AutomaticCustody(operator, self._send)

    def status(self):
        return {**self.stats, 'active': self.active, 'paused': self.paused, 'fenced': self.fenced,
                'pending_frames': len(self.frames), 'pending_outbound': len(self.outbox),
                'last_peer_refusal': self.last_peer_refusal, 'regtest_drop_new_views': self.regtest_drop_new_views,
                'source_frozen': self.operator.pending_frozen,
                'source_end': self.operator.pending['source']['to'] if self.operator.pending is not None else None,
                'acknowledged': {str(seat): value for seat, value in self.acknowledged.items()},
                'automatic': self.automatic.status(), 'custody': self.custody.info(), 'mainnet_enabled': False}

    def start(self, signatures):
        if self.active:
            raise CustodyError('authenticated runtime is already active')
        result = self.worker.request('network_start', signatures=signatures)
        self.active = True
        self.thread = threading.Thread(target=self._run, name='fm2-authenticated-peer-loop', daemon=True)
        self.thread.start()
        return result

    def close(self):
        self.stop_event.set()
        if self.thread is not None and self.thread is not threading.current_thread():
            self.thread.join(timeout=15)
        self.custody.close()

    def _run(self):
        while not self.stop_event.is_set():
            try:
                with self.operator.state_lock:
                    self.step()
            except Exception as error:
                # No signing endpoint stays armed after ambiguous source ACK or
                # storage/process failure. Existing state remains for inspection.
                self.fenced = str(error)
                self.active = False
                return
            self.stop_event.wait(.001 if self.busy else .005)

    def _send(self, seat, value):
        encoded = canonical({'format': 'fm2-network-app/1', **value}).encode()
        if len(encoded) > self.MAX_APP_BYTES:
            raise CustodyError('bounded authenticated application output queue is full')
        # Application records are idempotent and reconstructible. One latest
        # record per peer/type avoids an offline peer accumulating old hellos,
        # batch hints or certified-history retries without bound.
        key = (seat, value['kind'])
        self.outbox[key] = encoded.hex()

    def _flush(self):
        for key, value in list(self.outbox.items()):
            seat = key[0]
            try:
                self.worker.request('network_send', to_seat=seat, message=value)
            except CustodyError as error:
                if 'BUSY' not in str(error):
                    raise
                continue
            self.outbox.pop(key)

    def _broadcast(self, value):
        for seat in range(4):
            if seat != self.seat:
                self._send(seat, value)

    def _drop(self, frame):
        self.worker.request('network_drop', token=frame['token'])

    def _consume_bulk(self, frame):
        self.worker.request('network_drop', token=frame['token'])  # passive BULK metadata never dispatches agreement

    def _request_history(self, seat, info):
        key = (seat, info['height'] + 1, info['head_hash'])
        now = time.monotonic()
        if now - self.last_request.get(key, 0) < .2:
            return
        self.last_request[key] = now
        self._send(seat, {'kind': 'history_request', 'height': info['height'] + 1,
                          'parent': info['head_hash']})

    def probe_history(self, peer, height):
        peer, height = uint(peer, 8), uint(height, 64)
        if peer >= 4 or peer == self.seat:
            raise CustodyError('history probe requires another pinned authenticated peer')
        retained = self.worker.request('network_history', height=height)
        if not retained['found']:
            raise CustodyError('historical replay probe must name an existing certified local slot')
        self._send(peer, {'kind': 'history_request', 'height': height,
                          'parent': header_binding(retained['header'])['parent']})
        self._flush()
        return {'requested': True, 'height': height}

    def _observation(self, value):
        if set(value) != {'parent', 'anchor', 'events', 'evm_end'}:
            raise CustodyError('authenticated source descriptor has unknown or missing fields')
        return {'parent': raw_hex(value['parent']), 'anchor': value['anchor'], 'events': value['events'],
                'evm_end': value['evm_end']}

    def _guard_pending(self):
        self.operator._validate_pending()
        self.last_guard = time.monotonic()

    def _guard_proposal(self, value):
        self.operator._validate_proposal(value)
        self.last_guard = time.monotonic()

    def _guard_retained(self):
        self.operator._verify_retained()
        self.last_guard = time.monotonic()

    def _authorize(self, descriptor, header=None, authority=None, info=None):
        descriptor = self._observation(descriptor)
        info = self.worker.request('info') if info is None else info
        if descriptor['parent'] != raw_hex(info['head_hash']):
            raise CustodyError('source descriptor is not the next contiguous committed parent')
        old = self.operator.pending
        if header is not None and old is not None and raw_hex(old['parent_head_hash']) == raw_hex(info['head_hash']):
            try:
                self.operator._check_proposal_binding({'header': header})
                if authority is not None:
                    # Even an unchanged source must complete the native NV
                    # capability's truthful view-entry/signing fence. No source
                    # file changes or new handover record are needed here.
                    self._guard_pending()
                    fields = {key: old[key] for key in ('anchor', 'facts_from', 'facts_to', 'facts')}
                    fields['observation_token'] = authority['token']
                    self.worker.request('check_observation', **fields)
                    self.worker.request('observe', **fields)
                return  # own complete source range already proves these exact facts
            except CustodyError:
                if authority is None:
                    raise
        same = (old is not None and raw_hex(old['parent_head_hash']) == descriptor['parent'] and
                old['anchor'] == descriptor['anchor'] and old['events'] == descriptor['events'] and
                old['source']['to'] == descriptor['evm_end'])
        if not same:
            # Each receipt and every omitted-log boundary is checked against this
            # operator's own pinned B3/EVM RPC before a worker receives the batch.
            end = descriptor['evm_end']
            handover = authority  # Native capabilities also bind an initial or still-unfrozen observation.
            self.operator._observe(descriptor['anchor'], descriptor['events'], evm_end=end, handover=handover)
            self.last_guard = time.monotonic()
            if handover is not None:
                self.stats['source_handovers'] += 1
                if authority.get('authority_kind') == 'own-quorum':
                    self.stats['own_source_handovers'] += 1
        if header is not None:
            self.operator._check_proposal_binding({'header': header})

    def _finish(self, reply):
        result = self.operator.source.finish(reply)
        self.automatic.on_applied(result)
        self.custody.on_applied(result)
        return result

    def _start_batch(self, descriptor, actions, candidate=None, info=None):
        key = (descriptor['parent'], hashlib.sha256(canonical(actions).encode()).hexdigest())
        if key in self.started_batches:
            return False
        self._authorize(descriptor, info=info)
        if len(actions) > 1024:
            raise CustodyError('client action batch exceeds the pinned protocol bound')
        for action in actions:
            self.worker.request('submit', frame=action)  # verifies client BIP340 and admission
        if candidate is None:
            candidate = self.worker.request('candidate', actions=actions)
        elif candidate['actions'] != actions:
            raise CustodyError('local cached candidate changed its exact native admitted actions')
        self._guard_proposal(candidate)
        # Refresh the native monotone clock at guarded ordinary startup. A
        # capability pause never enters this path; its exact NV/history resume
        # owns its own clock/authority rules. Idle ticks grant no fresh value.
        tick = self._finish(self.worker.request('pbft_timeout', now_ms=time.monotonic_ns() // 1_000_000))
        self.last_timer = time.monotonic()
        if tick.get('applied'):
            return True  # The existing exact certified decision already completed this parent.
        self.operator.pending_frozen = True
        self._finish(self.worker.request('pbft_start', header=candidate['header'], body=candidate['body']))
        self.started_batches.add(key)
        return True

    def seal(self, actions, anchor=None, evm_end=None):
        if not self.active or self.fenced:
            raise CustodyError('authenticated runtime is inactive or fenced')
        if actions is not None and (not isinstance(actions, list) or len(actions) > 1024):
            raise CustodyError('client action batch exceeds the pinned protocol bound')
        info = self.worker.request('info')
        anchor = self.operator.b3.anchor() if anchor is None else anchor
        self.operator._observe(anchor, None, evm_end=evm_end)
        saved = self.operator.pending
        descriptor = {'parent': raw_hex(info['head_hash']), 'anchor': saved['anchor'],
                      'events': saved['events'], 'evm_end': saved['source']['to']}
        candidate = (self.worker.request('candidate', max_actions=64, per_account=8) if actions is None else
                     self.worker.request('candidate', actions=actions))
        actions = candidate['actions']
        batch = {'observation': descriptor, 'actions': actions, 'header': candidate['header']}
        self.batches[descriptor['parent']] = batch
        self._cache_candidate(batch)
        self._broadcast({'kind': 'batch', **batch})
        self._flush()
        self._start_batch(descriptor, actions, candidate=candidate, info=info)
        return {'accepted': True, 'parent': info['head_hash'], 'expected_height': info['height'] + 1,
                'client_actions': len(actions), 'candidate_header': candidate['header'], 'candidate_hash': candidate['hash']}

    def _cache_candidate(self, batch):
        binding = header_binding(batch['header'])
        if binding['parent'] != raw_hex(batch['observation']['parent']):
            raise CustodyError('source descriptor names another candidate parent')
        key = binding['hash']
        self.candidates[key] = batch
        self.candidates.move_to_end(key)
        # These are availability hints, never voting authority. Keep the cache
        # bounded even when authenticated peers offer many alternate values.
        while len(self.candidates) > 16 or sum(len(canonical(item)) for item in self.candidates.values()) > 32 * 1024 * 1024:
            self.candidates.popitem(last=False)

    def _descriptor_for_header(self, header):
        binding = header_binding(header)
        candidate = self.candidates.get(binding['hash'])
        if candidate is not None:
            return candidate['observation']
        saved_values = [self.operator.pending] if self.operator.pending is not None else []
        paths = list((self.operator.directory / 'source-observations').glob('*.json'))
        if len(paths) > 4096 or sum(path.stat().st_size for path in paths) > 64 * 1024 * 1024:
            raise CustodyError('source observation availability history exceeds its bound')
        for path in paths:
            value = json.loads(path.read_text(encoding='utf-8'))
            if value.get('parent_head_hash') == binding['parent']:
                saved_values.append(value)
        for saved in saved_values:
            try:
                self.operator.source.verify_binding(saved, header, binding['hash'])
            except CustodyError:
                continue
            return {'parent': saved['parent_head_hash'], 'anchor': saved['anchor'],
                    'events': saved['events'], 'evm_end': saved['source']['to']}
        return None

    def _request_observation(self, header):
        key = ('observation', header_binding(header)['hash'])
        now = time.monotonic()
        if now - self.last_request.get(key, 0) >= .2:
            self.last_request[key] = now
            self._broadcast({'kind': 'observation_request', 'header': header})

    def _try_own_observation(self, info=None):
        info = self.worker.request('info') if info is None else info
        if not info['changing_view'] or info['next_leader'] != self.seat:
            return False
        tokens = [None] + [frame['token'] for frame in self.frames if frame.get('is_offer')][:16]
        for token in tokens:
            try:
                fields = {} if token is None else {'token': token}
                authority = self.worker.request('network_own_observation_authority', **fields)
            except CustodyError:
                if self.worker.process.poll() is not None:
                    raise
                continue
            if authority.get('available') is False:
                continue
            selected = self._descriptor_for_header(authority['header'])
            if selected is None:
                self._request_observation(authority['header'])
                return True
            self._authorize(selected, authority['header'], authority=authority, info=info)
            self.batches[raw_hex(info['head_hash'])] = {'header': authority['header'],
                'observation': selected, 'actions': authority['actions']}
            self._guard_pending()
            self._finish(self.worker.request('network_resume_observation'))
            return True
        return False

    def _history_reply(self, frame, value):
        height = uint(value['height'], 64)
        retained = self.worker.request('network_history', height=height)
        if not retained['found']:
            return
        binding = header_binding(retained['header'])
        if binding['parent'] != raw_hex(value['parent']):
            raise CustodyError('peer historical request names a conflicting certified parent')
        observation = next((entry['observation'] for entry in self.operator.source.records
                            if entry['slot_height'] == height and entry['head_hash'] == binding['hash']), None)
        if observation is None:
            raise CustodyError('certified history cannot leave before its durable source ACK')
        descriptor = {'parent': observation['parent_head_hash'], 'anchor': observation['anchor'],
                      'events': observation['events'], 'evm_end': observation['source']['to']}
        record = {key: retained[key] for key in ('header', 'body', 'certificate')}
        response = {'kind': 'history_response', 'record': record, 'observation': descriptor}
        for key in list(self.sent_history):
            if key[0] == frame['from_seat']:
                del self.sent_history[key]
        self.sent_history[(frame['from_seat'], height)] = {'response': response, 'last_sent': time.monotonic()}
        self._send(frame['from_seat'], response)

    def _preflight_history_observation(self, value, checked, info):
        descriptor = self._observation(value)
        binding = header_binding(checked['header'])
        if descriptor['parent'] != raw_hex(info['head_hash']) or binding['parent'] != descriptor['parent']:
            raise CustodyError('certified-history metadata changes the exact committed parent')
        self.operator.b3.verify_anchor(descriptor['anchor'])
        events, records, source_range = self.operator.source.scan(descriptor['evm_end'], descriptor['events'])
        facts, cursor = encode_source_facts(events, records, binding['facts_from'], info['domain'],
                                            source_range, self.operator.source.range_commitment)
        if facts != checked['facts']:
            raise CustodyError('certified-history metadata differs from the independently scanned native certified facts')
        saved = {'parent_head_hash': descriptor['parent'], 'anchor': descriptor['anchor'],
                 'events': events, 'records': records, 'facts_from': binding['facts_from'],
                 'facts_to': cursor, 'facts': facts, 'source': source_range}
        self.operator.source.verify_binding(saved, checked['header'], binding['hash'])

    def _history_apply(self, frame, value):
        checked = self.worker.request('network_check_history', token=frame['token'])
        record = value['record']
        binding = header_binding(record['header'])
        info = self.worker.request('info')
        if binding['height'] > info['height'] + 1:
            self._request_history(frame['from_seat'], info)
            self._consume_bulk(frame)
            return
        if binding['height'] == info['height'] + 1:
            authority = None
            frozen = info['observation_frozen'] or self.operator.pending_frozen
            if frozen:
                try:
                    self.operator._check_proposal_binding({'header': record['header']})
                except CustodyError:
                    # A final q3 certificate can replace an uncommitted frozen
                    # alternate source. Reject forged descriptor metadata before
                    # acquiring this non-signing native capability.
                    self._preflight_history_observation(value['observation'], checked, info)
                    authority = self.worker.request('network_history_observation_authority', token=frame['token'])
                    frame['certified_history_authority'] = True
                    message = bytes.fromhex(frame['message'])
                    if authority['message_sha256'] != hashlib.sha256(message).hexdigest():
                        raise CustodyError('native certified-history authority does not bind the exact retained frame bytes')
                    authority['message'] = frame['message']
            self._authorize(value['observation'], record['header'], authority=authority, info=info)
        elif binding['height'] <= info['height']:
            retained = self.worker.request('network_history', height=binding['height'])
            if not retained['found'] or retained['header'] != record['header'] or retained['body'] != record['body']:
                raise CustodyError('historical response conflicts with this operator certified prefix')
        if checked.get('acceptable') is not True:
            raise CustodyError('worker did not preflight the exact certified history frame')
        if binding['height'] == info['height'] + 1:
            self._guard_pending()
        else:
            self._guard_retained()
        self._finish(self.worker.request('network_dispatch', token=frame['token'], apply_history=True))
        if binding['height'] <= info['height']:
            self.stats['history_duplicates'] += 1
        else:
            self.stats['history_applied'] += 1
        # This ACK follows native DECISION + APPLIED barriers, publication and
        # the independent source-index file and directory barriers above.
        self._send(frame['from_seat'], {'kind': 'history_ack', 'height': binding['height'], 'head': binding['hash']})

    def _bulk(self, frame):
        raw = bytes.fromhex(frame['message'])
        if len(raw) > self.MAX_APP_BYTES:
            raise CustodyError('authenticated application record exceeds its bound')
        value = json.loads(raw)
        if not isinstance(value, dict) or value.get('format') != 'fm2-network-app/1':
            raise CustodyError('unknown authenticated application format')
        kind = value.get('kind')
        info = self.worker.request('info')
        if self.automatic.on_application(value):
            self._consume_bulk(frame)
        elif self.custody.on_application(frame['from_seat'], value):
            self._consume_bulk(frame)
        elif kind == 'hello':
            if uint(value['height'], 64) > info['height']:
                self._request_history(frame['from_seat'], info)
            elif uint(value['height'], 64) == info['height']:
                batch = self.batches.get(raw_hex(info['head_hash']))
                if batch is not None:
                    self._send(frame['from_seat'], {'kind': 'batch', **batch})
            self._consume_bulk(frame)
        elif kind == 'history_request':
            self._history_reply(frame, value)
            self._consume_bulk(frame)
        elif kind == 'history_response':
            self._history_apply(frame, value)
        elif kind == 'history_ack':
            height, head = uint(value['height'], 64), raw_hex(value['head'])
            retained = self.worker.request('network_history', height=height)
            if not retained['found'] or header_binding(retained['header'])['hash'] != head:
                raise CustodyError('peer ACK is not in this operator certified retained prefix')
            proof = {'format': 'fm2-authenticated-history-ack/1', 'peer': frame['from_seat'], 'height': height, 'head': head}
            self.write_once(self.operator.directory / 'peer-history-acks' / str(frame['from_seat']) /
                            (str(height).zfill(20) + '-' + head + '.json'), proof)
            old = self.acknowledged.get(frame['from_seat'])
            if old is None or height > old['height']:
                self.acknowledged[frame['from_seat']] = {'height': height, 'head': head}
            self.sent_history.pop((frame['from_seat'], height), None)
            self.stats['history_acks'] += 1
            self._consume_bulk(frame)
        elif kind == 'observation_request':
            descriptor = self._descriptor_for_header(value['header'])
            if descriptor is not None:
                self._send(frame['from_seat'], {'kind': 'observation_response', 'header': value['header'],
                                               'observation': descriptor})
            self._consume_bulk(frame)
        elif kind == 'observation_response':
            self._cache_candidate({'header': value['header'], 'observation': self._observation(value['observation']), 'actions': []})
            self._consume_bulk(frame)
        elif kind == 'batch':
            descriptor = self._observation(value['observation'])
            batch = {'observation': descriptor, 'actions': value['actions'], 'header': value['header']}
            self._cache_candidate(batch)
            if descriptor['parent'] == raw_hex(info['head_hash']):
                pending = self.operator.pending
                if not info['observation_frozen'] and not self.operator.pending_frozen:
                    self._start_batch(descriptor, value['actions'])
                    self.batches[descriptor['parent']] = batch
                elif pending is not None:
                    # A naked OFFER/batch cannot replace a frozen source range.
                    # Cache availability only; a verified NEW_VIEW must select it.
                    try:
                        self.operator._check_proposal_binding({'header': value['header']})
                        self._start_batch(descriptor, value['actions'])
                    except CustodyError:
                        pass
            elif len(self.batches) < 4:
                self.batches[descriptor['parent']] = batch
                self._request_history(frame['from_seat'], info)
            self._consume_bulk(frame)
        else:
            raise CustodyError('unknown authenticated application record kind')

    def step(self):
        if not self.active or self.fenced or self.paused:
            return
        current_info = self.worker.request('info')
        current_pending = self.operator.pending
        if time.monotonic() - self.last_guard >= .500:
            if current_pending is not None and raw_hex(current_pending['parent_head_hash']) == raw_hex(current_info['head_hash']):
                self._guard_pending()
            else:
                self._guard_retained()
        for _ in range(8):
            frame = self.worker.request('network_poll')
            if not frame['received']:
                break
            self.stats['authenticated_frames'] += 1
            if frame['lane'] == 1 and frame.get('is_new_view') is True:
                self.stats['new_view_frames'] = min(0xffffffff, self.stats['new_view_frames'] + 1)
                if self.regtest_drop_new_views:
                    # Private REGTEST receive loss only: classification and
                    # immutable token came from the authenticated native mesh.
                    # Drop before any authority can protect this token; history
                    # BULK is untouched and must prove recovery independently.
                    self._drop(frame)
                    self.stats['regtest_dropped_new_views'] = min(0xffffffff, self.stats['regtest_dropped_new_views'] + 1)
                    continue
            if frame['lane'] == 4:
                try:
                    self._bulk(frame)
                except (CustodyError, ValueError, KeyError, TypeError):
                    if not self.operator.opened or self.worker.process.poll() is not None:
                        raise
                    self.stats['peer_refusals'] += 1
                    if frame.get('certified_history_authority'):
                        raise  # Preserve the original storage/RPC failure after acquiring exact authority.
                    self._drop(frame)
            elif frame['lane'] in (1, 2):
                if len(self.frames) < self.MAX_PENDING:
                    self.frames.append(frame)
                else:
                    self._drop(frame)  # no delivery ACK; durable PBFT retries recover it
            else:
                self.stats['peer_refusals'] += 1
                self._drop(frame)
        for _ in range(len(self.frames)):
            frame = self.frames.popleft()
            info = self.worker.request('info')
            slot = frame.get('slot')
            if slot is not None and slot <= info['height']:
                self._drop(frame)
                continue
            if slot is not None and slot > info['height'] + 1:
                self._request_history(frame['from_seat'], info)
                self.frames.append(frame)
                continue
            descriptor = self.batches.get(raw_hex(info['head_hash']))
            pending = self.operator.pending
            if not frame.get('is_new_view') and descriptor is None and (pending is None or raw_hex(pending['parent_head_hash']) != raw_hex(info['head_hash'])):
                self.frames.append(frame)
                continue
            try:
                if frame.get('is_new_view'):
                    authority = self.worker.request('network_observation_authority', token=frame['token'])
                    selected = self._descriptor_for_header(authority['header'])
                    if selected is None:
                        self._request_observation(authority['header'])
                        self.frames.append(frame)
                        continue
                    self._authorize(selected, authority['header'], authority=authority, info=info)
                    self.batches[raw_hex(info['head_hash'])] = {'header': authority['header'],
                        'observation': selected, 'actions': authority['actions']}
                elif frame.get('header'):
                    selected = self._descriptor_for_header(frame['header'])
                    if selected is not None:
                        self._authorize(selected, frame['header'], info=info)
                    else:
                        self._request_observation(frame['header'])
                        self.frames.append(frame)
                        continue
                elif descriptor is not None:
                    self._authorize(descriptor['observation'], info=info)
                self._guard_pending()
                self.operator.pending_frozen = True
                self._finish(self.worker.request('network_dispatch', token=frame['token']))
                self.stats['agreement_frames'] += 1
            except CustodyError as error:
                if self.worker.process.poll() is not None:
                    raise
                self.stats['peer_refusals'] += 1
                self.last_peer_refusal = str(error)
                state = self.worker.request('info')
                if frame.get('is_new_view') and state.get('observation_handover_pending'):
                    # The stored authenticated capability is deliberately not
                    # droppable mid-handover. Retain its exact token for retry;
                    # the next source guard still fences a real canonical fault.
                    self.frames.append(frame)
                elif frame.get('is_offer'):
                    # A body that cannot yet pass local source observation may
                    # be required by a later q-report highest-prepared proof.
                    # Retaining it gives no voting permission or source thaw.
                    self.frames.append(frame)
                else:
                    self._drop(frame)
        info = self.worker.request('info')
        if self._try_own_observation(info):
            info = self.worker.request('info')
        now = time.monotonic()
        self.started_batches = {key for key in self.started_batches if key[0] == raw_hex(info['head_hash'])}
        past = {entry['head_hash'] for entry in self.operator.source.records} | {self.genesis_head}
        past.discard(raw_hex(info['head_hash']))
        self.batches = {parent: value for parent, value in self.batches.items() if parent not in past}
        if len(self.last_request) > 16:
            self.last_request = {key: value for key, value in self.last_request.items()
                                 if key[0] == 'observation' and now - value < 1 or key[0] != 'observation' and key[1] == info['height'] + 1}
        pending = self.operator.pending
        if (not info.get('observation_handover_pending') and pending is not None and
                raw_hex(pending['parent_head_hash']) == raw_hex(info['head_hash']) and now - self.last_timer >= .250):
            self._guard_pending()
            self._finish(self.worker.request('pbft_timeout', now_ms=time.monotonic_ns() // 1_000_000))
            self.last_timer = time.monotonic()
            self.stats['timer_ticks'] += 1
        now = time.monotonic()
        if now - self.last_hello >= .2:
            self._broadcast({'kind': 'hello', 'height': info['height'], 'head': info['head_hash']})
            self.last_hello = now
        for key, pending in list(self.sent_history.items()):
            if now - pending['last_sent'] >= .2:
                self._send(key[0], pending['response'])
                pending['last_sent'] = now
        # Once caught up, a batch received from a newer peer becomes runnable.
        batch = self.batches.get(raw_hex(info['head_hash']))
        if batch is not None and not info.get('observation_handover_pending'):
            self._start_batch(batch['observation'], batch['actions'])
        info = self.worker.request('info')
        if not info.get('observation_handover_pending'):
            self.automatic.step(now, info)
        if self.automatic.enabled and not info.get('observation_handover_pending'):
            self.custody.step(now, info)  # Custody reconstructs every authority from fresh native proofs internally.
        pending = self.operator.pending
        self.busy = bool(self.frames or self.automatic.pending or (pending is not None and
            raw_hex(pending['parent_head_hash']) == raw_hex(info['head_hash'])))
        self._flush()
