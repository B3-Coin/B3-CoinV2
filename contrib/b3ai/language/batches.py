# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Verified whole-document byte batches; no transitions across corpus splits."""

import hashlib
import json
from pathlib import Path
import re
import unicodedata

import numpy as np

from .corpus import LICENSE, MAX_DOCUMENT_BYTES, MAX_OUTPUT_BYTES, REPOSITORY, REVISION, SOURCES
from ..scratch.checkpoint import _no_duplicate_keys, _reject_constant


EOD_ID = 256
TOKENIZER_VERSION = "utf8-bytes-eod-v1"
SPLITS = ("train", "validation", "test")
ROW_FIELDS = {"id", "text", "split", "source_key", "source_document",
              "source_byte_start", "source_byte_end", "raw_sha256", "normalized_sha256"}


def _hash(data):
    return hashlib.sha256(data).hexdigest()


def _json(data):
    try:
        return json.loads(data, object_pairs_hook=_no_duplicate_keys, parse_constant=_reject_constant)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise ValueError("Invalid corpus JSON.") from error


def _checksum(value):
    return type(value) is str and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _integer(value, minimum=0, maximum=MAX_OUTPUT_BYTES):
    return type(value) is int and minimum <= value <= maximum


def encode(text):
    """Encode one complete story; EOD is distinct from every possible byte."""
    if type(text) is not str or not text or "\0" in text:
        raise ValueError("A document must be nonempty UTF-8 text without NUL.")
    try:
        raw = text.encode("utf-8", errors="strict")
    except UnicodeError as error:
        raise ValueError("Invalid document Unicode.") from error
    if len(raw) > MAX_DOCUMENT_BYTES:
        raise ValueError("Document exceeds its size bound.")
    return np.concatenate((np.frombuffer(raw, dtype=np.uint8).astype(np.uint16),
                           np.array([EOD_ID], dtype=np.uint16)))


