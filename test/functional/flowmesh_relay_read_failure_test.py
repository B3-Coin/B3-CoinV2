#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Offline relay body-read failure accounting regression.

Execute the real handler, server cleanup, and healthy/fault report finalization.
Only stream/socket/server boundaries are mocked; no sockets, nodes, or builds.
Run directly or discover alongside flowmesh_public_trace_test.py with unittest.
"""
from collections import Counter
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

import feature_flowmesh_performance as performance
import feature_flowmesh_performance_faults as faults
from test_framework import flowmesh_client_tls as relay_module
from test_framework.flowmesh_public_trace import PublicBodyCapture


class ReadFailureAccountingTests(unittest.TestCase):
    secret = "PRIVATE_EXCEPTION_DETAIL_MUST_NOT_APPEAR"

    def subject(self, temporary, fault=False, capture=True):
        cls = faults.FlowMeshPerformanceFaultsTest if fault else performance.FlowMeshPerformanceTest
        subject = object.__new__(cls)
        subject.options = SimpleNamespace(performance_public_trace=capture, tmpdir=temporary)
        subject.performance_report = {
            "samples": [], "windows": [], "correctness_pass": True,
            "performance_pass": True, "all_harness_threads_stopped": True}
        if fault:
            subject.performance_report["fault_scenario_pass"] = True
        subject.http_requests, subject.rpc_calls, subject.submit_records = [], [], []
        subject.http_trace_bytes = subject.http_trace_dropped = 0
        subject.rpc_trace_dropped = subject.rpc_trace_bytes = 0
        subject.relay_method_counts = Counter()
        subject.trace_drain_lock = threading.Lock()
        subject.log = Mock()
        relay = object.__new__(relay_module.FlowMeshTLSFaultRelay)
        relay.lock, relay.server = threading.Lock(), Mock()
        relay.public_capture = PublicBodyCapture(Path(temporary, "capture")) if capture else None
        relay.url = "mocked-generated-endpoint"
        relay.active_handlers = relay.record_bytes = relay.records_dropped = 0
        relay.capture_admission_closed = relay.unavailable = relay.drop_submit_once = False
        relay.reply_mutation, relay.response_hold_ms, relay.requests = None, {}, []
        subject.tls_relays = [relay]
        filename = "flowmesh-performance-faults.json" if fault else "flowmesh-performance.json"
        return subject, relay, filename

    def handler(self, relay, original):
        handler = object.__new__(relay_module._RelayHandler)
        handler.server = SimpleNamespace(relay=relay)
        handler.path = relay_module.API_PATH
        handler.headers = {"Content-Length": "17"}
        handler.rfile = Mock()
        handler.rfile.read.side_effect = original
        handler.reply = Mock()
        return handler

    def drive_server_cleanup(self, relay, original):
        """Real handler + outer cleanup; fake transport performs no socket IO."""
        handler = self.handler(relay, original)
        server = object.__new__(relay_module._BoundedTLSServer)
        server.relay = relay
        server.context = Mock()
        request = Mock()
        server.context.wrap_socket.return_value = request
        server.finish_request = Mock(side_effect=lambda *_: handler.do_POST())
        server.shutdown_request = Mock()
        server.slots = threading.BoundedSemaphore(1)
        server.slots.acquire()
        relay.active_handlers = 1
        with patch.object(relay_module.http.client, "HTTPSConnection") as upstream:
            server.process_request_thread(request, ("127.0.0.1", 1))
        upstream.assert_not_called()
        handler.reply.assert_not_called()
        handler.rfile.read.assert_called_once_with(17)
        server.shutdown_request.assert_called_once_with(request)
        self.assertEqual(relay.active_handlers, 0)
        self.assertTrue(server.slots.acquire(blocking=False))
        self.assertFalse(server.slots.acquire(blocking=False))
        self.assertEqual(relay.requests, [])

    def assert_saved_failure(self, subject, temporary, filename, fault):
        path = Path(temporary, filename)
        self.assertTrue(path.is_file(), "failure report must precede final assertion")
        saved = json.loads(path.read_text())
        self.assertTrue(saved["public_trace_finalization"]["quiescent"])
        self.assertFalse(saved["public_trace_finalization"]["timed_out"])
        self.assertFalse(saved["public_trace"]["complete_capture"])
        self.assertTrue(saved["public_trace"]["finalized"])
        self.assertFalse(saved["correctness_pass"])
        self.assertFalse(saved["performance_pass"])
        if fault:
            self.assertFalse(saved["fault_scenario_pass"])
        self.assertEqual(saved["public_trace"]["captures"][0]["failure_count"], 1)
        self.assertEqual(saved["http_requests"], [])
        self.assertEqual(saved["public_trace"]["relay_progress"][0]["active_handlers"], 0)
        self.assertEqual(saved["public_trace"]["relay_progress"][0]["pending_records"], 0)
        self.assertNotIn(self.secret, path.read_text())
        subject.prepare_public_trace_report()
        self.assertFalse(subject.performance_report["public_trace"]["complete_capture"])
        self.assertFalse(subject.performance_report["correctness_pass"])

    def test_body_read_oserrors_preserve_exception_and_record_public_type_only(self):
        for error_type in (TimeoutError, ConnectionResetError):
            with self.subTest(error=error_type.__name__), TemporaryDirectory() as temporary:
                _, relay, _ = self.subject(temporary)
                original = error_type(self.secret)
                handler = self.handler(relay, original)
                with patch.object(relay_module.http.client, "HTTPSConnection") as upstream:
                    with self.assertRaises(error_type) as caught:
                        handler.do_POST()
                self.assertIs(caught.exception, original)
                handler.rfile.read.assert_called_once_with(17)
                handler.reply.assert_not_called()
                upstream.assert_not_called()
                self.assertEqual(relay.requests, [])
                saved = relay.public_capture.snapshot()
                self.assertEqual(saved["failure_count"], 1)
                self.assertFalse(saved["complete_capture"])
                self.assertEqual(saved["errors"], [{
                    "request_id": None, "stage": "request",
                    "error": "request body read failed: " + error_type.__name__}])
                self.assertNotIn(self.secret, json.dumps(saved))

    def test_non_oserror_does_not_broaden_the_new_catch(self):
        with TemporaryDirectory() as temporary:
            _, relay, _ = self.subject(temporary)
            original = ValueError(self.secret)
            handler = self.handler(relay, original)
            with self.assertRaises(ValueError) as caught:
                handler.do_POST()
            self.assertIs(caught.exception, original)
            self.assertEqual(relay.public_capture.snapshot()["failure_count"], 0)
            handler.reply.assert_not_called()

    def test_trace_disabled_preserves_both_original_oserrors(self):
        for error_type in (TimeoutError, ConnectionResetError):
            with self.subTest(error=error_type.__name__), TemporaryDirectory() as temporary:
                _, relay, _ = self.subject(temporary, capture=False)
                original = error_type(self.secret)
                handler = self.handler(relay, original)
                with self.assertRaises(error_type) as caught:
                    handler.do_POST()
                self.assertIs(caught.exception, original)
                handler.reply.assert_not_called()
                self.assertEqual(relay.requests, [])

    def test_server_cleanup_is_quiescent_but_capture_failure_stays_sticky(self):
        for error_type in (TimeoutError, ConnectionResetError):
            with self.subTest(error=error_type.__name__), TemporaryDirectory() as temporary:
                _, relay, _ = self.subject(temporary)
                self.drive_server_cleanup(relay, error_type(self.secret))
                self.assertEqual(relay.public_capture.snapshot()["failure_count"], 1)
                self.assertFalse(relay.public_capture.snapshot()["complete_capture"])

    def check_final_report(self, fault):
        for error_type in (TimeoutError, ConnectionResetError):
            with self.subTest(error=error_type.__name__, fault=fault), TemporaryDirectory() as temporary:
                subject, relay, filename = self.subject(temporary, fault=fault)
                self.drive_server_cleanup(relay, error_type(self.secret))
                # Actual finalizer must write JSON before it raises exit failure.
                with self.assertRaisesRegex(AssertionError, "failure report preserved"):
                    subject.finish_report(False, filename, "OFFLINE_TEST_REPORT")
                self.assert_saved_failure(subject, temporary, filename, fault)

    def test_healthy_final_report_is_written_before_exit_assertion(self):
        self.check_final_report(fault=False)

    def test_fault_final_report_is_written_before_exit_assertion(self):
        self.check_final_report(fault=True)

    def test_primary_read_exception_survives_actual_report_finalization(self):
        for fault in (False, True):
            for error_type in (TimeoutError, ConnectionResetError):
                with self.subTest(fault=fault, error=error_type.__name__), TemporaryDirectory() as temporary:
                    subject, relay, filename = self.subject(temporary, fault=fault)
                    original = error_type(self.secret)
                    handler = self.handler(relay, original)
                    def exercise():
                        try:
                            handler.do_POST()
                        finally:
                            subject.finish_report(sys.exc_info()[0] is not None,
                                                  filename, "OFFLINE_TEST_REPORT")
                    with self.assertRaises(error_type) as caught:
                        exercise()
                    self.assertIs(caught.exception, original)
                    self.assert_saved_failure(subject, temporary, filename, fault)


if __name__ == "__main__":
    unittest.main(verbosity=2)
