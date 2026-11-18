import contextlib
import http.server
import json
import os
from pathlib import Path
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock

from export_witness import Objects, account_request
from devnet import safe_path, docker
from local_rpc import LocalNitroRpc, raw_hex, unique_object


class BoundaryTests(unittest.TestCase):
    def test_only_literal_loopback_and_bounded_port(self):
        for host in ('localhost', '0.0.0.0', '::1', '1.1.1.1'):
            with self.assertRaises(ValueError):
                LocalNitroRpc(18547, host=host)
        for port in (True, 0, 80, 65536):
            with self.assertRaises(ValueError):
                LocalNitroRpc(port)

    def test_read_transport_cannot_send_or_control_node(self):
        client = LocalNitroRpc(18547)
        for method in ('eth_sendTransaction', 'eth_sendRawTransaction',
                       'anvil_setStorageAt', 'debug_setHead', 'admin_addPeer'):
            with self.assertRaisesRegex(ValueError, 'read-only'):
                client.call(method)
        # A caller's Docker context or loader environment must never redirect
        # even a read-only status command to a remote/mainnet host.
        result = SimpleNamespace(returncode=0, stdout='local-only\n', stderr='')
        with mock.patch.dict(os.environ, {'DOCKER_HOST': 'tcp://example.invalid:2375',
                                         'DOCKER_CONTEXT': 'untrusted-remote', 'LD_PRELOAD': '/untrusted'}):
            with mock.patch('devnet.subprocess.run', return_value=result) as run:
                self.assertEqual(docker('version'), 'local-only')
        self.assertEqual(run.call_args.args[0],
                         ['/usr/bin/docker', '--host', 'unix:///var/run/docker.sock', 'version'])
        self.assertEqual(run.call_args.kwargs['env'],
                         {'PATH': '/usr/bin:/bin', 'HOME': '/nonexistent',
                          'LANG': 'C', 'LC_ALL': 'C', 'TZ': 'UTC'})

    def test_duplicate_json_members_refused(self):
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            json.loads('{"id":1,"id":2}', object_pairs_hook=unique_object)

    def test_hex_length_and_allocation_bounds(self):
        self.assertEqual(raw_hex('0x00ff', size=2), b'\x00\xff')
        for value in ('abc', '0x1', '0xgg', None):
            with self.assertRaises(ValueError):
                raw_hex(value)
        with self.assertRaises(ValueError):
            raw_hex('0x0000', maximum=1)
        with self.assertRaises(ValueError):
            raw_hex('0x00', size=2)

    def test_account_request_exact_keys_and_no_duplicates(self):
        good = {'address': '0x' + '12' * 20, 'storage_keys': ['0x' + '00' * 32]}
        self.assertEqual(account_request(good), good)
        for value in ({**good, 'verified': True}, {**good, 'storage_keys': good['storage_keys'] * 2},
                      {**good, 'address': '0x12'}):
            with self.assertRaises(ValueError):
                account_request(value)

    def test_objects_are_content_addressed_and_conflict_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            objects = Objects(Path(temp) / 'objects')
            digest = objects.put(b'actual raw bytes')
            self.assertEqual(objects.put(b'actual raw bytes'), digest)
            self.assertEqual(objects.count, 1)
            (objects.path / digest).write_bytes(b'corruption')
            with self.assertRaisesRegex(ValueError, 'conflict'):
                objects.put(b'actual raw bytes')

    @unittest.skipUnless(hasattr(os, 'symlink'), 'symlink primitive unavailable')
    def test_symbolic_runtime_path_is_refused(self):
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            (parent / 'real').mkdir()
            os.symlink(parent / 'real', parent / 'alias', target_is_directory=True)
            with self.assertRaisesRegex(ValueError, 'symbolic'):
                safe_path(parent / 'alias' / 'future', existing=False)

    def test_native_claim_in_rpc_reply_is_not_accepted(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
                body = json.dumps({'jsonrpc': '2.0', 'id': request['id'],
                                   'result': '0x1', 'verified': True}).encode()
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        with http.server.HTTPServer(('127.0.0.1', 0), Handler) as server:
            thread = threading.Thread(target=server.handle_request)
            thread.start()
            try:
                with self.assertRaises(RuntimeError):
                    LocalNitroRpc(server.server_port).call('eth_chainId')
            finally:
                thread.join(3)


if __name__ == '__main__':
    unittest.main()
