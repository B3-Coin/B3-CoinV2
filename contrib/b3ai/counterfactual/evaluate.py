# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Record genuine student generations, then score them mechanically in a separate pass.

Recording reads only each case's question and supplied result. Scoring reads the
saved raw outputs and the grading answers. Nothing is repaired, extracted,
substituted, re-sampled or selected; the first complete recording is the record.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import time

import numpy as np

from . import data as grounding_data
from .data import ACTION, FIELDS, PACKAGE, REPOSITORY, UNKNOWN, Exam, GroundingCorpus, prompt_text
from ..language.batches import EOD_ID

SCHEMA = "b3-counterfactual-grounding-recording/v1"
SCORE_SCHEMA = "b3-counterfactual-grounding-scores/v1"
BUDGET_SECONDS = 1800
MAX_MEMORY = 32 * 1024**3
GENERATION = {"temperature": 0.0, "seed": 1729, "maximum_new_tokens": 256}
FILES = ("gate.json", "probes.json", "tracked-exam.json", "validation-generations.json", "bare-diagnostic.json",
         "teacher-forced.json")
TRACKED_EXAM = "contrib/b3ai/tool_learning/exam.json"


def _sha(text):
    return hashlib.sha256(text.encode("utf-8") if type(text) is str else text).hexdigest()


def prompt_inputs(cases):
    """The only fields that may reach generation: no label, role or pairing information."""
    return [{"id": case["id"], "question": case["question"], "envelope": case["envelope"]} for case in cases]


def generate(model, item, generator):
    prompt = prompt_text(item["question"], item["envelope"])
    record = {"id": item["id"], "prompt": prompt, "prompt_sha256": _sha(prompt), "prior_call_supplied_by_host": ACTION}
    try:
        record.update(generator(model, prompt, **GENERATION))
    except Exception as error:
        # A failed generation is recorded and scored as a failure; it is never retried.
        record.update({"error": f"{type(error).__name__}: {error}"[:200], "text": None, "token_ids": None,
                       "valid_utf8": None, "finish_reason": None})
    return record


