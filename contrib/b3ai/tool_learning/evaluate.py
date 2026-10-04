# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Record genuine student decisions and reply generations with simulated tools."""

import argparse
import hashlib
import json
from pathlib import Path
import time

from ..language.batches import _json
from ..language.checkpoint import MAX_ARCHIVE_BYTES, load_checkpoint
from ..language.evaluate import continuation
from ..language.train import MODEL_FAMILY_ORIGIN, component_hashes, memory, runtime
from ..teaching.evaluate import normalize
from .data import InstructionCorpus
from .protocol import (TOOLS, compact, request_prompt, result_prompt, simulated_dispatch,
                       validate_action, validate_fixture)


def read_exam(path, corpus):
    with Path(path).open("rb") as source:
        raw = source.read(65537)
    if len(raw) > 65536:
        raise ValueError("Tool exam exceeds its size bound.")
    exam = _json(raw)
    generation = exam.get("generation") if type(exam) is dict else None
    if (type(exam) is not dict or exam.get("schema") != "b3-simulated-tool-exam/v1"
            or exam.get("source_curriculum_sha256") != corpus.source_curriculum_sha256
            or exam.get("source_baseline") != corpus.baseline_commit
            or exam.get("generation") != {"maximum_new_byte_tokens": 256, "temperature": 0.0, "seed": 1729}
            or type(generation["maximum_new_byte_tokens"]) is not int
            or type(generation["temperature"]) is not float or type(generation["seed"]) is not int
            or type(exam.get("questions")) is not list or not 1 <= len(exam["questions"]) <= 32):
        raise ValueError("Tool exam does not match the frozen instruction source/settings.")
    seen = {normalize(row["question"]) for rows in corpus.records.values() for row in rows}
    ids = set()
    for row in exam["questions"]:
        if (type(row) is not dict or type(row.get("id")) is not str or not row["id"]
                or len(row["id"]) > 80 or row["id"] in ids
                or type(row.get("tools_enabled")) is not bool):
            raise ValueError("Invalid or duplicate tool exam row.")
        prompt = request_prompt(row.get("question"), row["tools_enabled"])
        question = normalize(row["question"])
        if question in seen:
            raise ValueError("Tool exam wording overlaps training, validation or another exam row.")
        seen.add(question)
        ids.add(row["id"])
        category = row.get("category")
        common = {"id", "category", "question", "tools_enabled"}
        if category == "tool-request":
            if (set(row) != common | {"expected_tool", "fixtures"} or not row["tools_enabled"]
                    or row["expected_tool"] not in TOOLS or type(row["fixtures"]) is not dict
                    or set(row["fixtures"]) != set(TOOLS)):
                raise ValueError("Invalid simulated tool-request case.")
            for tool, fixture in row["fixtures"].items():
                validate_fixture(tool, fixture)
                potential = result_prompt(row["question"], {"tool": tool, "arguments": {}}, fixture)
                if len(potential.encode("utf-8")) + 256 > 2048:
                    raise ValueError("A potential simulated result exceeds the whole student context.")
        elif category == "result-reading":
            if (set(row) != common | {"prior_tool", "fixture"} or not row["tools_enabled"]
                    or row["prior_tool"] not in TOOLS):
                raise ValueError("Invalid supplied-result diagnostic.")
            prompt = result_prompt(row["question"], {"tool": row["prior_tool"], "arguments": {}}, row["fixture"])
        elif category in ("refusal", "b3-fact", "chat"):
            if set(row) != common:
                raise ValueError("Unexpected fields in reply-only exam case.")
        else:
            raise ValueError("Unknown tool exam category.")
        if len(prompt.encode("utf-8")) + 256 > 2048:
            raise ValueError("Whole tool exam prompt and response exceed the student context.")
    return exam, raw


def _parse_generation(generated, tools_enabled):
    if generated.get("valid_utf8") is not True:
        raise ValueError("Raw student output is not valid UTF-8; replacement text is not an action.")
    return validate_action(generated["text"], tools_enabled)


def trajectory(model, row, *, generator=continuation, check_resources=None):
    """No expected action enters prompts or dispatch; record errors without repairs."""
    result = {"id": row["id"], "category": row["category"], "question": row["question"],
              "tools_enabled": row["tools_enabled"], "simulated_only": True,
              "decision": None, "selected_tool": None, "fixture": None, "followup": None,
              "blocked": False, "parse_error": None, "correct_tool_choice": None}
    def limited():
        reason = check_resources() if check_resources is not None else None
        if reason is not None:
            result.update({"blocked": True, "resource_stop": reason})
            return True
        return False
    if limited():
        return result
    if row["category"] == "result-reading":
        action = {"tool": row["prior_tool"], "arguments": {}}
        prompt = result_prompt(row["question"], action, row["fixture"])
        result.update({"prior_call_supplied_by_host": action, "fixture": row["fixture"], "followup_prompt": prompt})
        generated = generator(model, prompt, temperature=0.0, seed=1729, maximum_new_tokens=256)
        result["followup"] = generated
        if limited():
            return result
        try:
            parsed = _parse_generation(generated, False)
            result["parsed_followup"] = parsed
        except ValueError as error:
            result.update({"blocked": True, "parse_error": str(error)})
        return result
    prompt = request_prompt(row["question"], row["tools_enabled"])
    result["decision_prompt"] = prompt
    generated = generator(model, prompt, temperature=0.0, seed=1729, maximum_new_tokens=256)
    result["decision"] = generated
    if limited():
        return result
    try:
        action = _parse_generation(generated, row["tools_enabled"])
        result["parsed_decision"] = action
        if "reply" in action:
            if row["category"] == "tool-request":
                result["correct_tool_choice"] = False
            return result
        result["selected_tool"] = action["tool"]
        if row["category"] != "tool-request":
            result.update({"blocked": True, "parse_error": "This probe has no simulated dispatcher; unexpected request recorded."})
            return result
        # The generated tool chooses the result. expected_tool is grading only.
        actual, fixture = simulated_dispatch(generated["text"], row["fixtures"], row["tools_enabled"])
        result.update({"fixture": fixture, "correct_tool_choice": actual["tool"] == row["expected_tool"]})
        followup_prompt = result_prompt(row["question"], actual, fixture)
        result["followup_prompt"] = followup_prompt
        if limited():
            return result
        followup = generator(model, followup_prompt, temperature=0.0, seed=1729, maximum_new_tokens=256)
        result["followup"] = followup
        if limited():
            return result
        result["parsed_followup"] = _parse_generation(followup, False)
    except ValueError as error:
        result.update({"blocked": True, "parse_error": str(error)})
        if row["category"] == "tool-request" and result["correct_tool_choice"] is None:
            result["correct_tool_choice"] = False
    return result


