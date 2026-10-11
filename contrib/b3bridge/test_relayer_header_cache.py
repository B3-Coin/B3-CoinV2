#!/usr/bin/env python3
"""Offline regression tests for authenticated header batching and durable reuse.

All HTTP calls are mocked. The synthetic chain has authenticated headers and
empty deposit blooms, so no receipt provider, wallet, or live node is involved.
"""

import copy
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import urllib.error
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
import b3_bridge_relayer as relayer
from test_b3_bridge_relayer import FakeRpc, H20, H32, TOKEN


RPC_URL = "https://offline.invalid/execution"


def response(value):
    return io.BytesIO(json.dumps(value).encode())


def rpc_result(request):
    return {"jsonrpc": "2.0", "id": request["id"],
            "result": {"number": request["params"][0]}}


def rehash(header):
    header["hash"] = "0x" + relayer.receipts.keccak256(
        relayer.encode_exec_header(header)).hex()


def empty_bloom_chain(first=50, last=69):
    empty_root = "0x" + relayer.receipts.keccak256(b"\x80").hex()
    template = {
        "parentHash": H32,
        "sha3Uncles": "0x" + relayer.receipts.keccak256(b"\xc0").hex(),
        "miner": H20, "stateRoot": H32, "transactionsRoot": empty_root,
        "receiptsRoot": empty_root, "logsBloom": "0x" + "00" * 256,
        "difficulty": "0x1", "gasLimit": "0x100000", "gasUsed": "0x0",
        "extraData": "0x", "mixHash": H32, "nonce": "0x" + "00" * 8,
        "transactions": [],
    }
    result, parent = {}, H32
    for number in range(first, last + 1):
        header = copy.deepcopy(template)
        header.update(number=hex(number), timestamp=hex(number), parentHash=parent)
        rehash(header)
        result[number] = header
        parent = header["hash"]
    return result


class BatchHeaders:
    """In-memory provider that cannot accidentally reach a real RPC endpoint."""

    def __init__(self, headers, fail_batch=None):
        self.headers, self.fail_batch = headers, fail_batch
        self.batches = []

    def execution_headers(self, numbers):
        self.batches.append(list(numbers))
        if len(self.batches) == self.fail_batch:
            raise relayer.RelayerError("injected interrupted header fetch")
        return [copy.deepcopy(self.headers[number]) for number in numbers]

    def call(self, method, params=()):
        raise AssertionError(f"unexpected RPC call: {method}")


class OfflineTestCase(unittest.TestCase):
    def setUp(self):
        # A missing per-test mock must fail immediately, never make a request.
        guard = patch.object(relayer.urllib.request, "urlopen",
                             side_effect=AssertionError("network access forbidden"))
        self.network_guard = guard.start()
        self.addCleanup(guard.stop)
        quiet = patch.object(relayer, "log")
        quiet.start()
        self.addCleanup(quiet.stop)