def _write(path, document):
    with Path(path).open("x", encoding="utf-8") as destination:
        json.dump(document, destination, ensure_ascii=False, indent=1, allow_nan=False)
        destination.write("\n")
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def record_run(checkpoint, expected_sha256, label, output, *, device="mps", freeze_directory=None, run_report=None,
               plan=None, plan_sha256=None, repository=REPOSITORY, generator=None, exam_path=None, sections=FILES):
    """Generate every frozen probe once, gate first, each section into its own exclusive file."""
    import torch
    from . import train as grounding_train
    from ..language import train as language_train
    from ..language.checkpoint import load_checkpoint
    from ..language.evaluate import continuation
    from ..tool_learning import evaluate as tool_evaluate
    from ..tool_learning.data import InstructionCorpus

    frozen_generator = generator is None
    generator = generator or continuation
    checkpoint, output, repository = Path(checkpoint), Path(output), Path(repository)
    if label not in ("before", "after") or output.exists() or output.is_symlink():
        raise FileExistsError("Recording needs a known label and a fresh exclusive directory.")
    if label == "after" and run_report is None:
        raise ValueError("The after recording needs the report of the single run.")
    if plan is None:
        plan, plan_sha256 = grounding_train.load_plan()
    seals, baseline = None, None
    if freeze_directory is not None:
        from . import freeze
        # One recording per checkpoint: its place inside the freeze directory is fixed.
        if output.resolve() != (Path(freeze_directory) / label).resolve():
            raise ValueError("A sealed recording must be written to the freeze directory under its label.")
        seals = freeze.verify(freeze_directory, 2 if label == "before" else 3)
        if label == "after":
            baseline = json.loads((Path(freeze_directory) / "before" / "recording.json").read_text(encoding="utf-8"))
    if grounding_train.file_hash(checkpoint) != expected_sha256:
        raise ValueError("Checkpoint differs from the expected state.")
    versions = language_train.runtime(device)
    model, _, metadata = load_checkpoint(checkpoint, device=device)
    if label == "before":
        if expected_sha256 != plan["parent"]["sha256"] or metadata.get("stage") != plan["parent"]["stage"]:
            raise ValueError("The before recording must use the frozen parent.")
    else:
        report = json.loads(Path(run_report).read_text(encoding="utf-8"))
        if (metadata.get("stage") != grounding_train.STAGE or report.get("selected_checkpoint_sha256") != expected_sha256
                or report.get("plan_sha256") != plan_sha256 or metadata.get("run_id") != report.get("run_id")):
            raise ValueError("The after recording must use the checkpoint selected by the single run.")
    if metadata.get("component_hashes") != language_train.component_hashes() or metadata.get("experimental") is not True:
        raise ValueError("Recording requires our compatible experimental student.")
    model.eval()
    exam = Exam(exam_path)
    grounding = GroundingCorpus(repository / plan["data"]["grounding_curriculum"])
    instructions = InstructionCorpus(repository / plan["data"]["instruction_corpus"], repository=repository)
    stories = grounding_train.Corpus(repository / plan["data"]["language_corpus"])
    if (stories.manifest_hash != plan["data"]["language_manifest_sha256"]
            or grounding.sha256 != plan["data"]["grounding_curriculum_sha256"]
            or instructions.sha256 != plan["data"]["instruction_corpus_sha256"]):
        raise ValueError("Story, replay or grounding data differs from the frozen plan.")
    header = {"schema": SCHEMA, "label": label, "experimental": True, "simulated_only": True,
              "live_tools_executed": False, "training_performed": False, "outputs_repaired": False,
              "checkpoint_sha256": expected_sha256, "checkpoint_stage": metadata.get("stage"),
              "exam_sha256": exam.sha256, "plan_sha256": plan_sha256, "seals": seals,
              "implementation_hash": grounding_train.implementation_hash(), "runtime": versions,
              "generation": GENERATION, "budget_seconds_per_section": BUDGET_SECONDS,
              "generator": "frozen language.evaluate.continuation" if frozen_generator
                           else "substituted generator (test or rehearsal only; not a student recording)"}
    if baseline is not None and any(baseline.get(key) != header[key] for key in (
            "exam_sha256", "plan_sha256", "implementation_hash", "generation", "runtime", "generator")):
        raise ValueError("The after recording must use the same code, exam, settings and runtime as the before recording.")
    output.mkdir(parents=True, exist_ok=False)
    peak = language_train.memory(device)

    def limited(started):
        for key, value in language_train.memory(device).items():
            peak[key] = max(peak[key], value)
        if max(max(peak.values()), sum(peak.values())) >= MAX_MEMORY:
            return "memory_limit"
        return "wall_limit" if time.monotonic() - started >= BUDGET_SECONDS else None

    def section(name, role, items):
        started, records, status = time.monotonic(), [], "complete"
        try:
            for item in items:
                reason = limited(started)
                if reason is not None:
                    status = f"incomplete: {reason}"
                    break
                records.append(generate(model, item, generator))
        except BaseException as error:
            status = f"incomplete: {type(error).__name__}"
            raise
        finally:
            # Whatever was generated is kept, even when the section is interrupted.
            digest = _write(output / name, {**header, "role": role, "status": status, "cases": len(items),
                                            "elapsed_seconds": time.monotonic() - started, "records": records})
            print(json.dumps({"section": name, "status": status, "records": len(records), "sha256": digest,
                              "seconds": round(time.monotonic() - started, 1)}), flush=True)
        return digest

    hashes = {}
    by_role = {role: prompt_inputs([case for case in exam.cases if case["role"] == role]) for role in grounding_data.ROLES}
    if "gate.json" in sections:
        hashes["gate.json"] = section("gate.json", "gate", by_role["gate"])
    if "probes.json" in sections:
        hashes["probes.json"] = section("probes.json", "missing-data", by_role["missing-data"])
    if "tracked-exam.json" in sections:
        tracked, raw = tool_evaluate.read_exam(repository / TRACKED_EXAM, instructions)
        started, outputs, status = time.monotonic(), [], "complete"
        for row in tracked["questions"]:
            if limited(started) is not None:
                status = "incomplete: resource limit"
                break
            try:
                outputs.append(tool_evaluate.trajectory(model, row, generator=generator,
                                                        check_resources=lambda: limited(started)))
            except Exception as error:
                # An exploratory probe must not cost the later sections.
                outputs.append({"id": row["id"], "category": row["category"], "blocked": True, "decision": None,
                                "followup": None, "correct_tool_choice": None,
                                "error": f"{type(error).__name__}: {error}"[:200]})
        hashes["tracked-exam.json"] = _write(output / "tracked-exam.json", {
            **header, "role": "retention: tracked 32-case tool exam; exploratory, previously seen, not acceptance evidence",
            "tracked_exam_sha256": hashlib.sha256(raw).hexdigest(), "status": status,
            "elapsed_seconds": time.monotonic() - started, "outputs": outputs})
        print(json.dumps({"section": "tracked-exam.json", "status": status, "records": len(outputs)}), flush=True)
    if "validation-generations.json" in sections:
        hashes["validation-generations.json"] = section("validation-generations.json", "validation agreement check",
                                                        prompt_inputs(grounding.turns["validation"]))
    if "bare-diagnostic.json" in sections:
        hashes["bare-diagnostic.json"] = section("bare-diagnostic.json", "bare-diagnostic", by_role["bare-diagnostic"])
    if "teacher-forced.json" in sections:
        rng = np.random.default_rng(plan["seeds"]["language_validation"])
        batches = [stories.batch("validation", rng, 4, min(2048, model.config.context_length)) for _ in range(4)]
        with torch.no_grad():
            values = {"language": language_train.score(model, batches, device),
                      "replay": grounding_train.score_replay(model, instructions, plan["replay_categories"], device),
                      "grounding_validation": grounding_train.score_grounding(model, grounding, "validation", device),
                      "training_fit": grounding_train.score_grounding(model, grounding, "train", device)}
        hashes["teacher-forced.json"] = _write(output / "teacher-forced.json", {**header, "role": "teacher-forced losses", **values})
    if grounding_train.file_hash(checkpoint) != expected_sha256:
        raise ValueError("Checkpoint changed during the recording.")
    _write(output / "recording.json", {**header, "files": hashes, "peak_memory": peak})
    return hashes


