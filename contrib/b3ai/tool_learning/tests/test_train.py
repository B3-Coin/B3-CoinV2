# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Real tiny CPU updates check mixed scheduling, selection and safe persistence."""

import contextlib
import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np
import torch

from contrib.b3ai.language.checkpoint import load_checkpoint, save_checkpoint
from contrib.b3ai.language.model import GpuConfig, TorchTransformer
from contrib.b3ai.language import train as language_train
from contrib.b3ai.tool_learning import train


SOURCE_HASHES = {"README.md": "a" * 64, "src/qt/b3aipublicdata.cpp": "b" * 64}


def batch(rng, size, instruction=False):
    tokens = rng.integers(65, 70, (size, 7), dtype=np.int64)
    targets = np.roll(tokens, -1, axis=1)
    targets[:, -1] = 256
    mask = np.ones(tokens.shape, dtype=np.float32)
    if instruction:
        mask[:, :3] = 0
    return tokens, targets, mask


class Stories:
    events = []
    manifest_hash = "d" * 64
    manifest = {"splits": {name: {"sha256": character * 64}
                           for name, character in (("train", "1"), ("validation", "2"), ("test", "3"))}}

    def __init__(self, path):
        self.splits = {name: [1] for name in self.manifest["splits"]}

    def batch(self, split, rng, size, context):
        if split == "train":
            self.events.append("language")
        return batch(rng, size)


class Instructions:
    sha256 = "e" * 64
    source_curriculum_sha256 = "c" * 64
    projection_source_sha256 = "b" * 64
    baseline_commit = "a" * 40
    source_hashes = SOURCE_HASHES
    review = {"status": "source-reviewed", "simulated_results": True}

    def __init__(self, path, context_length=1024):
        self.context_length = context_length
        self.records = {"train": [1, 2], "validation": [3, 4, 5]}

    def batch(self, split, rng, size):
        Stories.events.append("instruction")
        return batch(rng, size, True)

    def evaluation_batches(self, split, size):
        # Complete validation covers three differently sized batches, with a
        # deliberately different number of supervised targets in each.
        rng = np.random.default_rng(222)
        for count in (1, 2, 1):
            yield batch(rng, count, True)


class MixedTrainTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        previous = torch.get_num_threads()
        torch.set_num_threads(1)
        cls.addClassCleanup(torch.set_num_threads, previous)
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.parent = Path(cls.temporary.name) / "b3-parent.npz"
        cls.config = GpuConfig(context_length=32, width=8, layers=1, heads=2)
        model = TorchTransformer(cls.config)
        tokens, targets, mask = (torch.from_numpy(value) for value in batch(np.random.default_rng(8), 2))
        optimizer = torch.optim.AdamW(list(model.params.values()), lr=0.0003,
                                     weight_decay=0.01, foreach=False, fused=False)
        for _ in range(4):
            optimizer.zero_grad(set_to_none=True)
            model.loss(tokens, targets, mask).backward()
            optimizer.step()
        optimizer = torch.optim.AdamW(list(model.params.values()), lr=0.0001,
                                     weight_decay=0.01, foreach=False, fused=False)
        for _ in range(2):
            optimizer.zero_grad(set_to_none=True)
            model.loss(tokens, targets, mask).backward()
            optimizer.step()
        cls.metadata = {"stage": "b3-supervised-answers", "experimental": True,
            "model_family_origin": language_train.MODEL_FAMILY_ORIGIN,
            "component_hashes": language_train.component_hashes(), "runtime": language_train.runtime("cpu"),
            "inherited_language_steps": 4, "selected_or_saved_step": 2, "optimizer_reset": True,
            "curriculum_sha256": Instructions.source_curriculum_sha256,
            "baseline_commit": Instructions.baseline_commit, "source_hashes": SOURCE_HASHES,
            "implementation_hash": "f" * 64}
        save_checkpoint(cls.parent, model, optimizer, cls.metadata)
        cls.parent_bytes = cls.parent.read_bytes()
        cls.parent_sha = hashlib.sha256(cls.parent_bytes).hexdigest()

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        Stories.events = []
        self.options = {"initialize_from": self.parent, "expected_parent_sha256": self.parent_sha,
                        "device": "cpu", "steps": 3, "evaluation_interval": 100, "max_seconds": 60}
        self.plan = {"schema": "b3-mixed-tool-plan/v1", "stage": train.STAGE,
            "initial_parent_sha256": self.parent_sha, "inherited_language_steps": 4,
            "inherited_b3_teaching_steps": 2, "language_manifest_sha256": Stories.manifest_hash,
            "instruction_corpus_sha256": Instructions.sha256,
            "source_curriculum_sha256": Instructions.source_curriculum_sha256,
            "source_baseline": Instructions.baseline_commit, "projection_source_sha256": Instructions.projection_source_sha256,
            "language_batch_size": 4, "instruction_batch_size": 8, "language_context_length": 32,
            "instruction_context_cap": 32, "learning_rate": 0.0001, "clip_norm": 1.0,
            "evaluation_interval": 100, "maximum_chunks": 2, "maximum_updates_per_chunk": 1000,
            "maximum_attempted_chain_updates": 2000, "maximum_wall_seconds_per_chunk": 500,
            "maximum_chain_wall_seconds": 1100, "maximum_training_targets_per_chunk": 8192000,
            "maximum_memory_bytes": train.MAX_MEMORY}
        for name, value in (("Corpus", Stories), ("InstructionCorpus", Instructions),
                            ("_plan_document", lambda: copy.deepcopy(self.plan)), ("_plan_hash", lambda: "9" * 64)):
            patcher = mock.patch.object(train, name, value)
            patcher.start()
            self.addCleanup(patcher.stop)

    def fit(self, name="run", **options):
        with contextlib.redirect_stdout(io.StringIO()):
            return train.fit("stories", "instructions", self.directory / name, **dict(self.options, **options))

    def report(self, name="run"):
        return json.loads((self.directory / name / "report.json").read_text(),
                          parse_constant=lambda value: self.fail("Non-finite report: " + value))

    def assert_parameters_equal(self, first, second):
        for name in first.params:
            self.assertTrue(torch.equal(first.params[name], second.params[name]), name)

    def test_step_zero_inherits_weights_only_and_rolls_back_rejected_batch_rng(self):
        report = self.fit(max_tokens=1)
        self.assertEqual(report["attempted_steps"], 0)
        self.assertTrue(report["optimizer_reset"])
        self.assertTrue(report["batch_rng_reset"])
        self.assertEqual(report["inherited_language_steps"], 4)
        self.assertEqual(report["inherited_b3_teaching_steps"], 2)
        parent, _, _ = load_checkpoint(self.parent)
        for name in ("model.npz", "last.npz"):
            model, optimizer, metadata = load_checkpoint(self.directory / "run" / name)
            self.assert_parameters_equal(parent, model)
            self.assertFalse(optimizer.state)
            self.assertEqual(metadata["selected_or_saved_step"], 0)
            self.assertEqual(metadata["batch_rng_state"], np.random.default_rng(42).bit_generator.state)
        self.assertEqual(self.parent.read_bytes(), self.parent_bytes)
        self.assertIsNone(report["history"][0]["train"])

    def test_split_continuation_matches_six_updates_bitwise_and_preserves_global_phase(self):
        first = self.fit("first", steps=2)
        path = self.directory / "first" / "last.npz"
        original = path.read_bytes()
        second = self.fit("second", steps=4, initialize_from=path,
                          expected_parent_sha256=hashlib.sha256(original).hexdigest())
        self.assertEqual(Stories.events, ["language", "language", "instruction", "language", "language", "instruction"])
        Stories.events = []
        whole = self.fit("whole", steps=6)
        self.assertEqual(Stories.events, ["language", "language", "instruction", "language", "language", "instruction"])
        resumed, resumed_opt, resumed_meta = load_checkpoint(self.directory / "second" / "last.npz")
        direct, direct_opt, direct_meta = load_checkpoint(self.directory / "whole" / "last.npz")
        self.assert_parameters_equal(resumed, direct)
        for name in resumed.params:
            a, b = resumed_opt.state[resumed.params[name]], direct_opt.state[direct.params[name]]
            for key in ("step", "exp_avg", "exp_avg_sq"):
                self.assertTrue(torch.equal(a[key], b[key]), name + ":" + key)
        for key in ("batch_rng_state", "selected_or_saved_step", "training_tokens_at_save",
                    "story_tokens_at_save", "instruction_tokens_at_save"):
            self.assertEqual(resumed_meta[key], direct_meta[key])
        self.assertEqual(resumed_meta["selected_or_saved_step"], 6)
        self.assertEqual(second["base_mixed_step"], 2)
        self.assertFalse(second["optimizer_reset"])
        self.assertEqual(first["training_tokens"] + second["training_tokens"], whole["training_tokens"])
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(self.parent.read_bytes(), self.parent_bytes)
        with self.assertRaisesRegex(ValueError, "two mixed chunks"):
            self.fit("third", initialize_from=self.directory / "second" / "last.npz",
                expected_parent_sha256=hashlib.sha256((self.directory / "second" / "last.npz").read_bytes()).hexdigest())
        self.assertFalse((self.directory / "third").exists())

    def test_validation_uses_separate_target_weighting_then_two_to_one_combination(self):
        model = mock.Mock()
        model.loss.side_effect = [torch.tensor(1.0), torch.tensor(4.0), torch.tensor(7.0)]
        with mock.patch.object(language_train, "score", return_value=2.0) as stories:
            values = train.score_validation(model, ["fixed-story-batch"], Instructions("unused"), "cpu")
        self.assertEqual(values["language"], 2.0)
        self.assertEqual(values["instruction"], (1 * 4 + 4 * 8 + 7 * 4) / 16)
        self.assertEqual(values["combined"], (2 * 2 + values["instruction"]) / 3)
        self.assertEqual(stories.call_count, 1)
        self.assertEqual(model.loss.call_count, 3)

    def test_rejected_instruction_batch_preserves_state_after_two_story_updates(self):
        # Each complete story batch has 4*7 targets; the next instruction batch
        # would exceed this cap and must leave its RNG consumption uncommitted.
        limited = self.fit("limited", max_tokens=56)
        self.fit("two", steps=2)
        a, ao, am = load_checkpoint(self.directory / "limited" / "last.npz")
        b, bo, bm = load_checkpoint(self.directory / "two" / "last.npz")
        self.assertEqual(limited["stop_reason"], "token_limit")
        self.assertEqual(limited["attempted_steps"], 2)
        self.assertEqual(limited["story_training_tokens"], 56)
        self.assertEqual(limited["instruction_training_tokens"], 0)
        self.assert_parameters_equal(a, b)
        self.assertEqual(am["batch_rng_state"], bm["batch_rng_state"])
        for name in a.params:
            for key in ("step", "exp_avg", "exp_avg_sq"):
                self.assertTrue(torch.equal(ao.state[a.params[name]][key], bo.state[b.params[name]][key]))

    def test_baseline_and_earlier_best_selection_preserve_correct_moments_and_metrics(self):
        self.plan["evaluation_interval"] = 1
        for name, losses, selected in (("baseline", [1., 2., 3., 4.], 0), ("earlier", [3., 1., 2., 4.], 1)):
            with self.subTest(selection=name), mock.patch.object(train, "score_validation", side_effect=[
                    {"language": x, "instruction": x, "combined": x} for x in losses]):
                report = self.fit(name, evaluation_interval=1)
            model, optimizer, metadata = load_checkpoint(self.directory / name / "model.npz")
            self.assertEqual(report["selected_step"], selected)
            self.assertEqual(metadata["selected_or_saved_step"], selected)
            self.assertEqual(metadata["validation_loss_at_save"], min(losses))
            self.assertEqual(report["best_validation_loss"], min(losses))
            if selected == 0:
                self.assertFalse(optimizer.state)
                parent, _, _ = load_checkpoint(self.parent)
                self.assert_parameters_equal(model, parent)
            else:
                self.assertTrue(all(value["step"].item() == selected for value in optimizer.state.values()))

    def test_expected_hash_existing_output_and_changed_initial_sources_stop_safely(self):
        with mock.patch.object(train, "load_checkpoint") as loader:
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                self.fit(expected_parent_sha256="0" * 64)
        loader.assert_not_called()
        self.assertFalse((self.directory / "run").exists())
        existing = self.directory / "run"
        existing.mkdir()
        sentinel = existing / "keep"
        sentinel.write_text("preserved")
        with self.assertRaises(FileExistsError):
            self.fit()
        self.assertEqual(sentinel.read_text(), "preserved")
        model, optimizer, metadata = load_checkpoint(self.parent)
        metadata["source_hashes"] = {"README.md": "a" * 64}
        changed = self.directory / "missing-proof.npz"
        save_checkpoint(changed, model, optimizer, metadata)
        with self.assertRaisesRegex(ValueError, "source provenance"):
            self.fit("changed", initialize_from=changed, expected_parent_sha256=hashlib.sha256(changed.read_bytes()).hexdigest())
        self.assertFalse((self.directory / "changed").exists())

    def test_continuation_rejects_changed_plan_runtime_or_instruction_identity(self):
        self.fit("first")
        path = self.directory / "first" / "last.npz"
        options = {"initialize_from": path, "expected_parent_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        for name, patcher in (("plan", mock.patch.object(train, "_plan_hash", return_value="8" * 64)),
                              ("instructions", mock.patch.object(Instructions, "sha256", "7" * 64)),
                              ("code", mock.patch.object(train, "implementation_hash", return_value="6" * 64)),
                              ("runtime", mock.patch.object(language_train, "runtime", return_value={**language_train.runtime("cpu"), "torch": "changed"}))):
            with self.subTest(change=name), patcher, self.assertRaisesRegex(ValueError, "continuation code"):
                self.fit(name, **options)
            self.assertFalse((self.directory / name).exists())

    def test_initial_memory_cap_and_cap_after_update_do_not_allocate_last_checkpoint(self):
        real_step = torch.optim.AdamW.step
        for name, initial in (("initial-cap", True), ("update-cap", False)):
            updates = []

            def step(optimizer, *args, **kwargs):
                result = real_step(optimizer, *args, **kwargs)
                updates.append(1)
                return result

            def memory(device):
                return {"process_peak_rss_bytes": train.MAX_MEMORY if initial or updates else 1,
                        "gpu_driver_allocated_bytes": 0}

            with mock.patch.object(language_train, "memory", side_effect=memory), \
                    mock.patch.object(torch.optim.AdamW, "step", step), \
                    mock.patch.object(train, "save_checkpoint", wraps=save_checkpoint) as saver:
                report = self.fit(name)
            self.assertEqual(report["stop_reason"], "memory_limit")
            self.assertEqual(report["attempted_steps"], 0 if initial else 1)
            self.assertEqual(saver.call_count, 0 if initial else 1)
            self.assertFalse((self.directory / name / "last.npz").exists())
            self.assertIsNone(report["last_checkpoint_sha256"])
            self.assertFalse(report["last_state_validation_available"])
            self.assertEqual(self.report(name), report)

    def test_continuation_rejects_boolean_schedule_counter_before_creating_output(self):
        self.fit("first")
        model, optimizer, metadata = load_checkpoint(self.directory / "first" / "last.npz")
        metadata["instruction_updates_at_save"] = True  # Numerically equals one, but is not a counter.
        path = self.directory / "bad-counter.npz"
        save_checkpoint(path, model, optimizer, metadata)
        with self.assertRaisesRegex(ValueError, "instruction update counter"):
            self.fit("invalid", initialize_from=path, expected_parent_sha256=hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertFalse((self.directory / "invalid").exists())

    def test_cap_crossed_during_terminal_checkpoint_is_reported(self):
        terminal_saved = []

        def save(path, *args, **kwargs):
            save_checkpoint(path, *args, **kwargs)
            if path.name == "last.npz":
                terminal_saved.append(1)

        def memory(device):
            return {"process_peak_rss_bytes": train.MAX_MEMORY if terminal_saved else 1,
                    "gpu_driver_allocated_bytes": 0}

        with mock.patch.object(train, "save_checkpoint", side_effect=save), \
                mock.patch.object(language_train, "memory", side_effect=memory):
            report = self.fit()
        self.assertEqual(report["stop_reason"], "memory_limit")
        self.assertTrue(report["selected_checkpoint_available"])
        self.assertTrue((self.directory / "run" / "last.npz").exists())

    def test_wall_limit_after_update_stops_before_more_evaluation_or_checkpoint_copies(self):
        clock, real_step = [1000.0], torch.optim.AdamW.step

        def step(optimizer, *args, **kwargs):
            result = real_step(optimizer, *args, **kwargs)
            clock[0] += 60.0
            return result

        with mock.patch.object(train.time, "monotonic", side_effect=lambda: clock[0]), \
                mock.patch.object(torch.optim.AdamW, "step", step), \
                mock.patch.object(train, "score_validation", wraps=train.score_validation) as scorer:
            report = self.fit()
        self.assertEqual(report["stop_reason"], "wall_limit")
        self.assertEqual(report["attempted_steps"], 1)
        self.assertEqual(scorer.call_count, 1)
        self.assertEqual(report["elapsed_seconds"], 60.0)
        self.assertEqual(report["selected_step"], 0)
        self.assertFalse((self.directory / "run" / "last.npz").exists())

    def test_nonfinite_gradient_and_final_save_errors_preserve_finite_selected_report(self):
        real_loss = TorchTransformer.loss
        for name in ("loss", "gradient", "last-save", "final-device"):
            original_selected = []

            def loss(model, *args, **kwargs):
                value = real_loss(model, *args, **kwargs)
                if name == "loss" and torch.is_grad_enabled():
                    return value * torch.tensor(float("nan"))
                if name == "gradient" and torch.is_grad_enabled():
                    handle = next(iter(model.params.values())).register_hook(lambda value: torch.full_like(value, float("nan")))
                    self.addCleanup(handle.remove)
                return value

            def save(path, *args, **kwargs):
                if name == "last-save" and path.name == "last.npz":
                    raise OSError("injected terminal save failure")
                save_checkpoint(path, *args, **kwargs)
                if path.name == "model.npz":
                    original_selected.append(path.read_bytes())

            def sync(device):
                if name == "final-device" and (self.directory / name / "last.npz").exists():
                    raise RuntimeError("injected terminal device failure")

            with self.subTest(failure=name), mock.patch.object(TorchTransformer, "loss", loss), \
                    mock.patch.object(train, "save_checkpoint", side_effect=save), \
                    mock.patch.object(language_train, "synchronize", side_effect=sync), \
                    self.assertRaises((OSError, ValueError, RuntimeError)):
                self.fit(name)
            report = self.report(name)
            self.assertEqual(report["stop_reason"], "failure")
            self.assertEqual(report["attempted_steps"], 0 if name in ("loss", "gradient") else 3)
            self.assertEqual((self.directory / name / "model.npz").read_bytes(), original_selected[-1])
            self.assertIsNone(report["last_checkpoint_sha256"])
            self.assertFalse(report["last_state_validation_available"])
            self.assertEqual(self.parent.read_bytes(), self.parent_bytes)


if __name__ == "__main__":
    unittest.main()
