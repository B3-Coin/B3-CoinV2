#!/usr/bin/env python3
"""Bounded local process/RPC helpers for the isolated regtest fixture."""
import base64
import ipaddress
import json
import os
from pathlib import Path
import queue
import signal
import socket
import subprocess
import threading
import time
import urllib.error
import urllib.request

from custody import CustodyError, NoRedirect, hex_value, local_rpc_url, uint

MODERN_GENESIS = '10d0f5bb6fde880011fd56ea35dddbf9b32da2a7825f5579ac587a7904d6929a'


def raw_hex(value, size=32):
    return hex_value(value if value.startswith('0x') else '0x' + value, size)[2:]


def free_ports(count):
    sockets = []
    try:
        for _ in range(count):
            item = socket.socket()
            item.bind(('127.0.0.1', 0))
            sockets.append(item)
        return [item.getsockname()[1] for item in sockets]
    finally:
        for item in sockets:
            item.close()


class OwnedProcess:
    def __init__(self, arguments, log_path, json_pipe=False):
        self.arguments = list(map(str, arguments))
        self.log = Path(log_path).open('ab')
        options = {'start_new_session': True} if os.name != 'nt' else {'creationflags': subprocess.CREATE_NO_WINDOW}
        self.process = subprocess.Popen(self.arguments, stdin=subprocess.PIPE if json_pipe else subprocess.DEVNULL,
                                        stdout=subprocess.PIPE if json_pipe else self.log, stderr=self.log, **options)

    def close(self):
        if self.process.poll() is None:
            if os.name == 'nt':
                self.process.terminate()
            else:
                os.killpg(self.process.pid, signal.SIGTERM)
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                if os.name == 'nt':
                    self.process.kill()
                else:
                    os.killpg(self.process.pid, signal.SIGKILL)
                self.process.wait(timeout=10)
        self.log.close()


class JsonProcess(OwnedProcess):
    def __init__(self, arguments, log_path):
        super().__init__(arguments, log_path, json_pipe=True)
        self.queue = queue.Queue(maxsize=2)
        self.request_id = 0
        self.lock = threading.Lock()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        try:
            while True:
                line = self.process.stdout.readline(33_554_433)
                if not line:
                    raise CustodyError('owned JSON worker closed its output')
                if len(line) > 33_554_432 or not line.endswith(b'\n'):
                    raise CustodyError('owned JSON worker exceeded its framing bound')
                self.queue.put(json.loads(line))
        except Exception as error:
            self.queue.put(error)

    def request(self, command, timeout=120, **fields):
        if 'id' in fields or 'cmd' in fields:
            raise CustodyError('request fields collide with JSON framing identity/command')
        with self.lock:
            self.request_id += 1
            identity = self.request_id
            request = {'id': identity, 'cmd': command, **fields}
            encoded = json.dumps(request, sort_keys=True, separators=(',', ':')).encode() + b'\n'
            if len(encoded) > 33_554_432 or self.process.poll() is not None:
                raise CustodyError('worker unavailable or request exceeds framing bound')
            self.process.stdin.write(encoded)
            self.process.stdin.flush()
            try:
                reply = self.queue.get(timeout=timeout)
            except queue.Empty as error:
                # Do not keep using a stream with an ambiguous outstanding reply.
                self.close()
                raise CustodyError('JSON worker deadline expired; signer state retained and worker stopped') from error
            if isinstance(reply, Exception):
                raise CustodyError(str(reply)) from reply
            if not isinstance(reply, dict) or reply.get('id') != identity:
                self.close()
                raise CustodyError('JSON worker response identity mismatch')
            if reply.get('ok') is not True:
                raise CustodyError('worker refused ' + command + ': ' + json.dumps(reply.get('error'), sort_keys=True))
            return reply['result']


class _B3RpcResponseError(CustodyError):
    """Validated RPC errors retain query positions without trusting error text."""
    def __init__(self, failures, results=()):
        super().__init__('local B3 RPC refused or returned a mismatched response')
        self.failures, self.results = tuple(failures), tuple(results)


