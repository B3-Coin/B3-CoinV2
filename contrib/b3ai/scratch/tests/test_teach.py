# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.

import contextlib
import hashlib
import io
import json
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest import mock

import numpy as np

from contrib.b3ai.scratch.checkpoint import load_checkpoint, save_checkpoint
from contrib.b3ai.scratch.data import _record, corpus_bytes, split_hash
from contrib.b3ai.scratch.model import AdamW, ModelConfig, Transformer
from contrib.b3ai.scratch.teach import dialogue_batch, evaluate_dialogues, fit, parse_dialogue, reply


def dialogue(split, question, answer):
    return _record(split, f"\nUser: {question}\nAssistant: {answer}\n", "unit-test reviewed fixture")


def records():
    # Unequal answer lengths make record-average and token-average loss differ.
    return [dialogue("train", "A?", "aaaa."), dialogue("train", "More A?", "aaaaaaaa."),
            dialogue("validation", "B?", "bb."), dialogue("validation", "More B?", "bbbbbbbbbb.")]


def independent_answer_loss(model, rows):
    """Compute held-out CE from full UTF-8 records and public model logits only."""
    total_loss, total_tokens = 0.0, 0
    for row in rows:
        text = row["text"]
        before, separator, _ = text.partition("\nAssistant: ")
        if not separator:
            raise AssertionError("Malformed test fixture")
        prefix_length = len((before + separator).encode("utf-8"))
        encoded = np.frombuffer(text.encode("utf-8"), dtype=np.uint8).astype(np.int64)
        logits = model.forward(encoded[:-1][None, :]).astype(np.float64)
        logits -= np.max(logits, axis=-1, keepdims=True)
        losses = (np.log(np.exp(logits).sum(axis=-1))[0]
                  - logits[0, np.arange(encoded.size - 1), encoded[1:]])
        answer_losses = losses[prefix_length - 1:]
        total_loss += float(answer_losses.sum())
        total_tokens += answer_losses.size
    return total_loss / total_tokens


class ScriptedModel:
    """Supplies actual generated IDs; it has no facts, canned answers or tools."""

    def __init__(self, script, context_length=128):
        self.config = SimpleNamespace(context_length=context_length)
        self.script = list(script)
        self.calls = []

    def generate(self, token_ids, max_new_tokens, temperature=0, seed=42):
        self.calls.append((list(token_ids), max_new_tokens, temperature, seed))
        continuation = (self.script + [ord("?")] * max_new_tokens)[:max_new_tokens]
        return list(token_ids) + continuation


