// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_POS_H
#define B3COIN_MODERN_POS_H

#include <consensus/block_codec.h>
#include <consensus/era.h>
#include <consensus/validation.h>
#include <primitives/block.h>

class CBlockIndex;
class CCoinsViewCache;

namespace modern {

/**
 * Modern-era proof-of-stake boundary (heights > LEGACY_FINAL_HEIGHT).
 *
 * MISSING RULE SET — deliberately not invented here. The following modern
 * PoS semantics are NOT yet defined anywhere in this repository, and this
 * module must not guess them:
 *
 *  - stake eligibility: which UTXOs may stake, minimum amounts, ages;
 *  - kernel/selection: what replaces the legacy Peercoin-v1 kernel, and
 *    the randomness source backing it (legacy stake modifiers and legacy
 *    in-block transaction offsets are NOT carried into the modern era —
 *    no documented modern rule requires them);
 *  - difficulty/target adjustment for modern stakes;
 *  - reward schedule and fee treatment;
 *  - the structure that marks a modern block as proof-of-stake (the
 *    legacy coinstake shape is not assumed);
 *  - block-signature or attestation scheme;
 *  - validator-set and finality semantics.
 *
 * Until an approved rule set defines these, CheckModernStake() REJECTS
 * every block handed to it: a modern era without rules cannot validate
 * blocks, and failing closed is the only safe default. Tests may install
 * an adapter to exercise the dispatch plumbing.
 */
class PosValidator
{
public:
    virtual ~PosValidator() = default;

    /**
     * Validate the stake of a MODERN-era block against its connected
     * parent and the current UTXO view. The block is guaranteed by the
     * dispatcher to carry the modern codec marker and modern-encoded
     * transactions only.
     */
    virtual bool CheckStake(const CBlock& block, const CBlockIndex& parent,
                            const CCoinsViewCache& view, BlockValidationState& state) const = 0;
};

/**
 * Which stake-rule family may judge a block, given its codec marker and
 * its connected era. Legacy blocks can never enter modern PoS; modern
 * blocks can never enter the legacy stake kernel; a marker/era mismatch
 * belongs to neither (and is independently rejected by the contextual
 * codec check).
 */
enum class StakeRules {
    LEGACY,
    MODERN,
    MISMATCH,
};

constexpr StakeRules SelectStakeRules(const int32_t block_version, const Consensus::B3Era era)
{
    const bool modern_codec{Consensus::HasB3BlockCodecV2(block_version)};
    if (era == Consensus::B3Era::LEGACY) {
        return modern_codec ? StakeRules::MISMATCH : StakeRules::LEGACY;
    }
    return modern_codec ? StakeRules::MODERN : StakeRules::MISMATCH;
}

/**
 * Dispatch entry for a MODERN-era B3 block. Enforces that modern PoS
 * consumes only the modern codecs, then defers to the installed rule set.
 * With no rule set installed (the current state of the repository) every
 * block is rejected.
 */
inline bool CheckModernStake(const CBlock& block, const CBlockIndex& parent,
                             const CCoinsViewCache& view, BlockValidationState& state,
                             const PosValidator* validator)
{
    if (SelectStakeRules(block.nVersion, Consensus::B3Era::MODERN) != StakeRules::MODERN) {
        return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-pos-codec",
                             "legacy codec cannot enter modern PoS validation");
    }
    for (const CTransactionRef& tx : block.vtx) {
        if (tx->IsLegacyEncoded()) {
            return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "bad-pos-codec",
                                 "legacy-encoded transaction cannot enter modern PoS validation");
        }
    }
    if (validator) {
        return validator->CheckStake(block, parent, view, state);
    }
    return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "no-modern-pos-rules",
                         "modern proof-of-stake rule set is not defined");
}

} // namespace modern

#endif // B3COIN_MODERN_POS_H
