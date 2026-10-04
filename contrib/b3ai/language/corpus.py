# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Bounded TinyStories preparation; offline unless --fetch is explicitly given.

Stories are synthetic fiction, not reviewed B3 facts. The original training
file feeds train only. The original validation file feeds deterministic
validation/test subsets. Documents are deduplicated before tokenization, and
never split into several corpus partitions. This prepares data, not a model.
"""

import argparse
import contextlib
import hashlib
import json
from pathlib import Path
import re
import unicodedata
import urllib.parse
import urllib.request


VERSION = 1
REVISION = "f54c09fd23315a6f9c86f9dc80f725de7d8f9c64"
REPOSITORY = "roneneldan/TinyStories"
LICENSE = "CDLA-Sharing-1.0"
LICENSE_URL = "https://cdla.dev/sharing-1-0/"
DELIMITER = b"<|endoftext|>"
DEFAULT_MAX_BYTES = 32 * 1024 * 1024
DEFAULT_MAX_DOCUMENTS = 20000
MAX_SOURCE_BYTES = 128 * 1024 * 1024
MAX_OUTPUT_BYTES = 128 * 1024 * 1024
MAX_DOCUMENT_BYTES = 65536
MAX_DOCUMENTS = 100000
SOURCES = {
    "train": {
        "filename": "TinyStories-train.txt",
        "expected_full_sha256": "c5cf5e22ff13614e830afbe61a99fbcbe8bcb7dd72252b989fa1117a368d401f",
    },
    "validation": {
        "filename": "TinyStories-valid.txt",
        "expected_full_sha256": "94e431816c4cce81ff71e4408ff8d3bda9a42e8d2663986697c3954288cb38b4",
    },
}
for _source in SOURCES.values():
    _source["url"] = f"https://huggingface.co/datasets/{REPOSITORY}/resolve/{REVISION}/{_source['filename']}"
    _source["metadata_url"] = f"https://huggingface.co/datasets/{REPOSITORY}/blob/{REVISION}/{_source['filename']}"


def _json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


def _canonical_text(raw):
    if b"\0" in raw:
        raise ValueError("Source text contains NUL bytes.")
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise ValueError("Source text contains invalid or incomplete UTF-8.") from error
    return text.replace("\r\n", "\n").replace("\r", "\n").strip()


def _normalized_hash(text):
    normalized = " ".join(unicodedata.normalize("NFC", text).casefold().split())
    return hashlib.sha256(normalized.encode("utf-8")).hexdigest()


def _limits(max_bytes, max_documents, min_document_bytes, chunk_size):
    for name, value, lower, upper in (
        ("max_bytes", max_bytes, 1, MAX_SOURCE_BYTES),
        ("max_documents", max_documents, 1, MAX_DOCUMENTS),
        ("min_document_bytes", min_document_bytes, 1, MAX_DOCUMENT_BYTES),
        ("chunk_size", chunk_size, 1, 1024 * 1024),
    ):
        if type(value) is not int or not lower <= value <= upper:
            raise ValueError(f"{name} must be an integer from {lower} to {upper}.")


def _budgets(total, count):
    if count == 1:
        return [total]
    if total < 2:
        raise ValueError("Two input sources require budgets of at least two.")
    validation = max(1, total // 10)
    return [total - validation, validation]


class _OfficialRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, msg, headers, newurl):
        parsed = urllib.parse.urlsplit(newurl)
        host = (parsed.hostname or "").lower()
        official = host == "huggingface.co" or host.endswith(".huggingface.co") or host.endswith(".hf.co")
        if parsed.scheme != "https" or parsed.username or parsed.password or not official:
            raise ValueError("Refusing a redirect outside Hugging Face's HTTPS distribution hosts.")
        return super().redirect_request(request, fp, code, msg, headers, newurl)


@contextlib.contextmanager
def _remote_source(key, byte_budget):
    request = urllib.request.Request(SOURCES[key]["url"], headers={
        "Range": f"bytes=0-{byte_budget - 1}", "Accept-Encoding": "identity",
        "User-Agent": "B3-language-corpus/1",
    })
    opener = urllib.request.build_opener(_OfficialRedirect())
    with opener.open(request, timeout=30) as response:
        if response.headers.get("Content-Encoding", "identity").lower() != "identity":
            raise ValueError("Compressed HTTP transport cannot provide exact source byte offsets.")
        status = response.getcode()
        length = response.headers.get("Content-Length")
        if length is not None and (not length.isdecimal() or int(length) <= 0):
            raise ValueError("Invalid HTTP source length.")
        length = int(length) if length is not None else None
        if status == 206:
            match = re.fullmatch(r"bytes 0-(\d+)/(\d+)", response.headers.get("Content-Range", ""))
            if match is None:
                raise ValueError("The source must return a range beginning at byte zero.")
            end, total = map(int, match.groups())
            if not 0 <= end < total or end + 1 > byte_budget or (length is not None and length != end + 1):
                raise ValueError("The HTTP source range is inconsistent with the byte budget.")
            advertised = end + 1
        elif status == 200 and not response.headers.get("Content-Range"):
            total, advertised = length, length
        else:
            raise ValueError("Unexpected HTTP response for the pinned source file.")
        yield response, total, advertised


def _read_source(stream, key, byte_budget, document_budget, min_document_bytes,
                 chunk_size, source_size, advertised_size, kind, location,
                 records, exact_seen, normalized_seen):
    summary = {
        "source_key": key, **SOURCES[key], "revision": REVISION,
        "input_kind": kind, "input_location": location,
        "source_identity": "pinned HTTPS source" if kind == "https" else "caller-declared local source; identity unverified unless full hash matches",
        "byte_budget": byte_budget, "document_budget": document_budget,
        "bytes_received": 0, "bytes_read": 0, "source_size": source_size,
        "seen_documents": 0, "accepted_documents": 0,
        "duplicates_exact": 0, "duplicates_normalized": 0, "too_short": 0,
        "discarded_incomplete_tail_bytes": 0, "trailing_whitespace_bytes": 0,
    }
    digest = hashlib.sha256()
    buffer = b""
    buffer_start = 0
    stop = None
    while summary["bytes_received"] < byte_budget:
        old_received = summary["bytes_received"]
        chunk = stream.read(min(chunk_size, byte_budget - old_received))
        if not chunk:
            stop = "eof"
            break
        if not isinstance(chunk, bytes):
            raise ValueError("Source streams must provide binary bytes.")
        summary["bytes_received"] += len(chunk)
        if summary["bytes_received"] > byte_budget:
            raise ValueError("Source reader exceeded its byte budget.")
        old_digest = digest.copy()
        digest.update(chunk)
        buffer += chunk
        while True:
            boundary = buffer.find(DELIMITER)
            if boundary < 0:
                break
            raw = buffer[:boundary]
            if len(raw) > MAX_DOCUMENT_BYTES:
                raise ValueError("A source document exceeds the 64 KiB document bound.")
            text = _canonical_text(raw)
            summary["seen_documents"] += 1
            raw_hash = hashlib.sha256(raw).hexdigest()
            normalized_hash = _normalized_hash(text)
            if len(text.encode("utf-8")) < min_document_bytes:
                summary["too_short"] += 1
            elif raw_hash in exact_seen:
                summary["duplicates_exact"] += 1
            elif normalized_hash in normalized_seen:
                summary["duplicates_normalized"] += 1
            else:
                exact_seen.add(raw_hash)
                normalized_seen.add(normalized_hash)
                identifier = hashlib.sha256(text.encode("utf-8")).hexdigest()
                split = "train" if key == "train" else ("validation" if int(normalized_hash[:8], 16) % 2 == 0 else "test")
                records[split].append({
                    "id": identifier, "text": text, "split": split,
                    "source_key": key, "source_document": summary["seen_documents"],
                    "source_byte_start": buffer_start, "source_byte_end": buffer_start + boundary,
                    "raw_sha256": raw_hash, "normalized_sha256": normalized_hash,
                })
                summary["accepted_documents"] += 1
            consumed = boundary + len(DELIMITER)
            buffer_start += consumed
            buffer = buffer[consumed:]
            if summary["accepted_documents"] >= document_budget:
                # Only the prefix through this complete delimiter is admitted.
                # The bounded chunk lookahead is reported separately as received.
                digest = old_digest
                digest.update(chunk[:buffer_start - old_received])
                summary["bytes_read"] = buffer_start
                buffer = b""
                stop = "document_limit"
                break
        if stop:
            break
        if len(buffer) > MAX_DOCUMENT_BYTES + len(DELIMITER) - 1:
            raise ValueError("An unterminated document exceeds the 64 KiB document bound.")
    if stop != "document_limit":
        summary["bytes_read"] = summary["bytes_received"]
        # This also rejects byte prefixes cut halfway through a Unicode character.
        _canonical_text(buffer)
        if buffer.strip():
            summary["discarded_incomplete_tail_bytes"] = len(buffer)
        else:
            summary["trailing_whitespace_bytes"] = len(buffer)
    if stop is None:
        stop = "eof" if source_size is not None and summary["bytes_read"] == source_size else "byte_limit"
    if stop == "eof" and advertised_size is not None and summary["bytes_received"] < min(advertised_size, byte_budget):
        raise ValueError("Source ended before its advertised length.")
    full = source_size is not None and summary["bytes_read"] == source_size
    actual = digest.hexdigest()
    verified = full and actual == SOURCES[key]["expected_full_sha256"]
    if kind == "https" and full and not verified:
        raise ValueError("The complete pinned source failed its published SHA-256 check.")
    summary.update({
        "stop_reason": stop, "full_file": full,
        "actual_prefix_sha256": actual, "actual_full_sha256": actual if full else None,
        "upstream_identity_verified": verified,
        "published_sha256_matches": verified if full else None,
        "integrity": ("published whole-file SHA-256 verified" if verified else
            "complete local input differs from the published SHA-256; upstream identity unverified" if full else
            "prefix checksum recorded; published whole-file SHA-256 unverified"),
    })
    return summary


def _prepare(inputs, output, *, max_bytes, max_documents, min_document_bytes, chunk_size):
    _limits(max_bytes, max_documents, min_document_bytes, chunk_size)
    byte_budgets = _budgets(max_bytes, len(inputs))
    document_budgets = _budgets(max_documents, len(inputs))
    output = Path(output)
    # Reserving the directory now prevents two long preparations from replacing
    # one another's artifacts after both observed an empty destination.
    output.mkdir(parents=True, exist_ok=False)
    owned = []
    try:
        records = {name: [] for name in ("train", "validation", "test")}
        exact_seen, normalized_seen, summaries = set(), set(), []
        for (key, kind, path), byte_budget, document_budget in zip(inputs, byte_budgets, document_budgets):
            if kind == "local":
                path = Path(path)
                if not path.is_file():
                    raise ValueError("A local source must be a regular TXT file.")
                with path.open("rb") as stream:
                    size = path.stat().st_size
                    summaries.append(_read_source(stream, key, byte_budget, document_budget,
                        min_document_bytes, chunk_size, size, size, kind, str(path.resolve()),
                        records, exact_seen, normalized_seen))
            else:
                with _remote_source(key, byte_budget) as (stream, size, advertised):
                    summaries.append(_read_source(stream, key, byte_budget, document_budget,
                        min_document_bytes, chunk_size, size, advertised, kind, SOURCES[key]["url"],
                        records, exact_seen, normalized_seen))
        split_manifest, output_bytes = {}, 0
        for split, rows in records.items():
            data = b"".join(_json_bytes(row) for row in rows)
            output_bytes += len(data)
            if output_bytes > MAX_OUTPUT_BYTES:
                raise ValueError("Prepared JSONL exceeds the 128 MiB output bound.")
            path = output / f"{split}.jsonl"
            with path.open("xb") as stream:
                owned.append(path)
                stream.write(data)
            split_manifest[split] = {"documents": len(rows), "bytes": len(data),
                "text_bytes": sum(len(row["text"].encode("utf-8")) for row in rows),
                "sha256": hashlib.sha256(data).hexdigest()}
        attribution = {
            "dataset": REPOSITORY, "revision": REVISION,
            "data_providers": ["Ronen Eldan", "Yuanzhi Li"],
            "dataset_card": f"https://huggingface.co/datasets/{REPOSITORY}/blob/{REVISION}/README.md",
            "paper": "https://arxiv.org/abs/2305.07759", "license": LICENSE, "license_url": LICENSE_URL,
            "modified": True,
            "modifications": ["bounded document subset", "outer whitespace removed", "line endings normalized",
                "exact and normalized duplicates removed", "official validation divided into validation and test"],
            "publication_notice": "Data and modified subsets retain CDLA-Sharing-1.0 and provider attribution; code has its own license.",
        }
        manifest = {
            "format": "b3-language-corpus", "version": VERSION,
            "dataset": REPOSITORY, "revision": REVISION, "license": LICENSE,
            "source_summaries": summaries, "splits": split_manifest,
            "budgets": {"source_bytes_total": max_bytes, "documents_total": max_documents,
                "output_bytes_hard_limit": MAX_OUTPUT_BYTES, "min_document_bytes": min_document_bytes,
                "max_document_bytes": MAX_DOCUMENT_BYTES, "chunk_size": chunk_size},
            "synthetic": True, "individual_documents_reviewed": False,
            "purpose": "language pretraining fiction; not B3 knowledge, dialogue competence or tool behavior",
            "split_policy": "original train remains train; original validation SHA-256 parity gives validation/test; global dedup first, train wins",
            "deduplication": "raw SHA-256 exact; NFC+casefold+collapsed whitespace SHA-256 normalized",
            "original_delimiter": DELIMITER.decode("ascii"),
            "training_ready": all(split_manifest[name]["documents"] for name in split_manifest),
            "limits": "A bounded prefix is not a representative random sample. Exact/normalized dedup is not semantic near-duplicate detection. Test must stay out of training and checkpoint selection.",
        }
        for name, value in (("attribution.json", attribution), ("manifest.json", manifest)):
            path = output / name
            with path.open("xb") as stream:
                owned.append(path)
                stream.write(_json_bytes(value))
        return manifest
    except BaseException:
        # Remove only artifacts created by this call. Preserve any unexpected
        # file another process placed in the reserved directory.
        for path in owned:
            path.unlink(missing_ok=True)
        try:
            output.rmdir()
        except OSError:
            pass
        raise


def prepare_corpus(source, output, *, validation_source=None, source_split="train",
                   max_bytes=DEFAULT_MAX_BYTES, max_documents=DEFAULT_MAX_DOCUMENTS,
                   min_document_bytes=64, chunk_size=65536):
    """Prepare local TXT only; local source identity is explicitly caller-declared."""
    if source_split not in SOURCES:
        raise ValueError("Choose source_split train or validation.")
    if validation_source is not None and source_split != "train":
        raise ValueError("A separate validation source requires a train source first.")
    inputs = [(source_split, "local", source)]
    if validation_source is not None:
        inputs.append(("validation", "local", validation_source))
    return _prepare(inputs, output, max_bytes=max_bytes, max_documents=max_documents,
        min_document_bytes=min_document_bytes, chunk_size=chunk_size)


def prepare_fetch(output, *, max_bytes=DEFAULT_MAX_BYTES, max_documents=DEFAULT_MAX_DOCUMENTS,
                  min_document_bytes=64, chunk_size=65536):
    """Explicit bounded HTTPS import of the two known pinned official files only."""
    return _prepare([("train", "https", None), ("validation", "https", None)], output,
        max_bytes=max_bytes, max_documents=max_documents,
        min_document_bytes=min_document_bytes, chunk_size=chunk_size)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, help="Local original train or validation TXT; no network")
    parser.add_argument("--validation-source", type=Path, help="Optional separate local official validation TXT")
    parser.add_argument("--source-split", choices=tuple(SOURCES), default="train")
    parser.add_argument("--fetch", action="store_true", help="Explicitly fetch bounded prefixes of only the pinned official train/validation files")
    parser.add_argument("--output", type=Path, required=True, help="New output directory; existing directories are refused")
    parser.add_argument("--max-bytes", type=int, default=DEFAULT_MAX_BYTES)
    parser.add_argument("--max-documents", type=int, default=DEFAULT_MAX_DOCUMENTS)
    parser.add_argument("--min-document-bytes", type=int, default=64)
    parser.add_argument("--chunk-size", type=int, default=65536)
    args = parser.parse_args(argv)
    if args.fetch and (args.source is not None or args.validation_source is not None or args.source_split != "train"):
        parser.error("--fetch cannot be combined with local source arguments.")
    if not args.fetch and args.source is None:
        parser.error("Provide --source for offline preparation, or explicitly choose --fetch.")
    try:
        options = dict(max_bytes=args.max_bytes, max_documents=args.max_documents,
            min_document_bytes=args.min_document_bytes, chunk_size=args.chunk_size)
        if args.fetch:
            manifest = prepare_fetch(args.output, **options)
        else:
            manifest = prepare_corpus(args.source, args.output, validation_source=args.validation_source,
                source_split=args.source_split, **options)
        print(json.dumps({"output": str(args.output), "splits": manifest["splits"],
            "training_ready": manifest["training_ready"], "synthetic": True}, sort_keys=True))
    except (OSError, ValueError, TypeError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
