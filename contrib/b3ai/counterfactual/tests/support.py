# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Toy fixtures: a tiny random model stands in for the student in every test."""

import copy
from pathlib import Path

import torch

from contrib.b3ai.counterfactual import train
from contrib.b3ai.language import train as language_train
from contrib.b3ai.language.checkpoint import save_checkpoint
from contrib.b3ai.language.model import GpuConfig, TorchTransformer

REPOSITORY = Path(__file__).resolve().parents[4]
CORPUS = REPOSITORY / "build-ai/language-preparation/tinystories-pilot"
NEEDS_CORPUS = "needs the prepared local story corpus under build-ai/ (git-ignored)"
TOY = dict(vocab_size=257, context_length=2048, width=16, layers=1, heads=2, ff_multiplier=1, seed=42)


def toy_parent(directory, device="cpu"):
    model = TorchTransformer(GpuConfig(**TOY))
    optimizer = torch.optim.AdamW(list(model.params.values()), lr=1e-4, weight_decay=0.01, foreach=False, fused=False)
    path = Path(directory) / "toy-parent.npz"
    save_checkpoint(path, model, optimizer, {
        "stage": "toy-parent", "experimental": True, "model_family_origin": language_train.MODEL_FAMILY_ORIGIN,
        "component_hashes": language_train.component_hashes(), "runtime": language_train.runtime(device)})
    return path, train.file_hash(path)


def toy_plan(parent, sha256, updates=6):
    plan = copy.deepcopy(train.load_plan()[0])
    plan["parent"] = {"path": str(Path(parent).resolve()), "sha256": sha256, "stage": "toy-parent", "optimizer_steps": 0,
                      "language_validation_loss": None, "instruction_validation_loss": None}
    plan["phases"] = [{"name": "bare-value", "first": 1, "last": 2, "cycle": ["value"]},
                      {"name": "ramp", "first": 3, "last": 4, "cycle": ["value", "json"]},
                      {"name": "short-json", "first": 5, "last": updates, "cycle": ["json", "value"]}]
    plan["limits"].update(maximum_updates=updates)
    plan["optimizer"]["learning_rate"].update(warmup_updates=2, hold_until=4, final_update=updates)
    plan["evaluation"]["interval"] = 2
    plan["preflight"].update(warmup_passes=1, timed_passes=1, projection_limit_seconds=100000)
    plan["data"]["schedule_sha256"] = None
    return plan
