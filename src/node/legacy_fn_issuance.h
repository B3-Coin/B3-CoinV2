// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_LEGACY_FN_ISSUANCE_H
#define B3COIN_NODE_LEGACY_FN_ISSUANCE_H

#include <consensus/fn_params.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

class CBlock;
class ChainstateManager;

namespace Consensus {
struct Params;
}

namespace node {

struct PodRecord;

/**
 * The complete proof-free historical FN Genesis artifact pinned by the
 * transition release. `rights` is in canonical raw-PoD-id order and `root`
 * commits to these exact rows under `chain_domain`, `genesis_height` (H+1),
 * and `manifest_version`.
 */
struct LegacyFnGenesisManifest {
    uint256 chain_domain{};
    uint32_t genesis_height{0};
    uint16_t manifest_version{0};
    std::vector<Consensus::FnGenesisRight> rights;
    uint256 root{};

    friend bool operator==(const LegacyFnGenesisManifest&,
                           const LegacyFnGenesisManifest&) = default;
};

using LegacyFnBlockAt = std::function<bool(int height, CBlock& block)>;

/**
 * Assemble the canonical FN Genesis manifest from PoD records already
 * derived by `DerivePodRecords`. Each qualifying PoD is resolved back to its
 * sealed transaction; only the lowest-index exact 1-old-B3 byte-exact P2PKH
 * designation is a right. Funding-script claimability is deliberately
 * irrelevant. The full artifact is published only on success.
 *
 * `final_height`/`final_hash` are the operator-verified H/X pair. The block
 * source must carry X at H, and a configured hard-fork height must equal H+1.
 */
bool BuildLegacyFnGenesisManifestFromRecords(
    const Consensus::Params& params,
    int final_height,
    const uint256& final_hash,
    std::span<const PodRecord> records,
    const LegacyFnBlockAt& block_at,
    LegacyFnGenesisManifest& out,
    std::string& error);

/**
 * Archival sealed-chain sweep for the transition release. Requires the
 * consensus H/X boundary pinned on the active chain and complete block/undo
 * data through H. It derives PoDs with `DerivePodRecords`, then builds the
 * proof-free manifest above. No holder claim, proof, or legacy funding-key
 * authorization is constructed.
 */
bool BuildLegacyFnGenesisManifest(ChainstateManager& chainman,
                                  LegacyFnGenesisManifest& out,
                                  std::string& error);

} // namespace node

#endif // B3COIN_NODE_LEGACY_FN_ISSUANCE_H
