"""Bounded generated-store failures; no live keys, wallets, nodes or services.

The subprocess cases report actual exits. Injected fsync errors exercise the
adapter's explicit barrier, not SQLite's internal VFS or device power failure.
All destructive fixture changes target databases in fresh temporary directories;
the ownership lock file is never unlinked while a store may be using it.
"""
from copy import deepcopy
from contextlib import closing
import errno
import json
import os
from pathlib import Path
import select
import shutil
import signal
import sqlite3
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "flowmesh_v2_agreement"))
sys.path.insert(0, str(HERE.parent / "flowmesh_v2_model"))

import fm_disk_store as storage
import fm_codec as codec
from fm_disk_store import DiskStore, StorageError
from fm_codec import CodecError, decode, encode
from fm_memory import update_durable
from fm_simulator import Simulator
from test_model import Fixture


def generated_state():
    simulator = Simulator(Fixture().model.snapshot())
    simulator.offer(nodes=[0])
    node = simulator.nodes[0]
    identity = {"signer": node.index, "config": node.config,
                "genesis": node.d["parent"]}
    return identity, deepcopy(node.d)


def changed_state(original):
    candidate = deepcopy(original)
    candidate["records"][candidate["sequence"]]["view"] += 1
    candidate["records"][candidate["sequence"]]["mode"] = "CHANGING"
    return candidate


def ownership_child():
    """A cooperating owner/contender, launched with a fresh Python interpreter."""
    directory, identity_json, mode = sys.argv[2:]
    try:
        owner = DiskStore(directory, json.loads(identity_json))
    except StorageError as exc:
        print(json.dumps({"code": exc.code, "pid": os.getpid()}), flush=True)
        return 23
    print(json.dumps({"code": "OPEN", "pid": os.getpid(), "head": owner.head}),
          flush=True)
    if mode == "hold":
        sys.stdin.readline()
        # Intentionally bypass close/finalizers. The OS must release ownership.
        os._exit(81)
    owner.close()
    return 0


