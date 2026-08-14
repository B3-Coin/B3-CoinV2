// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Native coloured-asset conservation: deterministic fixed-supply
//! issuance, exact transfer conservation, explicit burn, native-only
//! fees, overflow safety, and the fail-closed test-only activation.

#include <modern/asset.h>

#include <modern/policy.h>
#include <modern/proof.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <vector>

BOOST_AUTO_TEST_SUITE(asset_conservation_tests)

namespace {

constexpr int SYNTHETIC_H{1000};
constexpr int MODERN_HEIGHT{SYNTHETIC_H + 1};

Consensus::Params B3Params()
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = SYNTHETIC_H + 1;
    return params;
}

// The asset policy set (BURN / DEX_VAULT / conservation) activated for a test,
// via the per-instance Params field that replaces the former global switch.
Consensus::Params B3ParamsActive()
{
    Consensus::Params params{B3Params()};
    params.test_only_asset_policies_active = true;
    return params;
}

COutPoint DefiningPrevout()
{
    return COutPoint{
        Txid::FromUint256(uint256{"00000000000000000000000000000000000000000000000000000000000000b3"}), 0};
}

modern::ModernOutput Out(const modern::AssetId& asset, const CAmount amount,
                         const modern::PolicyType type = modern::PolicyType::OWNER)
{
    modern::ModernOutput out;
    out.asset = asset;
    out.amount = amount;
    out.policy_type = static_cast<uint16_t>(type);
    out.policy_version = modern::POLICY_VERSION_V1;
    if (type == modern::PolicyType::OWNER) {
        out.policy_commitment =
            uint256{"00000000000000000000000000000000000000000000000000000000000000aa"};
    }
    return out;
}

modern::ModernTransition Spend(const COutPoint& first_prevout, const size_t n_inputs = 1)
{
    modern::ModernTransition t;
    t.inputs.resize(n_inputs);
    t.inputs[0].prevout = first_prevout;
    for (size_t i{1}; i < n_inputs; ++i) {
        t.inputs[i].prevout = COutPoint{first_prevout.hash, static_cast<uint32_t>(i + 1)};
        t.inputs[i].proof_index = static_cast<uint32_t>(i);
    }
    return t;
}

} // namespace

BOOST_AUTO_TEST_CASE(rules_are_inactive_by_default)
{
    const Consensus::Params params{B3Params()};
    modern::ModernTransition t{Spend(DefiningPrevout())};
    t.outputs.push_back(Out(modern::NativeAsset(), 100));
    BOOST_CHECK(modern::CheckAssetConservation({Out(modern::NativeAsset(), 100)}, t,
                                               MODERN_HEIGHT, params) ==
                modern::AssetCheck::NOT_ACTIVE);
    // The BURN policy is likewise unactivated outside tests.
    BOOST_CHECK(!modern::IsActivatedPolicy(static_cast<uint16_t>(modern::PolicyType::BURN),
                                           modern::POLICY_VERSION_V1));
}

BOOST_AUTO_TEST_CASE(issuance_identity_is_deterministic)
{
    const modern::AssetId id{modern::IssuanceAssetId(DefiningPrevout())};
    BOOST_CHECK(id == modern::IssuanceAssetId(DefiningPrevout()));
    BOOST_CHECK(!id.IsNull());
    BOOST_CHECK(id != modern::NativeAsset());
    // A different defining outpoint yields a different asset.
    BOOST_CHECK(id != modern::IssuanceAssetId(COutPoint{DefiningPrevout().hash, 1}));
}

BOOST_AUTO_TEST_CASE(fixed_supply_issuance_then_no_reissuance)
{
    const Consensus::Params params{B3ParamsActive()};
    const modern::AssetId asset{modern::IssuanceAssetId(DefiningPrevout())};

    // Issue 1000 units (600 + 400) with native change and a native fee.
    modern::ModernTransition issue{Spend(DefiningPrevout())};
    issue.outputs.push_back(Out(asset, 600));
    issue.outputs.push_back(Out(asset, 400));
    issue.outputs.push_back(Out(modern::NativeAsset(), 90)); // fee: 10 native
    const std::vector<modern::ModernOutput> issue_prevs{Out(modern::NativeAsset(), 100)};
    BOOST_CHECK(modern::CheckAssetConservation(issue_prevs, issue, MODERN_HEIGHT, params) ==
                modern::AssetCheck::OK);

    // A later transition (different first prevout, since the defining
    // outpoint is consumed forever) can never mint this asset again.
    const COutPoint other{DefiningPrevout().hash, 7};
    modern::ModernTransition remint{Spend(other)};
    remint.outputs.push_back(Out(asset, 1));
    BOOST_CHECK(modern::CheckAssetConservation({Out(modern::NativeAsset(), 5)}, remint,
                                               MODERN_HEIGHT, params) ==
                modern::AssetCheck::UNAUTHORIZED_MINT);

    // Minting any id other than the first input's issuance id fails too.
    modern::ModernTransition wrong_id{Spend(DefiningPrevout())};
    wrong_id.outputs.push_back(Out(modern::IssuanceAssetId(other), 1));
    BOOST_CHECK(modern::CheckAssetConservation(issue_prevs, wrong_id, MODERN_HEIGHT, params) ==
                modern::AssetCheck::UNAUTHORIZED_MINT);
}

