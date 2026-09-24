# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Bounded, test-only HTTPS fault relay for the public FlowMesh client API.

TLS terminates twice using one ephemeral regtest CA. The sole upstream is a
dedicated loopback trading API, never the wallet/admin RPC listener. Only
public request bodies are retained; this helper has no wallet credentials.
"""

import hashlib
import http.client
import http.server
import json
import re
import shutil
import socket
import ssl
import subprocess
import threading
import time
from pathlib import Path

from test_framework.flowmesh_public_trace import PublicBodyCapture, public_request_context, public_response_context


API_PATH = "/flowmesh/v1"
MAX_REQUEST_BYTES = 1024 * 1024
MAX_REPLY_BYTES = 32 * 1024 * 1024
MAX_REQUEST_RECORDS = 4096
MAX_REQUEST_RECORD_BYTES = 4 * 1024 * 1024
MAX_CONNECTIONS = 8
MAX_RESPONSE_HOLD_MS = 6000
RESPONSE_HOLD_METHODS = frozenset({"markets", "snapshot", "updates", "action", "submit"})
MAX_RECORDED_BODY_BYTES = 8192
MAX_PUBLIC_CONTEXT_BYTES = 64 * 1024


def validate_response_holds(value):
    if not isinstance(value, dict) or set(value) - RESPONSE_HOLD_METHODS:
        raise ValueError("Only bounded public client methods can be response-held")
    if any(type(delay) is not int or not 0 <= delay <= MAX_RESPONSE_HOLD_MS for delay in value.values()):
        raise ValueError("Response hold must be an integer in 0..6000 milliseconds")
    return dict(value)


def expire_action_receipt(method, body, action_id):
    """Existing receipt-ring-expiry fault, limited to one action/status reply."""
    if method != "action" or action_id is None:
        return body
    reply = json.loads(body)
    receipt = reply.get("result") if reply.get("ok") else None
    if not isinstance(receipt, dict) or receipt.get("action_id") != action_id:
        return body
    receipt.update(receipt_state="unknown", accepted=False, certificate_verified=False,
                   outcome_verified=False, reason="Synthetic endpoint receipt-ring expiry")
    for key in ("certified_payload", "microblock_hash", "microblock_sequence"):
        receipt.pop(key, None)
    return json.dumps(reply, separators=(",", ":")).encode()


def create_test_pki(directory):
    """Generate only fresh TLS fixture keys, beneath the regtest tempdir."""
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=False)
    openssl = shutil.which("openssl")
    if openssl is None:
        raise RuntimeError("openssl is required to generate isolated FlowMesh HTTPS test certificates")
    ca, ca_key = directory / "ca.pem", directory / "ca-key.pem"
    cert, key, csr = directory / "server.pem", directory / "server-key.pem", directory / "server.csr"
    ca_config = directory / "ca.cnf"
    # Config-file extensions also work with the system LibreSSL CLI on Macs
    # where the newer OpenSSL req -addext option is unavailable.
    ca_config.write_text("[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=v3_ca\n"
                         "[dn]\nCN=FlowMesh isolated regtest CA\n[v3_ca]\n"
                         "basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\n", encoding="utf-8")
    extension = directory / "server.ext"
    extension.write_text("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
                         "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:localhost,IP:127.0.0.1\n", encoding="utf-8")

    def run(*args):
        subprocess.run([openssl, *map(str, args)], check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, timeout=30)

    run("req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes", "-days", "2",
        "-config", ca_config, "-keyout", ca_key, "-out", ca)
    run("req", "-new", "-newkey", "rsa:2048", "-sha256", "-nodes",
        "-subj", "/CN=localhost", "-keyout", key, "-out", csr)
    run("x509", "-req", "-in", csr, "-CA", ca, "-CAkey", ca_key, "-CAcreateserial",
        "-days", "2", "-sha256", "-extfile", extension, "-out", cert)
    ca_key.chmod(0o600)
    key.chmod(0o600)
    return {"ca": ca, "certificate": cert, "key": key}


class _BoundedTLSServer(http.server.ThreadingHTTPServer):
    # Shutdown joins at most eight handlers, each with a ten-second socket
    # deadline, rather than leaving relay threads behind the functional run.
    daemon_threads = False
    block_on_close = True

    def __init__(self, address, context, relay):
        self.context = context
        self.relay = relay
        self.slots = threading.BoundedSemaphore(MAX_CONNECTIONS)
        super().__init__(address, _RelayHandler)

    def process_request(self, request, client_address):
        with self.relay.lock:
            if self.relay.capture_admission_closed or not self.slots.acquire(blocking=False):
                request.close()
                return
            # Includes TLS negotiation and malformed/incomplete HTTP bodies,
            # before a public request row exists. Finalization must await these
            # handlers too, not merely rows already in the archive.
            self.relay.active_handlers += 1
        try:
            super().process_request(request, client_address)
        except Exception:
            with self.relay.lock:
                self.relay.active_handlers -= 1
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        secured = request
        try:
            request.settimeout(10)
            secured = self.context.wrap_socket(request, server_side=True)
            self.finish_request(secured, client_address)
        except (OSError, ssl.SSLError, TimeoutError):
            pass
        finally:
            try:
                self.shutdown_request(secured)
            finally:
                with self.relay.lock:
                    self.relay.active_handlers -= 1
                self.slots.release()

    def handle_error(self, *_):
        # Deliberately broken TLS/HTTP peers are evidence recorded by the
        # caller, not unbounded traceback noise from a daemon helper thread.
        pass


class _RelayHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def reply(self, status, body, content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def do_POST(self):
        relay = self.server.relay
        def incomplete(reason):
            if relay.public_capture is not None:
                relay.public_capture.note_failure(reason, stage="request")
        if self.path != API_PATH or self.headers.get("Transfer-Encoding") is not None:
            incomplete("refused path or transfer encoding; request body not captured")
            self.reply(404, b'{"ok":false,"error":"test relay path refused"}')
            return
        try:
            length = int(self.headers.get("Content-Length", "-1"))
        except ValueError:
            length = -1
        if not 0 <= length <= MAX_REQUEST_BYTES:
            incomplete("request size refused; body not captured")
            self.reply(413, b'{"ok":false,"error":"test relay request bound"}')
            return
        body = self.rfile.read(length)
        if len(body) != length:
            incomplete("incomplete request body")
            return
        try:
            request = json.loads(body)
        except ValueError:
            incomplete("invalid request JSON")
            self.reply(400, b'{"ok":false,"error":"test relay JSON refused"}')
            return
        if not isinstance(request, dict) or not isinstance(request.get("params"), dict):
            incomplete("invalid request shape")
            self.reply(400, b'{"ok":false,"error":"test relay request shape"}')
            return
        method = request.get("method")
        with relay.lock:
            unavailable = relay.unavailable
            drop = method == "submit" and relay.drop_submit_once
            if drop:
                relay.drop_submit_once = False
            mutation = relay.reply_mutation
            hold_ms = relay.response_hold_ms.get(method, 0)
            hold_release = relay.hold_release
            expired_action = relay.receipt_expired_action_id
            row = {"host_monotonic_us": time.monotonic_ns() // 1000, "method": method,
                   "body_sha256": hashlib.sha256(body).hexdigest(), "forwarded": False,
                   "action_id": request["params"].get("action_id"),
                   "action_hex": request["params"].get("action_hex"), "response_dropped": False,
                   "record_index": len(relay.requests), "body_bytes": len(body)}
            row["request_id"] = relay.next_request_id
            row["public_capture_enabled"] = relay.public_capture is not None
            relay.next_request_id += 1
            if len(body) <= MAX_RECORDED_BODY_BYTES:
                row["body_hex"] = body.hex()
            else:
                row["body_hex_omitted_for_size"] = True
            # Reserve also the bounded completion flags/status/timestamp that
            # are appended to this same retained row after forwarding.
            row["completion_reservation_bytes"] = MAX_PUBLIC_CONTEXT_BYTES + 4096 if relay.public_capture else 1024
            row_bytes = len(json.dumps(row).encode("utf-8")) + row["completion_reservation_bytes"]
            if len(relay.requests) < MAX_REQUEST_RECORDS and relay.record_bytes + row_bytes <= MAX_REQUEST_RECORD_BYTES:
                relay.requests.append(row)
                relay.record_bytes += row_bytes
            else:
                relay.records_dropped += 1
        relay.record_public_request(row, request, body)
        if unavailable:
            try:
                error_body = b'{"ok":false,"error":"isolated test endpoint unavailable"}'
                relay.record_public_response(row, "relay_error", 503, error_body)
                self.reply(503, error_body)
            finally:
                with relay.lock:
                    row["handler_completed_us"] = time.monotonic_ns() // 1000
            return
        upstream = http.client.HTTPSConnection("127.0.0.1", relay.upstream_port, timeout=10,
                                               context=relay.client_context)
        try:
            with relay.lock:
                row["upstream_started_us"] = time.monotonic_ns() // 1000
            upstream.request("POST", API_PATH, body=body, headers={"Content-Type": "application/json"})
            with relay.lock:
                row["forwarded"] = True
            response = upstream.getresponse()
            result = response.read(MAX_REPLY_BYTES + 1)
            if len(result) > MAX_REPLY_BYTES:
                error_body = b'{"ok":false,"error":"test relay reply bound"}'
                if relay.public_capture is not None:
                    relay.public_capture.note_failure("upstream response exceeds forwarding/capture bound",
                                                      request_id=row["request_id"], stage="upstream")
                relay.record_public_response(row, "relay_error", 502, error_body)
                self.reply(502, error_body)
                return
            with relay.lock:
                row["upstream_http_status"] = response.status
                row["upstream_completed_us"] = time.monotonic_ns() // 1000
                if drop:
                    row["response_dropped"] = True
                    relay.unavailable = True
            relay.record_public_response(row, "upstream", response.status, result)
            relay.wait_response_hold(row, hold_ms, hold_release)
            if drop:
                # The upstream completed; the trader sees an ambiguous close.
                # It must retain/retry the exact instruction, not sign again.
                self.close_connection = True
                self.connection.shutdown(socket.SHUT_RDWR)
                self.connection.close()
                return
            if mutation is not None:
                result = mutation(method, result)
                if len(result) > MAX_REPLY_BYTES:
                    error_body = b'{"ok":false,"error":"test mutation reply bound"}'
                    relay.record_public_response(row, "relay_error", 502, error_body)
                    self.reply(502, error_body)
                    return
            if expired_action is not None:
                changed = expire_action_receipt(method, result, expired_action)
                with relay.lock:
                    row["receipt_expired_mutation"] = changed != result
                result = changed
            relay.record_public_response(row, "delivered", response.status, result)
            with relay.lock:
                row["client_response_attempted_us"] = time.monotonic_ns() // 1000
            self.reply(response.status, result)
            with relay.lock:
                row["client_response_completed_us"] = time.monotonic_ns() // 1000
        except (OSError, ssl.SSLError, TimeoutError, http.client.HTTPException) as error:
            with relay.lock:
                row["transport_error"] = type(error).__name__
            self.close_connection = True
        finally:
            upstream.close()
            with relay.lock:
                row["handler_completed_us"] = time.monotonic_ns() // 1000


class FlowMeshTLSFaultRelay:
    def __init__(self, port, upstream_port, pki):
        self.upstream_port = upstream_port
        self.lock = threading.Lock()
        self.requests = []
        self.records_dropped = 0
        self.record_bytes = 0
        self.next_request_id = 0
        self.public_capture = None
        self.active_handlers = 0
        self.capture_admission_closed = False
        self.unavailable = False
        self.drop_submit_once = False
        self.reply_mutation = None
        self.response_hold_ms = {}
        self.receipt_expired_action_id = None
        self.hold_release = threading.Event()
        self.stopping = False
        self.client_context = ssl.create_default_context(cafile=str(pki["ca"]))
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(str(pki["certificate"]), str(pki["key"]))
        self.server = _BoundedTLSServer(("127.0.0.1", port), context, self)
        self.port = self.server.server_address[1]
        self.url = f"https://127.0.0.1:{self.port}"
        self.thread = threading.Thread(target=self.server.serve_forever,
                                       kwargs={"poll_interval": .1}, daemon=True)
        self.thread.start()

    def enable_public_capture(self, directory):
        """Opt-in generated-fixture trace, without headers or wallet RPC data."""
        capture = PublicBodyCapture(directory)
        with self.lock:
            assert self.public_capture is None
            self.public_capture = capture

    def record_public_request(self, row, request, body):
        capture = self.public_capture
        if capture is None:
            return
        metadata = capture.capture(row["request_id"], "request", body)
        context = public_request_context(request)
        if len(json.dumps(context).encode("utf-8")) > MAX_PUBLIC_CONTEXT_BYTES // 3:
            context = {"inline_context_omitted_for_size": True, "full_body_complete": metadata["complete"]}
        with self.lock:
            row["request_context"] = context
            row["request_body"] = metadata

    def record_public_response(self, row, stage, status, body):
        capture = self.public_capture
        if capture is None:
            return
        digest = hashlib.sha256(body).hexdigest()
        if stage == "delivered" and row.get("upstream_body", {}).get("sha256") == digest:
            metadata = dict(row["upstream_body"])
            metadata["identical_to_upstream"] = True
        else:
            metadata = capture.capture(row["request_id"], stage, body)
        context = public_response_context(body)
        # Full bytes remain in the bounded artifact. Oversized projections
        # are explicit instead of overflowing the in-memory record budget.
        if len(json.dumps(context).encode("utf-8")) > MAX_PUBLIC_CONTEXT_BYTES // 3:
            context = {"inline_context_omitted_for_size": True, "full_body_complete": metadata["complete"]}
        with self.lock:
            row[stage + "_body"] = metadata
            row[stage + "_context"] = context
            row[stage + "_http_status"] = status
            if stage in {"delivered", "relay_error"}:
                row["client_http_status"] = status

    def configure(self, *, unavailable=False, drop_submit_once=False, reply_mutation=None,
                  response_hold_ms=None, receipt_expired_action_id=None):
        holds = validate_response_holds({} if response_hold_ms is None else response_hold_ms)
        if receipt_expired_action_id is not None and (
                not isinstance(receipt_expired_action_id, str) or
                re.fullmatch(r"[0-9a-f]{64}", receipt_expired_action_id) is None):
            raise ValueError("Receipt-expiry fault requires one exact public action ID")
        with self.lock:
            self.hold_release.set()  # Clear/reconfigure releases already-held replies.
            self.hold_release = threading.Event()
            if self.stopping:
                self.hold_release.set()
            self.unavailable = unavailable
            self.drop_submit_once = drop_submit_once
            self.reply_mutation = reply_mutation
            self.response_hold_ms = holds
            self.receipt_expired_action_id = receipt_expired_action_id

    def wait_response_hold(self, row, milliseconds, release):
        if not milliseconds:
            return
        # Called only after upstream forwarding completed. No wallet/RPC work
        # and no relay lock are held during this strictly bounded test wait.
        with self.lock:
            row["response_hold_ms"] = milliseconds
            row["response_hold_started_us"] = time.monotonic_ns() // 1000
        released = release.wait(milliseconds / 1000)
        with self.lock:
            row["response_hold_completed_us"] = time.monotonic_ns() // 1000
            row["response_hold_released"] = released

    def snapshot(self):
        with self.lock:
            result = {"url": self.url, "unavailable": self.unavailable,
                    "requests": [dict(row) for row in self.requests], "records_dropped": self.records_dropped,
                    "record_bytes": self.record_bytes, "response_hold_ms": dict(self.response_hold_ms),
                    "receipt_expired_action_id": self.receipt_expired_action_id,
                    "active_handlers": self.active_handlers,
                    "capture_admission_closed": self.capture_admission_closed}
        if self.public_capture is not None:
            result["public_capture"] = self.public_capture.snapshot()
        return result

    def begin_public_capture_finalization(self):
        """Close admission after workload cleanup; existing handlers still finish.

        Do not use server_close here: its thread joins have no aggregate
        deadline. The harness separately waits a bounded interval, records any
        survivors, then leaves ordinary fixture shutdown to close the server.
        """
        with self.lock:
            self.capture_admission_closed = True
        self.server.shutdown()

    def public_capture_progress(self):
        with self.lock:
            result = {"endpoint": self.url, "admission_closed": self.capture_admission_closed,
                      "active_handlers": self.active_handlers,
                      "pending_records": sum("handler_completed_us" not in row for row in self.requests),
                      "unarchived_records": len(self.requests)}
        capture = self.public_capture
        result["writes_in_progress"] = capture.snapshot()["writes_in_progress"] if capture else 0
        return result

    def stop(self):
        with self.lock:
            self.stopping = True
            self.hold_release.set()
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=2)