class B3Rpc:
    ALLOWED = {'getblockchaininfo', 'getblockhash', 'getblockheader', 'getpeerinfo', 'getnetworkinfo',
               'generatetodescriptor', 'invalidateblock', 'stop', 'ping'}
    BATCH_ALLOWED = {'getblockchaininfo', 'getblockhash', 'getblockheader', 'getpeerinfo', 'getnetworkinfo'}

    def __init__(self, url, cookie_file):
        self.url = local_rpc_url(url)
        self.cookie_file = Path(cookie_file).resolve()
        self.counter = 0
        self.id_lock = threading.Lock()
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())

    def _identities(self, count):
        with self.id_lock:
            first = self.counter + 1
            self.counter += count
            return list(range(first, self.counter + 1))

    @staticmethod
    def _unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise CustodyError('local B3 RPC returned a duplicate JSON field')
            result[key] = value
        return result

    @staticmethod
    def _nonfinite(value):
        raise CustodyError('local B3 RPC returned a nonfinite JSON constant')

    def _exchange(self, payload):
        cookie = self.cookie_file.read_text(encoding='utf-8').strip()
        headers = {'Content-Type': 'application/json',
                   'Authorization': 'Basic ' + base64.b64encode(cookie.encode()).decode()}
        try:
            body = json.dumps(payload, separators=(',', ':'), allow_nan=False).encode()
        except (TypeError, ValueError) as error:
            raise CustodyError('local B3 RPC request is not bounded JSON') from error
        if len(body) > 65536:
            raise CustodyError('local B3 RPC request exceeds its bound')
        request = urllib.request.Request(self.url, data=body, headers=headers)
        try:
            with self.opener.open(request, timeout=10) as reply:
                encoded = reply.read(4_194_305)
            if len(encoded) > 4_194_304:
                raise CustodyError('B3 RPC response exceeds its bound')
            return json.loads(encoded, object_pairs_hook=self._unique, parse_constant=self._nonfinite)
        except (OSError, urllib.error.URLError, json.JSONDecodeError, UnicodeError, RecursionError) as error:
            raise CustodyError('local B3 RPC unavailable: ' + str(error)) from error

    @staticmethod
    def _reply(result, identity):
        if (not isinstance(result, dict) or set(result) - {'jsonrpc', 'id', 'error', 'result'} or
                type(result.get('id')) is not int or result['id'] != identity or
                result.get('jsonrpc', '2.0') != '2.0'):
            raise CustodyError('local B3 RPC refused or returned a mismatched response')
        error = result.get('error')
        if error is not None:
            if (not isinstance(error, dict) or set(error) - {'code', 'message', 'data'} or
                    type(error.get('code')) is not int or not isinstance(error.get('message'), str) or
                    result.get('result') is not None):
                raise CustodyError('local B3 RPC refused or returned a mismatched response')
            raise _B3RpcResponseError(((None, error['code']),))
        if 'result' not in result:
            raise CustodyError('local B3 RPC refused or returned a mismatched response')
        return result['result']

    def call(self, method, *params):
        if method not in self.ALLOWED:
            raise CustodyError('B3 RPC method is outside the local test allowlist')
        if method in ('generatetodescriptor', 'invalidateblock'):
            self.assert_regtest()  # Recheck exact chain/peers immediately before any local chain mutation.
            if method == 'invalidateblock':
                if len(params) != 1:
                    raise CustodyError('local invalidation requires one exact block hash')
                raw_hex(params[0])
        identity = self._identities(1)[0]
        return self._reply(self._exchange({'jsonrpc': '2.0', 'id': identity, 'method': method, 'params': list(params)}), identity)

    def batch(self, calls):
        """Perform independent read-only queries without caching any guard.

        JSON-RPC replies may be reordered. Every requested identity must occur
        exactly once; partial success, extra replies and mutation methods fail.
        """
        if not isinstance(calls, (list, tuple)) or not 1 <= len(calls) <= 16:
            raise CustodyError('local B3 RPC batch exceeds its bounded query count')
        for item in calls:
            if (not isinstance(item, (list, tuple)) or len(item) != 2 or
                    not isinstance(item[0], str) or item[0] not in self.BATCH_ALLOWED or
                    not isinstance(item[1], (list, tuple)) or len(item[1]) > 16):
                raise CustodyError('local B3 RPC batch permits only bounded read-only queries')
        identities = self._identities(len(calls))
        payload = [{'jsonrpc': '2.0', 'id': identity, 'method': method, 'params': list(params)}
                   for identity, (method, params) in zip(identities, calls)]
        result = self._exchange(payload)
        if not isinstance(result, list) or len(result) != len(identities):
            raise CustodyError('local B3 RPC batch returned an incomplete or oversized reply set')
        replies = {}
        for reply in result:
            if (not isinstance(reply, dict) or type(reply.get('id')) is not int or
                    reply['id'] not in identities or reply['id'] in replies):
                raise CustodyError('local B3 RPC batch returned an unknown or repeated identity')
            replies[reply['id']] = reply
        # Validate every identity and envelope before classifying any RPC error.
        # A missing anchor must never conceal a malformed or unrelated reply.
        values, failures = [], []
        for position, identity in enumerate(identities):
            try:
                values.append(self._reply(replies[identity], identity))
            except _B3RpcResponseError as error:
                values.append(None)
                failures.extend((position, code) for _, code in error.failures)
        if failures:
            raise _B3RpcResponseError(failures, values)
        return values

    @staticmethod
    def _check_identity(info, genesis, peers):
        if (not isinstance(info, dict) or info.get('chain') != 'regtest' or
                not isinstance(genesis, str) or raw_hex(genesis) != MODERN_GENESIS):
            raise CustodyError('B3 RPC is not the exact modern regtest fixture')
        if not isinstance(peers, list):
            raise CustodyError('B3 peer inventory is not a bounded RPC array')
        try:
            for peer in peers:
                endpoint = peer['addr']
                host = endpoint[1:endpoint.index(']')] if endpoint.startswith('[') else endpoint.rsplit(':', 1)[0]
                if not ipaddress.ip_address(host).is_loopback:
                    raise CustodyError('B3 fixture has a non-loopback peer')
        except (KeyError, TypeError, AttributeError, ValueError) as error:
            raise CustodyError('B3 peer inventory contains an invalid endpoint') from error

    def assert_regtest(self):
        info, genesis, peers = self.batch([('getblockchaininfo', []), ('getblockhash', [0]), ('getpeerinfo', [])])
        self._check_identity(info, genesis, peers)
        return info

    def anchor(self, height=None):
        info = self.assert_regtest()
        height = info['blocks'] if height is None else uint(height, 32)
        block_hash = raw_hex(self.call('getblockhash', height))
        header = self.call('getblockheader', block_hash)
        if header.get('height') != height or header.get('confirmations', 0) < 1 or raw_hex(header['hash']) != block_hash:
            raise CustodyError('B3 anchor is not a confirmed canonical block')
        return {'height': height, 'hash': bytes.fromhex(block_hash)[::-1].hex()}

    def verify_anchor(self, anchor):
        height = uint(anchor['height'], 32)
        expected = bytes.fromhex(raw_hex(anchor['hash']))[::-1].hex()
        try:
            info, genesis, peers, block_hash, header = self.batch([
                ('getblockchaininfo', []), ('getblockhash', [0]), ('getpeerinfo', []),
                ('getblockhash', [height]), ('getblockheader', [expected])])
        except _B3RpcResponseError as error:
            if error.failures and all(failure in ((3, -8), (4, -5)) for failure in error.failures):
                self._check_identity(*error.results[:3])
                raise CustodyError('B3 anchor is unavailable or noncanonical on this operator\'s own chain') from error
            raise
        self._check_identity(info, genesis, peers)
        if (not isinstance(block_hash, str) or raw_hex(block_hash) != expected or not isinstance(header, dict) or type(header.get('height')) is not int or
                header['height'] != height or type(header.get('confirmations')) is not int or
                header['confirmations'] < 1 or not isinstance(header.get('hash'), str) or raw_hex(header['hash']) != expected):
            raise CustodyError('B3 anchor differs from this operator\'s own chain')
        return {'height': height, 'hash': bytes.fromhex(expected)[::-1].hex()}


def wait_until(predicate, seconds, detail):
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        try:
            result = predicate()
            if result:
                return result
        except (CustodyError, OSError, KeyError) as error:
            last = error
        time.sleep(0.1)
    raise CustodyError(detail + (': ' + str(last) if last else ''))
