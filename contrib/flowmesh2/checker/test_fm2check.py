#!/usr/bin/env python3
# Copyright (c) 2026 The B3 developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Self-tests of the M3-ENG-05 checker against goldens the C++ tests pin."""

import hashlib
import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from fm2check import bip340, codec  # noqa: E402

PROFILE_JSON = os.path.join(HERE, '..', '..', '..', 'src', 'flowmesh2', 'PROFILE_DEX1_REGTEST.json')


def label(text):
    return hashlib.sha256(text.encode()).digest()


class CodecGoldens(unittest.TestCase):
    def test_protocol_accounts(self):
        with open(PROFILE_JSON, encoding='utf-8') as f:
            profile_hash = bytes.fromhex(json.load(f)['profile_hash'])
        self.assertEqual(codec.seat_pool(profile_hash).hex(),
                         'fd93419098c3c5e92abf35c6137b61a61256c79b7ecbfc8035629686e365fafe')
        self.assertEqual(codec.treasury(profile_hash).hex(),
                         'f01fabb0e7ab2ad143342973b39fe69befe3a43481c84071f98105d0f240eac1')

    def test_totals_leaf(self):
        leaf = codec.totals_leaf_bytes(codec.test_asset('rUSD'), 1_000_000_000, 7, 11, 50)
        self.assertEqual(leaf.hex(), '6439dd037ac1af8e258b4177319230f77f127c3fcf5ec6436aba005bb25bd7ed00ca9a3b0000000007'
                                     '000000000000000b000000000000003200000000000000')
        self.assertEqual(codec.tagged('STATE_TOTALS', leaf).hex(),
                         '07910e487e30bbe3d6fb8a382cf9b3df817f9a1c2fc5c7e27a9a1d54da30f59c')

    def test_market_and_nullifier_leaves(self):
        m1 = codec.Market(codec.NATIVE, codec.test_asset('rUSD'), 1_000_000, 1)
        self.assertEqual(m1.leaf_value().hex(), 'f9ce30bd61833304f61f2e062039bf7c88975b570185cfe58906e8619879ec17')
        txid = label('fm2/test/T1')
        self.assertEqual(codec.tagged('STATE_NULLIFIERS', codec.nullifier_leaf_bytes(txid, 0)).hex(),
                         '0a1f8c51460c42eb30982e1e3ece013006484760cf3b94b8c0a26b72c613bc38')
        self.assertEqual(codec.tagged('STATE_NULLIFIERS', codec.nullifier_leaf_bytes(txid, 1)).hex(),
                         '7dab8aaebf792873afac2cd45fb95107d6d95e2f274e330a2116f6e06c4aef53')

    def test_root_of_roots(self):
        names = ('accounts', 'orders', 'markets', 'nullifiers', 'totals')
        roots = [label('fm2/test/root/' + n) for n in names]
        self.assertEqual(codec.state_root_v0(*roots).hex(),
                         '53f2f3f916a26a4ed3b5032dc50fc60c061a5aa333661458aeaddaa2e2381dc1')

    def test_resting_order_leaf(self):
        leaf = codec.order_leaf_bytes({
            'id': label('fm2/test/order/a'), 'market': label('fm2/test/market'), 'owner': label('fm2/test/account/bob'),
            'side': codec.SELL, 'price': 250, 'lots': 3_000, 'arrival_rank': 4_096, 'reserved_left': 3_000_000_000,
            'cum_quote': 500_000, 'fee_charged': 50, 'fills': 1})
        self.assertEqual(len(leaf), 153)
        self.assertEqual(codec.tagged('STATE_ORDERS', leaf).hex(),
                         '389e1c17b3896483c5d34187e9db601020a3107376cd3f751329b96bbd0d257c')

    def test_merkle_shapes_agree_with_pairwise_levels(self):
        # The spec says RFC 6962; the C++ builds pairwise levels and promotes an
        # unpaired node. They are the same tree for every size.
        def pairwise(values):
            level = [codec.merkle_leaf(v) for v in values]
            while len(level) > 1:
                level = [codec.merkle_node(level[i], level[i + 1]) if i + 1 < len(level) else level[i]
                         for i in range(0, len(level), 2)]
            return codec.tagged('MERKLE_ROOT', codec.u64(len(values)) + level[0])
        for n in range(1, 70):
            values = [label(str(i)) for i in range(n)]
            self.assertEqual(codec.merkle_root(values), pairwise(values), n)


class Bip340Vectors(unittest.TestCase):
    def test_vector_0(self):
        # BIP340 test-vectors.csv, index 0.
        sec = (3).to_bytes(32, 'big')
        pub = bytes.fromhex('F9308A019258C31049344F85F89D5229B531C845836F99B08601F113BCE036F9')
        sig = bytes.fromhex('E907831F80848D1069A5371B402410364BDF1C5F8307B0084C55F1CE2DCA8215'
                            '25F66A4A85EA8B71E482A74F382D2CE5EBEEE8FDB2172F477DF4900D310536C0')
        self.assertEqual(bip340.pubkey(sec), pub)
        self.assertEqual(bip340.sign(bytes(32), sec, bytes(32)), sig)
        self.assertTrue(bip340.verify(bytes(32), pub, sig))
        self.assertFalse(bip340.verify(b'\x01' + bytes(31), pub, sig))
        self.assertFalse(bip340.verify(bytes(32), pub, sig[:63] + bytes([sig[63] ^ 1])))


if __name__ == '__main__':
    unittest.main()
