#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Server-side bounded 'updates' watch on the restricted HTTPS trading API.

Reuse only the latency fixture's generated four-validator bootstrap, funding
and engine-off client. Raw public-API reads go straight to validator 0
(-flowmeshapiwatch=2000) and validator 1 (the default: watches off, the
previous contract). Checked: the capability on and off and the exact error
the disabled server returns; a quiet watch ends at its deadline with an empty
page; an order's commit ends a sleeping watch early with its certified head;
a foreign cursor is answered at once with a gap; a third watcher from one
address gets 'busy' while ordinary reads keep their workers; watched replies
carry no reported_data and stay small; a validator that ran the maximum
action waiters before watches existed still starts with watches on, while an
explicit watcher count beyond the room left is refused; and stopping
validator 0 ends a sleeping watch promptly. This is not a latency benchmark.
"""

import http.client
import json
import ssl
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from feature_flowmesh_independent import RECONCILING_REFUSAL
from feature_flowmesh_latency import FlowMeshLatencyTest, PRE_ADMISSION_REJECTIONS
from feature_flowmesh_release import TRADE_PRICE
from test_framework.authproxy import JSONRPCException
from test_framework.test_node import ErrorMatch
from test_framework.util import assert_equal

WATCH_MAX_MS = 2000
WATCH_REPLY_BOUND = 512 * 1024
WATCHED_KEYS = {"cursor", "gap", "more", "oldest_event_id", "latest_event_id", "events", "status", "watch_status"}


class FlowMeshUpdatesWatchTest(FlowMeshLatencyTest):
    def set_test_params(self):
        self.options.latency_production_logging = True
        super().set_test_params()
        # Validator 0 serves watches; validator 1 keeps the default contract.
        self.extra_args[0].append(f"-flowmeshapiwatch={WATCH_MAX_MS}")
        self.watch_report = {"fixture": "generated regtest restricted-API updates watch",
                             "scope": "correctness only; no latency claim", "correctness_pass": False,
                             "reads": []}

    def raw(self, method, params, index=0):
        context = ssl.create_default_context(cafile=str(self.pki["ca"]))
        connection = http.client.HTTPSConnection("127.0.0.1", self.api_ports[index], timeout=15, context=context)
        started = time.monotonic()
        try:
            connection.request("POST", "/flowmesh/v1", body=json.dumps({"method": method, "params": params}),
                               headers={"Content-Type": "application/json"})
            reply = connection.getresponse()
            body = reply.read(32 * 1024 * 1024 + 1)
            return reply.status, json.loads(body), (time.monotonic() - started) * 1000, len(body)
        finally:
            connection.close()

    def updates(self, market, index=0, **params):
        status, reply, elapsed_ms, size = self.raw("updates", {"market_id": market, **params}, index)
        assert_equal((status, reply["ok"]), (200, True))
        result = reply["result"]
        if "watch_ms" in params:
            # The page, the status object and the hint; never reported_data.
            assert_equal(set(result), WATCHED_KEYS)
            assert size < WATCH_REPLY_BOUND, size
            self.watch_report["reads"].append({"validator": index, "watch_ms": params["watch_ms"],
                                               "elapsed_ms": round(elapsed_ms, 3), "bytes": size,
                                               "watch_status": result["watch_status"],
                                               "events": len(result["events"])})
        else:
            assert "watch_status" not in result and "reported_data" in result, result
        return result, elapsed_ms

    def new_instruction(self, method, *params):
        deadline = time.monotonic() + 30
        while True:
            self.pump_b3()
            try:
                response = getattr(self.client, method)(*params)
                break
            except JSONRPCException as error:
                # Only an explicit refusal before submission permits signing again.
                if error.error.get("code") != -1 or error.error.get("message") not in PRE_ADMISSION_REJECTIONS:
                    raise
                assert time.monotonic() < deadline, "pre-admission gate did not reopen"
                time.sleep(.05)
        # As in feature_flowmesh_action_wait.py: resend the exact retained
        # action (never a new signature) while the reconciliation gate refuses it.
        action_id = response["action_id"]
        while response["receipt_state"] not in {"queued", "admitted", "certified_inclusion"}:
            assert response["receipt_state"] in {"rejected", "unknown"}, response
            assert_equal(response["reason"], RECONCILING_REFUSAL)
            assert time.monotonic() < deadline, "reconciliation gate did not reopen"
            time.sleep(.05)
            self.pump_b3()
            response = self.client.retryflowmeshaction(params[0], action_id)
            assert_equal(response["action_id"], action_id)
        return response

    def check_capability(self, market):
        for index, advertised in ((0, WATCH_MAX_MS), (1, None)):
            status, reply, _, _ = self.raw("markets", {}, index)
            assert_equal(status, 200)
            rows = [row for row in reply["result"] if row["market_id"] == market]
            assert_equal(len(rows), 1)
            objects = [rows[0]]
            for method in ("snapshot", "updates"):
                status, reply, _, _ = self.raw(method, {"market_id": market}, index)
                assert_equal(status, 200)
                objects.append(reply["result"]["status"])
            for row in objects:
                assert_equal(row.get("updates_watch_ms_max"), advertised)
        status, reply, _, _ = self.raw("updates", {"market_id": market, "watch_ms": 500}, 1)
        assert_equal((status, reply["ok"], reply["error"]), (400, False, "Unknown or duplicate client API field"))
        self.watch_report["capability"] = {"validator0": WATCH_MAX_MS, "validator1": None}

    def quiet_position(self, market):
        """A cursor and head at which no event is pending."""
        def settled():
            self.pump_b3()
            current, _ = self.updates(market)
            return current["status"]["pending_actions"] == 0 and not current["more"]
        self.wait_until(settled, timeout=60, check_interval=.1)
        current, _ = self.updates(market)
        return current["cursor"], current["status"]["last_microblock_hash"]

    def run_test(self):
        try:
            market, asset = self.bootstrap_latency_market()
            account = self.fund_latency_client(market, asset)
            self.watch_report["market_id"] = market
            self.begin_b3_workload(range(4))
            self.check_capability(market)

            # A quiet watch ends at its deadline with an empty page. Retry
            # when a commit happened to land meanwhile.
            for _ in range(5):
                cursor, head = self.quiet_position(market)
                result, elapsed = self.updates(market, cursor=cursor, known_head=head, watch_ms=500)
                if result["watch_status"] == "timeout":
                    break
            assert_equal(result["watch_status"], "timeout")
            assert_equal(result["events"], [])
            assert_equal(result["cursor"], cursor)
            assert 450 <= elapsed < 2000, elapsed

            # Answered at once and without a slot: a foreign cursor.
            result, elapsed = self.updates(market, cursor={"instance_id": "11" * 32, "event_id": 0}, watch_ms=WATCH_MAX_MS)
            assert_equal((result["watch_status"], result["gap"]), ("gap", True))
            assert elapsed < 1000, elapsed

            # Two slots per client address. A third watcher gets the
            # immediate reply, and ordinary reads keep their workers.
            cursor, head = self.quiet_position(market)
            with ThreadPoolExecutor(max_workers=2) as pool:
                sleepers = [pool.submit(self.updates, market, cursor=cursor, known_head=head, watch_ms=1500) for _ in range(2)]
                time.sleep(.3)
                third, elapsed = self.updates(market, cursor=cursor, known_head=head, watch_ms=1500)
                assert_equal(third["watch_status"], "busy")
                assert elapsed < 1000, elapsed
                status, _, elapsed, _ = self.raw("markets", {})
                assert_equal(status, 200)
                assert elapsed < 1000, elapsed
                for sleeper in sleepers:
                    result, elapsed = sleeper.result()
                    # A commit may end them early; either way they slept.
                    assert result["watch_status"] in {"timeout", "ready"}, result
                    assert elapsed >= 250, elapsed

            # An order ends a sleeping watch early: its first event (the queue
            # admission, since this watch has no account filter) wakes it,
            # and following the stream with further watches reaches the
            # commit's certified head and the new head in the status.
            cursor, head = self.quiet_position(market)
            with ThreadPoolExecutor(max_workers=1) as pool:
                watcher = pool.submit(self.updates, market, cursor=cursor, known_head=head, watch_ms=WATCH_MAX_MS)
                time.sleep(.2)
                started = time.monotonic()
                self.new_instruction("submitflowmeshorder", market, "bid", TRADE_PRICE // 2, 1, account["next_sequence"])
                woken, elapsed = watcher.result()
            assert woken["watch_status"] in {"ready", "head_changed"}, woken
            assert elapsed < WATCH_MAX_MS - 100, elapsed
            kinds = [event["kind"] for event in woken["events"]]
            position, deadline = woken["cursor"], time.monotonic() + 30
            while "certified_head" not in kinds:
                assert time.monotonic() < deadline, kinds
                self.pump_b3()
                result, _ = self.updates(market, cursor=position, known_head=head, watch_ms=WATCH_MAX_MS)
                assert result["watch_status"] in {"ready", "head_changed", "timeout"}, result
                kinds += [event["kind"] for event in result["events"]]
                position = result["cursor"]
            final, _ = self.updates(market, cursor=position, watch_ms=0)
            assert final["status"]["last_microblock_hash"] != head, final
            self.watch_report["order_wake"] = {"first_watch_status": woken["watch_status"], "first_wake_ms": round(elapsed, 3),
                                               "kinds": kinds,
                                               "certified_after_submit_ms": round((time.monotonic() - started) * 1000, 3)}

            # Shutdown ends a sleeping watch at once rather than after its wait.
            cursor, head = self.quiet_position(market)
            with ThreadPoolExecutor(max_workers=1) as pool:
                def waiting_read():
                    try:
                        return self.raw("updates", {"market_id": market, "cursor": cursor, "known_head": head,
                                                    "watch_ms": WATCH_MAX_MS})
                    except (OSError, http.client.HTTPException) as error:
                        return type(error).__name__
                    finally:
                        ended.append(time.monotonic())
                ended = []
                pending = pool.submit(waiting_read)
                time.sleep(.3)
                stop_started = time.monotonic()
                self.stop_node(0)
                outcome = pending.result()
            interrupted_ms = (ended[0] - stop_started) * 1000
            self.watch_report["shutdown"] = {"outcome": outcome if isinstance(outcome, str) else outcome[1]["result"].get("watch_status"),
                                             "read_ended_after_stop_ms": round(interrupted_ms, 3)}
            # The watch still had about 1.7 s left when the stop began.
            assert interrupted_ms < 1200, self.watch_report["shutdown"]
            # A validator running the maximum action waiters before watches
            # existed still starts with watches on (the default count shrinks
            # to the room left); an explicit count beyond that room is refused.
            self.end_b3_workload()
            args = [arg for arg in self.nodes[1].extra_args if not arg.startswith("-flowmeshapiwatch")]
            self.stop_node(1)
            self.nodes[1].assert_start_raises_init_error(
                [*args, "-flowmeshapiactionwaiters=12", f"-flowmeshapiwatch={WATCH_MAX_MS}", "-flowmeshapiwatchers=3", "-debug=0"],
                "-flowmeshapiwatchers=3 with 12 action-wait slots exceeds 14 wait slots", match=ErrorMatch.PARTIAL_REGEX)
            self.start_node(1, extra_args=[*args, "-flowmeshapiactionwaiters=12", f"-flowmeshapiwatch={WATCH_MAX_MS}"])
            self.nodes[1].setmocktime(self.mock_time)

            def advertised():
                status, reply, _, _ = self.raw("markets", {}, 1)
                rows = [row for row in reply.get("result") or [] if row.get("market_id") == market] if status == 200 else []
                return rows[0] if rows else None
            self.wait_until(lambda: advertised() is not None, timeout=60)
            row = advertised()
            assert_equal((row["updates_watch_ms_max"], row["action_wait_ms_max"]), (WATCH_MAX_MS, 2000))
            self.watch_report["upgrade_with_12_action_waiters"] = "started"
            self.watch_report["correctness_pass"] = True
        except Exception as error:
            self.watch_report["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            try:
                self.end_b3_workload()
            finally:
                path = Path(self.options.tmpdir, "flowmesh-updates-watch.json")
                path.write_text(json.dumps(self.watch_report, sort_keys=True, indent=2, default=str) + "\n", encoding="utf-8")
                self.log.info("FLOWMESH_UPDATES_WATCH_REPORT %s", path)


if __name__ == "__main__":
    FlowMeshUpdatesWatchTest(__file__).main()
