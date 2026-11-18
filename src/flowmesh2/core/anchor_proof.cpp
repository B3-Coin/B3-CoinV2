// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/anchor_proof.h>

namespace flowmesh2 {

Result<void> CheckAnchorProof(const AnchorProof& proof)
{
    const Error bad_path{ErrorCode::VERIFY_FAILED, ANCHOR_PROOF_PATH};
    const Error bad_pin{ErrorCode::VERIFY_FAILED, ANCHOR_PROOF_FINALITY};
    if (proof.anchor.height < proof.from.height) return bad_path;
    if (proof.finalized.height < proof.anchor.height) return bad_pin;
    // The path runs from `from` through the anchor to the pin (review of 2026-11-18: it stopped at the anchor and only
    // the pin's height was compared, so a pin on another fork, or another block at the anchor's height, passed).
    const uint64_t span{static_cast<uint64_t>(proof.finalized.height) - proof.from.height};
    if (proof.path.size() != span) return bad_path;
    bool anchor_on_path{proof.anchor == proof.from}; // the anchor is `from`, or the link at its height
    const uint256* previous{&proof.from.hash};
    uint32_t height{proof.from.height};
    for (const HeaderLink& link : proof.path) {
        ++height; // at most finalized.height: no overflow
        if (link.height != height || link.hash.IsNull() || link.parent != *previous) return bad_path;
        if (height == proof.anchor.height) anchor_on_path = link.hash == proof.anchor.hash;
        previous = &link.hash;
    }
    if (!anchor_on_path) return bad_path;
    if (*previous != proof.finalized.hash) return bad_pin; // the path ends at the pin
    return Ok();
}

} // namespace flowmesh2
