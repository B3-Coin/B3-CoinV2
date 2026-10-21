#!/usr/bin/env python3
"""Pinned source finality, with an optional read-only Arbitrum HTTPS boundary.

Arbitrum's finalized L2 tag follows batch data finalized on Ethereum. It is
not sequencer/latest, a child-block count, or completed withdrawal challenges.
These checks trust the configured independent providers, not a local consensus
light client or fraud-proof verifier. No mainnet write activation is provided.
"""
from pathlib import Path
import copy
import hashlib
import json
import os
import re
import stat
import threading

from custody import CustodyError, atomic_json, hex_value, uint
from arbitrum_rpc import HttpsReadOnlyRpc, ProviderIdentityError, quantity, strict_json


def canonical_hash(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':'),
                                     allow_nan=False).encode('utf-8')).hexdigest()


class SourceFinality:
    FIELDS = {'format', 'chain_id', 'genesis_hash', 'chain_kind', 'parent_chain_id',
              'mode', 'minimum_confirmations', 'provider_ids', 'mainnet_enabled'}
    BINDING_FIELDS = {'operator_id', 'endpoint_id', 'endpoint_origin', 'endpoint_commitment'}
    MAX_STATE_BYTES = 65536

    @classmethod
    def validate_policy(cls, policy):
        if not isinstance(policy, dict):
            raise CustodyError('source finality policy must be an exact object')
        live = policy.get('format') == 'fm2-source-finality-policy/2'
        if (set(policy) != cls.FIELDS | ({'provider_bindings'} if live else set()) or
                policy.get('format') not in ('fm2-source-finality-policy/1', 'fm2-source-finality-policy/2') or
                policy['mainnet_enabled'] is not False):
            raise CustodyError('unknown finality policy or mainnet activation requested')
        uint(policy['chain_id'])
        if type(policy['minimum_confirmations']) is not int:
            raise CustodyError('source confirmation policy must be an exact integer')
        hex_value(policy['genesis_hash'], 32)
        if policy['genesis_hash'] != policy['genesis_hash'].lower():
            raise CustodyError('genesis hash must be canonical lowercase')
        if policy['chain_kind'] not in ('isolated-regtest', 'ethereum-l1', 'arbitrum-l2'):
            raise CustodyError('unsupported source-chain finality semantics')
        if (policy['chain_kind'] == 'arbitrum-l2' and
                (type(policy['parent_chain_id']) is not int or policy['parent_chain_id'] != 1)) or (
                policy['chain_kind'] != 'arbitrum-l2' and policy['parent_chain_id'] is not None):
            raise CustodyError('source settlement parent must be explicitly bound')
        if policy['mode'] == 'confirmations':
            if (policy['chain_kind'] != 'isolated-regtest' or policy['chain_id'] != 31337 or
                    policy['minimum_confirmations'] != 2):
                raise CustodyError('block-count finality is permitted only in the named regtest policy')
        elif policy['mode'] == 'finalized':
            if policy['chain_kind'] == 'isolated-regtest' or policy['minimum_confirmations'] != 0:
                raise CustodyError('finalized source policies cannot silently downgrade to block counts')
        else:
            raise CustodyError('source finality tag/mode is not allowed')
        labels = policy['provider_ids']
        if (not isinstance(labels, list) or not 2 <= len(labels) <= 4 or
                any(not isinstance(item, str) or not re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9._-]{0,127}', item)
                    for item in labels) or len(set(labels)) != len(labels)):
            raise CustodyError('two to four explicitly distinct source-node providers are required')
        if live:
            if (type(policy['chain_id']) is not int or policy['chain_id'] != 42161 or
                    policy['chain_kind'] != 'arbitrum-l2' or policy['mode'] != 'finalized' or
                    not isinstance(policy['provider_bindings'], dict) or set(policy['provider_bindings']) != set(labels)):
                raise CustodyError('live read qualification is restricted to pinned Arbitrum One finalized data')
            bindings = policy['provider_bindings']
            for item in bindings.values():
                if not isinstance(item, dict) or set(item) != cls.BINDING_FIELDS:
                    raise CustodyError('provider binding must be exact')
                for field in ('operator_id', 'endpoint_id'):
                    if not isinstance(item[field], str) or not re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9._-]{0,127}', item[field]):
                        raise CustodyError('provider binding labels must be bounded public identifiers')
                if (not isinstance(item['endpoint_origin'], str) or
                        not re.fullmatch(r'https://[a-z0-9.-]+:443', item['endpoint_origin']) or
                        not isinstance(item['endpoint_commitment'], str) or
                        not re.fullmatch(r'[0-9a-f]{64}', item['endpoint_commitment'])):
                    raise CustodyError('provider origin/commitment is not canonical')
            for field in cls.BINDING_FIELDS:
                if len({item[field] for item in bindings.values()}) != len(labels):
                    raise CustodyError('provider operators and endpoints must be explicitly independent')
        return copy.deepcopy(policy)

    def __init__(self, policy, providers, checkpoint_path=None):
        self._lock, self._file_lock = threading.RLock(), None
        self.policy = self.validate_policy(policy)
        if not isinstance(providers, dict) or set(providers) != set(self.policy['provider_ids']):
            raise CustodyError('configured source providers differ from the pinned policy')
        self.providers = dict(providers)
        if self.policy['format'].endswith('/2'):
            for label, rpc in self.providers.items():
                if (not isinstance(rpc, HttpsReadOnlyRpc) or rpc.provider_id != label or
                        rpc.binding != self.policy['provider_bindings'][label] or
                        rpc._chain_id != self.policy['chain_id'] or rpc._genesis_hash != self.policy['genesis_hash']):
                    raise CustodyError('live source provider differs from its pinned HTTPS identity')
        self.path = Path(checkpoint_path).absolute() if checkpoint_path is not None else None
        self.previous, self.fence, self.observations = None, None, {}
        try:
            if self.path is not None:
                self._safe_path(self.path)
                self.path.parent.mkdir(parents=True, exist_ok=True)
                self._safe_path(self.path)
                self._acquire_file_lock()
                self._load()
        except BaseException:
            self.close()
            raise

    @staticmethod
    def _safe_path(path):
        for candidate in (path, *path.parents):
            try:
                info = candidate.lstat()
            except FileNotFoundError:
                continue
            if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
                raise CustodyError('source finality state cannot traverse symlinks/reparse points')
            if candidate == path and not stat.S_ISREG(info.st_mode):
                raise CustodyError('source finality state must be a regular file')
            if candidate == path and getattr(info, 'st_nlink', 1) != 1:
                raise CustodyError('source finality state cannot use aliased hard links')

    def _acquire_file_lock(self):
        lock_path = self.path.with_name(self.path.name + '.lock')
        self._safe_path(lock_path)
        fd = os.open(lock_path, os.O_RDWR | os.O_CREAT | getattr(os, 'O_NOFOLLOW', 0), 0o600)
        try:
            if os.name == 'posix':
                import fcntl
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            else:
                import msvcrt
                if os.fstat(fd).st_size == 0:
                    os.write(fd, b'\0')
                    os.fsync(fd)
                os.lseek(fd, 0, os.SEEK_SET)
                msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
            self._file_lock = fd
        except OSError:
            os.close(fd)
            raise CustodyError('source finality state already has an owner') from None

    def close(self):
        fd, self._file_lock = self._file_lock, None
        if fd is not None:
            os.close(fd)

    def __del__(self):
        self.close()

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        self.close()

    def _load(self):
        if not self.path.exists():
            return
        self._safe_path(self.path)
        try:
            fd = os.open(self.path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0))
            if not stat.S_ISREG(os.fstat(fd).st_mode):
                os.close(fd)
                raise ValueError('state is not a regular file')
            with os.fdopen(fd, 'rb') as stream:
                raw = stream.read(self.MAX_STATE_BYTES + 1)
            if len(raw) > self.MAX_STATE_BYTES:
                raise ValueError('oversize state')
            saved = strict_json(raw.decode('utf-8'))
            if not isinstance(saved, dict) or saved.get('policy') != self.policy:
                raise ValueError('different policy')
            if set(saved) == {'policy', 'checkpoint'} and self.policy['format'].endswith('/1'):
                self.previous = self._point(saved['checkpoint'])
                return
            if (set(saved) != {'format', 'policy', 'checkpoint', 'observations', 'fence'} or
                    saved['format'] != 'fm2-source-finality-state/2' or
                    not isinstance(saved['observations'], dict) or
                    set(saved['observations']) - set(self.policy['provider_ids'])):
                raise ValueError('invalid state shape')
            self.previous = self._point(saved['checkpoint']) if saved['checkpoint'] is not None else None
            self.observations = {key: self._point(value) for key, value in saved['observations'].items()}
            fence = saved['fence']
            if fence is not None and fence not in ('retained-checkpoint-replaced', 'finalized-head-regressed',
                                                   'finalized-checkpoint-conflict', 'chain-identity-changed'):
                raise ValueError('invalid fence')
            self.fence = fence
        except (ValueError, TypeError, OSError, UnicodeError, CustodyError, RecursionError):
            raise CustodyError('durable source finality state/policy refused; no reset or inferred migration') from None

    def _save(self):
        if self.path is None:
            raise CustodyError('durable finality advancement requires an explicit checkpoint path')
        self._safe_path(self.path)
        self._safe_path(self.path.with_name(self.path.name + '.tmp'))
        atomic_json(self.path, {'format': 'fm2-source-finality-state/2', 'policy': self.policy,
                              'checkpoint': self.previous, 'observations': self.observations, 'fence': self.fence})

    def _fence(self, reason):
        self.fence = reason
        if self.path is not None:
            self._save()  # Fsync before reporting: restoring the endpoint cannot erase the refusal.
        raise CustodyError('source finality rollback/identity conflict durably fenced: ' + reason)

    def _call(self, rpc, method, params=()):
        try:
            return rpc.call(method, params)
        except ProviderIdentityError:
            self._fence('chain-identity-changed')

    def _check_head(self, label, finalized):
        old_head = self.observations.get(label)
        if old_head is not None:
            if (finalized['height'] < old_head['height'] or
                    (finalized['height'] == old_head['height'] and finalized != old_head)):
                self._fence('finalized-head-regressed')
            retained = self._block(self._call(self.providers[label], 'eth_getBlockByNumber',
                                              [hex(old_head['height']), False]))
            if retained != old_head:
                self._fence('retained-checkpoint-replaced')
        if self.previous is not None and finalized['height'] < self.previous['height']:
            self._fence('finalized-head-regressed')

    @staticmethod
    def _point(value):
        if not isinstance(value, dict) or set(value) != {'height', 'hash'}:
            raise CustodyError('noncanonical source finality checkpoint')
        height, hash_ = uint(value['height'], 64), hex_value(value['hash'], 32)
        if type(value['height']) is not int or value['hash'] != hash_:
            raise CustodyError('checkpoint height/hash must have canonical encoding')
        return {'height': height, 'hash': hash_}

    @staticmethod
    def _block(value):
        if not isinstance(value, dict):
            raise CustodyError('required source block/finalized tag is unavailable; no latest fallback')
        try:
            height = quantity(value['number'])
            hash_ = hex_value(value['hash'], 32)
            if value['hash'] != hash_:
                raise ValueError('noncanonical hash')
            return {'height': height, 'hash': hash_}
        except (KeyError, ValueError, TypeError, CustodyError):
            raise CustodyError('source returned malformed block/finality metadata') from None

    def _assert_open(self):
        if self.path is not None and self._file_lock is None:
            raise CustodyError('source finality state owner is closed')
        if self.fence is not None:
            raise CustodyError('source finality remains durably fenced; operator review required')

    def finalized_heads(self):
        with self._lock:
            self._assert_open()
            if self.policy['mode'] != 'finalized':
                raise CustodyError('finalized-head selection is unavailable for regtest confirmation policies')
            heads = {}
            for label in self.policy['provider_ids']:
                head = self._block(self._call(self.providers[label], 'eth_getBlockByNumber', ['finalized', False]))
                self._check_head(label, head)
                heads[label] = head
            return heads

    def verify(self, checkpoint, persist=False):
        with self._lock:
            self._assert_open()
            point = self._point(checkpoint)
            if self.previous is not None and point['height'] < self.previous['height']:
                raise CustodyError('source finality checkpoint regresses below the durable high water mark')
            heads = {}
            for label in self.policy['provider_ids']:
                rpc = self.providers[label]
                if quantity(self._call(rpc, 'eth_chainId')) != self.policy['chain_id']:
                    self._fence('chain-identity-changed')
                genesis = self._block(self._call(rpc, 'eth_getBlockByNumber', ['0x0', False]))
                if genesis != {'height': 0, 'hash': self.policy['genesis_hash']}:
                    self._fence('chain-identity-changed')
                if self.previous is not None:
                    retained = self._block(self._call(rpc, 'eth_getBlockByNumber', [hex(self.previous['height']), False]))
                    if retained != self.previous:
                        self._fence('retained-checkpoint-replaced')
                canonical = self._block(self._call(rpc, 'eth_getBlockByNumber', [hex(point['height']), False]))
                by_hash = self._block(self._call(rpc, 'eth_getBlockByHash', [point['hash'], False]))
                if canonical != point or by_hash != point:
                    if self.previous is not None and point['height'] == self.previous['height']:
                        self._fence('finalized-checkpoint-conflict')
                    raise CustodyError('source providers disagree on the checkpoint canonical block')
                if self.policy['mode'] == 'finalized':
                    finalized = self._block(self._call(rpc, 'eth_getBlockByNumber', ['finalized', False]))
                    self._check_head(label, finalized)
                    if finalized['height'] < point['height']:
                        raise CustodyError('source checkpoint is not finalized by its settlement parent')
                    at_height = self._block(self._call(rpc, 'eth_getBlockByNumber', [hex(finalized['height']), False]))
                    if at_height != finalized:
                        raise CustodyError('source finalized tag is inconsistent with its canonical chain')
                    heads[label] = finalized
                else:
                    latest = quantity(self._call(rpc, 'eth_blockNumber'))
                    if latest < point['height'] + self.policy['minimum_confirmations'] - 1:
                        raise CustodyError('isolated regtest checkpoint awaits its configured confirmations')
            if persist:
                old_previous, old_observations = self.previous, self.observations
                self.previous, self.observations = point, heads
                try:
                    self._save()
                except BaseException:
                    self.previous, self.observations = old_previous, old_observations
                    raise
            return copy.deepcopy(point)


