# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see COPYING.

import unittest

import numpy as np

from contrib.b3ai.scratch.tokenizer import ByteTokenizer, VERSION


class ByteTokenizerTests(unittest.TestCase):
    def test_exact_utf8_roundtrip(self):
        tokenizer = ByteTokenizer()
        for text in ("", "B3 Hive\n", "हैलो B3", "你好", "🐝 café", "\x00\xff"):
            with self.subTest(text=text):
                self.assertEqual(tokenizer.encode(text), list(text.encode("utf-8")))
                self.assertEqual(tokenizer.decode(tokenizer.encode(text)), text)
        self.assertEqual(tokenizer.vocab_size, 256)
        self.assertEqual(tokenizer.version, VERSION)

    def test_generation_replaces_incomplete_utf8(self):
        tokenizer = ByteTokenizer()
        self.assertEqual(tokenizer.decode([0xE2, 0x82]), "�")
        self.assertEqual(tokenizer.decode([255]), "�")
        self.assertEqual(tokenizer.decode(np.array([66, 51], dtype=np.int64)), "B3")

    def test_invalid_ids_are_rejected(self):
        tokenizer = ByteTokenizer()
        for ids in ([256], [-1], [True], [1.0], ["1"], [None], [np.bool_(True)]):
            with self.subTest(ids=ids):
                with self.assertRaises(ValueError):
                    tokenizer.decode(ids)
        with self.assertRaises(TypeError):
            tokenizer.decode(None)
        with self.assertRaises(TypeError):
            tokenizer.encode(b"B3")
        with self.assertRaises(UnicodeEncodeError):
            tokenizer.encode("\ud800")


if __name__ == "__main__":
    unittest.main()
