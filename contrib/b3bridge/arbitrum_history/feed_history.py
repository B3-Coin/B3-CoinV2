#!/usr/bin/env python3
"""Bounded Nitro v1 feed availability parsing. Never a finality/execution proof.

The upstream feed sequence describes a sequencer promise. Its signature, a
confirmed-sequence notice, TLS or agreement of relays cannot authorize a B3
deposit. Gaps require canonical parent-chain history, not a cursor jump.
"""
from __future__ import annotations

import base64
import binascii
import re
import threading

from history_store import HistoryError, canonical, digest, require, strict_json

MAX_FRAME = 8 * 1024 * 1024
MAX_MESSAGES = 256
MAX_L2_MESSAGE = 1024 * 1024
MAX_UINT64 = (1 << 64) - 1
NITRO_GENESIS_BLOCK = 22207817


def _uint(value):
    require(type(value) is int and 0 <= value <= MAX_UINT64, "Invalid feed integer")
    return value


def _base64(value, maximum, *, nullable=False):
    if nullable and value is None:
        return
    require(type(value) is str and len(value) <= ((maximum + 2) // 3) * 4,
            "Feed binary field exceeds its bound")
    try:
        raw = base64.b64decode(value, validate=True)
    except (ValueError, binascii.Error) as error:
        raise HistoryError("Invalid feed binary encoding") from error
    require(len(raw) <= maximum and base64.b64encode(raw).decode() == value,
            "Noncanonical feed binary encoding")


def parse_feed_frame(raw: bytes, *, max_messages=MAX_MESSAGES) -> dict:
    require(type(raw) is bytes and 1 <= len(raw) <= MAX_FRAME, "Feed frame exceeds its bound")
    require(type(max_messages) is int and 1 <= max_messages <= 10000, "Invalid feed message bound")
    value = strict_json(raw)
    require(type(value) is dict and set(value) <=
            {"version", "messages", "confirmedSequenceNumberMessage"} and
            type(value.get("version")) is int and value["version"] == 1,
            "Unsupported feed frame")
    messages = value.get("messages", [])
    require(type(messages) is list and len(messages) <= max_messages, "Feed message count exceeds its bound")
    previous = None
    for item in messages:
        require(type(item) is dict, "Unsupported feed message")
        signature_key = "signatureV2" if "signatureV2" in item else "signature"
        require({"sequenceNumber", "message", signature_key} <= set(item) and
                set(item) <= {"sequenceNumber", "message", signature_key, "blockMetadata", "blockHash"},
                "Unsupported feed message")
        sequence = _uint(item["sequenceNumber"])
        require(sequence <= MAX_UINT64 - NITRO_GENESIS_BLOCK, "Feed sequence overflows L2 height")
        require(previous is None or sequence == previous + 1, "Feed frame contains a gap or reorder")
        previous = sequence
        _base64(item[signature_key], 65, nullable=True)
        if "blockHash" in item:
            require(type(item["blockHash"]) is str and re.fullmatch(r"0x[0-9a-fA-F]{64}", item["blockHash"]),
                    "Invalid feed block hash")
        if "blockMetadata" in item:
            _base64(item["blockMetadata"], 65536, nullable=True)
        metadata = item["message"]
        require(type(metadata) is dict and set(metadata) == {"message", "delayedMessagesRead"},
                "Unsupported feed metadata")
        _uint(metadata["delayedMessagesRead"])
        message = metadata["message"]
        require(type(message) is dict and {"header", "l2Msg"} <= set(message) and
                set(message) <= {"header", "l2Msg", "batchGasCost", "batchDataTokens"}, "Unsupported feed L1 message")
        _base64(message["l2Msg"], MAX_L2_MESSAGE)
        if "batchGasCost" in message and message["batchGasCost"] is not None:
            _uint(message["batchGasCost"])
        if "batchDataTokens" in message and message["batchDataTokens"] is not None:
            stats = message["batchDataTokens"]
            require(type(stats) is dict and set(stats) == {"length", "nonzeros"}, "Invalid feed batch stats")
            require(_uint(stats["nonzeros"]) <= _uint(stats["length"]), "Invalid feed batch stats")
        header = message["header"]
        require(type(header) is dict and set(header) ==
                {"kind", "sender", "blockNumber", "timestamp", "requestId", "baseFeeL1"},
                "Unsupported feed header")
        require(type(header["kind"]) is int and 0 <= header["kind"] <= 255, "Invalid feed message kind")
        require(type(header["sender"]) is str and re.fullmatch(r"0x[0-9a-fA-F]{40}", header["sender"]),
                "Invalid feed sender")
        _uint(header["blockNumber"])
        _uint(header["timestamp"])
        require(header["requestId"] is None or type(header["requestId"]) is str and
                re.fullmatch(r"0x[0-9a-fA-F]{64}", header["requestId"]), "Invalid feed request ID")
        require(header["baseFeeL1"] is None or type(header["baseFeeL1"]) is int and
                0 <= header["baseFeeL1"] < 1 << 256, "Invalid feed base fee")
    confirmed = value.get("confirmedSequenceNumberMessage")
    if confirmed is not None:
        require(type(confirmed) is dict and set(confirmed) == {"sequenceNumber"}, "Unsupported feed confirmation")
        _uint(confirmed["sequenceNumber"])
    require(bool(messages) or confirmed is not None, "Empty feed frame")
    return value


class FeedCursor:
    """Bounded soft continuity diagnostic; never writes a canonical source head.

    Caller supplies an explicit first sequence, normally a local Nitro replay
    cursor. A feed-reported confirmation never changes it. A gap/conflict fences
    this cursor; the canonical history/replay pipeline must reconcile separately.
    """
    def __init__(self, next_sequence: int):
        self._mutex = threading.Lock()
        self.next_sequence = _uint(next_sequence)
        self.previous_delayed = None
        self.last = None
        self.fenced = False

    def ingest(self, store, raw: bytes, *, peer_id: str):
        with self._mutex:
            return self._ingest(store, raw, peer_id=peer_id)

    def _ingest(self, store, raw: bytes, *, peer_id: str):
        require(not self.fenced, "Feed cursor is fenced; canonical history required")
        value = parse_feed_frame(raw)
        next_sequence = self.next_sequence
        delayed = self.previous_delayed
        last = self.last
        for item in value.get("messages", []):
            sequence, item_hash = item["sequenceNumber"], digest(canonical(item))
            if last is not None and sequence == last[0]:
                if item_hash != last[1]:
                    self.fenced = True
                    raise HistoryError("Conflicting sequencer feed; no source authority")
                continue
            if sequence != next_sequence:
                self.fenced = True
                raise HistoryError("Sequencer feed gap/reorder; canonical history required")
            count = item["message"]["delayedMessagesRead"]
            if delayed is not None and count < delayed:
                self.fenced = True
                raise HistoryError("Sequencer delayed-message count regressed")
            delayed, last, next_sequence = count, (sequence, item_hash), sequence + 1
        # Preserve exact bytes, not a reformatted pseudo-proof. State advances
        # only after raw availability bytes have been durably stored.
        descriptor = store.put("sequencer_feed_json", raw,
                               {"transport": "nitro-sequencer-feed-unverified/1", "peer_id": peer_id})
        self.next_sequence, self.previous_delayed, self.last = next_sequence, delayed, last
        return {"object": descriptor, "next_soft_sequence": next_sequence,
                "soft_only": True, "native_execution_verified": False,
                "arbitrum_authority": False, "mainnet_enabled": False}
