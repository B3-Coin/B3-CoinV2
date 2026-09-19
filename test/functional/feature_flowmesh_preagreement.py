#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Fresh-market preliminary agreement through actual isolated B3 nodes.

Reuse the existing four-process FMN2/HTTPS/unchanged-settlement fixtures.
Every key and coin is generated regtest data. Only the new market's operator
mode is selected before its bootstrap is mined; no store is reset or migrated.
This does not qualify deployed split-lock recovery, WAN or a power-loss crash.
"""

import json
from pathlib import Path

from feature_flowmesh_remote_client import FlowMeshRemoteClientTest
from feature_flowmesh_delivery_review import public_bytes_hash
from test_framework.util import assert_equal


class FlowMeshPreagreementTest(FlowMeshRemoteClientTest):
    def set_test_params(self):
        super().set_test_params()
        self.preagreement_market = None

    def configure_fresh_market(self, market_id):
        assert self.preagreement_market is None
        assert not self.pos_running
        self.preagreement_market = market_id
        flag = f"-flowmeshpreagreementmarket={market_id}"
        # The one exact bootstrap transaction is already in the test mempool,
        # but no bootstrap block, runtime, signing journal or checkpoint exists.
        mempool_before = set(self.nodes[0].getrawmempool())
        self.stop_nodes()
        for index, args in enumerate(self.extra_args):
            if flag not in args:
                args.append(flag)
            if flag not in self.nodes[index].extra_args:
                self.nodes[index].extra_args.append(flag)
        self.start_nodes()
        self.set_chain_time(self.mock_time)
        for index in range(1, self.num_nodes):
            self.connect_nodes(index, index - 1)
        self.sync_blocks()
        assert mempool_before.issubset(set(self.nodes[0].getrawmempool()))
        self.wait_for_independent_mesh()

    def restart_node(self, i, extra_args=None, clear_addrman=False, *, expected_stderr=""):
        if extra_args is not None and self.preagreement_market:
            extra_args = [arg for arg in extra_args if not arg.startswith("-flowmeshpreagreementmarket=")]
            extra_args.append(f"-flowmeshpreagreementmarket={self.preagreement_market}")
        super().restart_node(i, extra_args, clear_addrman, expected_stderr=expected_stderr)

    def assert_no_b3_flowmesh_traffic(self, indices=None):
        super().assert_no_b3_flowmesh_traffic(indices)
        for index in range(self.num_nodes) if indices is None else indices:
            for peer in self.nodes[index].getpeerinfo():
                for field in ("bytesrecv_per_msg", "bytessent_per_msg"):
                    assert_equal(peer.get(field, {}).get("fmagree", 0), 0)

    def returning_share(self, market_id, minimum_sequence):
        info = self.nodes[3].getflowmeshvalidatorinfo()
        assert_equal(info["wallet_armed_key_count"], 1)
        assert_equal(len(info["wallet_bls_pubkeys"]), 1)
        key_hash = public_bytes_hash(info["wallet_bls_pubkeys"][0])
        fields = ("sequence", "object_id", "epoch", "seat_set_hash", "seat_index", "bls_key_hash", "signature_hash")
        signed, accepted = {}, []
        for index, node in enumerate(self.nodes):
            assert node.debug_log_path.stat().st_size < 192 * 1024 * 1024
            with node.debug_log_path.open(encoding="utf-8") as stream:
                for line in stream:
                    if "FlowMeshTrace " not in line:
                        continue
                    event = json.loads(line.split("FlowMeshTrace ", 1)[1])
                    if (event.get("market_id") != market_id or event.get("sequence", -1) < minimum_sequence or
                            event.get("bls_key_hash") != key_hash or not all(field in event for field in fields)):
                        continue
                    identity = tuple(event[field] for field in fields)
                    if index == 3 and event["stage"] == "attestation_signed":
                        signed[identity] = event
                    if index != 3 and event["stage"] == "attestation_verified":
                        accepted.append((index, identity, event))
        for index, identity, event in accepted:
            if identity not in signed:
                continue
            target = {"sequence": event["sequence"], "hash": event["object_id"]}
            data = self.nodes[index].getflowmeshmarketdata(market_id, {"limit": 100, "curve_limit": 1})
            if not any(row["sequence"] == target["sequence"] and row["microblock_hash"] == target["hash"]
                       for row in data["history"]["entries"]):
                continue
            return {"returning_node": 3, "receiving_node": index, "target": target,
                    "signed": signed[identity], "verified": event,
                    "claim": "same returned-seat share verified remotely for a durably applied entry; aggregate participation not inferred"}
        return None

    def recover_slow_peer(self, market_id, cycle):
        super().recover_slow_peer(market_id, cycle)
        proof = self.returning_share(market_id, self.recoveries[-1]["target"]["sequence"] + 1)
        assert proof is not None, "rearmed seat has no observed exact share accepted by another replica"
        self.recoveries[-1]["signing_share_observed"] = True
        self.recoveries[-1]["accepted_share_proof"] = proof

    def qualification_workload(self, market_id):
        # Includes a validator-disabled ordinary client and actual valid and
        # invalid type8/type9 block submissions, not only mempool acceptance.
        super().qualification_workload(market_id)
        assert_equal(market_id, self.preagreement_market)
        # The parent proves the type-9 payout, but ends before its canonical
        # 30-deep fact is retired by production. Complete that EXISTING normal
        # settlement path before the unrelated disconnect/bulk workload.
        # Otherwise it hits the required type-8 barrier thirty blocks later.
        payout = next(row for row in self.compatibility if "valid_withdrawal_txid" in row)
        payout_height = payout["block_validation"]["height"]
        pause_log = self.nodes[0].debug_log_path.open(encoding="utf-8")
        pause_log.seek(0, 2)
        pause_evidence = {}

        def settlement_barrier_observed():
            for line in pause_log:
                if "FlowMeshTrace " not in line:
                    continue
                event = json.loads(line.split("FlowMeshTrace ", 1)[1])
                if (event.get("market_id") == market_id and event["stage"] == "proposer_wait" and
                        event["reason"].startswith("settlement_checkpoint_pending")):
                    pause_evidence.update(event)
            return bool(pause_evidence)

        self.mine_pos_blocks(max(0, payout_height + 30 - self.nodes[0].getblockcount()),
                             allow_overshoot=True)
        try:
            self.wait_until(settlement_barrier_observed, timeout=90)
        finally:
            pause_log.close()
        settlement_checkpoints = []
        for _ in range(64):
            status = self.market_status(self.nodes[0], market_id)
            if not status["checkpoint_pending"]:
                break
            pending_sequence = status["pending_checkpoint_sequence"]
            checkpoint = self.nodes[0].createflowmeshcheckpoint(market_id, {"broadcast": False})
            self.publish_parity_transaction(checkpoint, 8)
            settlement_checkpoints.append(checkpoint)
            # Earlier effect-bearing entries may precede settlement. Wait
            # for their B3 recognition without requiring that settlement's
            # safety pause has already lifted. Final convergence stays strict.
            self.wait_until(lambda: all(
                not (row := self.market_status(node, market_id))["checkpoint_pending"] or
                row["pending_checkpoint_sequence"] > pending_sequence
                for node in self.nodes), timeout=60)
        else:
            raise AssertionError("Existing settlement checkpoint backlog did not drain")
        assert settlement_checkpoints, "Required settlement checkpoint was not published"
        assert_equal(settlement_checkpoints[-1]["effect_count"], 0)
        self.wait_for_market_convergence(market_id)
        self.settlement_completion = {
            "payout_height": payout_height,
            "pause_evidence": pause_evidence,
            "connected_checkpoints": [{k: row[k] for k in ("txid", "effect_count")}
                                      for row in settlement_checkpoints],
            "scope": "existing checkpoint publication rules; no bypass or replacement withdrawal",
        }
        first_height = self.nodes[0].getblockcount()
        success = False
        try:
            self.begin_b3_workload(range(4))
            self.pair_workload(market_id, range(4), 1, "preagreement_four_online")
            self.matched_trade(market_id, label="preagreement_advancing_b3_trade")
            # One bounded disconnect/rejoin, held bulk, throttled catch-up and
            # explicit rearm on the SAME generated node/store. No duplicate key.
            self.recover_slow_peer(market_id, 0)
            self.assert_no_b3_flowmesh_traffic()
            assert self.nodes[0].getblockcount() > first_height
            success = True
        finally:
            self.fault.configure()
            self.end_b3_workload()
            report = {
                "success": success,
                "market_id": market_id,
                "scope": "four separate local regtest daemons; authenticated plaintext FMN2 TCP",
                "B3_blocks_during_fault_workload": self.nodes[0].getblockcount() - first_height,
                "samples": self.samples,
                "recoveries": self.recoveries,
                "compatibility": self.compatibility,
                "settlement_completion": self.settlement_completion,
                "client_results": self.client_results,
                "delivery": self.collect_delivery_trace(market_id),
                "fault_proxy": self.fault.snapshot(),
                "old_market_recovery_qualified": False,
                "WAN_qualified": False,
                "power_loss_qualified": False,
                "Qt_exercised": False,
            }
            Path(self.options.tmpdir, "preagreement-qualification.json").write_text(
                json.dumps(report, indent=2, sort_keys=True, default=str) + "\n", encoding="utf-8")

    def run_test(self):
        super().run_test()

    def shutdown(self):
        # The generic harness defers cleanup after failures. This bounded
        # qualification instead captures clean exits for every started test
        # daemon, while retaining all generated files for diagnosis.
        for node in [*self.nodes, *([self.client] if self.client else [])]:
            if node.running and node.rpc_connected:
                process = node.process
                node.stop_node()
                assert_equal(process.returncode, 0)
                self.log.info("PREAGREEMENT_TEST_CHILD_EXIT index=%s pid=%s status=%s",
                              node.index, process.pid, process.returncode)
        return super().shutdown()


if __name__ == "__main__":
    FlowMeshPreagreementTest(__file__).main()
