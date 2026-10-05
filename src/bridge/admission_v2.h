// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_BRIDGE_ADMISSION_V2_H
#define B3COIN_BRIDGE_ADMISSION_V2_H

#include <bridge/admission.h>
#include <consensus/amount.h>
#include <consensus/bridge_params.h>
#include <modern/policy.h>

#include <cstdint>

namespace bridge {

/**
 * Bridge V2 deposit admission for the single Vault V2 entry
 * (doc/design/b3-bridge-v2-node-changes.md sections 3.3 and 4). The receipt,
 * execution ancestry and finality proof must already be verified against the
 * V2 light-client store; exactly-once state stays with the caller
 * (node::BridgeStateIndex), which supplies the consumed budget.
 *
 * The rules are V1's admission rules applied to the V2 entry, in V1's order:
 * the entry must be complete and active (height >= V), the event block at or
 * after the vault's deployment block, the origin chain, emitting vault and
 * token exactly the entry's, the recipient a RECIPIENT_V1 P2PKH, the amount
 * an exact 6 -> 6 decimal conversion, and the per-block and per-epoch caps
 * respected. The nullifier is (origin chain, vault, deposit id) in
 * namespace 2, separate from every V1 key.
 *
 * Mintability rule (node changes section 4): Vault V2 never refunds, so every
 * check here is one the vault already enforces (RECIPIENT_V1, amount > 0,
 * amount <= MAX_DEPOSIT_RAW <= block cap <= epoch cap), except the caps on
 * the SUM of mints, which only delay a mint to a later block or epoch. No
 * separate per-deposit MAX_DEPOSIT_RAW check is added: it would only repeat
 * the vault's check and could strand a deposit if the B3 pin drifted below
 * the vault's immutable.
 *
 * A separate function rather than a BridgeAssetRegistryEntry, because
 * BridgeAssetRegistryEntryValid requires a vault runtime code hash that OI-3
 * leaves unpinned on regtest fixtures; BridgeV2Params::Valid() carries the
 * V2 completeness rules instead (both code hashes on origin chain 1).
 */
inline BridgeAdmissionResult AdmitDepositV2(
    const Consensus::BridgeV2Params& params, const modern::AssetId& asset_v2,
    const ProvenBridgeDeposit& deposit, const int32_t b3_height,
    const BridgeMintBudget& used, BridgeMintAuthorization& out)
{
    out = {};
    if (!params.Valid() || b3_height < params.activation_height) {
        return BridgeAdmissionResult::REGISTRY_INACTIVE;
    }
    // The caller derives the bUSD V2 AssetId from the chain domain; a missing
    // domain (no legacy final hash) must never mint an unnamed asset.
    if (asset_v2.IsNull()) return BridgeAdmissionResult::CONFIGURATION_INCOMPLETE;

    const Consensus::BridgeV2VaultEntry& entry{params.vault};
    if (deposit.execution_block_number < entry.origin_deployment_block) {
        return BridgeAdmissionResult::BEFORE_DEPLOYMENT;
    }
    if (deposit.origin_chain_id != entry.origin_chain_id) {
        return BridgeAdmissionResult::ORIGIN_MISMATCH;
    }
    if (deposit.vault_address != entry.vault_address) {
        return BridgeAdmissionResult::VAULT_MISMATCH;
    }
    if (deposit.event.token != entry.token_address) {
        return BridgeAdmissionResult::TOKEN_MISMATCH;
    }
    const auto recipient{DecodeRecipientV1(deposit.event.b3_recipient)};
    if (!recipient) return BridgeAdmissionResult::RECIPIENT_INVALID;
    const auto amount{ConvertRawUnitsExact(deposit.event.amount, entry.origin_decimals,
                                           entry.asset_decimals)};
    if (!amount) return BridgeAdmissionResult::AMOUNT_INVALID;

    const Consensus::BridgeMintCaps& caps{entry.mint_caps};
    if (used.minted_this_block < 0 || used.minted_this_block > caps.max_per_block ||
        *amount > caps.max_per_block - used.minted_this_block) {
        return BridgeAdmissionResult::BLOCK_CAP_EXCEEDED;
    }
    if (used.minted_this_epoch < 0 || used.minted_this_epoch > caps.max_per_epoch ||
        *amount > caps.max_per_epoch - used.minted_this_epoch) {
        return BridgeAdmissionResult::EPOCH_CAP_EXCEEDED;
    }

    out.asset = asset_v2;
    out.amount = *amount;
    out.recipient_script = RecipientV1Script(*recipient);
    out.nullifier = BridgeDepositKey{entry.origin_chain_id, entry.vault_address,
                                     deposit.event.deposit_id,
                                     BRIDGE_NULLIFIER_NAMESPACE_V2};
    return BridgeAdmissionResult::OK;
}

} // namespace bridge

#endif // B3COIN_BRIDGE_ADMISSION_V2_H
