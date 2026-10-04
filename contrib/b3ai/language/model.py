# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Our own random-start Transformer, accelerated with PyTorch autograd.

The architecture, parameter names and initialization match the small NumPy
reference. This separate version admits a larger bounded language experiment;
it does not load pretrained weights or claim conversation competence.
"""

from collections.abc import Mapping
from dataclasses import asdict, dataclass
import math

import numpy as np
import torch
from torch import nn
from torch.nn import functional as F


MAX_PARAMETERS = 8_000_000
MAX_BATCH_SIZE = 16
MAX_BATCH_TOKENS = 16_384
MAX_ATTENTION_ELEMENTS = 1_073_741_824
LAYER_NORM_EPSILON = 1e-5


@dataclass(frozen=True)
class GpuConfig:
    vocab_size: int = 257
    context_length: int = 2048
    width: int = 256
    layers: int = 6
    heads: int = 8
    ff_multiplier: int = 4
    seed: int = 42

    def __post_init__(self):
        limits = {
            "context_length": (1, 4096), "width": (1, 256),
            "layers": (1, 8), "heads": (1, 16),
            "ff_multiplier": (1, 8), "seed": (0, 2**63 - 1),
        }
        if type(self.vocab_size) is not int or self.vocab_size not in (256, 257):
            raise ValueError("vocab_size must be 256 (reference) or 257 (language)")
        for name, (minimum, maximum) in limits.items():
            value = getattr(self, name)
            if type(value) is not int or not minimum <= value <= maximum:
                raise ValueError(f"{name} must be an integer between {minimum} and {maximum}")
        if self.width % self.heads:
            raise ValueError("width must be divisible by heads")
        if _parameter_count(self) > MAX_PARAMETERS:
            raise ValueError(f"configuration exceeds the {MAX_PARAMETERS:,} parameter limit")

    def to_dict(self):
        return asdict(self)


def _parameter_count(config):
    width = config.width
    hidden = width * config.ff_multiplier
    block = 4 * width**2 + 2 * width * hidden + 9 * width + hidden
    return (2 * config.vocab_size * width + config.context_length * width
            + config.layers * block + 2 * width + config.vocab_size)


def parameter_shapes(config):
    """Return the canonical, ordered parameter schema without allocating tensors."""
    if not isinstance(config, GpuConfig):
        raise TypeError("config must be a GpuConfig")
    width, hidden = config.width, config.width * config.ff_multiplier
    shapes = {"token_embedding": (config.vocab_size, width),
              "position_embedding": (config.context_length, width)}

    def norm(prefix):
        shapes[prefix + ".weight"] = (width,)
        shapes[prefix + ".bias"] = (width,)

    for layer in range(config.layers):
        prefix = f"blocks.{layer}"
        norm(prefix + ".ln1")
        shapes[prefix + ".attention.qkv.weight"] = (width, 3 * width)
        shapes[prefix + ".attention.qkv.bias"] = (3 * width,)
        shapes[prefix + ".attention.projection.weight"] = (width, width)
        shapes[prefix + ".attention.projection.bias"] = (width,)
        norm(prefix + ".ln2")
        shapes[prefix + ".mlp.input.weight"] = (width, hidden)
        shapes[prefix + ".mlp.input.bias"] = (hidden,)
        shapes[prefix + ".mlp.output.weight"] = (hidden, width)
        shapes[prefix + ".mlp.output.bias"] = (width,)
    norm("final_ln")
    shapes["head.weight"] = (width, config.vocab_size)
    shapes["head.bias"] = (config.vocab_size,)
    return shapes


def _layer_norm(x, weight, bias):
    # Explicit centered variance preserves the reference's definition, rather
    # than delegating to a backend kernel with a different variance convention.
    centered = x - x.mean(dim=-1, keepdim=True)
    inverse_std = torch.rsqrt((centered * centered).mean(dim=-1, keepdim=True)
                             + LAYER_NORM_EPSILON)
    return centered * inverse_std * weight + bias


class TorchTransformer(nn.Module):
    """Pre-LN decoder with causal attention, ReLU MLP and an untied output head."""

    def __init__(self, config):
        super().__init__()
        if not isinstance(config, GpuConfig):
            raise TypeError("config must be a GpuConfig")
        self.config = config
        self._shapes = parameter_shapes(config)
        self._parameter_ids = {}
        rng = np.random.default_rng(config.seed)
        width = config.width
        hidden = width * config.ff_multiplier
        residual_scale = 1 / math.sqrt(2 * config.layers)

        for name, shape in self._shapes.items():
            if name.endswith(".bias"):
                array = np.zeros(shape, dtype=np.float32)
            elif ".ln" in name or name == "final_ln.weight":
                array = np.ones(shape, dtype=np.float32)
            else:
                if name in ("token_embedding", "position_embedding"):
                    scale = 0.02
                elif name.endswith("attention.projection.weight"):
                    scale = residual_scale / math.sqrt(width)
                elif name.endswith("mlp.input.weight"):
                    scale = math.sqrt(2 / width)
                elif name.endswith("mlp.output.weight"):
                    scale = residual_scale / math.sqrt(hidden)
                else:
                    scale = 1 / math.sqrt(width)
                array = rng.normal(0, scale, shape).astype(np.float32)
            # Dots are forbidden in direct nn.Parameter registration names.
            # Keep canonical external names in a mapping; registered IDs stay
            # stable while .to(device) is free to replace Parameter objects.
            identifier = f"p{len(self._parameter_ids)}"
            self._parameter_ids[name] = identifier
            self.register_parameter(identifier, nn.Parameter(torch.from_numpy(array)))

    @property
    def params(self):
        return {name: getattr(self, identifier)
                for name, identifier in self._parameter_ids.items()}

    @property
    def parameter_count(self):
        return _parameter_count(self.config)

    def load_arrays(self, arrays):
        """Validate every CPU array before copying any parameter to its device."""
        if not isinstance(arrays, Mapping) or arrays.keys() != self._shapes.keys():
            raise ValueError("parameter names do not match the configuration")
        for name, shape in self._shapes.items():
            value = arrays[name]
            if (not isinstance(value, np.ndarray) or value.shape != shape
                    or value.dtype != np.dtype(np.float32)
                    or not np.all(np.isfinite(value))):
                raise ValueError(f"invalid parameter shape, dtype or value: {name}")
        parameters = self.params
        if any(parameter.dtype != torch.float32 for parameter in parameters.values()):
            raise ValueError("model parameters must be float32")
        with torch.no_grad():
            for name, parameter in parameters.items():
                parameter.copy_(torch.from_numpy(np.array(arrays[name], copy=True)))

    def _tokens(self, tokens, name="tokens"):
        if not isinstance(tokens, torch.Tensor) or tokens.ndim != 2 or tokens.dtype != torch.long:
            raise ValueError(f"{name} must be an int64 tensor [batch, time]")
        batch, time = tokens.shape
        if not 1 <= batch <= MAX_BATCH_SIZE or not 1 <= time <= self.config.context_length:
            raise ValueError(f"{name} exceeds the batch or context limit, or is empty")
        if batch * time > MAX_BATCH_TOKENS:
            raise ValueError("batch exceeds the token limit")
        if batch * self.config.heads * time**2 * self.config.layers > MAX_ATTENTION_ELEMENTS:
            raise ValueError("batch exceeds the attention memory limit")
        parameter = self.p0
        if parameter.dtype != torch.float32:
            raise ValueError("model parameters must be float32")
        if tokens.device != parameter.device:
            raise ValueError(f"{name} and model must be on the same device")
        if bool(torch.any((tokens < 0) | (tokens >= self.config.vocab_size)).item()):
            raise ValueError(f"{name} contains a token outside the vocabulary")
        return tokens

    def forward(self, tokens):
        """Return next-token logits [batch, time, vocabulary], respecting causality."""
        tokens = self._tokens(tokens)
        batch, time = tokens.shape
        width, heads = self.config.width, self.config.heads
        head_width = width // heads
        parameters = self.params
        positions = torch.arange(time, device=tokens.device)
        x = (parameters["token_embedding"][tokens]
             + parameters["position_embedding"][positions][None, :, :])
        causal_mask = torch.ones((time, time), dtype=torch.bool,
                                 device=tokens.device).triu(diagonal=1)
        for layer in range(self.config.layers):
            prefix = f"blocks.{layer}"
            y = _layer_norm(x, parameters[prefix + ".ln1.weight"],
                            parameters[prefix + ".ln1.bias"])
            qkv = y @ parameters[prefix + ".attention.qkv.weight"] + parameters[prefix + ".attention.qkv.bias"]
            q, k, v = qkv.reshape(batch, time, 3, heads, head_width).permute(2, 0, 3, 1, 4).unbind(0)
            scores = (q @ k.transpose(-1, -2)) * (1 / math.sqrt(head_width))
            attention = torch.softmax(scores.masked_fill(causal_mask, float("-inf")), dim=-1)
            joined = (attention @ v).transpose(1, 2).reshape(batch, time, width)
            x = x + joined @ parameters[prefix + ".attention.projection.weight"] + parameters[prefix + ".attention.projection.bias"]
            y = _layer_norm(x, parameters[prefix + ".ln2.weight"],
                            parameters[prefix + ".ln2.bias"])
            hidden = torch.relu(y @ parameters[prefix + ".mlp.input.weight"] + parameters[prefix + ".mlp.input.bias"])
            x = x + hidden @ parameters[prefix + ".mlp.output.weight"] + parameters[prefix + ".mlp.output.bias"]
        final = _layer_norm(x, parameters["final_ln.weight"], parameters["final_ln.bias"])
        return final @ parameters["head.weight"] + parameters["head.bias"]

    def loss(self, tokens, targets, mask=None):
        """Mean next-token CE, optionally weighted by finite weights in [0, 1]."""
        self._tokens(tokens)
        self._tokens(targets, "targets")
        if tokens.shape != targets.shape:
            raise ValueError("tokens and targets must have matching shapes")
        if mask is not None:
            if (not isinstance(mask, torch.Tensor) or mask.shape != tokens.shape
                    or mask.device != tokens.device or mask.is_complex()):
                raise ValueError("mask must be a real tensor matching tokens and device")
            if bool(torch.any(~torch.isfinite(mask) | (mask < 0) | (mask > 1)).item()):
                raise ValueError("mask weights must be finite and between zero and one")
            mask = mask.to(dtype=torch.float32)
            denominator = mask.sum()
            if denominator.item() <= 0:
                raise ValueError("mask must include at least one positive weight")
        logits = self.forward(tokens)
        losses = F.cross_entropy(logits.reshape(-1, self.config.vocab_size),
                                 targets.reshape(-1), reduction="none").reshape(tokens.shape)
        return losses.mean() if mask is None else (losses * mask).sum() / denominator
