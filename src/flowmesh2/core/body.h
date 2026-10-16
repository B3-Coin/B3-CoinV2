// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_BODY_H
#define B3_FLOWMESH2_CORE_BODY_H

#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <cstddef>
#include <span>
#include <vector>

/** The slot body (slice R4; ARCHITECTURE.md section 9).
 *
 * A body is what a proposal carries besides the header and what the DECISION
 * journal record stores, so a decided slot can be re-executed (O-16):
 *
 * - facts: the canonical encodings of the chain facts the slot consumes
 *   (core/facts.h EncodeFact), in fact-cursor order;
 * - originals: the EXACT original ActionV2 bytes, signature included, in
 *   proposal order. Nobody re-encodes or re-signs them.
 *
 * Both lists are opaque byte strings here. The body codec only bounds them;
 * decoding, signature checks and the validity predicate run in the engine
 * (ValidateSlotInputs, R7), which refuses a body whose items do not decode.
 *
 * Encoding: CompactSize(#facts), each fact as CompactSize(len) || bytes, then
 * CompactSize(#originals), each original the same way. Decode(bytes, profile)
 * is exact and refuses, with OVERSIZE: more than MAX_SLOT_FACTS facts, a fact
 * longer than MAX_FACT_BYTES, more than profile.max_actions_per_slot
 * originals, an original longer than ActionV2::MaxEncodedSize(profile), or
 * input longer than MaxEncodedSize(profile). Every count is checked before
 * allocation. Encode() is total (the hash of any body exists).
 *
 * Commitments:
 * - BodyHash() = Tagged(BODY, Encode()).
 * - ActionLeaf(o) = Tagged(ACTION_LEAF, exact bytes of o). The signature is
 *   inside, so two valid signatures on one ActionId are two distinct leaves.
 * - ActionsRoot() = MerkleRoot(ActionLeaf(o) for o in originals): the
 *   header's actions_root.
 * - FactLeaf(f) = Tagged(FACT, bytes of f), which is FactId(f) for a decoded
 *   fact (core/facts.h). FactsRoot() = MerkleRoot(FactLeaf(f) for f in
 *   facts): the header's facts_root.
 *
 * PROVISIONAL (O-27): MAX_SLOT_FACTS and MAX_FACT_BYTES are outline
 * constants, not profile fields; the owner may move them into the profile.
 */
namespace flowmesh2 {

//! Most chain facts one slot may consume. PROVISIONAL (O-27).
inline constexpr size_t MAX_SLOT_FACTS{1'024};
//! Longest canonical fact encoding. PROVISIONAL (O-27).
inline constexpr size_t MAX_FACT_BYTES{1'024};

//! Tagged(ACTION_LEAF, exact original bytes).
uint256 ActionLeaf(std::span<const unsigned char> exact_bytes);
//! Tagged(FACT, canonical fact bytes) = FactId of the decoded fact.
uint256 FactLeaf(std::span<const unsigned char> fact_bytes);

struct SlotBody {
    std::vector<std::vector<unsigned char>> facts;
    std::vector<std::vector<unsigned char>> originals;

    friend bool operator==(const SlotBody&, const SlotBody&) = default;

    std::vector<unsigned char> Encode() const;
    //! Longest encoding Decode accepts under `profile`.
    static size_t MaxEncodedSize(const ProfileParams& profile);
    static Result<SlotBody> Decode(std::span<const unsigned char> bytes, const ProfileParams& profile);

    uint256 BodyHash() const;
    uint256 ActionsRoot() const;
    uint256 FactsRoot() const;
};

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_BODY_H
