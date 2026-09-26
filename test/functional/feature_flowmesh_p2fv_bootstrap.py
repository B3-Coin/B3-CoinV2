#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Real-chain bootstrap prerequisite, NOT P2FV trading qualification.

Reuse the existing four-operator regtest bootstrap and fifth engine-off node.
The new read-only TEST RPC verifies an existing V1 certificate/state using
locally resolved B3 authority. No P2FV signer, vote, worker, network protocol,
cutover, new order, or alternate settlement certificate is activated here.

Build with BUILD_TESTS=ON and BUILD_FLOWMESH_P2FV_REGTEST_GATE=ON. Invoke directly
with --configfile=<build>/test/config.ini --nocleanup. Production/default builds
do not register these RPCs. This is deliberately outside the broad test runner.
"""

import hashlib
import json
import struct
from pathlib import Path

from feature_flowmesh_latency import FlowMeshLatencyTest
from test_framework.authproxy import JSONRPCException
from test_framework.util import assert_equal


def flip(raw, offset=0):
    data = bytearray.fromhex(raw)
    assert data
    data[offset] ^= 1
    return data.hex()


def expected_instance(row):
    """Independent fixed-width TEST/v1 commitment oracle; no RPC self-comparison."""
    h = lambda value: bytes.fromhex(value)[::-1]
    data = struct.pack("<H", row["version"])
    for name in ("genesis", "domain", "market_id", "base_asset_id", "vault_id", "execution_config_id"):
        data += h(row[name])
    data += struct.pack("<Q", row["epoch"]) + h(row["seat_set_hash"])
    data += struct.pack("<Q", row["seat_anchor"]["height"]) + h(row["seat_anchor"]["hash"])
    data += struct.pack("<i", row["entry_anchor"]["height"]) + h(row["entry_anchor"]["hash"])
    data += h(row["head"]) + h(row["state_root"])
    data += struct.pack("<QQI", row["next_sequence"], row["next_effect_index"], 4)
    assert len(row["bindings"]) == 4
    for member in row["bindings"]:
        data += h(member["seat_id"]) + h(member["txid"]) + struct.pack("<I", member["vout"])
        data += bytes.fromhex(member["public_key"]) + bytes.fromhex(member["proof_of_possession"])
    tag = hashlib.sha256(b"B3/TEST-ONLY/P2FV/REGTEST-BOOTSTRAP/INSTANCE/1").digest()
    return hashlib.sha256(tag + tag + data).digest()[::-1].hex()


class FlowMeshP2fvBootstrapTest(FlowMeshLatencyTest):
    def set_test_params(self):
        super().set_test_params()
        self.child_records = []
        self.bootstrap_report = {
            "profile": "P2FV_REGTEST_BOOTSTRAP_GATE_V1",
            "qualification": "existing V1-certified base, local B3 authority, engine-off verifier only",
            "p2fv_trading_qualified": False,
            "p2fv_recovery_in_node_qualified": False,
            "latency_qualified": False,
            "new_client_signed_actions": 0,
            "checks": [], "complete": False,
        }

    def remember_children(self):
        nodes = list(self.nodes)
        if self.client is not None:
            nodes.append(self.client)
        known = {id(row["process"]) for row in self.child_records}
        for node in nodes:
            if node.process is not None and id(node.process) not in known:
                self.child_records.append({"node": node.index, "process": node.process,
                                           "log": node.debug_log_path})

    def start_node(self, *args, **kwargs):
        result = super().start_node(*args, **kwargs)
        self.remember_children()
        return result

    def start_nodes(self, *args, **kwargs):
        result = super().start_nodes(*args, **kwargs)
        self.remember_children()
        return result

    def start_ordinary_client(self):
        super().start_ordinary_client()
        self.remember_children()

    def reject(self, label, call):
        try:
            call()
        except JSONRPCException as error:
            # Record only the explicit application rejection, not transport
            # authentication, private endpoints or generated wallet contents.
            assert error.error["code"] in {-8, -1}, error.error
            self.bootstrap_report["checks"].append({"name": label, "rejected": True,
                "code": error.error["code"], "reason": error.error["message"]})
        else:
            raise AssertionError(f"{label}: invalid bootstrap accepted")

    def run_test(self):
        # Fail before any market/funds setup when the explicit test build is
        # absent. A missing RPC is not a skipped passing bootstrap assertion.
        self.nodes[0].help("getflowmeshregtestbootstrap")
        self.nodes[0].help("verifyflowmeshregtestbootstrap")
        market, asset = self.bootstrap_latency_market()
        self.start_ordinary_client()
        self.wait_client_b3_sync()
        self.assert_engine_off()
        head = self.nodes[0].getflowmeshmarketdata(market, {"limit": 1})["snapshot"]["last_microblock_hash"]
        bundle = self.nodes[0].getflowmeshregtestbootstrap(market, head)
        raw, state = bundle["certified_payload"], bundle["state_bytes"]
        verify = lambda node, m=market, h=head, p=raw, s=state: node.verifyflowmeshregtestbootstrap(m, h, p, s)
        results = [verify(node) for node in [*self.nodes, self.client]]
        assert all(row == results[0] for row in results), "nodes disagree on independently verified bootstrap"
        assert_equal(results[0]["version"], 1)
        assert_equal(results[0]["cutover_authorized"], False)
        assert_equal(results[0]["execution_authorized"], False)
        assert_equal(results[0]["next_sequence"], results[0]["sequence"] + 1)
        assert_equal(results[0]["next_effect_index"], results[0]["effect_start"] + results[0]["effect_count"])
        assert_equal(results[0]["instance"], expected_instance(results[0]))
        local_keys = {key for node in self.nodes for key in node.getflowmeshvalidatorinfo()["wallet_bls_pubkeys"]}
        assert_equal({member["public_key"] for member in results[0]["bindings"]}, local_keys)
        self.bootstrap_report.update(market_id=market, base_asset_id=asset,
            operator_subversions=[node.getnetworkinfo()["subversion"] for node in self.nodes],
            verified=results[0], certificate_sha256=hashlib.sha256(bytes.fromhex(raw)).hexdigest(),
            state_sha256=hashlib.sha256(bytes.fromhex(state)).hexdigest())
        self.bootstrap_report["checks"].append({"name": "four operators plus engine-off client agree", "passed": True})

        self.reject("wrong exact base head", lambda: verify(self.client, h="01" * 32))
        self.reject("wrong market", lambda: verify(self.client, m="02" * 32))
        self.reject("invalid market encoding", lambda: verify(self.client, m="1234"))
        self.reject("empty state", lambda: verify(self.client, s=""))
        self.reject("altered state", lambda: verify(self.client, s=flip(state)))
        self.reject("state trailing bytes", lambda: verify(self.client, s=state + "00"))
        self.reject("malformed state hex", lambda: verify(self.client, s="gg"))
        self.reject("oversized state before hex decoding", lambda: verify(self.client, s="00" * (8 * 1024 * 1024 + 1)))
        self.reject("empty certificate", lambda: verify(self.client, p=""))
        self.reject("altered certificate", lambda: verify(self.client, p=flip(raw, -1)))
        self.reject("certificate trailing bytes", lambda: verify(self.client, p=raw + "00"))
        self.reject("wrong export head", lambda: self.nodes[0].getflowmeshregtestbootstrap(market, "03" * 32))
        self.reject("engine-off node cannot export engine state", lambda: self.client.getflowmeshregtestbootstrap(market, head))

        before = [node.getblockcount() for node in [*self.nodes, self.client]]
        self.begin_b3_workload(range(4))
        try:
            self.wait_for_new_b3_block(max(before) + 2)
        finally:
            self.end_b3_workload()
        self.sync_blocks(timeout=120)
        self.wait_client_b3_sync()
        after = [node.getblockcount() for node in [*self.nodes, self.client]]
        assert all(b > a for a, b in zip(before, after))
        assert_equal(verify(self.client), results[0])
        self.bootstrap_report["checks"].append({"name": "anchored instance unchanged as real B3 advances", "passed": True,
                                                "heights_before": before, "heights_after": after})
        wallet_before = self.client.getwalletinfo()
        balances_before = self.client.getbalances()
        original_process = self.client.process
        self.client.stop_node()
        assert_equal(original_process.returncode, 0)
        self.client.start()
        self.remember_children()
        self.client.wait_for_rpc_connection()
        self.client_clock = None
        self.sync_client_time()
        wallet_after = self.client.getwalletinfo()
        for field in ("walletname", "txcount"):
            assert_equal(wallet_before[field], wallet_after[field])
        assert_equal(self.client.getbalances(), balances_before)
        assert_equal(verify(self.client), results[0])
        self.assert_engine_off()
        self.assert_no_b3_flowmesh_traffic()
        self.bootstrap_report["checks"].append({"name": "same engine-off wallet reopens with identical base", "passed": True})
        self.bootstrap_report["complete"] = True

    def shutdown(self):
        self.remember_children()
        code = super().shutdown()
        rows = []
        for record in self.child_records:
            process = record["process"]
            rows.append({"node": record["node"], "pid": process.pid,
                         "exit_code": process.poll()})
        self.bootstrap_report["children"] = rows
        self.bootstrap_report["shutdown_logs"] = [
            {"node": index, "shutdown_done_count": path.read_text(errors="replace").count("Shutdown done")}
            for index, path in sorted({row["node"]: row["log"] for row in self.child_records}.items())]
        self.bootstrap_report["harness_exit_code"] = code
        clean = bool(rows) and all(row["exit_code"] == 0 for row in rows)
        self.bootstrap_report["all_children_clean"] = clean
        if not clean or not all(row["shutdown_done_count"] for row in self.bootstrap_report["shutdown_logs"]):
            self.bootstrap_report["complete"] = False
            code = 1
        Path(self.options.tmpdir, "p2fv-bootstrap-gate.json").write_text(
            json.dumps(self.bootstrap_report, indent=2, sort_keys=True) + "\n")
        return code


if __name__ == "__main__":
    FlowMeshP2fvBootstrapTest(__file__).main()
