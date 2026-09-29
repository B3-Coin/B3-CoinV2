// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/market_view.h>

#include <flowmesh/state.h>
#include <util/int128.h>

namespace flowmesh {

ImbalanceSide ObserveImbalanceSide(const ClearingEngine::ClearingResult& clearing, const FlowMeshState& post)
{
    if (!clearing.cleared) return ImbalanceSide::NONE;
    util::Signed128 bid{0}, ask{0};
    for (const auto& [account, fill] : clearing.bid_fill) {
        bid += post.EffectiveQty(ClearingEngine::Side::BID, account, clearing.price);
    }
    for (const auto& [account, fill] : clearing.ask_fill) {
        ask += post.EffectiveQty(ClearingEngine::Side::ASK, account, clearing.price);
    }
    if (bid == 0 && ask == 0) return clearing.imbalance == 0 ? ImbalanceSide::NONE : ImbalanceSide::UNKNOWN;
    // Both sides cannot keep quantity at the price after a maximal clearing.
    if (bid > 0 && ask > 0) return ImbalanceSide::UNKNOWN;
    const util::Signed128 residual{bid > 0 ? bid : ask};
    const util::Signed128 committed{clearing.imbalance};
    // The committed imbalance saturates at MAX_MONEY, so a larger residual
    // is consistent only with a saturated value.
    const bool consistent{clearing.imbalance < MAX_MONEY ? residual == committed : residual >= committed};
    if (!consistent) return ImbalanceSide::UNKNOWN;
    return bid > 0 ? ImbalanceSide::BID : ImbalanceSide::ASK;
}

} // namespace flowmesh
