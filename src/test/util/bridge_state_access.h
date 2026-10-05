// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3COIN_TEST_UTIL_BRIDGE_STATE_ACCESS_H
#define B3COIN_TEST_UTIL_BRIDGE_STATE_ACCESS_H

#include <bridge/reserve_v2.h>
#include <node/bridge_state.h>

namespace node {

/**
 * Test-only access to BridgeStateIndex state that no record of this slice
 * can create: a reserve ledger with transit (only the migration leaf, B-23,
 * creates it) or a funded ledger under a hand-built delta that skips the
 * deposits. The seed bypasses undo, so a test must not disconnect blocks
 * connected before it.
 */
struct BridgeStateIndexTestAccess {
    static void SeedReserve(BridgeStateIndex& index,
                            const bridge::ReserveLedgerV2& ledger,
                            const bridge::ProvenDepositMapV2& deposits)
    {
        index.m_reserve_v2 = ledger;
        index.m_deposits_v2 = deposits;
        index.m_deposits_v2_total = 0;
        for (const auto& [id, amount] : deposits) {
            index.m_deposits_v2_total += amount;
        }
    }
};

} // namespace node

#endif // B3COIN_TEST_UTIL_BRIDGE_STATE_ACCESS_H
