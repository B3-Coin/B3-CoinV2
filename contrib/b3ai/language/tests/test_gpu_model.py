# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Check the accelerated model against the independent manual-gradient port."""

import json
from pathlib import Path
import tempfile
import unittest

import numpy as np
import torch

from contrib.b3ai.language.model import (
    GpuConfig, TorchTransformer, parameter_shapes,
)
from contrib.b3ai.language.reference import write_reference
from contrib.b3ai.scratch.model import ModelConfig, Transformer


PLAN = json.loads((Path(__file__).resolve().parents[1] / "pilot.json").read_text())


class _ParityChecks:
    DEVICE = "cpu"
    TOLERANCE_KEY = "cpu_float32_tolerance"

    @classmethod
    def setUpClass(cls):
        # Load a persisted pickle-free fixture, rather than deriving expected
        # values from any part of the PyTorch implementation under test.
        cls.directory = tempfile.TemporaryDirectory()
        path = Path(cls.directory.name) / "reference.npz"
        write_reference(path)
        with np.load(path, allow_pickle=False) as archive:
            cls.arrays = {name: archive[name].copy() for name in archive.files}
        cls.metadata = json.loads(cls.arrays["metadata"].tobytes().decode("utf-8"))
        cls.tolerance = PLAN["numerical_reference"][cls.TOLERANCE_KEY]

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def make_model(self):
        return TorchTransformer(GpuConfig(**self.metadata["config"])).to(self.DEVICE)

    def inputs(self):
        return tuple(torch.from_numpy(self.arrays[name]).to(self.DEVICE)
                     for name in ("tokens", "targets", "loss_mask"))

    def assert_close(self, actual, expected, description):
        if isinstance(actual, torch.Tensor):
            actual = actual.detach().cpu().numpy()
        np.testing.assert_allclose(actual, expected,
                                   atol=self.tolerance["absolute"],
                                   rtol=self.tolerance["relative"], err_msg=description)

    def test_logits_masked_loss_and_every_gradient(self):
        model = self.make_model()
        self.assertEqual(set(model.params), set(self.metadata["parameter_names"]))
        for name, parameter in model.params.items():
            np.testing.assert_array_equal(parameter.detach().cpu().numpy(),
                                          self.arrays["initial__" + name], err_msg=name)
        tokens, targets, mask = self.inputs()
        self.assert_close(model(tokens), self.arrays["logits"], "logits")
        loss = model.loss(tokens, targets, mask)
        self.assert_close(loss, self.arrays["loss"], "masked loss")
        loss.backward()
        for name, parameter in model.params.items():
            self.assertIsNotNone(parameter.grad, name)
            self.assert_close(parameter.grad, self.arrays["gradient__" + name], name)

    def test_one_clipped_adamw_update_and_all_moments_from_identical_gradients(self):
        model = self.make_model()
        settings = self.metadata["optimizer"]
        optimizer = torch.optim.AdamW(model.parameters(), lr=settings["learning_rate"],
                                     betas=(settings["beta1"], settings["beta2"]),
                                     eps=settings["eps"], weight_decay=settings["weight_decay"],
                                     foreach=False, fused=False)
        # Isolate optimizer equivalence from autograd equivalence, which is
        # checked above for every parameter. Attention key bias has an exactly
        # zero mathematical gradient; float32 reduction noise near 1e-9 is
        # amplified by Adam's 1e-8 epsilon at the first update. Using identical
        # fixture gradients makes this an optimizer comparison with the same
        # inputs rather than silently weakening the declared tolerances.
        for name, parameter in model.params.items():
            parameter.grad = torch.from_numpy(self.arrays["gradient__" + name]).to(self.DEVICE).clone()
        torch.nn.utils.clip_grad_norm_(model.parameters(), settings["clip_norm"],
                                       foreach=False, error_if_nonfinite=True)
        optimizer.step()
        for name, parameter in model.params.items():
            state = optimizer.state[parameter]
            self.assert_close(parameter, self.arrays["updated__" + name], "updated " + name)
            self.assert_close(state["exp_avg"], self.arrays["m__" + name], "moment m " + name)
            self.assert_close(state["exp_avg_sq"], self.arrays["v__" + name], "moment v " + name)
            self.assertEqual(state["step"].item(), int(self.arrays["optimizer_steps"]))

    def test_causal_prefix_invariance(self):
        model = self.make_model()
        prefix = int(self.arrays["causal_prefix_length"])
        tokens = torch.from_numpy(self.arrays["tokens"]).to(self.DEVICE)
        altered = torch.from_numpy(self.arrays["causal_tokens"]).to(self.DEVICE)
        original_logits = model(tokens)
        altered_logits = model(altered)
        self.assertTrue(torch.equal(original_logits[:, :prefix], altered_logits[:, :prefix]))
        self.assert_close(altered_logits, self.arrays["causal_logits"], "altered suffix")
        self.assertGreater((original_logits[:, prefix:] - altered_logits[:, prefix:]).abs().max().item(), 1e-3)

    def test_combined_update_preserves_logits_and_nonredundant_parameters(self):
        # A common key bias adds the same q dot bias to every unmasked score
        # in a row, and cancels in softmax. Independent float32 derivatives of
        # that zero can cause different first Adam updates on these redundant
        # entries. Require the declared tolerances on every other entry and
        # on the resulting function, without modifying either gradient.
        model = self.make_model()
        settings = self.metadata["optimizer"]
        optimizer = torch.optim.AdamW(model.parameters(), lr=settings["learning_rate"],
                                     betas=(settings["beta1"], settings["beta2"]),
                                     eps=settings["eps"], weight_decay=settings["weight_decay"],
                                     foreach=False, fused=False)
        tokens, targets, mask = self.inputs()
        model.loss(tokens, targets, mask).backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), settings["clip_norm"],
                                       foreach=False, error_if_nonfinite=True)
        optimizer.step()
        reference = Transformer(ModelConfig(**self.metadata["config"]))
        width = model.config.width
        for name, parameter in model.params.items():
            expected = self.arrays["updated__" + name]
            actual = parameter.detach().cpu().numpy()
            reference.params[name][...] = expected
            if name.endswith(".attention.qkv.bias"):
                indices = np.r_[0:width, 2 * width:3 * width]
                self.assert_close(actual[indices], expected[indices], "combined " + name)
            else:
                self.assert_close(actual, expected, "combined " + name)
        reference_logits = reference.forward(self.arrays["tokens"])
        reference_loss, _ = reference.loss_and_grad(
            self.arrays["tokens"], self.arrays["targets"], self.arrays["loss_mask"])
        self.assert_close(model(tokens), reference_logits, "combined updated logits")
        self.assert_close(model.loss(tokens, targets, mask), reference_loss, "combined updated loss")


