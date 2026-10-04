# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Synthetic CPU models check honest continuation recording, not language skill."""

import contextlib
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest

import torch

from contrib.b3ai.language import evaluate
from contrib.b3ai.language.checkpoint import save_checkpoint
from contrib.b3ai.language.model import GpuConfig, TorchTransformer


class LanguageEvaluateTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.model = TorchTransformer(GpuConfig(vocab_size=257, context_length=512,
            width=8, layers=1, heads=2, ff_multiplier=2, seed=42))
        self.model.eval()
        self.exam_path = Path(evaluate.__file__).with_name("language_exam.json")

    def force_next_token(self, token):
        # Controlled output exists only inside this numerical fixture; the
        # production evaluator must record what the actual model generated.
        with torch.no_grad():
            self.model.params["head.weight"].zero_()
            self.model.params["head.bias"].fill_(-100)
            self.model.params["head.bias"][token] = 100

    def save(self, name="model.npz", stage="language-pilot"):
        optimizer = torch.optim.AdamW(list(self.model.params.values()), lr=0.0003,
            weight_decay=0.01, foreach=False, fused=False)
        path = self.directory / name
        save_checkpoint(path, self.model, optimizer,
            {"stage": stage, "run_id": "synthetic-evaluation-test", "fixture": True})
        return path

    def record_exam(self, checkpoint, exam=None, output=None):
        with contextlib.redirect_stdout(io.StringIO()):
            return evaluate.run(checkpoint, exam or self.exam_path,
                output or self.directory / "report.json", device="cpu")

    def test_eod_is_retained_as_an_actual_token_and_stops_generation(self):
        self.force_next_token(256)
        result = evaluate.continuation(self.model, "A story begins.", maximum_new_tokens=8)
        self.assertEqual(result["token_ids"], [256])
        self.assertEqual(result["text"], "")
        self.assertTrue(result["valid_utf8"])
        self.assertEqual(result["finish_reason"], "end_of_document")
        self.assertEqual(result["maximum_new_tokens"], 8)
        # Empty language output stays empty; there is no helpful target answer
        # inserted into the recorded model response.
        self.assertEqual(evaluate.continuation(self.model, "Another beginning.",
            temperature=0.8, seed=2, maximum_new_tokens=8)["token_ids"], [256])

    def test_length_stop_preserves_actual_tokens_and_the_complete_generated_text(self):
        self.force_next_token(ord("x"))
        result = evaluate.continuation(self.model, "Once,", maximum_new_tokens=7)
        self.assertEqual(result["token_ids"], [ord("x")] * 7)
        self.assertEqual(result["text"], "xxxxxxx")
        self.assertEqual(result["finish_reason"], "length")
        self.assertTrue(result["valid_utf8"])

    def test_invalid_utf8_retains_raw_byte_ids_and_marks_replacement_text(self):
        self.force_next_token(255)
        result = evaluate.continuation(self.model, "The bird", maximum_new_tokens=3)
        self.assertEqual(result["token_ids"], [255, 255, 255])
        self.assertEqual(result["text"], "\ufffd\ufffd\ufffd")
        self.assertFalse(result["valid_utf8"])
        self.assertEqual(result["finish_reason"], "length")

    def test_temperature_sampling_uses_its_local_seed_and_preserves_settings(self):
        with torch.no_grad():
            self.model.params["head.weight"].zero_()
            self.model.params["head.bias"].fill_(-100)
            self.model.params["head.bias"][ord("a")] = 0.1
            self.model.params["head.bias"][ord("b")] = 0.2
        first = evaluate.continuation(self.model, "A", temperature=0.8, seed=91,
            maximum_new_tokens=16)
        self.addCleanup(torch.set_rng_state, torch.get_rng_state())
        torch.manual_seed(112233)
        torch.rand(37)
        second = evaluate.continuation(self.model, "A", temperature=0.8, seed=91,
            maximum_new_tokens=16)
        self.assertEqual(first, second)
        self.assertEqual(first["temperature"], 0.8)
        self.assertEqual(first["seed"], 91)
        self.assertEqual(len(first["token_ids"]), 16)
        self.assertEqual(first["text"].encode("utf-8"), bytes(first["token_ids"]))
        self.assertTrue(set(first["token_ids"]) <= {ord("a"), ord("b")})

    def test_context_budget_counts_utf8_bytes_and_accepts_exact_boundary(self):
        self.force_next_token(256)
        result = evaluate.continuation(self.model, "é" * 128, maximum_new_tokens=256)
        self.assertEqual(result["token_ids"], [256])
        with self.assertRaises(ValueError):
            evaluate.continuation(self.model, "é" * 129, maximum_new_tokens=256)

    def test_invalid_prompt_and_generation_settings_are_rejected(self):
        self.force_next_token(256)
        for prompt in (None, "", "a\0b", "\ud800", "x" * 65537):
            with self.subTest(prompt_type=type(prompt).__name__, length=len(prompt) if isinstance(prompt, str) else None):
                with self.assertRaises(ValueError):
                    evaluate.continuation(self.model, prompt, maximum_new_tokens=1)
        cases = {"temperature": (-0.1, 2.1, True, "0.8", float("nan"), float("inf")),
            "seed": (-1, 2**63, True, 1.0), "maximum_new_tokens": (0, 257, True, 1.0)}
        for name, values in cases.items():
            for value in values:
                with self.subTest(name=name, value=value):
                    with self.assertRaises(ValueError):
                        evaluate.continuation(self.model, "A", **{name: value})

    def test_nonfinite_logits_fail_instead_of_sampling_or_replacing_the_reply(self):
        self.force_next_token(ord("a"))
        for value in (float("nan"), float("inf"), -float("inf")):
            for temperature in (0, 0.8):
                with self.subTest(value=value, temperature=temperature):
                    with torch.no_grad():
                        self.model.params["head.bias"][ord("a")] = value
                    with self.assertRaises(ValueError):
                        evaluate.continuation(self.model, "A", temperature=temperature,
                            maximum_new_tokens=1)

    def test_exam_report_records_all_forty_outputs_and_exact_artifact_hashes(self):
        self.force_next_token(256)
        checkpoint = self.save()
        before = checkpoint.read_bytes()
        summary = self.record_exam(checkpoint)
        report = json.loads((self.directory / "report.json").read_text(encoding="utf-8"))
        exam = json.loads(self.exam_path.read_bytes())
        self.assertEqual(summary["continuations"], 40)
        self.assertEqual(summary["teacher_review"], "pending")
        self.assertEqual(len(report["outputs"]), 40)
        self.assertEqual(report["checkpoint_sha256"], hashlib.sha256(before).hexdigest())
        self.assertEqual(report["exam_sha256"], hashlib.sha256(self.exam_path.read_bytes()).hexdigest())
        self.assertEqual(report["model_config"], self.model.config.to_dict())
        self.assertEqual(report["review"], exam["review"])
        self.assertEqual(report["runtime"]["device"], "cpu")
        self.assertEqual(report["checkpoint_metadata"]["stage"], "language-pilot")
        for prompt, greedy, sampled in zip(exam["prompts"], report["outputs"][::2], report["outputs"][1::2]):
            for output, temperature in ((greedy, 0.0), (sampled, 0.8)):
                self.assertEqual(output["id"], prompt["id"])
                self.assertEqual(output["prompt"], prompt["text"])
                self.assertEqual(output["temperature"], temperature)
                self.assertEqual(output["token_ids"], [256])
                self.assertEqual(output["text"], "")
                self.assertEqual(output["finish_reason"], "end_of_document")
                self.assertTrue(output["valid_utf8"])
        self.assertEqual(checkpoint.read_bytes(), before)

    def test_existing_report_cannot_be_overwritten(self):
        self.force_next_token(256)
        checkpoint = self.save()
        output = self.directory / "report.json"
        output.write_bytes(b"An existing teacher report must be preserved.")
        before = output.read_bytes()
        with self.assertRaises(FileExistsError):
            self.record_exam(checkpoint, output=output)
        self.assertEqual(output.read_bytes(), before)

    def test_non_language_checkpoint_stage_is_rejected_without_creating_a_report(self):
        self.force_next_token(256)
        checkpoint = self.save(stage="sanity-dialogue")
        with self.assertRaises(ValueError):
            self.record_exam(checkpoint)
        self.assertFalse((self.directory / "report.json").exists())

    def test_invalid_exam_shape_generation_and_duplicate_ids_are_rejected(self):
        self.force_next_token(256)
        checkpoint = self.save()
        original = json.loads(self.exam_path.read_bytes())
        mutations = [
            lambda e: e.update(schema="another-exam"),
            lambda e: e["prompts"].pop(),
            lambda e: e["generation"].update(maximum_new_byte_tokens=257),
            lambda e: e["prompts"][1].update(id=e["prompts"][0]["id"]),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(case=index):
                exam = json.loads(json.dumps(original))
                mutate(exam)
                path = self.directory / ("exam-" + str(index) + ".json")
                path.write_text(json.dumps(exam), encoding="utf-8")
                with self.assertRaises(ValueError):
                    self.record_exam(checkpoint, exam=path)
                self.assertFalse((self.directory / "report.json").exists())


if __name__ == "__main__":
    unittest.main()
