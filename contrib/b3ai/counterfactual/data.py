# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Counterfactual grounding turns: one question, different supplied results.

A repeated question is permitted when the supplied simulated result differs;
identity is the whole rendered input. Every label is computed from a synthetic
fixture and re-derived from the rendered prompt bytes on each load. No node is
contacted, and no model output is ever a target.
"""

import argparse
import hashlib
import json
from pathlib import Path
import random
import re

import numpy as np

from ..language.batches import EOD_ID, _json
from ..teaching.data import normalize_question
from ..tool_learning.protocol import PUBLIC_ERROR, compact, result_prompt, validate_action, validate_fixture


PACKAGE = Path(__file__).resolve().parent
REPOSITORY = PACKAGE.parents[2]
CURRICULUM_SCHEMA = "b3-counterfactual-grounding-curriculum/v1"
EXAM_SCHEMA = "b3-counterfactual-grounding-exam/v1"
PRIOR_SCHEMA = "b3-counterfactual-prior-probes/v1"
MAX_CONTEXT = 1024
MAX_JSON_BYTES = 2 * 1024 * 1024
GENERATION_BUDGET = 256
MODEL_CONTEXT = 2048
TOOL = "getnetworkinfo"
ACTION = {"tool": TOOL, "arguments": {}}
UNKNOWN = "unknown"
UNAVAILABLE = {"simulated": True, "ok": False, "error": PUBLIC_ERROR}
JSON_PREFIX = '{"reply":"'
# Explicit answer-mode framing: the first bytes of every grounding question.
MODES = {"value": "Answer mode: bare value.", "json": "Answer mode: short JSON reply."}
# Shared field vocabulary: (phrase used in questions, key read from the result).
FIELDS = {"total": ("total connections", "connections"),
          "inbound": ("inbound connections", "connections_in"),
          "outbound": ("outbound connections", "connections_out")}
# A template family is one sentence frame; families are disjoint across splits.
FRAMES = {
    "train": ("How many {F} does the result list?",
              "Without guessing, state the number of {F} given in the result.",
              "Tell me the count of {F} from the node data.",
              "What number of {F} is in the returned data?",
              "Read out the {F} figure from the result.",
              "Looking at the result, what is the number of {F}?",
              "Give me the {F} count that the tool returned.",
              "Check the returned fields: how many {F} are there?"),
    "validation": ("Please look up the {F} value in the result.",
                   "Point out how many {F} the result holds."),
    "exam": ("Report the {F} number found in the tool output.",
             "From the data that came back, what is the {F} count?",
             "I need the {F} figure; what does the result say?",
             "Which number does the result show for {F}?",
             "Find {F} in the returned result and say the number.",
             "Using the tool result, how many {F} do you see?",
             "Could you say how many {F} the response contains?",
             "Look at the response and name the {F} count."),
}
RANGES = {"train": (8, 199), "validation": (400, 499), "exam": (200, 399)}
VERSIONS = {"train": (110000, 119999), "validation": (140000, 149999), "exam": (120000, 139999)}
PROTOCOLS = {"train": (70010, 70199), "validation": (70400, 70499), "exam": (70200, 70399)}
COUNTS = {"train": {"pairs": 72, "unknown": 16, "turns": 320},
          "validation": {"pairs": 16, "unknown": 8, "turns": 80}}
SEEDS = {"exam": 2026100501, "curriculum": 2026100502, "validation": 2026100503}
TRAIN_PATTERNS = (("teen-prefix", 12), ("single-prefix", 6), ("digit-diff", 6), ("swap", 4), ("single-long", 2),
                  ("teen-any", 10), ("short-short", 10), ("long-long", 10), ("short-long", 12))
TURN_FIELDS = {"id", "split", "mode", "kind", "unit", "pair", "member", "frame", "field", "question", "envelope", "answer"}
CASE_ORDER = ("id", "role", "mode", "kind", "unit", "pair", "member", "frame", "field", "question", "envelope")
CASE_FIELDS = set(CASE_ORDER)
ROLES = ("gate", "missing-data", "bare-diagnostic")
_PROMPT = re.compile(r"\nTools: enabled\nUser: (?P<question>[^\n]+)\nAssistant: (?P<call>[^\n]+)\n"
                     r"Tool: (?P<envelope>[^\n]+)\nAssistant: ")
# Loss weights inside one grounding answer; reporting classes are kept apart:
# value digits, the supervised byte that ends a value, wrapper/newline/EOD,
# the letters of "unknown", and the one terminator that training leaves unsupervised.
VALUE, TERMINATOR, BOILERPLATE, UNKNOWN_WORD, UNSUPERVISED_TERMINATOR = 1, 2, 3, 4, 5
BOILERPLATE_WEIGHT = 0.25


def _sha(data):
    return hashlib.sha256(data).hexdigest()


def _read(path, maximum=MAX_JSON_BYTES):
    path = Path(path)
    if path.is_symlink() or not path.is_file():
        raise ValueError("Grounding data must be a regular file.")
    with path.open("rb") as source:
        raw = source.read(maximum + 1)
    if len(raw) > maximum:
        raise ValueError("Grounding data exceeds its byte bound.")
    document = _json(raw)
    if type(document) is not dict:
        raise ValueError("Grounding data must be a JSON object.")
    return document, raw


def question_text(mode, frame, field):
    return MODES[mode] + " " + frame.replace("{F}", FIELDS[field][0])


def answer_text(mode, label):
    return label if mode == "value" else compact({"reply": label})


def prompt_text(question, envelope):
    """Only the question and the supplied result enter a prompt; never a label."""
    return result_prompt(question, ACTION, envelope)


def read_question(question):
    modes = [name for name, marker in MODES.items() if question.startswith(marker + " ")]
    if len(modes) != 1:
        raise ValueError("Grounding question lacks exactly one answer-mode instruction.")
    body = question[len(MODES[modes[0]]) + 1:]
    fields = [name for name, (phrase, _) in FIELDS.items() if phrase in body]
    if len(fields) != 1 or body.count(FIELDS[fields[0]][0]) != 1 or body.count("connections") != 1:
        raise ValueError("Grounding question must name exactly one requested field.")
    if any(char.isdigit() for char in question) or UNKNOWN in question.casefold():
        raise ValueError("Grounding question must not contain a number or an answer.")
    return modes[0], body, fields[0]


def derive(prompt):
    """Independent label: what the rendered bytes themselves say the answer is."""
    match = _PROMPT.fullmatch(prompt)
    if match is None or match["call"] != compact(ACTION):
        raise ValueError("Rendered grounding prompt does not have the frozen framing.")
    mode, _, field = read_question(match["question"])
    envelope = _json(match["envelope"])
    validate_fixture(TOOL, envelope)
    if envelope["ok"] is False:
        return mode, field, UNKNOWN, envelope
    value = envelope["data"].get(FIELDS[field][1])
    if value is None:
        return mode, field, UNKNOWN, envelope
    if type(value) is not int:
        raise ValueError("Requested synthetic value must be an integer.")
    return mode, field, str(value), envelope


def _numbers(split, envelope):
    """Connection counts, and irrelevant numeric fields, each inside its split range."""
    if envelope["ok"] is False:
        return [], []
    data = envelope["data"]
    counts = [data[key] for _, key in FIELDS.values() if key in data]
    extras = [data[key] for key in ("version", "protocolversion") if key in data]
    if any(type(value) is not int for value in counts + extras):
        raise ValueError("Synthetic numbers must be integers.")
    if any(type(data[key]) is not bool for key in ("networkactive", "localrelay") if key in data):
        raise ValueError("Synthetic flags must be booleans.")
    low, high = RANGES[split]
    if any(not low <= value <= high for value in counts) or len(set(counts)) != len(counts):
        raise ValueError("Connection counts must be distinct and inside the split range.")
    if ("version" in data and not VERSIONS[split][0] <= data["version"] <= VERSIONS[split][1]) or (
            "protocolversion" in data and not PROTOCOLS[split][0] <= data["protocolversion"] <= PROTOCOLS[split][1]):
        raise ValueError("Irrelevant numeric fields must stay inside the split range.")
    return counts, extras


def check_item(item, split, frames, reserved):
    """Validate one stored turn or exam case and return its rendered record."""
    mode, kind, field, frame = item["mode"], item["kind"], item["field"], item["frame"]
    if (mode not in MODES or kind not in ("pair", "missing", "unavailable") or field not in FIELDS
            or type(frame) is not int or not 1 <= frame <= len(frames)
            or type(item["id"]) is not str or re.fullmatch(r"[a-z0-9][a-z0-9.-]{0,95}", item["id"]) is None
            or type(item["unit"]) is not str or not item["unit"].startswith(split + ".")):
        raise ValueError("Grounding item fields are invalid.")
    if item["question"] != question_text(mode, frames[frame - 1], field):
        raise ValueError("Grounding question is not its declared frame, field and answer mode.")
    envelope = item["envelope"]
    validate_fixture(TOOL, envelope)
    if list(envelope) != (["simulated", "ok", "data"] if envelope["ok"] else ["simulated", "ok", "error"]):
        raise ValueError("Envelope keys must keep the protocol order.")
    counts, extras = _numbers(split, envelope)
    if set(counts) & reserved:
        raise ValueError("A reserved earlier probe value appears in a fixture.")
    prompt = prompt_text(item["question"], envelope)
    derived_mode, derived_field, label, _ = derive(prompt)
    if (derived_mode, derived_field) != (mode, field):
        raise ValueError("Rendered prompt disagrees with the declared mode or field.")
    key = FIELDS[field][1]
    if kind == "pair":
        if label == UNKNOWN or (item["pair"], item["member"]) == (None, None) or item["member"] not in ("a", "b"):
            raise ValueError("A paired turn needs a supplied requested value and its pair.")
        tokens = re.findall(r"\d+", prompt)
        if tokens.count(label) != 1 or any(label in token for token in tokens if token != label):
            raise ValueError("The requested value must occur exactly once and inside no other number.")
    elif item["pair"] is not None or item["member"] is not None or label != UNKNOWN:
        raise ValueError("An unknown turn cannot belong to a pair or carry a value.")
    elif kind == "missing" and (envelope["ok"] is not True or field == "total" or key in envelope["data"]):
        raise ValueError("A missing-field turn must omit one optional requested field.")
    elif kind == "unavailable" and envelope != UNAVAILABLE:
        raise ValueError("An unavailable turn requires the fixed unavailable result.")
    answer = answer_text(mode, label)
    if item.get("answer", answer) != answer:
        raise ValueError("A stored target is not the label derived from its fixture.")
    if mode == "json" and validate_action(answer, False) != {"reply": label}:
        raise ValueError("JSON target is not the protocol reply for the derived label.")
    text = prompt + answer + "\n"
    raw_prompt = prompt.encode("utf-8")
    if len(text.encode("utf-8")) + 1 > MAX_CONTEXT or len(raw_prompt) + GENERATION_BUDGET > MODEL_CONTEXT:
        raise ValueError("Whole grounding turn exceeds its context; cropping is forbidden.")
    return {**item, "prompt": prompt, "label": label, "answer": answer, "text": text,
            "answer_start": len(raw_prompt), "prompt_sha256": _sha(raw_prompt)}


def check_pair(first, second):
    """Both prompts are P + value + S with identical P and S; only the value differs."""
    if (first["member"], second["member"]) != ("a", "b") or any(
            first[key] != second[key] for key in ("pair", "unit", "mode", "frame", "field", "question")):
        raise ValueError("Pair members must share question, framing and identity.")
    key = FIELDS[first["field"]][1]
    anchor = f'"{key}":'
    if first["prompt"].count(anchor) != 1 or first["label"] == second["label"]:
        raise ValueError("A pair needs one requested key and two different values.")
    position = first["prompt"].index(anchor) + len(anchor)
    prefix, suffix = first["prompt"][:position], first["prompt"][position + len(first["label"]):]
    if (first["prompt"] != prefix + first["label"] + suffix or second["prompt"] != prefix + second["label"] + suffix
            or suffix[0] not in ",}" or list(first["envelope"]["data"]) != list(second["envelope"]["data"])):
        raise ValueError("Pair prompts differ outside the requested value.")
    for record, other in ((first, second), (second, first)):
        if other["label"] in re.findall(r"\d+", record["prompt"]):
            raise ValueError("A pair member's prompt contains its partner's value.")


def _group(records):
    """Pairs by (pair id, mode); contrast units share one base fixture across fields."""
    pairs, unknown = {}, []
    for record in records:
        if record["kind"] == "pair":
            pairs.setdefault((record["pair"], record["mode"]), {})[record["member"]] = record
        else:
            unknown.append(record)
    for members in pairs.values():
        if set(members) != {"a", "b"}:
            raise ValueError("Every pair needs exactly members a and b.")
        check_pair(members["a"], members["b"])
    units = {}
    for (_, mode), members in pairs.items():
        units.setdefault((members["a"]["unit"], mode), []).append(members)
    for members in units.values():
        if len(members) > 1 and (len(members) != 3 or {m["a"]["field"] for m in members} != set(FIELDS)
                                 or len({compact(m["a"]["envelope"]) for m in members}) != 1
                                 or len({m["a"]["frame"] for m in members}) != 1):
            raise ValueError("A contrast unit must ask all three fields about one shared base fixture.")
    return pairs, unknown, units


def _unique(records, name):
    for key in ("id", "prompt"):
        if len({record[key] for record in records}) != len(records):
            raise ValueError(f"Duplicate {name} {key}; identity is the whole input.")


def _split_numbers(split, records):
    values = set()
    for record in records:
        counts, extras = _numbers(split, record["envelope"])
        values.update(counts + extras)
    return values


def _document(path, schema, frame_splits):
    document, raw = _read(path)
    if (document.get("schema") != schema or document.get("modes") != MODES
            or document.get("fields") != {name: list(value) for name, value in FIELDS.items()}
            or document.get("frames") != {split: list(FRAMES[split]) for split in frame_splits}
            or document.get("ranges") != {split: list(RANGES[split]) for split in frame_splits}
            or type(document.get("reserved_values")) is not list
            or any(type(value) is not int for value in document["reserved_values"])):
        raise ValueError("Grounding document schema, framing or ranges are not the frozen ones.")
    return document, raw


def weights(record):
    """Per answer byte (then newline, then EOD): loss weight and reporting class."""
    label, offset = record["label"], 0 if record["mode"] == "value" else len(JSON_PREFIX)
    size = len(record["answer"].encode("utf-8")) + 2
    weight = np.full(size, BOILERPLATE_WEIGHT, dtype=np.float32)
    kind = np.full(size, BOILERPLATE, dtype=np.int8)
    numeric = label != UNKNOWN
    # After a two-digit value led by 2-9 the training range always terminates, so that
    # one stop decision could be learned without reading the result: leave it unsupervised.
    unsupervised = record.get("split") == "train" and len(label) == 2 and label[0] in "23456789"
    kind[offset:offset + len(label)] = VALUE if numeric else UNKNOWN_WORD
    kind[offset + len(label)] = UNSUPERVISED_TERMINATOR if unsupervised else TERMINATOR
    weight[offset:offset + (len(label) if numeric else 1)] = 1
    weight[offset + len(label)] = 0 if unsupervised else 1
    return weight, kind


def arrays(record, context_length=MAX_CONTEXT):
    """Whole turn from offset zero, as the frozen instruction loader lays it out."""
    raw = record["text"].encode("utf-8", errors="strict")
    if len(raw) + 1 > context_length:
        raise ValueError("Whole grounding turn exceeds its context; cropping is forbidden.")
    sequence = np.concatenate((np.frombuffer(raw, dtype=np.uint8).astype(np.int64), np.array([EOD_ID], dtype=np.int64)))
    tokens, targets = sequence[:-1].copy(), sequence[1:].copy()
    weight, kind = np.zeros(tokens.shape, dtype=np.float32), np.zeros(tokens.shape, dtype=np.int8)
    weight[record["answer_start"] - 1:], kind[record["answer_start"] - 1:] = weights(record)
    return tokens, targets, weight, kind


def stack(rows):
    """Right-pad whole rows with zeros to the longest row; padding carries no weight."""
    width = max(len(row[0]) for row in rows)
    tokens = np.zeros((len(rows), width), dtype=np.int64)
    targets = np.zeros_like(tokens)
    weight = np.zeros(tokens.shape, dtype=np.float32)
    kind = np.zeros(tokens.shape, dtype=np.int8)
    for index, row in enumerate(rows):
        size = len(row[0])
        tokens[index, :size], targets[index, :size], weight[index, :size] = row[0], row[1], row[2]
        if len(row) > 3:
            kind[index, :size] = row[3]
    return tokens, targets, weight, kind


class GroundingCorpus:
    """The frozen 320 training and 80 validation turns, verified on every load."""

    def __init__(self, path=None, context_length=MAX_CONTEXT):
        if type(context_length) is not int or not 1 <= context_length <= MAX_CONTEXT:
            raise ValueError("Grounding context must be an integer from 1 through 1024.")
        self.context_length = context_length
        document, raw = _document(path or PACKAGE / "curriculum.json", CURRICULUM_SCHEMA, ("train", "validation"))
        self.reserved, self.sha256 = frozenset(document["reserved_values"]), _sha(raw)
        turns = document.get("turns")
        if type(turns) is not list or any(type(turn) is not dict or set(turn) != TURN_FIELDS for turn in turns):
            raise ValueError("Grounding turns are missing or have unexpected fields.")
        self.turns, self.pairs, self.unknown, self.units = {}, {}, {}, {}
        for split, counts in COUNTS.items():
            records = [check_item(turn, split, FRAMES[split], self.reserved) for turn in turns if turn["split"] == split]
            for record in records:
                arrays(record, context_length)
            _unique(records, split)
            pairs, unknown, units = _group(records)
            for mode in MODES:
                if (sum(key[1] == mode for key in pairs) != counts["pairs"]
                        or sum(record["mode"] == mode for record in unknown) != counts["unknown"]):
                    raise ValueError("Grounding split does not have its frozen pair and unknown counts.")
            if len(records) != counts["turns"]:
                raise ValueError("Grounding split does not have its frozen turn count.")
            self.turns[split], self.pairs[split], self.unknown[split], self.units[split] = records, pairs, unknown, units
        if len(turns) != sum(counts["turns"] for counts in COUNTS.values()):
            raise ValueError("Grounding turns belong to an unknown split.")
        if ({record["prompt"] for record in self.turns["train"]} & {record["prompt"] for record in self.turns["validation"]}
                or _split_numbers("train", self.turns["train"]) & _split_numbers("validation", self.turns["validation"])):
            raise ValueError("Training and validation share an input or a number.")
        self.by_id = {record["id"]: record for records in self.turns.values() for record in records}

    def batch(self, identities):
        return stack([arrays(self.by_id[identity], self.context_length) for identity in identities])


class Exam:
    """Frozen held-out cases. Prompt inputs and grading-only answers are stored apart."""

    def __init__(self, path=None):
        document, raw = _document(path or PACKAGE / "exam.json", EXAM_SCHEMA, ("exam",))
        self.document, self.sha256, self.reserved = document, _sha(raw), frozenset(document["reserved_values"])
        cases, grading = document.get("cases"), document.get("grading")
        if (type(cases) is not list or any(type(case) is not dict or set(case) != CASE_FIELDS for case in cases)
                or type(grading) is not dict or set(grading) != {case["id"] for case in cases}
                or document.get("generation") != {"maximum_new_byte_tokens": 256, "temperature": 0.0, "seed": 1729}):
            raise ValueError("Exam cases, grading keys or generation settings are invalid.")
        self.cases, self.records = cases, {}
        for role in ROLES:
            records = [check_item(case, "exam", FRAMES["exam"], self.reserved) for case in cases if case["role"] == role]
            if any(record["answer"] != grading[record["id"]] for record in records):
                raise ValueError("A grading answer is not the label derived from its fixture.")
            _unique(records, role)
            self.records[role] = records
        if sum(len(records) for records in self.records.values()) != len(cases):
            raise ValueError("Exam case has an unknown role.")
        gate, probes, bare = (self.records[role] for role in ROLES)
        self.pairs, unknown, _ = _group(gate)
        if (len(self.pairs) != 64 or len(gate) != 128 or unknown or any(record["mode"] != "json" for record in gate + probes)
                or [record["kind"] for record in probes].count("missing") != 8
                or [record["kind"] for record in probes].count("unavailable") != 8 or len(probes) != 16
                or any(record["mode"] != "value" for record in bare) or len(bare) != 144):
            raise ValueError("Exam must hold 64 JSON gate pairs, 16 missing-data probes and the bare-mode copies.")
        for frame in range(1, len(FRAMES["exam"]) + 1):
            if sum(members["a"]["frame"] == frame for members in self.pairs.values()) != 8:
                raise ValueError("Every exam frame must carry exactly eight gate pairs.")
        _group(bare)
        bare_inputs = {(record["question"][len(MODES["value"]):], compact(record["envelope"])) for record in bare}
        if bare_inputs != {(record["question"][len(MODES["json"]):], compact(record["envelope"])) for record in gate + probes}:
            raise ValueError("Bare-mode diagnostics must reuse exactly the gate and probe fixtures.")
        self.grading = grading
        self.numbers = _split_numbers("exam", gate + probes + bare)
        self.prompt_hashes = sorted(record["prompt_sha256"] for records in self.records.values() for record in records)


def slot_windows(frame):
    """Three-word windows around the field slot; used to label near and far exam frames."""
    words = re.findall(r"[a-z]+|\{f\}", frame.casefold())
    index = words.index("{f}")
    return {" ".join(words[start:start + 3]) for start in (index - 2, index - 1, index)
            if start >= 0 and start + 3 <= len(words)}


def frame_groups():
    seen = set().union(*(slot_windows(frame) for split in ("train", "validation") for frame in FRAMES[split]))
    return {str(index): "near" if slot_windows(frame) & seen else "far" for index, frame in enumerate(FRAMES["exam"], 1)}


def _integers(value, found):
    if type(value) is int:
        found.add(value)
    elif isinstance(value, dict):
        for item in value.values():
            _integers(item, found)
    elif isinstance(value, list):
        for item in value:
            _integers(item, found)


PRIOR_SOURCES = ("contrib/b3ai/tool_learning/exam.json", "build-ai/tool-learning/test-now-x_hbgllt/exam.json",
                 "build-ai/tool-learning/tester-retest-tphh2e1j/exam.json")
PROPOSAL_EXAMPLE = {"question": "How many connections are shown?", "values": (37, 82)}


def build_prior(instructions, repository=REPOSITORY):
    """Earlier wordings and probe numbers that new grounding data must stay clear of.

    Reserved integers are every integer below 1000 in a fixture of the frozen
    validation records, the tracked exam and both diagnosis exams, plus the
    proposal's example. Frozen TRAINING values are old lessons, not probes.
    """
    questions = {normalize_question(record["question"]) for records in instructions.records.values() for record in records}
    integers, sources = set(PROPOSAL_EXAMPLE["values"]), {}
    for record in instructions.records["validation"]:
        if record["tool_result"] is not None:
            _integers(record["tool_result"]["envelope"], integers)
    for name in PRIOR_SOURCES:
        document, raw = _read(Path(repository) / name)
        sources[name] = _sha(raw)
        for row in document["questions"]:
            questions.add(normalize_question(row["question"]))
            for key in ("fixture", "fixtures"):
                _integers(row.get(key), integers)
    questions.add(normalize_question(PROPOSAL_EXAMPLE["question"]))
    return {"schema": PRIOR_SCHEMA, "instruction_corpus_sha256": instructions.sha256, "sources": sources,
            "proposal_example": {"question": PROPOSAL_EXAMPLE["question"], "values": list(PROPOSAL_EXAMPLE["values"])},
            "reserved_integers": sorted(value for value in integers if 0 <= value < 1000),
            "normalized_question_sha256": sorted(_sha(question.encode("utf-8")) for question in questions)}


def load_prior(path=None):
    document, raw = _read(path or PACKAGE / "prior_probes.json")
    if (document.get("schema") != PRIOR_SCHEMA or type(document.get("reserved_integers")) is not list
            or type(document.get("normalized_question_sha256")) is not list):
        raise ValueError("Prior probe snapshot is invalid.")
    return document, _sha(raw)


def cross_check(corpus, exam, instructions, prior):
    """Freeze-time separation of training, validation, exam and all earlier material."""
    frames = {split: {normalize_question(frame) for frame in FRAMES[split]} for split in FRAMES}
    if frames["train"] & frames["validation"] or frames["train"] & frames["exam"] or frames["validation"] & frames["exam"]:
        raise ValueError("Template families overlap between splits.")
    reserved = set(prior["reserved_integers"])
    if corpus.reserved != exam.reserved or set(corpus.reserved) != {
            value for value in reserved if any(low <= value <= high for low, high in RANGES.values())}:
        raise ValueError("Reserved values are not the mechanically derived earlier probe numbers.")
    exam_records = [record for records in exam.records.values() for record in records]
    numbers = {"train": _split_numbers("train", corpus.turns["train"]),
               "validation": _split_numbers("validation", corpus.turns["validation"]), "exam": exam.numbers}
    prompts = {"train": {record["prompt"] for record in corpus.turns["train"]},
               "validation": {record["prompt"] for record in corpus.turns["validation"]},
               "exam": {record["prompt"] for record in exam_records}}
    for first, second in (("train", "validation"), ("train", "exam"), ("validation", "exam")):
        if numbers[first] & numbers[second] or prompts[first] & prompts[second]:
            raise ValueError("Splits share a number or a whole input.")
    earlier = set(prior["normalized_question_sha256"])
    wordings = {normalize_question(record["question"]) for record in corpus.by_id.values()}
    wordings |= {normalize_question(record["question"]) for record in exam_records}
    for split in FRAMES:
        for frame in FRAMES[split]:
            for phrase, _ in FIELDS.values():
                wordings.add(normalize_question(frame.replace("{F}", phrase)))
    if any(_sha(wording.encode("utf-8")) in earlier for wording in wordings):
        raise ValueError("A grounding wording repeats an earlier curriculum, exam or diagnosis question.")
    targets = {}
    replay = [(record["prompt"], record["target_json"]) for records in instructions.records.values() for record in records]
    if any("Answer mode:" in prompt for prompt, _ in replay):
        raise ValueError("Replay material already contains an answer-mode instruction.")
    for prompt, target in replay + [(record["prompt"], record["answer"]) for record in list(corpus.by_id.values()) + exam_records]:
        if targets.setdefault(prompt, target) != target:
            raise ValueError("One full prompt has two different targets.")
    return {"wordings_checked": len(wordings), "prompts_checked": len(targets),
            "numbers": {split: len(values) for split, values in numbers.items()}}


# ---- deterministic generation ------------------------------------------------------------

def _pool(split, reserved):
    low, high = RANGES[split]
    return [value for value in range(low, high + 1) if value not in reserved]


def _draw(rng, pattern, pool):
    """Two different requested values for one pair, following a frozen digit pattern."""
    members = set(pool)
    singles = [v for v in pool if v < 10]
    teens = [v for v in pool if 10 <= v <= 19]
    longs = [v for v in pool if v >= 100]
    # Two-digit values led by 2, 3 or 4 are drawn twice as often: the held-out ranges
    # (exam 200-399, validation 400-499) lead with those digits. Only the published
    # ranges are used here, never an exam item.
    shorts = [v for v in pool if 20 <= v <= 99 for _ in range(2 if v < 50 else 1)]
    for _ in range(10000):
        if pattern == "any":
            first, second = rng.sample(pool, 2)
        elif pattern == "teen-prefix":
            first = rng.choice(teens)
            second = first * 10 + rng.randrange(10)
        elif pattern == "single-prefix":
            first = rng.choice(singles)
            second = first * 10 + rng.randrange(10)
        elif pattern == "digit-diff":
            first = rng.choice(longs if rng.random() < 2 / 3 else shorts)
            digits = list(str(first))
            position = rng.randrange(1, len(digits))
            digits[position] = str(rng.randrange(10))
            second = int("".join(digits))
        elif pattern == "swap":
            first = rng.choice(longs)
            digits = str(first)
            second = int(digits[0] + digits[2] + digits[1])
        elif pattern == "single-long":
            first, second = rng.choice(singles), rng.choice(longs)
        elif pattern == "teen-any":
            first, second = rng.choice(teens), rng.choice(longs if rng.random() < 0.5 else shorts)
            if str(second).startswith(str(first)):
                continue
        elif pattern == "short-short":
            first, second = rng.choice(shorts), rng.choice(shorts)
        elif pattern == "long-long":
            first, second = rng.choice(longs), rng.choice(longs)
        elif pattern == "short-long":
            first, second = rng.choice(shorts), rng.choice(longs)
        else:
            raise ValueError("Unknown value pattern.")
        if first != second and second in members:
            return (first, second) if rng.random() < 0.5 else (second, first)
    raise ValueError("Could not draw a value pair.")


def _extras(rng, split):
    extras = {}
    if rng.random() < 0.35:
        extras["version"] = rng.randint(*VERSIONS[split])
    if rng.random() < 0.35:
        extras["protocolversion"] = rng.randint(*PROTOCOLS[split])
    if rng.random() < 0.35:
        extras["localrelay"] = rng.random() < 0.5
    return extras


def _clear(values, others):
    """No requested value may equal, or be written inside, any other number in its prompt."""
    return all(str(value) not in str(other) for value in values for other in others)


def _distractor(rng, pool, near):
    if rng.random() < 0.5:
        similar = [v for v in pool if len(str(v)) == len(str(near)) and str(v)[0] == str(near)[0]]
        if similar:
            return rng.choice(similar)
    return rng.choice(pool)


def _unit(rng, split, pool, fields, patterns, contrast):
    """One shared layout: a contrast unit asks all three fields about one base fixture."""
    for _ in range(10000):
        values = {field: _draw(rng, patterns[field], pool) for field in fields}
        base = {FIELDS[field][1]: values[field][0] for field in fields}
        if not contrast:
            requested = FIELDS[fields[0]][1]
            for _, key in FIELDS.values():
                if key != requested and (key == "connections" or rng.random() < 0.5):
                    base[key] = _distractor(rng, pool, values[fields[0]][rng.randrange(2)])
        data = {"networkactive": rng.random() < 0.75, **base, **_extras(rng, split)}
        numbers = [value for key, value in data.items() if type(value) is int]
        everything = [value for pair in values.values() for value in pair] + [
            value for key, value in base.items() if key not in {FIELDS[field][1] for field in fields}]
        if len(set(everything)) != len(everything):
            continue
        if all(_clear(values[field], [n for n in numbers if n != values[field][0]]) for field in fields):
            keys = list(data)
            rng.shuffle(keys)
            return {key: data[key] for key in keys}, values
    raise ValueError("Could not draw a fixture.")


def _items(split, unit, frame, field, data, pair_values, modes):
    """Both members of one pair in each requested mode; only the requested value changes."""
    items, key = [], FIELDS[field][1]
    for mode in modes:
        for member, value in zip("ab", pair_values):
            envelope = {"simulated": True, "ok": True, "data": {**data, key: value}}
            items.append({"id": f"{unit}.{field}.{member}.{mode}", "split": split, "mode": mode, "kind": "pair",
                          "unit": unit, "pair": f"{unit}.{field}", "member": member, "frame": frame, "field": field,
                          "question": question_text(mode, FRAMES[split][frame - 1], field), "envelope": envelope,
                          "answer": answer_text(mode, str(value))})
    return items


def _unknown(split, unit, kind, frame, field, envelope, modes):
    return [{"id": f"{unit}.{mode}", "split": split, "mode": mode, "kind": kind, "unit": unit, "pair": None,
             "member": None, "frame": frame, "field": field,
             "question": question_text(mode, FRAMES[split][frame - 1], field), "envelope": envelope,
             "answer": answer_text(mode, UNKNOWN)} for mode in modes]


def _build(split, seed, reserved, modes, independent, missing, unavailable, patterns):
    """Per frame: two contrast units (three pairs each) and the listed independent pairs."""
    rng, pool, items, bases = random.Random(seed), _pool(split, reserved), [], {}
    deck = [name for name, count in patterns for _ in range(count)]
    rng.shuffle(deck)
    for frame in range(1, len(FRAMES[split]) + 1):
        for index in (1, 2):
            unit = f"{split}.f{frame}.g{index}"
            data, values = _unit(rng, split, pool, list(FIELDS), {field: deck.pop() for field in FIELDS}, True)
            bases[(frame, index)] = data
            for field in FIELDS:
                items += _items(split, unit, frame, field, data, values[field], modes)
        for field in independent(frame):
            unit = f"{split}.f{frame}.p-{field}"
            data, values = _unit(rng, split, pool, [field], {field: deck.pop()}, False)
            items += _items(split, unit, frame, field, data, values[field], modes)
    if deck:
        raise ValueError("Value patterns do not match the number of pairs.")
    for number, (frame, index, field, minimal) in enumerate(missing, 1):
        # A presence counterfactual: an existing contrast fixture with the requested key removed.
        drop = {FIELDS[field][1]} | ({FIELDS["inbound"][1], FIELDS["outbound"][1]} if minimal else set())
        data = {key: value for key, value in bases[(frame, index)].items() if key not in drop}
        items += _unknown(split, f"{split}.missing{number}", "missing", frame, field,
                          {"simulated": True, "ok": True, "data": data}, modes)
    for number, (frame, field) in enumerate(unavailable, 1):
        items += _unknown(split, f"{split}.unavailable{number}", "unavailable", frame, field, dict(UNAVAILABLE), modes)
    return items


_CYCLE = ("total", "inbound", "outbound")


def _header(schema, splits, reserved, seed, purpose):
    return {"schema": schema, "purpose": purpose,
            "generator": {"module": "contrib.b3ai.counterfactual.data", "seed": seed,
                          "targets": "computed from synthetic fixtures; no model output is a target"},
            "simulated_only": True, "modes": MODES, "fields": {name: list(value) for name, value in FIELDS.items()},
            "frames": {split: list(FRAMES[split]) for split in splits},
            "ranges": {split: list(RANGES[split]) for split in splits},
            "reserved_values": sorted(value for value in reserved
                                      if any(low <= value <= high for low, high in RANGES.values()))}


def build_exam(prior):
    reserved, modes = set(prior["reserved_integers"]), ("json", "value")
    items = _build("exam", SEEDS["exam"], reserved, modes,
                   lambda frame: ("total", "inbound" if frame % 2 else "outbound"),
                   [(frame, 1, "inbound" if frame % 2 else "outbound", frame > 4) for frame in range(1, 9)],
                   [(frame, _CYCLE[(frame - 1) % 3]) for frame in range(1, 9)], (("any", 64),))
    cases, grading = [], {}
    for item in items:
        role = "bare-diagnostic" if item["mode"] == "value" else "gate" if item["kind"] == "pair" else "missing-data"
        cases.append({key: role if key == "role" else item[key] for key in CASE_ORDER})
        grading[item["id"]] = item["answer"]
    cases.sort(key=lambda case: ROLES.index(case["role"]))
    groups, flags = frame_groups(), {}
    for case in cases:
        if case["role"] == "gate" and case["member"] == "a":
            data = case["envelope"]["data"]
            flags[case["pair"]] = {
                "frame": case["frame"], "frame_group": groups[str(case["frame"])], "field": case["field"],
                "unit_kind": "contrast" if ".g" in case["unit"] else "independent",
                "single_candidate": sum(key in data for _, key in FIELDS.values()) == 1,
                "requested_key_index": list(data).index(FIELDS[case["field"]][1]), "data_keys": len(data)}
    document = _header(EXAM_SCHEMA, ("exam",), reserved, SEEDS["exam"],
                       "Held-out result-reading probes. The prior tool call is supplied by the host; "
                       "student tool choice is not measured here. Never used for training or selection.")
    document.update({
        "generation": {"maximum_new_byte_tokens": 256, "temperature": 0.0, "seed": 1729},
        "roles": {"gate": "64 short-JSON pairs, 128 generated answers; the only gate",
                  "missing-data": "8 missing-field and 8 unavailable probes; scored separately, never pooled into the gate",
                  "bare-diagnostic": "the same fixtures in bare-value mode; not independent evidence, not the gate"},
        "gate": {"pairs": 64, "required_pairs": 52,
                 "primary": "both members: valid UTF-8, stopped by the learned end-of-document token, and the "
                            'generated bytes are exactly {"reply":"V"} followed by one newline, V being the supplied value',
                 "secondary": "protocol-valid reply equal to V with learned stop; reported beside the primary count",
                 "denominator": "always 64 pairs; a missing, errored or length-stopped generation fails"},
        "frame_groups": groups, "pair_flags": flags, "cases": cases, "grading": grading})
    return document


def build_curriculum(prior):
    reserved, modes, items = set(prior["reserved_integers"]), ("value", "json"), []
    items += _build("train", SEEDS["curriculum"], reserved, modes, lambda frame: _CYCLE,
                    [(frame, 1, "inbound" if frame % 2 else "outbound", frame > 4) for frame in range(1, 9)],
                    [(frame, _CYCLE[(frame - 1) % 3]) for frame in range(1, 9)], TRAIN_PATTERNS)
    items += _build("validation", SEEDS["validation"], reserved, modes,
                    lambda frame: ("total", "inbound" if frame % 2 else "outbound"),
                    [(1, 1, "inbound", False), (1, 2, "outbound", True), (2, 1, "inbound", True), (2, 2, "outbound", False)],
                    [(1, "total"), (1, "inbound"), (2, "total"), (2, "outbound")], (("any", 16),))
    document = _header(CURRICULUM_SCHEMA, ("train", "validation"), reserved, SEEDS["curriculum"],
                       "Paired result-reading turns: within a pair only the requested value differs. "
                       "320 training and 80 validation turns; validation selects, it never qualifies.")
    document.update({"counts": COUNTS, "train_value_patterns": [list(pattern) for pattern in TRAIN_PATTERNS],
                     "turns": items})
    return document


def render(document):
    return (json.dumps(document, ensure_ascii=False, indent=1, allow_nan=False) + "\n").encode("utf-8")


# ---- frozen sampling schedule ------------------------------------------------------------

def update_mode(update, phases):
    for phase in phases:
        if phase["first"] <= update <= phase["last"]:
            return phase["cycle"][(update - phase["first"]) % len(phase["cycle"])]
    raise ValueError("Update is outside the frozen phases.")


class _Deck:
    """Shuffled epochs: every item is dealt once before any item repeats."""

    def __init__(self, rng, items):
        self.rng, self.items, self.queue = rng, list(items), []

    def deal(self, count, used=()):
        dealt = []
        while len(dealt) < count:
            if not self.queue:
                self.queue = list(self.items)
                self.rng.shuffle(self.queue)
            item = self.queue.pop()
            if item not in dealt and item not in used:
                dealt.append(item)
            else:
                self.queue.insert(0, item)
        return dealt


def build_schedule(corpus, replay_ids, plan):
    """Every grounding and replay row of every update, fixed before training.

    Odd updates: one contrast unit (3 pairs) and 2 independent pairs. Even updates:
    one contrast unit, 1 independent pair and 2 unknown turns. One answer mode per update.
    """
    rng = random.Random(plan["seeds"]["schedule"])
    contrast = sorted({unit for (unit, mode), members in corpus.units["train"].items() if len(members) == 3})
    independent = sorted({unit for (unit, mode), members in corpus.units["train"].items() if len(members) == 1})
    scenarios = sorted({record["unit"] for record in corpus.unknown["train"]})
    decks = {"contrast": _Deck(rng, contrast), "independent": _Deck(rng, independent),
             "unknown": _Deck(rng, scenarios), "replay": _Deck(rng, sorted(replay_ids))}
    rows = plan["batch"]
    schedule = []
    for update in range(1, plan["limits"]["maximum_updates"] + 1):
        mode, even = update_mode(update, plan["phases"]), update % 2 == 0
        units = decks["contrast"].deal(1) + decks["independent"].deal(1 if even else 2)
        grounding = []
        for unit in units:
            for members in sorted(corpus.units["train"][(unit, mode)], key=lambda m: _CYCLE.index(m["a"]["field"])):
                grounding += [members["a"]["id"], members["b"]["id"]]
        if even:
            grounding += [f"{unit}.{mode}" for unit in decks["unknown"].deal(2)]
        if len(grounding) != rows["grounding_rows"] or len(set(grounding)) != len(grounding):
            raise ValueError("Grounding batch does not have its frozen row count.")
        schedule.append({"update": update, "mode": mode, "grounding": grounding,
                         "replay": decks["replay"].deal(rows["replay_rows"])})
    return schedule


def schedule_bytes(schedule):
    return (json.dumps(schedule, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


def _write(path, data):
    with Path(path).open("xb") as destination:
        destination.write(data)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=("prior", "exam", "curriculum", "verify"))
    args = parser.parse_args(argv)
    from ..tool_learning.data import InstructionCorpus
    instructions = InstructionCorpus(REPOSITORY / "contrib/b3ai/tool_learning/curriculum.json")
    if args.target == "prior":
        _write(PACKAGE / "prior_probes.json", render(build_prior(instructions)))
    prior, _ = load_prior()
    if args.target == "exam":
        _write(PACKAGE / "exam.json", render(build_exam(prior)))
    if args.target == "curriculum":
        _write(PACKAGE / "curriculum.json", render(build_curriculum(prior)))
    if args.target == "verify":
        corpus, exam = GroundingCorpus(), Exam()
        print(json.dumps({"curriculum_sha256": corpus.sha256, "exam_sha256": exam.sha256,
                          **cross_check(corpus, exam, instructions, prior)}))
    else:
        print(json.dumps({"written": args.target}))


if __name__ == "__main__":
    main()