# ---- mechanical scoring ------------------------------------------------------------------

def _raw(record):
    """Generated bytes before the learned stop, or None when the answer never stopped."""
    ids = record.get("token_ids")
    if record.get("finish_reason") != "end_of_document" or not ids or ids[-1] != EOD_ID:
        return None
    return bytes(ids[:-1]) if all(0 <= value < 256 for value in ids[:-1]) else None


def _reply(record):
    from ..tool_learning.protocol import validate_action
    try:
        action = validate_action(record["text"], False)
    except (ValueError, TypeError):
        return None
    return action.get("reply")


def grade(record, case, expected):
    """Strict byte-exact pass, lenient protocol pass, and one failure class."""
    label = expected if case["mode"] == "value" else json.loads(expected)["reply"]
    raw = _raw(record)
    stopped, valid = raw is not None, record.get("valid_utf8") is True
    text = record.get("text")
    # The saved text must be the saved tokens; a record that disagrees with itself never passes.
    consistent = not (valid and stopped) or (type(text) is str and text.encode("utf-8") == raw)
    strict = valid and stopped and consistent and raw == (expected + "\n").encode("utf-8")
    stopped = stopped and consistent
    if case["mode"] == "json":
        reply = _reply(record) if valid and stopped else None
    else:
        reply = text.strip() if valid and stopped and type(text) is str and "\n" not in text.strip() else None
    lenient = reply == label
    data = case["envelope"].get("data", {})
    requested = FIELDS[case["field"]][1]
    others = {str(data[key]) for _, key in FIELDS.values() if key in data and key != requested}
    irrelevant = {str(data[key]) for key in ("version", "protocolversion") if key in data}
    tokens = re.findall(r"\d+", text) if type(text) is str else []
    if record.get("error") is not None:
        kind = "generation-error"
    elif not consistent:
        kind = "inconsistent-record"
    elif not stopped:
        kind = "no-learned-stop"
    elif not valid:
        kind = "invalid-utf8"
    elif strict:
        kind = "exact"
    elif lenient:
        kind = "right-answer-format-not-byte-exact"
    elif reply is None:
        kind = "value-present-format-invalid" if label in tokens else "wrong-field-format-invalid" if others & set(tokens) \
            else "not-the-required-format"
    elif reply == UNKNOWN:
        kind = "says-unknown"
    elif not reply.isdigit():
        kind = "value-inside-longer-reply" if label in tokens else "wrong-field-inside-longer-reply" if others & set(tokens) \
            else "non-numeric-reply"
    elif label == UNKNOWN:
        kind = "invented-value"
    elif reply in others:
        kind = "wrong-field"
    elif reply in irrelevant:
        kind = "irrelevant-field"
    elif label.startswith(reply):
        kind = "truncated"
    elif reply.startswith(label):
        kind = "overrun"
    elif len(reply) == len(label):
        kind = "leading-digit-wrong" if reply[1:] == label[1:] else "wrong-digits-same-length"
    else:
        kind = "other-number"
    return {"id": case["id"], "expected": expected, "strict": bool(strict), "lenient": bool(lenient), "class": kind,
            "contains_expected_value": label in tokens, "contains_other_connection_count": bool(others & set(tokens)),
            "text": text, "finish_reason": record.get("finish_reason"), "generated_tokens": len(record.get("token_ids") or [])}


