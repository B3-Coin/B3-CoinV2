# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.
"""Independent document-boundary and corpus-integrity checks, using local fiction."""

import hashlib
import json
from pathlib import Path
import tempfile
import unicodedata
import unittest

import numpy as np

from contrib.b3ai.language import batches, corpus


SPLITS = ("train", "validation", "test")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def normalized_hash(text):
    return sha256(" ".join(unicodedata.normalize("NFC", text).casefold().split()).encode("utf-8"))


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")) + "\n").encode("utf-8")


class BatchTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.prepared = self.directory / "prepared"
        self.train_texts = ["Café 🐝.", "A fox ran into a quiet wood.", "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ"]
        # Independently choose enough local held-out stories to populate both
        # hash partitions, rather than relying on a particular importer seed.
        held_out = []
        parity_counts = [0, 0]
        for index in range(100):
            story = "Held-out local tale number " + str(index) + "."
            parity = int(normalized_hash(story)[:8], 16) % 2
            if parity_counts[parity] < 4:
                held_out.append(story)
                parity_counts[parity] += 1
            if min(parity_counts) == 4:
                break
        self.assertEqual(parity_counts, [4, 4])
        training = self.write_source("train.txt", self.train_texts)
        validation = self.write_source("valid.txt", held_out)
        corpus.prepare_corpus(training, self.prepared, validation_source=validation,
            max_bytes=100000, max_documents=100, min_document_bytes=1, chunk_size=3)

    def write_source(self, name, texts):
        path = self.directory / name
        path.write_bytes(b"".join(text.encode("utf-8") + corpus.DELIMITER for text in texts))
        return path

    def manifest(self):
        return json.loads((self.prepared / "manifest.json").read_text(encoding="utf-8"))

    def write_manifest(self, value):
        (self.prepared / "manifest.json").write_bytes(json_bytes(value))

    def rows(self, split):
        return [json.loads(line) for line in (self.prepared / (split + ".jsonl")).read_text(encoding="utf-8").splitlines()]

    def replace_rows(self, split, rows):
        data = b"".join(json_bytes(row) for row in rows)
        (self.prepared / (split + ".jsonl")).write_bytes(data)
        manifest = self.manifest()
        manifest["splits"][split] = {
            "documents": len(rows), "bytes": len(data),
            "text_bytes": sum(len(row["text"].encode("utf-8")) for row in rows),
            "sha256": sha256(data),
        }
        self.write_manifest(manifest)

    def assert_bad_manifest(self, mutate):
        manifest = self.manifest()
        mutate(manifest)
        self.write_manifest(manifest)
        with self.assertRaises(ValueError):
            batches.Corpus(self.prepared)

    def test_loader_retains_byte_ids_unicode_eod_and_manifest_identity(self):
        loaded = batches.Corpus(self.prepared)
        self.assertEqual(batches.EOD_ID, 256)
        self.assertEqual(set(loaded.splits), set(SPLITS))
        self.assertEqual(loaded.manifest_hash, sha256((self.prepared / "manifest.json").read_bytes()))
        for split in SPLITS:
            records = self.rows(split)
            self.assertEqual(len(loaded.splits[split]), len(records))
            for tokens, row in zip(loaded.splits[split], records):
                self.assertEqual(tokens.dtype, np.dtype(np.uint16))
                expected = list(row["text"].encode("utf-8")) + [256]
                np.testing.assert_array_equal(tokens, expected)
                self.assertTrue(np.all(tokens[:-1] < 256))

    def test_short_documents_shift_bytes_supervise_eod_and_mask_padding(self):
        loaded = batches.Corpus(self.prepared)
        inputs, targets, masks = loaded.batch("train", np.random.default_rng(7), 8, 64)
        self.assertEqual(inputs.shape, (8, 64))
        self.assertEqual(targets.shape, inputs.shape)
        self.assertEqual(masks.shape, inputs.shape)
        self.assertEqual(inputs.dtype, np.dtype(np.int64))
        self.assertEqual(targets.dtype, np.dtype(np.int64))
        self.assertEqual(masks.dtype, np.dtype(np.float32))
        expected_documents = [list(text.encode("utf-8")) + [256] for text in self.train_texts]
        for input_row, target_row, mask in zip(inputs, targets, masks):
            count = int(mask.sum())
            np.testing.assert_array_equal(mask, [1] * count + [0] * (64 - count))
            matching = [doc for doc in expected_documents if input_row[:count].tolist() == doc[:-1]]
            self.assertEqual(len(matching), 1)
            document = matching[0]
            self.assertEqual(count, len(document) - 1)
            np.testing.assert_array_equal(target_row[:count], document[1:])
            self.assertEqual(target_row[count - 1], 256)
            self.assertNotIn(256, input_row[:count])
            np.testing.assert_array_equal(input_row[count:], 0)
            np.testing.assert_array_equal(target_row[count:], 0)
            # There is no supervised transition from a story's EOD to the
            # first byte of another story, including a zero padding token.
            self.assertEqual(np.count_nonzero(target_row[:count] == 256), 1)

    def test_random_windows_remain_inside_one_document_and_can_target_last_eod(self):
        loaded = batches.Corpus(self.prepared)
        context = 4
        valid = set()
        for text in self.train_texts:
            document = list(text.encode("utf-8")) + [256]
            for start in range(len(document) - context):
                window = document[start:start + context + 1]
                valid.add((tuple(window[:-1]), tuple(window[1:])))
        saw_eod = False
        for seed in range(40):
            inputs, targets, masks = loaded.batch("train", np.random.default_rng(seed), 8, context)
            np.testing.assert_array_equal(masks, 1)
            for input_row, target_row in zip(inputs, targets):
                self.assertIn((tuple(input_row), tuple(target_row)), valid)
                saw_eod |= target_row[-1] == 256
                self.assertNotIn(256, input_row)
        self.assertTrue(saw_eod, "The last possible window must include the terminal EOD target.")

    def test_rng_state_determines_document_and_window_choices(self):
        loaded = batches.Corpus(self.prepared)
        left = np.random.default_rng(2026)
        right = np.random.default_rng(2026)
        for _ in range(3):
            left_batch = loaded.batch("validation", left, 3, 7)
            right_batch = loaded.batch("validation", right, 3, 7)
            for a, b in zip(left_batch, right_batch):
                np.testing.assert_array_equal(a, b)
        self.assertEqual(left.bit_generator.state, right.bit_generator.state)

    def test_boundary_batch_sizes_and_contexts_are_accepted(self):
        loaded = batches.Corpus(self.prepared)
        for size, context in ((1, 1), (1, 4096), (16, 1024)):
            with self.subTest(size=size, context=context):
                tokens, targets, mask = loaded.batch("test", np.random.default_rng(0), size, context)
                self.assertEqual(tokens.shape, (size, context))
                self.assertEqual(targets.shape, tokens.shape)
                self.assertEqual(mask.shape, tokens.shape)
                self.assertGreater(mask.sum(), 0)

    def test_invalid_batch_shapes_nonintegers_nonfinite_values_and_split_are_rejected(self):
        loaded = batches.Corpus(self.prepared)
        invalid = [(0, 1), (17, 1), (1, 0), (1, 4097), (16, 1025),
            (True, 1), (1, False), (1.0, 1), (1, 1.0), (float("nan"), 1),
            (1, float("inf")), ("1", 1), (1, None)]
        for size, context in invalid:
            with self.subTest(size=size, context=context):
                with self.assertRaises(ValueError):
                    loaded.batch("train", np.random.default_rng(0), size, context)
        with self.assertRaises(ValueError):
            loaded.batch("elsewhere", np.random.default_rng(0), 1, 1)

    def test_split_file_tampering_is_rejected_before_tokenization(self):
        path = self.prepared / "train.jsonl"
        path.write_bytes(path.read_bytes().replace(b"fox", b"cat"))
        with self.assertRaises(ValueError):
            batches.Corpus(self.prepared)

    def test_hash_valid_rows_with_invalid_text_identity_are_rejected(self):
        records = self.rows("train")
        records[0]["id"] = "0" * 64
        self.replace_rows("train", records)
        with self.assertRaises(ValueError):
            batches.Corpus(self.prepared)

    def test_hash_valid_rows_with_invalid_normalized_identity_are_rejected(self):
        records = self.rows("train")
        records[0]["normalized_sha256"] = "0" * 64
        self.replace_rows("train", records)
        with self.assertRaises(ValueError):
            batches.Corpus(self.prepared)

    def test_hash_valid_wrong_row_split_is_rejected(self):
        records = self.rows("train")
        records[0]["split"] = "test"
        self.replace_rows("train", records)
        with self.assertRaises(ValueError):
            batches.Corpus(self.prepared)

    def test_hash_valid_wrong_source_split_is_rejected(self):
        records = self.rows("train")
        records[0]["source_key"] = "validation"
        self.replace_rows("train", records)
        with self.assertRaises(ValueError):
            batches.Corpus(self.prepared)

    def test_held_out_document_cannot_move_to_the_other_hash_partition(self):
        validation = self.rows("validation")
        test = self.rows("test")
        moved = validation.pop()
        moved["split"] = "test"
        test.append(moved)
        self.replace_rows("validation", validation)
        self.replace_rows("test", test)
        with self.assertRaises(ValueError):
            batches.Corpus(self.prepared)

    def test_duplicate_within_split_is_rejected_even_with_updated_split_digest(self):
        records = self.rows("train")
        records[1] = dict(records[0], source_document=records[1]["source_document"],
            source_byte_start=records[1]["source_byte_start"], source_byte_end=records[1]["source_byte_end"])
        self.replace_rows("train", records)
        with self.assertRaises(ValueError):
            batches.Corpus(self.prepared)

    def test_normalized_overlap_across_splits_is_rejected_even_with_new_exact_hash(self):
        text = self.train_texts[1].upper().replace(" ", "  ")
        identity = normalized_hash(text)
        split = "validation" if int(identity[:8], 16) % 2 == 0 else "test"
        records = self.rows(split)
        source = next(summary for summary in self.manifest()["source_summaries"]
            if summary["source_key"] == "validation")
        records[0].update(text=text, id=sha256(text.encode("utf-8")),
            raw_sha256=sha256(text.encode("utf-8")), normalized_sha256=identity,
            source_byte_start=0, source_byte_end=source["bytes_read"])
        self.replace_rows(split, records)
        with self.assertRaises(ValueError):
            batches.Corpus(self.prepared)

    def test_unknown_record_fields_are_rejected_after_split_rehash(self):
        records = self.rows("train")
        records[0]["unexpected"] = "not part of the corpus schema"
        self.replace_rows("train", records)
        with self.assertRaises(ValueError):
            batches.Corpus(self.prepared)

    def test_source_offsets_cannot_escape_admitted_prefix(self):
        records = self.rows("train")
        records[0]["source_byte_end"] = self.manifest()["source_summaries"][0]["bytes_read"] + 1
        self.replace_rows("train", records)
        with self.assertRaises(ValueError):
            batches.Corpus(self.prepared)

    def test_nul_and_oversize_text_are_rejected_after_valid_rehash(self):
        original = self.rows("train")
        for text in ("Text with\0NUL", "x" * 65537):
            with self.subTest(length=len(text)):
                records = [dict(row) for row in original]
                records[0].update(text=text, id=sha256(text.encode("utf-8")),
                    raw_sha256=sha256(text.encode("utf-8")), normalized_sha256=normalized_hash(text))
                self.replace_rows("train", records)
                with self.assertRaises(ValueError):
                    batches.Corpus(self.prepared)

    def test_schema_version_and_dataset_provenance_are_required(self):
        original = self.manifest()
        mutations = [
            lambda m: m.update(format="another-format"),
            lambda m: m.update(version=2),
            lambda m: m.update(version=True),
            lambda m: m.update(dataset="an-unrelated-dataset"),
            lambda m: m.update(revision="0" * 40),
            lambda m: m.update(license="MIT"),
            lambda m: m.update(synthetic=False),
            lambda m: m.update(individual_documents_reviewed=True),
            lambda m: m["splits"].pop("test"),
            lambda m: m["splits"].update(extra=dict(m["splits"]["train"])),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(case=index):
                self.write_manifest(original)
                self.assert_bad_manifest(mutate)

    def test_unbounded_or_nonfinite_manifest_numbers_are_rejected(self):
        original = self.manifest()
        mutations = [
            lambda m: m["splits"]["train"].update(documents=100001),
            lambda m: m["splits"]["train"].update(bytes=128 * 1024 * 1024 + 1),
            lambda m: m["splits"]["train"].update(text_bytes=float("inf")),
            lambda m: m["splits"]["train"].update(documents=True),
            lambda m: m["budgets"].update(source_bytes_total=float("nan")),
            lambda m: m["source_summaries"][0].update(bytes_read=float("inf")),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(case=index):
                self.write_manifest(original)
                self.assert_bad_manifest(mutate)

    def test_source_identity_and_whole_file_integrity_claims_are_consistent(self):
        original = self.manifest()
        mutations = [
            lambda m: m["source_summaries"][0].update(url="https://example.com/another-corpus.txt"),
            lambda m: m["source_summaries"][0].update(revision="0" * 40),
            lambda m: m["source_summaries"][0].update(filename="another-corpus.txt"),
            lambda m: m["source_summaries"][0].update(expected_full_sha256="0" * 64),
            lambda m: m["source_summaries"][0].update(input_kind="ftp"),
            lambda m: m["source_summaries"][0].update(upstream_identity_verified=True),
            lambda m: m["source_summaries"][0].update(actual_full_sha256="0" * 64),
            lambda m: m["source_summaries"][0].update(published_sha256_matches=True),
        ]
        for index, mutate in enumerate(mutations):
            with self.subTest(case=index):
                self.write_manifest(original)
                self.assert_bad_manifest(mutate)

    def test_missing_corpus_path_reports_value_error(self):
        with self.assertRaises(ValueError):
            batches.Corpus(self.directory / "does-not-exist")


if __name__ == "__main__":
    unittest.main()
