// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <node/flowmesh_client_ticks.h>

#include <consensus/amount.h>
#include <flowmesh/observed_time.h>

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

namespace {

int64_t Signed(const UniValue& object, const char* key)
{
    const auto& value{object.find_value(key)};
    if (!value.isNum()) Reject(std::string{"missing "} + key);
    const auto number{value.getInt<int64_t>()};
    if (number < 0) Reject(std::string{"out of range: "} + key);
    return number;
}

//! An observed time or bucket start (below OBSERVED_MS_LIMIT), or with
//! `window_end` a window's end (at most OBSERVED_MS_LIMIT).
int64_t Time(const UniValue& object, const char* key, const bool window_end = false)
{
    const int64_t ms{Signed(object, key)};
    if (window_end ? ms > flowmesh::OBSERVED_MS_LIMIT : !flowmesh::ObservedTimeInRange(ms)) Reject(std::string{"out of range: "} + key);
    return ms;
}

uint256 Hash(const UniValue& object, const char* key)
{
    const auto text{String(object, key, 64)};
    const auto parsed{uint256::FromHex(text)};
    if (text.size() != 64 || !parsed || parsed->IsNull()) Reject(std::string{"invalid "} + key);
    return *parsed;
}

//! Only flags a public reply may carry; unknown names are ignored.
uint8_t PublicFlags(const UniValue& object)
{
    const auto& flags{object.find_value("flags")};
    if (!flags.isArray() || flags.size() > 16) Reject("invalid flags");
    uint8_t out{0};
    for (const auto& flag : flags.getValues()) {
        if (!flag.isStr()) Reject("invalid flag");
        const auto& name{flag.get_str()};
        if (name == "clock_clamped") out |= flowmesh::OBSERVED_CLOCK_CLAMPED;
        else if (name == "clock_step") out |= flowmesh::OBSERVED_CLOCK_STEP;
        else if (name == "clock_suspect") out |= flowmesh::OBSERVED_CLOCK_SUSPECT;
        else if (name == "restart") out |= flowmesh::OBSERVED_RESTART;
    }
    return out;
}

util::Unsigned128 Decimal(const UniValue& object, const char* key)
{
    const auto text{String(object, key, 39)};
    if (text.empty() || (text.size() > 1 && text.front() == '0')) Reject(std::string{"invalid "} + key);
    util::Unsigned128 out{0};
    const util::Unsigned128 max{~util::Unsigned128{0}};
    for (const char c : text) {
        if (c < '0' || c > '9') Reject(std::string{"invalid "} + key);
        const unsigned digit{static_cast<unsigned>(c - '0')};
        if (out > (max - digit) / 10) Reject(std::string{"out of range: "} + key);
        out = out * 10 + digit;
    }
    return out;
}

} // namespace

bool FlowMeshAdvertisedObservedTime(const UniValue& status) noexcept
{
    try {
        return status.isObject() && status.find_value("observed_time_v1").isTrue();
    } catch (...) {
        return false;
    }
}

size_t FlowMeshAdvertisedObservedCandles(const UniValue& status) noexcept
{
    try {
        if (!status.isObject()) return 0;
        const auto& value{status.find_value("observed_candles_max")};
        if (!value.isNum()) return 0;
        return static_cast<size_t>(std::min<uint64_t>(value.getInt<uint64_t>(), flowmesh::OBSERVED_CANDLE_PAGE_MAX));
    } catch (...) {
        return 0;
    }
}

