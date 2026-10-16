#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Lightweight rejection controls; these do NOT execute native/sanitizer tests."""

from contextlib import redirect_stderr, redirect_stdout
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("flowmesh_native_guardrails", ROOT / "ci" / "check_flowmesh_native_guardrails.py")
RUNNER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = RUNNER
SPEC.loader.exec_module(RUNNER)


def expected_inventory():
    return {suite: RUNNER.REQUIRED_CASES.get(suite, set()) | {"synthetic_control"}
            for suite in RUNNER.SUITE_SOURCES}


def listing(inventory=None):
    inventory = expected_inventory() if inventory is None else inventory
    return "".join(suite + "*\n" + "".join(f"    {case}*\n" for case in sorted(cases))
                   for suite, cases in inventory.items())


def xml_report(suite, cases):
    zero = {"result": "passed", "assertions_passed": "1", "assertions_failed": "0",
            "warnings_failed": "0", "expected_failures": "0"}
    counters = {"test_cases_passed": str(len(cases)), "test_cases_passed_with_warnings": "0",
                "test_cases_failed": "0", "test_cases_skipped": "0", "test_cases_aborted": "0",
                "test_cases_timed_out": "0", "test_suites_timed_out": "0"}
    document = ET.Element("TestResult")
    master = ET.SubElement(document, "TestSuite", name="Bitcoin Core Test Suite", **zero, **counters)
    selected = ET.SubElement(master, "TestSuite", name=suite, **zero, **counters)
    for name in sorted(cases):
        ET.SubElement(selected, "TestCase", name=name, **zero)
    return document


