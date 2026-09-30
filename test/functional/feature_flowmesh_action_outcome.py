#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Verified action outcomes, local admission and the opt-in outcome API.

Reuses only the latency fixture's generated four-validator bootstrap, funding
and engine-off remote client. Validator 0 offers outcomes (the default);
validator 1 runs -flowmeshapioutcome=0 and keeps the previous contract. The
client's relay to validator 1 is made unavailable for the outcome scenarios,
so every verified outcome below comes from validator 0's evidence, which the
client checks against the BLS-certified result root itself.

Scenarios: an applied bid and a cancel refused by certified state (both
certified inclusions whose outcome the client verifies); a stale cancel at an
already used sequence, proven not executed from its certified consumer, still
proven after a client restart and never resent; a lost-reply resubmission of
an executed action answered with its certified inclusion by both validators;
the local next+64 / next+65 sequence bound; validator 1's unchanged contract;
and the relay-window counters (recorded, no hard claim). Regtest only, no
outside peers. Not a latency benchmark.
"""

import http.client
import json
import ssl
import time
from pathlib import Path

from feature_flowmesh_independent import RECONCILING_REFUSAL
from feature_flowmesh_latency import FlowMeshLatencyTest, PRE_ADMISSION_REJECTIONS
from feature_flowmesh_release import TRADE_PRICE
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, p2p_port

UNKNOWN_FIELD = "Unknown or duplicate client API field"
NOT_EXPOSED = "This method is not exposed by the public trading API"


class FlowMeshActionOutcomeTest(FlowMeshLatencyTest):
    def set_test_params(self):
        self.options.latency_production_logging = True
        super().set_test_params()
        # Validator 1 keeps the previous request and response contract.
        self.extra_args[1].append("-flowmeshapioutcome=0")
        self.outcome_report = {"fixture": "generated regtest verified outcomes",
                               "scope": "correctness only; no latency claim", "correctness_pass": False}

    def raw(self, method, params, index=0):
        """One public API request, compact JSON as in-tree clients write it.
        A budget refusal (the harness shares this loopback identity with the
        client's relays) is retried after the one-second window."""
        body = json.dumps({"method": method, "params": params}, separators=(",", ":"))
        for _ in range(5):
            context = ssl.create_default_context(cafile=str(self.pki["ca"]))
            connection = http.client.HTTPSConnection("127.0.0.1", self.api_ports[index], timeout=15, context=context)
            try:
                connection.request("POST", "/flowmesh/v1", body=body, headers={"Content-Type": "application/json"})
                reply = connection.getresponse()
                status, data = reply.status, reply.read(32 * 1024 * 1024 + 1)
            finally:
                connection.close()
            if status != 429:
                return status, json.loads(data)
            time.sleep(1.1)
        raise AssertionError(f"public API budget stayed exhausted for {method}")

    def ok(self, method, params, index=0):
        status, reply = self.raw(method, params, index)
        assert_equal((status, reply["ok"]), (200, True))
        return reply["result"]

    def refused(self, method, params, index=0):
        status, reply = self.raw(method, params, index)
        assert_equal((status, reply["ok"]), (400, False))
        return reply["error"]

    def new_instruction(self, method, *params):
        """Signs once; resends only the exact retained action while the
        endpoint's reconciliation gate refuses it before admission."""
        deadline = time.monotonic() + 30
        while True:
            self.pump_b3()
            try:
                response = getattr(self.client, method)(*params)
                break
            except JSONRPCException as error:
                if error.error.get("code") != -1 or error.error.get("message") not in PRE_ADMISSION_REJECTIONS:
                    raise
                assert time.monotonic() < deadline, "pre-admission gate did not reopen"
                time.sleep(.05)
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

    def refused_instruction(self, text, method, *params):
        """Signs once and expects the endpoint's local admission to refuse it
        with `text`. A reconciliation-gate refusal before admission is
        followed by an exact-byte retry (a later refusal then reads unknown:
        the client cannot rule out the earlier delivery)."""
        response = self.new_signed(method, *params)
        deadline = time.monotonic() + 30
        while response["reason"] == RECONCILING_REFUSAL:
            assert time.monotonic() < deadline, "reconciliation gate did not reopen"
            time.sleep(.05)
            self.pump_b3()
            response = self.client.retryflowmeshaction(params[0], response["action_id"])
        assert response["receipt_state"] in {"rejected", "unknown"}, response
        assert text in response["reason"], response
        return response

    def new_signed(self, method, *params):
        deadline = time.monotonic() + 30
        while True:
            self.pump_b3()
            try:
                return getattr(self.client, method)(*params)
            except JSONRPCException as error:
                if error.error.get("code") != -1 or error.error.get("message") not in PRE_ADMISSION_REJECTIONS:
                    raise
                assert time.monotonic() < deadline, "pre-admission gate did not reopen"
                time.sleep(.05)

    def wait_status(self, market, action_id, predicate, timeout=90):
        observed = {}

        def ready():
            self.pump_b3()
            status = self.client.getflowmeshactionstatus(market, action_id, 2000)
            assert_equal(status["action_id"], action_id)
            # The verified-outcome label is exactly the presence of an outcome.
            assert_equal(status["outcome_verified"], "outcome" in status)
            observed["status"] = status
            return predicate(status)
        self.wait_until(ready, timeout=timeout, check_interval=.1)
        return observed["status"]

    def submits_for(self, action_id):
        return [row for relay in self.tls_relays for row in relay.snapshot()["requests"]
                if row["method"] == "submit" and row["action_id"] == action_id]

    def saved_row(self, market, action_id):
        rows = [row for row in self.client.listflowmeshactions(market)["actions"] if row["action_id"] == action_id]
        assert_equal(len(rows), 1)
        return rows[0]

    def check_contracts(self, market):
        """Validator 0 advertises and serves outcomes; validator 1 keeps the
        previous contract exactly."""
        rows = [row for row in self.ok("markets", {}, 0) if row["market_id"] == market]
        assert_equal(rows[0].get("action_outcome_v1"), True)
        rows = [row for row in self.ok("markets", {}, 1) if row["market_id"] == market]
        assert "action_outcome_v1" not in rows[0], rows[0]
        assert_equal(self.refused("action", {"market_id": market, "action_id": "11" * 32, "outcome": True}, 1), UNKNOWN_FIELD)
        assert_equal(self.refused("outcome_evidence", {"market_id": market, "microblock_sequence": 0,
                                                       "microblock_hash": "22" * 32}, 1), NOT_EXPOSED)
        # "outcome": false is exactly the v1 request.
        plain = self.ok("action", {"market_id": market, "action_id": "11" * 32, "outcome": False}, 0)
        assert_equal(plain["receipt_state"], "unknown")
        assert "reason_code" not in plain and "pool_notice" not in plain, plain
        assert self.refused("outcome_evidence", {"market_id": market, "microblock_sequence": 0,
                                                 "microblock_hash": "22" * 32}, 0).startswith(
            "FlowMesh outcome evidence is unavailable: "), "unknown microblock"
        self.outcome_report["contracts"] = {"validator0_advertised": True, "validator1_advertised": False}

    def run_test(self):
        try:
            market, asset = self.bootstrap_latency_market()
            account = self.fund_latency_client(market, asset)
            self.outcome_report["market_id"] = market
            self.begin_b3_workload(range(4))
            self.check_contracts(market)
            # Every client request below goes to validator 0.
            self.tls_relays[1].configure(unavailable=True)
            self.client.listflowmeshmarkets()
            first = account["next_sequence"]

            # 1. An applied bid: a verified, final "applied" outcome whose
            # fill fields come from the certified result.
            bid = self.new_instruction("submitflowmeshorder", market, "bid", TRADE_PRICE // 2, 1, first)
            bid_id = bid["action_id"]
            status = self.wait_status(market, bid_id, lambda row: row.get("outcome_verified"))
            assert_equal(status["receipt_state"], "certified_inclusion")
            assert_equal(status["certificate_verified"], True)
            outcome = status["outcome"]
            assert_equal((outcome["status"], outcome["final"], outcome["reason_code"]), ("applied", True, "APPLIED"))
            assert_equal(outcome["evidence"], "certified_result_preimage")
            assert_equal(outcome["microblock_hash"], status["microblock_hash"])
            assert_equal(outcome["fill"]["side"], "bid")
            assert_equal(status["stage"], "outcome_verified")
            bid_hash = status["microblock_hash"]
            # The raw opt-in reply carries evidence the evidence method repeats.
            raw = self.ok("action", {"market_id": market, "action_id": bid_id, "outcome": True}, 0)
            assert_equal(raw["receipt_state"], "certified_inclusion")
            evidence = self.ok("outcome_evidence", {"market_id": market, "microblock_sequence": raw["microblock_sequence"],
                                                    "microblock_hash": raw["microblock_hash"]}, 0)
            assert_equal(evidence["outcome_evidence"], raw["outcome_evidence"])
            # Validator 1 applies the bid's certificate on its own schedule,
            # possibly after the client verified it through validator 0: wait
            # (bounded) until it reports the inclusion before comparing the
            # two validators' v1 replies.
            def validator1_included():
                self.pump_b3()
                reply = self.ok("action", {"market_id": market, "action_id": bid_id}, 1)
                return reply["receipt_state"] == "certified_inclusion"
            self.wait_until(validator1_included, timeout=60, check_interval=.2)
            v1_keys = set(self.ok("action", {"market_id": market, "action_id": bid_id}, 1))
            assert_equal(set(self.ok("action", {"market_id": market, "action_id": bid_id}, 0)), v1_keys)
            self.outcome_report["applied"] = {"action_id": bid_id, "microblock_sequence": status["microblock_sequence"]}

            # 2. A cancel with no standing ask: included, and refused by
            # certified state. That is an execution outcome of an included
            # action, not a receipt-state "rejected".
            cancel = self.new_instruction("cancelflowmeshorder", market, "ask", first + 1)
            cancel_id = cancel["action_id"]
            status = self.wait_status(market, cancel_id, lambda row: row.get("outcome_verified"))
            assert_equal(status["receipt_state"], "certified_inclusion")
            assert_equal((status["outcome"]["status"], status["outcome"]["final"], status["outcome"]["reason_code"]),
                         ("rejected", True, "REJECTED_BY_STATE"))
            self.outcome_report["rejected_by_state"] = {"action_id": cancel_id}

            # 3. A lost-reply resubmission of the executed bid's exact bytes:
            # both validators answer with its certified inclusion, never
            # "rejected", and the client's retry never resends it.
            exact = self.submits_for(bid_id)[0]["action_hex"]
            for index in (0, 1):
                again = self.ok("submit", {"market_id": market, "action_id": bid_id, "action_hex": exact}, index)
                assert_equal((again["receipt_state"], again["microblock_hash"]), ("certified_inclusion", bid_hash))
            submitted = len(self.submits_for(bid_id))
            retried = self.client.retryflowmeshaction(market, bid_id)
            assert_equal(retried["receipt_state"], "certified_inclusion")
            assert_equal(len(self.submits_for(bid_id)), submitted)
            self.outcome_report["lost_reply_resubmission"] = {"action_id": bid_id, "validators": [0, 1]}

            # 5. A different action at the bid's (already used) sequence: the
            # endpoint refuses it naming the certified consumer, and the
            # client proves from the consumer's certified result that it can
            # never execute.
            stale = self.refused_instruction("already used by another certified action",
                                             "cancelflowmeshorder", market, "bid", first)
            stale_id = stale["action_id"]
            status = self.wait_status(market, stale_id, lambda row: row.get("outcome_verified"))
            assert_equal(status["receipt_state"], "rejected")
            assert_equal(status["certificate_verified"], False)
            assert_equal((status["outcome"]["status"], status["outcome"]["final"]), ("not_executed", True))
            assert_equal(status["outcome"]["reason_code"], "NOT_EXECUTED_SEQUENCE_CONSUMED")
            assert_equal(status["outcome"]["consumed_by"]["action_id"], bid_id)
            assert_equal(status["stage"], "not_executed")
            row = self.saved_row(market, stale_id)
            assert_equal(row["receipt"]["outcome"]["status"], "not_executed")
            stale_submits = len(self.submits_for(stale_id))
            assert stale_submits >= 1

            # 5 (continued). After a client restart the durable marker forbids
            # resending: unknown until proven again, then not_executed again.
            self.client.stop_node()
            self.client.start(extra_args=list(self.client_args))
            self.client.wait_for_rpc_connection()
            self.client_clock = None
            self.sync_client_time()
            self.client.addnode(f"127.0.0.1:{p2p_port(0)}", "onetry")
            self.wait_client_b3_sync()
            restored = self.saved_row(market, stale_id)["receipt"]
            assert_equal(restored["receipt_state"], "unknown")
            assert "Previously proven not executed" in restored["reason"], restored
            assert "outcome" not in restored, restored
            self.client.listflowmeshmarkets()
            retried = self.client.retryflowmeshaction(market, stale_id)
            status = retried if retried.get("outcome_verified") else self.wait_status(
                market, stale_id, lambda row: row.get("outcome_verified"))
            assert_equal(status["receipt_state"], "rejected")
            assert_equal(status["outcome"]["status"], "not_executed")
            assert_equal(status["outcome"]["consumed_by"]["action_id"], bid_id)
            assert_equal(len(self.submits_for(stale_id)), stale_submits)
            self.outcome_report["not_executed"] = {"action_id": stale_id, "consumer": bid_id,
                                                   "resubmissions_after_restart": 0}

            # 6. Local admission bounds the sequence gap at 64. Last: the admitted
            # gapped action stays queued.
            current = self.wait_client_balance(market, lambda row: row["next_sequence"] >= first + 2)["next_sequence"]
            self.refused_instruction("more than 64 ahead", "cancelflowmeshorder", market, "bid", current + 65)
            near = self.new_instruction("cancelflowmeshorder", market, "bid", current + 64)
            assert near["receipt_state"] in {"queued", "admitted"}, near
            self.outcome_report["sequence_gap"] = {"next": current, "admitted": current + 64, "refused": current + 65}

            # 9. Relay-window counters are published (recorded, no claim).
            counters = {}
            for index, node in enumerate(self.nodes):
                row = node.getflowmeshdeliveryinfo(market)["markets"][0]
                counters[str(index)] = row["action_admission"]
                for key in ("header_refused_identity", "header_refused_far", "header_refused_unproven",
                            "header_accepted_ahead", "header_restamped_behind"):
                    assert key in row["action_admission"], row["action_admission"]
            self.outcome_report["action_admission"] = counters
            self.outcome_report["correctness_pass"] = True
        except Exception as error:
            self.outcome_report["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            try:
                for relay in self.tls_relays:
                    relay.configure()
                self.end_b3_workload()
            finally:
                path = Path(self.options.tmpdir, "flowmesh-action-outcome.json")
                path.write_text(json.dumps(self.outcome_report, sort_keys=True, indent=2, default=str) + "\n",
                                encoding="utf-8")
                self.log.info("FLOWMESH_ACTION_OUTCOME_REPORT %s", path)


if __name__ == "__main__":
    FlowMeshActionOutcomeTest(__file__).main()