std::string FlowMeshMergeObservedTimes(std::map<uint64_t, flowmesh::ObservedStamp>& held, const UniValue& section,
                                       const std::deque<flowmesh::ClearingTick>& ring, const int64_t received_ms,
                                       bool& clock_suspect) noexcept
{
    try {
        try {
            if (!section.isObject()) Reject("section is not an object");
            if (!Boolean(section, "available")) {
                return "endpoint_unavailable: " + String(section, "unavailable_reason", 64);
            }
            if (String(section, "source_kind", 32) != "operator_observed") Reject("unknown source_kind");
            clock_suspect = Boolean(section, "clock_suspect");
            const auto& rows{section.find_value("rows")};
            if (!rows.isArray() || rows.size() > flowmesh::MARKET_DATA_MAX_TICKS + flowmesh::MARKET_DATA_MAX_HISTORY + 1) {
                Reject("rows exceed their bound");
            }
            std::map<uint64_t, flowmesh::ObservedStamp> merged{held};
            for (const auto& row : rows.getValues()) {
                if (!row.isObject()) Reject("row is not an object");
                flowmesh::ObservedStamp stamp;
                stamp.sequence = Unsigned(row, "sequence", std::numeric_limits<uint64_t>::max());
                stamp.hash = Hash(row, "microblock_hash");
                const auto mode{String(row, "mode", 16)};
                if (mode == "live") {
                    stamp.mode = flowmesh::ObservedMode::LIVE;
                    stamp.ms = Time(row, "observed_ms");
                } else if (mode == "delayed") {
                    stamp.mode = flowmesh::ObservedMode::DELAYED;
                    stamp.ms = Time(row, "applied_here_ms");
                } else if (mode == "untimed") {
                    stamp.mode = flowmesh::ObservedMode::UNTIMED;
                } else {
                    Reject("unknown mode");
                }
                stamp.flags = PublicFlags(row);
                // Bound to a held tick of this very entry; any other row is dropped.
                if (ring.empty() || stamp.sequence < ring.front().sequence || stamp.sequence > ring.back().sequence) continue;
                if (ring[stamp.sequence - ring.front().sequence].microblock_hash != stamp.hash) continue;
                merged[stamp.sequence] = stamp;
            }
            std::erase_if(merged, [&](const auto& item) {
                return ring.empty() || item.first < ring.front().sequence || item.first > ring.back().sequence ||
                       ring[item.first - ring.front().sequence].microblock_hash != item.second.hash;
            });
            std::optional<int64_t> live;
            for (auto& [sequence, stamp] : merged) {
                (void)sequence;
                const bool future{stamp.mode != flowmesh::ObservedMode::UNTIMED &&
                                  stamp.ms > received_ms + FLOWMESH_CLIENT_OBSERVED_FUTURE_MS};
                const bool backwards{stamp.mode == flowmesh::ObservedMode::LIVE && live && stamp.ms < *live};
                if (future || backwards) stamp.flags |= flowmesh::OBSERVED_IMPLAUSIBLE;
                if (stamp.mode == flowmesh::ObservedMode::LIVE && !backwards) live = stamp.ms;
            }
            held = std::move(merged);
            return {};
        } catch (const std::exception& e) {
            held.clear();
            return std::string{"endpoint_observed_time_rejected: "} + e.what();
        }
    } catch (...) {
        held.clear();
        return "endpoint_observed_time_rejected";
    }
}

