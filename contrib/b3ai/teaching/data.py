# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Pinned-source B3 lessons and whole-answer supervised byte/EOD batches."""

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import unicodedata

import numpy as np

from ..language.batches import EOD_ID
from ..scratch.checkpoint import _no_duplicate_keys, _reject_constant


SCHEMA = "b3-teaching-curriculum/v1"
MAX_JSON_BYTES = 1024 * 1024
MAX_SOURCE_BYTES = 4 * 1024 * 1024
MAX_TOTAL_SOURCE_BYTES = 16 * 1024 * 1024
MAX_CONTEXT = 512
TOPIC_FIELDS = {"id", "status", "questions", "validation_question", "answer", "sources", "limits"}
SOURCE_FIELDS = {"file", "line_start", "line_end", "file_sha256", "evidence"}


def _text(value, maximum, name, single_line=False):
    if type(value) is not str or not value.strip() or "\0" in value:
        raise ValueError(f"{name} must be nonempty text without NUL.")
    try:
        raw = value.encode("utf-8", errors="strict")
    except UnicodeError as error:
        raise ValueError(f"{name} is not valid UTF-8.") from error
    if len(raw) > maximum or (single_line and any(char in value for char in "\r\n")):
        raise ValueError(f"{name} exceeds its byte bound or framing limits.")
    return raw


def normalize_question(text):
    return " ".join(unicodedata.normalize("NFC", text).casefold().split())


def _checksum(value):
    return type(value) is str and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _source_path(value):
    if type(value) is not str or len(value) > 256 or "\\" in value:
        return False
    path = PurePosixPath(value)
    if path.is_absolute() or any(part in (".", "..") for part in value.split("/")):
        return False
    return (value in ("README.md", "CLAUDE.md", "src/CMakeLists.txt", "src/qt/CMakeLists.txt")
            or re.fullmatch(r"doc/design/[A-Za-z0-9_-][A-Za-z0-9_.-]*\.md", value) is not None
            or re.fullmatch(r"src/(?:[A-Za-z0-9_-][A-Za-z0-9_.-]*/)*[A-Za-z0-9_-][A-Za-z0-9_.-]*\.(?:cpp|h)", value) is not None)


def _git_blob(repository, baseline, path):
    # No shell, text conversion, working-tree reads, replace objects or network.
    environment = dict(os.environ, GIT_NO_REPLACE_OBJECTS="1")
    process = subprocess.Popen(["git", "show", baseline + ":" + path], cwd=repository,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               env=environment)
    try:
        raw = process.stdout.read(MAX_SOURCE_BYTES + 1)
        if len(raw) > MAX_SOURCE_BYTES:
            raise ValueError("Pinned source exceeds its 4 MiB bound.")
        if process.wait(timeout=10) != 0:
            raise ValueError("Pinned source is missing from the baseline commit.")
        raw.decode("utf-8", errors="strict")
        return raw
    except (UnicodeError, subprocess.TimeoutExpired) as error:
        raise ValueError("Pinned source is invalid or could not be read.") from error
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
        process.stdout.close()


