# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Reviewed B3 facts mixed with bounded, explicitly simulated tool teaching.

No node is contacted. Source hashes establish immutable identity, not truth.
Validation repeats known concepts with distinct wording and synthetic values;
it is not an independent competence or live-operation exam.
"""

import hashlib
import json
from pathlib import Path
import re

import numpy as np

from ..language.batches import EOD_ID
from ..scratch.checkpoint import _no_duplicate_keys, _reject_constant
from ..teaching.data import Curriculum, _git_blob, _text, normalize_question
from .protocol import request_prompt, result_prompt, validate_action, validate_fixture


SCHEMA = "b3-tool-instruction-curriculum/v1"
MAX_CONTEXT = 1024
MAX_JSON_BYTES = 2 * 1024 * 1024
FACT_PATH = "contrib/b3ai/teaching/b3_curriculum.json"
PROJECTION_PATH = "src/qt/b3aipublicdata.cpp"
CATEGORIES = {"tool_choice", "refusal", "chat", "tool_result", "tool_failure"}
RECORD_FIELDS = {"id", "split", "category", "tools", "question", "answer", "tool_result"}


def _compact(value):
    return json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":"))


def _hash(value):
    return type(value) is str and re.fullmatch(r"[0-9a-f]{64}", value) is not None


class InstructionCorpus:
    """Load a fixed simulator corpus and verify its separately reviewed facts.

    ``curriculum_path`` refers to this stage's curriculum.json, which pins the
    reviewed teaching curriculum and the Git blob containing public projections.
    The latter is read from Git, never from mutable working-tree source files.
    """

    def __init__(self, curriculum_path, context_length=MAX_CONTEXT, repository=None):
        if type(context_length) is not int or not 1 <= context_length <= MAX_CONTEXT:
            raise ValueError("Instruction context must be an integer from 1 through 1024.")
        self.context_length = context_length
        self.path = Path(curriculum_path)
        self.repository = Path(repository) if repository is not None else Path(__file__).resolve().parents[3]
        with self.path.open("rb") as source:
            raw = source.read(MAX_JSON_BYTES + 1)
        if len(raw) > MAX_JSON_BYTES:
            raise ValueError("Instruction curriculum exceeds its 2 MiB bound.")
        try:
            document = json.loads(raw, object_pairs_hook=_no_duplicate_keys, parse_constant=_reject_constant)
        except (ValueError, UnicodeError, RecursionError) as error:
            raise ValueError("Instruction curriculum JSON is invalid.") from error
        if (type(document) is not dict
                or set(document) != {"schema", "baseline_commit", "review", "source_curriculum", "projection_source", "records"}
                or document["schema"] != SCHEMA
                or type(document["baseline_commit"]) is not str
                or re.fullmatch(r"[0-9a-f]{40}", document["baseline_commit"]) is None):
            raise ValueError("Instruction schema or baseline is invalid.")
        self.baseline_commit = document["baseline_commit"]
        review = document["review"]
        if (type(review) is not dict
                or set(review) != {"status", "scope", "not_independent_exam", "simulated_results"}
                or review["status"] != "source-reviewed"
                or review["not_independent_exam"] is not True
                or review["simulated_results"] is not True):
            raise ValueError("Instruction curriculum must identify source review and synthetic, non-exam data.")
        _text(review["scope"], 4096, "instruction review scope")
        reference = document["source_curriculum"]
        if (type(reference) is not dict or set(reference) != {"file", "sha256"}
                or reference["file"] != FACT_PATH or not _hash(reference["sha256"])):
            raise ValueError("Instruction facts must pin the reviewed B3 teaching curriculum.")
        facts = Curriculum(self.repository / FACT_PATH, repository=self.repository)
        if facts.sha256 != reference["sha256"] or facts.baseline_commit != self.baseline_commit:
            raise ValueError("Reviewed factual curriculum hash or baseline does not match.")
        self.source_curriculum_sha256 = facts.sha256
        self.source_hashes = dict(facts.source_hashes)
        projection = document["projection_source"]
        if (type(projection) is not dict
                or set(projection) != {"file", "file_sha256", "line_start", "line_end", "evidence"}
                or projection["file"] != PROJECTION_PATH or not _hash(projection["file_sha256"])
                or type(projection["line_start"]) is not int or type(projection["line_end"]) is not int
                or not 1 <= projection["line_start"] <= projection["line_end"] <= 100000
                or projection["line_end"] - projection["line_start"] > 1000):
            raise ValueError("Public projection source reference is invalid.")
        evidence = _text(projection["evidence"], 16384, "public projection evidence")
        blob = _git_blob(self.repository, self.baseline_commit, PROJECTION_PATH)
        self.projection_source_sha256 = hashlib.sha256(blob).hexdigest()
        lines = blob.splitlines(keepends=True)
        if (self.projection_source_sha256 != projection["file_sha256"]
                or projection["line_end"] > len(lines)
                or evidence not in b"".join(lines[projection["line_start"] - 1:projection["line_end"]])):
            raise ValueError("Public projection Git hash or exact line evidence does not match.")
        if PROJECTION_PATH in self.source_hashes and self.source_hashes[PROJECTION_PATH] != self.projection_source_sha256:
            raise ValueError("Factual and simulator projection provenance disagree.")
        self.source_hashes[PROJECTION_PATH] = self.projection_source_sha256
        extras = document["records"]
        if type(extras) is not list or not 1 <= len(extras) <= 2048:
            raise ValueError("Instruction curriculum must contain from 1 through 2048 synthetic records.")
        self.records = {"train": [], "validation": []}
        self._questions, self._ids = set(), set()
        for split, factual_records in facts.records.items():
            for index, record in enumerate(factual_records):
                # Knowing facts does not require a live tool; exercise both permission modes.
                tools = "enabled" if index % 2 == 0 else "disabled"
                self._add(record["id"], split, "b3_fact", tools, record["question"],
                          {"reply": record["answer"]}, None, "source-reviewed")
        for record in extras:
            if (type(record) is not dict or set(record) != RECORD_FIELDS
                    or type(record["id"]) is not str
                    or re.fullmatch(r"[a-z0-9][a-z0-9_.-]{0,95}", record["id"]) is None
                    or type(record["category"]) is not str or record["category"] not in CATEGORIES):
                raise ValueError("Synthetic instruction record fields, ID or category are invalid.")
            self._add(record["id"], record["split"], record["category"], record["tools"],
                      record["question"], record["answer"], record["tool_result"], "teacher-synthetic")
        if not all(self.records.values()):
            raise ValueError("Instruction training and validation must both be nonempty.")
        self.sha256 = hashlib.sha256(raw).hexdigest()
        self.review = review
        self.source_review = facts.review

    def _add(self, identity, split, category, tools, question, answer, tool_result, provenance):
        if split not in ("train", "validation") or tools not in ("enabled", "disabled"):
            raise ValueError("Instruction split or tool-permission mode is invalid.")
        _text(question, 256, "instruction question", single_line=True)
        normalized = normalize_question(question)
        if normalized in self._questions or identity in self._ids:
            raise ValueError("Instruction IDs and normalized user questions must be globally unique.")
        self._questions.add(normalized)
        self._ids.add(identity)
        if type(answer) is not dict:
            raise ValueError("Instruction answer must be a protocol JSON object.")
        target_json = _compact(answer)
        validate_action(target_json, tools_enabled=tools == "enabled")
        if "reply" in answer:
            _text(answer["reply"], 512, "instruction reply", single_line=True)
        if category == "tool_choice" and ("tool" not in answer or tool_result is not None):
            raise ValueError("A tool-choice target must be a permitted call without a result.")
        if category != "tool_choice" and "reply" not in answer:
            raise ValueError("Facts, chat, refusals and result explanations must be reply targets.")
        prompt = request_prompt(question, tools_enabled=tools == "enabled")
        if tool_result is not None:
            if (tools != "enabled" or category not in ("tool_result", "tool_failure")
                    or type(tool_result) is not dict or set(tool_result) != {"tool", "envelope"}):
                raise ValueError("A simulator continuation requires enabled tools and a labeled result.")
            action = {"tool": tool_result["tool"], "arguments": {}}
            validate_action(_compact(action), tools_enabled=True)
            validate_fixture(tool_result["tool"], tool_result["envelope"])
            expected_ok = category == "tool_result"
            if tool_result["envelope"]["ok"] is not expected_ok or not answer["reply"].startswith("Simulated"):
                raise ValueError("Simulator target must match success/failure and identify itself as simulated.")
            prompt = result_prompt(question, action, tool_result["envelope"], tools_enabled=True)
        elif category in ("tool_result", "tool_failure"):
            raise ValueError("Result explanations require their explicit simulated envelope.")
        if type(prompt) is not str or not prompt.endswith("\nAssistant: "):
            raise ValueError("Protocol prompt does not have the expected assistant boundary.")
        text = prompt + target_json + "\n"
        record = {"id": identity, "category": category, "tools": tools, "question": question,
                  "prompt": prompt, "answer": answer, "target": answer, "target_json": target_json,
                  "tool_result": tool_result, "provenance": provenance,
                  "simulated": tool_result is not None, "text": text,
                  "answer_start": len(prompt.encode("utf-8"))}
        self.arrays(record)  # Reject complete-turn overflow before training; never crop.
        self.records[split].append(record)

    def arrays(self, record):
        raw = record["text"].encode("utf-8", errors="strict")
        if len(raw) + 1 > self.context_length:
            raise ValueError("Whole instruction turn exceeds its context; cropping is forbidden.")
        sequence = np.concatenate((np.frombuffer(raw, dtype=np.uint8).astype(np.int64),
                                   np.array([EOD_ID], dtype=np.int64)))
        tokens, targets = sequence[:-1].copy(), sequence[1:].copy()
        mask = np.zeros(tokens.shape, dtype=np.float32)
        mask[record["answer_start"] - 1:] = 1
        return tokens, targets, mask

    def _batch(self, records):
        arrays = [self.arrays(record) for record in records]
        width = max(len(array[0]) for array in arrays)
        tokens = np.zeros((len(arrays), width), dtype=np.int64)
        targets = np.zeros_like(tokens)
        mask = np.zeros(tokens.shape, dtype=np.float32)
        for index, (inputs, outputs, weights) in enumerate(arrays):
            size = len(inputs)
            tokens[index, :size], targets[index, :size], mask[index, :size] = inputs, outputs, weights
        return tokens, targets, mask

    @staticmethod
    def _batch_size(value):
        if type(value) is not int or not 1 <= value <= 16:
            raise ValueError("Instruction batch size must be an integer from 1 through 16.")

    def batch(self, split, rng, batch_size):
        self._batch_size(batch_size)
        if type(split) is not str or split not in self.records or not isinstance(rng, np.random.Generator):
            raise ValueError("Invalid instruction split or batch generator.")
        records = self.records[split]
        return self._batch([records[int(rng.integers(len(records)))] for _ in range(batch_size)])

    def evaluation_batches(self, split, batch_size):
        self._batch_size(batch_size)
        if type(split) is not str or split not in self.records:
            raise ValueError("Invalid instruction evaluation split.")
        records = self.records[split]
        for start in range(0, len(records), batch_size):
            yield self._batch(records[start:start + batch_size])
