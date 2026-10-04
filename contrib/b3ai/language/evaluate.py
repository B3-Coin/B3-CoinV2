# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Record genuine language continuations; never supply corrected target answers."""

import argparse
import hashlib
import json
import math
from pathlib import Path

import torch

from .batches import EOD_ID, encode
from .checkpoint import load_checkpoint
from .train import runtime


def continuation(model, prompt, *, temperature=0.0, seed=1729, maximum_new_tokens=256):
    if (type(temperature) not in (int, float) or not math.isfinite(temperature) or not 0 <= temperature <= 2
            or type(seed) is not int or not 0 <= seed < 2**63
            or type(maximum_new_tokens) is not int or not 1 <= maximum_new_tokens <= 256):
        raise ValueError("Invalid bounded generation settings.")
    tokens = encode(prompt)[:-1].astype("int64").tolist()
    if len(tokens) + maximum_new_tokens > model.config.context_length:
        raise ValueError("Prompt and generation budget exceed the model context.")
    device = next(model.parameters()).device
    rng = torch.Generator(device="cpu").manual_seed(seed)
    generated, stopped = [], False
    with torch.no_grad():
        for _ in range(maximum_new_tokens):
            inputs = torch.tensor([tokens], dtype=torch.long, device=device)
            logits = model(inputs)[0, -1].cpu()
            if not bool(torch.isfinite(logits).all()):
                raise ValueError("Non-finite generated logits.")
            token = int(torch.argmax(logits)) if temperature == 0 else int(
                torch.multinomial(torch.softmax(logits / temperature, dim=-1), 1, generator=rng))
            generated.append(token)
            if token == EOD_ID:
                stopped = True
                break
            tokens.append(token)
    raw = bytes(token for token in generated if token != EOD_ID)
    try:
        text, valid = raw.decode("utf-8", errors="strict"), True
    except UnicodeError:
        text, valid = raw.decode("utf-8", errors="replace"), False
    return {"text": text, "token_ids": generated, "valid_utf8": valid,
            "finish_reason": "end_of_document" if stopped else "length",
            "temperature": temperature, "seed": seed, "maximum_new_tokens": maximum_new_tokens}


def run(checkpoint, exam_path, output, *, device="cpu"):
    versions = runtime(device)
    exam_path, checkpoint, output = Path(exam_path), Path(checkpoint), Path(output)
    if output.exists() or output.is_symlink():
        raise FileExistsError("Evaluation output already exists.")
    with exam_path.open("rb") as source:
        raw_exam = source.read(65537)
    if len(raw_exam) > 65536:
        raise ValueError("Exam exceeds its size bound.")
    exam = json.loads(raw_exam)
    if (type(exam) is not dict or exam.get("schema") != "b3-language-continuation-exam/v1"
            or type(exam.get("prompts")) is not list or len(exam["prompts"]) != 20
            or exam.get("generation") != {"maximum_new_byte_tokens": 256, "temperatures": [0.0, 0.8],
                                          "seed": 1729, "stop": "learned end-of-document token or token budget; preserve raw output"}):
        raise ValueError("Unsupported frozen language exam.")
    if len({item["id"] for item in exam["prompts"]}) != len(exam["prompts"]):
        raise ValueError("Duplicate exam prompt IDs.")
    model, _, metadata = load_checkpoint(checkpoint, device=device)
    if metadata.get("stage") != "language-pilot":
        raise ValueError("Checkpoint is not a language-pilot selection.")
    model.eval()
    report = {"stage": "independent continuation recording; teacher review pending",
              "checkpoint_sha256": hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
              "exam_sha256": hashlib.sha256(raw_exam).hexdigest(), "runtime": versions,
              "model_config": model.config.to_dict(), "checkpoint_metadata": metadata,
              "selection": "checkpoint already selected using validation; exam results do not select it",
              "review": exam["review"], "outputs": []}
    for prompt in exam["prompts"]:
        for temperature in exam["generation"]["temperatures"]:
            result = continuation(model, prompt["text"], temperature=temperature,
                                  seed=exam["generation"]["seed"],
                                  maximum_new_tokens=exam["generation"]["maximum_new_byte_tokens"])
            report["outputs"].append({"id": prompt["id"], "prompt": prompt["text"], **result})
        print(json.dumps({"completed_prompt": prompt["id"]}), flush=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as destination:
        json.dump(report, destination, ensure_ascii=False, indent=2, allow_nan=False)
        destination.write("\n")
    return {"output": str(output), "continuations": len(report["outputs"]), "teacher_review": "pending"}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--exam", type=Path, default=Path(__file__).with_name("language_exam.json"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device", choices=("cpu", "mps"), default="mps")
    args = parser.parse_args(argv)
    try:
        print(json.dumps(run(args.checkpoint, args.exam, args.output, device=args.device)))
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        parser.exit(2, f"Language evaluation failed: {error}\n")


if __name__ == "__main__":
    main()