class Curriculum:
    """Validate immutable evidence, then expose train and validation answers.

    Evidence checks prove that the cited text exists in a pinned source. They
    do not automatically establish that a teacher's answer follows from it.
    Source review remains explicit, and this corpus is never an independent exam.
    """

    def __init__(self, path, context_length=MAX_CONTEXT, repository=None):
        if type(context_length) is not int or not 1 <= context_length <= MAX_CONTEXT:
            raise ValueError("Teaching context must be an integer from 1 through 512.")
        self.context_length = context_length
        self.path = Path(path)
        self.repository = Path(repository) if repository is not None else Path(__file__).resolve().parents[3]
        with self.path.open("rb") as source:
            raw = source.read(MAX_JSON_BYTES + 1)
        if len(raw) > MAX_JSON_BYTES:
            raise ValueError("Curriculum exceeds its 1 MiB bound.")
        try:
            document = json.loads(raw, object_pairs_hook=_no_duplicate_keys, parse_constant=_reject_constant)
        except (ValueError, UnicodeError, RecursionError) as error:
            raise ValueError("Curriculum JSON is invalid.") from error
        if (type(document) is not dict or set(document) != {"schema", "baseline_commit", "review", "topics"}
                or document["schema"] != SCHEMA or type(document["baseline_commit"]) is not str
                or re.fullmatch(r"[0-9a-f]{40}", document["baseline_commit"]) is None):
            raise ValueError("Curriculum schema or immutable baseline is invalid.")
        self.baseline_commit = document["baseline_commit"]
        commit = subprocess.run(["git", "cat-file", "-t", self.baseline_commit], cwd=self.repository,
                                capture_output=True, timeout=10,
                                env=dict(os.environ, GIT_NO_REPLACE_OBJECTS="1"))
        if commit.returncode or commit.stdout != b"commit\n":
            raise ValueError("Curriculum baseline must identify a local Git commit.")
        review = document["review"]
        if (type(review) is not dict or set(review) != {"status", "scope", "not_independent_exam"}
                or review["status"] != "source-reviewed" or review["not_independent_exam"] is not True):
            raise ValueError("Curriculum must record source review and exclude exam use.")
        _text(review["scope"], 4096, "review scope")
        topics = document["topics"]
        if type(topics) is not list or not 1 <= len(topics) <= 64:
            raise ValueError("Curriculum must contain from 1 through 64 topics.")
        self.records = {"train": [], "validation": []}
        self.source_hashes, blobs, question_ids, topic_ids = {}, {}, set(), set()
        total_source_bytes = 0
        for topic in topics:
            if (type(topic) is not dict or set(topic) != TOPIC_FIELDS
                    or type(topic["id"]) is not str or re.fullmatch(r"[a-z0-9][a-z0-9_.-]{0,63}", topic["id"]) is None
                    or topic["id"] in topic_ids or topic["status"] not in ("implemented", "design", "implemented-with-limits")
                    or type(topic["questions"]) is not list or len(topic["questions"]) != 4
                    or type(topic["sources"]) is not list or not 1 <= len(topic["sources"]) <= 4):
                raise ValueError("Curriculum topic fields, identity or bounds are invalid.")
            topic_ids.add(topic["id"])
            _text(topic["answer"], 180, "answer", single_line=True)
            _text(topic["limits"], 2048, "topic limits")
            source_ids = set()
            for source in topic["sources"]:
                if (type(source) is not dict or set(source) != SOURCE_FIELDS or not _source_path(source["file"])
                        or not _checksum(source["file_sha256"])
                        or type(source["line_start"]) is not int or type(source["line_end"]) is not int
                        or not 1 <= source["line_start"] <= source["line_end"] <= 100000
                        or source["line_end"] - source["line_start"] > 1000):
                    raise ValueError("Curriculum source reference is invalid.")
                evidence = _text(source["evidence"], 16384, "source evidence")
                identity = (source["file"], source["line_start"], source["line_end"], evidence)
                if identity in source_ids:
                    raise ValueError("Duplicate source evidence within a topic.")
                source_ids.add(identity)
                if source["file"] not in blobs:
                    blob = _git_blob(self.repository, self.baseline_commit, source["file"])
                    total_source_bytes += len(blob)
                    if total_source_bytes > MAX_TOTAL_SOURCE_BYTES:
                        raise ValueError("Curriculum source snapshots exceed their total bound.")
                    blobs[source["file"]] = blob
                    self.source_hashes[source["file"]] = hashlib.sha256(blob).hexdigest()
                blob = blobs[source["file"]]
                lines = blob.splitlines(keepends=True)
                if (source["file_sha256"] != self.source_hashes[source["file"]]
                        or source["line_end"] > len(lines)
                        or evidence not in b"".join(lines[source["line_start"] - 1:source["line_end"]])):
                    raise ValueError("Pinned source hash or exact evidence does not match its declared lines.")
            for split, questions in (("train", topic["questions"]), ("validation", [topic["validation_question"]])):
                for question in questions:
                    _text(question, 256, "question", single_line=True)
                    normalized = normalize_question(question)
                    if normalized in question_ids:
                        raise ValueError("Duplicate normalized question within or across curriculum splits.")
                    question_ids.add(normalized)
                    prefix = "\nUser: " + question + "\nAssistant: "
                    text = prefix + topic["answer"] + "\n"
                    record = {"id": hashlib.sha256((topic["id"] + "\0" + split + "\0" + text).encode("utf-8")).hexdigest(),
                              "topic_id": topic["id"], "question": question, "answer": topic["answer"],
                              "text": text, "answer_start": len(prefix.encode("utf-8"))}
                    self.arrays(record)  # Reject oversize records before any training begins.
                    self.records[split].append(record)
        self.sha256 = hashlib.sha256(raw).hexdigest()
        self.review, self.topics = review, topics

    def arrays(self, record):
        raw = record["text"].encode("utf-8", errors="strict")
        if len(raw) + 1 > self.context_length:
            raise ValueError("Whole teaching record exceeds its context; cropping is forbidden.")
        sequence = np.concatenate((np.frombuffer(raw, dtype=np.uint8).astype(np.int64),
                                   np.array([EOD_ID], dtype=np.int64)))
        tokens, targets = sequence[:-1].copy(), sequence[1:].copy()
        mask = np.zeros(tokens.shape, dtype=np.float32)
        mask[record["answer_start"] - 1:] = 1
        return tokens, targets, mask

    def _batch(self, records):
        arrays = [self.arrays(record) for record in records]
        time = max(len(batch[0]) for batch in arrays)
        tokens = np.zeros((len(arrays), time), dtype=np.int64)
        targets = np.zeros_like(tokens)
        mask = np.zeros(tokens.shape, dtype=np.float32)
        for index, (inputs, outputs, weights) in enumerate(arrays):
            size = len(inputs)
            tokens[index, :size], targets[index, :size], mask[index, :size] = inputs, outputs, weights
        return tokens, targets, mask

    @staticmethod
    def _batch_size(value):
        if type(value) is not int or not 1 <= value <= 16:
            raise ValueError("Teaching batch size must be an integer from 1 through 16.")

    def batch(self, split, rng, batch_size):
        self._batch_size(batch_size)
        if split not in self.records or not isinstance(rng, np.random.Generator):
            raise ValueError("Invalid teaching split or batch generator.")
        records = self.records[split]
        return self._batch([records[int(rng.integers(len(records)))] for _ in range(batch_size)])

    def evaluation_batches(self, split, batch_size):
        self._batch_size(batch_size)
        if split not in self.records:
            raise ValueError("Invalid teaching evaluation split.")
        records = self.records[split]
        for start in range(0, len(records), batch_size):
            yield self._batch(records[start:start + batch_size])