class HeaderBatchRpcTests(OfflineTestCase):
    def test_shuffled_replies_keep_requested_order_and_monotonic_ids(self):
        requests = []

        def exchange(request, timeout):
            payload = json.loads(request.data)
            requests.append(payload)
            self.assertEqual(timeout, 7)
            return response([rpc_result(item) for item in reversed(payload)])

        rpc = relayer.JsonRpc(RPC_URL, timeout=7)
        with patch.object(relayer.urllib.request, "urlopen", side_effect=exchange):
            first = rpc.execution_headers([5, 3, 4])
            second = rpc.execution_headers([8, 7])
        self.assertEqual([item["number"] for item in first], ["0x5", "0x3", "0x4"])
        self.assertEqual([item["number"] for item in second], ["0x8", "0x7"])
        self.assertEqual([item["id"] for item in requests[0]], [1, 2, 3])
        ids = [item["id"] for batch in requests for item in batch]
        self.assertEqual(ids, sorted(set(ids)))
        for batch, numbers in zip(requests, ([5, 3, 4], [8, 7])):
            for item, number in zip(batch, numbers):
                self.assertEqual(item["jsonrpc"], "2.0")
                self.assertEqual(item["method"], "eth_getBlockByNumber")
                self.assertEqual(item["params"], [hex(number), False])
                self.assertIs(item["params"][1], False)

    def test_invalid_batch_heights_fail_before_http(self):
        invalid = ([], list(range(17)), [1, 1], [True, 2], [False, 2],
                   [1.0, 2], ["1", 2], [-1, 0], [None, 2])
        for numbers in invalid:
            with self.subTest(numbers=numbers):
                with self.assertRaises(relayer.RelayerError):
                    relayer.JsonRpc(RPC_URL).execution_headers(numbers)
        self.network_guard.assert_not_called()

    def test_single_header_keeps_sequential_compatibility(self):
        requests = []

        def exchange(request, timeout):
            payload = json.loads(request.data)
            requests.append(payload)
            self.assertIsInstance(payload, dict)
            return response(rpc_result(payload))

        with patch.object(relayer.urllib.request, "urlopen", side_effect=exchange):
            result = relayer.JsonRpc(RPC_URL).execution_headers([0])
        self.assertEqual(result, [{"number": "0x0"}])
        self.assertEqual(requests[0]["params"], ["0x0", False])

    def test_malformed_batch_envelopes_do_not_trigger_fallback(self):
        good = [{"jsonrpc": "2.0", "id": i, "result": {"number": hex(i)}}
                for i in (1, 2)]
        cases = {
            "duplicate_id": [good[0], good[0]],
            "missing_id": [good[0]],
            "unexpected_id": [good[0], dict(good[1], id=3)],
            "boolean_id": [dict(good[0], id=True), good[1]],
            "string_id": [dict(good[0], id="1"), good[1]],
            "null_id": [dict(good[0], id=None), good[1]],
            "wrong_version": [dict(good[0], jsonrpc="1.0"), good[1]],
            "missing_version": [{"id": 1, "result": {}}, good[1]],
            "missing_result": [{"jsonrpc": "2.0", "id": 1}, good[1]],
            "non_object_item": [None, good[1]],
            "non_array_reply": {"jsonrpc": "2.0", "id": 1, "result": {}},
            "null_reply": None,
            "extra_response": good + [dict(good[1], id=3)],
        }
        for name, reply in cases.items():
            with self.subTest(case=name):
                with patch.object(relayer.urllib.request, "urlopen",
                                  return_value=response(reply)) as http:
                    with self.assertRaises(relayer.RelayerError):
                        relayer.JsonRpc(RPC_URL).execution_headers([1, 2])
                self.assertEqual(http.call_count, 1)

    def test_per_item_errors_never_retry_even_unsupported_method(self):
        for error in ({"code": -32600, "message": "invalid request"},
                      {"code": -32601, "message": "method unavailable"},
                      {"code": -32000, "message": "provider failure"},
                      "not an error object", ["not", "an", "object"]):
            with self.subTest(error=error):
                reply = [{"jsonrpc": "2.0", "id": 1, "error": error},
                         {"jsonrpc": "2.0", "id": 2, "result": {}}]
                with patch.object(relayer.urllib.request, "urlopen",
                                  return_value=response(reply)) as http:
                    with self.assertRaises(relayer.RelayerError):
                        relayer.JsonRpc(RPC_URL).execution_headers([1, 2])
                self.assertEqual(http.call_count, 1)

    def assert_fallback_is_sticky(self, refusal):
        requests = []
        if isinstance(refusal, urllib.error.HTTPError):
            self.addCleanup(refusal.close)

        def exchange(request, timeout):
            payload = json.loads(request.data)
            requests.append(payload)
            if isinstance(payload, list):
                self.assertEqual(len(requests), 1, "a refused batch must not be probed again")
                if isinstance(refusal, Exception):
                    raise refusal
                return response(refusal)
            return response(rpc_result(payload))

        rpc = relayer.JsonRpc(RPC_URL)
        with patch.object(relayer.urllib.request, "urlopen", side_effect=exchange):
            self.assertEqual(rpc.execution_headers([4, 3]),
                             [{"number": "0x4"}, {"number": "0x3"}])
            self.assertEqual(rpc.execution_headers([2, 1]),
                             [{"number": "0x2"}, {"number": "0x1"}])
        self.assertEqual(len(requests), 5)
        self.assertTrue(all(isinstance(item, dict) for item in requests[1:]))
        ids = [item["id"] for item in requests[0]] + [item["id"] for item in requests[1:]]
        self.assertEqual(ids, sorted(set(ids)))
        for item in requests[1:]:
            self.assertEqual(item["method"], "eth_getBlockByNumber")
            self.assertIs(item["params"][1], False)

    def test_explicit_top_level_unsupported_batch_falls_back_once(self):
        for code in (-32600, -32601):
            with self.subTest(code=code):
                self.assert_fallback_is_sticky(
                    {"jsonrpc": "2.0", "id": None,
                     "error": {"code": code, "message": "batch unsupported"}})

    def test_malformed_whole_request_refusals_never_trigger_fallback(self):
        for code in (-32600, -32601):
            valid = {"jsonrpc": "2.0", "id": None,
                     "error": {"code": code, "message": "batch unsupported"}}
            cases = {
                "missing_version": {"id": None, "error": valid["error"]},
                "wrong_version": dict(valid, jsonrpc="1.0"),
                "missing_id": {"jsonrpc": "2.0", "error": valid["error"]},
                "expected_item_id": dict(valid, id=1),
                "unexpected_id": dict(valid, id=99),
                "boolean_id": dict(valid, id=True),
                "string_id": dict(valid, id="1"),
                "result_present": dict(valid, result=None),
                "missing_code": dict(valid, error={"message": "batch unsupported"}),
                "float_code": dict(valid, error={"code": float(code), "message": "unsupported"}),
                "boolean_code": dict(valid, error={"code": True, "message": "unsupported"}),
                "string_code": dict(valid, error={"code": str(code), "message": "unsupported"}),
                "missing_message": dict(valid, error={"code": code}),
                "null_message": dict(valid, error={"code": code, "message": None}),
                "numeric_message": dict(valid, error={"code": code, "message": 1}),
                "non_object_error": dict(valid, error="batch unsupported"),
            }
            for name, reply in cases.items():
                with self.subTest(code=code, case=name):
                    with patch.object(relayer.urllib.request, "urlopen",
                                      return_value=response(reply)) as http:
                        with self.assertRaises(relayer.RelayerError):
                            relayer.JsonRpc(RPC_URL).execution_headers([2, 1])
                    self.assertEqual(http.call_count, 1)

    def test_unstructured_unsupported_http_status_falls_back_once(self):
        for status in (400, 405, 413, 415, 422, 501):
            with self.subTest(status=status):
                self.assert_fallback_is_sticky(urllib.error.HTTPError(
                    RPC_URL, status, "unsupported", {}, io.BytesIO(b"batch not supported")))

    def test_structured_other_error_on_unsupported_status_does_not_fallback(self):
        payload = {"jsonrpc": "2.0", "id": None,
                   "error": {"code": -32000, "message": "temporary failure"}}
        error = urllib.error.HTTPError(RPC_URL, 400, "bad request", {}, response(payload))
        self.addCleanup(error.close)
        with patch.object(relayer.urllib.request, "urlopen", side_effect=error) as http:
            with self.assertRaises(relayer.RpcError) as raised:
                relayer.JsonRpc(RPC_URL).execution_headers([2, 1])
        self.assertEqual(raised.exception.code, -32000)
        self.assertEqual(http.call_count, 1)

    def test_rate_limit_server_errors_and_transport_never_retry(self):
        errors = [TimeoutError("offline timeout"), OSError("offline disconnect"),
                  urllib.error.URLError("offline network error")]
        # Even a misleading unsupported-batch body on HTTP 429 must not fan out.
        unsupported = {"error": {"code": -32600, "message": "batch unsupported"}}
        errors.extend(urllib.error.HTTPError(RPC_URL, status, "unavailable", {},
                                            response(unsupported))
                      for status in (429, 500, 502, 503))
        for error in errors:
            if isinstance(error, urllib.error.HTTPError):
                self.addCleanup(error.close)
            with self.subTest(error=str(error)):
                with patch.object(relayer.urllib.request, "urlopen", side_effect=error) as http:
                    with self.assertRaises(relayer.RelayerError):
                        relayer.JsonRpc(RPC_URL).execution_headers([2, 1])
                self.assertEqual(http.call_count, 1)

    def test_invalid_json_and_oversized_body_do_not_retry(self):
        for raw in (b"not json", b"[" + b" " * 100):
            with self.subTest(raw_length=len(raw)):
                with patch.object(relayer, "MAX_HEADER_BATCH_RESPONSE_BYTES", 64):
                    with patch.object(relayer.urllib.request, "urlopen",
                                      return_value=io.BytesIO(raw)) as http:
                        with self.assertRaises(relayer.RelayerError):
                            relayer.JsonRpc(RPC_URL).execution_headers([2, 1])
                self.assertEqual(http.call_count, 1)


