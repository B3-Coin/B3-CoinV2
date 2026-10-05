# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Ordered hash seals for the counterfactual experiment.

Seal 1 fixes the exam. Seal 2 fixes all code, data, the plan and the sampling
schedule before any model reads an exam prompt. Seal 3 fixes the baseline
recordings before training. Each seal embeds the hash of the previous one and
is created exclusively; hashes prove identity and order, not correctness.
"""

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess

from . import data as grounding_data
from .data import PACKAGE, REPOSITORY

SCHEMA = "b3-counterfactual-grounding-seal/v1"
SEALS = {1: "seal-1-exam.json", 2: "seal-2-freeze.json", 3: "seal-3-baseline.json"}
# The hash-pinned frozen pipeline plus the unpinned modules this experiment executes.
FROZEN = ("contrib/b3ai/tool_learning/train.py", "contrib/b3ai/tool_learning/data.py",
          "contrib/b3ai/tool_learning/protocol.py", "contrib/b3ai/tool_learning/plan.json",
          "contrib/b3ai/tool_learning/curriculum.json", "contrib/b3ai/tool_learning/exam.json",
          "contrib/b3ai/teaching/data.py", "contrib/b3ai/teaching/b3_curriculum.json",
          "contrib/b3ai/language/model.py", "contrib/b3ai/language/checkpoint.py",
          "contrib/b3ai/language/batches.py", "contrib/b3ai/language/train.py",
          "contrib/b3ai/language/evaluate.py", "contrib/b3ai/language/corpus.py",
          "contrib/b3ai/tool_learning/evaluate.py", "contrib/b3ai/teaching/evaluate.py",
          "contrib/b3ai/scratch/checkpoint.py", "contrib/b3ai/scratch/tokenizer.py",
          "contrib/b3ai/language/__init__.py", "contrib/b3ai/scratch/__init__.py",
          "contrib/b3ai/teaching/__init__.py", "contrib/b3ai/tool_learning/__init__.py")
OUTPUTS = ("before", "run", "after", "run-ledger.json")
REQUIRED = ("__init__.py", "data.py", "train.py", "evaluate.py", "freeze.py", "rehearse.py", "plan.json",
            "curriculum.json", "exam.json", "prior_probes.json", "tests/support.py", "tests/test_data.py",
            "tests/test_train.py", "tests/test_evaluate.py", "tests/test_freeze.py")
BASELINE_FILES = ("gate.json", "probes.json", "tracked-exam.json", "validation-generations.json",
                  "bare-diagnostic.json", "teacher-forced.json", "recording.json", "scores.json", "answers.txt")


def _hash(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def _write(path, document):
    with Path(path).open("x", encoding="utf-8") as destination:
        json.dump(document, destination, ensure_ascii=False, indent=1, allow_nan=False)
        destination.write("\n")
    return _hash(path)


def read_seal(directory, number):
    return json.loads((Path(directory) / SEALS[number]).read_text(encoding="utf-8"))


def _relevant(path, base):
    """Caches and hidden operating-system files are not part of any sealed set."""
    parts = path.relative_to(base).parts
    return path.is_file() and "__pycache__" not in parts and not any(part.startswith(".") for part in parts)


def package_files():
    """Every file of the experiment package; an incomplete set is refused, never sealed."""
    files = {str(path.relative_to(REPOSITORY)): _hash(path) for path in sorted(PACKAGE.rglob("*"))
             if _relevant(path, PACKAGE)}
    prefix = str(PACKAGE.relative_to(REPOSITORY))
    missing = [name for name in REQUIRED if f"{prefix}/{name}" not in files]
    if missing:
        raise ValueError(f"Experiment package files are missing from the sealed set: {missing}")
    return files


def _tree(directory):
    directory = Path(directory)
    return {str(path.relative_to(directory)): _hash(path) for path in sorted(directory.rglob("*"))
            if _relevant(path, directory)}


def _git(*arguments):
    return subprocess.run(["git", "-C", str(REPOSITORY), *arguments], capture_output=True, text=True, check=True,
                          timeout=30).stdout.strip()


def seal_exam(directory):
    """Seal 1: the exam. It records what else existed, so the order is visible, not assumed."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    exam = grounding_data.Exam()
    frames = [hashlib.sha256(grounding_data.normalize_question(frame).encode("utf-8")).hexdigest()
              for frame in grounding_data.FRAMES["exam"]]
    drafts = {name: _hash(PACKAGE / name) for name in ("curriculum.json", "plan.json") if (PACKAGE / name).is_file()}
    return _write(directory / SEALS[1], {
        "schema": SCHEMA, "seal": 1, "created_utc": _now(), "exam": "contrib/b3ai/counterfactual/exam.json",
        "order_note": "Drafts of the training curriculum and plan already existed when this seal was written; "
                      "the seal proves the exam did not change afterwards and before any model read it.",
        "drafts_existing_at_seal": drafts,
        "exam_sha256": exam.sha256, "generator_sha256_at_seal": _hash(PACKAGE / "data.py"),
        "generator_seed": grounding_data.SEEDS["exam"], "prior_probes_sha256": _hash(PACKAGE / "prior_probes.json"),
        "cases": len(exam.cases), "prompt_sha256": exam.prompt_hashes, "numbers": sorted(exam.numbers),
        "frame_sha256": frames, "frame_groups": exam.document["frame_groups"]})