# Exact bounded health profile, independent of RUNNER's mandatory manifest.
HEALTH_REQUIRED_CASES = {
    "flowmesh_https_tests": {
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
    "flowmesh_client_watch_lane_tests": {
        "public_health_https_uses_leaf_observations_while_cs_main_is_held",
        "synthetic_cs_main_handler_saturation_must_not_starve_public_health_tls",
    },
}


# Independent names protect the new policy and its two adapted old controls.
ADMISSION_REQUIRED_CASES = {
    "flowmesh_https_tests": {
        "client_read_admission_eight_waiters_are_fifo_without_idle_stealing",
        "client_read_admission_absolute_deadline_survives_spurious_wakes_and_unlinks_expiry",
        "client_read_admission_owner_exception_releases_exactly_one_reservation",
    },
    "flowmesh_client_watch_lane_tests": {
        "prospective_foreground_signing_market_waits_outside_work_for_passive_other_account",
        "prospective_foreground_explicit_data_waits_and_rechecks_reconnected_context",
        "foreground_preflight_after_admission_joins_new_original_stamp_without_another_body",
        "foreground_data_queue_time_remains_in_original_freshness_age",
        "authenticated_watch_stays_immediate_and_failed_queued_read_releases_owner",
    },
}
ADMISSION_PROTECTED_CASES = {
    suite: cases | ({
        "signing_preflight_can_join_only_the_original_stamp_during_passive_https",
        "explicit_connect_and_reconnect_fence_an_original_preflight_join",
    } if suite == "flowmesh_client_watch_lane_tests" else set())
    for suite, cases in ADMISSION_REQUIRED_CASES.items()
}


# Independent exact names protect diagnostic tracing without qualifying timing.
TRACE_REQUIRED_CASES = {
    "flowmesh_client_watch_lane_tests": {
        "client_read_admission_trace_captures_real_held_foreground_and_admitted_failure",
        "client_read_admission_trace_default_off_and_bench_entry_guard_are_explicit",
    },
}


# Independent exact Catalog names, not derived from RUNNER or source discovery.
CATALOG_REQUIRED_CASES = {
    "flowmesh_client_watch_lane_tests": {
        "passive_catalog_tls_leaves_local_status_unsigned_refusal_and_health_available",
        "catalog_owner_is_immediate_busy_reconnect_does_not_reset_and_empty_is_success",
        "catalog_stale_trust_or_endpoint_observation_cannot_publish_labels_or_hints",
        "catalog_same_path_ca_replacement_refuses_held_reply_and_recovers",
        "catalog_validates_whole_rowset_before_metadata_readiness_or_advertisement",
        "catalog_reconnect_is_fresh_shape_only_counted_and_never_touches_outbox",
    },
}


# Literal frozen v6 manifest, independent of RUNNER and the new suite count.
FROZEN_V6_SUITE_SOURCES = {
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
}

# Exact outcome evidence cases, independent of RUNNER's required-case map.
OUTCOME_EVIDENCE_REQUIRED_CASES = {
    "advertised_action_outcome_parses_only_true",
    "remote_client_verifies_outcome_evidence_only_on_advertising_endpoints",
    "evidence_failure_never_downgrades_inclusion",
    "outcome_upgrade_at_most_once_per_endpoint",
    "transient_outcome_evidence_budget_recovers_without_resubmitting_included_action",
    "unknown_field_error_clears_both_capabilities",
}


class DiscoveryTests(unittest.TestCase):
    def test_exact_enabled_manifest(self):
        self.assertEqual(RUNNER.parse_discovery(listing()), expected_inventory())

    def test_unselected_compiled_suite_is_not_executed(self):
        self.assertEqual(RUNNER.parse_discovery(listing() + "unselected_suite*\n    unrelated*\n"), expected_inventory())

    def test_missing_suite(self):
        inventory = expected_inventory()
        del inventory[next(iter(inventory))]
        with self.assertRaisesRegex(RUNNER.GuardrailError, "Missing compiled suites"):
            RUNNER.parse_discovery(listing(inventory))

    def test_disabled_or_duplicate_suite(self):
        first = next(iter(RUNNER.SUITE_SOURCES))
        for text in (listing().replace(first + "*", first + " ", 1), listing() + first + "*\n"):
            with self.subTest(text=text[-100:]), self.assertRaisesRegex(RUNNER.GuardrailError, "Duplicate/disabled"):
                RUNNER.parse_discovery(text)

    def test_disabled_duplicate_and_nested_case(self):
        for suffix in ("    disabled \n", "    synthetic_control*\n", "        nested*\n"):
            with self.subTest(suffix=suffix), self.assertRaisesRegex(RUNNER.GuardrailError, "selected case"):
                RUNNER.parse_discovery(listing() + suffix)

    def test_empty_suite(self):
        inventory = expected_inventory()
        inventory[next(iter(inventory))] = set()
        with self.assertRaisesRegex(RUNNER.GuardrailError, "Empty selected suite"):
            RUNNER.parse_discovery(listing(inventory))

    def test_missing_required_regression(self):
        inventory = expected_inventory()
        suite = next(iter(RUNNER.REQUIRED_CASES))
        inventory[suite].remove(next(iter(RUNNER.REQUIRED_CASES[suite])))
        with self.assertRaisesRegex(RUNNER.GuardrailError, "missing required regression"):
            RUNNER.parse_discovery(listing(inventory))

    def test_empty_discovery(self):
        with self.assertRaises(RUNNER.GuardrailError):
            RUNNER.parse_discovery("")


class XMLTests(unittest.TestCase):
    suite = "flowmesh_runtime_tests"
    cases = {"first", "second"}

    def check(self, document):
        return RUNNER.check_xml(ET.tostring(document, encoding="unicode"), self.suite, self.cases)

    def test_all_exact_cases_pass(self):
        self.assertEqual(self.check(xml_report(self.suite, self.cases)), 2)

    def test_missing_duplicate_extra_cases(self):
        for names in ({"first"}, self.cases | {"unlisted"}, set()):
            with self.subTest(names=names), self.assertRaises(RUNNER.GuardrailError):
                self.check(xml_report(self.suite, names))
        document = xml_report(self.suite, self.cases)
        selected = document.find("./TestSuite/TestSuite")
        selected.append(ET.fromstring(ET.tostring(selected[0])))
        with self.assertRaisesRegex(RUNNER.GuardrailError, "case mismatch"):
            self.check(document)

    def test_wrong_pass_count_and_any_selected_skip_failure(self):
        keys = ("test_cases_passed_with_warnings", "test_cases_failed", "test_cases_skipped",
                "test_cases_aborted", "test_cases_timed_out", "test_suites_timed_out")
        for key in keys + ("test_cases_passed",):
            document = xml_report(self.suite, self.cases)
            document.find("./TestSuite/TestSuite").set(key, "1")
            with self.subTest(key=key), self.assertRaises(RUNNER.GuardrailError):
                self.check(document)

    def test_missing_counters_not_success(self):
        document = xml_report(self.suite, self.cases)
        del document.find("./TestSuite/TestSuite").attrib["test_cases_skipped"]
        with self.assertRaises(RUNNER.GuardrailError):
            self.check(document)

    def test_selected_result_and_assertions(self):
        for key, value in (("result", "skipped"), ("result", "timed-out"), ("result", "failed"),
                           ("assertions_failed", "1"), ("warnings_failed", "1"), ("expected_failures", "1")):
            document = xml_report(self.suite, self.cases)
            document.find(".//TestCase").set(key, value)
            with self.subTest(key=key, value=value), self.assertRaises(RUNNER.GuardrailError):
                self.check(document)

    def test_failed_master_and_wrong_or_duplicate_suite(self):
        document = xml_report(self.suite, self.cases)
        document.find("TestSuite").set("result", "failed")
        with self.assertRaisesRegex(RUNNER.GuardrailError, "master result"):
            self.check(document)
        document = xml_report("wrong_suite", self.cases)
        with self.assertRaisesRegex(RUNNER.GuardrailError, "selected suite result"):
            self.check(document)
        document = xml_report(self.suite, self.cases)
        document.find("TestSuite").append(xml_report(self.suite, self.cases).find("./TestSuite/TestSuite"))
        with self.assertRaisesRegex(RUNNER.GuardrailError, "selected suite result"):
            self.check(document)

    def test_unselected_skips_allowed_but_execution_forbidden(self):
        for result in ("skipped", "passed"):
            document = xml_report(self.suite, self.cases)
            other = ET.SubElement(document.find("TestSuite"), "TestSuite", name="not_selected", result="skipped")
            ET.SubElement(other, "TestCase", name="other_case", result=result)
            if result == "skipped":
                self.assertEqual(self.check(document), 2)
            else:
                with self.assertRaisesRegex(RUNNER.GuardrailError, "Unselected cases executed"):
                    self.check(document)

    def test_malformed_empty_and_wrong_xml(self):
        for text in ("", "<TestResult>", "<NotBoost/>", "no test cases matching filter"):
            with self.subTest(text=text), self.assertRaises(RUNNER.GuardrailError):
                RUNNER.check_xml(text, self.suite, self.cases)


class ReservedHealthRegressionTests(unittest.TestCase):
    def test_health_positive_discovery_and_exact_execution(self):
        self.assertEqual(sum(map(len, HEALTH_REQUIRED_CASES.values())), 13)
        discovered = RUNNER.parse_discovery(listing())
        for suite, mandatory in HEALTH_REQUIRED_CASES.items():
            with self.subTest(suite=suite):
                self.assertTrue(mandatory.issubset(RUNNER.REQUIRED_CASES[suite]))
                self.assertTrue(mandatory.issubset(discovered[suite]))
                self.assertEqual(RUNNER.check_xml(
                    ET.tostring(xml_report(suite, discovered[suite]), encoding="unicode"),
                    suite, discovered[suite]), len(discovered[suite]))

    def test_each_health_case_cannot_be_undiscovered_or_disabled(self):
        for suite, mandatory in HEALTH_REQUIRED_CASES.items():
            for case in mandatory:
                inventory = expected_inventory()
                inventory[suite].remove(case)
                with self.subTest(suite=suite, case=case, fault="undiscovered"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "missing required regression"):
                    RUNNER.parse_discovery(listing(inventory))
                disabled = listing().replace(f"    {case}*\n", f"    {case} \n", 1)
                self.assertNotEqual(disabled, listing())
                with self.subTest(suite=suite, case=case, fault="disabled"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "selected case"):
                    RUNNER.parse_discovery(disabled)

    def test_each_health_case_cannot_be_unexecuted_or_skipped(self):
        for suite, mandatory in HEALTH_REQUIRED_CASES.items():
            expected = expected_inventory()[suite]
            for case in mandatory:
                missing = xml_report(suite, expected - {case})
                with self.subTest(suite=suite, case=case, fault="unexecuted"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "case mismatch"):
                    RUNNER.check_xml(ET.tostring(missing, encoding="unicode"), suite, expected)
                skipped = xml_report(suite, expected)
                row = next(row for row in skipped.iter("TestCase") if row.get("name") == case)
                row.set("result", "skipped")
                # Even apparently all-passed counters cannot conceal one
                # omitted execution behind a skipped selected leaf.
                with self.subTest(suite=suite, case=case, fault="skipped"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "Selected case/suite skipped or failed"):
                    RUNNER.check_xml(ET.tostring(skipped, encoding="unicode"), suite, expected)


class ForegroundAdmissionRegressionTests(unittest.TestCase):
    def test_admission_positive_discovery_and_exact_execution(self):
        self.assertEqual(sum(map(len, ADMISSION_REQUIRED_CASES.values())), 8)
        self.assertEqual(sum(map(len, ADMISSION_PROTECTED_CASES.values())), 10)
        discovered = RUNNER.parse_discovery(listing())
        for suite, mandatory in ADMISSION_PROTECTED_CASES.items():
            with self.subTest(suite=suite):
                self.assertTrue(mandatory.issubset(RUNNER.REQUIRED_CASES[suite]))
                self.assertTrue(mandatory.issubset(discovered[suite]))
                self.assertEqual(RUNNER.check_xml(
                    ET.tostring(xml_report(suite, discovered[suite]), encoding="unicode"),
                    suite, discovered[suite]), len(discovered[suite]))

    def test_each_admission_case_cannot_be_undiscovered_or_disabled(self):
        for suite, mandatory in ADMISSION_PROTECTED_CASES.items():
            for case in mandatory:
                inventory = expected_inventory()
                inventory[suite].remove(case)
                with self.subTest(suite=suite, case=case, fault="undiscovered"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "missing required regression"):
                    RUNNER.parse_discovery(listing(inventory))
                disabled = listing().replace(f"    {case}*\n", f"    {case} \n", 1)
                self.assertNotEqual(disabled, listing())
                with self.subTest(suite=suite, case=case, fault="disabled"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "selected case"):
                    RUNNER.parse_discovery(disabled)

    def test_each_admission_case_cannot_be_unexecuted_or_skipped(self):
        for suite, mandatory in ADMISSION_PROTECTED_CASES.items():
            expected = expected_inventory()[suite]
            for case in mandatory:
                missing = xml_report(suite, expected - {case})
                with self.subTest(suite=suite, case=case, fault="unexecuted"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "case mismatch"):
                    RUNNER.check_xml(ET.tostring(missing, encoding="unicode"), suite, expected)
                skipped = xml_report(suite, expected)
                row = next(row for row in skipped.iter("TestCase") if row.get("name") == case)
                row.set("result", "skipped")
                with self.subTest(suite=suite, case=case, fault="skipped"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "Selected case/suite skipped or failed"):
                    RUNNER.check_xml(ET.tostring(skipped, encoding="unicode"), suite, expected)


class AdmissionTraceRegressionTests(unittest.TestCase):
    def test_trace_positive_discovery_and_exact_execution(self):
        self.assertEqual(sum(map(len, TRACE_REQUIRED_CASES.values())), 2)
        discovered = RUNNER.parse_discovery(listing())
        for suite, mandatory in TRACE_REQUIRED_CASES.items():
            with self.subTest(suite=suite):
                self.assertTrue(mandatory.issubset(RUNNER.REQUIRED_CASES[suite]))
                self.assertTrue(mandatory.issubset(discovered[suite]))
                self.assertEqual(RUNNER.check_xml(
                    ET.tostring(xml_report(suite, discovered[suite]), encoding="unicode"),
                    suite, discovered[suite]), len(discovered[suite]))

    def test_each_trace_case_cannot_be_undiscovered_or_disabled(self):
        for suite, mandatory in TRACE_REQUIRED_CASES.items():
            for case in mandatory:
                inventory = expected_inventory()
                inventory[suite].remove(case)
                with self.subTest(suite=suite, case=case, fault="undiscovered"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "missing required regression"):
                    RUNNER.parse_discovery(listing(inventory))
                disabled = listing().replace(f"    {case}*\n", f"    {case} \n", 1)
                self.assertNotEqual(disabled, listing())
                with self.subTest(suite=suite, case=case, fault="disabled"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "selected case"):
                    RUNNER.parse_discovery(disabled)

    def test_each_trace_case_cannot_be_unexecuted_or_skipped(self):
        for suite, mandatory in TRACE_REQUIRED_CASES.items():
            expected = expected_inventory()[suite]
            for case in mandatory:
                missing = xml_report(suite, expected - {case})
                with self.subTest(suite=suite, case=case, fault="unexecuted"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "case mismatch"):
                    RUNNER.check_xml(ET.tostring(missing, encoding="unicode"), suite, expected)
                skipped = xml_report(suite, expected)
                row = next(row for row in skipped.iter("TestCase") if row.get("name") == case)
                row.set("result", "skipped")
                with self.subTest(suite=suite, case=case, fault="skipped"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "Selected case/suite skipped or failed"):
                    RUNNER.check_xml(ET.tostring(skipped, encoding="unicode"), suite, expected)


class OutcomeEvidenceRegressionTests(unittest.TestCase):
    suite = "flowmesh_client_outcome_evidence_tests"

    def test_outcome_evidence_positive_discovery_and_exact_execution(self):
        self.assertEqual(len(OUTCOME_EVIDENCE_REQUIRED_CASES), 6)
        self.assertEqual(RUNNER.SUITE_SOURCES[self.suite], "src/test/flowmesh_https_tests.cpp")
        self.assertEqual(RUNNER.REQUIRED_CASES[self.suite], OUTCOME_EVIDENCE_REQUIRED_CASES)
        discovered = RUNNER.parse_discovery(listing())
        self.assertTrue(OUTCOME_EVIDENCE_REQUIRED_CASES.issubset(discovered[self.suite]))
        self.assertEqual(RUNNER.check_xml(
            ET.tostring(xml_report(self.suite, discovered[self.suite]), encoding="unicode"),
            self.suite, discovered[self.suite]), len(discovered[self.suite]))

    def test_outcome_evidence_suite_and_each_case_cannot_be_missing_or_disabled(self):
        missing_suite = expected_inventory()
        del missing_suite[self.suite]
        with self.subTest(fault="missing suite"), self.assertRaisesRegex(
                RUNNER.GuardrailError, "Missing compiled suites"):
            RUNNER.parse_discovery(listing(missing_suite))
        disabled_suite = listing().replace(self.suite + "*\n", self.suite + " \n", 1)
        self.assertNotEqual(disabled_suite, listing())
        with self.subTest(fault="disabled suite"), self.assertRaisesRegex(
                RUNNER.GuardrailError, "Duplicate/disabled"):
            RUNNER.parse_discovery(disabled_suite)
        for case in OUTCOME_EVIDENCE_REQUIRED_CASES:
            inventory = expected_inventory()
            inventory[self.suite].remove(case)
            with self.subTest(case=case, fault="undiscovered"), self.assertRaisesRegex(
                    RUNNER.GuardrailError, "missing required regression"):
                RUNNER.parse_discovery(listing(inventory))
            disabled = listing().replace(f"    {case}*\n", f"    {case} \n", 1)
            self.assertNotEqual(disabled, listing())
            with self.subTest(case=case, fault="disabled"), self.assertRaisesRegex(
                    RUNNER.GuardrailError, "selected case"):
                RUNNER.parse_discovery(disabled)

    def test_each_outcome_evidence_case_cannot_be_unexecuted_or_skipped(self):
        expected = expected_inventory()[self.suite]
        for case in OUTCOME_EVIDENCE_REQUIRED_CASES:
            missing = xml_report(self.suite, expected - {case})
            with self.subTest(case=case, fault="unexecuted"), self.assertRaisesRegex(
                    RUNNER.GuardrailError, "case mismatch"):
                RUNNER.check_xml(ET.tostring(missing, encoding="unicode"), self.suite, expected)
            skipped = xml_report(self.suite, expected)
            row = next(row for row in skipped.iter("TestCase") if row.get("name") == case)
            row.set("result", "skipped")
            with self.subTest(case=case, fault="skipped"), self.assertRaisesRegex(
                    RUNNER.GuardrailError, "Selected case/suite skipped or failed"):
                RUNNER.check_xml(ET.tostring(skipped, encoding="unicode"), self.suite, expected)


class CatalogIsolationRegressionTests(unittest.TestCase):
    def test_catalog_positive_discovery_and_exact_execution(self):
        self.assertEqual(sum(map(len, CATALOG_REQUIRED_CASES.values())), 6)
        discovered = RUNNER.parse_discovery(listing())
        for suite, mandatory in CATALOG_REQUIRED_CASES.items():
            with self.subTest(suite=suite):
                self.assertEqual(RUNNER.SUITE_SOURCES[suite], "src/test/flowmesh_https_tests.cpp")
                self.assertTrue(mandatory.issubset(RUNNER.REQUIRED_CASES[suite]))
                self.assertTrue(mandatory.issubset(discovered[suite]))
                self.assertEqual(RUNNER.check_xml(
                    ET.tostring(xml_report(suite, discovered[suite]), encoding="unicode"),
                    suite, discovered[suite]), len(discovered[suite]))

    def test_each_catalog_case_cannot_be_undiscovered_or_disabled(self):
        for suite, mandatory in CATALOG_REQUIRED_CASES.items():
            for case in mandatory:
                inventory = expected_inventory()
                inventory[suite].remove(case)
                with self.subTest(suite=suite, case=case, fault="undiscovered"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "missing required regression"):
                    RUNNER.parse_discovery(listing(inventory))
                disabled = listing().replace(f"    {case}*\n", f"    {case} \n", 1)
                self.assertNotEqual(disabled, listing())
                with self.subTest(suite=suite, case=case, fault="disabled"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "selected case"):
                    RUNNER.parse_discovery(disabled)

    def test_each_catalog_case_cannot_be_unexecuted_or_skipped(self):
        for suite, mandatory in CATALOG_REQUIRED_CASES.items():
            expected = expected_inventory()[suite]
            for case in mandatory:
                missing = xml_report(suite, expected - {case})
                with self.subTest(suite=suite, case=case, fault="unexecuted"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "case mismatch"):
                    RUNNER.check_xml(ET.tostring(missing, encoding="unicode"), suite, expected)
                skipped = xml_report(suite, expected)
                row = next(row for row in skipped.iter("TestCase") if row.get("name") == case)
                row.set("result", "skipped")
                with self.subTest(suite=suite, case=case, fault="skipped"), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "Selected case/suite skipped or failed"):
                    RUNNER.check_xml(ET.tostring(skipped, encoding="unicode"), suite, expected)


class BuildFixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="flowmesh-ci-control-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.build = self.root / "build"
        self.build.mkdir()

    def make_build(self, profile="lockorder"):
        self.cache = {"CMAKE_BUILD_TYPE": "Debug" if profile == "lockorder" else "RelWithDebInfo",
                      "CMAKE_CXX_COMPILER": "/usr/bin/clang++-18", "CMAKE_HOME_DIRECTORY": str(self.root),
                      "BUILD_TESTS": "ON", "ENABLE_WALLET": "ON", "BUILD_GUI": "OFF",
                      "ENABLE_IPC": "OFF", "BUILD_DAEMON": "OFF", "BUILD_CLI": "OFF",
                      "SANITIZERS": "" if profile == "lockorder" else "thread"}
        flags = ["clang++-18", "-std=c++20", "-Wthread-safety"]
        flags.append("-DDEBUG_LOCKORDER" if profile == "lockorder" else "-fsanitize=thread")
        self.commands = [{"file": str(self.root / source), "directory": str(self.build),
                          "arguments": flags + ["-c", str(self.root / source)]}
                         for source in sorted(RUNNER.NATIVE_SOURCES)]
        self.save_build()

    def save_build(self):
        (self.build / "CMakeCache.txt").write_text("".join(f"{key}:STRING={value}\n" for key, value in self.cache.items()))
        (self.build / "compile_commands.json").write_text(json.dumps(self.commands))