class DialogueTests(unittest.TestCase):
    def test_exact_single_turn_parser_and_unicode(self):
        row = dialogue("train", "What is B3?", "One continuous chain.")
        self.assertEqual(parse_dialogue(row), ("\nUser: What is B3?\nAssistant: ", "One continuous chain.\n"))
        self.assertEqual(parse_dialogue(dialogue("train", "π?", "Sí.")),
                         ("\nUser: π?\nAssistant: ", "Sí.\n"))

    def test_malformed_turns_and_invalid_unicode_are_rejected(self):
        invalid = ["User: Q\nAssistant: A\n", "\nUser: Q\nAssistant: A", "\nUser: \nAssistant: A\n",
                   "\nUser: Q\nAssistant: \n", "\nUser:   \nAssistant: A\n", "\nUser: Q\nAssistant:  \n",
                   "\nUser: Q\nextra\nAssistant: A\n", "\nUser: Q\nAssistant: A\nextra\n",
                   "\nUser: Q\r\nAssistant: A\n", "\nUser: Q\nAssistant: A\x00\n",
                   "\nUser: Q\nAssistant: A\n\n", "\nUser: Q\nAssistant: A\nUser: another\nAssistant: B\n",
                   "\nUser: \ud800\nAssistant: A\n"]
        for text in invalid:
            with self.subTest(text=repr(text)), self.assertRaises(ValueError):
                parse_dialogue({"text": text})
        for value in (None, [], {}, {"text": b"bytes"}, {"text": 42}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                parse_dialogue(value)

    def test_batch_supervises_only_answer_bytes_and_the_final_newline(self):
        rows = [dialogue("train", "π?", "Sí."), dialogue("train", "A longer 字 question?", "好, thanks.")]
        x, y, mask = dialogue_batch(rows, 8, 128, np.random.default_rng(7))
        self.assertEqual(x.shape, y.shape)
        self.assertEqual(x.shape, mask.shape)
        self.assertEqual(x.shape[0], 8)
        self.assertLessEqual(x.shape[1], 128)
        self.assertTrue(np.issubdtype(x.dtype, np.integer))
        self.assertTrue(np.issubdtype(y.dtype, np.integer))
        seen, padding_seen = set(), False
        for input_row, target_row, weights in zip(x, y, mask):
            matched = None
            for index, row in enumerate(rows):
                encoded = np.frombuffer(row["text"].encode("utf-8"), dtype=np.uint8)
                count = encoded.size - 1
                if (count <= input_row.size and np.array_equal(input_row[:count], encoded[:-1])
                        and np.array_equal(target_row[:count], encoded[1:])):
                    matched = (index, row, encoded, count)
                    break
            self.assertIsNotNone(matched)
            index, row, encoded, count = matched
            seen.add(index)
            prompt, answer = parse_dialogue(row)
            prefix_length = len(prompt.encode("utf-8"))
            expected = np.zeros(weights.shape)
            expected[prefix_length - 1:count] = 1
            np.testing.assert_array_equal(weights, expected)
            self.assertEqual(bytes(target_row[weights.astype(bool)].tolist()).decode("utf-8"), answer)
            self.assertEqual(int(target_row[count - 1]), 10)
            self.assertEqual(float(weights[count - 1]), 1.0)
            np.testing.assert_array_equal(input_row[:prefix_length], list(prompt.encode("utf-8")))
            np.testing.assert_array_equal(input_row[count:], np.zeros(input_row.size - count))
            np.testing.assert_array_equal(target_row[count:], np.zeros(target_row.size - count))
            if count < input_row.size:
                padding_seen = True
        self.assertEqual(seen, {0, 1})
        self.assertTrue(padding_seen)

    def test_complete_turn_boundary_uses_utf8_bytes_and_never_truncates(self):
        row = dialogue("train", "π?", "Sí.")
        encoded = list(row["text"].encode("utf-8"))
        x, y, mask = dialogue_batch([row], 1, len(encoded) - 1, np.random.default_rng(1))
        np.testing.assert_array_equal(x[0], encoded[:-1])
        np.testing.assert_array_equal(y[0], encoded[1:])
        self.assertEqual(mask[0, -1], 1)
        with self.assertRaises(ValueError):
            dialogue_batch([row], 1, len(encoded) - 2, np.random.default_rng(1))
        with self.assertRaises(ValueError):
            dialogue_batch([row], 1, len(row["text"]) - 1, np.random.default_rng(1))
        for batch_size, length in ((0, 128), (17, 128), (True, 128), (1, 0), (1, True)):
            with self.subTest(batch_size=batch_size, length=length), self.assertRaises(ValueError):
                dialogue_batch([row], batch_size, length, np.random.default_rng(1))
        with self.assertRaises(ValueError):
            dialogue_batch([], 1, 128, np.random.default_rng(1))

    def test_reply_returns_generated_bytes_and_stops_at_first_newline(self):
        prompt = "\nUser: Q?\nAssistant: "
        model = ScriptedModel("hé\nThis must not appear".encode("utf-8"))
        text, reason = reply(model, prompt, max_new_tokens=32, temperature=0.7, seed=9)
        self.assertEqual((text, reason), ("hé", "stop"))
        self.assertEqual(model.calls, [(list(prompt.encode("utf-8")), 32, 0.7, 9)])
        self.assertEqual(reply(ScriptedModel(b"xyz"), prompt, max_new_tokens=3), ("xyz", "length"))
        self.assertEqual(reply(ScriptedModel(b"\n"), prompt, max_new_tokens=3), ("", "stop"))

    def test_reply_uses_real_transformer_output_even_when_empty_or_meaningless(self):
        model = Transformer(ModelConfig(context_length=128, width=8, layers=1, heads=2))
        for parameter in model.params.values():
            parameter.fill(0)
        model.params["head.bias"][10] = 1
        prompt = "\nUser: Explain B3?\nAssistant: "
        self.assertEqual(reply(model, prompt, max_new_tokens=5), ("", "stop"))
        model.params["head.bias"][10] = 0
        model.params["head.bias"][ord("x")] = 1
        self.assertEqual(reply(model, prompt, max_new_tokens=5), ("xxxxx", "length"))

    def test_reply_rejects_invalid_prompt_or_context_before_generation(self):
        model = ScriptedModel(b"answer\n", context_length=32)
        for prompt, tokens in (("", 1), (" ", 1), ("\ud800", 1), ("bad\0prompt", 1),
                               ("Q", 0), ("Q", True), ("Q", 257), ("Q" * 32, 1)):
            with self.subTest(prompt=repr(prompt), tokens=tokens), self.assertRaises(ValueError):
                reply(model, prompt, max_new_tokens=tokens)
        self.assertEqual(model.calls, [])


class TeachingFitTests(unittest.TestCase):
    def config(self, context_length=128):
        return ModelConfig(context_length=context_length, width=8, layers=1, heads=2, seed=11)

    def kwargs(self):
        return dict(config=self.config(), batch_size=2, learning_rate=0.01, evaluation_interval=2, patience=2)

    def test_real_fit_uses_heldout_answer_loss_and_saves_the_best_checkpoint(self):
        rows = records()
        training = [row for row in rows if row["split"] == "train"]
        validation = [row for row in rows if row["split"] == "validation"]
        initial = Transformer(self.config())
        expected_initial_train = independent_answer_loss(initial, training)
        expected_initial_validation = independent_answer_loss(initial, validation)
        self.assertNotAlmostEqual(expected_initial_train, expected_initial_validation, places=4)
        self.assertAlmostEqual(evaluate_dialogues(initial, validation), expected_initial_validation, places=5)
        with tempfile.TemporaryDirectory() as temporary, contextlib.redirect_stdout(io.StringIO()):
            output = Path(temporary) / "fit"
            report = fit(output, rows, steps=8, **self.kwargs())
            model, optimizer, metadata = load_checkpoint(output / "model.npz")
            self.assertEqual(report["stage"], "sanity-dialogue")
            self.assertEqual(report["readiness"], "experimental")
            self.assertEqual(report["initialization"], "random")
            self.assertFalse(report["pretrained_weights"])
            self.assertAlmostEqual(report["initial_metrics"]["train_loss"], expected_initial_train, places=5)
            self.assertAlmostEqual(report["initial_metrics"]["validation_loss"], expected_initial_validation, places=5)
            self.assertAlmostEqual(independent_answer_loss(model, validation), report["best_validation_loss"], places=5)
            self.assertAlmostEqual(independent_answer_loss(model, validation), report["final_metrics"]["validation_loss"], places=5)
            candidates = [(0, report["initial_metrics"]["validation_loss"])] + [
                (entry["step"], entry["validation_loss"]) for entry in report["evaluations_this_run"]]
            expected_step, expected_loss = min(candidates, key=lambda entry: entry[1])
            self.assertEqual(report["best_step"], expected_step)
            self.assertEqual(report["best_validation_loss"], expected_loss)
            self.assertEqual(optimizer.steps, expected_step)
            self.assertEqual(report["saved_optimizer_steps"], expected_step)
            self.assertEqual(metadata["best_step"], expected_step)
            self.assertEqual(metadata["stage"], "sanity-dialogue")
            self.assertLessEqual(expected_step, report["completed_steps"])
            self.assertLessEqual(report["completed_steps"], 8)
            self.assertEqual(report["train_records"], len(training))
            self.assertEqual(report["validation_records"], len(validation))
            self.assertEqual(model.config.to_dict(), self.config().to_dict())
            expected_rng = np.random.default_rng(self.config().seed + 1)
            for _ in range(expected_step * 2):
                expected_rng.integers(len(training))
            self.assertEqual(metadata["rng_state"], expected_rng.bit_generator.state)
            self.assertEqual((output / "corpus.jsonl").read_bytes(), corpus_bytes(rows))
            self.assertEqual(report["dataset_sha256"], hashlib.sha256(corpus_bytes(rows)).hexdigest())
            self.assertEqual(report["training_split_hash"], split_hash(rows, "train"))
            self.assertEqual(report["validation_split_hash"], split_hash(rows, "validation"))
            self.assertEqual(report["checkpoint_sha256"], hashlib.sha256((output / "model.npz").read_bytes()).hexdigest())
            self.assertEqual(json.loads((output / "report.json").read_text()), report)

    def test_patience_stops_training_and_restores_weights_optimizer_and_rng(self):
        rows = records()
        # Each pair is train/validation; the last pair reevaluates the restored best.
        long_losses = [9, 5, 8, 4, 7, 6, 6, 7, 8, 4]
        short_losses = [9, 5, 8, 4, 8, 4]
        with tempfile.TemporaryDirectory() as temporary, contextlib.redirect_stdout(io.StringIO()):
            long_output, short_output = Path(temporary) / "early-stop", Path(temporary) / "step-two"
            with mock.patch("contrib.b3ai.scratch.teach.evaluate_dialogues", side_effect=long_losses) as evaluated:
                report = fit(long_output, rows, steps=20, **self.kwargs())
                self.assertEqual(evaluated.call_count, len(long_losses))
            with mock.patch("contrib.b3ai.scratch.teach.evaluate_dialogues", side_effect=short_losses):
                fit(short_output, rows, steps=2, **self.kwargs())
            self.assertEqual(report["completed_steps"], 6)
            self.assertEqual(report["best_step"], 2)
            self.assertEqual(report["best_validation_loss"], 4)
            self.assertTrue(report["stopped_early"])
            self.assertEqual([row["step"] for row in report["evaluations_this_run"]], [2, 4, 6])
            best, best_optimizer, best_metadata = load_checkpoint(long_output / "model.npz")
            expected, expected_optimizer, expected_metadata = load_checkpoint(short_output / "model.npz")
            self.assertEqual(best_optimizer.steps, 2)
            for name in best.params:
                np.testing.assert_array_equal(best.params[name], expected.params[name])
                np.testing.assert_array_equal(best_optimizer.m[name], expected_optimizer.m[name])
                np.testing.assert_array_equal(best_optimizer.v[name], expected_optimizer.v[name])
            self.assertEqual(best_metadata["rng_state"], expected_metadata["rng_state"])

    def test_invalid_schema_or_dialogue_never_creates_an_experiment(self):
        invalid = []
        unreviewed = records()
        unreviewed[0]["reviewed"] = False
        invalid.append(unreviewed)
        wrong_id = records()
        wrong_id[0]["id"] = "0" * 64
        invalid.append(wrong_id)
        duplicate_split = records()
        duplicate_split[2] = _record("validation", duplicate_split[0]["text"], duplicate_split[0]["source"])
        invalid.append(duplicate_split)
        bad_turn = records()
        bad_turn[0] = _record("train", "\nUser: Q\nAssistant: A\nextra\n", "unit-test fixture")
        invalid.append(bad_turn)
        too_long = records()
        too_long[0] = dialogue("train", "Q", "a" * 128)
        invalid.append(too_long)
        with tempfile.TemporaryDirectory() as temporary:
            for index, rows in enumerate(invalid):
                output = Path(temporary) / str(index)
                with self.subTest(index=index), self.assertRaises(ValueError):
                    fit(output, rows, steps=1, **self.kwargs())
                self.assertFalse(output.exists())

    def test_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "existing"
            output.mkdir()
            sentinel = output / "keep.txt"
            sentinel.write_bytes(b"existing experiment must stay exact")
            with self.assertRaises(ValueError):
                fit(output, records(), steps=1, **self.kwargs())
            self.assertEqual(list(output.iterdir()), [sentinel])
            self.assertEqual(sentinel.read_bytes(), b"existing experiment must stay exact")
            empty = Path(temporary) / "existing-empty"
            empty.mkdir()
            with self.assertRaises(ValueError):
                fit(empty, records(), steps=1, **self.kwargs())
            self.assertEqual(list(empty.iterdir()), [])

    def test_float64_transfer_is_rejected_before_creating_output(self):
        previous = Transformer(self.config(context_length=64), dtype=np.float64)
        previous_optimizer = AdamW(previous.params)
        with tempfile.TemporaryDirectory() as temporary:
            parent_path = Path(temporary) / "float64-parent.npz"
            save_checkpoint(parent_path, previous, previous_optimizer,
                            {"stage": "sanity-pretraining", "readiness": "experimental"})
            output = Path(temporary) / "rejected-transfer"
            with self.assertRaises(ValueError):
                fit(output, records(), steps=1, initialize_from=parent_path, **self.kwargs())
            self.assertFalse(output.exists())

    def test_transfer_preserves_parent_weights_and_records_optimizer_reset(self):
        previous_config = self.config(context_length=64)
        previous = Transformer(previous_config)
        previous_optimizer = AdamW(previous.params)
        previous_optimizer.step(previous.params, {name: np.full_like(value, 0.01)
            for name, value in previous.params.items()}, lr=0.001)
        with tempfile.TemporaryDirectory() as temporary, contextlib.redirect_stdout(io.StringIO()):
            parent_path = Path(temporary) / "parent.npz"
            save_checkpoint(parent_path, previous, previous_optimizer,
                            {"stage": "sanity-pretraining", "readiness": "experimental"})
            output = Path(temporary) / "transfer"
            # Both teaching updates score worse, so retain the transferred initial state.
            with mock.patch("contrib.b3ai.scratch.teach.evaluate_dialogues", side_effect=[9, 4, 8, 5, 9, 4]):
                report = fit(output, records(), steps=2, initialize_from=parent_path, **self.kwargs())
            transferred, optimizer, metadata = load_checkpoint(output / "model.npz")
            fresh = Transformer(self.config())
            for name in previous.params:
                if name == "position_embedding":
                    np.testing.assert_array_equal(transferred.params[name][:64], previous.params[name])
                    np.testing.assert_array_equal(transferred.params[name][64:], fresh.params[name][64:])
                else:
                    np.testing.assert_array_equal(transferred.params[name], previous.params[name])
                np.testing.assert_array_equal(optimizer.m[name], np.zeros_like(transferred.params[name]))
                np.testing.assert_array_equal(optimizer.v[name], np.zeros_like(transferred.params[name]))
            self.assertEqual(optimizer.steps, 0)
            self.assertEqual(report["best_step"], 0)
            self.assertTrue(report["optimizer_reset"])
            self.assertEqual(report["parent"]["checkpoint_sha256"], hashlib.sha256(parent_path.read_bytes()).hexdigest())
            self.assertEqual(report["parent"]["prior_optimizer_steps"], 1)
            self.assertEqual(report["parent"]["new_positions"], 64)
            self.assertEqual(metadata["parent"], report["parent"])


if __name__ == "__main__":
    unittest.main()
