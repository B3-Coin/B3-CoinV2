// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Native coloured-asset model (simple v1, owner rulings 2026-08-22):
//! chain-bound rule-bound AssetId, the immutable genesis record, explicit
//! issuance via the creation action minting exactly the declared supply,
//! exact transfer conservation, explicit burn, native-only fees, overflow
//! safety, and the fail-closed test-only activation.

#include <modern/asset.h>

#include <hash.h>
#include <modern/chain_domain.h>
#include <modern/creation_action.h>
#include <modern/policy.h>
#include <modern/proof.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

BOOST_AUTO_TEST_SUITE(asset_conservation_tests)

namespace {

constexpr int SYNTHETIC_H{1000};
constexpr int MODERN_HEIGHT{SYNTHETIC_H + 1};
const uint256 TEST_GENESIS{"00000000000000000000000000000000000000000000000000000000000000c1"};
const uint256 TEST_X{"00000000000000000000000000000000000000000000000000000000000000c2"};

Consensus::Params B3Params()
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = SYNTHETIC_H + 1;
    params.hashGenesisBlock = TEST_GENESIS;
    params.legacy_final_hash = TEST_X; // pinned: the chain domain exists
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

uint256 TestDomain() { return *modern::ModernChainDomain(TEST_GENESIS, TEST_X); }

COutPoint DefiningPrevout()
{
    return COutPoint{
        Txid::FromUint256(uint256{"00000000000000000000000000000000000000000000000000000000000000b3"}), 0};
}

modern::AssetGenesisV1 Genesis(const uint64_t max_supply = 1000, const uint8_t decimals = 0)
{
    return modern::AssetGenesisV1{.max_supply = max_supply, .decimals = decimals,
                                  .issuance_mode = modern::ASSET_ISSUANCE_MODE_GENESIS_FIXED};
}

//! The asset issued by spending DefiningPrevout() with Genesis(1000).
modern::AssetId Asset()
{
    return modern::AssetIdV1(TestDomain(), DefiningPrevout(),
                             modern::AssetGenesisCommitment(Genesis()));
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

//! A v2 issuance transition: spends `first_prevout` and declares `g`.
modern::ModernTransitionV2 Issue(const COutPoint& first_prevout, const modern::AssetGenesisV1& g)
{
    modern::ModernTransitionV2 t;
    t.inputs.resize(1);
    t.inputs[0].prevout = first_prevout;
    t.creation_actions.push_back(modern::MakeAssetIssuanceAction(g));
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

//! The genesis record: bounds, strict 10-byte codec, and commitment.
BOOST_AUTO_TEST_CASE(genesis_record_is_bounded_and_strictly_coded)
{
    BOOST_CHECK(modern::AssetGenesisValid(Genesis()));
    BOOST_CHECK(modern::AssetGenesisValid(Genesis(MAX_MONEY, modern::ASSET_MAX_DECIMALS)));
    BOOST_CHECK(!modern::AssetGenesisValid(Genesis(0)));
    BOOST_CHECK(!modern::AssetGenesisValid(Genesis(static_cast<uint64_t>(MAX_MONEY) + 1)));
    BOOST_CHECK(!modern::AssetGenesisValid(Genesis(1000, modern::ASSET_MAX_DECIMALS + 1)));
    // Reserved future modes (room for PoW-minable, authority-minted and
    // bridge-backed assets) and any mode parameters are refused in v1.
    modern::AssetGenesisV1 pow_mode{Genesis()};
    pow_mode.issuance_mode = modern::ASSET_ISSUANCE_MODE_POW_MINED;
    BOOST_CHECK(!modern::AssetGenesisValid(pow_mode));
    modern::AssetGenesisV1 with_params{Genesis()};
    with_params.mode_params = {0x01};
    BOOST_CHECK(!modern::AssetGenesisValid(with_params));

    // Codec: exactly ASSET_GENESIS_V1_SIZE bytes, round trip, strict size.
    const modern::CreationAction action{modern::MakeAssetIssuanceAction(Genesis(123456, 8))};
    BOOST_CHECK_EQUAL(action.action_type, modern::CREATION_ACTION_ASSET_ISSUANCE);
    BOOST_CHECK_EQUAL(action.action_version, modern::ASSET_ISSUANCE_ACTION_VERSION_V1);
    BOOST_CHECK_EQUAL(action.payload.size(), modern::ASSET_GENESIS_V1_SIZE); // 11 bytes, empty mode_params
    BOOST_CHECK(modern::IsKnownCreationAction(action.action_type, action.action_version));
    modern::AssetGenesisV1 decoded;
    std::string error;
    BOOST_CHECK(modern::DecodeAssetIssuanceAction(action, decoded, error));
    BOOST_CHECK(decoded == Genesis(123456, 8));
    modern::CreationAction trailing{action};
    trailing.payload.push_back(0x00);
    BOOST_CHECK(!modern::DecodeAssetIssuanceAction(trailing, decoded, error));
    modern::CreationAction truncated{action};
    truncated.payload.pop_back();
    BOOST_CHECK(!modern::DecodeAssetIssuanceAction(truncated, decoded, error));
    modern::CreationAction wrong_type{action};
    wrong_type.action_type = modern::CREATION_ACTION_LEGACY_FN_ISSUANCE;
    BOOST_CHECK(!modern::DecodeAssetIssuanceAction(wrong_type, decoded, error));
    // The layout already carries room for future modes: a record with mode
    // parameters decodes at the codec layer (so a future rule set can read
    // it) but is INVALID in v1; an over-bound blob is refused before use.
    modern::AssetGenesisV1 future{Genesis()};
    future.issuance_mode = modern::ASSET_ISSUANCE_MODE_POW_MINED;
    future.mode_params.assign(32, 0xab);
    BOOST_CHECK(modern::DecodeAssetIssuanceAction(modern::MakeAssetIssuanceAction(future), decoded, error));
    BOOST_CHECK(decoded == future);
    BOOST_CHECK(!modern::AssetGenesisValid(decoded));
    modern::AssetGenesisV1 too_big{future};
    too_big.mode_params.assign(modern::MAX_ASSET_MODE_PARAMS + 1, 0xab);
    BOOST_CHECK(!modern::DecodeAssetIssuanceAction(modern::MakeAssetIssuanceAction(too_big), decoded, error));
    // A tiny payload whose compact-size CLAIMS millions of parameter bytes
    // is refused by the length bound before any allocation (and before
    // the reader could even run short of data).
    modern::CreationAction bomb{action};
    bomb.payload.assign(action.payload.begin(), action.payload.begin() + 10); // fields, no blob
    bomb.payload.insert(bomb.payload.end(), {0xfe, 0x40, 0x4b, 0x4c, 0x00}); // compact-size 5,000,000
    BOOST_CHECK(!modern::DecodeAssetIssuanceAction(bomb, decoded, error));
    BOOST_CHECK_EQUAL(error, "asset genesis mode parameters exceed the bound");

    // The commitment is the tagged hash of exactly the serialized record.
    HashWriter mirror{TaggedHash("B3/ASSET/GENESIS/V1")};
    mirror << Genesis(123456, 8);
    BOOST_CHECK(modern::AssetGenesisCommitment(Genesis(123456, 8)) == mirror.GetSHA256());
    BOOST_CHECK(modern::AssetGenesisCommitment(Genesis(123456, 8)) !=
                modern::AssetGenesisCommitment(Genesis(123456, 9)));
}

//! The id binds chain, outpoint and rules; reconstructable byte-exactly.
BOOST_AUTO_TEST_CASE(asset_identity_is_chain_and_rule_bound)
{
    const modern::AssetId id{Asset()};
    BOOST_CHECK(id == Asset());
    BOOST_CHECK(!id.IsNull());
    BOOST_CHECK(id != modern::NativeAsset());

    const uint256 commit{modern::AssetGenesisCommitment(Genesis())};
    // Different outpoint → different asset.
    BOOST_CHECK(id != modern::AssetIdV1(TestDomain(), COutPoint{DefiningPrevout().hash, 1}, commit));
    // Different chain domain (another chain / fork) → different asset.
    const uint256 other_domain{*modern::ModernChainDomain(TEST_GENESIS, uint256::ONE)};
    BOOST_CHECK(id != modern::AssetIdV1(other_domain, DefiningPrevout(), commit));
    // Different rules (supply or decimals) → different asset: nobody can
    // reissue "the same asset" with other rules.
    BOOST_CHECK(id != modern::AssetIdV1(TestDomain(), DefiningPrevout(),
                                        modern::AssetGenesisCommitment(Genesis(1001))));
    BOOST_CHECK(id != modern::AssetIdV1(TestDomain(), DefiningPrevout(),
                                        modern::AssetGenesisCommitment(Genesis(1000, 2))));
    // Preimage pinned: tag || domain || outpoint || genesis commitment.
    HashWriter mirror{TaggedHash("B3/ASSET/V1")};
    mirror << TestDomain() << DefiningPrevout() << commit;
    BOOST_CHECK(id == mirror.GetSHA256());
    // No pinned chain, no asset id (the domain itself fails closed).
    BOOST_CHECK(!modern::ModernChainDomain(TEST_GENESIS, uint256{}).has_value());
}

BOOST_AUTO_TEST_CASE(issuance_mints_exactly_the_declared_supply_once)
{
    const Consensus::Params params{B3ParamsActive()};
    const modern::AssetId asset{Asset()};
    const std::vector<modern::ModernOutput> issue_prevs{Out(modern::NativeAsset(), 100)};

    // Genesis: declare 1000, mint 1000 (600 + 400) with native change.
    modern::ModernTransitionV2 issue{Issue(DefiningPrevout(), Genesis())};
    issue.outputs.push_back(Out(asset, 600));
    issue.outputs.push_back(Out(asset, 400));
    issue.outputs.push_back(Out(modern::NativeAsset(), 90)); // fee: 10 native
    BOOST_CHECK(modern::CheckAssetConservation(issue_prevs, issue, MODERN_HEIGHT, params) ==
                modern::AssetCheck::OK);

    // Under- or over-minting the declaration is invalid.
    modern::ModernTransitionV2 under{Issue(DefiningPrevout(), Genesis())};
    under.outputs.push_back(Out(asset, 999));
    BOOST_CHECK(modern::CheckAssetConservation(issue_prevs, under, MODERN_HEIGHT, params) ==
                modern::AssetCheck::ISSUANCE_INVALID);
    modern::ModernTransitionV2 over{Issue(DefiningPrevout(), Genesis())};
    over.outputs.push_back(Out(asset, 1001));
    BOOST_CHECK(modern::CheckAssetConservation(issue_prevs, over, MODERN_HEIGHT, params) ==
                modern::AssetCheck::ISSUANCE_INVALID);
    // A declaration that mints nothing is invalid too.
    modern::ModernTransitionV2 empty{Issue(DefiningPrevout(), Genesis())};
    empty.outputs.push_back(Out(modern::NativeAsset(), 90));
    BOOST_CHECK(modern::CheckAssetConservation(issue_prevs, empty, MODERN_HEIGHT, params) ==
                modern::AssetCheck::ISSUANCE_INVALID);

    // Without the action nothing can be minted — not in v2, not in v1.
    modern::ModernTransitionV2 no_action{Issue(DefiningPrevout(), Genesis())};
    no_action.creation_actions.clear();
    no_action.outputs.push_back(Out(asset, 1000));
    BOOST_CHECK(modern::CheckAssetConservation(issue_prevs, no_action, MODERN_HEIGHT, params) ==
                modern::AssetCheck::UNAUTHORIZED_MINT);
    modern::ModernTransition v1_mint{Spend(DefiningPrevout())};
    v1_mint.outputs.push_back(Out(asset, 1000));
    BOOST_CHECK(modern::CheckAssetConservation(issue_prevs, v1_mint, MODERN_HEIGHT, params) ==
                modern::AssetCheck::UNAUTHORIZED_MINT);

    // Two declarations in one transition are invalid.
    modern::ModernTransitionV2 twice{Issue(DefiningPrevout(), Genesis())};
    twice.creation_actions.push_back(modern::MakeAssetIssuanceAction(Genesis(5)));
    twice.outputs.push_back(Out(asset, 1000));
    BOOST_CHECK(modern::CheckAssetConservation(issue_prevs, twice, MODERN_HEIGHT, params) ==
                modern::AssetCheck::ISSUANCE_INVALID);

    // Minting any id other than the one derived from THIS first input and
    // THIS record is unauthorized, even with a declaration present.
    const COutPoint other{DefiningPrevout().hash, 7};
    modern::ModernTransitionV2 wrong_id{Issue(DefiningPrevout(), Genesis())};
    wrong_id.outputs.push_back(Out(
        modern::AssetIdV1(TestDomain(), other, modern::AssetGenesisCommitment(Genesis())), 1000));
    BOOST_CHECK(modern::CheckAssetConservation(issue_prevs, wrong_id, MODERN_HEIGHT, params) ==
                modern::AssetCheck::UNAUTHORIZED_MINT);

    // Invalid records (zero supply, too many decimals, any authority) are
    // refused at issuance.
    for (const modern::AssetGenesisV1& bad :
         {Genesis(0), Genesis(1000, modern::ASSET_MAX_DECIMALS + 1)}) {
        modern::ModernTransitionV2 t{Issue(DefiningPrevout(), bad)};
        t.outputs.push_back(Out(asset, 1000));
        BOOST_CHECK(modern::CheckAssetConservation(issue_prevs, t, MODERN_HEIGHT, params) ==
                    modern::AssetCheck::ISSUANCE_INVALID);
    }
    modern::AssetGenesisV1 pow_mode{Genesis()};
    pow_mode.issuance_mode = modern::ASSET_ISSUANCE_MODE_POW_MINED; // reserved for V2
    modern::ModernTransitionV2 reserved_mode{Issue(DefiningPrevout(), pow_mode)};
    reserved_mode.outputs.push_back(Out(asset, 1000));
    BOOST_CHECK(modern::CheckAssetConservation(issue_prevs, reserved_mode, MODERN_HEIGHT, params) ==
                modern::AssetCheck::ISSUANCE_INVALID);

    // Reissuance is impossible: a later transition has a different first
    // input, so its declaration defines a DIFFERENT asset, and the original
    // id can never gain supply again.
    modern::ModernTransitionV2 remint{Issue(other, Genesis())};
    remint.outputs.push_back(Out(asset, 1));
    BOOST_CHECK(modern::CheckAssetConservation({Out(modern::NativeAsset(), 5)}, remint,
                                               MODERN_HEIGHT, params) ==
                modern::AssetCheck::UNAUTHORIZED_MINT);
    const modern::AssetId second{
        modern::AssetIdV1(TestDomain(), other, modern::AssetGenesisCommitment(Genesis()))};
    BOOST_CHECK(second != asset);
    modern::ModernTransitionV2 second_issue{Issue(other, Genesis())};
    second_issue.outputs.push_back(Out(second, 1000));
    BOOST_CHECK(modern::CheckAssetConservation({Out(modern::NativeAsset(), 5)}, second_issue,
                                               MODERN_HEIGHT, params) ==
                modern::AssetCheck::OK);
}

//! Issuance fails closed while the chain is unpinned; transfers don't need
//! the domain and keep working.
BOOST_AUTO_TEST_CASE(issuance_requires_a_pinned_chain)
{
    Consensus::Params params{B3ParamsActive()};
    params.legacy_final_hash.reset();
    modern::ModernTransitionV2 issue{Issue(DefiningPrevout(), Genesis())};
    issue.outputs.push_back(Out(Asset(), 1000));
    BOOST_CHECK(modern::CheckAssetConservation({Out(modern::NativeAsset(), 100)}, issue,
                                               MODERN_HEIGHT, params) ==
                modern::AssetCheck::ISSUANCE_INVALID);
    modern::ModernTransition transfer{Spend(COutPoint{DefiningPrevout().hash, 9})};
    transfer.outputs.push_back(Out(Asset(), 1000));
    BOOST_CHECK(modern::CheckAssetConservation({Out(Asset(), 1000)}, transfer, MODERN_HEIGHT,
                                               params) == modern::AssetCheck::OK);
}

BOOST_AUTO_TEST_CASE(transfer_conserves_exactly)
{
    const Consensus::Params params{B3ParamsActive()};
    const modern::AssetId asset{Asset()};
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
    const modern::AssetId asset{Asset()};
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

    // Native surplus is never issuance — not even with a declaration.
    modern::ModernTransition native_mint{Spend(other)};
    native_mint.outputs.push_back(Out(modern::NativeAsset(), 101));
    BOOST_CHECK(modern::CheckAssetConservation({Out(modern::NativeAsset(), 100)}, native_mint,
                                               MODERN_HEIGHT, params) ==
                modern::AssetCheck::CONSERVATION_MISMATCH);
    modern::ModernTransitionV2 declared_native{Issue(other, Genesis())};
    declared_native.outputs.push_back(Out(modern::NativeAsset(), 101));
    BOOST_CHECK(modern::CheckAssetConservation({Out(modern::NativeAsset(), 100)}, declared_native,
                                               MODERN_HEIGHT, params) ==
                modern::AssetCheck::CONSERVATION_MISMATCH);
}

BOOST_AUTO_TEST_CASE(arithmetic_is_overflow_safe)
{
    const Consensus::Params params{B3ParamsActive()};
    const modern::AssetId asset{Asset()};
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