class Corpus:
    """Load only locally prepared split files after verifying their contracts.

    Checksums establish artifact consistency, not factual correctness or source
    authenticity. Prefix-source limitations remain part of the importer manifest.
    """

    def __init__(self, path):
        self.path = Path(path)
        try:
            with (self.path / "manifest.json").open("rb") as stream:
                raw_manifest = stream.read(1024 * 1024 + 1)
            if len(raw_manifest) > 1024 * 1024:
                raise ValueError("Corpus manifest exceeds its size bound.")
            manifest = _json(raw_manifest)
            if (type(manifest) is not dict or manifest.get("format") != "b3-language-corpus"
                    or type(manifest.get("version")) is not int or manifest["version"] != 1
                    or manifest.get("dataset") != REPOSITORY or manifest.get("revision") != REVISION
                    or manifest.get("license") != LICENSE or manifest.get("synthetic") is not True
                    or manifest.get("individual_documents_reviewed") is not False
                    or type(manifest.get("splits")) is not dict or set(manifest["splits"]) != set(SPLITS)):
                raise ValueError("Unsupported language-corpus manifest.")
            summaries = manifest.get("source_summaries")
            if type(summaries) is not list or not 1 <= len(summaries) <= 2:
                raise ValueError("Invalid corpus source summaries.")
            sources = {}
            for summary in summaries:
                if (type(summary) is not dict or summary.get("source_key") not in ("train", "validation")
                        or summary["source_key"] in sources
                        or not _integer(summary.get("bytes_read"))
                        or not _integer(summary.get("seen_documents"), maximum=100000)
                        or not _checksum(summary.get("actual_prefix_sha256"))):
                    raise ValueError("Invalid corpus source identity or bounds.")
                sources[summary["source_key"]] = summary
                key = summary["source_key"]
                expected = SOURCES[key]
                if (any(summary.get(field) != expected[field] for field in
                        ("filename", "url", "metadata_url", "expected_full_sha256"))
                        or summary.get("revision") != REVISION
                        or summary.get("input_kind") not in ("local", "https")
                        or type(summary.get("full_file")) is not bool
                        or type(summary.get("upstream_identity_verified")) is not bool):
                    raise ValueError("Invalid pinned source metadata.")
                if summary["input_kind"] == "https" and summary.get("input_location") != expected["url"]:
                    raise ValueError("Unexpected remote source location.")
                full, actual = summary["full_file"], summary.get("actual_full_sha256")
                if ((not full and (actual is not None or summary.get("published_sha256_matches") is not None))
                        or (full and (not _checksum(actual) or actual != summary["actual_prefix_sha256"]
                                      or type(summary.get("published_sha256_matches")) is not bool
                                      or summary["published_sha256_matches"] != (actual == expected["expected_full_sha256"])))
                        or summary["upstream_identity_verified"] != (full and actual == expected["expected_full_sha256"])
                        or summary.get("integrity") != ("published whole-file SHA-256 verified" if summary["upstream_identity_verified"]
                                                        else "complete local input differs from the published SHA-256; upstream identity unverified" if full
                                                        else "prefix checksum recorded; published whole-file SHA-256 unverified")):
                    raise ValueError("Inconsistent source checksum verification status.")
            self.splits, total_bytes, seen_ids, seen_normalized = {}, 0, set(), set()
            for split in SPLITS:
                summary = manifest["splits"][split]
                if (type(summary) is not dict or set(summary) != {"documents", "bytes", "text_bytes", "sha256"}
                        or not _integer(summary["documents"], maximum=100000)
                        or not _integer(summary["bytes"]) or not _integer(summary["text_bytes"])
                        or not _checksum(summary["sha256"])):
                    raise ValueError("Invalid split summary.")
                total_bytes += summary["bytes"]
                if total_bytes > MAX_OUTPUT_BYTES:
                    raise ValueError("Corpus exceeds its total byte bound.")
                with (self.path / (split + ".jsonl")).open("rb") as stream:
                    payload = stream.read(summary["bytes"] + 1)
                if len(payload) != summary["bytes"] or _hash(payload) != summary["sha256"]:
                    raise ValueError("Corpus split checksum or size mismatch.")
                documents, text_bytes = [], 0
                for line in payload.splitlines():
                    row = _json(line)
                    source_key = "train" if split == "train" else "validation"
                    if (type(row) is not dict or set(row) != ROW_FIELDS or row["split"] != split
                            or row["source_key"] != source_key or source_key not in sources
                            or not all(_checksum(row[key]) for key in ("id", "raw_sha256", "normalized_sha256"))
                            or not _integer(row["source_document"], 1, sources[source_key]["seen_documents"])
                            or not _integer(row["source_byte_start"])
                            or not _integer(row["source_byte_end"], row["source_byte_start"] + 1, sources[source_key]["bytes_read"])):
                        raise ValueError("Invalid document provenance or split.")
                    document = encode(row["text"])
                    raw = row["text"].encode("utf-8")
                    normal = " ".join(unicodedata.normalize("NFC", row["text"]).casefold().split())
                    normal_hash = _hash(normal.encode("utf-8"))
                    if (_hash(raw) != row["id"] or normal_hash != row["normalized_sha256"]
                            or (split != "train" and split != ("validation" if int(normal_hash[:8], 16) % 2 == 0 else "test"))
                            or len(raw) > row["source_byte_end"] - row["source_byte_start"]
                            or row["id"] in seen_ids or normal_hash in seen_normalized):
                        raise ValueError("Invalid document checksum or duplicate across corpus splits.")
                    seen_ids.add(row["id"])
                    seen_normalized.add(normal_hash)
                    documents.append(document)
                    text_bytes += len(raw)
                if len(documents) != summary["documents"] or text_bytes != summary["text_bytes"]:
                    raise ValueError("Corpus document totals mismatch.")
                self.splits[split] = documents
            if sum(len(documents) for documents in self.splits.values()) > 100000:
                raise ValueError("Corpus exceeds its total document bound.")
            self.manifest, self.manifest_hash = manifest, _hash(raw_manifest)
        except (OSError, KeyError, TypeError, UnicodeError, OverflowError) as error:
            raise ValueError("Cannot safely load the prepared corpus.") from error

    def batch(self, split, rng, batch_size, context_length):
        if (split not in SPLITS or not self.splits[split]
                or not isinstance(rng, np.random.Generator)
                or not _integer(batch_size, 1, 16) or not _integer(context_length, 1, 4096)
                or batch_size * context_length > 16384):
            raise ValueError("Invalid or empty split, generator, batch or context.")
        tokens = np.zeros((batch_size, context_length), dtype=np.int64)
        targets = np.zeros_like(tokens)
        mask = np.zeros(tokens.shape, dtype=np.float32)
        documents = self.splits[split]
        for index in range(batch_size):
            doc = documents[int(rng.integers(len(documents)))]
            start = int(rng.integers(max(1, len(doc) - context_length)))
            size = min(context_length, len(doc) - start - 1)
            tokens[index, :size] = doc[start:start + size]
            targets[index, :size] = doc[start + 1:start + size + 1]
            mask[index, :size] = 1
        return tokens, targets, mask
