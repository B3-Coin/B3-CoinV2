// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/engine/residual.h>

#include <algorithm>

namespace flowmesh2::engine {

Result<ResidualCross> FindResidualCross(std::span<const Taker> buys, std::span<const Taker> sells)
{
    for (const Taker& t : buys) {
        if (t.side != Side::BUY) return Error{ErrorCode::INVALID_INPUT, "residual buy is not a BUY"};
        if (t.remaining_lots == 0) return Error{ErrorCode::INVALID_INPUT, "residual without lots"};
    }
    for (const Taker& t : sells) {
        if (t.side != Side::SELL) return Error{ErrorCode::INVALID_INPUT, "residual sell is not a SELL"};
        if (t.remaining_lots == 0) return Error{ErrorCode::INVALID_INPUT, "residual without lots"};
    }
    ResidualCross cross;
    if (buys.empty() || sells.empty()) return Ok(std::move(cross));
    const auto by_limit = [](const Taker& a, const Taker& b) { return a.limit_ticks < b.limit_ticks; };
    const uint64_t highest_buy{std::max_element(buys.begin(), buys.end(), by_limit)->limit_ticks};
    const uint64_t lowest_sell{std::min_element(sells.begin(), sells.end(), by_limit)->limit_ticks};
    for (size_t i{0}; i < buys.size(); ++i) {
        if (buys[i].limit_ticks >= lowest_sell) cross.buys.push_back(i);
    }
    for (size_t i{0}; i < sells.size(); ++i) {
        if (sells[i].limit_ticks <= highest_buy) cross.sells.push_back(i);
    }
    return Ok(std::move(cross));
}

Result<ResidualCall> ResolveResidualCross(OrderBook&, std::span<const Taker>, std::span<const Taker>, const ProfileParams&)
{
    return NotImplemented("M3-ENG-02"); // TODO(rebirth:M3-ENG-02)
}

} // namespace flowmesh2::engine
