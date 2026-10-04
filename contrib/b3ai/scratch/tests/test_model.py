"""Behavioral and numerical checks for the independently initialized model."""

import copy
import math
import unittest

import numpy as np

from contrib.b3ai.scratch.model import AdamW, ModelConfig, Transformer


def reference_loss(model, tokens, targets, mask=None):
    """Independent scalar cross-entropy using only the model's public logits."""
    logits = model.forward(tokens).astype(np.float64)
    probabilities = np.exp(logits - np.max(logits, axis=-1, keepdims=True))
    probabilities /= probabilities.sum(axis=-1, keepdims=True)
    selected = np.take_along_axis(probabilities, targets[..., None], axis=-1)[..., 0]
    weights = np.ones(targets.shape) if mask is None else mask
    return float(np.sum(-np.log(selected) * weights) / np.sum(weights))


class ModelTests(unittest.TestCase):
    def tiny(self, **overrides):
        values = dict(vocab_size=7, context_length=4, width=4,
                      layers=1, heads=2, ff_multiplier=2, seed=17)
        values.update(overrides)
        return Transformer(ModelConfig(**values), dtype=np.float64)

    def test_default_configuration_and_reproducible_initialization(self):
        config = ModelConfig()
        self.assertEqual(config.to_dict(), dict(vocab_size=256, context_length=128,
            width=64, layers=2, heads=4, ff_multiplier=4, seed=42))
        first = Transformer(config)
        second = Transformer(ModelConfig(**config.to_dict()))
        self.assertEqual(first.parameter_count, 141312)
        self.assertEqual(first.parameter_count, sum(array.size for array in first.params.values()))
        for name in first.params:
            np.testing.assert_array_equal(first.params[name], second.params[name])
            self.assertEqual(first.params[name].dtype, np.dtype(np.float32))
        different = Transformer(ModelConfig(seed=43))
        self.assertFalse(np.array_equal(first.params["token_embedding"], different.params["token_embedding"]))

    def test_configuration_and_cpu_caps_fail_before_allocation(self):
        invalid = [dict(vocab_size=0), dict(context_length=0), dict(context_length=513),
                   dict(width=257), dict(width=5, heads=2), dict(layers=9), dict(heads=0),
                   dict(ff_multiplier=9), dict(seed=-1), dict(width=True),
                   dict(width=256, vocab_size=4096, layers=8)]
        for overrides in invalid:
            with self.subTest(overrides=overrides), self.assertRaises(ValueError):
                ModelConfig(**overrides)
        with self.assertRaises(ValueError):
            Transformer(ModelConfig(), dtype=np.float16)
        with self.assertRaises(TypeError):
            Transformer({"width": 4})

    def test_forward_shape_and_input_validation(self):
        model = self.tiny()
        tokens = np.array([[1, 2, 3], [3, 2, 1]])
        logits = model.forward(tokens)
        self.assertEqual(logits.shape, (2, 3, 7))
        self.assertEqual(logits.dtype, np.dtype(np.float64))
        self.assertTrue(np.all(np.isfinite(logits)))
        invalid = [np.array([1, 2]), np.empty((1, 0), dtype=int), np.array([[7]]),
                   np.array([[-1]]), np.ones((1, 5), dtype=int), np.ones((17, 1), dtype=int),
                   np.array([[1.0]]), np.array([[True]])]
        for tokens in invalid:
            with self.subTest(tokens=tokens), self.assertRaises(ValueError):
                model.forward(tokens)
        bounded = Transformer(ModelConfig(vocab_size=7, context_length=512,
            width=16, heads=16, layers=2, ff_multiplier=1))
        with self.assertRaisesRegex(ValueError, "attention"):
            bounded.forward(np.ones((1, 512), dtype=int))

    def test_causality_future_tokens_cannot_change_prefix(self):
        model = self.tiny(context_length=6, width=8, heads=2, layers=2)
        original = np.array([[1, 2, 3, 4, 5, 6], [2, 1, 2, 1, 2, 1]])
        changed = original.copy()
        changed[:, 3:] = np.array([[0, 0, 0], [6, 5, 4]])
        logits = model.forward(original)
        np.testing.assert_allclose(logits[:, :3], model.forward(changed)[:, :3], rtol=0, atol=0)
        np.testing.assert_allclose(logits[:, :3], model.forward(original[:, :3]), rtol=1e-12, atol=1e-12)
        self.assertFalse(np.allclose(logits[:, 3:], model.forward(changed)[:, 3:]))

    def check_finite_differences(self, model, tokens, targets, mask=None, all_elements=True):
        loss, gradients = model.loss_and_grad(tokens, targets, mask)
        self.assertEqual(gradients.keys(), model.params.keys())
        self.assertAlmostEqual(loss, reference_loss(model, tokens, targets, mask), places=12)
        epsilon = 1e-5
        checked = 0
        for name, parameter in model.params.items():
            self.assertEqual(parameter.shape, gradients[name].shape)
            self.assertEqual(parameter.dtype, gradients[name].dtype)
            self.assertTrue(np.all(np.isfinite(gradients[name])))
            if all_elements:
                indices = list(np.ndindex(parameter.shape))
            else:
                # Pick each family's strongest derivative plus a distinct entry.
                maximum = np.unravel_index(np.argmax(np.abs(gradients[name])), parameter.shape)
                indices = [maximum, tuple(0 for _ in parameter.shape)]
            for index in indices:
                value = parameter[index].copy()
                try:
                    parameter[index] = value + epsilon
                    plus = reference_loss(model, tokens, targets, mask)
                    parameter[index] = value - epsilon
                    minus = reference_loss(model, tokens, targets, mask)
                finally:
                    parameter[index] = value
                numerical = (plus - minus) / (2 * epsilon)
                with self.subTest(parameter=name, index=index):
                    np.testing.assert_allclose(gradients[name][index], numerical, rtol=2e-4, atol=2e-6)
                checked += 1
        return checked

    def test_manual_gradients_every_element_of_every_parameter_family(self):
        model = self.tiny()
        tokens = np.array([[1, 2, 1], [3, 4, 2]])
        targets = np.array([[2, 1, 3], [4, 2, 5]])
        self.assertEqual(self.check_finite_differences(model, tokens, targets), model.parameter_count)

    def test_manual_gradients_through_multiple_blocks_and_fractional_mask(self):
        model = self.tiny(layers=2, seed=29)
        tokens = np.array([[1, 2, 1], [3, 4, 2]])
        targets = np.array([[2, 1, 3], [4, 2, 5]])
        mask = np.array([[1, 0, 0.5], [0, 1, 1]], dtype=np.float64)
        self.assertEqual(self.check_finite_differences(model, tokens, targets, mask, False), 2 * len(model.params))

    def test_masked_targets_do_not_contribute_to_objective(self):
        model = self.tiny()
        tokens = np.array([[1, 2, 3]])
        first_targets = np.array([[2, 3, 4]])
        second_targets = np.array([[2, 6, 0]])
        mask = np.array([[1, 0, 0]])
        first_loss, first_grads = model.loss_and_grad(tokens, first_targets, mask)
        second_loss, second_grads = model.loss_and_grad(tokens, second_targets, mask)
        self.assertEqual(first_loss, second_loss)
        for name in first_grads:
            np.testing.assert_array_equal(first_grads[name], second_grads[name])
        invalid = [np.zeros((1, 3)), np.ones((3,)), np.array([[1, -1, 0]]),
                   np.array([[1, np.nan, 0]]), np.array([[1, 2, 0]])]
        for value in invalid:
            with self.subTest(mask=value), self.assertRaises(ValueError):
                model.loss_and_grad(tokens, first_targets, value)
        with self.assertRaises(ValueError):
            model.loss_and_grad(tokens, np.array([[1, 2]]))

    def test_model_rejects_corrupt_parameters_and_stays_stateless(self):
        model = self.tiny()
        tokens = np.array([[1, 2, 3]])
        targets = np.array([[2, 3, 4]])
        before_loss, before_grads = model.loss_and_grad(tokens, targets)
        model.forward(np.array([[6, 5, 4, 3]]))
        after_loss, after_grads = model.loss_and_grad(tokens, targets)
        self.assertEqual(before_loss, after_loss)
        for name in before_grads:
            np.testing.assert_array_equal(before_grads[name], after_grads[name])
        model.params["head.weight"][0, 0] = np.nan
        with self.assertRaisesRegex(ValueError, "non-finite"):
            model.forward(tokens)

    def test_generation_greedy_sampling_and_context_cropping(self):
        model = self.tiny(context_length=3)
        prompt = [1, 2, 3, 4, 5, 6]
        expected = prompt.copy()
        for _ in range(5):
            expected.append(int(np.argmax(model.forward(np.array([expected[-3:]]))[0, -1])))
        self.assertEqual(model.generate(prompt, 5), expected)
        self.assertEqual(model.generate(prompt, 0), prompt)
        first = model.generate(prompt, 8, temperature=0.7, seed=12)
        self.assertEqual(first, model.generate(prompt, 8, temperature=0.7, seed=12))
        self.assertEqual(first[:len(prompt)], prompt)
        self.assertEqual(len(first), len(prompt) + 8)
        self.assertTrue(all(0 <= value < model.config.vocab_size for value in first))
        self.assertEqual(model.generate(prompt, 5, temperature=1e-300), expected)
        for arguments in [([], 1, 0), ([7], 1, 0), ([1.0], 1, 0), ([1], -1, 0),
                          ([1], 257, 0), ([1], 1, -1), ([1], 1, math.inf), ([1], 1, "cold")]:
            with self.subTest(arguments=arguments), self.assertRaises(ValueError):
                model.generate(*arguments)

    def test_float32_training_is_finite_and_learns_tiny_repeated_corpus(self):
        model = Transformer(ModelConfig(vocab_size=8, context_length=9, width=16,
            layers=1, heads=4, ff_multiplier=2, seed=123))
        optimizer = AdamW(model.params)
        tokens = np.array([[1, 2, 3, 1, 2, 3, 1, 2, 3],
                           [2, 3, 1, 2, 3, 1, 2, 3, 1],
                           [3, 1, 2, 3, 1, 2, 3, 1, 2]])
        targets = np.roll(tokens, -1, axis=1)
        initial, _ = model.loss_and_grad(tokens, targets)
        for _ in range(60):
            loss, gradients = model.loss_and_grad(tokens, targets)
            self.assertTrue(math.isfinite(loss))
            for name, gradient in gradients.items():
                self.assertEqual(gradient.dtype, np.dtype(np.float32))
                self.assertTrue(np.all(np.isfinite(gradient)), name)
            optimizer.step(model.params, gradients, lr=0.01, weight_decay=0, clip_norm=1)
        final, _ = model.loss_and_grad(tokens, targets)
        self.assertLess(final, initial * 0.1)
        self.assertLess(final, 0.05)
        self.assertEqual(optimizer.steps, 60)
        self.assertEqual(model.generate([1, 2, 3], 6), [1, 2, 3, 1, 2, 3, 1, 2, 3])


