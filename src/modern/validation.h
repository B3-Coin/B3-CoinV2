// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_VALIDATION_H
#define B3COIN_MODERN_VALIDATION_H

class CBlock;
class CBlockIndex;
class BlockValidationState;

namespace modern {

/**
 * PROTOTYPE-ONLY SKELETON. Full validation for MODERN-era blocks
 * (heights > LEGACY_FINAL_HEIGHT).
 *
 * The modern era begins with the block at H + 1 referencing the pinned
 * boundary hash X and uses the new proof-of-stake rules with complete
 * validation. Two properties are structural rather than per-block rules:
 *  - a reorganization crossing the legacy boundary is permanently
 *    prohibited (CanReorgTo below), and
 *  - future consensus features attach through typed Policy Outputs
 *    validated by generic transition proofs carried in the spending
 *    context — never through new fields on CTxIn/CTxOut. This is the
 *    accommodation point for DEX_VAULT-style policies (withdrawal
 *    transition proofs, per-asset conservation, forced vault change);
 *    none of that logic lives in this spike.
 */
class BlockValidator
{
public:
    virtual ~BlockValidator() = default;

    /** Context-free structural and rule checks for a modern block. */
    virtual bool CheckBlock(const CBlock& block, BlockValidationState& state) const = 0;

    /** Contextual validation against the connected parent index. */
    virtual bool ContextualCheckBlock(const CBlock& block, const CBlockIndex& parent,
                                      BlockValidationState& state) const = 0;

    /**
     * Whether the active chain may be rewound to `fork_point`. Must return
     * false for any fork point at or below LEGACY_FINAL_HEIGHT once the
     * boundary is configured.
     */
    virtual bool CanReorgTo(const CBlockIndex& fork_point) const = 0;
};

} // namespace modern

#endif // B3COIN_MODERN_VALIDATION_H
