# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Record real answers to frozen B3 questions outside training and selection."""

import argparse
import hashlib
import json
from pathlib import Path
import unicodedata

from ..language.batches import _json
from ..language.checkpoint import load_checkpoint
from ..language.evaluate import continuation
from ..language.train import MODEL_FAMILY_ORIGIN, runtime
from .data import Curriculum


def normalize(text):
    return " ".join(unicodedata.normalize("NFC", text).casefold().split())


def read_exam(path, curriculum):
    """Check held-out wording before any model generation; answers stay offline."""
    with Path(path).open("rb") as source:
        raw = source.read(65537)
    if len(raw) > 65536:
        raise ValueError("B3 exam exceeds its size bound.")
    exam = _json(raw)
    if (type(exam) is not dict or exam.get("schema") != "b3-held-out-questions/v1"
            or exam.get("curriculum_sha256") != curriculum.sha256
            or exam.get("baseline_commit") != curriculum.baseline_commit
            or type(exam.get("questions")) is not list or not 1 <= len(exam["questions"]) <= 64):
        raise ValueError("B3 exam does not match the reviewed curriculum.")
    generation = exam.get("generation")
    if (type(generation) is not dict or set(generation) != {"maximum_new_byte_tokens", "temperatures", "seed"}
            or type(generation.get("maximum_new_byte_tokens")) is not int
            or not 1 <= generation["maximum_new_byte_tokens"] <= 256
            or generation.get("temperatures") != [0.0, 0.8]
            or any(type(value) not in (int, float) for value in generation["temperatures"])
            or type(generation.get("seed")) is not int or not 0 <= generation["seed"] < 2**63):
        raise ValueError("Unsupported bounded B3 generation settings.")
    # Source-checked answers may repeat across known-fact paraphrases. Questions
    # must not repeat any training or validation wording after normalization.
    topics = {topic["id"]: topic for topic in curriculum.topics}
    seen_questions = {normalize(question) for topic in topics.values()
                      for question in (*topic["questions"], topic["validation_question"])}
    seen_ids = set()
    for row in exam["questions"]:
        if (type(row) is not dict or set(row) != {"id", "topic_id", "text"}
                or type(row["id"]) is not str or not row["id"] or len(row["id"]) > 80
                or row["id"] in seen_ids or type(row["topic_id"]) is not str or row["topic_id"] not in topics
                or type(row["text"]) is not str or not row["text"].strip()
                or len(row["text"].encode("utf-8")) > 256
                or any(ord(char) < 32 for char in row["text"])):
            raise ValueError("Invalid held-out B3 question.")
        normalized = normalize(row["text"])
        if normalized in seen_questions:
            raise ValueError("B3 exam question overlaps teaching or validation wording.")
        seen_questions.add(normalized)
        seen_ids.add(row["id"])
    return exam, raw


def run(checkpoint, curriculum_path, exam_path, output, *, expected_checkpoint_sha256,
        device="mps", greedy_only=False):
    output, checkpoint = Path(output), Path(checkpoint)
    if output.exists() or output.is_symlink():
        raise FileExistsError("B3 exam output already exists.")
    if checkpoint.stat().st_size > 256 * 1024**2:
        raise ValueError("Checkpoint exceeds the archive bound.")
    checkpoint_hash = hashlib.sha256(checkpoint.read_bytes()).hexdigest()
    if checkpoint_hash != expected_checkpoint_sha256:
        raise ValueError("Checkpoint differs from the preselected model.")
    corpus = Curriculum(curriculum_path)
    exam, raw_exam = read_exam(exam_path, corpus)
    versions = runtime(device)
    model, _, metadata = load_checkpoint(checkpoint, device=device)
    if (metadata.get("stage") not in ("language-pilot", "b3-supervised-answers")
            or metadata.get("experimental") is not True
            or metadata.get("model_family_origin") != MODEL_FAMILY_ORIGIN):
        raise ValueError("B3 evaluation requires our experimental model family.")
    if (metadata["stage"] == "b3-supervised-answers"
            and metadata.get("curriculum_sha256") != corpus.sha256):
        raise ValueError("Teaching checkpoint and examined curriculum differ.")
    prompts = [(row, "\nUser: " + row["text"] + "\nAssistant: ") for row in exam["questions"]]
    budget = exam["generation"]["maximum_new_byte_tokens"]
    if any(len(prompt.encode("utf-8")) + budget > model.config.context_length for _, prompt in prompts):
        raise ValueError("A whole B3 question and generation budget exceed the model context.")
    model.eval()
    temperatures = [0.0] if greedy_only else exam["generation"]["temperatures"]
    result = {"stage": "raw held-out B3 answers; teacher review pending",
              "qualification": "Known reviewed facts with unseen question wording; not unseen knowledge or general task competence.",
              "selection": "The exam does not train weights or select a checkpoint.",
              "checkpoint_sha256": checkpoint_hash, "curriculum_sha256": corpus.sha256,
              "exam_sha256": hashlib.sha256(raw_exam).hexdigest(), "runtime": versions,
              "checkpoint_metadata": metadata, "generation": {**exam["generation"], "temperatures": temperatures},
              "outputs": []}
    for row, prompt in prompts:
        for temperature in temperatures:
            generated = continuation(model, prompt, temperature=temperature,
                                     seed=exam["generation"]["seed"], maximum_new_tokens=budget)
            result["outputs"].append({"id": row["id"], "topic_id": row["topic_id"],
                                      "question": row["text"], "prompt": prompt, **generated})
        print(json.dumps({"completed_question": row["id"]}), flush=True)
    if hashlib.sha256(checkpoint.read_bytes()).hexdigest() != checkpoint_hash:
        raise ValueError("Checkpoint changed during evaluation.")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as destination:
        json.dump(result, destination, ensure_ascii=False, indent=2, allow_nan=False)
        destination.write("\n")
    return {"output": str(output), "answers": len(result["outputs"]), "teacher_review": "pending"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--curriculum", type=Path, required=True)
    parser.add_argument("--exam", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--expected-checkpoint-sha256", required=True)
    parser.add_argument("--device", choices=("cpu", "mps"), default="mps")
    parser.add_argument("--greedy-only", action="store_true")
    args = parser.parse_args(argv)
    try:
        print(json.dumps(run(args.checkpoint, args.curriculum, args.exam, args.output,
                             expected_checkpoint_sha256=args.expected_checkpoint_sha256,
                             device=args.device, greedy_only=args.greedy_only)))
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, UnicodeError) as error:
        parser.exit(2, f"B3 evaluation failed: {error}\n")


if __name__ == "__main__":
    main()
