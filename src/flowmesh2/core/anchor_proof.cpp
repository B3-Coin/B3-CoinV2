// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/anchor_proof.h>

namespace flowmesh2 {

Result<void> CheckAnchorProof(const AnchorProof& proof)
{
    const Error bad_path{ErrorCode::VERIFY_FAILED, ANCHOR_PROOF_PATH};
    if (proof.anchor.height < proof.from.height) return bad_path;
    const uint64_t span{static_cast<uint64_t>(proof.anchor.height) - proof.from.height};
    if (proof.path.size() != span) return bad_path;
    if (proof.path.empty()) {
        if (!(proof.anchor == proof.from)) return bad_path;
    } else {
        const uint256* previous{&proof.from.hash};
        uint32_t height{proof.from.height};
        for (const HeaderLink& link : proof.path) {
            ++height;
            if (link.height != height || link.hash.IsNull() || link.parent != *previous) return bad_path;
            previous = &link.hash;
        }
        if (*previous != proof.anchor.hash) return bad_path;
    }
    if (proof.finalized.height < proof.anchor.height) return Error{ErrorCode::VERIFY_FAILED, ANCHOR_PROOF_FINALITY};
    return Ok();
}

} // namespace flowmesh2
