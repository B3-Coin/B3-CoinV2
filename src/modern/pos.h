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
 * The rule set is the frozen Modern PoS V1 specification
 * (doc/design/b3-modern-pos-spec.md; primitives in modern/pos_v1.h):
 * deterministic stake-weighted eligibility over a chained seed, exact
 * timestamps encoding recovery rounds, and BIP340 validator signatures.
 * Legacy stake modifiers and legacy in-block transaction offsets are NOT
 * carried into the modern era.
 *
 * Dispatch remains fail-closed by configuration: while the chain's
 * Consensus::Params::modern_pos parameter block is unset — every shipped
 * network — CheckModernStake() REJECTS every block (`no-modern-pos-rules`).
 * The V1 rules are applied by validation only when the block is configured
 * (regtest scaffolding until mainnet numbers are ratified). Tests may still
 * install a PosValidator adapter to exercise the dispatch plumbing; it is
 * never set in production (guard-tested).
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
 * The codec gate every modern-PoS path shares: modern PoS consumes only the
 * modern block and transaction codecs, never a legacy encoding.
 */
inline bool CheckModernPosCodec(const CBlock& block, BlockValidationState& state)
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
    return true;
}

/**
 * Dispatch entry for a MODERN-era B3 block on the test-adapter and
 * fail-closed paths. Enforces the codec gate, then defers to the installed
 * test rule set; with none installed AND no configured V1 parameter block
 * (validation routes configured chains to the V1 rules directly) every
 * block is rejected.
 */
inline bool CheckModernStake(const CBlock& block, const CBlockIndex& parent,
                             const CCoinsViewCache& view, BlockValidationState& state,
                             const PosValidator* validator)
{
    if (!CheckModernPosCodec(block, state)) return false;
    if (validator) {
        return validator->CheckStake(block, parent, view, state);
    }
    return state.Invalid(BlockValidationResult::BLOCK_CONSENSUS, "no-modern-pos-rules",
                         "modern proof-of-stake rule set is not defined");
}

} // namespace modern

#endif // B3COIN_MODERN_POS_H
