#!/usr/bin/env python3
"""Synthetic dates/transport only; no node, wallet, or external connection."""
import copy
from datetime import timedelta
import importlib.util
import json
from pathlib import Path
import unittest
from unittest import mock

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("regtest_session", HERE / "session_preflight.py")
SESSION = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SESSION)


class SessionTests(unittest.TestCase):
    def setUp(self):
        self.raw = (HERE / "profile-vps-20260928.json").read_bytes()
        self.window_raw = (HERE / "session-window.json").read_bytes()
        self.window = SESSION.parse_window(self.window_raw, self.raw)
        self.now = SESSION.utc("2026-10-10T00:00:00Z")  # Synthetic, not the live clock.

    def test_profile_bytes_and_storage_identity_are_preserved(self):
        self.assertEqual(self.window["profile_sha256"],
                         "de4061353cdcc4e94079638a582328f9ea5978df9cc1dd6f4b61fc5152e20f09")
        self.assertEqual(SESSION.inspect_session(self.raw, self.window_raw, self.now), self.window)

    def test_expired_and_exact_expiry_refused(self):
        end = SESSION.utc(self.window["maintenance_ends_at"])
        for now in (end, end + timedelta(seconds=1), end + timedelta(days=2)):
            with self.subTest(now=now), self.assertRaisesRegex(RuntimeError, "window has ended"):
                SESSION.check_window(self.window, now)

    def test_one_hour_distribution_margin(self):
        end = SESSION.utc(self.window["maintenance_ends_at"])
        SESSION.check_window(self.window, end - timedelta(hours=1))
        with self.assertRaisesRegex(RuntimeError, "Less than one hour"):
            SESSION.check_window(self.window, end - timedelta(seconds=3599))

    def test_changed_profile_or_identity_refused(self):
        for key, value in (("profile_sha256", "0" * 64), ("profile_id", "other"),
                           ("format", "unknown")):
            changed = copy.deepcopy(self.window)
            changed[key] = value
            with self.subTest(key=key), self.assertRaises(RuntimeError):
                SESSION.parse_window(json.dumps(changed).encode(), self.raw)
        with self.assertRaisesRegex(RuntimeError, "exact embedded profile"):
            SESSION.parse_window(self.window_raw, self.raw + b"\n")

    def test_malformed_oversized_duplicate_and_extra_fields_refused(self):
        changed = {**self.window, "approve_anyway": True}
        values = (b"", b"x" * 4097, b"{", b"[]", json.dumps(changed).encode(),
                  self.window_raw.rstrip()[:-1] + b', "format": "b3-regtest-session-window-v1"}')
        for raw in values:
            with self.subTest(raw=raw[:60]), self.assertRaises(RuntimeError):
                SESSION.parse_window(raw, self.raw)

    def test_noncanonical_times_and_invalid_calendar_refused(self):
        for value in ("2026-10-11", "2026-10-11T00:00:00+00:00", "2026-02-30T00:00:00Z", 1, None):
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                SESSION.utc(value)
        with self.assertRaisesRegex(RuntimeError, "UTC"):
            SESSION.check_window(self.window, self.now.replace(tzinfo=None))

    def test_window_cannot_outlive_leaf(self):
        beyond_leaf = SESSION.utc(self.window["recorded_leaf_expires_at"]) + timedelta(seconds=1)
        changed = {**self.window, "maintenance_ends_at": beyond_leaf.strftime("%Y-%m-%dT%H:%M:%SZ")}
        with self.assertRaisesRegex(RuntimeError, "cannot outlive"):
            SESSION.parse_window(json.dumps(changed).encode(), self.raw)

    def test_tls_expiry_mismatch_is_not_accepted(self):
        observed = {"expires_at": self.window["recorded_leaf_expires_at"]}
        SESSION.inspect_session(self.raw, self.window_raw, self.now, observed)
        observed["expires_at"] = "2026-11-12T14:59:29Z"
        with self.assertRaisesRegex(RuntimeError, "differs"):
            SESSION.inspect_session(self.raw, self.window_raw, self.now, observed)

    def test_probe_requires_ca_and_hostname_verification(self):
        _, profile = SESSION.approved_inputs()
        context = SESSION.ssl.SSLContext(SESSION.ssl.PROTOCOL_TLS_CLIENT)
        with mock.patch.object(SESSION.ssl, "SSLContext", return_value=context) as create_context, \
             mock.patch.object(SESSION.socket, "create_connection", side_effect=OSError("synthetic unreachable")):
            with self.assertRaisesRegex(OSError, "synthetic unreachable"):
                SESSION.probe_tls(profile)
            create_context.assert_called_once_with(SESSION.ssl.PROTOCOL_TLS_CLIENT)
        # This is the actual context configured inside probe_tls, not a separate default context.
        self.assertTrue(context.check_hostname)
        self.assertEqual(context.verify_mode, SESSION.ssl.CERT_REQUIRED)
        self.assertEqual(context.cert_store_stats()["x509_ca"], 1)

    def test_cli_expired_refuses_before_network_and_never_claims_release(self):
        with mock.patch.object(SESSION, "probe_tls") as probe, \
             mock.patch.object(SESSION, "datetime") as clock, \
             mock.patch.object(SESSION, "inspect_session", side_effect=RuntimeError("window has ended")), \
             mock.patch("sys.argv", ["session_preflight.py", "--probe-tls"]), \
             mock.patch("builtins.print") as printed:
            clock.now.return_value = self.now
            self.assertEqual(SESSION.main(), 1)
            probe.assert_not_called()
            result = json.loads(printed.call_args.args[0])
            self.assertEqual(result["status"], "NOT_READY")
            self.assertFalse(result["full_release_qualified"])


if __name__ == "__main__":
    unittest.main()
