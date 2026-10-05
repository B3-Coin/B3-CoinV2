# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Recording and mechanical scoring, proven on synthetic outputs before any model is examined."""

import json
from pathlib import Path
import tempfile
import unittest

import torch

import support
from contrib.b3ai.counterfactual import data, evaluate, rehearse, train
from contrib.b3ai.language.batches import EOD_ID


def output(text, finish="end_of_document", valid=True):
    ids = list(text.encode("utf-8")) + ([EOD_ID] if finish == "end_of_document" else [])
    return {"text": text, "token_ids": ids, "valid_utf8": valid, "finish_reason": finish,
            "temperature": 0.0, "seed": 1729, "maximum_new_tokens": 256}


class Scoring(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.exam = data.Exam()

    def test_scorer_self_test_table(self):
        table = evaluate.check_self_test(evaluate.self_test(self.exam))
        self.assertEqual(table["first-count-in-result"]["strict_pairs"], 22)
        broken = dict(table, oracle={"strict_pairs": 63, "lenient_pairs": 64})
        with self.assertRaisesRegex(ValueError, "self-test failed"):
            evaluate.check_self_test(broken)
        with self.assertRaisesRegex(ValueError, "self-test failed"):
            evaluate.check_self_test(dict(table, **{"largest-count": {"strict_pairs": 52, "lenient_pairs": 52}}))
        self.assertEqual(table["oracle"], {"strict_pairs": 64, "lenient_pairs": 64})
        self.assertEqual(table["right-answer-no-newline"], {"strict_pairs": 0, "lenient_pairs": 64})
        for name in ("partner-swapped", "constant-number", "right-answer-never-stops", "bare-number",
                     "right-answer-then-extra-text", "empty-output", "invalid-utf8-flag", "generation-error", "no-recording",
                     "text-disagrees-with-tokens", "tokens-disagree-with-text"):
            self.assertEqual(table[name], {"strict_pairs": 0, "lenient_pairs": 0}, name)
        for name in ("always-total", "inbound-else-total", "outbound-inbound-total", "first-count-in-result", "largest-count"):
            self.assertLess(table[name]["lenient_pairs"], 40, name)
            self.assertEqual(table[name]["strict_pairs"], table[name]["lenient_pairs"])
        self.assertEqual(table["always-total"]["strict_pairs"], 24)

    def test_failure_classes(self):
        case = next(case for case in self.exam.cases if case["id"] == "exam.f1.g1.total.a.json")
        expected = self.exam.grading[case["id"]]
        value = json.loads(expected)["reply"]
        inbound = str(case["envelope"]["data"]["connections_in"])
        extra = str(case["envelope"]["data"]["protocolversion"])
        cases = {
            expected + "\n": "exact", expected: "right-answer-format-not-byte-exact",
            '{"reply": "%s"}\n' % value: "right-answer-format-not-byte-exact",
            '{"reply":"%s"}\n' % inbound: "wrong-field", '{"reply":"%s"}\n' % extra: "irrelevant-field",
            '{"reply":"%s"}\n' % value[:2]: "truncated", '{"reply":"%s7"}\n' % value: "overrun",
            '{"reply":"1%s"}\n' % value[1:]: "leading-digit-wrong", '{"reply":"999"}\n': "wrong-digits-same-length",
            '{"reply":"5"}\n': "other-number", '{"reply":"unknown"}\n': "says-unknown",
            value + "\n": "value-present-format-invalid", inbound + "\n": "wrong-field-format-invalid",
            '{"reply":"Simulated node: networking enabled; %s connections."}\n' % value: "value-inside-longer-reply",
            '{"reply":"Simulated node: networking enabled; 0 connections."}\n': "non-numeric-reply",
            '{"tool":"getnetworkinfo","arguments":{}}\n': "not-the-required-format", "": "not-the-required-format",
        }
        for text, kind in cases.items():
            graded = evaluate.grade(output(text), case, expected)
            self.assertEqual(graded["class"], kind, text)
            self.assertEqual(graded["strict"], kind == "exact")
        self.assertEqual(evaluate.grade(output(expected + "\n", finish="length"), case, expected)["class"], "no-learned-stop")
        self.assertEqual(evaluate.grade(output(expected + "\n", valid=False), case, expected)["class"], "invalid-utf8")
        forged = dict(output('{"reply":"0"}\n'), text=expected + "\n")
        graded = evaluate.grade(forged, case, expected)
        self.assertEqual((graded["class"], graded["strict"], graded["lenient"]), ("inconsistent-record", False, False))
        failed = {"error": "RuntimeError: device", "text": None, "token_ids": None, "valid_utf8": None, "finish_reason": None}
        self.assertEqual(evaluate.grade(failed, case, expected)["class"], "generation-error")
        probe = next(case for case in self.exam.cases if case["role"] == "missing-data")
        self.assertEqual(evaluate.grade(output('{"reply":"213"}\n'), probe, self.exam.grading[probe["id"]])["class"], "invented-value")
        bare = next(case for case in self.exam.cases if case["id"] == "exam.f1.g1.total.a.value")
        self.assertTrue(evaluate.grade(output(value + "\n"), bare, value)["strict"])
        self.assertFalse(evaluate.grade(output(expected + "\n"), bare, value)["lenient"])

    def test_pairs_need_both_members_and_identical_outputs_are_counted(self):
        gate = [case for case in self.exam.cases if case["role"] == "gate"]
        first = self.exam.grading[gate[0]["id"]]
        records = [{"id": case["id"], **output(first + "\n")} for case in gate]
        summary, answers, rows = evaluate.score_cases(gate, self.exam.grading, records, self.exam.document["pair_flags"])
        self.assertEqual(summary["pairs"], {"strict": 0, "lenient": 0, "total": 64})
        self.assertEqual(summary["pairs_identical_output"], 64)
        self.assertEqual(summary["answers"]["strict"], 1)
        self.assertEqual(sum(group["total"] for group in summary["pairs_by_frame_group"].values()), 64)
        self.assertEqual({name: group["total"] for name, group in summary["pairs_by_frame"].items()}, {str(n): 8 for n in range(1, 9)})
        chosen = evaluate.representative(rows, answers)
        self.assertTrue(all(len(examples) <= 3 for examples in chosen.values()))
        self.assertEqual(next(iter(chosen.values()))[0]["pair"], rows[1]["pair"] if rows[0]["strict"] else rows[0]["pair"])


@unittest.skipUnless(support.CORPUS.is_dir(), support.NEEDS_CORPUS)
class Recording(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        torch.set_num_threads(1)
        cls.directory = tempfile.TemporaryDirectory()
        cls.root = Path(cls.directory.name)
        cls.parent, cls.sha = support.toy_parent(cls.root)
        cls.plan = support.toy_plan(cls.parent, cls.sha, updates=6)
        cls.report = train.fit(cls.plan, "toy-plan", cls.root / "run", device="cpu")
        cls.seen = []

        def oracle(model, prompt, **settings):
            cls.seen.append((prompt, settings))
            try:
                mode, _, label, _ = data.derive(prompt)
                return output(data.answer_text(mode, label) + "\n")
            except ValueError:
                return output('{"reply":"ok"}\n')

        cls.hashes = evaluate.record_run(cls.root / "run/model.npz", cls.report["selected_checkpoint_sha256"], "after",
                                         cls.root / "after", device="cpu", run_report=cls.root / "run/report.json",
                                         plan=cls.plan, plan_sha256="toy-plan", generator=oracle)
        cls.scores = evaluate.score_directory(cls.root / "after", plan=cls.plan)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_sections_are_recorded_gate_first_with_raw_fields(self):
        self.assertEqual(list(self.hashes), list(evaluate.FILES))
        gate = json.loads((self.root / "after/gate.json").read_text())
        self.assertEqual((gate["status"], len(gate["records"]), gate["role"]), ("complete", 128, "gate"))
        record = gate["records"][0]
        self.assertTrue({"id", "prompt", "prompt_sha256", "text", "token_ids", "valid_utf8", "finish_reason",
                         "prior_call_supplied_by_host"} <= set(record))
        self.assertEqual(gate["checkpoint_sha256"], self.report["selected_checkpoint_sha256"])
        self.assertEqual(gate["generation"], {"temperature": 0.0, "seed": 1729, "maximum_new_tokens": 256})
        self.assertEqual(self.seen[0][0], record["prompt"])
        self.assertTrue(all(settings == evaluate.GENERATION for _, settings in self.seen))
        exam = data.Exam()
        self.assertEqual(sorted(r["prompt_sha256"] for r in gate["records"]),
                         sorted(r["prompt_sha256"] for r in exam.records["gate"]))
        tracked = json.loads((self.root / "after/tracked-exam.json").read_text())
        self.assertEqual(len(tracked["outputs"]), 32)

    def test_scores_of_an_oracle_and_the_written_transcript(self):
        scores = self.scores
        self.assertEqual(scores["gate"]["pairs"], {"strict": 64, "lenient": 64, "total": 64})
        self.assertTrue(scores["gate"]["passed_primary"] and scores["gate"]["recording_complete"])
        self.assertEqual(scores["missing_data"]["missing_field"]["strict"], 8)
        self.assertEqual(scores["missing_data"]["unavailable"]["strict"], 8)
        self.assertEqual(scores["bare_diagnostic"]["pairs"]["strict"], 64)
        self.assertEqual(scores["validation_generation"]["answers"]["strict"], 80)
        self.assertEqual(scores["validation_generation"]["pairs"], {"strict": 32, "lenient": 32, "total": 32})
        self.assertEqual({mode: group["total"] for mode, group in scores["validation_generation"]["pairs_by_mode"].items()},
                         {"json": 16, "value": 16})
        self.assertTrue(scores["headline"].startswith("GATE MET: 64 of 64"))
        self.assertIn("substituted generator", scores["generator"])
        self.assertEqual(len(scores["validation_generation"]["teacher_forced_exact_vector"]), 80)
        self.assertEqual(scores["tracked_exam"]["cases"], 32)
        self.assertIn("language", scores["teacher_forced"])
        text = (self.root / "after/answers.txt").read_text()
        self.assertIn("exam.f1.g1.total  strict=True", text)

    def test_recording_refuses_wrong_checkpoints_and_existing_output(self):
        with self.assertRaisesRegex(FileExistsError, "fresh exclusive directory"):
            evaluate.record_run(self.root / "run/model.npz", self.report["selected_checkpoint_sha256"], "after",
                                self.root / "after", device="cpu", run_report=self.root / "run/report.json",
                                plan=self.plan, plan_sha256="toy-plan")
        with self.assertRaisesRegex(ValueError, "differs from the expected state"):
            evaluate.record_run(self.root / "run/model.npz", "0" * 64, "after", self.root / "x1", device="cpu",
                                run_report=self.root / "run/report.json", plan=self.plan, plan_sha256="toy-plan")
        with self.assertRaisesRegex(ValueError, "must use the frozen parent"):
            evaluate.record_run(self.root / "run/model.npz", self.report["selected_checkpoint_sha256"], "before",
                                self.root / "x2", device="cpu", plan=self.plan, plan_sha256="toy-plan")
        with self.assertRaisesRegex(ValueError, "selected by the single run"):
            evaluate.record_run(self.parent, self.sha, "after", self.root / "x3", device="cpu",
                                run_report=self.root / "run/report.json", plan=self.plan, plan_sha256="toy-plan")
        self.assertFalse(any((self.root / name).exists() for name in ("x1", "x2", "x3")))

    def test_an_interrupted_section_keeps_what_it_generated(self):
        calls = []

        def failing(model, prompt, **settings):
            calls.append(1)
            if len(calls) == 3:
                raise RuntimeError("device error")
            if len(calls) == 6:
                raise KeyboardInterrupt()
            return output('{"reply":"0"}\n')

        with self.assertRaises(KeyboardInterrupt):
            evaluate.record_run(self.root / "run/model.npz", self.report["selected_checkpoint_sha256"], "after",
                                self.root / "interrupted", device="cpu", run_report=self.root / "run/report.json",
                                plan=self.plan, plan_sha256="toy-plan", generator=failing)
        gate = json.loads((self.root / "interrupted/gate.json").read_text())
        self.assertEqual((gate["status"], len(gate["records"])), ("incomplete: KeyboardInterrupt", 5))
        self.assertIn("RuntimeError: device error", gate["records"][2]["error"])
        scores = evaluate.score_directory(self.root / "interrupted", plan=self.plan)
        self.assertFalse(scores["gate"]["passed_primary"])
        self.assertIn("incomplete", scores["headline"])
        with self.assertRaisesRegex(ValueError, "needs the report of the single run"):
            evaluate.record_run(self.root / "run/model.npz", self.report["selected_checkpoint_sha256"], "after",
                                self.root / "x4", device="cpu", plan=self.plan, plan_sha256="toy-plan")

    def test_incomplete_gate_recording_never_passes(self):
        directory = self.root / "partial"
        directory.mkdir()
        gate = json.loads((self.root / "after/gate.json").read_text())
        gate.update(status="incomplete: wall_limit", records=gate["records"][:100])
        (directory / "gate.json").write_text(json.dumps(gate))
        scores = evaluate.score_directory(directory, plan=self.plan)
        self.assertFalse(scores["gate"]["passed_primary"] or scores["gate"]["recording_complete"])
        self.assertEqual(scores["gate"]["pairs"]["total"], 64)
        self.assertEqual(scores["gate"]["pairs"]["strict"], 50)

    def test_rehearsal_plan_is_valid_and_points_away_from_the_student(self):
        plan = rehearse.rehearsal_plan(train.load_plan()[0], self.parent, self.sha, 12)
        train.validate_plan(plan)
        self.assertEqual(plan["parent"]["stage"], "rehearsal-random-init")
        self.assertNotIn("mps-stage-2", plan["parent"]["path"])


if __name__ == "__main__":
    unittest.main()
