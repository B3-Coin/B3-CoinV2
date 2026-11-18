#!/usr/bin/env python3
"""Bounded untrusted raw witnesses for the native B3ARPR01 proof decoder.

This module performs framing, content-addressed IO and structural checks only.
It verifies no Ethereum finality, Nitro execution, rollup settlement or custody
authority. Source identity, trusted RangeRequest and policy stay OUTSIDE the
wire proof. Native verification must bind them independently.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import struct
import sys

from history_store import ARBITRUM_GENESIS, HistoryError, HistoryStore, strict_json

MAGIC = b"B3ARPR01"
DIGEST_TAG = b"B3/arbitrum-proof-wire/1"
MAX_FRAME = 32 * 1024 * 1024
MAX_MANIFEST = 16 * 1024 * 1024
MAX_HEADERS = 128
MAX_HEADER = 4096
MAX_RECORDS = 8192
MAX_RECORD = 1024 * 1024
MAX_ACCOUNTS = 16
MAX_STORAGE = 65
MAX_NODES = 64
MAX_NODE = 8192
MAX_TOTAL_RECEIPTS = 65536
MAX_TOTAL_TRANSACTIONS = 65536
MAX_TOTAL_NODES = 8192
SHA = re.compile(r"[0-9a-f]{64}\Z")
ZERO = bytes(32)


class ProofError(HistoryError):
    pass


def require(ok, message):
    if not ok:
        raise ProofError(message)


def integer(value, bits):
    require(type(value) is int and 0 <= value < (1 << bits), "Noncanonical unsigned integer")
    return value


def raw(value, size=None, maximum=None):
    require(type(value) is bytes, "Raw bytes required")
    if size is not None:
        require(len(value) == size, "Wrong fixed field length")
    if maximum is not None:
        require(0 < len(value) <= maximum, "Raw blob length exceeds bound or is empty")
    return value


def sequence(value, maximum):
    require(type(value) is tuple and len(value) <= maximum, "Bounded immutable witness vector required")
    return value


@dataclass(frozen=True)
class Assertion:
    parent_assertion: bytes = ZERO
    block_hash: bytes = ZERO
    send_root: bytes = ZERO
    inbox_position: int = 0
    position_in_message: int = 0
    machine_status: int = 1
    end_history_root: bytes = ZERO
    inbox_accumulator: bytes = ZERO


@dataclass(frozen=True)
class StorageWitness:
    slot: bytes
    nodes: tuple[bytes, ...] = ()


@dataclass(frozen=True)
class AccountWitness:
    address: bytes
    nodes: tuple[bytes, ...] = ()
    storage: tuple[StorageWitness, ...] = ()


@dataclass(frozen=True)
class IndexedRecord:
    index: int
    encoded: bytes


@dataclass(frozen=True)
class BlockWitness:
    header: bytes
    receipts: tuple[IndexedRecord, ...] = ()
    transactions: tuple[IndexedRecord, ...] = ()


@dataclass(frozen=True)
class Proof:
    assertion: Assertion
    l1_accounts: tuple[AccountWitness, ...]
    blocks: tuple[BlockWitness, ...]
    confirmed_accounts: tuple[AccountWitness, ...] = ()
    historical_accounts: tuple[AccountWitness, ...] = ()


class _Writer:
    def __init__(self):
        self.data = bytearray(MAGIC + bytes(4))

    def put(self, value):
        require(len(value) <= MAX_FRAME - len(self.data), "Proof frame exceeds 32 MiB")
        self.data.extend(value)

    def u32(self, value):
        self.put(struct.pack(">I", integer(value, 32)))

    def u64(self, value):
        self.put(struct.pack(">Q", integer(value, 64)))

    def blob(self, value, maximum):
        raw(value, maximum=maximum)
        self.u32(len(value))
        self.put(value)


class _Reader:
    def __init__(self, value):
        raw(value)
        require(12 <= len(value) <= MAX_FRAME and value[:8] == MAGIC, "Invalid proof frame")
        require(struct.unpack_from(">I", value, 8)[0] == len(value) - 12, "Wrong proof payload length")
        self.data, self.position = memoryview(value), 12

    def take(self, size):
        require(size <= len(self.data) - self.position, "Truncated proof frame")
        value = bytes(self.data[self.position:self.position + size])
        self.position += size
        return value

    def u32(self):
        return struct.unpack(">I", self.take(4))[0]

    def u64(self):
        return struct.unpack(">Q", self.take(8))[0]

    def count(self, maximum):
        value = self.u32()
        require(value <= maximum, "Proof vector count exceeds bound")
        return value

    def blob(self, maximum):
        size = self.count(maximum)
        require(size != 0, "Empty raw proof blob")
        return self.take(size)


class _Budget:
    def __init__(self):
        self.nodes = self.receipts = self.transactions = 0

    def add(self, name, count, maximum):
        total = getattr(self, name) + count
        require(total <= maximum, "Aggregate proof count exceeds bound: " + name)
        setattr(self, name, total)


def encode_proof(proof: Proof) -> bytes:
    require(type(proof) is Proof and type(proof.assertion) is Assertion, "Exact Proof/Assertion required")
    out, budget = _Writer(), _Budget()
    a = proof.assertion
    for value in (a.parent_assertion, a.block_hash, a.send_root):
        out.put(raw(value, size=32))
    out.u64(a.inbox_position)
    out.u64(a.position_in_message)
    out.put(bytes((integer(a.machine_status, 8),)))
    out.put(raw(a.end_history_root, size=32))
    out.put(raw(a.inbox_accumulator, size=32))

    def nodes(values):
        values = sequence(values, MAX_NODES)
        budget.add("nodes", len(values), MAX_TOTAL_NODES)
        out.u32(len(values))
        for value in values:
            out.blob(value, MAX_NODE)

    def accounts(values):
        values = sequence(values, MAX_ACCOUNTS)
        out.u32(len(values))
        for account in values:
            require(type(account) is AccountWitness, "Exact AccountWitness required")
            out.put(raw(account.address, size=20))
            nodes(account.nodes)
            storage = sequence(account.storage, MAX_STORAGE)
            out.u32(len(storage))
            for item in storage:
                require(type(item) is StorageWitness, "Exact StorageWitness required")
                out.put(raw(item.slot, size=32))
                nodes(item.nodes)

    def records(values, name, maximum):
        values = sequence(values, MAX_RECORDS)
        budget.add(name, len(values), maximum)
        out.u32(len(values))
        for position, value in enumerate(values):
            require(type(value) is IndexedRecord and type(value.index) is int and value.index == position,
                    "Raw record indices must be contiguous from zero")
            out.u64(value.index)
            out.blob(value.encoded, MAX_RECORD)

    accounts(proof.l1_accounts)
    blocks = sequence(proof.blocks, MAX_HEADERS)
    require(len(blocks) != 0, "At least one complete block required")
    out.u32(len(blocks))
    for block in blocks:
        require(type(block) is BlockWitness, "Exact BlockWitness required")
        require(type(block.receipts) is tuple and type(block.transactions) is tuple and
                len(block.receipts) == len(block.transactions), "Transaction/receipt count mismatch")
        out.blob(block.header, MAX_HEADER)
        records(block.receipts, "receipts", MAX_TOTAL_RECEIPTS)
        records(block.transactions, "transactions", MAX_TOTAL_TRANSACTIONS)
    accounts(proof.confirmed_accounts)
    accounts(proof.historical_accounts)
    struct.pack_into(">I", out.data, 8, len(out.data) - 12)
    return bytes(out.data)


def decode_proof(encoded: bytes) -> Proof:
    inp, budget = _Reader(encoded), _Budget()
    a = Assertion(inp.take(32), inp.take(32), inp.take(32), inp.u64(), inp.u64(),
                  inp.take(1)[0], inp.take(32), inp.take(32))

    def nodes():
        count = inp.count(MAX_NODES)
        budget.add("nodes", count, MAX_TOTAL_NODES)
        return tuple(inp.blob(MAX_NODE) for _ in range(count))

    def accounts():
        result = []
        for _ in range(inp.count(MAX_ACCOUNTS)):
            address, account_nodes = inp.take(20), nodes()
            storage = tuple(StorageWitness(inp.take(32), nodes()) for _ in range(inp.count(MAX_STORAGE)))
            result.append(AccountWitness(address, account_nodes, storage))
        return tuple(result)

    def records(name, maximum):
        count = inp.count(MAX_RECORDS)
        budget.add(name, count, maximum)
        result = []
        for position in range(count):
            index = inp.u64()
            require(index == position, "Raw record indices must be contiguous from zero")
            result.append(IndexedRecord(index, inp.blob(MAX_RECORD)))
        return tuple(result)

    l1, blocks = accounts(), []
    count = inp.count(MAX_HEADERS)
    require(count != 0, "At least one complete block required")
    for _ in range(count):
        header = inp.blob(MAX_HEADER)
        receipts = records("receipts", MAX_TOTAL_RECEIPTS)
        transactions = records("transactions", MAX_TOTAL_TRANSACTIONS)
        require(len(receipts) == len(transactions), "Transaction/receipt count mismatch")
        blocks.append(BlockWitness(header, receipts, transactions))
    proof = Proof(a, l1, tuple(blocks), accounts(), accounts())
    require(inp.position == len(inp.data), "Trailing proof bytes")
    return proof


def wire_digest(encoded: bytes) -> str:
    """Native SHA256(tag || exact frame), not Bitcoin display-order hex."""
    decode_proof(encoded)
    return hashlib.sha256(DIGEST_TAG + encoded).hexdigest()


def _fields(value, fields):
    require(type(value) is dict and set(value) == set(fields), "Unknown or missing manifest field")


def _hex(value, size):
    require(type(value) is str and re.fullmatch(r"0x[0-9a-f]{" + str(size * 2) + r"}", value),
            "Canonical raw-order hex required")
    return bytes.fromhex(value[2:])


def _sha(value):
    require(type(value) is str and SHA.fullmatch(value), "Canonical object SHA256 required")
    return value


def source_context(value):
    _fields(value, ("kind", "chain_id", "genesis_hash"))
    chain = integer(value["chain_id"], 64)
    genesis = _hex(value["genesis_hash"], 32)
    require(chain != 0 and genesis != ZERO, "Exact nonzero source chain/genesis required")
    require(value["kind"] in ("arbitrum-one", "owned-regtest"), "Unknown source provenance kind")
    if value["kind"] == "arbitrum-one":
        require(chain == 42161 and value["genesis_hash"] == ARBITRUM_GENESIS, "Arbitrum origin mismatch")
    else:
        require(chain not in (1, 42161), "Owned local development chain cannot alias a live chain")
    return dict(value)


def _regular(path, maximum):
    """Bounded flat object IO; never follows a named-file symlink."""
    entry = path.lstat()
    require(stat.S_ISREG(entry.st_mode) and entry.st_nlink == 1 and
            not (getattr(entry, "st_file_attributes", 0) & 0x400) and entry.st_size <= maximum,
            "Unsafe or oversized input file")
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    try:
        actual = os.fstat(fd)
        require(stat.S_ISREG(actual.st_mode) and actual.st_nlink == 1 and
                (actual.st_dev, actual.st_ino) == (entry.st_dev, entry.st_ino), "Input file replaced")
        data = bytearray()
        while True:
            chunk = os.read(fd, min(65536, maximum + 1 - len(data)))
            if not chunk:
                break
            data.extend(chunk)
            require(len(data) <= maximum, "Input file exceeds bound")
        require(len(data) == actual.st_size, "Input file changed during read")
        return bytes(data)
    finally:
        os.close(fd)


class DirectoryObjects:
    """Flat SHA256 files from an untrusted exporter, with explicit dev origin.

    Directory names and the source annotation confer no authority. The native
    request/policy must independently authenticate source identity and roots.
    """
    def __init__(self, directory, source):
        self.directory = Path(directory).absolute()
        for ancestor in (self.directory, *self.directory.parents):
            entry = ancestor.lstat()
            require(stat.S_ISDIR(entry.st_mode) and not (getattr(entry, "st_file_attributes", 0) & 0x400),
                    "Object directory must have plain directory ancestors")
        self.source = source_context(source)

    def read(self, kind, sha256, maximum):
        del kind  # Flat object kinds are selected by exact manifest position.
        value = _regular(self.directory / _sha(sha256), maximum)
        require(hashlib.sha256(value).hexdigest() == sha256, "Raw object hash mismatch")
        return value


class HistoryObjects:
    """Existing 42161 CAS adapter, without raw_block's expanded JSON hex."""
    def __init__(self, store):
        self.store = store
        self.source = {"kind": "arbitrum-one", "chain_id": store.policy["source_chain_id"],
                       "genesis_hash": store.policy["source_genesis_hash"]}
        source_context(self.source)

    def read(self, kind, sha256, maximum):
        _sha(sha256)
        descriptor = self.store.entries.get(sha256, {})
        require(descriptor.get("kind") == kind and type(descriptor.get("size")) is int and
                descriptor["size"] <= maximum, "Wrong CAS raw object kind or size")
        value = self.store.get(sha256)
        require(len(value) <= maximum and hashlib.sha256(value).hexdigest() == sha256,
                "CAS object length/hash mismatch")
        return value