def _tally(grades, key=None):
    table = {}
    for item in grades:
        table.setdefault(str(item[key]) if key else "all", []).append(item)
    return {name: {"strict": sum(item["strict"] for item in items), "lenient": sum(item["lenient"] for item in items),
                   "total": len(items)} for name, items in sorted(table.items())}


def score_cases(cases, grading, records, flags=None):
    """Grade every frozen case; a case without a record fails. Pairs need both members."""
    by_id = {record["id"]: record for record in records}
    missing = {"text": None, "token_ids": None, "valid_utf8": None, "finish_reason": None, "error": "no recording"}
    grades = {case["id"]: grade(by_id.get(case["id"], missing), case, grading[case["id"]]) for case in cases}
    answers = [{**grades[case["id"]], "kind": case["kind"], "field": case["field"], "frame": case["frame"]} for case in cases]
    pairs = {}
    for case in cases:
        if case["kind"] == "pair":
            pairs.setdefault((case["pair"], case["mode"]), {})[case["member"]] = grades[case["id"]]
    rows = []
    for (name, mode), members in pairs.items():
        first, second = members["a"], members["b"]
        rows.append({"pair": name, "mode": mode, "members": [first["id"], second["id"]],
                     "strict": first["strict"] and second["strict"], "lenient": first["lenient"] and second["lenient"],
                     "identical_output": first["text"] is not None and first["text"] == second["text"],
                     "classes": [first["class"], second["class"]], **((flags or {}).get(name, {}))})
    classes = {}
    for item in answers:
        classes[item["class"]] = classes.get(item["class"], 0) + 1
    summary = {"answers": _tally(answers)["all"], "answer_classes": dict(sorted(classes.items())),
               "answers_by_kind": _tally(answers, "kind"), "answers_by_field": _tally(answers, "field"),
               "answers_containing_expected_value": sum(item["contains_expected_value"] for item in answers),
               "answers_containing_other_connection_count": sum(item["contains_other_connection_count"] for item in answers)}
    if rows:
        summary.update({"pairs": _tally(rows)["all"], "pairs_identical_output": sum(row["identical_output"] for row in rows),
                        "pairs_outputs_differ": sum(not row["identical_output"] for row in rows)})
        for key in ("mode", "frame", "frame_group", "field", "unit_kind", "single_candidate", "requested_key_index"):
            if rows and key in rows[0]:
                summary[f"pairs_by_{key}"] = _tally(rows, key)
    return summary, answers, rows


def representative(rows, answers, limit=3):
    """Pre-declared rule: the first failing pairs in frozen exam order for each failure class."""
    by_id, chosen = {item["id"]: item for item in answers}, {}
    for row in rows:
        if row["strict"]:
            continue
        kind = next(kind for kind in row["classes"] if kind != "exact")
        if len(chosen.setdefault(kind, [])) < limit:
            chosen[kind].append({"pair": row["pair"], "members": [
                {key: by_id[identity][key] for key in ("expected", "text", "class", "finish_reason")}
                for identity in row["members"]]})
    return chosen


