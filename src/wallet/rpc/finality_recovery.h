// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_WALLET_RPC_FINALITY_RECOVERY_H
#define BITCOIN_WALLET_RPC_FINALITY_RECOVERY_H

#include <interfaces/chain.h>
#include <node/finality_recovery_options.h>
#include <util/strencodings.h>

namespace wallet {

//! Public data only. This is an operator exception, never a quorum proof.
inline UniValue FinalityRecoveryManifestToJSON(const Consensus::FinalitySignerRecovery& plan)
{
    UniValue result{UniValue::VOBJ};
    result.pushKV("version", 1);
    result.pushKV("chain_domain", plan.chain_domain.GetHex());
    if (plan.validator_key) result.pushKV("validator_key", HexStr(*plan.validator_key));
    result.pushKV("incident_height", plan.incident_height);
    result.pushKV("incident_block_hash", plan.incident_block_hash.GetHex());
    result.pushKV("incident_epoch", plan.incident_epoch);
    result.pushKV("incident_signing_set_hash", plan.incident_signing_set_hash.GetHex());
    result.pushKV("incident_successor_set_hash", plan.incident_successor_set_hash.GetHex());
    result.pushKV("anchor_height", plan.anchor_height);
    result.pushKV("anchor_block_hash", plan.anchor_block_hash.GetHex());
    return result;
}

//! Parse before mutation and require this wallet's existing public identity.
//! Failed parsing or ownership checks leave output unchanged.
inline bool ParseWalletFinalityRecoveryManifest(
    const UniValue& manifest, const UniValue& accepted_anchor,
    const std::array<unsigned char, 32>& wallet_validator,
    Consensus::FinalitySignerRecovery& output, std::string& error)
{
    if (!manifest.isObject() || !accepted_anchor.isStr()) {
        error = "manifest must be an object and accepted_anchor must be a string";
        return false;
    }
    Consensus::FinalitySignerRecovery candidate;
    if (!node::ParseFinalityRecoveryManifest(manifest.write(), accepted_anchor.get_str(), candidate, error)) return false;
    if (!candidate.validator_key || *candidate.validator_key != wallet_validator) {
        error = "Recovery manifest targets a different wallet validator";
        return false;
    }
    output = std::move(candidate);
    return true;
}

//! Informative preflight only. The node repeats these checks under its own
//! start/stop serialization before changing pending configuration.
inline bool CheckWalletFinalityRecoveryControl(
    const interfaces::FinalityRecoveryControl& control,
    const std::array<unsigned char, 32>& wallet_validator, std::string& error)
{
    error.clear();
    if (!control.supported) error = "Finality recovery is unavailable in this node";
    else if (control.running) error = "Stop staking before changing finality recovery configuration";
    else if (control.configured &&
             (!control.configured->validator_key || *control.configured->validator_key != wallet_validator)) {
        error = "The configured recovery belongs to a different wallet validator";
    }
    return error.empty();
}

inline UniValue FinalityRecoveryControlToJSON(
    const interfaces::FinalityRecoveryControl& control,
    const std::optional<std::array<unsigned char, 32>>& wallet_validator)
{
    UniValue result{UniValue::VOBJ};
    const bool own{wallet_validator && control.configured &&
                   control.configured->validator_key == wallet_validator};
    result.pushKV("supported", control.supported);
    result.pushKV("staking_running", control.running);
    result.pushKV("configured_for_wallet", own);
    result.pushKV("configured_for_other_wallet", control.configured.has_value() && !own);
    result.pushKV("persistence", "memory_only; startup options are unchanged");
    if (own) result.pushKV("manifest", FinalityRecoveryManifestToJSON(*control.configured));
    result.pushKV("warning", "Configuration is not recovery. This explicit trust exception does not revoke old signatures or prove that a conflicting certificate cannot exist.");
    return result;
}

} // namespace wallet

#endif // BITCOIN_WALLET_RPC_FINALITY_RECOVERY_H
