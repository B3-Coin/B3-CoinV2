# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Bounded local language pilot from our own random weights; no tool execution."""

import argparse
import hashlib
import json
import math
from pathlib import Path
import platform
import resource
import time
import uuid

import numpy as np
import torch

from .batches import Corpus, TOKENIZER_VERSION
from .checkpoint import save_checkpoint
from .model import GpuConfig, TorchTransformer


MAX_SECONDS = 7200
MAX_TOKENS = 8192000
MAX_MEMORY = 32 * 1024**3


def implementation_hash():
    digest = hashlib.sha256()
    for name in ("model.py", "checkpoint.py", "batches.py", "train.py"):
        digest.update(name.encode("ascii") + b"\0")
        digest.update(Path(__file__).with_name(name).read_bytes())
    return digest.hexdigest()


def runtime(device):
    if device not in ("cpu", "mps") or (device == "mps" and not torch.backends.mps.is_available()):
        raise ValueError("Requested local device is unavailable; no silent fallback.")
    return {"python": platform.python_version(), "numpy": np.__version__,
            "torch": str(torch.__version__), "device": device, "precision": "float32"}


def synchronize(device):
    if device == "mps":
        torch.mps.synchronize()


def memory(device):
    rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    if platform.system() != "Darwin":
        rss *= 1024
    driver = torch.mps.driver_allocated_memory() if device == "mps" else 0
    # Unified-memory RSS and device allocations can overlap, so report both.
    return {"process_peak_rss_bytes": int(rss), "gpu_driver_allocated_bytes": int(driver)}


def _tensors(batch, device):
    return tuple(torch.from_numpy(value).to(device) for value in batch)


def score(model, batches, device):
    """Token-weighted CE on fixed batches, including complete short-doc targets."""
    total, count = 0.0, 0.0
    with torch.no_grad():
        for batch in batches:
            weight = float(batch[2].sum())
            loss = float(model.loss(*_tensors(batch, device)).cpu())
            if not math.isfinite(loss):
                raise ValueError("Non-finite evaluation loss.")
            total += loss * weight
            count += weight
    if count <= 0:
        raise ValueError("Empty evaluation targets.")
    return total / count


def _limits(config, steps, batch_size, evaluation_interval, validation_batches,
            max_seconds, max_tokens, learning_rate):
    if not isinstance(config, GpuConfig) or config.vocab_size != 257:
        raise ValueError("Language training requires the byte/EOD model configuration.")
    for name, value, low, high in (("steps", steps, 1, 1000), ("batch_size", batch_size, 1, 16),
            ("evaluation_interval", evaluation_interval, 1, 1000), ("validation_batches", validation_batches, 1, 16),
            ("max_seconds", max_seconds, 1, MAX_SECONDS), ("max_tokens", max_tokens, 1, MAX_TOKENS)):
        if type(value) is not int or not low <= value <= high:
            raise ValueError(f"{name} must be an integer between {low} and {high}.")
    if batch_size * config.context_length > 16384:
        raise ValueError("Batch exceeds the token bound.")
    if batch_size * config.heads * config.context_length**2 * config.layers > 1073741824:
        raise ValueError("Batch exceeds the attention bound.")
    if type(learning_rate) not in (int, float) or not math.isfinite(learning_rate) or not 0 < learning_rate <= 0.01:
        raise ValueError("Invalid bounded learning rate.")


def _report(output, report):
    temporary = output / ".report.json.tmp"
    temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(output / "report.json")


