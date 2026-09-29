#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Local FlowMesh health, advisory endpoint readiness and the public 'health' method.

Reuse only the latency fixture's generated four-validator bootstrap, funding
and engine-off client. Validator 0 serves the public 'health' method and the
readiness status keys (default on); validator 1 runs -flowmeshapihealth=0 and
keeps the previous contract exactly. Every validator uses the regtest-only
-flowmeshhealthtestgracems so that isolation is reported within seconds.

Checked: the capability on and off; getflowmeshhealth (rpcdoccheck) against
the public object and the certified head the market reports after an order;
health reads keep answering while B3 blocks are produced; the public method's
per-client budget; the client shows each endpoint's advertised readiness
without changing which endpoint it reads from; and stopping every FMN2 byte
relay makes validator 0 report no_flowmesh_peers until the mesh is back.
Readiness is a local observation, never evidence; nothing here is a latency
claim.
"""

import copy
import http.client
import json
import ssl
import time
from pathlib import Path

from feature_flowmesh_latency import FlowMeshLatencyTest
from feature_flowmesh_release import TRADE_PRICE
from test_framework.flowmesh_net_proxy import FlowMeshLoopbackProxy
from test_framework.util import assert_equal

GRACE_MS = 2000
READINESS_KEYS = {"health_api_version", "endpoint_ready", "endpoint_not_ready"}
# Present only with operator detail (the local RPC).
OPERATOR_SERVICE_KEYS = {"flowmesh_peers", "no_peers_ms"}
OPERATOR_MARKET_KEYS = {"error", "proposal_wait_reason"}
OPERATOR_CATCHUP_KEYS = {"peer_hint_next_sequence", "peers_hinting_ahead"}


def without_time_and_operator_fields(health):
    """The public object's comparable part: no local ages, no operator detail."""
    out = copy.deepcopy(health)
    for key in ["chain_gate_closed_ms", "worker_observation_age_ms", *OPERATOR_SERVICE_KEYS]:
        out["service"].pop(key, None)
    for row in out["markets"]:
        for key in ["head_age_ms", *OPERATOR_MARKET_KEYS]:
            row.pop(key, None)
        if "catchup" in row:
            for key in ["behind_ms", *OPERATOR_CATCHUP_KEYS]:
                row["catchup"].pop(key, None)
    return out


def not_ready_mutation(method, body):
    """Test relay: the endpoint advertises catching_up in every status object."""
    try:
        reply = json.loads(body)
    except ValueError:
        return body
    result = reply.get("result")
    objects = []
    if method == "markets" and isinstance(result, list):
        objects = result
    elif method in {"updates", "snapshot"} and isinstance(result, dict) and isinstance(result.get("status"), dict):
        objects = [result["status"]]
    for status in objects:
        status.update(health_api_version=1, endpoint_ready=False, endpoint_not_ready=["catching_up"])
    return json.dumps(reply).encode()