def qualify_read_only(policy, providers, checkpoint_path):
    """Choose a jointly canonical parent-finalized height, then fsync its fence.

    Head heights may legitimately differ between independent providers. Both
    must return the exact same selected block by number/hash and each own
    finalized tag must be canonical. No latest fallback is possible.
    """
    if policy.get('format') != 'fm2-source-finality-policy/2':
        raise CustodyError('live qualification requires the strict HTTPS finality policy/2')
    with SourceFinality(policy, providers, checkpoint_path) as verifier:
        heads = verifier.finalized_heads()
        height = min(item['height'] for item in heads.values())
        label = verifier.policy['provider_ids'][0]
        checkpoint = verifier._block(verifier._call(providers[label], 'eth_getBlockByNumber', [hex(height), False]))
        if checkpoint['height'] != height:
            raise CustodyError('provider returned another height during finalized selection')
        verifier.verify(checkpoint, persist=True)
        return {'format': 'fm2-arbitrum-read-only-qualification/1',
                'policy_sha256': canonical_hash(verifier.policy), 'checkpoint': checkpoint,
                'finalized_heads': copy.deepcopy(verifier.observations),
                'provider_bindings': copy.deepcopy(verifier.policy['provider_bindings']),
                'mainnet_enabled': False, 'writes_enabled': False,
                'trust': 'configured-independent-provider-rpc; no consensus light client'}
