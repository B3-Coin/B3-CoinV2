#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""RT2: the B3 Ethereum light client on regtest (doc/design/b3-bridge-v2-regtest-plan.md §5).

REGTEST ONLY. Every process is local: four B3 regtest validators linked only
to each other, n4 (a startup-refusal and pin-record fixture that never
stakes), one anvil on 127.0.0.1 (chain 31337, never forked), the synthetic
sync-committee tool and the mock beacon. forge runs with --offline.

1. anvil gets MockTetherToken, MockDepositVerifier and the production
   B3StakerBridge (deployment block d = 3), then head c = d + 20,000. The
   pinned checkpoint H(c) is the +20,000 window case, 64 slots before a
   sync-committee period boundary. The pin writer and the node both refuse
   a checkpoint at c + 1 (+20,001); the node also refuses the pin file
   without -b3flowmeshtest.
2. n0-n3 start from height 0 with the pins and bring up live BLS finality.
3. A deposit D1 lands at c + 70. The mock beacon finalizes c + 90 across the
   period boundary; the rotation update is signed by exactly 342 of 512.
4. The unchanged contrib/b3bridge/b3_bridge_relayer.py, run one cycle at a
   time, relays the bootstrap, both updates and the D1 mint. Its first
   Ethereum scan walks d..c (20,001 headers) from the bootstrap anchor.
5. Refused by B3 consensus prevalidation: 341 of 512 signers, a wrong
   genesis validators root, a two-period jump, an unpinned fork, a
   conflicting finalized header and a no-op update. A second deposit D2 at
   c + 93 is finalized by a mined update; its unclaimed mint passes
   prevalidation (not broadcast) with the light-client head exactly 8,192
   slots behind and is refused as stale one second later. The replayed D1
   mint is refused as nullified.
6. n3 reindexes and agrees with every node on the light client, the
   finalized store export and the deposit proof status. The pin-file record
   refuses n3 with altered caps after activation, with -reindex or
   -reindex-chainstate, and with the record deleted (a missing record means
   no pin file); nothing is wiped and the original file restarts n3 at the
   same tip. On n4 a pin change is recorded while no block at or above
   activation is stored, still refused by a reindex, and refused once n4
   stores blocks at or above activation, even with its tip at A3 - 1
   (-stopatheight).
