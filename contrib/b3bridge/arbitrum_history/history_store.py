#!/usr/bin/env python3
"""Durable, bounded *untrusted availability* for raw Arbitrum history objects.

SHA256 identifies local bytes; it is not an Ethereum/Arbitrum consensus proof.
Only the native verifier can check a finalized anchor, headers, receipt tries,
state witnesses and rollup assertions. Nothing in this module grants authority.
"""
from __future__ import annotations

import hashlib
import json
import os
from functools import wraps
from pathlib import Path
import re
import stat
import struct
import threading

try:
    import fcntl
except ImportError:  # Actual POSIX permissions and locking are mandatory.
    fcntl = None

ARBITRUM_GENESIS = "0x7ee576b35482195fc49205cec9af72ce14f003b9ae69f6ba0faef4514be8b442"
KINDS = frozenset(("l1_header_rlp", "l2_header_rlp", "l1_receipt", "l2_receipt", "l1_transaction", "l2_transaction",
                   "mpt_node", "beacon_block_ssz", "blob_sidecar_ssz", "data_column_ssz",
                   "nitro_batch", "delayed_message", "sequencer_feed_json"))
TRANSPORTS = frozenset(("b3-raw-object-http/1", "local-nitro-replay/1",
                        "nitro-sequencer-feed-unverified/1", "synthetic-adversarial-fixture/1"))
POLICY_KEYS = frozenset(("format", "mainnet_enabled", "writes_enabled", "source_chain_id",
                         "source_genesis_hash", "parent_chain_id", "max_object_bytes",
                         "max_total_bytes", "max_objects"))
HEX = re.compile(r"[0-9a-f]{64}\Z")
MAX_ENTRY = 4096
ZERO = "0" * 64


class HistoryError(ValueError):
    pass


def require(condition: bool, message: str):
    if not condition:
        raise HistoryError(message)


def strict_json(raw: bytes | str):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "Duplicate JSON field")
            result[key] = value
        return result
    def integer(value):
        require(len(value.lstrip("-")) <= 78, "JSON integer exceeds uint256 bound")
        result = int(value)
        require(abs(result) < 1 << 256, "JSON integer exceeds uint256 bound")
        return result
    try:
        return json.loads(raw, object_pairs_hook=pairs,
                          parse_int=integer,
                          parse_constant=lambda _: (_ for _ in ()).throw(HistoryError("Nonfinite JSON")))
    except (UnicodeError, ValueError, RecursionError) as error:
        raise HistoryError("Malformed JSON") from error


def canonical(value) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True,
                      allow_nan=False).encode("ascii")


def digest(raw: bytes) -> str:
    return hashlib.sha256(raw).hexdigest()


def default_policy() -> dict:
    return {"format": "b3-arbitrum-history-policy/1", "mainnet_enabled": False,
            "writes_enabled": False, "source_chain_id": 42161,
            "source_genesis_hash": ARBITRUM_GENESIS, "parent_chain_id": 1,
            "max_object_bytes": 16 * 1024 * 1024, "max_total_bytes": 512 * 1024 * 1024,
            "max_objects": 4096}


def validate_policy(value: dict) -> dict:
    require(type(value) is dict and set(value) == POLICY_KEYS, "Exact history policy required")
    require(value["format"] == "b3-arbitrum-history-policy/1" and
            value["mainnet_enabled"] is False and value["writes_enabled"] is False,
            "History must remain read-only with mainnet disabled")
    require(type(value["source_chain_id"]) is int and value["source_chain_id"] == 42161 and
            type(value["parent_chain_id"]) is int and value["parent_chain_id"] == 1 and
            value["source_genesis_hash"] == ARBITRUM_GENESIS, "Wrong history chain pins")
    for key, low, high in (("max_object_bytes", 1, 16 * 1024 * 1024),
                           ("max_total_bytes", 1, 512 * 1024 * 1024),
                           ("max_objects", 1, 4096)):
        require(type(value[key]) is int and low <= value[key] <= high, "Invalid history bound")
    require(value["max_total_bytes"] >= value["max_object_bytes"], "Insufficient total byte bound")
    return strict_json(canonical(value))


