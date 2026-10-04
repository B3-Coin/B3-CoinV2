# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""CPU sanity pretraining of our own small randomly initialized Transformer."""

import argparse
import hashlib
import json
import math
import os
import platform
from pathlib import Path
import tempfile
import time

import numpy as np

from .checkpoint import load_checkpoint, save_checkpoint
from .data import BASELINE, builtin_curriculum, corpus_bytes, load_curriculum, split_hash
from .model import AdamW, ModelConfig, Transformer
from .tokenizer import ByteTokenizer


def implementation_hash():
    digest = hashlib.sha256()
    for name in ("model.py", "tokenizer.py", "checkpoint.py", "data.py", "train.py"):
        digest.update(name.encode() + b"\0")
        digest.update(Path(__file__).with_name(name).read_bytes())
    return digest.hexdigest()


def batch(documents, batch_size, length, rng):
    eligible = [doc for doc in documents if len(doc) > length]
    if not eligible:
        raise ValueError("No complete record is longer than the training sequence.")
    x = np.empty((batch_size, length), dtype=np.int64)
    y = np.empty_like(x)
    for i in range(batch_size):
        doc = eligible[int(rng.integers(len(eligible)))]
        start = int(rng.integers(len(doc) - length))
        x[i] = doc[start:start + length]
        y[i] = doc[start + 1:start + length + 1]
    return x, y


def evaluate(model, documents, batch_size, length, batches=4):
    # Reset a separate RNG for comparable evaluations without changing training.
    rng = np.random.default_rng(1729)
    losses = []
    for _ in range(batches):
        x, y = batch(documents, batch_size, length, rng)
        logits = model.forward(x).astype(np.float64)
        logits -= logits.max(axis=-1, keepdims=True)
        log_partition = np.log(np.exp(logits).sum(axis=-1))
        selected = np.take_along_axis(logits, y[..., None], axis=-1)[..., 0]
        loss = float(np.mean(log_partition - selected))
        if not math.isfinite(loss):
            raise ValueError("Evaluation produced a non-finite loss.")
        losses.append(loss)
    return float(np.mean(losses))


def atomic_write(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".b3-scratch-", delete=False) as stream:
            temporary = stream.name
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            os.unlink(temporary)


