#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license.
"""Live pinned target admission/retry; isolated walletless regtest nodes only.

Transport authentication is not FN eligibility, certification, or WAN evidence.
No FN secret, binding, signer journal, trade, or live wallet is used.
"""

from feature_flowmesh_release import B3_ARGS
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, p2p_port


class FlowMeshConnectRPCTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 3
        self.wallet_names = []
        self.ports = [p2p_port(4), p2p_port(5)]
        common = [*B3_ARGS, "-disablewallet", "-enableflowmeshvalidator=1"]
        self.extra_args = [
            [*common, "-flowmeshtransport=independent", "-flowmeshrole=observer",
             "-flowmeshlisten=1", "-flowmeshbind=127.0.0.1",
             f"-flowmeshport={port}"] for port in self.ports
        ] + [[*common, "-flowmeshtransport=legacy"]]

    def pair_ready(self):
        statuses = [node.getflowmeshnetworkinfo() for node in self.nodes[:2]]
        return all(any(peer["operator_pubkey"] == statuses[1 - index]["operator_pubkey"]
                       and all(peer[name] for name in ("authenticated", "live", "actions", "bulk"))
                       for peer in status["peers"])
                   for index, status in enumerate(statuses))

    def run_test(self):
        first, second, legacy = self.nodes
        infos = [node.getflowmeshnetworkinfo() for node in self.nodes[:2]]
        keys = [info["operator_pubkey"] for info in infos]
        for info in infos:
            assert info["running"] and info["listening"]
            assert_equal(info["targets"], [])
            assert_equal(info["peers"], [])
        target = f"{keys[1]}@127.0.0.1:{self.ports[1]}"

        self.log.info("Malformed, unpinned and self targets are explicit refusals")
        for invalid in ("", "x" * 257, f"127.0.0.1:{self.ports[1]}",
                        f"{keys[1]}@127.0.0.1:0", f"{keys[1]}@127.0.0.1:65536",
                        f"{keys[1]}@localhost:{self.ports[1]}",
                        f"{keys[0]}@127.0.0.1:{self.ports[1]}"):
            refused = first.flowmeshconnect(invalid)
            assert_equal(refused["accepted"], False)
            assert_equal(refused["status"], "refused")
            assert refused["error"]
        assert_equal(first.getflowmeshnetworkinfo()["targets"], [])
        assert_raises_rpc_error(-3, "", first.flowmeshconnect, 7)
        assert_equal(legacy.flowmeshconnect(target)["accepted"], False)

        self.log.info("Runtime RPC admission establishes all three channels without restart")
        config_before = (first.datadir_path / "b3coin.conf").read_bytes()
        admitted = first.flowmeshconnect(target)
        assert_equal(admitted["accepted"], True)
        assert_equal(admitted["status"], "queued")
        assert_equal(admitted["already_present"], False)
        assert_equal(admitted["operator_pubkey"], keys[1])
        assert_equal(admitted["persistence"], "memory_only")
        assert_equal(admitted["connection_is_quorum_proof"], False)
        assert_equal(admitted["error"], "")
        repeated = first.flowmeshconnect(target)
        assert_equal(repeated["already_present"], True)
        assert_equal(repeated["accepted"], True)
        assert_equal(repeated["status"], "already_present")
        self.wait_until(self.pair_ready, timeout=30, check_interval=0.05)
        observed = first.getflowmeshnetworkinfo()["targets"]
        assert_equal(len(observed), 1)
        assert observed[0]["runtime_added"] and observed[0]["admitted_to_worker"]
        assert observed[0]["authenticated"]
        for channel in ("critical", "action", "bulk"):
            assert observed[0][channel]["authenticated"]
            assert observed[0][channel]["attempts"] >= 1

        different_pin = ("03" if keys[1].startswith("02") else "02") + keys[1][2:]
        conflict = first.flowmeshconnect(f"{different_pin}@127.0.0.1:{self.ports[1]}")
        assert_equal(conflict["accepted"], False)
        assert conflict["error"]
        assert_equal(len(first.getflowmeshnetworkinfo()["targets"]), 1)

        self.log.info("B3 block advancement remains independent of live target addition")
        previous_height = first.getblockcount()
        self.generatetoaddress(first, 1, first.get_deterministic_priv_key().address)
        for node in self.nodes:
            assert_equal(node.getblockcount(), previous_height + 1)
            assert_equal(node.getbestblockhash(), first.getbestblockhash())

        self.log.info("Disconnect produces observable failures and retries, then authenticates again")
        old_attempts = first.getflowmeshnetworkinfo()["targets"][0]["critical"]["attempts"]
        self.stop_node(1)
        self.wait_until(lambda: first.getflowmeshnetworkinfo()["targets"][0]["critical"]["attempts"] > old_attempts,
                        timeout=20, check_interval=0.05)
        self.wait_until(lambda: first.getflowmeshnetworkinfo()["targets"][0]["critical"]["failures"] > 0,
                        timeout=20, check_interval=0.05)
        failed = first.getflowmeshnetworkinfo()["targets"][0]
        assert failed["critical"]["last_error"]
        assert not failed["authenticated"]
        self.start_node(1)
        assert_equal(second.getflowmeshnetworkinfo()["operator_pubkey"], keys[1])
        self.wait_until(self.pair_ready, timeout=30, check_interval=0.05)
        assert_equal((first.datadir_path / "b3coin.conf").read_bytes(), config_before)

        self.log.info("Restart forgets runtime-only target, preserves network identity and configuration")
        self.restart_node(0)
        restarted = first.getflowmeshnetworkinfo()
        assert_equal(restarted["operator_pubkey"], keys[0])
        assert_equal(restarted["targets"], [])
        assert_equal((first.datadir_path / "b3coin.conf").read_bytes(), config_before)


if __name__ == "__main__":
    FlowMeshConnectRPCTest(__file__).main()