def score_tracked(document):
    """Mechanical retention facts only; reply quality is not machine-graded."""
    outputs = document["outputs"]
    generations = [row[key] for row in outputs for key in ("decision", "followup") if row.get(key)]
    summary = {"cases": len(outputs), "generations": len(generations), "errors": sum("error" in row for row in outputs),
               "learned_stop": sum(item["finish_reason"] == "end_of_document" for item in generations),
               "valid_decision_objects": sum("parsed_decision" in row for row in outputs),
               "decisions": sum(row.get("decision") is not None for row in outputs),
               "valid_followup_objects": sum("parsed_followup" in row for row in outputs),
               "followups": sum(row.get("followup") is not None for row in outputs),
               "correct_tool_choices": sum(row.get("correct_tool_choice") is True for row in outputs),
               "tool_request_cases": sum(row["category"] == "tool-request" for row in outputs), "by_category": {}}
    for row in outputs:
        entry = summary["by_category"].setdefault(row["category"], {"cases": 0, "blocked": 0, "valid_objects": 0})
        entry["cases"] += 1
        entry["blocked"] += bool(row["blocked"])
        entry["valid_objects"] += ("parsed_followup" in row) if row["category"] == "result-reading" else ("parsed_decision" in row)
    return summary


def score_directory(directory, exam_path=None, repository=REPOSITORY, plan=None):
    """Second phase: read the saved raw recordings and write scores; no model is loaded."""
    from . import train as grounding_train
    directory = Path(directory)
    exam = Exam(exam_path)
    if plan is None:
        plan, _ = grounding_train.load_plan()
    grounding = GroundingCorpus(Path(repository) / plan["data"]["grounding_curriculum"])
    documents = {name: json.loads((directory / name).read_text(encoding="utf-8")) for name in FILES if (directory / name).exists()}
    for name, document in documents.items():
        # Scores are only meaningful against the exam and settings the recording was made with.
        if (document["exam_sha256"] != exam.sha256 or document["generation"] != GENERATION
                or document["label"] != documents["gate.json"]["label"]
                or document["checkpoint_sha256"] != documents["gate.json"]["checkpoint_sha256"]):
            raise ValueError(f"Recording section {name} does not match the frozen exam, settings or checkpoint.")
    flags = exam.document["pair_flags"]
    result = {"schema": SCORE_SCHEMA, "label": documents["gate.json"]["label"],
              "checkpoint_sha256": documents["gate.json"]["checkpoint_sha256"], "exam_sha256": exam.sha256,
              "recording_sha256": {name: hashlib.sha256((directory / name).read_bytes()).hexdigest() for name in documents},
              "gate_rule": exam.document["gate"], "scoring": "mechanical; no AI or human review entered any score"}
    cases = {role: [case for case in exam.cases if case["role"] == role] for role in grounding_data.ROLES}
    summary, answers, rows = score_cases(cases["gate"], exam.grading, documents["gate.json"]["records"], flags)
    complete = documents["gate.json"]["status"] == "complete" and len(documents["gate.json"]["records"]) == 128
    required = exam.document["gate"]["required_pairs"]
    primary = bool(complete and summary["pairs"]["strict"] >= required)
    secondary = bool(complete and summary["pairs"]["lenient"] >= required)
    result["generator"] = documents["gate.json"].get("generator")
    result["headline"] = (
        f"GATE {'MET' if primary else 'NOT MET'}: {summary['pairs']['strict']} of 64 held-out pairs byte-exact with learned stop "
        f"(need {required}); {summary['pairs']['lenient']} of 64 under protocol validity."
        + ("" if primary == secondary else " The primary and secondary rules fall on opposite sides of the gate.")
        + ("" if complete else " The gate recording is incomplete, so the gate is not met."))
    result["gate"] = {**summary, "recording_complete": complete, "required_pairs": required,
                      "passed_primary": primary, "passed_secondary": secondary,
                      "representative_failures": representative(rows, answers), "pair_rows": rows, "answer_rows": answers}
    if "probes.json" in documents:
        summary, answers, _ = score_cases(cases["missing-data"], exam.grading, documents["probes.json"]["records"])
        result["missing_data"] = {"missing_field": summary["answers_by_kind"].get("missing"),
                                  "unavailable": summary["answers_by_kind"].get("unavailable"),
                                  "answer_classes": summary["answer_classes"], "answer_rows": answers}
    if "bare-diagnostic.json" in documents:
        summary, answers, rows = score_cases(cases["bare-diagnostic"], exam.grading, documents["bare-diagnostic.json"]["records"])
        result["bare_diagnostic"] = {"note": "same fixtures as the gate in bare-value mode; not independent evidence, not the gate",
                                     **summary, "answer_rows": answers}
    if "validation-generations.json" in documents:
        cases_validation = grounding.turns["validation"]
        grading = {record["id"]: record["answer"] for record in cases_validation}
        summary, answers, _ = score_cases(cases_validation, grading, documents["validation-generations.json"]["records"])
        vector = "".join("1" if item["strict"] else "0" for item in answers)
        forced = documents.get("teacher-forced.json", {}).get("grounding_validation", {}).get("exact_vector")
        result["validation_generation"] = {
            "note": "validation turns, never the exam; checks that teacher-forced exactness equals genuine greedy generation",
            **summary, "generated_exact_vector": vector, "teacher_forced_exact_vector": forced,
            "agreement": None if forced is None else sum(a == b for a, b in zip(vector, forced))}
    if "tracked-exam.json" in documents:
        result["tracked_exam"] = score_tracked(documents["tracked-exam.json"])
    if "teacher-forced.json" in documents:
        forced = documents["teacher-forced.json"]
        result["teacher_forced"] = {key: forced[key] for key in ("language", "replay")}
        for name in ("grounding_validation", "training_fit"):
            result["teacher_forced"][name] = {key: value for key, value in forced[name].items() if key != "exact_vector"}
    _write(directory / "scores.json", result)
    lines = [f"{result['label'].upper()} - raw student outputs, exactly as generated (Python repr).",
             f"Checkpoint SHA256: {result['checkpoint_sha256']}", f"Exam SHA256: {exam.sha256}", "",
             "GATE (64 short-JSON pairs; byte-exact output with learned stop is the primary rule)"]
    grades = {item["id"]: item for item in result["gate"]["answer_rows"]}
    for row in result["gate"]["pair_rows"]:
        first = next(case for case in cases["gate"] if case["id"] == row["members"][0])
        lines.append(f"{row['pair']}  strict={row['strict']} lenient={row['lenient']} identical={row['identical_output']}")
        lines.append(f"  Question: {first['question']}")
        for member, identity in zip("ab", row["members"]):
            item = grades[identity]
            lines.append(f"  {member}: expected {item['expected']!r}; raw {item['text']!r}; finish {item['finish_reason']}; class {item['class']}")
    with (directory / "answers.txt").open("x", encoding="utf-8") as destination:
        destination.write("\n".join(lines) + "\n")
    return result


