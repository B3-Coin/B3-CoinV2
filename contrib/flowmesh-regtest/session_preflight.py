#!/usr/bin/env python3
"""Bounded regtest session check, not consensus or full release qualification.

The historical embedded profile is immutable: changing its bytes changes the
storage identity. This separate, profile-bound window records its existing
limits without silently renewing availability. TLS probing is explicit and
read-only; it does not query a wallet or send a trading instruction.
"""
import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import socket
import ssl
from urllib.parse import urlsplit


HERE = Path(__file__).resolve().parent
MAX_WINDOW_BYTES = 4096
MIN_REMAINING = timedelta(hours=1)


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "Duplicate session-window field")
        result[key] = value
    return result


def utc(value):
    require(isinstance(value, str) and
            re.fullmatch(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z", value),
            "Session-window timestamps must be exact UTC seconds ending in Z")
    try:
        return datetime.strptime(value, "%Y-%m-%dT%H:%M:%SZ").replace(tzinfo=timezone.utc)
    except ValueError as error:
        raise RuntimeError("Invalid session-window calendar timestamp") from error


def parse_window(raw, profile_raw):
    require(0 < len(raw) <= MAX_WINDOW_BYTES, "Missing or oversized session window")
    try:
        window = json.loads(raw, object_pairs_hook=unique_object)
        profile = json.loads(profile_raw)
    except (ValueError, UnicodeError) as error:
        raise RuntimeError("Invalid session-window/profile JSON") from error
    require(isinstance(window, dict) and set(window) == {
        "format", "profile_id", "profile_sha256", "maintenance_ends_at", "recorded_leaf_expires_at"},
        "Unsupported session-window fields")
    require(window["format"] == "b3-regtest-session-window-v1", "Wrong session-window format")
    require(isinstance(profile, dict) and window["profile_id"] == profile.get("profile_id") and
            window["profile_sha256"] == hashlib.sha256(profile_raw).hexdigest(),
            "Session window does not match the exact embedded profile")
    require(utc(window["maintenance_ends_at"]) <= utc(window["recorded_leaf_expires_at"]),
            "Maintenance window cannot outlive the recorded TLS leaf")
    return window


def check_window(window, now):
    require(isinstance(now, datetime) and now.utcoffset() == timedelta(0), "UTC check time required")
    end = utc(window["maintenance_ends_at"])
    leaf_end = utc(window["recorded_leaf_expires_at"])
    require(now < end, "Test-session maintenance window has ended; coordinator renewal required")
    require(end - now >= MIN_REMAINING, "Less than one hour remains in the test-session window")
    require(leaf_end - now >= MIN_REMAINING, "Recorded TLS leaf has less than one hour remaining")


def probe_tls(profile):
    """One verified handshake with the validated allowlisted endpoint; no HTTP."""
    endpoint = urlsplit(profile["https_endpoints"][0])
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.load_verify_locations(cadata=profile["ca_pem"])
    with socket.create_connection((endpoint.hostname, endpoint.port or 443), timeout=5) as connection:
        with context.wrap_socket(connection, server_hostname=endpoint.hostname) as secure:
            certificate = secure.getpeercert()
            return {
                "leaf_sha256": hashlib.sha256(secure.getpeercert(binary_form=True)).hexdigest(),
                "expires_at": datetime.fromtimestamp(
                    ssl.cert_time_to_seconds(certificate["notAfter"]), timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            }


def approved_inputs():
    # Reuse the packaging allowlist, not an arbitrary command-line URL/CA.
    spec = importlib.util.spec_from_file_location("regtest_package_preflight", HERE / "package.py")
    package = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(package)
    raw = package.PROFILE_PATH.read_bytes()
    profile = package.validate_profile(raw, package.CA_PATH.read_bytes())
    return raw, profile


def inspect_session(profile_raw, window_raw, now, tls_observation=None):
    window = parse_window(window_raw, profile_raw)
    check_window(window, now)
    if tls_observation is not None:
        require(tls_observation["expires_at"] == window["recorded_leaf_expires_at"],
                "Observed TLS expiry differs from the recorded leaf; review the session window")
        require(utc(tls_observation["expires_at"]) - now >= MIN_REMAINING,
                "Verified TLS leaf has less than one hour remaining")
    return window


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe-tls", action="store_true",
                        help="Perform one read-only verified TLS handshake to the fixed approved endpoint")
    args = parser.parse_args()
    now = datetime.now(timezone.utc)
    result = {"format": "b3-regtest-session-check-v1", "checked_at": now.strftime("%Y-%m-%dT%H:%M:%SZ"),
              "full_release_qualified": False, "tls_probed": False}
    try:
        raw, profile = approved_inputs()
        result["profile_id"] = profile["profile_id"]
        # Refuse an expired declaration before making any network connection.
        window_raw = (HERE / "session-window.json").read_bytes()
        inspect_session(raw, window_raw, now)
        if args.probe_tls:
            result["tls"] = probe_tls(profile)
            result["tls_probed"] = True
            inspect_session(raw, window_raw, datetime.now(timezone.utc), result["tls"])
        result["status"] = "WINDOW_VALID_TLS_VERIFIED" if args.probe_tls else "WINDOW_VALID_TLS_UNCHECKED"
        result["message"] = "Session check only: chain, quorum, funds, GUI and release qualification remain separate"
        code = 0
    except (RuntimeError, OSError, ValueError, KeyError) as error:
        result["status"] = "NOT_READY"
        result["message"] = str(error)
        code = 1
    print(json.dumps(result, sort_keys=True))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
