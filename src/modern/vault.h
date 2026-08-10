// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_MODERN_VAULT_H
#define B3COIN_MODERN_VAULT_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <modern/asset.h>
#include <modern/policy.h>
#include <modern/proof.h>
#include <serialize.h>
#include <uint256.h>

#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace modern {

/**
 * DEX_VAULT withdrawal policy — custody only, no matching
 * (doc/design/b3-architecture-contract.md, DEX ARCHITECTURE).
 *
 * A vault output has NO private key and no script path. The only way
 * value leaves custody is a finalized withdrawal receipt produced by
 * FlowMesh's deterministic clearing. Because the receipt fixes asset,
 * amount, destination, receipt id, finalized slot and the required change
 * vault, the authorized withdrawal is fully determined: ANYONE may relay
 * it, and nobody — relayer included — can redirect it. Trading, matching,
 * balances and reservations never touch the chain.
 */
struct WithdrawalReceipt {
    //! One-time receipt identity; consumed exactly once.
    uint256 receipt_id;
    AssetId asset;
    CAmount amount{0};
    //! OWNER v1 commitment of the payee. Fixed by finalization.
    uint256 destination;
    //! FlowMesh clearing slot in which this receipt finalized.
    uint64_t finalized_slot{0};
    //! The approved vault this receipt withdraws from; all remainder must
    //! return to DEX_VAULT outputs of this same commitment.
    uint256 vault_commitment;

    SERIALIZE_METHODS(WithdrawalReceipt, obj)
    {
        READWRITE(obj.receipt_id, obj.asset, obj.amount, obj.destination,
                  obj.finalized_slot, obj.vault_commitment);
    }
};

/**
 * Bounded interface to finalized-receipt state. Until FlowMesh state
 * exists this is backed by test mocks only. GetFinalized() must return a
 * receipt only while it is finalized AND not yet consumed; consumers of
 * CheckVaultWithdrawal must mark the reported receipts consumed when the
 * containing transition connects (and unconsumed on disconnect), so each
 * receipt authorizes exactly one withdrawal ever.
 */
class FinalizedReceiptView
{
public:
    virtual ~FinalizedReceiptView() = default;
    virtual std::optional<WithdrawalReceipt> GetFinalized(const uint256& receipt_id) const = 0;
};

enum class VaultCheck {
    OK,
    NOT_ACTIVE,
    BASE_INVALID,
    NO_VAULT_INPUT,
    MIXED_VAULTS,
    BAD_PROOF,
    RECEIPT_UNKNOWN,
    RECEIPT_WRONG_VAULT,
    DESTINATION_MISMATCH,
    CHANGE_MISMATCH,
    AMOUNT_OVERFLOW,
};

/**
 * Verify a vault withdrawal transition. `prev_outputs[i]` is the coin
 * spent by `inputs[i]`. On success, `consumed_out` (if given) receives
 * the receipt ids this transition consumes.
 *
 * Enforced, on top of exact per-asset conservation (modern/asset.h):
 *  - at least one DEX_VAULT input; all vault inputs (any shards) belong
 *    to one approved vault commitment;
 *  - every vault input carries the identical canonical receipt-id list;
 *    ids resolve through the finalized view and belong to this vault;
 *  - for each receipt, OWNER outputs paying its committed destination in
 *    its asset total at least its amount — redirection is impossible;
 *  - per asset, vault inflow equals receipt outflow plus vault change
 *    exactly, and every vault output returns to the approved vault
 *    commitment (partial withdrawals force all remainder back, possibly
 *    split across shards);
 *  - batching: one transition may satisfy many receipts.
 */
