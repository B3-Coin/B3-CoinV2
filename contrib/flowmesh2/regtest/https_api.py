#!/usr/bin/env python3
"""Bounded TLS client API for a serialized, independently validating operator.

Peer traffic never enters this API. Orders authenticate with their native
BIP340 ActionV2 signature; read replies retain the native certificate/proof.
No TLS certificate or live deployment is provisioned automatically here.
"""
import hashlib
import hmac
import http.client
import ipaddress
import json
import os
from pathlib import Path
import select
import socket
import socketserver
import ssl
import struct
import threading
import time
from urllib.parse import parse_qs, urlsplit


class HttpsApiError(RuntimeError):
    def __init__(self, status, detail):
        self.status = status
        super().__init__(detail)


def _unique(pairs):
    out = {}
    for key, value in pairs:
        if key in out:
            raise ValueError('duplicate JSON field')
        out[key] = value
    return out


def _pin(certificate):
    return hashlib.sha256(ssl.PEM_cert_to_DER_cert(certificate.read_text(encoding='ascii'))).hexdigest()


def _origin(value):
    if not isinstance(value, str) or len(value) > 256 or any(ord(c) <= 32 or ord(c) == 127 for c in value):
        raise ValueError('expected a bounded HTTPS origin')
    parsed = urlsplit(value)
    if (parsed.scheme != 'https' or not parsed.hostname or parsed.username is not None or
            parsed.password is not None or parsed.path not in ('', '/') or parsed.query or parsed.fragment or
            parsed.port == 0 or '?' in value or '#' in value):
        raise ValueError('expected an explicit HTTPS origin')
    return parsed