def seal_freeze(directory, rehearsal, evidence=()):
    """Seal 2: everything that could be tuned, before the parent reads any exam prompt."""
    import numpy
    import torch
    from . import evaluate, train
    from ..language.batches import Corpus
    from ..tool_learning.data import InstructionCorpus
    directory, rehearsal = Path(directory), Path(rehearsal)
    first = read_seal(directory, 1)
    if (directory / SEALS[2]).exists():
        raise FileExistsError("Seal 2 already exists.")
    # The full-size rehearsal must be of exactly the code being sealed, and must show
    # that the frozen plan's own go/no-go rule would start the run.
    rehearsed = json.loads((rehearsal / "rehearsal.json").read_text(encoding="utf-8"))
    rehearsal_files = _tree(rehearsal)
    if "rehearsal.json" not in rehearsal_files or "run/report.json" not in rehearsal_files:
        raise ValueError("The rehearsal record is incomplete.")
    if (rehearsed["implementation_hash"] != train.implementation_hash()
            or rehearsed["frozen_plan_sha256"] != train.load_plan()[1]
            or rehearsed["student_loaded"] is not False or rehearsed["frozen_plan_projection"]["would_start"] is not True
            or json.loads((rehearsal / "run/report.json").read_text())["implementation_hash"] != train.implementation_hash()):
        raise ValueError("The rehearsal is not of the code and plan being sealed, or its projection would not start.")
    exam, corpus = grounding_data.Exam(), grounding_data.GroundingCorpus()
    if exam.sha256 != first["exam_sha256"]:
        raise ValueError("The exam changed after it was sealed.")
    if any((directory / name).exists() for name in OUTPUTS):
        raise ValueError("Recordings or a run already exist; the freeze must come first.")
    plan, plan_sha256 = train.load_plan()
    train.validate_plan(plan)
    instructions = InstructionCorpus(REPOSITORY / plan["data"]["instruction_corpus"])
    prior, prior_sha256 = grounding_data.load_prior()
    if grounding_data.build_prior(instructions) != prior:
        raise ValueError("Earlier-probe snapshot differs from its sources.")
    if (grounding_data.render(grounding_data.build_exam(prior)) != (PACKAGE / "exam.json").read_bytes()
            or grounding_data.render(grounding_data.build_curriculum(prior)) != (PACKAGE / "curriculum.json").read_bytes()):
        raise ValueError("Frozen data files are not what the generator produces.")
    separation = grounding_data.cross_check(corpus, exam, instructions, prior)
    train.check_separation(corpus, first)
    replay = train.replay_records(instructions, plan["replay_categories"])
    schedule = grounding_data.build_schedule(corpus, replay, plan)
    schedule_raw = grounding_data.schedule_bytes(schedule)
    schedule_sha256 = hashlib.sha256(schedule_raw).hexdigest()
    stories = Corpus(REPOSITORY / plan["data"]["language_corpus"])
    if (plan["data"]["schedule_sha256"] != schedule_sha256 or plan["data"]["grounding_curriculum_sha256"] != corpus.sha256
            or plan["data"]["exam_sha256"] != exam.sha256 or plan["data"]["prior_probes_sha256"] != prior_sha256
            or plan["data"]["instruction_corpus_sha256"] != instructions.sha256
            or plan["data"]["language_manifest_sha256"] != stories.manifest_hash
            or plan["data"]["language_train_documents"] != len(stories.splits["train"])
            or plan["seeds"]["exam_data"] != grounding_data.SEEDS["exam"]
            or plan["seeds"]["curriculum_data"] != grounding_data.SEEDS["curriculum"]
            or plan["seeds"]["validation_data"] != grounding_data.SEEDS["validation"]
            or plan["gate"]["primary_required_pairs"] != exam.document["gate"]["required_pairs"]):
        raise ValueError("Plan does not pin the current data, seeds, gate and schedule.")
    self_test = evaluate.check_self_test(evaluate.self_test(exam), exam.document["gate"]["required_pairs"])
    exposure = {}
    for entry in schedule:
        for identity in entry["grounding"]:
            exposure[identity] = exposure.get(identity, 0) + 1
    parent = REPOSITORY / plan["parent"]["path"]
    manifest = REPOSITORY / plan["data"]["language_corpus"] / "manifest.json"
    document = {
        "schema": SCHEMA, "seal": 2, "created_utc": _now(), "previous_seal_sha256": _hash(directory / SEALS[1]),
        "git": {"head": _git("rev-parse", "HEAD"), "branch": _git("rev-parse", "--abbrev-ref", "HEAD"),
                "status": _git("status", "--porcelain", "--untracked-files=all").splitlines()},
        "package_files": package_files(), "frozen_files": {name: _hash(REPOSITORY / name) for name in FROZEN},
        "implementation_hash": train.implementation_hash(),
        "parent": {"path": plan["parent"]["path"], "sha256": _hash(parent)},
        "plan_sha256": plan_sha256, "plan": plan, "exam_sha256": exam.sha256, "curriculum_sha256": corpus.sha256,
        "prior_probes_sha256": prior_sha256, "prior_sources": prior["sources"],
        "language_manifest_sha256": _hash(manifest), "schedule_sha256": schedule_sha256,
        "schedule_summary": {"updates": len(schedule), "modes": {mode: sum(entry["mode"] == mode for entry in schedule)
                                                                 for mode in grounding_data.MODES},
                             "grounding_turn_draws": {"minimum": min(exposure.values()), "maximum": max(exposure.values()),
                                                      "turns_drawn": len(exposure)}},
        "separation": separation, "scorer_self_test": self_test,
        "frame_groups": exam.document["frame_groups"],
        "rehearsal": {"directory": str(rehearsal), "summary": rehearsed, "files": rehearsal_files},
        "runtime": {"python": platform.python_version(), "numpy": numpy.__version__, "torch": str(torch.__version__),
                    "platform": platform.platform(), "machine": platform.machine(),
                    "environment": {key: value for key, value in sorted(os.environ.items())
                                    if key.startswith(("PYTORCH_", "OPENBLAS_", "VECLIB_", "OMP_"))}},
        "evidence": {str(path): _hash(path) for path in evidence},
        "outputs_not_yet_existing": list(OUTPUTS),
    }
    if document["parent"]["sha256"] != plan["parent"]["sha256"]:
        raise ValueError("Parent checkpoint differs from the plan.")
    # Nothing is written until every check has passed, so a refused freeze can be retried.
    with (directory / "schedule.json").open("xb") as destination:
        destination.write(schedule_raw)
    return _write(directory / SEALS[2], document)