class BuildTests(BuildFixture):
    def test_profile_header_dependencies_are_hashed_and_missing_is_refused(self):
        self.assertEqual(RUNNER.PROFILE_HEADER_DEPENDENCIES, {"src/node/flowmesh_client_read_admission.h"})
        with self.assertRaisesRegex(RUNNER.GuardrailError, "Missing profile header dependency"):
            RUNNER.profile_header_hashes(self.root)
        header = self.root / "src/node/flowmesh_client_read_admission.h"
        header.parent.mkdir(parents=True)
        header.write_text("// Synthetic header for a source-hash control only.\n")
        before = RUNNER.profile_header_hashes(self.root)
        self.assertEqual(before, {"src/node/flowmesh_client_read_admission.h": RUNNER.file_hash(header)})
        header.write_text("// Changed synthetic header; no native compilation.\n")
        self.assertNotEqual(RUNNER.profile_header_hashes(self.root), before)

    def test_exact_debug_and_tsan_builds(self):
        for profile in ("lockorder", "tsan"):
            self.make_build(profile)
            self.assertEqual(RUNNER.validate_build(self.root, self.build, profile), "/usr/bin/clang++-18")

    def test_missing_or_wrong_native_flags(self):
        for profile, flag in (("lockorder", "-DDEBUG_LOCKORDER"), ("tsan", "-fsanitize=thread"),
                              ("lockorder", "-Wthread-safety"), ("lockorder", "-std=c++20")):
            self.make_build(profile)
            self.commands[0]["arguments"].remove(flag)
            self.save_build()
            with self.subTest(profile=profile, flag=flag), self.assertRaises(RUNNER.GuardrailError):
                RUNNER.validate_build(self.root, self.build, profile)

    def test_missing_native_command(self):
        self.make_build()
        self.commands.pop()
        self.save_build()
        with self.assertRaisesRegex(RUNNER.GuardrailError, "Missing native compile commands"):
            RUNNER.validate_build(self.root, self.build, "lockorder")

    def test_wrong_source_and_build_settings(self):
        for key, value in (("CMAKE_HOME_DIRECTORY", "/wrong-source"), ("BUILD_TESTS", "OFF"),
                           ("ENABLE_WALLET", "OFF"), ("BUILD_GUI", "ON"), ("CMAKE_BUILD_TYPE", "Release"),
                           ("SANITIZERS", "thread"), ("BUILD_DAEMON", "ON"), ("ENABLE_IPC", "ON")):
            self.make_build()
            self.cache[key] = value
            self.save_build()
            with self.subTest(key=key), self.assertRaises(RUNNER.GuardrailError):
                RUNNER.validate_build(self.root, self.build, "lockorder")

    def test_shell_encoded_compile_command(self):
        self.make_build()
        for row in self.commands:
            row["command"] = " ".join(row.pop("arguments"))
        self.save_build()
        RUNNER.validate_build(self.root, self.build, "lockorder")

    def test_tsan_alternative_combination_refused(self):
        self.make_build("tsan")
        self.cache["SANITIZERS"] = "thread,address"
        self.save_build()
        with self.assertRaises(RUNNER.GuardrailError):
            RUNNER.validate_build(self.root, self.build, "tsan")


