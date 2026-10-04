# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Persistence safety and same-runtime exact optimizer-resume checks."""

import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import warnings
import zipfile

import numpy as np
import torch

from contrib.b3ai.language import checkpoint
from contrib.b3ai.language.model import GpuConfig, TorchTransformer


class GpuCheckpointTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "model.npz"
        self.model = TorchTransformer(GpuConfig(vocab_size=257, context_length=8, width=8,
                                                layers=2, heads=2, ff_multiplier=2, seed=42))
        self.optimizer = torch.optim.AdamW(list(self.model.params.values()), lr=0.0003,
                                          betas=(0.8, 0.95), eps=1e-7, weight_decay=0.02,
                                          foreach=False, fused=False)
        self.tokens = torch.tensor([[1, 12, 200, 256], [9, 2, 4, 6]], dtype=torch.int64)
        self.targets = torch.tensor([[12, 200, 256, 5], [2, 4, 6, 8]], dtype=torch.int64)
        self.mask = torch.tensor([[1, 0.5, 0.25, 1], [1, 1, 0, 1]], dtype=torch.float32)
        self.metadata = {"run_id": "test-run", "step": 1, "rng_state": np.random.default_rng(12).bit_generator.state,
                         "corpus_sha256": "a" * 64, "quality": "unverified"}

    def update(self, model=None, optimizer=None):
        model = self.model if model is None else model
        optimizer = self.optimizer if optimizer is None else optimizer
        optimizer.zero_grad(set_to_none=True)
        loss = model.loss(self.tokens, self.targets, self.mask)
        loss.backward()
        optimizer.step()
        return loss.detach().item()

    def save(self):
        checkpoint.save_checkpoint(self.path, self.model, self.optimizer, self.metadata)

    def bundle(self):
        with np.load(self.path, allow_pickle=False) as archive:
            return {key: archive[key].copy() for key in archive.files}

    def mutate_manifest(self, mutation):
        bundle = self.bundle()
        manifest = json.loads(bundle["manifest"].tobytes())
        mutation(manifest)
        bundle["manifest"] = np.frombuffer(json.dumps(manifest).encode(), dtype=np.uint8)
        np.savez(self.path, **bundle)

    def test_full_roundtrip_preserves_parameters_settings_states_and_rng(self):
        self.update()
        self.save()
        model, optimizer, metadata = checkpoint.load_checkpoint(self.path)
        self.assertEqual(metadata, self.metadata)
        for name, parameter in self.model.params.items():
            torch.testing.assert_close(model.params[name], parameter, rtol=0, atol=0)
            before, after = self.optimizer.state[parameter], optimizer.state[model.params[name]]
            self.assertEqual(set(before), set(after))
            for key in before:
                torch.testing.assert_close(before[key], after[key], rtol=0, atol=0)
        for key in ("lr", "betas", "eps", "weight_decay", "foreach", "fused"):
            self.assertEqual(self.optimizer.param_groups[0][key], optimizer.param_groups[0][key])
        rng = np.random.default_rng()
        rng.bit_generator.state = metadata["rng_state"]
        expected = np.random.default_rng(12)
        np.testing.assert_array_equal(rng.integers(0, 1000, 16), expected.integers(0, 1000, 16))

    def test_cpu_resume_update_is_bitwise_exact_with_current_runtime(self):
        self.update()
        self.save()
        resumed_model, resumed_optimizer, _ = checkpoint.load_checkpoint(self.path)
        self.assertEqual(self.update(), self.update(resumed_model, resumed_optimizer))
        for name, parameter in self.model.params.items():
            torch.testing.assert_close(parameter, resumed_model.params[name], rtol=0, atol=0)
            for key in ("step", "exp_avg", "exp_avg_sq"):
                torch.testing.assert_close(self.optimizer.state[parameter][key],
                                           resumed_optimizer.state[resumed_model.params[name]][key], rtol=0, atol=0)

    def test_uninitialized_and_partially_initialized_optimizer_states_are_preserved(self):
        self.save()
        _, optimizer, _ = checkpoint.load_checkpoint(self.path)
        self.assertEqual(len(optimizer.state), 0)
        self.optimizer.zero_grad(set_to_none=True)
        first = next(iter(self.model.params.values()))
        first.grad = torch.ones_like(first)
        self.optimizer.step()
        self.save()
        model, optimizer, _ = checkpoint.load_checkpoint(self.path)
        self.assertEqual(len(optimizer.state), 1)
        name = next(iter(self.model.params))
        self.assertIn(model.params[name], optimizer.state)

    def test_compatible_atomic_replacement_and_unknown_file_protection(self):
        self.save()
        self.update()
        self.save()
        model, _, _ = checkpoint.load_checkpoint(self.path)
        for name in model.params:
            torch.testing.assert_close(model.params[name], self.model.params[name], rtol=0, atol=0)
        self.path.write_bytes(b"precious existing data")
        with self.assertRaises(ValueError):
            self.save()
        self.assertEqual(self.path.read_bytes(), b"precious existing data")

    def test_other_run_and_incompatible_config_are_not_overwritten(self):
        self.save()
        original = self.path.read_bytes()
        other_metadata = dict(self.metadata, run_id="different-run")
        with self.assertRaises(ValueError):
            checkpoint.save_checkpoint(self.path, self.model, self.optimizer, other_metadata)
        other_model = TorchTransformer(GpuConfig(vocab_size=257, context_length=9, width=8, layers=2, heads=2))
        other_optimizer = torch.optim.AdamW(list(other_model.params.values()), foreach=False, fused=False)
        with self.assertRaises(ValueError):
            checkpoint.save_checkpoint(self.path, other_model, other_optimizer, self.metadata)
        self.assertEqual(self.path.read_bytes(), original)

    def test_symlink_destination_is_not_overwritten(self):
        target = self.path.with_name("target")
        target.write_bytes(b"keep")
        self.path.symlink_to(target)
        with self.assertRaises(ValueError):
            self.save()
        self.assertEqual(target.read_bytes(), b"keep")

    def test_eod_format_and_config_bounds_are_enforced(self):
        for field, invalid in (("format", "b3-scratch-transformer"), ("version", 2),
                               ("tokenizer_version", "utf8-bytes-v1"), ("model_dtype", "float64")):
            with self.subTest(field=field):
                self.path.unlink(missing_ok=True)
                self.save()
                self.mutate_manifest(lambda manifest: manifest.update({field: invalid}))
                with self.assertRaises(ValueError):
                    checkpoint.load_checkpoint(self.path)
        for field, invalid in (("vocab_size", 256), ("context_length", 10**12),
                               ("width", True), ("heads", 3), ("seed", -1)):
            with self.subTest(config=field):
                self.path.unlink(missing_ok=True)
                self.save()
                self.mutate_manifest(lambda manifest: manifest["model_config"].update({field: invalid}))
                with self.assertRaises(ValueError):
                    checkpoint.load_checkpoint(self.path)

    def test_all_arrays_are_validated_before_device_model_construction(self):
        self.save()
        bundle = self.bundle()
        key = sorted(key for key in bundle if key.startswith("v__"))[-1]
        bundle[key].flat[0] = -1
        np.savez(self.path, **bundle)
        with mock.patch("contrib.b3ai.language.model.TorchTransformer") as constructor:
            with self.assertRaises(ValueError):
                checkpoint.load_checkpoint(self.path, device="mps")
            constructor.assert_not_called()

    def test_nonfinite_negative_wrong_shape_and_object_arrays_are_rejected(self):
        self.save()
        original = self.bundle()
        for kind in ("nonfinite", "negative_variance", "shape", "dtype", "object"):
            with self.subTest(kind=kind):
                bundle = {key: value.copy() for key, value in original.items()}
                key = "v__head.bias" if kind == "negative_variance" else "p__head.bias"
                if kind == "nonfinite":
                    bundle[key].flat[0] = np.inf
                elif kind == "negative_variance":
                    bundle[key].flat[0] = -0.001
                elif kind == "shape":
                    bundle[key] = bundle[key][:-1]
                elif kind == "dtype":
                    bundle[key] = bundle[key].astype(np.float64)
                else:
                    bundle[key] = np.array([{"pickle": "must never run"}], dtype=object)
                np.savez(self.path, **bundle)
                with self.assertRaises(ValueError):
                    checkpoint.load_checkpoint(self.path)

    def test_missing_extra_duplicate_and_truncated_members_are_rejected(self):
        self.save()
        original = self.path.read_bytes()
        for kind in ("missing", "extra", "duplicate", "truncated"):
            with self.subTest(kind=kind):
                self.path.write_bytes(original)
                if kind == "truncated":
                    self.path.write_bytes(original[:-20])
                else:
                    with zipfile.ZipFile(io.BytesIO(original)) as source:
                        members = [(name, source.read(name)) for name in source.namelist()]
                    with zipfile.ZipFile(self.path, "w") as output:
                        for name, value in members[1:] if kind == "missing" else members:
                            output.writestr(name, value)
                        if kind == "extra":
                            output.writestr("unexpected.npy", b"junk")
                        elif kind == "duplicate":
                            with warnings.catch_warnings():
                                warnings.simplefilter("ignore", UserWarning)
                                output.writestr(members[0][0], members[0][1])
                with self.assertRaises(ValueError):
                    checkpoint.load_checkpoint(self.path)

    def test_npy_header_bomb_is_rejected_without_body_allocation(self):
        self.save()
        original = self.path.read_bytes()
        header = io.BytesIO()
        np.lib.format.write_array_header_1_0(header, {"descr": "<f4", "fortran_order": False,
                                                    "shape": (2**60,)})
        with zipfile.ZipFile(io.BytesIO(original)) as source, zipfile.ZipFile(self.path, "w") as output:
            for name in source.namelist():
                output.writestr(name, header.getvalue() if name == "p__head.bias.npy" else source.read(name))
        with self.assertRaises(ValueError):
            checkpoint.load_checkpoint(self.path)

    def test_duplicate_json_keys_bad_steps_and_unknown_optimizer_states_are_rejected(self):
        self.save()
        original = self.path.read_bytes()
        bundle = self.bundle()
        raw = bundle["manifest"].tobytes().decode()
        bundle["manifest"] = np.frombuffer(raw.replace('"version":1', '"version":1,"version":1').encode(), dtype=np.uint8)
        np.savez(self.path, **bundle)
        with self.assertRaises(ValueError):
            checkpoint.load_checkpoint(self.path)
        for mutation in (
            lambda manifest: manifest["optimizer"]["steps"].update({"head.bias": -1}),
            lambda manifest: manifest["optimizer"]["steps"].update({"head.bias": 0.5}),
            lambda manifest: manifest["optimizer"].update({"betas": [0.9, 1]}),
            lambda manifest: manifest["optimizer"]["initialized"].append("not.a.parameter"),
            lambda manifest: manifest["optimizer"].update({"lr": float("nan")}),
        ):
            self.path.write_bytes(original)
            self.mutate_manifest(mutation)
            with self.assertRaises(ValueError):
                checkpoint.load_checkpoint(self.path)

    def test_metadata_structure_and_archive_limits(self):
        for invalid in ({"bad": np.int64(1)}, {"bad": float("nan")}, {"bad": "a" * 65_537},
                        {"bad": [None] * 8193}):
            with self.subTest(type=type(invalid["bad"]).__name__):
                with self.assertRaises(ValueError):
                    checkpoint.save_checkpoint(self.path, self.model, self.optimizer, invalid)
        self.save()
        with mock.patch.object(checkpoint, "MAX_ARCHIVE_BYTES", 64):
            with self.assertRaises(ValueError):
                checkpoint.load_checkpoint(self.path)
        with mock.patch.object(checkpoint, "MAX_MANIFEST_BYTES", 64):
            with self.assertRaises(ValueError):
                checkpoint.load_checkpoint(self.path)

    def test_unsupported_optimizer_options_and_invalid_numeric_state_cannot_be_saved(self):
        self.optimizer.param_groups[0]["foreach"] = True
        with self.assertRaises(ValueError):
            self.save()
        self.optimizer.param_groups[0]["foreach"] = False
        self.update()
        first = next(iter(self.model.params.values()))
        with torch.no_grad():
            self.optimizer.state[first]["exp_avg_sq"].view(-1)[0] = -1
        with self.assertRaises(ValueError):
            self.save()


if __name__ == "__main__":
    unittest.main()
