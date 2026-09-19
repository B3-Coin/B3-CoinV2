#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Runtime public HTTPS endpoint selection using isolated regtest fixtures.

Two fresh engine-off wallets use an empty-market, walletless loopback operator.
The generated CA is installed only in child-process default trust, never in the
host trust store. No economic action, live wallet, FN binding or mainnet key is
used. Empty discovery proves HTTPS availability, not certified market readiness.
"""

import hashlib
import ssl
from pathlib import Path

from feature_flowmesh_release import B3_ARGS
from test_framework.authproxy import JSONRPCException
from test_framework.flowmesh_client_tls import FlowMeshTLSFaultRelay, create_test_pki
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, p2p_port, rpc_port


class FlowMeshClientConnectTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        # -listen=0 must not be combined with the framework's default bind.
        self.bind_to_localhost_only = False
        self.wallet_names = [self.default_wallet_name, False, self.default_wallet_name]
        self.common_args = [*B3_ARGS, "-listen=0", "-connect=0", "-dnsseed=0"]
        # Deliberately omit -enableflowmeshvalidator on both ordinary clients.
        self.extra_args = [list(self.common_args),
                           [*self.common_args, "-disablewallet", "-enableflowmeshvalidator=1",
                            "-flowmeshtransport=legacy"],
                           list(self.common_args)]
        self.api_port = rpc_port(8)
        self.relay_port = rpc_port(9)
        self.url = f"https://127.0.0.1:{self.relay_port}"
        self.relays = []
        self.trust_env = {}

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.pki = create_test_pki(Path(self.options.tmpdir, "client-connect-tls"))
        self.other_pki = create_test_pki(Path(self.options.tmpdir, "client-connect-other-tls"))
        self.trust_env = {"SSL_CERT_FILE": str(self.pki["ca"])}
        self.extra_args[1] += ["-flowmeshapi=1", "-flowmeshapibind=127.0.0.1",
                               f"-flowmeshapiport={self.api_port}",
                               f"-flowmeshapicert={self.pki['certificate']}",
                               f"-flowmeshapikey={self.pki['key']}"]
        # No B3 peers or outside listeners are needed for empty market discovery.
        self.add_nodes(self.num_nodes, self.extra_args)
        for index in range(self.num_nodes):
            self.start_node(index)
        self.import_deterministic_coinbase_privkeys()

    def start_node(self, index, *args, **kwargs):
        super().start_node(index, *args, env=self.trust_env, **kwargs)

    @staticmethod
    def endpoint(status, url):
        return next(row for row in status["endpoints"] if row["url"] == url)

    def assert_read_only(self, node, wallet_before):
        status = node.getflowmeshclientinfo()
        assert_equal(status["backend"], "remote")
        assert_equal(status["engine_enabled"], False)
        assert_equal(status["pending_actions"], 0)
        assert_equal(node.listflowmeshactions(), {"source": "local-retained-outbox", "actions": []})
        assert_equal(node.getrawmempool(), [])
        wallet = node.getwalletinfo()
        for field in ("txcount", "keypoolsize", "keypoolsize_hd_internal", "unlocked_until"):
            if field in wallet_before:
                assert_equal(wallet[field], wallet_before[field])
        assert not (node.chain_path / "flowmesh").exists()
        assert "B3_FLOWMESH" not in node.getnetworkinfo()["localservicesnames"]

    def wait_for_read_recovery(self, node):
        def recovered():
            try:
                assert_equal(node.listflowmeshmarkets(), [])
            except JSONRPCException:
                return False
            return self.endpoint(node.getflowmeshclientinfo(), self.url)["available"]

        self.wait_until(recovered, timeout=20, check_interval=0.1)

    def configured_args(self, *, ca, pin=None):
        args = [*self.common_args, f"-flowmeshendpoint={self.url}/flowmesh/v1",
                f"-flowmeshendpointca={ca}"]
        if pin is not None:
            args.append(f"-flowmeshendpointpin={pin}")
        return args

    def run_test(self):
        client, _, configured = self.nodes
        client.encryptwallet("generated client-connect fixture only")
        wallet_before = client.getwalletinfo()
        assert_equal(wallet_before["unlocked_until"], 0)
        config_before = (client.datadir_path / "b3coin.conf").read_bytes()
        initial = client.getflowmeshclientinfo()
        assert_equal(initial["endpoints"], [])
        assert_equal(initial["selected_endpoint"], "")
        self.assert_read_only(client, wallet_before)

        self.log.info("Invalid endpoint configuration is refused without changing the client")
        invalid_urls = (
            "", "http://127.0.0.1", "ftp://127.0.0.1", "127.0.0.1",
            f"https://user:password@127.0.0.1:{self.relay_port}",
            self.url + "?key=fixture", self.url + "#fragment", self.url + "/wallet",
            self.url + "/flowmesh/v1/", "https://127.0.0.1:0", "https://127.0.0.1:65536",
            "https://127.0.0.1:invalid", "https://", self.url + " ", self.url + "\n",
            self.url + "\0", "https://127.0.0.1\\other", "https://127.0.0.1/" + "x" * 2048,
        )
        for invalid in invalid_urls:
            assert_raises_rpc_error(-8, "", client.flowmeshclientconnect, invalid)
            assert_equal(client.getflowmeshclientinfo(), initial)
        assert_raises_rpc_error(-3, "", client.flowmeshclientconnect, 7)
        assert_equal(client.getflowmeshclientinfo(), initial)

        self.log.info("An unavailable endpoint is remembered and recovers through ordinary reads")
        unavailable = client.flowmeshclientconnect(self.url + "/flowmesh/v1")
        assert_equal(unavailable, client.getflowmeshclientinfo())
        assert_equal(unavailable["selected_endpoint"], self.url)
        assert_equal(unavailable["active_endpoint"], "")
        assert_equal(len(unavailable["endpoints"]), 1)
        row = self.endpoint(unavailable, self.url)
        assert_equal(row["available"], False)
        assert_equal(row["transport_available"], False)
        assert row["last_error"]
        assert row["last_attempt_ms"] > 0
        assert row["retry_after_ms"] > row["last_attempt_ms"]
        assert_equal(row["consecutive_failures"], 1)
        # Status is a local observation: it never retries, signs or submits.
        assert_equal(client.getflowmeshclientinfo(), unavailable)
        self.assert_read_only(client, wallet_before)

        self.relays.append(FlowMeshTLSFaultRelay(self.relay_port, self.api_port, self.pki))
        relay = self.relays[0]
        self.log.info("Explicit reconnect probes a runtime endpoint during automatic-read cooldown")
        reconnect = client.reconnectflowmeshclient()
        assert_equal(reconnect["status"], "reachable")
        assert_equal(reconnect["attempted_endpoints"], 1)
        assert_equal(reconnect["endpoint"], self.url)
        self.assert_read_only(client, wallet_before)
        self.wait_for_read_recovery(client)
        recovered = client.getflowmeshclientinfo()
        row = self.endpoint(recovered, self.url)
        assert_equal(recovered["active_endpoint"], self.url)
        assert_equal(recovered["selected_endpoint"], self.url)
        assert_equal(row["available"], True)
        assert_equal(row["transport_available"], True)
        assert_equal(row["last_error"], "")
        assert_equal(row["retry_after_ms"], 0)
        assert_equal(row["consecutive_failures"], 0)
        assert row["last_attempt_ms"] >= unavailable["endpoints"][0]["last_attempt_ms"]
        for suffix in ("", "/", "/flowmesh/v1"):
            duplicate = client.flowmeshclientconnect(self.url + suffix)
            assert_equal(len(duplicate["endpoints"]), 1)
            assert_equal(duplicate["selected_endpoint"], self.url)
            assert_equal(self.endpoint(duplicate, self.url)["available"], True)

        self.log.info("Runtime URL and selected endpoint survive restart without editing configuration")
        self.restart_node(0)
        restored = client.getflowmeshclientinfo()
        assert_equal([row["url"] for row in restored["endpoints"]], [self.url])
        assert_equal(restored["selected_endpoint"], self.url)
        self.wait_for_read_recovery(client)
        assert_equal((client.datadir_path / "b3coin.conf").read_bytes(), config_before)
        self.assert_read_only(client, wallet_before)

        self.log.info("Reachable API errors remain distinct from transport failure and retry immediately")
        relay.configure(unavailable=True)
        api_unavailable = client.flowmeshclientconnect(self.url)
        row = self.endpoint(api_unavailable, self.url)
        assert_equal(row["available"], False)
        assert_equal(row["transport_available"], True)
        assert_equal(row["retry_after_ms"], 0)
        assert_equal(row["consecutive_failures"], 0)
        assert row["last_error"]
        relay.configure()
        assert_equal(client.listflowmeshmarkets(), [])
        assert_equal(self.endpoint(client.getflowmeshclientinfo(), self.url)["available"], True)

        self.log.info("Selecting a configured endpoint preserves explicit CA and certificate pin")
        configured_wallet_before = configured.getwalletinfo()
        self.restart_node(2, self.configured_args(ca=self.other_pki["ca"]))
        before_requests = len(relay.snapshot()["requests"])
        wrong_ca = configured.flowmeshclientconnect(self.url + "/")
        row = self.endpoint(wrong_ca, self.url)
        assert_equal(row["transport_available"], False)
        assert_equal(row["available"], False)
        assert row["last_error"]
        assert_equal(len(wrong_ca["endpoints"]), 1)
        assert_equal(len(relay.snapshot()["requests"]), before_requests)
        self.assert_read_only(configured, configured_wallet_before)

        self.restart_node(2, self.configured_args(ca=self.pki["ca"], pin="00" * 32))
        wrong_pin = configured.flowmeshclientconnect(self.url)
        row = self.endpoint(wrong_pin, self.url)
        assert_equal(row["transport_available"], False)
        assert_equal(row["available"], False)
        assert row["last_error"]
        assert_equal(wrong_pin["active_endpoint"], "")
        assert_equal(len(wrong_pin["endpoints"]), 1)
        assert_equal(len(relay.snapshot()["requests"]), before_requests)

        certificate = ssl.PEM_cert_to_DER_cert(self.pki["certificate"].read_text())
        pin = hashlib.sha256(certificate).hexdigest()
        self.restart_node(2, self.configured_args(ca=self.pki["ca"], pin=pin))
        pinned = configured.flowmeshclientconnect(self.url + "/flowmesh/v1")
        assert_equal(self.endpoint(pinned, self.url)["available"], True)
        assert_equal(pinned["selected_endpoint"], self.url)
        assert_equal(len(pinned["endpoints"]), 1)
        self.assert_read_only(configured, configured_wallet_before)

        self.log.info("Canonical duplicates do not consume the eight-endpoint limit")
        extra_urls = [f"https://127.0.0.1:{rpc_port(index)}" for index in range(4, 8)]
        extra_urls += [f"https://127.0.0.1:{p2p_port(index)}" for index in range(4, 6)]
        extra_urls += [f"https://localhost:{p2p_port(6)}"]
        for index, url in enumerate(extra_urls):
            supplied = url.replace("localhost", "LOCALHOST") + "/flowmesh/v1"
            added = client.flowmeshclientconnect(supplied)
            assert_equal(added["selected_endpoint"], url)
            assert_equal(len(added["endpoints"]), index + 2)
        full = client.getflowmeshclientinfo()
        assert_equal([row["url"] for row in full["endpoints"]], [self.url, *extra_urls])
        assert_raises_rpc_error(-8, "eight", client.flowmeshclientconnect,
                                f"https://127.0.0.1:{p2p_port(7)}")
        assert_equal(client.getflowmeshclientinfo(), full)
        duplicate = client.flowmeshclientconnect(self.url + "/")
        assert_equal(len(duplicate["endpoints"]), 8)
        assert_equal(duplicate["selected_endpoint"], self.url)
        self.restart_node(0)
        restored = client.getflowmeshclientinfo()
        assert_equal([row["url"] for row in restored["endpoints"]], [self.url, *extra_urls])
        assert_equal(restored["selected_endpoint"], self.url)
        self.wait_for_read_recovery(client)
        assert_equal((client.datadir_path / "b3coin.conf").read_bytes(), config_before)

        for node, before in ((client, wallet_before), (configured, configured_wallet_before)):
            self.assert_read_only(node, before)
            validator = node.getflowmeshvalidatorinfo()
            assert_equal(validator["service_available"], False)
            assert_equal(validator["armed"], False)
            assert_equal(validator["wallet_key_count"], 0)
        captured = relay.snapshot()
        assert captured["requests"]
        assert_equal(captured["records_dropped"], 0)
        assert all(request["method"] == "markets" for request in captured["requests"])
        assert all(request.get("action_hex") is None for request in captured["requests"])
        self.log.info("Runtime endpoint selection, restart, retry, trust and read-only checks passed")

    def shutdown(self):
        try:
            for relay in self.relays:
                relay.stop()
        finally:
            exit_code = super().shutdown()
        return exit_code


if __name__ == "__main__":
    FlowMeshClientConnectTest(__file__).main()