class CpuParityTests(_ParityChecks, unittest.TestCase):
    pass


@unittest.skipUnless(torch.backends.mps.is_available(), "MPS device is unavailable")
class MpsParityTests(_ParityChecks, unittest.TestCase):
    DEVICE = "mps"
    TOLERANCE_KEY = "mps_float32_tolerance"


class ConfigurationTests(unittest.TestCase):
    def test_language_parameter_count_and_registered_parameters(self):
        config = GpuConfig()
        model = TorchTransformer(config)
        self.assertEqual(model.parameter_count, 5_395_201)
        self.assertEqual(sum(p.numel() for p in model.parameters()), model.parameter_count)
        self.assertEqual({name: tuple(p.shape) for name, p in model.params.items()},
                         parameter_shapes(config))
        self.assertEqual(config.to_dict(), {name: PLAN["proposed_language_model"][name]
                                          for name in config.to_dict()})
        self.assertTrue(all(p.dtype == torch.float32 for p in model.parameters()))

    def test_invalid_configuration_rejected_before_allocation(self):
        invalid = [{"vocab_size": 258}, {"vocab_size": True},
                   {"context_length": 4097}, {"context_length": 0},
                   {"width": 257}, {"width": 7, "heads": 2},
                   {"heads": 17}, {"layers": 9}, {"seed": -1},
                   {"ff_multiplier": 0}, {"layers": 8, "ff_multiplier": 8},
                   {"width": True}, {"seed": 1.0}]
        for kwargs in invalid:
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                GpuConfig(**kwargs)
        with self.assertRaises(TypeError):
            TorchTransformer({})
        with self.assertRaises(TypeError):
            parameter_shapes({})