class ProcessTests(unittest.TestCase):
    def test_nonzero_timeout_overflow_and_sanitizer_diagnostic_rejected(self):
        for result in (RUNNER.ChildResult(201, "", "failed assertion"), RUNNER.ChildResult(-9, "", "", True),
                       RUNNER.ChildResult(0, "", "", output_overflow=True),
                       RUNNER.ChildResult(0, "", "FATAL: ThreadSanitizer: unexpected memory mapping")):
            with self.subTest(result=result), redirect_stdout(io.StringIO()), self.assertRaises(RUNNER.GuardrailError):
                RUNNER.require_child(result, "mock native result")

    def test_actual_python_exit_and_bounded_process_group_kill(self):
        env = dict(os.environ)
        result = RUNNER.run_child([sys.executable, "-B", "-c", "print('control'); raise SystemExit(7)"],
                                  3, cwd=ROOT, env=env)
        self.assertEqual((result.returncode, result.stdout.strip(), result.timed_out), (7, "control", False))
        result = RUNNER.run_child([sys.executable, "-B", "-c", "import time; time.sleep(30)"],
                                  .1, cwd=ROOT, env=env)
        self.assertTrue(result.timed_out)
        self.assertEqual(result.returncode, -signal.SIGKILL)

    def test_actual_python_output_is_capped(self):
        with patch.object(RUNNER, "MAX_OUTPUT_BYTES", 64):
            result = RUNNER.run_child([sys.executable, "-B", "-c", "print('x'*256)"], 3, cwd=ROOT, env=dict(os.environ))
        self.assertTrue(result.output_overflow)
        self.assertLessEqual(len(result.stdout), 64)


