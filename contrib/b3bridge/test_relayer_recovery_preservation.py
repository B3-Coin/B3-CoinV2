"""Offline regression: retries/reorgs may not erase prepared bridge identities.

Reproduced against frozen 269ebc48370b94be3ed26c180232c22236241257:
reorg/direct-reset/missing-input tests lost original bytes; both pending-plan
tests stopped at optional beacon capture. The missing-input method tests two
RPC errors. Six selected methods produced four assertion failures (including
subtests), two beacon errors, and one passing pristine-sync control. No live
wallet, provider or production database is used by this suite.
"""
import contextlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from test_b3_bridge_relayer import relayer, H32, ROOT_A, FakeRpc
import test_relayer_scheduling as scheduling


class RecoveryPreservationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.path = Path(self.tmp.name) / "recovery.sqlite3"
        self.state = relayer.State(self.path)
        self.addCleanup(self.state.close)
        self.state.bind({"bridge": "synthetic-recovery"}, 1)

    def rows(self):
        return [dict(row) for row in self.state.db.execute("SELECT * FROM jobs ORDER BY id")]

    def prepare_reorg(self):
        self.state.add_plan([
            {"kind": "execution-backfill", "payload_hex": "01", "target_block_hash": H32},
            {"kind": "mint", "payload_hex": "02", "deposit_id": 7,
             "target_block_hash": H32, "amount": "5", "address": "synthetic-address"},
            {"kind": "update", "payload_hex": "03"}])
        self.state.confirm_effect(1, 50, "aa" * 32)
        self.state.prepared(2, "cc" * 32, "deadbeef", 900, "2026-09-30", 1000, 1000)
        self.state.set_state(2, "broadcast")
        self.state.set_state(3, "superseded")

    def test_reorg_preserves_unknown_transaction_and_fee_reservation(self):
        self.prepare_reorg()
        before = self.rows()
        node = FakeRpc({"getfinalitystatus": {"finalized": {"height": 60}},
                        "getbridgeinfo": {"state_available": True},
                        "getblockhash": "bb" * 32})
        with patch.object(relayer, "log"):
            relayer.audit_confirmed(self.state, node, FakeRpc(), 1)
        after = self.rows()
        for field in ("txid", "raw_tx", "fee_atoms", "fee_day", "payload_id", "metadata"):
            self.assertEqual(after[1][field], before[1][field], field)
        self.assertEqual(after[1]["state"], "broadcast")
        self.assertEqual(after[2], before[2], "superseded work must not be resurrected")
        self.state.close()
        self.state = relayer.State(self.path)
        self.addCleanup(self.state.close)
        self.assertEqual(self.rows(), after)
        # The old transaction may still settle. Its possible fee is not free
        # budget for a newly prepared transaction on this same date.
        self.state.add_plan([{"kind": "mint", "payload_hex": "04"}])
        with self.assertRaisesRegex(relayer.RelayerError, "daily relayer fee budget"):
            self.state.prepared(4, "dd" * 32, "cafe", 200, "2026-09-30", 1000, 1000)

    def test_direct_reset_cannot_erase_signed_transaction(self):
        self.prepare_reorg()
        before = self.rows()
        with self.assertRaises(relayer.RelayerError):
            self.state.reset_transaction(2)
        self.assertEqual(self.rows(), before)

    def test_incomplete_signed_record_refuses_reopen_atomically(self):
        self.prepare_reorg()
        self.state.db.execute("UPDATE jobs SET raw_tx=NULL WHERE id=2")
        before = self.rows()
        with self.assertRaisesRegex(relayer.RelayerError, "incomplete prepared"):
            self.state.reopen_from(1)
        self.assertEqual(self.rows(), before)

    def test_inconsistent_pending_state_cannot_prepare_or_send(self):
        self.state.add_plan([{"kind": "bootstrap", "payload_hex": "01"}])
        self.state.prepared(1, "aa" * 32, "deadbeef", 900, "2026-09-30", 1000, 1000)
        self.state.set_state(1, "planned")
        node, wallet = FakeRpc(), FakeRpc()
        before = self.rows()
        with self.assertRaisesRegex(relayer.RelayerError, "planned bridge job has transaction history"):
            relayer.process_jobs(self.state, node, wallet, 1, False, 1000, 1000)
        with self.assertRaises(relayer.RelayerError):
            self.state.prepared(1, "bb" * 32, "cafe", 100, "2026-09-30", 1000, 1000)
        self.assertEqual(self.rows(), before)
        self.assertEqual((node.calls, wallet.calls), ([], []))

    def test_missing_fee_history_cannot_authorize_original_send(self):
        self.state.add_plan([{"kind": "bootstrap", "payload_hex": "01"}])
        self.state.prepared(1, "aa" * 32, "deadbeef", 900, "2026-09-30", 1000, 1000)
        self.state.db.execute("UPDATE jobs SET fee_atoms=NULL WHERE id=1")
        node, wallet = FakeRpc(), FakeRpc()
        before = self.rows()
        with self.assertRaisesRegex(relayer.RelayerError, "incomplete prepared"):
            relayer.process_jobs(self.state, node, wallet, 1, False, 1000, 1000)
        self.assertEqual(self.rows(), before)
        self.assertEqual((node.calls, wallet.calls), ([], []))

    def test_missing_inputs_waits_then_retransmits_exact_original(self):
        for message in ("txn-mempool-conflict", "bad-txns-inputs-missingorspent"):
            with self.subTest(message=message), tempfile.TemporaryDirectory() as directory:
                state = relayer.State(Path(directory) / "retry.sqlite3")
                self.addCleanup(state.close)
                state.bind({"bridge": "synthetic-retry"}, 1)
                state.add_plan([{"kind": "bootstrap", "payload_hex": "01"}])
                txid, raw = "aa" * 32, "deadbeef"
                state.prepared(1, txid, raw, 900, "2026-09-30", 1000, 1000)
                absent = relayer.RpcError({"code": -5, "message": "not found"})
                wallet = FakeRpc({"gettransaction": absent})
                sends = []
                def send(params):
                    sends.append(params[0])
                    if len(sends) == 1:
                        raise relayer.RpcError({"code": -26, "message": message})
                    return txid
                node = FakeRpc({"getbridgeinfo": {"active": True, "state_available": True,
                                                    "light_client_bootstrapped": False},
                                "getfinalitystatus": {"finalized": {"height": 60}},
                                "getmempoolentry": absent, "sendrawtransaction": send})
                with patch.object(relayer, "log"):
                    relayer.process_jobs(state, node, wallet, 1, False, 1000, 1000)
                    self.assertEqual(state.first()["txid"], txid)
                    self.assertEqual(state.first()["raw_tx"], raw)
                    relayer.process_jobs(state, node, wallet, 1, False, 1000, 1000)
                self.assertEqual(sends, [raw, raw])
                self.assertEqual(state.first()["fee_atoms"], 900)
                self.assertFalse(any(method in ("submitbridgecarrier", "claimbridgedeposit")
                                     for method, _ in wallet.calls))


