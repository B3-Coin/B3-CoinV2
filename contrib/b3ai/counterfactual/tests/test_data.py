# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Frozen grounding data: verified labels, pair invariants, separation and schedule."""

import collections
import copy
import hashlib
import json
from pathlib import Path
import re
import tempfile
import unittest

import numpy as np

from contrib.b3ai.counterfactual import data, train
from contrib.b3ai.tool_learning.data import InstructionCorpus
from contrib.b3ai.tool_learning.protocol import validate_action, validate_fixture

REPOSITORY = Path(__file__).resolve().parents[4]
SOURCES = all((REPOSITORY / name).is_file() for name in data.PRIOR_SOURCES)


class FrozenData(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.corpus, cls.exam = data.GroundingCorpus(), data.Exam()
        cls.instructions = InstructionCorpus(REPOSITORY / "contrib/b3ai/tool_learning/curriculum.json")
        cls.prior, _ = data.load_prior()
        cls.plan, _ = train.load_plan()

    def test_counts_follow_the_proposal(self):
        self.assertEqual({split: len(turns) for split, turns in self.corpus.turns.items()}, {"train": 320, "validation": 80})
        for split, pairs in (("train", 72), ("validation", 16)):
            for mode in data.MODES:
                self.assertEqual(sum(key[1] == mode for key in self.corpus.pairs[split]), pairs)
        self.assertEqual(len(self.exam.pairs), 64)
        self.assertEqual({role: len(records) for role, records in self.exam.records.items()},
                         {"gate": 128, "missing-data": 16, "bare-diagnostic": 144})
        fields = collections.Counter(members["a"]["field"] for members in self.exam.pairs.values())
        self.assertEqual(fields, {"total": 24, "inbound": 20, "outbound": 20})

    def test_frozen_files_are_what_the_generator_produces(self):
        self.assertEqual(data.render(data.build_exam(self.prior)), (data.PACKAGE / "exam.json").read_bytes())
        self.assertEqual(data.render(data.build_curriculum(self.prior)), (data.PACKAGE / "curriculum.json").read_bytes())

    @unittest.skipUnless(SOURCES, "needs the earlier diagnosis exams under build-ai/ (git-ignored)")
    def test_reserved_values_are_derived_from_earlier_probes(self):
        self.assertEqual(data.build_prior(self.instructions), self.prior)
        for value in (37, 82, 41, 86, 23, 58, 17, 64, 180, 70, 74, 341, 410):
            self.assertIn(value, self.prior["reserved_integers"])

    def test_every_label_is_rederived_from_the_rendered_prompt(self):
        records = list(self.corpus.by_id.values()) + [r for records in self.exam.records.values() for r in records]
        self.assertEqual(len(records), 400 + 288)
        for record in records:
            question, envelope = record["question"], record["envelope"]
            match = re.fullmatch(r"\nTools: enabled\nUser: (.+)\nAssistant: (.+)\nTool: (.+)\nAssistant: ", record["prompt"])
            self.assertEqual(match[1], question)
            supplied = json.loads(match[3])
            validate_fixture("getnetworkinfo", supplied)
            key = {"total connections": "connections", "inbound connections": "connections_in",
                   "outbound connections": "connections_out"}[re.search(r"(total|inbound|outbound) connections", question)[0]]
            expected = str(supplied["data"][key]) if supplied["ok"] and key in supplied["data"] else "unknown"
            self.assertEqual(record["label"], expected)
            wrapped = expected if record["mode"] == "value" else '{"reply":"%s"}' % expected
            self.assertEqual(record["answer"], wrapped)
            self.assertRegex(expected, r"\A(?:[1-9][0-9]{0,2}|unknown)\Z")
            self.assertNotRegex(question, r"[0-9]")
            self.assertEqual(record["text"], record["prompt"] + wrapped + "\n")
            if record["mode"] == "json":
                self.assertEqual(validate_action(record["answer"], False), {"reply": expected})

    def test_pair_members_differ_only_in_the_requested_value(self):
        pairs = list(self.corpus.pairs["train"].values()) + list(self.corpus.pairs["validation"].values()) + list(self.exam.pairs.values())
        for members in pairs:
            first, second = members["a"], members["b"]
            self.assertEqual(first["question"], second["question"])
            self.assertNotEqual(first["label"], second["label"])
            key = data.FIELDS[first["field"]][1]
            self.assertEqual(first["prompt"].replace(f'"{key}":{first["label"]}', f'"{key}":{second["label"]}'), second["prompt"])
            changed = {name for name in first["envelope"]["data"] if first["envelope"]["data"][name] != second["envelope"]["data"][name]}
            self.assertEqual(changed, {key})
            self.assertEqual(list(first["envelope"]["data"]), list(second["envelope"]["data"]))
            self.assertTrue({"networkactive", "connections"} <= set(first["envelope"]["data"]))

    def test_one_question_repeats_with_different_results(self):
        seen = collections.defaultdict(set)
        for record in self.corpus.turns["train"]:
            seen[record["question"]].add(json.dumps(record["envelope"]))
        self.assertGreater(max(len(results) for results in seen.values()), 4)
        with self.assertRaisesRegex(ValueError, "globally unique"):
            self.instructions._add("x", "train", "chat", "enabled", self.instructions.records["train"][0]["question"],
                                   {"reply": "again"}, None, "teacher-synthetic")

    def test_contrast_units_ask_three_fields_about_one_fixture(self):
        sizes = collections.Counter(len(members) for (unit, mode), members in self.corpus.units["train"].items() if mode == "json")
        self.assertEqual(sizes, {3: 16, 1: 24})
        unit = self.corpus.units["train"][("train.f1.g1", "json")]
        self.assertEqual(len({json.dumps(members["a"]["envelope"]) for members in unit}), 1)
        self.assertEqual(len({members["a"]["label"] for members in unit}), 3)

    def test_fixtures_cover_orders_omissions_and_irrelevant_fields(self):
        success = [record for record in self.corpus.turns["train"] if record["envelope"]["ok"]]
        self.assertGreater(len({tuple(record["envelope"]["data"]) for record in success}), 30)
        keys = collections.Counter(key for record in success for key in record["envelope"]["data"])
        for key in ("version", "protocolversion", "localrelay"):
            self.assertGreater(keys[key], 0)
        self.assertTrue(any("connections_out" not in record["envelope"]["data"] for record in success))
        index = {list(r["envelope"]["data"]).index(data.FIELDS[r["field"]][1]) for r in success if r["kind"] == "pair"}
        self.assertGreaterEqual(len(index), 4)
        kinds = collections.Counter((record["kind"], record["mode"]) for record in self.corpus.unknown["train"])
        self.assertEqual(kinds, {("missing", "value"): 8, ("missing", "json"): 8, ("unavailable", "value"): 8, ("unavailable", "json"): 8})

    def test_training_values_break_the_length_shortcut(self):
        values = [record["label"] for record in self.corpus.turns["train"] if record["kind"] == "pair" and record["mode"] == "json"]
        self.assertEqual(len(values), 144)
        self.assertGreaterEqual(sum(len(v) == 2 and v[0] == "1" for v in values) / 144, 0.15)
        self.assertGreaterEqual(sum(len(v) == 1 for v in values) / 144, 0.05)
        prefix = sum(m["a"]["label"].startswith(m["b"]["label"]) or m["b"]["label"].startswith(m["a"]["label"])
                     for (pair, mode), m in self.corpus.pairs["train"].items() if mode == "json")
        self.assertGreaterEqual(prefix, 18)

    def test_splits_and_earlier_material_stay_separate(self):
        result = data.cross_check(self.corpus, self.exam, self.instructions, self.prior)
        self.assertEqual(result["prompts_checked"], 342 + 400 + 288)
        ranges = {"train": (8, 199), "validation": (400, 499), "exam": (200, 399)}
        for split, records in (("train", self.corpus.turns["train"]), ("validation", self.corpus.turns["validation"]),
                               ("exam", [r for records in self.exam.records.values() for r in records])):
            for record in records:
                counts = [value for key, value in record["envelope"].get("data", {}).items() if key.startswith("connections")]
                self.assertTrue(all(ranges[split][0] <= value <= ranges[split][1] for value in counts))
                self.assertFalse(set(counts) & set(self.prior["reserved_integers"]))
        frames = [frame for split in data.FRAMES for frame in data.FRAMES[split]]
        self.assertEqual(len(set(frames)), 18)
        self.assertEqual(data.frame_groups(), {"1": "far", "2": "near", "3": "near", "4": "far", "5": "far",
                                              "6": "near", "7": "near", "8": "near"})
        seal = {"prompt_sha256": self.exam.prompt_hashes, "numbers": sorted(self.exam.numbers), "frame_sha256": []}
        train.check_separation(self.corpus, seal)
        leaked = dict(seal, prompt_sha256=[self.corpus.turns["train"][0]["prompt_sha256"]])
        with self.assertRaisesRegex(ValueError, "overlaps the sealed exam"):
            train.check_separation(self.corpus, leaked)

    def test_grading_answers_cannot_reach_a_prompt(self):
        from contrib.b3ai.counterfactual import evaluate
        inputs = evaluate.prompt_inputs(self.exam.cases)
        self.assertTrue(all(set(item) == {"id", "question", "envelope"} for item in inputs))
        poisoned = copy.deepcopy(self.exam.document)
        poisoned["grading"] = {key: "SENTINEL-9999" for key in poisoned["grading"]}
        prompts = [data.prompt_text(item["question"], item["envelope"]) for item in evaluate.prompt_inputs(poisoned["cases"])]
        self.assertEqual(sorted(hashlib.sha256(p.encode()).hexdigest() for p in prompts), self.exam.prompt_hashes)
        self.assertFalse(any("SENTINEL" in prompt for prompt in prompts))
        for record in self.exam.records["gate"]:
            partner = self.exam.grading[record["id"].replace(".a.json", ".X.json").replace(".b.json", ".a.json").replace(".X.json", ".b.json")]
            tokens = re.findall(r"\d+", record["prompt"])
            self.assertEqual(tokens.count(record["label"]), 1)
            self.assertNotIn(json.loads(partner)["reply"], tokens)

    def test_tampered_data_is_refused(self):
        document = json.loads((data.PACKAGE / "curriculum.json").read_text())

        def load(change):
            changed = copy.deepcopy(document)
            change(changed["turns"])
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "curriculum.json"
                path.write_bytes(data.render(changed))
                return data.GroundingCorpus(path)

        pair = next(i for i, turn in enumerate(document["turns"]) if turn["kind"] == "pair" and turn["member"] == "b")
        with self.assertRaisesRegex(ValueError, "not the label derived"):
            load(lambda turns: turns[pair].__setitem__("answer", "7" if turns[pair]["mode"] == "value" else '{"reply":"7"}'))
        with self.assertRaisesRegex(ValueError, "differ outside the requested value|shared base fixture|distinct"):
            load(lambda turns: turns[pair]["envelope"]["data"].__setitem__("networkactive", not turns[pair]["envelope"]["data"]["networkactive"]))
        with self.assertRaisesRegex(ValueError, "reserved earlier probe value"):
            load(lambda turns: [turn["envelope"]["data"].__setitem__("connections", 37) for turn in turns
                                if turn["unit"] == "train.f1.p-total"])
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            load(lambda turns: turns.__setitem__(1, dict(turns[0], id=turns[1]["id"])))
        with self.assertRaisesRegex(ValueError, "frozen turn count|pair and unknown counts|exactly members"):
            load(lambda turns: turns.pop())

    def test_arrays_match_the_frozen_instruction_layout(self):
        for identity in ("train.f1.g1.inbound.a.value", "train.f1.g1.inbound.a.json", "train.missing1.json"):
            record = self.corpus.by_id[identity]
            tokens, targets, weight, kind = data.arrays(record)
            frozen = self.instructions.arrays({"text": record["text"], "answer_start": record["answer_start"]})
            np.testing.assert_array_equal(tokens, frozen[0])
            np.testing.assert_array_equal(targets, frozen[1])
            np.testing.assert_array_equal(kind > 0, frozen[2] == 1)
            self.assertEqual(int(targets[-1]), 256)
            self.assertEqual(weight.dtype, np.float32)

    def test_loss_weights_and_the_unsupervised_terminator(self):
        zero = collections.Counter()
        for split, records in self.corpus.turns.items():
            for record in records:
                weight, kind = data.weights(record)
                label = record["label"]
                payload = data.VALUE if label != "unknown" else data.UNKNOWN_WORD
                self.assertEqual(int((kind == payload).sum()), len(label))
                self.assertEqual(int(np.isin(kind, (data.VALUE, data.UNKNOWN_WORD)).sum()), len(label))
                stops = np.isin(kind, (data.TERMINATOR, data.UNSUPERVISED_TERMINATOR))
                self.assertEqual(int(stops.sum()), 1)
                self.assertEqual(float(weight[-1]), 0.25)
                stop = float(weight[stops][0])
                expected = 0.0 if split == "train" and len(label) == 2 and label[0] in "23456789" else 1.0
                self.assertEqual(stop, expected)
                self.assertEqual(int(kind[stops][0]), data.TERMINATOR if stop else data.UNSUPERVISED_TERMINATOR)
                self.assertEqual(int((weight == 0).sum()), 0 if stop else 1)
                zero[(split, stop)] += 1
                if label != "unknown":
                    self.assertTrue((weight[kind == data.VALUE] == 1).all())
                else:
                    self.assertEqual(weight[kind == data.UNKNOWN_WORD].tolist(), [1.0] + [0.25] * 6)
        self.assertEqual(zero[("validation", 0.0)], 0)
        self.assertGreater(zero[("train", 0.0)], 0)

    def test_schedule_is_frozen_balanced_and_bare_first(self):
        replay = train.replay_records(self.instructions, self.plan["replay_categories"])
        self.assertEqual(len(replay), 206)
        self.assertEqual({record["category"] for record in replay.values()}, {"b3_fact", "chat", "refusal", "tool_choice"})
        schedule = data.build_schedule(self.corpus, replay, self.plan)
        self.assertEqual(hashlib.sha256(data.schedule_bytes(schedule)).hexdigest(), self.plan["data"]["schedule_sha256"])
        self.assertEqual(len(schedule), 1000)
        self.assertTrue(all(entry["mode"] == "value" for entry in schedule[:250]))
        self.assertEqual(collections.Counter(entry["mode"] for entry in schedule), {"json": 625, "value": 375})
        draws = collections.Counter()
        for entry in schedule:
            self.assertEqual(len(entry["grounding"]), 10)
            self.assertEqual(len(set(entry["replay"])), 3)
            self.assertTrue(set(entry["replay"]) <= set(replay))
            records = [self.corpus.by_id[identity] for identity in entry["grounding"]]
            self.assertEqual({record["mode"] for record in records}, {entry["mode"]})
            self.assertTrue(all(record["split"] == "train" for record in records))
            paired = [record for record in records if record["kind"] == "pair"]
            for first, second in zip(paired[0::2], paired[1::2]):
                self.assertEqual((first["pair"], first["member"], second["member"]), (second["pair"], "a", "b"))
            draws.update(record["pair"] for record in paired if record["member"] == "a")
        self.assertEqual(len(draws), 72)
        self.assertLessEqual(max(draws.values()) - min(draws.values()), 2)


if __name__ == "__main__":
    unittest.main()
