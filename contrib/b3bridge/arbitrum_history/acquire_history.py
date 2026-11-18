#!/usr/bin/env python3
"""Fetch opaque content-addressed objects, never RPC results or chain authority.

This is an explicitly B3-specific raw object exchange, NOT an Arbitrum/Nitro
peer protocol. A real archive/replay producer must offer the bytes. HTTP/TLS,
matching peer hashes, and availability provenance do not verify chain state.
"""
from __future__ import annotations

import argparse
import hashlib
import http.client
import ipaddress
import os
from pathlib import Path
import socket
import ssl
import threading
import time
from urllib.parse import urlsplit
import zlib

from history_store import (HEX, HistoryError, HistoryStore, canonical, default_policy,
                           digest, require, strict_json)

MAX_OBJECT = 16 * 1024 * 1024
PREFIX = "/b3/history/v1/objects/sha256/"
USER_AGENT = "B3-Untrusted-Raw-History/1"


class RawObjectClient:
    """Read-only GET of one explicitly requested SHA256 object at a time.

    Public HTTPS: DNS answers must all be public, and the chosen address is
    pinned for the TCP connection while original hostname SNI/CA checks remain.
    The only HTTP exception is literal IPv4 loopback, explicitly fixture-only.
    """
    def __init__(self, base_url: str, *, allow_loopback=False, timeout=10):
        require(type(base_url) is str and len(base_url) <= 2048 and
                all(0x21 <= ord(c) <= 0x7e for c in base_url), "Invalid raw history origin")
        require(type(allow_loopback) is bool and type(timeout) in (int, float) and
                0 < timeout <= 30, "Invalid raw history transport bound")
        try:
            parsed = urlsplit(base_url)
            port = parsed.port
        except ValueError as error:
            raise HistoryError("Invalid raw history origin") from error
        require(parsed.path in ("", "/") and not parsed.query and not parsed.fragment and
                parsed.username is None and parsed.password is None and parsed.hostname,
                "Raw history requires an origin without credentials/path/query")
        require(parsed.hostname == parsed.hostname.lower() and len(parsed.hostname) <= 253,
                "Invalid raw history hostname")
        fixture = allow_loopback and parsed.scheme == "http" and parsed.hostname == "127.0.0.1"
        require(fixture or parsed.scheme == "https", "Public raw history requires HTTPS")
        require(port is None or 1 <= port <= 65535, "Invalid raw history port")
        self.host, self.port = parsed.hostname, port or (80 if fixture else 443)
        self.fixture, self.timeout = fixture, float(timeout)
        self.origin = f"{parsed.scheme}://{self.host}:{self.port}"
        self.peer_id = "sha256:" + digest(self.origin.encode("ascii"))
        self._fetch_lock = threading.Lock()
        self._resolver_fenced = False

    def _addresses(self, deadline):
        if self.fixture:
            return [(socket.AF_INET, ("127.0.0.1", self.port))]
        require(not self._resolver_fenced, "Raw history resolver is fenced")
        result = []
        def resolve():
            try:
                result.append(socket.getaddrinfo(self.host, self.port, type=socket.SOCK_STREAM))
            except OSError:
                result.append(None)
        # The OS resolver can outlive a request. At most one daemon resolver
        # exists per client; after a timeout this client permanently refuses.
        resolver = threading.Thread(target=resolve, daemon=True)
        resolver.start()
        resolver.join(max(0, deadline - time.monotonic()))
        if resolver.is_alive():
            self._resolver_fenced = True
            raise HistoryError("Raw history DNS deadline exceeded")
        require(result and result[0] is not None, "Raw history DNS failed")
        answers = result[0]
        require(0 < len(answers) <= 64, "Invalid raw history DNS answer count")
        addresses = []
        for family, socktype, protocol, name, address in answers:
            require(family in (socket.AF_INET, socket.AF_INET6), "Unsupported raw history address")
            ip = ipaddress.ip_address(address[0])
            require(ip.is_global and not ip.is_multicast and not ip.is_unspecified,
                    "Raw history DNS contains a nonpublic address")
            addresses.append((family, address))
        return sorted(set(addresses), key=str)

    def _connection(self, deadline, active):
        # Do not call a URL opener: redirects, environment proxies, cookies and
        # credentials have no role in this fixed raw-byte protocol.
        addresses = self._addresses(deadline)
        class Http(http.client.HTTPConnection):
            def connect(inner):
                super().connect()
                active.append(inner.sock)
        class Https(http.client.HTTPSConnection):
            def connect(inner):
                super().connect()
                active.append(inner.sock)
        if self.fixture:
            connection = Http(self.host, self.port, timeout=self.timeout)
        else:
            context = ssl.create_default_context()
            context.minimum_version = ssl.TLSVersion.TLSv1_2
            connection = Https(self.host, self.port, timeout=self.timeout, context=context)

        def connect_pinned(_address, timeout=None, source_address=None):
            require(source_address is None, "Unexpected raw history source binding")
            last_error = None
            # All addresses were resolved ONCE and validated as public above.
            # An unavailable IPv6 route must not suppress a working IPv4 peer.
            # This retries TCP only; TLS still verifies the ORIGINAL hostname
            # with the same CA/SNI policy, without re-resolution or redirection.
            for family, address in addresses:
                sock = None
                try:
                    remaining = deadline - time.monotonic()
                    require(remaining > 0, "Raw history deadline exceeded")
                    sock = socket.socket(family, socket.SOCK_STREAM)
                    sock.settimeout(min(self.timeout, remaining))
                    sock.connect(address)
                    remaining = deadline - time.monotonic()
                    require(remaining > 0, "Raw history deadline exceeded")
                    sock.settimeout(min(self.timeout, remaining))
                    return sock
                except OSError as error:
                    last_error = error
                    if sock is not None:
                        sock.close()
                except BaseException:
                    if sock is not None:
                        sock.close()
                    raise
            raise HistoryError("Raw history connection unavailable") from last_error
        connection._create_connection = connect_pinned
        return connection

    def fetch(self, sha256: str, expected_size: int) -> bytes:
        require(self._fetch_lock.acquire(blocking=False), "Raw history client already has a request")
        try:
            return self._fetch(sha256, expected_size)
        finally:
            self._fetch_lock.release()

    def _fetch(self, sha256: str, expected_size: int) -> bytes:
        require(type(sha256) is str and HEX.fullmatch(sha256), "Invalid requested raw object hash")
        require(type(expected_size) is int and 1 <= expected_size <= MAX_OBJECT,
                "Invalid requested raw object size")
        raw = self._exchange(PREFIX + sha256, expected_size, expected_size=expected_size)
        require(digest(raw) == sha256, "Raw object hash mismatch")
        return raw

    def _exchange(self, path, maximum, *, expected_size=None, feed_json=False, gzip_body=False):
        # Callers choose only the two fixed upstream feed routes or CAS route;
        # no caller-supplied arbitrary URL, method, headers or request body.
        require(path.startswith(PREFIX) or path == "/feed/v1/info" or
                path.startswith("/feed/v1/chunk/") and path[len("/feed/v1/chunk/"):].isdigit(),
                "Unsupported raw history path")
        deadline = time.monotonic() + self.timeout
        active = []
        connection = self._connection(deadline, active)
        expired = threading.Event()
        def expire():
            expired.set()
            for transport in active:
                try:
                    transport.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
        # Enforce a total transport deadline even if a malicious peer slowly
        # drips headers or body bytes faster than a per-read socket timeout.
        timer = threading.Timer(max(0, deadline - time.monotonic()), expire)
        timer.daemon = True
        timer.start()
        try:
            connection.request("GET", path, headers={
                "Accept": "application/json" if feed_json else "application/octet-stream",
                "Accept-Encoding": "gzip" if gzip_body else "identity",
                "User-Agent": USER_AGENT, "Connection": "close"})
            response = connection.getresponse()
            require(response.status == 200, "Raw history object unavailable")
            headers = response.getheaders()
            def values(name):
                return [value for key, value in headers if key.lower() == name]
            accepted_types = (["application/json"], ["application/json; charset=utf-8"]) if feed_json else (
                ["application/octet-stream"],)
            require(values("content-type") in accepted_types, "Wrong raw object content type")
            lengths, transfer = values("content-length"), values("transfer-encoding")
            chunked = feed_json and transfer == ["chunked"] and not lengths
            length = None
            if not chunked:
                require(not transfer and len(lengths) == 1 and lengths[0].isdigit() and
                        len(lengths[0]) <= 9 and lengths[0] == str(int(lengths[0])) and
                        1 <= int(lengths[0]) <= maximum, "Wrong or ambiguous raw object length")
                length = int(lengths[0])
            require(expected_size is None or length == expected_size, "Wrong raw object length")
            require(values("content-encoding") == (["gzip"] if gzip_body else []) and
                    not values("trailer"), "Unsupported raw object encoding/trailers")
            output = bytearray()
            while length is None or len(output) < length:
                remaining = deadline - time.monotonic()
                require(remaining > 0, "Raw history deadline exceeded")
                # HTTPResponse may close the SSL socket on the final length.
                # Check only while a read is needed, never after EOF/length0.
                if connection.sock is not None:
                    connection.sock.settimeout(min(self.timeout, remaining))
                chunk = response.read1(min(65536, maximum + 1 - len(output)))
                if not chunk:
                    require(length is None or len(output) == length, "Truncated raw object")
                    break
                output.extend(chunk)
                require(len(output) <= maximum, "Raw object body exceeds its bound")
            raw = bytes(output)
            require(not expired.is_set() and time.monotonic() <= deadline and
                    (length is None or len(raw) == length), "Raw object length or deadline mismatch")
            return raw
        except (OSError, http.client.HTTPException, ssl.SSLError) as error:
            raise HistoryError("Raw history transport refused") from error
        finally:
            timer.cancel()
            timer.join()
            connection.close()

    def acquire(self, store: HistoryStore, kind: str, sha256: str, expected_size: int) -> dict:
        # Reject object policy/quota failures BEFORE contacting an untrusted peer.
        descriptor = store._descriptor({"sha256": sha256, "size": expected_size, "kind": kind})
        if sha256 in store.entries:
            require(store.entries[sha256] == descriptor, "Existing history descriptor conflict")
            store.get(sha256)
            return dict(descriptor)
        require(store.sequence < store.policy["max_objects"] and
                store.total_bytes + expected_size <= store.policy["max_total_bytes"], "History quota exceeded")
        raw = self.fetch(sha256, expected_size)
        return store.put(kind, raw, {"transport": "b3-raw-object-http/1", "peer_id": self.peer_id})