class ProfileTests(BuildFixture):
    def prepare_profile(self):
        self.make_build()
        for source in RUNNER.PROFILE_HEADER_DEPENDENCIES:
            header = self.root / source
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text("// Synthetic profile header; never compiled.\n")
        (self.build / "bin").mkdir()
        binary = self.build / "bin" / "test_bitcoin"
        binary.write_text("synthetic mock only; never executed")
        binary.chmod(0o700)
        self.output = self.root / "results"
        self.commands_seen = []

    def child(self, command, timeout, **kwargs):
        self.commands_seen.append(command)
        if command[-1] == "--version":
            return RUNNER.ChildResult(0, "Ubuntu clang version 18\n", "")
        if command[:3] == ["git", "rev-parse", "HEAD"]:
            return RUNNER.ChildResult(0, "a" * 40 + "\n", "")
        sink = Path(next(arg.split("=", 1)[1] for arg in command if arg.startswith("--report_sink=")))
        if "--list_content=HRF" in command:
            sink.write_text(listing())
        else:
            suite = next(arg.split("=", 1)[1] for arg in command if arg.startswith("--run_test="))
            sink.write_text(ET.tostring(xml_report(suite, expected_inventory()[suite]), encoding="unicode"))
        return RUNNER.ChildResult(0, "", "")

    def test_full_mock_run_has_exact_exits_counts_and_disposable_data(self):
        self.prepare_profile()
        with patch.object(RUNNER.sys, "platform", "linux"), patch.object(RUNNER, "run_child", side_effect=self.child), redirect_stdout(io.StringIO()):
            RUNNER.run_profile(self.root, self.build, self.output, "lockorder")
        report = json.loads((self.output / "summary.json").read_text())
        self.assertTrue(report["passed"])
        self.assertEqual(report["profile_header_dependencies_sha256"], RUNNER.profile_header_hashes(self.root))
        self.assertEqual(len(report["suites"]), len(RUNNER.SUITE_SOURCES))
        for row in report["suites"]:
            self.assertEqual(row["executed_cases"], len(expected_inventory()[row["name"]]))
            self.assertEqual(row["actual_exit"], 0)
        for command in self.commands_seen:
            for arg in command:
                if arg.startswith("-testdatadir="):
                    self.assertFalse(Path(arg.split("=", 1)[1]).exists())
                    self.assertNotIn("DEBUG_LOG_OUT", command)

    def test_mock_native_failure_is_retained_not_retried(self):
        self.prepare_profile()
        def failed(command, timeout, **kwargs):
            if any(arg.startswith("--run_test=") for arg in command):
                self.commands_seen.append(command)
                return RUNNER.ChildResult(66, "", "WARNING: ThreadSanitizer: data race")
            return self.child(command, timeout, **kwargs)
        with patch.object(RUNNER.sys, "platform", "linux"), patch.object(RUNNER, "run_child", side_effect=failed), redirect_stdout(io.StringIO()), self.assertRaises(RUNNER.GuardrailError):
            RUNNER.run_profile(self.root, self.build, self.output, "lockorder")
        report = json.loads((self.output / "summary.json").read_text())
        self.assertFalse(report["passed"])
        self.assertEqual([row["actual_exit"] for row in report["suites"]], [66])
        self.assertEqual(sum(any(arg.startswith("--run_test=") for arg in cmd) for cmd in self.commands_seen), 1)

    def test_nonlinux_and_existing_output_are_refused(self):
        self.prepare_profile()
        with patch.object(RUNNER.sys, "platform", "darwin"), self.assertRaisesRegex(RUNNER.GuardrailError, "requires Linux"):
            RUNNER.run_profile(self.root, self.build, self.output, "lockorder")
        self.output.mkdir()
        with patch.object(RUNNER.sys, "platform", "linux"), self.assertRaises(FileExistsError):
            RUNNER.run_profile(self.root, self.build, self.output, "lockorder")


