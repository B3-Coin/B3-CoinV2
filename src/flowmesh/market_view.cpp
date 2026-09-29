// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/market_view.h>

#include <flowmesh/state.h>
#include <util/int128.h>

#include <algorithm>
#include <limits>

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

ClearingTickPage PageClearingTicks(const std::deque<MarketHistoryEntry>& history, const ClearingTickQuery& query)
{
    ClearingTickPage out;
    if (query.limit == 0 || query.limit > MARKET_DATA_MAX_TICKS) {
        out.unavailable_reason = "invalid_limit";
        return out;
    }
    out.available = true;
    if (history.empty()) return out;
    out.oldest_retained_sequence = history.front().sequence;
    auto first{history.end() - std::min(query.limit, history.size())};
    if (query.after_sequence) {
        const uint64_t after{*query.after_sequence};
        first = std::find_if(history.begin(), history.end(), [&](const auto& entry) { return entry.sequence > after; });
        out.gap = after < std::numeric_limits<uint64_t>::max() && after + 1 < *out.oldest_retained_sequence;
    }
    for (auto it{first}; it != history.end(); ++it) {
        if (out.ticks.size() == query.limit) {
            out.more = true;
            break;
        }
        out.ticks.push_back(MakeClearingTick(*it));
    }
    return out;
}

} // namespace flowmesh
