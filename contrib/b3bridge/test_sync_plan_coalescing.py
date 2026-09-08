#!/usr/bin/env python3
"""Offline State-only queue tests: no RPC, proof fetching, wallet or signing."""
import copy
import hashlib
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from b3_bridge_relayer import RelayerError, State


def record(value, kind="update"):
    return {"kind": kind, "payload_hex": f"{value:04x}",
            "anchor_block_number": value * 100, "finalized_beacon_slot": value * 32}


class SyncPlanCoalescingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = Path(self.directory.name) / "state.sqlite3"
        self.state = State(self.path)

    def tearDown(self):
        self.state.close()
        self.directory.cleanup()

    def rows(self):
        return [dict(row) for row in self.state.db.execute("SELECT * FROM jobs ORDER BY id")]

    def meta(self):
        return list(self.state.db.execute("SELECT key,value FROM meta ORDER BY key"))

    def payloads(self):
        return [row["payload"] for row in self.state.pending()]

    def update(self, row_id, **fields):
        for key, value in fields.items():
            self.state.db.execute(f"UPDATE jobs SET {key}=? WHERE id=?", (value, row_id))

    def test_old_plans_retained_as_audit_rows_without_mutating_payloads(self):
        self.state.add_plan([record(1), record(2), record(3)])
        before = self.rows()
        result = self.state.coalesce_verified_sync_plan([record(2), record(4)])
        self.assertEqual(result, {"superseded": 2, "reactivated": 0, "fallback": None})
        self.assertEqual(self.payloads(), ["0002", "0004"])
        after = self.rows()
        self.assertEqual(len(after), 4)  # Nothing deleted or renumbered.
        for old, new in zip(before, after):
            expected = dict(old)
            if old["id"] in (1, 3):
                expected["state"] = "superseded"
            self.assertEqual(new, expected)

    def test_required_transition_order_is_exact_even_when_old_ids_are_reused(self):
        self.state.add_plan([record(20), record(5)])
        self.state.coalesce_verified_sync_plan([record(5), record(10), record(20)])
        self.assertEqual(self.payloads(), ["0005", "000a", "0014"])
        self.assertEqual(self.state.first()["payload"], "0005")
        self.state.close()
        self.state = State(self.path)
        self.assertEqual(self.payloads(), ["0005", "000a", "0014"])

    def test_fresh_verified_plan_can_reactivate_only_unused_superseded_rows(self):
        self.state.add_plan([record(1), record(2)])
        self.state.coalesce_verified_sync_plan([record(2)])
        original = self.rows()[0]
        self.assertEqual(original["state"], "superseded")
        result = self.state.coalesce_verified_sync_plan([record(1), record(3)])
        self.assertEqual(result["reactivated"], 1)
        self.assertEqual(self.rows()[0], {**original, "state": "planned"})
        self.assertEqual(self.payloads(), ["0001", "0003"])

    def test_prepared_broadcast_and_every_history_field_are_untouched(self):
        for fields in ({"state": "prepared"}, {"state": "broadcast"},
                       {"txid": "signed-tx"}, {"raw_tx": "signed-bytes"},
                       {"fee_atoms": 0}, {"fee_day": "2026-09-08"},
                       {"effect_height": 0}, {"effect_block": "observed-block"}):
            with self.subTest(fields=fields):
                self.state.close()
                self.state = State(Path(self.directory.name) / (str(len(fields)) + repr(fields) + ".sqlite3"))
                self.state.add_plan([record(1), record(2)])
                self.update(1, **fields)
                before = self.rows()
                result = self.state.coalesce_verified_sync_plan([record(3)])
                self.assertIsNotNone(result["fallback"])
                self.assertEqual(self.rows()[:2], before)
                self.assertEqual(len(self.rows()), 3)  # Ordinary append fallback.
                self.assertEqual(self.state.first()["id"], 1)

    def test_used_superseded_record_is_not_reactivated(self):
        self.state.add_plan([record(1)])
        self.update(1, state="superseded", fee_atoms=10, fee_day="2026-09-08")
        original = self.rows()[0]
        result = self.state.coalesce_verified_sync_plan([record(1), record(2)])
        self.assertIsNotNone(result["fallback"])
        self.assertEqual(self.rows()[0], original)
        self.assertEqual(self.payloads(), ["0002"])

    def test_confirmed_records_preserved_and_not_reactivated(self):
        self.state.add_plan([record(1)])
        self.update(1, state="confirmed", txid="tx", effect_height=100, effect_block="hash", fee_atoms=5)
        original = self.rows()[0]
        self.state.coalesce_verified_sync_plan([record(2)])
        self.assertEqual(self.rows()[0], original)
        result = self.state.coalesce_verified_sync_plan([record(1), record(3)])
        self.assertIsNotNone(result["fallback"])
        self.assertEqual(self.rows()[0], original)

    def test_pending_mint_or_backfill_preserves_previous_sync_dependencies(self):
        for kind in ("mint", "execution-backfill"):
            with self.subTest(kind=kind):
                self.state.close()
                self.state = State(Path(self.directory.name) / (kind + ".sqlite3"))
                self.state.add_plan([record(1), record(2, kind)])
                before = self.rows()
                result = self.state.coalesce_verified_sync_plan([record(3)])
                self.assertEqual(result["fallback"], "pending deposit/backfill dependency")
                self.assertEqual(self.rows()[:2], before)
                self.assertEqual(self.rows()[0]["state"], "planned")

    def test_unplanned_deposit_preserves_previous_sync_dependencies(self):
        self.state.add_plan([record(1)])
        self.state.db.execute("""INSERT INTO deposits(deposit_id,block_number,block_hash,tx_hash,
            tx_index,receipt_log_index,amount,recipient) VALUES('1',1,'block','tx',0,0,'3','recipient')""")
        before = self.rows()
        result = self.state.coalesce_verified_sync_plan([record(2)])
        self.assertEqual(result["fallback"], "unplanned deposit dependency")
        self.assertEqual(self.rows()[0], before[0])

    def test_conflicting_metadata_rolls_back_insertions_and_existing_state(self):
        self.state.add_plan([record(1), record(2)])
        self.state.coalesce_verified_sync_plan([record(2), record(1)])
        before, before_meta = self.rows(), self.meta()
        conflicting = copy.deepcopy(record(1))
        conflicting["anchor_block_number"] += 1
        with self.assertRaisesRegex(RelayerError, "conflicting RPC metadata"):
            self.state.coalesce_verified_sync_plan([record(3), conflicting])
        self.assertEqual(self.rows(), before)
        self.assertEqual(self.meta(), before_meta)

    def test_failure_after_supersession_rolls_back_everything(self):
        self.state.add_plan([record(1), record(2)])
        self.state.db.execute("""CREATE TRIGGER deny_plan_order BEFORE INSERT ON meta
            WHEN NEW.key='verified_sync_order' BEGIN SELECT RAISE(ABORT,'test fault'); END""")
        before = self.rows()
        with self.assertRaises(sqlite3.IntegrityError):
            self.state.coalesce_verified_sync_plan([record(3)])
        self.assertEqual(self.rows(), before)
        self.assertEqual(self.meta(), [])

    def test_non_sync_or_duplicate_payload_plan_rejected_without_writes(self):
        self.state.add_plan([record(1)])
        for records in ([record(2, "mint")], [record(2), record(2)]):
            before = self.rows()
            with self.assertRaises(RelayerError):
                self.state.coalesce_verified_sync_plan(records)
            self.assertEqual(self.rows(), before)

    def test_empty_verified_plan_retires_only_unused_sync_rows(self):
        self.state.add_plan([record(1), record(2)])
        result = self.state.coalesce_verified_sync_plan([])
        self.assertEqual(result["superseded"], 2)
        self.assertEqual(len(self.rows()), 2)
        self.assertIsNone(self.state.first())

    def test_corrupt_persisted_order_fails_closed(self):
        self.state.add_plan([record(1)])
        self.state.db.execute("INSERT INTO meta VALUES('verified_sync_order','[true]')")
        with self.assertRaisesRegex(RelayerError, "invalid durable"):
            self.state.first()

    def test_reorg_reopen_discards_order_bound_to_previous_snapshot(self):
        self.state.add_plan([record(2), record(1)])
        self.state.coalesce_verified_sync_plan([record(1), record(2)])
        self.state.reopen_from(1)
        self.assertIsNone(self.state.db.execute(
            "SELECT value FROM meta WHERE key='verified_sync_order'").fetchone())
        self.assertEqual(self.payloads(), ["0002", "0001"])


if __name__ == "__main__":
    unittest.main()
