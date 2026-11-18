import base64
import copy
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import feed_history as module
import history_store
from feed_history import FeedCursor, parse_feed_frame
from history_store import HistoryError, HistoryStore, canonical, default_policy


def message(sequence=0, delayed=0):
    return {"sequenceNumber": sequence, "message": {"message": {
        "header": {"kind": 3, "sender": "0xa4b000000000000000000073657175656e636572",
                   "blockNumber": 24000000, "timestamp": 1800000000,
                   "requestId": None, "baseFeeL1": None},
        "l2Msg": base64.b64encode(b"synthetic-not-executed-transaction").decode()},
        "delayedMessagesRead": delayed}, "signature": None}


def frame(*items, confirmed=None):
    value = {"version": 1, "messages": list(items)}
    if confirmed is not None:
        value["confirmedSequenceNumberMessage"] = {"sequenceNumber": confirmed}
    return canonical(value)


class FeedParserTests(unittest.TestCase):
    def test_documented_v1_shape_is_availability_only(self):
        first, second = message(7, 9), message(8, 10)
        first["blockMetadata"] = None
        value = parse_feed_frame(frame(first, second, confirmed=99999))
        self.assertEqual([row["sequenceNumber"] for row in value["messages"]], [7, 8])
        self.assertEqual(value["confirmedSequenceNumberMessage"]["sequenceNumber"], 99999)

    def test_pinned_nitro_3_12_1_wire_signaturev2_blockhash_and_batch_stats(self):
        value = message(7, 9)
        value["signatureV2"] = value.pop("signature")
        value["blockHash"] = "0x" + "ab" * 32
        value["blockMetadata"] = base64.b64encode(b"metadata").decode()
        value["message"]["message"]["batchDataTokens"] = {"length": 10, "nonzeros": 4}
        self.assertEqual(parse_feed_frame(frame(value))["messages"][0], value)
        for change in (lambda row: row.update(signature=None),
                       lambda row: row.update(blockHash="0x01"),
                       lambda row: row["message"]["message"].update(batchDataTokens={"length": 2, "nonzeros": 3})):
            hostile = copy.deepcopy(value)
            change(hostile)
            with self.assertRaises(HistoryError):
                parse_feed_frame(frame(hostile))

    def test_versions_unknown_fields_gap_reorder_and_bool_integer_refuse(self):
        cases = [{"version": 2, "messages": [message()]},
                 {"version": True, "messages": [message()]},
                 {"version": 1, "messages": [message()], "verified": True},
                 {"version": 1, "messages": [message(1), message(3)]},
                 {"version": 1, "messages": [message(2), message(1)]},
                 {"version": 1, "messages": []}]
        boolean = message()
        boolean["sequenceNumber"] = True
        cases.append({"version": 1, "messages": [boolean]})
        for value in cases:
            with self.subTest(value=value), self.assertRaises(HistoryError):
                parse_feed_frame(canonical(value))

    def test_invalid_base64_overflow_header_substitution_refuse(self):
        for mutate in (
            lambda m: m.update(signature="!invalid"),
            lambda m: m.update(signature=base64.b64encode(b"x" * 66).decode()),
            lambda m: m.update(sequenceNumber=module.MAX_UINT64),
            lambda m: m["message"].update(delayedMessagesRead=-1),
            lambda m: m["message"]["message"].update(l2Msg="YR=="),
            lambda m: m["message"]["message"]["header"].update(sender="0xabc"),
            lambda m: m["message"]["message"]["header"].update(blockNumber=1 << 64),
            lambda m: m["message"]["message"]["header"].update(baseFeeL1=1 << 256),
            lambda m: m["message"]["message"]["header"].update(requestId="0x01"),
            lambda m: m["message"]["message"]["header"].update(trusted=True),
        ):
            value = message()
            mutate(value)
            with self.subTest(value=value), self.assertRaises(HistoryError):
                parse_feed_frame(frame(value))

    def test_limits_duplicate_keys_deep_nesting_and_huge_integer_are_stable_errors(self):
        cases = [b"x" * (module.MAX_FRAME + 1), frame(*[message(i) for i in range(257)]),
                 b'{"version":1,"version":1,"messages":[]}',
                 b"[" * 2000 + b"0" + b"]" * 2000,
                 b'{"version":1,"n":' + b"9" * 5000 + b"}",
                 b'{"version":1,"n":NaN}', b'\xff']
        for raw in cases:
            with self.subTest(length=len(raw)), self.assertRaises(HistoryError):
                parse_feed_frame(raw)


