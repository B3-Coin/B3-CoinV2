#!/usr/bin/env python3
"""Offline adversarial RPC boundary tests; no network or live-chain pass."""
import copy
import json
import http.server
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import unittest
from unittest import mock

from arbitrum_rpc import HttpsReadOnlyRpc, ProviderIdentityError, _PinnedConnection, quantity, strict_json
from custody import CustodyError, RpcError


GENESIS = '0x' + '17' * 32
HASH = '0x' + '23' * 32
ADDRESS = '0x' + 'ab' * 20


def provider(url='https://provider.example/key-private-value', label='provider-a', operator='operator-a'):
    return HttpsReadOnlyRpc(url, label, operator, label + '-endpoint', chain_id=42161, genesis_hash=GENESIS)


def response(encoded):
    requests = json.loads(encoded)
    results = []
    for item in requests:
        if item['method'] == 'eth_chainId':
            value = hex(42161)
        elif item['method'] == 'eth_getBlockByNumber' and item['params'][0] == '0x0':
            value = {'number': '0x0', 'hash': GENESIS}
        else:
            value = {'number': '0xa', 'hash': HASH}
        results.append({'jsonrpc': '2.0', 'id': item['id'], 'result': value})
    return results


class ReadOnlyRpcTests(unittest.TestCase):
    def test_identity_is_verified_in_every_batch_and_reordering_is_valid(self):
        rpc = provider()
        with mock.patch.object(rpc, '_exchange', side_effect=lambda raw: list(reversed(response(raw)))) as exchange:
            self.assertEqual(rpc.call('eth_getBlockByNumber', ['finalized', False])['hash'], HASH)
            self.assertEqual(rpc.call('eth_getBlockByHash', [HASH, False])['hash'], HASH)
        for call in exchange.call_args_list:
            sent = json.loads(call.args[0])
            self.assertEqual([item['method'] for item in sent[:2]], ['eth_chainId', 'eth_getBlockByNumber'])
            self.assertEqual(sent[1]['params'], ['0x0', False])
        self.assertNotEqual(json.loads(exchange.call_args_list[0].args[0])[0]['id'],
                            json.loads(exchange.call_args_list[1].args[0])[0]['id'])

    def test_writes_forks_accounts_nonce_and_implicit_latest_are_impossible(self):
        rpc = provider()
        with mock.patch.object(rpc, '_exchange') as exchange:
            for method in ('eth_sendRawTransaction', 'eth_sendTransaction', 'eth_sign', 'personal_sign',
                           'anvil_reset', 'evm_mine', 'eth_accounts', 'eth_getTransactionCount', 'eth_blockNumber'):
                with self.subTest(method=method), self.assertRaises(RpcError):
                    rpc.call(method, [])
            for tag in ('latest', 'pending', 'safe', 'earliest', '0x00', '0xA', True):
                with self.subTest(tag=tag), self.assertRaises(CustodyError):
                    rpc.call('eth_getBlockByNumber', [tag, False])
            with self.assertRaises(RpcError):
                rpc.call('eth_call', [{'to': ADDRESS, 'data': '0x'}])
            exchange.assert_not_called()

    def test_state_reads_are_exactly_bound_and_overrides_refused(self):
        rpc = provider()
        with mock.patch.object(rpc, '_exchange', side_effect=response):
            rpc.call('eth_call', [{'to': ADDRESS, 'data': '0x1234'}, {'blockHash': HASH, 'requireCanonical': True}])
            rpc.call('eth_getCode', [ADDRESS, '0x4'])
            rpc.call('eth_getStorageAt', [ADDRESS, '0x' + '00' * 32, 'finalized'])
        for params in ([{'to': ADDRESS, 'data': '0x', 'value': '0x0'}, '0x4'],
                       [{'to': ADDRESS, 'data': '0x'}, '0x4', {}],
                       [{'to': ADDRESS, 'data': '0x'}, {'blockHash': HASH, 'requireCanonical': False}]):
            with self.assertRaises(CustodyError):
                rpc.call('eth_call', params)

    def test_log_ranges_addresses_and_topic_bounds(self):
        rpc = provider()
        log_filter = {'fromBlock': '0x10', 'toBlock': '0x11', 'address': ADDRESS, 'topics': [HASH]}
        with mock.patch.object(rpc, '_exchange', side_effect=response):
            rpc.call('eth_getLogs', [log_filter])
        for change in ({'toBlock': hex(16 + rpc.MAX_LOG_RANGE)}, {'fromBlock': 'latest'},
                       {'address': []}, {'topics': []}, {'topics': [[HASH] * 9]}, {'blockHash': HASH}):
            bad = dict(log_filter, **change)
            with self.subTest(change=change), self.assertRaises(CustodyError):
                rpc.call('eth_getLogs', [bad])

    def test_batch_protocol_and_identity_negatives(self):
        rpc = provider()
        def mutate(fn):
            def exchange(raw):
                items = response(raw)
                fn(items)
                return items
            return exchange
        mutations = [lambda x: x.pop(), lambda x: x.append(x[0]),
            lambda x: x[1].update(id=x[0]['id']), lambda x: x[0].update(id=True),
            lambda x: x[0].update(jsonrpc='1.0'), lambda x: x[0].update(extra=1),
            lambda x: x[2].update(error={'code': -1, 'message': 'private-provider-value'}),
            lambda x: x[0].update(result='0x1'),
            lambda x: x[1].update(result={'number': '0x0', 'hash': HASH})]
        for change in mutations:
            with self.subTest(change=change), mock.patch.object(rpc, '_exchange', side_effect=mutate(change)):
                with self.assertRaises(CustodyError) as caught:
                    rpc.call('eth_getBlockByNumber', ['finalized', False])
                self.assertNotIn('private-provider-value', str(caught.exception))
        with mock.patch.object(rpc, '_exchange', side_effect=mutate(lambda x: x[0].update(result='0x1'))):
            with self.assertRaises(ProviderIdentityError):
                rpc.call('eth_chainId')

    def test_strict_json_duplicates_nonfinite_and_quantity(self):
        for raw in ('{"id":1,"id":2}', '{"x":NaN}', '{"x":Infinity}'):
            with self.assertRaises(ValueError):
                strict_json(raw)
        for value in ('0x00', '0x', '0xA', True, 10, hex(1 << 64)):
            with self.assertRaises(CustodyError):
                quantity(value)

    def test_nonpublic_nonhttps_url_or_chain_refused(self):
        for url in ('http://provider.example', 'https://localhost', 'https://127.0.0.1',
                    'https://10.0.0.1', 'https://[::1]', 'https://a:b@provider.example',
                    'https://provider.example:8443', 'https://provider.example/#fragment',
                    'https://provider.example/\nsecret'):
            with self.subTest(url=url), self.assertRaises(CustodyError):
                provider(url)
        with self.assertRaises(CustodyError):
            HttpsReadOnlyRpc('https://provider.example', 'p', 'o', 'e', chain_id=31337, genesis_hash=GENESIS)

    def test_dns_rebinding_private_address_refused_before_connect(self):
        rpc = provider()
        for answers in ([('AF_INET', 1, 6, '', ('127.0.0.1', 443))],
                        [('AF_INET', 1, 6, '', ('8.8.8.8', 443)), ('AF_INET', 1, 6, '', ('10.1.1.1', 443))]):
            with mock.patch('arbitrum_rpc.socket.getaddrinfo', return_value=answers), \
                    mock.patch('arbitrum_rpc._PinnedConnection') as connection:
                with self.assertRaises(RpcError):
                    rpc.call('eth_chainId')
                connection.assert_not_called()

    def test_response_oversize_redirect_encoding_and_invalid_json_refuse(self):
        rpc = provider()
        good = response(json.dumps([{'id': 1, 'method': 'eth_chainId'},
            {'id': 2, 'method': 'eth_getBlockByNumber', 'params': ['0x0']}, {'id': 3, 'method': 'eth_chainId'}]).encode())
        for status, headers, body in ((302, {}, b''), (200, {'Content-Encoding': 'gzip'}, b''),
            (200, {'Content-Length': str(rpc.MAX_RESPONSE_BYTES + 1)}, b''),
            (200, {}, b'{"x":1,"x":2}'), (200, {}, b'\xff'),
            (200, {}, b'x' * (rpc.MAX_RESPONSE_BYTES + 1))):
            class Reply:
                def __init__(self): self.status, self.position = status, 0
                def isclosed(self): return self.position >= len(body)
                def getheader(self, key, default=None):
                    return headers.get(key, 'application/json' if key == 'Content-Type' else default)
                def read1(self, size):
                    chunk = body[self.position:self.position+size]
                    self.position += len(chunk)
                    return chunk
            connection = mock.Mock()
            connection.getresponse.return_value = Reply()
            with mock.patch('arbitrum_rpc.socket.getaddrinfo', return_value=[(2, 1, 6, '', ('8.8.8.8', 443))]), \
                    mock.patch('arbitrum_rpc._PinnedConnection', return_value=connection):
                with self.assertRaises(CustodyError):
                    rpc.call('eth_chainId')
                request_headers = connection.request.call_args.args[3]
                self.assertEqual(request_headers['User-Agent'], 'B3-Arbitrum-ReadOnly-Qualification/1')
                self.assertEqual(request_headers['Accept-Encoding'], 'identity')

    def test_binding_is_frozen_and_credentials_absent(self):
        rpc = provider()
        binding = rpc.binding
        self.assertNotIn('key-private-value', json.dumps(binding))
        binding['operator_id'] = 'mutated'
        self.assertEqual(rpc.binding['operator_id'], 'operator-a')

    def test_public_ip_connection_retains_tls_hostname_and_default_verification_context(self):
        tls = mock.Mock()
        context = mock.Mock()
        context.wrap_socket.return_value = tls
        raw = mock.Mock()
        with mock.patch('arbitrum_rpc.ssl.create_default_context', return_value=context) as defaults, \
                mock.patch('arbitrum_rpc.socket.create_connection', return_value=raw) as connect:
            connection = _PinnedConnection('provider.example', 443, '8.8.8.8', 2)
            connection.connect()
            defaults.assert_called_once_with()
            self.assertEqual(connect.call_args.args, (('8.8.8.8', 443),))
            self.assertGreater(connect.call_args.kwargs['timeout'], 0)
            self.assertLessEqual(connect.call_args.kwargs['timeout'], 2)
            context.wrap_socket.assert_called_once_with(raw, server_hostname='provider.example',
                                                       do_handshake_on_connect=False)
            tls.do_handshake.assert_called_once_with()
            connection.close()

    def test_connect_consuming_deadline_cannot_start_a_fresh_tls_timeout(self):
        context, raw = mock.Mock(), mock.Mock()
        with mock.patch('arbitrum_rpc.ssl.create_default_context', return_value=context), \
                mock.patch('arbitrum_rpc.socket.create_connection', return_value=raw), \
                mock.patch('arbitrum_rpc.time.monotonic', side_effect=[0, 0, 1.1]):
            connection = _PinnedConnection('provider.example', 443, '8.8.8.8', 1)
            with self.assertRaisesRegex(RpcError, 'connection deadline'):
                connection.connect()
            raw.close.assert_called_once_with()
            context.wrap_socket.assert_not_called()

    def test_dns_wait_is_deadline_bounded_without_unbounded_resolver_creation(self):
        rpc = HttpsReadOnlyRpc('https://provider.example', 'p', 'o', 'e', chain_id=42161,
                              genesis_hash=GENESIS, timeout=0.02)
        def stalled(*_args, **_kwargs):
            time.sleep(0.12)
            return [(2, 1, 6, '', ('8.8.8.8', 443))]
        start = time.monotonic()
        with mock.patch('arbitrum_rpc.socket.getaddrinfo', side_effect=stalled), \
                mock.patch('arbitrum_rpc._PinnedConnection') as connection:
            with self.assertRaisesRegex(RpcError, 'DNS deadline'):
                rpc.call('eth_chainId')
            self.assertLess(time.monotonic() - start, 0.1)
            connection.assert_not_called()
        # Let the one outstanding bounded daemon release its slot before tests exit.
        time.sleep(0.13)

    def test_tls_error_and_upstream_errors_do_not_publish_private_endpoint(self):
        rpc = provider()
        with mock.patch('arbitrum_rpc.socket.getaddrinfo', return_value=[(2, 1, 6, '', ('8.8.8.8', 443))]), \
                mock.patch('arbitrum_rpc._PinnedConnection', side_effect=OSError(rpc._url)):
            with self.assertRaises(RpcError) as caught:
                rpc.call('eth_chainId')
            self.assertNotIn('key-private-value', str(caught.exception))

    def test_batch_count_and_request_bytes_are_bounded_before_io(self):
        rpc = provider()
        with mock.patch.object(rpc, '_exchange') as exchange:
            for requests in ([], [('eth_chainId', [])] * 31, ['invalid']):
                with self.assertRaises(CustodyError):
                    rpc.batch(requests)
            rpc.MAX_REQUEST_BYTES = 32
            with self.assertRaises(CustodyError):
                rpc.call('eth_chainId')
            exchange.assert_not_called()

    @unittest.skipUnless(shutil.which('openssl'), 'local real TLS regression requires openssl')
    def test_real_tls_content_length_close_and_truncation(self):
        # Generate an ephemeral TEST identity in temporary storage; no private
        # material is embedded in source or public evidence. Production CA/TLS
        # policy is replaced only inside this local test's mock context.
        with tempfile.TemporaryDirectory() as directory:
            certificate, key = Path(directory) / 'certificate.pem', Path(directory) / 'private-key.pem'
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
                '-noenc', '-days', '1', '-subj', '/CN=provider.example', '-addext', 'subjectAltName=DNS:provider.example',
                '-keyout', str(key), '-out', str(certificate)], check=True, capture_output=True, timeout=10)
            server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            server_context.load_cert_chain(certificate, key)
            client_context = ssl.create_default_context(cafile=str(certificate))
            class Handler(http.server.BaseHTTPRequestHandler):
                protocol_version = 'HTTP/1.1'
                def log_message(self, *_args): pass
                def do_POST(self):
                    body = self.rfile.read(int(self.headers['Content-Length']))
                    payload = json.dumps(response(body)).encode()
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/json')
                    self.send_header('Content-Length', str(len(payload) + int(self.path == '/truncated')))
                    self.send_header('Connection', 'close')
                    self.end_headers()
                    self.wfile.write(payload)
                    self.wfile.flush()
                    self.close_connection = True
            server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
            server.socket = server_context.wrap_socket(server.socket, server_side=True)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            original_dns, connection_class = socket.getaddrinfo, _PinnedConnection
            def dns(host, port, *args, **kwargs):
                if host == 'provider.example':
                    return [(socket.AF_INET, socket.SOCK_STREAM, socket.IPPROTO_TCP, '', ('8.8.8.8', port))]
                return original_dns(host, port, *args, **kwargs)
            def connection(host, _port, _address, timeout):
                return connection_class(host, server.server_port, '127.0.0.1', timeout)
            try:
                with mock.patch('arbitrum_rpc.socket.getaddrinfo', side_effect=dns), \
                        mock.patch('arbitrum_rpc.ssl.create_default_context', return_value=client_context), \
                        mock.patch('arbitrum_rpc._PinnedConnection', side_effect=connection):
                    self.assertEqual(provider('https://provider.example/ok').call('eth_chainId'), hex(42161))
                    with self.assertRaisesRegex(RpcError, 'declared length'):
                        provider('https://provider.example/truncated').call('eth_chainId')
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=5)


if __name__ == '__main__':
    unittest.main(verbosity=2)
