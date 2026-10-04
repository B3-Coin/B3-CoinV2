# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.

import contextlib
from collections import Counter
import io
from pathlib import Path
import re
import tempfile
import unittest

from contrib.b3ai.scratch.data import corpus_bytes, load_curriculum, validate_records
from contrib.b3ai.scratch.dialogue_data import builtin_dialogues, main


class DialogueDataTests(unittest.TestCase):
    def test_record_format_bounds_and_distinct_questions(self):
        rows = builtin_dialogues()
        self.assertEqual(validate_records(rows), rows)
        questions = {"train": set(), "validation": set()}
        counts = {"train": 0, "validation": 0}
        answers = {"train": Counter(), "validation": Counter()}
        short_records = 0
        for row in rows:
            match = re.fullmatch(r"\nUser: ([^\n]+)\nAssistant: ([^\n]+)\n", row["text"])
            self.assertIsNotNone(match)
            question, answer = match.groups()
            turn_bytes = len(row["text"].encode("utf-8"))
            self.assertLessEqual(turn_bytes - 1, 256)
            short_records += turn_bytes <= 64
            self.assertGreaterEqual(len(answer.encode("utf-8")), 15)
            self.assertLessEqual(len(answer.encode("utf-8")), 80)
            self.assertNotIn(question.casefold(), questions[row["split"]])
            questions[row["split"]].add(question.casefold())
            answers[row["split"]][answer] += 1
            self.assertTrue(row["reviewed"])
            self.assertRegex(row["source"], r"^teacher-authored;(135fc56|30825f9):")
            counts[row["split"]] += 1
        self.assertTrue(150 <= counts["train"] <= 250)
        self.assertTrue(18 <= counts["validation"] <= 24)
        self.assertFalse(questions["train"] & questions["validation"])
        self.assertTrue(18 <= len(answers["train"]) <= 24)
        self.assertEqual(set(answers["train"]), set(answers["validation"]))
        self.assertEqual(set(answers["train"].values()), {8})
        self.assertEqual(set(answers["validation"].values()), {1})
        self.assertGreaterEqual(short_records, 2)
        self.assertTrue({"hello", "what is b3?", "check node status", "does a hash prove truth?", "what are local memories?"} <= questions["train"])
        self.assertGreaterEqual(sum(len(question.split()) <= 6 for question in questions["train"]), 50)
        self.assertGreaterEqual(sum(len(question.split()) >= 12 for question in questions["train"]), 24)
        self.assertGreaterEqual(sum(len(question.split()) <= 6 for question in questions["validation"]), 8)
        self.assertGreaterEqual(sum(len(question.split()) >= 10 for question in questions["validation"]), 8)

    def test_targets_are_fresh_and_contain_no_claimed_live_observations(self):
        first = builtin_dialogues()
        second = builtin_dialogues()
        self.assertEqual(first, second)
        first[0]["text"] = "changed copy"
        self.assertNotEqual(first, builtin_dialogues())
        content = "\n".join(row["text"] for row in second)
        self.assertIn("Current node status is unknown; no live read has been run.", content)
        self.assertIn("errors are not empty lists", content)
        self.assertIn("No spending, signing, trading", content)
        self.assertIn("does not prove factual truth", content)
        # No generated addresses, amounts, block heights, certificates or success receipts.
        self.assertNotRegex(content, r"(?i)(balance is \d|height is \d|transaction succeeded|sent \d+ B3)")

    def test_local_memory_claim_cites_the_store_and_page(self):
        rows = builtin_dialogues()
        memory = [row for row in rows if "Local lessons are reference memory" in row["text"]]
        self.assertEqual(len(memory), 9)
        for row in memory:
            self.assertIn("135fc56:src/qt/b3ailessonstore.cpp;src/qt/b3aipage.cpp", row["source"])
            self.assertNotIn("deposit.h", row["source"])

    def test_cli_creates_new_corpus_and_refuses_existing_path(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "dialogues.jsonl"
            with contextlib.redirect_stdout(io.StringIO()):
                main(["--output", str(output)])
            expected = corpus_bytes(builtin_dialogues())
            self.assertEqual(output.read_bytes(), expected)
            self.assertEqual(load_curriculum(output), builtin_dialogues())
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit):
                    main(["--output", str(output)])
            self.assertEqual(output.read_bytes(), expected)


if __name__ == "__main__":
    unittest.main()
