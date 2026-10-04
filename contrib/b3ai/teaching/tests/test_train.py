# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Tiny own-language CPU parents check teaching resets and checkpoint selection."""

import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

import numpy as np
import torch

from contrib.b3ai.language.checkpoint import load_checkpoint, save_checkpoint
from contrib.b3ai.language.model import GpuConfig, TorchTransformer
from contrib.b3ai.language import train as language_train
from contrib.b3ai.teaching.data import Curriculum
from contrib.b3ai.teaching import train


class TeachingTrainTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.previous_threads = torch.get_num_threads()
        torch.set_num_threads(1)
        cls.addClassCleanup(torch.set_num_threads, cls.previous_threads)
        cls.parent_directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.parent_directory.cleanup)
        cls.repository = Path(__file__).resolve().parents[4]
        cls.baseline = subprocess.check_output(
            ["git", "-C", str(cls.repository), "rev-parse", "HEAD"], text=True).strip()
        source = subprocess.check_output(
            ["git", "-C", str(cls.repository), "show", cls.baseline + ":README.md"])
        cls.source_sha = hashlib.sha256(source).hexdigest()
        cls.evidence = source.decode("utf-8").splitlines()[0]
        cls.config = GpuConfig(context_length=256, width=8, layers=1, heads=2, seed=42)
        model = TorchTransformer(cls.config)
        optimizer = torch.optim.AdamW(list(model.params.values()), lr=0.0003,
                                     weight_decay=0.01, foreach=False, fused=False)
        # The inherited optimizer has real moments and four updates, rather than
        # a fabricated counter; every byte/EOD transition contributes to the loss.
        tokens = torch.tensor([[65, 66, 67]], dtype=torch.int64)
        targets = torch.tensor([[66, 67, 256]], dtype=torch.int64)
        for _ in range(4):
            optimizer.zero_grad(set_to_none=True)
            model.loss(tokens, targets).backward()
            optimizer.step()
        parent_rng = np.random.default_rng(cls.config.seed)
        parent_rng.integers(100, size=12)
        cls.parent_metadata = {
            "stage": "language-pilot",
            "experimental": True,
            "model_family_origin": language_train.MODEL_FAMILY_ORIGIN,
            "component_hashes": language_train.component_hashes(),
            "implementation_hash": language_train.implementation_hash(),
            "runtime": language_train.runtime("cpu"),
            "selected_or_saved_step": 4,
            "training_tokens_at_save": 12,
            "batch_rng_state": parent_rng.bit_generator.state,
        }
        cls.parent_path = Path(cls.parent_directory.name) / "own-language.npz"
        save_checkpoint(cls.parent_path, model, optimizer, cls.parent_metadata)
        cls.parent_bytes = cls.parent_path.read_bytes()
        cls.parent_sha = hashlib.sha256(cls.parent_bytes).hexdigest()

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.curriculum_path = self.directory / "curriculum.json"
        self.document = {
            "schema": "b3-teaching-curriculum/v1",
            "baseline_commit": self.baseline,
            "review": {"status": "source-reviewed", "scope": "Synthetic CPU test fixtures.",
                       "not_independent_exam": True},
            "topics": [],
        }
        for index, answer in enumerate(("B3 Hive.", "Élan café 🐝 has a longer answer.")):
            self.document["topics"].append({
                "id": "fixture-" + str(index),
                "status": "implemented",
                "questions": ["Fixture " + str(index) + " question " + str(number) + "?" for number in range(4)],
                "validation_question": "Held-out fixture " + str(index) + "?",
                "answer": answer,
                "sources": [{"file": "README.md", "line_start": 1, "line_end": 1,
                             "file_sha256": self.source_sha, "evidence": self.evidence}],
                "limits": "This is a representation test, not an independent factual exam.",
            })
        self.curriculum_path.write_text(json.dumps(self.document, ensure_ascii=False) + "\n", encoding="utf-8")
        self.options = {"initialize_from": self.parent_path, "expected_parent_sha256": self.parent_sha,
                        "device": "cpu", "steps": 2, "batch_size": 2, "learning_rate": 0.0003,
                        "evaluation_interval": 1, "max_seconds": 60, "max_tokens": 10000}

    def fit(self, output_name="run", **options):
        with contextlib.redirect_stdout(io.StringIO()):
            return train.fit(self.curriculum_path, self.directory / output_name, **dict(self.options, **options))

    def assert_equal_weights(self, first, second):
        self.assertEqual(set(first.params), set(second.params))
        for name in first.params:
            self.assertTrue(torch.equal(first.params[name], second.params[name]), name)

    def assert_steps(self, model, optimizer, count):
        if count == 0:
            self.assertEqual(len(optimizer.state), 0)
        else:
            self.assertEqual(set(optimizer.state), set(model.params.values()))
            for state in optimizer.state.values():
                self.assertEqual(state["step"].item(), count)

    def test_token_cap_keeps_parent_weights_and_starts_with_fresh_optimizer_and_rng(self):
        report = self.fit(max_tokens=1)
        self.assertEqual(report["stop_reason"], "token_limit")
        self.assertEqual(report["attempted_steps"], 0)
        self.assertEqual(report["selected_step"], 0)
        self.assertEqual(report["training_tokens"], 0)
        self.assertEqual(report["inherited_language_steps"], 4)
        self.assertEqual(report["inherited_language_training_tokens"], 12)
        self.assertTrue(report["optimizer_reset"])
        self.assertTrue(report["batch_rng_reset"])
        parent_model, parent_optimizer, _ = load_checkpoint(self.parent_path)
        self.assert_steps(parent_model, parent_optimizer, 4)
        fresh_rng = np.random.default_rng(42).bit_generator.state
        self.assertNotEqual(fresh_rng, self.parent_metadata["batch_rng_state"])
        for name in ("model.npz", "last.npz"):
            model, optimizer, metadata = load_checkpoint(self.directory / "run" / name)
            self.assert_equal_weights(parent_model, model)
            self.assert_steps(model, optimizer, 0)
            self.assertEqual(metadata["stage"], train.STAGE)
            self.assertEqual(metadata["selected_or_saved_step"], 0)
            self.assertEqual(metadata["training_tokens_at_save"], 0)
            self.assertEqual(metadata["inherited_language_steps"], 4)
            self.assertEqual(metadata["batch_rng_state"], fresh_rng)
            self.assertEqual(metadata["initial_batch_rng_state"], fresh_rng)
            self.assertEqual(optimizer.param_groups[0]["lr"], 0.0003)
        self.assertEqual(self.parent_path.read_bytes(), self.parent_bytes)
        self.assertTrue(report["parent_unchanged"])

    def test_real_updates_reproduce_selected_loss_over_every_validation_answer(self):
        report = self.fit()
        self.assertEqual(report["attempted_steps"], 2)
        self.assertEqual([item["step"] for item in report["history"]], [0, 1, 2])
        self.assertFalse(report["independent_exam_used"])
        curriculum = Curriculum(self.curriculum_path, context_length=256, repository=self.repository)
        model, optimizer, metadata = load_checkpoint(self.directory / "run" / "model.npz")
        last_model, last_optimizer, last = load_checkpoint(self.directory / "run" / "last.npz")
        self.assert_steps(model, optimizer, report["selected_step"])
        self.assert_steps(last_model, last_optimizer, 2)
        self.assertEqual(last["selected_or_saved_step"], 2)
        self.assertEqual(metadata["selected_or_saved_step"], report["selected_step"])
        actual = train.score(model, curriculum, "validation", 2, "cpu")
        self.assertAlmostEqual(actual, metadata["validation_loss_at_save"], places=6)
        self.assertAlmostEqual(actual, report["best_validation_loss"], places=6)
        # Independently score each full held-out answer, including both terminal
        # targets, and weight by answer bytes rather than number of questions.
        weighted, total = 0.0, 0
        with torch.no_grad():
            for record in curriculum.records["validation"]:
                arrays = curriculum.arrays(record)
                tensors = tuple(torch.from_numpy(array[None, :]) for array in arrays)
                count = int(arrays[2].sum())
                weighted += model.loss(*tensors).item() * count
                total += count
        self.assertAlmostEqual(actual, weighted / total, places=5)
        self.assertEqual(metadata["curriculum_sha256"], hashlib.sha256(self.curriculum_path.read_bytes()).hexdigest())
        self.assertEqual(metadata["baseline_commit"], self.baseline)
        self.assertEqual(metadata["source_hashes"], {"README.md": self.source_sha})
        self.assertEqual(metadata["parent_checkpoint_sha256"], self.parent_sha)
        self.assertEqual(metadata["component_hashes"], language_train.component_hashes())
        parent_model, _, _ = load_checkpoint(self.parent_path)
        self.assertTrue(any(not torch.equal(parent_model.params[name], last_model.params[name]) for name in parent_model.params))
        self.assertEqual(self.parent_path.read_bytes(), self.parent_bytes)

    def test_earlier_best_checkpoint_preserves_its_local_optimizer_rng_and_token_counts(self):
        # Evaluation is controlled; both tiny teaching updates still use real
        # backpropagation and fresh AdamW moments.
        with mock.patch.object(train, "score", side_effect=[2.0, 3.0, 2.0, 1.0, 2.0, 2.0]):
            report = self.fit()
        self.assertEqual(report["attempted_steps"], 2)
        self.assertEqual(report["selected_step"], 1)
        selected_model, selected_optimizer, selected = load_checkpoint(self.directory / "run" / "model.npz")
        last_model, last_optimizer, last = load_checkpoint(self.directory / "run" / "last.npz")
        self.assert_steps(selected_model, selected_optimizer, 1)
        self.assert_steps(last_model, last_optimizer, 2)
        self.assertEqual(selected["selected_or_saved_step"], 1)
        self.assertEqual(last["selected_or_saved_step"], 2)
        self.assertEqual(selected["validation_loss_at_save"], 1.0)
        self.assertEqual(last["validation_loss_at_save"], 2.0)
        self.assertEqual(selected["inherited_language_steps"], 4)
        self.assertTrue(any(not torch.equal(selected_model.params[name], last_model.params[name]) for name in selected_model.params))
        curriculum = Curriculum(self.curriculum_path, context_length=256, repository=self.repository)
        rng, count = np.random.default_rng(42), 0
        for step in (1, 2):
            count += int(curriculum.batch("train", rng, 2)[2].sum())
            saved = selected if step == 1 else last
            self.assertEqual(saved["batch_rng_state"], rng.bit_generator.state)
            self.assertEqual(saved["training_tokens_at_save"], count)
        self.assertEqual(report["training_tokens"], count)
        for name, key in (("model.npz", "selected_checkpoint_sha256"), ("last.npz", "last_checkpoint_sha256")):
            self.assertEqual(report[key], hashlib.sha256((self.directory / "run" / name).read_bytes()).hexdigest())
        self.assertEqual(self.parent_path.read_bytes(), self.parent_bytes)

    def test_baseline_can_win_after_real_updates_with_fresh_zero_step_state(self):
        with mock.patch.object(train, "score", side_effect=[2.0, 1.0, 2.0, 2.0, 2.0, 3.0]):
            report = self.fit()
        self.assertEqual(report["attempted_steps"], 2)
        self.assertEqual(report["selected_step"], 0)
        self.assertEqual(report["best_validation_loss"], 1.0)
        model, optimizer, metadata = load_checkpoint(self.directory / "run" / "model.npz")
        last_model, last_optimizer, last = load_checkpoint(self.directory / "run" / "last.npz")
        parent_model, _, _ = load_checkpoint(self.parent_path)
        self.assert_equal_weights(parent_model, model)
        self.assert_steps(model, optimizer, 0)
        self.assert_steps(last_model, last_optimizer, 2)
        self.assertEqual(metadata["selected_or_saved_step"], 0)
        self.assertEqual(metadata["training_tokens_at_save"], 0)
        self.assertEqual(metadata["batch_rng_state"], np.random.default_rng(42).bit_generator.state)
        self.assertEqual(metadata["inherited_language_steps"], 4)
        self.assertEqual(last["selected_or_saved_step"], 2)
        self.assertGreater(last["training_tokens_at_save"], 0)
        self.assertTrue(any(not torch.equal(model.params[name], last_model.params[name]) for name in model.params))
        self.assertEqual(self.parent_path.read_bytes(), self.parent_bytes)

    def test_score_weights_all_answer_targets_without_sampling(self):
        batches = [
            (np.array([[1, 2]], dtype=np.int64), np.array([[2, 256]], dtype=np.int64), np.ones((1, 2), dtype=np.float32)),
            (np.array([[3, 4]], dtype=np.int64), np.array([[4, 256]], dtype=np.int64), np.array([[0, 1]], dtype=np.float32)),
        ]
        curriculum = mock.Mock()
        curriculum.evaluation_batches.return_value = iter(batches)
        model = mock.Mock()
        model.loss.side_effect = [torch.tensor(2.0), torch.tensor(6.0)]
        self.assertEqual(train.score(model, curriculum, "validation", 2, "cpu"), 10 / 3)
        curriculum.evaluation_batches.assert_called_once_with("validation", 2)
        self.assertEqual(model.loss.call_count, 2)
        for call, batch in zip(model.loss.call_args_list, batches):
            for tensor, array in zip(call.args, batch):
                np.testing.assert_array_equal(tensor.numpy(), array)

    def test_score_resource_guard_stops_after_a_complete_batch_without_returning_partial_loss(self):
        batch = (np.array([[1, 2]], dtype=np.int64), np.array([[2, 256]], dtype=np.int64),
                 np.ones((1, 2), dtype=np.float32))
        curriculum = mock.Mock()
        curriculum.evaluation_batches.return_value = iter((batch, batch))
        model = mock.Mock()
        model.loss.return_value = torch.tensor(2.0)
        checks = []

        def guard():
            checks.append(model.loss.call_count)
            if model.loss.call_count == 1:
                raise train.ResourceLimitError("memory_limit")

        with self.assertRaises(train.ResourceLimitError) as raised:
            train.score(model, curriculum, "validation", 2, "cpu", check_resources=guard)
        self.assertEqual(raised.exception.reason, "memory_limit")
        self.assertEqual(checks, [0, 1])
        self.assertEqual(model.loss.call_count, 1)

    def test_memory_cap_before_baseline_saves_a_finite_report_without_checkpoint_or_evaluation(self):
        memory = {"process_peak_rss_bytes": train.MAX_MEMORY, "gpu_driver_allocated_bytes": 0}
        with mock.patch.object(language_train, "memory", return_value=memory), \
                mock.patch.object(train, "score") as scorer, \
                mock.patch.object(train, "save_checkpoint") as saver:
            report = self.fit()
        scorer.assert_not_called()
        saver.assert_not_called()
        self.assertEqual(report["stop_reason"], "memory_limit")
        self.assertEqual(report["history"], [])
        self.assertEqual(report["attempted_steps"], 0)
        self.assertEqual(report["training_tokens"], 0)
        self.assertIsNone(report["best_validation_loss"])
        self.assertIsNone(report["selected_checkpoint_sha256"])
        self.assertIsNone(report["last_checkpoint_sha256"])
        self.assertFalse(report["selected_checkpoint_available"])
        self.assertFalse(report["last_state_validation_available"])
        self.assertEqual({path.name for path in (self.directory / "run").iterdir()}, {"report.json"})
        saved = json.loads((self.directory / "run" / "report.json").read_text(),
                           parse_constant=lambda value: self.fail("Non-finite report constant: " + value))
        self.assertEqual(saved, report)
        self.assertEqual(self.parent_path.read_bytes(), self.parent_bytes)

    def test_memory_cap_after_real_update_retains_baseline_and_skips_last_checkpoint(self):
        real_step = torch.optim.AdamW.step
        updates = []

        def observed_step(optimizer, *args, **kwargs):
            result = real_step(optimizer, *args, **kwargs)
            updates.append(1)
            return result

        def memory(device):
            self.assertEqual(device, "cpu")
            return {"process_peak_rss_bytes": train.MAX_MEMORY if updates else 1,
                    "gpu_driver_allocated_bytes": 0}

        with mock.patch.object(torch.optim.AdamW, "step", observed_step), \
                mock.patch.object(language_train, "memory", side_effect=memory), \
                mock.patch.object(train, "score", wraps=train.score) as scorer:
            report = self.fit()
        self.assertEqual(len(updates), 1)
        self.assertEqual(scorer.call_count, 2, "Only the complete train/validation baseline may be evaluated.")
        self.assertEqual(report["stop_reason"], "memory_limit")
        self.assertEqual(report["attempted_steps"], 1)
        self.assertGreater(report["training_tokens"], 0)
        self.assertEqual(report["selected_step"], 0)
        self.assertEqual([entry["step"] for entry in report["history"]], [0])
        self.assertTrue(report["selected_checkpoint_available"])
        self.assertIsNone(report["last_checkpoint_sha256"])
        self.assertFalse(report["last_state_validation_available"])
        self.assertFalse((self.directory / "run" / "last.npz").exists())
        selected_path = self.directory / "run" / "model.npz"
        model, optimizer, metadata = load_checkpoint(selected_path)
        self.assertEqual(metadata["selected_or_saved_step"], 0)
        self.assert_steps(model, optimizer, 0)
        parent_model, _, _ = load_checkpoint(self.parent_path)
        self.assert_equal_weights(parent_model, model)
        self.assertEqual(report["selected_checkpoint_sha256"], hashlib.sha256(selected_path.read_bytes()).hexdigest())
        saved = json.loads((self.directory / "run" / "report.json").read_text(),
                           parse_constant=lambda value: self.fail("Non-finite report constant: " + value))
        self.assertEqual(saved, report)
        self.assertEqual(self.parent_path.read_bytes(), self.parent_bytes)

    def test_nonfinite_training_loss_or_gradient_preserves_baseline_and_writes_finite_failure_report(self):
        real_loss = TorchTransformer.loss
        for failure_kind in ("loss", "gradient"):
            with self.subTest(failure=failure_kind):
                output_name = "nonfinite-" + failure_kind

                def broken_loss(model, *args, **kwargs):
                    value = real_loss(model, *args, **kwargs)
                    if torch.is_grad_enabled():
                        if failure_kind == "loss":
                            return value * torch.tensor(float("nan"))
                        handle = next(iter(model.params.values())).register_hook(
                            lambda gradient: torch.full_like(gradient, float("nan")))
                        self.addCleanup(handle.remove)
                    return value

                with mock.patch.object(TorchTransformer, "loss", broken_loss), \
                        mock.patch.object(torch.optim.AdamW, "step") as updater:
                    with self.assertRaisesRegex((ValueError, RuntimeError), "non-finite"):
                        self.fit(output_name)
                updater.assert_not_called()
                output = self.directory / output_name
                report = json.loads((output / "report.json").read_text(),
                    parse_constant=lambda value: self.fail("Non-finite failure report constant: " + value))
                self.assertTrue(report["status"].startswith("failed teaching experiment"))
                self.assertEqual(report["stop_reason"], "failure")
                self.assertIn("non-finite", report["failure"])
                self.assertEqual(report["attempted_steps"], 0)
                self.assertEqual(report["training_tokens"], 0)
                self.assertEqual([entry["step"] for entry in report["history"]], [0])
                self.assertTrue(report["selected_checkpoint_available"])
                self.assertIsNone(report["last_checkpoint_sha256"])
                self.assertFalse(report["last_state_validation_available"])
                self.assertFalse((output / "last.npz").exists())
                model, optimizer, metadata = load_checkpoint(output / "model.npz")
                parent_model, _, _ = load_checkpoint(self.parent_path)
                self.assert_equal_weights(parent_model, model)
                self.assert_steps(model, optimizer, 0)
                self.assertEqual(metadata["selected_or_saved_step"], 0)
                self.assertEqual(report["selected_checkpoint_sha256"], hashlib.sha256((output / "model.npz").read_bytes()).hexdigest())
        self.assertEqual(self.parent_path.read_bytes(), self.parent_bytes)

    def test_teaching_checkpoints_are_rejected_by_language_continuation_stage_guard(self):
        self.fit(max_tokens=1)
        model, optimizer, metadata = load_checkpoint(self.directory / "run" / "model.npz")
        metadata["corpus_manifest_hash"] = "x"
        # Everything checked beside stage in the first language guard matches;
        # rejection therefore does not depend on a different shape or runtime.
        with self.assertRaisesRegex(ValueError, "Continuation configuration, corpus or runtime"):
            language_train._parent_state(model, optimizer, metadata, model.config,
                language_train.runtime("cpu"), SimpleNamespace(manifest_hash="x"), 2, 0.0003, 1,
                language_train.implementation_hash(), language_train.component_hashes(), 2)

    def test_final_save_or_device_failure_preserves_selected_and_writes_finite_failure_report(self):
        real_save, real_sync = train.save_checkpoint, language_train.synchronize
        cases = (("save-os", OSError), ("save-value", ValueError),
                 ("save-runtime", RuntimeError), ("device", RuntimeError))
        for failure_kind, exception_type in cases:
            with self.subTest(failure=failure_kind):
                output = self.directory / failure_kind
                selected_bytes = []
                problem = "injected final " + failure_kind + " failure"
                options, expected_steps = {}, 2
                scorer = contextlib.nullcontext()
                if failure_kind == "save-os":
                    # A token stop between scheduled evaluations selects better
                    # weights only during the final fallback evaluation.
                    curriculum = Curriculum(self.curriculum_path, context_length=256)
                    tokens = int(curriculum.batch("train", np.random.default_rng(42), 2)[2].sum())
                    options = {"evaluation_interval": 3, "max_tokens": tokens}
                    expected_steps = 1
                    scorer = mock.patch.object(train, "score", side_effect=[2.0, 3.0, 2.0, 1.0])

                def save(path, *args, **kwargs):
                    if path.name == "last.npz" and failure_kind != "device":
                        raise exception_type(problem)
                    real_save(path, *args, **kwargs)
                    if path.name == "model.npz":
                        selected_bytes.append(path.read_bytes())

                def synchronize(device):
                    if failure_kind == "device" and (output / "last.npz").exists():
                        raise exception_type(problem)
                    real_sync(device)

                with scorer, mock.patch.object(train, "save_checkpoint", side_effect=save), \
                        mock.patch.object(language_train, "synchronize", side_effect=synchronize):
                    with self.assertRaisesRegex(exception_type, problem):
                        self.fit(failure_kind, **options)
                report = json.loads((output / "report.json").read_text(),
                    parse_constant=lambda value: self.fail("Non-finite failure report constant: " + value))
                self.assertTrue(report["status"].startswith("failed teaching experiment"))
                self.assertEqual(report["stop_reason"], "failure")
                self.assertEqual(report["failure"], problem)
                self.assertEqual(report["attempted_steps"], expected_steps)
                self.assertEqual([item["step"] for item in report["history"]], list(range(expected_steps + 1)))
                self.assertGreater(report["training_tokens"], 0)
                self.assertTrue(report["selected_checkpoint_available"])
                self.assertIsNone(report["last_checkpoint_sha256"])
                self.assertFalse(report["last_state_validation_available"])
                self.assertEqual((output / "last.npz").exists(), failure_kind == "device")
                self.assertTrue(selected_bytes)
                self.assertEqual((output / "model.npz").read_bytes(), selected_bytes[-1])
                self.assertEqual(report["selected_checkpoint_sha256"], hashlib.sha256(selected_bytes[-1]).hexdigest())
                model, optimizer, metadata = load_checkpoint(output / "model.npz")
                self.assert_steps(model, optimizer, report["selected_step"])
                self.assertEqual(metadata["selected_or_saved_step"], report["selected_step"])
                self.assertEqual(metadata["validation_loss_at_save"], report["best_validation_loss"])
                if failure_kind == "save-os":
                    self.assertEqual(report["selected_step"], 1)
                    self.assertEqual(report["best_validation_loss"], 1.0)
                self.assertEqual(self.parent_path.read_bytes(), self.parent_bytes)

    def test_existing_output_is_never_overwritten(self):
        output = self.directory / "run"
        output.mkdir()
        sentinel = output / "leave-me.txt"
        sentinel.write_bytes(b"existing output remains intact")
        with self.assertRaises(FileExistsError):
            self.fit()
        self.assertEqual(list(output.iterdir()), [sentinel])
        self.assertEqual(sentinel.read_bytes(), b"existing output remains intact")
        self.assertEqual(self.parent_path.read_bytes(), self.parent_bytes)

    def test_expected_parent_hash_is_required_and_checked_before_output_creation(self):
        for index, value in enumerate((None, "", "0" * 64, self.parent_sha[:12])):
            with self.subTest(hash=value):
                name = "bad-hash-" + str(index)
                with self.assertRaises(ValueError):
                    self.fit(name, expected_parent_sha256=value)
                self.assertFalse((self.directory / name).exists())
        self.assertEqual(self.parent_path.read_bytes(), self.parent_bytes)

    def test_incompatible_parent_provenance_and_inherited_update_counts_are_rejected(self):
        mutations = {
            "stage": lambda metadata: metadata.__setitem__("stage", train.STAGE),
            "origin": lambda metadata: metadata.__setitem__("model_family_origin", "external weights"),
            "implementation": lambda metadata: metadata.__setitem__("implementation_hash", "0" * 64),
            "component": lambda metadata: metadata["component_hashes"].__setitem__("model.py", "0" * 64),
            "runtime": lambda metadata: metadata["runtime"].__setitem__("torch", "incompatible"),
            "optimizer count": lambda metadata: metadata.__setitem__("selected_or_saved_step", 5),
            "token count": lambda metadata: metadata.__setitem__("training_tokens_at_save", -1),
        }
        model, optimizer, _ = load_checkpoint(self.parent_path)
        for index, (label, mutate) in enumerate(mutations.items()):
            with self.subTest(case=label):
                metadata = copy.deepcopy(self.parent_metadata)
                mutate(metadata)
                candidate = self.directory / ("bad-parent-" + str(index) + ".npz")
                save_checkpoint(candidate, model, optimizer, metadata)
                expected = hashlib.sha256(candidate.read_bytes()).hexdigest()
                name = "rejected-" + str(index)
                with self.assertRaises(ValueError):
                    self.fit(name, initialize_from=candidate, expected_parent_sha256=expected)
                self.assertFalse((self.directory / name).exists())
        self.assertEqual(self.parent_path.read_bytes(), self.parent_bytes)


if __name__ == "__main__":
    unittest.main()
