#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Four isolated headless FN operators: FMN2 delivery while B3 advances.

Reuse real regtest seats, deposits, fee-exact trading and settlement fixtures.
The additional workload measures RPC admission, client-observed certified
state, and each replica's fixed-target application separately. A loopback
proxy holds/throttles only BULK during two graceful disconnect/catch-up cycles.
No SIGKILL, production wallet, key migration, journal reset or quorum change.

This is same-machine network emulation, not WAN or engine-off remote-client
qualification. RPC observations are trusted local durable-runtime reads, not
new balance proofs. Socket writes and arming are not delivery/signing proofs.
"""

import json
import math
import time
from decimal import Decimal
from pathlib import Path

from feature_flowmesh_release import FlowMeshReleaseTest, TRADE_PRICE
from feature_flowmesh_speed import collect_stall_diagnostic
from test_framework.flowmesh_net_proxy import BulkFaultProfile, FlowMeshLoopbackProxy
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal, p2p_port


FLOWMESH_COMMANDS = ("fmhello", "fmaction", "fmprop", "fmattest", "fmcert", "fmget", "fmentries")


class FlowMeshIndependentTest(FlowMeshReleaseTest):
    def set_test_params(self):
        super().set_test_params()
        # Four B3 ports, four FMN2 listeners, four bounded loopback proxies.
        self.fm_ports = [p2p_port(4 + index) for index in range(self.num_nodes)]
        self.proxy_ports = [p2p_port(8 + index) for index in range(self.num_nodes)]
        self.proxies = []
        self.fault = BulkFaultProfile()
        self.clock_indices = None
        self.next_clock_tick = 0
        self.samples = []
        self.recoveries = []
        self.fm_args = [
            ["-flowmeshtransport=independent", "-flowmeshrole=validator",
             "-flowmeshlisten=1", "-flowmeshbind=127.0.0.1",
             f"-flowmeshport={self.fm_ports[index]}",
             *[f"-flowmeshconnect=127.0.0.1:{port}"
               for other, port in enumerate(self.proxy_ports) if other != index]]
            for index in range(self.num_nodes)
        ]
        self.extra_args = [list(args) + self.fm_args[index]
                           for index, args in enumerate(self.extra_args)]
        self.operator_keys = None
        self.last_market = None

    def restart_node(self, i, extra_args=None, clear_addrman=False, *, expected_stderr=""):
        # The inherited clean-reindex test supplies explicit B3_ARGS. Never
        # let that replacement silently restore legacy FlowMesh transport.
        if extra_args is not None:
            extra_args = [arg for arg in extra_args
                          if not arg.startswith(("-flowmeshtransport=", "-flowmeshrole=",
                                                 "-flowmeshlisten=", "-flowmeshbind=",
                                                 "-flowmeshport=", "-flowmeshconnect="))]
            extra_args += self.fm_args[i]
        super().restart_node(i, extra_args, clear_addrman, expected_stderr=expected_stderr)

    def setup_network(self):
        for port, target in zip(self.proxy_ports, self.fm_ports):
            self.proxies.append(FlowMeshLoopbackProxy(port, target, self.fault))
        super().setup_network()
        self.wait_for_independent_mesh()

    def wait_for_independent_mesh(self, indices=None):
        indices = list(range(self.num_nodes)) if indices is None else list(indices)

        def ready():
            infos = {index: self.nodes[index].getflowmeshnetworkinfo() for index in indices}
            keys = {index: info["operator_pubkey"] for index, info in infos.items()}
            if any(not key for key in keys.values()) or len(set(keys.values())) != len(indices):
                return False
            for index, info in infos.items():
                assert_equal(info["mode"], "independent")
                assert_equal(info["legacy_enabled"], False)
                assert_equal(info["connection_is_quorum_proof"], False)
                if not info["running"] or not info["listening"] or info["error"]:
                    return False
                ready_keys = {peer["operator_pubkey"] for peer in info["peers"]
                              if peer["authenticated"] and peer["live"] and peer["actions"] and peer["bulk"]}
                expected = {key for other, key in keys.items() if other != index}
                if not expected.issubset(ready_keys):
                    return False
            if self.operator_keys is None and len(indices) == self.num_nodes:
                self.operator_keys = keys
                for index, key in keys.items():
                    self.proxies[index].target_key = key
            elif self.operator_keys is not None:
                for index, key in keys.items():
                    assert_equal(key, self.operator_keys[index])
            return True

        try:
            self.wait_until(ready, timeout=90, check_interval=0.1)
        except AssertionError:
            self.dump_diagnostics(indices, self.last_market)
            raise
        self.assert_no_b3_flowmesh_traffic(indices)

    def assert_no_b3_flowmesh_traffic(self, indices=None):
        indices = range(self.num_nodes) if indices is None else indices
        for index in indices:
            node = self.nodes[index]
            info = node.getflowmeshnetworkinfo()
            assert_equal(info["mode"], "independent")
            assert_equal(info["legacy_enabled"], False)
            for peer in node.getpeerinfo():
                for field in ("bytesrecv_per_msg", "bytessent_per_msg"):
                    for command in FLOWMESH_COMMANDS:
                        assert_equal(peer.get(field, {}).get(command, 0), 0)

    def dump_diagnostics(self, indices, market_id, account_sequence=None):
        for index in indices:
            node = self.nodes[index]
            try:
                network = node.getflowmeshnetworkinfo()
                self.log.error("FMNET_NETWORK_DIAGNOSTIC %s", json.dumps(
                    {"node": index, "network": network}, sort_keys=True))
            except Exception as error:
                self.log.error("FMNET_NETWORK_DIAGNOSTIC node=%d error_type=%s",
                               index, type(error).__name__)
            if market_id is not None:
                diagnostic = collect_stall_diagnostic(node, index, market_id, account_sequence)
                self.log.error("FMNET_RUNTIME_DIAGNOSTIC %s",
                               json.dumps(diagnostic, sort_keys=True, default=str))

    def shutdown(self):
        # Stop test daemons before their byte relays; preserve normal framework
        # cleanup/reporting even if setup or a qualification assertion failed.
        try:
            return super().shutdown()
        finally:
            for proxy in self.proxies:
                proxy.stop()

    @staticmethod
    def target(snapshot):
        assert snapshot["certified"]
        assert snapshot["next_microblock_sequence"] > 0
        return {"sequence": snapshot["next_microblock_sequence"] - 1,
                "hash": snapshot["last_microblock_hash"],
                "state_root": snapshot["state_root"]}

    def target_applied(self, index, market_id, target, *, require_signing=False,
                       require_unpaused=False):
        data = self.nodes[index].getflowmeshmarketdata(
            market_id, {"limit": 100, "curve_limit": 1})
        snapshot = data["snapshot"]
        if (not snapshot["certified"] or not snapshot["running"] or
                snapshot["halt"] != "none" or
                (require_signing and snapshot["observer_only"]) or
                (require_unpaused and snapshot["paused"])):
            return False
        if snapshot["next_microblock_sequence"] <= target["sequence"]:
            return False
        if snapshot["next_microblock_sequence"] == target["sequence"] + 1:
            assert_equal(snapshot["last_microblock_hash"], target["hash"])
            assert_equal(snapshot["state_root"], target["state_root"])
            return True
        # History comes from this replica's durably applied certified log.
        # A greater sequence alone is NOT convergence; the exact ancestor
        # must appear. Workloads stay below the bounded 100-entry window.
        matches = [entry for entry in data["history"]["entries"]
                   if entry["sequence"] == target["sequence"]]
        if not matches:
            return False
        assert_equal(len(matches), 1)
        assert_equal(matches[0]["microblock_hash"], target["hash"])
        return True

    def wait_for_market_convergence(self, market_id, require_unpaused=True):
        self.last_market = market_id
        holder = {}

        def choose():
            status = self.market_status(self.nodes[0], market_id)
            if status is None or status["next_microblock_sequence"] == 0:
                return False
            data = self.nodes[0].getflowmeshmarketdata(market_id, {"limit": 1})
            if not data["snapshot"]["certified"]:
                return False
            holder.update(self.target(data["snapshot"]))
            return True

        try:
            self.wait_until(choose, timeout=120, check_interval=0.05)
            self.wait_for_target(market_id, holder, range(self.num_nodes),
                                 require_signing=True, require_unpaused=require_unpaused)
        except AssertionError:
            self.dump_diagnostics(range(self.num_nodes), market_id)
            raise
        self.assert_no_b3_flowmesh_traffic()

    def wait_for_target(self, market_id, target, indices, *, started=None,
                        require_signing=False, require_unpaused=False):
        started = time.monotonic() if started is None else started
        pending = set(indices)
        observed = {}

        def observe():
            self.pump_b3()
            for index in sorted(pending):
                if self.target_applied(index, market_id, target,
                                       require_signing=require_signing,
                                       require_unpaused=require_unpaused):
                    observed[str(index)] = (time.monotonic() - started) * 1000
                    pending.remove(index)
            return not pending

        try:
            self.wait_until(observe, timeout=120, check_interval=0.025)
        except AssertionError:
            self.dump_diagnostics(sorted(pending), market_id)
            raise
        return observed

    def pump_b3(self):
        """All RPC calls remain on the harness thread, including mock-time."""
        if self.clock_indices is None or time.monotonic() < self.next_clock_tick:
            return
        self.next_clock_tick = time.monotonic() + 0.5
        schedules = []
        for index in self.clock_indices:
            staking = self.nodes[index].getstakinginfo()["staking"]
            if staking["running"] and "next_block_time" in staking:
                schedules.append(staking["next_block_time"])
        future = [slot for slot in schedules if slot > self.mock_time]
        self.mock_time = min(future) if future else self.mock_time + 1
        for index in self.clock_indices:
            self.nodes[index].setmocktime(self.mock_time)

    def begin_b3_workload(self, indices):
        self.clock_indices = list(indices)
        for index in indices:
            assert_equal(self.nodes[index].startstaking()["running"], True)
        self.next_clock_tick = 0

    def end_b3_workload(self):
        if self.clock_indices is not None:
            for index in self.clock_indices:
                if self.nodes[index].running:
                    assert_equal(self.nodes[index].stopstaking()["running"], False)
        self.clock_indices = None
        self.pos_running = False

    def wait_for_new_b3_block(self, old_height):
        def advanced():
            self.pump_b3()
            return all(self.nodes[index].getblockcount() > old_height
                       for index in self.clock_indices)
        self.wait_until(advanced, timeout=90, check_interval=0.05)

    def submit_observed(self, market_id, index, side, *, price=None, quantity=1,
                        indices=range(4), label="four_live"):
        node = self.nodes[index]
        before = node.getflowmeshbalance(market_id)["account"]
        sequence = before["next_sequence"]
        b3_height = node.getblockcount()
        started = time.monotonic()
        admission_refusals = []
        response = None

        def admit():
            nonlocal response
            self.pump_b3()
            try:
                if price is None:
                    response = node.cancelflowmeshorder(market_id, side, sequence)
                else:
                    response = node.submitflowmeshorder(market_id, side, price, quantity, sequence)
                return True
            except JSONRPCException as error:
                # These exact errors are returned before runtime admission:
                # the service reconciliation gate or GetWalletActionContext's
                # paused status check, before SignAction/SubmitSignedAction.
                # The latter existing text also covers a transient B3 pause;
                # it is not proof that this four-seat fixture lost a member.
                # A genuinely persistent pause still fails this bounded wait.
                # Never retry an unknown result, transport error, sequence
                # error or arbitrary failed RPC as a new action.
                pre_admission_refusals = {
                    "FlowMesh service is reconciling the B3 tip",
                    "FlowMesh market is paused (at least four active seats are required)",
                }
                if error.error.get("code") != -1 or error.error.get("message") not in pre_admission_refusals:
                    raise
                admission_refusals.append({"after_ms": (time.monotonic() - started) * 1000,
                                           "reason": error.error["message"]})
                return False

        self.wait_until(admit, timeout=30, check_interval=0.025)
        admitted = time.monotonic()
        assert_equal(response["accepted"], True)
        assert_equal(response["sequence"], sequence)
        result = {}

        def certified():
            self.pump_b3()
            data = node.getflowmeshmarketdata(market_id, {"limit": 1, "curve_limit": 2})
            if data.get("account", {}).get("next_sequence", 0) <= sequence:
                return False
            assert data["snapshot"]["certified"]
            result.update(data)
            return True

        try:
            self.wait_until(certified, timeout=90, check_interval=0.025)
        except AssertionError:
            self.dump_diagnostics(indices, market_id, sequence + 1)
            raise
        client_observed = time.monotonic()
        target = self.target(result["snapshot"])
        applied = self.wait_for_target(market_id, target, indices, started=started)
        sample = {"scenario": label, "client_node": index, "action_id": response["action_id"],
                  "account_sequence": sequence, "target": target,
                  "rpc_admission_ms": (admitted - started) * 1000,
                  "explicit_admission_refusals": admission_refusals,
                  "client_certified_observed_ms": (client_observed - started) * 1000,
                  "replica_applied_observed_ms": applied,
                  "replication_tail_ms": max(applied.values()),
                  "B3_height_before_submission": b3_height,
                  "B3_height_after_replica_observation": node.getblockcount()}
        self.samples.append(sample)
        self.log.info("FMNET_ACTION_RESULT %s", json.dumps(sample, sort_keys=True))
        return result

    def pair_workload(self, market_id, indices, pairs, label):
        buyer = self.nodes[1]
        before = buyer.getflowmeshbalance(market_id)["account"]
        for _ in range(pairs):
            opened = self.submit_observed(market_id, 1, "bid", price=TRADE_PRICE // 2,
                                          indices=indices, label=label)
            assert_equal(len(opened["account"]["curves"]), 1)
            assert_equal(opened["account"]["b3_reserved_atoms"], TRADE_PRICE // 2)
            cancelled = self.submit_observed(market_id, 1, "bid", indices=indices, label=label)
            assert_equal(cancelled["account"]["curves"], [])
            assert_equal(cancelled["account"]["b3_reserved_atoms"], 0)
        after = buyer.getflowmeshbalance(market_id)["account"]
        for field in ("base_available", "base_reserved", "b3_available", "b3_reserved"):
            assert_equal(after[field], before[field])
        assert_equal(after["next_sequence"], before["next_sequence"] + pairs * 2)

    def matched_trade(self, market_id, indices=range(4), label="matched_trade"):
        seller, buyer = self.nodes[:2]
        seller_before = seller.getflowmeshbalance(market_id)["account"]
        buyer_before = buyer.getflowmeshbalance(market_id)["account"]
        height = seller.getblockcount()
        self.submit_observed(market_id, 1, "bid", price=TRADE_PRICE,
                             indices=indices, label=label + "_bid")
        # B3 advances between bid submission and matching. This does not assert
        # that the block connected after the bid's certification observation.
        self.wait_for_new_b3_block(height)
        filled = self.submit_observed(market_id, 0, "ask", price=TRADE_PRICE,
                                      indices=indices, label=label + "_ask")
        notional = Decimal(TRADE_PRICE) / Decimal(1_000_000_000)
        fee_atoms = TRADE_PRICE // 10_000
        fee = Decimal(fee_atoms) / Decimal(1_000_000_000)
        seller_after = seller.getflowmeshbalance(market_id)["account"]
        buyer_after = buyer.getflowmeshbalance(market_id)["account"]
        assert_equal(seller_after["base_available"], seller_before["base_available"] - 1)
        assert_equal(buyer_after["base_available"], buyer_before["base_available"] + 1)
        assert_equal(seller_after["b3_available"], seller_before["b3_available"] + notional - fee)
        assert_equal(buyer_after["b3_available"], buyer_before["b3_available"] - notional)
        assert_equal(seller_after["base_reserved"], 0)
        assert_equal(buyer_after["b3_reserved"], 0)
        fills = [entry for entry in filled["history"]["entries"] if entry["cleared"]]
        assert_equal(len(fills), 1)
        assert_equal(fills[0]["quantity"], 1)
        assert_equal(fills[0]["fee_atoms"], fee_atoms)
        assert_equal(fills[0]["account_ask_fill"], 1)
        assert_equal(fills[0]["account_fills_known"], True)

    def recover_slow_peer(self, market_id, cycle):
        all_indices, live_indices = list(range(4)), [0, 1, 2]
        self.end_b3_workload()
        before = self.target(self.nodes[3].getflowmeshmarketdata(market_id)["snapshot"])
        self.log.info("Gracefully disconnect FN3, retaining its original keys and durable store")
        self.stop_node(3)
        self.wait_for_independent_mesh(live_indices)
        self.begin_b3_workload(live_indices)
        height = self.nodes[0].getblockcount()
        self.pair_workload(market_id, live_indices, 4, f"offline_fn_{cycle}")
        self.matched_trade(market_id, live_indices, f"offline_trade_{cycle}")
        self.wait_for_new_b3_block(height)
        target = self.target(self.nodes[0].getflowmeshmarketdata(market_id)["snapshot"])
        assert target["sequence"] > before["sequence"]
        fault_before = self.fault.snapshot()["delayed_bulk_bytes"]
        self.fault.configure(self.operator_keys[3], hold=True)
        restarted = time.monotonic()
        self.start_node(3)
        self.nodes[3].setmocktime(self.mock_time)
        self.connect_nodes(3, 2)
        self.wait_for_independent_mesh()
        connected_ms = (time.monotonic() - restarted) * 1000
        assert_equal(self.nodes[3].getflowmeshvalidatorinfo()["armed"], False)
        assert not self.target_applied(3, market_id, target)
        # Bulk remains held after authenticating all three channels. Critical
        # traffic among the live quorum must still certify exact new actions.
        try:
            self.pair_workload(market_id, live_indices, 1, f"held_bulk_{cycle}")
            self.matched_trade(market_id, live_indices, f"held_bulk_trade_{cycle}")
        finally:
            self.fault.configure(self.operator_keys[3], rate=2048)
        self.pair_workload(market_id, live_indices, 2, f"slow_bulk_{cycle}")
        self.matched_trade(market_id, live_indices, f"slow_bulk_trade_{cycle}")
        self.clock_indices = all_indices
        target = self.target(self.nodes[0].getflowmeshmarketdata(market_id)["snapshot"])
        applied = self.wait_for_target(market_id, target, all_indices, started=restarted)
        catchup_ms = applied["3"]
        assert_equal(self.market_status(self.nodes[3], market_id)["observer_only"], True)
        assert self.fault.snapshot()["delayed_bulk_bytes"] > fault_before
        self.fault.configure()
        # Explicit operator arming follows observed verified catch-up. Armed
        # and eligible is readiness, not proof a subsequent share was emitted.
        arm_started = time.monotonic()
        armed = self.nodes[3].startflowmeshvalidator()
        assert_equal(armed["running"], True)
        assert_equal(armed["armed_keys"], 1)
        self.wait_for_target(market_id, target, [3], require_signing=True)
        ready_ms = (time.monotonic() - arm_started) * 1000
        assert_equal(self.nodes[3].startstaking()["running"], True)
        self.pair_workload(market_id, all_indices, 1, f"recovered_fn_{cycle}")
        self.recoveries.append({"cycle": cycle, "target": target,
                                "restart_to_authenticated_mesh_ms": connected_ms,
                                "restart_to_caught_up_observed_ms": catchup_ms,
                                "explicit_arm_to_eligible_observed_ms": ready_ms,
                                "critical_actions_certified_while_bulk_held": 4,
                                "recovery_target_distance": target["sequence"] - before["sequence"],
                                "signing_share_observed": False,
                                "delayed_bulk_bytes": self.fault.snapshot()["delayed_bulk_bytes"] - fault_before})
        self.assert_no_b3_flowmesh_traffic()

    def collect_delivery_trace(self, market_id):
        traces = {}
        for index, node in enumerate(self.nodes):
            try:
                traces[str(index)] = node.getflowmeshdeliveryinfo(market_id)
            except Exception as error:
                # Preserve failure evidence; success qualification below
                # requires these diagnostics instead of silently skipping them.
                traces[str(index)] = {"unavailable": type(error).__name__}
        return traces

    def qualification_workload(self, market_id):
        self.last_market = market_id
        self.wait_for_independent_mesh()
        first_height = self.nodes[0].getblockcount()
        started = time.monotonic()
        success = False
        try:
            self.begin_b3_workload(range(4))
            self.pair_workload(market_id, range(4), 4, "four_live_advancing_b3")
            self.wait_for_new_b3_block(first_height)
            self.matched_trade(market_id)
            for cycle in range(2):
                self.recover_slow_peer(market_id, cycle)
            traces = self.collect_delivery_trace(market_id)
            for index in range(self.num_nodes):
                trace = traces[str(index)]
                assert "unavailable" not in trace, trace
                assert_equal(trace["remote_receipt_proven"], False)
                assert_equal(len(trace["markets"]), 1)
                stats = trace["markets"][0]
                assert stats["durably_applied"] > 0, stats
                assert stats["verified"] > 0, stats
                network = self.nodes[index].getflowmeshnetworkinfo()
                assert network["critical"]["received"] > 0, network
            success = True
        finally:
            self.fault.configure()
            cleanup_errors = []
            try:
                self.end_b3_workload()
                self.assert_no_b3_flowmesh_traffic(
                    [index for index, node in enumerate(self.nodes) if node.running])
            except Exception as error:
                if success:
                    raise
                cleanup_errors.append(type(error).__name__)
            values = sorted(sample["client_certified_observed_ms"] for sample in self.samples)
            percentile = lambda p: values[max(0, math.ceil(len(values) * p) - 1)] if values else None
            report = {"network": "four headless local regtest operators; FMN2 through loopback byte proxies",
                      "success": success, "elapsed_ms": (time.monotonic() - started) * 1000,
                      "samples": self.samples, "recoveries": self.recoveries,
                      "client_certified_p50_ms": percentile(.50),
                      "client_certified_p95_ms": percentile(.95),
                      "client_certified_p99_ms": percentile(.99),
                      "new_B3_blocks_during_workload": self.nodes[0].getblockcount() - first_height,
                      "poll_interval_ms": 25, "B3_clock_pump_interval_ms": 500,
                      "fault_proxy": self.fault.snapshot(),
                      "cleanup_errors": cleanup_errors,
                      "delivery_trace": self.collect_delivery_trace(market_id),
                      "engine_off_remote_client_qualified": False, "WAN_qualified": False,
                      "crash_restart_qualified": False, "automatic_signing_gate_qualified": False,
                      "measurement": "local RPC observation upper bounds, not network ACKs or remote balance proofs"}
            Path(self.options.tmpdir, "fmnet-qualification.json").write_text(
                json.dumps(report, sort_keys=True, indent=2, default=str) + "\n", encoding="utf-8")
            self.log.info("FMNET_QUALIFICATION_RESULT %s", json.dumps(report, sort_keys=True, default=str))

    def run_test(self):
        # Inherited assertions cover the original matched trade, custody,
        # settlement and graceful reindex without changing their exact totals.
        super().run_test()
        # The inherited final clean-reindex restarts node0 without restoring
        # its one-try B3 connection. Restore ordinary block relay explicitly
        # before the advancing-chain qualification, not FlowMesh carriage.
        self.connect_nodes(0, 1)
        self.sync_all()
        self.wait_for_independent_mesh()
        assert self.last_market is not None
        self.qualification_workload(self.last_market)
        self.assert_no_b3_flowmesh_traffic()
        self.log.info("Independent FMN2 delivery qualification completed")


if __name__ == "__main__":
    FlowMeshIndependentTest(__file__).main()
