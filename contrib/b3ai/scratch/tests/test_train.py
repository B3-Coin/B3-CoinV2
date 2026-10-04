# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.

import contextlib
import copy
import io
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import numpy as np

from contrib.b3ai.scratch.checkpoint import load_checkpoint
from contrib.b3ai.scratch.data import BASELINE, builtin_curriculum, canonical, corpus_bytes, load_curriculum, validate_records
from contrib.b3ai.scratch.model import ModelConfig
from contrib.b3ai.scratch.train import batch, run


class TrainingTests(unittest.TestCase):
    def test_source_fixture_and_split_integrity(self):
        rows = load_curriculum()
        training = {row["text"] for row in rows if row["split"] == "train"}
        validation = {row["text"] for row in rows if row["split"] == "validation"}
        self.assertTrue(training and validation)
        self.assertFalse(training & validation)
        self.assertTrue(all(row["reviewed"] and row["source"] for row in rows))
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "corpus.jsonl"
            path.write_bytes(corpus_bytes(rows))
            self.assertEqual(load_curriculum(path), rows)
        for change in ("text", "reviewed", "split"):
            modified = copy.deepcopy(rows)
            modified[0][change] = {"text": "altered", "reviewed": False, "split": "exam"}[change]
            with self.assertRaises(ValueError):
                validate_records(modified)
        with self.assertRaises(ValueError):
            validate_records(rows + [rows[0]])

    def test_windows_never_cross_records(self):
        documents = [[1] * 12, [9] * 15]
        x, y = batch(documents, 16, 8, np.random.default_rng(5))
        for input_row, target_row in zip(x, y):
            self.assertIn(int(input_row[0]), (1, 9))
            self.assertTrue(np.all(input_row == input_row[0]))
            np.testing.assert_array_equal(input_row, target_row)
        with self.assertRaises(ValueError):
            batch([[1, 2]], 1, 2, np.random.default_rng(5))

    def test_custom_corpus_does_not_claim_builtin_review(self):
        rows = builtin_curriculum()
        rows[0]["source"] = "custom-source: caller's review"
        content = {key: value for key, value in rows[0].items() if key != "id"}
        rows[0]["id"] = hashlib.sha256(canonical(content)).hexdigest()
        config = ModelConfig(context_length=8, width=8, heads=2, layers=1)
        with tempfile.TemporaryDirectory() as root, contextlib.redirect_stdout(io.StringIO()):
            report = run(root, rows, steps=1, sequence_length=8, batch_size=2, config=config)
            _, _, metadata = load_checkpoint(Path(root) / "model.npz")
            for provenance in (report, metadata):
                self.assertEqual(provenance["training_baseline"], BASELINE)
                self.assertEqual(provenance["corpus_origin"], "custom-corpus")
                self.assertIsNone(provenance["source_revision"])
                self.assertEqual(provenance["teacher_review"], "caller-declared review; not independently verified")

    def test_training_and_exact_resume(self):
        config = ModelConfig(context_length=8, width=8, heads=2, layers=1, seed=3)
        kwargs = {"sequence_length": 8, "batch_size": 2, "learning_rate": 0.01,
                  "config": config, "evaluation_interval": 4}
        rows = builtin_curriculum()
        with tempfile.TemporaryDirectory() as root, contextlib.redirect_stdout(io.StringIO()):
            direct = Path(root) / "direct"
            resumed = Path(root) / "resumed"
            report = run(direct, rows, steps=12, **kwargs)
            self.assertLess(report["final_metrics"]["train_loss"], report["initial_metrics"]["train_loss"])
            self.assertEqual(report["readiness"], "experimental")
            self.assertFalse(report["pretrained_weights"])
            self.assertEqual(report["completed_steps"], 12)
            self.assertEqual(report["corpus_origin"], "builtin-b3-fixture")
            self.assertEqual(report["source_revision"], BASELINE)
            run(resumed, rows, steps=4, **kwargs)
            resumed_report = run(resumed, rows, steps=8, resume=True, **kwargs)
            first, first_optimizer, _ = load_checkpoint(direct / "model.npz")
            second, second_optimizer, _ = load_checkpoint(resumed / "model.npz")
            for name in first.params:
                np.testing.assert_array_equal(first.params[name], second.params[name])
                np.testing.assert_array_equal(first_optimizer.m[name], second_optimizer.m[name])
                np.testing.assert_array_equal(first_optimizer.v[name], second_optimizer.v[name])
            self.assertEqual(resumed_report["initial_metrics"], report["initial_metrics"])
            stored = json.loads((direct / "report.json").read_text())
            self.assertEqual(stored["dataset_sha256"], report["dataset_sha256"])
            self.assertEqual(stored["checkpoint_sha256"], hashlib.sha256((direct / "model.npz").read_bytes()).hexdigest())
            self.assertEqual((direct / "corpus.jsonl").read_bytes(), corpus_bytes(rows))
            with self.assertRaises(ValueError):
                run(direct, rows, steps=1, **kwargs)
            with self.assertRaises(ValueError):
                run(resumed, rows, steps=1, resume=True, **{**kwargs, "batch_size": 3})
            with self.assertRaises(ValueError):
                run(resumed, rows, steps=1, resume=True, **{**kwargs, "learning_rate": 0.02})


if __name__ == "__main__":
    unittest.main()
