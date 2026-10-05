# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Real updates on a tiny random CPU model exercise the bounded trainer end to end."""

import copy
import json
import math
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import numpy as np
import torch

import support
from contrib.b3ai.counterfactual import data, train
from contrib.b3ai.language.batches import Corpus
from contrib.b3ai.language.checkpoint import load_checkpoint
from contrib.b3ai.language.evaluate import continuation
from contrib.b3ai.tool_learning.data import InstructionCorpus


class PlanRules(unittest.TestCase):
    def test_frozen_plan_is_valid_and_bounded(self):
        plan, _ = train.load_plan()
        train.validate_plan(plan)
        self.assertEqual((plan["limits"]["maximum_updates"], plan["limits"]["maximum_wall_seconds"]), (1000, 500))
        self.assertEqual((plan["limits"]["maximum_supervised_targets"], plan["limits"]["maximum_memory_bytes"]),
                         (8192000, 34359738368))
        self.assertEqual(plan["replay_categories"], ["b3_fact", "chat", "refusal", "tool_choice"])
        for change in (lambda p: p["limits"].update(maximum_updates=1001), lambda p: p["limits"].update(maximum_wall_seconds=501),
                       lambda p: p["limits"].update(maximum_supervised_targets=8192001),
                       lambda p: p["limits"].update(maximum_memory_bytes=34359738369),
                       lambda p: p["batch"].update(language_rows=4), lambda p: p["phases"][0].update(cycle=["json"]),
                       lambda p: p["phases"][-1].update(last=999)):
            broken = copy.deepcopy(plan)
            change(broken)
            with self.assertRaises(ValueError):
                train.validate_plan(broken)

    def test_learning_rate_schedule(self):
        rate = train.load_plan()[0]["optimizer"]["learning_rate"]
        values = [train.learning_rate(update, rate) for update in range(1, 1001)]
        self.assertTrue(all(type(value) is float for value in values))
        self.assertAlmostEqual(values[0], 6e-6)
        self.assertEqual(values[49], 3e-4)
        self.assertEqual(values[699], 3e-4)
        self.assertAlmostEqual(values[849], 2e-4)
        self.assertAlmostEqual(values[999], 1e-4)
        self.assertEqual(max(values), 3e-4)

    def test_selection_follows_the_json_gate_then_loss_then_recency(self):
        def state(json_pairs, json_turns, turns, loss):
            return {"language": loss, "replay": {"replayed": loss},
                    "grounding_validation": {"loss": loss, "exact_turns": turns,
                                             "json": {"pairs_both_exact": json_pairs, "exact_turns": json_turns}}}
        bare_only = train.selection_key(state(0, 0, 40, 0.1), 250)
        some_json = train.selection_key(state(1, 9, 20, 0.9), 400)
        self.assertGreater(some_json, bare_only)
        self.assertGreater(train.selection_key(state(1, 9, 20, 0.5), 300), some_json)
        self.assertGreater(train.selection_key(state(1, 9, 20, 0.9), 500), some_json)


