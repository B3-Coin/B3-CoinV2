# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Bounded mixed story/instruction learning for our own byte/EOD Transformer.

Two story updates precede each instruction update. Validation selects weights;
no independent exam, generation, model download or real tool execution occurs.
"""

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

from .data import InstructionCorpus
from ..language.batches import Corpus
from ..language.checkpoint import MAX_ARCHIVE_BYTES, MAX_STEPS, load_checkpoint, save_checkpoint
from ..language import train as language_train
from ..scratch.checkpoint import _no_duplicate_keys, _reject_constant


STAGE = "mixed-b3-tools"
MAX_SECONDS = 500
MAX_TOKENS = 8_192_000
MAX_MEMORY = 32 * 1024**3
MAX_CHAIN_SECONDS = 1100
MAX_CHUNKS = 2
MAX_MIXED_STEPS = 2000


class ResourceLimitError(RuntimeError):
    def __init__(self, reason):
        super().__init__(reason)
        self.reason = reason


def _file_hash(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _bounded_json(path, maximum=1024 * 1024):
    path = Path(path)
    if path.is_symlink() or not path.is_file() or path.stat().st_size > maximum:
        raise ValueError("Contract must be a bounded regular JSON file.")
    with path.open("rb") as source:
        raw = source.read(maximum + 1)
    if len(raw) > maximum:
        raise ValueError("JSON contract exceeds its byte bound.")
    try:
        value = json.loads(raw, object_pairs_hook=_no_duplicate_keys, parse_constant=_reject_constant)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise ValueError("JSON contract is invalid.") from error
    if type(value) is not dict:
        raise ValueError("JSON contract must be an object.")
    return value, hashlib.sha256(raw).hexdigest()


def _plan_hash():
    return _bounded_json(Path(__file__).with_name("plan.json"), 65536)[1]


def _plan_document():
    return _bounded_json(Path(__file__).with_name("plan.json"), 65536)[0]


def _validate_plan(stories, instructions, training, state):
    plan = _plan_document()
    expected = {"schema": "b3-mixed-tool-plan/v1", "stage": STAGE,
                "initial_parent_sha256": state["initial_parent_sha"],
                "inherited_language_steps": state["inherited_language"],
                "inherited_b3_teaching_steps": state["inherited_b3"],
                "language_manifest_sha256": stories.manifest_hash,
                "instruction_corpus_sha256": instructions.sha256,
                "source_curriculum_sha256": instructions.source_curriculum_sha256,
                "source_baseline": instructions.baseline_commit,
                "projection_source_sha256": instructions.projection_source_sha256,
                "language_batch_size": 4, "instruction_batch_size": 8,
                "language_context_length": training["story_context_length"],
                "instruction_context_cap": training["instruction_context_length"],
                "learning_rate": training["learning_rate"], "clip_norm": 1.0,
                "evaluation_interval": training["evaluation_interval"],
                "maximum_chunks": MAX_CHUNKS, "maximum_updates_per_chunk": 1000,
                "maximum_attempted_chain_updates": MAX_MIXED_STEPS,
                "maximum_wall_seconds_per_chunk": MAX_SECONDS,
                "maximum_chain_wall_seconds": MAX_CHAIN_SECONDS,
                "maximum_training_targets_per_chunk": MAX_TOKENS, "maximum_memory_bytes": MAX_MEMORY}
    if any(type(plan.get(key)) is not type(value) or plan[key] != value for key, value in expected.items()):
        raise ValueError("Initial lineage, corpus, settings or resource bounds do not match the frozen mixed plan.")


def mixed_component_hashes():
    here = Path(__file__).resolve().parent
    paths = {"instruction_data.py": here / "data.py", "protocol.py": here / "protocol.py",
             "source_curriculum_loader.py": here.parent / "teaching/data.py",
             "language_train.py": here.parent / "language/train.py"}
    return {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in paths.items()}


def implementation_hash():
    digest = hashlib.sha256(Path(__file__).read_bytes())
    digest.update(json.dumps(language_train.component_hashes(), sort_keys=True).encode("ascii"))
    digest.update(json.dumps(mixed_component_hashes(), sort_keys=True).encode("ascii"))
    return digest.hexdigest()


def _integer(value, maximum, name):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError(f"Invalid {name} counter.")
    return value


def _loss(value, name, optional=False):
    if optional and value is None:
        return None
    if type(value) not in (int, float) or not math.isfinite(value) or value < 0:
        raise ValueError(f"Invalid finite {name} loss.")
    return float(value)


def _rng(state, seed):
    if (type(state) is not dict or set(state) != {"bit_generator", "state", "has_uint32", "uinteger"}
            or state["bit_generator"] != "PCG64" or type(state["state"]) is not dict
            or set(state["state"]) != {"state", "inc"}
            or any(type(value) is not int or not 0 <= value < 2**128 for value in state["state"].values())
            or state["state"]["inc"] % 2 != 1
            or type(state["has_uint32"]) is not int or state["has_uint32"] not in (0, 1)
            or type(state["uinteger"]) is not int or not 0 <= state["uinteger"] < 2**32):
        raise ValueError("Invalid mixed batch RNG state.")
    rng = np.random.default_rng(seed)
    rng.bit_generator.state = copy.deepcopy(state)
    if rng.bit_generator.state != state:
        raise ValueError("Mixed batch RNG did not restore exactly.")
    return rng


def _optimizer_steps(model, optimizer, step):
    if (step == 0 and optimizer.state) or (step > 0 and (
            set(optimizer.state) != set(model.params.values())
            or any(state["step"].item() != step for state in optimizer.state.values()))):
        raise ValueError("AdamW steps do not match the saved objective update.")


def _limits(steps, learning_rate, evaluation_interval, max_seconds, max_tokens):
    for name, value, maximum in (("steps", steps, 1000), ("evaluation_interval", evaluation_interval, 1000),
                                ("max_seconds", max_seconds, MAX_SECONDS), ("max_tokens", max_tokens, MAX_TOKENS)):
        if type(value) is not int or not 1 <= value <= maximum:
            raise ValueError(f"{name} must be an integer between 1 and {maximum}.")
    if type(learning_rate) not in (int, float) or not math.isfinite(learning_rate) or not 0 < learning_rate <= 0.001:
        raise ValueError("Mixed learning rate must be finite and in (0, 0.001].")


def _identity(stories, instructions):
    return {"language_corpus_manifest_hash": stories.manifest_hash,
            "split_hashes": {key: value["sha256"] for key, value in stories.manifest["splits"].items()},
            "instruction_corpus_sha256": instructions.sha256,
            "source_curriculum_sha256": instructions.source_curriculum_sha256,
            "projection_source_sha256": instructions.projection_source_sha256,
            "baseline_commit": instructions.baseline_commit, "source_hashes": instructions.source_hashes}


def _parent(path, expected_sha, versions, stories, instructions, training, steps):
    path = Path(path)
    if type(expected_sha) is not str or re.fullmatch(r"[0-9a-f]{64}", expected_sha) is None:
        raise ValueError("Complete expected parent SHA-256 is required.")
    if path.is_symlink() or not path.is_file() or path.stat().st_size > MAX_ARCHIVE_BYTES:
        raise ValueError("Parent must be a bounded regular checkpoint file.")
    if _file_hash(path) != expected_sha:
        raise ValueError("Approved parent SHA-256 does not match.")
    model, optimizer, metadata = load_checkpoint(path, device="cpu")
    if _file_hash(path) != expected_sha:
        raise ValueError("Parent changed while loading.")
    training.update(story_context_length=min(2048, model.config.context_length),
                    instruction_context_length=min(1024, model.config.context_length))
    if (metadata.get("experimental") is not True
            or metadata.get("model_family_origin") != language_train.MODEL_FAMILY_ORIGIN
            or metadata.get("component_hashes") != language_train.component_hashes()):
        raise ValueError("Parent is not from the compatible own random-start model family.")
    inherited_language = _integer(metadata.get("inherited_language_steps"), MAX_STEPS, "inherited language")
    if metadata.get("stage") == "b3-supervised-answers":
        previous = metadata.get("runtime")
        if (type(previous) is not dict or previous.get("device") not in ("cpu", "mps")
                or any(previous.get(key) != versions[key] for key in ("python", "numpy", "torch", "precision"))
                or metadata.get("optimizer_reset") is not True
                or metadata.get("curriculum_sha256") != instructions.source_curriculum_sha256
                or metadata.get("baseline_commit") != instructions.baseline_commit
                or metadata.get("source_hashes") != instructions.source_hashes):
            raise ValueError("B3 parent runtime or reviewed source provenance is incompatible.")
        inherited_b3 = _integer(metadata.get("selected_or_saved_step"), MAX_STEPS, "inherited B3 teaching")
        _optimizer_steps(model, optimizer, inherited_b3)
        optimizer = torch.optim.AdamW(list(model.params.values()), lr=training["learning_rate"],
                                     weight_decay=0.01, foreach=False, fused=False)
        state = {"base_step": 0, "base_tokens": 0, "story_tokens": 0, "instruction_tokens": 0,
                 "chunk_index": 1, "chain_elapsed": 0.0, "inherited_language": inherited_language,
                 "inherited_b3": inherited_b3, "initial_parent_sha": expected_sha,
                 "rng": np.random.default_rng(model.config.seed), "reset": True, "saved_validation": None}
    elif metadata.get("stage") == STAGE:
        expected_contract = {**_identity(stories, instructions), "runtime": versions, "training": training,
                             "implementation_hash": implementation_hash(), "plan_sha256": _plan_hash(),
                             "mixed_component_hashes": mixed_component_hashes()}
        if any(metadata.get(key) != value for key, value in expected_contract.items()):
            raise ValueError("Mixed continuation code, runtime, corpus or plan does not match.")
        base = _integer(metadata.get("selected_or_saved_step"), MAX_MIXED_STEPS, "mixed update")
        tokens = _integer(metadata.get("training_tokens_at_save"), base * 8192, "mixed targets")
        story_tokens = _integer(metadata.get("story_tokens_at_save"), (base - base // 3) * 8192, "story targets")
        instruction_tokens = _integer(metadata.get("instruction_tokens_at_save"), (base // 3) * 8192, "instruction targets")
        story_updates = _integer(metadata.get("story_updates_at_save"), MAX_MIXED_STEPS, "story update")
        instruction_updates = _integer(metadata.get("instruction_updates_at_save"), MAX_MIXED_STEPS, "instruction update")
        if (tokens != story_tokens + instruction_tokens
                or story_updates != base - base // 3 or instruction_updates != base // 3
                or story_tokens < story_updates * 4 or instruction_tokens < instruction_updates * 8
                or metadata.get("initial_objective_reset") is not True
                or base + steps > MAX_MIXED_STEPS):
            raise ValueError("Mixed continuation target, schedule or reset counters are inconsistent.")
        local = _integer(metadata.get("run_local_step"), 1000, "local mixed")
        previous_base = _integer(metadata.get("base_mixed_step"), MAX_MIXED_STEPS, "base mixed")
        local_tokens = _integer(metadata.get("run_local_training_tokens_at_save"), local * 8192, "local targets")
        previous_tokens = _integer(metadata.get("base_mixed_training_tokens"), MAX_MIXED_STEPS * 8192, "base targets")
        if local + previous_base != base or local_tokens + previous_tokens != tokens:
            raise ValueError("Mixed continuation local/global counters are inconsistent.")
        _optimizer_steps(model, optimizer, base)
        group = optimizer.param_groups[0]
        if (group["lr"] != training["learning_rate"] or group["betas"] != (0.9, 0.999)
                or group["eps"] != 1e-8 or group["weight_decay"] != 0.01
                or any(group.get(key) is not False for key in
                       ("amsgrad", "maximize", "capturable", "differentiable", "foreach", "fused"))):
            raise ValueError("Mixed continuation AdamW configuration is incompatible.")
        previous_report, _ = _bounded_json(path.with_name("report.json"))
        if (previous_report.get("stage") != STAGE
                or not str(previous_report.get("status", "")).startswith("finished mixed experiment")
                or expected_sha not in (previous_report.get("selected_checkpoint_sha256"), previous_report.get("last_checkpoint_sha256"))
                or previous_report.get("implementation_hash") != expected_contract["implementation_hash"]
                or previous_report.get("plan_sha256") != expected_contract["plan_sha256"]):
            raise ValueError("Mixed continuation requires its matching final parent report.")
        chain_elapsed = previous_report.get("chain_elapsed_seconds")
        if type(chain_elapsed) not in (int, float) or not math.isfinite(chain_elapsed) or not 0 <= chain_elapsed < MAX_CHAIN_SECONDS:
            raise ValueError("Invalid parent chain wall-time accounting.")
        chunk = _integer(metadata.get("chunk_index"), MAX_CHUNKS, "mixed chunk")
        if chunk >= MAX_CHUNKS or previous_report.get("chunk_index") != chunk:
            raise ValueError("At most two mixed chunks are permitted.")
        state = {"base_step": base, "base_tokens": tokens, "story_tokens": story_tokens,
                 "instruction_tokens": instruction_tokens, "chunk_index": chunk + 1,
                 "chain_elapsed": float(chain_elapsed), "inherited_language": inherited_language,
                 "inherited_b3": _integer(metadata.get("inherited_b3_teaching_steps"), MAX_STEPS, "inherited B3 teaching"),
                 "initial_parent_sha": metadata.get("initial_b3_parent_sha256"),
                 "rng": _rng(metadata.get("batch_rng_state"), model.config.seed), "reset": False,
                 "saved_validation": metadata.get("validation_loss_at_save")}
        _loss(state["saved_validation"], "saved combined validation", optional=True)
        language_loss = _loss(metadata.get("language_validation_loss_at_save"), "saved story validation", optional=True)
        instruction_loss = _loss(metadata.get("instruction_validation_loss_at_save"), "saved instruction validation", optional=True)
        if ((state["saved_validation"] is None and (language_loss is not None or instruction_loss is not None))
                or (state["saved_validation"] is not None and (language_loss is None or instruction_loss is None
                    or not math.isclose(state["saved_validation"], (2 * language_loss + instruction_loss) / 3,
                                        rel_tol=1e-12, abs_tol=1e-12)))):
            raise ValueError("Saved mixed validation components are inconsistent.")
        if type(state["initial_parent_sha"]) is not str or re.fullmatch(r"[0-9a-f]{64}", state["initial_parent_sha"]) is None:
            raise ValueError("Initial B3 parent lineage is missing.")
    else:
        raise ValueError("Parent stage is neither reviewed B3 teaching nor compatible mixed learning.")
    return model, optimizer, metadata, state


def score_validation(model, story_batches, instructions, device, check_resources=None):
    """Keep story and complete-instruction target weighting separate."""
    def checked_stories():
        for batch in story_batches:
            if check_resources is not None:
                check_resources()
            yield batch
            if check_resources is not None:
                check_resources()
    language = language_train.score(model, checked_stories(), device)
    total, targets = 0.0, 0
    with torch.no_grad():
        for batch in instructions.evaluation_batches("validation", 8):
            if check_resources is not None:
                check_resources()
            count = int(batch[2].sum())
            value = model.loss(*(torch.from_numpy(array).to(device) for array in batch)).item()
            _loss(value, "instruction validation")
            if count <= 0:
                raise ValueError("Instruction validation has empty target masks.")
            total += value * count
            targets += count
            if check_resources is not None:
                check_resources()
    if targets <= 0:
        raise ValueError("Instruction validation is empty.")
    instruction = total / targets
    return {"language": language, "instruction": instruction, "combined": (2 * language + instruction) / 3}


def _report(output, report):
    temporary = output / ".report.json.tmp"
    temporary.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(output / "report.json")


def fit(language_corpus_path, curriculum_path, output, *, initialize_from, expected_parent_sha256,
        device="mps", steps=1000, learning_rate=0.0001, evaluation_interval=100,
        max_seconds=MAX_SECONDS, max_tokens=MAX_TOKENS):
    """Start the changed objective, or preserve state for one compatible continuation."""
    _limits(steps, learning_rate, evaluation_interval, max_seconds, max_tokens)
    output = Path(output)
    if output.exists() or output.is_symlink():
        raise FileExistsError("Mixed output exists; a fresh exclusive directory is required.")
    versions, started = language_train.runtime(device), time.monotonic()
    stories, instructions = Corpus(language_corpus_path), InstructionCorpus(curriculum_path)
    if not all(stories.splits.get(split) for split in ("train", "validation", "test")):
        raise ValueError("Verified story train, validation and untouched test splits must be nonempty.")
    if not all(instructions.records.get(split) for split in ("train", "validation")):
        raise ValueError("Reviewed instruction train and validation splits must be nonempty.")
    training = {"story_batch_size": 4, "instruction_batch_size": 8, "schedule": ["language", "language", "instruction"],
                "learning_rate": learning_rate, "weight_decay": 0.01, "clip_norm": 1.0,
                "evaluation_interval": evaluation_interval, "story_validation_batches": 4, "story_validation_seed": 1729}
    model, optimizer, parent, state = _parent(initialize_from, expected_parent_sha256, versions, stories, instructions, training, steps)
    story_context, instruction_context = training["story_context_length"], training["instruction_context_length"]
    instructions = InstructionCorpus(curriculum_path, context_length=instruction_context)
    _validate_plan(stories, instructions, training, state)
    for batch, context in ((4, story_context), (8, instruction_context)):
        if batch * context > 16384 or batch * model.config.heads * context**2 * model.config.layers > 1073741824:
            raise ValueError("Mixed batch exceeds model token or attention bounds.")
    validation_rng = np.random.default_rng(1729)
    story_batches = [stories.batch("validation", validation_rng, 4, story_context) for _ in range(4)]
    rng = state["rng"]
    report = {"stage": STAGE, "status": "running", "experimental": True,
              "objective": "two story updates then one reviewed B3/tool instruction update; no real tool execution",
              "optimizer_reset": state["reset"], "batch_rng_reset": state["reset"], "initial_objective_reset": True,
              "inherited_language_steps": state["inherited_language"], "inherited_b3_teaching_steps": state["inherited_b3"],
              "initial_b3_parent_sha256": state["initial_parent_sha"], "parent_checkpoint_sha256": expected_parent_sha256,
              "parent_implementation_hash": parent.get("implementation_hash"),
              "runtime": versions, "config": model.config.to_dict(), "parameter_count": model.parameter_count,
              "model_family_origin": language_train.MODEL_FAMILY_ORIGIN, "run_id": str(uuid.uuid4()),
              "implementation_hash": implementation_hash(), "component_hashes": language_train.component_hashes(),
              "mixed_component_hashes": mixed_component_hashes(), "plan_sha256": _plan_hash(),
              **_identity(stories, instructions), "source_review": instructions.review,
              "record_counts": {key: len(value) for key, value in instructions.records.items()},
              "training": training, "resource_limits": {"requested_steps": steps, "max_seconds": max_seconds,
                  "max_training_tokens": max_tokens, "max_memory_bytes": MAX_MEMORY, "max_chunks": MAX_CHUNKS,
                  "max_chain_seconds": MAX_CHAIN_SECONDS},
              "chunk_index": state["chunk_index"], "base_mixed_step": state["base_step"],
              "base_mixed_training_tokens": state["base_tokens"], "attempted_steps": 0, "selected_step": 0,
              "attempted_mixed_steps": state["base_step"], "selected_mixed_step": state["base_step"],
              "training_tokens": 0, "cumulative_mixed_training_tokens": state["base_tokens"],
              "story_training_tokens": 0, "instruction_training_tokens": 0,
              "history": [], "best_validation_loss": None, "independent_exam_used": False,
              "test_split_used_for_selection": False, "real_tools_executed": False,
              "limitations": "Validation loss selects weights, not assistant qualification. Tool lessons use synthetic projected results. Caps are checked between complete operations."}
    output.mkdir(parents=True, exist_ok=False)
    peak = language_train.memory(device)
    best, latest_train = None, None

    def observe():
        for key, value in language_train.memory(device).items():
            peak[key] = max(peak[key], value)

    def elapsed():
        return time.monotonic() - started

    def resource_stop():
        observe()
        if max(peak.values()) >= MAX_MEMORY:
            return "memory_limit"
        if elapsed() >= max_seconds or state["chain_elapsed"] + elapsed() >= MAX_CHAIN_SECONDS:
            return "wall_limit"
        return None

    def guard():
        reason = resource_stop()
        if reason:
            raise ResourceLimitError(reason)

    def snapshot(name, local_step, values):
        observe()
        if max(peak.values()) >= MAX_MEMORY:
            raise ResourceLimitError("memory_limit")
        global_step = state["base_step"] + local_step
        info = {key: report[key] for key in ("stage", "experimental", "objective", "run_id", "runtime",
            "implementation_hash", "component_hashes", "mixed_component_hashes", "plan_sha256", "model_family_origin",
            "training", "inherited_language_steps", "inherited_b3_teaching_steps", "initial_b3_parent_sha256",
            "parent_checkpoint_sha256", "parent_implementation_hash", "initial_objective_reset", "optimizer_reset",
            "batch_rng_reset", "chunk_index", "base_mixed_step", "base_mixed_training_tokens")}
        info.update(_identity(stories, instructions))
        info.update({"selected_or_saved_step": global_step, "run_local_step": local_step,
                     "training_tokens_at_save": state["base_tokens"] + report["training_tokens"],
                     "run_local_training_tokens_at_save": report["training_tokens"],
                     "story_updates_at_save": global_step - global_step // 3, "instruction_updates_at_save": global_step // 3,
                     "story_tokens_at_save": state["story_tokens"] + report["story_training_tokens"],
                     "instruction_tokens_at_save": state["instruction_tokens"] + report["instruction_training_tokens"],
                     "validation_loss_at_save": values["combined"] if values else None,
                     "language_validation_loss_at_save": values["language"] if values else None,
                     "instruction_validation_loss_at_save": values["instruction"] if values else None,
                     "batch_rng_state": copy.deepcopy(rng.bit_generator.state), "source_review": instructions.review})
        save_checkpoint(output / name, model, optimizer, info)

    def evaluate(local_step):
        guard()
        values = score_validation(model, story_batches, instructions, device, check_resources=guard)
        for key, value in values.items():
            _loss(value, key)
        report["history"].append({"step": local_step, "mixed_step": state["base_step"] + local_step,
            "train": latest_train, "train_kind": None if local_step == 0 else training["schedule"][(state["base_step"] + local_step - 1) % 3],
            "language_validation": values["language"], "instruction_validation": values["instruction"],
            "validation": values["combined"], "elapsed_seconds": elapsed()})
        return values

    def finish_report(status, reason):
        selected, last = output / "model.npz", output / "last.npz"
        report.update({"status": status, "stop_reason": reason, "elapsed_seconds": elapsed(),
                       "chain_elapsed_seconds": state["chain_elapsed"] + elapsed(), "peak_memory": peak,
                       "selected_checkpoint_available": selected.exists(),
                       "selected_checkpoint_sha256": _file_hash(selected) if selected.exists() else None,
                       "last_checkpoint_sha256": _file_hash(last) if last.exists() and reason != "failure" else None,
                       "last_state_validation_available": last.exists() and reason != "failure" and bool(report["history"])
                           and report["history"][-1]["step"] == report["attempted_steps"],
                       "parent_unchanged": _file_hash(initialize_from) == expected_parent_sha256})
        if not report["parent_unchanged"]:
            report.update(status="failed mixed experiment; selected checkpoint preserved", stop_reason="failure",
                          failure="Parent checkpoint changed externally during the run.")
        _report(output, report)

    try:
        guard()
        optimizer = language_train._move_state(model, optimizer, device)
        guard()
        initial = evaluate(0)
        if state["saved_validation"] is not None and not math.isclose(initial["combined"], state["saved_validation"],
                abs_tol=language_train.VALIDATION_ATOL, rel_tol=language_train.VALIDATION_RTOL):
            raise ValueError("Mixed parent validation did not reproduce on the fixed batches.")
        report["baseline"] = initial
        snapshot("model.npz", 0, initial)
        best = initial["combined"]
        report["best_validation_loss"] = best
        _report(output, report)
        stop = "update_limit"
        for local_step in range(1, steps + 1):
            reason = resource_stop()
            if reason:
                stop = reason
                break
            old_rng = copy.deepcopy(rng.bit_generator.state)
            kind = training["schedule"][(state["base_step"] + local_step - 1) % 3]
            batch = (stories.batch("train", rng, 4, story_context) if kind == "language"
                     else instructions.batch("train", rng, 8))
            count = int(batch[2].sum())
            if count <= 0:
                raise ValueError("Mixed training has empty target masks.")
            if report["training_tokens"] + count > max_tokens:
                rng.bit_generator.state = old_rng
                stop = "token_limit"
                break
            optimizer.zero_grad(set_to_none=True)
            loss = model.loss(*(torch.from_numpy(array).to(device) for array in batch))
            latest_train = _loss(loss.item(), "training")
            loss.backward()
            torch.nn.utils.clip_grad_norm_(list(model.params.values()), 1.0, foreach=False, error_if_nonfinite=True)
            optimizer.step()
            report["attempted_steps"], report["attempted_mixed_steps"] = local_step, state["base_step"] + local_step
            report["training_tokens"] += count
            report["cumulative_mixed_training_tokens"] = state["base_tokens"] + report["training_tokens"]
            report[("story" if kind == "language" else "instruction") + "_training_tokens"] += count
            language_train.synchronize(device)
            guard()
            if local_step % evaluation_interval == 0 or local_step == steps:
                values = evaluate(local_step)
                if values["combined"] < best:
                    snapshot("model.npz", local_step, values)
                    best = values["combined"]
                    report.update(selected_step=local_step, selected_mixed_step=state["base_step"] + local_step,
                                  best_validation_loss=best)
                guard()
                _report(output, report)
                print(json.dumps({"stage": STAGE, "step": local_step, "mixed_step": state["base_step"] + local_step,
                                  **values, "selected_mixed_step": report["selected_mixed_step"], "elapsed_seconds": elapsed()}), flush=True)
    except ResourceLimitError as error:
        stop = error.reason
    except (OSError, ValueError, RuntimeError) as error:
        try:
            observe()
        except RuntimeError:
            pass
        report["failure"] = str(error)[:500]
        finish_report("failed mixed experiment; selected checkpoint preserved", "failure")
        raise
    attempted = report["attempted_steps"]
    try:
        reason = resource_stop()
        if reason:
            stop = reason
        if report["history"] and report["history"][-1]["step"] != attempted and stop not in ("memory_limit", "wall_limit"):
            values = evaluate(attempted)
            if best is None or values["combined"] < best:
                snapshot("model.npz", attempted, values)
                best = values["combined"]
                report.update(selected_step=attempted, selected_mixed_step=state["base_step"] + attempted,
                              best_validation_loss=best)
        current = None
        if report["history"] and report["history"][-1]["step"] == attempted:
            entry = report["history"][-1]
            current = {"language": entry["language_validation"], "instruction": entry["instruction_validation"],
                       "combined": entry["validation"]}
        reason = resource_stop()
        if reason:
            stop = reason
        if stop not in ("memory_limit", "wall_limit") and (output / "model.npz").exists():
            snapshot("last.npz", attempted, current)
            language_train.synchronize(device)
            reason = resource_stop()
            if reason:
                stop = reason
        finish_report("finished mixed experiment; assistant and tools not qualified", stop)
    except ResourceLimitError as error:
        finish_report("finished mixed experiment; assistant and tools not qualified", error.reason)
    except (OSError, ValueError, RuntimeError) as error:
        report["failure"] = str(error)[:500]
        finish_report("failed mixed experiment; selected checkpoint preserved", "failure")
        raise
    return report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--language-corpus", type=Path, required=True)
    parser.add_argument("--curriculum", type=Path, default=Path(__file__).with_name("curriculum.json"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--initialize-from", type=Path, required=True)
    parser.add_argument("--expected-parent-sha256", required=True)
    parser.add_argument("--device", choices=("cpu", "mps"), default="mps")
    parser.add_argument("--steps", type=int, default=1000)
    parser.add_argument("--learning-rate", type=float, default=0.0001)
    parser.add_argument("--evaluation-interval", type=int, default=100)
    parser.add_argument("--max-seconds", type=int, default=MAX_SECONDS)
    parser.add_argument("--max-tokens", type=int, default=MAX_TOKENS)
    args = parser.parse_args(argv)
    try:
        result = fit(args.language_corpus, args.curriculum, args.output, initialize_from=args.initialize_from,
                     expected_parent_sha256=args.expected_parent_sha256, device=args.device, steps=args.steps,
                     learning_rate=args.learning_rate, evaluation_interval=args.evaluation_interval,
                     max_seconds=args.max_seconds, max_tokens=args.max_tokens)
        print(json.dumps(result, allow_nan=False))
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        parser.exit(2, f"Mixed learning failed: {error}\n")


if __name__ == "__main__":
    main()
