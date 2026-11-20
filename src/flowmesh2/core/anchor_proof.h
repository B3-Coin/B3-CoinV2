// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_CORE_ANCHOR_PROOF_H
#define B3_FLOWMESH2_CORE_ANCHOR_PROOF_H

#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/result.h>
#include <uint256.h>

#include <cstdint>
#include <string_view>
#include <vector>

/** The anchor proof (M7-ADP-01; plan section 3.3, PLAN-11).
 *
 * A proposal's anchor must be verifiable by a follower whose own b3d lags:
 * the plan has every proposal carry "the M7 finality certificate reference
 * and header path". This is that path as the finalized b3d adapter
 * (chain/b3d_adapter.h) produces it from the chain it was handed: one
 * HeaderLink per block from `from` (exclusive) to `anchor` (inclusive), each
 * naming its height, its hash and its parent's hash, and `finalized`, the
 * block the node's finality had pinned when the proof was made, at or above
 * the anchor.
 *
 * CheckAnchorProof is the structural check: the links are contiguous in
 * height from `from.height + 1` to `anchor.height`, the first parent is
 * `from.hash`, every later parent is the previous link's hash, the last hash
 * is `anchor.hash` (an empty path means anchor == from), and the finality
 * pin is at or above the anchor; VERIFY_FAILED with ANCHOR_PROOF_PATH or
 * ANCHOR_PROOF_FINALITY otherwise. It does not know the chain: whether the
 * links are real headers (their hashes recompute from header bytes) and
 * whether `finalized` is a block the verifier trusts is the verifier's own
 * knowledge, b3verify's `VerifyAnchorProof` for clients (M7-ADP-02) and the
 * follower's adapter for seats (`VerifyAnchor`).
 *
 * Lives in core/ (fm2_types) because fm2_verify never links fm2_core (the
 * O-33 pattern).
 */
namespace flowmesh2 {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view ANCHOR_PROOF_PATH{"anchor proof path is not a parent chain from its base to its anchor"}; //!< (VERIFY_FAILED)
inline constexpr std::string_view ANCHOR_PROOF_FINALITY{"anchor proof finality pin is below its anchor"};               //!< (VERIFY_FAILED)

//! One block of the path: its height, its hash and the hash of the block below it.
struct HeaderLink {
    uint32_t height{0};
    uint256 hash;
    uint256 parent;

    friend bool operator==(const HeaderLink&, const HeaderLink&) = default;
};

struct AnchorProof {
    Anchor from;                   //!< the block the verifier already holds (its last certified anchor)
    Anchor anchor;                 //!< the block proven
    std::vector<HeaderLink> path;  //!< heights from.height + 1 .. anchor.height, in order
    Anchor finalized;              //!< the finality pin that covers the anchor

    friend bool operator==(const AnchorProof&, const AnchorProof&) = default;
};

//! The structural check of the file comment.
Result<void> CheckAnchorProof(const AnchorProof& proof);

} // namespace flowmesh2

#endif // B3_FLOWMESH2_CORE_ANCHOR_PROOF_H