class _Server(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True
    block_on_close = False

    def process_request(self, request, client_address):
        if not self.gateway.connections.acquire(blocking=False):
            request.close()
            return
        try:
            super().process_request(request, client_address)
        except Exception:
            self.gateway.connections.release()
            request.close()
            raise


class _Handler(socketserver.BaseRequestHandler):
    def handle(self):
        gateway = self.server.gateway
        connection = None
        try:
            self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.request.settimeout(gateway.header_timeout)
            connection = gateway.context.wrap_socket(self.request, server_side=True)
            # A client that asks for keep-alive may send its next request on
            # this connection after a 200 reply. Every request is read, budgeted
            # and validated exactly as a first one; an idle or closed connection
            # ends silently after header_timeout; the per-connection cap bounds
            # how long one client holds a connection slot.
            for served in range(gateway.keep_alive_requests):
                if served and not connection.pending():
                    if not select.select([connection], [], [], gateway.header_timeout)[0]:
                        break
                if not gateway.handle(connection, self.client_address[0],
                                      keep_alive=served + 1 < gateway.keep_alive_requests):
                    break
        except (OSError, ValueError, ssl.SSLError):
            pass
        finally:
            if connection is not None:
                connection.close()
            self.server.gateway.connections.release()


class HttpsGateway:
    def __init__(self, config, dispatch):
        required = {'listen_address', 'listen_port', 'certificate', 'private_key', 'certificate_sha256'}
        optional = {'allowed_origins', 'max_connections', 'requests_per_minute', 'max_body_bytes',
                    'header_timeout_seconds', 'stream_seconds'}
        if not isinstance(config, dict) or not required <= set(config) or set(config) - required - optional:
            raise ValueError('incomplete or unknown HTTPS API configuration')
        host = str(ipaddress.ip_address(config['listen_address']))
        port = config['listen_port']
        if type(port) is not int or not 0 <= port <= 65535:
            raise ValueError('invalid HTTPS port')
        certificate = Path(config['certificate']).resolve(strict=True)
        key = Path(config['private_key'])
        if key.is_symlink() or not key.is_file() or not certificate.is_file():
            raise ValueError('HTTPS identity must use explicit regular files')
        if os.name != 'nt' and key.stat().st_mode & 0o077:
            raise ValueError('HTTPS private key permissions are too broad')
        self.certificate_sha256 = _pin(certificate)
        if not hmac.compare_digest(self.certificate_sha256, config['certificate_sha256']):
            raise ValueError('HTTPS certificate differs from its configured identity')
        capacity = config.get('max_connections', 16)
        rate = config.get('requests_per_minute', 1200)
        self.max_body = config.get('max_body_bytes', 32768)
        self.header_timeout = config.get('header_timeout_seconds', 3)
        self.stream_seconds = config.get('stream_seconds', 10)
        self.keep_alive_requests = 100
        if any(type(value) is not int for value in (capacity, rate, self.max_body, self.header_timeout, self.stream_seconds)):
            raise ValueError('HTTPS bounds must be integers')
        if not (4 <= capacity <= 128 and 10 <= rate <= 100000 and 1024 <= self.max_body <= 1048576 and
                1 <= self.header_timeout <= 10 and 1 <= self.stream_seconds <= 30):
            raise ValueError('HTTPS bounds exceed the release policy')
        origins = config.get('allowed_origins', [])
        if not isinstance(origins, list) or len(origins) > 16:
            raise ValueError('HTTPS origins require an explicit bounded allowlist')
        for origin in origins:
            if _origin(origin).path:
                raise ValueError('allowlisted origin must omit its path')
        self.origins, self.rate = set(origins), rate
        self.dispatch = dispatch
        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.minimum_version = ssl.TLSVersion.TLSv1_3
        self.context.maximum_version = ssl.TLSVersion.TLSv1_3
        self.context.load_cert_chain(str(certificate), str(key.resolve(strict=True)))
        self.connections = threading.BoundedSemaphore(capacity)
        self.streams = threading.BoundedSemaphore(max(1, capacity // 4))
        self.lock = threading.Lock()
        self.clients = {}
        self.stopped = threading.Event()
        server_type = _Server
        if ':' in host:
            server_type = type('_V6Server', (_Server,), {'address_family': socket.AF_INET6})
        self.server = server_type((host, port), _Handler)
        self.server.gateway = self
        self.address = self.server.server_address[:2]
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': .1}, daemon=True)

    def start(self):
        self.thread.start()
        return self

    def info(self):
        host, port = self.address
        return {'url': 'https://' + ('[' + host + ']' if ':' in host else host) + ':' + str(port),
                'certificate_sha256': self.certificate_sha256, 'tls': '1.3'}

    def close(self):
        self.stopped.set()
        if self.thread.is_alive():
            self.server.shutdown()
            self.thread.join(timeout=2)
        self.server.server_close()

    def _budget(self, address):
        now = time.monotonic()
        with self.lock:
            expired = [key for key, value in self.clients.items() if now - value[0] >= 60]
            for key in expired:
                del self.clients[key]
            if address not in self.clients:
                if len(self.clients) >= 4096:
                    return False
                self.clients[address] = [now, 0]
            value = self.clients[address]
            value[1] += 1
            return value[1] <= self.rate

    def _read_request(self, connection):
        deadline = time.monotonic() + self.header_timeout
        data = bytearray()
        while b'\r\n\r\n' not in data:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or len(data) >= 16384:
                raise HttpsApiError(431, 'request headers exceed their time/size limit')
            connection.settimeout(remaining)
            part = connection.recv(min(4096, 16384 - len(data)))
            if not part:
                raise HttpsApiError(400, 'incomplete request')
            data.extend(part)
        head, body = bytes(data).split(b'\r\n\r\n', 1)
        lines = head.decode('ascii').split('\r\n')
        if len(lines) > 65 or any('\x00' in line for line in lines):
            raise HttpsApiError(431, 'too many or invalid request headers')
        pieces = lines[0].split(' ')
        if (len(pieces) != 3 or pieces[2] != 'HTTP/1.1' or len(pieces[1]) > 4096 or
                not pieces[1].startswith('/') or any(ord(c) <= 32 or ord(c) == 127 for c in pieces[1])):
            raise HttpsApiError(400, 'expected a bounded HTTP/1.1 origin request')
        method, target = pieces[:2]
        headers = {}
        for line in lines[1:]:
            name, separator, value = line.partition(':')
            if not separator or not name or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-' for c in name):
                raise HttpsApiError(400, 'invalid request header')
            name = name.lower()
            if name in headers or any((ord(c) < 32 and c != '\t') or ord(c) == 127 for c in value):
                raise HttpsApiError(400, 'duplicate or invalid request header')
            headers[name] = value.strip()
        if 'host' not in headers or 'transfer-encoding' in headers or 'expect' in headers:
            raise HttpsApiError(400, 'ambiguous request framing is refused')
        length = headers.get('content-length', '0')
        if not length.isascii() or not length.isdecimal() or len(length) > 8:
            raise HttpsApiError(400, 'invalid content length')
        length = int(length)
        if length > self.max_body:
            raise HttpsApiError(413, 'request body exceeds its configured limit')
        if method != 'POST' and length:
            raise HttpsApiError(400, 'body not allowed for this method')
        if method == 'POST' and ('content-length' not in headers or headers.get('content-type') != 'application/json'):
            raise HttpsApiError(415, 'POST requires length-delimited application/json')
        if len(body) > length:
            raise HttpsApiError(400, 'pipelined or conflicting request framing is refused')
        while len(body) < length:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise HttpsApiError(408, 'request body timed out')
            connection.settimeout(remaining)
            part = connection.recv(min(4096, length - len(body)))
            if not part:
                raise HttpsApiError(400, 'incomplete body')
            body += part
        return method, target, headers, body

    def _write(self, connection, status, value, origin=None, keep=False):
        payload = json.dumps(value, sort_keys=True, separators=(',', ':')).encode()
        if len(payload) > 1048576:
            status, payload, keep = 500, b'{"error":"native response exceeded API bound"}', False
        keep = keep and status == 200
        headers = [f'HTTP/1.1 {status} Response', 'Content-Type: application/json',
                   'Content-Length: ' + str(len(payload)), 'Connection: ' + ('keep-alive' if keep else 'close'),
                   'Cache-Control: no-store',
                   'X-Content-Type-Options: nosniff']
        if origin:
            headers += ['Access-Control-Allow-Origin: ' + origin, 'Vary: Origin',
                        'Access-Control-Allow-Methods: GET, POST, OPTIONS', 'Access-Control-Allow-Headers: Content-Type']
        connection.settimeout(2)
        connection.sendall(('\r\n'.join(headers) + '\r\n\r\n').encode() + payload)
        return keep

    def _events(self, connection, after, origin):
        if not self.streams.acquire(blocking=False):
            raise HttpsApiError(429, 'event stream capacity reached')
        try:
            headers = ['HTTP/1.1 200 OK', 'Content-Type: text/event-stream', 'Cache-Control: no-store', 'Connection: close']
            if origin:
                headers += ['Access-Control-Allow-Origin: ' + origin, 'Vary: Origin']
            connection.settimeout(2)
            connection.sendall(('\r\n'.join(headers) + '\r\n\r\nretry: 200\n\n').encode())
            deadline = time.monotonic() + self.stream_seconds
            while not self.stopped.is_set() and time.monotonic() < deadline:
                info = self.dispatch('info', {})
                height = int(info['height'])
                if height > after:
                    proof = self.dispatch('totals', {})
                    header = bytes.fromhex(proof['header'].removeprefix('0x'))
                    if len(header) != 366:
                        raise ValueError('invalid native certified state envelope')
                    actual = struct.unpack_from('<Q', header, 73)[0]
                    event = ('id: ' + str(actual) + '\nevent: certified-state\ndata: ' +
                             json.dumps(proof, sort_keys=True, separators=(',', ':')) + '\n\n').encode()
                    if len(event) > 1048576:
                        raise ValueError('event exceeds API bound')
                    connection.sendall(event)
                    after = actual
                self.stopped.wait(.1)
        except Exception:
            # Headers are already sent: terminate this stream; never append a
            # second HTTP response to a successful event-stream envelope.
            return
        finally:
            self.streams.release()

    def handle(self, connection, address, keep_alive=False):
        """Answer one request; True only if a 200 reply kept the connection open."""
        origin = None
        try:
            if not self._budget(address):
                raise HttpsApiError(429, 'client request budget exhausted')
            method, target, headers, body = self._read_request(connection)
            keep = keep_alive and headers.get('connection', '').lower() == 'keep-alive'
            supplied = headers.get('origin')
            if supplied is not None:
                if supplied not in self.origins:
                    raise HttpsApiError(403, 'origin is not allowed')
                origin = supplied
            parsed = urlsplit(target)
            if parsed.netloc or parsed.fragment:
                raise HttpsApiError(400, 'invalid origin request target')
            if method == 'OPTIONS':
                return self._write(connection, 200, {'allowed': True}, origin, keep)
            if method == 'GET' and parsed.path == '/v1/events':
                query = parse_qs(parsed.query, strict_parsing=True)
                if set(query) - {'after-height'} or len(query.get('after-height', ['0'])) != 1:
                    raise HttpsApiError(400, 'invalid event cursor')
                raw = query.get('after-height', ['0'])[0]
                if not raw.isdecimal() or len(raw) > 20 or int(raw) >= 1 << 64:
                    raise HttpsApiError(400, 'invalid event cursor')
                self._events(connection, int(raw), origin)
                return False
            if parsed.query:
                raise HttpsApiError(400, 'unexpected query parameters')
            if method == 'POST' and parsed.path == '/v1/submit':
                value = json.loads(body.decode('utf-8'), object_pairs_hook=_unique,
                                   parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite JSON')))
                if not isinstance(value, dict) or set(value) != {'frame'} or not isinstance(value['frame'], str):
                    raise HttpsApiError(400, 'submit requires one signed action frame')
                frame = value['frame'].removeprefix('0x')
                if not frame or len(frame) % 2 or any(c not in '0123456789abcdef' for c in frame):
                    raise HttpsApiError(400, 'signed action frame must be canonical hex')
                result = self.dispatch('submit', value)
                if not isinstance(result, dict) or result.get('code') not in ('ADMITTED', 'DUPLICATE'):
                    self._write(connection, 422, {'error': 'signed action refused', 'result': result}, origin)
                    return False
            elif method == 'GET' and parsed.path in ('/v1/info', '/v1/totals'):
                result = self.dispatch(parsed.path.rsplit('/', 1)[1], {})
            elif method == 'GET' and parsed.path.startswith(('/v1/account/', '/v1/withdrawal/')):
                kind, identifier = parsed.path.split('/')[2:]
                if len(identifier) != 64 or any(c not in '0123456789abcdef' for c in identifier):
                    raise HttpsApiError(400, 'proof identifier must be canonical 32-byte hex')
                result = self.dispatch(kind, {'account' if kind == 'account' else 'withdrawal_id': identifier})
            else:
                raise HttpsApiError(404, 'public API route is not available')
            return self._write(connection, 200, {'result': result}, origin, keep)
        except HttpsApiError as error:
            self._write(connection, error.status, {'error': str(error)}, origin)
        except (UnicodeError, ValueError, json.JSONDecodeError):
            self._write(connection, 400, {'error': 'malformed public API input'}, origin)
        except Exception:
            # Never echo a private worker request/config or an operator secret.
            self._write(connection, 503, {'error': 'operator refused the request or is unavailable'}, origin)
        return False


class HttpsClient:
    """Pinned TLS 1.3 client.

    With keep_alive (the default) each thread reuses one pinned connection
    while the gateway agrees and the connection was idle under IDLE_SECONDS,
    which is below the gateway's smallest header_timeout. The pin is checked
    on every new connection before any request is sent. A failed request is
    never retried, and any reply other than a complete keep-alive 200 closes
    the connection.
    """
    IDLE_SECONDS = 0.5

    def __init__(self, url, certificate, certificate_sha256, keep_alive=True, clock=time.monotonic):
        parsed = _origin(url)
        self.host, self.port = parsed.hostname, parsed.port or 443
        self.context = ssl.create_default_context(cafile=str(certificate))
        self.context.minimum_version = ssl.TLSVersion.TLSv1_3
        self.pin = certificate_sha256
        self.keep_alive, self.clock = keep_alive, clock
        self.local = threading.local()

    def _connection(self):
        local = self.local
        connection = getattr(local, 'connection', None)
        if connection is not None and self.clock() - local.last_used > self.IDLE_SECONDS:
            self._drop()
            connection = None
        if connection is None:
            connection = http.client.HTTPSConnection(self.host, self.port, context=self.context, timeout=10)
            local.connection = connection
            connection.connect()
            actual = hashlib.sha256(connection.sock.getpeercert(binary_form=True)).hexdigest()
            if not hmac.compare_digest(actual, self.pin):
                raise HttpsApiError(495, 'HTTPS server certificate pin differs')
        return connection

    def _drop(self):
        connection = getattr(self.local, 'connection', None)
        self.local.connection = None
        if connection is not None:
            connection.close()

    def request(self, method, path, value=None, headers=None):
        reusable = False
        try:
            connection = self._connection()
            body = None if value is None else json.dumps(value, separators=(',', ':')).encode()
            fields = {'Connection': 'keep-alive' if self.keep_alive else 'close', **(headers or {})}
            if body is not None:
                fields['Content-Type'] = 'application/json'
            connection.request(method, path, body=body, headers=fields)
            response = connection.getresponse()
            data = response.read(1048577)
            reusable = (fields['Connection'] == 'keep-alive' and response.status == 200 and
                        response.isclosed() and not response.will_close)
            if len(data) > 1048576:
                raise HttpsApiError(502, 'HTTPS response exceeds client bound')
            result = json.loads(data)
            if response.status != 200:
                raise HttpsApiError(response.status, result.get('error', 'HTTPS request refused'))
            return result['result'] if 'result' in result else result
        except BaseException:
            reusable = False
            raise
        finally:
            if reusable:
                self.local.last_used = self.clock()
            else:
                self._drop()


def create_local_tls(directory):
    """Generate a fresh local test certificate; never replace an existing key."""
    import subprocess
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    certificate, key = directory / 'public-api.crt', directory / 'private-api.key'
    if certificate.exists() != key.exists():
        raise ValueError('incomplete original TLS identity; refusing key reset')
    if not certificate.exists():
        previous = os.umask(0o077)
        try:
            subprocess.run(['openssl', 'req', '-x509', '-newkey', 'ec', '-pkeyopt', 'ec_paramgen_curve:P-256',
                            '-noenc', '-days', '2', '-subj', '/CN=fm2-local-operator',
                            '-addext', 'subjectAltName=IP:127.0.0.1,IP:::1',
                            # A server certificate, not a CA: rustls/webpki (B3 Hive)
                            # refuses a CA certificate presented by a server.
                            '-addext', 'basicConstraints=critical,CA:FALSE',
                            '-keyout', str(key), '-out', str(certificate)], check=True, capture_output=True)
        finally:
            os.umask(previous)
    return {'listen_address': '127.0.0.1', 'listen_port': 0, 'certificate': str(certificate.resolve()),
            'private_key': str(key.resolve()), 'certificate_sha256': _pin(certificate),
            'requests_per_minute': 10000}