inline VaultCheck CheckVaultWithdrawal(const std::vector<ModernOutput>& prev_outputs,
                                       const ModernTransition& t,
                                       const FinalizedReceiptView& receipts, const int height,
                                       const Consensus::Params& params,
                                       std::vector<uint256>* consumed_out = nullptr)
{
    if (!AssetPoliciesActiveSlot()) return VaultCheck::NOT_ACTIVE;
    if (CheckAssetConservation(prev_outputs, t, height, params) != AssetCheck::OK) {
        return VaultCheck::BASE_INVALID;
    }
    if (t.proofs.size() != t.inputs.size()) return VaultCheck::BAD_PROOF;

    // Collect the vault side of the spend.
    uint256 vault_commitment{};
    std::map<AssetId, CAmount> vault_in;
    std::optional<std::vector<uint256>> receipt_ids;
    for (size_t i{0}; i < t.inputs.size(); ++i) {
        const ModernOutput& prev{prev_outputs[i]};
        if (prev.policy_type != static_cast<uint16_t>(PolicyType::DEX_VAULT)) continue;
        if (vault_commitment.IsNull()) {
            vault_commitment = prev.policy_commitment;
        } else if (vault_commitment != prev.policy_commitment) {
            return VaultCheck::MIXED_VAULTS;
        }
        CAmount& sum{vault_in[prev.asset]};
        if (prev.amount < 0 || prev.amount > MAX_MONEY - sum) return VaultCheck::AMOUNT_OVERFLOW;
        sum += prev.amount;

        const std::optional<std::vector<uint256>> ids{
            ParseVaultReceiptIds(t.proofs[i].payload)};
        if (!ids) return VaultCheck::BAD_PROOF;
        if (!receipt_ids) {
            receipt_ids = ids;
        } else if (*receipt_ids != *ids) {
            // Every vault input must claim the identical receipt set.
            return VaultCheck::BAD_PROOF;
        }
    }
    if (vault_commitment.IsNull()) return VaultCheck::NO_VAULT_INPUT;

    // Resolve receipts: finalized, unconsumed, and belonging to this vault.
    std::map<AssetId, CAmount> receipt_sums;
    std::map<std::pair<AssetId, uint256>, CAmount> destination_due;
    for (const uint256& id : *receipt_ids) {
        const std::optional<WithdrawalReceipt> receipt{receipts.GetFinalized(id)};
        if (!receipt) return VaultCheck::RECEIPT_UNKNOWN;
        if (receipt->vault_commitment != vault_commitment) {
            return VaultCheck::RECEIPT_WRONG_VAULT;
        }
        if (receipt->amount < 0) return VaultCheck::AMOUNT_OVERFLOW;
        CAmount& sum{receipt_sums[receipt->asset]};
        if (receipt->amount > MAX_MONEY - sum) return VaultCheck::AMOUNT_OVERFLOW;
        sum += receipt->amount;
        CAmount& due{destination_due[{receipt->asset, receipt->destination}]};
        if (receipt->amount > MAX_MONEY - due) return VaultCheck::AMOUNT_OVERFLOW;
        due += receipt->amount;
    }

    // Output side: vault change and destination payments.
    std::map<AssetId, CAmount> vault_change;
    std::map<std::pair<AssetId, uint256>, CAmount> owner_paid;
    for (const ModernOutput& out : t.outputs) {
        if (out.policy_type == static_cast<uint16_t>(PolicyType::DEX_VAULT)) {
            // All remainder returns to the approved vault, on any shard.
            if (out.policy_commitment != vault_commitment) return VaultCheck::CHANGE_MISMATCH;
            CAmount& sum{vault_change[out.asset]};
            if (out.amount > MAX_MONEY - sum) return VaultCheck::AMOUNT_OVERFLOW;
            sum += out.amount;
        } else if (out.policy_type == static_cast<uint16_t>(PolicyType::OWNER)) {
            CAmount& sum{owner_paid[{out.asset, out.policy_commitment}]};
            if (out.amount > MAX_MONEY - sum) return VaultCheck::AMOUNT_OVERFLOW;
            sum += out.amount;
        }
    }

    // Redirection is impossible: every committed destination receives its
    // full due in the committed asset.
    for (const auto& [key, due] : destination_due) {
        const auto paid{owner_paid.find(key)};
        if (paid == owner_paid.end() || paid->second < due) {
            return VaultCheck::DESTINATION_MISMATCH;
        }
    }

    // Exact custody equation per asset: inflow == receipts + change.
    std::map<AssetId, CAmount> touched{vault_in};
    for (const auto& [asset, sum] : receipt_sums) touched.try_emplace(asset, 0);
    for (const auto& [asset, sum] : vault_change) touched.try_emplace(asset, 0);
    for (const auto& [asset, unused] : touched) {
        const CAmount in{vault_in.count(asset) ? vault_in.at(asset) : 0};
        const CAmount out{receipt_sums.count(asset) ? receipt_sums.at(asset) : 0};
        const CAmount change{vault_change.count(asset) ? vault_change.at(asset) : 0};
        if (out > MAX_MONEY - change) return VaultCheck::AMOUNT_OVERFLOW;
        if (in != out + change) return VaultCheck::CHANGE_MISMATCH;
    }

    if (consumed_out) *consumed_out = *receipt_ids;
    return VaultCheck::OK;
}

} // namespace modern

#endif // B3COIN_MODERN_VAULT_H
