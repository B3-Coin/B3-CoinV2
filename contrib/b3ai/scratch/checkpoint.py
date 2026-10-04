# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Bounded, pickle-free persistence for randomly initialized scratch models.

The JSON manifest and arrays are untrusted input. A valid manifest records
provenance rather than establishing dataset correctness or model capability.
"""

import json
import math
import os
from pathlib import Path
import re
import tempfile
import zipfile
import zlib

import numpy as np

from .tokenizer import VERSION as TOKENIZER_VERSION


FORMAT = "b3-scratch-transformer"
VERSION = 1
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_ARCHIVE_BYTES = 256 * 1024 * 1024
MAX_PARAMETERS = 8_000_000
MAX_HEADER_BYTES = 4096
_CONFIG_FIELDS = {"vocab_size", "context_length", "width", "layers", "heads", "ff_multiplier", "seed"}
_MANIFEST_FIELDS = {
    "format", "version", "initialization", "pretrained_weights", "tokenizer_version",
    "model_config", "model_dtype", "optimizer", "parameter_names", "metadata",
}


def _json_value(value, depth=0, budget=None):
    if budget is None:
        budget = [50_000]
    budget[0] -= 1
    if depth > 32 or budget[0] < 0:
        raise ValueError("Checkpoint metadata exceeds its structural limits.")
    if value is None or type(value) is bool:
        return
    if type(value) is int:
        if value.bit_length() > 256:
            raise ValueError("Checkpoint metadata contains an oversized integer.")
        return
    if type(value) is float:
        if not math.isfinite(value):
            raise ValueError("Checkpoint metadata contains a nonfinite number.")
        return
    if type(value) is str:
        if len(value) > 65_536:
            raise ValueError("Checkpoint metadata contains an oversized text field.")
        value.encode("utf-8", errors="strict")
        return
    if type(value) is list:
        if len(value) > 8192:
            raise ValueError("Checkpoint metadata contains an oversized list.")
        for item in value:
            _json_value(item, depth + 1, budget)
        return
    if type(value) is dict:
        if len(value) > 4096 or any(type(key) is not str for key in value):
            raise ValueError("Checkpoint metadata must have bounded string-keyed objects.")
        for key, item in value.items():
            _json_value(key, depth + 1, budget)
            _json_value(item, depth + 1, budget)
        return
    raise ValueError("Checkpoint metadata must contain only plain JSON values.")


def _config(raw):
    from .model import ModelConfig

    if type(raw) is not dict or set(raw) != _CONFIG_FIELDS:
        raise ValueError("Checkpoint model configuration has unexpected fields.")
    if any(type(value) is not int for value in raw.values()):
        raise ValueError("Checkpoint model configuration requires integer fields.")
    limits = {"vocab_size": (256, 256), "context_length": (1, 2048), "width": (4, 512),
              "layers": (1, 12), "heads": (1, 32), "ff_multiplier": (1, 8), "seed": (0, 2**63 - 1)}
    if any(not low <= raw[key] <= high for key, (low, high) in limits.items()):
        raise ValueError("Checkpoint configuration is outside the scratch-model bounds.")
    if raw["width"] % raw["heads"]:
        raise ValueError("Checkpoint width must be divisible by the number of attention heads.")
    width, vocab, context, layers, ff = (raw[key] for key in
                                        ("width", "vocab_size", "context_length", "layers", "ff_multiplier"))
    parameters = 2 * vocab * width + context * width + layers * (
        4 * width * width + 2 * width * width * ff + 9 * width + width * ff
    ) + 2 * width + vocab
    if parameters > MAX_PARAMETERS:
        raise ValueError("Checkpoint model exceeds the parameter-count limit.")
    return ModelConfig(**raw)


def _optimizer_spec(raw):
    if type(raw) is not dict or set(raw) != {"steps", "beta1", "beta2", "eps"}:
        raise ValueError("Checkpoint optimizer specification is incomplete.")
    if type(raw["steps"]) is not int or not 0 <= raw["steps"] <= 2**63 - 1:
        raise ValueError("Checkpoint optimizer step count is invalid.")
    for key in ("beta1", "beta2", "eps"):
        if type(raw[key]) not in (int, float) or not math.isfinite(raw[key]):
            raise ValueError("Checkpoint optimizer settings must be finite numbers.")
    if not (0 <= raw["beta1"] < 1 and 0 <= raw["beta2"] < 1 and raw["eps"] > 0):
        raise ValueError("Checkpoint optimizer settings are invalid.")
    return raw


def _no_duplicate_keys(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Checkpoint JSON contains duplicate keys.")
        result[key] = value
    return result


def _reject_constant(_):
    raise ValueError("Nonfinite JSON is forbidden.")


def _npy(archive, member, expected_shape=None, expected_dtype=None, manifest=False):
    """Inspect the NPY header before allocating or accepting its array body."""
    with archive.open(member, "r") as stream:
        version = np.lib.format.read_magic(stream)
        if version == (1, 0):
            shape, fortran, dtype = np.lib.format.read_array_header_1_0(stream, max_header_size=MAX_HEADER_BYTES)
        elif version == (2, 0):
            shape, fortran, dtype = np.lib.format.read_array_header_2_0(stream, max_header_size=MAX_HEADER_BYTES)
        else:
            raise ValueError("Checkpoint NPY version is unsupported.")
        if manifest:
            if dtype != np.dtype(np.uint8) or len(shape) != 1 or not 0 < shape[0] <= MAX_MANIFEST_BYTES:
                raise ValueError("Checkpoint manifest must be a bounded UTF-8 byte array.")
        elif shape != expected_shape or dtype != expected_dtype:
            raise ValueError(f"Checkpoint array {member} has an incompatible shape or dtype.")
        if dtype.hasobject:
            raise ValueError("Object arrays and pickle are forbidden in checkpoints.")
        count = math.prod(shape)
        size = count * dtype.itemsize
        if size > MAX_ARCHIVE_BYTES:
            raise ValueError("Checkpoint array exceeds its byte limit.")
        body = stream.read(size + 1)
        if len(body) != size:
            raise ValueError("Checkpoint array is truncated or contains trailing data.")
        array = np.frombuffer(body, dtype=dtype).reshape(shape, order="F" if fortran else "C").copy()
        if not manifest and not np.isfinite(array).all():
            raise ValueError(f"Checkpoint array {member} contains nonfinite values.")
        return array


def _manifest(archive):
    names = archive.namelist()
    if len(names) != len(set(names)) or len(names) > 1024 or "manifest.npy" not in names:
        raise ValueError("Checkpoint ZIP members are missing, duplicated or excessive.")
    entries = archive.infolist()
    if any(entry.is_dir() or entry.flag_bits & 1 for entry in entries):
        raise ValueError("Checkpoint ZIP directories and encrypted entries are forbidden.")
    if any(entry.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED) for entry in entries):
        raise ValueError("Checkpoint ZIP compression method is unsupported.")
    if sum(entry.file_size for entry in entries) > MAX_ARCHIVE_BYTES:
        raise ValueError("Checkpoint uncompressed archive exceeds its byte limit.")
    if archive.getinfo("manifest.npy").file_size > MAX_MANIFEST_BYTES + MAX_HEADER_BYTES:
        raise ValueError("Checkpoint manifest exceeds its byte limit.")
    raw = _npy(archive, "manifest.npy", manifest=True).tobytes().decode("utf-8", errors="strict")
    value = json.loads(raw, object_pairs_hook=_no_duplicate_keys, parse_constant=_reject_constant)
    _json_value(value)
    if type(value) is not dict or set(value) != _MANIFEST_FIELDS:
        raise ValueError("Checkpoint manifest schema is unsupported.")
    if value["format"] != FORMAT or type(value["version"]) is not int or value["version"] != VERSION:
        raise ValueError("Checkpoint format or version is unsupported.")
    if value["initialization"] != "random" or value["pretrained_weights"] is not False:
        raise ValueError("Checkpoint provenance is incompatible with random-start training.")
    if value["tokenizer_version"] != TOKENIZER_VERSION or value["model_dtype"] not in ("float32", "float64"):
        raise ValueError("Checkpoint tokenizer or model dtype is incompatible.")
    if type(value["metadata"]) is not dict:
        raise ValueError("Checkpoint metadata must be an object.")
    parameter_names = value["parameter_names"]
    if type(parameter_names) is not list or not parameter_names or any(
        type(name) is not str or re.fullmatch(r"[A-Za-z0-9_.]+", name) is None for name in parameter_names
    ) or parameter_names != sorted(set(parameter_names)):
        raise ValueError("Checkpoint parameter names are invalid or duplicated.")
    expected = {"manifest.npy"} | {f"{prefix}__{name}.npy" for name in parameter_names for prefix in ("p", "m", "v")}
    if set(names) != expected:
        raise ValueError("Checkpoint has unexpected or incomplete named arrays.")
    _optimizer_spec(value["optimizer"])
    _config(value["model_config"])
    return value


def load_checkpoint(path):
    """Load validated weights and AdamW state without evaluating stored code."""
    from .model import AdamW, Transformer

    path = Path(path)
    try:
        if path.stat().st_size > MAX_ARCHIVE_BYTES:
            raise ValueError("Checkpoint file exceeds its byte limit.")
        with zipfile.ZipFile(path, "r") as archive:
            manifest = _manifest(archive)
            config = _config(manifest["model_config"])
            dtype = np.dtype(manifest["model_dtype"])
            model = Transformer(config, dtype=dtype)
            names = sorted(model.params)
            if manifest["parameter_names"] != names:
                raise ValueError("Checkpoint parameters do not match the model architecture.")
            spec = manifest["optimizer"]
            optimizer = AdamW(model.params, beta1=spec["beta1"], beta2=spec["beta2"], eps=spec["eps"])
            for name in names:
                shape = model.params[name].shape
                for prefix, target in (("p", model.params), ("m", optimizer.m), ("v", optimizer.v)):
                    member = f"{prefix}__{name}.npy"
                    if archive.getinfo(member).file_size > math.prod(shape) * dtype.itemsize + MAX_HEADER_BYTES:
                        raise ValueError("Checkpoint member exceeds its expected size.")
                    array = _npy(archive, member, expected_shape=shape, expected_dtype=dtype)
                    if prefix == "v" and np.any(array < 0):
                        raise ValueError("Checkpoint optimizer variances cannot be negative.")
                    target[name][...] = array
            optimizer.steps = spec["steps"]
            return model, optimizer, manifest["metadata"]
    except (OSError, zipfile.BadZipFile, zlib.error, EOFError, KeyError, TypeError, UnicodeError,
            json.JSONDecodeError, RecursionError, OverflowError) as exc:
        raise ValueError("Checkpoint could not be read safely or is corrupt.") from exc


def save_checkpoint(path, model, optimizer, metadata: dict):
    """Atomically replace only a compatible checkpoint; refuse unknown existing files."""
    path = Path(path)
    config = _config(model.config.to_dict())
    if type(metadata) is not dict:
        raise ValueError("Checkpoint metadata must be an object.")
    _json_value(metadata)
    names = sorted(model.params)
    if not names or set(optimizer.m) != set(names) or set(optimizer.v) != set(names):
        raise ValueError("Checkpoint optimizer arrays are incomplete.")
    dtype = model.params[names[0]].dtype
    if dtype not in (np.dtype("float32"), np.dtype("float64")):
        raise ValueError("Checkpoint model requires float32 or float64 arrays.")
    from .model import Transformer
    expected = Transformer(config, dtype=dtype)
    if sorted(expected.params) != names:
        raise ValueError("Checkpoint parameters do not match the model architecture.")
    arrays = {}
    for name in names:
        for prefix, source in (("p", model.params), ("m", optimizer.m), ("v", optimizer.v)):
            array = source[name]
            if not isinstance(array, np.ndarray) or array.dtype != dtype or array.shape != expected.params[name].shape:
                raise ValueError("Checkpoint arrays have incompatible types, shapes or dtypes.")
            if not np.isfinite(array).all() or (prefix == "v" and np.any(array < 0)):
                raise ValueError("Checkpoint arrays contain invalid numeric values.")
            arrays[f"{prefix}__{name}"] = array
    optimizer_spec = _optimizer_spec({"steps": optimizer.steps, "beta1": optimizer.beta1,
                                      "beta2": optimizer.beta2, "eps": optimizer.eps})
    manifest = {"format": FORMAT, "version": VERSION, "initialization": "random", "pretrained_weights": False,
                "tokenizer_version": TOKENIZER_VERSION, "model_config": config.to_dict(), "model_dtype": dtype.name,
                "optimizer": optimizer_spec, "parameter_names": names, "metadata": metadata}
    # Apply the same complete-manifest depth/count limits used during loading.
    _json_value(manifest)
    try:
        encoded = json.dumps(manifest, sort_keys=True, ensure_ascii=False, allow_nan=False,
                             separators=(",", ":")).encode("utf-8")
    except (TypeError, ValueError, UnicodeError) as exc:
        raise ValueError("Checkpoint metadata is not safe JSON.") from exc
    if len(encoded) > MAX_MANIFEST_BYTES:
        raise ValueError("Checkpoint manifest exceeds its byte limit.")
    arrays["manifest"] = np.frombuffer(encoded, dtype=np.uint8)
    if sum(array.nbytes + MAX_HEADER_BYTES for array in arrays.values()) > MAX_ARCHIVE_BYTES:
        raise ValueError("Checkpoint arrays exceed the archive byte limit.")
    if path.exists():
        existing_model, _, _ = load_checkpoint(path)
        if existing_model.config.to_dict() != config.to_dict() or existing_model.params[names[0]].dtype != dtype:
            raise ValueError("Refusing to overwrite an incompatible checkpoint.")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w+b", prefix=f".{path.name}.", suffix=".tmp", dir=path.parent,
                                         delete=False) as stream:
            temporary = Path(stream.name)
            np.savez(stream, **arrays)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
        # Directory durability is best effort on filesystems without directory fsync.
        try:
            descriptor = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
        except OSError:
            pass
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
