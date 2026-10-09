// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/bridge_sponsor.h>

#include <bridge/proof.h>
#include <modern/asset_output.h>
#include <modern/bridge_binding.h>
#include <policy/policy.h>
#include <script/script_error.h>
#include <script/verify_flags.h>
#include <tinyformat.h>
#include <util/translation.h>

#include <string>
#include <utility>

namespace wallet {
namespace {

//! The R10 script rule a sponsored user input must satisfy: the standard
//! flags plus the bridge rule that admits only full-output sighash modes.
constexpr script_verify_flags SPONSORED_USER_INPUT_FLAGS{
    STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL_OUTPUTS};

util::Error SponsorError(std::string message)
{
    return util::Error{Untranslated(std::move(message))};
}

} // namespace

util::Result<SponsoredBurnTip> CheckSponsoredBurnPartial(
    const CTransaction& partial, const modern::AssetId& busd_v2,
    const CAmount min_tip,
    const std::function<bool(const CTxOut&)>& pays_relayer)
{
    if (partial.vin.empty()) {
        return SponsorError("sponsor-no-inputs: the partial spends no input");
    }

    // The type-10 record is the sole MPA record and its BRIDGE_RECORD output
    // is exact and unique (mpa.h). The user's signatures commit that output,
    // so the record, and with it the recipient and the route, is fixed.
    if (partial.mpa.size() != 1) {
        return SponsorError(strprintf(
            "sponsor-not-a-bridge-burn: %u MPA records, expected exactly the "
            "bridge record",
            partial.mpa.size()));
    }
    const CMpaRecord& record{partial.mpa.front()};
    const auto decoded{
        bridge::DecodeBridgeMpaRecordV1(record, bridge::BRIDGE_MPT_LIMITS_V2)};
    if (!decoded || decoded->kind != bridge::BridgeRecordKindV1::BRIDGE_BURN) {
        return SponsorError(
            "sponsor-not-a-bridge-burn: the MPA record is not a kind 6 bridge "
            "burn");
    }
    std::string binding_error;
    if (!modern::CheckBridgeRecordBinding(partial, record, binding_error)) {
        return SponsorError(
            strprintf("sponsor-not-a-bridge-burn: %s", binding_error));
    }

    for (size_t i{0}; i < partial.vin.size(); ++i) {
        const CTxIn& input{partial.vin[i]};
        if (input.scriptSig.empty() && input.scriptWitness.IsNull()) {
            return SponsorError(strprintf("sponsor-input-unsigned: input %u", i));
        }
    }

    // The relayer's fee input is the only native value it adds. Were the
    // partial to pay native B3, that coin would fund the user's own output.
    for (size_t i{0}; i < partial.vout.size(); ++i) {
        if (partial.vout[i].nValue != 0) {
            return SponsorError(strprintf(
                "sponsor-native-output: output %u pays native B3; a sponsored "
                "partial may pay none",
                i));
        }
    }

    std::optional<uint32_t> tip_vout;
    for (size_t i{0}; i < partial.vout.size(); ++i) {
        if (!pays_relayer(partial.vout[i])) continue;
        if (tip_vout) {
            return SponsorError(strprintf(
                "sponsor-tip-ambiguous: outputs %u and %u both pay the relayer",
                *tip_vout, i));
        }
        tip_vout = static_cast<uint32_t>(i);
    }
    if (!tip_vout) {
        return SponsorError("sponsor-tip-missing: no output pays the relayer");
    }
    const auto tip{modern::ParseAssetOutput(partial.vout[*tip_vout])};
    if (!tip ||
        tip->policy_type != static_cast<uint16_t>(modern::PolicyType::OWNER) ||
        tip->asset != busd_v2) {
        return SponsorError(strprintf(
            "sponsor-tip-not-busd: output %u is not a bUSD V2 owner output",
            *tip_vout));
    }
    if (tip->amount < min_tip) {
        return SponsorError(strprintf("sponsor-tip-below-minimum: tip %d < %d",
                                      tip->amount, min_tip));
    }
    return SponsoredBurnTip{.vout = *tip_vout, .amount = tip->amount};
}

util::Result<void> VerifySponsoredUserInputs(
    const CTransaction& completed, const std::vector<Coin>& spent,
    const size_t user_inputs, const std::optional<int> legacy_final_height)
{
    if (spent.size() != completed.vin.size()) {
        return SponsorError(strprintf(
            "sponsor-spent-mismatch: %u coins for %u inputs", spent.size(),
            completed.vin.size()));
    }
    if (user_inputs == 0 || user_inputs > completed.vin.size()) {
        return SponsorError(strprintf(
            "sponsor-user-inputs-invalid: %u user inputs of %u", user_inputs,
            completed.vin.size()));
    }
    std::vector<CTxOut> spent_outputs;
    spent_outputs.reserve(spent.size());
    for (size_t i{0}; i < spent.size(); ++i) {
        if (spent[i].IsSpent()) {
            return SponsorError(strprintf(
                "sponsor-input-unknown: input %u has no coin", i));
        }
        spent_outputs.push_back(spent[i].out);
    }
    PrecomputedTransactionData txdata;
    txdata.Init(completed, std::move(spent_outputs));

    for (size_t i{0}; i < user_inputs; ++i) {
        const Coin& coin{spent[i]};
        if (legacy_final_height &&
            static_cast<int>(coin.nHeight) <= *legacy_final_height) {
            return SponsorError(strprintf(
                "sponsor-legacy-input: input %u was created at height %u, at "
                "or below the legacy boundary %d",
                i, coin.nHeight, *legacy_final_height));
        }
        const CTxIn& input{completed.vin[i]};
        ScriptError error{SCRIPT_ERR_UNKNOWN_ERROR};
        // As consensus: a post-H coin runs its asset owner suffix; a chain
        // without a legacy boundary runs every coin's full script.
        if (!VerifyScript(input.scriptSig, coin.out.scriptPubKey,
                          &input.scriptWitness, SPONSORED_USER_INPUT_FLAGS,
                          TransactionSignatureChecker{
                              &completed, static_cast<unsigned int>(i),
                              coin.out.nValue, txdata,
                              MissingDataBehavior::FAIL},
                          &error,
                          /*enable_asset_owner=*/legacy_final_height.has_value())) {
            return SponsorError(strprintf(
                "sponsor-signature-invalid: input %u (%s): an output changed "
                "after the user signed, or the input is not signed "
                "SIGHASH_ALL|ANYONECANPAY",
                i, ScriptErrorString(error)));
        }
    }
    return {};
}

std::optional<size_t> SelectSponsorFeeCoin(const std::span<const CAmount> values,
                                           const CAmount needed,
                                           const CAmount max_fee)
{
    std::optional<size_t> best;
    for (size_t i{0}; i < values.size(); ++i) {
        const CAmount value{values[i]};
        if (value < needed || value > max_fee) continue;
        if (!best || value < values[*best]) best = i;
    }
    return best;
}

} // namespace wallet
