import hashlib
import os
from pathlib import Path
import stat
import struct
import tempfile
import threading
import unittest
from unittest.mock import patch

import history_store as module
from history_store import HistoryError, HistoryStore, canonical, default_policy, digest


PROVENANCE = {"transport": "synthetic-adversarial-fixture/1", "peer_id": "fixture-one"}


@unittest.skipUnless(module.fcntl is not None, "Actual POSIX permission/lock/fsync tests require Linux")
class HistoryStoreTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.parent = Path(self.temporary.name)
        self.directory = self.parent / "history"
        self.policy = default_policy()

    def tearDown(self):
        self.temporary.cleanup()

    def store(self):
        return HistoryStore(self.directory, self.policy, create=True)

    def test_durable_raw_bytes_reopen_and_exact_idempotence(self):
        with self.store() as store:
            first = store.put("l2_header_rlp", b"raw-header", PROVENANCE)
            receipt = store.put("l2_receipt", b"raw-receipt", PROVENANCE)
            self.assertEqual(store.put("l2_receipt", b"raw-receipt", PROVENANCE), receipt)
            before = store.stats()
            self.assertEqual(before["sequence"], 2)
            self.assertFalse(before["arbitrum_authority"])
            self.assertFalse(before["mainnet_enabled"])
            self.assertEqual(store.get(first["sha256"]), b"raw-header")
        with HistoryStore(self.directory, self.policy) as store:
            self.assertEqual(store.stats(), before)
            self.assertEqual(store.get(receipt["sha256"]), b"raw-receipt")
        for path in self.directory.rglob("*"):
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o700 if path.is_dir() else 0o600)

    def test_policy_pins_bounds_and_no_activation_flags(self):
        for key, value in (("mainnet_enabled", True), ("writes_enabled", 0),
                           ("source_chain_id", True), ("source_chain_id", 31337),
                           ("source_genesis_hash", "0x" + "ab" * 32),
                           ("max_object_bytes", 16 * 1024 * 1024 + 1),
                           ("max_total_bytes", -1), ("max_objects", 4097)):
            with self.subTest(key=key, value=value):
                policy = dict(self.policy, **{key: value})
                with self.assertRaises(HistoryError):
                    HistoryStore(self.directory, policy, create=True)
                self.assertFalse(self.directory.exists())
        with self.assertRaises(HistoryError):
            HistoryStore(self.directory, dict(self.policy, verified=True), create=True)

    def test_existing_directory_and_policy_are_never_reset(self):
        with self.store() as store:
            store.put("mpt_node", b"node", PROVENANCE)
        before = (self.directory / "history.log").read_bytes()
        with self.assertRaises(FileExistsError):
            self.store()
        changed = dict(self.policy, max_objects=8)
        with self.assertRaises(HistoryError):
            HistoryStore(self.directory, changed)
        self.assertEqual((self.directory / "history.log").read_bytes(), before)

    def test_second_writer_refused_then_lock_releases(self):
        with self.store():
            with self.assertRaisesRegex(HistoryError, "locked"):
                HistoryStore(self.directory, self.policy)
        with HistoryStore(self.directory, self.policy) as store:
            self.assertEqual(store.sequence, 0)

    def test_byte_and_count_quota_reject_before_any_write(self):
        self.policy.update(max_object_bytes=4, max_total_bytes=6, max_objects=2)
        with self.store() as store:
            store.put("mpt_node", b"1234", PROVENANCE)
            before = (self.directory / "history.log").read_bytes()
            with self.assertRaises(HistoryError):
                store.put("mpt_node", b"567", PROVENANCE)
            self.assertEqual((self.directory / "history.log").read_bytes(), before)
            self.assertEqual(len(list(store.objects.iterdir())), 1)
            store.put("mpt_node", b"56", PROVENANCE)
            with self.assertRaises(HistoryError):
                store.put("mpt_node", b"7", PROVENANCE)
            with self.assertRaises(HistoryError):
                store.put("mpt_node", b"12345", PROVENANCE)
            self.assertEqual(store.total_bytes, 6)

    def test_corruption_is_refused_on_read_and_replay(self):
        with self.store() as store:
            descriptor = store.put("mpt_node", b"node-one", PROVENANCE)
            path = store.objects / descriptor["sha256"]
            path.write_bytes(b"node-two")
            with self.assertRaises(HistoryError):
                store.get(descriptor["sha256"])
        with self.assertRaises(HistoryError):
            HistoryStore(self.directory, self.policy)
        self.assertEqual(path.read_bytes(), b"node-two")

    def test_torn_journal_is_not_truncated_or_repaired(self):
        for suffix in (b"\x00", struct.pack(">I", 20) + b"partial"):
            with self.subTest(suffix=suffix):
                directory = self.parent / ("test-" + suffix.hex())
                with HistoryStore(directory, self.policy, create=True) as store:
                    store.put("mpt_node", b"node", PROVENANCE)
                path = directory / "history.log"
                path.write_bytes(path.read_bytes() + suffix)
                before = path.read_bytes()
                with self.assertRaises(HistoryError):
                    HistoryStore(directory, self.policy)
                self.assertEqual(path.read_bytes(), before)

    def test_journal_order_and_unknown_fields_refused(self):
        with self.store() as store:
            store.put("mpt_node", b"node", PROVENANCE)
        path = self.directory / "history.log"
        raw = path.read_bytes()
        entry = module.strict_json(raw[4:])
        for key, value in (("previous", "ab" * 32), ("sequence", 2), ("verified", True)):
            replacement = canonical(dict(entry, **{key: value}))
            path.write_bytes(struct.pack(">I", len(replacement)) + replacement)
            with self.assertRaises(HistoryError):
                HistoryStore(self.directory, self.policy)
        path.write_bytes(raw)
        with HistoryStore(self.directory, self.policy) as store:
            self.assertEqual(store.sequence, 1)

    def test_orphan_missing_and_unexpected_files_refuse_without_erasure(self):
        with self.store() as store:
            descriptor = store.put("mpt_node", b"node", PROVENANCE)
        orphan = self.directory / "objects" / digest(b"orphan")
        orphan.write_bytes(b"orphan")
        orphan.chmod(0o600)
        with self.assertRaises(HistoryError):
            HistoryStore(self.directory, self.policy)
        self.assertTrue(orphan.exists())
        orphan.unlink()
        original = self.directory / "objects" / descriptor["sha256"]
        original.unlink()
        with self.assertRaises((HistoryError, FileNotFoundError)):
            HistoryStore(self.directory, self.policy)
        self.assertTrue((self.directory / "history.log").exists())

    def test_symlink_store_parent_and_files_are_refused(self):
        self.directory.symlink_to(self.parent, target_is_directory=True)
        with self.assertRaises(HistoryError):
            HistoryStore(self.directory, self.policy)
        self.directory.unlink()
        linked_parent = self.parent / "linked"
        linked_parent.symlink_to(self.parent, target_is_directory=True)
        with self.assertRaises(HistoryError):
            HistoryStore(linked_parent / "new", self.policy, create=True)
        with self.store() as store:
            descriptor = store.put("mpt_node", b"node", PROVENANCE)
        object_path = self.directory / "objects" / descriptor["sha256"]
        object_path.unlink()
        object_path.symlink_to(self.directory / "policy.json")
        with self.assertRaises((HistoryError, OSError)):
            HistoryStore(self.directory, self.policy)

    def test_hardlink_and_public_permissions_refused(self):
        with self.store() as store:
            descriptor = store.put("mpt_node", b"node", PROVENANCE)
        path = self.directory / "objects" / descriptor["sha256"]
        linked = self.parent / "hardlink"
        os.link(path, linked)
        with self.assertRaises(HistoryError):
            HistoryStore(self.directory, self.policy)
        linked.unlink()
        path.chmod(0o644)
        with self.assertRaises(HistoryError):
            HistoryStore(self.directory, self.policy)

    def test_short_journal_writes_are_completed(self):
        actual = os.write
        with self.store() as store:
            with patch.object(module.os, "write", side_effect=lambda fd, raw: actual(fd, raw[:7])):
                descriptor = store.put("mpt_node", b"node", PROVENANCE)
            self.assertEqual(store.get(descriptor["sha256"]), b"node")
        with HistoryStore(self.directory, self.policy) as store:
            self.assertEqual(store.sequence, 1)

    def test_two_concurrent_writers_and_reads_produce_replayable_unique_sequence(self):
        failures = []
        barrier = threading.Barrier(3)
        with self.store() as store:
            def worker(name):
                barrier.wait()
                try:
                    for index in range(20):
                        raw = (name + str(index)).encode()
                        descriptor = store.put("mpt_node", raw, PROVENANCE)
                        self.assertEqual(store.get(descriptor["sha256"]), raw)
                        self.assertGreaterEqual(store.stats()["sequence"], 1)
                except BaseException as error:
                    failures.append(error)
            workers = [threading.Thread(target=worker, args=(name,)) for name in ("one-", "two-")]
            for thread in workers:
                thread.start()
            barrier.wait()
            for thread in workers:
                thread.join(timeout=5)
                self.assertFalse(thread.is_alive())
            self.assertEqual(failures, [])
            self.assertEqual(store.sequence, 40)
        with HistoryStore(self.directory, self.policy) as store:
            self.assertEqual(store.sequence, 40)
            for name in ("one-", "two-"):
                for index in range(20):
                    raw = (name + str(index)).encode()
                    self.assertEqual(store.get(digest(raw)), raw)

    def test_same_instance_read_racing_close_returns_bytes_or_clean_refusal(self):
        failures = []
        barrier = threading.Barrier(2)
        store = self.store()
        descriptor = store.put("mpt_node", b"node", PROVENANCE)
        def reader():
            barrier.wait()
            try:
                self.assertEqual(store.get(descriptor["sha256"]), b"node")
            except HistoryError:
                pass  # Closed before acquisition of the per-instance lock.
            except BaseException as error:
                failures.append(error)
        worker = threading.Thread(target=reader)
        worker.start()
        barrier.wait()
        store.close()
        worker.join(timeout=5)
        self.assertFalse(worker.is_alive())
        self.assertEqual(failures, [])
        with HistoryStore(self.directory, self.policy) as reopened:
            self.assertEqual(reopened.get(descriptor["sha256"]), b"node")

    def test_failed_partial_append_fences_and_preserves_torn_evidence(self):
        actual = os.write
        attempts = []
        def fail(fd, raw):
            if not attempts:
                attempts.append(True)
                return actual(fd, raw[:5])
            raise OSError("synthetic disk refusal")
        with self.store() as store:
            with patch.object(module.os, "write", side_effect=fail):
                with self.assertRaises(OSError):
                    store.put("mpt_node", b"node", PROVENANCE)
            self.assertTrue(store.stats()["fenced"])
            with self.assertRaises(HistoryError):
                store.put("mpt_node", b"later", PROVENANCE)
        before = (self.directory / "history.log").read_bytes()
        with self.assertRaises(HistoryError):
            HistoryStore(self.directory, self.policy)
        self.assertEqual((self.directory / "history.log").read_bytes(), before)

    def test_receipt_envelope_preserves_indices_and_grants_no_authority(self):
        with self.store() as store:
            header = store.put("l2_header_rlp", b"header", PROVENANCE)["sha256"]
            receipt = store.put("l2_receipt", b"receipt", PROVENANCE)["sha256"]
            transaction = store.put("l2_transaction", b"\x64tx", PROVENANCE)["sha256"]
            output = store.raw_block(header, [receipt, receipt], transaction_digests=[transaction, transaction])
            self.assertEqual(output["receipts"], [{"index": 0, "encoded": "0x72656365697074"},
                                                   {"index": 1, "encoded": "0x72656365697074"}])
            self.assertEqual(output["transactions"], [{"index": 0, "encoded": "0x647478"},
                                                       {"index": 1, "encoded": "0x647478"}])
            self.assertFalse(output["authority"])
            with self.assertRaises(HistoryError):
                store.raw_block(header, [receipt], transaction_digests=[transaction], chain_id=1)
            with self.assertRaises(HistoryError):
                store.raw_block(header, [header], transaction_digests=[transaction])
            with self.assertRaises(HistoryError):
                store.raw_block(header, [], transaction_digests=[], chain_id=True)
            with self.assertRaises(HistoryError):
                store.raw_block(header, [receipt], transaction_digests=[])
            with self.assertRaises(HistoryError):
                store.raw_block(header, [receipt], transaction_digests=[receipt])

    def test_descriptor_and_provenance_cannot_smuggle_authority_or_paths(self):
        with self.store() as store:
            for kind, raw, provenance in (("verified_header", b"x", PROVENANCE),
                                          ("mpt_node", b"", PROVENANCE),
                                          ("mpt_node", b"x", dict(PROVENANCE, verified=True)),
                                          ("mpt_node", b"x", {"transport": "rpc-dump/1", "peer_id": "x"}),
                                          ("mpt_node", b"x", {"transport": "local-nitro-replay/1", "peer_id": "../key"})):
                with self.assertRaises(HistoryError):
                    store.put(kind, raw, provenance)
            for bad in ("../policy.json", "AB" * 32, None):
                with self.assertRaises(HistoryError):
                    store.get(bad)
            self.assertEqual(store.sequence, 0)

    def test_strict_json_rejects_duplicate_and_nonfinite_fields(self):
        for raw in (b'{"x":1,"x":2}', b'{"x":NaN}', b'{"x":Infinity}', b'\xff'):
            with self.assertRaises(HistoryError):
                module.strict_json(raw)


if __name__ == "__main__":
    unittest.main()