BOOST_AUTO_TEST_CASE(transfer_conserves_exactly)
{
    const Consensus::Params params{B3ParamsActive()};
    const modern::AssetId asset{modern::IssuanceAssetId(DefiningPrevout())};
    const COutPoint other{DefiningPrevout().hash, 9};
    const std::vector<modern::ModernOutput> prevs{Out(asset, 1000),
                                                  Out(modern::NativeAsset(), 50)};

    modern::ModernTransition ok{Spend(other, 2)};
    ok.outputs.push_back(Out(asset, 600));
    ok.outputs.push_back(Out(asset, 400));
    ok.outputs.push_back(Out(modern::NativeAsset(), 45)); // native fee 5
    BOOST_CHECK(modern::CheckAssetConservation(prevs, ok, MODERN_HEIGHT, params) ==
                modern::AssetCheck::OK);

    // Implicit loss is invalid: burns must be explicit.
    modern::ModernTransition lossy{Spend(other, 2)};
    lossy.outputs.push_back(Out(asset, 999));
    BOOST_CHECK(modern::CheckAssetConservation(prevs, lossy, MODERN_HEIGHT, params) ==
                modern::AssetCheck::CONSERVATION_MISMATCH);

    // Inflation outside issuance is invalid.
    modern::ModernTransition inflating{Spend(other, 2)};
    inflating.outputs.push_back(Out(asset, 1001));
    BOOST_CHECK(modern::CheckAssetConservation(prevs, inflating, MODERN_HEIGHT, params) ==
                modern::AssetCheck::UNAUTHORIZED_MINT);
}

BOOST_AUTO_TEST_CASE(explicit_burn_is_exact_and_visible)
{
    const Consensus::Params params{B3ParamsActive()};
    const modern::AssetId asset{modern::IssuanceAssetId(DefiningPrevout())};
    const COutPoint other{DefiningPrevout().hash, 11};
    const std::vector<modern::ModernOutput> prevs{Out(asset, 1000)};

    modern::ModernTransition burn{Spend(other)};
    burn.outputs.push_back(Out(asset, 600));
    burn.outputs.push_back(Out(asset, 400, modern::PolicyType::BURN));
    BOOST_CHECK(modern::CheckAssetConservation(prevs, burn, MODERN_HEIGHT, params) ==
                modern::AssetCheck::OK);

    // The burn output itself is canonical: params-free, zero commitment.
    modern::ModernOutput bad_burn{Out(asset, 400, modern::PolicyType::BURN)};
    bad_burn.policy_commitment =
        uint256{"0000000000000000000000000000000000000000000000000000000000000001"};
    modern::ModernTransition bad{Spend(other)};
    bad.outputs.push_back(Out(asset, 600));
    bad.outputs.push_back(bad_burn);
    BOOST_CHECK(modern::CheckAssetConservation(prevs, bad, MODERN_HEIGHT, params) ==
                modern::AssetCheck::POLICY_INVALID);

    // Burning native B3 is also explicit and exact.
    modern::ModernTransition native_burn{Spend(other)};
    native_burn.outputs.push_back(Out(modern::NativeAsset(), 40, modern::PolicyType::BURN));
    BOOST_CHECK(modern::CheckAssetConservation({Out(modern::NativeAsset(), 50)}, native_burn,
                                               MODERN_HEIGHT, params) ==
                modern::AssetCheck::OK);
}

BOOST_AUTO_TEST_CASE(fees_are_native_only_and_native_never_mints)
{
    const Consensus::Params params{B3ParamsActive()};
    const COutPoint other{DefiningPrevout().hash, 13};

    // Native deficit is the fee.
    modern::ModernTransition fee_ok{Spend(other)};
    fee_ok.outputs.push_back(Out(modern::NativeAsset(), 70));
    BOOST_CHECK(modern::CheckAssetConservation({Out(modern::NativeAsset(), 100)}, fee_ok,
                                               MODERN_HEIGHT, params) ==
                modern::AssetCheck::OK);

    // Native surplus is never issuance.
    modern::ModernTransition native_mint{Spend(other)};
    native_mint.outputs.push_back(Out(modern::NativeAsset(), 101));
    BOOST_CHECK(modern::CheckAssetConservation({Out(modern::NativeAsset(), 100)}, native_mint,
                                               MODERN_HEIGHT, params) ==
                modern::AssetCheck::CONSERVATION_MISMATCH);
}

BOOST_AUTO_TEST_CASE(arithmetic_is_overflow_safe)
{
    const Consensus::Params params{B3ParamsActive()};
    const modern::AssetId asset{modern::IssuanceAssetId(DefiningPrevout())};
    const COutPoint other{DefiningPrevout().hash, 17};

    // Two max-value inputs of the same asset would overflow a naive sum.
    const std::vector<modern::ModernOutput> prevs{Out(asset, MAX_MONEY), Out(asset, MAX_MONEY)};
    modern::ModernTransition t{Spend(other, 2)};
    t.outputs.push_back(Out(asset, 1));
    BOOST_CHECK(modern::CheckAssetConservation(prevs, t, MODERN_HEIGHT, params) ==
                modern::AssetCheck::AMOUNT_OVERFLOW);

    // Live + burn totals are guarded on the output side too.
    modern::ModernTransition out_overflow{Spend(other)};
    out_overflow.outputs.push_back(Out(asset, MAX_MONEY));
    out_overflow.outputs.push_back(Out(asset, MAX_MONEY, modern::PolicyType::BURN));
    BOOST_CHECK(modern::CheckAssetConservation({Out(asset, MAX_MONEY)}, out_overflow,
                                               MODERN_HEIGHT, params) ==
                modern::AssetCheck::AMOUNT_OVERFLOW);
}

BOOST_AUTO_TEST_SUITE_END()