"""

import hashlib
import os
import sqlite3
import time

from test_framework.anvil import ArchiveCodeProxy, deploy
from test_framework.b3bridge import (
    BRIDGE_ACTIVATION_HEIGHT,
    DEFAULT_MAX_PER_BLOCK,
    DEFAULT_MAX_PER_EPOCH,
    MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS,
    MAX_SYNC_LAG_SLOTS,
    PERIOD_SLOTS,
    REGTEST_ETH_CHAIN_ID,
    REGTEST_GVR,
    SLOTS_PER_EPOCH,
    SYNC_COMMITTEE_SUPERMAJORITY,
    B3BridgeChainMixin,
    BridgePinError,
    abi_call,
    b3_bridge_args,
    build_direct_mint_plan,
    call_address,
    call_uint,
    enc_address,
    enc_bytes32,
    enc_uint,
    file_sha256,
    find_deposits,
    recipient_v1,
    regtest_chain_domain,
    rpc_refusal,
    start_refused,
    to_0x,
    write_regtest_bridge_pins,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
    p2p_port,
)

MAX_DEPOSIT_RAW = DEFAULT_MAX_PER_BLOCK  # B3StakerBridge.MAX_DEPOSIT_RAW
TOKEN_SUPPLY = 10**12
D1_RAW = 25_000_000
D1_ID = 0  # B3StakerBridge.nextDepositId starts at zero.
D2_RAW = 7_000_000
D2_ID = 1
SYNC_COMMITTEE_SIZE = 512
WINDOW = MAX_BRIDGE_CUMULATIVE_BACKFILL_BLOCKS
# Block offsets from the checkpoint block c.
BOUNDARY_OFFSET = 64   # slot(c + 64) is the first slot of period P.
DEPOSIT_OFFSET = 70    # D1's block.
FINALIZED_OFFSET = 90  # The newest execution block the relayed updates finalize.
D2_OFFSET = 93         # D2's block, finalized by an update the test mines.
# A mint is fresh while now - T_f <= max_sync_lag_slots * 12 s
# (FinalizedHeadFresh, src/node/bridge_state.cpp).
STALE_AFTER_SECONDS = MAX_SYNC_LAG_SLOTS * 12
RELAY_TIMEOUT = 600
RELAY_REPEATED_FAILURES = 30
RPC_VERIFY_REJECTED = -26
PINS_RECORD_FILENAME = "b3bridgepins.sha256"  # The pin-file record, in <datadir>/regtest
# getbridgeinfo light-client and bridge-state fields every node must agree on.
LIGHT_CLIENT_FIELDS = (
    "light_client_bootstrapped", "light_client_period", "finalized_beacon_slot",
    "finalized_beacon_root", "finalized_execution_block", "finalized_execution_hash",
    "current_sync_committee_root", "next_sync_committee_root",
    "light_client_connected_height", "light_client_connected_block",
    "registry_id", "asset_id", "nullifiers", "trusted_checkpoint_root",
    "trusted_checkpoint_slot",
)
NODE_WINDOW_ERROR = ("Invalid -b3bridgepins file: v1_decentralized.light_client."
                     "checkpoint_execution_block ({checkpoint}) must lie within "
                     f"{WINDOW} blocks at or after origin_deployment_block ({{deployment}})")
NODE_NEEDS_FLOWMESH_ERROR = "-b3bridgepins requires -b3modernregtest and -b3flowmeshtest (regtest-only)"
NODE_PINS_CHANGED_ERROR = ("-b3bridgepins differs from the pin file recorded for this datadir, which already "
                           "stores blocks at or above bridge activation; start from a fresh -datadir")
NODE_PINS_REINDEX_ERROR = ("-b3bridgepins differs from the pin file recorded for this datadir; -reindex and "
                           "-reindex-chainstate cannot apply a changed pin file; start from a fresh -datadir")


def gas_used(receipt):
    return int(receipt["gasUsed"], 16)


def read_record(path):
    with open(path, encoding="utf8") as f:
        return f.read().strip()


def boundary_slot_offset(checkpoint_block):
    """slot_offset S0 putting the period boundary at checkpoint_block + 64.

    P = (c + 64) // 8192 + 1 and S0 = 8192 * P - 64 - c, so slot(c) =
    8192 * P - 64 lies in period P - 1 and slot(c + 64) starts period P.
    """
    period = (checkpoint_block + BOUNDARY_OFFSET) // PERIOD_SLOTS + 1
    return PERIOD_SLOTS * period - BOUNDARY_OFFSET - checkpoint_block, period


class B3BridgeRT2LightClientTest(B3BridgeChainMixin, BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 5  # n4 only proves startup refusals.
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_foundry(("anvil", "forge"))
        self.skip_if_no_bridge_tools()

    # -- Ethereum side, pins and startup refusals ---------------------------
    def setup_network(self):
        self.log.info("anvil: MockTetherToken, MockDepositVerifier and B3StakerBridge")
        anvil = self.anvil = self.start_anvil()
        self.owner, self.depositor = anvil.accounts()[:2]
        self.bridge_forge_build()
        domain = regtest_chain_domain()
        self.token, token_block, _ = self.deploy_contract("MockTetherToken.sol", "MockTetherToken")
        self.verifier, verifier_block, _ = self.deploy_contract(
            "MockDepositVerifier.sol", "MockDepositVerifier",
            enc_bytes32(domain) + enc_uint(BRIDGE_ACTIVATION_HEIGHT))
        verifier_code_hash = anvil.code_hash(self.verifier)
        self.vault, self.deployment_block, _ = self.deploy_contract(
            "B3StakerBridge.sol", "B3StakerBridge",
            enc_address(self.verifier) + verifier_code_hash + domain +
            enc_address(self.token) + enc_uint(MAX_DEPOSIT_RAW))
        d = self.deployment_block
        assert_equal((token_block, verifier_block, d), (1, 2, 3))
        assert_equal(call_address(anvil, self.vault, "ORIGIN_TOKEN()"), self.token)
        assert_equal(call_uint(anvil, self.vault, "MAX_DEPOSIT_RAW()"), MAX_DEPOSIT_RAW)
        assert_equal(call_uint(anvil, self.vault, "BRIDGE_ACTIVATION_HEIGHT()"),
                     BRIDGE_ACTIVATION_HEIGHT)

        self.log.info("anvil: issue, fund the depositor and approve the vault once")
        anvil.send(self.owner, self.token, abi_call("issue(uint256)", enc_uint(TOKEN_SUPPLY)))
        anvil.send(self.owner, self.token, abi_call(
            "transfer(address,uint256)", enc_address(self.depositor), enc_uint(TOKEN_SUPPLY)))
        anvil.send(self.depositor, self.token, abi_call(
            "approve(address,uint256)", enc_address(self.vault), enc_uint(TOKEN_SUPPLY)))

        # The relayer re-checks every pinned runtime at block d on each cycle;
        # anvil drops that state 500 blocks later (see ArchiveCodeProxy).
        self.log.info("Archive proxy: record the pinned runtimes at the deployment block")
        self.eth_proxy = self.register_bridge_process(ArchiveCodeProxy(anvil.url)).start()
        for address in (self.vault, self.token, self.verifier):
            code = self.eth_proxy.record_code(address, d)
            assert code == anvil.code(address), f"runtime at {address} changed after block {d}"

        self.log.info(f"anvil: mine to c = d + {WINDOW}, the +{WINDOW} checkpoint window case")
        c = self.checkpoint_block = d + WINDOW
        anvil.mine_to(c)
        self.slot_offset, self.period = boundary_slot_offset(c)
        self.beacon = self.start_synthetic_beacon(anvil, self.slot_offset, c)
        self.bootstrap = self.beacon.bootstrap()
        assert_equal(self.bootstrap["checkpoint_block"], c)
        assert_equal(self.bootstrap["checkpoint_block_hash"], anvil.block(c)["hash"])
        assert_equal(self.bootstrap["checkpoint_slot"], PERIOD_SLOTS * self.period - BOUNDARY_OFFSET)
        assert_equal(self.bootstrap["period"], self.period - 1)
        assert_equal(self.beacon.period_of_block(c + BOUNDARY_OFFSET - 1), self.period - 1)
        assert_equal(self.beacon.period_of_block(c + BOUNDARY_OFFSET), self.period)
        self.pins_path = os.path.join(self.options.tmpdir, "b3bridgepins.json")
        self.pins = write_regtest_bridge_pins(
            self.pins_path, anvil, self.vault, self.token, self.verifier, d, self.bootstrap)
        v1 = self.pins["v1_decentralized"]
        assert_equal(v1["origin_deployment_block"], d)
        assert_equal(v1["light_client"]["checkpoint_execution_block"], d + WINDOW)
        self.evidence.step("ethereum deployed, +20,000 checkpoint pinned",
                           eth_block=anvil.block_number(), token=self.token,
                           verifier=self.verifier, vault=self.vault, deployment_block=d,
                           checkpoint_block=c, checkpoint_slot=self.bootstrap["checkpoint_slot"],
                           checkpoint_root=self.bootstrap["checkpoint_root"],
                           rotation_period=self.period, slot_offset=self.slot_offset,
                           pins_sha256=file_sha256(self.pins_path))

        self.log.info("+20,001: the pin writer refuses a checkpoint at c + 1")
        anvil.mine(1)
        late_beacon = self.start_synthetic_beacon(anvil, self.slot_offset, c + 1, name="beacon_late")
        late_bootstrap = late_beacon.bootstrap()
        assert_equal(late_bootstrap["checkpoint_block"], d + WINDOW + 1)
        late_path = os.path.join(self.options.tmpdir, "b3bridgepins-plus-20001.json")
        try:
            write_regtest_bridge_pins(late_path, anvil, self.vault, self.token, self.verifier, d,
                                      late_bootstrap)
        except BridgePinError as e:
            writer_refusal = str(e)
        else:
            raise AssertionError("the pin writer accepted a +20,001 checkpoint")
        assert f"must lie within {WINDOW} blocks" in writer_refusal, writer_refusal
        assert not os.path.exists(late_path)
        # Written anyway (check=False) only to prove the node's own refusal.
        write_regtest_bridge_pins(late_path, anvil, self.vault, self.token, self.verifier, d,
                                  late_bootstrap, check=False)

        self.add_nodes(self.num_nodes, [b3_bridge_args(self.pins_path) for _ in range(self.num_nodes)])
        n4 = self.nodes[4]
        self.log.info("+20,001: the node refuses the same checkpoint at startup")
        node_window_error = NODE_WINDOW_ERROR.format(checkpoint=d + WINDOW + 1, deployment=d)
        node_refusal = start_refused(n4, b3_bridge_args(late_path), node_window_error)
        self.log.info("The pin file needs -b3flowmeshtest")
        flowmesh_refusal = start_refused(
            n4, [arg for arg in b3_bridge_args(self.pins_path) if arg != "-b3flowmeshtest"],
            NODE_NEEDS_FLOWMESH_ERROR)
        self.evidence.assertion("+20,001 checkpoint refused by the pin writer and the node",
                                expected={"writer": f"must lie within {WINDOW} blocks",
                                          "node": node_window_error},
                                actual={"writer": writer_refusal, "node": node_refusal},
                                late_pins_sha256=file_sha256(late_path))
        self.evidence.assertion("the pin file needs -b3flowmeshtest",
                                expected=NODE_NEEDS_FLOWMESH_ERROR, actual=flowmesh_refusal)

        self.start_bridge_validators(self.pins_path)

    def deploy_contract(self, file, contract, ctor_bytes=b""):
        address, block, code_hash = deploy(
            self.anvil, self.owner, self.bridge_artifact(file, contract), ctor_bytes)
        for tx_hash in self.anvil.block(block)["transactions"]:
            receipt = self.anvil.receipt(tx_hash)
            if (receipt.get("contractAddress") or "").lower() == address:
                self.evidence.gas(f"deploy {contract}", gas_used(receipt), block=block)
        return address, block, code_hash

    # -- test ---------------------------------------------------------------
    def run_test(self):
        self.log.info("B3: four validators with live BLS finality up to A3 - 1")
        self.bring_up_validators(BRIDGE_ACTIVATION_HEIGHT - 1)
        # bring_up_validators() waits for n0 only; every node must evaluate A3.
        self.wait_until(lambda: all(node.getblockcount() >= BRIDGE_ACTIVATION_HEIGHT - 1
                                    for node in self.bridge_nodes()))
        self.evidence_step("B3 validators live", self.anvil,
                           finalized=self.finalized_height(self.nodes[0]))
        self.check_pins_on_nodes()
        self.d1 = self.make_deposit()
        self.serve_beacon()
        self.relay_with_unchanged_relayer()
        self.check_first_scan()
        self.check_rejected_updates()
        self.d2 = self.make_finalized_deposit_d2()
        self.check_stale_head_and_replay()
        self.check_reindex()
        self.check_pin_guard()
        self.check_pin_change_below_activation()
        self.evidence_step("RT2 complete", self.anvil)

    def check_pins_on_nodes(self):
        self.log.info("Every node runs the pinned deployment and records the pin file")
        v1 = self.pins["v1_decentralized"]
        light = v1["light_client"]
        pins_sha256 = file_sha256(self.pins_path)
        infos, records = [], set()
        for node in self.bridge_nodes():
            info = node.getbridgeinfo()
            assert_equal((info["status"], info["configured"], info["ready"], info["active"]),
                         ("active", True, True, True))
            assert_equal(info["withdrawal_mode"], "decentralized-verifier-v1")
            assert_equal(info["ethereum_chain_id"], REGTEST_ETH_CHAIN_ID)
            assert_equal(info["origin_deployment_block"], self.deployment_block)
            assert_equal("0x" + info["vault"], self.vault)
            assert_equal("0x" + info["token"], self.token)
            assert_equal("0x" + info["decentralized_verifier"], self.verifier)
            assert_equal("0x" + info["trusted_checkpoint_root"], light["trusted_checkpoint_root"])
            assert_equal(info["trusted_checkpoint_slot"], light["trusted_checkpoint_slot"])
            assert_equal(info["fork_schedule_valid_through_epoch"],
                         light["fork_schedule_valid_through_epoch"])
            assert_equal("0x" + info["genesis_validators_root"], to_0x(REGTEST_GVR))
            assert_equal(info["min_sync_committee_participants"], SYNC_COMMITTEE_SUPERMAJORITY)
            assert_equal(info["max_sync_lag_slots"], MAX_SYNC_LAG_SLOTS)
            assert_equal(info["light_client_bootstrapped"], False)
            # The pin-file record holds this file's hash from startup.
            records.add(read_record(os.path.join(node.chain_path, PINS_RECORD_FILENAME)))
            infos.append(info)
        assert_equal(records, {pins_sha256})
        for key in ("registry_id", "asset_id", "max_per_block", "max_per_epoch"):
            assert_equal({info[key] for info in infos}, {infos[0][key]})
        self.asset_id = infos[0]["asset_id"]
        self.registry_id = infos[0]["registry_id"]
        self.evidence.assertion("every node runs the pins and records the pin file",
                                expected=[pins_sha256], actual=sorted(records),
                                registry_id=self.registry_id, asset_id=self.asset_id)

    def make_deposit(self):
        c = self.checkpoint_block
        self.log.info(f"anvil: deposit D1 of {D1_RAW} raw units at c + {DEPOSIT_OFFSET}")
        anvil = self.anvil
        anvil.mine_to(c + DEPOSIT_OFFSET - 1)
        self.recipient = self.nodes[0].getnewaddress()
        receipt = anvil.send(self.depositor, self.vault, abi_call(
            "deposit(uint256,bytes32)", enc_uint(D1_RAW), recipient_v1(self.recipient)))
        self.evidence.gas("B3StakerBridge.deposit", gas_used(receipt))
        k = int(receipt["blockNumber"], 16)
        assert_equal(k, c + DEPOSIT_OFFSET)
        # The attested block of the update that finalizes c + 90.
        anvil.mine_to(c + FINALIZED_OFFSET + 1)
        assert_equal(call_uint(anvil, self.vault, "locked()"), D1_RAW)
        deposits = find_deposits(anvil, self.vault, self.token, k, k)
        assert_equal(len(deposits), 1)
        d1 = deposits[0]
        assert_equal((d1["deposit_id"], d1["block_number"], d1["amount"]), (D1_ID, k, D1_RAW))
        assert_equal(d1["recipient"], to_0x(recipient_v1(self.recipient)))
        self.evidence_step("ethereum deposit D1", anvil, deposit_block=k, deposit_id=D1_ID,
                           amount=D1_RAW, recipient=self.recipient)
        return d1

    def serve_beacon(self):
        c, period = self.checkpoint_block, self.period
        self.log.info(f"Mock beacon: finalize c + {FINALIZED_OFFSET}, rotating into period "
                      f"{period} with {SYNC_COMMITTEE_SUPERMAJORITY} of {SYNC_COMMITTEE_SIZE} signers")
        beacon = self.beacon
        beacon.set_participants(period, SYNC_COMMITTEE_SUPERMAJORITY)
        assert_equal(beacon.finalize_through(c + FINALIZED_OFFSET), c + FINALIZED_OFFSET)
        served = beacon.period_updates()
        assert_equal([update["period"] for update in served], [period - 1, period])
        before, rotation = served
        # Period P - 1: the last finalizable slot, carrying committee P.
        assert_equal(before["finalized_slot"], PERIOD_SLOTS * period - 3)
        assert_equal(before["finalized_block"], c + BOUNDARY_OFFSET - 3)
        assert_equal(before["signature_slot"], PERIOD_SLOTS * period - 1)
        assert_equal((before["participants"], before["has_next_committee"]),
                     (SYNC_COMMITTEE_SIZE, True))
        # Period P: signed by committee P (store.next after the first update).
        assert_equal(rotation["finalized_block"], c + FINALIZED_OFFSET)
        assert_equal(rotation["signature_slot"] // PERIOD_SLOTS, period)
        assert_equal((rotation["participants"], rotation["has_next_committee"]),
                     (SYNC_COMMITTEE_SUPERMAJORITY, True))
        finality = beacon.snapshot()["finality_update"]
        assert_equal((finality["finalized_block"], finality["has_next_committee"]),
                     (c + FINALIZED_OFFSET, False))
        self.beacon_url = beacon.serve()
        self.evidence_step("mock beacon serving", self.anvil, url=self.beacon_url,
                           updates=[{key: update[key] for key in (
                               "period", "finalized_block", "finalized_slot", "attested_slot",
                               "signature_slot", "participants", "has_next_committee")}
                               for update in served])

    # -- the unchanged relayer ----------------------------------------------
    def relayer_jobs(self):
        """The relayer's jobs table (b3_bridge_relayer.State), read between cycles."""
        if not os.path.exists(self.relayer_state):
            return []
        db = sqlite3.connect(self.relayer_state)
        try:
            db.row_factory = sqlite3.Row
            return [dict(row) for row in db.execute(
                "SELECT id, kind, payload, amount, address, state, txid, effect_height, "
                "effect_block FROM jobs ORDER BY id")]
        finally:
            db.close()

    def relay_complete(self):
        n0 = self.nodes[0]
        info = n0.getbridgeinfo()
        if not (info.get("light_client_bootstrapped") and
                info.get("light_client_period") == self.period and
                info.get("finalized_execution_block") == self.checkpoint_block + FINALIZED_OFFSET):
            return False
        if not n0.getbridgeproofstatus(self.d1["block_hash"], D1_ID)["deposit"]["claimed"]:
            return False
        if self.wallet_asset(n0, self.asset_id)["confirmed"] != D1_RAW:
            return False
        # The relayer confirms a job only once its effect is B3-final.
        jobs = self.relayer_jobs()
        return (all(job["state"] in ("confirmed", "superseded") for job in jobs) and
                any(job["kind"] == "mint" and job["state"] == "confirmed" for job in jobs))

    def relay_with_unchanged_relayer(self):
        self.log.info("contrib/b3bridge/b3_bridge_relayer.py relays bootstrap, updates and the mint")
        n0 = self.nodes[0]
        self.relayer_state = os.path.join(self.options.tmpdir, "relayer", "relayer.sqlite3")
        self.relayer_events = []
        cycles = failures = 0
        repeated, last_errors = 0, None
        started = time.time()
        while True:
            run = self.run_bridge_relayer_once(
                n0, self.beacon_url, self.eth_proxy.url, self.bootstrap["checkpoint_root"],
                state=self.relayer_state)
            cycles += 1
            self.relayer_events.extend(run.events)
            if run.returncode != 0:
                failures += 1
                errors = [event.get("error") for event in run.events
                          if event["event"] == "cycle_failed"] or [run.stderr[-500:]]
                self.log.debug(f"relayer cycle {cycles} failed: {errors}")
                # A retry is expected after a tip or finality race; the same
                # error on every cycle is not.
                repeated = repeated + 1 if errors == last_errors else 1
                last_errors = errors
                if repeated >= RELAY_REPEATED_FAILURES:
                    raise AssertionError(f"relayer failed {repeated} cycles in a row: {errors}")
            else:
                repeated, last_errors = 0, None
            self.synchronize_mempools()
            if self.relay_complete():
                break
            if time.time() - started > RELAY_TIMEOUT:
                raise AssertionError(
                    f"relayer did not finish within {RELAY_TIMEOUT} s ({cycles} cycles, "
                    f"{failures} failed); last stderr:\n{run.stderr[-4000:]}")
            time.sleep(1)
        elapsed = round(time.time() - started, 1)
        self.log.info(f"Relayed in {cycles} relayer cycles ({failures} retried) and {elapsed} s")

        self.log.info("The relayer's confirmed jobs are exactly the synthetic beacon's records")
        normalized = [(job["kind"], job["payload"]) for job in self.relayer_jobs()
                      if job["state"] == "confirmed"]
        served = self.beacon.period_updates()
        expected_sync = [("bootstrap", self.bootstrap["payload_hex"]),
                         ("update", served[0]["payload_hex"]),
                         ("update", served[1]["payload_hex"])]
        assert_equal(normalized[:3], [(kind, payload.lower().removeprefix("0x"))
                                      for kind, payload in expected_sync])
        mints = [job for job in self.relayer_jobs() if job["kind"] == "mint"]
        assert_equal(len(mints), 1)
        mint = mints[0]
        assert_equal((mint["state"], int(mint["amount"]), mint["address"]),
                     ("confirmed", D1_RAW, self.recipient))
        assert_equal([kind for kind, _ in normalized if kind != "execution-backfill"],
                     ["bootstrap", "update", "update", "mint"])
        status = n0.getbridgeproofstatus(self.d1["block_hash"], D1_ID)["deposit"]
        assert_equal((status["claimed_height"], status["claimed_block"]),
                     (mint["effect_height"], mint["effect_block"]))
        assert mint["txid"] in n0.getblock(status["claimed_block"])["tx"]
        prepared = [event["kind"] for event in self.relayer_events if event["event"] == "job_prepared"]
        assert_equal([kind for kind in prepared if kind != "execution-backfill"],
                     ["bootstrap", "update", "update", "mint"])
        # Every cycle re-checked the pinned runtimes at block d through the proxy.
        assert_greater_than(self.eth_proxy.served_from_archive, 0)
        self.evidence_step("relayer relayed bootstrap, rotation and the D1 mint", self.anvil,
                           relayer_cycles=cycles, relayer_failed_cycles=failures,
                           seconds=elapsed, mint_txid=mint["txid"],
                           mint_height=mint["effect_height"],
                           backfills=sum(kind == "execution-backfill" for kind, _ in normalized),
                           archive_code_answers=self.eth_proxy.served_from_archive)
        self.evidence.note("anvil keeps only its newest 500 states; the relayer's pinned-runtime "
                           "checks at the deployment block were answered from code recorded at "
                           "that block (ArchiveCodeProxy); every other request reached anvil "
                           "unchanged", answers=self.eth_proxy.served_from_archive)

    def check_first_scan(self):
        d, c = self.deployment_block, self.checkpoint_block
        self.log.info(f"The relayer's first scan walked d..c ({WINDOW + 1} headers) from the bootstrap anchor")
        events = self.relayer_events
        header_scans = [event for event in events if event["event"] == "ethereum_header_scan"]
        first = header_scans[0]
        assert_equal((first["first"], first["anchor"], first["total"]), (d, c, WINDOW + 1))
        assert any((event["first"], event["anchor"], event["verified"]) == (d, c, WINDOW + 1)
                   for event in header_scans)
        prioritized = [event for event in events
                       if event["event"] == "authenticated_history_prioritized"]
        assert_equal((prioritized[0]["cursor"], prioritized[0]["anchor_source"]),
                     (d, "retained-b3-finalized"))
        # Receipts-root completeness: one forward, contiguous pass over d..c+90.
        covered = d
        for event in events:
            if event["event"] == "ethereum_scan":
                assert_equal(event["first"], covered)
                covered = event["last"] + 1
        assert_equal(covered, c + FINALIZED_OFFSET + 1)
        planned = [event for event in events if event["event"] == "deposit_planned"]
        assert_equal([event["deposit_id"] for event in planned], [D1_ID])
        self.evidence.assertion("first relayer scan covered d..c from the +20,000 bootstrap anchor",
                                expected=WINDOW + 1, actual=first["total"],
                                first=first["first"], anchor=first["anchor"])

    # -- consensus refusals -------------------------------------------------
    def light_client_info(self, node):
        info = node.getbridgeinfo()
        return {key: info.get(key) for key in LIGHT_CLIENT_FIELDS}

    def check_rejected_updates(self):
        c, period = self.checkpoint_block, self.period
        self.log.info("B3 refuses invalid light-client updates from the synthetic committee")
        n0 = self.nodes[0]
        # The attested block of an update finalizing c + 91.
        self.anvil.mine_to(c + FINALIZED_OFFSET + 2)
        horizon = self.pins["v1_decentralized"]["light_client"]["fork_schedule_valid_through_epoch"]
        newest = c + FINALIZED_OFFSET + 1
        cases = [
            ("341 of 512 signers", {"finalized": newest, "participants": SYNC_COMMITTEE_SUPERMAJORITY - 1},
             "bridge light-client update is invalid"),
            ("wrong genesis validators root",
             {"finalized": newest, "sign_genesis_validators_root": hashlib.sha256(b"wrong").digest()},
             "bridge light-client update is invalid"),
            ("two-period jump", {"finalized": newest, "slot_shift": 2 * PERIOD_SLOTS},
             "bridge light-client update is invalid"),
            ("unpinned fork", {"finalized": newest, "slot_shift": 40_000},
             "bridge light-client update uses an unpinned Ethereum fork"),
            ("conflicting finalized header",
             {"finalized": c + FINALIZED_OFFSET, "finalized_proposer_index": 1},
             "bridge light-client update conflicts at a finalized slot"),
            ("no-op", {"finalized": c + FINALIZED_OFFSET}, "bridge light-client update is a no-op"),
        ]
        control = {"finalized": newest, "participants": SYNC_COMMITTEE_SUPERMAJORITY}
        outputs = self.beacon.updates([spec for _, spec, _ in cases] + [control])
        by_label = dict(zip([label for label, _, _ in cases], outputs))
        assert_equal(by_label["341 of 512 signers"]["participants"], SYNC_COMMITTEE_SUPERMAJORITY - 1)
        assert_equal(by_label["two-period jump"]["period"], period + 2)
        assert_greater_than(horizon + 1, by_label["two-period jump"]["signature_slot"] // SLOTS_PER_EPOCH)
        assert_greater_than(by_label["unpinned fork"]["signature_slot"] // SLOTS_PER_EPOCH, horizon)
        assert_equal(by_label["conflicting finalized header"]["finalized_slot"],
                     self.slot_offset + c + FINALIZED_OFFSET)
        self.freeze()
        before = self.light_client_info(n0)
        observed = {}
        for (label, _, message), output in zip(cases, outputs):
            self.log.debug(f"refused: {label}")
            observed[label] = rpc_refusal(RPC_VERIFY_REJECTED, message,
                                          n0.submitbridgecarrier, output["payload_hex"])
        # Control: the same update with 342 signers passes the same check. It
        # is built without broadcasting, so the store stays at c + 90.
        result = n0.submitbridgecarrier(outputs[-1]["payload_hex"], {"broadcast": False})
        assert_equal((result["record_kind"], result["broadcast"]), ("update", False))
        assert result["txid"] not in n0.getrawmempool()
        for node in self.bridge_nodes():
            assert_equal(self.light_client_info(node), before)
        self.evidence.assertion("invalid light-client updates refused by B3 prevalidation",
                                expected={label: message for label, _, message in cases},
                                actual=observed,
                                control={"case": "342 of 512 at c + 91 (not broadcast)",
                                         "record_kind": result["record_kind"],
                                         "txid": result["txid"]})

    def make_finalized_deposit_d2(self):
        """D2 at c + 93, finalized on B3 by a mined update; its mint is planned
        by b3-bridge-ethcheck and never broadcast."""
        c, anvil, n0 = self.checkpoint_block, self.anvil, self.nodes[0]
        self.log.info(f"anvil: deposit D2 of {D2_RAW} raw units at c + {D2_OFFSET}; "
                      "B3 mines an update finalizing it")
        # check_rejected_updates() left anvil at c + 92.
        assert_equal(anvil.block_number(), c + FINALIZED_OFFSET + 2)
        receipt = anvil.send(self.depositor, self.vault, abi_call(
            "deposit(uint256,bytes32)", enc_uint(D2_RAW), recipient_v1(self.recipient)))
        self.evidence.gas("B3StakerBridge.deposit (D2)", gas_used(receipt))
        k = int(receipt["blockNumber"], 16)
        assert_equal(k, c + D2_OFFSET)
        anvil.mine(1)  # The update's attested block k + 1.
        deposits = find_deposits(anvil, self.vault, self.token, k, k)
        assert_equal(len(deposits), 1)
        d2 = deposits[0]
        assert_equal((d2["deposit_id"], d2["block_number"], d2["amount"]), (D2_ID, k, D2_RAW))
        assert_equal(self.beacon.period_of_block(k + 1), self.period)

        update = self.beacon.update(k)
        assert_equal((update["finalized_block"], update["period"], update["participants"]),
                     (k, self.period, SYNC_COMMITTEE_SIZE))
        _, update_height = self.submit_and_mine("update finalizing D2 mined",
                                                lambda: n0.submitbridgecarrier(update["payload_hex"]))
        # getbridgelightclientstore exports only B3-finalized connections.
        self.resume()
        self.wait_for_finalized(update_height)
        self.freeze()
        self.lc_finalized_block = k
        for node in self.bridge_nodes():
            info = node.getbridgeinfo()
            assert_equal((info["finalized_execution_block"], "0x" + info["finalized_execution_hash"]),
                         (k, d2["block_hash"]))
            assert_equal(node.getbridgeproofstatus(d2["block_hash"], D2_ID)["deposit"]["claimed"], False)

        # The plan is checked from the pinned bootstrap: the period P - 1
        # update carrying committee P, then the update B3 just mined.
        plan = build_direct_mint_plan(os.path.join(self.options.tmpdir, "d2_mint_plan"), self.beacon,
                                      anvil.url, d2, self.registry_id,
                                      [self.beacon.period_updates()[0], update])
        assert_equal(plan["updates"][-1]["payload_hex"], update["payload_hex"])
        mint = plan["mint"]
        assert_equal((mint["deposit_id"], mint["amount"], mint["source_anchor_hash"]),
                     (D2_ID, D2_RAW, d2["block_hash"]))
        d2["mint_payload_hex"] = mint["payload_hex"]
        return d2

    def check_stale_head_and_replay(self):
        self.log.info("D2's mint is fresh exactly 8,192 slots behind the light-client head and stale "
                      "one second later; the D1 mint replay is nullified")
        n0, d2 = self.nodes[0], self.d2
        info = n0.getbridgeinfo()
        assert_equal(info["finalized_execution_block"], d2["block_number"])
        anchor = n0.getbridgeproofstatus("0x" + info["finalized_execution_hash"])["anchor"]
        finalized_time = anchor["execution_timestamp"]
        assert_equal(finalized_time, int(self.anvil.block(d2["block_number"])["timestamp"], 16))
        assert_greater_than(finalized_time + STALE_AFTER_SECONDS, int(time.time()))
        # Frozen since D2's update: the tip and the light-client head stay put.
        n0.setmocktime(finalized_time + STALE_AFTER_SECONDS)
        fresh = n0.claimbridgedeposit(d2["mint_payload_hex"], D2_RAW, self.recipient,
                                      {"broadcast": False})
        assert_equal((fresh["broadcast"], fresh["amount"], fresh["owner_address"]),
                     (False, D2_RAW, self.recipient))
        n0.setmocktime(finalized_time + STALE_AFTER_SECONDS + 1)
        stale = rpc_refusal(RPC_VERIFY_REJECTED, "bridge mint light-client head is stale",
                            n0.claimbridgedeposit, d2["mint_payload_hex"], D2_RAW, self.recipient)
        self.evidence.jump("n0 mocktime", STALE_AFTER_SECONDS + 1, before=finalized_time,
                           after=finalized_time + STALE_AFTER_SECONDS + 1, result=stale)
        n0.setmocktime(0)
        assert fresh["txid"] not in n0.getrawmempool()

        mints = [job for job in self.relayer_jobs() if job["kind"] == "mint"]
        payload, address = mints[0]["payload"], mints[0]["address"]
        nullified = "bridge deposit is already nullified"
        replays = {
            "D1 mint": rpc_refusal(RPC_VERIFY_REJECTED, nullified,
                                   n0.claimbridgedeposit, payload, D1_RAW, address),
            "D1 mint amount + 1": rpc_refusal(RPC_VERIFY_REJECTED, nullified,
                                              n0.claimbridgedeposit, payload, D1_RAW + 1, address),
        }
        for node in self.bridge_nodes():
            assert_equal(node.getbridgeinfo()["nullifiers"], 1)
            assert_equal(node.getbridgeproofstatus(d2["block_hash"], D2_ID)["deposit"]["claimed"], False)
        self.evidence.assertion("an unclaimed mint is fresh at 8,192 slots behind and stale one second later",
                                expected={"at +8192 slots": "authorized (not broadcast)",
                                          "at +8192 slots + 1 s": "bridge mint light-client head is stale"},
                                actual={"at +8192 slots": {"txid": fresh["txid"], "broadcast": fresh["broadcast"]},
                                        "at +8192 slots + 1 s": stale},
                                deposit_id=D2_ID, finalized_execution_timestamp=finalized_time,
                                fresh_through=finalized_time + STALE_AFTER_SECONDS)
        self.evidence.assertion("the D1 mint replay is refused", expected=nullified, actual=replays)

    # -- reindex and the pin-file record ------------------------------------
    def light_client_views(self):
        """Assert every running node exports the same bridge light-client state."""
        views = []
        for node in self.bridge_nodes():
            info = node.getbridgeinfo()
            assert_equal(info["state_available"], True)
            views.append({
                "tip": node.getbestblockhash(),
                "finalized": node.getfinalitystatus()["finalized"],
                "info": {key: info.get(key) for key in LIGHT_CLIENT_FIELDS},
                "store": node.getbridgelightclientstore(),
                "d1": node.getbridgeproofstatus(self.d1["block_hash"], D1_ID),
                "d2": node.getbridgeproofstatus(self.d2["block_hash"], D2_ID),
                "finalized_anchor": node.getbridgeproofstatus("0x" + info["finalized_execution_hash"]),
            })
        for view in views[1:]:
            assert_equal(view, views[0])
        view = views[0]
        assert_equal(view["info"]["light_client_period"], self.period)
        assert_equal(view["info"]["finalized_execution_block"], self.lc_finalized_block)
        assert_equal(view["info"]["nullifiers"], 1)
        assert_equal(view["d1"]["deposit"]["claimed"], True)
        assert_equal(view["d2"]["deposit"]["claimed"], False)
        assert_equal(view["finalized_anchor"]["anchor"]["known"], True)
        assert_equal(view["store"]["connection"]["height"], view["info"]["light_client_connected_height"])
        return view

    def check_reindex(self):
        self.log.info("n3 reindexes while the others stake; every node then agrees")
        n0, n3 = self.nodes[0], self.nodes[3]
        self.resume()
        height = n0.getblockcount()
        self.restart_node(3, extra_args=b3_bridge_args(self.pins_path) + ["-reindex"])
        self.connect_nodes(3, 2)
        self.wait_until(lambda: n3.getblockcount() >= height + 3, timeout=180)
        self.freeze()
        view = self.light_client_views()
        self.evidence_step("n3 reindexed and agrees", self.anvil,
                           tip=view["tip"], finalized=view["finalized"],
                           light_client=view["info"])

    def check_pin_guard(self):
        self.log.info("The pin-file record refuses a changed file after activation and on any reindex")
        n3 = self.nodes[3]
        self.altered_path = altered_path = os.path.join(self.options.tmpdir, "b3bridgepins-altered-caps.json")
        write_regtest_bridge_pins(altered_path, self.anvil, self.vault, self.token, self.verifier,
                                  self.deployment_block, self.bootstrap,
                                  caps={"max_per_epoch": 2 * DEFAULT_MAX_PER_EPOCH})
        pins_sha256 = file_sha256(self.pins_path)
        assert file_sha256(altered_path) != pins_sha256
        height, tip = n3.getblockcount(), n3.getbestblockhash()
        assert_greater_than_or_equal(height, BRIDGE_ACTIVATION_HEIGHT)
        record = os.path.join(n3.chain_path, PINS_RECORD_FILENAME)
        self.stop_node(3)
        refusals = {"altered caps": start_refused(n3, b3_bridge_args(altered_path), NODE_PINS_CHANGED_ERROR)}
        for flag in ("-reindex", "-reindex-chainstate"):
            refusals[f"altered caps {flag}"] = start_refused(
                n3, b3_bridge_args(altered_path) + [flag], NODE_PINS_REINDEX_ERROR)
        assert_equal(read_record(record), pins_sha256)

        self.log.info("A missing record means no pin file: the pinned datadir is refused, also on reindex")
        os.remove(record)
        refusals["record deleted"] = start_refused(n3, b3_bridge_args(self.pins_path), NODE_PINS_CHANGED_ERROR)
        refusals["record deleted -reindex"] = start_refused(
            n3, b3_bridge_args(self.pins_path) + ["-reindex"], NODE_PINS_REINDEX_ERROR)
        assert not os.path.exists(record)
        with open(record, "w", encoding="utf8") as f:
            f.write(pins_sha256 + "\n")

        self.log.info("Nothing was wiped: the original pin file restarts n3 at the same tip")
        self.start_node(3, extra_args=b3_bridge_args(self.pins_path))
        assert_equal((n3.getblockcount(), n3.getbestblockhash()), (height, tip))
        assert_equal(read_record(record), pins_sha256)
        self.connect_nodes(3, 2)
        self.sync_bridge_blocks()
        self.light_client_views()
        self.resume()
        height = self.nodes[0].getblockcount()
        self.wait_until(lambda: all(node.getblockcount() >= height + 3
                                    for node in self.bridge_nodes()), timeout=180)
        self.freeze()
        view = self.light_client_views()
        self.evidence.assertion("changed pin file refused after activation and on reindex; original accepted",
                                expected={"altered caps": NODE_PINS_CHANGED_ERROR,
                                          "altered caps -reindex": NODE_PINS_REINDEX_ERROR,
                                          "altered caps -reindex-chainstate": NODE_PINS_REINDEX_ERROR,
                                          "record deleted": NODE_PINS_CHANGED_ERROR,
                                          "record deleted -reindex": NODE_PINS_REINDEX_ERROR},
                                actual=refusals, record_after=read_record(record),
                                original_sha256=pins_sha256, altered_sha256=file_sha256(altered_path))
        self.evidence_step("pin guard checked", self.anvil, tip=view["tip"])

    def check_pin_change_below_activation(self):
        self.log.info("n4: a changed pin file is recorded while no block at or above activation is stored")
        n0, n4 = self.nodes[0], self.nodes[4]
        record = os.path.join(n4.chain_path, PINS_RECORD_FILENAME)
        original, altered = file_sha256(self.pins_path), file_sha256(self.altered_path)
        recorded = []
        for path in (self.pins_path, self.altered_path):
            self.start_node(4, extra_args=b3_bridge_args(path))
            assert_equal(n4.getblockcount(), 0)
            self.stop_node(4)
            recorded.append(read_record(record))
        assert_equal(recorded, [original, altered])
        reindex_refusal = start_refused(n4, b3_bridge_args(self.pins_path) + ["-reindex-chainstate"],
                                        NODE_PINS_REINDEX_ERROR)
        assert_equal(read_record(record), altered)

        stop_at = BRIDGE_ACTIVATION_HEIGHT - 1
        self.log.info(f"n4: syncs under the original file and stops at A3 - 1 = {stop_at}")
        self.start_node(4, extra_args=b3_bridge_args(self.pins_path) + [f"-stopatheight={stop_at}"])
        # n3 dials n4, so no RPC reaches n4 while it syncs and stops by itself.
        self.nodes[3].addnode(f"127.0.0.1:{p2p_port(4)}", "onetry")
        n4.wait_until_stopped(timeout=180)
        assert_equal(read_record(record), original)

        self.log.info("n4: with blocks at or above activation stored, a changed file is refused at tip A3 - 1")
        with n4.assert_debug_log([f"hashBestChain={n0.getblockhash(stop_at)} height={stop_at} "]):
            stored_refusal = start_refused(n4, b3_bridge_args(self.altered_path), NODE_PINS_CHANGED_ERROR)
        assert_equal(read_record(record), original)
        self.start_node(4, extra_args=b3_bridge_args(self.pins_path))
        activation_block = n0.getblockhash(BRIDGE_ACTIVATION_HEIGHT)
        assert_equal(n4.getblockheader(activation_block)["height"], BRIDGE_ACTIVATION_HEIGHT)
        self.stop_node(4)
        self.evidence.assertion("pin change accepted only while no block at or above activation is stored",
                                expected={"records": [original, altered],
                                          "-reindex-chainstate": NODE_PINS_REINDEX_ERROR,
                                          f"stored blocks, tip {stop_at}": NODE_PINS_CHANGED_ERROR},
                                actual={"records": recorded, "-reindex-chainstate": reindex_refusal,
                                        f"stored blocks, tip {stop_at}": stored_refusal},
                                record_after=read_record(record))

if __name__ == '__main__':
    B3BridgeRT2LightClientTest(__file__).main()