def run(output, records, *, steps=200, sequence_length=64, batch_size=4,
        learning_rate=0.001, config=None, resume=False, evaluation_interval=50):
    if type(steps) is not int or not 1 <= steps <= 5000:
        raise ValueError("Choose 1 to 5000 additional sanity-training steps.")
    if type(batch_size) is not int or not 1 <= batch_size <= 16:
        raise ValueError("Choose a batch size from 1 to 16.")
    if type(sequence_length) is not int or not 2 <= sequence_length <= 256:
        raise ValueError("Choose a sequence length from 2 to 256.")
    if not math.isfinite(learning_rate) or not 0 < learning_rate <= 0.1:
        raise ValueError("Invalid learning rate.")
    if type(evaluation_interval) is not int or not 1 <= evaluation_interval <= 5000:
        raise ValueError("Invalid evaluation interval.")
    # A review flag records a teacher decision, not an automated truth proof.
    from .data import validate_records
    validate_records(records)
    encoded = corpus_bytes(records)
    builtin_corpus = encoded == corpus_bytes(builtin_curriculum())
    digest = hashlib.sha256(encoded).hexdigest()
    code_digest = implementation_hash()
    output = Path(output)
    checkpoint = output / "model.npz"
    if checkpoint.exists() and not resume:
        raise ValueError("An experiment already exists; use --resume to continue it.")
    tokenizer = ByteTokenizer()
    train_docs = [tokenizer.encode(r["text"]) for r in records if r["split"] == "train"]
    validation_docs = [tokenizer.encode(r["text"]) for r in records if r["split"] == "validation"]
    if resume:
        model, optimizer, metadata = load_checkpoint(checkpoint)
        if config is not None and model.config.to_dict() != config.to_dict():
            raise ValueError("The resumed model configuration differs.")
        if metadata.get("dataset_sha256") != digest or metadata.get("stage") != "sanity-pretraining":
            raise ValueError("The resumed corpus or training stage differs.")
        if metadata.get("implementation_sha256") != code_digest:
            raise ValueError("The training implementation has changed; resume with the original code.")
        rng = np.random.default_rng()
        rng.bit_generator.state = metadata["rng_state"]
        if metadata.get("sequence_length") != sequence_length or metadata.get("batch_size") != batch_size:
            raise ValueError("Resume with the original sequence length and batch size.")
        if metadata.get("learning_rate") != learning_rate:
            raise ValueError("Resume with the original learning rate.")
    else:
        # Train every learned position used by the default sampling window.
        selected_config = config or ModelConfig(context_length=sequence_length)
        if selected_config.vocab_size != 256:
            raise ValueError("The UTF-8 byte tokenizer requires exactly 256 vocabulary entries.")
        model = Transformer(selected_config)
        optimizer = AdamW(model.params)
        rng = np.random.default_rng(model.config.seed + 1)
        metadata = {}
    if model.config.vocab_size != 256:
        raise ValueError("The checkpoint vocabulary is incompatible with byte tokenization.")
    if sequence_length > model.config.context_length:
        raise ValueError("The sequence exceeds the model's configured context.")
    # Check both split windows before creating any output artifacts.
    batch(train_docs, batch_size, sequence_length, np.random.default_rng(1))
    batch(validation_docs, batch_size, sequence_length, np.random.default_rng(1))
    started = time.monotonic()
    initial_train = evaluate(model, train_docs, batch_size, sequence_length)
    initial_validation = evaluate(model, validation_docs, batch_size, sequence_length)
    baseline = metadata.get("initial_metrics", {"train_loss": initial_train, "validation_loss": initial_validation})
    print(json.dumps({"event": "start", "parameters": model.parameter_count, "initialization": "random",
                      "pretrained_weights": False, "starting_step": optimizer.steps,
                      "train_loss": initial_train, "validation_loss": initial_validation}), flush=True)
    history = []
    for index in range(1, steps + 1):
        x, y = batch(train_docs, batch_size, sequence_length, rng)
        loss, grads = model.loss_and_grad(x, y)
        if not math.isfinite(loss):
            raise ValueError("Training produced a non-finite loss; checkpoint not replaced.")
        optimizer.step(model.params, grads, lr=learning_rate, clip_norm=1.0)
        if index % evaluation_interval == 0 or index == steps:
            metrics = {"step": optimizer.steps,
                       "train_loss": evaluate(model, train_docs, batch_size, sequence_length),
                       "validation_loss": evaluate(model, validation_docs, batch_size, sequence_length)}
            history.append(metrics)
            print(json.dumps({"event": "evaluation", **metrics}), flush=True)
    final = history[-1]
    report = {"stage": "sanity-pretraining", "readiness": "experimental",
              "initialization": "random", "pretrained_weights": False,
              "parameters": model.parameter_count, "config": model.config.to_dict(),
              "completed_steps": optimizer.steps, "additional_steps": steps,
              "sequence_length": sequence_length, "batch_size": batch_size, "learning_rate": learning_rate,
              "elapsed_seconds": time.monotonic() - started, "dataset_sha256": digest,
              "implementation_sha256": code_digest,
              "software": {"python": platform.python_version(), "numpy": np.__version__},
              "training_split_hash": split_hash(records, "train"), "validation_split_hash": split_hash(records, "validation"),
              "training_baseline": BASELINE,
              "corpus_origin": "builtin-b3-fixture" if builtin_corpus else "custom-corpus",
              "source_revision": BASELINE if builtin_corpus else None,
              "teacher_review": ("source-reviewed fixture; not a general language corpus" if builtin_corpus
                                 else "caller-declared review; not independently verified"),
              "train_records": len(train_docs), "validation_records": len(validation_docs),
              "initial_metrics": baseline, "final_metrics": final, "evaluations_this_run": history,
              "limits": "Byte-prediction experiment only. No demonstrated chat, tool use or independent task accuracy."}
    metadata = {**report, "rng_state": rng.bit_generator.state}
    output.mkdir(parents=True, exist_ok=True)
    save_checkpoint(checkpoint, model, optimizer, metadata)
    # This identifies the saved artifact; it is not a proof of model quality.
    checkpoint_digest = hashlib.sha256()
    with checkpoint.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            checkpoint_digest.update(chunk)
    report["checkpoint_sha256"] = checkpoint_digest.hexdigest()
    atomic_write(output / "corpus.jsonl", encoded)
    atomic_write(output / "report.json", (json.dumps(report, indent=2, sort_keys=True) + "\n").encode())
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build-ai/scratch-experiment"))
    parser.add_argument("--corpus", type=Path, help="Reviewed JSONL corpus; defaults to the small B3 fixture")
    parser.add_argument("--steps", type=int, default=200)
    parser.add_argument("--sequence-length", type=int, default=64)
    parser.add_argument("--batch-size", type=int, default=4)
    parser.add_argument("--learning-rate", type=float, default=0.001)
    parser.add_argument("--evaluation-interval", type=int, default=50)
    parser.add_argument("--config", type=Path, help="JSON object with ModelConfig fields")
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    try:
        config = ModelConfig(**json.loads(args.config.read_text())) if args.config else None
        report = run(args.output, load_curriculum(args.corpus), steps=args.steps,
                     sequence_length=args.sequence_length, batch_size=args.batch_size,
                     learning_rate=args.learning_rate, config=config, resume=args.resume,
                     evaluation_interval=args.evaluation_interval)
    except (OSError, ValueError, TypeError, KeyError, FloatingPointError) as error:
        parser.error(str(error))
    print(json.dumps({"event": "saved", "checkpoint": str(args.output / "model.npz"),
                      "readiness": report["readiness"]}), flush=True)


if __name__ == "__main__":
    main()
