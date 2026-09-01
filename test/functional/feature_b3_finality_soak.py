#!/usr/bin/env python3
# Copyright (c) 2026 The B3Coin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""B3 Modern PoS finality release-qualification soak (plan Commit 19, section D).

Four validator nodes on the B3 modern regtest (-b3modernregtest, the
architecture-contract section-64 activation overrides): corridor funding,
STAKE + FINALITY_KEY bindings for four independent wallets, a four-member
Set_0 where no single validator reaches quorum, automatic staking with BLS
finality signing on every node, multi-node finsig gossip aggregated into
consensus certificates, epoch rotation with a certified handover, the
persisted finality pin, restart reproduction, pin-refused invalidation and a
partition reorg that respects finality.
"""

import time
from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than_or_equal, assert_raises_rpc_error

# Corridor 160 => the first modern-PoS block is M = 161. Pacing is scaled to
# 1 s (corridor and PoS), so the soak runs in real time.
M = 161
B3_ARGS = [
    '-b3modernregtest',
    '-b3corridorlength=160',
    '-fallbackfee=0.00001',
    # FINALITY_KEY evidence has a 700-vbyte verification-cost floor. Keep the
    # relay floor high enough that a wallet which prices only the base bytes
    # would be rejected; bindfinalitykey must fund the complete MPA-aware fee.
    '-minrelaytxfee=0.00001',
    # The soak deliberately consolidates fragmented corridor rewards. Give
    # those large synthetic transactions a test-local total-fee ceiling;
    # production wallet defaults are not under test here.
    '-maxtxfee=0.001',
    '-addresstype=legacy',
    '-changetype=legacy',
    '-debug=validation',
]
# Amounts are B3 (1 B3 = 1e9 base units = one snapshot weight).
STAKES = [15, 10, 5, 1]  # weights 15 / 10 / 5 / 1 -> W = 31, quorum = 21


class B3FinalitySoakTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 4
        self.setup_clean_chain = True
        self.extra_args = [B3_ARGS] * self.num_nodes

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mine_corridor(self, count, addr):
        """Mine corridor blocks in pacing-respecting chunks (times may run at
        most the future bound ahead of the wall clock)."""
        while count > 0:
            chunk = min(count, 90)
            try:
                self.generatetoaddress(self.nodes[0], chunk, addr)
            except AssertionError:
                for i, n in enumerate(self.nodes):
                    self.log.error(f"STALL node{i} height={n.getblockcount()} tips={n.getchaintips()}")
                raise
            count -= chunk
            if count > 0:
                time.sleep(45)

    def finality(self, node):
        return node.getfinalitystatus()

    def run_test(self):
        n0 = self.nodes[0]
        # Each node uses its own default wallet (independent validator keys).
        addr0 = n0.getnewaddress()

        self.log.info("Corridor: funding blocks (coinbase maturity 100)")
        self.mine_corridor(135, addr0)
        assert_equal(n0.getblockcount(), 135)
        for st in (self.finality(n) for n in self.nodes):
            assert_equal(st['configured'], True)
            assert_equal(st['bootstrapped'], False)

        self.log.info("Funding the other three validators")
        for i in range(1, 4):
            n0.sendtoaddress(self.nodes[i].getnewaddress(), STAKES[i] + 5)
        self.generatetoaddress(n0, 2, addr0)

        self.log.info("STAKE + FINALITY_KEY binding on all four wallets")
        binds = []
        for i, n in enumerate(self.nodes):
            n.createstake(STAKES[i])
            binds.append(n.bindfinalitykey())
            assert_equal(binds[i]['seq'], 0)
            assert_equal(binds[i]['action'], 'bind')
            assert binds[i]['txid'] in n.getrawmempool()
            entry = n.getmempoolentry(binds[i]['txid'])
            assert_equal(entry['vsize'], 700)
            assert_greater_than_or_equal(entry['fees']['base'], Decimal('0.000007'))
        assert_raises_rpc_error(
            -4, 'already unconfirmed', self.nodes[0].bindfinalitykey)
        self.sync_mempools(timeout=120)  # bindings + stakes relay to the miner
        self.generatetoaddress(n0, 3, addr0)  # include stake + binding txs
        for n in self.nodes:
            info = n.getfinalityinfo()
            assert_equal(info['binding']['bound'], True)
            assert_equal(info['binding']['key_is_ours'], True)

        self.log.info("Corridor to M-1; stakes mature 20 blocks after creation")
        self.mine_corridor(160 - n0.getblockcount(), addr0)
        assert_equal(n0.getblockcount(), 160)

        self.log.info("startstaking on all nodes: finality signers armed")
        for n in self.nodes:
            r = n.startstaking()
            assert_equal(r['finality_signing'], True)

        self.log.info("Modern PoS: waiting for the four-member Set_0 and quorum finality")
        self.wait_until(lambda: n0.getblockcount() >= M + 12, timeout=240)
        self.sync_blocks(timeout=120)

        def finalized_at_least(height):
            for n in self.nodes:
                st = self.finality(n)
                if not st['bootstrapped'] or st.get('finalized', {}).get('height', -1) < height:
                    return False
            return True
        self.wait_until(lambda: finalized_at_least(M + 5), timeout=240)
        st = self.finality(n0)
        assert_equal(st['validator_set']['size'], 4)
        assert_equal(st['validator_set']['total_weight'], 31)
        assert_equal(st['validator_set']['quorum_weight'], 21)  # > any single validator (max 15)
        assert_greater_than_or_equal(st['pin']['height'], M + 5)

        self.log.info("Epoch rotation with a certified handover (E = 30)")
        self.wait_until(lambda: all(self.finality(n)['epoch'] >= 1 for n in self.nodes), timeout=360)
        for n in self.nodes:
            st = self.finality(n)
            assert_greater_than_or_equal(st['epoch_start'], M + 30)
            assert_equal(st['lineage_broken'], False)

        self.log.info("Finality pin: invalidating at or below the pin is refused")
        pin = self.finality(n0)['pin']
        assert_raises_rpc_error(-20, 'modern-finality-violation', n0.invalidateblock,
                                n0.getblockhash(pin['height']))
        assert_raises_rpc_error(-20, 'modern-finality-violation', n0.invalidateblock,
                                n0.getblockhash(pin['height'] - 3))

        self.log.info("Restart: node3 reproduces finalized state and pin, then resumes")
        st3_before = self.finality(self.nodes[3])
        self.restart_node(3, extra_args=B3_ARGS)
        self.connect_nodes(3, 0)
        st3_after = self.finality(self.nodes[3])
        assert_greater_than_or_equal(st3_after['pin']['height'], st3_before['pin']['height'])
        assert_greater_than_or_equal(st3_after['finalized']['height'], M + 5)
        assert_equal(st3_after['bootstrapped'], True)
        self.nodes[3].startstaking()

        self.log.info("Partition: the majority advances, the minority reorgs back onto it")
        tip3 = self.nodes[3].getbestblockhash()
        self.disconnect_nodes(3, 0)
        height_now = n0.getblockcount()
        self.wait_until(lambda: n0.getblockcount() >= height_now + 6, timeout=240)
        pin_before_join = self.finality(n0)['pin']
        self.connect_nodes(3, 0)
        self.sync_blocks(timeout=180)
        # The minority followed the heavier chain; the pin only ever advances
        # (after a deep rejoin the anchor raise waits for the next tracker
        # sync, so allow it to catch up rather than asserting instantly).
        assert self.nodes[3].getbestblockhash() == n0.getbestblockhash()
        self.wait_until(lambda: all(self.finality(n).get('pin', {}).get('height', -1) >= pin_before_join['height']
                                    for n in self.nodes), timeout=120)
        for n in self.nodes:
            assert_equal(self.finality(n)['lineage_broken'], False)
        self.log.info(f"Soak complete at height {n0.getblockcount()}, epoch {self.finality(n0)['epoch']}, "
                      f"finalized {self.finality(n0)['finalized']['height']}")
        del tip3


if __name__ == '__main__':
    B3FinalitySoakTest(__file__).main()
