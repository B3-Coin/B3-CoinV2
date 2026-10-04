"""A small, randomly initialized causal Transformer with manual NumPy gradients.

There are no pretrained weights, framework autograd, network calls, or persistent
forward caches. This CPU reference model is intended for controlled experiments,
not deployment or claims of useful pretrained language understanding.
"""

from dataclasses import asdict, dataclass
import math

import numpy as np


MAX_PARAMETERS = 2_000_000
MAX_BATCH_SIZE = 16
MAX_BATCH_TOKENS = 4096
MAX_ATTENTION_ELEMENTS = 4_194_304
MAX_GENERATION_TOKENS = 256
MAX_PROMPT_TOKENS = 4096
MAX_GENERATION_WORK = 10_000_000_000
LAYER_NORM_EPSILON = 1e-5


@dataclass(frozen=True)
class ModelConfig:
    vocab_size: int = 256
    context_length: int = 128
    width: int = 64
    layers: int = 2
    heads: int = 4
    ff_multiplier: int = 4
    seed: int = 42

    def __post_init__(self):
        limits = {
            "vocab_size": (2, 4096),
            "context_length": (1, 512),
            "width": (1, 256),
            "layers": (1, 8),
            "heads": (1, 16),
            "ff_multiplier": (1, 8),
            "seed": (0, 2**63 - 1),
        }
        for name, (minimum, maximum) in limits.items():
            value = getattr(self, name)
            if type(value) is not int or not minimum <= value <= maximum:
                raise ValueError(f"{name} must be an integer between {minimum} and {maximum}")
        if self.width % self.heads:
            raise ValueError("width must be divisible by heads")
        if _parameter_count(self) > MAX_PARAMETERS:
            raise ValueError(f"configuration exceeds the {MAX_PARAMETERS:,} parameter CPU limit")

    def to_dict(self):
        return asdict(self)


def _parameter_count(config):
    width = config.width
    hidden = width * config.ff_multiplier
    block = 4 * width**2 + 2 * width * hidden + 9 * width + hidden
    return (2 * config.vocab_size * width + config.context_length * width
            + config.layers * block + 2 * width + config.vocab_size)


def _layer_norm(x, weight, bias):
    centered = x - x.mean(axis=-1, keepdims=True)
    inverse_std = 1 / np.sqrt(np.mean(centered * centered, axis=-1, keepdims=True)
                              + LAYER_NORM_EPSILON)
    normalized = centered * inverse_std
    return normalized * weight + bias, (normalized, inverse_std, weight)


def _layer_norm_backward(gradient, cache):
    normalized, inverse_std, weight = cache
    weight_gradient = np.sum(gradient * normalized, axis=(0, 1))
    bias_gradient = np.sum(gradient, axis=(0, 1))
    normalized_gradient = gradient * weight
    x_gradient = inverse_std * (
        normalized_gradient - normalized_gradient.mean(axis=-1, keepdims=True)
        - normalized * np.mean(normalized_gradient * normalized, axis=-1, keepdims=True))
    return x_gradient, weight_gradient, bias_gradient


def _softmax(x):
    exponentials = np.exp(x - np.max(x, axis=-1, keepdims=True))
    return exponentials / np.sum(exponentials, axis=-1, keepdims=True)


def _matrix_gradient(inputs, outputs):
    return inputs.reshape(-1, inputs.shape[-1]).T @ outputs.reshape(-1, outputs.shape[-1])