def seal_baseline(directory):
    """Seal 3: the baseline recordings and scores, before any training update."""
    directory = Path(directory)
    verify(directory, 2)
    if any((directory / name).exists() for name in ("run", "after", "run-ledger.json")):
        raise ValueError("Training already started; the baseline must be sealed first.")
    recordings = _tree(directory / "before")
    if set(recordings) != set(BASELINE_FILES):
        raise ValueError(f"The baseline must hold exactly its recording, scores and transcript: {sorted(recordings)}")
    second = read_seal(directory, 2)
    index = json.loads((directory / "before/recording.json").read_text(encoding="utf-8"))
    gate = json.loads((directory / "before/gate.json").read_text(encoding="utf-8"))
    # The baseline must be the frozen parent, recorded by the sealed code on the sealed exam.
    if (index["label"] != "before" or index["checkpoint_sha256"] != second["parent"]["sha256"]
            or index["exam_sha256"] != second["exam_sha256"] or index["plan_sha256"] != second["plan_sha256"]
            or index["implementation_hash"] != second["implementation_hash"]
            or index["seals"].get("seal_2") != _hash(directory / SEALS[2])
            or not index["generator"].startswith("frozen")
            or any(recordings[name] != digest for name, digest in index["files"].items())
            or gate["status"] != "complete" or len(gate["records"]) != 128):
        raise ValueError("The baseline recording is not the frozen parent under the sealed code, exam and plan.")
    return _write(directory / SEALS[3], {"schema": SCHEMA, "seal": 3, "created_utc": _now(),
                                         "previous_seal_sha256": _hash(directory / SEALS[2]),
                                         "parent_sha256": index["checkpoint_sha256"], "before": recordings})


