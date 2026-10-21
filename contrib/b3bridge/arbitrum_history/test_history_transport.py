import contextlib
import gzip
import http.server
import os
from pathlib import Path
import socket
import socketserver
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import acquire_history as module
import history_store
from acquire_history import PREFIX, RawObjectClient, NitroFeedBacklogClient
from feed_history import MAX_FRAME
from history_store import HistoryError, HistoryStore, default_policy, digest
from test_feed_history import frame, message


class Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


@contextlib.contextmanager
def raw_server(responder):
    class Handler(socketserver.BaseRequestHandler):
        def handle(self):
            self.request.settimeout(2)
            data = bytearray()
            while b"\r\n\r\n" not in data and len(data) <= 8192:
                block = self.request.recv(1024)
                if not block:
                    return
                data.extend(block)
            try:
                responder(self.request, bytes(data))
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass  # An adversarial response is expected to be disconnected.
    server = Server(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
    thread.start()
    try:
        yield "http://127.0.0.1:" + str(server.server_address[1])
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def response(raw, *, status="200 OK", headers=None):
    values = [("Content-Type", "application/octet-stream"), ("Content-Length", str(len(raw)))]
    if headers is not None:
        values = headers
    return ("HTTP/1.1 " + status + "\r\n" +
            "".join(key + ": " + value + "\r\n" for key, value in values) +
            "Connection: close\r\n\r\n").encode() + raw


class RawHistoryTransportTests(unittest.TestCase):
    def test_actual_raw_socket_bytes_hash_and_exact_get_only(self):
        raw, requests = b"\x00\xffopaque-rlp-not-JSON-RPC" * 2000, []
        def serve(sock, request):
            requests.append(request)
            payload = response(raw)
            for position in range(0, len(payload), 997):
                sock.sendall(payload[position:position + 997])
        with raw_server(serve) as origin:
            with patch.dict(os.environ, {"HTTP_PROXY": "http://127.0.0.1:1",
                                         "HTTPS_PROXY": "http://127.0.0.1:1"}):
                client = RawObjectClient(origin, allow_loopback=True)
                self.assertEqual(client.fetch(digest(raw), len(raw)), raw)
        self.assertEqual(len(requests), 1)
        self.assertTrue(requests[0].startswith(("GET " + PREFIX + digest(raw) + " HTTP/1.1\r\n").encode()))
        self.assertIn(b"Accept: application/octet-stream\r\n", requests[0])
        self.assertNotIn(b"Authorization", requests[0])
        self.assertNotIn(b"jsonrpc", requests[0])

    def test_redirect_error_or_rpc_content_are_never_followed(self):
        for status, headers in (("302 Found", [("Location", "http://127.0.0.1:1/secret")]),
                                ("404 Not Found", []),
                                ("200 OK", [("Content-Type", "application/json"), ("Content-Length", "3")])):
            requests = []
            def serve(sock, request):
                requests.append(request)
                sock.sendall(response(b"abc", status=status, headers=headers))
            with self.subTest(status=status, headers=headers), raw_server(serve) as origin:
                with self.assertRaises(HistoryError):
                    RawObjectClient(origin, allow_loopback=True).fetch(digest(b"abc"), 3)
            self.assertEqual(len(requests), 1)

    def test_wrong_hash_length_duplicate_framing_and_encoding_are_refused(self):
        base = [("Content-Type", "application/octet-stream"), ("Content-Length", "3")]
        cases = [(b"abd", base), (b"abc", base + [("Content-Length", "3")]),
                 (b"abc", [("Content-Type", "application/octet-stream"), ("Content-Length", "4")]),
                 (b"abc", [("Content-Type", "application/octet-stream"), ("Content-Length", "03")]),
                 (b"abc", base + [("Transfer-Encoding", "chunked")]),
                 (b"abc", base + [("Content-Encoding", "gzip")]),
                 (b"abc", base + [("Trailer", "Digest")]),
                 (b"abc", base + [("Content-Type", "application/octet-stream")])]
        for raw, headers in cases:
            with self.subTest(raw=raw, headers=headers):
                with raw_server(lambda sock, req: sock.sendall(response(raw, headers=headers))) as origin:
                    with self.assertRaises(HistoryError):
                        RawObjectClient(origin, allow_loopback=True).fetch(digest(b"abc"), 3)

    def test_partial_body_and_zero_length_refused(self):
        with raw_server(lambda sock, req: sock.sendall(response(b"a", headers=[
                ("Content-Type", "application/octet-stream"), ("Content-Length", "3")]))) as origin:
            with self.assertRaises(HistoryError):
                RawObjectClient(origin, allow_loopback=True).fetch(digest(b"abc"), 3)
        with self.assertRaises(HistoryError):
            RawObjectClient("http://127.0.0.1:1", allow_loopback=True).fetch(digest(b""), 0)

    def test_slow_body_cannot_extend_total_deadline(self):
        def serve(sock, request):
            sock.sendall(response(b"", headers=[("Content-Type", "application/octet-stream"),
                                                ("Content-Length", "10")]))
            for _ in range(10):
                time.sleep(0.07)
                sock.sendall(b"a")
        with raw_server(serve) as origin:
            started = time.monotonic()
            with self.assertRaises(HistoryError):
                RawObjectClient(origin, allow_loopback=True, timeout=0.2).fetch(digest(b"a" * 10), 10)
            self.assertLess(time.monotonic() - started, 0.65)

    def test_slow_headers_cannot_extend_total_deadline(self):
        def serve(sock, request):
            sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: ")
            for byte in b"application/octet-stream\r\n":
                time.sleep(0.03)
                sock.sendall(bytes([byte]))
        with raw_server(serve) as origin:
            started = time.monotonic()
            with self.assertRaises(HistoryError):
                RawObjectClient(origin, allow_loopback=True, timeout=0.2).fetch(digest(b"abc"), 3)
            self.assertLess(time.monotonic() - started, 0.65)

    def test_public_origin_and_loopback_exception_are_explicit(self):
        for origin in ("http://example.org", "http://127.0.0.1:1234", "http://localhost:1",
                       "https://user:secret@example.org", "https://example.org/path",
                       "https://example.org?key=secret", "https://example.org#secret",
                       "https://example.org\r\nHost: x", "file:///tmp/secret"):
            with self.subTest(origin=origin), self.assertRaises(HistoryError):
                RawObjectClient(origin)
        with self.assertRaises(HistoryError):
            RawObjectClient("http://localhost:1", allow_loopback=True)
        self.assertTrue(RawObjectClient("http://127.0.0.1:1234", allow_loopback=True).fixture)

    def test_dns_rebinding_private_mixed_and_multicast_answers_refuse_before_socket(self):
        client = RawObjectClient("https://history.example")
        for answers in (["127.0.0.1"], ["10.0.0.1"], ["169.254.169.254"],
                        ["224.0.0.1"], ["93.184.216.34", "127.0.0.1"], ["::1"], ["fe80::1"]):
            values = [(socket.AF_INET6 if ":" in ip else socket.AF_INET, socket.SOCK_STREAM,
                       0, "", (ip, 443)) for ip in answers]
            with self.subTest(answers=answers), patch.object(socket, "getaddrinfo", return_value=values), \
                    patch.object(socket, "socket", side_effect=AssertionError("must refuse before connect")):
                with self.assertRaises(HistoryError):
                    client.fetch(digest(b"abc"), 3)

    def test_dns_timeout_is_bounded_and_client_fences_without_more_threads(self):
        release = threading.Event()
        calls = []
        def slow(*args, **kwargs):
            calls.append(True)
            release.wait(2)
            return []
        client = RawObjectClient("https://history.example", timeout=0.05)
        try:
            with patch.object(socket, "getaddrinfo", side_effect=slow):
                with self.assertRaises(HistoryError):
                    client.fetch(digest(b"abc"), 3)
                with self.assertRaises(HistoryError):
                    client.fetch(digest(b"abc"), 3)
                self.assertEqual(len(calls), 1)
        finally:
            release.set()

    def test_unavailable_address_falls_back_without_reresolution_or_deadline_reset(self):
        raw, requests = b"raw-object", []
        def serve(sock, request):
            requests.append(request)
            sock.sendall(response(raw))
        with raw_server(serve) as origin:
            client = RawObjectClient(origin, allow_loopback=True, timeout=.5)
            # The first real TCP attempt cannot connect to the server's IPv6
            # loopback; its listening socket is IPv4 only. The second can.
            answers = [(socket.AF_INET6, ("::1", client.port, 0, 0)),
                       (socket.AF_INET, ("127.0.0.1", client.port))]
            with patch.object(client, "_addresses", return_value=answers) as resolve:
                started = time.monotonic()
                self.assertEqual(client.fetch(digest(raw), len(raw)), raw)
                self.assertLess(time.monotonic() - started, .5)
                resolve.assert_called_once()
            self.assertEqual(len(requests), 1)

    def test_invalid_hash_size_or_concurrent_fetch_never_opens_socket(self):
        client = RawObjectClient("http://127.0.0.1:1", allow_loopback=True)
        with patch.object(socket, "socket", side_effect=AssertionError("must refuse before connect")):
            for sha, size in (("../secret", 3), ("ab" * 32, 0), ("ab" * 32, True),
                              ("ab" * 32, 16 * 1024 * 1024 + 1)):
                with self.assertRaises(HistoryError):
                    client.fetch(sha, size)
            client._fetch_lock.acquire()
            try:
                with self.assertRaises(HistoryError):
                    client.fetch(digest(b"abc"), 3)
            finally:
                client._fetch_lock.release()

    @unittest.skipUnless(history_store.fcntl is not None, "Actual durable store requires Linux")
    def test_real_acquire_is_durable_retry_is_local_and_bad_payload_never_commits(self):
        raw, requests = b"opaque raw receipt", []
        def serve(sock, request):
            requests.append(request)
            sock.sendall(response(raw))
        with tempfile.TemporaryDirectory() as directory, raw_server(serve) as origin:
            path = Path(directory) / "store"
            policy = default_policy()
            with HistoryStore(path, policy, create=True) as store:
                client = RawObjectClient(origin, allow_loopback=True)
                descriptor = client.acquire(store, "l2_receipt", digest(raw), len(raw))
                self.assertEqual(client.acquire(store, "l2_receipt", digest(raw), len(raw)), descriptor)
                self.assertEqual(len(requests), 1)
                before = store.stats()
                with self.assertRaises(HistoryError):
                    client.acquire(store, "l2_receipt", digest(b"wrong payload"), len(raw))
                self.assertEqual(store.stats(), before)
                self.assertFalse(store.stats()["arbitrum_authority"])
            with HistoryStore(path, policy) as store:
                self.assertEqual(store.get(descriptor["sha256"]), raw)

    @unittest.skipUnless(history_store.fcntl is not None, "Actual durable store requires Linux")
    def test_store_quota_or_unknown_kind_refuse_before_network(self):
        with tempfile.TemporaryDirectory() as directory:
            policy = default_policy()
            policy.update(max_object_bytes=4, max_total_bytes=4, max_objects=1)
            with HistoryStore(Path(directory) / "store", policy, create=True) as store:
                client = RawObjectClient("http://127.0.0.1:1", allow_loopback=True)
                with patch.object(client, "fetch", side_effect=AssertionError("no fetch")):
                    with self.assertRaises(HistoryError):
                        client.acquire(store, "unknown", digest(b"a"), 1)
                    with self.assertRaises(HistoryError):
                        client.acquire(store, "mpt_node", digest(b"abcde"), 5)
                store.put("mpt_node", b"abcd", {"transport": "synthetic-adversarial-fixture/1", "peer_id": "fixture"})
                with patch.object(client, "fetch", side_effect=AssertionError("no fetch")):
                    with self.assertRaises(HistoryError):
                        client.acquire(store, "mpt_node", digest(b"e"), 1)


class NitroBacklogTests(unittest.TestCase):
    def test_known_info_and_exact_gzip_chunk_actual_upstream_paths(self):
        raw = frame(message(10), message(11))
        compressed, requests = gzip.compress(raw), []
        def serve(sock, request):
            requests.append(request)
            if request.startswith(b"GET /feed/v1/info "):
                body = b'{"chainId":42161,"feedVersion":1,"chunkSize":2}'
                headers = [("Content-Type", "application/json"), ("Content-Length", str(len(body)))]
            else:
                body = compressed
                headers = [("Content-Type", "application/json"), ("Content-Length", str(len(body))),
                           ("Content-Encoding", "gzip")]
            sock.sendall(response(body, headers=headers))
        with raw_server(serve) as origin:
            client = NitroFeedBacklogClient(origin, allow_loopback=True)
            self.assertEqual(client.fetch_info(), {"chainId": 42161, "feedVersion": 1, "chunkSize": 2})
            self.assertEqual(client.fetch_chunk(10, 2), raw)
        self.assertTrue(requests[0].startswith(b"GET /feed/v1/info HTTP/1.1\r\n"))
        self.assertTrue(requests[1].startswith(b"GET /feed/v1/chunk/10 HTTP/1.1\r\n"))
        self.assertIn(b"Accept-Encoding: gzip\r\n", requests[1])

    def test_feed_info_chain_version_count_and_hostile_json_refuse(self):
        cases = [b'{"chainId":1,"feedVersion":1,"chunkSize":2}',
                 b'{"chainId":42161,"feedVersion":2,"chunkSize":2}',
                 b'{"chainId":42161,"feedVersion":1,"chunkSize":0}',
                 b'{"chainId":42161,"feedVersion":1,"chunkSize":10001}',
                 b'{"chainId":42161,"feedVersion":1,"chunkSize":true}',
                 b'{"chainId":42161,"feedVersion":1,"chunkSize":2,"verified":true}',
                 b'{"chainId":42161,"chainId":1,"feedVersion":1,"chunkSize":2}',
                 b"[" * 2000 + b"0" + b"]" * 2000]
        for body in cases:
            with self.subTest(body=body[:100]):
                headers = [("Content-Type", "application/json"), ("Content-Length", str(len(body)))]
                with raw_server(lambda sock, req: sock.sendall(response(body, headers=headers))) as origin:
                    with self.assertRaises(HistoryError):
                        NitroFeedBacklogClient(origin, allow_loopback=True).fetch_info()

    def test_gzip_bomb_concatenated_trailing_and_truncated_chunks_refuse(self):
        valid = gzip.compress(frame(message(0)))
        cases = [gzip.compress(b"a" * (MAX_FRAME + 1)), valid + valid, valid + b"trailing",
                 valid[:-3], b"not-gzip"]
        for body in cases:
            headers = [("Content-Type", "application/json"), ("Content-Length", str(len(body))),
                       ("Content-Encoding", "gzip")]
            with self.subTest(length=len(body)), raw_server(lambda sock, req: sock.sendall(response(body, headers=headers))) as origin:
                with self.assertRaises(HistoryError):
                    NitroFeedBacklogClient(origin, allow_loopback=True).fetch_chunk(0, 1)

    def test_missing_reordered_wrong_range_and_unaligned_chunks_refuse(self):
        cases = [frame(message(10)), frame(message(10), message(12)),
                 frame(message(12), message(13)), frame(message(10), message(11), confirmed=100)]
        for raw in cases:
            body = gzip.compress(raw)
            headers = [("Content-Type", "application/json"), ("Content-Length", str(len(body))),
                       ("Content-Encoding", "gzip")]
            with self.subTest(raw=raw[:80]), raw_server(lambda sock, req: sock.sendall(response(body, headers=headers))) as origin:
                with self.assertRaises(HistoryError):
                    NitroFeedBacklogClient(origin, allow_loopback=True).fetch_chunk(10, 2)
        client = NitroFeedBacklogClient("http://127.0.0.1:1", allow_loopback=True)
        with patch.object(client, "_exchange", side_effect=AssertionError("no network")):
            for start, count in ((1, 2), (-1, 2), (0, 10001), (True, 1)):
                with self.assertRaises(HistoryError):
                    client.fetch_chunk(start, count)

    def test_standard_chunked_gzip_is_bounded_and_validated(self):
        raw, requests = frame(message(0)), []
        body = gzip.compress(raw)
        def serve(sock, request):
            requests.append(request)
            sock.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\n"
                         b"Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
            for offset in range(0, len(body), 7):
                chunk = body[offset:offset + 7]
                sock.sendall((f"{len(chunk):x}\r\n").encode() + chunk + b"\r\n")
            sock.sendall(b"0\r\n\r\n")
        with raw_server(serve) as origin:
            self.assertEqual(NitroFeedBacklogClient(origin, allow_loopback=True).fetch_chunk(0, 1), raw)

    @unittest.skipUnless(history_store.fcntl is not None, "Actual durable store requires Linux")
    def test_actual_backlog_acquire_checks_info_and_stores_soft_bytes_only(self):
        raw = frame(message(0))
        def serve(sock, request):
            is_info = request.startswith(b"GET /feed/v1/info ")
            body = b'{"chainId":42161,"feedVersion":1,"chunkSize":1}' if is_info else gzip.compress(raw)
            headers = [("Content-Type", "application/json"), ("Content-Length", str(len(body)))]
            if not is_info:
                headers.append(("Content-Encoding", "gzip"))
            sock.sendall(response(body, headers=headers))
        with tempfile.TemporaryDirectory() as directory, raw_server(serve) as origin:
            with HistoryStore(Path(directory) / "store", default_policy(), create=True) as store:
                client = NitroFeedBacklogClient(origin, allow_loopback=True)
                result = client.acquire_chunk(store, 0, 1)
                self.assertEqual(store.get(result["object"]["sha256"]), raw)
                self.assertTrue(result["soft_only"])
                self.assertFalse(result["native_execution_verified"])
                self.assertFalse(result["arbitrum_authority"])
                before = store.stats()
                with self.assertRaises(HistoryError):
                    client.acquire_chunk(store, 0, 2)
                self.assertEqual(store.stats(), before)


if __name__ == "__main__":
    unittest.main()