def assemble_raw_block(objects, manifest):
    """Assemble COMPLETE indexed raw bodies; cryptographic validity is native."""
    _fields(manifest, ("header_sha256", "receipt_sha256", "transaction_sha256"))
    receipts, transactions = manifest["receipt_sha256"], manifest["transaction_sha256"]
    require(type(receipts) is list and type(transactions) is list and
            len(receipts) == len(transactions) and len(receipts) <= MAX_RECORDS,
            "Complete bounded receipt/transaction lists required")
    return BlockWitness(objects.read("l2_header_rlp", _sha(manifest["header_sha256"]), MAX_HEADER),
        tuple(IndexedRecord(i, objects.read("l2_receipt", _sha(digest), MAX_RECORD)) for i, digest in enumerate(receipts)),
        tuple(IndexedRecord(i, objects.read("l2_transaction", _sha(digest), MAX_RECORD)) for i, digest in enumerate(transactions)))


def assemble_proof(objects, manifest) -> Proof:
    _fields(manifest, ("format", "mainnet_enabled", "writes_enabled", "source", "assertion",
                       "l1_accounts", "blocks", "confirmed_accounts", "historical_accounts"))
    require(manifest["format"] == "b3-arbitrum-proof-manifest/1" and manifest["mainnet_enabled"] is False and
            manifest["writes_enabled"] is False, "Disabled untrusted proof manifest required")
    require(source_context(manifest["source"]) == objects.source, "Object source context differs from manifest")
    a = manifest["assertion"]
    _fields(a, Assertion.__dataclass_fields__)
    assertion = Assertion(*(_hex(a[name], 32) for name in ("parent_assertion", "block_hash", "send_root")),
        integer(a["inbox_position"], 64), integer(a["position_in_message"], 64), integer(a["machine_status"], 8),
        _hex(a["end_history_root"], 32), _hex(a["inbox_accumulator"], 32))
    budget = _Budget()
    loaded_bytes = 0

    class BoundedObjects:
        def read(self, kind, sha256, maximum):
            nonlocal loaded_bytes
            value = objects.read(kind, sha256, maximum)
            raw(value, maximum=maximum)
            require(len(value) <= MAX_FRAME - 12 - loaded_bytes, "Aggregate raw object byte bound exceeded")
            loaded_bytes += len(value)
            return value

    bounded = BoundedObjects()

    def nodes(values):
        require(type(values) is list and len(values) <= MAX_NODES, "Bounded MPT node list required")
        budget.add("nodes", len(values), MAX_TOTAL_NODES)
        return tuple(bounded.read("mpt_node", _sha(value), MAX_NODE) for value in values)

    def accounts(values):
        require(type(values) is list and len(values) <= MAX_ACCOUNTS, "Bounded account list required")
        result = []
        for item in values:
            _fields(item, ("address", "nodes", "storage"))
            require(type(item["storage"]) is list and len(item["storage"]) <= MAX_STORAGE,
                    "Bounded storage list required")
            storage = []
            for slot in item["storage"]:
                _fields(slot, ("slot", "nodes"))
                storage.append(StorageWitness(_hex(slot["slot"], 32), nodes(slot["nodes"])))
            result.append(AccountWitness(_hex(item["address"], 20), nodes(item["nodes"]), tuple(storage)))
        return tuple(result)

    blocks = manifest["blocks"]
    require(type(blocks) is list and 1 <= len(blocks) <= MAX_HEADERS, "Bounded nonempty block list required")
    for block in blocks:
        _fields(block, ("header_sha256", "receipt_sha256", "transaction_sha256"))
        for key, counter, maximum in (("receipt_sha256", "receipts", MAX_TOTAL_RECEIPTS),
                                      ("transaction_sha256", "transactions", MAX_TOTAL_TRANSACTIONS)):
            require(type(block[key]) is list and len(block[key]) <= MAX_RECORDS, "Bounded raw record list required")
            budget.add(counter, len(block[key]), maximum)
    proof = Proof(assertion, accounts(manifest["l1_accounts"]), tuple(assemble_raw_block(bounded, block) for block in blocks),
                  accounts(manifest["confirmed_accounts"]), accounts(manifest["historical_accounts"]))
    # Enforce total bytes, nonempty blobs and EVERY native structural constraint
    # before this untrusted object can be handed to a native decoder.
    encode_proof(proof)
    return proof