def fit(corpus_path, output, config, *, device="cpu", steps=1000, batch_size=4,
        evaluation_interval=100, validation_batches=4, max_seconds=MAX_SECONDS,
        max_tokens=MAX_TOKENS, learning_rate=0.0003):
    """Reserve a fresh run; select by validation, never inspect the test split."""
    _limits(config, steps, batch_size, evaluation_interval, validation_batches,
            max_seconds, max_tokens, learning_rate)
    versions = runtime(device)
    corpus = Corpus(corpus_path)
    if not all(corpus.splits[split] for split in ("train", "validation", "test")):
        raise ValueError("A pilot needs nonempty train, validation and untouched test splits.")
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    model = TorchTransformer(config).to(device)
    optimizer = torch.optim.AdamW(list(model.params.values()), lr=learning_rate,
                                 weight_decay=0.01, foreach=False, fused=False)
    rng = np.random.default_rng(config.seed)
    train_rng, validation_rng = np.random.default_rng(1234), np.random.default_rng(1729)
    probes = {split: [corpus.batch(split, generator, batch_size, config.context_length)
                     for _ in range(validation_batches)]
              for split, generator in (("train", train_rng), ("validation", validation_rng))}
    report = {"stage": "language-pilot", "status": "running", "experimental": True,
              "initialization": "our own random weights; no pretrained weights",
              "runtime": versions, "config": config.to_dict(), "parameter_count": model.parameter_count,
              "tokenizer": TOKENIZER_VERSION, "run_id": str(uuid.uuid4()),
              "implementation_hash": implementation_hash(), "corpus_manifest_hash": corpus.manifest_hash,
              "split_hashes": {key: value["sha256"] for key, value in corpus.manifest["splits"].items()},
              "training": {"batch_size": batch_size, "learning_rate": learning_rate, "weight_decay": 0.01,
                           "clip_norm": 1.0, "requested_steps": steps, "max_seconds": max_seconds,
                           "max_training_tokens": max_tokens, "max_memory_bytes": MAX_MEMORY,
                           "validation_batches": validation_batches, "evaluation_interval": evaluation_interval},
              "test_split_used_for_selection": False,
              "limitations": "Fixed-window validation is a selection metric, not language or assistant qualification. Resource caps are checked between complete operations.",
              "history": [], "attempted_steps": 0, "selected_step": 0, "training_tokens": 0}
    peak = memory(device)

    def observe_memory():
        peak.update({key: max(peak[key], value) for key, value in memory(device).items()})

    def snapshot(path, step, validation):
        metadata = {"stage": "language-pilot", "experimental": True, "run_id": report["run_id"],
                    "implementation_hash": report["implementation_hash"], "corpus_manifest_hash": corpus.manifest_hash,
                    "runtime": versions, "selected_or_saved_step": step, "validation_loss_at_save": validation,
                    "training_tokens_at_save": report["training_tokens"], "batch_rng_state": rng.bit_generator.state,
                    "training": report["training"], "limits": report["limitations"]}
        save_checkpoint(path, model, optimizer, metadata)
        observe_memory()

    def evaluate(step):
        values = {split: score(model, batches, device) for split, batches in probes.items()}
        observe_memory()
        report["history"].append({"step": step, **values, "elapsed_seconds": time.monotonic() - started})
        return values

    initial = evaluate(0)
    report["baseline"] = initial
    best = initial["validation"]
    snapshot(output / "model.npz", 0, best)
    stop, last_validation = "update_limit", best
    _report(output, report)
    for step in range(1, steps + 1):
        if time.monotonic() - started >= max_seconds:
            stop = "wall_limit"
            break
        measured = memory(device)
        peak.update({key: max(peak[key], value) for key, value in measured.items()})
        if max(measured.values()) >= MAX_MEMORY:
            stop = "memory_limit"
            break
        old_rng_state = rng.bit_generator.state
        batch = corpus.batch("train", rng, batch_size, config.context_length)
        token_count = int(batch[2].sum())
        if report["training_tokens"] + token_count > max_tokens:
            rng.bit_generator.state = old_rng_state
            stop = "token_limit"
            break
        optimizer.zero_grad(set_to_none=True)
        loss = model.loss(*_tensors(batch, device))
        if not bool(torch.isfinite(loss).cpu()):
            raise ValueError("Non-finite training loss; checkpoint remains at last valid selection.")
        loss.backward()
        torch.nn.utils.clip_grad_norm_(list(model.params.values()), 1.0, foreach=False, error_if_nonfinite=True)
        optimizer.step()
        synchronize(device)
        report["attempted_steps"] = step
        report["training_tokens"] += token_count
        observe_memory()
        if max(peak.values()) >= MAX_MEMORY:
            stop = "memory_limit"
            break
        if time.monotonic() - started >= max_seconds:
            stop = "wall_limit"
            break
        if step % evaluation_interval == 0 or step == steps:
            values = evaluate(step)
            last_validation = values["validation"]
            if last_validation < best:
                best, report["selected_step"] = last_validation, step
                snapshot(output / "model.npz", step, best)
            report["best_validation_loss"] = best
            report["peak_memory"] = peak
            _report(output, report)
            print(json.dumps({"step": step, "training_tokens": report["training_tokens"],
                              "train": values["train"], "validation": last_validation,
                              "best_step": report["selected_step"], "elapsed_seconds": time.monotonic() - started}), flush=True)
    attempted = report["attempted_steps"]
    if report["history"][-1]["step"] != attempted and stop not in ("wall_limit", "memory_limit"):
        values = evaluate(attempted)
        last_validation = values["validation"]
        if last_validation < best:
            best, report["selected_step"] = last_validation, attempted
            snapshot(output / "model.npz", attempted, best)
    # Preserve the last state when feasible; stale validation is never attached
    # to a newer unscored state after a resource limit stops further evaluation.
    if stop != "memory_limit":
        validation_at_last = last_validation if report["history"][-1]["step"] == attempted else None
        snapshot(output / "last.npz", attempted, validation_at_last)
    synchronize(device)
    elapsed = time.monotonic() - started
    report.update({"status": "finished experiment; language quality not assessed", "stop_reason": stop,
                   "best_validation_loss": best, "elapsed_seconds": elapsed, "peak_memory": peak,
                   "effective_training_tokens_per_second": report["training_tokens"] / elapsed,
                   "selected_checkpoint_sha256": hashlib.sha256((output / "model.npz").read_bytes()).hexdigest(),
                   "last_checkpoint_sha256": hashlib.sha256((output / "last.npz").read_bytes()).hexdigest() if (output / "last.npz").exists() else None})
    _report(output, report)
    return report