def verify(directory, upto):
    """Refuse unless every sealed file is byte-identical and the seals chain in order."""
    directory = Path(directory)
    hashes = {}
    first = read_seal(directory, 1)
    if _hash(PACKAGE / "exam.json") != first["exam_sha256"]:
        raise ValueError("The exam differs from seal 1.")
    hashes["seal_1"] = _hash(directory / SEALS[1])
    if upto >= 2:
        second = read_seal(directory, 2)
        if (second["previous_seal_sha256"] != hashes["seal_1"] or package_files() != second["package_files"]
                or {name: _hash(REPOSITORY / name) for name in FROZEN} != second["frozen_files"]
                or _hash(REPOSITORY / second["parent"]["path"]) != second["parent"]["sha256"]
                or _hash(directory / "schedule.json") != second["schedule_sha256"]):
            raise ValueError("Code, data, plan, schedule or parent differs from seal 2.")
        hashes["seal_2"] = _hash(directory / SEALS[2])
    if upto >= 3:
        third = read_seal(directory, 3)
        if third["previous_seal_sha256"] != hashes["seal_2"] or _tree(directory / "before") != third["before"]:
            raise ValueError("Baseline recordings differ from seal 3.")
        hashes["seal_3"] = _hash(directory / SEALS[3])
    return hashes


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("exam", "freeze", "baseline", "verify"))
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--rehearsal", type=Path)
    parser.add_argument("--evidence", type=Path, nargs="*", default=())
    parser.add_argument("--upto", type=int, choices=(1, 2, 3), default=3)
    args = parser.parse_args(argv)
    try:
        if args.command == "exam":
            result = {"seal_1": seal_exam(args.directory)}
        elif args.command == "freeze":
            result = {"seal_2": seal_freeze(args.directory, args.rehearsal, args.evidence)}
        elif args.command == "baseline":
            result = {"seal_3": seal_baseline(args.directory)}
        else:
            result = verify(args.directory, args.upto)
        print(json.dumps(result))
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        parser.exit(2, f"Seal failed: {type(error).__name__}: {error}\n")


if __name__ == "__main__":
    main()
