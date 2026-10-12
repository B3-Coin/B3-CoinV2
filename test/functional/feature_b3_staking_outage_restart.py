#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Age-only IBD cannot permanently strand regtest staking after a long outage.

REGTEST ONLY: two local modern-regtest nodes, independent 80/20 stakes,
real BLS checkpoint votes and unchanged on-disk signing journals. No bridge
contract, Anvil, FlowMesh market or external endpoint is used.

Restart uses startup -mocktime more than 24 hours past the persisted tip;
setting mocktime by RPC after startup could accidentally let IBD latch
false first. The ordinary default tip-age limit is never widened.

The recovery exception is production-only. With an eligible unfinalized
checkpoint already on disk, no peer and a known missing higher block must
both leave the actual signing watermark and journal unchanged. A genuine
outbound peer's solicited header reply may allow an H1 escape block (default)
or a V1 block at universal eligibility saturation (--pre-h1), but
new checkpoint signing must occur only after that block makes the tip
recent and ordinary IBD exit is logged.

This is process-restart and local-peer policy qualification, not a claim
that one peer proves global synchronization, or a power-loss test.
"""

import hashlib
import json
import time

from test_framework.b3bridge import (
    B3_BRIDGE_ARGS,
    CHECKPOINT_INTERVAL,
    MODERN_START,
    B3BridgeChainMixin,
)
from test_framework.messages import CBlockHeader, from_hex, msg_headers
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_greater_than_or_equal


STAKES = (80, 20)
H1_HEIGHT = MODERN_START + 1
ESCAPE_ROUNDS = 60
CHECKPOINT_DEPTH = 3
OUTAGE_SECONDS = 24 * 60 * 60 + 300
# At least one complete five-second baseline IBD wait occurs in each
# negative phase; timeout scaling is applied to these observation windows.
NEGATIVE_SECONDS = 6


class HeaderOnlyModernPeer(P2PInterface):
    """Modern-wire inbound test peer that deliberately supplies no bodies.

    It cannot authorize the recovery exception, which requires an actual
    outbound/manual peer and a solicited nonempty response for the tip.
    """

    def peer_connect_send_version(self, services):
        super().peer_connect_send_version(services)
        self.on_connection_send_msg.nVersion = 80010


class B3StakingOutageRestartTest(B3BridgeChainMixin, BitcoinTestFramework):
    bridge_validator_count = len(STAKES)

    def add_options(self, parser):
        parser.add_argument("--pre-h1", action="store_true",
                            help="Exercise preserved V1 Modern PoS without activating H1")

    def set_test_params(self):
        self.num_nodes = len(STAKES)
        self.setup_clean_chain = True
        args = [arg for arg in B3_BRIDGE_ARGS if not arg.startswith("-b3minfinalityset=")]
        self.b3_args = args + [
            "-b3minfinalityset=2",
            "-debug=net",
            "-debug=validation",
        ]
        if not self.options.pre_h1:
            self.b3_args += [f"-b3h1height={H1_HEIGHT}",
                             f"-b3h1escaperounds={ESCAPE_ROUNDS}"]
        self.extra_args = [list(self.b3_args) for _ in range(self.num_nodes)]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.add_nodes(self.num_nodes, self.extra_args)
        self.start_nodes()
        for i in range(self.num_nodes):
            self.init_wallet(node=i)
        self.connect_nodes(1, 0)

    @staticmethod
    def journals(node):
        """Read test signer records only; never delete or rewrite them."""
        directory = node.chain_path / "finality_signer"
        records = {path.name: path.read_bytes() for path in sorted(directory.glob("*.dat"))}
        assert records, f"no persistent signing record in {directory}"
        return records

    @staticmethod
    def watermark(node):
        return node.getstakinginfo()["staking"]["last_signed_height"]

    @staticmethod
    def log_since(node, offset):
        with node.debug_log_path.open("rb") as stream:
            stream.seek(offset)
            return stream.read().decode("utf8", errors="replace")

    def assert_still_waiting(self, node, *, tip, height, watermark, journals, log_offset):
        """Observe a real interval, not only a single favorable RPC sample."""
        deadline = time.monotonic() + NEGATIVE_SECONDS * self.options.timeout_factor
        while time.monotonic() < deadline:
            assert_equal(node.getblockchaininfo()["initialblockdownload"], True)
            assert_equal(node.getbestblockhash(), tip)
            assert_equal(node.getblockcount(), height)
            assert_equal(self.watermark(node), watermark)
            assert_equal(self.journals(node), journals)
            assert "staking: signed " not in self.log_since(node, log_offset)
            assert "staking: produced modern-PoS block " not in self.log_since(node, log_offset)
            time.sleep(0.1)

    @staticmethod
    def round_passed(state, round_number, next_height):
        return (
            f"recovery round {round_number} at height {next_height};" in state
            and ("no proposer slot" in state or "window expired" in state)
        )

    def produce_one(self, producer, *, synchronize):
        """Produce one genuine signed block by stepping the normal rounds.

        The other validator is stopped, so a one-member signature cannot
        reach the two-member headcount quorum. Mock time is still recent;
        this setup helper does not use the outage-recovery exception.
        """
        height = producer.getblockcount()
        parent_time = producer.getblockheader(producer.getbestblockhash())["time"]
        deadline = time.monotonic() + 90 * self.options.timeout_factor
        assert_equal(producer.startstaking()["finality_signing"], True)
        try:
            while producer.getblockcount() == height:
                if time.monotonic() >= deadline:
                    raise AssertionError(f"solo validator did not produce height {height + 1}")
                timestamp = max(self.mock_time + 1, parent_time + 1)
                self.set_chain_time(timestamp)
                round_number = timestamp - parent_time - 1
                round_deadline = min(deadline, time.monotonic() + 2 * self.options.timeout_factor)
                while producer.getblockcount() == height and time.monotonic() < round_deadline:
                    if self.round_passed(producer.getstakinginfo()["staking"]["state"], round_number, height + 1):
                        break
                    time.sleep(0.02)
        finally:
            producer.stopstaking()
        assert_equal(producer.getblockcount(), height + 1)
        if synchronize:
            self.sync_blocks(self.nodes)
        return producer.getbestblockhash()

    def run_test(self):
        n0, n1 = self.nodes
        self.pos_running = False
        self.mock_time = 0

        self.log.info("Bootstrap two independent signers (%s)",
                      "pre-H1 V1" if self.options.pre_h1 else "H1")
        self.bring_up_validators(MODERN_START + 12, stakes=STAKES, timeout=180)
        self.freeze()
        assert_greater_than_or_equal(n0.getblockcount(), H1_HEIGHT)
        for node in self.nodes:
            producers = node.getfinalitystatus().get("producers")
            if self.options.pre_h1:
                assert producers is None, "pre-H1 profile unexpectedly configured H1"
            else:
                assert_equal(producers["active"], True)
        assert_equal(n0.getblockchaininfo()["initialblockdownload"], False)
        before_watermark = self.watermark(n0)
        assert_greater_than_or_equal(before_watermark, MODERN_START)

        # Leave a checkpoint that n0 really could sign, not merely an empty
        # signing pool for which "no votes during IBD" would be vacuous.
        # Checkpoints are on the M + k*interval grid; depth is three.
        checkpoint = MODERN_START + (
            (max(before_watermark, self.finalized_height(n0)) - MODERN_START) // CHECKPOINT_INTERVAL + 1
        ) * CHECKPOINT_INTERVAL
        self.set_chain_time(max(n0.getblockheader(n0.getbestblockhash())["time"], int(time.time())))
        # The extra block lets n1 run its signing pass at depth three
        # before produce_one() stops it immediately after publication.
        while n0.getblockcount() < checkpoint + CHECKPOINT_DEPTH + 1:
            self.produce_one(n1, synchronize=True)
        assert_equal(self.watermark(n0), before_watermark)
        assert_greater_than(checkpoint, self.finalized_height(n0))
        assert_greater_than_or_equal(self.watermark(n1), checkpoint)
        assert_equal(n0.getfinalitystatus()["lineage_broken"], False)

        persisted_tip = n0.getbestblockhash()
        persisted_height = n0.getblockcount()
        journals_before = self.journals(n0)

        # n1 owns one additional, fully valid modern block. n0 sees its
        # genuine header after restart, but no body until explicitly released.
        self.disconnect_nodes(1, 0)
        withheld_hash = self.produce_one(n1, synchronize=False)
        withheld_header = from_hex(CBlockHeader(), n1.getblockheader(withheld_hash, False))
        withheld_body = n1.getblock(withheld_hash, 0)
        assert_equal(n0.getbestblockhash(), persisted_tip)
        assert_equal(withheld_header.hash_hex, withheld_hash)

        self.stop_nodes()
        assert_equal(self.journals(n0), journals_before)
        stale_now = withheld_header.nTime + OUTAGE_SECONDS
        self.mock_time = stale_now
        stale_args = self.b3_args + [f"-mocktime={stale_now}"]
        restart_offset = n0.debug_log_path.stat().st_size
        self.start_node(0, extra_args=stale_args)
        self.start_node(1, extra_args=stale_args)
        # Both nodes remain disconnected. The framework's config disables
        # automatic connections, discovery and DNS/fixed seeds.
        assert_equal(n0.getconnectioncount(), 0)
        assert_equal(n0.getblockchaininfo()["initialblockdownload"], True)
        assert_equal(n1.getblockchaininfo()["initialblockdownload"], True)
        assert_equal(n0.getbestblockhash(), persisted_tip)
        assert_equal(self.journals(n0), journals_before)
        assert_equal(n0.startstaking()["finality_signing"], True)
        self.wait_until(lambda: self.watermark(n0) == before_watermark)

        self.log.info("Age-only IBD with no peer must not produce or sign")
        self.assert_still_waiting(
            n0, tip=persisted_tip, height=persisted_height, watermark=before_watermark,
            journals=journals_before, log_offset=restart_offset,
        )

        self.log.info("Known higher valid header without its body must remain blocked")
        header_peer = n0.add_p2p_connection(HeaderOnlyModernPeer())
        header_peer.send_and_ping(msg_headers([withheld_header]))
        self.wait_until(lambda: n0.getblockchaininfo()["headers"] == persisted_height + 1)
        assert_equal(n0.getblockheader(withheld_hash)["height"], persisted_height + 1)
        self.assert_still_waiting(
            n0, tip=persisted_tip, height=persisted_height, watermark=before_watermark,
            journals=journals_before, log_offset=restart_offset,
        )

        # Deliver the original body, not a replacement block or a locally
        # synthesized authority. An inbound peer/body alone is insufficient.
        assert_equal(n0.submitblock(withheld_body), None)
        assert_equal(n0.getbestblockhash(), withheld_hash)
        assert_equal(n0.getblockchaininfo()["initialblockdownload"], True)
        n0.disconnect_p2ps()
        self.wait_until(lambda: n0.getconnectioncount() == 0)
        self.assert_still_waiting(
            n0, tip=withheld_hash, height=persisted_height + 1, watermark=before_watermark,
            journals=journals_before, log_offset=restart_offset,
        )

        self.log.info("A real outbound modern peer can confirm the exact stale tip")
        # n1 is not staking. Only n0 can create the recovery block. A normal
        # GETHEADERS round trip must precede this; connection count alone or
        # an unsolicited inbound announcement must not be sufficient.
        n0.stopstaking()
        self.connect_nodes(0, 1)
        probe_text = f"Regtest staking tip probe completed at height {persisted_height + 1} "
        self.wait_until(lambda: probe_text in self.log_since(n0, restart_offset), timeout=10)
        # Let the initial hint expire without disconnecting. An already-started
        # sync session must perform another bounded query, not strand recovery
        # permanently because fSyncStarted remains true.
        self.set_chain_time(self.mock_time + 5 * 60 + 1)
        self.wait_until(lambda: self.log_since(n0, restart_offset).count(probe_text) >= 2, timeout=10)
        assert_equal(n0.getblockchaininfo()["initialblockdownload"], True)
        assert_equal(self.journals(n0), journals_before)
        assert_equal(n0.startstaking()["finality_signing"], True)
        recovery_deadline = time.monotonic() + 60 * self.options.timeout_factor
        while n0.getblockcount() == persisted_height + 1:
            if time.monotonic() >= recovery_deadline:
                raise AssertionError(
                    "stale regtest tip did not recover after an outbound header round trip: "
                    f"{n0.getstakinginfo()['staking']}"
                )
            parent_time = withheld_header.nTime
            round_number = self.mock_time - parent_time - 1
            if self.round_passed(n0.getstakinginfo()["staking"]["state"], round_number,
                                 persisted_height + 2):
                # A fixed mock clock must advance if the 20% validator owns
                # this ordinary recovery round; the disabled peer never stakes.
                self.set_chain_time(self.mock_time + 1)
            time.sleep(0.05)
        n0.stopstaking()
        self.wait_until(lambda: not n0.getblockchaininfo()["initialblockdownload"], timeout=10)
        first_recovery_hash = n0.getblockhash(persisted_height + 2)
        first_recovery_header = n0.getblockheader(first_recovery_hash)
        assert_greater_than_or_equal(first_recovery_header["time"], stale_now)
        if self.options.pre_h1:
            # This fixture has a one-second block interval and round duration.
            assert_greater_than_or_equal(first_recovery_header["time"] - withheld_header.nTime - 1, 128)
            assert "producers" not in n0.getfinalitystatus()
        self.sync_blocks(self.nodes)
        assert_equal(n1.getblockchaininfo()["initialblockdownload"], False)

        # It is possible to stop immediately after production but before the
        # next signer iteration. Restarting the loop after IBD exit must now
        # permit the checkpoint that was deliberately left eligible earlier.
        assert_equal(n0.startstaking()["finality_signing"], True)
        self.wait_until(lambda: self.watermark(n0) >= checkpoint, timeout=15)
        n0.stopstaking()
        assert self.journals(n0) != journals_before

        recovery_log = self.log_since(n0, restart_offset)
        probe = recovery_log.index(
            f"Regtest staking tip probe completed at height {persisted_height + 1} "
        )
        produced = recovery_log.index(f"staking: produced modern-PoS block {first_recovery_hash}")
        ibd_exit = recovery_log.index("Leaving InitialBlockDownload (latching to false)")
        signed = recovery_log.index("staking: signed ")
        assert probe < produced, "recovery production preceded the exact-tip solicited peer reply"
        assert ibd_exit < signed, "new checkpoint signing preceded ordinary IBD exit"
        assert produced < signed, "new checkpoint signing preceded the recovery block"

        # Retain a compact, secret-free record beside the ordinary framework
        # logs. The journal is compared as bytes, but only hashes are reported.
        evidence = {
            "profile": ("b3-pre-h1-age-only-outage-restart-v1" if self.options.pre_h1
                        else "b3-h1-age-only-outage-restart-v1"),
            "h1_activation_height": None if self.options.pre_h1 else H1_HEIGHT,
            "production_round_gate": 128 if self.options.pre_h1 else ESCAPE_ROUNDS,
            "stakes": list(STAKES),
            "minimum_finality_set": 2,
            "outage_seconds": OUTAGE_SECONDS,
            "same_connection_probe_renewal": True,
            "persisted_height": persisted_height,
            "persisted_tip": persisted_tip,
            "eligible_unfinalized_checkpoint": checkpoint,
            "watermark_before_restart": before_watermark,
            "withheld_block": withheld_hash,
            "recovery_block": first_recovery_hash,
            "watermark_after_ibd_exit": self.watermark(n0),
            "journal_sha256_before": {
                name: hashlib.sha256(data).hexdigest() for name, data in journals_before.items()
            },
            "log_order": {
                "solicited_tip_reply": probe,
                "produced": produced,
                "ibd_exit": ibd_exit,
                "first_new_signing": signed,
            },
            "limitations": [
                "process restart, not machine power loss",
                "one solicited local peer is not proof of global synchronization",
                "fixed synthetic regtest membership; no bridge contract or live activation",
            ],
        }
        evidence_path = n0.datadir_path.parent / "staking_outage_restart.json"
        evidence_path.write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf8")
        self.log.info("Stale-tip restart recovered without bypassing finality IBD: %s", evidence_path)


if __name__ == "__main__":
    B3StakingOutageRestartTest(__file__).main()
