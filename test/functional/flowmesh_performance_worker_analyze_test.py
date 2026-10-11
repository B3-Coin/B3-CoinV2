#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Focused offline synthetic tests; no daemon, keys or benchmark fixture."""

import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import flowmesh_performance_worker_analyze as analyzer


def event(family="FlowMeshTrace", stage="agreement_span", time=11000, **values):
    row = {"trace_stream": family, "stage": stage, "monotonic_us": time,
           "node": "0", "source": "synthetic.log", "segment": 0, "line": 1}
    row.update(values)
    return row


def aggregates(result):
    return {row["stage"]: row for row in result["ranked_inclusive_bracket_aggregates"]}


def wire(**values):
    row = {"market_id": "market", "epoch": 0, "sequence": 12, "peer": 3, "wire_hash": "exact-wire"}
    row.update(values)
    return row


class WorkerAnalyzerTest(unittest.TestCase):
    def test_nested_spans_are_inclusive_not_summed_or_subtracted(self):
        child = event(time=8000, started_us=5000, market_id="m", span_id=2,
                      parent_span_id=1, reason="journal_write_batch_sync")
        parent = event(started_us=1000, market_id="m", span_id=1,
                       parent_span_id=0, reason="persist_signing_intent", line=2)
        result = analyzer.analyze([child, parent])
        rows = aggregates(result)
        self.assertEqual(rows["agreement.persist_signing_intent"]["max_ms"], 10)
        self.assertEqual(rows["agreement.journal_write_batch_sync"]["max_ms"], 3)
        self.assertEqual(rows["agreement.persist_signing_intent.before_sync_including_serialization"]["max_ms"], 4)
        self.assertEqual(result["agreement_nesting"], {"matched": 1, "root": 1})
        self.assertTrue(all("sum_ms" not in row for row in rows.values()))
        self.assertNotIn("exclusive_ms", json.dumps(result))

    def test_missing_duplicate_and_noncontaining_parents_are_unknown(self):
        child = event(started_us=1000, market_id="m", span_id=2,
                      parent_span_id=1, reason="journal_write_batch_sync")
        missing = analyzer.analyze([child])
        self.assertEqual(missing["agreement_nesting"], {"missing_parent": 1})
        parent = event(time=5000, started_us=2000, market_id="m", span_id=1,
                       parent_span_id=0, reason="persist_candidate")
        result = analyzer.analyze([child, parent])
        self.assertEqual(result["agreement_nesting"]["parent_not_containing_child"], 1)
        duplicate = analyzer.analyze([child, parent, dict(parent, line=4)])
        self.assertEqual(duplicate["agreement_nesting"]["ambiguous_parent"], 1)
        self.assertFalse(any("before_sync" in row["stage"] for row in duplicate["largest_intervals"]))

    def test_zero_missing_and_negative_endpoints_are_not_zero_latency(self):
        rows = [event("FlowMeshStoreTrace", "append_execution", sync_started_us=10000, sync_completed_us=9000),
                event("FlowMeshStoreTrace", "lock_candidate", sync_started_us=0, sync_completed_us=0),
                event("FlowMeshStoreTrace", "append_execution", sync_started_us=1000, sync_completed_us=0)]
        result = analyzer.analyze(rows)
        self.assertEqual(len(result["invalid_intervals"]), 1)
        self.assertEqual(result["invalid_intervals"][0]["elapsed_ms"], -1)
        self.assertNotIn("store.append_execution.synchronous_db_call_not_fsync_only", aggregates(result))
        self.assertIn("store.append_execution.synchronous_db_call_not_fsync_only", result["missing_or_invalid_brackets"])

    def test_unique_queue_match_and_observed_wake_bracket(self):
        ingress = event("FlowMeshWorkerTrace", "wire_enqueue", time=4000, queue_result=0,
                        enqueued_us=2500, notified_us=3000, notify_completed_us=3500,
                        worker_waiting=True, queue_before=1, queue_after=2, **wire())
        worker = event("FlowMeshWorkerTrace", "worker_iteration", time=12000, work="message",
                       wait_started_us=1000, wait_returned_us=5000, dequeued_us=6000,
                       work_started_us=7000, processing_completed_us=10000,
                       queue_before=2, queue_after=1, **wire())
        result = analyzer.analyze([worker, ingress])  # Completion-log order need not equal start order.
        rows = aggregates(result)
        self.assertEqual(rows["worker.unique_observed_enqueue_to_dequeue"]["max_ms"], 3.5)
        wake = "worker.observed_notification_to_cv_return_including_scheduling_and_reacquire"
        self.assertEqual(rows[wake]["max_ms"], 2)
        self.assertEqual(result["queue_observations"][0]["max_observed_queue_after"], 2)
        self.assertIn("cannot separate", result["measurement_limits"]["wake"])

    def test_duplicate_coalesced_and_cross_process_queue_work_is_not_guessed(self):
        ingress = event("FlowMeshWorkerTrace", "wire_enqueue", queue_result=0, enqueued_us=1000, **wire())
        worker = event("FlowMeshWorkerTrace", "worker_iteration", work="message", dequeued_us=5000, **wire())
        duplicate = analyzer.analyze([ingress, dict(ingress, line=2), worker])
        self.assertEqual(duplicate["queue_correlations"], {"ambiguous_repeated_wire": 1})
        self.assertNotIn("worker.unique_observed_enqueue_to_dequeue", aggregates(duplicate))
        coalesced = analyzer.analyze([dict(ingress, coalesced=True), worker])
        self.assertEqual(coalesced["queue_correlations"], {"unmatched": 1})
        for changed in ({"node": "1"}, {"segment": 1}, {"source": "other.log"}, {"market_id": "other"}):
            result = analyzer.analyze([ingress, dict(worker, **changed)])
            self.assertNotIn("worker.unique_observed_enqueue_to_dequeue", aggregates(result))

    def test_market_mutex_service_and_gate_brackets(self):
        rows = [event(stage="message_processing", reason="dequeue_us=1000 lock_request_us=2000 locked_us=5000"),
                event("FlowMeshServiceTrace", "chain_acceptable", timing={"started_us": 1000,
                      "cs_main_requested_us": 2000, "cs_main_acquired_us": 4000, "chain_reconciling": False}),
                event("FlowMeshServiceTrace", "gate_closed", time=1000),
                event("FlowMeshServiceTrace", "gate_closed", time=2000),
                event("FlowMeshServiceTrace", "gate_opened", time=9000),
                event("FlowMeshServiceTrace", "gate_closed", time=12000)]
        result = analyzer.analyze(rows)
        measured = aggregates(result)
        self.assertEqual(measured["runtime.market_mutex_wait"]["max_ms"], 3)
        self.assertEqual(measured["service.chain_acceptable.cs_main_wait"]["max_ms"], 2)
        self.assertEqual(measured["service.observed_reconciliation_gate_closed"]["max_ms"], 8)
        self.assertEqual(result["reconciliation_coverage"]["closed_without_observed_open"], 1)

    def test_store_sync_is_not_pure_fsync_and_handoff_validation_is_broad(self):
        row = event("FlowMeshStoreTrace", "append_handoff", started_us=1000,
                    certificate_started_us=2000, certificate_completed_us=5000,
                    sync_started_us=7000, sync_completed_us=9000)
        result = analyzer.analyze([row])
        measured = aggregates(result)
        self.assertEqual(measured["store.append_handoff.handoff_validation_including_certificate"]["max_ms"], 3)
        self.assertEqual(measured["store.append_handoff.synchronous_db_call_not_fsync_only"]["max_ms"], 2)
        self.assertIn("Device fsync alone remains unknown", result["measurement_limits"]["fsync"])

    def test_old_gap_is_not_retroactively_attributed(self):
        rows = [event(stage="execution_completed", time=1390036113748),
                event(stage="agreement_delivery_attempt", time=1390037396680, reason="stage=4 view=1")]
        result = analyzer.analyze(rows)
        self.assertFalse(result["new_agreement_spans_available"])
        self.assertEqual(result["historical_1282_932_ms_gap"]["attribution"], "unknown")
        self.assertEqual(result["interval_count"], 0)

    def test_strict_outlier_threshold_and_bounded_details(self):
        result = analyzer.analyze([event(stage="work", started_us=1000, time=end)
                                   for end in (601000, 601001, 801000)], detail_limit=1)
        self.assertEqual(result["outlier_count"], 2)
        self.assertEqual(len(result["outliers"]), 1)
        self.assertEqual(result["largest_intervals"][0]["elapsed_ms"], 800)

    def test_parser_all_streams_malformed_restart_and_every_limit_marker(self):
        rows = ["ordinary log\n", "FlowMeshTrace broken-json\n"]
        for family in ("FlowMeshTrace", "FlowMeshBenchTrace", "FlowMeshWorkerTrace", "FlowMeshServiceTrace", "FlowMeshStoreTrace"):
            row = {"stage": "trace_limit_reached", "monotonic_us": 1000, "stream": "timing_spans"}
            rows.append("prefix " + family + " " + json.dumps(row) + "\n")
        rows += ["B3Coin Core version vtest\n",
                 'FlowMeshTrace {"stage":"x","monotonic_us":2000,"market_id":"m","event_id":10}\n',
                 'FlowMeshTrace {"stage":"x","monotonic_us":1900,"market_id":"m","event_id":11}\n',
                 'FlowMeshTrace {"stage":"x","monotonic_us":3000,"market_id":"m","event_id":1}\n']
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "debug.log"
            path.write_text("".join(rows), encoding="utf-8")
            events, coverage = analyzer.read_traces([("0", path)])
        self.assertEqual(coverage[0]["counts"]["malformed"], 1)
        self.assertEqual(len(coverage[0]["truncation_markers"]), 5)
        self.assertEqual(coverage[0]["truncation_markers"][3]["stream"], "timing_spans")
        self.assertEqual(coverage[0]["detected_segments"], 3)
        self.assertEqual([row["segment"] for row in events[-3:]], [1, 1, 2])
        result = analyzer.analyze(events, coverage)
        self.assertTrue(result["censored_capture"])
        self.assertEqual(result["interval_count"], 0)

    def test_private_bench_ids_never_reset_operational_cursor(self):
        rows = [event(stage="message_processing", event_id=10, market_id="m"),
                event("FlowMeshBenchTrace", "agreement_span", diagnostic_event_id=1,
                      market_id="m", started_us=1000, span_id=1, parent_span_id=0, reason="pump"),
                event(stage="message_processing", event_id=11, market_id="m")]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "debug.log"
            path.write_text("".join(row["trace_stream"] + " " + json.dumps(row) + "\n" for row in rows), encoding="utf-8")
            parsed, coverage = analyzer.read_traces([("0", path)])
        self.assertEqual(coverage[0]["detected_segments"], 1)
        result = analyzer.analyze(parsed, coverage)
        self.assertEqual(aggregates(result)["agreement.pump"]["max_ms"], 10)

    def test_malformed_field_types_are_rejected_and_absent_caps_do_not_prove_completeness(self):
        for changes in ({"market_id": []}, {"sequence": True}, {"reason": {}},
                        {"timing": []}, {"started_us": True}, {"monotonic_us": 1.5}):
            self.assertFalse(analyzer.valid_event(event(**changes)))
        self.assertIsNone(analyzer.analyze([])["censored_capture"])

    def test_resource_bounds_fail_closed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "debug.log"
            path.write_text('FlowMeshTrace {"stage":"x","monotonic_us":1}\n', encoding="utf-8")
            for field, limit in (("MAX_FILE_BYTES", 1), ("MAX_TOTAL_BYTES", 1), ("MAX_LINE_BYTES", 1), ("MAX_EVENTS", 0)):
                with self.subTest(field=field), patch.object(analyzer, field, limit):
                    with self.assertRaises(ValueError):
                        analyzer.read_traces([("0", path)])
            with patch.object(analyzer, "MAX_FILES", 0):
                with self.assertRaises(ValueError):
                    analyzer.read_traces([("0", path)])
            with self.assertRaises(ValueError):
                analyzer.read_traces([("0", path), ("1", path)])
        with patch.object(analyzer, "MAX_INTERVALS", 0):
            with self.assertRaises(ValueError):
                analyzer.analyze([event(stage="work", started_us=1000)])


if __name__ == "__main__":
    unittest.main()
