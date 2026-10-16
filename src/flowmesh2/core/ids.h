// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_IDS_H
#define B3_FLOWMESH2_CORE_IDS_H

#include <pubkey.h>
#include <serialize.h>
#include <uint256.h>

#include <compare>
#include <cstdint>

/** Identifiers of the FlowMesh V2 clean core (slice R3).
 *
 * Each identifier is a distinct type around a uint256 so that an AccountId can
 * never be passed where a MarketId is expected. All of them:
 * - compare as a total order: lexicographic over the 32 bytes (memcmp, the
 *   uint256 operator<), never by pointer or hash-table order;
 * - serialize as their 32 raw bytes (Outpoint: txid then vout as u32 LE).
 *
 * Who makes them:
 * - AssetId: Native() is the all-zero id of B3; other assets come from chain
 *   facts or listings (R4, M7-GOV-01). Tests and the simulator name synthetic
 *   assets Tagged(TEST_ASSET, name).
 * - AccountId: only FromKey, Tagged(ACCOUNT, x-only key). An account exists
 *   once a deposit fact creates its leaf (R5).
 * - MarketId: MarketDescriptor::Id (core/market.h).
 * - ActionId: ActionV2::Id in R4, the tagged hash of every action field except
 *   the signature. OrderId: the ActionId of the PLACE that created the order.
 * - Outpoint: the B3 transaction output a deposit fact consumes (credited once).
 */
namespace flowmesh2 {

struct AssetId {
    uint256 id;

    //! B3 itself: the all-zero AssetId.
    static AssetId Native() { return AssetId{}; }
    bool IsNative() const { return id.IsNull(); }

    friend bool operator==(const AssetId&, const AssetId&) = default;
    friend std::strong_ordering operator<=>(const AssetId&, const AssetId&) = default;
    SERIALIZE_METHODS(AssetId, obj) { READWRITE(obj.id); }
};

struct AccountId {
    uint256 id;

    //! Tagged(ACCOUNT, the 32 bytes of the x-only key).
    static AccountId FromKey(const XOnlyPubKey& key);

    friend bool operator==(const AccountId&, const AccountId&) = default;
    friend std::strong_ordering operator<=>(const AccountId&, const AccountId&) = default;
    SERIALIZE_METHODS(AccountId, obj) { READWRITE(obj.id); }
};

struct MarketId {
    uint256 id;

    friend bool operator==(const MarketId&, const MarketId&) = default;
    friend std::strong_ordering operator<=>(const MarketId&, const MarketId&) = default;
    SERIALIZE_METHODS(MarketId, obj) { READWRITE(obj.id); }
};

struct ActionId {
    uint256 id;

    friend bool operator==(const ActionId&, const ActionId&) = default;
    friend std::strong_ordering operator<=>(const ActionId&, const ActionId&) = default;
    SERIALIZE_METHODS(ActionId, obj) { READWRITE(obj.id); }
};

struct OrderId {
    uint256 id;

    //! The order created by a PLACE action is named by that action's id.
    static OrderId FromPlace(const ActionId& place) { return OrderId{place.id}; }

    friend bool operator==(const OrderId&, const OrderId&) = default;
    friend std::strong_ordering operator<=>(const OrderId&, const OrderId&) = default;
    SERIALIZE_METHODS(OrderId, obj) { READWRITE(obj.id); }
};

struct Outpoint {
    uint256 txid;
    uint32_t vout{0};

    //! Order: txid (lexicographic), then vout.
    friend bool operator==(const Outpoint&, const Outpoint&) = default;
    friend std::strong_ordering operator<=>(const Outpoint&, const Outpoint&) = default;
    SERIALIZE_METHODS(Outpoint, obj) { READWRITE(obj.txid, obj.vout); }
};

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_IDS_H
