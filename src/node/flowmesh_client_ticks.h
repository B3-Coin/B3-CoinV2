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

} // namespace node

#endif // BITCOIN_NODE_FLOWMESH_CLIENT_TICKS_H
