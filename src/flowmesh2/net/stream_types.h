// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_NET_STREAM_TYPES_H
#define B3_FLOWMESH2_NET_STREAM_TYPES_H

#include <flowmesh2/core/account_leaf.h>
#include <flowmesh2/core/ids.h>
#include <flowmesh2/core/outcome.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

/** The stream events (M5-API-02, stage 2 step 4; plan 3.8 GET /v2/stream;
 * ARCHITECTURE.md O-103). Every event carries a cursor (stream_epoch, seq,
 * head_slot): stream_epoch names the publisher instance (a restart is a
 * visible gap), seq is strictly increasing per subscription from 1 (ordering
 * only), head_slot is the slot the event describes. A SNAPSHOT resets the
 * client's state for its channel; a DELTA applies to the SNAPSHOT's head_slot
 * or later. Resubscription names the last head_slot processed: the hub
 * replays the slots after it when it still holds them, else it sends a fresh
 * SNAPSHOT (net/stream_hub.h).
 *
 * Channels (net/client_api.h StreamChannel): SLOTS (every committed slot:
 * its header, certificate envelope and counts), ACCOUNT (one account's leaf
 * after each slot that changed it, with that slot's balance deltas), BOOK
 * (one market's price ladder after each slot that changed it), TRADES (one
 * market's fills per slot, when any).
 */
namespace flowmesh2::net {

//! APPEND-ONLY; zero is never a value.
enum class StreamEventKind : uint8_t {
    SNAPSHOT = 1,
    DELTA = 2,
};
std::string_view ToString(StreamEventKind kind);

struct BookLevel {
    uint64_t price_ticks{0};
    uint64_t lots{0};
    uint32_t orders{0};
    friend bool operator==(const BookLevel&, const BookLevel&) = default;
};

struct SlotEvent {
    uint64_t height{0};
    uint256 header_hash;
    uint256 state_root;
    uint32_t action_count{0};
    uint32_t outcome_count{0};
    std::vector<unsigned char> envelope; //!< read::CertifiedEnvelope::Encode(): header || certificate, for b3verify
    friend bool operator==(const SlotEvent&, const SlotEvent&) = default;
};

struct AccountEvent {
    AccountId account;
    std::optional<AccountLeafV0> leaf; //!< the leaf after the slot; nullopt while the account does not exist
    std::vector<BalanceDelta> deltas;  //!< this slot's deltas of the account (empty in a SNAPSHOT)
    friend bool operator==(const AccountEvent&, const AccountEvent&) = default;
};

struct BookEvent {
    MarketId market;
    std::vector<BookLevel> bids; //!< best first
    std::vector<BookLevel> asks; //!< best first
    friend bool operator==(const BookEvent&, const BookEvent&) = default;
};

struct TradesEvent {
    MarketId market;
    std::vector<Fill> fills; //!< this slot's fills in the market, emission order (empty in a SNAPSHOT)
    friend bool operator==(const TradesEvent&, const TradesEvent&) = default;
};

using StreamBody = std::variant<SlotEvent, AccountEvent, BookEvent, TradesEvent>;

} // namespace flowmesh2::net

#endif // B3_FLOWMESH2_NET_STREAM_TYPES_H
