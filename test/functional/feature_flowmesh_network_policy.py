#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Short walletless FMNET policy/isolation smoke test, not trading qualification.

Three private regtest processes: two independent transports and one default
legacy transport. This first integration stage intentionally retains the legacy
transport default; it does not qualify the future engine-off remote client.
No wallet, FN binding, staking, custody or live chain is used. Empty-block
convergence is a B3 availability check, not checkpoint/withdrawal compatibility.
"""

import socket

from feature_flowmesh_release import B3_ARGS
from test_framework.messages import msg_generic
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, p2p_port


class FlowMeshNetworkPolicyTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.common_args = [*B3_ARGS, "-disablewallet"]
        self.fm_ports = [p2p_port(4), p2p_port(5)]
        self.occupied_port = p2p_port(6)
        self.independent_args = [
            [*self.common_args, "-flowmeshtransport=independent",
             "-flowmeshlisten=1", "-flowmeshbind=127.0.0.1",
             f"-flowmeshport={self.fm_ports[index]}",
             f"-flowmeshconnect=127.0.0.1:{self.fm_ports[1 - index]}"]
            for index in range(2)
        ]
        self.extra_args = [*self.independent_args, self.common_args]

    @staticmethod
    def check_legacy_default(node):
        status = node.getflowmeshnetworkinfo()
        assert_equal(status["mode"], "legacy")
        assert_equal(status["legacy_enabled"], True)
        assert_equal(status["running"], False)
        assert_equal(status["listening"], False)
        assert_equal(status["operator_pubkey"], "")
        assert_equal(status["peers"], [])
        assert_equal(status["error"], "")
        assert_equal(status["connection_is_quorum_proof"], False)
        assert "B3_FLOWMESH" in node.getnetworkinfo()["localservicesnames"]

    def wait_for_independent_pair(self):
        def ready():
            statuses = [self.nodes[index].getflowmeshnetworkinfo() for index in range(2)]
            public_keys = [status["operator_pubkey"] for status in statuses]
            if any(not key for key in public_keys) or public_keys[0] == public_keys[1]:
                return False
            for index, status in enumerate(statuses):
                assert_equal(status["mode"], "independent")
                assert_equal(status["legacy_enabled"], False)
                assert_equal(status["connection_is_quorum_proof"], False)
                if not status["running"] or not status["listening"] or status["error"]:
                    return False
                ready_keys = {peer["operator_pubkey"] for peer in status["peers"]
                              if peer["authenticated"] and peer["live"] and peer["actions"] and peer["bulk"]}
                if public_keys[1 - index] not in ready_keys:
                    return False
            return True
        self.wait_until(ready, timeout=45, check_interval=0.05)
        for node in self.nodes[:2]:
            assert "B3_FLOWMESH" not in node.getnetworkinfo()["localservicesnames"]

    def run_test(self):
        independent0, independent1, legacy = self.nodes
        self.log.info("Independent paired channels are observable without a wallet or FN keys")
        self.wait_for_independent_pair()
        self.check_legacy_default(legacy)
        for node in self.nodes:
            delivery = node.getflowmeshdeliveryinfo()
            assert_equal(delivery["remote_receipt_proven"], False)
            assert_equal(delivery["markets"], [])
        transport_keys = [node.getflowmeshnetworkinfo()["operator_pubkey"]
                          for node in self.nodes[:2]]

        self.log.info("Disabled legacy FlowMesh messages cannot disconnect an independent node's B3 peer")
        peer = independent0.add_p2p_connection(P2PInterface())
        for command in (b"fmhello", b"fmaction", b"fmprop", b"fmattest", b"fmcert", b"fmget", b"fmentries"):
            peer.send_without_ping(msg_generic(command, b""))
            peer.send_without_ping(msg_generic(command, b"\x00"))
        peer.sync_with_ping(timeout=10)
        assert_equal(peer.is_connected, True)
        assert_equal(independent0.getnetworkinfo()["networkactive"], True)
        peer.peer_disconnect()
        peer.wait_for_disconnect()

        self.log.info("Default legacy mode retains its negotiated-service framing policy")
        legacy_peer = legacy.add_p2p_connection(P2PInterface())
        legacy_peer.send_without_ping(msg_generic(b"fmprop", b""))
        legacy_peer.send_without_ping(msg_generic(b"fmprop", b""))
        legacy_peer.wait_for_disconnect(timeout=10)

        self.log.info("An occupied independent port reports failure but B3 starts and connects blocks")
        self.stop_node(1)
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as occupied:
            occupied.bind(("127.0.0.1", self.occupied_port))
            occupied.listen(1)
            failing_args = [arg for arg in self.independent_args[1]
                            if not arg.startswith("-flowmeshport=")]
            failing_args.append(f"-flowmeshport={self.occupied_port}")
            self.start_node(1, extra_args=failing_args)
            status = independent1.getflowmeshnetworkinfo()
            assert_equal(status["mode"], "independent")
            assert_equal(status["legacy_enabled"], False)
            assert_equal(status["running"], False)
            assert_equal(status["listening"], False)
            assert "Cannot bind FlowMesh listener" in status["error"]
            assert "B3_FLOWMESH" not in independent1.getnetworkinfo()["localservicesnames"]
            self.connect_nodes(1, 0)
            self.connect_nodes(2, 1)
            self.sync_all()
            height = independent0.getblockcount()
            self.generatetoaddress(independent0, 1, independent0.get_deterministic_priv_key().address)
            for node in self.nodes:
                assert_equal(node.getblockcount(), height + 1)
                assert_equal(node.getbestblockhash(), independent0.getbestblockhash())
            assert_equal(independent1.getnetworkinfo()["networkactive"], True)
            assert_equal(independent1.getflowmeshnetworkinfo()["legacy_enabled"], False)

        self.restart_node(1)
        self.connect_nodes(1, 0)
        self.connect_nodes(2, 1)
        self.sync_all()
        self.wait_for_independent_pair()
        assert_equal([node.getflowmeshnetworkinfo()["operator_pubkey"]
                      for node in self.nodes[:2]], transport_keys)

        self.log.info("Invalid transport mode, ports and role fail explicitly rather than silently falling back")
        self.stop_node(2)
        cases = [
            (["-flowmeshtransport=typo"],
             "Error: Invalid -flowmeshtransport; use legacy, dual or independent"),
            (["-flowmeshtransport=independent", "-flowmeshport=0"],
             "Error: -flowmeshport must be between 1 and 65535"),
            (["-flowmeshtransport=independent", "-flowmeshport=65536"],
             "Error: -flowmeshport must be between 1 and 65535"),
            (["-flowmeshtransport=independent", "-flowmeshrole=blockmaker"],
             "Error: -flowmeshrole must be observer, sentry or validator"),
        ]
        for args, expected in cases:
            legacy.assert_start_raises_init_error(extra_args=[*self.common_args, *args],
                                                  expected_msg=expected)
        self.start_node(2)
        self.connect_nodes(2, 1)
        self.sync_all()
        self.check_legacy_default(legacy)
        self.log.info("Walletless independent-network policy and B3 failure-isolation smoke test passed")


if __name__ == "__main__":
    FlowMeshNetworkPolicyTest(__file__).main()
