# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Full-size rehearsal on freshly random weights; the student is never loaded.

Exercises the real trainer, checkpoint size, recorder and scorer end to end on
the real data and device. Random weights are created here and discarded; this
is a software rehearsal, not training of the student and not evidence about it.
"""

import argparse
import copy
import json
from pathlib import Path

import torch

from . import evaluate, train
from ..language import train as language_train
from ..language.checkpoint import save_checkpoint
from ..language.evaluate import continuation
from ..language.model import GpuConfig, TorchTransformer

REHEARSAL_STAGE = "rehearsal-random-init"


def rehearsal_plan(plan, parent, parent_sha256, updates=12):
    """The frozen plan compressed to a few updates, pointing at throwaway random weights."""
    plan = copy.deepcopy(plan)
    plan["parent"] = {"path": str(parent), "sha256": parent_sha256, "stage": REHEARSAL_STAGE, "optimizer_steps": 0,
                      "language_validation_loss": None, "instruction_validation_loss": None}
    third = updates // 3
    plan["phases"] = [{"name": "bare-value", "first": 1, "last": third, "cycle": ["value"]},
                      {"name": "ramp", "first": third + 1, "last": third + 2, "cycle": ["value", "json"]},
                      {"name": "short-json", "first": third + 3, "last": updates, "cycle": plan["phases"][-1]["cycle"]}]
    plan["limits"].update(maximum_updates=updates, maximum_wall_seconds=300, wall_reserve_seconds=20)
    plan["optimizer"]["learning_rate"].update(warmup_updates=2, hold_until=updates - 4, final_update=updates)
    plan["evaluation"]["interval"] = 4
    plan["preflight"]["projection_limit_seconds"] = 280
    plan["data"]["schedule_sha256"] = None
    return plan


def rehearse(directory, device="mps", updates=12, short_tokens=24):
    """The gate section uses the real 256-token budget; later sections use a short one to save time."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=False)
    versions = language_train.runtime(device)
    model = TorchTransformer(GpuConfig())
    optimizer = torch.optim.AdamW(list(model.params.values()), lr=1e-4, weight_decay=0.01, foreach=False, fused=False)
    parent = directory / "random-parent.npz"
    save_checkpoint(parent, model, optimizer, {
        "stage": REHEARSAL_STAGE, "experimental": True, "model_family_origin": language_train.MODEL_FAMILY_ORIGIN,
        "component_hashes": language_train.component_hashes(), "runtime": versions,
        "note": "freshly random weights for a software rehearsal; not the student"})
    frozen, frozen_sha256 = train.load_plan()
    plan = rehearsal_plan(frozen, parent.resolve(), train.file_hash(parent), updates)
    report = train.fit(plan, "rehearsal-plan", directory / "run", device=device, ledger=directory / "run-ledger.json")
    calls = []

    def short(model, prompt, **settings):
        calls.append(1)
        budget = settings["maximum_new_tokens"] if len(calls) <= 128 else short_tokens
        return continuation(model, prompt, temperature=settings["temperature"], seed=settings["seed"],
                            maximum_new_tokens=budget)

    selected = directory / "run" / "model.npz"
    evaluate.record_run(selected, report["selected_checkpoint_sha256"], "after", directory / "after", device=device,
                        run_report=directory / "run" / "report.json", plan=plan, plan_sha256="rehearsal-plan",
                        generator=short)
    scores = evaluate.score_directory(directory / "after", plan=plan)
    updates_done = report["attempted_updates"]
    per_update = [json.loads(line) for line in (directory / "run" / "updates.jsonl").read_text().splitlines()]
    preflight, limits = report["preflight"], frozen["limits"]
    # The real plan's own go/no-go rule, applied to the timings just measured at full size.
    steps, interval = limits["maximum_updates"], frozen["evaluation"]["interval"]
    projection = (preflight["elapsed_before_projection"]
                  + steps * (preflight["typical_pass_seconds"] + frozen["preflight"]["step_allowance_seconds"])
                  + (steps // interval) * preflight["evaluation_seconds"])
    summary = {"rehearsal": True, "student_loaded": False, "weights": "freshly random, discarded",
               "implementation_hash": train.implementation_hash(), "frozen_plan_sha256": frozen_sha256,
               "generation_budget": {"gate_section": 256, "other_sections": short_tokens},
               "frozen_plan_projection": {"projected_seconds": projection,
                                          "limit_seconds": frozen["preflight"]["projection_limit_seconds"],
                                          "would_start": projection <= frozen["preflight"]["projection_limit_seconds"]},
               "status": report["status"], "stop_reason": report["stop_reason"], "updates": updates_done,
               "elapsed_seconds": report["elapsed_seconds"], "preflight": report["preflight"],
               "peak_memory": report["peak_memory"], "selected_update": report["selected_update"],
               "evaluations": len(report["history"]), "modes": [entry["mode"] for entry in per_update],
               "update_seconds": [round(b["elapsed_seconds"] - a["elapsed_seconds"], 4)
                                  for a, b in zip(per_update, per_update[1:])],
               "widths": [entry["width"] for entry in per_update],
               "gate_pairs_strict": scores["gate"]["pairs"]["strict"],
               "validation_agreement": scores["validation_generation"]["agreement"]}
    (directory / "rehearsal.json").write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    return summary


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--device", choices=("cpu", "mps"), default="mps")
    parser.add_argument("--updates", type=int, default=12)
    args = parser.parse_args(argv)
    print(json.dumps(rehearse(args.directory, args.device, args.updates), indent=1))


if __name__ == "__main__":
    main()
