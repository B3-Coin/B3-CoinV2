"""Wire/IO tests only; synthetic bytes NEVER establish Arbitrum authority."""
from dataclasses import replace
import hashlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import history_store
import proof_bundle as m


# Independent literal layout agreed with the native codec owner. This is a
# STRUCTURAL vector: c0/01/02 are not a valid execution/receipt/transaction proof.
GOLDEN_HEX = (
    "4233415250523031000000e8" + "11" * 32 + "22" * 32 + "33" * 32 +
    "0102030405060708111213141516171801" + "44" * 32 + "55" * 32 +
    "000000000000000100000001c00000000100000000000000000000000101" +
    "00000001000000000000000000000001020000000000000000")
GOLDEN = bytes.fromhex(GOLDEN_HEX)


def golden():
    a = m.Assertion(bytes.fromhex("11" * 32), bytes.fromhex("22" * 32), bytes.fromhex("33" * 32),
                    0x0102030405060708, 0x1112131415161718, 1,
                    bytes.fromhex("44" * 32), bytes.fromhex("55" * 32))
    return m.Proof(a, (), (m.BlockWitness(b"\xc0", (m.IndexedRecord(0, b"\x01"),),
                                               (m.IndexedRecord(0, b"\x02"),)),))


SOURCE = {"kind": "owned-regtest", "chain_id": 412346, "genesis_hash": "0x" + "ab" * 32}


def manifest():
    return {"format": "b3-arbitrum-proof-manifest/1", "mainnet_enabled": False, "writes_enabled": False,
        "source": dict(SOURCE), "assertion": {
            "parent_assertion": "0x" + "00" * 32, "block_hash": "0x" + "00" * 32,
            "send_root": "0x" + "00" * 32, "inbox_position": 0, "position_in_message": 0,
            "machine_status": 1, "end_history_root": "0x" + "00" * 32,
            "inbox_accumulator": "0x" + "00" * 32},
        "l1_accounts": [], "blocks": [{"header_sha256": hashlib.sha256(b"header").hexdigest(),
            "receipt_sha256": [hashlib.sha256(b"receipt").hexdigest()],
            "transaction_sha256": [hashlib.sha256(b"transaction").hexdigest()]}],
        "confirmed_accounts": [], "historical_accounts": []}


class MemoryObjects:
    def __init__(self):
        self.source, self.values, self.reads = dict(SOURCE), {}, 0
        for kind, value in (("l2_header_rlp", b"header"), ("l2_receipt", b"receipt"),
                            ("l2_transaction", b"transaction"), ("mpt_node", b"node")):
            self.values[hashlib.sha256(value).hexdigest()] = (kind, value)

    def read(self, kind, digest, maximum):
        self.reads += 1
        m.require(digest in self.values, "Missing test object")
        actual_kind, value = self.values[digest]
        m.require(kind == actual_kind and 0 < len(value) <= maximum and
                  hashlib.sha256(value).hexdigest() == digest, "Test object integrity/kind failure")
        return value