def self_test(exam):
    """Synthetic outputs with known scores prove the scorer before any model is recorded."""
    gate = [case for case in exam.cases if case["role"] == "gate"]
    partner = {case["id"]: exam.grading[f"{case['pair']}.{'b' if case['member'] == 'a' else 'a'}.json"] for case in gate}

    def fake(text, finish="end_of_document", valid=True):
        ids = list(text.encode("utf-8")) + ([EOD_ID] if finish == "end_of_document" else [])
        return {"text": text, "token_ids": ids, "valid_utf8": valid, "finish_reason": finish}

    def key_policy(order):
        def answer(case):
            data = case["envelope"]["data"]
            return fake(json.dumps({"reply": str(next(data[key] for key in order if key in data))}, separators=(",", ":")) + "\n")
        return answer

    def extreme(choose):
        def answer(case):
            data, keys = case["envelope"]["data"], {key for _, key in FIELDS.values()}
            values = [data[key] for key in data if key in keys]  # in the result's own field order
            return fake(json.dumps({"reply": str(choose(values))}, separators=(",", ":")) + "\n")
        return answer

    policies = {
        "oracle": lambda case: fake(exam.grading[case["id"]] + "\n"),
        "partner-swapped": lambda case: fake(partner[case["id"]] + "\n"),
        "constant-number": lambda case: fake('{"reply":"250"}\n'),
        "always-total": key_policy(("connections",)),
        "inbound-else-total": key_policy(("connections_in", "connections")),
        "outbound-inbound-total": key_policy(("connections_out", "connections_in", "connections")),
        "first-count-in-result": extreme(lambda values: values[0]),
        "largest-count": extreme(max),
        "right-answer-no-newline": lambda case: fake(exam.grading[case["id"]]),
        "right-answer-never-stops": lambda case: fake(exam.grading[case["id"]] + "\n", finish="length"),
        "bare-number": lambda case: fake(json.loads(exam.grading[case["id"]])["reply"] + "\n"),
        "right-answer-then-extra-text": lambda case: fake(exam.grading[case["id"]] + " ok\n"),
        "empty-output": lambda case: fake(""),
        "invalid-utf8-flag": lambda case: fake(exam.grading[case["id"]] + "\n", valid=False),
        "text-disagrees-with-tokens": lambda case: {**fake(partner[case["id"]] + "\n"), "text": exam.grading[case["id"]] + "\n"},
        "tokens-disagree-with-text": lambda case: {**fake(exam.grading[case["id"]] + "\n"), "text": partner[case["id"]] + "\n"},
        "generation-error": lambda case: {"error": "Non-finite generated logits.", "text": None, "token_ids": None,
                                          "valid_utf8": None, "finish_reason": None},
    }
    table = {}
    for name, policy in policies.items():
        records = [{"id": case["id"], **policy(case)} for case in gate]
        summary, _, _ = score_cases(gate, exam.grading, records)
        table[name] = {"strict_pairs": summary["pairs"]["strict"], "lenient_pairs": summary["pairs"]["lenient"]}
    summary, _, _ = score_cases(gate, exam.grading, [])
    table["no-recording"] = {"strict_pairs": summary["pairs"]["strict"], "lenient_pairs": summary["pairs"]["lenient"]}
    return table


