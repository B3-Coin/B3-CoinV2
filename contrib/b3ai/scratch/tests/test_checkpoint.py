# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.

import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import numpy as np

from contrib.b3ai.scratch.checkpoint import load_checkpoint, save_checkpoint
from contrib.b3ai.scratch.model import AdamW, ModelConfig, Transformer


class CheckpointTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "student.npz"
        self.config = ModelConfig(context_length=8, width=8, layers=1, heads=2, ff_multiplier=2, seed=42)

    def pair(self, dtype=np.float32):
        model = Transformer(self.config, dtype=dtype)
        return model, AdamW(model.params)

    def mutate(self, transform):
        with np.load(self.path, allow_pickle=False) as archive:
            arrays = {key: archive[key].copy() for key in archive.files}
        transform(arrays)
        with self.path.open("wb") as stream:
            np.savez(stream, **arrays)

    def manifest_change(self, change):
        def mutate(arrays):
            manifest = json.loads(arrays["manifest"].tobytes().decode("utf-8"))
            change(manifest)
            arrays["manifest"] = np.frombuffer(json.dumps(manifest).encode("utf-8"), dtype=np.uint8)
        self.mutate(mutate)

    def test_roundtrip_logits_and_exact_next_optimizer_step(self):
        for dtype in (np.float32, np.float64):
            with self.subTest(dtype=dtype):
                self.path.unlink(missing_ok=True)
                model, optimizer = self.pair(dtype)
                rng = np.random.default_rng(77)
                grads = {name: rng.normal(size=array.shape).astype(dtype) for name, array in model.params.items()}
                optimizer.step(model.params, grads, lr=0.001)
                tokens = np.array([[66, 51, 32, 72]], dtype=np.int64)
                original_logits = model.forward(tokens)
                metadata = {"stage": "sanity-pretraining", "readiness": "experimental", "completed_steps": optimizer.steps,
                            "rng_state": rng.bit_generator.state, "source_revision": "test-fixture"}
                save_checkpoint(self.path, model, optimizer, metadata)
                restored, resumed, saved = load_checkpoint(self.path)
                self.assertEqual(saved, metadata)
                np.testing.assert_array_equal(restored.forward(tokens), original_logits)
                self.assertEqual(resumed.steps, optimizer.steps)
                for name in model.params:
                    np.testing.assert_array_equal(resumed.m[name], optimizer.m[name])
                    np.testing.assert_array_equal(resumed.v[name], optimizer.v[name])
                optimizer.step(model.params, grads, lr=0.001)
                resumed.step(restored.params, grads, lr=0.001)
                for name in model.params:
                    np.testing.assert_array_equal(restored.params[name], model.params[name])
                    np.testing.assert_array_equal(resumed.m[name], optimizer.m[name])
                    np.testing.assert_array_equal(resumed.v[name], optimizer.v[name])
                save_checkpoint(self.path, restored, resumed, saved)
                with np.load(self.path, allow_pickle=False) as archive:
                    manifest = json.loads(archive["manifest"].tobytes().decode("utf-8"))
                    self.assertEqual(manifest["initialization"], "random")
                    self.assertIs(manifest["pretrained_weights"], False)

    def test_corrupt_incompatible_and_unexpected_arrays_are_rejected(self):
        transforms = [
            lambda arrays: arrays.pop(next(key for key in arrays if key.startswith("m__"))),
            lambda arrays: arrays.__setitem__("unexpected", np.zeros(1)),
            lambda arrays: arrays.__setitem__("p__token_embedding", np.zeros((1, 1), dtype=np.float32)),
            lambda arrays: arrays.__setitem__("p__token_embedding", arrays["p__token_embedding"].astype(np.float64)),
            lambda arrays: arrays["p__token_embedding"].__setitem__((0, 0), np.nan),
            lambda arrays: arrays["v__token_embedding"].__setitem__((0, 0), -1),
            lambda arrays: arrays.__setitem__("manifest", np.array([{}], dtype=object)),
        ]
        for transform in transforms:
            with self.subTest(transform=transform):
                self.path.unlink(missing_ok=True)
                model, optimizer = self.pair()
                save_checkpoint(self.path, model, optimizer, {})
                self.mutate(transform)
                with self.assertRaises(ValueError):
                    load_checkpoint(self.path)
        self.path.write_bytes(b"not an npz file")
        with self.assertRaises(ValueError):
            load_checkpoint(self.path)

    def test_manifest_config_provenance_and_metadata_bounds(self):
        changes = [
            lambda item: item.__setitem__("version", 2),
            lambda item: item.__setitem__("pretrained_weights", True),
            lambda item: item.__setitem__("tokenizer_version", "unknown"),
            lambda item: item["model_config"].__setitem__("vocab_size", 128),
            lambda item: item["model_config"].__setitem__("width", 1_000_000),
            lambda item: item["model_config"].__setitem__("seed", True),
            lambda item: item["optimizer"].__setitem__("steps", -1),
            lambda item: item["optimizer"].__setitem__("beta1", 1),
            lambda item: item["metadata"].__setitem__("prompt", "x" * 65_537),
            lambda item: item["metadata"].__setitem__("value", float("nan")),
        ]
        for change in changes:
            with self.subTest(change=change):
                self.path.unlink(missing_ok=True)
                model, optimizer = self.pair()
                save_checkpoint(self.path, model, optimizer, {})
                self.manifest_change(change)
                with self.assertRaises(ValueError):
                    load_checkpoint(self.path)
        model, optimizer = self.pair()
        for metadata in ({"bad": np.int64(1)}, {"bad": float("inf")}, {"prompt": "x" * 65_537}):
            with self.assertRaises(ValueError):
                save_checkpoint(self.path, model, optimizer, metadata)

    def test_duplicate_zip_and_json_members_are_rejected(self):
        model, optimizer = self.pair()
        save_checkpoint(self.path, model, optimizer, {})
        with zipfile.ZipFile(self.path, "a") as archive:
            import warnings
            with warnings.catch_warnings():
                warnings.simplefilter("ignore", UserWarning)
                archive.writestr("manifest.npy", b"duplicate")
        with self.assertRaises(ValueError):
            load_checkpoint(self.path)
        self.path.unlink()
        save_checkpoint(self.path, model, optimizer, {})
        def duplicate(arrays):
            text = arrays["manifest"].tobytes().decode("utf-8")
            text = text[:-1] + ',"version":1}'
            arrays["manifest"] = np.frombuffer(text.encode("utf-8"), dtype=np.uint8)
        self.mutate(duplicate)
        with self.assertRaises(ValueError):
            load_checkpoint(self.path)

    def test_atomic_failure_preserves_old_file_and_removes_temporary(self):
        model, optimizer = self.pair()
        save_checkpoint(self.path, model, optimizer, {"completed_steps": 0})
        before = self.path.read_bytes()
        with patch("contrib.b3ai.scratch.checkpoint.os.replace", side_effect=OSError("fixture write failure")):
            with self.assertRaises(OSError):
                save_checkpoint(self.path, model, optimizer, {"completed_steps": 1})
        self.assertEqual(self.path.read_bytes(), before)
        self.assertEqual(list(self.path.parent.glob("*.tmp")), [])
        self.path.write_bytes(b"unknown file must be preserved")
        with self.assertRaises(ValueError):
            save_checkpoint(self.path, model, optimizer, {})
        self.assertEqual(self.path.read_bytes(), b"unknown file must be preserved")

    def test_forged_huge_npy_header_is_rejected_before_allocation(self):
        model, optimizer = self.pair()
        save_checkpoint(self.path, model, optimizer, {})
        with zipfile.ZipFile(self.path, "r") as archive:
            members = {name: archive.read(name) for name in archive.namelist()}
        forged = io.BytesIO()
        np.lib.format.write_array_header_1_0(forged, {"descr": "<f4", "fortran_order": False, "shape": (2**60, 256)})
        members["p__token_embedding.npy"] = forged.getvalue()
        with zipfile.ZipFile(self.path, "w") as archive:
            for name, data in members.items():
                archive.writestr(name, data)
        with self.assertRaises(ValueError):
            load_checkpoint(self.path)

    def test_deep_untrusted_json_and_unsupported_compression_are_rejected(self):
        model, optimizer = self.pair()
        save_checkpoint(self.path, model, optimizer, {})
        self.mutate(lambda arrays: arrays.__setitem__("manifest", np.frombuffer(
            b"[" * 2000 + b"0" + b"]" * 2000, dtype=np.uint8)))
        with self.assertRaises(ValueError):
            load_checkpoint(self.path)
        self.path.unlink()
        save_checkpoint(self.path, model, optimizer, {})
        with zipfile.ZipFile(self.path, "r") as archive:
            members = {name: archive.read(name) for name in archive.namelist()}
        with zipfile.ZipFile(self.path, "w", compression=zipfile.ZIP_BZIP2) as archive:
            for name, data in members.items():
                archive.writestr(name, data)
        with self.assertRaises(ValueError):
            load_checkpoint(self.path)


if __name__ == "__main__":
    unittest.main()
