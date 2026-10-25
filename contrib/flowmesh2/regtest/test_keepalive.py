#!/usr/bin/env python3
"""The loopback RPC clients reuse one connection without weakening any check."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parent))
from custody import CustodyError, LocalRpc, RpcError  # noqa: E402
from keepalive import KeepAliveOpener  # noqa: E402
from local_process import B3Rpc  # noqa: E402


class Server:
    """Answers each JSON-RPC request with `answer(payload)` and records the client port."""

    def __init__(self):
        self.ports, self.payloads, self.headers = [], [], []
        self.status, self.close, self.pad = 200, False, 0
        owner = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = 'HTTP/1.1'

            def log_message(self, *args):
                pass

            def do_POST(self):
                payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                owner.ports.append(self.client_address[1])
                owner.payloads.append(payload)
                owner.headers.append(dict(self.headers))
                calls = payload if isinstance(payload, list) else [payload]
                replies = [{'jsonrpc': '2.0', 'id': call['id'], 'result': '0x1' + '0' * owner.pad} for call in calls]
                body = json.dumps(replies if isinstance(payload, list) else replies[0]).encode()
                self.send_response(owner.status)
                self.send_header('Content-Type', 'application/json')
                self.send_header('Content-Length', str(len(body)))
                if owner.close:
                    self.send_header('Connection', 'close')
                self.end_headers()
                self.wfile.write(body)

        self.httpd = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.httpd.daemon_threads = True
        self.url = 'http://127.0.0.1:%d' % self.httpd.server_address[1]
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)
        self.thread.start()

    def stop(self):
        self.httpd.shutdown()
        self.httpd.server_close()

    def connections(self):
        return len(set(self.ports))


class Clock:
    def __init__(self):
        self.now = 100.0

    def __call__(self):
        return self.now


class KeepAliveTests(unittest.TestCase):
    def setUp(self):
        self.server = Server()
        self.addCleanup(self.server.stop)

    def test_evm_rpc_reuses_one_connection_for_every_request(self):
        rpc = LocalRpc(self.server.url)
        for _ in range(5):
            self.assertEqual(rpc.call('eth_chainId'), '0x1')
        self.assertEqual(rpc.batch([('eth_blockNumber', []), ('eth_chainId', [])]), ['0x1', '0x1'])
        self.assertEqual(len(self.server.payloads), 6)
        self.assertEqual(self.server.connections(), 1)

    def test_b3_rpc_reuses_one_connection_and_sends_the_cookie_each_time(self):
        with tempfile.TemporaryDirectory() as directory:
            cookie = Path(directory) / '.cookie'
            cookie.write_text('__cookie__:first', encoding='utf-8')
            rpc = B3Rpc(self.server.url, cookie)
            self.assertEqual(rpc.call('ping'), '0x1')
            cookie.write_text('__cookie__:second', encoding='utf-8')
            self.assertEqual(rpc.call('ping'), '0x1')
        self.assertEqual(self.server.connections(), 1)
        self.assertNotEqual(self.server.headers[0]['Authorization'], self.server.headers[1]['Authorization'])

    def test_error_status_raises_as_before_and_drops_the_connection(self):
        rpc = LocalRpc(self.server.url)
        rpc.call('eth_chainId')
        self.server.status = 500
        with self.assertRaises(RpcError):
            rpc.call('eth_chainId')
        self.server.status = 200
        rpc.call('eth_chainId')
        self.assertEqual(len(self.server.payloads), 3)
        self.assertEqual(self.server.connections(), 2)

    def test_b3_error_status_is_still_unavailable(self):
        with tempfile.TemporaryDirectory() as directory:
            cookie = Path(directory) / '.cookie'
            cookie.write_text('__cookie__:x', encoding='utf-8')
            rpc = B3Rpc(self.server.url, cookie)
            self.server.status = 401
            with self.assertRaisesRegex(CustodyError, 'unavailable'):
                rpc.call('ping')

    def test_server_close_is_honoured(self):
        self.server.close = True
        rpc = LocalRpc(self.server.url)
        for _ in range(3):
            rpc.call('eth_chainId')
        self.assertEqual(self.server.connections(), 3)

    def test_idle_connection_is_not_reused(self):
        clock = Clock()
        rpc = LocalRpc(self.server.url)
        rpc.opener = KeepAliveOpener(rpc.url, idle_seconds=1.0, clock=clock)
        rpc.call('eth_chainId')
        clock.now += 0.5
        rpc.call('eth_chainId')
        clock.now += 1.5
        rpc.call('eth_chainId')
        self.assertEqual(self.server.connections(), 2)
        self.assertEqual(rpc.opener.connects, 2)

    def test_oversized_reply_is_refused_and_the_connection_dropped(self):
        rpc = LocalRpc(self.server.url)
        rpc.MAX_RESPONSE_BYTES = 64
        self.server.pad = 200
        with self.assertRaisesRegex(RpcError, 'exceeds'):
            rpc.call('eth_chainId')
        self.server.pad = 0
        rpc.call('eth_chainId')
        self.assertEqual(self.server.connections(), 2)

    def test_each_thread_has_its_own_connection(self):
        rpc = LocalRpc(self.server.url)
        rpc.call('eth_chainId')
        worker = threading.Thread(target=lambda: [rpc.call('eth_chainId') for _ in range(3)])
        worker.start()
        worker.join()
        rpc.call('eth_chainId')
        self.assertEqual(self.server.connections(), 2)

    def test_opener_is_bound_to_its_loopback_url(self):
        opener = KeepAliveOpener(self.server.url)
        other = urllib.request.Request('http://127.0.0.1:1/', data=b'{}')
        with self.assertRaises(ValueError):
            opener.open(other, timeout=1)
        for url in ('https://127.0.0.1:1', 'http://127.0.0.1', 'file:///x'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                KeepAliveOpener(url)
        with self.assertRaises(ValueError):
            KeepAliveOpener(self.server.url, idle_seconds=0)

    def test_refused_connection_is_a_transport_error(self):
        url = self.server.url
        self.server.stop()
        with self.assertRaises(RpcError):
            LocalRpc(url, timeout=1).call('eth_chainId')


if __name__ == '__main__':
    unittest.main()
