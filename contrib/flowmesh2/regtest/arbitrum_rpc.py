#!/usr/bin/env python3
"""Bounded, identity-pinned HTTPS reads. This client has no transaction API.

TLS authenticates the configured provider, not Ethereum consensus. Provider
operator labels are explicit deployment assertions; different URLs cannot prove
organizational independence. Credentials remain in private configuration and
are never included in diagnostics or provenance. LocalRpc is not relaxed.
"""
import copy
import hashlib
import http.client
import ipaddress
import json
import queue
import re
import socket
import ssl
import threading
import time
import urllib.parse

from custody import CustodyError, RpcError, hex_value, uint


class ProviderIdentityError(RpcError):
    """A complete, structurally valid provider response violated an identity pin."""


def quantity(value):
    if not isinstance(value, str) or not re.fullmatch(r'0x(?:0|[1-9a-f][0-9a-f]*)', value):
        raise RpcError('RPC quantity is not canonical lowercase hexadecimal')
    return uint(int(value, 16), 64)


def _unique_fields(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON field')
        result[key] = value
    return result


def strict_json(raw):
    def invalid(_value):
        raise ValueError('nonfinite JSON number')
    return json.loads(raw, object_pairs_hook=_unique_fields, parse_constant=invalid)


def _public_address(value):
    address = ipaddress.ip_address(value)
    if not address.is_global or address.is_multicast or address.is_unspecified:
        raise RpcError('HTTPS provider resolves to a non-public address')
    return str(address)


class _PinnedConnection(http.client.HTTPSConnection):
    """Connect to the already validated address, retaining hostname TLS/SNI."""
    def __init__(self, hostname, port, address, timeout):
        super().__init__(hostname, port, timeout=timeout, context=ssl.create_default_context())
        self.address = address

    def connect(self):
        deadline = time.monotonic() + self.timeout
        def remaining():
            value = deadline - time.monotonic()
            if value <= 0:
                raise RpcError('provider connection deadline exceeded')
            return value
        sock = socket.create_connection((self.address, self.port), timeout=remaining())
        try:
            sock.settimeout(remaining())
            self.sock = self._context.wrap_socket(sock, server_hostname=self.host,
                                                  do_handshake_on_connect=False)
            self.sock.settimeout(remaining())
            self.sock.do_handshake()
        except BaseException:
            sock.close()
            raise


class HttpsReadOnlyRpc:
    MAX_BATCH_ITEMS = 30  # Plus the two mandatory chain/genesis identity reads.
    MAX_REQUEST_BYTES = 256 * 1024
    MAX_RESPONSE_BYTES = 8 * 1024 * 1024
    MAX_LOG_RANGE = 2000
    _dns_slots = threading.BoundedSemaphore(4)
    ALLOWED = frozenset(('eth_chainId', 'eth_getBlockByNumber', 'eth_getBlockByHash',
        'eth_getCode', 'eth_getStorageAt', 'eth_call', 'eth_getLogs', 'eth_getTransactionReceipt'))

    def __init__(self, url, provider_id, operator_id, endpoint_id, *, chain_id, genesis_hash, timeout=12):
        # No configurable CA bypass, proxy, redirect handler or caller headers.
        if not isinstance(url, str) or len(url) > 2048:
            raise RpcError('invalid HTTPS provider configuration')
        try:
            parsed = urllib.parse.urlsplit(url)
            port = parsed.port or 443
            hostname = (parsed.hostname or '').encode('idna').decode('ascii').lower()
        except (TypeError, ValueError, UnicodeError) as error:
            raise RpcError('invalid HTTPS provider configuration') from error
        if (parsed.scheme != 'https' or
                not hostname or parsed.username is not None or parsed.password is not None or
                parsed.fragment or port != 443 or any(ord(c) < 33 or ord(c) == 127 for c in url) or
                '%' in hostname or hostname.endswith('.') or hostname in ('localhost', 'localhost.localdomain')):
            raise RpcError('provider requires public HTTPS on port443 without userinfo or fragment')
        if not re.fullmatch(r'[a-z0-9.-]+', hostname):
            raise RpcError('provider hostname is not canonical')
        try:
            _public_address(hostname)
        except ValueError:
            if '.' not in hostname or '..' in hostname:
                raise RpcError('provider requires a public hostname')
        for label in (provider_id, operator_id, endpoint_id):
            if not isinstance(label, str) or not re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9._-]{0,127}', label):
                raise RpcError('provider labels must be bounded public identifiers')
        if type(chain_id) is not int or chain_id != 42161:
            raise RpcError('this live read adapter is restricted to Arbitrum One chain42161')
        if isinstance(timeout, bool) or not isinstance(timeout, (int, float)) or not 0 < timeout <= 30:
            raise RpcError('HTTPS provider timeout must be in (0,30] seconds')
        self._chain_id, self._genesis_hash = chain_id, hex_value(genesis_hash, 32)
        self._url, self._host, self._port = url, hostname, port
        self._target = urllib.parse.urlunsplit(('', '', parsed.path or '/', parsed.query, ''))
        self._timeout, self._counter, self._lock = float(timeout), 0, threading.Lock()
        self._binding = {'operator_id': operator_id, 'endpoint_id': endpoint_id,
                         'endpoint_origin': 'https://' + hostname + ':443',
                         # Detect even a private path/API-key replacement without publishing it.
                         'endpoint_commitment': hashlib.sha256(url.encode('utf-8')).hexdigest()}
        self.provider_id = provider_id

    @property
    def binding(self):
        return copy.deepcopy(self._binding)

    def _block_selector(self, value):
        if value == 'finalized':
            return
        if isinstance(value, dict):
            if set(value) != {'blockHash', 'requireCanonical'} or value['requireCanonical'] is not True:
                raise RpcError('hash-selected state requires exact canonical block binding')
            hex_value(value['blockHash'], 32)
        else:
            quantity(value)

    def _validate(self, method, params):
        if method not in self.ALLOWED or not isinstance(params, (list, tuple)):
            raise RpcError('method is outside the read-only Arbitrum RPC allowlist')
        p = list(params)
        if method == 'eth_chainId':
            if p:
                raise RpcError('chain identity takes no arguments')
        elif method == 'eth_getBlockByNumber':
            if len(p) != 2 or p[1] is not False or isinstance(p[0], dict):
                raise RpcError('block read requires an explicit number/finalized and no full transactions')
            self._block_selector(p[0])
        elif method in ('eth_getBlockByHash', 'eth_getTransactionReceipt'):
            if len(p) != (2 if method == 'eth_getBlockByHash' else 1):
                raise RpcError('hash read has invalid arguments')
            hex_value(p[0], 32)
            if len(p) == 2 and p[1] is not False:
                raise RpcError('full transaction block reads are forbidden')
        elif method in ('eth_getCode', 'eth_getStorageAt'):
            if len(p) != (3 if method == 'eth_getStorageAt' else 2):
                raise RpcError('state read requires an explicit immutable block selector')
            hex_value(p[0], 20)
            if method == 'eth_getStorageAt':
                hex_value(p[1], 32)
            self._block_selector(p[-1])
        elif method == 'eth_call':
            if (len(p) != 2 or not isinstance(p[0], dict) or set(p[0]) != {'to', 'data'}):
                raise RpcError('contract read requires only to/data and an explicit block; no overrides')
            hex_value(p[0]['to'], 20)
            if len(hex_value(p[0]['data'])) > 2 + 128 * 1024:
                raise RpcError('contract read data exceeds its bound')
            self._block_selector(p[1])
        elif method == 'eth_getLogs':
            if len(p) != 1 or not isinstance(p[0], dict):
                raise RpcError('log read requires one bounded filter')
            f = p[0]
            required = {'address', 'topics'}
            if set(f) not in (required | {'fromBlock', 'toBlock'}, required | {'blockHash'}):
                raise RpcError('log filter requires explicit bounded blocks and addresses/topics')
            addresses = f['address'] if isinstance(f['address'], list) else [f['address']]
            if not 1 <= len(addresses) <= 8:
                raise RpcError('log address count is outside its bound')
            for address in addresses:
                hex_value(address, 20)
            if not isinstance(f['topics'], list) or not 1 <= len(f['topics']) <= 4:
                raise RpcError('log topic count is outside its bound')
            for item in f['topics']:
                options = item if isinstance(item, list) else [item]
                if not 1 <= len(options) <= 8:
                    raise RpcError('log topic alternatives exceed their bound')
                for option in options:
                    if option is not None:
                        hex_value(option, 32)
            if 'blockHash' in f:
                hex_value(f['blockHash'], 32)
            else:
                start, end = quantity(f['fromBlock']), quantity(f['toBlock'])
                if end < start or end - start + 1 > self.MAX_LOG_RANGE:
                    raise RpcError('log block range exceeds its bound')
        return p

    def _exchange(self, encoded):
        connection, watchdog, transport = None, None, None
        deadline = time.monotonic() + self._timeout
        try:
            # OS DNS is not timeout-bounded. Bound outstanding daemon resolutions
            # and the caller's wait, so unavailable DNS cannot hang qualification.
            if not self._dns_slots.acquire(blocking=False):
                raise RpcError('provider DNS concurrency bound reached')
            resolved = queue.Queue(maxsize=1)
            def resolve():
                try:
                    resolved.put(socket.getaddrinfo(self._host, self._port, type=socket.SOCK_STREAM))
                except OSError:
                    resolved.put(None)
                finally:
                    self._dns_slots.release()
            threading.Thread(target=resolve, daemon=True).start()
            try:
                addresses = resolved.get(timeout=max(0, deadline - time.monotonic()))
            except queue.Empty:
                raise RpcError('provider DNS deadline exceeded') from None
            if addresses is None:
                raise RpcError('provider DNS resolution failed')
            # Reject an entire DNS answer containing a private/loopback destination.
            if not 1 <= len(addresses) <= 16:
                raise RpcError('provider DNS answer exceeds its bound')
            validated = [(item[0], _public_address(item[4][0])) for item in addresses]
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RpcError('provider request deadline exceeded')
            # The official Arbitrum public RPC documents IPv4 support. Prefer
            # IPv4 when present without retrying another provider or tag.
            selected = next((address for family, address in validated if family == socket.AF_INET), validated[0][1])
            connection = _PinnedConnection(self._host, self._port, selected, remaining)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RpcError('provider request deadline exceeded')
            connection.timeout = remaining
            def abort():
                active = transport if transport is not None else connection.sock
                if active is not None:
                    try:
                        active.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    active.close()
            watchdog = threading.Timer(remaining, abort)
            watchdog.daemon = True
            watchdog.start()
            connection.connect()
            transport = connection.sock
            connection.request('POST', self._target, encoded,
                               {'Content-Type': 'application/json', 'Accept': 'application/json',
                                'Accept-Encoding': 'identity', 'Connection': 'close',
                                'User-Agent': 'B3-Arbitrum-ReadOnly-Qualification/1'})
            response = connection.getresponse()
            if response.status != 200:
                raise RpcError('provider HTTP response refused; redirects are forbidden')
            if response.getheader('Content-Encoding', 'identity').lower() != 'identity':
                raise RpcError('compressed provider responses are forbidden')
            content_type = response.getheader('Content-Type', '').split(';', 1)[0].strip().lower()
            if content_type not in ('application/json', 'application/json-rpc'):
                raise RpcError('provider response is not JSON')
            length = response.getheader('Content-Length')
            if length is not None and (not re.fullmatch(r'[0-9]+', length) or int(length) > self.MAX_RESPONSE_BYTES):
                raise RpcError('provider response length exceeds its bound')
            chunks, total = [], 0
            while not response.isclosed():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RpcError('provider request deadline exceeded')
                # read1 performs at most one buffered raw read; reset its deadline.
                transport.settimeout(remaining)
                chunk = response.read1(min(65536, self.MAX_RESPONSE_BYTES + 1 - total))
                if not chunk:
                    break
                chunks.append(chunk)
                total += len(chunk)
                if total > self.MAX_RESPONSE_BYTES:
                    raise RpcError('provider response exceeds its bound')
            if length is not None and total != int(length):
                raise RpcError('provider response ended before its declared length')
            return strict_json(b''.join(chunks).decode('utf-8'))
        except RpcError:
            raise
        except (OSError, ValueError, UnicodeError, http.client.HTTPException, AttributeError, RecursionError):
            # Never interpolate an upstream error, target, URL or provider credential.
            raise RpcError('HTTPS provider transport or JSON protocol refused') from None
        finally:
            if watchdog is not None:
                watchdog.cancel()
            if connection is not None:
                connection.close()

    def batch(self, requests):
        if not isinstance(requests, (list, tuple)) or not 1 <= len(requests) <= self.MAX_BATCH_ITEMS:
            raise RpcError('read-only batch count exceeds its bound')
        wanted = []
        for request in requests:
            if not isinstance(request, (list, tuple)) or len(request) != 2:
                raise RpcError('batch request must contain method and parameters')
            method, params = request
            wanted.append((method, self._validate(method, copy.deepcopy(params))))
        calls = [('eth_chainId', []), ('eth_getBlockByNumber', ['0x0', False])] + wanted
        with self._lock:
            ids = list(range(self._counter + 1, self._counter + len(calls) + 1))
            self._counter += len(calls)
        body = [{'jsonrpc': '2.0', 'id': id_, 'method': method, 'params': params}
                for id_, (method, params) in zip(ids, calls)]
        try:
            encoded = json.dumps(body, allow_nan=False, separators=(',', ':')).encode('utf-8')
        except (TypeError, ValueError):
            raise RpcError('read-only request is not canonical JSON') from None
        if len(encoded) > self.MAX_REQUEST_BYTES:
            raise RpcError('read-only request exceeds its bound')
        reply = self._exchange(encoded)
        if not isinstance(reply, list) or len(reply) != len(ids):
            raise RpcError('provider batch reply count differs')
        results = {}
        for item in reply:
            if (not isinstance(item, dict) or set(item) != {'jsonrpc', 'id', 'result'} or
                    item['jsonrpc'] != '2.0' or type(item['id']) is not int or
                    item['id'] not in ids or item['id'] in results):
                raise RpcError('provider batch identity/error/protocol refused')
            results[item['id']] = item['result']
        if quantity(results[ids[0]]) != self._chain_id:
            raise ProviderIdentityError('provider differs from the pinned Arbitrum chain identity')
        genesis = results[ids[1]]
        if (not isinstance(genesis, dict) or quantity(genesis.get('number')) != 0 or
                hex_value(genesis.get('hash'), 32) != self._genesis_hash):
            raise ProviderIdentityError('provider differs from the pinned Arbitrum genesis')
        return [results[id_] for id_ in ids[2:]]

    def call(self, method, params=()):
        return self.batch([(method, params)])[0]