@unittest.skipUnless(support.CORPUS.is_dir(), support.NEEDS_CORPUS)
class ToyRun(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        torch.set_num_threads(1)
        cls.directory = tempfile.TemporaryDirectory()
        cls.root = Path(cls.directory.name)
        cls.parent, cls.sha = support.toy_parent(cls.root)
        cls.plan = support.toy_plan(cls.parent, cls.sha)
        cls.report = train.fit(cls.plan, "toy-plan", cls.root / "run", device="cpu", ledger=cls.root / "ledger.json")

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_plan(self, name, change=None, **arguments):
        plan = copy.deepcopy(self.plan)
        if change:
            change(plan)
        return train.fit(plan, "toy-plan", self.root / name, device="cpu", **arguments)

    def test_complete_run_crosses_phases_and_saves_both_states(self):
        report = self.report
        self.assertEqual((report["attempted_updates"], report["stop_reason"]), (6, "update_limit"))
        self.assertTrue(report["status"].startswith("finished grounding experiment"))
        self.assertTrue(report["parent_unchanged"])
        self.assertEqual([entry["update"] for entry in report["history"]], [0, 2, 4, 6])
        self.assertFalse(report["exam_parsed_by_trainer"] or report["generation_performed"])
        lines = [json.loads(line) for line in (self.root / "run/updates.jsonl").read_text().splitlines()]
        self.assertEqual([line["mode"] for line in lines], ["value", "value", "value", "json", "json", "value"])
        self.assertEqual([line["learning_rate"] for line in lines][:2], [1.5e-4, 3e-4])
        self.assertAlmostEqual(lines[-1]["learning_rate"], 1e-4)
        self.assertTrue(all(line["width"] == 1024 and len(line["targets"]) == 3 for line in lines))
        self.assertTrue(all(line["value"] is not None and line["formatting"] is not None for line in lines))
        self.assertGreaterEqual(sum(lines[0]["class_targets"].values()), lines[0]["targets"][1])
        self.assertEqual(report["training_targets"], sum(sum(line["targets"]) for line in lines))
        self.assertEqual(report["training_targets"], sum(report["stream_targets"].values()))
        model, optimizer, metadata = load_checkpoint(self.root / "run/last.npz")
        self.assertEqual((metadata["stage"], metadata["selected_or_saved_step"]), ("counterfactual-grounding", 6))
        self.assertTrue(metadata["optimizer_reset"] and metadata["batch_rng_reset"])
        self.assertEqual({state["step"].item() for state in optimizer.state.values()}, {6.0})
        self.assertEqual(metadata["parent_checkpoint_sha256"], self.sha)
        selected = load_checkpoint(self.root / "run/model.npz")[2]
        self.assertEqual(selected["selected_or_saved_step"], report["selected_update"])
        self.assertEqual(train.file_hash(self.root / "run/model.npz"), report["selected_checkpoint_sha256"])
        ledger = json.loads((self.root / "ledger.json").read_text())
        self.assertEqual(ledger["run_id"], report["run_id"])
        entry = report["history"][-1]
        self.assertIn("value_target_loss", entry["grounding_validation"])
        self.assertIn("formatting_loss", entry["grounding_validation"])
        self.assertEqual(entry["grounding_validation"]["class_targets"]["unsupervised_terminator"], 0)
        unsupervised = sum(len(record["label"]) == 2 and record["label"][0] in "23456789"
                           for record in data.GroundingCorpus().turns["train"])
        self.assertEqual(entry["training_fit"]["class_targets"]["unsupervised_terminator"], unsupervised)
        self.assertGreater(unsupervised, 0)
        self.assertGreaterEqual(entry["training_fit"]["supervised_exact_turns"], entry["training_fit"]["exact_turns"])
        self.assertEqual(len(entry["grounding_validation"]["exact_vector"]), 80)
        self.assertEqual(len(entry["training_fit"]["exact_vector"]), 320)
        self.assertEqual(entry["grounding_validation"]["json"]["pairs"], 16)
        self.assertEqual(set(entry["replay"]["categories"]), {"b3_fact", "chat", "refusal", "tool_choice", "tool_result", "tool_failure"})

    def test_second_start_is_refused_by_the_ledger_and_by_the_output(self):
        with self.assertRaisesRegex(FileExistsError, "run ledger exists"):
            self.run_plan("again", ledger=self.root / "ledger.json")
        with self.assertRaisesRegex(FileExistsError, "fresh exclusive directory"):
            self.run_plan("run")
        self.assertFalse((self.root / "again").exists())

    def test_stream_losses_equal_the_frozen_loss_and_use_one_forward(self):
        plan = self.plan
        stories = Corpus(support.CORPUS)
        instructions = InstructionCorpus(support.REPOSITORY / plan["data"]["instruction_corpus"])
        grounding = data.GroundingCorpus()
        replay = train.replay_records(instructions, plan["replay_categories"])
        schedule = data.build_schedule(grounding, replay, plan)
        model = load_checkpoint(self.parent)[0]
        for entry in (schedule[0], schedule[4]):
            batch = train.build_batch(stories, grounding, instructions, replay, entry, np.random.default_rng(5), plan)
            self.assertEqual(batch["tokens"].shape[0], 16)
            self.assertEqual(batch["weight"].dtype, np.float32)
            with mock.patch.object(type(model), "forward", autospec=True, side_effect=type(model).forward) as forward:
                total, streams, classes = train.batch_loss(model, batch, "cpu")
            self.assertEqual(forward.call_count, 1)
            for index in range(3):
                rows = batch["stream"] == index
                frozen = model.loss(*(torch.from_numpy(batch[key][rows]) for key in ("tokens", "targets", "weight")))
                self.assertAlmostEqual(streams[index], float(frozen.detach()), places=5)
            self.assertAlmostEqual(float(total.detach()), sum(streams) / 3, places=5)
            self.assertTrue(math.isfinite(classes["value"]) and math.isfinite(classes["formatting"]))
            self.assertEqual(sum(classes["class_targets"].values()), int((batch["kind"] > 0).sum()))
            grounding_rows = batch["kind"][batch["stream"] == 1]
            self.assertTrue((grounding_rows > 0).any() and not (batch["kind"][batch["stream"] != 1] > 0).any())

    def test_target_cap_stops_before_the_update_and_restores_the_generator(self):
        first = sum(json.loads((self.root / "run/updates.jsonl").read_text().splitlines()[0])["targets"])
        report = self.run_plan("capped", lambda plan: plan["limits"].update(maximum_supervised_targets=first))
        self.assertEqual((report["attempted_updates"], report["stop_reason"]), (1, "target_limit"))
        self.assertLessEqual(report["training_targets"], first)
        metadata = load_checkpoint(self.root / "capped/last.npz")[2]
        rng = np.random.default_rng(self.plan["seeds"]["language_batches"])
        Corpus(support.CORPUS).batch("train", rng, 3, 1024)
        self.assertEqual(metadata["language_rng_state"], rng.bit_generator.state)

    def test_wall_cap_stops_between_updates_and_still_reports(self):
        report = self.run_plan("walled", lambda plan: plan["limits"].update(maximum_wall_seconds=400, wall_reserve_seconds=399.999))
        self.assertEqual((report["attempted_updates"], report["stop_reason"]), (0, "wall_limit"))
        self.assertTrue((self.root / "walled/model.npz").exists() and (self.root / "walled/last.npz").exists())
        self.assertEqual(report["selected_update"], 0)

    def test_memory_cap_counts_process_plus_device(self):
        report = self.run_plan("memory", lambda plan: plan["limits"].update(maximum_memory_bytes=1024))
        self.assertEqual(report["stop_reason"], "preflight_abort")
        self.assertEqual(report["attempted_updates"], 0)

    def test_preflight_abort_makes_no_update_and_no_ledger(self):
        ledger = self.root / "ledger-preflight.json"
        report = self.run_plan("preflight", lambda plan: plan["preflight"].update(projection_limit_seconds=0.001), ledger=ledger)
        self.assertEqual((report["attempted_updates"], report["stop_reason"]), (0, "preflight_abort"))
        self.assertFalse(ledger.exists() or (self.root / "preflight/updates.jsonl").exists())
        before, after = load_checkpoint(self.parent)[0], load_checkpoint(self.root / "preflight/model.npz")[0]
        self.assertTrue(all(torch.equal(before.params[name], after.params[name]) for name in before.params))

    def test_timing_passes_change_no_weight_and_a_failed_first_update_consumes_nothing(self):
        original, sums = train.batch_loss, []

        def spying(model, batch, device):
            sums.append(float(sum(parameter.detach().double().abs().sum() for parameter in model.params.values())))
            return original(model, batch, device)

        with mock.patch.object(train, "batch_loss", spying):
            self.run_plan("spied", lambda plan: plan["preflight"].update(warmup_passes=2, timed_passes=3))
        parent = load_checkpoint(self.parent)[0]
        expected = float(sum(parameter.detach().double().abs().sum() for parameter in parent.params.values()))
        # Five timing passes and the first real update all see the untouched parent.
        self.assertEqual(sums[:6], [expected] * 6)
        self.assertNotEqual(sums[6], expected)
        ledger, calls = self.root / "ledger-first.json", []

        def failing(model, batch, device):
            calls.append(1)
            if len(calls) == 3:
                raise RuntimeError("device error on the first real batch")
            return original(model, batch, device)

        with mock.patch.object(train, "batch_loss", failing), self.assertRaisesRegex(RuntimeError, "device error"):
            self.run_plan("first-fails", ledger=ledger)
        report = json.loads((self.root / "first-fails/report.json").read_text())
        self.assertEqual((report["attempted_updates"], report["first_update_completed"]), (0, False))
        self.assertFalse(ledger.exists())

    def test_interrupt_still_writes_a_truthful_report(self):
        original, calls = train.batch_loss, []

        def interrupting(model, batch, device):
            calls.append(1)
            if len(calls) == 5:
                raise KeyboardInterrupt()
            return original(model, batch, device)

        with mock.patch.object(train, "batch_loss", interrupting), self.assertRaises(KeyboardInterrupt):
            self.run_plan("interrupted")
        report = json.loads((self.root / "interrupted/report.json").read_text())
        self.assertEqual((report["stop_reason"], report["attempted_updates"]), ("interrupted", 2))
        self.assertEqual(report["selected_checkpoint_sha256"], train.file_hash(self.root / "interrupted/model.npz"))

    def test_parent_must_reproduce_and_match_before_any_update(self):
        ledger = self.root / "ledger-parent.json"
        with self.assertRaisesRegex(ValueError, "did not reproduce"):
            self.run_plan("mismatch", lambda plan: plan["parent"].update(language_validation_loss=0.5), ledger=ledger)
        self.assertFalse(ledger.exists())
        report = json.loads((self.root / "mismatch/report.json").read_text())
        self.assertEqual((report["attempted_updates"], report["stop_reason"]), (0, "failure"))
        for change, message in ((lambda plan: plan["parent"].update(sha256="0" * 64), "SHA-256 does not match"),
                                (lambda plan: plan["parent"].update(stage="other"), "compatible own random-start"),
                                (lambda plan: plan["parent"].update(optimizer_steps=7), "AdamW steps"),
                                (lambda plan: plan["data"].update(grounding_curriculum_sha256="0" * 64), "differs from the frozen plan"),
                                (lambda plan: plan["data"].update(schedule_sha256="0" * 64), "Sampling schedule differs")):
            with self.assertRaisesRegex(ValueError, message):
                self.run_plan("refused", change)
            self.assertFalse((self.root / "refused").exists())

    def test_nonfinite_loss_ends_the_run_and_keeps_the_selected_state(self):
        original, calls = train.batch_loss, []

        def poisoned(model, batch, device):
            total, streams, classes = original(model, batch, device)
            calls.append(1)
            # Calls 1-2 are the no-update timing passes; call 4 is the second real update.
            return (total * float("nan"), streams, classes) if len(calls) == 4 else (total, streams, classes)

        with mock.patch.object(train, "batch_loss", poisoned), self.assertRaisesRegex(ValueError, "Non-finite training loss"):
            self.run_plan("poisoned")
        report = json.loads((self.root / "poisoned/report.json").read_text())
        self.assertEqual((report["stop_reason"], report["attempted_updates"]), ("failure", 1))
        self.assertIn("Non-finite training loss", report["failure"])
        self.assertTrue((self.root / "poisoned/model.npz").exists())

    def test_teacher_forced_exactness_equals_greedy_generation(self):
        grounding = data.GroundingCorpus()
        model = load_checkpoint(self.root / "run/last.npz")[0]
        records = grounding.turns["validation"]
        target = next(record for record in records if record["answer"] == "unknown")
        text = (target["answer"] + "\n").encode()
        # A constant model that repeats one byte forever: both paths must agree nothing is exact.
        with torch.no_grad():
            for name, parameter in model.params.items():
                if name != "head.bias":
                    parameter.zero_()
            model.params["head.bias"].fill_(-50.0)
            model.params["head.bias"][text[0]] = 5.0
        forced = train.score_grounding(model, grounding, "validation", "cpu")
        self.assertEqual(len(forced["exact_vector"]), 80)
        for record, flag in list(zip(records, forced["exact_vector"]))[:16]:
            generated = continuation(model, record["prompt"], maximum_new_tokens=24)
            exact = generated["finish_reason"] == "end_of_document" and generated["text"] == record["answer"] + "\n"
            self.assertEqual(exact, flag == "1")
        trained = load_checkpoint(self.root / "run/last.npz")[0]
        forced = train.score_grounding(trained, grounding, "validation", "cpu")
        for record, flag in list(zip(records, forced["exact_vector"]))[:8]:
            generated = continuation(trained, record["prompt"], maximum_new_tokens=24)
            exact = generated["finish_reason"] == "end_of_document" and generated["text"] == record["answer"] + "\n"
            self.assertEqual(exact, flag == "1")


if __name__ == "__main__":
    unittest.main()