class WorkflowTests(unittest.TestCase):
    def test_frozen_v6_suite_names_and_source_mappings_remain_independently_required(self):
        self.assertEqual(len(FROZEN_V6_SUITE_SOURCES), 27)
        self.assertTrue(set(FROZEN_V6_SUITE_SOURCES).issubset(RUNNER.SUITE_SOURCES))
        self.assertEqual(set(RUNNER.SUITE_SOURCES) - set(FROZEN_V6_SUITE_SOURCES),
                         {"flowmesh_client_outcome_evidence_tests"})
        for suite, source in FROZEN_V6_SUITE_SOURCES.items():
            with self.subTest(suite=suite):
                self.assertEqual(RUNNER.SUITE_SOURCES[suite], source)

    def test_minimal_branch_only_bounded_nonpublishing_contract(self):
        text = (ROOT / ".github" / "workflows" / "flowmesh-native-guardrails.yml").read_text()
        for required in ("workflow_dispatch:", "branches: [flowmeshV2-dev]", "contents: read",
                         "persist-credentials: false", "timeout-minutes: 120", "--parallel 2 --target test_bitcoin",
                         "profile: [lockorder, tsan]", "github.ref == 'refs/heads/flowmeshV2-dev'",
                         "python3 -B ci/test_flowmesh_native_guardrails.py", "--profile \"$GUARDRAIL_PROFILE\"",
                         "if: always()", "system libraries", "ConnectTip", "plain std::mutex"):
            self.assertIn(required, text)
        for forbidden in ("secrets.", "contents: write", "pull_request:", "release:", "git push", "continue-on-error", "retry", "upload-release"):
            self.assertNotIn(forbidden, text)
        artifact = text.split("      - name: Preserve compact results only", 1)[1]
        self.assertIn("summary.json", artifact)
        self.assertIn("/*.xml", artifact)
        for forbidden in ("/work/", "wallet", "journal", "**", "build-native/"):
            self.assertNotIn(forbidden, artifact)

    def test_profile_is_versioned_and_excludes_prototype(self):
        self.assertEqual(RUNNER.PROFILE_VERSION, "native-guardrails-v8")
        self.assertEqual(len(RUNNER.SUITE_SOURCES), 28)
        self.assertTrue(set(RUNNER.REQUIRED_CASES).issubset(RUNNER.SUITE_SOURCES))
        self.assertFalse(any("v2_market" in source or "/v2/" in source for source in RUNNER.NATIVE_SOURCES))
        self.assertEqual(len(RUNNER.REQUIRED_CASES["validation_chainstatemanager_tests"]), 8)

    def test_frozen_retention_metadata_and_p2p_regressions_are_required(self):
        self.assertTrue({
            "accepted_local_original_waits_for_authority_then_resumes_without_client_resend",
            "local_source_reservation_bounds_preserve_critical_capacity_and_original_expiry",
            "local_source_survives_queue_drop_and_cannot_be_claimed_by_remote_alias",
            "local_source_open_chain_seat_pause_preserves_retry_pacing_and_expiry",
            "local_source_stop_start_stale_tokens_and_delayed_preflight_fail_closed",
            "local_action_producer_reserves_before_acceptance_and_rejects_forgery_while_closed",
            "local_source_start_keeps_idempotent_fast_path_during_stop_join",
            "local_source_malformed_reward_credentials_do_not_shelter_behind_closed_authority",
            "seat_key_observation_publishes_without_a_tick_or_certified_state_change",
        }.issubset(RUNNER.REQUIRED_CASES["flowmesh_runtime_tests"]))
        self.assertIn("local_retention_token_is_private_queue_metadata_not_wire_or_admission",
                      RUNNER.REQUIRED_CASES["flowmesh_p2p_tests"])
        self.assertIn("src/flowmesh/p2p.cpp", RUNNER.NATIVE_SOURCES)

    def test_each_ordinary_read_regression_is_mandatory(self):
        suites = ("flowmesh_client_watch_lane_tests", "flowmesh_client_market_view_tests")
        self.assertEqual(sum(len(RUNNER.REQUIRED_CASES[suite] - ADMISSION_REQUIRED_CASES.get(suite, set())
                                 - TRACE_REQUIRED_CASES.get(suite, set())
                                 - CATALOG_REQUIRED_CASES.get(suite, set()))
                             for suite in suites), 15) # Preserve frozen13 ordinary +2 health independently.
        self.assertIn("src/node/flowmesh_https.cpp", RUNNER.NATIVE_SOURCES)
        for suite in suites:
            for case in RUNNER.REQUIRED_CASES[suite]:
                inventory = expected_inventory()
                inventory[suite].remove(case)
                with self.subTest(suite=suite, case=case), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "missing required regression"):
                    RUNNER.parse_discovery(listing(inventory))

    def test_each_exact_publication_regression_is_mandatory(self):
        runtime_cases = {
            "preagreement_local_signed_publications_resume_exactly_without_clock_advance",
            "local_agreement_publication_cache_bounds_and_peer_origin",
            "local_agreement_publication_cache_accounts_large_encoded_copies",
            "local_agreement_publication_retry_work_is_global_and_paced",
            "local_agreement_publication_reclose_and_budget_do_not_reset_pacing",
            "local_agreement_publication_normal_success_and_context_refusal_retire_copies",
            "local_agreement_publication_restart_retains_exact_journal_fallback",
        }
        self.assertTrue(runtime_cases.issubset(RUNNER.REQUIRED_CASES["flowmesh_runtime_tests"]))
        agreement_cases = RUNNER.REQUIRED_CASES["flowmesh_agreement_tests"]
        self.assertEqual(len(agreement_cases), 2)
        self.assertIn("src/node/flowmesh_agreement.cpp", RUNNER.NATIVE_SOURCES)
        for suite, cases in (("flowmesh_runtime_tests", runtime_cases),
                             ("flowmesh_agreement_tests", agreement_cases)):
            for case in cases:
                inventory = expected_inventory()
                inventory[suite].remove(case)
                with self.subTest(suite=suite, case=case), self.assertRaisesRegex(
                        RUNNER.GuardrailError, "missing required regression"):
                    RUNNER.parse_discovery(listing(inventory))


if __name__ == "__main__":
    unittest.main()
