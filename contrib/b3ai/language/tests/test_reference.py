# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Golden fixtures check numerical state, safe publication, and prompt counts."""

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

from contrib.b3ai.language.reference import (
    FORMAT, MAX_REFERENCE_BYTES, build_reference, measure_hive_prompt, write_reference,
)
from contrib.b3ai.scratch.model import ModelConfig, Transformer
from contrib.b3ai.scratch.tokenizer import VERSION as TOKENIZER_VERSION
from contrib.b3ai.scratch.train import implementation_hash


def metadata(arrays):
    return json.loads(arrays["metadata"].tobytes().decode("utf-8"))


class ReferenceTests(unittest.TestCase):
    def test_fixture_reproducible_complete_and_bounded(self):
        first, second = build_reference(), build_reference()
        self.assertEqual(set(first), set(second))
        for key in first:
            np.testing.assert_array_equal(first[key], second[key], err_msg=key)
            self.assertFalse(first[key].dtype.hasobject, key)
            self.assertTrue(np.all(np.isfinite(first[key])), key)
        info = metadata(first)
        self.assertEqual(info["format"], FORMAT)
        self.assertEqual(info["source_implementation_hash"], implementation_hash())
        self.assertEqual(info["tokenizer_version"], TOKENIZER_VERSION)
        self.assertEqual(info["config"], ModelConfig(width=8, context_length=8,
                         layers=2, heads=2).to_dict())
        self.assertIn("randomly initialized", info["provenance"])
        self.assertEqual(info["dtype"], "float32")
        self.assertLess(sum(value.nbytes for value in first.values()), MAX_REFERENCE_BYTES)
        for prefix in ("initial", "gradient", "updated", "m", "v"):
            self.assertEqual({key[len(prefix) + 2:] for key in first
                              if key.startswith(prefix + "__")},
                             set(info["parameter_names"]))

    def test_stored_logits_causal_and_masked_loss_is_independently_correct(self):
        arrays = build_reference()
        tokens, targets, mask = (arrays[name] for name in ("tokens", "targets", "loss_mask"))
        self.assertEqual(tokens.shape, (2, 8))
        np.testing.assert_array_equal(tokens[:, 1:], targets[:, :-1])
        self.assertTrue(np.all(mask[:, :5] == 0))
        self.assertTrue(np.all(mask[:, -1] == 1))
        self.assertTrue(np.all(targets[:, -1] == ord("\n")))
        self.assertTrue(np.any((mask > 0) & (mask < 1)))
        prefix = int(arrays["causal_prefix_length"])
        np.testing.assert_array_equal(arrays["logits"][:, :prefix],
                                      arrays["causal_logits"][:, :prefix])
        self.assertGreater(np.max(np.abs(arrays["logits"][:, prefix:]
                                       - arrays["causal_logits"][:, prefix:])), 1e-3)
        logits = arrays["logits"].astype(np.float64)
        shifted = logits - logits.max(axis=-1, keepdims=True)
        log_normalizer = np.log(np.exp(shifted).sum(axis=-1))
        selected = np.take_along_axis(shifted, targets[..., None], axis=-1)[..., 0]
        expected = float(((log_normalizer - selected) * mask).sum() / mask.sum())
        self.assertAlmostEqual(float(arrays["loss"]), expected, places=5)

    def test_initial_parameters_are_not_updated_copies_and_adam_is_consistent(self):
        arrays = build_reference()
        info = metadata(arrays)
        fresh = Transformer(ModelConfig(**info["config"]), dtype=np.float32)
        settings = info["optimizer"]
        names = info["parameter_names"]
        norm = np.sqrt(sum(float(np.sum(arrays["gradient__" + name].astype(np.float64)**2))
                           for name in names))
        scale = min(1.0, settings["clip_norm"] / norm)
        self.assertGreater(norm, settings["clip_norm"])
        self.assertEqual(int(arrays["optimizer_steps"]), 1)
        changed = []
        for name in names:
            initial = arrays["initial__" + name]
            updated = arrays["updated__" + name]
            np.testing.assert_array_equal(initial, fresh.params[name], err_msg=name)
            self.assertFalse(np.shares_memory(initial, updated))
            gradient = arrays["gradient__" + name].astype(np.float64) * scale
            expected_m = (1 - settings["beta1"]) * gradient
            expected_v = (1 - settings["beta2"]) * gradient**2
            np.testing.assert_allclose(arrays["m__" + name], expected_m,
                                       rtol=3e-6, atol=1e-10, err_msg=name)
            np.testing.assert_allclose(arrays["v__" + name], expected_v,
                                       rtol=3e-6, atol=1e-12, err_msg=name)
            # At step one, bias correction reduces m_hat/v_hat to g/g**2.
            expected = initial.astype(np.float64) * (1 - settings["learning_rate"] * settings["weight_decay"])
            expected -= settings["learning_rate"] * gradient / (np.abs(gradient) + settings["eps"])
            np.testing.assert_allclose(updated, expected, rtol=3e-6, atol=2e-7, err_msg=name)
            changed.append(not np.array_equal(initial, updated))
        self.assertTrue(any(changed))
        np.testing.assert_array_equal(arrays["initial__head.bias"], np.zeros(256, dtype=np.float32))
        self.assertGreater(np.max(np.abs(arrays["updated__head.bias"])), 0)

    def test_archive_loads_without_pickle_and_never_overwrites(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "nested" / "golden.npz"
            report = write_reference(output)
            original = output.read_bytes()
            self.assertEqual(report["archive_bytes"], len(original))
            self.assertEqual(report["archive_sha256"], hashlib.sha256(original).hexdigest())
            with np.load(output, allow_pickle=False) as loaded:
                expected = build_reference()
                self.assertEqual(set(loaded.files), set(expected))
                for key in expected:
                    np.testing.assert_array_equal(loaded[key], expected[key], err_msg=key)
            with self.assertRaises(FileExistsError):
                write_reference(output)
            self.assertEqual(output.read_bytes(), original)
            self.assertEqual(list(output.parent.iterdir()), [output])

    def test_failed_or_racing_publication_cleans_temporary(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "golden.npz"
            with patch("contrib.b3ai.language.reference.os.link", side_effect=OSError("failure")):
                with self.assertRaises(OSError):
                    write_reference(output)
            self.assertEqual(list(Path(directory).iterdir()), [])
            original_link = __import__("os").link

            def racing_link(source, target):
                Path(target).write_bytes(b"other writer")
                original_link(source, target)

            with patch("contrib.b3ai.language.reference.os.link", side_effect=racing_link):
                with self.assertRaises(FileExistsError):
                    write_reference(output)
            self.assertEqual(output.read_bytes(), b"other writer")
            self.assertEqual(list(Path(directory).iterdir()), [output])

    def test_cli_creates_fixture_and_reports_existing_output(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "reference.npz"
            command = [sys.executable, "-m", "contrib.b3ai.language.reference",
                       "--output", str(output)]
            result = subprocess.run(command, capture_output=True, text=True, check=True)
            self.assertEqual(json.loads(result.stdout)["format"], FORMAT)
            original = output.read_bytes()
            repeated = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(repeated.returncode, 2)
            self.assertIn("already exists", repeated.stderr)
            self.assertEqual(output.read_bytes(), original)


class HivePromptTests(unittest.TestCase):
    BLOCK = '''QJsonArray B3AiClient::messages() const
{
    QString system = QStringLiteral("B3 \\u00e9. " "Second. ");
    system += m_tools_enabled ? QStringLiteral("enabled\\n") :
                               QStringLiteral("disabled\\n");
    system += QStringLiteral("BEGIN\\n") + m_lesson_context +
              QStringLiteral("\\nEND");
}'''

    def test_counts_decoded_utf8_and_empty_lesson_markers(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "client.cpp"
            path.write_text(self.BLOCK, encoding="utf-8")
            report = measure_hive_prompt(path)
            base = "B3 é. Second. ".encode("utf-8")
            disabled = base + b"disabled\nBEGIN\n\nEND"
            self.assertEqual(report["base_bytes"], len(base))
            self.assertEqual(report["tools_disabled_bytes"], len(disabled))
            self.assertEqual(report["source_sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertEqual(report["base_sha256"], hashlib.sha256(base).hexdigest())
            self.assertEqual(report["tools_disabled_sha256"], hashlib.sha256(disabled).hexdigest())

    def test_rejects_changed_expressions_unsupported_escapes_or_ambiguity(self):
        invalid = [self.BLOCK.replace('"Second. "', 'arbitrary_function()'),
                   self.BLOCK.replace("\\u00e9", "\\xE9"), self.BLOCK + self.BLOCK,
                   "QString system = external_prompt();"]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "client.cpp"
            for source in invalid:
                path.write_text(source, encoding="utf-8")
                with self.subTest(source=source), self.assertRaises(ValueError):
                    measure_hive_prompt(path)

    def test_actual_hive_prompt_exceeds_old_byte_context(self):
        root = Path(__file__).resolve().parents[4]
        path = root / "src" / "qt" / "b3aiclient.cpp"
        report = measure_hive_prompt(path)
        self.assertGreater(report["base_bytes"], 256)
        self.assertGreater(report["tools_disabled_bytes"], report["base_bytes"])


if __name__ == "__main__":
    unittest.main()
