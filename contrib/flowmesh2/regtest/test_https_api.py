#!/usr/bin/env python3
"""Exercise the actual TLS listener, framing and public/private API boundary."""
import http.client
import json
from pathlib import Path
import socket
import ssl
import struct
import tempfile
import threading
import unittest

from https_api import HttpsApiError, HttpsClient, HttpsGateway, create_local_tls


class HttpsBoundary(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.config = create_local_tls(Path(cls.temp.name))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.calls, self.lock = [], threading.Lock()
        self.fail_totals = False
        config = {**self.config, 'max_connections': 8, 'stream_seconds': 1,
                  'allowed_origins': ['https://example.test']}
        self.gateway = HttpsGateway(config, self.dispatch).start()
        self.client = HttpsClient(self.gateway.info()['url'], self.config['certificate'],
                                  self.config['certificate_sha256'])

    def tearDown(self):
        self.gateway.close()

    def dispatch(self, kind, value):
        with self.lock:
            self.calls.append((kind, value))
        if kind == 'info':
            return {'height': 1}
        if kind == 'totals':
            if self.fail_totals:
                raise RuntimeError('private diagnostic must not escape')
            header = bytearray(366)
            struct.pack_into('<Q', header, 73, 1)
            return {'header': header.hex(), 'certificate': 'public-test-proof'}
        if kind == 'submit':
            return {'code': 'ADMITTED' if value['frame'] == 'aabb' else 'BAD_SIGNATURE', 'reason': 'test'}
        return {'verified': True}

    def connection(self):
        context = ssl.create_default_context(cafile=self.config['certificate'])
        context.minimum_version = ssl.TLSVersion.TLSv1_3
        return context.wrap_socket(socket.create_connection(self.gateway.address, timeout=3), server_hostname='127.0.0.1')

    def raw(self, data):
        with self.connection() as connection:
            connection.sendall(data)
            response = bytearray()
            while True:
                block = connection.recv(65536)
                if not block:
                    break
                response.extend(block)
            return bytes(response)

    def test_native_results_and_private_routes(self):
        self.assertEqual(self.client.request('GET', '/v1/info'), {'height': 1})
        self.assertEqual(self.client.request('POST', '/v1/submit', {'frame': 'aabb'})['code'], 'ADMITTED')
        with self.assertRaises(HttpsApiError) as caught:
            self.client.request('POST', '/v1/submit', {'frame': 'bbcc'})
        self.assertEqual(caught.exception.status, 422)
        before = len(self.calls)
        for route in ('/v1/network_dispatch', '/v1/seal', '/v1/sign', '/v1/open'):
            with self.assertRaises(HttpsApiError) as caught:
                self.client.request('GET', route)
            self.assertEqual(caught.exception.status, 404)
        self.assertEqual(len(self.calls), before)

    def test_wrong_certificate_pin_prevents_application_request(self):
        client = HttpsClient(self.gateway.info()['url'], self.config['certificate'], '00' * 32)
        with self.assertRaises(HttpsApiError) as caught:
            client.request('POST', '/v1/submit', {'frame': 'aabb'})
        self.assertEqual(caught.exception.status, 495)
        self.assertEqual(self.calls, [])

    def test_tls12_and_plaintext_never_dispatch(self):
        context = ssl.create_default_context(cafile=self.config['certificate'])
        context.maximum_version = ssl.TLSVersion.TLSv1_2
        with socket.create_connection(self.gateway.address, timeout=3) as connection:
            with self.assertRaises(ssl.SSLError):
                context.wrap_socket(connection, server_hostname='127.0.0.1')
        with socket.create_connection(self.gateway.address, timeout=3) as connection:
            connection.sendall(b'GET /v1/info HTTP/1.1\r\nHost: localhost\r\n\r\n')
            try:
                self.assertEqual(connection.recv(1024), b'')
            except ConnectionResetError:
                pass
        self.assertEqual(self.calls, [])

    def test_ambiguous_framing_and_oversize_refused_before_dispatch(self):
        cases = [(b'Content-Length: 0\r\nContent-Length: 0\r\n', 400),
                 (b'Transfer-Encoding: chunked\r\n', 400),
                 (b'Content-Length: 999999\r\nContent-Type: application/json\r\n', 413),
                 (b'Expect: 100-continue\r\n', 400)]
        for fields, status in cases:
            response = self.raw(b'POST /v1/submit HTTP/1.1\r\nHost: localhost\r\n' + fields + b'\r\n')
            self.assertTrue(response.startswith(f'HTTP/1.1 {status} '.encode()), response)
        self.assertEqual(self.calls, [])

    def test_duplicate_json_nonfinite_json_and_bad_identifiers_refused(self):
        for body in (b'{"frame":"signed","frame":"wrong"}', b'{"frame":NaN}'):
            request = (b'POST /v1/submit HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: ' +
                       str(len(body)).encode() + b'\r\n\r\n' + body)
            self.assertTrue(self.raw(request).startswith(b'HTTP/1.1 400 '))
        for route in ('/v1/account/' + 'A' * 64, '/v1/withdrawal/' + '1' * 63, '/v1/info?x=1'):
            with self.assertRaises(HttpsApiError) as caught:
                self.client.request('GET', route)
            self.assertEqual(caught.exception.status, 400)
        self.assertEqual(self.calls, [])

    def test_cors_exact_allowlist(self):
        self.assertEqual(self.client.request('GET', '/v1/info', headers={'Origin': 'https://example.test'})['height'], 1)
        before = len(self.calls)
        with self.assertRaises(HttpsApiError) as caught:
            self.client.request('GET', '/v1/info', headers={'Origin': 'https://example.test.evil'})
        self.assertEqual(caught.exception.status, 403)
        self.assertEqual(len(self.calls), before)

    def test_origin_configuration_rejects_path_credentials_query_fragment_controls(self):
        for origin in ('https://x/a', 'https://user@x', 'https://x?q=1', 'https://x#f', 'https://x\r\nBad: yes', 'https://x:0'):
            with self.assertRaises(ValueError):
                HttpsClient(origin, self.config['certificate'], self.config['certificate_sha256'])
            with self.assertRaises(ValueError):
                HttpsGateway({**self.config, 'allowed_origins': [origin]}, self.dispatch)

    def test_bounded_event_stream_carries_native_proof(self):
        response = self.raw(b'GET /v1/events?after-height=0 HTTP/1.1\r\nHost: localhost\r\n\r\n')
        self.assertEqual(response.count(b'HTTP/1.1 '), 1)
        self.assertIn(b'event: certified-state', response)
        event = response.split(b'data: ', 1)[1].split(b'\n', 1)[0]
        self.assertEqual(json.loads(event)['certificate'], 'public-test-proof')

    def test_stream_failure_disconnects_without_second_response_or_private_error(self):
        self.fail_totals = True
        response = self.raw(b'GET /v1/events HTTP/1.1\r\nHost: localhost\r\n\r\n')
        self.assertEqual(response.count(b'HTTP/1.1 '), 1)
        self.assertNotIn(b'private diagnostic', response)
        self.assertEqual(self.client.request('GET', '/v1/info')['height'], 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
