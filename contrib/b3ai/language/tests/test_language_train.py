# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Tiny real CPU runs check selection, state preservation and bounded execution."""

import contextlib
import hashlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np
import torch

from contrib.b3ai.language.batches import Corpus
from contrib.b3ai.language.checkpoint import load_checkpoint
from contrib.b3ai.language.corpus import prepare_corpus
from contrib.b3ai.language.model import GpuConfig
from contrib.b3ai.language import train


class LanguageTrainTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        train_source, validation_source = self.directory / "train.txt", self.directory / "valid.txt"
        train_source.write_text("".join(f"A little bird visits its friend in garden number {index}.<|endoftext|>"
                                      for index in range(16)), encoding="utf-8")
        validation_source.write_text("".join(f"The rabbit carries a basket to meadow number {index}.<|endoftext|>"
                                           for index in range(32)), encoding="utf-8")
        self.corpus_path = self.directory / "corpus"
        prepare_corpus(train_source, self.corpus_path, validation_source=validation_source,
                       max_documents=128, max_bytes=100_000, min_document_bytes=1)
        self.config = GpuConfig(vocab_size=257, context_length=8, width=8, layers=1,
                                heads=2, ff_multiplier=2, seed=42)
        self.options = {"device": "cpu", "steps": 3, "batch_size": 2,
                        "evaluation_interval": 1, "validation_batches": 1,
                        "max_seconds": 60, "max_tokens": 1000, "learning_rate": 0.0003}

    def fit(self, output_name="run", **options):
        arguments = dict(self.options, **options)
        with contextlib.redirect_stdout(io.StringIO()):
            return train.fit(self.corpus_path, self.directory / output_name, self.config, **arguments)

    def assert_same_state(self, before_model, before_optimizer, after_model, after_optimizer):
        for name, before_parameter in before_model.params.items():
            after_parameter = after_model.params[name]
            torch.testing.assert_close(before_parameter, after_parameter, rtol=0, atol=0)
            before, after = before_optimizer.state.get(before_parameter, {}), after_optimizer.state.get(after_parameter, {})
            self.assertEqual(set(before), set(after))
            for key in before:
                torch.testing.assert_close(before[key], after[key], rtol=0, atol=0)

    def test_score_weights_each_batch_by_its_supervised_targets(self):
        batches = [
            (np.array([[1, 2]], dtype=np.int64), np.array([[2, 3]], dtype=np.int64),
             np.array([[1, 1]], dtype=np.float32)),
            (np.array([[4, 0]], dtype=np.int64), np.array([[5, 0]], dtype=np.int64),
             np.array([[0.5, 0]], dtype=np.float32)),
        ]
        model = mock.Mock()
        model.loss.side_effect = [torch.tensor(2.0), torch.tensor(6.0)]
        self.assertEqual(train.score(model, batches, "cpu"), (2 * 2 + 6 * 0.5) / 2.5)
        self.assertEqual(model.loss.call_count, 2)
        for call, batch in zip(model.loss.call_args_list, batches):
            for tensor, array in zip(call.args, batch):
                np.testing.assert_array_equal(tensor.numpy(), array)
        with self.assertRaises(ValueError):
            train.score(model, [], "cpu")
        model.loss.side_effect = [torch.tensor(float("nan"))]
        with self.assertRaises(ValueError):
            train.score(model, batches[:1], "cpu")

    def test_validation_selects_best_weights_optimizer_and_rng_not_final_attempt(self):
        # Only selection losses are mocked; all three actual AdamW updates run.
        losses = [4.0, 3.0, 3.0, 2.0, 2.0, 2.5, 1.0, 4.0]
        with mock.patch.object(train, "score", side_effect=losses):
            report = self.fit()
        self.assertEqual(report["attempted_steps"], 3)
        self.assertEqual(report["selected_step"], 1)
        self.assertEqual(report["best_validation_loss"], 2.0)
        selected_model, selected_optimizer, selected = load_checkpoint(self.directory / "run" / "model.npz")
        last_model, last_optimizer, last = load_checkpoint(self.directory / "run" / "last.npz")
        self.assertEqual(selected["selected_or_saved_step"], 1)
        self.assertEqual(last["selected_or_saved_step"], 3)
        self.assertEqual(selected["validation_loss_at_save"], 2.0)
        self.assertEqual(last["validation_loss_at_save"], 4.0)
        self.assertTrue(any(not torch.equal(selected_model.params[name], last_model.params[name])
                            for name in selected_model.params))
        for state in selected_optimizer.state.values():
            self.assertEqual(state["step"].item(), 1)
        for state in last_optimizer.state.values():
            self.assertEqual(state["step"].item(), 3)
        corpus, rng = Corpus(self.corpus_path), np.random.default_rng(self.config.seed)
        counts = 0
        for index in range(3):
            counts += int(corpus.batch("train", rng, 2, 8)[2].sum())
            if index == 0:
                self.assertEqual(selected["batch_rng_state"], rng.bit_generator.state)
                self.assertEqual(selected["training_tokens_at_save"], counts)
        self.assertEqual(last["batch_rng_state"], rng.bit_generator.state)
        self.assertEqual(last["training_tokens_at_save"], counts)
        self.assertEqual(report["training_tokens"], counts)

    def test_baseline_zero_is_retained_when_later_validation_never_improves(self):
        with mock.patch.object(train, "score", side_effect=[4.0, 1.0, 3.0, 1.0, 2.0, 2.0, 1.0, 3.0]):
            report = self.fit()
        self.assertEqual(report["selected_step"], 0)
        _, optimizer, selected = load_checkpoint(self.directory / "run" / "model.npz")
        _, last_optimizer, last = load_checkpoint(self.directory / "run" / "last.npz")
        self.assertEqual(len(optimizer.state), 0)
        self.assertEqual(selected["selected_or_saved_step"], 0)
        self.assertEqual(selected["training_tokens_at_save"], 0)
        self.assertEqual(selected["batch_rng_state"], np.random.default_rng(self.config.seed).bit_generator.state)
        self.assertEqual(last["selected_or_saved_step"], 3)
        self.assertTrue(all(state["step"].item() == 3 for state in last_optimizer.state.values()))

    def test_fresh_runs_are_reproducible_and_never_batch_test_for_selection(self):
        real_batch = Corpus.batch
        seen_splits = []

        def observed_batch(corpus, split, *args, **kwargs):
            seen_splits.append(split)
            return real_batch(corpus, split, *args, **kwargs)

        with mock.patch.object(Corpus, "batch", autospec=True, side_effect=observed_batch):
            first = self.fit("first")
            second = self.fit("second")
        self.assertIn("validation", seen_splits)
        self.assertIn("train", seen_splits)
        self.assertNotIn("test", seen_splits)
        self.assertFalse(first["test_split_used_for_selection"])
        self.assertFalse(second["test_split_used_for_selection"])
        self.assertEqual(first["selected_step"], second["selected_step"])
        self.assertEqual(first["training_tokens"], second["training_tokens"])
        self.assertEqual(first["implementation_hash"], second["implementation_hash"])
        self.assertEqual(first["corpus_manifest_hash"], second["corpus_manifest_hash"])
        self.assertNotEqual(first["run_id"], second["run_id"])
        for before, after in zip(first["history"], second["history"]):
            self.assertEqual({key: value for key, value in before.items() if key != "elapsed_seconds"},
                             {key: value for key, value in after.items() if key != "elapsed_seconds"})
        for filename in ("model.npz", "last.npz"):
            first_model, first_optimizer, first_metadata = load_checkpoint(self.directory / "first" / filename)
            second_model, second_optimizer, second_metadata = load_checkpoint(self.directory / "second" / filename)
            self.assert_same_state(first_model, first_optimizer, second_model, second_optimizer)
            self.assertEqual(first_metadata["batch_rng_state"], second_metadata["batch_rng_state"])
            self.assertEqual(first_metadata["runtime"], second_metadata["runtime"])
        selected_path = self.directory / "first" / "model.npz"
        self.assertEqual(first["selected_checkpoint_sha256"], hashlib.sha256(selected_path.read_bytes()).hexdigest())

    def test_existing_run_cannot_be_overwritten(self):
        self.fit()
        output = self.directory / "run"
        original = {path.name: path.read_bytes() for path in output.iterdir()}
        with self.assertRaises(FileExistsError):
            self.fit()
        self.assertEqual({path.name: path.read_bytes() for path in output.iterdir()}, original)

    def test_malformed_limits_fail_before_reserving_output(self):
        cases = {"steps": (0, True, 1001), "batch_size": (0, 17), "evaluation_interval": (0, 1001),
                 "validation_batches": (0, 17), "max_seconds": (0, 7201), "max_tokens": (0, 8192001),
                 "learning_rate": (True, 0, float("nan"), 0.02)}
        for field, values in cases.items():
            for index, value in enumerate(values):
                with self.subTest(field=field, value=value):
                    output = field + str(index)
                    with self.assertRaises(ValueError):
                        self.fit(output, **{field: value})
                    self.assertFalse((self.directory / output).exists())
        for config in (GpuConfig(vocab_size=256, context_length=8, width=8, layers=1, heads=2),
                       GpuConfig(vocab_size=257, context_length=4096, width=8, layers=8, heads=8)):
            with self.assertRaises(ValueError):
                train._limits(config, 3, 16, 1, 1, 60, 1000, 0.0003)

    def test_token_cap_stops_before_an_optimizer_update(self):
        report = self.fit(max_tokens=1)
        self.assertEqual(report["stop_reason"], "token_limit")
        self.assertEqual(report["attempted_steps"], 0)
        self.assertEqual(report["training_tokens"], 0)
        model, optimizer, metadata = load_checkpoint(self.directory / "run" / "last.npz")
        self.assertEqual(metadata["selected_or_saved_step"], 0)
        self.assertEqual(len(optimizer.state), 0)
        self.assertEqual(metadata["batch_rng_state"], np.random.default_rng(self.config.seed).bit_generator.state)
        baseline, baseline_optimizer, _ = load_checkpoint(self.directory / "run" / "model.npz")
        self.assert_same_state(model, optimizer, baseline, baseline_optimizer)

    def test_final_unevaluated_step_is_scored_and_can_be_selected(self):
        # A token cap ends the run between scheduled evaluations: final state
        # must receive its own validation score before checkpoint selection.
        with mock.patch.object(train, "score", side_effect=[4.0, 3.0, 3.0, 2.0]):
            report = self.fit(evaluation_interval=3, max_tokens=16)
        self.assertEqual(report["attempted_steps"], 1)
        self.assertEqual(report["selected_step"], 1)
        self.assertEqual([row["step"] for row in report["history"]], [0, 1])
        self.assertEqual(report["best_validation_loss"], 2.0)
        _, _, metadata = load_checkpoint(self.directory / "run" / "model.npz")
        self.assertEqual(metadata["selected_or_saved_step"], 1)
        rng = np.random.default_rng(self.config.seed)
        Corpus(self.corpus_path).batch("train", rng, 2, 8)
        self.assertEqual(metadata["batch_rng_state"], rng.bit_generator.state)

    def test_wall_limit_does_not_label_an_unscored_last_state_with_stale_validation(self):
        with mock.patch.object(train.time, "monotonic", side_effect=[0, 0, 0, 70, 70]), \
                mock.patch.object(train, "score", side_effect=[4.0, 3.0]) as scorer:
            report = self.fit()
        self.assertEqual(report["stop_reason"], "wall_limit")
        self.assertEqual(report["attempted_steps"], 1)
        self.assertEqual(report["selected_step"], 0)
        self.assertEqual(scorer.call_count, 2)
        self.assertEqual([row["step"] for row in report["history"]], [0])
        _, _, last = load_checkpoint(self.directory / "run" / "last.npz")
        self.assertEqual(last["selected_or_saved_step"], 1)
        self.assertIsNone(last["validation_loss_at_save"])

    def test_memory_limit_retains_selected_checkpoint_without_extra_evaluation_or_snapshot(self):
        observations = [{"process_peak_rss_bytes": 1, "gpu_driver_allocated_bytes": 0}] * 4 + [
            {"process_peak_rss_bytes": train.MAX_MEMORY, "gpu_driver_allocated_bytes": 0}]
        with mock.patch.object(train, "memory", side_effect=observations), \
                mock.patch.object(train, "score", side_effect=[4.0, 3.0]) as scorer:
            report = self.fit()
        self.assertEqual(report["stop_reason"], "memory_limit")
        self.assertEqual(report["attempted_steps"], 1)
        self.assertEqual(report["selected_step"], 0)
        self.assertEqual(scorer.call_count, 2)
        self.assertEqual(report["peak_memory"]["process_peak_rss_bytes"], train.MAX_MEMORY)
        self.assertTrue((self.directory / "run" / "model.npz").is_file())
        self.assertFalse((self.directory / "run" / "last.npz").exists())
        self.assertIsNone(report["last_checkpoint_sha256"])


if __name__ == "__main__":
    unittest.main()
