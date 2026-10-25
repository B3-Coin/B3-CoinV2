#!/usr/bin/env python3
"""Keep-alive HTTP transport for the harness's loopback JSON-RPC clients.

`LocalRpc` (EVM) and `B3Rpc` (b3coind) used urllib, which opens one TCP
connection per request. The custody and source guards issue dozens of fresh
batches per slot, so the connection setup was a measured share of the
trading latency (task 1.4). `KeepAliveOpener` replaces only the transport:

- every request is still sent and every reply still read and validated by
  the caller, so no guard reads a cached answer;
- the opener is bound to one loopback URL and never uses a proxy;
- any status outside 2xx raises `urllib.error.HTTPError` and drops the
  connection; a redirect is never followed;
- a connection is reused only after its reply was read completely and the
  server did not ask to close it, and only within `idle_seconds`, well under
  the servers' idle timeouts;
- a failed request is never retried, because a JSON-RPC call may mutate the
  local chain; the caller sees the same transport error as before;
- each thread has its own connection.
"""
import http.client
import threading
import time
import urllib.error
import urllib.parse


class KeepAliveOpener:
    """Drop-in for the `opener.open(request, timeout=...)` use in the RPC clients."""

    IDLE_SECONDS = 1.0

    def __init__(self, url, idle_seconds=IDLE_SECONDS, clock=time.monotonic):
        parts = urllib.parse.urlsplit(url)
        if parts.scheme != 'http' or not parts.hostname or parts.port is None:
            raise ValueError('keep-alive opener needs an explicit http://host:port URL')
        if idle_seconds <= 0:
            raise ValueError('idle_seconds must be positive')
        self.url = url
        self.host, self.port = parts.hostname, parts.port
        self.path = (parts.path or '/') + ('?' + parts.query if parts.query else '')
        self.idle_seconds = idle_seconds
        self.clock = clock
        self.local = threading.local()
        self.connects = 0
        self.connects_lock = threading.Lock()

    def open(self, request, timeout):
        if request.full_url != self.url:
            raise ValueError('keep-alive opener is bound to one RPC URL')
        connection = self._connection(timeout)
        try:
            connection.request(request.get_method(), self.path, body=request.data,
                               headers=dict(request.header_items()))
            response = connection.getresponse()
        except http.client.HTTPException as error:
            self._drop()
            raise urllib.error.URLError(error) from error
        except BaseException:
            self._drop()
            raise
        if not 200 <= response.status < 300:
            self._drop()
            raise urllib.error.HTTPError(self.url, response.status, response.reason, response.headers, None)
        return _Reply(self, response)

    def _connection(self, timeout):
        local = self.local
        connection = getattr(local, 'connection', None)
        if connection is not None and self.clock() - local.last_used > self.idle_seconds:
            self._drop()
            connection = None
        if connection is None:
            connection = http.client.HTTPConnection(self.host, self.port, timeout=timeout)
            local.connection = connection
            with self.connects_lock:
                self.connects += 1
        elif connection.timeout != timeout:
            connection.timeout = timeout
            if connection.sock is not None:
                connection.sock.settimeout(timeout)
        return connection

    def _drop(self):
        connection = getattr(self.local, 'connection', None)
        self.local.connection = None
        if connection is not None:
            connection.close()

    def _release(self, response, failed):
        if failed or not response.isclosed() or response.will_close:
            self._drop()
        else:
            self.local.last_used = self.clock()


class _Reply:
    """The part of urllib's response object the RPC clients use."""

    def __init__(self, owner, response):
        self.owner, self.response = owner, response

    def read(self, amount):
        try:
            return self.response.read(amount)
        except http.client.HTTPException as error:
            raise urllib.error.URLError(error) from error

    def __enter__(self):
        return self

    def __exit__(self, kind, value, traceback):
        self.owner._release(self.response, kind is not None)
        return False