class FlowMeshHealthTest(FlowMeshLatencyTest):
    def set_test_params(self):
        self.options.latency_production_logging = True
        super().set_test_params()
        for args in self.extra_args:
            args.append(f"-flowmeshhealthtestgracems={GRACE_MS}")
        # Validator 1 keeps the previous request and reply contract exactly.
        self.extra_args[1].append("-flowmeshapihealth=0")
        self.health_report = {"fixture": "generated regtest restricted-API health and readiness",
                              "scope": "correctness only; no latency claim", "correctness_pass": False}

    def raw(self, method, params, index=0):
        context = ssl.create_default_context(cafile=str(self.pki["ca"]))
        connection = http.client.HTTPSConnection("127.0.0.1", self.api_ports[index], timeout=15, context=context)
        try:
            connection.request("POST", "/flowmesh/v1", body=json.dumps({"method": method, "params": params}),
                               headers={"Content-Type": "application/json"})
            reply = connection.getresponse()
            return reply.status, json.loads(reply.read(32 * 1024 * 1024 + 1))
        finally:
            connection.close()

    def status_objects(self, market, index):
        status, reply = self.raw("markets", {}, index)
        assert_equal(status, 200)
        rows = [row for row in reply["result"] if row["market_id"] == market]
        assert_equal(len(rows), 1)
        objects = [rows[0]]
        status, reply = self.raw("updates", {"market_id": market}, index)
        assert_equal(status, 200)
        objects.append(reply["result"]["status"])
        return objects

    def market_health(self, market, index=0):
        health = self.nodes[index].getflowmeshhealth(market)
        assert_equal(health["health_version"], 1)
        assert_equal(health["observation"], "local-node")
        assert_equal(len(health["markets"]), 1)
        return health, health["markets"][0]

    def check_contract(self, market):
        for row in self.status_objects(market, 0):
            assert READINESS_KEYS.issubset(row), row
            assert_equal(row["health_api_version"], 1)
            assert isinstance(row["endpoint_ready"], bool)
            assert_equal(row["endpoint_ready"], not row["endpoint_not_ready"])
        for row in self.status_objects(market, 1):
            assert not READINESS_KEYS.intersection(row), row
        status, reply = self.raw("health", {}, 1)
        assert_equal((status, reply["ok"], reply["error"]), (400, False, "This method is not exposed by the public trading API"))
        status, reply = self.raw("health", {"market_id": "11" * 32}, 0)
        assert_equal((status, reply["error"]), (400, "Unknown market"))
        status, reply = self.raw("health", {"verbose": True}, 0)
        assert_equal((status, reply["error"]), (400, "Unknown or duplicate client API field"))
        self.health_report["capability"] = {"validator0": True, "validator1": False}

    def check_head(self, market, account):
        sample = self.observe_action(market, "submitflowmeshorder", (market, "bid", TRADE_PRICE // 2, 1,
                                     account["next_sequence"]), "health_order")
        certified = sample["status"]["microblock_sequence"]

        def matches():
            self.pump_b3()
            _, row = self.market_health(market)
            data = self.nodes[0].getflowmeshmarketdata(market, {"limit": 1, "curve_limit": 1})["snapshot"]
            return (row["ready"] and row["next_microblock_sequence"] > certified and
                    row["next_microblock_sequence"] == data["next_microblock_sequence"] and
                    row["last_microblock_hash"] == data["last_microblock_hash"])
        self.wait_until(matches, timeout=30, check_interval=.1)
        health, row = self.market_health(market)
        assert_equal(health["service"]["state"], "running")
        assert_equal((row["halt"], row["active_seats"], row["quorum_required"]), ("none", 4, 3))
        assert_equal(row["not_ready"], [])
        assert row["head_age_ms"] < 60000, row
        assert_equal(row["state"], "live")
        assert_equal(row["catchup"]["behind"], False)
        assert health["service"]["flowmesh_peers"] >= 3, health["service"]
        self.health_report["head"] = {"certified_sequence": certified, "row": row}

        # The public object equals the RPC's without operator detail or local
        # ages. B3 does not advance without the harness clock pump.
        def parity():
            status, reply = self.raw("health", {"market_id": market}, 0)
            assert_equal(status, 200)
            public = reply["result"]
            assert not OPERATOR_SERVICE_KEYS.intersection(public["service"]), public
            assert not OPERATOR_MARKET_KEYS.intersection(public["markets"][0]), public
            assert not OPERATOR_CATCHUP_KEYS.intersection(public["markets"][0]["catchup"]), public
            rpc = self.nodes[0].getflowmeshhealth(market)
            return without_time_and_operator_fields(public) == without_time_and_operator_fields(rpc)
        self.wait_until(parity, timeout=10, check_interval=.3)

    def check_responsiveness(self, market):
        started_height = self.nodes[0].getblockcount()
        latencies = []
        gate_closed = 0
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and (self.nodes[0].getblockcount() < started_height + 2 or len(latencies) < 60):
            self.pump_b3()
            before = time.monotonic()
            health = self.nodes[0].getflowmeshhealth(market)
            latencies.append((time.monotonic() - before) * 1000)
            gate_closed += health["service"]["chain_gate"] == "closed"
            time.sleep(.05)
        assert self.nodes[0].getblockcount() >= started_height + 2, "B3 did not advance during the health poll"
        assert max(latencies) < 1000, max(latencies)
        self.health_report["responsiveness"] = {"polls": len(latencies), "max_ms": round(max(latencies), 3),
                                                "b3_blocks": self.nodes[0].getblockcount() - started_height,
                                                "gate_closed_observations": gate_closed}

    def check_rate_limit(self, market):
        time.sleep(1.1)
        started = time.monotonic()
        statuses = [self.raw("health", {} if i % 2 else {"market_id": market}, 0)[0] for i in range(4)]
        status, reply = self.raw("health", {}, 0)
        if time.monotonic() - started < 1:
            assert_equal(statuses, [200] * 4)
            assert_equal((status, reply["error"]), (429, "Public health request budget exhausted"))
        # The ordinary admission budget is separate and unchanged.
        assert_equal(self.raw("markets", {}, 0)[0], 200)
        time.sleep(1.1)
        assert_equal(self.raw("health", {}, 0)[0], 200)

    def client_endpoints(self):
        info = self.client.getflowmeshclientinfo()
        assert_equal(len(info["endpoints"]), 2)
        return info

    def check_client_display(self, market):
        relay0, relay1 = self.tls_relays
        self.client.getflowmeshmarketdata(market, {"limit": 1, "curve_limit": 1})
        info = self.client_endpoints()
        first, second = info["endpoints"]
        assert_equal((first["readiness"], first["readiness_market"], first["not_ready"]), ("ready", market, []))
        assert first["readiness_observed_ms"] > 0
        assert_equal((second["readiness"], second["readiness_market"], second["not_ready"]), ("unknown", "", []))
        gaps = info["event_gaps"]
        counts = [len(relay0.snapshot()["requests"]), len(relay1.snapshot()["requests"])]

        # Display only: the endpoint now advertising catching_up still serves
        # every read; the client neither switches nor resynchronizes.
        relay0.configure(reply_mutation=not_ready_mutation)
        try:
            self.client.getflowmeshmarketdata(market, {"limit": 1, "curve_limit": 1})
            self.client.listflowmeshmarkets()
            info = self.client_endpoints()
            assert_equal((info["endpoints"][0]["readiness"], info["endpoints"][0]["not_ready"]), ("not_ready", ["catching_up"]))
            assert_equal(info["event_gaps"], gaps)
            assert len(relay0.snapshot()["requests"]) > counts[0]
            assert_equal(len(relay1.snapshot()["requests"]), counts[1])
        finally:
            relay0.configure()
        self.client.getflowmeshmarketdata(market, {"limit": 1, "curve_limit": 1})
        assert_equal(self.client_endpoints()["endpoints"][0]["readiness"], "ready")
        self.health_report["client_display"] = {"not_ready_shown": True, "endpoint_switches": 0}

    def check_isolation(self, market):
        # Every FMN2 connection runs through a loopback byte relay; stopping
        # them all leaves each validator without FlowMesh peers. The grace is
        # checked on the node's own observation: no_peers_ms comes from the
        # same clock and the same instant as the verdict. This process's clock
        # cannot bound it, because a relay closes its sockets before stop()
        # has joined its threads and returned.
        reported = []

        def isolated():
            health, row = self.market_health(market)
            if "no_flowmesh_peers" not in health["not_ready"]:
                return False
            reported.append({"flowmesh_peers": health["service"].get("flowmesh_peers"),
                             "no_peers_ms": health["service"].get("no_peers_ms")})
            return row["state"] == "isolated" and not row["ready"]
        for proxy in self.proxies:
            proxy.stop()
        self.wait_until(isolated, timeout=30, check_interval=.1)
        # Never reported before the node had been without peers for the whole
        # grace by its own clock.
        for observation in reported:
            assert_equal(observation["flowmesh_peers"], 0)
            assert observation["no_peers_ms"] is not None and observation["no_peers_ms"] >= GRACE_MS, reported
        for row in self.status_objects(market, 0):
            assert_equal(row["endpoint_ready"], False)
            assert "no_flowmesh_peers" in row["endpoint_not_ready"], row
        status, reply = self.raw("health", {}, 0)
        assert_equal(status, 200)
        assert_equal(reply["result"]["service_ready"], False)
        # Validator 1's disabled contract is unchanged while it is isolated too.
        for row in self.status_objects(market, 1):
            assert not READINESS_KEYS.intersection(row), row

        keys = [proxy.target_key for proxy in self.proxies]
        self.proxies = [FlowMeshLoopbackProxy(port, target, self.fault)
                        for port, target in zip(self.proxy_ports, self.fm_ports)]
        for proxy, key in zip(self.proxies, keys):
            proxy.target_key = key
        self.wait_for_independent_mesh()

        def recovered():
            self.pump_b3()
            health, row = self.market_health(market)
            return health["service_ready"] and row["ready"]
        self.wait_until(recovered, timeout=90, check_interval=.2)
        self.health_report["isolation"] = {"first_reported_no_peers_ms": reported[0]["no_peers_ms"],
                                           "reported_polls": len(reported), "recovered": True}

    def run_test(self):
        try:
            market, asset = self.bootstrap_latency_market()
            account = self.fund_latency_client(market, asset)
            self.health_report["market_id"] = market
            self.begin_b3_workload(range(4))
            self.check_contract(market)
            self.check_head(market, account)
            self.check_responsiveness(market)
            self.check_rate_limit(market)
            self.check_client_display(market)
            self.check_isolation(market)
            self.health_report["correctness_pass"] = True
        except Exception as error:
            self.health_report["error"] = f"{type(error).__name__}: {error}"
            raise
        finally:
            try:
                self.end_b3_workload()
            finally:
                path = Path(self.options.tmpdir, "flowmesh-health.json")
                path.write_text(json.dumps(self.health_report, sort_keys=True, indent=2, default=str) + "\n", encoding="utf-8")
                self.log.info("FLOWMESH_HEALTH_REPORT %s", path)


if __name__ == "__main__":
    FlowMeshHealthTest(__file__).main()
