// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_WALLET_RPC_FINALITY_RECOVERY_STATUS_H
#define BITCOIN_WALLET_RPC_FINALITY_RECOVERY_STATUS_H

#include <interfaces/chain.h>
#include <univalue.h>

#include <utility>

namespace wallet {

//! Pure presentation of an authoritative running-signer snapshot. No error
//! parsing, key access, journal opening, or recovery authorization occurs here.
inline UniValue FinalityRecoveryStatusToJSON(
    const interfaces::StakingStatus& staking,
    const std::array<unsigned char, 32>& wallet_validator)
{
    UniValue obj{UniValue::VOBJ};
    const bool own_signer{staking.running && staking.validator_key &&
                          *staking.validator_key == wallet_validator};
    const bool available{own_signer && staking.finality_recovery &&
                          staking.finality_recovery->observed_tip_height >= 0};
    obj.pushKV("available", available);
    if (!available) {
        obj.pushKV("state", !staking.running ? "signer_not_running" :
                             !own_signer ? "different_wallet_signer" :
                                           "awaiting_signer_observation");
        return obj;
    }
    const auto& status{*staking.finality_recovery};
    obj.pushKV("state", status.state);
    obj.pushKV("source", "running_signer_snapshot");
    UniValue observed{UniValue::VOBJ};
    observed.pushKV("height", status.observed_tip_height);
    observed.pushKV("hash", status.observed_tip_hash.GetHex());
    obj.pushKV("observed_tip", std::move(observed));
    obj.pushKV("journal_open", status.journal_open);
    obj.pushKV("journal_present", status.journal_present);
    obj.pushKV("permanent_error", status.permanent_error);
    obj.pushKV("blocked_on_orphan_vote", status.blocked_on_orphan_vote);
    if (status.last_signed_height >= 0) {
        UniValue vote{UniValue::VOBJ};
        vote.pushKV("height", status.last_signed_height);
        vote.pushKV("hash", status.last_signed_hash.GetHex());
        vote.pushKV("digest", status.last_signed_digest.GetHex());
        obj.pushKV("last_vote", std::move(vote));
    }
    if (status.lock_height) {
        UniValue lock{UniValue::VOBJ};
        lock.pushKV("height", *status.lock_height);
        lock.pushKV("hash", status.lock_hash.GetHex());
        lock.pushKV("digest", status.lock_digest.GetHex());
        lock.pushKV("epoch", status.lock_epoch);
        lock.pushKV("signing_set_hash", status.lock_signing_set_hash.GetHex());
        lock.pushKV("successor_set_hash", status.lock_successor_set_hash.GetHex());
        if (status.current_chain_hash) lock.pushKV("current_chain_hash", status.current_chain_hash->GetHex());
        obj.pushKV("lock", std::move(lock));

        UniValue required{UniValue::VOBJ};
        required.pushKV("type", "newer_included_same_epoch_same_set_certificate");
        required.pushKV("checkpoint_height_strictly_greater_than", *status.lock_height);
        required.pushKV("epoch", status.lock_epoch);
        required.pushKV("signing_set_hash", status.lock_signing_set_hash.GetHex());
        obj.pushKV("required_proof", std::move(required));

        UniValue checks{UniValue::VOBJ};
        checks.pushKV("included_on_active_chain", status.certificate_included);
        checks.pushKV("strictly_newer", status.certificate_strictly_newer);
        checks.pushKV("same_epoch", status.certificate_same_epoch);
        checks.pushKV("same_signing_set", status.certificate_same_set);
        checks.pushKV("checkpoint_reconstructible", status.certificate_reconstructible);
        obj.pushKV("latest_certificate_checks", std::move(checks));
    }
    if (status.finalized_height) {
        UniValue finalized{UniValue::VOBJ};
        finalized.pushKV("height", *status.finalized_height);
        finalized.pushKV("hash", status.finalized_hash.GetHex());
        finalized.pushKV("epoch", status.finalized_epoch);
        finalized.pushKV("included_at_height", status.finalized_certified_at);
        if (status.finalized_signing_set_hash) finalized.pushKV("signing_set_hash", status.finalized_signing_set_hash->GetHex());
        obj.pushKV("observed_finalized", std::move(finalized));
    }
    return obj;
}

} // namespace wallet

#endif // BITCOIN_WALLET_RPC_FINALITY_RECOVERY_STATUS_H