class PendingPlanIndependenceTests(unittest.TestCase):
    def setUp(self):
        # Reuse setup, not inheritance: do not silently count another copy of
        # all scheduling tests as additional independent coverage.
        self.fixture = scheduling.SchedulingTests()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)

    @contextlib.contextmanager
    def unavailable_fresh_proof(self):
        with (patch.object(relayer, "capture_common", side_effect=relayer.RelayerError("beacon unavailable")) as capture,
              patch.object(relayer, "emit_plan") as emit,
              patch.object(relayer, "process_jobs") as process,
              patch.object(relayer, "scan_deposits") as scan,
              patch.object(relayer, "log")):
            yield capture, emit, process, scan

    def test_verified_pending_mint_does_not_need_a_new_beacon_proof(self):
        case = self.fixture
        case.state.add_plan([{"kind": "mint", "payload_hex": "03"}])
        with self.unavailable_fresh_proof() as (capture, emit, process, scan):
            case.cycle()
            capture.assert_not_called()
            emit.assert_not_called()
            process.assert_called_once()
            scan.assert_not_called()

    def test_prepared_sync_keeps_exact_bytes_without_optional_refresh(self):
        case = self.fixture
        case.state.add_plan([case.record])
        case.state.prepared(case.state.first()["id"], "aa" * 32, "deadbeef", 900,
                            "2026-09-30", 1000, 1000)
        with self.unavailable_fresh_proof() as (capture, emit, process, _):
            case.cycle()
            capture.assert_not_called()
            emit.assert_not_called()
            process.assert_called_once()
        self.assertEqual(case.state.first()["raw_tx"], "deadbeef")

    def test_pristine_sync_still_requires_new_independently_verified_plan(self):
        case = self.fixture
        case.state.add_plan([case.record])
        with self.unavailable_fresh_proof() as (capture, _, process, scan):
            with self.assertRaisesRegex(relayer.RelayerError, "beacon unavailable"):
                case.cycle()
            capture.assert_called_once()
            process.assert_not_called()
            scan.assert_not_called()

    def test_recovery_checks_runtime_pins_before_processing(self):
        case = self.fixture
        case.state.add_plan([{"kind": "mint", "payload_hex": "03"}])
        case.witness.replies["eth_getCode"] = "0x6004"
        with self.unavailable_fresh_proof() as (_, _, process, _):
            with self.assertRaisesRegex(relayer.RelayerError, "runtime"):
                case.cycle()
            process.assert_not_called()
        self.assertEqual(case.wallet.calls, [])

    def test_recovery_checks_identity_race_before_processing(self):
        case = self.fixture
        case.state.add_plan([{"kind": "mint", "payload_hex": "03"}])
        observations = iter([case.info, case.info, {**case.info, "registry_id": ROOT_A}])
        case.node.replies["getbridgeinfo"] = lambda _: next(observations)
        with self.unavailable_fresh_proof() as (_, _, process, _):
            with self.assertRaisesRegex(relayer.RelayerError, "pins changed before pending"):
                case.cycle()
            process.assert_not_called()
        self.assertEqual(case.wallet.calls, [])

    def test_recovery_checks_finalized_store_race_before_processing(self):
        case = self.fixture
        case.state.add_plan([{"kind": "mint", "payload_hex": "03"}])
        changed = {**case.snapshot, "b3_finalized": {"height": 140, "block_hash": H32}}
        observations = iter([case.snapshot, changed])
        case.node.replies["getbridgelightclientstore"] = lambda _: next(observations)
        with self.unavailable_fresh_proof() as (_, _, process, _):
            with self.assertRaisesRegex(relayer.RelayerError, "store changed before pending"):
                case.cycle()
            process.assert_not_called()
        self.assertEqual(case.wallet.calls, [])

    def test_dry_run_cannot_enter_recovery_submission_path(self):
        case = self.fixture
        case.state.add_plan([{"kind": "mint", "payload_hex": "03"}])
        case.args.dry_run = True
        with self.unavailable_fresh_proof() as (capture, _, process, _):
            with self.assertRaisesRegex(relayer.RelayerError, "beacon unavailable"):
                case.cycle()
            capture.assert_called_once()
            process.assert_not_called()
        self.assertEqual(case.wallet.calls, [])


if __name__ == "__main__":
    unittest.main()
