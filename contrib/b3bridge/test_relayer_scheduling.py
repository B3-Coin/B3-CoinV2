"""Offline regressions for live sync/deposit scheduling; no network or wallet."""
import contextlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import test_b3_bridge_relayer as fixtures
from test_b3_bridge_relayer import H32, ROOT_A, store, relayer


class SchedulingTests(unittest.TestCase):
    def setUp(self):
        fixtures.FinalitySequencingTests.setUp(self)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.args.work_root = self.tmp.name
        self.args.dry_run = False
        self.args.max_fee_atoms = 20000000
        self.args.daily_fee_budget_atoms = 100000000
        self.state = relayer.State(str(Path(self.tmp.name) / "state.sqlite3"))
        self.addCleanup(self.state.close)
        self.finalized = {"height": 130, "hash": ROOT_A}
        self.record = store("update", "02", 8224)
        self.plan = {"store": store("store", "01", 8192),
                     "updates": [self.record], "backfills": []}
        self.frozen_plan = {**self.plan, "updates": []}
        self.common = ({}, None, [], {}, 100, H32, self.snapshot)

    def cycle(self):
        relayer.run_once(self.args, self.state, self.primary, [self.witness],
                         self.node, self.wallet, 63)

    @contextlib.contextmanager
    def mocked_cycle(self):
        with (patch.object(relayer, "capture_common", return_value=self.common),
              patch.object(relayer, "emit_plan", side_effect=[self.plan, self.frozen_plan]) as emit,
              patch.object(relayer, "process_jobs") as process,
              patch.object(relayer, "scan_deposits") as scan,
              patch.object(relayer, "log")):
            yield emit, process, scan

    def test_near_head_scans_exact_finalized_store_before_optional_refresh(self):
        self.state.add_plan([self.record])
        with self.mocked_cycle() as (emit, process, scan):
            self.cycle()
            self.assertEqual(emit.call_count, 2)
            process.assert_not_called()
            scan.assert_called_once()
            frozen = scan.call_args.args[-1]
            self.assertEqual(frozen[2:4], ([], None))
            self.assertEqual(frozen[4:6], (100, H32))
            self.assertEqual(frozen[6], self.snapshot)
        self.assertIsNone(self.state.first())
        self.assertEqual(self.state.db.execute("SELECT state FROM jobs").fetchone()[0],
                         "superseded")
        self.assertEqual(self.wallet.calls, [])

    def test_pending_mint_drains_without_adding_optional_updates(self):
        self.state.add_plan([{"kind": "mint", "payload_hex": "03"}])
        with self.mocked_cycle() as (emit, process, scan):
            self.cycle()
            self.assertEqual(emit.call_count, 1)
            process.assert_called_once()
            scan.assert_not_called()
        self.assertEqual([row["kind"] for row in self.state.pending()], ["mint"])

    def test_far_behind_prioritizes_covered_history_without_optional_update(self):
        self.record["anchor_block_number"] = 229
        self.state.add_plan([self.record])
        with self.mocked_cycle() as (emit, process, scan):
            self.cycle()
            self.assertEqual(emit.call_count, 2)
            process.assert_not_called()
            scan.assert_called_once()
            self.assertEqual(scan.call_args.args[-1][4:6], (100, H32))
        self.assertIsNone(self.state.first())
        self.assertEqual(self.state.db.execute("SELECT state FROM jobs").fetchone()[0],
                         "superseded")
        self.assertEqual(self.wallet.calls, [])

    def test_covered_history_wins_at_equal_near_and_large_refresh_gaps(self):
        for gap in (0, 128, 129, 1000, 20000):
            with self.subTest(gap=gap):
                self.record["anchor_block_number"] = 100 + gap
                with self.mocked_cycle() as (emit, process, scan):
                    self.cycle()
                    self.assertEqual(emit.call_count, 2)
                    process.assert_not_called()
                    scan.assert_called_once()
                    self.assertEqual(scan.call_args.args[-1][4:6], (100, H32))
                self.assertIsNone(self.state.first())
        self.assertEqual(self.wallet.calls, [])

    def test_large_refresh_gap_dry_run_still_only_previews_update(self):
        self.args.dry_run = True
        self.record["anchor_block_number"] = 1100
        with self.mocked_cycle() as (emit, process, scan):
            self.cycle()
            self.assertEqual(emit.call_count, 1)
            process.assert_called_once()
            self.assertTrue(process.call_args.args[4])
            scan.assert_not_called()
        self.assertEqual(self.state.first()["kind"], "update")
        self.assertEqual(self.state.first()["state"], "planned")
        self.assertIsNone(self.state.first()["txid"])
        self.assertEqual(self.wallet.calls, [])

    def test_regressing_proven_height_never_scans_or_submits(self):
        self.record["anchor_block_number"] = 99
        with self.mocked_cycle() as (_, process, scan):
            with self.assertRaisesRegex(relayer.RelayerError, "execution advance exceeds"):
                self.cycle()
            process.assert_not_called()
            scan.assert_not_called()
        self.assertIsNone(self.state.first())
        self.assertEqual(self.state.cursor(), 50)
        self.assertEqual(self.wallet.calls, [])

    def test_history_scan_uses_retained_anchor_not_current_tip_distance(self):
        identity = relayer.bridge_identity(self.info, 1, self.args.trusted_root)
        self.state.bind(identity, 50)
        historical = relayer.validate_execution_anchor(
            {**fixtures.retained_anchor(100), "found": True, "target_block": 50}, 50)
        frozen = (*self.common[:4], 30100, H32, self.snapshot)
        self.args.scan_chunk = 32
        self.args.max_ancestry = 20000
        with (patch.object(relayer, "fetch_execution_anchor", return_value=historical) as anchor,
              patch.object(relayer, "corroborate_execution_anchor") as corroborate,
              patch.object(relayer, "scan_finalized") as scan,
              patch.object(relayer, "process_jobs") as process):
            relayer.scan_deposits(self.args, self.state, self.primary, [self.witness],
                                 self.node, self.wallet, 63, self.info, identity, frozen)
            anchor.assert_called_once_with(self.node, 50)
            corroborate.assert_called_once_with(historical, [self.primary, self.witness])
            self.assertEqual(scan.call_args.args[3:5], (100, H32))
            self.assertEqual(scan.call_args.args[6], 20000)
            self.assertTrue(callable(scan.call_args.kwargs["before_commit"]))
            process.assert_called_once()
        self.assertEqual(self.state.cursor(), 50) # Mocked authentication never advances history.

    def test_already_scanned_tip_queues_refresh(self):
        self.state.bind(relayer.bridge_identity(self.info, 1, self.args.trusted_root), 50)
        self.state.db.execute("UPDATE meta SET value='101' WHERE key='next_block'")
        self.state.db.commit()
        with self.mocked_cycle() as (_, process, scan):
            self.cycle()
            process.assert_called_once()
            scan.assert_not_called()
        self.assertIsNotNone(self.state.first())

    def test_frozen_plan_cannot_smuggle_an_update(self):
        self.frozen_plan["updates"] = [self.record]
        with self.mocked_cycle() as (_, process, scan):
            with self.assertRaisesRegex(relayer.RelayerError, "scan plan changed"):
                self.cycle()
            process.assert_not_called()
            scan.assert_not_called()

    def test_uncovered_legacy_deposit_does_not_block_needed_refresh(self):
        self.state.bind(relayer.bridge_identity(self.info, 1, self.args.trusted_root), 50)
        self.state.db.execute("UPDATE meta SET value='101' WHERE key='next_block'")
        self.state.db.commit()
        with (patch.object(self.state, "unplanned", return_value=[{"block_number": 229}]),
              self.mocked_cycle() as (_, process, scan)):
            self.cycle()
            process.assert_called_once()
            scan.assert_not_called()

    def test_scan_skips_uncovered_legacy_deposit_without_anchor_lookup(self):
        self.state.bind(relayer.bridge_identity(self.info, 1, self.args.trusted_root), 50)
        self.state.db.execute("UPDATE meta SET value='101' WHERE key='next_block'")
        self.state.db.commit()
        with (patch.object(self.state, "unplanned", return_value=[{"block_number": 229}]),
              patch.object(relayer, "fetch_execution_anchor") as anchor,
              patch.object(relayer, "process_jobs") as process):
            relayer.scan_deposits(self.args, self.state, self.primary, [self.witness],
                                 self.node, self.wallet, 63, self.info,
                                 {"token": fixtures.TOKEN}, self.common)
            anchor.assert_not_called()
            process.assert_called_once()

    def test_frozen_snapshot_race_preserves_old_queue(self):
        self.state.add_plan([self.record])
        changed = {**self.snapshot, "b3_finalized": {"height": 140, "block_hash": H32}}
        snapshots = iter([self.snapshot, self.snapshot, changed])
        self.node.replies["getbridgelightclientstore"] = lambda _: next(snapshots)
        with self.mocked_cycle() as (_, process, scan):
            with self.assertRaisesRegex(relayer.RelayerError, "before finalized-only scanning"):
                self.cycle()
            process.assert_not_called()
            scan.assert_not_called()
        self.assertEqual(self.state.first()["state"], "planned")

    def test_signed_job_is_preserved_and_processed_before_scan(self):
        self.record["anchor_block_number"] = 1100 # Freshness cannot replace a submitted job.
        self.state.add_plan([self.record])
        self.state.db.execute("UPDATE jobs SET state='broadcast',txid=?,raw_tx='1234'", (H32,))
        self.state.db.commit()
        with self.mocked_cycle() as (_, process, scan):
            self.cycle()
            process.assert_called_once()
            scan.assert_not_called()
        self.assertEqual(self.state.first()["state"], "broadcast")
        self.assertEqual(self.state.first()["raw_tx"], "1234")

    def test_frozen_common_removes_stale_optional_proof(self):
        directory = Path(self.tmp.name) / "proof"
        relayer.write_common(directory, self.common)
        self.assertTrue((directory / "finality_update.json").exists())
        frozen = ({}, None, [], None, 100, H32, self.snapshot)
        relayer.write_common(directory, frozen)
        self.assertFalse((directory / "finality_update.json").exists())
        self.assertEqual(json.loads((directory / "updates.json").read_text()), [])
        self.assertEqual(json.loads((directory / "store.json").read_text()), self.snapshot)


if __name__ == "__main__":
    unittest.main()
