"""Deterministic bounded-delivery and historical catch-up regressions."""
from pathlib import Path
from copy import deepcopy
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from fm_simulator import Simulator
from fm_application import canonical
from fm_protocol import PROFILE, Invalid
from fm_memory import update_durable
from test_model import Fixture


class IndexedHistory(dict):
    """Fail if the retry scheduler scans or copies the durable record table."""
    def __iter__(self):
        raise AssertionError("retry iterated durable history")

    def values(self):
        raise AssertionError("retry scanned durable history values")

    def items(self):
        raise AssertionError("retry scanned durable history items")


class RetryBoundsTests(unittest.TestCase):
    def history(self, count, n=4, nodes=None, policy=None):
        sim = Simulator(Fixture().model.snapshot(), n=n, policy=policy)
        for sequence in range(count):
            sim.offer(nodes=nodes)
            sim.run(180, stop=lambda s: s.settled(sequence + 1, nodes=nodes))
            self.assertTrue(sim.settled(sequence + 1, nodes=nodes))
            # The scheduler may lose already queued retransmissions. Durable
            # history must remain recoverable after this finite loss.
            sim.events.clear()
        return sim

    def test_retry_does_not_rebroadcast_all_retained_decisions(self):
        sim = self.history(31)
        node = sim.nodes[0]
        node.retry()
        certificates = [event for event in sim.events
                        if event.source == node.index and event.kind == "CERT"
                        and event.destination == node.index]
        self.assertLessEqual(len(certificates), 2,
                             "one retry rebroadcasts every historical decision")
        self.assertEqual(sum(rec["applied"] for rec in node.d["records"].values()), 31)

    def assert_work_bounded(self, work):
        limits = PROFILE["delivery"]
        for field, cap in (("records_inspected", "records_per_retry"),
                           ("objects_processed", "objects_per_retry"),
                           ("messages_scheduled", "messages_per_retry"),
                           ("encoded_bytes", "payload_bytes_per_retry"),
                           ("scheduled_payload_bytes", "payload_bytes_per_retry")):
            self.assertLessEqual(work[field], limits[cap], (field, work))

    def test_work_and_actual_delivery_are_bounded_at_1_4_16_31_decisions(self):
        sim = Simulator(Fixture().model.snapshot())
        measurements = []
        for sequence in range(31):
            sim.offer()
            sim.run(180, stop=lambda s: s.settled(sequence + 1))
            self.assertTrue(sim.settled(sequence + 1))
            sim.events.clear()
            if sequence + 1 not in (1, 4, 16, 31):
                continue
            node, sent = sim.nodes[0], []
            original, emit = node.d["records"], node.emit
            node.d["records"] = IndexedHistory(original)
            node.emit = lambda source, kind, data, destination: sent.append((kind, data, destination))
            node.history_requests[3] = 0
            sim.now += 1
            try:
                node.retry()
            finally:
                node.d["records"], node.emit = original, emit
            work = deepcopy(node.retry_work)
            self.assert_work_bounded(work)
            self.assertEqual(work["records_inspected"], 3)
            self.assertEqual(work["messages_scheduled"], sum(sim.n if dest is None else 1
                                                           for _, _, dest in sent))
            self.assertEqual(work["encoded_bytes"], sum(len(canonical(data)) for _, data, _ in sent))
            self.assertEqual(work["scheduled_payload_bytes"], sum(len(canonical(data)) *
                             (sim.n if dest is None else 1) for _, data, dest in sent))
            self.assertEqual(sum(rec["applied"] for rec in original.values()), sequence + 1)
            measurements.append(work)
        # Structural selection work is constant while retained history grows.
        self.assertEqual(len({w["objects_processed"] for w in measurements}), 1)
        self.assertEqual(len({w["messages_scheduled"] for w in measurements}), 1)
        from fm_checker import check
        check(sim)

    def test_laggard_discovers_and_fetches_missed_history_after_restart(self):
        sim = self.history(8, nodes=[0, 1, 2],
                           policy=lambda source, target, kind, data:
                           None if source == 3 or target == 3 else 1)
        laggard = sim.nodes[3]
        self.assertEqual(laggard.d["sequence"], 0)
        self.assertEqual(laggard.record["signed"], {})
        sim.policy = None
        laggard.crash()
        laggard.restart()
        sim.run(400, stop=lambda s: s.settled(8))
        self.assertTrue(sim.settled(8), [n.last_reason for n in sim.nodes])
        self.assertEqual(laggard.d["parent"], sim.nodes[0].d["parent"])
        self.assertTrue(all(rec["apply_count"] == 1 for rec in laggard.d["records"].values()
                            if rec["applied"]))
        self.assertTrue(any(event["event"] == "delivered" and event["kind"] == "GET_CERT"
                            for event in sim.trace))
        from fm_checker import check
        check(sim)

    def test_historical_request_lane_is_fair_with_current_work(self):
        sim = self.history(4)
        node = sim.nodes[0]
        sim.offer(nodes=[0])
        for peer in range(sim.n):
            for sequence in (2, 0, 1, 0):
                node._delivery_handle("GET_CERT", {"config": node.config, "sequence": sequence}, peer)
        self.assertEqual(len(node.history_requests), sim.n)
        served = set()
        for _ in range(sim.n):
            sim.now += 1
            sim.events.clear()
            node.retry()
            self.assert_work_bounded(node.retry_work)
            served.update(event.destination for event in sim.events if event.source == 0
                          and event.kind == "CERT"
                          and event.data["prepared"]["proposal"]["payload"]["instance"]["sequence"] == 0)
            # Repeated requests from the first peer stay behind waiting peers.
            node._delivery_handle("GET_CERT", {"config": node.config, "sequence": 0}, 0)
        self.assertEqual(served, set(range(sim.n)))

    def test_untrusted_status_and_fetch_requests_cannot_vote_or_grow_without_bound(self):
        sim = Simulator(Fixture().model.snapshot())
        node = sim.nodes[1]
        original = deepcopy(node.d)
        for peer in range(sim.n):
            for _ in range(40):
                node._delivery_handle("STATUS", {"config": node.config, "next_sequence": 32}, peer)
                node._delivery_handle("GET_CERT", {"config": node.config, "sequence": 31}, peer)
        self.assertLessEqual(len(node.peer_status), sim.n)
        self.assertLessEqual(len(node._history_sent), sim.n)
        self.assertLessEqual(len(node.history_requests), sim.n)
        self.assertEqual(node.d, original)
        self.assertIsNone(node.deadline)
        for kind, data, source in (
                ("STATUS", {"config": node.config, "next_sequence": True}, 0),
                ("STATUS", {"config": "0" * 64, "next_sequence": 1}, 0),
                ("GET_CERT", {"config": node.config, "sequence": 32}, 0),
                ("GET_CERT", {"config": node.config, "sequence": -1}, 0),
                ("STATUS", {"config": node.config, "next_sequence": 1}, 1000),
                ("STATUS", {"config": node.config, "next_sequence": 1, "extra": []}, 0)):
            with self.assertRaises(Invalid):
                node._delivery_handle(kind, data, source)
        self.assertEqual(node.d, original)

    def test_same_tick_retries_share_budget_before_encoding_and_fanout(self):
        sim = self.history(1, n=7)
        node = sim.nodes[0]
        sim.now += 1
        sim.events.clear()
        for _ in range(100):
            node.retry()
        self.assert_work_bounded(node.retry_work)
        self.assertEqual(node.retry_work["messages_scheduled"],
                         sum(event.source == node.index for event in sim.events))
        before = deepcopy(node.retry_work)
        node.retry()
        self.assertEqual(node.retry_work, before)

    def test_old_view_signatures_retransmit_exactly_without_being_resigned(self):
        sim = Simulator(Fixture().model.snapshot())
        node = sim.nodes[1]
        for view in range(1, PROFILE["limits"]["views"]):
            node.change_view(view)
        expected = deepcopy(node.record["signed"])
        issued = deepcopy(sim.authentication.issued)
        sim.events.clear()
        for _ in range(6):
            sim.now += 1
            node.retry()
            self.assert_work_bounded(node.retry_work)
        published = {event.data["auth"] for event in sim.events
                     if event.source == node.index and event.kind == "SIGNED"}
        self.assertEqual(published, {signed["auth"] for signed in expected.values()})
        self.assertEqual(node.record["signed"], expected)
        self.assertEqual(sim.authentication.issued, issued)

    def test_exhausted_budget_stops_before_payload_encoding(self):
        sim = Simulator(Fixture().model.snapshot())
        node = sim.nodes[0]
        node.retry()
        sim.events.clear()
        limit = PROFILE["delivery"]["payload_bytes_per_retry"]
        node.retry_work["encoded_bytes"] = limit - PROFILE["limits"]["proof_bytes"] + 1
        node._delivery_in_retry = True
        with patch("fm_delivery.canonical", side_effect=AssertionError("encoded after budget exhausted")):
            node._send("STATUS", {"config": node.config, "next_sequence": 0})
        self.assertEqual(sim.events, [])
        node.retry_work["encoded_bytes"] = 0
        node.retry_work["messages_scheduled"] = PROFILE["delivery"]["messages_per_retry"] - sim.n + 1
        with patch("fm_delivery.canonical", side_effect=AssertionError("encoded after fanout budget exhausted")):
            node._send("STATUS", {"config": node.config, "next_sequence": 0})
        self.assertEqual(sim.events, [])
        node._delivery_in_retry = False

    def test_exhausted_same_tick_retry_preserves_unserved_history_request(self):
        sim = self.history(1)
        node = sim.nodes[0]
        sim.now += 1
        for _ in range(100):
            node.retry()
        sim.events.clear()
        node._delivery_handle("GET_CERT", {"config": node.config, "sequence": 0}, 3)
        node.retry()
        self.assertEqual(node.history_requests.get(3), 0)
        self.assertEqual(sim.events, [])
        sim.now += 1
        node.retry()
        self.assertNotIn(3, node.history_requests)
        self.assertTrue(any(event.kind == "CERT" and event.destination == 3 for event in sim.events))

    def test_large_current_proofs_cannot_starve_history_discovery(self):
        sim = Simulator(Fixture().model.snapshot(), n=7)
        node = sim.nodes[1]
        for view in range(1, PROFILE["limits"]["views"]):
            node.change_view(view)
        node.peer_status[0] = 1
        original_codec = canonical

        def maximum_signed_cost(data):
            # Charge real retained signatures their maximum permitted encoded
            # cost without manufacturing a large or invalid voting proof.
            if type(data) is dict and set(data) == {"payload", "auth"}:
                return b"x" * PROFILE["limits"]["proof_bytes"]
            return original_codec(data)

        for _ in range(3):
            sim.now += 1
            sim.events.clear()
            with patch("fm_delivery.canonical", side_effect=maximum_signed_cost):
                node.retry()
            self.assert_work_bounded(node.retry_work)
            kinds = {event.kind for event in sim.events if event.source == node.index}
            self.assertIn("STATUS", kinds)
            self.assertIn("GET_CERT", kinds)
            self.assertIn("SIGNED", kinds)

    def test_atomic_update_copies_touched_records_without_scanning_history(self):
        class Uncopyable:
            def __deepcopy__(self, memo):
                raise AssertionError("copied an untouched historical record")

        current = {"accepted": {}, "signed": {}}
        records = IndexedHistory({i: Uncopyable() for i in range(31)})
        records[31] = current
        old = {"records": records, "retained_bodies": {}, "anchor": {"height": 1}, "sequence": 31}

        def mutation(candidate):
            candidate["records"][31]["accepted"][0] = {"test": 1}
            candidate["records"][31]["signed"]["slot"] = {"test": 2}
            candidate["records"][32] = {"accepted": {}, "signed": {}}
            candidate["sequence"] = 32

        updated, stats = update_durable(old, mutation)
        self.assertEqual(stats["records_copied"], 1)
        self.assertEqual(stats["records_written"], 2)
        self.assertEqual(current, {"accepted": {}, "signed": {}})
        self.assertEqual(updated["records"][31]["accepted"], {0: {"test": 1}})
        self.assertEqual(updated["sequence"], 32)

    def test_restart_retry_can_finish_an_intent_without_copying_history(self):
        sim = self.history(4)
        node = sim.nodes[0]
        node.cut = ("after_intent", "PROPOSE")
        sim.offer(nodes=[0])
        self.assertFalse(node.alive)
        self.assertIn(("PROPOSE", 0), node.record["intents"])
        self.assertNotIn(("PROPOSE", 0), node.record["signed"])
        retry = node.retry

        def indexed_retry():
            # Restart reconstruction may scan retained safety evidence. The
            # following actual retry, including durable intent completion,
            # must only touch indexed records.
            node.d["records"] = IndexedHistory(node.d["records"])
            try:
                retry()
            finally:
                node.d["records"] = dict.copy(node.d["records"])

        node.retry = indexed_retry
        node.restart()
        node.retry = retry
        self.assertIn(("PROPOSE", 0), node.record["signed"])
        self.assert_work_bounded(node.retry_work)
        sim.run(100, stop=lambda s: s.settled(5))
        self.assertTrue(sim.settled(5))
        from fm_checker import check
        check(sim)

    def test_failed_atomic_update_leaves_all_durable_state_unchanged(self):
        old = {"records": {0: {"accepted": {}}}, "retained_bodies": {"value": {"body": 1}},
               "anchor": {"height": 1}, "sequence": 0}
        before = deepcopy(old)

        def failure(candidate):
            candidate["records"][0]["accepted"][0] = {"test": 1}
            candidate["records"][1] = {}
            candidate["retained_bodies"]["value"]["body"] = 2
            del candidate["retained_bodies"]["value"]
            candidate["anchor"]["height"] = 2
            candidate["sequence"] = 1
            raise RuntimeError("injected mutation failure")

        with self.assertRaises(RuntimeError):
            update_durable(old, failure)
        self.assertEqual(old, before)


if __name__ == "__main__":
    unittest.main()