class NitroFeedBacklogClient(RawObjectClient):
    """Actual Nitro v3.12.1 REST-backfill wire protocol, unverified soft data.

    Upstream: broadcastclient/backfill.go at
    70fa99a6e711f09eedd96a907290c3df0f3bcf7d. This client deliberately does
    NOT emulate Nitro's sequencer signer verifier or accept its result as
    canonical execution. Feed signatures and blockHash remain untrusted bytes.
    """
    def fetch_info(self):
        require(self._fetch_lock.acquire(blocking=False), "Raw history client already has a request")
        try:
            raw = self._exchange("/feed/v1/info", 16384, feed_json=True)
        finally:
            self._fetch_lock.release()
        info = strict_json(raw)
        require(type(info) is dict and set(info) == {"chainId", "feedVersion", "chunkSize"} and
                type(info["chainId"]) is int and info["chainId"] == 42161 and
                type(info["feedVersion"]) is int and info["feedVersion"] == 1 and
                type(info["chunkSize"]) is int and 1 <= info["chunkSize"] <= 10000,
                "Wrong Nitro backlog chain/version/chunk bound")
        return info

    def fetch_chunk(self, start, chunk_size):
        from feed_history import MAX_FRAME, MAX_UINT64, parse_feed_frame
        require(type(start) is int and type(chunk_size) is int and 1 <= chunk_size <= 10000 and
                0 <= start <= MAX_UINT64 - chunk_size and start % chunk_size == 0,
                "Explicit aligned Nitro backlog range required")
        require(self._fetch_lock.acquire(blocking=False), "Raw history client already has a request")
        try:
            compressed = self._exchange("/feed/v1/chunk/" + str(start), MAX_FRAME,
                                        feed_json=True, gzip_body=True)
        finally:
            self._fetch_lock.release()
        try:
            decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
            raw = decoder.decompress(compressed, MAX_FRAME + 1)
            require(len(raw) <= MAX_FRAME and decoder.eof and not decoder.unused_data and
                    not decoder.unconsumed_tail, "Invalid, concatenated or oversized Nitro backlog gzip")
        except zlib.error as error:
            raise HistoryError("Invalid Nitro backlog gzip") from error
        value = parse_feed_frame(raw, max_messages=10000)
        rows = value.get("messages", [])
        require(len(rows) == chunk_size and rows[0]["sequenceNumber"] == start and
                rows[-1]["sequenceNumber"] == start + chunk_size - 1 and
                value.get("confirmedSequenceNumberMessage") is None,
                "Nitro backlog chunk is missing or substitutes requested messages")
        return raw

    def acquire_chunk(self, store, start, chunk_size):
        require(self.fetch_info()["chunkSize"] == chunk_size, "Nitro backlog chunk policy changed")
        raw = self.fetch_chunk(start, chunk_size)
        descriptor = store.put("sequencer_feed_json", raw,
                               {"transport": "nitro-sequencer-feed-unverified/1", "peer_id": self.peer_id})
        return {"object": descriptor, "first_sequence": start, "message_count": chunk_size,
                "soft_only": True, "native_execution_verified": False,
                "arbitrum_authority": False, "mainnet_enabled": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--origin-env", required=True,
                        help="Name of an existing environment variable holding the raw HTTPS origin")
    parser.add_argument("--kind", required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--size", required=True, type=int)
    parser.add_argument("--create", action="store_true", help="Create a new store; existing paths refuse")
    args = parser.parse_args()
    # Never accept an endpoint/RPC result dump or source-authority flag.
    require(args.origin_env.startswith("B3_HISTORY_") and
            args.origin_env.replace("_", "").isalnum() and args.origin_env.isupper(),
            "Explicit B3_HISTORY_ origin variable required")
    origin = os.environ.get(args.origin_env)
    require(origin is not None, "Raw history origin variable is missing")
    require(not args.policy.is_symlink() and args.policy.stat().st_size <= 32768,
            "Invalid history policy file")
    policy = strict_json(args.policy.read_bytes())
    client = RawObjectClient(origin)
    with HistoryStore(args.store, policy, create=args.create) as store:
        result = client.acquire(store, args.kind, args.sha256, args.size)
        print(canonical({"object": result, "availability": store.stats(),
                         "client_json_rpc_requests": 0, "history_producer_provenance_verified": False,
                         "native_proof_verified": False, "arbitrum_authority": False,
                         "mainnet_enabled": False}).decode())


if __name__ == "__main__":
    try:
        main()
    except (HistoryError, OSError, KeyError, TypeError):
        # Origin credentials/private paths are never echoed in public errors.
        raise SystemExit("Raw history acquisition refused; no source authority granted")
