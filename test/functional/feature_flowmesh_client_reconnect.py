#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Read-only trader HTTPS reconnect: generated wallets, loopback TLS, no FN keys."""

from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import time

from feature_flowmesh_release import B3_ARGS
from test_framework.flowmesh_client_tls import FlowMeshTLSFaultRelay, create_test_pki
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, get_rpc_proxy, rpc_port


class FlowMeshClientReconnectTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [[*B3_ARGS, "-enableflowmeshvalidator=1", "-flowmeshlisten=0"],
                           [*B3_ARGS, "-enableflowmeshvalidator=0"]]
        self.relays = []

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.pki = create_test_pki(Path(self.options.tmpdir, "reconnect-tls"))
        self.other_pki = create_test_pki(Path(self.options.tmpdir, "untrusted-tls"))
        self.extra_args[0] += ["-flowmeshapi=1", "-flowmeshapibind=127.0.0.1",
                               f"-flowmeshapiport={rpc_port(7)}",
                               f"-flowmeshapicert={self.pki['certificate']}",
                               f"-flowmeshapikey={self.pki['key']}"]
        for index in (8, 9):
            self.relays.append(FlowMeshTLSFaultRelay(rpc_port(index), rpc_port(7), self.pki))
        self.client_args = [*B3_ARGS, "-enableflowmeshvalidator=0",
                            f"-flowmeshendpointca={self.pki['ca']}",
                            *[f"-flowmeshendpoint={relay.url}" for relay in self.relays]]
        self.extra_args[1] = list(self.client_args)
        super().setup_network()

    def check_result(self, value, status, attempts):
        assert_equal(value["status"], status)
        assert_equal(value["attempted_endpoints"], attempts)
        assert_equal(value["endpoint_available"], status == "reachable")
        assert_equal(value["actions_submitted"], 0)
        assert_equal(value["availability_is_certification"], False)
        if status != "reachable":
            assert_equal(value["endpoint"], "")
            assert value["error"]
        return value

    def run_test(self):
        client = self.nodes[1]
        self.check_result(self.nodes[0].reconnectflowmeshclient(), "not_remote", 0)
        client.createwallet("reconnect-encrypted", passphrase="generated-reconnect-test-only", load_on_startup=True)
        wallet = client.get_wallet_rpc("reconnect-encrypted")
        before = wallet.getwalletinfo()
        assert_equal(before["unlocked_until"], 0)
        saved = wallet.listflowmeshactions()
        assert_equal(saved["actions"], [])
        first, second = self.relays
        result = self.check_result(client.reconnectflowmeshclient(), "reachable", 1)
        assert_equal(result["endpoint"], first.url)
        assert_equal(wallet.getflowmeshclientinfo()["engine_enabled"], False)

        first.configure(unavailable=True)
        result = self.check_result(client.reconnectflowmeshclient(), "reachable", 2)
        assert_equal(result["endpoint"], second.url)
        second.configure(unavailable=True)
        self.check_result(client.reconnectflowmeshclient(), "unavailable", 2)
        info = wallet.getflowmeshclientinfo()
        assert_equal(info["active_endpoint"], "")
        assert all(not row["available"] and row["last_error"] for row in info["endpoints"])
        first.configure()
        self.check_result(client.reconnectflowmeshclient(), "reachable", 2)

        # A held passive market read owns the single backend worker. Repeated
        # reconnect RPCs must return explicit busy without another HTTP request.
        count = len(first.snapshot()["requests"])
        first.configure(response_hold_ms={"markets": 4000})
        with ThreadPoolExecutor(max_workers=1) as executor:
            # get_wallet_rpc shares its parent's HTTP connection; concurrent
            # RPC callers need separate test-side connections.
            worker = get_rpc_proxy(client.url, 1, timeout=15) / "wallet/reconnect-encrypted"
            pending = executor.submit(worker.listflowmeshmarkets)
            self.wait_until(lambda: any(row.get("response_hold_started_us")
                                       for row in first.snapshot()["requests"][count:]), timeout=10)
            counts = [len(relay.snapshot()["requests"]) for relay in self.relays]
            started = time.monotonic()
            for _ in range(3):
                self.check_result(client.reconnectflowmeshclient(), "busy", 0)
            assert time.monotonic() - started < 2
            assert_equal([len(relay.snapshot()["requests"]) for relay in self.relays], counts)
            first.configure()
            assert_equal(pending.result(timeout=10), [])

        # Invalid application response is not connectivity success. A fresh
        # valid probe restores availability without restarting the client.
        first.configure(reply_mutation=lambda method, body: json.dumps({"ok": True, "result": "invalid"}).encode())
        self.check_result(client.reconnectflowmeshclient(), "unavailable", 2)
        first.configure()
        self.check_result(client.reconnectflowmeshclient(), "reachable", 1)
        assert_equal(wallet.listflowmeshactions(), saved)
        after = wallet.getwalletinfo()
        for field in ("unlocked_until", "txcount", "private_keys_enabled"):
            assert_equal(after[field], before[field])
        assert_equal(client.getrawmempool(), [])
        assert not (client.chain_path / "flowmesh" / "network").exists()

        # Reconnect must not weaken pin or CA trust even to obtain a response.
        for changed in (
            [*self.client_args, "-flowmeshendpointpin=" + "00" * 32,
             "-flowmeshendpointpin=" + "00" * 32],
            [arg for arg in self.client_args if not arg.startswith("-flowmeshendpointca=")]
            + [f"-flowmeshendpointca={self.other_pki['ca']}"],
        ):
            self.restart_node(1, changed)
            client = self.nodes[1]
            counts = [len(relay.snapshot()["requests"]) for relay in self.relays]
            self.check_result(client.reconnectflowmeshclient(), "unavailable", 2)
            assert_equal([len(relay.snapshot()["requests"]) for relay in self.relays], counts)
        self.restart_node(1, [*B3_ARGS, "-enableflowmeshvalidator=0"])
        self.check_result(self.nodes[1].reconnectflowmeshclient(), "not_configured", 0)
        self.restart_node(1, self.client_args)
        self.check_result(self.nodes[1].reconnectflowmeshclient(), "reachable", 1)
        reopened = self.nodes[1].get_wallet_rpc("reconnect-encrypted")
        assert_equal(reopened.listflowmeshactions(), saved)
        assert_equal(reopened.getwalletinfo()["unlocked_until"], 0)
        for relay in self.relays:
            snapshot = relay.snapshot()
            assert_equal(snapshot["records_dropped"], 0)
            assert all(row["method"] == "markets" for row in snapshot["requests"])
        self.log.info("READ_ONLY_RECONNECT_PASS: failover, unavailable, busy, malformed response, pin/CA rejection, locked wallet and same-wallet reopen; only markets requests")

    def shutdown(self):
        try:
            for relay in self.relays:
                relay.configure()  # Release test-owned holds before joining.
            for node in self.nodes:
                if node.running and node.process is not None and node.process.poll() is None:
                    process = node.process
                    node.stop_node()
                    self.log.info("RECONNECT_TEST_CHILD_EXIT node=%d pid=%d exit=%s", node.index, process.pid, process.returncode)
        finally:
            try:
                for relay in self.relays:
                    relay.stop()
            finally:
                result = super().shutdown()
        return result


if __name__ == "__main__":
    FlowMeshClientReconnectTest(__file__).main()
