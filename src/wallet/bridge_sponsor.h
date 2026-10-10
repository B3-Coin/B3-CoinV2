// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_WALLET_BRIDGE_SPONSOR_H
#define BITCOIN_WALLET_BRIDGE_SPONSOR_H

#include <coins.h>
#include <consensus/amount.h>
#include <modern/policy.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <util/result.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

/**
 * Owner ruling R10 (open issue OI-8): sponsored native-B3 fees for bridge V2
 * burns (routes d1 and d3).
 *
 * A bUSD-only user builds the burn with only bUSD inputs and the outputs
 * [burn, BRIDGE_RECORD binding, bUSD tip to a named relayer, bUSD change], and
 * signs every input SIGHASH_ALL|SIGHASH_ANYONECANPAY. That signature commits
 * every output, so no one can change the outputs: a relayer appends one
 * native-B3 fee input, signs it SIGHASH_ALL and broadcasts. Each user
 * signature commits only its own input, though: with more than one user
 * input, anyone may also drop or replace a user input as long as it supplies
 * the same bUSD itself, so one multi-input partial can confirm as more than
 * one burn, each with the same outputs (the user is never charged more than
 * one partial's inputs, and each burn is real). From the envelope's own
 * sponsored sighash pin (>= V) the node accepts the user's signature byte
 * only under SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL_OUTPUTS, which a bridge V2
 * kind 6 burn selects (node::BridgeV2SponsoredSighashAllowed; regtest
 * -b3bridgepins only); mainnet and testnet are unchanged.
 *
 * The helpers here are pure (no chain, no wallet lock); the wallet's coin
 * map for signing is CWallet::CoinsForInputs. The node's own prevalidation
 * still decides whether the partial is an admissible V2 burn (entry, gate,
 * reserve).
 */
namespace wallet {

//! The sighash a sponsored-burn user signs every input with.
inline constexpr int SPONSORED_BURN_SIGHASH{SIGHASH_ALL | SIGHASH_ANYONECANPAY};

//! The relayer's bUSD tip output in a sponsored partial.
struct SponsoredBurnTip {
    uint32_t vout{0};
    CAmount amount{0};
};

/**
 * Structural relayer-side check of a sponsored-burn partial, before any fee
 * input is added. Rules, in order, each failing with a stable error prefix:
 *  - `sponsor-no-inputs`: the partial spends nothing.
 *  - `sponsor-not-a-bridge-burn`: not exactly one MPA record, the record is
 *    not a kind 6 burn under the V2 record set, or its binding output is not
 *    the exact, sole BRIDGE_RECORD output (mirrors mpa.h).
 *  - `sponsor-input-unsigned: input N`: an input has neither scriptSig nor
 *    witness.
 *  - `sponsor-native-output: output N`: an output pays native B3. The partial
 *    may pay none, so the fee input can fund nothing but the fee.
 *  - `sponsor-tip-missing`, `sponsor-tip-ambiguous: outputs N and M`: not
 *    exactly one output satisfies `pays_relayer`.
 *  - `sponsor-tip-not-busd: output N`: that output is not an OWNER output of
 *    `busd_v2`.
 *  - `sponsor-tip-below-minimum: tip A < B`.
 * Signatures are not checked here; see VerifySponsoredUserInputs.
 */
util::Result<SponsoredBurnTip> CheckSponsoredBurnPartial(
    const CTransaction& partial, const modern::AssetId& busd_v2,
    CAmount min_tip, const std::function<bool(const CTxOut&)>& pays_relayer);

/**
 * Verify the user's inputs [0, user_inputs) of a completed sponsored burn
 * under STANDARD_SCRIPT_VERIFY_FLAGS | SCRIPT_VERIFY_BRIDGE_SIGHASH_ALL_OUTPUTS,
 * so each must be signed SIGHASH_ALL or SIGHASH_ALL|SIGHASH_ANYONECANPAY (or
 * the Schnorr default) over the completed transaction's outputs. `spent` holds
 * the coin of every input of `completed`, in order. A coin created at or
 * below `legacy_final_height` is refused (`sponsor-legacy-input: input N`):
 * bUSD never predates H. Coins above H run their asset owner suffix, as
 * consensus does; with no legacy boundary the full script runs, again as
 * consensus does. A failing input gives `sponsor-signature-invalid: input N`.
 */
util::Result<void> VerifySponsoredUserInputs(
    const CTransaction& completed, const std::vector<Coin>& spent,
    size_t user_inputs, std::optional<int> legacy_final_height);

/**
 * Pick the relayer fee coin: the index of the smallest value in
 * [needed, max_fee], ties broken by the lowest index; nullopt if none. A pool
 * of pre-sized coins with max_fee at the pool value means the whole coin is
 * the fee and the completed burn needs no change output.
 */
std::optional<size_t> SelectSponsorFeeCoin(std::span<const CAmount> values,
                                           CAmount needed, CAmount max_fee);

} // namespace wallet

#endif // BITCOIN_WALLET_BRIDGE_SPONSOR_H