class ProofWireTests(unittest.TestCase):
    def test_independent_native_golden_exact_bytes(self):
        self.assertEqual(len(GOLDEN), 244)
        self.assertEqual(GOLDEN[8:12], b"\x00\x00\x00\xe8")
        self.assertEqual(m.encode_proof(golden()), GOLDEN)
        self.assertEqual(m.decode_proof(GOLDEN), golden())
        self.assertEqual(hashlib.sha256(GOLDEN).hexdigest(),
                         "b09b19ebcbc5f1ad29166bc563f278f6cfd07742ee9c7216a56629d71ae23d88")
        self.assertEqual(m.wire_digest(GOLDEN),
                         "aa989c48713b75b092e0863a2766e5870f1a1a807f7b8acfcf682745a68876d3")

    def test_nonempty_accounts_storage_and_raw_order_roundtrip(self):
        account = m.AccountWitness(bytes(range(20)), (b"\xc1\x80",),
            (m.StorageWitness(bytes(range(32)), (b"\xc2\x01\x02", b"\xc0")),))
        proof = replace(golden(), l1_accounts=(account,), confirmed_accounts=(account,), historical_accounts=(account,))
        encoded = m.encode_proof(proof)
        self.assertIn(bytes(range(32)), encoded)
        self.assertEqual(m.decode_proof(encoded), proof)

    def test_every_truncated_prefix_refused(self):
        for end in range(len(GOLDEN)):
            with self.subTest(end=end), self.assertRaises(m.ProofError):
                m.decode_proof(GOLDEN[:end])

    def test_wrong_magic_length_and_trailing_bytes_refused(self):
        for value in (b"B3ARPR02" + GOLDEN[8:], GOLDEN[:8] + bytes(4) + GOLDEN[12:], GOLDEN + b"\x00",
                      GOLDEN[:8] + struct.pack(">I", 233) + GOLDEN[12:] + b"\x00"):
            with self.assertRaises(m.ProofError):
                m.decode_proof(value)

    def test_bounded_counts_rejected_before_iteration(self):
        for offset, count in ((189, 17), (193, 129), (202, 8193)):
            value = bytearray(GOLDEN)
            struct.pack_into(">I", value, offset, count)
            with self.assertRaises(m.ProofError):
                m.decode_proof(bytes(value))

    def test_nonzero_first_and_duplicate_record_indices_refused(self):
        value = bytearray(GOLDEN)
        struct.pack_into(">Q", value, 206, 1)
        with self.assertRaises(m.ProofError):
            m.decode_proof(bytes(value))
        block = replace(golden().blocks[0], receipts=(m.IndexedRecord(0, b"a"), m.IndexedRecord(0, b"b")),
                        transactions=(m.IndexedRecord(0, b"a"), m.IndexedRecord(1, b"b")))
        with self.assertRaises(m.ProofError):
            m.encode_proof(replace(golden(), blocks=(block,)))

    def test_receipt_transaction_count_mismatch_refused(self):
        block = replace(golden().blocks[0], transactions=())
        with self.assertRaises(m.ProofError):
            m.encode_proof(replace(golden(), blocks=(block,)))
        # Remove the one transaction while preserving an otherwise exact frame.
        value = GOLDEN[:219] + bytes(4) + GOLDEN[236:]
        value = value[:8] + struct.pack(">I", len(value) - 12) + value[12:]
        with self.assertRaises(m.ProofError):
            m.decode_proof(value)

    def test_empty_and_oversized_raw_fields_refused(self):
        for block in (replace(golden().blocks[0], header=b""),
                      replace(golden().blocks[0], header=b"h" * (m.MAX_HEADER + 1)),
                      replace(golden().blocks[0], receipts=(m.IndexedRecord(0, b""),)),
                      replace(golden().blocks[0], transactions=(m.IndexedRecord(0, b"x" * (m.MAX_RECORD + 1)),))):
            with self.assertRaises(m.ProofError):
                m.encode_proof(replace(golden(), blocks=(block,)))

    def test_fixed_width_integer_and_type_rejection(self):
        for field, value in (("inbox_position", True), ("inbox_position", -1),
                             ("inbox_position", 1 << 64), ("machine_status", 256),
                             ("parent_assertion", b"x" * 31), ("send_root", bytearray(32))):
            with self.assertRaises(m.ProofError):
                m.encode_proof(replace(golden(), assertion=replace(golden().assertion, **{field: value})))
        with self.assertRaises(m.ProofError):
            m.encode_proof(replace(golden(), blocks=list(golden().blocks)))

    def test_neutral_history_assertion_is_not_authority(self):
        proof = replace(golden(), assertion=m.Assertion(), l1_accounts=())
        self.assertEqual(m.decode_proof(m.encode_proof(proof)), proof)
        self.assertEqual(proof.assertion.block_hash, bytes(32))
        # u8 status is structural. Only the native semantic verifier accepts FINISHED.
        bad_execution = replace(proof, assertion=replace(proof.assertion, machine_status=255))
        self.assertEqual(m.decode_proof(m.encode_proof(bad_execution)), bad_execution)

    def test_node_storage_and_account_bounds(self):
        for account in (m.AccountWitness(bytes(20), (b"x",) * 65),
                        m.AccountWitness(bytes(20), (b"x" * 8193,)),
                        m.AccountWitness(bytes(20), (), (m.StorageWitness(bytes(32)),) * 66)):
            with self.assertRaises(m.ProofError):
                m.encode_proof(replace(golden(), confirmed_accounts=(account,)))
        with self.assertRaises(m.ProofError):
            m.encode_proof(replace(golden(), l1_accounts=(m.AccountWitness(bytes(20)),) * 17))

    def test_aggregate_node_count(self):
        account = m.AccountWitness(bytes(20), (), (m.StorageWitness(bytes(32), (b"x",) * 64),) * 65)
        with self.assertRaises(m.ProofError):
            m.encode_proof(replace(golden(), l1_accounts=(account, account)))

    def test_aggregate_receipt_count(self):
        records = tuple(m.IndexedRecord(i, b"x") for i in range(8192))
        block = m.BlockWitness(b"h", records, records)
        with self.assertRaises(m.ProofError):
            m.encode_proof(replace(golden(), blocks=(block,) * 9))

    def test_aggregate_frame_bound(self):
        records = (m.IndexedRecord(0, b"x" * m.MAX_RECORD),)
        block = m.BlockWitness(b"h", records, records)
        with self.assertRaises(m.ProofError):
            m.encode_proof(replace(golden(), blocks=(block,) * 16))
        with self.assertRaises(m.ProofError):
            m.decode_proof(MAGIC_TOO_LONG := m.MAGIC + bytes(m.MAX_FRAME - 7))
        self.assertGreater(len(MAGIC_TOO_LONG), m.MAX_FRAME)


