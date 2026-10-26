#!/usr/bin/env python3
"""HTTPS keep-alive reuses one pinned connection and keeps every request check."""
from pathlib import Path
import socket
import ssl
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import https_api  # noqa: E402
from https_api import HttpsApiError, HttpsClient, HttpsGateway, create_local_tls  # noqa: E402


class Clock:
    def __init__(self):
        self.now = 100.0

    def __call__(self):
        return self.now


class HttpsKeepAlive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.config = create_local_tls(Path(cls.temp.name))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.calls, self.lock = [], threading.Lock()
        self.accepted = 0
        original = https_api._Handler.handle

        def counting(handler):
            with self.lock:
                self.accepted += 1
            return original(handler)
        patcher = mock.patch.object(https_api._Handler, 'handle', counting)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.start_gateway()

    def start_gateway(self, **bounds):
        if getattr(self, 'gateway', None) is not None:
            self.gateway.close()
        config = {**self.config, 'max_connections': 8, 'header_timeout_seconds': 1, **bounds}
        self.gateway = HttpsGateway(config, self.dispatch).start()
        self.addCleanup(self.gateway.close)
        self.client = HttpsClient(self.gateway.info()['url'], self.config['certificate'],
                                  self.config['certificate_sha256'])

    def dispatch(self, kind, value):
        with self.lock:
            self.calls.append(kind)
        if kind == 'submit':
            return {'code': 'ADMITTED' if value['frame'] == 'aabb' else 'BAD_SIGNATURE'}
        return {'height': 1}

    def tls(self):
        context = ssl.create_default_context(cafile=self.config['certificate'])
        context.minimum_version = ssl.TLSVersion.TLSv1_3
        return context.wrap_socket(socket.create_connection(self.gateway.address, timeout=5), server_hostname='127.0.0.1')

    @staticmethod
    def response(connection):
        """Read one length-delimited response; return (status line, headers, body)."""
        data = bytearray()
        while b'\r\n\r\n' not in data:
            block = connection.recv(4096)
            if not block:
                return None
            data.extend(block)
        head, body = bytes(data).split(b'\r\n\r\n', 1)
        lines = head.decode().split('\r\n')
        headers = {line.split(':', 1)[0].lower(): line.split(':', 1)[1].strip() for line in lines[1:]}
        while len(body) < int(headers['content-length']):
            body += connection.recv(4096)
        return lines[0], headers, body

    def test_client_reuses_one_pinned_connection(self):
        for _ in range(5):
            self.assertEqual(self.client.request('GET', '/v1/info'), {'height': 1})
        self.assertEqual(self.client.request('POST', '/v1/submit', {'frame': 'aabb'})['code'], 'ADMITTED')
        self.assertEqual(len(self.calls), 6)
        self.assertEqual(self.accepted, 1)

    def test_refused_reply_closes_and_the_next_request_reconnects(self):
        self.client.request('GET', '/v1/info')
        with self.assertRaises(HttpsApiError) as caught:
            self.client.request('POST', '/v1/submit', {'frame': 'bbcc'})
        self.assertEqual(caught.exception.status, 422)
        self.client.request('GET', '/v1/info')
        self.assertEqual(self.accepted, 2)

    def test_idle_client_connection_is_not_reused(self):
        clock = Clock()
        client = HttpsClient(self.gateway.info()['url'], self.config['certificate'],
                             self.config['certificate_sha256'], clock=clock)
        client.request('GET', '/v1/info')
        clock.now += HttpsClient.IDLE_SECONDS / 2
        client.request('GET', '/v1/info')
        clock.now += HttpsClient.IDLE_SECONDS * 2
        client.request('GET', '/v1/info')
        self.assertEqual(self.accepted, 2)

    def test_each_thread_has_its_own_connection(self):
        self.client.request('GET', '/v1/info')
        worker = threading.Thread(target=lambda: [self.client.request('GET', '/v1/info') for _ in range(3)])
        worker.start()
        worker.join()
        self.client.request('GET', '/v1/info')
        self.assertEqual(self.accepted, 2)

    def test_keep_alive_off_matches_the_old_client(self):
        client = HttpsClient(self.gateway.info()['url'], self.config['certificate'],
                             self.config['certificate_sha256'], keep_alive=False)
        for _ in range(3):
            client.request('GET', '/v1/info')
        self.assertEqual(self.accepted, 3)

    def test_pin_mismatch_still_sends_nothing(self):
        client = HttpsClient(self.gateway.info()['url'], self.config['certificate'], '00' * 32)
        for _ in range(2):
            with self.assertRaises(HttpsApiError) as caught:
                client.request('GET', '/v1/info')
            self.assertEqual(caught.exception.status, 495)
        self.assertEqual(self.calls, [])

    def test_gateway_closes_unless_the_client_asks(self):
        request = b'GET /v1/info HTTP/1.1\r\nHost: localhost\r\n\r\n'
        with self.tls() as connection:
            connection.sendall(request)
            status, headers, _ = self.response(connection)
            self.assertEqual(headers['connection'], 'close')
            self.assertEqual(connection.recv(1), b'')

    def test_gateway_serves_sequential_requests_then_closes_when_idle(self):
        request = b'GET /v1/info HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n'
        with self.tls() as connection:
            for _ in range(3):
                connection.sendall(request)
                status, headers, body = self.response(connection)
                self.assertTrue(status.startswith('HTTP/1.1 200 '))
                self.assertEqual(headers['connection'], 'keep-alive')
            started = time.monotonic()
            self.assertEqual(connection.recv(1), b'')
            self.assertLess(time.monotonic() - started, 4)
        self.assertEqual(self.calls, ['info'] * 3)
        self.assertEqual(self.accepted, 1)

    def test_pipelined_requests_are_still_refused(self):
        request = b'GET /v1/info HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n'
        with self.tls() as connection:
            connection.sendall(request + request)
            status, headers, _ = self.response(connection)
            self.assertTrue(status.startswith('HTTP/1.1 400 '))
            self.assertEqual(headers['connection'], 'close')
        self.assertEqual(self.calls, [])

    def test_error_on_a_kept_connection_closes_it(self):
        good = b'GET /v1/info HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n'
        bad = b'GET /v1/nothing HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n'
        with self.tls() as connection:
            connection.sendall(good)
            self.assertEqual(self.response(connection)[1]['connection'], 'keep-alive')
            connection.sendall(bad)
            status, headers, _ = self.response(connection)
            self.assertTrue(status.startswith('HTTP/1.1 404 '))
            self.assertEqual(headers['connection'], 'close')
            self.assertEqual(connection.recv(1), b'')

    def test_per_connection_cap_ends_the_connection(self):
        self.gateway.keep_alive_requests = 3
        request = b'GET /v1/info HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n'
        with self.tls() as connection:
            seen = []
            for _ in range(3):
                connection.sendall(request)
                seen.append(self.response(connection)[1]['connection'])
            self.assertEqual(seen, ['keep-alive', 'keep-alive', 'close'])
            self.assertEqual(connection.recv(1), b'')

    def test_request_budget_still_counts_every_request(self):
        self.start_gateway(requests_per_minute=10)
        for _ in range(10):
            self.client.request('GET', '/v1/info')
        with self.assertRaises(HttpsApiError) as caught:
            self.client.request('GET', '/v1/info')
        self.assertEqual(caught.exception.status, 429)
        self.assertEqual(len(self.calls), 10)


if __name__ == '__main__':
    unittest.main()