FIELD_BLIND = ("always-total", "inbound-else-total", "outbound-inbound-total", "first-count-in-result", "largest-count")


def check_self_test(table, required=52):
    """The scorer is only trusted when known outputs get their known scores."""
    zero = {"strict_pairs": 0, "lenient_pairs": 0}
    if (table["oracle"] != {"strict_pairs": 64, "lenient_pairs": 64}
            or table["right-answer-no-newline"] != {"strict_pairs": 0, "lenient_pairs": 64}
            or any(table[name]["lenient_pairs"] >= required for name in FIELD_BLIND)
            or any(value != zero for name, value in table.items()
                   if name not in FIELD_BLIND + ("oracle", "right-answer-no-newline"))):
        raise ValueError("Scorer self-test failed: known outputs did not receive their known scores.")
    return table


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    record = commands.add_parser("record")
    record.add_argument("--checkpoint", type=Path, required=True)
    record.add_argument("--expected-checkpoint-sha256", required=True)
    record.add_argument("--label", choices=("before", "after"), required=True)
    record.add_argument("--freeze-directory", type=Path, required=True)
    record.add_argument("--run-report", type=Path)
    record.add_argument("--output", type=Path, required=True)
    record.add_argument("--device", choices=("cpu", "mps"), default="mps")
    score = commands.add_parser("score")
    score.add_argument("--directory", type=Path, required=True)
    commands.add_parser("self-test")
    args = parser.parse_args(argv)
    try:
        if args.command == "record":
            print(json.dumps(record_run(args.checkpoint, args.expected_checkpoint_sha256, args.label, args.output,
                                        device=args.device, freeze_directory=args.freeze_directory,
                                        run_report=args.run_report)))
        elif args.command == "score":
            result = score_directory(args.directory)
            print(json.dumps({"label": result["label"], "headline": result["headline"],
                              "gate_pairs_strict": result["gate"]["pairs"]["strict"],
                              "gate_pairs_lenient": result["gate"]["pairs"]["lenient"],
                              "passed_primary": result["gate"]["passed_primary"],
                              "passed_secondary": result["gate"]["passed_secondary"]}))
        else:
            print(json.dumps(check_self_test(self_test(Exam())), indent=1))
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        parser.exit(2, f"Grounding evaluation failed: {type(error).__name__}: {error}\n")


if __name__ == "__main__":
    main()