@unittest.skipUnless(history_store.fcntl is not None, "Actual POSIX durable feed spool requires Linux")
class FeedSpoolTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.path = Path(self.temporary.name) / "history"
        self.store = HistoryStore(self.path, default_policy(), create=True)

    def tearDown(self):
        self.store.close()
        self.temporary.cleanup()

    def test_durable_soft_sequence_exact_retry_and_notice_cannot_advance(self):
        cursor = FeedCursor(10)
        raw = frame(message(10, 5), confirmed=999999)
        reply = cursor.ingest(self.store, raw, peer_id="relay-one")
        self.assertEqual(cursor.next_sequence, 11)
        self.assertTrue(reply["soft_only"])
        self.assertFalse(reply["arbitrum_authority"])
        self.assertFalse(reply["native_execution_verified"])
        self.assertEqual(self.store.get(reply["object"]["sha256"]), raw)
        cursor.ingest(self.store, raw, peer_id="relay-two")
        self.assertEqual(self.store.sequence, 1)
        cursor.ingest(self.store, frame(confirmed=1000000), peer_id="relay-one")
        self.assertEqual(cursor.next_sequence, 11)
        self.assertFalse(self.store.stats()["arbitrum_authority"])

    def test_conflict_gap_and_delayed_regression_fence_without_storing(self):
        cases = [message(10, 5), message(12, 6), message(11, 4)]
        cases[0]["message"]["message"]["l2Msg"] = base64.b64encode(b"equivocation").decode()
        for index, item in enumerate(cases):
            with self.subTest(index=index):
                cursor = FeedCursor(10)
                cursor.ingest(self.store, frame(message(10, 5)), peer_id="relay-one")
                before = self.store.stats()
                with self.assertRaises(HistoryError):
                    cursor.ingest(self.store, frame(item), peer_id="relay-two")
                self.assertTrue(cursor.fenced)
                self.assertEqual(cursor.next_sequence, 11)
                self.assertEqual(self.store.stats(), before)
                with self.assertRaises(HistoryError):
                    cursor.ingest(self.store, frame(message(11, 5)), peer_id="relay-one")

    def test_store_failure_never_advances_feed_cursor(self):
        cursor = FeedCursor(5)
        with patch.object(self.store, "put", side_effect=OSError("synthetic disk refusal")):
            with self.assertRaises(OSError):
                cursor.ingest(self.store, frame(message(5, 2)), peer_id="relay-one")
        self.assertEqual(cursor.next_sequence, 5)
        self.assertIsNone(cursor.last)
        self.assertIsNone(cursor.previous_delayed)
        self.assertEqual(self.store.sequence, 0)

    def test_hostile_parser_inputs_cannot_advance_cursor_or_store(self):
        cursor = FeedCursor(1)
        before = self.store.stats()
        for raw in (b"[" * 3000 + b"0" + b"]" * 3000,
                    b'{"version":' + b"9" * 5000 + b"}",
                    b'{"version":1,"version":2}', frame(message(1 << 64))):
            with self.assertRaises(HistoryError):
                cursor.ingest(self.store, raw, peer_id="relay-one")
            self.assertEqual(cursor.next_sequence, 1)
            self.assertIsNone(cursor.last)
            self.assertEqual(self.store.stats(), before)


if __name__ == "__main__":
    unittest.main()
