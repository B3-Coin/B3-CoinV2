# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""One bounded counterfactual-grounding run for our own byte/EOD Transformer.

Every update is one AdamW step on one batch that mixes story practice, paired
grounding turns and reviewed replay. Validation selects weights. The trainer
never opens the exam, never generates text and never executes a tool.
"""

import argparse
import copy
import datetime
import hashlib
import json
import math
from pathlib import Path
import re
import time
import uuid

import numpy as np
import torch
import torch.nn.functional as F

from . import data as grounding_data
from .data import (BOILERPLATE, PACKAGE, REPOSITORY, TERMINATOR, UNKNOWN_WORD, UNSUPERVISED_TERMINATOR, VALUE,
                   GroundingCorpus)
from ..language import train as language_train
from ..language.batches import Corpus
from ..language.checkpoint import MAX_ARCHIVE_BYTES, load_checkpoint, save_checkpoint
from ..language.batches import _json
from ..tool_learning import train as mixed_train
from ..tool_learning.data import InstructionCorpus


STAGE = "counterfactual-grounding"
PLAN_SCHEMA = "b3-counterfactual-grounding-plan/v1"
MAX_UPDATES = 1000
MAX_SECONDS = 500
MAX_TARGETS = 8_192_000
MAX_MEMORY = 32 * 1024**3
CODE_FILES = ("__init__.py", "data.py", "train.py", "evaluate.py", "freeze.py", "rehearse.py")
# Reporting classes: "value" is the digits of a supplied value and nothing else.
CLASS_NAMES = {VALUE: "value", TERMINATOR: "terminator", BOILERPLATE: "boilerplate", UNKNOWN_WORD: "unknown_word",
               UNSUPERVISED_TERMINATOR: "unsupervised_terminator"}
FORMATTING = (TERMINATOR, BOILERPLATE)


def file_hash(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def load_plan(path=None):
    path = Path(path or PACKAGE / "plan.json")
    raw = path.read_bytes()
    plan = _json(raw)
    if type(plan) is not dict or plan.get("schema") != PLAN_SCHEMA or plan.get("stage") != STAGE:
        raise ValueError("Plan is not the frozen counterfactual-grounding plan.")
    return plan, hashlib.sha256(raw).hexdigest()


def implementation_hash():
    digest = hashlib.sha256()
    for name in CODE_FILES:
        digest.update(name.encode("ascii") + b"\0" + (PACKAGE / name).read_bytes())
    digest.update(json.dumps(language_train.component_hashes(), sort_keys=True).encode("ascii"))
    digest.update(json.dumps(mixed_train.mixed_component_hashes(), sort_keys=True).encode("ascii"))
    return digest.hexdigest()


def learning_rate(update, schedule):
    """Linear warm-up, hold, then linear decay; a plain Python float for the checkpoint."""
    if update <= schedule["warmup_updates"]:
        return float(schedule["peak"] * update / schedule["warmup_updates"])
    if update <= schedule["hold_until"]:
        return float(schedule["peak"])
    span = schedule["final_update"] - schedule["hold_until"]
    fraction = min(1.0, (update - schedule["hold_until"]) / span)
    return float(schedule["peak"] + (schedule["final"] - schedule["peak"]) * fraction)


def validate_plan(plan):
    limits, batch, rate = plan["limits"], plan["batch"], plan["optimizer"]["learning_rate"]
    rows = batch["language_rows"] + batch["grounding_rows"] + batch["replay_rows"]
    if (type(limits["maximum_updates"]) is not int or not 1 <= limits["maximum_updates"] <= MAX_UPDATES
            or not 0 < limits["maximum_wall_seconds"] <= MAX_SECONDS
            or not 0 <= limits["wall_reserve_seconds"] < limits["maximum_wall_seconds"]
            or not 0 < limits["maximum_supervised_targets"] <= MAX_TARGETS
            or not 0 < limits["maximum_memory_bytes"] <= MAX_MEMORY):
        raise ValueError("Plan limits exceed the frozen experiment bounds.")
    if (rows > 16 or batch["maximum_width"] > 1024 or batch["language_context"] > batch["maximum_width"]
            or rows * batch["maximum_width"] > 16384 or batch["grounding_rows"] != 10):
        raise ValueError("Plan batch exceeds the one-batch model bounds.")
    if (not 0 < rate["final"] <= rate["peak"] <= 0.001 or rate["warmup_updates"] < 1
            or rate["hold_until"] < rate["warmup_updates"] or rate["final_update"] <= rate["hold_until"]):
        raise ValueError("Plan learning-rate schedule is invalid.")
    covered = [update for phase in plan["phases"] for update in range(phase["first"], phase["last"] + 1)]
    if covered != list(range(1, limits["maximum_updates"] + 1)) or any(
            mode not in grounding_data.MODES for phase in plan["phases"] for mode in phase["cycle"]):
        raise ValueError("Plan phases must cover every update exactly once with known answer modes.")
    if plan["phases"][0]["cycle"] != ["value"]:
        raise ValueError("Bare-value teaching must come first.")


def replay_records(instructions, categories):
    return {record["id"]: record for record in instructions.records["train"] if record["category"] in categories}


def build_batch(stories, grounding, instructions, replay, entry, language_rng, plan):
    """One batch of whole rows: story windows, then grounding turns, then replay turns."""
    rows = plan["batch"]
    parts = [stories.batch("train", language_rng, rows["language_rows"], rows["language_context"]),
             grounding.batch(entry["grounding"]),
             grounding_data.stack([instructions.arrays(replay[identity]) for identity in entry["replay"]])]
    # Every batch has the same shape: rows are right-padded with weightless zeros to the
    # maximum width, which the causal mask makes numerically neutral.
    count, width = sum(len(part[0]) for part in parts), rows["maximum_width"]
    if count > 16 or max(part[0].shape[1] for part in parts) > width or count * width > 16384:
        raise ValueError("Mixed batch exceeds the one-batch model bounds.")
    tokens = np.zeros((count, width), dtype=np.int64)
    targets = np.zeros_like(tokens)
    weight = np.zeros(tokens.shape, dtype=np.float32)
    kind = np.zeros(tokens.shape, dtype=np.int8)
    stream = np.zeros(count, dtype=np.int64)
    start = 0
    for index, part in enumerate(parts):
        size, columns = part[0].shape
        tokens[start:start + size, :columns], targets[start:start + size, :columns] = part[0], part[1]
        weight[start:start + size, :columns] = part[2]
        if len(part) > 3:
            kind[start:start + size, :columns] = part[3]
        stream[start:start + size] = index
        start += size
    return {"tokens": tokens, "targets": targets, "weight": weight, "kind": kind, "stream": stream}


def batch_loss(model, batch, device):
    """One forward pass; each stream keeps its own weighted mean, as model.loss defines it."""
    tokens = torch.from_numpy(batch["tokens"]).to(device)
    targets = torch.from_numpy(batch["targets"]).to(device)
    weight = torch.from_numpy(batch["weight"]).to(device)
    logits = model(tokens)
    losses = F.cross_entropy(logits.reshape(-1, logits.shape[-1]), targets.reshape(-1),
                             reduction="none").reshape(tokens.shape)
    streams = []
    for index in range(3):
        rows = torch.from_numpy(np.flatnonzero(batch["stream"] == index)).to(device)
        selected = weight.index_select(0, rows)
        streams.append((losses.index_select(0, rows) * selected).sum() / selected.sum())
    total = (streams[0] + streams[1] + streams[2]) / 3
    detached, kind = losses.detach().cpu().numpy(), batch["kind"]
    classes = {name: float(detached[kind == code].mean()) if (kind == code).any() else None
               for code, name in CLASS_NAMES.items()}
    classes["formatting"] = float(detached[np.isin(kind, FORMATTING)].mean())
    classes["class_targets"] = {name: int((kind == code).sum()) for code, name in CLASS_NAMES.items()}
    return total, [float(value.detach().cpu()) for value in streams], classes


def score_replay(model, instructions, categories, device):
    """Teacher-forced answer loss per frozen validation category; replayed ones reported apart."""
    sums, counts = {}, {}
    records = instructions.records["validation"]
    with torch.no_grad():
        for start in range(0, len(records), 8):
            chunk = records[start:start + 8]
            tokens, targets, weight, _ = grounding_data.stack([instructions.arrays(record) for record in chunk])
            logits = model(torch.from_numpy(tokens).to(device))
            losses = F.cross_entropy(logits.reshape(-1, logits.shape[-1]), torch.from_numpy(targets).to(device).reshape(-1),
                                     reduction="none").reshape(tokens.shape).cpu().numpy()
            for index, record in enumerate(chunk):
                category = record["category"]
                sums[category] = sums.get(category, 0.0) + float((losses[index] * weight[index]).sum())
                counts[category] = counts.get(category, 0) + int(weight[index].sum())

    def pooled(names):
        return sum(sums[name] for name in names) / sum(counts[name] for name in names)

    kept = [name for name in sums if name in categories]
    other = [name for name in sums if name not in categories]
    return {"all": pooled(list(sums)), "replayed": pooled(kept), "not_replayed": pooled(other) if other else None,
            "categories": {name: sums[name] / counts[name] for name in sorted(sums)},
            "targets": {name: counts[name] for name in sorted(counts)}}


def score_grounding(model, grounding, split, device):
    """Teacher-forced grounding scores; a turn is exact when every answer byte is the argmax."""
    records = grounding.turns[split]
    sums = {name: 0.0 for name in CLASS_NAMES.values()}
    counts = {name: 0 for name in CLASS_NAMES.values()}
    weighted, weights, exact, supervised_exact, margins = 0.0, 0.0, {}, {}, []
    positions = {name: [0, 0] for name in ("digit1", "digit2", "digit3", "terminator")}
    with torch.no_grad():
        for start in range(0, len(records), 16):
            chunk = records[start:start + 16]
            tokens, targets, weight, kind = grounding.batch([record["id"] for record in chunk])
            logits = model(torch.from_numpy(tokens).to(device)).cpu()
            target_tensor = torch.from_numpy(targets)
            losses = F.cross_entropy(logits.reshape(-1, logits.shape[-1]), target_tensor.reshape(-1),
                                     reduction="none").reshape(tokens.shape).numpy()
            top = torch.topk(logits, 2, dim=-1)
            predicted, margin = top.indices[..., 0].numpy(), (top.values[..., 0] - top.values[..., 1]).numpy()
            weighted += float((losses * weight).sum())
            weights += float(weight.sum())
            for code, name in CLASS_NAMES.items():
                sums[name] += float(losses[kind == code].sum())
                counts[name] += int((kind == code).sum())
            for index, record in enumerate(chunk):
                answer = kind[index] > 0
                correct = predicted[index] == targets[index]
                # Exact means every answer byte, which is what greedy generation must produce.
                exact[record["id"]] = bool(correct[answer].all())
                supervised_exact[record["id"]] = bool(correct[weight[index] > 0].all())
                margins.append(float(margin[index][answer].min()))
                if record["label"] != grounding_data.UNKNOWN:
                    digits = np.flatnonzero(kind[index] == VALUE)
                    for number, position in enumerate(digits[:3], 1):
                        positions[f"digit{number}"][0] += int(correct[position])
                        positions[f"digit{number}"][1] += 1
                    position = int(np.flatnonzero(np.isin(kind[index], (TERMINATOR, UNSUPERVISED_TERMINATOR)))[0])
                    positions["terminator"][0] += int(correct[position])
                    positions["terminator"][1] += 1
    formatting = sum(counts[CLASS_NAMES[code]] for code in FORMATTING)
    summary = {"loss": weighted / weights, "exact_turns": sum(exact.values()), "turns": len(records),
               "supervised_exact_turns": sum(supervised_exact.values()),
               "value_target_loss": sums["value"] / counts["value"],
               "formatting_loss": sum(sums[CLASS_NAMES[code]] for code in FORMATTING) / formatting,
               "class_loss": {name: sums[name] / counts[name] if counts[name] else None for name in sums},
               "class_targets": dict(counts),
               "position_accuracy": {name: value[0] / value[1] if value[1] else None for name, value in positions.items()},
               "minimum_margin": min(margins),
               "exact_vector": "".join("1" if exact[record["id"]] else "0" for record in records)}
    for mode in grounding_data.MODES:
        members = [pair for (_, pair_mode), pair in grounding.pairs[split].items() if pair_mode == mode]
        unknown = [record for record in grounding.unknown[split] if record["mode"] == mode]
        summary[mode] = {"pairs_both_exact": sum(exact[pair["a"]["id"]] and exact[pair["b"]["id"]] for pair in members),
                         "pairs": len(members),
                         "exact_turns": sum(exact[record["id"]] for record in records if record["mode"] == mode),
                         "turns": sum(record["mode"] == mode for record in records),
                         "unknown_exact": sum(exact[record["id"]] for record in unknown), "unknown": len(unknown)}
    return summary


def selection_key(values, update):
    """Validation only, aligned with the JSON-mode gate; a later update wins an exact tie."""
    validation = values["grounding_validation"]
    mean = (values["language"] + values["replay"]["replayed"] + validation["loss"]) / 3
    return (validation["json"]["pairs_both_exact"], validation["json"]["exact_turns"], validation["exact_turns"], -mean, update)


def check_separation(grounding, seal):
    """The trainer never parses the exam: it compares against hashes sealed with it."""
    prompts = {record["prompt_sha256"] for record in grounding.by_id.values()}
    numbers = set()
    for split, records in grounding.turns.items():
        numbers |= grounding_data._split_numbers(split, records)
    frames = {hashlib.sha256(grounding_data.normalize_question(frame).encode("utf-8")).hexdigest()
              for split in ("train", "validation") for frame in grounding_data.FRAMES[split]}
    if prompts & set(seal["prompt_sha256"]) or numbers & set(seal["numbers"]) or frames & set(seal["frame_sha256"]):
        raise ValueError("Training or validation data overlaps the sealed exam.")


def _parent(plan, versions, repository):
    parent = plan["parent"]
    path = Path(repository) / parent["path"]
    expected = parent["sha256"]
    if type(expected) is not str or re.fullmatch(r"[0-9a-f]{64}", expected) is None:
        raise ValueError("Complete expected parent SHA-256 is required.")
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_ARCHIVE_BYTES:
        raise ValueError("Parent must be a bounded regular checkpoint file.")
    if file_hash(path) != expected:
        raise ValueError("Approved parent SHA-256 does not match.")
    model, optimizer, metadata = load_checkpoint(path, device="cpu")
    if file_hash(path) != expected:
        raise ValueError("Parent changed while loading.")
    previous = metadata.get("runtime")
    if (metadata.get("stage") != parent["stage"] or metadata.get("experimental") is not True
            or metadata.get("model_family_origin") != language_train.MODEL_FAMILY_ORIGIN
            or metadata.get("component_hashes") != language_train.component_hashes()
            or type(previous) is not dict
            or any(previous.get(key) != versions[key] for key in ("python", "numpy", "torch", "precision"))):
        raise ValueError("Parent is not the compatible own random-start student on this runtime.")
    steps = parent["optimizer_steps"]
    if (steps == 0 and optimizer.state) or (steps > 0 and (
            set(optimizer.state) != set(model.params.values())
            or any(state["step"].item() != steps for state in optimizer.state.values()))):
        raise ValueError("Parent AdamW steps do not match the planned lineage.")
    return path, model, metadata


def fit(plan, plan_sha256, output, *, device="mps", repository=REPOSITORY, ledger=None, exam_seal=None,
        seals=None, updates_log=None):
    """Run the frozen schedule once. A stop before the first update consumes nothing."""
    started = time.monotonic()
    validate_plan(plan)
    output, repository, limits = Path(output), Path(repository), plan["limits"]
    ledger = Path(ledger) if ledger is not None else None
    if output.exists() or output.is_symlink():
        raise FileExistsError("Grounding output exists; a fresh exclusive directory is required.")
    if ledger is not None and (ledger.exists() or ledger.is_symlink()):
        raise FileExistsError("The run ledger exists: the single bounded run was already started.")
    versions = language_train.runtime(device)
    stories = Corpus(repository / plan["data"]["language_corpus"])
    instructions = InstructionCorpus(repository / plan["data"]["instruction_corpus"], repository=repository)
    grounding = GroundingCorpus(repository / plan["data"]["grounding_curriculum"])
    if (stories.manifest_hash != plan["data"]["language_manifest_sha256"]
            or len(stories.splits["train"]) != plan["data"]["language_train_documents"]
            or instructions.sha256 != plan["data"]["instruction_corpus_sha256"]
            or grounding.sha256 != plan["data"]["grounding_curriculum_sha256"]):
        raise ValueError("Story, replay or grounding data differs from the frozen plan.")
    if exam_seal is not None:
        check_separation(grounding, exam_seal)
    replay = replay_records(instructions, plan["replay_categories"])
    schedule = grounding_data.build_schedule(grounding, replay, plan)
    schedule_sha = hashlib.sha256(grounding_data.schedule_bytes(schedule)).hexdigest()
    if plan["data"].get("schedule_sha256") not in (None, schedule_sha):
        raise ValueError("Sampling schedule differs from the frozen schedule.")
    parent_path, model, parent = _parent(plan, versions, repository)
    if model.config.context_length < plan["batch"]["maximum_width"]:
        raise ValueError("Model context is smaller than the planned batch width.")
    validation_rng = np.random.default_rng(plan["seeds"]["language_validation"])
    story_batches = [stories.batch("validation", validation_rng, 4, min(2048, model.config.context_length)) for _ in range(4)]
    language_rng = np.random.default_rng(plan["seeds"]["language_batches"])
    rate = plan["optimizer"]["learning_rate"]
    steps = limits["maximum_updates"]
    report = {
        "stage": STAGE, "status": "running", "experimental": True,
        "objective": "one mixed batch per update: story practice, paired counterfactual grounding, reviewed replay",
        "update_definition": plan["update_definition"], "optimizer_reset": True, "batch_rng_reset": True,
        "parent_checkpoint": plan["parent"]["path"], "parent_checkpoint_sha256": plan["parent"]["sha256"],
        "parent_stage": parent.get("stage"), "parent_selected_or_saved_step": parent.get("selected_or_saved_step"),
        "parent_implementation_hash": parent.get("implementation_hash"),
        "inherited_language_steps": parent.get("inherited_language_steps"),
        "inherited_b3_teaching_steps": parent.get("inherited_b3_teaching_steps"),
        "runtime": versions, "config": model.config.to_dict(), "parameter_count": model.parameter_count,
        "model_family_origin": language_train.MODEL_FAMILY_ORIGIN, "run_id": str(uuid.uuid4()),
        "implementation_hash": implementation_hash(), "component_hashes": language_train.component_hashes(),
        "mixed_component_hashes": mixed_train.mixed_component_hashes(), "plan_sha256": plan_sha256,
        "language_corpus_manifest_hash": stories.manifest_hash, "instruction_corpus_sha256": instructions.sha256,
        "grounding_curriculum_sha256": grounding.sha256, "schedule_sha256": schedule_sha, "seals": seals,
        "replay_records": len(replay), "resource_limits": limits, "phases": plan["phases"], "batch": plan["batch"],
        "optimizer": plan["optimizer"], "seeds": plan["seeds"],
        "attempted_updates": 0, "selected_update": 0, "training_targets": 0,
        "stream_targets": {"language": 0, "grounding": 0, "replay": 0}, "history": [],
        "independent_exam_used": False, "exam_parsed_by_trainer": False, "generation_performed": False,
        "real_tools_executed": False,
        "limitations": "Validation selects weights and is not qualification. All tool results are synthetic. "
                       "Caps are checked between complete updates.",
    }
    output.mkdir(parents=True, exist_ok=False)
    log_path = Path(updates_log) if updates_log is not None else output / "updates.jsonl"
    peak = language_train.memory(device)
    selected_key = None

    def observe():
        for key, value in language_train.memory(device).items():
            peak[key] = max(peak[key], value)

    def elapsed():
        language_train.synchronize(device)
        return time.monotonic() - started

    def memory_stop():
        observe()
        return max(max(peak.values()), sum(peak.values())) >= limits["maximum_memory_bytes"]

    def write_report():
        temporary = output / ".report.json.tmp"
        temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
        temporary.replace(output / "report.json")

    def snapshot(name, update, values):
        observe()
        if update and any(state["step"].item() != update for state in optimizer.state.values()):
            raise ValueError("AdamW steps do not match the completed updates.")
        info = {key: report[key] for key in (
            "stage", "experimental", "objective", "run_id", "runtime", "implementation_hash", "component_hashes",
            "mixed_component_hashes", "plan_sha256", "model_family_origin", "optimizer_reset", "batch_rng_reset",
            "parent_checkpoint_sha256", "parent_stage", "parent_selected_or_saved_step", "parent_implementation_hash",
            "inherited_language_steps", "inherited_b3_teaching_steps", "language_corpus_manifest_hash",
            "instruction_corpus_sha256", "grounding_curriculum_sha256", "schedule_sha256")}
        info.update({"selected_or_saved_step": update, "training_targets_at_save": report["training_targets"],
                     "learning_rate_schedule": rate, "language_rng_state": copy.deepcopy(language_rng.bit_generator.state),
                     "validation_at_save": None if values is None else {
                         "language": values["language"], "replay_replayed": values["replay"]["replayed"],
                         "replay_all": values["replay"]["all"], "grounding_loss": values["grounding_validation"]["loss"],
                         "json_pairs_both_exact": values["grounding_validation"]["json"]["pairs_both_exact"],
                         "exact_turns": values["grounding_validation"]["exact_turns"]}})
        save_checkpoint(output / name, model, optimizer, info)

    def evaluate(update, train_loss):
        begin = elapsed()
        values = {"update": update, "language": language_train.score(model, story_batches, device),
                  "replay": score_replay(model, instructions, plan["replay_categories"], device),
                  "grounding_validation": score_grounding(model, grounding, "validation", device),
                  "training_fit": score_grounding(model, grounding, "train", device),
                  "train_loss": train_loss, "learning_rate": float(optimizer.param_groups[0]["lr"])}
        for value in (values["language"], values["replay"]["all"], values["grounding_validation"]["loss"]):
            if not math.isfinite(value):
                raise ValueError("Non-finite validation loss.")
        values["selection_key"] = list(selection_key(values, update))
        values["elapsed_seconds"] = elapsed()
        values["evaluation_seconds"] = values["elapsed_seconds"] - begin
        report["history"].append(values)
        return values

    def finish(status, reason):
        selected, last = output / "model.npz", output / "last.npz"
        report.update({"status": status, "stop_reason": reason, "elapsed_seconds": elapsed(), "peak_memory": peak,
                       "peak_memory_sum_bytes": sum(peak.values()),
                       "selected_checkpoint_sha256": file_hash(selected) if selected.exists() else None,
                       "last_checkpoint_sha256": file_hash(last) if last.exists() else None,
                       "parent_unchanged": file_hash(parent_path) == plan["parent"]["sha256"]})
        if not report["parent_unchanged"]:
            report.update(status="failed grounding experiment; parent changed", stop_reason="failure")
        write_report()

    stop, optimizer = "update_limit", None
    try:
        model.to(device)
        optimizer = torch.optim.AdamW(list(model.params.values()), lr=learning_rate(1, rate),
                                      betas=tuple(plan["optimizer"]["betas"]), eps=plan["optimizer"]["eps"],
                                      weight_decay=plan["optimizer"]["weight_decay"], foreach=False, fused=False)
        parameters = list(model.params.values())
        initial = evaluate(0, None)
        frozen = mixed_train.score_validation(model, story_batches, instructions, device)
        report["baseline"] = {"language": frozen["language"], "instruction": frozen["instruction"]}
        expected = plan["parent"]
        for name, key in (("language", "language_validation_loss"), ("instruction", "instruction_validation_loss")):
            if expected.get(key) is not None and not math.isclose(
                    frozen[name], expected[key], abs_tol=language_train.VALIDATION_ATOL, rel_tol=language_train.VALIDATION_RTOL):
                raise ValueError("Parent validation did not reproduce on the fixed batches; no update was made.")
        snapshot("model.npz", 0, initial)
        selected_key = selection_key(initial, 0)
        report["selected_checkpoint_sha256"] = file_hash(output / "model.npz")
        write_report()
        # Timed passes compute and discard gradients with a separate generator; no weight changes.
        timing_rng, samples = np.random.default_rng(plan["seeds"]["timing"]), []
        warmup = plan["preflight"]["warmup_passes"]
        for index in range(warmup + plan["preflight"]["timed_passes"]):
            begin = elapsed()
            batch = build_batch(stories, grounding, instructions, replay, schedule[index % len(schedule)], timing_rng, plan)
            batch_loss(model, batch, device)[0].backward()
            torch.nn.utils.clip_grad_norm_(parameters, plan["optimizer"]["clip_norm"], foreach=False, error_if_nonfinite=True)
            optimizer.zero_grad(set_to_none=True)
            samples.append(elapsed() - begin)
        evaluations = steps // plan["evaluation"]["interval"]
        timed = sorted(samples[warmup:])
        typical, before = timed[len(timed) // 2], elapsed()
        projected = (before + steps * (typical + plan["preflight"]["step_allowance_seconds"])
                     + evaluations * initial["evaluation_seconds"])
        report["preflight"] = {"warmup_pass_seconds": samples[:warmup], "timed_pass_seconds": samples[warmup:],
                               "typical_pass_seconds": typical, "evaluation_seconds": initial["evaluation_seconds"],
                               "elapsed_before_projection": before, "projected_total_seconds": projected,
                               "limit_seconds": plan["preflight"]["projection_limit_seconds"]}
        if memory_stop():
            finish("not started: memory use already exceeds the frozen limit; no update was made", "preflight_abort")
            return report
        if projected > plan["preflight"]["projection_limit_seconds"]:
            finish("not started: preflight projection exceeds the frozen limit; no update was made", "preflight_abort")
            return report
        report["first_update_completed"] = False
        write_report()
        with log_path.open("x", encoding="utf-8") as log:
            for update in range(1, steps + 1):
                if memory_stop():
                    stop = "memory_limit"
                    break
                if elapsed() >= limits["maximum_wall_seconds"] - limits["wall_reserve_seconds"]:
                    stop = "wall_limit"
                    break
                old_rng = copy.deepcopy(language_rng.bit_generator.state)
                entry = schedule[update - 1]
                batch = build_batch(stories, grounding, instructions, replay, entry, language_rng, plan)
                counts = [int((batch["weight"][batch["stream"] == index] > 0).sum()) for index in range(3)]
                if min(counts) <= 0:
                    raise ValueError("A training stream has no supervised target.")
                if report["training_targets"] + sum(counts) > limits["maximum_supervised_targets"]:
                    language_rng.bit_generator.state = old_rng
                    stop = "target_limit"
                    break
                rate_now = learning_rate(update, rate)
                optimizer.param_groups[0]["lr"] = rate_now
                optimizer.zero_grad(set_to_none=True)
                total, streams, classes = batch_loss(model, batch, device)
                if not math.isfinite(float(total.detach().cpu())) or not all(math.isfinite(value) for value in streams):
                    raise ValueError("Non-finite training loss.")
                total.backward()
                norm = torch.nn.utils.clip_grad_norm_(parameters, plan["optimizer"]["clip_norm"], foreach=False,
                                                      error_if_nonfinite=True)
                if update == 1 and ledger is not None:
                    # The single run is consumed here: the first weight change follows immediately.
                    with ledger.open("x", encoding="utf-8") as destination:
                        json.dump({"run_id": report["run_id"], "plan_sha256": plan_sha256, "output": str(output),
                                   "parent_sha256": plan["parent"]["sha256"],
                                   "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat()},
                                  destination, indent=1)
                        destination.write("\n")
                optimizer.step()
                report["attempted_updates"], report["first_update_completed"] = update, True
                report["training_targets"] += sum(counts)
                for name, count in zip(("language", "grounding", "replay"), counts):
                    report["stream_targets"][name] += count
                train_loss = {"language": streams[0], "grounding": streams[1], "replay": streams[2], **classes}
                log.write(json.dumps({"update": update, "mode": entry["mode"], "learning_rate": rate_now,
                                      "loss": float(total.detach().cpu()), **train_loss,
                                      "gradient_norm": float(norm.detach().cpu()), "targets": counts,
                                      "width": int(batch["tokens"].shape[1]), "elapsed_seconds": elapsed()}) + "\n")
                log.flush()
                report["elapsed_seconds"] = elapsed()
                write_report()
                if update % plan["evaluation"]["interval"] == 0 or update == steps:
                    values = evaluate(update, train_loss)
                    key = selection_key(values, update)
                    if key > selected_key:
                        snapshot("model.npz", update, values)
                        selected_key = key
                        report["selected_update"] = update
                    report["selected_checkpoint_sha256"] = file_hash(output / "model.npz")
                    write_report()
                    print(json.dumps({"stage": STAGE, "update": update, "mode": entry["mode"], "language": values["language"],
                                      "replay": values["replay"]["replayed"],
                                      "grounding": values["grounding_validation"]["loss"],
                                      "json_pairs": values["grounding_validation"]["json"]["pairs_both_exact"],
                                      "value_pairs": values["grounding_validation"]["value"]["pairs_both_exact"],
                                      "training_fit": values["training_fit"]["exact_turns"],
                                      "selected_update": report["selected_update"],
                                      "elapsed_seconds": values["elapsed_seconds"]}), flush=True)
        attempted = report["attempted_updates"]
        if stop != "memory_limit" and report["history"][-1]["update"] != attempted and (
                elapsed() < limits["maximum_wall_seconds"] - limits["wall_reserve_seconds"] / 2):
            # A stop between evaluations still scores its final state inside the wall reserve.
            values = evaluate(attempted, None)
            key = selection_key(values, attempted)
            if key > selected_key:
                snapshot("model.npz", attempted, values)
                selected_key = key
                report["selected_update"] = attempted
        if stop != "memory_limit":
            scored = report["history"][-1] if report["history"][-1]["update"] == attempted else None
            snapshot("last.npz", attempted, scored)
        finish("finished grounding experiment; narrow value reading only, assistant not qualified", stop)
    except BaseException as error:
        # Interrupts count too: the report must say how far the single run got.
        report["failure"] = f"{type(error).__name__}: {error}"[:500]
        try:
            finish("failed grounding experiment; selected checkpoint preserved",
                   "failure" if isinstance(error, Exception) else "interrupted")
        except Exception:
            pass
        raise
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--freeze-directory", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device", choices=("cpu", "mps"), default="mps")
    args = parser.parse_args(argv)
    from . import freeze
    plan, plan_sha256 = load_plan()
    try:
        seals = freeze.verify(args.freeze_directory, 3)
        exam_seal = freeze.read_seal(args.freeze_directory, 1)
        result = fit(plan, plan_sha256, args.output, device=args.device, exam_seal=exam_seal, seals=seals,
                     ledger=args.freeze_directory / "run-ledger.json")
        print(json.dumps({key: result.get(key) for key in (
            "status", "stop_reason", "attempted_updates", "selected_update", "training_targets", "elapsed_seconds",
            "peak_memory", "selected_checkpoint_sha256", "last_checkpoint_sha256", "parent_unchanged")}, allow_nan=False))
    except (OSError, ValueError, RuntimeError, TypeError, KeyError) as error:
        parser.exit(2, f"Grounding run failed: {type(error).__name__}: {error}\n")


if __name__ == "__main__":
    main()
