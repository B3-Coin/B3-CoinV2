// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <node/flowmesh_client_ticks.h>

#include <consensus/amount.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace node {
namespace {

[[noreturn]] void Reject(const std::string& reason) { throw std::runtime_error{reason}; }

uint64_t Unsigned(const UniValue& object, const char* key, const uint64_t maximum)
{
    const auto& value{object.find_value(key)};
    if (!value.isNum()) Reject(std::string{"missing "} + key);
    const auto number{value.getInt<uint64_t>()};
    if (number > maximum) Reject(std::string{"out of range: "} + key);
    return number;
}

bool Boolean(const UniValue& object, const char* key)
{
    const auto& value{object.find_value(key)};
    if (!value.isBool()) Reject(std::string{"missing "} + key);
    return value.get_bool();
}

std::string String(const UniValue& object, const char* key, const size_t maximum)
{
    const auto& value{object.find_value(key)};
    if (!value.isStr() || value.get_str().size() > maximum) Reject(std::string{"missing "} + key);
    return value.get_str();
}

flowmesh::ClearingTick ParseTick(const UniValue& row)
{
    if (!row.isObject()) Reject("tick is not an object");
    flowmesh::ClearingTick tick;
    tick.sequence = Unsigned(row, "sequence", std::numeric_limits<uint64_t>::max());
    const auto hash{String(row, "microblock_hash", 64)};
    const auto parsed{uint256::FromHex(hash)};
    if (hash.size() != 64 || !parsed || parsed->IsNull()) Reject("invalid microblock_hash");
    tick.microblock_hash = *parsed;
    const auto kind{String(row, "kind", 16)};
    if (kind == "execution") tick.kind = flowmesh::ClearingTick::Kind::EXECUTION;
    else if (kind == "settlement") tick.kind = flowmesh::ClearingTick::Kind::SETTLEMENT;
    else if (kind == "handoff") tick.kind = flowmesh::ClearingTick::Kind::HANDOFF;
    else Reject("unknown kind");
    tick.cleared = Boolean(row, "cleared");
    tick.price = static_cast<CAmount>(Unsigned(row, "price", MAX_MONEY));
    tick.volume = static_cast<CAmount>(Unsigned(row, "volume", MAX_MONEY));
    tick.imbalance = static_cast<CAmount>(Unsigned(row, "imbalance", MAX_MONEY));
    const auto side{String(row, "imbalance_side", 16)};
    if (side == "none") tick.imbalance_side = flowmesh::ImbalanceSide::NONE;
    else if (side == "bid") tick.imbalance_side = flowmesh::ImbalanceSide::BID;
    else if (side == "ask") tick.imbalance_side = flowmesh::ImbalanceSide::ASK;
    else if (side == "unknown") tick.imbalance_side = flowmesh::ImbalanceSide::UNKNOWN;
    else Reject("unknown imbalance_side");
    // What the engine can have produced: only an ordinary execution clears;
    // without a clearing there is no price, volume, imbalance or side; with
    // one, volume is positive and a known side matches the imbalance.
    if (!tick.cleared) {
        if (tick.price != 0 || tick.volume != 0 || tick.imbalance != 0 || tick.imbalance_side != flowmesh::ImbalanceSide::NONE) {
            Reject("values without a clearing");
        }
    } else if (tick.kind != flowmesh::ClearingTick::Kind::EXECUTION || tick.volume == 0 ||
               (tick.imbalance_side != flowmesh::ImbalanceSide::UNKNOWN &&
                (tick.imbalance_side == flowmesh::ImbalanceSide::NONE) != (tick.imbalance == 0))) {
        Reject("inconsistent clearing");
    }
    return tick;
}

} // namespace

size_t FlowMeshAdvertisedClearingTicks(const UniValue& status) noexcept
{
    try {
        if (!status.isObject()) return 0;
        const auto& value{status.find_value("clearing_ticks_max")};
        if (!value.isNum()) return 0;
        return static_cast<size_t>(std::min<uint64_t>(value.getInt<uint64_t>(), flowmesh::MARKET_DATA_MAX_TICKS));
    } catch (...) {
        return 0;
    }
}

std::string FlowMeshMergeClearingTicks(std::deque<flowmesh::ClearingTick>& ring, const UniValue& page,
                                       const std::optional<uint64_t> after, const size_t limit, const uint64_t head_sequence,
                                       const uint256& head_hash) noexcept
{
    try {
        try {
            if (!page.isObject()) Reject("page is not an object");
            if (!Boolean(page, "available")) {
                // The endpoint's own statement (history not replayed yet):
                // nothing to check, nothing held is contradicted.
                return "endpoint_unavailable: " + String(page, "unavailable_reason", 64);
            }
            const bool gap{Boolean(page, "gap")};
            (void)Boolean(page, "more");
            const auto& rows{page.find_value("ticks")};
            if (!rows.isArray() || rows.size() > std::min(limit, flowmesh::MARKET_DATA_MAX_TICKS)) Reject("page exceeds its bound");
            std::vector<flowmesh::ClearingTick> ticks;
            ticks.reserve(rows.size());
            for (const auto& row : rows.getValues()) {
                auto tick{ParseTick(row)};
                if (!ticks.empty() && tick.sequence != ticks.back().sequence + 1) Reject("ticks are not consecutive");
                ticks.push_back(std::move(tick));
            }
            if (after && !ticks.empty()) {
                if (ticks.front().sequence <= *after) Reject("tick at or before after_sequence");
                if (ticks.front().sequence != *after + 1 && !gap) Reject("skipped ticks without a gap");
            }
            // Beyond the verified head nothing can be bound to it.
            std::erase_if(ticks, [&](const auto& tick) { return tick.sequence > head_sequence; });
            std::deque<flowmesh::ClearingTick> merged{ring};
            for (const auto& tick : ticks) {
                if (!merged.empty() && tick.sequence <= merged.back().sequence) {
                    if (tick.sequence < merged.front().sequence) continue;
                    if (merged[tick.sequence - merged.front().sequence] != tick) Reject("tick conflicts with one already held");
                    continue;
                }
                // A page that does not continue the ring starts it again.
                if (!merged.empty() && tick.sequence != merged.back().sequence + 1) merged.clear();
                merged.push_back(tick);
                if (merged.size() > flowmesh::MARKET_DATA_MAX_TICKS) merged.pop_front();
            }
            if (!merged.empty() && merged.back().sequence == head_sequence && merged.back().microblock_hash != head_hash) {
                Reject("tick at the verified head has another hash");
            }
            ring = std::move(merged);
            return {};
        } catch (const std::exception& e) {
            ring.clear();
            return std::string{"endpoint_clearing_ticks_rejected: "} + e.what();
        }
    } catch (...) {
        ring.clear();
        return "endpoint_clearing_ticks_rejected";
    }
}

} // namespace node
