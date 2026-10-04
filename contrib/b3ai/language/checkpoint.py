# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Bounded, pickle-free checkpoints for the separate GPU language experiment.

Every archive and array is validated on the CPU before allocating device state.
Metadata is a caller's provenance record, not evidence of model capability.
The original scratch checkpoint format is deliberately incompatible.
"""

import json
import math
import os
from pathlib import Path
import tempfile
import zipfile
import zlib

import numpy as np

from ..scratch.checkpoint import _json_value, _no_duplicate_keys, _npy, _reject_constant


FORMAT = "b3-language-transformer"
VERSION = 1
TOKENIZER_VERSION = "utf8-bytes-eod-v1"
MAX_ARCHIVE_BYTES = 256 * 1024 * 1024
MAX_MANIFEST_BYTES = 1024 * 1024
MAX_HEADER_BYTES = 4096
# AdamW's non-capturable float32 step counter is exact through this value.
MAX_STEPS = 2**24
_CONFIG_FIELDS = {"vocab_size", "context_length", "width", "layers", "heads", "ff_multiplier", "seed"}
_MANIFEST_FIELDS = {
    "format", "version", "initialization", "pretrained_weights", "tokenizer_version",
    "model_config", "model_dtype", "parameter_names", "optimizer", "metadata",
}
_OPTIMIZER_FIELDS = {"type", "lr", "betas", "eps", "weight_decay", "steps", "initialized"}


def _config(raw):
    from .model import GpuConfig

    if type(raw) is not dict or set(raw) != _CONFIG_FIELDS:
        raise ValueError("Checkpoint model configuration has unexpected fields.")
    if any(type(value) is not int for value in raw.values()):
        raise ValueError("Checkpoint configuration requires integer fields.")
    if raw["vocab_size"] != 257:
        raise ValueError("Language checkpoints require the byte-plus-EOD vocabulary.")
    return GpuConfig(**raw)


def _number(raw, name, minimum=0, maximum=None, strict_minimum=False):
    if type(raw) not in (int, float) or not math.isfinite(raw):
        raise ValueError(f"Checkpoint optimizer {name} must be a finite number.")
    if raw < minimum or (strict_minimum and raw == minimum) or (maximum is not None and raw > maximum):
        raise ValueError(f"Checkpoint optimizer {name} is outside its bounds.")
    return float(raw)


def _optimizer_spec(raw, names):
    if type(raw) is not dict or set(raw) != _OPTIMIZER_FIELDS or raw["type"] != "torch.optim.AdamW":
        raise ValueError("Checkpoint optimizer schema is unsupported.")
    _number(raw["lr"], "lr")
    _number(raw["eps"], "eps", strict_minimum=True)
    _number(raw["weight_decay"], "weight_decay")
    if type(raw["betas"]) is not list or len(raw["betas"]) != 2:
        raise ValueError("Checkpoint AdamW betas must be a two-element list.")
    for beta in raw["betas"]:
        _number(beta, "beta")
        if beta >= 1:
            raise ValueError("Checkpoint AdamW betas must be below one.")
    if type(raw["steps"]) is not dict or set(raw["steps"]) != set(names):
        raise ValueError("Checkpoint optimizer steps do not match its parameters.")
    if any(type(step) is not int or not 0 <= step <= MAX_STEPS for step in raw["steps"].values()):
        raise ValueError("Checkpoint optimizer step counters are invalid.")
    initialized = raw["initialized"]
    if type(initialized) is not list or any(type(name) is not str for name in initialized):
        raise ValueError("Checkpoint optimizer initialized states are invalid.")
    if initialized != sorted(set(initialized)) or not set(initialized) <= set(names):
        raise ValueError("Checkpoint optimizer initialized states are duplicated or unknown.")
    if any(step != 0 for name, step in raw["steps"].items() if name not in initialized):
        raise ValueError("Uninitialized optimizer parameters cannot have steps.")
    return raw


def _manifest(archive):
    """Validate the ZIP envelope, JSON schema and architecture without device state."""
    from .model import parameter_shapes

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
    encoded_manifest = _npy(archive, "manifest.npy", manifest=True)
    if encoded_manifest.nbytes > MAX_MANIFEST_BYTES:
        raise ValueError("Checkpoint manifest exceeds its byte limit.")
    raw = encoded_manifest.tobytes().decode("utf-8", errors="strict")
    manifest = json.loads(raw, object_pairs_hook=_no_duplicate_keys, parse_constant=_reject_constant)
    _json_value(manifest)
    if type(manifest) is not dict or set(manifest) != _MANIFEST_FIELDS:
        raise ValueError("Checkpoint manifest schema is unsupported.")
    if manifest["format"] != FORMAT or type(manifest["version"]) is not int or manifest["version"] != VERSION:
        raise ValueError("Checkpoint format or version is unsupported.")
    if manifest["initialization"] != "random" or manifest["pretrained_weights"] is not False:
        raise ValueError("Checkpoint provenance is incompatible with own random-start training.")
    if manifest["tokenizer_version"] != TOKENIZER_VERSION or manifest["model_dtype"] != "float32":
        raise ValueError("Checkpoint tokenizer or model dtype is incompatible.")
    if type(manifest["metadata"]) is not dict:
        raise ValueError("Checkpoint metadata must be a plain JSON object.")
    config = _config(manifest["model_config"])
    shapes = parameter_shapes(config)
    parameter_names = manifest["parameter_names"]
    if type(parameter_names) is not list or parameter_names != sorted(shapes):
        raise ValueError("Checkpoint parameter names do not match its architecture.")
    expected = {"manifest.npy"} | {
        f"{prefix}__{name}.npy" for name in parameter_names for prefix in ("p", "m", "v")
    }
    if set(names) != expected:
        raise ValueError("Checkpoint has unexpected or incomplete named arrays.")
    _optimizer_spec(manifest["optimizer"], parameter_names)
    expected_bytes = sum(math.prod(shape) * 4 * 3 for shape in shapes.values())
    if expected_bytes + len(raw.encode("utf-8")) > MAX_ARCHIVE_BYTES:
        raise ValueError("Checkpoint arrays exceed the archive byte limit.")
    return manifest, config, shapes


def _read_bundle(path):
    """Read only validated CPU NumPy arrays; no torch serialization is involved."""
    path = Path(path)
    try:
        if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_ARCHIVE_BYTES:
            raise ValueError("Checkpoint must be a bounded regular file without a symlink.")
        with zipfile.ZipFile(path, "r") as archive:
            manifest, config, shapes = _manifest(archive)
            arrays = {prefix: {} for prefix in ("p", "m", "v")}
            for name, shape in shapes.items():
                for prefix in arrays:
                    member = f"{prefix}__{name}.npy"
                    if archive.getinfo(member).file_size > math.prod(shape) * 4 + MAX_HEADER_BYTES:
                        raise ValueError("Checkpoint member exceeds its expected size.")
                    value = _npy(archive, member, expected_shape=shape, expected_dtype=np.dtype("float32"))
                    if prefix == "v" and np.any(value < 0):
                        raise ValueError("Checkpoint optimizer variances cannot be negative.")
                    if prefix != "p" and name not in manifest["optimizer"]["initialized"] and np.any(value != 0):
                        raise ValueError("Uninitialized optimizer moments must be zero.")
                    arrays[prefix][name] = value
            return manifest, config, arrays
    except (OSError, zipfile.BadZipFile, zlib.error, EOFError, KeyError, TypeError, UnicodeError,
            json.JSONDecodeError, RecursionError, OverflowError) as exc:
        raise ValueError("Checkpoint could not be read safely or is corrupt.") from exc


def load_checkpoint(path, device="cpu"):
    """Restore validated float32 weights and every AdamW state on a requested device."""
    manifest, config, arrays = _read_bundle(path)
    import torch
    from .model import TorchTransformer

    model = TorchTransformer(config).to(device)
    model.load_arrays(arrays["p"])
    spec = manifest["optimizer"]
    optimizer = torch.optim.AdamW(
        list(model.params.values()), lr=spec["lr"], betas=tuple(spec["betas"]),
        eps=spec["eps"], weight_decay=spec["weight_decay"], foreach=False, fused=False,
    )
    for name in spec["initialized"]:
        parameter = model.params[name]
        optimizer.state[parameter] = {
            "step": torch.tensor(float(spec["steps"][name]), dtype=torch.float32, device="cpu"),
            "exp_avg": torch.from_numpy(arrays["m"][name]).to(device=parameter.device),
            "exp_avg_sq": torch.from_numpy(arrays["v"][name]).to(device=parameter.device),
        }
    return model, optimizer, manifest["metadata"]


def _optimizer_arrays(model, optimizer, shapes):
    import torch

    if not isinstance(optimizer, torch.optim.AdamW) or len(optimizer.param_groups) != 1:
        raise ValueError("Checkpoint requires AdamW with one parameter group.")
    group = optimizer.param_groups[0]
    if len(group["params"]) != len(shapes) or {id(value) for value in group["params"]} != {
        id(value) for value in model.params.values()
    }:
        raise ValueError("Checkpoint optimizer must contain each model parameter exactly once.")
    for option in ("amsgrad", "maximize", "capturable", "differentiable", "foreach", "fused"):
        if group.get(option) is not False:
            raise ValueError(f"Checkpoint requires AdamW {option}=False.")
    if set(optimizer.state) - set(model.params.values()):
        raise ValueError("Checkpoint optimizer contains unknown state.")
    arrays = {}
    steps, initialized = {}, []
    for name in sorted(shapes):
        parameter = model.params[name]
        if not isinstance(parameter, torch.nn.Parameter) or parameter.dtype != torch.float32 or tuple(parameter.shape) != shapes[name]:
            raise ValueError("Checkpoint model parameter types, shapes or dtypes are incompatible.")
        arrays[f"p__{name}"] = parameter.detach().cpu().numpy().copy()
        state = optimizer.state.get(parameter, {})
        if not state:
            steps[name] = 0
            arrays[f"m__{name}"] = np.zeros(shapes[name], dtype=np.float32)
            arrays[f"v__{name}"] = np.zeros(shapes[name], dtype=np.float32)
            continue
        if set(state) != {"step", "exp_avg", "exp_avg_sq"}:
            raise ValueError("Checkpoint optimizer state schema is unsupported.")
        step = state["step"]
        if not isinstance(step, torch.Tensor) or step.dtype != torch.float32 or step.shape != torch.Size([]):
            raise ValueError("Checkpoint optimizer step must be a float32 scalar tensor.")
        value = step.detach().cpu().item()
        if not math.isfinite(value) or value != int(value) or not 0 <= value <= MAX_STEPS:
            raise ValueError("Checkpoint optimizer step counter is invalid.")
        steps[name] = int(value)
        initialized.append(name)
        for prefix, key in (("m", "exp_avg"), ("v", "exp_avg_sq")):
            tensor = state[key]
            if not isinstance(tensor, torch.Tensor) or tensor.dtype != torch.float32 or tuple(tensor.shape) != shapes[name]:
                raise ValueError("Checkpoint optimizer moments have incompatible types, shapes or dtypes.")
            arrays[f"{prefix}__{name}"] = tensor.detach().cpu().numpy().copy()
    spec = {"type": "torch.optim.AdamW", "lr": group["lr"], "betas": list(group["betas"]),
            "eps": group["eps"], "weight_decay": group["weight_decay"],
            "steps": steps, "initialized": initialized}
    _optimizer_spec(spec, sorted(shapes))
    for key, array in arrays.items():
        if not np.isfinite(array).all() or (key.startswith("v__") and np.any(array < 0)):
            raise ValueError("Checkpoint arrays contain invalid numeric values.")
    return arrays, spec


def save_checkpoint(path, model, optimizer, metadata):
    """Atomically save, replacing only a valid checkpoint of this configuration."""
    from .model import parameter_shapes

    path = Path(path)
    config = _config(model.config.to_dict())
    shapes = parameter_shapes(config)
    if sorted(model.params) != sorted(shapes):
        raise ValueError("Checkpoint model parameters do not match its configuration.")
    if type(metadata) is not dict:
        raise ValueError("Checkpoint metadata must be a plain JSON object.")
    _json_value(metadata)
    arrays, optimizer_spec = _optimizer_arrays(model, optimizer, shapes)
    manifest = {
        "format": FORMAT, "version": VERSION, "initialization": "random", "pretrained_weights": False,
        "tokenizer_version": TOKENIZER_VERSION, "model_config": config.to_dict(), "model_dtype": "float32",
        "parameter_names": sorted(shapes), "optimizer": optimizer_spec, "metadata": metadata,
    }
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
    previous = None
    if path.exists() or path.is_symlink():
        previous = path.stat(follow_symlinks=False)
        existing_manifest, existing_config, _ = _read_bundle(path)
        if existing_config.to_dict() != config.to_dict():
            raise ValueError("Refusing to overwrite an incompatible language checkpoint.")
        # A run identity, when supplied, cannot silently migrate into another run.
        if existing_manifest["metadata"].get("run_id") != metadata.get("run_id"):
            raise ValueError("Refusing to overwrite another training run's checkpoint.")
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w+b", prefix=f".{path.name}.", suffix=".tmp",
                                         dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            np.savez(stream, **arrays)
            stream.flush()
            os.fsync(stream.fileno())
        if temporary.stat().st_size > MAX_ARCHIVE_BYTES:
            raise ValueError("Checkpoint file exceeds its byte limit.")
        if previous is None:
            # Exclusive hard-link creation does not clobber a concurrently created file.
            os.link(temporary, path)
            temporary.unlink()
        else:
            current = path.stat(follow_symlinks=False)
            if (current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns) != (
                previous.st_dev, previous.st_ino, previous.st_size, previous.st_mtime_ns
            ):
                raise ValueError("Checkpoint changed while preparing its replacement.")
            os.replace(temporary, path)
        temporary = None
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
