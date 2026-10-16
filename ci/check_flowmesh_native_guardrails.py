#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Strict, bounded Linux native-test guardrails; never a performance gate.

Version native-guardrails-v11 selects exact compiled Boost suites, not CTest's
skip-on-empty filters. Discovery and detailed XML must agree on every selected
leaf case. TSan covers instrumented native code, not all system libraries; the
existing repository suppression file includes a ConnectTip deadlock suppression.
DEBUG_LOCKORDER observes Bitcoin lock wrappers, not every plain std::mutex.
Neither check proves absence of stalls, physical power-loss safety or authority.

Run only against a fresh, dedicated CI build. Tests use generated private data;
do not upload its work directory, fixture wallets, keys, journals or debug logs.
"""

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shlex
import signal
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET


PROFILE_VERSION = "native-guardrails-v11"
CONTROL_TIMEOUT = 30
SUITE_TIMEOUT = 600
TOTAL_TEST_TIMEOUT = 1800
MAX_OUTPUT_BYTES = 8 * 1024 * 1024

# No prototype/uncommitted source, Qt, live node or performance workload is used.
SUITE_SOURCES = {
    "flowmesh_p2p_tests": "src/test/flowmesh_p2p_tests.cpp",
    "flowmesh_runtime_tests": "src/test/flowmesh_runtime_tests.cpp",
    "flowmesh_agreement_tests": "src/test/flowmesh_agreement_tests.cpp",
    "flowmesh_production_store_tests": "src/test/flowmesh_production_store_tests.cpp",
    "flowmesh_service_tests": "src/test/flowmesh_service_tests.cpp",
    "flowmesh_transport_policy_tests": "src/test/flowmesh_service_tests.cpp",
    "flowmesh_service_startup_tests": "src/test/flowmesh_service_tests.cpp",
    "flowmesh_https_tests": "src/test/flowmesh_https_tests.cpp",
    "flowmesh_client_connect_tests": "src/test/flowmesh_https_tests.cpp",
    "flowmesh_client_wait_lane_tests": "src/test/flowmesh_https_tests.cpp",
    "flowmesh_client_submit_wait_tests": "src/test/flowmesh_https_tests.cpp",
    "flowmesh_client_outcome_evidence_tests": "src/test/flowmesh_https_tests.cpp",
    "flowmesh_client_watch_lane_tests": "src/test/flowmesh_https_tests.cpp",
    "flowmesh_client_market_view_tests": "src/test/flowmesh_https_tests.cpp",
    "flowmesh_api_watch_tests": "src/test/flowmesh_https_tests.cpp",
    "flowmesh_api_outcome_tests": "src/test/flowmesh_https_tests.cpp",
    "flowmesh_api_health_tests": "src/test/flowmesh_https_tests.cpp",
    "flowmesh_client_work_tests": "src/test/flowmesh_client_work_tests.cpp",
    "flowmesh_client_poll_tests": "src/test/flowmesh_client_poll_tests.cpp",
    "flowmesh_client_action_wait_tests": "src/test/flowmesh_client_action_wait_tests.cpp",
    "flowmesh_client_watch_tests": "src/test/flowmesh_client_watch_tests.cpp",
    "flowmesh_client_outcome_tests": "src/test/flowmesh_client_outcome_tests.cpp",
    "flowmesh_client_readiness_tests": "src/test/flowmesh_client_readiness_tests.cpp",
    "flowmesh_local_admission_tests": "src/test/flowmesh_local_admission_tests.cpp",
    "flowmesh_health_tests": "src/test/flowmesh_health_tests.cpp",
    "flowmesh_timing_tests": "src/test/flowmesh_timing_tests.cpp",
    "flowmesh_checkpoint_index_tests": "src/test/flowmesh_checkpoint_index_tests.cpp",
    "validation_chainstatemanager_tests": "src/test/validation_chainstatemanager_tests.cpp",
    "flowmesh_sync_tests": "src/test/flowmesh_sync_tests.cpp",
}
REQUIRED_CASES = {
    "flowmesh_https_tests": {
        "client_read_admission_eight_waiters_are_fifo_without_idle_stealing",
        "client_read_admission_absolute_deadline_survives_spurious_wakes_and_unlinks_expiry",
        "client_read_admission_owner_exception_releases_exactly_one_reservation",
        "https_generic_unconfigured_two_handler_saturation_explicitly_refuses_health_tls",
        "https_reserved_health_declares_slow_reserve_and_full_admission_refusal_limits",
        "https_reserved_health_has_separate_capacity_with_fourteen_sleepers_and_two_active",
        "https_reserved_health_overflow_only_leaves_unloaded_submits_on_ordinary_workers",
        "https_reserved_health_preserves_http_bounds_and_tls_authentication",
        "https_reserved_health_preserves_ordinary_resumer_priority",
        "https_reserved_health_reclassifies_keepalive_and_never_dispatches_nonhealth",
        "https_reserved_health_small_reply_bound_refuses_invalid_profile_and_fallback_closes",
        "https_reserved_health_stop_joins_its_partial_body_owner_and_restart_is_clean",
    },
    "flowmesh_api_health_tests": {
        "api_reserved_and_ordinary_health_share_exactly_once_existing_budgets",
        "api_reserved_health_strict_decoded_envelope_never_dispatches_other_methods",
    },
    "flowmesh_agreement_tests": {
        "local_signature_publication_callback_excludes_received_evidence",
        "local_signature_callback_never_runs_before_signed_sync",
    },
    "flowmesh_client_watch_lane_tests": {
        "selected_certified_account_proof_remains_available_while_catalog_tls_is_held",
        "selected_corrupted_state_is_refused_while_catalog_tls_is_held",
        "passive_catalog_tls_leaves_local_status_unsigned_refusal_and_health_available",
        "catalog_owner_is_immediate_busy_reconnect_does_not_reset_and_empty_is_success",
        "catalog_stale_trust_or_endpoint_observation_cannot_publish_labels_or_hints",
        "catalog_same_path_ca_replacement_refuses_held_reply_and_recovers",
        "catalog_validates_whole_rowset_before_metadata_readiness_or_advertisement",
        "catalog_reconnect_is_fresh_shape_only_counted_and_never_touches_outbox",
        "client_read_admission_trace_captures_real_held_foreground_and_admitted_failure",
        "client_read_admission_trace_default_off_and_bench_entry_guard_are_explicit",
        "prospective_foreground_signing_market_waits_outside_work_for_passive_other_account",
        "prospective_foreground_explicit_data_waits_and_rechecks_reconnected_context",
        "foreground_preflight_after_admission_joins_new_original_stamp_without_another_body",
        "foreground_data_queue_time_remains_in_original_freshness_age",
        "authenticated_watch_stays_immediate_and_failed_queued_read_releases_owner",
        "public_health_https_uses_leaf_observations_while_cs_main_is_held",
        "synthetic_cs_main_handler_saturation_must_not_starve_public_health_tls",
        "ordinary_snapshot_and_updates_leave_work_free_while_https_is_held",
        "ordinary_read_busy_has_no_queue_and_reconnect_fences_the_held_reply",
        "ordinary_read_endpoint_observations_do_not_starve_a_valid_held_body",
        "ordinary_snapshot_rechecks_connected_authority_before_publication",
        "superseded_ordinary_updates_do_not_clear_a_newer_watch_join",
        "ordinary_read_refuses_a_same_path_ca_replaced_while_reply_is_held",
        "ordinary_snapshot_failed_save_never_publishes_or_joins_an_older_head",
        "watch_head_change_snapshot_leaves_work_free_and_busy_reads_refuse",
        "superseded_watch_timeout_does_not_shrink_the_next_watch_budget",
        "signing_preflight_can_join_only_the_original_stamp_during_passive_https",
        "current_failed_refresh_withdraws_its_original_preflight_join",
        "explicit_connect_and_reconnect_fence_an_original_preflight_join",
    },
    "flowmesh_client_market_view_tests": {
        "stale_tick_rejection_cannot_withdraw_a_newer_advertisement",
    },
    "flowmesh_p2p_tests": {
        "local_retention_token_is_private_queue_metadata_not_wire_or_admission",
    },
    "flowmesh_runtime_tests": {
        "committed_anchor_frontier_rechecks_only_proven_descendant_and_keeps_duplicate_semantics",
        "committed_anchor_frontier_missing_or_throwing_proof_keeps_full_scan_for_lifecycle",
        "committed_anchor_frontier_catchup_and_restart_preserve_full_startup_validation",
        "committed_anchor_frontier_default_false_preserves_independent_older_anchor_check",
        "certified_reads_do_not_wait_for_a_blocked_market_worker",
        "certified_read_versions_are_immutable_and_survive_rejected_certificate_and_restart",
        "certified_read_public_history_refusal_preserves_exact_durable_entry",
        "idempotent_start_does_not_drain_a_busy_writers_retired_views",
        "recent_window_published_before_inclusion_events",
        "seat_key_observation_publishes_without_a_tick_or_certified_state_change",
        "accepted_local_original_waits_for_authority_then_resumes_without_client_resend",
        "local_source_reservation_bounds_preserve_critical_capacity_and_original_expiry",
        "local_source_survives_queue_drop_and_cannot_be_claimed_by_remote_alias",
        "local_source_open_chain_seat_pause_preserves_retry_pacing_and_expiry",
        "local_source_stop_start_stale_tokens_and_delayed_preflight_fail_closed",
        "local_action_producer_reserves_before_acceptance_and_rejects_forgery_while_closed",
        "local_source_start_keeps_idempotent_fast_path_during_stop_join",
        "local_source_malformed_reward_credentials_do_not_shelter_behind_closed_authority",
        "preagreement_local_signed_publications_resume_exactly_without_clock_advance",
        "local_agreement_publication_cache_bounds_and_peer_origin",
        "local_agreement_publication_cache_accounts_large_encoded_copies",
        "local_agreement_publication_retry_work_is_global_and_paced",
        "local_agreement_publication_reclose_and_budget_do_not_reset_pacing",
        "local_agreement_publication_normal_success_and_context_refusal_retire_copies",
        "local_agreement_publication_restart_retains_exact_journal_fallback",
    },
    "flowmesh_production_store_tests": {
        "checkpoint_advisory_resumes_bounded_suffix_and_retains_exact_candidate",
        "checkpoint_advisory_leaf_getter_and_connection_jump_replay",
        "checkpoint_advisory_discovers_corruption_without_decoding_oversized_records",
        "checkpoint_advisory_failed_sync_invalidates_without_retrying_unknown_write",
        "checkpoint_advisory_failed_replay_and_missing_marker_never_publish_empty",
    },
    "validation_chainstatemanager_tests": {
        "chain_mutation_manager_destruction_invalidates_owned_ticket",
        "chain_mutation_ticket_supersession_ownership_and_exhaustion",
        "chain_mutation_nested_scopes_refuse_reentrant_publication_and_unwind",
        "chain_mutation_fences_connect_before_flush_with_background_callbacks_delayed",
        "chain_mutation_fences_disconnect_and_leaf_reads_do_not_take_chain_lock",
        "chain_mutation_fences_snapshot_switch_load_targets_remove_and_reset",
        "chain_mutation_fences_snapshot_validation_success",
        "chain_mutation_fences_invalid_snapshot_mode",
    },
    "flowmesh_client_outcome_evidence_tests": {
        "included_outcome_demand_services_older_unresolved_fifo_without_resubmitting",
        "unresolved_caller_services_older_included_outcome_without_misattribution_or_resubmission",
        "advertised_action_outcome_parses_only_true",
        "remote_client_verifies_outcome_evidence_only_on_advertising_endpoints",
        "evidence_failure_never_downgrades_inclusion",
        "outcome_upgrade_at_most_once_per_endpoint",
        "transient_outcome_evidence_budget_recovers_without_resubmitting_included_action",
        "unknown_field_error_clears_both_capabilities",
    },
    "flowmesh_client_poll_tests": {
        "mixed_status_and_outcome_demand_keeps_fifo_bounds_and_nonrefunding_removal",
    },
    "flowmesh_sync_tests": {
        "chain_anchor_immutable_ancestry_is_exact_and_not_canonical_authority",
    },
}
NATIVE_SOURCES = {
    "src/flowmesh/p2p.cpp",
    "src/node/flowmesh_runtime.cpp", "src/node/flowmesh_production_store.cpp",
    "src/node/flowmesh_service.cpp", "src/node/flowmesh_client.cpp", "src/node/flowmesh_https.cpp",
    "src/node/flowmesh_anchor.cpp",
    "src/node/flowmesh_timing.cpp", "src/node/flowmesh_agreement.cpp", "src/validation.cpp", "src/test/main.cpp",
} | set(SUITE_SOURCES.values())
# Headers have no standalone compile-command row. Record this profile's new
# header dependency separately, without weakening translation-unit checks.
PROFILE_HEADER_DEPENDENCIES = {
    "src/node/flowmesh_client_read_admission.h",
    "src/flowmesh/deposit.h", "src/node/flowmesh_anchor.h",
}


class GuardrailError(Exception):
    """Missing evidence is failure, not a successful skipped qualification."""


@dataclass(frozen=True)
class ChildResult:
    returncode: int
    stdout: str
    stderr: str
    timed_out: bool = False
    output_overflow: bool = False


def run_child(command, timeout, *, cwd, env):
    """Bound the process group, elapsed time and in-memory diagnostic output."""
    if os.name != "posix":
        raise GuardrailError("POSIX process-group support is required")
    try:
        child = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 start_new_session=True)
    except OSError as error:
        raise GuardrailError(f"Cannot execute child: {error}") from error
    buffers = {"stdout": bytearray(), "stderr": bytearray()}
    selector = selectors.DefaultSelector()
    selector.register(child.stdout, selectors.EVENT_READ, "stdout")
    selector.register(child.stderr, selectors.EVENT_READ, "stderr")
    deadline = time.monotonic() + timeout
    timed_out = overflow = False
    try:
        while selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                timed_out = True
                break
            for key, _ in selector.select(min(remaining, .1)):
                data = os.read(key.fileobj.fileno(), 65536)
                if not data:
                    selector.unregister(key.fileobj)
                    continue
                target = buffers[key.data]
                room = MAX_OUTPUT_BYTES - len(target)
                target.extend(data[:room])
                if len(data) > room:
                    overflow = True
                    break
            if overflow:
                break
        if not timed_out and not overflow:
            try:
                child.wait(timeout=max(.001, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                timed_out = True
    finally:
        if child.poll() is None or timed_out or overflow:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        try:
            child.wait(timeout=2)
        except subprocess.TimeoutExpired as error:
            raise GuardrailError("Child did not exit after process-group termination") from error
        finally:
            selector.close()
            child.stdout.close()
            child.stderr.close()
    return ChildResult(child.returncode, buffers["stdout"].decode(errors="replace"),
                       buffers["stderr"].decode(errors="replace"), timed_out, overflow)


def require_child(result, label):
    print(f"{label}: actual exit={result.returncode}, timed_out={result.timed_out}, output_overflow={result.output_overflow}")
    diagnostic = result.stdout + result.stderr
    if result.timed_out or result.output_overflow or result.returncode != 0:
        raise GuardrailError(f"{label} failed; diagnostics: {diagnostic[-4000:]}")
    if re.search(r"ThreadSanitizer|Sanitizer: CHECK failed|FATAL:.*[Ss]anitizer", diagnostic):
        raise GuardrailError(f"{label} emitted sanitizer diagnostics despite zero exit: {diagnostic[-4000:]}")


def read_small(path):
    try:
        with path.open("rb") as source:
            data = source.read(MAX_OUTPUT_BYTES + 1)
    except OSError as error:
        raise GuardrailError(f"Missing/unreadable evidence {path.name}: {error}") from error
    if len(data) > MAX_OUTPUT_BYTES:
        raise GuardrailError(f"Evidence exceeds {MAX_OUTPUT_BYTES} bytes: {path.name}")
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise GuardrailError(f"Evidence is not UTF-8: {path.name}") from error


def file_hash(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def profile_header_hashes(root):
    hashes = {}
    for source in sorted(PROFILE_HEADER_DEPENDENCIES):
        path = root / source
        if not path.is_file():
            raise GuardrailError(f"Missing profile header dependency: {source}")
        hashes[source] = file_hash(path)
    return hashes


def validate_build(root, build, profile):
    cache = {}
    for line in read_small(build / "CMakeCache.txt").splitlines():
        match = re.fullmatch(r"([^/#][^:]*):[^=]+=(.*)", line)
        if match:
            cache[match[1]] = match[2]
    expected = {"CMAKE_BUILD_TYPE": "Debug" if profile == "lockorder" else "RelWithDebInfo",
                "BUILD_TESTS": "ON", "ENABLE_WALLET": "ON", "BUILD_GUI": "OFF",
                "ENABLE_IPC": "OFF", "BUILD_DAEMON": "OFF", "BUILD_CLI": "OFF",
                "SANITIZERS": "" if profile == "lockorder" else "thread"}
    for key, value in expected.items():
        if cache.get(key, "" if key == "SANITIZERS" else None) != value:
            raise GuardrailError(f"Wrong build configuration {key}: expected {value!r}")
    if Path(cache.get("CMAKE_HOME_DIRECTORY", "")).resolve() != root:
        raise GuardrailError("Build source directory differs from this checkout")
    try:
        commands = json.loads(read_small(build / "compile_commands.json"))
    except (ValueError, TypeError) as error:
        raise GuardrailError("Invalid compile command manifest") from error
    seen = set()
    for row in commands:
        source = Path(row["file"])
        if not source.is_absolute():
            source = Path(row["directory"]) / source
        try:
            relative = source.resolve().relative_to(root).as_posix()
        except ValueError:
            continue
        if relative not in NATIVE_SOURCES:
            continue
        args = row.get("arguments") or shlex.split(row["command"])
        if not any(flag in args for flag in ("-std=c++20", "-std=gnu++20")):
            raise GuardrailError(f"C++20 missing for {relative}")
        if "-Wthread-safety" not in args:
            raise GuardrailError(f"Clang thread-safety diagnostics missing for {relative}")
        if profile == "lockorder" and "-DDEBUG_LOCKORDER" not in args:
            raise GuardrailError(f"DEBUG_LOCKORDER missing for {relative}")
        sanitizers = {part for flag in args if flag.startswith("-fsanitize=")
                      for part in flag.removeprefix("-fsanitize=").split(",")}
        if (profile == "tsan") != ("thread" in sanitizers):
            raise GuardrailError(f"Wrong native TSan instrumentation for {relative}")
        seen.add(relative)
    if seen != NATIVE_SOURCES:
        raise GuardrailError(f"Missing native compile commands: {sorted(NATIVE_SOURCES - seen)}")
    compiler = cache.get("CMAKE_CXX_COMPILER")
    if not compiler:
        raise GuardrailError("Missing CMake C++ compiler identity")
    return compiler


def parse_discovery(text):
    """Read Boost HRF listing (4-space children), requiring enabled exact leaves."""
    discovered = {}
    current = None
    for line in text.splitlines():
        if not line.startswith(" "):
            match = re.fullmatch(r"([A-Za-z0-9_]+)(\*)?(?:\s*:.*)?\s*", line)
            current = match[1] if match and match[1] in SUITE_SOURCES else None
            if current is not None:
                if current in discovered or not match[2]:
                    raise GuardrailError(f"Duplicate/disabled selected suite: {current}")
                discovered[current] = set()
        elif current is not None:
            match = re.fullmatch(r" {4}([A-Za-z0-9_]+)(\*)?(?:\s*:.*)?\s*", line)
            if not match or not match[2] or match[1] in discovered[current]:
                raise GuardrailError(f"Malformed/nested/duplicate/disabled selected case: {line!r}")
            discovered[current].add(match[1])
    if set(discovered) != set(SUITE_SOURCES):
        raise GuardrailError(f"Missing compiled suites: {sorted(set(SUITE_SOURCES) - set(discovered))}")
    for suite, cases in discovered.items():
        if not cases or not REQUIRED_CASES.get(suite, set()).issubset(cases):
            raise GuardrailError(f"Empty selected suite or missing required regression: {suite}")
    return discovered


def check_xml(text, suite, expected_cases):
    try:
        document = ET.fromstring(text)
    except ET.ParseError as error:
        raise GuardrailError(f"Malformed Boost result for {suite}") from error
    if document.tag != "TestResult":
        raise GuardrailError(f"Not a Boost TestResult for {suite}")
    masters = document.findall("TestSuite")
    if len(masters) != 1 or masters[0].get("result") != "passed":
        raise GuardrailError(f"Missing/failed master result for {suite}")
    for key in ("assertions_failed", "warnings_failed", "expected_failures"):
        if masters[0].get(key) != "0":
            raise GuardrailError(f"Nonzero/missing master {key} for {suite}")
    selected = [row for row in document.iter("TestSuite") if row.get("name") == suite]
    if len(selected) != 1:
        raise GuardrailError(f"Missing/duplicate selected suite result: {suite}")
    selected = selected[0]
    cases = list(selected.iter("TestCase"))
    names = [case.get("name") for case in cases]
    if not names or len(names) != len(set(names)) or set(names) != expected_cases:
        raise GuardrailError(f"Executed/discovered case mismatch for {suite}")
    if selected.get("test_cases_passed") != str(len(expected_cases)):
        raise GuardrailError(f"Passed case count mismatch for {suite}")
    for key in ("test_cases_passed_with_warnings", "test_cases_failed", "test_cases_skipped",
                "test_cases_aborted", "test_cases_timed_out", "test_suites_timed_out"):
        if selected.get(key) != "0":
            raise GuardrailError(f"Nonzero/missing {key} for {suite}")
    for row in selected.iter():
        if row.tag not in ("TestSuite", "TestCase"):
            raise GuardrailError(f"Unexpected selected result element: {row.tag}")
        if row.get("result") != "passed":
            raise GuardrailError(f"Selected case/suite skipped or failed: {row.get('name')}")
        for key in ("assertions_failed", "warnings_failed", "expected_failures"):
            if row.get(key) != "0":
                raise GuardrailError(f"Nonzero/missing {key} for {row.get('name')}")
    # Other suites are normally listed as skipped, but cannot execute secretly.
    outside = [case for case in document.iter("TestCase") if case not in cases and case.get("result") != "skipped"]
    if outside:
        raise GuardrailError("Unselected cases executed outside the exact filter")
    return len(cases)


def run_profile(root, build, output, profile):
    if sys.platform != "linux":
        raise GuardrailError("This profile requires Linux; unavailable is not a pass")
    output.mkdir(mode=0o700, parents=False, exist_ok=False)
    binary = build / "bin" / "test_bitcoin"
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise GuardrailError("Missing executable build/bin/test_bitcoin")
    compiler = validate_build(root, build, profile)
    env = {key: value for key, value in os.environ.items() if not key.startswith("BOOST_TEST_")}
    suppression = root / "test" / "sanitizer_suppressions" / "tsan"
    if profile == "tsan":
        if not suppression.is_file():
            raise GuardrailError("Missing declared repository TSan suppressions")
        env["TSAN_OPTIONS"] = f"suppressions={suppression}:halt_on_error=1:exitcode=66:second_deadlock_stack=1"
    else:
        env.pop("TSAN_OPTIONS", None)
    version = run_child([compiler, "--version"], CONTROL_TIMEOUT, cwd=root, env=env)
    require_child(version, "compiler identity")
    if "clang" not in version.stdout.lower():
        raise GuardrailError("Clang native build is required")
    revision = run_child(["git", "rev-parse", "HEAD"], CONTROL_TIMEOUT, cwd=root, env=env)
    require_child(revision, "source revision")
    if not re.fullmatch(r"[0-9a-f]{40}\n?", revision.stdout):
        raise GuardrailError("Invalid exact source revision")
    report = {"profile_version": PROFILE_VERSION, "profile": profile,
              "revision": revision.stdout.strip(), "compiler": version.stdout.splitlines()[0],
              "binary_sha256": file_hash(binary),
              "profile_header_dependencies_sha256": profile_header_hashes(root),
              "limits": {"suite_timeout_seconds": SUITE_TIMEOUT, "total_test_timeout_seconds": TOTAL_TEST_TIMEOUT},
              "scope": "native code only; not Qt, WAN, power-loss, performance or release qualification",
              "system_libraries_instrumented": False,
              "plain_mutex_lockorder_coverage": False,
              "tsan_suppression_sha256": file_hash(suppression) if profile == "tsan" else None,
              "tsan_connecttip_deadlock_suppressed": profile == "tsan", "suites": [], "passed": False}
    try:
        listing = output / "discovery.txt"
        result = run_child([str(binary), "--list_content=HRF", "--color_output=no", f"--report_sink={listing}"],
                           CONTROL_TIMEOUT, cwd=root, env=env)
        require_child(result, "native suite discovery / sanitizer startup")
        discovered = parse_discovery(read_small(listing))
        started = time.monotonic()
        # Fresh private test data is never an artifact. Do not use owner datadirs.
        with tempfile.TemporaryDirectory(prefix="flowmesh-native-guardrails-") as work:
            for suite in SUITE_SOURCES:
                remaining = TOTAL_TEST_TIMEOUT - (time.monotonic() - started)
                if remaining <= 0:
                    raise GuardrailError("Overall selected-test deadline exhausted")
                xml = output / f"{suite}.xml"
                command = [str(binary), f"--run_test={suite}", "--catch_system_error=no",
                           "--report_format=XML", "--report_level=detailed", "--log_level=nothing",
                           "--color_output=no", f"--report_sink={xml}", "--", f"-testdatadir={work}/{suite}"]
                result = run_child(command, min(SUITE_TIMEOUT, remaining), cwd=root, env=env)
                row = {"name": suite, "discovered_cases": len(discovered[suite]),
                       "actual_exit": result.returncode, "timed_out": result.timed_out,
                       "output_overflow": result.output_overflow, "passed": False}
                report["suites"].append(row)
                require_child(result, suite)
                row["executed_cases"] = check_xml(read_small(xml), suite, discovered[suite])
                row["passed"] = True
                print(f"PASS {suite}: {row['executed_cases']} exact cases")
        report["passed"] = True
        print(f"PASS {len(report['suites'])} exact native suites, {sum(row['executed_cases'] for row in report['suites'])} cases; {PROFILE_VERSION}")
    except GuardrailError as error:
        report["error"] = str(error)
        raise
    finally:
        (output / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", choices=("lockorder", "tsan"), required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True, help="Fresh, nonexistent private result directory")
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    def interrupted(signum, _frame):
        raise GuardrailError(f"Interrupted by signal {signum}; no qualification")
    original_term = signal.signal(signal.SIGTERM, interrupted)
    try:
        run_profile(root, args.build_dir.resolve(), args.output_dir.absolute(), args.profile)
    except (GuardrailError, OSError, ValueError, KeyError, TypeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    finally:
        signal.signal(signal.SIGTERM, original_term)
    return 0


if __name__ == "__main__":
    sys.exit(main())
