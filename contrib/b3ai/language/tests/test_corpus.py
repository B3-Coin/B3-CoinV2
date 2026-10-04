# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.

import contextlib
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unicodedata
import unittest
from unittest import mock

from contrib.b3ai.language import corpus


DELIMITER = b"<|endoftext|>"
SPLITS = ("train", "validation", "test")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def normalized_hash(text):
    normalized = " ".join(unicodedata.normalize("NFC", text).casefold().split())
    return sha256(normalized.encode("utf-8"))


class FakeHTTPResponse(io.BytesIO):
    def __init__(self, data, status=206, headers=None):
        super().__init__(data)
        self.status = status
        self.headers = headers if headers is not None else {
            "Content-Range": "bytes 0-" + str(len(data) - 1) + "/1000000",
            "Content-Length": str(len(data)),
        }
        self.read_sizes = []

    def getcode(self):
        return self.status

    def read(self, size=-1):
        self.read_sizes.append(size)
        return super().read(size)


class CorpusTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)

    def source(self, name, documents, tail=b""):
        path = self.directory / name
        encoded = [item.encode("utf-8") if isinstance(item, str) else item for item in documents]
        path.write_bytes(b"".join(item + DELIMITER for item in encoded) + tail)
        return path

    def prepare(self, source, output_name="prepared", **options):
        settings = {"max_bytes": 1_000_000, "max_documents": 1_000, "min_document_bytes": 1}
        settings.update(options)
        output = self.directory / output_name
        manifest = corpus.prepare_corpus(source, output, **settings)
        return output, manifest

    def records(self, output, split=None):
        selected = (split,) if split is not None else SPLITS
        records = []
        for name in selected:
            data = (output / (name + ".jsonl")).read_text(encoding="utf-8")
            records.extend(json.loads(line) for line in data.splitlines())
        return records

    def check_manifest_files(self, output, manifest):
        self.assertEqual(json.loads((output / "manifest.json").read_text(encoding="utf-8")), manifest)
        self.assertIsInstance(json.loads((output / "attribution.json").read_text(encoding="utf-8")), dict)
        self.assertEqual(set(manifest["splits"]), set(SPLITS))
        for split in SPLITS:
            payload = (output / (split + ".jsonl")).read_bytes()
            summary = manifest["splits"][split]
            self.assertEqual(summary["documents"], len(self.records(output, split)))
            self.assertEqual(summary["bytes"], len(payload))
            self.assertEqual(summary["text_bytes"], sum(len(row["text"].encode("utf-8")) for row in self.records(output, split)))
            self.assertEqual(summary["sha256"], sha256(payload))

    def test_chunk_boundaries_preserve_unicode_delimiters_and_raw_provenance(self):
        raw_documents = [
            b"  A first story.\r\nIts second line remains separate.  \r\n",
            "Café, हिन्दी, and 🐝 make this a story with several UTF-8 widths.".encode("utf-8"),
            b"A final complete story follows the second delimiter.",
        ]
        source = self.source("unicode.txt", raw_documents)
        expected_texts = [
            "A first story.\nIts second line remains separate.",
            raw_documents[1].decode("utf-8"),
            raw_documents[2].decode("utf-8"),
        ]
        expected_fields = {
            "id", "text", "split", "source_key", "source_document",
            "source_byte_start", "source_byte_end", "raw_sha256", "normalized_sha256",
        }
        previous = None
        for chunk_size in (1, 7, 13, 29, 65536):
            with self.subTest(chunk_size=chunk_size):
                output, manifest = self.prepare(source, "chunks-" + str(chunk_size), chunk_size=chunk_size)
                rows = self.records(output)
                self.assertEqual([row["text"] for row in rows], expected_texts)
                self.assertEqual(len(rows), 3)
                offset = 0
                for number, (row, raw, text) in enumerate(zip(rows, raw_documents, expected_texts), 1):
                    self.assertTrue(expected_fields <= set(row))
                    self.assertEqual(row["id"], sha256(text.encode("utf-8")))
                    self.assertEqual(row["split"], "train")
                    self.assertEqual(row["source_key"], "train")
                    self.assertEqual(row["source_document"], number)
                    self.assertEqual(row["source_byte_start"], offset)
                    self.assertEqual(row["source_byte_end"], offset + len(raw))
                    self.assertEqual(source.read_bytes()[offset:row["source_byte_end"]], raw)
                    self.assertEqual(row["raw_sha256"], sha256(raw))
                    self.assertEqual(row["normalized_sha256"], normalized_hash(text))
                    offset += len(raw) + len(DELIMITER)
                serialized = [(output / (split + ".jsonl")).read_bytes() for split in SPLITS]
                if previous is not None:
                    self.assertEqual(serialized, previous)
                previous = serialized
                summary = manifest["source_summaries"][0]
                self.assertEqual(summary["bytes_read"], source.stat().st_size)
                self.assertTrue(summary["full_file"])
                self.assertEqual(summary["actual_full_sha256"], sha256(source.read_bytes()))
                self.assertEqual(summary["actual_prefix_sha256"], sha256(source.read_bytes()))
                self.assertFalse(summary["upstream_identity_verified"])
                self.check_manifest_files(output, manifest)

    def test_unterminated_final_story_and_partial_delimiter_are_discarded(self):
        complete = "A complete story has its delimiter."
        tail = b"A long unfinished story cannot become a training record.<|endof"
        source = self.source("unfinished.txt", [complete], tail)
        output, manifest = self.prepare(source, chunk_size=5)
        self.assertEqual([row["text"] for row in self.records(output)], [complete])
        summary = manifest["source_summaries"][0]
        self.assertEqual(summary["discarded_incomplete_tail_bytes"], len(tail))
        self.assertEqual(summary["bytes_read"], source.stat().st_size)
        self.assertTrue(summary["full_file"])
        self.assertEqual(summary["actual_full_sha256"], sha256(source.read_bytes()))

    def test_byte_cap_reports_only_admitted_prefix_and_no_full_file_hash(self):
        first = "First complete story."
        second = "Second complete story."
        tail = b"A third story extends beyond the permitted prefix."
        source = self.source("prefix.txt", [first, second], tail)
        complete_prefix = self.source("prefix-reference.txt", [first, second]).read_bytes()
        cap = len(complete_prefix) + 11
        output, manifest = self.prepare(source, max_bytes=cap, chunk_size=7)
        self.assertEqual([row["text"] for row in self.records(output)], [first, second])
        summary = manifest["source_summaries"][0]
        self.assertEqual(summary["bytes_read"], cap)
        self.assertFalse(summary["full_file"])
        self.assertIsNone(summary["actual_full_sha256"])
        self.assertEqual(summary["actual_prefix_sha256"], sha256(source.read_bytes()[:cap]))
        self.assertNotEqual(summary["actual_prefix_sha256"], sha256(source.read_bytes()))
        self.assertEqual(summary["discarded_incomplete_tail_bytes"], 11)
        self.assertFalse(summary["upstream_identity_verified"])

    def test_document_cap_is_a_continuous_delimiter_terminated_prefix(self):
        stories = ["Distinct complete story number " + str(number) + "." for number in range(8)]
        source = self.source("documents.txt", stories)
        output, manifest = self.prepare(source, max_documents=3, chunk_size=65536)
        self.assertEqual([row["text"] for row in self.records(output)], stories[:3])
        prefix = b"".join(story.encode("utf-8") + DELIMITER for story in stories[:3])
        summary = manifest["source_summaries"][0]
        self.assertEqual(summary["bytes_read"], len(prefix))
        self.assertEqual(summary["actual_prefix_sha256"], sha256(prefix))
        self.assertFalse(summary["full_file"])
        self.assertIsNone(summary["actual_full_sha256"])

    def test_document_cap_excludes_received_lookahead_from_admitted_hash(self):
        story = "A complete admitted story."
        prefix = story.encode("utf-8") + DELIMITER
        source = self.directory / "lookahead.txt"
        source.write_bytes(prefix + b"unadmitted \xff\x00 bytes")
        output, manifest = self.prepare(source, max_documents=1, chunk_size=65536)
        self.assertEqual([row["text"] for row in self.records(output)], [story])
        summary = manifest["source_summaries"][0]
        self.assertEqual(summary["bytes_received"], source.stat().st_size)
        self.assertEqual(summary["bytes_read"], len(prefix))
        self.assertEqual(summary["actual_prefix_sha256"], sha256(prefix))
        self.assertFalse(summary["full_file"])
        self.assertIsNone(summary["actual_full_sha256"])

    def test_source_split_assignment_is_deterministic_and_disjoint(self):
        training = ["Training story " + str(number) + " belongs only in training." for number in range(20)]
        official = ["Official validation story " + str(number) + " is reserved for evaluation." for number in range(40)]
        source = self.source("training.txt", training)
        validation = self.source("official-validation.txt", official)
        outputs = []
        for name, chunk_size in (("first", 7), ("second", 257)):
            output, manifest = self.prepare(source, name, validation_source=validation, chunk_size=chunk_size)
            outputs.append(output)
            self.assertEqual([row["text"] for row in self.records(output, "train")], training)
            by_split = {split: self.records(output, split) for split in SPLITS}
            ids = {split: {row["id"] for row in rows} for split, rows in by_split.items()}
            self.assertTrue(by_split["validation"])
            self.assertTrue(by_split["test"])
            self.assertFalse(ids["train"] & ids["validation"])
            self.assertFalse(ids["train"] & ids["test"])
            self.assertFalse(ids["validation"] & ids["test"])
            self.assertEqual({row["text"] for row in by_split["validation"] + by_split["test"]}, set(official))
            for row in by_split["validation"] + by_split["test"]:
                self.assertEqual(row["source_key"], "validation")
            self.assertEqual([item["source_key"] for item in manifest["source_summaries"]], ["train", "validation"])
            self.check_manifest_files(output, manifest)
        for split in SPLITS:
            self.assertEqual((outputs[0] / (split + ".jsonl")).read_bytes(), (outputs[1] / (split + ".jsonl")).read_bytes())

    def test_global_deduplication_keeps_training_before_official_validation(self):
        original = "Café birds sail across a quiet river and return to the old garden."
        variant = "CAFE\u0301\tBIRDS   SAIL ACROSS A QUIET RIVER AND RETURN TO THE OLD GARDEN."
        training_only = "A different training story describes the warm morning breeze."
        validation_only = "A unique official validation story describes a distant hill."
        source = self.source("dedup-train.txt", [original, original, variant, training_only])
        validation = self.source("dedup-validation.txt", [original, variant, validation_only])
        output, manifest = self.prepare(source, validation_source=validation, chunk_size=3)
        rows = self.records(output)
        self.assertEqual({row["text"] for row in rows}, {original, training_only, validation_only})
        self.assertEqual(len(rows), 3)
        original_row = next(row for row in rows if row["text"] == original)
        self.assertEqual(original_row["split"], "train")
        self.assertEqual(original_row["source_document"], 1)
        self.assertEqual(original_row["normalized_sha256"], normalized_hash(variant))
        self.assertEqual(next(row for row in rows if row["text"] == training_only)["source_document"], 4)
        self.assertEqual(next(row for row in rows if row["text"] == validation_only)["source_document"], 3)
        self.assertEqual(len({row["normalized_sha256"] for row in rows}), 3)
        for summary in manifest["source_summaries"]:
            self.assertEqual(summary["duplicates_exact"], 1)
            self.assertEqual(summary["duplicates_normalized"], 1)

    def test_two_source_document_budgets_are_ninety_ten(self):
        training = ["Training budget story " + str(number) + "." for number in range(30)]
        official = ["Validation budget story " + str(number) + "." for number in range(30)]
        source = self.source("budget-train.txt", training)
        validation = self.source("budget-validation.txt", official)
        output, _ = self.prepare(source, validation_source=validation, max_documents=10, chunk_size=11)
        self.assertEqual([row["text"] for row in self.records(output, "train")], training[:9])
        held_out = self.records(output, "validation") + self.records(output, "test")
        self.assertEqual([row["text"] for row in held_out], official[:1])

    def test_two_source_byte_budgets_are_ninety_ten(self):
        training = [("Train story " + str(number) + ".").ljust(64, "x") for number in range(30)]
        official = [("Valid story " + str(number) + ".").ljust(64, "y") for number in range(30)]
        source = self.source("byte-budget-train.txt", training)
        validation = self.source("byte-budget-validation.txt", official)
        output, manifest = self.prepare(source, validation_source=validation, max_bytes=1000, chunk_size=17)
        self.assertEqual([item["bytes_read"] for item in manifest["source_summaries"]], [900, 100])
        self.assertEqual(len(self.records(output, "train")), 900 // (64 + len(DELIMITER)))
        self.assertEqual(len(self.records(output, "validation") + self.records(output, "test")), 100 // (64 + len(DELIMITER)))
        for summary, path, budget in zip(manifest["source_summaries"], (source, validation), (900, 100)):
            self.assertFalse(summary["full_file"])
            self.assertIsNone(summary["actual_full_sha256"])
            self.assertEqual(summary["actual_prefix_sha256"], sha256(path.read_bytes()[:budget]))

    def test_single_validation_source_uses_full_document_budget(self):
        stories = ["A distinct held-out story " + str(number) + "." for number in range(40)]
        source = self.source("validation-only.txt", stories)
        output, manifest = self.prepare(source, source_split="validation", max_documents=20, chunk_size=5)
        self.assertEqual(self.records(output, "train"), [])
        rows = self.records(output, "validation") + self.records(output, "test")
        self.assertEqual(len(rows), 20)
        self.assertEqual({row["text"] for row in rows}, set(stories[:20]))
        self.assertEqual({row["source_key"] for row in rows}, {"validation"})
        self.assertEqual(manifest["source_summaries"][0]["source_key"], "validation")

    def test_default_minimum_uses_utf8_bytes_and_counts_skipped_segments(self):
        ascii_story = "a" * 64
        unicode_story = "é" * 32
        source = self.source("minimum.txt", [b"", "b" * 63, ascii_story, unicode_story])
        output = self.directory / "minimum-output"
        corpus.prepare_corpus(source, output, chunk_size=7)
        rows = self.records(output)
        self.assertEqual([row["text"] for row in rows], [ascii_story, unicode_story])
        self.assertEqual([row["source_document"] for row in rows], [3, 4])

    def test_existing_outputs_are_refused_without_modification(self):
        source = self.source("overwrite.txt", ["A complete source story."])
        for kind in ("empty-directory", "populated-directory", "file"):
            with self.subTest(kind=kind):
                output = self.directory / kind
                if kind == "file":
                    output.write_bytes(b"existing file")
                else:
                    output.mkdir()
                    if kind == "populated-directory":
                        (output / "keep.txt").write_bytes(b"existing directory content")
                before = {path.name: path.read_bytes() for path in output.iterdir()} if output.is_dir() else output.read_bytes()
                with self.assertRaises((FileExistsError, ValueError)):
                    corpus.prepare_corpus(source, output, min_document_bytes=1)
                after = {path.name: path.read_bytes() for path in output.iterdir()} if output.is_dir() else output.read_bytes()
                self.assertEqual(after, before)

    def test_invalid_utf8_or_nul_in_any_admitted_bytes_removes_output(self):
        valid = b"A complete valid story." + DELIMITER
        bad_sources = {
            "invalid-start": b"\xff" + DELIMITER,
            "invalid-sequence": b"bad \xc3( text" + DELIMITER,
            "incomplete-utf8-tail": valid + b"unfinished \xe2\x82",
            "invalid-tail": valid + b"unfinished \xff",
            "nul-document": b"bad\x00story" + DELIMITER,
            "nul-tail": valid + b"unfinished\x00story",
        }
        for name, payload in bad_sources.items():
            with self.subTest(name=name):
                source = self.directory / (name + ".txt")
                source.write_bytes(payload)
                output = self.directory / (name + "-output")
                with self.assertRaises(ValueError):
                    corpus.prepare_corpus(source, output, min_document_bytes=1, chunk_size=3)
                self.assertFalse(output.exists())

    def test_byte_cap_that_truncates_utf8_is_rejected_and_cleaned_up(self):
        first = "A complete earlier story."
        source = self.source("truncated-utf8.txt", [first, "é starts the next story."])
        cap = len(first.encode("utf-8")) + len(DELIMITER) + 1
        output = self.directory / "truncated-output"
        with self.assertRaises(ValueError):
            corpus.prepare_corpus(source, output, max_bytes=cap, min_document_bytes=1, chunk_size=2)
        self.assertFalse(output.exists())

    def test_invalid_validation_source_rolls_back_after_valid_training(self):
        source = self.source("rollback-train.txt", ["A complete training story."])
        validation = self.source("rollback-validation.txt", [b"An invalid\x00validation story."])
        output = self.directory / "rollback-output"
        with self.assertRaises(ValueError):
            corpus.prepare_corpus(source, output, validation_source=validation, min_document_bytes=1, chunk_size=3)
        self.assertFalse(output.exists())

    def test_cli_defaults_to_offline_and_requires_a_source_or_explicit_fetch(self):
        output = self.directory / "offline-default"
        with mock.patch.object(corpus.urllib.request, "urlopen", side_effect=AssertionError("unexpected network access")) as network, \
                mock.patch.object(corpus.urllib.request, "build_opener", side_effect=AssertionError("unexpected network opener")) as opener:
            with contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as failure:
                    corpus.main(["--output", str(output)])
            self.assertNotEqual(failure.exception.code, 0)
            network.assert_not_called()
            opener.assert_not_called()
        self.assertFalse(output.exists())

    def test_cli_imports_local_sources_offline_with_all_budget_options(self):
        source = self.source("cli-train.txt", ["A local training story."])
        validation = self.source("cli-validation.txt", ["An official local validation story."])
        output = self.directory / "cli-output"
        arguments = [
            "--source", str(source), "--validation-source", str(validation),
            "--source-split", "train", "--output", str(output),
            "--max-bytes", "10000", "--max-documents", "100",
            "--min-document-bytes", "1", "--chunk-size", "7",
        ]
        with mock.patch.object(corpus.urllib.request, "urlopen", side_effect=AssertionError("unexpected network access")) as network, \
                mock.patch.object(corpus.urllib.request, "build_opener", side_effect=AssertionError("unexpected network opener")) as opener:
            with contextlib.redirect_stdout(io.StringIO()):
                corpus.main(arguments)
            network.assert_not_called()
            opener.assert_not_called()
        self.assertEqual([row["text"] for row in self.records(output, "train")], ["A local training story."])
        rows = self.records(output, "validation") + self.records(output, "test")
        self.assertEqual([row["text"] for row in rows], ["An official local validation story."])
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        self.check_manifest_files(output, manifest)

    def test_explicit_fetch_cli_accepts_zero_offset_ranges_with_bounded_reads(self):
        payloads = [
            b"A fetched training story." + DELIMITER,
            "A fetched validation story includes café and 🐝.".encode("utf-8") + DELIMITER,
        ]
        responses = [FakeHTTPResponse(payload) for payload in payloads]
        opener = mock.Mock()
        opener.open.side_effect = responses
        output = self.directory / "fetch-cli"
        with mock.patch.object(corpus.urllib.request, "build_opener", return_value=opener) as build, \
                mock.patch.object(corpus.urllib.request, "urlopen", side_effect=AssertionError("unmocked network access")) as network:
            with contextlib.redirect_stdout(io.StringIO()):
                corpus.main([
                    "--fetch", "--output", str(output), "--max-bytes", "10000",
                    "--max-documents", "100", "--min-document-bytes", "1", "--chunk-size", "7",
                ])
            network.assert_not_called()
        self.assertEqual(build.call_count, 2)
        self.assertEqual(opener.open.call_count, 2)
        requests = [call.args[0] for call in opener.open.call_args_list]
        for request, key, budget, response in zip(requests, ("train", "validation"), (9000, 1000), responses):
            self.assertEqual(request.full_url, corpus.SOURCES[key]["url"])
            headers = {name.lower(): value for name, value in request.header_items()}
            self.assertEqual(headers["range"], "bytes=0-" + str(budget - 1))
            self.assertEqual(headers["accept-encoding"], "identity")
            self.assertTrue(response.read_sizes)
            self.assertTrue(all(0 < size <= 7 for size in response.read_sizes))
        self.assertEqual([row["text"] for row in self.records(output, "train")], ["A fetched training story."])
        held_out = self.records(output, "validation") + self.records(output, "test")
        self.assertEqual([row["text"] for row in held_out], ["A fetched validation story includes café and 🐝."])
        manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
        self.assertIs(type(manifest["training_ready"]), bool)
        for summary, payload in zip(manifest["source_summaries"], payloads):
            self.assertEqual(summary["input_kind"], "https")
            self.assertFalse(summary["full_file"])
            self.assertIsNone(summary["actual_full_sha256"])
            self.assertFalse(summary["upstream_identity_verified"])
            self.assertEqual(summary["actual_prefix_sha256"], sha256(payload))
        self.check_manifest_files(output, manifest)

    def test_fetch_rejects_invalid_ranges_encoding_truncation_and_full_hash(self):
        payload = b"A complete HTTP training story." + DELIMITER
        cases = {
            "wrong-offset": (206, {"Content-Range": "bytes 1-" + str(len(payload)) + "/1000000", "Content-Length": str(len(payload))}),
            "oversized-range": (206, {"Content-Range": "bytes 0-9000/1000000", "Content-Length": "9001"}),
            "compressed": (206, {"Content-Range": "bytes 0-" + str(len(payload) - 1) + "/1000000", "Content-Length": str(len(payload)), "Content-Encoding": "gzip"}),
            "truncated-length": (206, {"Content-Range": "bytes 0-" + str(len(payload) + 9) + "/1000000", "Content-Length": str(len(payload) + 10)}),
            "full-bad-sha": (200, {"Content-Length": str(len(payload))}),
        }
        for name, (status, headers) in cases.items():
            with self.subTest(name=name):
                output = self.directory / ("fetch-" + name)
                opener = mock.Mock()
                opener.open.return_value = FakeHTTPResponse(payload, status=status, headers=headers)
                with mock.patch.object(corpus.urllib.request, "build_opener", return_value=opener), \
                        mock.patch.object(corpus.urllib.request, "urlopen", side_effect=AssertionError("unmocked network access")) as network:
                    with self.assertRaises(ValueError):
                        corpus.prepare_fetch(output, max_bytes=10000, max_documents=100, min_document_bytes=1, chunk_size=7)
                    network.assert_not_called()
                self.assertTrue(opener.open.called)
                self.assertFalse(output.exists())

    def test_fetch_full_identity_requires_matching_expected_sha(self):
        payloads = [b"A complete training fixture." + DELIMITER, b"A complete validation fixture." + DELIMITER]
        responses = [FakeHTTPResponse(payload, status=200, headers={"Content-Length": str(len(payload))}) for payload in payloads]
        opener = mock.Mock()
        opener.open.side_effect = responses
        output = self.directory / "fetch-verified"
        with mock.patch.object(corpus.urllib.request, "build_opener", return_value=opener), \
                mock.patch.object(corpus.urllib.request, "urlopen", side_effect=AssertionError("unmocked network access")) as network, \
                mock.patch.dict(corpus.SOURCES["train"], {"expected_full_sha256": sha256(payloads[0])}), \
                mock.patch.dict(corpus.SOURCES["validation"], {"expected_full_sha256": sha256(payloads[1])}):
            manifest = corpus.prepare_fetch(output, max_bytes=10000, max_documents=100, min_document_bytes=1, chunk_size=5)
            network.assert_not_called()
        for summary, payload in zip(manifest["source_summaries"], payloads):
            self.assertTrue(summary["full_file"])
            self.assertTrue(summary["upstream_identity_verified"])
            self.assertEqual(summary["actual_full_sha256"], sha256(payload))
            self.assertEqual(summary["actual_prefix_sha256"], sha256(payload))
        self.check_manifest_files(output, manifest)


if __name__ == "__main__":
    unittest.main()