class Transformer:
    """Pre-LN decoder: learned embeddings, causal attention, ReLU MLP, untied head."""

    def __init__(self, config, dtype=np.float32):
        if not isinstance(config, ModelConfig):
            raise TypeError("config must be a ModelConfig")
        self.config = config
        self.dtype = np.dtype(dtype)
        if self.dtype not in (np.dtype(np.float32), np.dtype(np.float64)):
            raise ValueError("model dtype must be float32 or float64")
        self.params = {}
        rng = np.random.default_rng(config.seed)

        def normal(name, shape, scale):
            self.params[name] = rng.normal(0, scale, shape).astype(self.dtype)

        def zeros(name, size):
            self.params[name] = np.zeros(size, dtype=self.dtype)

        def norm(name):
            self.params[name + ".weight"] = np.ones(config.width, dtype=self.dtype)
            zeros(name + ".bias", config.width)

        width = config.width
        hidden = width * config.ff_multiplier
        residual_scale = 1 / math.sqrt(2 * config.layers)
        normal("token_embedding", (config.vocab_size, width), 0.02)
        normal("position_embedding", (config.context_length, width), 0.02)
        for layer in range(config.layers):
            prefix = f"blocks.{layer}"
            norm(prefix + ".ln1")
            normal(prefix + ".attention.qkv.weight", (width, 3 * width), 1 / math.sqrt(width))
            zeros(prefix + ".attention.qkv.bias", 3 * width)
            normal(prefix + ".attention.projection.weight", (width, width), residual_scale / math.sqrt(width))
            zeros(prefix + ".attention.projection.bias", width)
            norm(prefix + ".ln2")
            normal(prefix + ".mlp.input.weight", (width, hidden), math.sqrt(2 / width))
            zeros(prefix + ".mlp.input.bias", hidden)
            normal(prefix + ".mlp.output.weight", (hidden, width), residual_scale / math.sqrt(hidden))
            zeros(prefix + ".mlp.output.bias", width)
        norm("final_ln")
        normal("head.weight", (width, config.vocab_size), 1 / math.sqrt(width))
        zeros("head.bias", config.vocab_size)
        self._shapes = {name: value.shape for name, value in self.params.items()}

    @property
    def parameter_count(self):
        return sum(value.size for value in self.params.values())

    def _validate_params(self):
        if self.params.keys() != self._shapes.keys():
            raise ValueError("model parameter names do not match its configuration")
        for name, shape in self._shapes.items():
            value = self.params[name]
            if not isinstance(value, np.ndarray) or value.shape != shape or value.dtype != self.dtype:
                raise ValueError(f"invalid parameter shape or dtype: {name}")
            if not np.all(np.isfinite(value)):
                raise ValueError(f"non-finite model parameter: {name}")

    def _tokens(self, tokens, name="tokens"):
        value = np.asarray(tokens)
        if value.ndim != 2 or value.dtype.kind not in "iu":
            raise ValueError(f"{name} must be a two-dimensional integer array [batch, time]")
        batch, time = value.shape
        if not 1 <= batch <= MAX_BATCH_SIZE or not 1 <= time <= self.config.context_length:
            raise ValueError(f"{name} exceeds the batch or context limit, or is empty")
        if batch * time > MAX_BATCH_TOKENS:
            raise ValueError("batch exceeds the CPU token limit")
        if batch * self.config.heads * time**2 * self.config.layers > MAX_ATTENTION_ELEMENTS:
            raise ValueError("batch exceeds the CPU attention memory limit")
        if np.min(value) < 0 or np.max(value) >= self.config.vocab_size:
            raise ValueError(f"{name} contains a token outside the vocabulary")
        return value.astype(np.int64, copy=False)

    def _run(self, tokens, cache_required):
        self._validate_params()
        tokens = self._tokens(tokens)
        batch, time = tokens.shape
        width = self.config.width
        heads = self.config.heads
        head_width = width // heads
        scale = 1 / math.sqrt(head_width)
        causal_mask = np.triu(np.ones((time, time), dtype=bool), k=1)
        x = (self.params["token_embedding"][tokens]
             + self.params["position_embedding"][np.arange(time)][None, :, :])
        blocks = []
        with np.errstate(over="raise", invalid="raise", divide="raise"):
            for layer in range(self.config.layers):
                prefix = f"blocks.{layer}"
                y1, ln1_cache = _layer_norm(x, self.params[prefix + ".ln1.weight"], self.params[prefix + ".ln1.bias"])
                qkv = y1 @ self.params[prefix + ".attention.qkv.weight"] + self.params[prefix + ".attention.qkv.bias"]
                q, k, v = qkv.reshape(batch, time, 3, heads, head_width).transpose(2, 0, 3, 1, 4)
                scores = (q @ k.transpose(0, 1, 3, 2)) * scale
                attention = _softmax(np.where(causal_mask[None, None, :, :], -np.inf, scores))
                attended = attention @ v
                joined = attended.transpose(0, 2, 1, 3).reshape(batch, time, width)
                x1 = x + joined @ self.params[prefix + ".attention.projection.weight"] + self.params[prefix + ".attention.projection.bias"]
                y2, ln2_cache = _layer_norm(x1, self.params[prefix + ".ln2.weight"], self.params[prefix + ".ln2.bias"])
                preactivation = y2 @ self.params[prefix + ".mlp.input.weight"] + self.params[prefix + ".mlp.input.bias"]
                hidden = np.maximum(preactivation, 0)
                x = x1 + hidden @ self.params[prefix + ".mlp.output.weight"] + self.params[prefix + ".mlp.output.bias"]
                if cache_required:
                    blocks.append((y1, ln1_cache, q, k, v, attention, joined,
                                   y2, ln2_cache, preactivation, hidden))
            final, final_norm_cache = _layer_norm(x, self.params["final_ln.weight"], self.params["final_ln.bias"])
            logits = final @ self.params["head.weight"] + self.params["head.bias"]
        if not np.all(np.isfinite(logits)):
            raise FloatingPointError("model produced non-finite logits")
        cache = (tokens, blocks, final, final_norm_cache) if cache_required else None
        return logits, cache

    def forward(self, tokens):
        """Return causal next-token logits of shape [batch, time, vocabulary]."""
        return self._run(tokens, False)[0]

    def loss_and_grad(self, tokens, targets, loss_mask=None):
        """Mean masked next-token cross-entropy and all manual analytic gradients.

        loss_mask may contain weights between zero and one; normalization uses
        their sum. An entirely masked batch is rejected rather than silently
        yielding an optimizer update without a training objective.
        """
        tokens = self._tokens(tokens)
        targets = self._tokens(targets, "targets")
        if tokens.shape != targets.shape:
            raise ValueError("tokens and targets must have matching shapes")
        weights = np.ones(tokens.shape, dtype=self.dtype) if loss_mask is None else np.asarray(loss_mask)
        if weights.shape != tokens.shape or weights.dtype.kind not in "bifu":
            raise ValueError("loss_mask must be a numeric array matching tokens")
        if not np.all(np.isfinite(weights)) or np.any(weights < 0) or np.any(weights > 1):
            raise ValueError("loss_mask weights must be finite and between zero and one")
        weights = weights.astype(self.dtype)
        denominator = float(np.sum(weights, dtype=np.float64))
        if denominator <= 0:
            raise ValueError("loss_mask must include at least one positive weight")

        logits, cache = self._run(tokens, True)
        shifted = logits - np.max(logits, axis=-1, keepdims=True)
        exponentials = np.exp(shifted)
        normalizer = np.sum(exponentials, axis=-1, keepdims=True)
        log_probabilities = shifted - np.log(normalizer)
        batch, time = tokens.shape
        rows = np.arange(batch)[:, None]
        columns = np.arange(time)[None, :]
        loss = -float(np.sum(log_probabilities[rows, columns, targets] * weights, dtype=np.float64)) / denominator
        gradient = exponentials / normalizer
        gradient[rows, columns, targets] -= 1
        gradient *= weights[..., None] / denominator

        grads = {name: np.zeros_like(value) for name, value in self.params.items()}
        _, blocks, final, final_norm_cache = cache
        grads["head.weight"] = _matrix_gradient(final, gradient)
        grads["head.bias"] = np.sum(gradient, axis=(0, 1))
        x_gradient, grads["final_ln.weight"], grads["final_ln.bias"] = _layer_norm_backward(
            gradient @ self.params["head.weight"].T, final_norm_cache)
        width = self.config.width
        heads = self.config.heads
        head_width = width // heads
        scale = 1 / math.sqrt(head_width)
        with np.errstate(over="raise", invalid="raise", divide="raise"):
            for layer in reversed(range(self.config.layers)):
                prefix = f"blocks.{layer}"
                (y1, ln1_cache, q, k, v, attention, joined,
                 y2, ln2_cache, preactivation, hidden) = blocks[layer]
                grads[prefix + ".mlp.output.weight"] = _matrix_gradient(hidden, x_gradient)
                grads[prefix + ".mlp.output.bias"] = np.sum(x_gradient, axis=(0, 1))
                hidden_gradient = x_gradient @ self.params[prefix + ".mlp.output.weight"].T
                preactivation_gradient = hidden_gradient * (preactivation > 0)
                grads[prefix + ".mlp.input.weight"] = _matrix_gradient(y2, preactivation_gradient)
                grads[prefix + ".mlp.input.bias"] = np.sum(preactivation_gradient, axis=(0, 1))
                y2_gradient = preactivation_gradient @ self.params[prefix + ".mlp.input.weight"].T
                normalized_gradient, grads[prefix + ".ln2.weight"], grads[prefix + ".ln2.bias"] = _layer_norm_backward(y2_gradient, ln2_cache)
                residual_gradient = x_gradient + normalized_gradient

                grads[prefix + ".attention.projection.weight"] = _matrix_gradient(joined, residual_gradient)
                grads[prefix + ".attention.projection.bias"] = np.sum(residual_gradient, axis=(0, 1))
                joined_gradient = residual_gradient @ self.params[prefix + ".attention.projection.weight"].T
                attended_gradient = joined_gradient.reshape(batch, time, heads, head_width).transpose(0, 2, 1, 3)
                attention_gradient = attended_gradient @ v.transpose(0, 1, 3, 2)
                value_gradient = attention.transpose(0, 1, 3, 2) @ attended_gradient
                score_gradient = attention * (attention_gradient - np.sum(attention_gradient * attention, axis=-1, keepdims=True))
                query_gradient = (score_gradient @ k) * scale
                key_gradient = (score_gradient.transpose(0, 1, 3, 2) @ q) * scale
                qkv_gradient = np.stack((query_gradient.transpose(0, 2, 1, 3),
                                         key_gradient.transpose(0, 2, 1, 3),
                                         value_gradient.transpose(0, 2, 1, 3)), axis=2).reshape(batch, time, 3 * width)
                grads[prefix + ".attention.qkv.weight"] = _matrix_gradient(y1, qkv_gradient)
                grads[prefix + ".attention.qkv.bias"] = np.sum(qkv_gradient, axis=(0, 1))
                y1_gradient = qkv_gradient @ self.params[prefix + ".attention.qkv.weight"].T
                normalized_gradient, grads[prefix + ".ln1.weight"], grads[prefix + ".ln1.bias"] = _layer_norm_backward(y1_gradient, ln1_cache)
                x_gradient = residual_gradient + normalized_gradient
            np.add.at(grads["token_embedding"], tokens.reshape(-1), x_gradient.reshape(-1, width))
            grads["position_embedding"][:time] = np.sum(x_gradient, axis=0)
        if not math.isfinite(loss) or any(not np.all(np.isfinite(value)) for value in grads.values()):
            raise FloatingPointError("training produced a non-finite loss or gradient")
        return loss, grads

    def generate(self, token_ids, max_new_tokens, temperature=0, seed=42):
        """Generate from the current weights; return the prompt and new token IDs."""
        if type(max_new_tokens) is not int or not 0 <= max_new_tokens <= MAX_GENERATION_TOKENS:
            raise ValueError(f"max_new_tokens must be between zero and {MAX_GENERATION_TOKENS}")
        if isinstance(temperature, (bool, str)) or not np.isscalar(temperature):
            raise ValueError("temperature must be a finite nonnegative number")
        temperature = float(temperature)
        if not math.isfinite(temperature) or temperature < 0:
            raise ValueError("temperature must be a finite nonnegative number")
        if type(seed) is not int or not 0 <= seed <= 2**63 - 1:
            raise ValueError("seed must be a nonnegative integer within the supported range")
        prompt = np.asarray(token_ids)
        if (prompt.ndim != 1 or prompt.dtype.kind not in "iu" or not 1 <= prompt.size <= MAX_PROMPT_TOKENS
                or np.min(prompt) < 0 or np.max(prompt) >= self.config.vocab_size):
            raise ValueError("prompt must be a nonempty list of valid token IDs within the prompt limit")
        work = self.parameter_count * min(self.config.context_length, prompt.size + max_new_tokens) * max_new_tokens
        if work > MAX_GENERATION_WORK:
            raise ValueError("generation request exceeds the CPU work limit")
        result = [int(token) for token in prompt]
        rng = np.random.default_rng(seed)
        for _ in range(max_new_tokens):
            context = np.asarray([result[-self.config.context_length:]], dtype=np.int64)
            logits = self.forward(context)[0, -1]
            if temperature == 0:
                next_token = int(np.argmax(logits))
            else:
                # Shift before dividing so tiny positive temperatures remain safe.
                with np.errstate(over="ignore", under="ignore"):
                    shifted = (logits.astype(np.float64) - float(np.max(logits))) / temperature
                    probabilities = _softmax(shifted)
                next_token = int(rng.choice(self.config.vocab_size, p=probabilities))
            result.append(next_token)
        return result