def benchmark(corpus_path, output, config, *, device="mps", batch_size=4, updates=3):
    _limits(config, updates, batch_size, 1, 1, MAX_SECONDS, MAX_TOKENS, 0.0003)
    versions = runtime(device)
    corpus = Corpus(corpus_path)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    model = TorchTransformer(config).to(device)
    optimizer = torch.optim.AdamW(list(model.params.values()), lr=0.0003, weight_decay=0.01, foreach=False, fused=False)
    rng = np.random.default_rng(42)
    durations, supervised, peak = [], 0, memory(device)
    benchmark_started = time.monotonic()
    for index in range(updates + 1):
        if time.monotonic() - benchmark_started >= MAX_SECONDS:
            raise ValueError("Benchmark reached its wall limit; do not start long training.")
        batch = corpus.batch("train", rng, batch_size, config.context_length)
        synchronize(device)
        start = time.monotonic()
        optimizer.zero_grad(set_to_none=True)
        loss = model.loss(*_tensors(batch, device))
        loss.backward()
        torch.nn.utils.clip_grad_norm_(list(model.params.values()), 1.0, foreach=False, error_if_nonfinite=True)
        optimizer.step()
        synchronize(device)
        elapsed = time.monotonic() - start
        peak = {key: max(peak[key], value) for key, value in memory(device).items()}
        if max(peak.values()) >= MAX_MEMORY:
            raise ValueError("Benchmark exceeded memory cap; do not start long training.")
        if index:
            durations.append(elapsed)
            supervised += int(batch[2].sum())
    result = {"mode": "benchmark only; random model discarded", "runtime": versions,
              "implementation_hash": implementation_hash(), "config": config.to_dict(),
              "parameter_count": model.parameter_count, "batch_size": batch_size, "warmup_updates": 1,
              "measured_updates": updates, "update_seconds": durations,
              "mean_update_seconds": sum(durations) / len(durations),
              "effective_supervised_tokens_per_second": supervised / sum(durations), "peak_memory": peak}
    _report(output, result)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device", choices=("cpu", "mps"), default="mps")
    parser.add_argument("--context-length", type=int, default=2048)
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--layers", type=int, default=6)
    parser.add_argument("--heads", type=int, default=8)
    parser.add_argument("--batch-size", type=int, default=4)
    parser.add_argument("--steps", type=int, default=1000)
    parser.add_argument("--max-seconds", type=int, default=MAX_SECONDS)
    parser.add_argument("--max-tokens", type=int, default=MAX_TOKENS)
    parser.add_argument("--evaluation-interval", type=int, default=100)
    parser.add_argument("--benchmark", action="store_true")
    args = parser.parse_args(argv)
    try:
        config = GpuConfig(context_length=args.context_length, width=args.width, layers=args.layers, heads=args.heads)
        if args.benchmark:
            result = benchmark(args.corpus, args.output, config, device=args.device, batch_size=args.batch_size)
        else:
            result = fit(args.corpus, args.output, config, device=args.device, batch_size=args.batch_size,
                         steps=args.steps, max_seconds=args.max_seconds, max_tokens=args.max_tokens,
                         evaluation_interval=args.evaluation_interval)
        print(json.dumps(result, allow_nan=False), flush=True)
    except (OSError, ValueError, RuntimeError) as error:
        parser.exit(2, f"Language experiment failed: {error}\n")


if __name__ == "__main__":
    main()
