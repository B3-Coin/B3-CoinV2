// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_LEGACY_FN_ISSUANCE_H
#define B3COIN_NODE_LEGACY_FN_ISSUANCE_H

#include <modern/legacy_fn_issuance.h>

#include <string>
#include <vector>

class ChainstateManager;

namespace node {

//! One buildable legacy FN issuance: the canonical proof plus the facts
//! it establishes (already self-verified by the pure builder).
struct LegacyFnIssuanceCandidate {
    modern::LegacyFnIssuanceProofV1 proof;
    modern::LegacyFnIssuanceFacts facts;
};

/**
 * The archival builder sweep (owner ruling 2026-08-17;
 * doc/design/b3-legacy-fn-issuance-proposal.md): scan the sealed legacy
 * prefix [1, H] for qualifying disintegrations that carry the 1-coin
 * P2PKH designation (those without it are IGNORED), locate their funding
 * transactions, and build one canonical self-verified issuance proof
 * per PoD. Runs on the ONE node that chooses to build the deferred
 * issuance queue — it needs full block and undo data through H (a
 * reindexed archival node); no other node ever runs this.
 *
 * Requires the boundary pinned and the active chain to carry X at H.
 * Deterministic: candidates ascend by (height, position) and are
 * byte-identical on every node that runs the sweep — the builder confers
 * no authority. Fails closed with `error` set (missing block/undo data
 * reports that a reindex/redownload is required); on failure `out` is
 * left empty.
 */
bool BuildAllLegacyFnIssuances(ChainstateManager& chainman,
                               std::vector<LegacyFnIssuanceCandidate>& out,
                               std::string& error);

} // namespace node

#endif // B3COIN_NODE_LEGACY_FN_ISSUANCE_H