class AdamW:
    """Bias-corrected Adam with decoupled decay and global gradient clipping.

    Decay applies to every supplied parameter. State and parameter updates are
    validated and staged before in-place mutation, so a bad gradient cannot
    leave a partially updated model or optimizer checkpoint.
    """

    def __init__(self, params, beta1=0.9, beta2=0.999, eps=1e-8):
        self.beta1, self.beta2, self.eps = self._hyperparameters(beta1, beta2, eps)
        if not isinstance(params, dict) or not params:
            raise ValueError("params must be a nonempty dictionary of floating arrays")
        self._specs = {}
        for name, value in params.items():
            if (not isinstance(name, str) or not isinstance(value, np.ndarray)
                    or value.dtype not in (np.dtype(np.float32), np.dtype(np.float64))
                    or not np.all(np.isfinite(value))):
                raise ValueError("params must contain named finite float32 or float64 arrays")
            self._specs[name] = (value.shape, value.dtype)
        self.m = {name: np.zeros_like(value) for name, value in params.items()}
        self.v = {name: np.zeros_like(value) for name, value in params.items()}
        self.steps = 0

    @staticmethod
    def _hyperparameters(beta1, beta2, eps):
        values = tuple(float(value) for value in (beta1, beta2, eps))
        if not all(math.isfinite(value) for value in values) or not (0 <= values[0] < 1 and 0 <= values[1] < 1 and values[2] > 0):
            raise ValueError("AdamW requires beta values in [0, 1) and a positive finite epsilon")
        return values

    def _validate_arrays(self, values, description, exact_dtype=True):
        if not isinstance(values, dict) or values.keys() != self._specs.keys():
            raise ValueError(f"{description} names do not match optimizer parameters")
        for name, (shape, dtype) in self._specs.items():
            value = values[name]
            if (not isinstance(value, np.ndarray) or value.shape != shape or value.dtype.kind != "f"
                    or (exact_dtype and value.dtype != dtype) or not np.all(np.isfinite(value))):
                raise ValueError(f"invalid shape, dtype or non-finite value in {description}: {name}")

    def step(self, params, grads, lr, weight_decay=0.01, clip_norm=1.0):
        lr, weight_decay = float(lr), float(weight_decay)
        if not math.isfinite(lr) or lr < 0 or not math.isfinite(weight_decay) or weight_decay < 0 or lr * weight_decay > 1:
            raise ValueError("lr and weight_decay must be finite and nonnegative, with lr * weight_decay <= 1")
        if clip_norm is not None and (not math.isfinite(float(clip_norm)) or float(clip_norm) <= 0):
            raise ValueError("clip_norm must be positive and finite, or None")
        if type(self.steps) is not int or not 0 <= self.steps < 2**63 - 1:
            raise ValueError("invalid optimizer step counter")
        self._validate_arrays(params, "parameters")
        self._validate_arrays(grads, "gradients", exact_dtype=False)
        self._validate_arrays(self.m, "first moments")
        self._validate_arrays(self.v, "second moments")
        if any(np.any(value < 0) for value in self.v.values()):
            raise ValueError("second moments must be nonnegative")
        squared_norm = sum(float(np.sum(np.square(value, dtype=np.float64), dtype=np.float64)) for value in grads.values())
        norm = math.sqrt(squared_norm)
        if not math.isfinite(norm):
            raise ValueError("gradient norm is not finite")
        scale = min(1.0, float(clip_norm) / norm) if clip_norm is not None and norm > 0 else 1.0
        next_step = self.steps + 1
        next_params, next_m, next_v = {}, {}, {}
        with np.errstate(over="raise", invalid="raise", divide="raise"):
            for name, parameter in params.items():
                gradient = (grads[name] * scale).astype(parameter.dtype)
                first = self.beta1 * self.m[name] + (1 - self.beta1) * gradient
                second = self.beta2 * self.v[name] + (1 - self.beta2) * gradient * gradient
                first_corrected = first / (1 - self.beta1**next_step)
                second_corrected = second / (1 - self.beta2**next_step)
                updated = parameter * (1 - lr * weight_decay) - lr * first_corrected / (np.sqrt(second_corrected) + self.eps)
                if not all(np.all(np.isfinite(value)) for value in (first, second, updated)):
                    raise FloatingPointError("optimizer produced a non-finite update")
                next_params[name], next_m[name], next_v[name] = updated, first, second
        for name in params:
            params[name][...] = next_params[name]
            self.m[name][...] = next_m[name]
            self.v[name][...] = next_v[name]
        self.steps = next_step

    def state_dict(self):
        return {"steps": self.steps, "beta1": self.beta1, "beta2": self.beta2, "eps": self.eps,
                "m": {name: value.copy() for name, value in self.m.items()},
                "v": {name: value.copy() for name, value in self.v.items()}}

    def load_state_dict(self, state):
        if not isinstance(state, dict) or state.keys() != {"steps", "beta1", "beta2", "eps", "m", "v"}:
            raise ValueError("invalid optimizer state fields")
        steps = state["steps"]
        if type(steps) is not int or not 0 <= steps < 2**63 - 1:
            raise ValueError("invalid optimizer step counter")
        beta1, beta2, eps = self._hyperparameters(state["beta1"], state["beta2"], state["eps"])
        self._validate_arrays(state["m"], "first moments")
        self._validate_arrays(state["v"], "second moments")
        if any(np.any(value < 0) for value in state["v"].values()):
            raise ValueError("second moments must be nonnegative")
        self.m = {name: value.copy() for name, value in state["m"].items()}
        self.v = {name: value.copy() for name, value in state["v"].items()}
        self.steps, self.beta1, self.beta2, self.eps = steps, beta1, beta2, eps