std::optional<flowmesh::ObservedCandlePage> FlowMeshParseObservedCandles(const UniValue& result, const flowmesh::CandleInterval interval,
                                                                         const uint64_t head_sequence, std::string& error) noexcept
{
    try {
        try {
            if (!result.isObject()) Reject("reply is not an object");
            flowmesh::ObservedCandlePage page;
            page.remote = true;
            page.interval = interval;
            page.available = Boolean(result, "available");
            if (!page.available) {
                page.unavailable_reason = "endpoint_unavailable: " + String(result, "unavailable_reason", 64);
                return page;
            }
            if (String(result, "source_kind", 32) != "operator_observed") Reject("unknown source_kind");
            if (String(result, "interval", 8) != flowmesh::CandleIntervalName(interval)) Reject("another interval");
            const int64_t step{flowmesh::CandleIntervalMs(interval)};
            if (Signed(result, "interval_ms") != step) Reject("another interval length");
            page.from_ms = Time(result, "from_ms", /*window_end=*/true);
            page.before_ms = Time(result, "before_ms", /*window_end=*/true);
            if (page.before_ms < page.from_ms || (page.before_ms - page.from_ms) % step != 0 ||
                page.before_ms - page.from_ms > static_cast<int64_t>(flowmesh::OBSERVED_CANDLE_PAGE_MAX) * step) {
                Reject("invalid window");
            }
            page.more = Boolean(result, "more");
            const auto& rows{result.find_value("candles")};
            if (!rows.isArray() || rows.size() > flowmesh::OBSERVED_CANDLE_PAGE_MAX) Reject("candles exceed their bound");
            for (const auto& row : rows.getValues()) {
                if (!row.isObject()) Reject("candle is not an object");
                flowmesh::ObservedCandle candle;
                candle.open_ms = Time(row, "open_ms");
                if (candle.open_ms % step != 0 || candle.open_ms < page.from_ms || candle.open_ms >= page.before_ms) Reject("misaligned candle");
                if (!page.candles.empty() && candle.open_ms <= page.candles.back().open_ms) Reject("candles out of order");
                candle.first_sequence = Unsigned(row, "first_sequence", std::numeric_limits<uint64_t>::max());
                candle.last_sequence = Unsigned(row, "last_sequence", std::numeric_limits<uint64_t>::max());
                if (candle.last_sequence < candle.first_sequence || candle.last_sequence > head_sequence) Reject("invalid sequences");
                if (!page.candles.empty() && candle.first_sequence <= page.candles.back().last_sequence) Reject("overlapping candles");
                candle.entries = static_cast<uint32_t>(Unsigned(row, "entries", std::numeric_limits<uint32_t>::max()));
                candle.clearings = static_cast<uint32_t>(Unsigned(row, "clearings", std::numeric_limits<uint32_t>::max()));
                if (candle.entries == 0 || candle.clearings > candle.entries ||
                    candle.entries - 1 > candle.last_sequence - candle.first_sequence) Reject("invalid counts");
                candle.base_volume = Decimal(row, "base_volume");
                candle.quote_volume = Decimal(row, "quote_volume");
                if (candle.clearings > 0) {
                    candle.open = static_cast<CAmount>(Unsigned(row, "open", MAX_MONEY));
                    candle.high = static_cast<CAmount>(Unsigned(row, "high", MAX_MONEY));
                    candle.low = static_cast<CAmount>(Unsigned(row, "low", MAX_MONEY));
                    candle.close = static_cast<CAmount>(Unsigned(row, "close", MAX_MONEY));
                    if (candle.low > candle.high || candle.open < candle.low || candle.open > candle.high ||
                        candle.close < candle.low || candle.close > candle.high || candle.base_volume == 0) Reject("inconsistent prices");
                } else if (row.exists("open") || candle.base_volume != 0 || candle.quote_volume != 0) {
                    Reject("values without a clearing");
                }
                const auto& flags{row.find_value("flags")};
                if (!flags.isArray() || flags.size() > 8) Reject("invalid flags");
                for (const auto& flag : flags.getValues()) {
                    if (!flag.isStr()) Reject("invalid flag");
                    const auto& name{flag.get_str()};
                    if (name == "partial") candle.flags |= flowmesh::CANDLE_PARTIAL;
                    else if (name == "late") candle.flags |= flowmesh::CANDLE_LATE;
                    else if (name == "seam_before") candle.flags |= flowmesh::CANDLE_SEAM_BEFORE;
                    else if (name == "clock") candle.flags |= flowmesh::CANDLE_CLOCK;
                }
                page.candles.push_back(candle);
            }
            return page;
        } catch (const std::exception& e) {
            error = std::string{"endpoint_candles_rejected: "} + e.what();
            return std::nullopt;
        }
    } catch (...) {
        error = "endpoint_candles_rejected";
        return std::nullopt;
    }
}

} // namespace node
