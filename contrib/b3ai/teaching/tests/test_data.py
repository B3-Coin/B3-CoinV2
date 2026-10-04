# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Whole-turn supervision and pinned-source curriculum integrity checks."""

from collections import Counter
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

from contrib.b3ai.teaching.data import Curriculum


def digest(raw):
    return hashlib.sha256(raw).hexdigest()


class CurriculumTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.repository = Path(__file__).resolve().parents[4]
        # Read the existing immutable commit; fixtures never create or change Git state.
        cls.baseline = subprocess.check_output(
            ["git", "-C", str(cls.repository), "rev-parse", "HEAD"], text=True).strip()
        cls.source_blob = subprocess.check_output(
            ["git", "-C", str(cls.repository), "show", cls.baseline + ":README.md"])
        cls.source_sha = digest(cls.source_blob)
        cls.source_lines = cls.source_blob.decode("utf-8").splitlines()

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.path = Path(temporary.name) / "curriculum.json"
        self.document = {
            "schema": "b3-teaching-curriculum/v1",
            "baseline_commit": self.baseline,
            "review": {
                "status": "source-reviewed",
                "scope": "Synthetic loader fixtures checked against a pinned README excerpt.",
                "not_independent_exam": True,
            },
            "topics": [self.topic(index) for index in range(3)],
        }
        self.write_document()

    def topic(self, index):
        # Vary prompt and answer byte lengths, including a multibyte first answer byte.
        answers = ["Élan café 🐝.", "B3 Hive.", "A complete, deliberately longer fixture answer."]
        return {
            "id": "fixture-" + str(index),
            "status": ("implemented", "design", "implemented-with-limits")[index % 3],
            "questions": ["Question " + str(index) + "/" + str(number) + "?" + " Details" * number
                          for number in range(4)],
            "validation_question": "Held-out question for fixture " + str(index) + "?",
            "answer": answers[index % len(answers)],
            "sources": [{
                "file": "README.md",
                "line_start": 1,
                "line_end": 1,
                "file_sha256": self.source_sha,
                "evidence": self.source_lines[0],
            }],
            "limits": "Synthetic tests exercise representation, not factual assessment.",
        }

    def write_document(self, document=None):
        value = self.document if document is None else document
        self.path.write_bytes((json.dumps(value, ensure_ascii=False) + "\n").encode("utf-8"))

    def load(self, context_length=512):
        return Curriculum(self.path, context_length=context_length, repository=self.repository)

    def assert_invalid(self, mutate):
        document = copy.deepcopy(self.document)
        mutate(document)
        self.write_document(document)
        with self.assertRaises(ValueError):
            self.load()

    def expected_arrays(self, record):
        # Compute byte shifts independently from the returned record offset/mask.
        prefix = ("\nUser: " + record["question"] + "\nAssistant: ").encode("utf-8")
        raw = prefix + record["answer"].encode("utf-8") + b"\n"
        full = list(raw) + [256]
        mask = [0] * (len(prefix) - 1) + [1] * (len(full) - len(prefix))
        return full[:-1], full[1:], mask, len(prefix)

    def assert_complete_batch(self, loaded, split, batch):
        tokens, targets, masks = batch
        self.assertEqual(tokens.ndim, 2)
        self.assertEqual(targets.shape, tokens.shape)
        self.assertEqual(masks.shape, tokens.shape)
        self.assertEqual(tokens.dtype, np.dtype(np.int64))
        self.assertEqual(targets.dtype, np.dtype(np.int64))
        self.assertEqual(masks.dtype, np.dtype(np.float32))
        matched = []
        for input_row, target_row, mask_row in zip(tokens, targets, masks):
            candidates = []
            for record in loaded.records[split]:
                expected_input, expected_target, expected_mask, _ = self.expected_arrays(record)
                length = len(expected_input)
                if length <= len(input_row) and input_row[:length].tolist() == expected_input:
                    candidates.append((record, expected_target, expected_mask, length))
            self.assertEqual(len(candidates), 1, "Each padded row must contain one entire known turn.")
            record, expected_target, expected_mask, length = candidates[0]
            np.testing.assert_array_equal(target_row[:length], expected_target)
            np.testing.assert_array_equal(mask_row[:length], expected_mask)
            np.testing.assert_array_equal(input_row[length:], 0)
            np.testing.assert_array_equal(target_row[length:], 0)
            np.testing.assert_array_equal(mask_row[length:], 0)
            self.assertEqual(target_row[length - 1], 256)
            self.assertEqual(np.count_nonzero(target_row == 256), 1)
            self.assertNotIn(256, input_row[:length])
            matched.append(record)
        self.assertEqual(tokens.shape[1], max(len(self.expected_arrays(row)[0]) for row in matched))
        return matched

    def test_loader_preserves_split_records_and_reviewed_source_identity(self):
        loaded = self.load()
        self.assertEqual(set(loaded.records), {"train", "validation"})
        self.assertEqual(len(loaded.records["train"]), 12)
        self.assertEqual(len(loaded.records["validation"]), 3)
        self.assertEqual(loaded.sha256, digest(self.path.read_bytes()))
        self.assertEqual(loaded.baseline_commit, self.baseline)
        self.assertEqual(loaded.source_hashes, {"README.md": self.source_sha})
        self.assertEqual(loaded.review, self.document["review"])
        self.assertEqual(loaded.topics, self.document["topics"])
        ids = []
        for split, records in loaded.records.items():
            for record in records:
                self.assertTrue({"id", "topic_id", "question", "answer", "text", "answer_start"} <= set(record))
                topic = next(topic for topic in self.document["topics"] if topic["id"] == record["topic_id"])
                expected_questions = topic["questions"] if split == "train" else [topic["validation_question"]]
                self.assertIn(record["question"], expected_questions)
                self.assertEqual(record["answer"], topic["answer"])
                self.assertEqual(record["text"], "\nUser: " + record["question"] + "\nAssistant: " + record["answer"] + "\n")
                self.assertLessEqual(len(record["text"].encode("utf-8")) + 1, 512)
                ids.append(record["id"])
        self.assertEqual(len(ids), len(set(ids)))

    def test_arrays_shift_utf8_and_supervise_first_answer_byte_newline_and_eod(self):
        self.document["topics"][0]["questions"][0] = "Café 🐝 asks about the first byte?"
        self.write_document()
        loaded = self.load()
        record = loaded.records["train"][0]
        tokens, targets, mask = loaded.arrays(record)
        expected_input, expected_target, expected_mask, start = self.expected_arrays(record)
        self.assertEqual(record["answer_start"], start)
        self.assertGreater(start, len("\nUser: " + record["question"] + "\nAssistant: "))
        self.assertEqual(tokens.dtype, np.dtype(np.int64))
        self.assertEqual(targets.dtype, np.dtype(np.int64))
        self.assertEqual(mask.dtype, np.dtype(np.float32))
        np.testing.assert_array_equal(tokens, expected_input)
        np.testing.assert_array_equal(targets, expected_target)
        np.testing.assert_array_equal(mask, expected_mask)
        self.assertEqual(targets[start - 1], "É".encode("utf-8")[0])
        np.testing.assert_array_equal(mask[:start - 1], 0)
        np.testing.assert_array_equal(mask[start - 1:], 1)
        self.assertEqual(targets[-2:].tolist(), [10, 256])
        self.assertEqual(mask.sum(), len(record["answer"].encode("utf-8")) + 2)

    def test_batch_keeps_full_turns_and_masks_dynamic_padding(self):
        loaded = self.load()
        records = self.assert_complete_batch(loaded, "train", loaded.batch("train", np.random.default_rng(7), 8))
        lengths = [len(self.expected_arrays(row)[0]) for row in records]
        self.assertGreater(len(set(lengths)), 1, "Fixture should exercise actual padding.")
        self.assertLess(max(lengths), 512)

    def test_batches_are_reproducible_from_generator_state(self):
        loaded = self.load()
        left, right = np.random.default_rng(12), np.random.default_rng(12)
        for _ in range(3):
            for actual, expected in zip(loaded.batch("train", left, 6), loaded.batch("train", right, 6)):
                np.testing.assert_array_equal(actual, expected)
        self.assertEqual(left.bit_generator.state, right.bit_generator.state)

    def test_evaluation_batches_visit_every_record_and_answer_in_order_once(self):
        loaded = self.load()
        for split in ("train", "validation"):
            with self.subTest(split=split):
                seen = []
                batches = list(loaded.evaluation_batches(split, 2))
                self.assertEqual(sum(batch[0].shape[0] for batch in batches), len(loaded.records[split]))
                for batch in batches:
                    self.assertLessEqual(batch[0].shape[0], 2)
                    seen.extend(self.assert_complete_batch(loaded, split, batch))
                self.assertEqual([record["id"] for record in seen], [record["id"] for record in loaded.records[split]])
                expected_counts = Counter(topic["answer"] for topic in self.document["topics"])
                multiplier = 4 if split == "train" else 1
                self.assertEqual(Counter(record["answer"] for record in seen),
                                 Counter({answer: count * multiplier for answer, count in expected_counts.items()}))

    def test_context_boundary_counts_the_terminal_eod(self):
        loaded = self.load()
        total_tokens = max(len(record["text"].encode("utf-8")) + 1
                           for rows in loaded.records.values() for record in rows)
        self.load(context_length=total_tokens)
        with self.assertRaises(ValueError):
            self.load(context_length=total_tokens - 1)

    def test_oversized_turn_is_rejected_instead_of_truncated(self):
        self.assert_invalid(lambda document: document["topics"][0]["questions"].__setitem__(0, "x" * 490))

    def test_answer_limit_counts_utf8_bytes(self):
        self.document["topics"][0]["answer"] = "é" * 90
        self.write_document()
        self.load()
        self.assert_invalid(lambda document: document["topics"][0].__setitem__("answer", "é" * 91))

    def test_source_hash_and_evidence_must_match_the_pinned_blob_and_lines(self):
        mutations = {
            "hash": lambda source: source.__setitem__("file_sha256", "0" * 64),
            "invented evidence": lambda source: source.__setitem__("evidence", "This text is absent from the source."),
            "evidence outside cited lines": lambda source: source.__setitem__("evidence", self.source_lines[2]),
            "line start zero": lambda source: source.__setitem__("line_start", 0),
            "line start boolean": lambda source: source.__setitem__("line_start", True),
            "reversed lines": lambda source: source.update(line_start=2, line_end=1),
            "past end": lambda source: source.__setitem__("line_end", len(self.source_lines) + 1),
        }
        for label, mutate in mutations.items():
            with self.subTest(case=label):
                self.assert_invalid(lambda document: mutate(document["topics"][0]["sources"][0]))

    def test_loader_reads_pinned_git_blobs_without_opening_worktree_sources(self):
        real_open = Path.open

        def guarded_open(path, *args, **kwargs):
            if path.absolute() == self.repository / "README.md":
                raise AssertionError("Curriculum evidence must come from the pinned Git blob.")
            return real_open(path, *args, **kwargs)

        with patch.object(Path, "open", guarded_open):
            loaded = self.load()
        self.assertEqual(loaded.source_hashes, {"README.md": self.source_sha})

    def test_source_traversal_absolute_paths_and_nonreference_files_are_rejected(self):
        for filename in ("../README.md", "/README.md", "src/../README.md", "src/../../README.md",
                         "src//net.cpp", "README.md\\..\\src\\net.cpp", ".git/config", "contrib/b3ai/teaching/data.py"):
            with self.subTest(file=filename):
                self.assert_invalid(lambda document: document["topics"][0]["sources"][0].__setitem__("file", filename))

    def test_baseline_requires_a_real_full_commit_hash(self):
        for commit in (self.baseline[:7], "HEAD", "0" * 40, "--help", self.baseline + ":README.md"):
            with self.subTest(commit=commit):
                self.assert_invalid(lambda document: document.__setitem__("baseline_commit", commit))

    def test_questions_are_unique_after_unicode_case_and_whitespace_normalization(self):
        variants = ("  CAFÉ   🐝?  ", "cafe\u0301 🐝?")
        self.document["topics"][0]["questions"][0] = "Café 🐝?"
        for variant in variants:
            with self.subTest(question=variant, location="validation"):
                self.assert_invalid(lambda document: document["topics"][1].__setitem__("validation_question", variant))
            with self.subTest(question=variant, location="other topic"):
                self.assert_invalid(lambda document: document["topics"][1]["questions"].__setitem__(0, variant))
        self.assert_invalid(lambda document: document["topics"][0]["questions"].__setitem__(1, "Café 🐝?"))

    def test_schema_review_topic_and_source_contracts_are_strict(self):
        mutations = {
            "unknown root field": lambda document: document.__setitem__("extra", True),
            "schema": lambda document: document.__setitem__("schema", "b3-teaching-curriculum/v2"),
            "unreviewed": lambda document: document["review"].__setitem__("status", "unreviewed"),
            "claimed independent exam": lambda document: document["review"].__setitem__("not_independent_exam", False),
            "missing review scope": lambda document: document["review"].pop("scope"),
            "no topics": lambda document: document.__setitem__("topics", []),
            "unknown topic field": lambda document: document["topics"][0].__setitem__("extra", True),
            "missing limits": lambda document: document["topics"][0].pop("limits"),
            "unknown status": lambda document: document["topics"][0].__setitem__("status", "production"),
            "duplicate topic id": lambda document: document["topics"][1].__setitem__("id", document["topics"][0]["id"]),
            "three train questions": lambda document: document["topics"][0]["questions"].pop(),
            "blank answer": lambda document: document["topics"][0].__setitem__("answer", " "),
            "multiline question": lambda document: document["topics"][0]["questions"].__setitem__(0, "First\nSecond"),
            "multiline answer": lambda document: document["topics"][0].__setitem__("answer", "First\nSecond"),
            "no sources": lambda document: document["topics"][0].__setitem__("sources", []),
            "unknown source field": lambda document: document["topics"][0]["sources"][0].__setitem__("extra", True),
        }
        for label, mutate in mutations.items():
            with self.subTest(case=label):
                self.assert_invalid(mutate)

    def test_topic_count_is_bounded(self):
        self.assert_invalid(lambda document: document.__setitem__("topics", [self.topic(index) for index in range(65)]))

    def test_malformed_unicode_and_duplicate_json_keys_are_rejected(self):
        original = self.path.read_bytes()
        duplicate_root = b'{"schema":"wrong",' + original[1:]
        duplicate_topic = original.replace(b'"id": "fixture-0"', b'"id": "shadow", "id": "fixture-0"', 1)
        duplicate_source = original.replace(b'"line_start": 1', b'"line_start": 2, "line_start": 1', 1)
        invalid = (b"{", b"[]", original[:-2], b"\xff", b"[" * 1500 + b"]" * 1500,
                   duplicate_root, duplicate_topic, duplicate_source,
                   original.replace(b'"line_start": 1', b'"line_start": NaN', 1),
                   original.replace(b'"line_start": 1', b'"line_start": Infinity', 1),
                   original.replace(b'"answer": "', b'"answer": "\\ud800', 1))
        for index, payload in enumerate(invalid):
            with self.subTest(case=index):
                self.path.write_bytes(payload)
                with self.assertRaises(ValueError):
                    self.load()

    def test_oversized_json_is_rejected_before_parsing(self):
        self.path.write_bytes(b" " * (1024 * 1024 + 1))
        with patch("contrib.b3ai.teaching.data.json.loads") as parse:
            with self.assertRaises(ValueError):
                self.load()
        parse.assert_not_called()

    def test_invalid_context_lengths_and_batch_requests_are_rejected(self):
        for context in (0, 513, True, 512.0, None):
            with self.subTest(context=context):
                with self.assertRaises(ValueError):
                    self.load(context_length=context)
        loaded = self.load()
        for size in (0, -1, True, 1.0, None):
            with self.subTest(batch_size=size):
                with self.assertRaises(ValueError):
                    loaded.batch("train", np.random.default_rng(0), size)
                with self.assertRaises(ValueError):
                    list(loaded.evaluation_batches("validation", size))
        with self.assertRaises(ValueError):
            loaded.batch("test", np.random.default_rng(0), 1)
        with self.assertRaises(ValueError):
            list(loaded.evaluation_batches("test", 1))


if __name__ == "__main__":
    unittest.main()
