# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Seals must cover every package file and notice any later change."""

import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

from contrib.b3ai.counterfactual import freeze, train

PARENT = freeze.REPOSITORY / train.load_plan()[0]["parent"]["path"]


class SealedSets(unittest.TestCase):
    def test_package_set_is_complete_and_ignores_caches_and_hidden_files(self):
        files = freeze.package_files()
        prefix = "contrib/b3ai/counterfactual/"
        self.assertEqual({name[len(prefix):] for name in files}, set(freeze.REQUIRED))
        self.assertTrue(all(len(digest) == 64 for digest in files.values()))
        self.assertEqual(files[prefix + "train.py"], freeze._hash(freeze.PACKAGE / "train.py"))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "nested").mkdir()
            (root / "__pycache__").mkdir()
            for name in ("a.json", "nested/b.txt", ".DS_Store", "nested/.hidden", "__pycache__/c.pyc"):
                (root / name).write_text(name)
            self.assertEqual(set(freeze._tree(root)), {"a.json", "nested/b.txt"})
            with mock.patch.object(freeze, "PACKAGE", root), mock.patch.object(freeze, "REPOSITORY", root.parent):
                with self.assertRaisesRegex(ValueError, "missing from the sealed set"):
                    freeze.package_files()

    def test_frozen_set_names_existing_files(self):
        self.assertEqual(len(set(freeze.FROZEN)), 22)
        self.assertTrue(all((freeze.REPOSITORY / name).is_file() for name in freeze.FROZEN))

    @unittest.skipUnless(PARENT.is_file(), "needs the local parent checkpoint under build-ai/ (git-ignored)")
    def test_verify_notices_any_change_and_any_broken_chain(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = freeze.seal_exam(root)
            (root / "schedule.json").write_text("[]\n")
            document = {"previous_seal_sha256": first, "package_files": freeze.package_files(),
                        "frozen_files": {name: freeze._hash(freeze.REPOSITORY / name) for name in freeze.FROZEN},
                        "parent": {"path": train.load_plan()[0]["parent"]["path"], "sha256": freeze._hash(PARENT)},
                        "schedule_sha256": freeze._hash(root / "schedule.json")}

            def sealed(change=None):
                changed = json.loads(json.dumps(document))
                if change:
                    change(changed)
                (root / freeze.SEALS[2]).write_text(json.dumps(changed))
                return freeze.verify(root, 2)

            self.assertEqual(set(sealed()), {"seal_1", "seal_2"})
            name = "contrib/b3ai/counterfactual/train.py"
            for change in (lambda d: d["package_files"].__setitem__(name, "0" * 64),
                           lambda d: d["package_files"].pop(name),
                           lambda d: d["package_files"].__setitem__("contrib/b3ai/counterfactual/extra.py", "0" * 64),
                           lambda d: d["frozen_files"].__setitem__(freeze.FROZEN[0], "0" * 64),
                           lambda d: d["parent"].__setitem__("sha256", "0" * 64),
                           lambda d: d.__setitem__("schedule_sha256", "0" * 64),
                           lambda d: d.__setitem__("previous_seal_sha256", "0" * 64)):
                with self.assertRaisesRegex(ValueError, "differs from seal 2"):
                    sealed(change)
            sealed()
            (root / "before").mkdir()
            (root / "before/gate.json").write_text("{}")
            (root / "before/.DS_Store").write_text("x")
            third = {"previous_seal_sha256": freeze._hash(root / freeze.SEALS[2]), "before": freeze._tree(root / "before")}
            (root / freeze.SEALS[3]).write_text(json.dumps(third))
            self.assertEqual(set(freeze.verify(root, 3)), {"seal_1", "seal_2", "seal_3"})
            (root / "before/gate.json").write_text("{ }")
            with self.assertRaisesRegex(ValueError, "differ from seal 3"):
                freeze.verify(root, 3)
            with self.assertRaisesRegex(ValueError, "exactly its recording"):
                (root / freeze.SEALS[3]).unlink()
                freeze.seal_baseline(root)


if __name__ == "__main__":
    unittest.main()
