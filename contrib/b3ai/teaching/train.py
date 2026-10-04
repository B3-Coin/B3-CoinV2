# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Teach reviewed B3 answers using inherited language weights and fresh AdamW."""

import argparse
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess
import time
import uuid

import numpy as np
import torch

from .data import Curriculum
from ..language import train as language_train
from ..language.checkpoint import MAX_ARCHIVE_BYTES, MAX_STEPS, load_checkpoint, save_checkpoint


STAGE = "b3-supervised-answers"
MAX_SECONDS = 600
MAX_TOKENS = 2_048_000
MAX_MEMORY = 32 * 1024**3


class ResourceLimitError(RuntimeError):
    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


def _file_hash(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def implementation_hash():
    digest = hashlib.sha256()
    for name in ("data.py", "train.py"):
        digest.update(name.encode("ascii") + b"\0")
        digest.update(Path(__file__).with_name(name).read_bytes())
    digest.update(json.dumps(language_train.component_hashes(), sort_keys=True).encode("ascii"))
    return digest.hexdigest()


def _parent(path, expected_sha256, versions):
    if type(expected_sha256) is not str or re.fullmatch(r"[0-9a-f]{64}", expected_sha256) is None:
        raise ValueError("A complete expected parent SHA-256 is required.")
    path = Path(path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_ARCHIVE_BYTES:
        raise ValueError("Parent must be a bounded regular checkpoint file.")
    if _file_hash(path) != expected_sha256:
        raise ValueError("Parent checkpoint SHA-256 does not match the approved parent.")
    model, inherited_optimizer, metadata = load_checkpoint(path, device="cpu")
    if _file_hash(path) != expected_sha256:
        raise ValueError("Parent checkpoint changed while it was loaded.")
    if (metadata.get("stage") != "language-pilot" or metadata.get("experimental") is not True
            or metadata.get("model_family_origin") != language_train.MODEL_FAMILY_ORIGIN
            or metadata.get("component_hashes") != language_train.component_hashes()
            or metadata.get("implementation_hash") != language_train.implementation_hash()):
        raise ValueError("Parent is not a compatible own-language checkpoint with current provenance.")
    previous_runtime = metadata.get("runtime")
    if (type(previous_runtime) is not dict
            or previous_runtime.get("device") not in ("cpu", "mps")
            or any(previous_runtime.get(key) != versions[key] for key in ("python", "numpy", "torch", "precision"))):
        raise ValueError("Parent runtime precision or dependency versions are incompatible.")
    inherited_steps, inherited_tokens = metadata.get("selected_or_saved_step"), metadata.get("training_tokens_at_save")
    if (type(inherited_steps) is not int or not 0 <= inherited_steps <= MAX_STEPS
            or type(inherited_tokens) is not int or not 0 <= inherited_tokens <= inherited_steps * 16384):
        raise ValueError("Parent language update or token count is invalid.")
    states = inherited_optimizer.state
    if ((inherited_steps == 0 and states)
            or (inherited_steps > 0 and (set(states) != set(model.params.values())
                or any(state["step"].item() != inherited_steps for state in states.values())))):
        raise ValueError("Parent language optimizer steps do not match its saved update.")
    if "run_local_step" in metadata:
        local, base = metadata["run_local_step"], metadata.get("base_step")
        local_tokens, base_tokens = metadata.get("run_local_training_tokens_at_save"), metadata.get("base_training_tokens")
        if (type(local) is not int or type(base) is not int or min(local, base) < 0 or local + base != inherited_steps
                or type(local_tokens) is not int or type(base_tokens) is not int or min(local_tokens, base_tokens) < 0
                or local_tokens + base_tokens != inherited_tokens
                or metadata.get("cumulative_training_tokens_at_save") != inherited_tokens):
            raise ValueError("Parent language local and cumulative counts are inconsistent.")
    # This objective does not inherit either the language optimizer or its RNG.
    del inherited_optimizer
    return model, metadata, inherited_steps, inherited_tokens


def _limits(steps, batch_size, learning_rate, evaluation_interval, max_seconds, max_tokens):
    for name, value, high in (("steps", steps, 1000), ("batch_size", batch_size, 16),
                              ("evaluation_interval", evaluation_interval, 1000),
                              ("max_seconds", max_seconds, MAX_SECONDS), ("max_tokens", max_tokens, MAX_TOKENS)):
        if type(value) is not int or not 1 <= value <= high:
            raise ValueError(f"{name} must be an integer from 1 through {high}.")
    if type(learning_rate) not in (int, float) or not math.isfinite(learning_rate) or not 0 < learning_rate <= 0.001:
        raise ValueError("Teaching learning rate must be finite and in (0, 0.001].")


def score(model, curriculum, split, batch_size, device, *, check_resources=None):
    """Token-weighted loss over every complete reviewed answer in the split."""
    total, denominator = 0.0, 0
    with torch.no_grad():
        for batch in curriculum.evaluation_batches(split, batch_size):
            if check_resources is not None:
                check_resources()
            weight = int(batch[2].sum())
            tensors = tuple(torch.from_numpy(array).to(device) for array in batch)
            value = model.loss(*tensors).item()
            if not math.isfinite(value) or value < 0 or weight <= 0:
                raise ValueError("Teaching evaluation produced invalid loss or target counts.")
            total += value * weight
            denominator += weight
            if check_resources is not None:
                check_resources()
    if denominator <= 0:
        raise ValueError("Teaching evaluation has no answer targets.")
    return total / denominator


def _report(path, report):
    temporary = path / ".report.json.tmp"
    temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path / "report.json")


def fit(curriculum_path, output, *, initialize_from, expected_parent_sha256, device="mps",
        steps=1000, batch_size=8, learning_rate=0.0001, evaluation_interval=100,
        max_seconds=MAX_SECONDS, max_tokens=MAX_TOKENS):
    """Start a distinct teaching objective; never open an exam or overwrite a run."""
    _limits(steps, batch_size, learning_rate, evaluation_interval, max_seconds, max_tokens)
    versions = language_train.runtime(device)
    output = Path(output)
    if output.exists() or output.is_symlink():
        raise FileExistsError("Teaching output exists; a fresh exclusive directory is required.")
    model, parent, inherited_steps, inherited_tokens = _parent(initialize_from, expected_parent_sha256, versions)
    curriculum = Curriculum(curriculum_path, context_length=min(512, model.config.context_length))
    model.to(device)
    optimizer = torch.optim.AdamW(list(model.params.values()), lr=learning_rate,
                                 weight_decay=0.01, foreach=False, fused=False)
    rng = np.random.default_rng(model.config.seed)
    fresh_rng = copy.deepcopy(rng.bit_generator.state)
    started = time.monotonic()
    output.mkdir(parents=True, exist_ok=False)
    code_hash, components = implementation_hash(), language_train.component_hashes()
    report = {"stage": STAGE, "status": "running", "experimental": True,
              "objective": "answer-only whole B3 turns, including final newline and end-of-document",
              "initialization": "inherit only own language weights; fresh AdamW and fresh batch RNG",
              "optimizer_reset": True, "batch_rng_reset": True,
              "inherited_language_steps": inherited_steps, "inherited_language_training_tokens": inherited_tokens,
              "model_family_origin": language_train.MODEL_FAMILY_ORIGIN,
              "parent_checkpoint_sha256": expected_parent_sha256,
              "parent_implementation_hash": parent["implementation_hash"],
              "runtime": versions, "config": model.config.to_dict(), "parameter_count": model.parameter_count,
              "tokenizer": "utf8-bytes-eod-v1", "run_id": str(uuid.uuid4()),
              "implementation_hash": code_hash, "component_hashes": components,
              "curriculum_sha256": curriculum.sha256, "baseline_commit": curriculum.baseline_commit,
              "source_hashes": curriculum.source_hashes, "source_review": curriculum.review,
              "record_counts": {split: len(records) for split, records in curriculum.records.items()},
              "training": {"batch_size": batch_size, "learning_rate": learning_rate, "weight_decay": 0.01,
                           "clip_norm": 1.0, "requested_steps": steps, "max_seconds": max_seconds,
                           "max_training_tokens": max_tokens, "max_memory_bytes": MAX_MEMORY,
                           "evaluation_interval": evaluation_interval, "context_length_cap": curriculum.context_length},
              "independent_exam_used": False, "history": [], "attempted_steps": 0,
              "selected_step": 0, "training_tokens": 0,
              "limitations": "Teacher-provided targets are source-reviewed, not independently verified by the model. Loss is not factual reliability. Resource caps are checked between complete operations."}
    peak = language_train.memory(device)

    def observe():
        for key, value in language_train.memory(device).items():
            peak[key] = max(peak[key], value)

    def resource_stop():
        observe()
        if max(peak.values()) >= MAX_MEMORY:
            return "memory_limit"
        if time.monotonic() - started >= max_seconds:
            return "wall_limit"
        return None

    def guard():
        reason = resource_stop()
        if reason is not None:
            raise ResourceLimitError(reason)

    def snapshot(name, step, validation):
        observe()
        if max(peak.values()) >= MAX_MEMORY:
            raise ResourceLimitError("memory_limit")
        metadata = {"stage": STAGE, "experimental": True, "objective": report["objective"],
                    "run_id": report["run_id"], "implementation_hash": code_hash,
                    "component_hashes": components, "model_family_origin": language_train.MODEL_FAMILY_ORIGIN,
                    "parent_checkpoint_sha256": expected_parent_sha256,
                    "parent_implementation_hash": parent["implementation_hash"], "optimizer_reset": True,
                    "batch_rng_reset": True, "inherited_language_steps": inherited_steps,
                    "inherited_language_training_tokens": inherited_tokens,
                    "runtime": versions, "selected_or_saved_step": step,
                    "training_tokens_at_save": report["training_tokens"],
                    "validation_loss_at_save": validation, "batch_rng_state": copy.deepcopy(rng.bit_generator.state),
                    "initial_batch_rng_state": fresh_rng, "curriculum_sha256": curriculum.sha256,
                    "baseline_commit": curriculum.baseline_commit, "source_hashes": curriculum.source_hashes,
                    "source_review": curriculum.review, "training": report["training"], "limits": report["limitations"]}
        save_checkpoint(output / name, model, optimizer, metadata)
        observe()

    def evaluate(step):
        guard()
        values = {split: score(model, curriculum, split, batch_size, device, check_resources=guard)
                  for split in ("train", "validation")}
        observe()
        report["history"].append({"step": step, **values, "elapsed_seconds": time.monotonic() - started})
        return values

    def failure(problem):
        observe()
        selected = output / "model.npz"
        report.update({"status": "failed teaching experiment; last valid selected checkpoint preserved",
                       "stop_reason": "failure", "failure": str(problem)[:500],
                       "elapsed_seconds": time.monotonic() - started, "peak_memory": peak,
                       "best_validation_loss": report.get("best_validation_loss"),
                       "selected_checkpoint_available": selected.exists(),
                       "selected_checkpoint_sha256": _file_hash(selected) if selected.exists() else None,
                       "last_checkpoint_sha256": None, "last_state_validation_available": False})
        _report(output, report)

    try:
        initial = evaluate(0)
        report["baseline"] = initial
        best = initial["validation"]
        snapshot("model.npz", 0, best)
    except ResourceLimitError as error:
        report.update({"status": "stopped before a complete baseline checkpoint; training not started",
                       "stop_reason": error.reason, "elapsed_seconds": time.monotonic() - started,
                       "peak_memory": peak, "best_validation_loss": None,
                       "selected_checkpoint_sha256": None, "last_checkpoint_sha256": None,
                       "selected_checkpoint_available": False, "last_state_validation_available": False,
                       "parent_unchanged": _file_hash(initialize_from) == expected_parent_sha256})
        _report(output, report)
        return report
    except (ValueError, RuntimeError) as error:
        failure(error)
        raise
    report["best_validation_loss"] = best
    _report(output, report)
    stop = "update_limit"
    for step in range(1, steps + 1):
        stop_now = resource_stop()
        if stop_now is not None:
            stop = stop_now
            break
        old_rng = copy.deepcopy(rng.bit_generator.state)
        batch = curriculum.batch("train", rng, batch_size)
        count = int(batch[2].sum())
        if report["training_tokens"] + count > max_tokens:
            rng.bit_generator.state = old_rng
            stop = "token_limit"
            break
        try:
            optimizer.zero_grad(set_to_none=True)
            loss = model.loss(*(torch.from_numpy(array).to(device) for array in batch))
            if not bool(torch.isfinite(loss).item()):
                raise ValueError("Teaching loss is non-finite; selected checkpoint remains unchanged.")
            loss.backward()
            torch.nn.utils.clip_grad_norm_(list(model.params.values()), 1.0,
                                           foreach=False, error_if_nonfinite=True)
            optimizer.step()
        except (ValueError, RuntimeError) as error:
            failure(error)
            raise
        language_train.synchronize(device)
        report["attempted_steps"] = step
        report["training_tokens"] += count
        stop_now = resource_stop()
        if stop_now is not None:
            stop = stop_now
            break
        if step % evaluation_interval == 0 or step == steps:
            try:
                values = evaluate(step)
                if values["validation"] < best:
                    snapshot("model.npz", step, values["validation"])
                    best, report["selected_step"] = values["validation"], step
            except ResourceLimitError as error:
                stop = error.reason
                break
            except (ValueError, RuntimeError) as error:
                failure(error)
                raise
            report["best_validation_loss"], report["peak_memory"] = best, peak
            _report(output, report)
            print(json.dumps({"stage": STAGE, "step": step, "training_tokens": report["training_tokens"],
                              **values, "selected_step": report["selected_step"],
                              "elapsed_seconds": time.monotonic() - started}), flush=True)
    attempted = report["attempted_steps"]
    stop_now = resource_stop()
    if stop_now is not None:
        stop = stop_now
    if report["history"][-1]["step"] != attempted and stop not in ("memory_limit", "wall_limit"):
        try:
            values = evaluate(attempted)
            if values["validation"] < best:
                snapshot("model.npz", attempted, values["validation"])
                best, report["selected_step"] = values["validation"], attempted
        except ResourceLimitError as error:
            stop = error.reason
        except (ValueError, RuntimeError) as error:
            failure(error)
            raise
    current_validation = report["history"][-1]["validation"] if report["history"][-1]["step"] == attempted else None
    stop_now = resource_stop()
    if stop_now is not None:
        stop = stop_now
    report["best_validation_loss"] = best
    try:
        if stop != "memory_limit":
            try:
                snapshot("last.npz", attempted, current_validation)
            except ResourceLimitError as error:
                stop = error.reason
        language_train.synchronize(device)
    except (OSError, ValueError, RuntimeError) as error:
        failure(error)
        raise
    report.update({"status": "finished teaching experiment; factual answers not qualified", "stop_reason": stop,
                   "best_validation_loss": best, "elapsed_seconds": time.monotonic() - started,
                   "peak_memory": peak, "selected_checkpoint_available": True,
                   "last_state_validation_available": current_validation is not None and (output / "last.npz").exists(),
                   "selected_checkpoint_sha256": _file_hash(output / "model.npz"),
                   "last_checkpoint_sha256": _file_hash(output / "last.npz") if (output / "last.npz").exists() else None,
                   "parent_unchanged": _file_hash(initialize_from) == expected_parent_sha256})
    if not report["parent_unchanged"]:
        raise ValueError("The inherited parent checkpoint changed externally during teaching.")
    _report(output, report)
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--curriculum", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--initialize-from", type=Path, required=True)
    parser.add_argument("--expected-parent-sha256", required=True)
    parser.add_argument("--device", choices=("cpu", "mps"), default="mps")
    parser.add_argument("--steps", type=int, default=1000)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--learning-rate", type=float, default=0.0001)
    parser.add_argument("--evaluation-interval", type=int, default=100)
    parser.add_argument("--max-seconds", type=int, default=MAX_SECONDS)
    parser.add_argument("--max-tokens", type=int, default=MAX_TOKENS)
    args = parser.parse_args(argv)
    try:
        result = fit(args.curriculum, args.output, initialize_from=args.initialize_from,
                     expected_parent_sha256=args.expected_parent_sha256, device=args.device, steps=args.steps,
                     batch_size=args.batch_size, learning_rate=args.learning_rate,
                     evaluation_interval=args.evaluation_interval, max_seconds=args.max_seconds, max_tokens=args.max_tokens)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(2, f"B3 teaching failed: {error}\n")
    print(json.dumps(result, allow_nan=False), flush=True)


if __name__ == "__main__":
    main()