def _safe_parents(path: Path):
    require(path.is_absolute(), "History path must be absolute")
    require(".." not in path.parts, "History path cannot traverse parents")
    for parent in (path, *path.parents):
        info = parent.lstat()
        require(stat.S_ISDIR(info.st_mode) and not stat.S_ISLNK(info.st_mode), "Unsafe history directory")
        require(info.st_uid in (0, os.getuid()), "Unowned history ancestor")
        # A root-owned sticky /tmp is safe above an owner-only store. No other
        # group/world-writable ancestor is accepted.
        sticky_root = info.st_uid == 0 and bool(info.st_mode & stat.S_ISVTX)
        require(not info.st_mode & 0o022 or sticky_root, "Writable history ancestor")


def _file_fd(path: Path, flags: int):
    fd = os.open(path, flags | os.O_NOFOLLOW)
    try:
        info = os.fstat(fd)
        require(stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and
                info.st_nlink == 1 and info.st_mode & 0o777 == 0o600, "Unsafe history file")
        return fd
    except BaseException:
        os.close(fd)
        raise


def _sync_directory(path: Path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def _write_new(path: Path, raw: bytes):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    try:
        os.fchmod(fd, 0o600)
        with os.fdopen(fd, "wb", closefd=False) as output:
            output.write(raw)
            output.flush()
            os.fsync(fd)
    finally:
        os.close(fd)
    _sync_directory(path.parent)


def _locked(function):
    @wraps(function)
    def call(self, *args, **kwargs):
        with self._mutex:
            return function(self, *args, **kwargs)
    return call


class HistoryStore:
    """One POSIX writer, durable CAS, immutable policy, fully replayed journal.

    A torn write, missing object or orphan from a crash is a refusal, not an
    instruction to erase files. Restore/reacquisition requires an explicit new
    availability store; the independent native authority journal is preserved.
    Local hash chains cannot detect rollback of the entire filesystem image.
    """
    def __init__(self, directory, policy, *, create=False):
        require(fcntl is not None and hasattr(os, "O_NOFOLLOW"), "POSIX history storage required")
        self._mutex = threading.RLock()
        self.policy = validate_policy(policy)
        self.directory = Path(directory)
        self.entries, self.total_bytes, self.tip = {}, 0, ZERO
        self.sequence, self._lock, self._journal, self._fenced = 0, None, None, False
        require(type(create) is bool, "Explicit create flag required")
        if create:
            _safe_parents(self.directory.parent)
            # Never initialize or reset any existing directory.
            self.directory.mkdir(mode=0o700)
            os.chmod(self.directory, 0o700)
            _sync_directory(self.directory.parent)
        _safe_parents(self.directory)
        info = self.directory.lstat()
        require(info.st_uid == os.getuid() and info.st_mode & 0o777 == 0o700, "Owner-only history directory required")
        try:
            if create:
                _write_new(self.directory / "lock", b"")
            self._lock = _file_fd(self.directory / "lock", os.O_RDWR)
            try:
                fcntl.flock(self._lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as error:
                raise HistoryError("History store is already locked") from error
            if create:
                self.objects.mkdir(mode=0o700)
                _sync_directory(self.directory)
                _write_new(self.directory / "policy.json", canonical(self.policy))
                _write_new(self.directory / "history.log", b"")
            require(set(p.name for p in self.directory.iterdir()) ==
                    {"lock", "policy.json", "history.log", "objects"}, "Unexpected history store files")
            _safe_parents(self.objects)
            obj_info = self.objects.lstat()
            require(obj_info.st_uid == os.getuid() and obj_info.st_mode & 0o777 == 0o700,
                    "Owner-only object directory required")
            require(self._read_file(self.directory / "policy.json", 32768) == canonical(self.policy),
                    "History policy changed; preserve the existing store")
            self._journal = _file_fd(self.directory / "history.log", os.O_RDWR | os.O_APPEND)
            self._replay()
        except BaseException:
            self.close()
            raise

    @property
    def objects(self):
        return self.directory / "objects"

    @staticmethod
    def _read_file(path, maximum):
        fd = _file_fd(path, os.O_RDONLY)
        try:
            size = os.fstat(fd).st_size
            require(0 <= size <= maximum, "History object exceeds its bound")
            with os.fdopen(fd, "rb", closefd=False) as source:
                raw = source.read(maximum + 1)
            require(len(raw) == size, "History file changed during read")
            return raw
        finally:
            os.close(fd)

    @staticmethod
    def _provenance(value):
        require(type(value) is dict and set(value) == {"transport", "peer_id"}, "Exact availability provenance required")
        require(value["transport"] in TRANSPORTS and type(value["peer_id"]) is str and
                re.fullmatch(r"[a-zA-Z0-9._:-]{1,128}", value["peer_id"]), "Invalid availability provenance")
        return dict(value)

    def _replay(self):
        maximum = self.policy["max_objects"] * MAX_ENTRY
        require(os.fstat(self._journal).st_size <= maximum, "History journal bound exceeded")
        os.lseek(self._journal, 0, os.SEEK_SET)
        with os.fdopen(os.dup(self._journal), "rb") as source:
            while True:
                length = source.read(4)
                if not length:
                    break
                require(len(length) == 4, "Torn history journal length")
                size = struct.unpack(">I", length)[0]
                require(1 <= size <= MAX_ENTRY - 4, "Invalid history journal entry size")
                raw = source.read(size)
                require(len(raw) == size, "Torn history journal entry")
                entry = strict_json(raw)
                require(type(entry) is dict and set(entry) == {"sequence", "previous", "object", "provenance"},
                        "Invalid history entry")
                require(type(entry["sequence"]) is int and entry["sequence"] == self.sequence + 1 and
                        entry["previous"] == self.tip and canonical(entry) == raw, "History chain mismatch")
                self._provenance(entry["provenance"])
                descriptor = self._descriptor(entry["object"])
                require(descriptor["sha256"] not in self.entries, "Duplicate journal object")
                value = self._read_file(self.objects / descriptor["sha256"], self.policy["max_object_bytes"])
                require(len(value) == descriptor["size"] and digest(value) == descriptor["sha256"], "Corrupt history object")
                self._admit(descriptor)
                self.entries[descriptor["sha256"]] = descriptor
                self.tip, self.sequence = digest(raw), self.sequence + 1
        require(set(p.name for p in self.objects.iterdir()) == set(self.entries),
                "Orphan or unexpected history object; do not reset the store")

    def _descriptor(self, value):
        require(type(value) is dict and set(value) == {"sha256", "size", "kind"}, "Exact object descriptor required")
        require(type(value["sha256"]) is str and HEX.fullmatch(value["sha256"]) and
                type(value["kind"]) is str and value["kind"] in KINDS and
                type(value["size"]) is int and 1 <= value["size"] <= self.policy["max_object_bytes"],
                "Invalid object descriptor")
        return dict(value)

    def _admit(self, descriptor):
        require(self.sequence < self.policy["max_objects"] and
                self.total_bytes + descriptor["size"] <= self.policy["max_total_bytes"],
                "History quota exceeded")
        self.total_bytes += descriptor["size"]

    @_locked
    def put(self, kind: str, raw: bytes, provenance: dict) -> dict:
        require(self._journal is not None and not self._fenced, "History store closed or fenced")
        require(type(raw) is bytes, "Exact raw bytes required")
        descriptor = self._descriptor({"sha256": digest(raw), "size": len(raw), "kind": kind})
        provenance = self._provenance(provenance)
        old = self.entries.get(descriptor["sha256"])
        if old is not None:
            require(old == descriptor and self.get(old["sha256"]) == raw, "Object kind or content conflict")
            return dict(old)
        require(self.sequence < self.policy["max_objects"] and
                self.total_bytes + len(raw) <= self.policy["max_total_bytes"], "History quota exceeded")
        entry = {"sequence": self.sequence + 1, "previous": self.tip,
                 "object": descriptor, "provenance": provenance}
        encoded = canonical(entry)
        require(len(encoded) + 4 <= MAX_ENTRY, "Journal frame too large")
        try:
            _write_new(self.objects / descriptor["sha256"], raw)
            frame = struct.pack(">I", len(encoded)) + encoded
            # Short writes are possible. Never publish an index before fsync.
            position = 0
            while position < len(frame):
                written = os.write(self._journal, frame[position:])
                require(written > 0, "History journal write failed")
                position += written
            os.fsync(self._journal)
        except BaseException:
            self._fenced = True
            raise
        self._admit(descriptor)
        self.entries[descriptor["sha256"]] = descriptor
        self.sequence, self.tip = self.sequence + 1, digest(encoded)
        return dict(descriptor)

    @_locked
    def get(self, sha256: str) -> bytes:
        require(self._journal is not None and not self._fenced, "History store closed or fenced")
        require(type(sha256) is str and HEX.fullmatch(sha256), "Invalid object digest")
        require(sha256 in self.entries, "History object unavailable")
        descriptor = self.entries[sha256]
        try:
            raw = self._read_file(self.objects / sha256, self.policy["max_object_bytes"])
            require(len(raw) == descriptor["size"] and digest(raw) == sha256, "History object integrity failure")
            return raw
        except (HistoryError, OSError):
            self._fenced = True
            raise

    @_locked
    def raw_block(self, header_digest: str, receipt_digests: list[str], *, transaction_digests: list[str], chain_id=42161):
        require(type(chain_id) is int and chain_id in (1, 42161), "Unsupported raw witness chain")
        require(type(receipt_digests) is list and len(receipt_digests) <= 4096,
                "Receipt witness count exceeds its bound")
        require(type(transaction_digests) is list and len(transaction_digests) == len(receipt_digests),
                "Exact transaction body count required for receipt witness")
        header_kind, receipt_kind = ("l1_header_rlp", "l1_receipt") if chain_id == 1 else ("l2_header_rlp", "l2_receipt")
        transaction_kind = "l1_transaction" if chain_id == 1 else "l2_transaction"
        require(self.entries.get(header_digest, {}).get("kind") == header_kind, "Wrong raw header kind")
        header, receipts, transactions, size = self.get(header_digest), [], [], 0
        size += len(header)
        for index, sha256 in enumerate(receipt_digests):
            require(self.entries.get(sha256, {}).get("kind") == receipt_kind, "Wrong raw receipt kind")
            value = self.get(sha256)
            size += len(value)
            require(size <= 32 * 1024 * 1024, "Raw receipt witness byte bound exceeded")
            receipts.append({"index": index, "encoded": "0x" + value.hex()})
        for index, sha256 in enumerate(transaction_digests):
            require(self.entries.get(sha256, {}).get("kind") == transaction_kind, "Wrong raw transaction kind")
            value = self.get(sha256)
            size += len(value)
            require(size <= 32 * 1024 * 1024, "Raw block witness byte bound exceeded")
            transactions.append({"index": index, "encoded": "0x" + value.hex()})
        return {"format": "b3-raw-receipt-block/1", "chain_id": chain_id,
                "header_rlp": "0x" + header.hex(), "receipts": receipts,
                "transactions": transactions, "authority": False}

    @_locked
    def stats(self):
        return {"format": "b3-arbitrum-history-availability/1", "objects": len(self.entries),
                "total_bytes": self.total_bytes, "sequence": self.sequence, "tip": self.tip,
                "mainnet_enabled": False, "writes_enabled": False, "arbitrum_authority": False,
                "fenced": self._fenced}

    @_locked
    def close(self):
        if self._journal is not None:
            os.close(self._journal)
            self._journal = None
        if self._lock is not None:
            os.close(self._lock)
            self._lock = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