class TypedCodecTests(unittest.TestCase):
    def test_roundtrip_preserves_integer_tuple_keys_bytes_and_container_types(self):
        value = {0: {("PROPOSE", 0): b"\x00\xffgenerated"},
                 "0": [None, True, False, -7, 2**80, "unicode \u03bb"],
                 ("view", 1): (b"", {}, [], ())}
        actual = decode(encode(value))
        self.assertEqual(actual, value)
        self.assertIs(type(actual[("view", 1)]), tuple)
        self.assertIs(type(actual["0"]), list)
        self.assertIs(type(actual[0][("PROPOSE", 0)]), bytes)
        self.assertIs(type(next(key for key in actual if key == 0)), int)

    def test_encoding_is_deterministic_across_map_insertion_order(self):
        left = {("COMMIT", 3): {2: b"b", 1: b"a"}, 0: "zero", "0": "text"}
        right = {"0": "text", 0: "zero", ("COMMIT", 3): {1: b"a", 2: b"b"}}
        self.assertEqual(encode(left), encode(right))
        self.assertEqual(encode(decode(encode(left))), encode(left))

    def test_unsupported_python_objects_are_refused(self):
        for value in (1.5, float("nan"), {1, 2}, object(), bytearray(b"x")):
            with self.subTest(type=type(value).__name__):
                with self.assertRaises(CodecError):
                    encode(value)

    def test_malformed_truncated_and_untyped_json_are_refused(self):
        valid = encode({("PREPARE", 0): b"synthetic"})
        for payload in (b"", b"{", b"null", b"{}", b"[]", b"\xff",
                        valid[:-1], valid[:len(valid) // 2], valid + b"trailing"):
            with self.subTest(payload=repr(payload[:50])):
                with self.assertRaises(CodecError):
                    decode(payload)

    def test_deep_nesting_is_a_bounded_refusal(self):
        value = None
        for _ in range(256):
            value = [value]
        with self.assertRaises(CodecError):
            encode(value)

    def test_duplicate_keys_invalid_tags_versions_and_noncanonical_scalars_refuse(self):
        duplicate = ["d", [[["i", "0"], ["s", "first"]],
                           [["i", "0"], ["s", "second"]]]]
        for node in (duplicate, ["unknown", "x"], ["i", "01"], ["i", "+1"],
                     ["b", "invalid!"], ["z", 1], ["n", None]):
            with self.subTest(node=node):
                payload = json.dumps(["flowmesh-test-json", 1, node],
                                     separators=(",", ":")).encode()
                with self.assertRaises(CodecError):
                    decode(payload)
        for payload in (b'["flowmesh-test-json",2,["n"]]',
                        b'["flowmesh-test-json",true,["n"]]',
                        b'["flowmesh-test-json",1, ["n"]]'):
            with self.subTest(payload=payload):
                with self.assertRaises(CodecError):
                    decode(payload)

    def test_allocation_bounds_are_explicit_codec_errors(self):
        cases = (("MAX_ATOM_BYTES", 3, b"four"),
                 ("MAX_CONTAINER_ITEMS", 1, [0, 1]),
                 ("MAX_NODES", 2, [0, 1]),
                 ("MAX_INTEGER_BITS", 8, 256),
                 ("MAX_ENCODED_BYTES", 8, None))
        for bound, cap, value in cases:
            with self.subTest(bound=bound):
                encoded = encode(value)
                with patch.object(codec, bound, cap):
                    with self.assertRaises(CodecError):
                        encode(value)
                    with self.assertRaises(CodecError):
                        decode(encoded)


class DiskStoreTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="fm-v2-store-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name) / "signer-0"
        self.identity, self.initial = generated_state()

    def open_store(self, **kwargs):
        store = DiskStore(self.directory, self.identity, **kwargs)
        self.addCleanup(store.close)
        return store

    def assert_refusal(self, code, function, *args, **kwargs):
        with self.assertRaises(StorageError) as caught:
            function(*args, **kwargs)
        self.assertEqual(caught.exception.code, code)
        return caught.exception

    def provision(self):
        store = self.open_store(initial=self.initial)
        self.assertEqual(store.head["generation"], 0)
        store.close()

    def corrupt_database(self, statement, parameters=()):
        database = self.directory / storage.STORE_FILE
        with closing(sqlite3.connect(database)) as connection, connection:
            connection.execute(statement, parameters)

    def test_reopen_preserves_exact_state_and_identity_and_keeps_lock_inode(self):
        store = self.open_store(initial=self.initial)
        lock = self.directory / storage.LOCK_FILE
        inode = lock.stat().st_ino
        original_head = deepcopy(store.head)
        self.assertEqual(store.load(), self.initial)
        candidate = changed_state(self.initial)
        store.commit(self.initial, candidate, "GENERATED_CHANGE_VIEW")
        head = deepcopy(store.head)
        self.assertEqual(head["generation"], 1)
        self.assertNotEqual(head["digest"], original_head["digest"])
        store.close()
        reopened = self.open_store(trusted_head=head)
        self.assertEqual(reopened.load(), candidate)
        self.assertEqual(reopened.head, head)
        self.assertEqual(lock.stat().st_ino, inode)
        self.assertEqual(reopened.load()["records"][0]["signed"],
                         self.initial["records"][0]["signed"])

    def test_load_returns_private_state(self):
        store = self.open_store(initial=self.initial)
        loaded = store.load()
        loaded["records"][0]["signed"].clear()
        loaded["anchor"].clear()
        self.assertEqual(store.load(), self.initial)

    def test_missing_storage_is_not_silently_reprovisioned(self):
        self.assert_refusal("STORE_MISSING", self.open_store)
        self.assertFalse((self.directory / storage.STORE_FILE).exists())

    def test_existing_directory_without_database_is_missing(self):
        self.directory.mkdir()
        self.assert_refusal("STORE_MISSING", self.open_store)
        self.assertFalse((self.directory / storage.STORE_FILE).exists())

    def test_file_instead_of_directory_is_unavailable(self):
        self.directory.write_bytes(b"generated non-directory fixture")
        self.assert_refusal("STORE_UNAVAILABLE", self.open_store, initial=self.initial)
        self.assertEqual(self.directory.read_bytes(), b"generated non-directory fixture")

    def test_identity_mismatch_refuses_and_releases_ownership(self):
        self.provision()
        for field, replacement in (("signer", 1), ("config", "different-config"),
                                   ("genesis", "different-genesis")):
            with self.subTest(field=field):
                wrong = dict(self.identity, **{field: replacement})
                self.assert_refusal("IDENTITY_MISMATCH", DiskStore,
                                    self.directory, wrong)
                correct = self.open_store()
                self.assertEqual(correct.load(), self.initial)
                correct.close()

    def test_malformed_and_truncated_database_refuse_even_if_initial_supplied(self):
        self.provision()
        database = self.directory / storage.STORE_FILE
        valid = database.read_bytes()
        for payload in (b"not a SQLite database", valid[:100], valid[:len(valid) // 2]):
            with self.subTest(length=len(payload)):
                database.write_bytes(payload)
                self.assert_refusal("STORE_CORRUPT", self.open_store)
                self.assert_refusal("STORE_CORRUPT", self.open_store,
                                    initial=self.initial)
                self.assertEqual(database.read_bytes(), payload)

    def test_truncated_typed_record_refuses(self):
        self.provision()
        self.corrupt_database("UPDATE state SET value=? WHERE section='records'",
                              (b'{"truncated":',))
        self.assert_refusal("STORE_CORRUPT", self.open_store)

    def test_well_formed_but_wrong_materialized_state_refuses(self):
        self.provision()
        self.corrupt_database("UPDATE state SET value=? WHERE section='metadata' AND key=?",
                              (encode("UNJOURNALED_CHANGE"), encode("halt")))
        self.assert_refusal("STORE_CORRUPT", self.open_store)

    def test_deleted_protected_record_refuses(self):
        self.provision()
        self.corrupt_database("DELETE FROM state WHERE section='records'")
        self.assert_refusal("STORE_CORRUPT", self.open_store)

    def test_invalid_required_record_shape_is_refused_before_acknowledgment(self):
        for invalid_field in ("intents", "signed", "instance", "apply_count"):
            with self.subTest(field=invalid_field):
                invalid = deepcopy(self.initial)
                del invalid["records"][0][invalid_field]
                self.assert_refusal("STORE_CORRUPT", DiskStore,
                                    Path(self.temporary.name) / invalid_field, self.identity,
                                    initial=invalid)

    def test_missing_identity_metadata_is_not_treated_as_fresh_store(self):
        self.provision()
        self.corrupt_database("DELETE FROM meta WHERE name='identity'")
        self.assert_refusal("STORE_CORRUPT", self.open_store, initial=self.initial)

    def test_malformed_journal_payload_refuses(self):
        self.provision()
        self.corrupt_database("UPDATE journal SET payload=? WHERE generation=0",
                              (b"truncated typed journal",))
        self.assert_refusal("STORE_CORRUPT", self.open_store)

    def test_journal_digest_or_predecessor_mismatch_refuses(self):
        for column in ("digest", "previous_digest"):
            with self.subTest(column=column):
                case = Path(self.temporary.name) / column
                store = DiskStore(case, self.identity, initial=self.initial)
                store.commit(self.initial, changed_state(self.initial), "GENERATED_CHANGE")
                store.close()
                with closing(sqlite3.connect(case / storage.STORE_FILE)) as connection, connection:
                    connection.execute(f"UPDATE journal SET {column}=? WHERE generation=1",
                                       ("f" * 64,))
                self.assert_refusal("STORE_CORRUPT", DiskStore, case, self.identity)

    def test_missing_journal_entry_refuses(self):
        store = self.open_store(initial=self.initial)
        store.commit(self.initial, changed_state(self.initial), "GENERATED_CHANGE")
        store.close()
        self.corrupt_database("DELETE FROM journal WHERE generation=0")
        self.assert_refusal("STORE_CORRUPT", self.open_store)

    def test_stale_writer_state_cannot_replace_current_obligations(self):
        store = self.open_store(initial=self.initial)
        candidate = changed_state(self.initial)
        store.commit(self.initial, candidate, "GENERATED_CHANGE")
        head = deepcopy(store.head)
        other = deepcopy(self.initial)
        other["halt"] = "STALE_CANDIDATE"
        self.assert_refusal("STALE_STATE", store.commit, self.initial, other, "STALE")
        store.close()
        reopened = self.open_store()
        self.assertEqual(reopened.load(), candidate)
        self.assertEqual(reopened.head, head)

    def test_record_pruning_is_refused_without_erasing_signed_obligations(self):
        store = self.open_store(initial=self.initial)
        candidate = deepcopy(self.initial)
        candidate["records"].clear()
        self.assert_refusal("STORE_CORRUPT", store.commit,
                            self.initial, candidate, "PRUNE_OBLIGATION")
        store.close()
        reopened = self.open_store()
        self.assertEqual(reopened.load(), self.initial)

    def test_injected_io_at_every_transaction_stage_poison_and_reopen_atomically(self):
        before_commit = {"before_begin", "after_snapshot_row", "after_rows", "before_commit"}
        stages = ("before_begin", "after_snapshot_row", "after_rows", "before_commit", "after_commit",
                  "before_fsync", "after_fsync")
        self.assertEqual(set(storage.STAGES), set(stages))
        for stage in stages:
            with self.subTest(stage=stage):
                directory = Path(self.temporary.name) / stage
                store = DiskStore(directory, self.identity, initial=self.initial)
                self.addCleanup(store.close)
                candidate = changed_state(self.initial)
                # Byte-store atomicity only: semantic accounting snapshots are
                # exercised through DiskReplica in the real-process cases.
                candidate["snapshot"] = b"generated-byte-store-transaction"
                visited = []

                def fault(point, reason):
                    visited.append((point, reason))
                    if point == stage:
                        raise OSError(errno.ENOSPC, "generated full-device error")

                store.fault = fault
                self.assert_refusal("STORAGE_IO", store.commit,
                                    self.initial, candidate, "GENERATED_IO_FAILURE")
                self.assertIn((stage, "GENERATED_IO_FAILURE"), visited)
                self.assert_refusal("STORE_POISONED", store.load)
                self.assert_refusal("STORE_POISONED", store.commit,
                                    self.initial, candidate, "RETRY_AFTER_FAILURE")
                store.close()
                reopened = DiskStore(directory, self.identity)
                self.addCleanup(reopened.close)
                expected = self.initial if stage in before_commit else candidate
                self.assertEqual(reopened.load(), expected)
                self.assertEqual(reopened.head["generation"],
                                 0 if stage in before_commit else 1)
                self.assertEqual(reopened.load()["records"][0]["signed"],
                                 self.initial["records"][0]["signed"])
                reopened.close()

    def test_explicit_file_and_directory_fsync_errors_poison_after_commit(self):
        for fail_directory in (False, True):
            with self.subTest(fail_directory=fail_directory):
                directory = Path(self.temporary.name) / ("dir-fsync" if fail_directory else "file-fsync")
                store = DiskStore(directory, self.identity, initial=self.initial)
                self.addCleanup(store.close)
                candidate = changed_state(self.initial)
                observed = []
                real_fsync = os.fsync

                def failed_fsync(descriptor):
                    is_directory = stat.S_ISDIR(os.fstat(descriptor).st_mode)
                    observed.append(is_directory)
                    if is_directory == fail_directory:
                        raise OSError(errno.EIO, "generated explicit fsync failure")
                    return real_fsync(descriptor)

                with patch.object(storage.os, "fsync", side_effect=failed_fsync):
                    self.assert_refusal("STORAGE_IO", store.commit,
                                        self.initial, candidate, "FSYNC_FAILURE")
                self.assertIn(fail_directory, observed)
                self.assert_refusal("STORE_POISONED", store.load)
                store.close()
                reopened = DiskStore(directory, self.identity)
                self.addCleanup(reopened.close)
                self.assertEqual(reopened.load(), candidate)
                self.assertEqual(reopened.head["generation"], 1)
                reopened.close()

    def test_sqlite_write_refusal_poisons_without_publishing_partial_rows(self):
        store = self.open_store(initial=self.initial)
        old_head = deepcopy(store.head)
        self.assertEqual(store._connection.execute("PRAGMA journal_mode").fetchone(),
                         ("delete",))
        self.assertEqual(store._connection.execute("PRAGMA synchronous").fetchone(),
                         (2,))
        # SQLite itself rejects the attempted write. This is separate from the
        # injected transaction hooks and does not claim a device/VFS failure.
        store._connection.execute("PRAGMA query_only=ON")
        self.assert_refusal("STORAGE_IO", store.commit, self.initial,
                            changed_state(self.initial), "SQLITE_READONLY_FAILURE")
        self.assert_refusal("STORE_POISONED", store.load)
        store.close()
        reopened = self.open_store()
        self.assertEqual(reopened.head, old_head)
        self.assertEqual(reopened.load(), self.initial)

    def test_memory_callback_failure_never_publishes_unacknowledged_candidate(self):
        for stage in ("before_commit", "after_commit"):
            with self.subTest(stage=stage):
                directory = Path(self.temporary.name) / stage
                old = deepcopy(self.initial)
                records_alias = old["records"]
                record_alias = old["records"][0]
                encoded_before = encode(old)
                store = DiskStore(directory, self.identity, initial=old)
                self.addCleanup(store.close)

                def fault(point, reason):
                    if point == stage:
                        raise OSError(errno.EIO, "generated acknowledgment failure")

                def mutate(candidate):
                    candidate["records"][0]["view"] += 1
                    candidate["records"][0]["mode"] = "CHANGING"

                store.fault = fault
                self.assert_refusal(
                    "STORAGE_IO", update_durable, old, mutate,
                    before_commit=lambda original, candidate:
                    store.commit(original, candidate, "MEMORY_CALLBACK_FAILURE"))
                self.assertIs(old["records"], records_alias)
                self.assertIs(old["records"][0], record_alias)
                self.assertEqual(encode(old), encoded_before)
                self.assert_refusal("STORE_POISONED", store.load)
                store.close()
                reopened = DiskStore(directory, self.identity)
                self.addCleanup(reopened.close)
                self.assertEqual(reopened.load(), old if stage == "before_commit"
                                 else changed_state(old))
                reopened.close()

    def test_bounds_stop_new_writes_without_pruning_existing_obligations(self):
        for bound, cap in (("MAX_JOURNAL_ENTRIES", 1), ("MAX_JOURNAL_BYTES", 1),
                           ("MAX_DATABASE_BYTES", 1), ("MAX_ROWS", 1),
                           ("MAX_ROW_BYTES", 1)):
            with self.subTest(bound=bound):
                directory = Path(self.temporary.name) / bound.lower()
                store = DiskStore(directory, self.identity, initial=self.initial)
                self.addCleanup(store.close)
                head = deepcopy(store.head)
                with patch.object(storage, bound, cap):
                    self.assert_refusal("STORAGE_BOUND", store.commit,
                                        self.initial, changed_state(self.initial), "BOUND")
                store.close()
                reopened = DiskStore(directory, self.identity)
                self.addCleanup(reopened.close)
                self.assertEqual(reopened.head, head)
                self.assertEqual(reopened.load(), self.initial)
                reopened.close()

    def test_trusted_generation_refuses_restored_backup_but_unobserved_rollback_is_gap(self):
        store = self.open_store(initial=self.initial)
        old_head = deepcopy(store.head)
        store.close()
        backup = Path(self.temporary.name) / "coherent-old.sqlite3"
        shutil.copyfile(self.directory / storage.STORE_FILE, backup)
        store = self.open_store()
        store.commit(self.initial, changed_state(self.initial), "AFTER_BACKUP")
        observed_head = deepcopy(store.head)
        store.close()
        shutil.copyfile(backup, self.directory / storage.STORE_FILE)
        self.assert_refusal("ROLLBACK_DETECTED", self.open_store,
                            trusted_head=observed_head)
        # Coherent restoration without an external observation is explicitly
        # undetectable. This accepted open is a documented gap, not a guarantee.
        unobserved = self.open_store()
        self.assertEqual(unobserved.head, old_head)
        self.assertEqual(unobserved.load(), self.initial)

    def test_trusted_same_generation_wrong_digest_refuses(self):
        store = self.open_store(initial=self.initial)
        head = dict(store.head, digest="f" * 64)
        store.close()
        self.assert_refusal("ROLLBACK_DETECTED", self.open_store, trusted_head=head)

    def test_older_trusted_head_must_match_the_retained_chain(self):
        store = self.open_store(initial=self.initial)
        observed = deepcopy(store.head)
        store.commit(self.initial, changed_state(self.initial), "NEXT_GENERATION")
        store.close()
        reopened = self.open_store(trusted_head=observed)
        self.assertEqual(reopened.head["generation"], 1)
        reopened.close()
        self.assert_refusal("ROLLBACK_DETECTED", self.open_store,
                            trusted_head=dict(observed, digest="e" * 64))

    def test_closed_store_refuses_reads_and_writes(self):
        store = self.open_store(initial=self.initial)
        store.close()
        self.assert_refusal("STORE_CLOSED", store.load)
        self.assert_refusal("STORE_CLOSED", store.commit,
                            self.initial, changed_state(self.initial), "CLOSED")
        store.close()

    def test_duplicate_ownership_in_distinct_processes_and_real_exit_release(self):
        self.provision()
        lock = self.directory / storage.LOCK_FILE
        inode = lock.stat().st_ino
        command = [sys.executable, str(Path(__file__).resolve()), "--ownership-child",
                   str(self.directory), json.dumps(self.identity)]
        for termination in ("exit", "kill"):
            with self.subTest(termination=termination):
                owner = subprocess.Popen(command + ["hold"], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                         text=True)
                try:
                    readable, _, _ = select.select([owner.stdout], [], [], 10)
                    self.assertTrue(readable, "owner failed to report readiness within 10s")
                    ready = json.loads(owner.stdout.readline())
                    self.assertEqual(ready["code"], "OPEN")
                    self.assertNotEqual(ready["pid"], os.getpid())
                    contender = subprocess.run(command + ["contend"], capture_output=True,
                                               text=True, timeout=10, check=False)
                    self.assertEqual(contender.returncode, 23, contender.stderr)
                    refused = json.loads(contender.stdout)
                    self.assertEqual(refused["code"], "STORE_LOCKED")
                    self.assertNotEqual(refused["pid"], ready["pid"])
                    if termination == "kill":
                        owner.kill()
                        owner.communicate(timeout=10)
                        self.assertEqual(owner.returncode, -signal.SIGKILL)
                    else:
                        owner.communicate("exit\n", timeout=10)
                        self.assertEqual(owner.returncode, 81)
                finally:
                    if owner.poll() is None:
                        owner.kill()
                        owner.communicate(timeout=10)
                    for stream in (owner.stdin, owner.stdout, owner.stderr):
                        stream.close()
                reopened = self.open_store()
                self.assertEqual(reopened.load(), self.initial)
                self.assertEqual(lock.stat().st_ino, inode)
                reopened.close()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--ownership-child":
        raise SystemExit(ownership_child())
    unittest.main()
