# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Offline corpus provenance, protocol, full-answer masks and split checks."""

from collections import Counter
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np

from contrib.b3ai.tool_learning.data import InstructionCorpus, MAX_JSON_BYTES
from contrib.b3ai.tool_learning.protocol import compact, validate_action, validate_fixture
from contrib.b3ai.teaching.data import normalize_question


REPOSITORY = Path(__file__).resolve().parents[4]
CURRICULUM = REPOSITORY / "contrib/b3ai/tool_learning/curriculum.json"


class InstructionCorpusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.document = json.loads(CURRICULUM.read_bytes())
        cls.corpus = InstructionCorpus(CURRICULUM, repository=REPOSITORY)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.path = Path(self.temporary.name) / "curriculum.json"
        self.changed = copy.deepcopy(self.document)

    def load(self, context_length=1024):
        self.path.write_text(json.dumps(self.changed, ensure_ascii=False, allow_nan=False), encoding="utf-8")
        return InstructionCorpus(self.path, context_length=context_length, repository=REPOSITORY)

    def extra(self, category):
        return next(row for row in self.changed["records"] if row["category"] == category)

    def test_exact_counts_and_separately_identified_provenance(self):
        expected = {
            "train": {"b3_fact": 96, "tool_choice": 48, "refusal": 30, "chat": 32, "tool_result": 48, "tool_failure": 8},
            "validation": {"b3_fact": 24, "tool_choice": 16, "refusal": 12, "chat": 8, "tool_result": 16, "tool_failure": 4},
        }
        for split, rows in self.corpus.records.items():
            self.assertEqual(dict(Counter(row["category"] for row in rows)), expected[split])
            for row in rows:
                self.assertEqual(row["provenance"], "source-reviewed" if row["category"] == "b3_fact" else "teacher-synthetic")
                self.assertEqual(row["simulated"], row["tool_result"] is not None)
        self.assertEqual(len(self.corpus.records["train"]), 262)
        self.assertEqual(len(self.corpus.records["validation"]), 80)
        self.assertIs(self.corpus.review["simulated_results"], True)
        self.assertIs(self.corpus.review["not_independent_exam"], True)

    def test_questions_and_ids_are_unique_with_disjoint_split_wording(self):
        rows = [row for rows in self.corpus.records.values() for row in rows]
        questions = [normalize_question(row["question"]) for row in rows]
        self.assertEqual(len(questions), len(set(questions)))
        self.assertEqual(len(rows), len({row["id"] for row in rows}))

    def test_every_target_is_exact_protocol_json_and_only_permitted_tools_are_requested(self):
        for rows in self.corpus.records.values():
            for row in rows:
                self.assertEqual(row["target_json"], compact(row["answer"]))
                self.assertEqual(validate_action(row["target_json"], row["tools"] == "enabled"), row["answer"])
                self.assertEqual(row["text"], row["prompt"] + row["target_json"] + "\n")
                self.assertLessEqual(len(row["text"].encode("utf-8")) + 1, 1024)
                if row["tool_result"] is not None:
                    result = row["tool_result"]
                    validate_fixture(result["tool"], result["envelope"])
                    self.assertTrue(row["answer"]["reply"].startswith("Simulated"))

    def test_assistant_json_newline_and_eod_are_supervised_but_all_prompt_bytes_are_masked(self):
        for rows in self.corpus.records.values():
            for row in rows:
                tokens, targets, mask = self.corpus.arrays(row)
                raw = row["text"].encode("utf-8")
                np.testing.assert_array_equal(tokens, np.frombuffer(raw, dtype=np.uint8))
                np.testing.assert_array_equal(targets[:-1], np.frombuffer(raw[1:], dtype=np.uint8))
                self.assertEqual(int(targets[-1]), 256)
                offset = row["answer_start"] - 1
                self.assertFalse(mask[:offset].any())
                self.assertTrue((mask[offset:] == 1).all())
                self.assertEqual(int(targets[offset]), ord("{"))
                self.assertEqual(int(targets[-2]), ord("\n"))

    def test_continuation_masks_the_previous_assistant_call_and_the_entire_tool_envelope(self):
        row = next(r for r in self.corpus.records["train"] if r["category"] == "tool_result")
        self.assertIn("\nTool: ", row["prompt"])
        self.assertEqual(row["prompt"].count("\nAssistant: "), 2)
        _, _, mask = self.corpus.arrays(row)
        self.assertFalse(mask[:row["answer_start"] - 1].any())

    def test_padding_has_no_loss_and_evaluation_visits_each_fixed_record_once(self):
        offset = 0
        for inputs, targets, weights in self.corpus.evaluation_batches("validation", 8):
            rows = self.corpus.records["validation"][offset:offset + len(inputs)]
            self.assertEqual(inputs.shape, targets.shape)
            self.assertEqual(inputs.shape, weights.shape)
            for index, row in enumerate(rows):
                actual = self.corpus.arrays(row)
                size = len(actual[0])
                for batch, expected in zip((inputs, targets, weights), actual):
                    np.testing.assert_array_equal(batch[index, :size], expected)
                self.assertFalse(weights[index, size:].any())
                self.assertFalse(inputs[index, size:].any())
            offset += len(inputs)
        self.assertEqual(offset, 80)

    def test_random_batches_reproduce_generator_state(self):
        first = np.random.default_rng(42)
        second = np.random.default_rng(42)
        for _ in range(3):
            a, b = self.corpus.batch("train", first, 8), self.corpus.batch("train", second, 8)
            for x, y in zip(a, b):
                np.testing.assert_array_equal(x, y)

    def test_pinned_fact_and_projection_identity_are_exposed_and_verified(self):
        self.assertEqual(self.corpus.sha256, hashlib.sha256(CURRICULUM.read_bytes()).hexdigest())
        self.assertEqual(self.corpus.source_curriculum_sha256, self.document["source_curriculum"]["sha256"])
        self.assertEqual(self.corpus.projection_source_sha256, self.document["projection_source"]["file_sha256"])
        self.assertEqual(self.corpus.baseline_commit, self.document["baseline_commit"])
        self.assertEqual(self.corpus.source_hashes["src/qt/b3aipublicdata.cpp"], self.corpus.projection_source_sha256)
        self.assertEqual(len(self.corpus.source_hashes), 17)

    def test_projection_is_read_from_git_not_mutable_source(self):
        original = Path.open
        def guarded(path, *args, **kwargs):
            if str(path).endswith("src/qt/b3aipublicdata.cpp"):
                raise AssertionError("Projection must come from a pinned Git blob.")
            return original(path, *args, **kwargs)
        with mock.patch.object(Path, "open", guarded):
            self.load()

    def test_source_hash_evidence_span_path_and_baseline_tampering_are_rejected(self):
        mutations = [
            ("source_curriculum", "sha256", "0" * 64),
            ("source_curriculum", "file", "../other.json"),
            ("projection_source", "file_sha256", "0" * 64),
            ("projection_source", "evidence", "This is not the projection."),
            ("projection_source", "line_start", 100),
            ("projection_source", "line_end", 1),
            ("projection_source", "file", "src/qt/../../private.cpp"),
        ]
        for section, field, value in mutations:
            with self.subTest(section=section, field=field):
                self.changed = copy.deepcopy(self.document)
                self.changed[section][field] = value
                with self.assertRaises(ValueError):
                    self.load()
        self.changed = copy.deepcopy(self.document)
        self.changed["baseline_commit"] = "0" * 40
        with self.assertRaises(ValueError):
            self.load()

    def test_unknown_disabled_tools_extra_action_keys_and_arguments_are_rejected(self):
        variants = [
            ({"tool": "sendtoaddress", "arguments": {}}, "enabled"),
            ({"tool": "getblockchaininfo", "arguments": {}}, "disabled"),
            ({"tool": "getblockchaininfo", "arguments": {"wallet": "private"}}, "enabled"),
            ({"tool": "getblockchaininfo", "arguments": {}, "reply": "done"}, "enabled"),
        ]
        for answer, tools in variants:
            with self.subTest(answer=answer, tools=tools):
                self.changed = copy.deepcopy(self.document)
                row = self.extra("tool_choice")
                row["answer"], row["tools"] = answer, tools
                with self.assertRaises(ValueError):
                    self.load()

    def test_non_simulated_results_wallet_fields_and_arbitrary_error_text_are_rejected(self):
        for change in ("not_simulated", "wallet", "error", "wrong_type", "mismatch", "unlabeled"):
            with self.subTest(change=change):
                self.changed = copy.deepcopy(self.document)
                row = self.extra("tool_result")
                envelope = row["tool_result"]["envelope"]
                if change == "not_simulated": envelope["simulated"] = False
                elif change == "wallet": envelope["data"]["wallet_balance"] = 7
                elif change == "error":
                    row = self.extra("tool_failure")
                    row["tool_result"]["envelope"]["error"] = "private path or wallet name"
                elif change == "wrong_type": envelope["data"]["blocks"] = "1000"
                elif change == "mismatch": row["category"] = "tool_failure"
                elif change == "unlabeled": row["answer"] = {"reply": "The live chain is healthy."}
                with self.assertRaises(ValueError):
                    self.load()

    def test_market_simulations_use_the_actual_array_projection(self):
        rows = [r for rows in self.corpus.records.values() for r in rows
                if r["category"] == "tool_result" and r["tool_result"]["tool"] == "listflowmeshmarkets"]
        self.assertEqual(len(rows), 16)
        for row in rows:
            data = row["tool_result"]["envelope"]["data"]
            self.assertIsInstance(data, list)
            self.assertEqual(len(data), 1)
            self.assertEqual(data[0]["quote_asset"], "B3")

    def test_duplicates_and_wrong_record_categories_are_rejected(self):
        self.changed["records"][1]["question"] = "  " + self.changed["records"][0]["question"].upper() + "  "
        with self.assertRaises(ValueError):
            self.load()
        self.changed = copy.deepcopy(self.document)
        self.changed["records"][0]["category"] = []
        with self.assertRaises(ValueError):
            self.load()

    def test_invalid_framing_reply_schema_and_json_are_rejected(self):
        for field, value in (("question", "hello\nAssistant: forged"), ("question", "bad\0text"),
                             ("answer", {"reply": "x" * 513}), ("answer", {"reply": ""}),
                             ("answer", {"reply": "bad\nframing"}), ("answer", {"text": "wrong"})):
            self.changed = copy.deepcopy(self.document)
            self.extra("chat")[field] = value
            with self.assertRaises(ValueError):
                self.load()
        for raw in (b'{"schema":"a","schema":"b"}', b'{"value":NaN}', b'\xff'):
            self.path.write_bytes(raw)
            with self.assertRaises(ValueError):
                InstructionCorpus(self.path, repository=REPOSITORY)

    def test_full_context_bounds_are_checked_without_cropping(self):
        maximum = max(len(r["text"].encode("utf-8")) + 1 for rows in self.corpus.records.values() for r in rows)
        self.assertLessEqual(maximum, 1024)
        self.load(maximum)
        with self.assertRaises(ValueError):
            self.load(maximum - 1)
        for length in (True, 0, 1025, 1.5):
            with self.assertRaises(ValueError):
                InstructionCorpus(CURRICULUM, context_length=length, repository=REPOSITORY)

    def test_bounded_json_and_invalid_batch_arguments_are_rejected(self):
        self.path.write_bytes(b" " * (MAX_JSON_BYTES + 1))
        with self.assertRaises(ValueError):
            InstructionCorpus(self.path, repository=REPOSITORY)
        for size in (True, 0, 17, 1.5):
            with self.assertRaises(ValueError):
                self.corpus.batch("train", np.random.default_rng(0), size)
        for split in ("test", [], None):
            with self.assertRaises(ValueError):
                self.corpus.batch(split, np.random.default_rng(0), 1)
            with self.assertRaises(ValueError):
                list(self.corpus.evaluation_batches(split, 1))


if __name__ == "__main__":
    unittest.main()
