# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.

import copy
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

from contrib.b3ai.tool_learning import evaluate
from contrib.b3ai.tool_learning.data import InstructionCorpus
from contrib.b3ai.tool_learning.protocol import TOOLS, compact, make_fixture


def generated(text, valid=True):
    return {"text": text, "valid_utf8": valid, "token_ids": list(text.encode()),
            "finish_reason": "end_of_document", "temperature": 0.0, "seed": 1729}


class EvaluationTests(unittest.TestCase):
    def setUp(self):
        self.row = {"id": "probe", "category": "tool-request", "question": "Read a node observation.",
                    "tools_enabled": True, "expected_tool": "getblockchaininfo",
                    "fixtures": {tool: make_fixture(tool, 31) for tool in TOOLS}}

    def test_wrong_generated_tool_gets_its_own_fixture_without_answer_substitution(self):
        calls = []
        outputs = iter([generated('{"tool":"getnetworkinfo","arguments":{}}'),
                        generated('{"reply":"Simulation: connections=71."}')])
        def generate(model, prompt, **kwargs):
            calls.append(prompt)
            return next(outputs)
        result = evaluate.trajectory(None, self.row, generator=generate)
        self.assertEqual(result["selected_tool"], "getnetworkinfo")
        self.assertFalse(result["correct_tool_choice"])
        self.assertEqual(result["fixture"], self.row["fixtures"]["getnetworkinfo"])
        self.assertIn(compact(self.row["fixtures"]["getnetworkinfo"]), calls[1])
        self.assertNotIn('"blocks"', calls[1])
        self.assertNotIn("expected_tool", calls[0])
        self.assertEqual(result["followup"]["text"], '{"reply":"Simulation: connections=71."}')

    def test_invalid_or_disabled_outputs_are_preserved_without_dispatch(self):
        for text, valid in (("use getblockchaininfo", True), ('{"tool":"shell","arguments":{}}', True),
                            ('{"tool":"getnetworkinfo","arguments":{}}', False)):
            with self.subTest(text=text, valid=valid), mock.patch.object(evaluate, "simulated_dispatch") as dispatch:
                output = generated(text, valid)
                result = evaluate.trajectory(None, self.row, generator=lambda *a, **k: output)
                dispatch.assert_not_called()
                self.assertTrue(result["blocked"])
                self.assertEqual(result["decision"], output)
                self.assertIsNone(result["fixture"])
        row = {"id": "disabled", "category": "refusal", "question": "Check connections.", "tools_enabled": False}
        with mock.patch.object(evaluate, "simulated_dispatch") as dispatch:
            result = evaluate.trajectory(None, row, generator=lambda *a, **k: generated('{"tool":"getnetworkinfo","arguments":{}}'))
            self.assertTrue(result["blocked"])
            dispatch.assert_not_called()

    def test_reply_and_bad_followup_never_become_second_tool_calls(self):
        with mock.patch.object(evaluate, "simulated_dispatch") as dispatch:
            result = evaluate.trajectory(None, self.row, generator=lambda *a, **k: generated('{"reply":"I cannot observe it."}'))
            dispatch.assert_not_called()
            self.assertFalse(result["correct_tool_choice"])
        outputs = iter([generated('{"tool":"getblockchaininfo","arguments":{}}'),
                        generated('{"tool":"getnetworkinfo","arguments":{}}')])
        result = evaluate.trajectory(None, self.row, generator=lambda *a, **k: next(outputs))
        self.assertTrue(result["correct_tool_choice"])
        self.assertTrue(result["blocked"])
        self.assertIsNotNone(result["fixture"])
        self.assertIn("disabled", result["parse_error"])

    def test_supplied_result_probe_is_labeled_as_host_context_not_a_model_choice(self):
        row = {"id": "provided", "category": "result-reading", "question": "Explain this example.",
               "tools_enabled": True, "prior_tool": "getnetworkinfo", "fixture": make_fixture("getnetworkinfo", 31)}
        result = evaluate.trajectory(None, row, generator=lambda *a, **k: generated('{"reply":"Simulation: local connections=71."}'))
        self.assertEqual(result["prior_call_supplied_by_host"]["tool"], "getnetworkinfo")
        self.assertIsNone(result["decision"])
        self.assertIsNone(result["selected_tool"])
        self.assertIsNone(result["correct_tool_choice"])

    def test_resource_caps_preserve_raw_decision_and_final_reply_without_more_generation(self):
        first = generated('{"tool":"getblockchaininfo","arguments":{}}')
        generate = mock.Mock(return_value=first)
        checks = iter([None, "wall_limit"])
        result = evaluate.trajectory(None, self.row, generator=generate, check_resources=lambda: next(checks))
        self.assertEqual(result["resource_stop"], "wall_limit")
        self.assertEqual(result["decision"], first)
        self.assertIsNone(result["fixture"])
        self.assertEqual(generate.call_count, 1)
        second = generated('{"reply":"Simulation: blocks=700031."}')
        outputs = iter([first, second])
        checks = iter([None, None, None, "memory_limit"])
        result = evaluate.trajectory(None, self.row, generator=lambda *a, **k: next(outputs),
                                     check_resources=lambda: next(checks))
        self.assertEqual(result["resource_stop"], "memory_limit")
        self.assertEqual(result["followup"], second)
        self.assertTrue(result["correct_tool_choice"])

    def test_actual_frozen_exam_has_disjoint_wording_and_valid_source_identity(self):
        corpus = InstructionCorpus(Path(__file__).parents[1] / "curriculum.json")
        exam, raw = evaluate.read_exam(Path(__file__).parents[1] / "exam.json", corpus)
        self.assertEqual(len(exam["questions"]), 32)
        self.assertLess(len(raw), 65536)

    def test_exam_overlap_mismatched_sources_unsafe_fixtures_and_boolean_temperature_reject(self):
        corpus = SimpleNamespace(records={"train": [{"question": "old wording"}], "validation": []},
                                 source_curriculum_sha256="a" * 64, baseline_commit="b" * 40)
        base = {"schema": "b3-simulated-tool-exam/v1", "source_curriculum_sha256": "a" * 64,
                "source_baseline": "b" * 40, "generation": {"maximum_new_byte_tokens": 256, "temperature": 0.0, "seed": 1729},
                "questions": [copy.deepcopy(self.row)]}
        cases = []
        overlap = copy.deepcopy(base); overlap["questions"][0]["question"] = " OLD   WORDING "
        cases.append(overlap)
        mismatch = copy.deepcopy(base); mismatch["source_curriculum_sha256"] = "c" * 64
        cases.append(mismatch)
        unsafe = copy.deepcopy(base); unsafe["questions"][0]["fixtures"]["getnetworkinfo"]["data"]["proxy"] = "secret"
        cases.append(unsafe)
        flag = copy.deepcopy(base); flag["generation"]["temperature"] = False
        cases.append(flag)
        oversized = copy.deepcopy(base)
        market = make_fixture("listflowmeshmarkets", 2)["data"][0]
        oversized["questions"][0]["fixtures"]["listflowmeshmarkets"]["data"] = [market] * 16
        cases.append(oversized)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "exam.json"
            for case in cases:
                path.write_text(json.dumps(case))
                with self.subTest(case=case), self.assertRaises(ValueError):
                    evaluate.read_exam(path, corpus)

    def test_output_overwrite_and_bad_hash_stop_before_runtime_or_generation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "out.json"; output.write_text("preserve")
            with mock.patch.object(evaluate, "runtime") as runtime:
                with self.assertRaises(FileExistsError):
                    evaluate.run(root / "missing", root / "c", root / "e", output, expected_checkpoint_sha256="x")
                checkpoint = root / "checkpoint.npz"; checkpoint.write_bytes(b"bad")
                with self.assertRaises(ValueError):
                    evaluate.run(checkpoint, root / "c", root / "e", root / "fresh", expected_checkpoint_sha256="x")
                runtime.assert_not_called()
            self.assertEqual(output.read_text(), "preserve")


if __name__ == "__main__":
    unittest.main()
