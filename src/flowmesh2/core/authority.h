// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_AUTHORITY_H
#define B3_FLOWMESH2_CORE_AUTHORITY_H

#include <serialize.h>
#include <uint256.h>

#include <compare>
#include <cstdint>

/** Chain anchor, fact cursor and authority version (slice R3).
 *
 * Header-only value types shared by the slot header (core/slot.h), the chain
 * adapter (R10) and the state owner (R9). ARCHITECTURE.md section 8.
 *
 * - Anchor: the B3 block a slot is anchored to. The validity predicate (R7)
 *   refuses an anchor whose height is below the parent's anchor height.
 * - FactCursor: a position in the stream of finalized chain facts, the fact
 *   `index` inside block `height`. Cursors are totally ordered by (height,
 *   index). A slot consumes the half-open range [facts_from, facts_to); it is
 *   empty when both are equal, and facts_from equals the parent's facts_to.
 * - AuthorityVersion: what a signature or an apply is valid for. The epoch
 *   changes only when a seat-set fact is finalized or when a reorg removes a
 *   block at or below the anchor (plan PLAN-11). The three authority checks of
 *   ARCHITECTURE.md section 8 compare a whole AuthorityVersion.
 *
 * Serialization: Anchor = u32 LE height, 32 raw hash bytes; FactCursor = u32
 * LE height, u32 LE index; AuthorityVersion = u64 LE epoch, seat_set_root,
 * profile_hash.
 */
namespace flowmesh2 {

struct Anchor {
    uint32_t height{0};
    uint256 hash;

    friend bool operator==(const Anchor&, const Anchor&) = default;
    SERIALIZE_METHODS(Anchor, obj) { READWRITE(obj.height, obj.hash); }
};

struct FactCursor {
    uint32_t height{0};
    uint32_t index{0};

    //! Total order: height, then index.
    friend bool operator==(const FactCursor&, const FactCursor&) = default;
    friend std::strong_ordering operator<=>(const FactCursor&, const FactCursor&) = default;
    SERIALIZE_METHODS(FactCursor, obj) { READWRITE(obj.height, obj.index); }
};

struct AuthorityVersion {
    uint64_t epoch{0};
    uint256 seat_set_root;
    uint256 profile_hash;

    friend bool operator==(const AuthorityVersion&, const AuthorityVersion&) = default;
    SERIALIZE_METHODS(AuthorityVersion, obj) { READWRITE(obj.epoch, obj.seat_set_root, obj.profile_hash); }
};

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_AUTHORITY_H