class AdamWTests(unittest.TestCase):
    def test_known_bias_correction_decay_and_global_clipping(self):
        params = {"a": np.array([1.0]), "b": np.array([-2.0])}
        optimizer = AdamW(params)
        grads = {"a": np.array([3.0]), "b": np.array([-4.0])}
        optimizer.step(params, grads, lr=0.1, weight_decay=0.01, clip_norm=1)
        expected_gradient = {"a": 0.6, "b": -0.8}
        original = {"a": 1.0, "b": -2.0}
        for name, gradient in expected_gradient.items():
            expected = original[name] * 0.999 - 0.1 * gradient / (abs(gradient) + 1e-8)
            np.testing.assert_allclose(params[name], [expected], rtol=0, atol=1e-12)
            np.testing.assert_allclose(optimizer.m[name], [0.1 * gradient], rtol=0, atol=1e-12)
            np.testing.assert_allclose(optimizer.v[name], [0.001 * gradient**2], rtol=0, atol=1e-12)
        self.assertEqual(optimizer.steps, 1)

    def test_state_roundtrip_preserves_exact_continuation_and_owns_copies(self):
        params = {"matrix": np.array([[0.2, -0.3], [1.1, 0.5]], dtype=np.float32)}
        optimizer = AdamW(params)
        grads = {"matrix": np.array([[0.1, 0.2], [-0.4, 0.5]], dtype=np.float32)}
        for _ in range(3):
            optimizer.step(params, grads, lr=0.001)
        saved = optimizer.state_dict()
        resumed_params = {name: value.copy() for name, value in params.items()}
        resumed = AdamW(resumed_params)
        resumed.load_state_dict(saved)
        for _ in range(4):
            optimizer.step(params, grads, lr=0.001)
            resumed.step(resumed_params, grads, lr=0.001)
        self.assertEqual(optimizer.steps, resumed.steps)
        for name in params:
            np.testing.assert_array_equal(params[name], resumed_params[name])
            np.testing.assert_array_equal(optimizer.m[name], resumed.m[name])
            np.testing.assert_array_equal(optimizer.v[name], resumed.v[name])
        saved["m"]["matrix"][...] = 123
        self.assertFalse(np.any(resumed.m["matrix"] == 123))
        self.assertFalse(np.any(optimizer.m["matrix"] == 123))

    def test_bad_gradient_never_partially_updates_model_or_state(self):
        params = {"first": np.array([1.0]), "last": np.array([2.0])}
        optimizer = AdamW(params)
        original = copy.deepcopy(params)
        invalid = [{"first": np.array([0.1]), "last": np.array([np.nan])},
                   {"first": np.array([0.1]), "last": np.array([np.inf])},
                   {"first": np.array([0.1]), "last": np.array([1.0, 2.0])},
                   {"first": np.array([0.1])}]
        for grads in invalid:
            with self.subTest(grads=grads), self.assertRaises(ValueError):
                optimizer.step(params, grads, lr=0.001)
            self.assertEqual(optimizer.steps, 0)
            for name in params:
                np.testing.assert_array_equal(params[name], original[name])
                np.testing.assert_array_equal(optimizer.m[name], np.zeros_like(params[name]))
                np.testing.assert_array_equal(optimizer.v[name], np.zeros_like(params[name]))
        with self.assertRaises(ValueError):
            optimizer.step(params, {name: value.copy() for name, value in params.items()}, lr=math.inf)

    def test_bad_state_restore_is_atomic(self):
        params = {"x": np.array([1.0])}
        optimizer = AdamW(params)
        optimizer.step(params, {"x": np.array([0.2])}, lr=0.001)
        original = optimizer.state_dict()
        bad = optimizer.state_dict()
        bad["v"]["x"][0] = -1
        with self.assertRaises(ValueError):
            optimizer.load_state_dict(bad)
        self.assertEqual(optimizer.steps, original["steps"])
        np.testing.assert_array_equal(optimizer.m["x"], original["m"]["x"])
        np.testing.assert_array_equal(optimizer.v["x"], original["v"]["x"])
        for overrides in [dict(beta1=1), dict(beta2=-0.1), dict(eps=0), dict(eps=math.nan)]:
            with self.subTest(overrides=overrides), self.assertRaises(ValueError):
                AdamW(params, **overrides)


if __name__ == "__main__":
    unittest.main()
