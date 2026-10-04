# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Small, reproducible numerical fixtures for a future accelerated model port.

This uses our own randomly initialized NumPy model. It is a mathematical
reference, not a trained checkpoint or evidence of language competence. No
framework, network access, pretrained weights, or new dependency is involved.
"""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import tempfile

import numpy as np

from ..scratch import model as reference_model
from ..scratch.model import AdamW, ModelConfig, Transformer
from ..scratch.tokenizer import VERSION as TOKENIZER_VERSION
from ..scratch.train import implementation_hash


FORMAT = "b3-numpy-golden-v1"
MAX_REFERENCE_BYTES = 8 * 1024 * 1024
MAX_SOURCE_BYTES = 4 * 1024 * 1024
OPTIMIZER_SETTINGS = {
    "beta1": 0.9, "beta2": 0.999, "eps": 1e-8,
    "learning_rate": 0.001, "weight_decay": 0.01, "clip_norm": 1.0,
}


def build_reference():
    """Return copied numeric arrays and a UTF-8 JSON metadata byte array.

    Parameters use the existing model's exact names, prefixed by ``initial__``,
    ``gradient__``, ``updated__``, ``m__``, or ``v__``. ``loss`` is a scalar;
    ``optimizer_steps`` is one. Future ports should reproduce initial values
    before comparing logits, masked loss, gradients, and the optimizer update.
    """
    config = ModelConfig(vocab_size=256, context_length=8, width=8, layers=2,
                         heads=2, ff_multiplier=4, seed=42)
    model = Transformer(config, dtype=np.float32)
    # Compact synthetic questions exercise shared embedding accumulation. The
    # targets are the next bytes; only answer bytes and their newline have loss.
    tokens = np.array([list(b"Q:a\nA:bc"), list(b"Q:z\nA:de")], dtype=np.int64)
    targets = np.array([list(b":a\nA:bc\n"), list(b":z\nA:de\n")], dtype=np.int64)
    loss_mask = np.array([[0, 0, 0, 0, 0, 1, 0.5, 1],
                          [0, 0, 0, 0, 0, 0.25, 1, 1]], dtype=np.float32)
    initial = {name: value.copy() for name, value in model.params.items()}
    logits = model.forward(tokens)
    loss, gradients = model.loss_and_grad(tokens, targets, loss_mask)
    causal_tokens = tokens.copy()
    causal_tokens[:, 6:] = [[120, 121], [122, 119]]
    causal_logits = model.forward(causal_tokens)
    optimizer = AdamW(model.params, beta1=OPTIMIZER_SETTINGS["beta1"],
                      beta2=OPTIMIZER_SETTINGS["beta2"], eps=OPTIMIZER_SETTINGS["eps"])
    optimizer.step(model.params, gradients, lr=OPTIMIZER_SETTINGS["learning_rate"],
                   weight_decay=OPTIMIZER_SETTINGS["weight_decay"],
                   clip_norm=OPTIMIZER_SETTINGS["clip_norm"])
    metadata = {
        "format": FORMAT,
        "purpose": "CPU numerical equivalence for an accelerated implementation",
        "provenance": "own randomly initialized NumPy model; no pretrained weights",
        "readiness": "numerical reference only; no language quality claim",
        "source_implementation_hash": implementation_hash(),
        "tokenizer_version": TOKENIZER_VERSION,
        "numpy_version": np.__version__,
        "config": config.to_dict(),
        "dtype": model.dtype.name,
        "parameter_count": model.parameter_count,
        "parameter_names": sorted(model.params),
        "optimizer": OPTIMIZER_SETTINGS.copy(),
        "loss_reduction": "sum(mask * next-byte cross entropy) / sum(mask)",
        "mask_scope": "answer bytes, including the final newline; fractional weights",
        "causal_prefix_length": 6,
        "reference_caps": {
            "max_archive_and_array_bytes": MAX_REFERENCE_BYTES,
            "max_parameters": reference_model.MAX_PARAMETERS,
            "max_batch_size": reference_model.MAX_BATCH_SIZE,
            "max_batch_tokens": reference_model.MAX_BATCH_TOKENS,
            "max_attention_elements": reference_model.MAX_ATTENTION_ELEMENTS,
        },
    }
    arrays = {
        "tokens": tokens, "targets": targets, "loss_mask": loss_mask,
        "logits": logits, "loss": np.array(loss, dtype=np.float64),
        "causal_tokens": causal_tokens, "causal_logits": causal_logits,
        "causal_prefix_length": np.array(6, dtype=np.int64),
        "optimizer_steps": np.array(optimizer.steps, dtype=np.int64),
    }
    for prefix, values in (("initial", initial), ("gradient", gradients),
                           ("updated", model.params), ("m", optimizer.m),
                           ("v", optimizer.v)):
        for name in sorted(values):
            arrays[f"{prefix}__{name}"] = values[name].copy()
    arrays["metadata"] = np.frombuffer(
        json.dumps(metadata, sort_keys=True, separators=(",", ":"),
                   allow_nan=False).encode("utf-8"), dtype=np.uint8).copy()
    if sum(value.nbytes for value in arrays.values()) > MAX_REFERENCE_BYTES:
        raise ValueError("Reference arrays exceed the fixed archive budget.")
    if any(value.dtype.hasobject for value in arrays.values()):
        raise ValueError("Reference arrays must never contain Python objects.")
    return arrays


def write_reference(path):
    """Publish a complete bounded NPZ atomically, refusing any existing path."""
    path = Path(path)
    if path.exists() or path.is_symlink():
        raise FileExistsError(f"Reference output already exists: {path}")
    arrays = build_reference()
    stream = io.BytesIO()
    np.savez_compressed(stream, **arrays)
    payload = stream.getvalue()
    if len(payload) > MAX_REFERENCE_BYTES:
        raise ValueError("Reference archive exceeds its size budget.")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".b3-golden-",
                                         delete=False) as output:
            temporary = output.name
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        # Same-directory hardlink publication is atomic and fails on EEXIST,
        # including a destination created while the fixture was being built.
        os.link(temporary, path)
    finally:
        if temporary is not None:
            os.unlink(temporary)
    return {
        "format": FORMAT,
        "path": str(path),
        "archive_sha256": hashlib.sha256(payload).hexdigest(),
        "archive_bytes": len(payload),
        "array_bytes": sum(value.nbytes for value in arrays.values()),
        "array_count": len(arrays),
    }


_STRING = r'"(?:[^"\\\r\n]|\\[^\r\n])*"'
_PROMPT_BLOCK = re.compile(
    r"QJsonArray\s+B3AiClient::messages\(\)\s+const\s*\{\s*"
    r"QString\s+system\s*=\s*QStringLiteral\(\s*"
    rf"(?P<base>(?:{_STRING}\s*)+)\);\s*"
    r"system\s*\+=\s*m_tools_enabled\s*\?\s*QStringLiteral\(\s*"
    rf"(?P<enabled>{_STRING})\s*\)\s*:\s*QStringLiteral\(\s*"
    rf"(?P<disabled>{_STRING})\s*\);\s*"
    r"system\s*\+=\s*QStringLiteral\(\s*"
    rf"(?P<begin>{_STRING})\s*\)\s*\+\s*m_lesson_context\s*\+\s*"
    rf"QStringLiteral\(\s*(?P<end>{_STRING})\s*\);"
)


def _decode_literals(expression):
    # Accept only adjacent JSON-compatible C++ string literals. Never evaluate
    # source, invoke a compiler, or resolve arbitrary QString expressions.
    chunks = re.findall(_STRING, expression)
    if not chunks or re.sub(_STRING, "", expression).strip():
        raise ValueError("Unsupported Hive prompt string expression.")
    try:
        result = "".join(json.loads(chunk) for chunk in chunks)
        result.encode("utf-8", errors="strict")
        return result
    except (ValueError, UnicodeError) as error:
        raise ValueError("Hive prompt uses unsupported string escapes.") from error


def measure_hive_prompt(path):
    """Measure the known C++ system block with no lesson, user, or history.

    Returns byte counts and hashes of the source and decoded text. This narrow
    source parser deliberately rejects changed expressions instead of guessing.
    Counts exclude JSON/role framing and do not describe the complete request.
    """
    path = Path(path)
    with path.open("rb") as source:
        raw = source.read(MAX_SOURCE_BYTES + 1)
    if len(raw) > MAX_SOURCE_BYTES:
        raise ValueError("Hive source exceeds the measurement budget.")
    try:
        text = raw.decode("utf-8", errors="strict")
    except UnicodeError as error:
        raise ValueError("Hive source must be UTF-8.") from error
    matches = list(_PROMPT_BLOCK.finditer(text))
    if len(matches) != 1:
        raise ValueError("Expected exactly one known B3AiClient system prompt block.")
    components = {name: _decode_literals(matches[0].group(name))
                  for name in ("base", "enabled", "disabled", "begin", "end")}
    base = components["base"].encode("utf-8")
    disabled = (components["base"] + components["disabled"]
                + components["begin"] + components["end"]).encode("utf-8")
    return {
        "source_sha256": hashlib.sha256(raw).hexdigest(),
        "base_bytes": len(base),
        "tools_disabled_bytes": len(disabled),
        "base_sha256": hashlib.sha256(base).hexdigest(),
        "tools_disabled_sha256": hashlib.sha256(disabled).hexdigest(),
        "scope": "system text with tools disabled and empty lesson; excludes user, history, roles, and JSON framing",
        "tokenizer_version": TOKENIZER_VERSION,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path,
                        help="new golden NPZ path; existing paths are refused")
    args = parser.parse_args(argv)
    try:
        report = write_reference(args.output)
    except (OSError, ValueError, FloatingPointError) as error:
        parser.exit(2, f"Reference generation failed: {error}\n")
    print(json.dumps(report, sort_keys=True))


if __name__ == "__main__":
    main()