class HeaderCacheTests(OfflineTestCase):
    @classmethod
    def setUpClass(cls):
        cls.first, cls.last = 50, 69
        cls.headers = empty_bloom_chain(cls.first, cls.last)

    def setUp(self):
        super().setUp()
        temporary = tempfile.TemporaryDirectory(prefix="b3-header-cache-test-")
        self.addCleanup(temporary.cleanup)
        self.path = Path(temporary.name) / "state.sqlite"
        self.state = relayer.State(self.path)
        self.state.bind({"bridge": "offline-header-cache-test"}, self.first)
        self.addCleanup(lambda: self.state.close())

    def verify(self, eth, anchor=None, first=None):
        return relayer.verified_execution_headers(
            eth, self.first if first is None else first, self.last,
            self.headers[self.last]["hash"] if anchor is None else anchor,
            relayer.MAX_ANCESTRY_DISTANCE, header_cache=self.state)

    def scan(self, eth, anchor=None, before_commit=None):
        return relayer.scan_finalized(
            self.state, eth, {"vault": H20, "token": TOKEN}, self.last,
            self.headers[self.last]["hash"] if anchor is None else anchor,
            chunk=7, max_ancestry=relayer.MAX_ANCESTRY_DISTANCE,
            plan_deposit=lambda deposit: self.fail("empty bloom cannot create a deposit"),
            before_commit=before_commit)

    def no_rpc(self):
        return FakeRpc({"eth_getBlockByNumber": AssertionError("cache must avoid RPC")})

    def cache_chain(self):
        self.state.cache_execution_headers(
            [self.headers[number] for number in range(self.last, self.first - 1, -1)])

    def replace_payload(self, block_hash, payload, encoded_size):
        self.state.db.execute(
            "UPDATE execution_headers SET payload=?,encoded_size=? WHERE block_hash=?",
            (payload, encoded_size, block_hash))

    def test_safety_limits_are_bounded(self):
        self.assertEqual(relayer.HEADER_BATCH_SIZE, 16)
        self.assertEqual(relayer.MAX_HEADER_CACHE_ENTRIES, 20001)
        self.assertEqual(relayer.MAX_HEADER_CACHE_BYTES, 64 * 1024 * 1024)
        self.assertEqual(relayer.MAX_CACHED_HEADER_BYTES, 1024 * 1024)

    def test_verified_batches_are_descending_bounded_and_do_not_advance_cursor(self):
        eth = BatchHeaders(self.headers)
        self.assertEqual(self.verify(eth), self.headers)
        self.assertEqual(eth.batches, [list(range(69, 53, -1)), list(range(53, 49, -1))])
        self.assertTrue(all(1 <= len(batch) <= 16 for batch in eth.batches))
        self.assertEqual(self.state.cursor(), self.first)
        for header in self.headers.values():
            self.assertEqual(self.state.cached_execution_header(header["hash"]), header)
        eth = self.no_rpc()
        self.assertEqual(self.verify(eth), self.headers)
        self.assertEqual(eth.calls, [])
        self.assertEqual(self.state.cursor(), self.first)

    def test_production_client_scans_in_three_header_batches(self):
        batches = []

        def exchange(request, timeout):
            payload = json.loads(request.data)
            self.assertIsInstance(payload, list)
            numbers = [int(item["params"][0], 16) for item in payload]
            batches.append(numbers)
            return response([{"jsonrpc": "2.0", "id": item["id"],
                              "result": self.headers[int(item["params"][0], 16)]}
                             for item in reversed(payload)])

        rpc = relayer.JsonRpc(RPC_URL)
        self.assertEqual(rpc.execution_header_batch_size, 3)
        with patch.object(relayer.urllib.request, "urlopen", side_effect=exchange):
            self.assertEqual(self.verify(rpc), self.headers)
        self.assertEqual([len(batch) for batch in batches], [3, 3, 3, 3, 3, 3, 2])
        self.assertEqual([number for batch in batches for number in batch],
                         list(range(self.last, self.first - 1, -1)))
        self.assertEqual(relayer.HEADER_BATCH_SIZE, 16)
        # The same production client must perform no HTTP at all on a warm replay.
        self.assertEqual(self.verify(rpc), self.headers)
        self.network_guard.assert_not_called()
        self.assertEqual(self.state.cursor(), self.first)

    def test_invalid_client_batch_hints_fail_before_fetch_or_cursor_advance(self):
        for hint in (None, False, True, -1, 0, 17, 3.0, "3"):
            with self.subTest(hint=hint):
                eth = BatchHeaders(self.headers)
                eth.execution_header_batch_size = hint
                with self.assertRaisesRegex(relayer.RelayerError, "batch size"):
                    self.verify(eth)
                self.assertEqual(eth.batches, [])
                self.assertEqual(self.state.cursor(), self.first)
        self.network_guard.assert_not_called()

    def test_cache_survives_reopening_and_preserves_full_public_json(self):
        headers = copy.deepcopy(self.headers)
        headers[self.last]["public_fixture_metadata"] = {"provider_fields": [1, "full JSON"]}
        self.verify(BatchHeaders(headers))
        self.state.close()
        self.state = relayer.State(self.path)
        self.state.bind({"bridge": "offline-header-cache-test"}, self.first)
        eth = self.no_rpc()
        self.assertEqual(self.verify(eth), headers)
        self.assertEqual(eth.calls, [])
        self.assertEqual(self.state.cursor(), self.first)

    def test_stored_payload_is_compressed_complete_json_with_decoded_size(self):
        self.cache_chain()
        header = self.headers[self.last]
        row = self.state.db.execute(
            "SELECT block_number,payload,encoded_size FROM execution_headers WHERE block_hash=?",
            (header["hash"],)).fetchone()
        encoded = zlib.decompress(row["payload"])
        self.assertEqual(row["block_number"], self.last)
        self.assertEqual(row["encoded_size"], len(encoded))
        self.assertLess(len(row["payload"]), len(encoded))
        self.assertEqual(encoded, json.dumps(header, sort_keys=True, separators=(",", ":")).encode())
        self.assertEqual(json.loads(encoded), header)
        self.assertEqual(self.state.cursor(), self.first)

    def test_legacy_provider_without_batch_method_remains_compatible(self):
        eth = FakeRpc({"eth_getBlockByNumber": lambda params:
                       copy.deepcopy(self.headers[int(params[0], 16)])})
        self.assertEqual(self.verify(eth), self.headers)
        self.assertEqual(eth.calls, [("eth_getBlockByNumber", [hex(number), False])
                                    for number in range(self.last, self.first - 1, -1)])

    def test_interrupted_fetch_keeps_verified_prefix_for_resume(self):
        eth = BatchHeaders(self.headers, fail_batch=2)
        with self.assertRaisesRegex(relayer.RelayerError, "interrupted"):
            self.scan(eth)
        self.assertEqual(self.state.cursor(), self.first)
        for number in range(54, 70):
            self.assertEqual(self.state.cached_execution_header(self.headers[number]["hash"]),
                             self.headers[number])
        self.assertIsNone(self.state.cached_execution_header(self.headers[53]["hash"]))
        resumed = BatchHeaders(self.headers)
        self.scan(resumed)
        self.assertEqual(resumed.batches, [list(range(53, 49, -1))])
        self.assertEqual(self.state.cursor(), self.last + 1)

    def test_interruption_before_forward_scan_reuses_cache_without_skipping_cursor(self):
        def interrupt():
            raise relayer.RelayerError("injected failure before cursor commit")

        with self.assertRaisesRegex(relayer.RelayerError, "before cursor commit"):
            self.scan(BatchHeaders(self.headers), before_commit=interrupt)
        self.assertEqual(self.state.cursor(), self.first)
        self.assertEqual(self.state.db.execute("SELECT COUNT(*) FROM deposits").fetchone()[0], 0)
        eth = self.no_rpc()
        self.scan(eth)
        self.assertEqual(eth.calls, [])
        self.assertEqual(self.state.cursor(), self.last + 1)

    def test_valid_rlp_with_broken_parent_link_saves_only_anchor_side_prefix(self):
        broken = copy.deepcopy(self.headers)
        cut = self.last - 4
        for number in range(cut, self.last + 1):
            broken[number]["parentHash"] = H32 if number == cut else broken[number - 1]["hash"]
            rehash(broken[number])
        eth = BatchHeaders(broken)
        with self.assertRaisesRegex(relayer.RelayerError, "header mismatch"):
            self.scan(eth, anchor=broken[self.last]["hash"])
        self.assertEqual(self.state.cursor(), self.first)
        self.assertEqual(len(eth.batches), 1)
        for number in range(cut, self.last + 1):
            self.assertEqual(self.state.cached_execution_header(broken[number]["hash"]), broken[number])
        self.assertIsNone(self.state.cached_execution_header(broken[cut - 1]["hash"]))
        resumed = BatchHeaders(broken)
        with self.assertRaises(relayer.RelayerError):
            self.scan(resumed, anchor=broken[self.last]["hash"])
        self.assertEqual(resumed.batches[0][0], cut - 1)
        self.assertEqual(self.state.cursor(), self.first)

    def test_cached_old_anchor_never_replaces_current_authenticated_anchor(self):
        self.cache_chain()
        eth = BatchHeaders(self.headers)
        with self.assertRaisesRegex(relayer.RelayerError, "header mismatch"):
            self.scan(eth, anchor=H32)
        self.assertEqual(eth.batches[0][0], self.last)
        self.assertEqual(self.state.cursor(), self.first)

    def test_corrupt_compression_fails_closed_without_rpc_or_cursor_advance(self):
        self.cache_chain()
        header = self.headers[self.last]
        encoded = json.dumps(header).encode()
        corruptions = (
            (b"not zlib", len(encoded)),
            (zlib.compress(encoded)[:-1], len(encoded)),
            (zlib.compress(encoded) + b"trailing data", len(encoded)),
            (zlib.compress(encoded), len(encoded) + 1),
            (zlib.compress(b"[]"), 2),
            (zlib.compress(b"not json"), 8),
            (zlib.compress(b" " * (relayer.MAX_CACHED_HEADER_BYTES + 1)), 1),
        )
        for payload, encoded_size in corruptions:
            with self.subTest(payload_size=len(payload), encoded_size=encoded_size):
                self.replace_payload(header["hash"], payload, encoded_size)
                eth = self.no_rpc()
                with self.assertRaisesRegex(relayer.RelayerError, "corrupt execution header cache"):
                    self.scan(eth)
                self.assertEqual(eth.calls, [])
                self.assertEqual(self.state.cursor(), self.first)

    def test_cached_header_fields_are_reauthenticated_not_trusted(self):
        self.cache_chain()
        header = self.headers[self.last]
        changes = ({"number": hex(self.last - 1)}, {"number": None},
                   {"hash": H32}, {"parentHash": H32}, {"gasUsed": "0x1"},
                   {"logsBloom": "0x00"}, {"transactions": None})
        for change in changes:
            with self.subTest(change=change):
                altered = dict(header, **change)
                encoded = json.dumps(altered).encode()
                self.replace_payload(header["hash"], zlib.compress(encoded), len(encoded))
                eth = self.no_rpc()
                with self.assertRaises(relayer.RelayerError):
                    self.scan(eth)
                self.assertEqual(eth.calls, [])
                self.assertEqual(self.state.cursor(), self.first)

    def test_reinsertion_does_not_silently_replace_corrupt_cached_data(self):
        self.cache_chain()
        header = self.headers[self.last]
        self.replace_payload(header["hash"], b"corrupt stored payload", 123)
        self.cache_chain()
        eth = self.no_rpc()
        with self.assertRaisesRegex(relayer.RelayerError, "corrupt execution header cache"):
            self.scan(eth)
        self.assertEqual(eth.calls, [])
        self.assertEqual(self.state.cursor(), self.first)

    def test_cache_eviction_obeys_row_limit_and_keeps_highest_headers(self):
        with patch.object(relayer, "MAX_HEADER_CACHE_ENTRIES", 5):
            self.cache_chain()
        rows = self.state.db.execute(
            "SELECT block_number FROM execution_headers ORDER BY block_number").fetchall()
        self.assertEqual([row[0] for row in rows], list(range(self.last - 4, self.last + 1)))
        self.assertEqual(self.state.cursor(), self.first)

    def test_cache_eviction_obeys_compressed_byte_limit(self):
        self.cache_chain()
        quota = self.state.db.execute(
            "SELECT SUM(length(payload)) FROM execution_headers WHERE block_number>?",
            (self.last - 3,)).fetchone()[0]
        with patch.object(relayer, "MAX_HEADER_CACHE_BYTES", quota):
            self.cache_chain()
        count, size = self.state.db.execute(
            "SELECT COUNT(*),SUM(length(payload)) FROM execution_headers").fetchone()
        self.assertEqual(count, 3)
        self.assertLessEqual(size, quota)
        self.assertEqual(self.state.cursor(), self.first)

    def test_repeated_insert_deduplicates_and_oversized_header_is_not_cached(self):
        self.cache_chain()
        self.cache_chain()
        self.assertEqual(self.state.db.execute("SELECT COUNT(*) FROM execution_headers").fetchone()[0],
                         len(self.headers))
        oversized = copy.deepcopy(self.headers[self.last])
        oversized["number"] = hex(self.last + 1)
        oversized["parentHash"] = self.headers[self.last]["hash"]
        oversized["large_public_field"] = "x" * relayer.MAX_CACHED_HEADER_BYTES
        rehash(oversized)
        self.state.cache_execution_headers([oversized])
        self.assertIsNone(self.state.cached_execution_header(oversized["hash"]))
        self.assertEqual(self.state.cursor(), self.first)

    def test_ancestry_limit_fails_before_rpc_and_cannot_be_raised_past_hard_limit(self):
        eth = self.no_rpc()
        with self.assertRaises(relayer.RelayerError):
            relayer.verified_execution_headers(eth, self.first, self.last,
                                               self.headers[self.last]["hash"], 1,
                                               header_cache=self.state)
        with self.assertRaises(relayer.RelayerError):
            relayer.verified_execution_headers(eth, 0, relayer.MAX_ANCESTRY_DISTANCE + 1,
                                               H32, relayer.MAX_ANCESTRY_DISTANCE + 10,
                                               header_cache=self.state)
        self.assertEqual(eth.calls, [])
        self.assertEqual(self.state.cursor(), self.first)


if __name__ == "__main__":
    unittest.main()