def _write_new(path, data):
    path = Path(path).absolute()
    for ancestor in path.parents:
        entry = ancestor.lstat()
        require(stat.S_ISDIR(entry.st_mode) and not (getattr(entry, "st_file_attributes", 0) & 0x400),
                "Output ancestors must be plain directories")
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_NOFOLLOW", 0), 0o600)
    with os.fdopen(fd, "wb") as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    if hasattr(os, "O_DIRECTORY"):
        fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    export = sub.add_parser("export-native-proof")
    export.add_argument("--manifest", required=True)
    locations = export.add_mutually_exclusive_group(required=True)
    locations.add_argument("--objects-dir")
    locations.add_argument("--history-store")
    export.add_argument("--history-policy")
    export.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    require(bool(args.history_store) == bool(args.history_policy), "History store requires exactly its policy file")
    manifest_raw = _regular(Path(args.manifest), MAX_MANIFEST)
    manifest = strict_json(manifest_raw)
    store = None
    try:
        if args.history_store:
            policy = strict_json(_regular(Path(args.history_policy), 32768))
            store = HistoryStore(args.history_store, policy, create=False)
            objects = HistoryObjects(store)
        else:
            require(type(manifest) is dict and "source" in manifest, "Proof manifest source required")
            objects = DirectoryObjects(args.objects_dir, manifest["source"])
        proof = assemble_proof(objects, manifest)
        encoded = encode_proof(proof)
        _write_new(args.output, encoded)
        report = {"format": "b3-arbitrum-proof-export/1", "source": objects.source,
            "manifest_sha256": hashlib.sha256(manifest_raw).hexdigest(),
            "frame_sha256": hashlib.sha256(encoded).hexdigest(), "wire_digest": wire_digest(encoded),
            "bytes": len(encoded), "blocks": len(proof.blocks),
            "receipts": sum(len(b.receipts) for b in proof.blocks),
            "transactions": sum(len(b.transactions) for b in proof.blocks),
            "mainnet_enabled": False, "writes_enabled": False, "arbitrum_authority": False,
            "native_verified": False, "execution_verified": False, "settlement_verified": False,
            "no_rpc_end_to_end_qualified": False, "external_anchor_required": True}
        print(json.dumps(report, sort_keys=True, separators=(",", ":")))
        return 0
    finally:
        if store is not None:
            store.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (HistoryError, OSError, ValueError) as error:
        # Do not echo raw provider data, proof bytes or arbitrary input paths.
        print(json.dumps({"format": "b3-arbitrum-proof-export/1", "result": "REFUSED",
                          "error_type": type(error).__name__, "mainnet_enabled": False,
                          "arbitrum_authority": False}), file=sys.stderr)
        sys.exit(1)