class ProofManifestTests(unittest.TestCase):
    def test_exact_cas_block_assembly_without_expanded_hex(self):
        objects, value = MemoryObjects(), manifest()
        proof = m.assemble_proof(objects, value)
        self.assertEqual(proof.blocks[0].header, b"header")
        self.assertEqual(proof.blocks[0].transactions[0].encoded, b"transaction")
        self.assertEqual(m.decode_proof(m.encode_proof(proof)), proof)
        self.assertEqual(value, manifest())

    def test_unknown_authority_policy_and_request_fields_refused(self):
        for name, value in (("verified", True), ("policy", {}), ("request", {}),
                            ("mainnet_enabled", True), ("writes_enabled", 0)):
            candidate = dict(manifest(), **{name: value})
            with self.assertRaises(m.ProofError):
                m.assemble_proof(MemoryObjects(), candidate)

    def test_explicit_source_chain_and_genesis_no_aliasing(self):
        for source in (dict(SOURCE, chain_id=42161), dict(SOURCE, chain_id=1), dict(SOURCE, chain_id=True),
                       dict(SOURCE, genesis_hash="0x" + "00" * 32), dict(SOURCE, kind="verified")):
            with self.assertRaises(m.ProofError):
                m.source_context(source)
        candidate = manifest()
        candidate["source"]["chain_id"] = 31337
        with self.assertRaises(m.ProofError):
            m.assemble_proof(MemoryObjects(), candidate)

    def test_missing_wrong_kind_and_changed_raw_objects(self):
        for mutation in ("missing", "kind", "bytes"):
            objects = MemoryObjects()
            key = manifest()["blocks"][0]["receipt_sha256"][0]
            if mutation == "missing":
                del objects.values[key]
            elif mutation == "kind":
                objects.values[key] = ("l2_transaction", b"receipt")
            else:
                objects.values[key] = ("l2_receipt", b"corrupt")
            with self.assertRaises(m.ProofError):
                m.assemble_proof(objects, manifest())

    def test_counts_and_missing_body_refused(self):
        for transactions in ([], ["00" * 32] * 8193):
            value, objects = manifest(), MemoryObjects()
            value["blocks"][0]["transaction_sha256"] = transactions
            with self.assertRaises(m.ProofError):
                m.assemble_proof(objects, value)
            self.assertEqual(objects.reads, 0)

    def test_account_nodes_and_storage_are_raw_objects(self):
        value = manifest()
        node = hashlib.sha256(b"node").hexdigest()
        value["confirmed_accounts"] = [{"address": "0x" + "12" * 20, "nodes": [node],
            "storage": [{"slot": "0x" + "34" * 32, "nodes": [node]}]}]
        proof = m.assemble_proof(MemoryObjects(), value)
        self.assertEqual(proof.confirmed_accounts[0].nodes, (b"node",))
        self.assertEqual(proof.confirmed_accounts[0].storage[0].slot, bytes.fromhex("34" * 32))

    def test_aggregate_io_bound_stops_before_unbounded_assembly(self):
        objects, value = MemoryObjects(), manifest()
        for kind, byte, key in (("l2_receipt", b"r", "receipt_sha256"),
                                ("l2_transaction", b"t", "transaction_sha256")):
            data = byte * m.MAX_RECORD
            digest = hashlib.sha256(data).hexdigest()
            objects.values[digest] = (kind, data)
            value["blocks"][0][key] = [digest]
        value["blocks"] *= 128
        with self.assertRaises(m.ProofError):
            m.assemble_proof(objects, value)
        self.assertLess(objects.reads, 55)


class ProofFileTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.objects = self.root / "objects"
        self.objects.mkdir()
        for _, value in MemoryObjects().values.values():
            (self.objects / hashlib.sha256(value).hexdigest()).write_bytes(value)
        self.path = self.root / "manifest.json"
        self.path.write_text(json.dumps(manifest()), encoding="utf-8")

    def tearDown(self):
        self.temporary.cleanup()

    def test_actual_cli_binary_output_and_truthful_scope(self):
        output = self.root / "proof.bin"
        environment = {"PATH": os.environ.get("PATH", ""), "SYSTEMROOT": os.environ.get("SYSTEMROOT", ""),
                       "PYTHONPATH": str(Path(history_store.__file__).parent)}
        result = subprocess.run([sys.executable, "-B", m.__file__, "export-native-proof", "--manifest", str(self.path),
                                 "--objects-dir", str(self.objects), "--output", str(output)],
                                env=environment, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertFalse(report["native_verified"])
        self.assertFalse(report["arbitrum_authority"])
        self.assertFalse(report["no_rpc_end_to_end_qualified"])
        self.assertTrue(report["external_anchor_required"])
        self.assertEqual(report["source"], SOURCE)
        self.assertEqual(hashlib.sha256(output.read_bytes()).hexdigest(), report["frame_sha256"])
        self.assertEqual(m.decode_proof(output.read_bytes()).blocks[0].header, b"header")

    def test_output_never_overwrites_prior_evidence(self):
        output = self.root / "proof.bin"
        output.write_bytes(b"original")
        with self.assertRaises(FileExistsError), patch("sys.stdout", new=io.StringIO()):
            m.main(["export-native-proof", "--manifest", str(self.path), "--objects-dir", str(self.objects),
                    "--output", str(output)])
        self.assertEqual(output.read_bytes(), b"original")

    def test_object_path_traversal_and_corruption_refused(self):
        objects = m.DirectoryObjects(self.objects, SOURCE)
        for digest in ("../manifest.json", "A" * 64, "0x" + "00" * 32):
            with self.assertRaises(m.ProofError):
                objects.read("l2_header_rlp", digest, 4096)
        digest = hashlib.sha256(b"header").hexdigest()
        (self.objects / digest).write_bytes(b"changed")
        with self.assertRaises(m.ProofError):
            objects.read("l2_header_rlp", digest, 4096)

    def test_unknown_manifest_and_invalid_json_create_no_output(self):
        output = self.root / "proof.bin"
        for raw in (b'{"source":{},"source":{}}', b'[' * 5000 + b']' * 5000,
                    b'{"n":' + b'9' * 10000 + b'}'):
            self.path.write_bytes(raw)
            with self.assertRaises(history_store.HistoryError):
                m.main(["export-native-proof", "--manifest", str(self.path), "--objects-dir", str(self.objects),
                        "--output", str(output)])
            self.assertFalse(output.exists())

    @unittest.skipUnless(os.name == "posix", "Actual POSIX symlink/hardlink test requires Linux")
    def test_symlink_and_hardlink_objects_refused(self):
        digest = hashlib.sha256(b"header").hexdigest()
        target = self.objects / digest
        target.unlink()
        source = self.root / "external"
        source.write_bytes(b"header")
        target.symlink_to(source)
        objects = m.DirectoryObjects(self.objects, SOURCE)
        with self.assertRaises(m.ProofError):
            objects.read("l2_header_rlp", digest, 4096)
        target.unlink()
        os.link(source, target)
        with self.assertRaises(m.ProofError):
            objects.read("l2_header_rlp", digest, 4096)

    @unittest.skipUnless(history_store.fcntl is not None, "Actual owner-only CAS requires Linux")
    def test_actual_history_cas_assembler_preserves_journal(self):
        root = self.root / "history"
        with history_store.HistoryStore(root, history_store.default_policy(), create=True) as store:
            for kind, value in (("l2_header_rlp", b"header"), ("l2_receipt", b"receipt"),
                                ("l2_transaction", b"transaction")):
                store.put(kind, value, {"transport": "synthetic-adversarial-fixture/1", "peer_id": "codec-test"})
            before = store.stats()
            value = manifest()
            value["source"] = {"kind": "arbitrum-one", "chain_id": 42161,
                               "genesis_hash": history_store.ARBITRUM_GENESIS}
            proof = m.assemble_proof(m.HistoryObjects(store), value)
            self.assertEqual(proof.blocks[0].header, b"header")
            self.assertEqual(before, store.stats())
            with self.assertRaises(m.ProofError):
                m.assemble_proof(m.HistoryObjects(store), manifest())
            self.assertEqual(before, store.stats())


if __name__ == "__main__":
    unittest.main()