class InputAndLoadingTests(unittest.TestCase):
    def setUp(self):
        self.model = TorchTransformer(GpuConfig(context_length=8, width=8,
                                                layers=2, heads=2))
        self.tokens = torch.tensor([[1, 2, 3, 256]], dtype=torch.long)
        self.targets = torch.tensor([[2, 3, 256, 4]], dtype=torch.long)

    def test_eod_and_fractional_masks_work_and_unmasked_positions_have_no_loss(self):
        mask = torch.tensor([[0.0, 0.25, 1.0, 0.0]])
        loss = self.model.loss(self.tokens, self.targets, mask)
        changed = self.targets.clone()
        changed[0, 0], changed[0, 3] = 17, 128
        self.assertEqual(loss.item(), self.model.loss(self.tokens, changed, mask).item())
        losses = torch.nn.functional.cross_entropy(self.model(self.tokens).reshape(-1, 257),
                                                   self.targets.reshape(-1), reduction="none")
        expected = (losses * mask.reshape(-1)).sum() / mask.sum()
        self.assertEqual(loss.item(), expected.item())
        self.assertEqual(self.model.loss(self.tokens, self.targets).item(), losses.mean().item())
        self.assertEqual(self.model.loss(self.tokens, self.targets, torch.ones_like(self.tokens)).item(),
                         losses.mean().item())

    def test_invalid_tokens_and_targets(self):
        invalid = [torch.tensor([1, 2]), torch.ones((1, 2)),
                   torch.empty((0, 2), dtype=torch.long), torch.empty((1, 0), dtype=torch.long),
                   torch.zeros((17, 1), dtype=torch.long), torch.zeros((1, 9), dtype=torch.long),
                   torch.tensor([[-1]]), torch.tensor([[257]]), [[1, 2]]]
        for value in invalid:
            with self.subTest(value=str(value)), self.assertRaises(ValueError):
                self.model(value)
        with self.assertRaises(ValueError):
            self.model.loss(self.tokens, self.targets[:, :-1])
        with self.assertRaises(ValueError):
            self.model.loss(self.tokens, torch.tensor([[258, 1, 2, 3]]))
        with self.assertRaises(ValueError):
            self.model.double()(self.tokens)

    def test_invalid_masks(self):
        invalid = [torch.zeros_like(self.tokens), torch.ones((1, 3)),
                   torch.full((1, 4), float("nan")), torch.full((1, 4), float("inf")),
                   torch.full((1, 4), -0.1), torch.full((1, 4), 1.1),
                   torch.ones((1, 4), dtype=torch.complex64), [[1, 1, 1, 1]]]
        for mask in invalid:
            with self.subTest(mask=str(mask)), self.assertRaises(ValueError):
                self.model.loss(self.tokens, self.targets, mask)

    def test_resource_caps_fail_before_attention_allocation(self):
        model = TorchTransformer(GpuConfig(context_length=4096, width=8, layers=6, heads=8))
        with self.assertRaisesRegex(ValueError, "token limit"):
            model(torch.zeros((5, 4096), dtype=torch.long))
        with self.assertRaisesRegex(ValueError, "attention memory"):
            model(torch.zeros((4, 4096), dtype=torch.long))

    def test_load_arrays_copies_values_and_rejects_whole_invalid_mapping(self):
        arrays = {name: value.detach().numpy().copy() for name, value in self.model.params.items()}
        original = {name: value.copy() for name, value in arrays.items()}
        final = next(reversed(arrays))
        for corruption in (arrays[final].astype(np.float64),
                           np.full_like(arrays[final], np.nan), arrays[final][:-1]):
            invalid = {name: value.copy() for name, value in arrays.items()}
            invalid["token_embedding"] += 1
            invalid[final] = corruption
            with self.assertRaises(ValueError):
                self.model.load_arrays(invalid)
            for name, value in self.model.params.items():
                np.testing.assert_array_equal(value.detach().numpy(), original[name])
        with self.assertRaises(ValueError):
            self.model.load_arrays({})
        arrays["head.bias"] += 0.5
        self.model.load_arrays(arrays)
        arrays["head.bias"] += 1
        np.testing.assert_array_equal(self.model.params["head.bias"].detach().numpy(),
                                      np.full(257, 0.5, dtype=np.float32))


if __name__ == "__main__":
    unittest.main()