def run(checkpoint, curriculum_path, exam_path, output, *, expected_checkpoint_sha256, device="mps"):
    checkpoint, output = Path(checkpoint), Path(output)
    if output.exists() or output.is_symlink():
        raise FileExistsError("Tool exam output already exists.")
    if checkpoint.is_symlink() or not checkpoint.is_file() or checkpoint.stat().st_size > MAX_ARCHIVE_BYTES:
        raise ValueError("Tool exam requires a bounded regular own checkpoint.")
    checkpoint_hash = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
    if checkpoint_hash != expected_checkpoint_sha256:
        raise ValueError("Tool exam checkpoint differs from the preselected state.")
    corpus = InstructionCorpus(curriculum_path)
    exam, raw = read_exam(exam_path, corpus)
    versions = runtime(device)
    model, _, metadata = load_checkpoint(checkpoint, device=device)
    if (metadata.get("stage") not in ("b3-supervised-answers", "mixed-b3-tools")
            or metadata.get("experimental") is not True or metadata.get("model_family_origin") != MODEL_FAMILY_ORIGIN
            or metadata.get("component_hashes") != component_hashes()
            or model.config.context_length < 2048):
        raise ValueError("Tool evaluation requires our compatible experimental student.")
    if metadata["stage"] == "mixed-b3-tools" and metadata.get("instruction_corpus_sha256") != corpus.sha256:
        raise ValueError("Selected mixed model and instruction corpus differ.")
    model.eval()
    started = time.monotonic()
    peak = memory(device)
    def resource_stop():
        for key, value in memory(device).items():
            peak[key] = max(peak[key], value)
        if max(peak.values()) >= 32 * 1024**3:
            return "memory_limit"
        if time.monotonic() - started >= 600:
            return "wall_limit"
        return None
    result = {"stage": "raw simulated tool and dialogue exam", "status": "complete; human review pending",
              "experimental": True, "live_tools_executed": False, "training_performed": False,
              "qualification": "Strict JSON and simulator choices are separate from factual answers, general dialogue and live-node qualification.",
              "checkpoint_sha256": checkpoint_hash, "exam_sha256": hashlib.sha256(raw).hexdigest(),
              "instruction_corpus_sha256": corpus.sha256, "source_curriculum_sha256": corpus.source_curriculum_sha256,
              "checkpoint_metadata": metadata, "runtime": versions, "generation": exam["generation"], "outputs": []}
    for row in exam["questions"]:
        if resource_stop() is not None:
            result["status"] = "resource limit; partial exam, no qualification"
            break
        current = trajectory(model, row, check_resources=resource_stop)
        result["outputs"].append(current)
        if current.get("resource_stop") is not None:
            result["status"] = "resource limit; partial exam, no qualification"
            break
        print(json.dumps({"completed_case": row["id"]}), flush=True)
    if hashlib.sha256(checkpoint.read_bytes()).hexdigest() != checkpoint_hash:
        raise ValueError("Student checkpoint changed during the tool exam.")
    if resource_stop() is not None:
        result["status"] = "resource limit; partial exam, no qualification"
    result.update({"elapsed_seconds": time.monotonic() - started, "peak_memory": peak,
                   "mechanical_correct_tool_choices": sum(row["correct_tool_choice"] is True for row in result["outputs"]),
                   "attempted_cases": len(result["outputs"]),
                   "completed_cases": sum(row.get("resource_stop") is None for row in result["outputs"])})
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as destination:
        json.dump(result, destination, ensure_ascii=False, indent=2, allow_nan=False)
        destination.write("\n")
    return {"output": str(output), "cases": len(result["outputs"]), "mechanical_tool_choices": result["mechanical_correct_tool_choices"],
            "human_review": "pending", "live_tools_executed": False}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--expected-checkpoint-sha256", required=True)
    parser.add_argument("--curriculum", type=Path, required=True)
    parser.add_argument("--exam", type=Path, default=Path(__file__).with_name("exam.json"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device", choices=("cpu", "mps"), default="mps")
    args = parser.parse_args(argv)
    try:
        print(json.dumps(run(args.checkpoint, args.curriculum, args.exam, args.output,
                             expected_checkpoint_sha256=args.expected_checkpoint_sha256, device=args.device)))
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, UnicodeError) as error:
        parser.exit(2, f"Simulated tool evaluation failed: {error}\n")


if __name__ == "__main__":
    main()
