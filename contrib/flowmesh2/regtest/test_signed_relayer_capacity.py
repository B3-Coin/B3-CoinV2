#!/usr/bin/env python3
"""Durable gas-history capacity fences before indexing/publication."""
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from custody import CustodyError
from custody_policy import canonical
import signed_relayer as relayer


class JournalCapacityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.subject = relayer.DurableSignedRelayer.__new__(relayer.DurableSignedRelayer)
        self.subject.directory = Path(self.temp.name)
        self.subject.journal = self.subject.directory / 'transactions.jsonl'
        self.subject.journal.write_bytes(b'retained-existing-prefix\n')
        self.subject.tip, self.subject.sequence, self.subject.fenced = '00' * 32, 0, False
        self.indexed = []
        self.subject._index = lambda kind, value: self.indexed.append((kind, value))
        self.value = {'nonce': 0, 'exact-public-intent': 'fixture'}
        record = {'sequence': 1, 'previous': self.subject.tip, 'kind': 'signed', 'value': self.value}
        digest = hashlib.sha256(canonical(record).encode()).hexdigest()
        self.line = canonical({**record, 'hash': digest}).encode() + b'\n'
        self.prefix = self.subject.journal.read_bytes()

    def tearDown(self):
        self.temp.cleanup()

    def test_overflow_refuses_before_memory_index_and_preserves_every_byte(self):
        limit = len(self.prefix) + len(self.line) - 1
        with patch.object(relayer, 'MAX_JOURNAL_BYTES', limit):
            with self.assertRaisesRegex(CustodyError, 'journal capacity exhausted; history retained'):
                self.subject._append('signed', self.value)
            self.assertEqual(self.subject.capacity(), {'journal_bytes': len(self.prefix),
                'journal_capacity_bytes': limit, 'journal_remaining_bytes': limit - len(self.prefix)})
        self.assertTrue(self.subject.fenced)
        self.assertEqual(self.indexed, [])
        self.assertEqual(self.subject.sequence, 0)
        self.assertEqual(self.subject.journal.read_bytes(), self.prefix)

    def test_exact_capacity_appends_durable_full_record_then_fences_next(self):
        limit = len(self.prefix) + len(self.line)
        with patch.object(relayer, 'MAX_JOURNAL_BYTES', limit):
            self.subject._append('signed', self.value)
            self.assertEqual(self.subject.journal.read_bytes(), self.prefix + self.line)
            self.assertEqual(self.subject.sequence, 1)
            self.assertEqual(self.subject.capacity()['journal_remaining_bytes'], 0)
            with self.assertRaisesRegex(CustodyError, 'journal capacity exhausted'):
                self.subject._append('signed', {'nonce': 1})
        self.assertEqual(self.indexed, [('signed', self.value)])
        self.assertEqual(self.subject.journal.read_bytes(), self.prefix + self.line)

    def test_absent_journal_reports_full_capacity_without_creating_history(self):
        self.subject.journal = self.subject.directory / 'not-created.jsonl'
        self.assertEqual(self.subject.capacity(), {'journal_bytes': 0,
            'journal_capacity_bytes': relayer.MAX_JOURNAL_BYTES, 'journal_remaining_bytes': relayer.MAX_JOURNAL_BYTES})
        self.assertFalse(self.subject.journal.exists())


if __name__ == '__main__':
    unittest.main()
