#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""RT1: H1 producer rotation and the H2 hardened checkpoint (bridge V2 S1).

REGTEST ONLY. Every process is local: up to eight B3 regtest nodes linked
only to each other. No anvil, no Ethereum endpoint and no external peer is
used. The knobs are the regtest-only -b3h1height=220 (R_h),
-b3h1escaperounds=60 (R_escape) and -b3h2checkpoint (finality dependency
section 5, FD-1..FD-4; regtest plan section 3.6 fixtures B and I). E is the
40-block -b3epochlength; M = 131.

Roles: n0-n3 validators (stakes 40/30/20/10), n4 new staker A (15), n5 new
staker B (25), n6 the late upgrader (fixture B), n7 a fresh sync node that
also carries the H2 pin (fixture B).

--fixture=B (broken V1 lineage, H1 from 220; mock time throughout):

1. No V1 certificate is ever formed: before any node first stakes, its V1
   finality journal path is made a regular file, so every staking loop
   keeps producing with finality signing "disabled safely" and never votes
   (in place of the plan's withholding knob, see below). The signers stay
   silent for the whole fixture: once checkpoints are signable the signer
   also refuses to recreate an absent journal (anti-equivocation guard), so
   the file is simply left in place. The producers n0-n3 (and n6) run with
   -b3maxepochextension=1000 until 219 so the regtest local production
   policy does not pause them; n4 and n5 run the fixture's 40 and see the
   formal V1 break at 211 = 131 + 40 + 40.
2. At 212 the V1 set is frozen at Set_0 (4 members, W 100), V1 has no
   finalized checkpoint, and n4/n5 report lineage_broken: n4 (bound,
   ACTIVE 15) is excluded and its staking loop says "not in the active
   validator set"; n3, which spent its stake at ~150, still has weight 10
   and produces a block alone.
3. At 219 n0-n3 restart with extension 40: every node derives
   lineage_broken from block data, and from R_h the producer set is
   Snapshot(219) = {n0,n1,n2,n4} (W 105) with no certificate: n4 produces
   220 alone, n3 is refused. No node ever pools a V1 vote.
4. Rotation at 260 (ProducerSet_1 = the bootstrap set re-stamped; next =
   Snapshot(259) = {n0,n1,n4,n5}, W 110): n5 is still excluded and n2, which
   spent its stake at ~225, still produces 260 (residual). Rotation at 300:
   n5 produces 300 alone, n2 is out; n1 spent at ~270 so Snapshot(299) has
   three members and the next set is a carry-over (same members, epoch 3),
   while an escape block at 300 would take Snapshot(299) itself (three
   members, W 80: the escape has no minimum-set floor).
5. Escape (FD-2): n3 restakes at ~301 (its binding is still live); with
   every in-force producer stopped, n3 alone produces h_e in round >= 60 from
   the escape set Snapshot(h_e - 1) = {n0,n3,n4,n5} (W 90); the schedule
   restarts at h_e and the next rotation is exactly h_e + 40, not 340; n1 is
   out. No other block is ever an escape.
6. Coinbase scan; restart of n0-n5 and -reindex of n0 reproduce the V1 and
   producer views; n7 syncs from genesis with -b3h2checkpoint=300:<hash>;
   with a wrong hash n7 refuses to start, with the wrong hash and -reindex
   it stops at 299, with the right hash and -reindex it is back at the tip.
7. Late upgrader (finality section 5.4/5.5 item 5): n6, stopped at 219 and
   never given H1, restores n3's wallet and produces its own 220..265 under
   the frozen V1 set; restarted with the H1 knobs it refuses n0's branch
   (modern-reorg-too-deep, horizon 40) and keeps its tip; restarted with
   -reindex its own block 220 fails (bad-pos-ineligible) and it then syncs
   to n0 and agrees.

--fixture=I (intact V1 lineage, H1 from 220; live BLS in real time):

1. bring_up_validators; n4 is funded at ~152.
2. Mock time from ~186: n4 stakes 15 and binds in exactly block 198, so its
   stake is ACTIVE from 218: in Snapshot(219) (H1's ProducerSet_0) but not
   in V1's snapshot for the set in force at 220 (V1 epoch 2 starts at or
   before 218, so that snapshot is at most 217; n4 enters V1 only with
   Set_4).
3. Frozen at 218: producers are inactive and follow the V1 set in force;
   n4 has no weight; V1 has rotated and is not broken.
4. At 219 the producer set for 220 (5 members, W 115) differs from V1's set
   in force (4 members, W 100, without n4): n4 produces 220 alone, a block
   only the H1 schedule admits.
5. Real time to >= 301 with V1 finalized above 290: producer epochs start
   at 220 + 40k, no escape, n4 has weight and has produced since R_h, V1's
   epoch advanced after R_h and the lineage is intact.
6. Restart, -reindex of n0 and a fresh n7 sync agree.

Fixture B deviation from regtest plan section 3.6 (S1) (reported,
solve-and-report): the plan breaks V1 with
-b3regtestwithholdcertificates=131-211 (its sections 3.6 and 7). This build
does not provide that knob: its CChainParams accessor and miner hunk were
refused by a permission check in S1, and an accepted but inert knob was
removed, so the node refuses it as an unknown parameter. The silent-signer
journal file plus the stalled extension above produce the same chain (no
certificate, frozen Set_0, the break at 211 derived from block data)
without any source change. Two consequences:
- It cannot show the plan's "every signer live while broken": with live
  signers the regtest policy (REQUIRE_HANDOVER at 210, below R_h) would
  include the handover certificate, so that drill needs the knob wired.
- -b3maxepochextension differs between nodes (1000 on n0-n3 and n6, 40 on
  n4/n5) and changes on n0-n3 at their restart at 219, against the plan's
  replay rule (section 3.3: no parameter may change how a connected block
  is judged). It is harmless here only because no certificate and no burn
  ever exist: the extension then decides nothing but when the V1 lineage
  counts as broken and the local regtest production pause, and every node
  derives the same break at 211 after the restart (checked).
"""

from decimal import Decimal
import os
import time

from test_framework.b3bridge import (
    B3_BRIDGE_ARGS,
    CORRIDOR_END,
    EPOCH_LENGTH,
    MODERN_START,
    VALIDATOR_COUNT,
    VALIDATOR_STAKES,
    B3BridgeChainMixin,
    assert_regtest_genesis,
    start_refused,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
)

# -- schedule (regtest plan section 3.6) --------------------------------------
R_H = 220  # -b3h1height
ESCAPE_ROUNDS = 60  # -b3h1escaperounds
REORG_HORIZON = 40  # -b3reorghorizon (fixture B)
BLOCK_INTERVAL = 1  # -b3blockinterval in B3_BRIDGE_ARGS
ROUND_SECONDS = 1  # -b3roundseconds in B3_BRIDGE_ARGS
MIN_FINALITY_SET = VALIDATOR_COUNT  # -b3minfinalityset in B3_BRIDGE_ARGS
V1_EXTENSION = EPOCH_LENGTH  # -b3maxepochextension in B3_BRIDGE_ARGS
V1_BREAK = MODERN_START + EPOCH_LENGTH + V1_EXTENSION  # 211
STALL_EXTENSION = 1000  # fixture B producers until 219 (and n6 on its own branch)
STAKE_ACTIVATION_DEPTH = 20  # src/modern/stake.h
H2_HEIGHT = 300
LATE_TIP = R_H + REORG_HORIZON + 5  # n6's own branch 220..265
# Fixture I: n4's stake block. Its stake is ACTIVE from 218, inside
# Snapshot(219) (H1's ProducerSet_0) and after V1's epoch-2 snapshot.
I_STAKE_HEIGHT = R_H - 2 - STAKE_ACTIVATION_DEPTH
assert f"-b3blockinterval={BLOCK_INTERVAL}" in B3_BRIDGE_ARGS
assert f"-b3roundseconds={ROUND_SECONDS}" in B3_BRIDGE_ARGS
assert f"-b3maxepochextension={V1_EXTENSION}" in B3_BRIDGE_ARGS
assert f"-b3minfinalityset={MIN_FINALITY_SET}" in B3_BRIDGE_ARGS

H1_ARGS = [f"-b3h1height={R_H}", f"-b3h1escaperounds={ESCAPE_ROUNDS}"]
HORIZON_ARGS = [f"-b3reorghorizon={REORG_HORIZON}"]

# Roles.
VALIDATORS = (0, 1, 2, 3)
STAKER_A = 4
STAKER_B = 5
LATE = 6
FRESH = 7
STAKE_A = 15
STAKE_B = 25
FUNDING = 60
UNSTAKE_FEE = Decimal("0.001")

# Messages (src/node/staking.cpp, src/node/chainstate.cpp, src/validation.cpp).
NOT_IN_SET = "not in the active validator set"
SIGNER_SILENT = "finality signing disabled safely: cannot create finality signer directory"
H2_MISMATCH = f"does not match the hardened modern checkpoint at height {H2_HEIGHT}"

# getfinalitystatus fields every node must agree on (the signature pool and
# the persisted pin are node-local and left out).
V1_KEYS = ("configured", "active", "bootstrapped", "epoch", "epoch_start", "handover_certified",
           "lineage_broken", "validator_set", "finalized")


def with_extension(args, extension):
    """args with -b3maxepochextension replaced by `extension`."""
    out = [arg for arg in args if not arg.startswith("-b3maxepochextension=")]
    out.append(f"-b3maxepochextension={extension}")
    return out


def chain_view(node):
    status = node.getfinalitystatus()
    view = {key: status.get(key) for key in V1_KEYS}
    view["producers"] = status.get("producers")
    view["tip"] = node.getbestblockhash()
    return view


def coinbase_key(node, height):
    """The validator key declared in a modern-PoS coinbase (BIP34 push, then
    a direct 32-byte push; src/modern/pos_v1.h)."""
    block = node.getblock(node.getblockhash(height), 2)
    script = bytes.fromhex(block["tx"][0]["vin"][0]["coinbase"])
    op = script[0]
    if 1 <= op <= 75:
        pos = 1 + op
    elif op == 0 or 0x51 <= op <= 0x60:
        pos = 1
    else:
        raise AssertionError(f"unexpected BIP34 push at height {height}: {script.hex()}")
    assert_equal(script[pos], 32)
    return script[pos + 1:pos + 33].hex()


def block_round(node, height):
    """The consensus round of a block: exact timestamps (src/modern/pos_v1.h)."""
    header = node.getblockheader(node.getblockhash(height))
    parent = node.getblockheader(header["previousblockhash"])
    offset = header["time"] - parent["time"] - BLOCK_INTERVAL
    assert_equal(offset % ROUND_SECONDS, 0)
    return offset // ROUND_SECONDS


def assert_set(producer_set, *, epoch, size, weight):
    assert_equal((producer_set["epoch"], producer_set["size"], producer_set["total_weight"]),
                 (epoch, size, weight))


class B3H1ProducerRotationTest(B3BridgeChainMixin, BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_argument("--fixture", dest="fixture", default="B", choices=("B", "I"),
                            help="B: broken V1 lineage with H1 (default); I: intact V1 lineage with H1")

    def set_test_params(self):
        self.num_nodes = 8
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        # The fixture starts each node with its own arguments.
        self.add_nodes(self.num_nodes, [list(B3_BRIDGE_ARGS) for _ in range(self.num_nodes)])

    def run_test(self):
        self.pos_running = False
        self.mock_time = 0
        self.keys = {}
        if self.options.fixture == "B":
            self.run_fixture_b()
        else:
            self.run_fixture_i()

    # -- node helpers -------------------------------------------------------
    def run_args(self, args, *extra, mocktime=None):
        clock = self.mock_time if mocktime is None else mocktime
        out = list(args) + list(extra)
        if clock:
            out.append(f"-mocktime={clock}")
        return out

    def start_b3(self, i, args, *extra, mocktime=None):
        clock = self.mock_time if mocktime is None else mocktime
        self.start_node(i, extra_args=self.run_args(args, *extra, mocktime=clock))
        if clock:
            self.nodes[i].setmocktime(clock)

    def signer_path(self, node):
        return node.chain_path / "finality_signer"

    def silence_signer(self, node):
        """Make the V1 journal path a regular file before the node ever
        stakes: FinalitySignerStore::Open refuses it, so the staking loop
        produces with finality signing disabled safely and never votes."""
        path = self.signer_path(node)
        assert not path.exists(), f"{path} already exists"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("RT1 fixture B: V1 finality signing held off\n", encoding="utf8")

    def key_of(self, i):
        if i not in self.keys:
            self.keys[i] = self.nodes[i].getstakinginfo()["validator_key"]
        return self.keys[i]

    def staking_state(self, node, phrase):
        """Start the staking loop, wait until its state contains `phrase`, stop."""
        node.startstaking()
        try:
            self.wait_until(lambda: phrase in node.getstakinginfo()["staking"]["state"], timeout=30)
        finally:
            node.stopstaking()

    def assert_signer_silent(self, node):
        node.startstaking()
        try:
            self.wait_until(lambda: SIGNER_SILENT in node.getstakinginfo()["staking"].get("last_error", ""),
                            timeout=30)
            assert_equal(node.getstakinginfo()["staking"]["finality_signing"], False)
        finally:
            node.stopstaking()

    def sync_tips(self, nodes, timeout=60):
        self.wait_until(lambda: len({node.getbestblockhash() for node in nodes}) == 1, timeout=timeout)

    def assert_agree(self, nodes, *, wallets=True):
        """Equal V1 fields, producer schedule and tip on `nodes`; every
        wallet's total_active_weight is the producer set's W."""
        views = [chain_view(node) for node in nodes]
        for node, view in zip(nodes, views):
            if view != views[0]:
                raise AssertionError(f"node{node.index} disagrees with node{nodes[0].index}:\n"
                                     f"{view}\n!=\n{views[0]}")
        producers = views[0]["producers"]
        if wallets and producers is not None:
            for node in nodes:
                if node.listwallets():
                    assert_equal(node.getstakinginfo()["total_active_weight"], producers["set"]["total_weight"])
        return views[0]

    # -- mock-time production -------------------------------------------------
    def produce_to(self, target, stakers, *, nodes=None, clock=None, allow_escape=False, first_round=0,
                   timeout=600):
        """Modern PoS under mock time until stakers[0]'s tip is exactly `target`.

        Every staker runs (finality signing is not asserted). The clock steps
        one round at a time from round `first_round` of the next block and
        every node in `nodes` is synced after each block, so no staker builds
        on a stale tip (regtest has one proposer slot per round). On the main
        chain (no `clock`) a block at or above R_h must stay below R_escape
        unless allow_escape; the clock never steps into an escape round
        then.
        """
        ref = stakers[0]
        nodes = list(nodes) if nodes is not None else self.bridge_nodes()
        main_chain = clock is None
        get_time, set_time = clock if clock is not None else (lambda: self.mock_time, self.set_chain_time)
        assert_greater_than_or_equal(target, ref.getblockcount())
        if ref.getblockcount() == target:
            return
        for node in stakers:
            assert_equal(node.startstaking()["running"], True)
        try:
            deadline = time.time() + timeout
            while ref.getblockcount() < target:
                if time.time() > deadline:
                    raise AssertionError(f"no block {ref.getblockcount() + 1} within {timeout}s")
                height = ref.getblockcount()
                round0 = ref.getblockheader(ref.getbestblockhash())["time"] + BLOCK_INTERVAL
                next_time = max(get_time() + ROUND_SECONDS, round0 + first_round * ROUND_SECONDS)
                if (main_chain and not allow_escape and height + 1 >= R_H and
                        next_time - round0 >= ESCAPE_ROUNDS * ROUND_SECONDS):
                    raise AssertionError(f"no staker produced block {height + 1} in rounds 0..{ESCAPE_ROUNDS - 1}; "
                                         "refusing to step into an escape round")
                set_time(next_time)
                if not self.wait_round(stakers, height, (next_time - round0) // ROUND_SECONDS):
                    continue
                self.sync_tips(nodes)
                assert_equal(ref.getblockcount(), height + 1)
                if main_chain and height + 1 >= R_H and not allow_escape:
                    assert_greater_than(ESCAPE_ROUNDS, block_round(ref, height + 1))
        finally:
            for node in stakers:
                if node.running:
                    node.stopstaking()
        self.sync_tips(nodes)

    @staticmethod
    def round_passed(state, rnd, height):
        """The staking loop state says it has no slot in round rnd at height
        (src/node/staking.cpp), or it is not in any set at all."""
        if NOT_IN_SET in state:
            return True
        return (f"recovery round {rnd} at height {height};" in state and
                ("no proposer slot" in state or "window expired" in state))

    def wait_round(self, stakers, height, rnd, timeout=3):
        """True once a staker produced block height + 1; False once every
        staker has passed round rnd without a slot, or after timeout (e.g.
        a local production pause), so the caller steps to the next round."""
        end = time.time() + timeout
        while time.time() < end:
            time.sleep(0.05)
            if any(node.getblockcount() > height for node in stakers):
                return True
            if all(self.round_passed(node.getstakinginfo()["staking"]["state"], rnd, height + 1)
                   for node in stakers):
                return False
        return any(node.getblockcount() > height for node in stakers)

    def solo_block(self, i):
        """Exactly one block with only node i staking; its coinbase declares i's key."""
        node = self.nodes[i]
        height = node.getblockcount() + 1
        self.produce_to(height, [node])
        assert_equal(coinbase_key(node, height), self.key_of(i))
        return height

    def mine_txids_mock(self, txids, stakers, max_blocks=3):
        """Share the wallets' transactions with every running node, then
        produce until all of `txids` are mined (normally in one block)."""
        self.synchronize_mempools()
        first = stakers[0].getblockcount() + 1
        for _ in range(max_blocks):
            self.produce_to(stakers[0].getblockcount() + 1, stakers)
            heights = [self.mined_height(stakers[0], txid, first) for txid in txids]
            if all(height is not None for height in heights):
                return heights
        raise AssertionError(f"transactions {txids} not mined within {max_blocks} blocks")

    def unstake(self, node):
        """Spend the wallet's one STAKE output (an ordinary signed spend)."""
        stakes = node.getstakinginfo()["stakes"]
        assert_equal(len(stakes), 1)
        stake = stakes[0]
        assert_equal(stake["status"], "ACTIVE")
        raw = node.createrawtransaction([{"txid": stake["txid"], "vout": stake["vout"]}],
                                        [{node.getnewaddress(): stake["amount"] - UNSTAKE_FEE}])
        signed = node.signrawtransactionwithwallet(raw)
        assert_equal(signed["complete"], True)
        return node.sendrawtransaction(signed["hex"])

    # =========================================================== fixture B
    def run_fixture_b(self):
        n = self.nodes
        self.main_args = list(B3_BRIDGE_ARGS) + H1_ARGS + HORIZON_ARGS
        stall_args = with_extension(self.main_args, STALL_EXTENSION)
        # The late upgrader never gets H1 before it upgrades.
        late_args = with_extension(list(B3_BRIDGE_ARGS) + HORIZON_ARGS, STALL_EXTENSION)
        validators = [n[i] for i in VALIDATORS]
        stakers = [n[i] for i in range(6)]

        self.log.info("Fixture B: n0-n6 with silent V1 signers; producers n0-n3 stall-extended until 219")
        for i in range(LATE + 1):
            self.silence_signer(n[i])
        self.set_chain_time(int(time.time()) - 1000)
        for i in VALIDATORS:
            self.start_b3(i, stall_args)
        for i in (STAKER_A, STAKER_B):
            self.start_b3(i, self.main_args)
        self.start_b3(LATE, late_args)
        for i in range(6):
            self.init_wallet(node=i)
        for a, b in ((1, 0), (2, 1), (3, 2), (4, 0), (5, 0), (6, 0)):
            self.connect_nodes(a, b)
        for i in range(LATE + 1):
            assert_regtest_genesis(n[i])

        self.log.info("Corridor 1-130: funding, four stakes with finality keys")
        n0 = n[0]
        self.mine_corridor(101)
        for i in (1, 2, 3):
            n0.sendtoaddress(n[i].get_deterministic_priv_key().address, 120)
        for i in (STAKER_A, STAKER_B):
            n0.sendtoaddress(n[i].get_deterministic_priv_key().address, FUNDING)
        self.synchronize_mempools()
        self.mine_corridor(1)
        for node, amount in zip(validators, VALIDATOR_STAKES):
            node.createstake(amount)
            node.bindfinalitykey()
        self.synchronize_mempools()
        self.mine_corridor(1)
        self.mine_corridor(CORRIDOR_END - n0.getblockcount())
        assert_equal(n0.getblockcount(), CORRIDOR_END)
        for node in validators:
            preview = node.getfinalitystatus()["set0_preview"]
            assert_equal((preview["ready"], preview["size"]), (True, VALIDATOR_COUNT))

        self.log.info("M=131: n0-n3 produce; every V1 signer is silent")
        self.produce_to(MODERN_START + 4, validators)
        for node in validators:
            self.assert_signer_silent(node)
        self.produce_to(140, validators)

        self.log.info("~140: n4 stakes 15 and binds; ~150: n3 spends its stake (wallet backed up)")
        self.mine_txids_mock([n[STAKER_A].createstake(STAKE_A)["txid"], n[STAKER_A].bindfinalitykey()["txid"]],
                             validators)
        self.produce_to(150, validators)
        unstake_n3 = self.unstake(n[3])
        self.mine_txids_mock([unstake_n3], validators)
        self.n3_backup = os.path.join(self.options.tmpdir, "n3_wallet.bak")
        n[3].backupwallet(self.n3_backup)
        self.produce_to(212, validators)
        self.check_frozen_v1(n)

        self.log.info("Produce to 219 under the frozen V1 set; stop n6")
        self.produce_to(R_H - 1, stakers)
        assert_equal(n0.getfinalitystatus()["pool_checkpoints"], 0)
        self.stop_node(LATE)

        self.log.info("219: n0-n3 restart with extension 40: the break at 211 is derived on every node")
        for i in VALIDATORS:
            self.stop_node(i)
        for i in VALIDATORS:
            self.start_b3(i, self.main_args)
        for a, b in ((1, 0), (2, 1), (3, 2), (4, 0), (5, 0)):
            self.connect_nodes(a, b)
        self.check_h1_bootstrap(n)

        self.log.info("~222: n5 stakes 25 and binds; ~225: n2 spends its stake")
        self.produce_to(222, stakers)
        self.mine_txids_mock([n[STAKER_B].createstake(STAKE_B)["txid"], n[STAKER_B].bindfinalitykey()["txid"]],
                             stakers)
        self.produce_to(225, stakers)
        unstake_n2 = self.unstake(n[2])
        self.mine_txids_mock([unstake_n2], stakers)
        self.produce_to(R_H + EPOCH_LENGTH - 1, stakers)
        self.check_rotation_260(n)

        self.log.info("~270: n1 spends its stake, so Snapshot(299) is below the minimum set")
        self.produce_to(270, stakers)
        unstake_n1 = self.unstake(n[1])
        self.mine_txids_mock([unstake_n1], stakers)
        self.produce_to(R_H + 2 * EPOCH_LENGTH - 1, stakers)
        self.check_rotation_300(n)
        self.hash300 = n0.getblockhash(H2_HEIGHT)

        self.log.info("~301: n3 restakes 10 with its live binding")
        restake_n3 = n[3].createstake(10)["txid"]
        self.mine_txids_mock([restake_n3], stakers)
        created = n[3].getstakinginfo()["stakes"][0]["created_height"]
        self.produce_to(created + STAKE_ACTIVATION_DEPTH, stakers)
        h_e = self.check_escape(n)

        self.log.info("Resume every staker: the schedule rotates at h_e + 40, not at 340")
        self.produce_to(R_H + 3 * EPOCH_LENGTH - 1, stakers)
        p = self.assert_agree(stakers)["producers"]
        assert_equal((p["epoch"], p["epoch_start"], p["next_rotation"]), (3, h_e, h_e + EPOCH_LENGTH))
        self.produce_to(h_e + EPOCH_LENGTH - 2, stakers)
        p = self.assert_agree(stakers)["producers"]
        assert_equal((p["epoch"], p["epoch_start"]), (3, h_e))
        self.produce_to(h_e + EPOCH_LENGTH - 1, stakers)
        p = self.assert_agree(stakers)["producers"]
        assert_equal((p["epoch"], p["epoch_start"], p["next_rotation"]), (4, h_e + EPOCH_LENGTH, h_e + 2 * EPOCH_LENGTH))
        assert_set(p["set"], epoch=4, size=4, weight=90)
        assert_equal((p["last_escape_height"], p["escapes"]), (h_e, 1))
        # V1 never moved: still broken in epoch 0, no vote pooled anywhere.
        for node in stakers:
            status = node.getfinalitystatus()
            assert_equal((status["lineage_broken"], status["epoch"], status.get("finalized"), status["pool_checkpoints"]),
                         (True, 0, None, 0))
        self.check_coinbase_scan_b(h_e)

        view = self.check_restart_reindex_b()
        self.check_h2(view)
        self.check_late_upgrader(view)

    def check_frozen_v1(self, n):
        self.log.info("212: V1 frozen at Set_0 with no certificate; n4/n5 see the break at 211")
        views = {i: chain_view(n[i]) for i in range(LATE + 1)}
        for i, view in views.items():
            for key in V1_KEYS:
                if key == "lineage_broken":
                    continue
                assert_equal(view[key], views[0][key])
            assert_equal(view["tip"], views[0]["tip"])
            # n0-n3 and n6 run the stalled extension; n4 and n5 the fixture's 40.
            assert_equal(view["lineage_broken"], i in (STAKER_A, STAKER_B))
        v1 = views[0]
        assert_equal((v1["bootstrapped"], v1["epoch"], v1["epoch_start"], v1["handover_certified"]),
                     (True, 0, MODERN_START, False))
        assert_equal(v1["finalized"], None)
        assert_equal((v1["validator_set"]["size"], v1["validator_set"]["total_weight"]), (4, 100))
        # The producer schedule is not active yet and equals V1's set.
        for i in range(6):
            assert_equal(views[i]["producers"], views[0]["producers"])
        assert_equal(views[LATE]["producers"], None)
        p = views[0]["producers"]
        assert_equal((p["activation_height"], p["escape_rounds"], p["active"]), (R_H, ESCAPE_ROUNDS, False))
        assert_equal((p["next_rotation"], p["snapshot_height"], p["stake_enters_at"]), (R_H, R_H - 1, R_H))
        for key in ("epoch", "epoch_start", "next_set", "escape_set", "last_escape_height", "escapes"):
            assert key not in p, key
        assert_set(p["set"], epoch=0, size=4, weight=100)
        assert_equal(p["set"]["set_hash"], v1["validator_set"]["set_hash"])
        for i in range(6):
            assert_equal(n[i].getstakinginfo()["total_active_weight"], 100)
        # n4: bound and ACTIVE, yet excluded.
        a = n[STAKER_A].getstakinginfo()
        assert_equal([stake["status"] for stake in a["stakes"]], ["ACTIVE"])
        assert_equal(a["active_weight"], 0)
        self.staking_state(n[STAKER_A], NOT_IN_SET)
        # n3: no stake, still weight 10, produces alone.
        info = n[3].getstakinginfo()
        assert_equal((info["stakes"], info["active_weight"]), ([], 10))
        height = self.solo_block(3)
        assert_greater_than(height, V1_BREAK)
        self.log.info(f"n3 (no stake) produced {height} under the frozen V1 set")

    def check_h1_bootstrap(self, n):
        stakers = [n[i] for i in range(6)]
        v = self.assert_agree(stakers)
        assert_equal((v["lineage_broken"], v["epoch"], v["epoch_start"], v["finalized"]),
                     (True, 0, MODERN_START, None))
        assert_equal((v["validator_set"]["size"], v["validator_set"]["total_weight"]), (4, 100))
        p = v["producers"]
        assert_equal((p["active"], p["epoch"], p["epoch_start"]), (True, 0, R_H))
        assert_equal((p["next_rotation"], p["snapshot_height"], p["stake_enters_at"]),
                     (R_H + EPOCH_LENGTH, R_H + EPOCH_LENGTH - 1, R_H + 2 * EPOCH_LENGTH))
        assert_equal((p["last_escape_height"], p["escapes"]), (-1, 0))
        assert_set(p["set"], epoch=0, size=4, weight=105)
        assert_set(p["next_set"], epoch=1, size=4, weight=105)
        assert_set(p["escape_set"], epoch=1, size=4, weight=105)
        self.next_set_hash = p["next_set"]["set_hash"]
        # n3 is out of Snapshot(219); n4 is in.
        assert_equal(n[3].getstakinginfo()["active_weight"], 0)
        self.staking_state(n[3], NOT_IN_SET)
        assert_equal(n[STAKER_A].getstakinginfo()["active_weight"], STAKE_A)
        self.log.info("220: n4 produces alone from ProducerSet_0 = Snapshot(219)")
        assert_equal(self.solo_block(STAKER_A), R_H)

    def check_rotation_260(self, n):
        stakers = [n[i] for i in range(6)]
        p = self.assert_agree(stakers)["producers"]
        assert_equal((p["epoch"], p["epoch_start"]), (1, R_H + EPOCH_LENGTH))
        assert_set(p["set"], epoch=1, size=4, weight=105)
        assert_equal(p["set"]["set_hash"], self.next_set_hash)
        assert_set(p["next_set"], epoch=2, size=4, weight=110)
        self.next_set_hash = p["next_set"]["set_hash"]
        b = n[STAKER_B].getstakinginfo()
        assert_equal(([stake["status"] for stake in b["stakes"]], b["active_weight"]), (["ACTIVE"], 0))
        info = n[2].getstakinginfo()
        assert_equal((info["stakes"], info["active_weight"]), ([], 20))
        self.log.info("260: n2 (stake spent at ~225) still produces alone (residual)")
        assert_equal(self.solo_block(2), R_H + EPOCH_LENGTH)

    def check_rotation_300(self, n):
        stakers = [n[i] for i in range(6)]
        p = self.assert_agree(stakers)["producers"]
        assert_equal((p["epoch"], p["epoch_start"]), (2, R_H + 2 * EPOCH_LENGTH))
        assert_set(p["set"], epoch=2, size=4, weight=110)
        assert_equal(p["set"]["set_hash"], self.next_set_hash)
        # Snapshot(299) = {n0, n4, n5} is below the minimum: the rotation
        # carries the set over, the escape takes the snapshot itself (FD-2).
        assert_set(p["next_set"], epoch=3, size=4, weight=110)
        assert_set(p["escape_set"], epoch=3, size=3, weight=80)
        assert p["next_set"]["set_hash"] != p["escape_set"]["set_hash"]
        assert_equal(n[STAKER_B].getstakinginfo()["active_weight"], STAKE_B)
        assert_equal(n[2].getstakinginfo()["active_weight"], 0)
        self.log.info("300: n5 produces alone; n2 is out")
        assert_equal(self.solo_block(STAKER_B), R_H + 2 * EPOCH_LENGTH)
        assert_equal(n[2].getstakinginfo()["active_weight"], 0)

    def check_escape(self, n):
        stakers = [n[i] for i in range(6)]
        v = self.assert_agree(stakers)
        p = v["producers"]
        tip = n[0].getblockcount()
        assert_greater_than(R_H + 3 * EPOCH_LENGTH, tip + 1)
        assert_equal((p["epoch"], p["last_escape_height"], p["escapes"]), (2, -1, 0))
        assert_set(p["escape_set"], epoch=3, size=4, weight=90)
        assert_equal(n[3].getstakinginfo()["active_weight"], 0)
        assert_equal(p["escape_time"], n[0].getblockheader(n[0].getbestblockhash())["time"] +
                     BLOCK_INTERVAL + ESCAPE_ROUNDS * ROUND_SECONDS)

        self.log.info("Escape drill: every in-force producer stopped; n3 alone from round 60")
        h_e = tip + 1
        self.produce_to(h_e, [n[3]], allow_escape=True, first_round=ESCAPE_ROUNDS)
        assert_equal(coinbase_key(n[0], h_e), self.key_of(3))
        escape_round = block_round(n[0], h_e)
        assert_greater_than_or_equal(escape_round, ESCAPE_ROUNDS)
        p = self.assert_agree(stakers)["producers"]
        assert_equal((p["epoch"], p["epoch_start"], p["next_rotation"]), (3, h_e, h_e + EPOCH_LENGTH))
        assert_equal((p["last_escape_height"], p["escapes"]), (h_e, 1))
        assert_set(p["set"], epoch=3, size=4, weight=90)
        assert_set(p["next_set"], epoch=4, size=4, weight=90)
        assert_equal(n[1].getstakinginfo()["active_weight"], 0)
        assert_equal(n[3].getstakinginfo()["active_weight"], 10)
        self.log.info(f"Escape block {h_e} in round {escape_round}; next rotation {h_e + EPOCH_LENGTH}")
        self.h_e = h_e
        return h_e

    def check_coinbase_scan_b(self, h_e):
        n0 = self.nodes[0]
        tip = n0.getblockcount()
        keys = {i: self.key_of(i) for i in range(6)}
        producers = {height: coinbase_key(n0, height) for height in range(R_H, tip + 1)}
        assert keys[3] not in [producers[h] for h in range(R_H, h_e)]
        assert keys[STAKER_A] in [producers[h] for h in range(R_H, R_H + EPOCH_LENGTH)]
        assert keys[2] not in [producers[h] for h in range(R_H + 2 * EPOCH_LENGTH, tip + 1)]
        assert keys[1] not in [producers[h] for h in range(h_e, tip + 1)]
        assert all(producers[h] in keys.values() for h in producers)
        escapes = [h for h in range(R_H, tip + 1) if block_round(n0, h) >= ESCAPE_ROUNDS]
        assert_equal(escapes, [h_e])
        counts = {i: sum(1 for key in producers.values() if key == keys[i]) for i in range(6)}
        self.log.info(f"Producers of {R_H}..{tip} by node: {counts}")

    def check_restart_reindex_b(self):
        n = self.nodes
        stakers = [n[i] for i in range(6)]
        before = self.assert_agree(stakers)
        self.log.info("n0-n5 restart and reproduce the V1 and producer views")
        for i in range(6):
            self.stop_node(i)
        for i in range(6):
            self.start_b3(i, self.main_args)
        for a, b in ((1, 0), (2, 1), (3, 2), (4, 0), (5, 0)):
            self.connect_nodes(a, b)
        assert_equal(self.assert_agree(stakers), before)
        self.log.info("n0 reindexes and reproduces them")
        self.stop_node(0)
        self.start_b3(0, self.main_args, "-reindex")
        self.wait_until(lambda: n[0].getbestblockhash() == before["tip"], timeout=600)
        for a in (1, 4, 5):
            self.connect_nodes(a, 0)
        n[0].syncwithvalidationinterfacequeue()
        assert_equal(self.assert_agree(stakers), before)

        self.log.info(f"n7 syncs from genesis with -b3h2checkpoint={H2_HEIGHT}:<hash> and agrees")
        self.h2_args = self.main_args + [f"-b3h2checkpoint={H2_HEIGHT}:{self.hash300}"]
        self.start_b3(FRESH, self.h2_args)
        self.connect_nodes(FRESH, 0)
        self.sync_tips([n[0], n[FRESH]], timeout=300)
        n[FRESH].syncwithvalidationinterfacequeue()
        assert_equal(chain_view(n[FRESH]), before)
        return before

    def check_h2(self, view):
        n7 = self.nodes[FRESH]
        wrong = self.nodes[0].getblockhash(H2_HEIGHT - 1)
        wrong_args = self.main_args + [f"-b3h2checkpoint={H2_HEIGHT}:{wrong}"]
        self.log.info("H2: n7 with a wrong hash at 300 refuses to start")
        self.stop_node(FRESH)
        start_refused(n7, self.run_args(wrong_args), H2_MISMATCH)
        self.log.info("H2: n7 with the wrong hash and -reindex stops at 299")
        with n7.assert_debug_log(["bad-modern-checkpoint", "Reindexing finished"], timeout=300):
            self.start_b3(FRESH, wrong_args, "-reindex")
        self.wait_until(lambda: n7.getblockcount() == H2_HEIGHT - 1, timeout=300)
        assert_equal(n7.getbestblockhash(), self.nodes[0].getblockhash(H2_HEIGHT - 1))
        self.log.info("H2: n7 with the right hash and -reindex is back at the tip")
        self.stop_node(FRESH)
        self.start_b3(FRESH, self.h2_args, "-reindex")
        self.wait_until(lambda: n7.getbestblockhash() == view["tip"], timeout=600)
        n7.syncwithvalidationinterfacequeue()
        assert_equal(chain_view(n7), view)

    def check_late_upgrader(self, view):
        n = self.nodes
        n0 = n[0]
        n6 = n[LATE]
        late_args = with_extension(list(B3_BRIDGE_ARGS) + HORIZON_ARGS, STALL_EXTENSION)
        fork_time = n0.getblockheader(n0.getblockhash(R_H - 1))["time"]
        self.log.info("Late upgrader: n6 (no H1, stopped at 219) restores n3's wallet and produces its own branch")
        self.start_b3(LATE, late_args, mocktime=fork_time)
        assert_equal(n6.getblockcount(), R_H - 1)
        assert_equal(n6.getpeerinfo(), [])
        n6.restorewallet("n3_backup", self.n3_backup)
        assert_equal(n6.getstakinginfo()["validator_key"], self.key_of(3))
        assert_equal(n6.getstakinginfo()["active_weight"], 10)
        self.produce_to(LATE_TIP, [n6], nodes=[n6], clock=(lambda: n6.mocktime, n6.setmocktime), timeout=1800)
        assert all(coinbase_key(n6, h) == self.key_of(3) for h in range(R_H, LATE_TIP + 1))
        late_tip = n6.getbestblockhash()
        late_time = n6.getblockheader(late_tip)["time"]
        assert late_tip != view["tip"]

        self.log.info("n6 upgrades (H1 knobs) past R_h + horizon: n0's branch is too deep, n6 keeps its tip")
        self.stop_node(LATE)
        self.set_chain_time(max(self.mock_time, late_time))
        self.start_b3(LATE, self.main_args)
        with n6.assert_debug_log(["modern-reorg-too-deep"], timeout=60):
            self.connect_nodes(LATE, 0)
        time.sleep(1)
        assert_equal(n6.getbestblockhash(), late_tip)
        assert_equal(n0.getbestblockhash(), view["tip"])

        self.log.info("n6 reindexes with the H1 knobs: its own block 220 is ineligible; it then syncs to n0")
        self.stop_node(LATE)
        with n6.assert_debug_log(["bad-pos-ineligible", "Reindexing finished"], timeout=300):
            self.start_b3(LATE, self.main_args, "-reindex")
        self.wait_until(lambda: n6.getbestblockhash() == n0.getblockhash(R_H - 1), timeout=300)
        self.connect_nodes(LATE, 0)
        self.sync_tips([n0, n6], timeout=300)
        n6.syncwithvalidationinterfacequeue()
        assert_equal(chain_view(n6), view)
        self.assert_agree([node for node in n if node.running])

    # =========================================================== fixture I
    def run_fixture_i(self):
        n = self.nodes
        n0 = n[0]
        self.main_args = list(B3_BRIDGE_ARGS) + H1_ARGS
        validators = [n[i] for i in VALIDATORS]
        a = n[STAKER_A]
        self.n4_staking = False

        self.log.info("Fixture I: n0-n3 validators and n4 with H1 from 220; live V1 BLS finality")
        self.start_bridge_validators(extra_args=H1_ARGS)
        self.start_b3(STAKER_A, self.main_args)
        self.init_wallet(node=STAKER_A)
        self.connect_nodes(STAKER_A, 0)
        self.bring_up_validators(MODERN_START + 19)

        self.log.info("~152: n4 is funded")
        self.freeze_i()
        first = n0.getblockcount() + 1
        txid = n0.sendtoaddress(a.get_deterministic_priv_key().address, FUNDING)
        self.synchronize_mempools()
        self.resume_i()
        self.wait_mined(txid, first_height=first)
        self.wait_until(lambda: n0.getblockcount() >= I_STAKE_HEIGHT - 12, timeout=600)

        self.log.info(f"Mock time: n4 stakes 15 and binds in exactly block {I_STAKE_HEIGHT} "
                      f"(ACTIVE from {I_STAKE_HEIGHT + STAKE_ACTIVATION_DEPTH})")
        self.freeze_i()
        assert_greater_than(I_STAKE_HEIGHT, n0.getblockcount())
        tip_time = n0.getblockheader(n0.getbestblockhash())["time"]
        self.set_chain_time(max(int(time.time()), tip_time))
        self.produce_to(I_STAKE_HEIGHT - 1, validators)
        txids = [a.createstake(STAKE_A)["txid"], a.bindfinalitykey()["txid"]]
        assert_equal(self.mine_txids_mock(txids, validators, max_blocks=1), [I_STAKE_HEIGHT] * 2)
        assert_equal(a.getstakinginfo()["stakes"][0]["created_height"], I_STAKE_HEIGHT)
        assert_equal(a.startstaking()["finality_signing"], True)
        a.stopstaking()
        self.n4_staking = True

        self.log.info("Frozen at exactly 218: producers follow the V1 set in force; n4 has no weight")
        self.produce_to(R_H - 2, validators + [a])
        nodes = self.bridge_nodes()
        v = self.assert_agree(nodes)
        p = v["producers"]
        assert_equal((p["active"], p["next_rotation"], p["stake_enters_at"]), (False, R_H, R_H))
        assert p["set"]["set_hash"] in (v["validator_set"]["set_hash"], v["validator_set"]["next_set_hash"])
        # V1 epoch 2 started at or before 218: V1's snapshot for its next set
        # (at most 217) predates n4's activation at 218.
        assert_greater_than_or_equal(v["epoch"], 2)
        assert_greater_than_or_equal(R_H - 2, v["epoch_start"])
        assert_equal(v["lineage_broken"], False)
        v1_epoch = v["epoch"]
        key_a = self.key_of(STAKER_A)
        assert_equal([stake["status"] for stake in a.getstakinginfo()["stakes"]], ["ACTIVE"])
        assert_equal(a.getstakinginfo()["active_weight"], 0)

        self.log.info("219: the producer set for 220 is Snapshot(219) with n4; V1's set in force lacks n4")
        self.produce_to(R_H - 1, validators + [a])
        v = self.assert_agree(nodes)
        p = v["producers"]
        assert_equal((p["active"], p["epoch"], p["epoch_start"]), (True, 0, R_H))
        v1_weight = sum(VALIDATOR_STAKES)
        assert_set(p["set"], epoch=0, size=VALIDATOR_COUNT + 1, weight=v1_weight + STAKE_A)
        assert_equal((v["validator_set"]["size"], v["validator_set"]["total_weight"]), (VALIDATOR_COUNT, v1_weight))
        assert p["set"]["set_hash"] not in (v["validator_set"]["set_hash"], v["validator_set"]["next_set_hash"])
        assert key_a not in [member["validator_key"] for member in n0.getfinalityset()["members"]]
        assert_equal(v["lineage_broken"], False)
        assert_equal(a.getstakinginfo()["active_weight"], STAKE_A)
        self.log.info("220: n4 produces alone, a block only the H1 schedule admits")
        assert_equal(self.solo_block(STAKER_A), R_H)
        for node in nodes:
            node.setmocktime(0)
        self.mock_time = 0

        self.log.info("Real time to >= 301 with V1 finalized above 290")
        self.resume_i()
        self.wait_until(lambda: n0.getblockcount() >= R_H + 2 * EPOCH_LENGTH + 1 and
                        self.finalized_height(n0) > R_H + 2 * EPOCH_LENGTH - 10, timeout=900)
        self.freeze_i()
        v = self.assert_agree(nodes)
        p = v["producers"]
        assert_equal(p["active"], True)
        assert_greater_than_or_equal(p["epoch"], 2)
        assert_equal((p["epoch_start"] - R_H) % EPOCH_LENGTH, 0)
        assert_equal(p["epoch_start"], R_H + p["epoch"] * EPOCH_LENGTH)
        assert_equal((p["last_escape_height"], p["escapes"]), (-1, 0))
        assert_greater_than(a.getstakinginfo()["active_weight"], 0)
        assert_greater_than(v["finalized"]["height"], R_H + 2 * EPOCH_LENGTH - 10)
        assert_equal(v["lineage_broken"], False)
        assert_greater_than(v["epoch"], v1_epoch)
        tip = n0.getblockcount()
        assert all(block_round(n0, h) < ESCAPE_ROUNDS for h in range(R_H, tip + 1))
        a_blocks =[h for h in range(R_H, tip + 1) if coinbase_key(n0, h) == key_a]
        assert a_blocks, "n4 produced no block since R_h"
        self.log.info(f"V1 epoch {v1_epoch} -> {v['epoch']}, finalized {v['finalized']['height']}; "
                      f"producer epoch {p['epoch']}; n4 produced {len(a_blocks)} blocks since R_h")

        self.log.info("Restart, -reindex of n0 and a fresh n7 sync agree")
        for i in (0, 1, 2, 3, STAKER_A):
            self.restart_node(i, extra_args=self.run_args(self.main_args))
        for x, y in ((1, 0), (2, 1), (3, 2), (STAKER_A, 0)):
            self.connect_nodes(x, y)
        assert_equal(self.assert_agree(nodes), v)
        self.restart_node(0, extra_args=self.run_args(self.main_args, "-reindex"))
        self.wait_until(lambda: n0.getbestblockhash() == v["tip"], timeout=600)
        for x in (1, STAKER_A):
            self.connect_nodes(x, 0)
        n0.syncwithvalidationinterfacequeue()
        assert_equal(self.assert_agree(nodes), v)
        self.start_b3(FRESH, self.main_args)
        self.connect_nodes(FRESH, 0)
        self.sync_tips([n0, n[FRESH]], timeout=300)
        n[FRESH].syncwithvalidationinterfacequeue()
        assert_equal(chain_view(n[FRESH]), v)

    def freeze_i(self):
        if self.n4_staking:
            self.nodes[STAKER_A].stopstaking()
        self.freeze()

    def resume_i(self):
        self.resume()
        if self.n4_staking:
            assert_equal(self.nodes[STAKER_A].startstaking()["running"], True)


if __name__ == '__main__':
    B3H1ProducerRotationTest(__file__).main()
