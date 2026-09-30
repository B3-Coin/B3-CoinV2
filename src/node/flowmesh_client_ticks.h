// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#ifndef BITCOIN_NODE_FLOWMESH_CLIENT_TICKS_H
#define BITCOIN_NODE_FLOWMESH_CLIENT_TICKS_H

#include <flowmesh/market_data.h>
#include <uint256.h>
#include <univalue.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>

namespace node {

/** Client side of clearing ticks on the restricted API.
 *
 * Endpoint-reported display data, NOT evidence: ticks never feed signing,
 * admission, balances or any verification label. They are requested only
 * from an endpoint that advertises clearing_ticks_max (older endpoints reject
 * "ticks" as an unknown field), only by market-data reads and never by a
 * signing preflight. The only bound to verified state is the tick at the
 * client's verified head, which must carry that head's hash. */

//! After an endpoint refuses "ticks" as an unknown field, its advertisement
//! is ignored this long: a stale or flapping advertisement then costs one
//! extra request per period, not one per read.
inline constexpr std::chrono::minutes FLOWMESH_CLIENT_TICKS_WITHDRAWN_FOR{10};

//! clearing_ticks_max in a market status object, capped at the largest page.
//! Zero (never send "ticks") when absent or not an unsigned integer: a
//! malformed hint only disables ticks, never fails the read.
size_t FlowMeshAdvertisedClearingTicks(const UniValue& status) noexcept;

/** Checks one endpoint 'clearing_ticks' page and merges it into `ring`, a
 * run of consecutive ticks, oldest first, none beyond the verified head.
 *
 * `after` and `limit` are what the request asked for; `head_sequence` and
 * `head_hash` identify the client's verified head. Every tick must be
 * well-formed and consistent (bounded amounts; no clearing for a handoff or
 * settlement; nothing but side "none" without a clearing), the page's
 * sequences consecutive and after `after` (a later start only with the
 * endpoint's gap flag), ticks already held identical, and a tick at the
 * verified head that head's own. Ticks beyond the verified head are dropped;
 * a page that does not continue the ring replaces it. At most
 * MARKET_DATA_MAX_TICKS are kept.
 *
 * Returns empty on success. An endpoint's own "available": false leaves the
 * ring and returns its reason; any other failure clears the ring and returns
 * why. Never throws. */
std::string FlowMeshMergeClearingTicks(std::deque<flowmesh::ClearingTick>& ring, const UniValue& page,
                                       std::optional<uint64_t> after, size_t limit, uint64_t head_sequence,
                                       const uint256& head_hash) noexcept;

/** Client side of operator-observed time on the restricted API.
 *
 * Endpoint-reported display data, NOT evidence: when that endpoint says it
 * applied each entry, by its own clock. Requested together with clearing
 * ticks, only from an endpoint that advertises observed_time_v1 (and never
 * by a signing preflight), kept per endpoint (series of different endpoints
 * are never merged), and only for the ticks the client holds. A stamp that
 * fails a plausibility check is flagged OBSERVED_IMPLAUSIBLE, never
 * corrected. */

//! observed_time_v1 in a market status object; false when absent or malformed.
bool FlowMeshAdvertisedObservedTime(const UniValue& status) noexcept;
//! observed_candles_max in a market status object, capped at the largest
//! page; zero (never call 'candles') when absent or malformed.
size_t FlowMeshAdvertisedObservedCandles(const UniValue& status) noexcept;

//! A stamp more than this after the client received it is implausible.
inline constexpr int64_t FLOWMESH_CLIENT_OBSERVED_FUTURE_MS{5000};

/** Checks an endpoint 'observed_time' section and merges its rows into
 * `held` (that endpoint's stamps by sequence). A row is kept only for a
 * sequence of `ring` whose tick carries the row's hash; `held` afterwards
 * holds only sequences in `ring`. Flagged, never corrected: a LIVE time
 * below an earlier sequence's LIVE time, and any time more than
 * FLOWMESH_CLIENT_OBSERVED_FUTURE_MS after `received_ms`. Only public flags
 * are taken from the endpoint. `clock_suspect` receives the section's flag.
 *
 * Returns empty on success. An endpoint's own "available": false leaves
 * `held` and returns its reason; any other failure clears `held` and returns
 * why. Never throws. */
std::string FlowMeshMergeObservedTimes(std::map<uint64_t, flowmesh::ObservedStamp>& held, const UniValue& section,
                                       const std::deque<flowmesh::ClearingTick>& ring, int64_t received_ms,
                                       bool& clock_suspect) noexcept;

/** Checks one 'candles' reply of an endpoint: the interval it was asked for,
 * a window of at most OBSERVED_CANDLE_PAGE_MAX intervals, aligned ascending
 * opens inside it, ascending non-overlapping sequences none beyond
 * `head_sequence` (the client's verified head), consistent prices and exact
 * decimal volumes. The endpoint's prices, volumes and times stay its
 * claims: only their structure is checked. nullopt and `error` when
 * refused. Never throws. */
std::optional<flowmesh::ObservedCandlePage> FlowMeshParseObservedCandles(const UniValue& result, flowmesh::CandleInterval interval,
                                                                         uint64_t head_sequence, std::string& error) noexcept;

} // namespace node

#endif // BITCOIN_NODE_FLOWMESH_CLIENT_TICKS_H
