// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

//! Versioned B3 Policy Output primitives: activation, era gating, bounds,
//! serialization stability, and the pre-H LEGACY_LOCK view.

#include <modern/policy.h>

#include <coins.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <script/script.h>
#include <streams.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(policy_output_tests)

namespace {

constexpr int SYNTHETIC_H{1000};

Consensus::Params B3Params()
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = SYNTHETIC_H + 1;
    return params;
}

modern::ModernOutput OwnerVector()
{
    modern::ModernOutput out;
    out.asset = uint256{"00000000000000000000000000000000000000000000000000000000000000a5"};
    out.amount = 1'234'567;
    out.policy_type = static_cast<uint16_t>(modern::PolicyType::OWNER);
    out.policy_version = modern::POLICY_VERSION_V1;
    out.policy_commitment = uint256{"00000000000000000000000000000000000000000000000000000000000000c7"};
    return out;
}

//! Frozen serialized bytes of OwnerVector(). Consensus-stable: a change
//! here means the policy-output wire format changed.
const std::string OWNER_VECTOR_HEX{
    "a5000000000000000000000000000000000000000000000000000000000000008"
    "7d612000000000001000100c7000000000000000000000000000000000000000000"
    "0000000000000000000000"};

} // namespace

BOOST_AUTO_TEST_CASE(asset_id_semantics_are_stable)
{
    // 32-byte ids; the native B3 asset is the all-zero id, permanently.
    BOOST_CHECK_EQUAL(modern::NativeAsset().size(), 32U);
    BOOST_CHECK(modern::NativeAsset().IsNull());
    DataStream s;
    s << modern::NativeAsset();
    BOOST_CHECK_EQUAL(s.size(), 32U);
}

BOOST_AUTO_TEST_CASE(serialization_is_frozen)
{
    DataStream s;
    s << OwnerVector();
    BOOST_CHECK_EQUAL(HexStr(s), OWNER_VECTOR_HEX);

    modern::ModernOutput decoded;
    s >> decoded;
    BOOST_CHECK(decoded == OwnerVector());
}

BOOST_AUTO_TEST_CASE(only_activated_policies_are_valid)
{
    const Consensus::Params params{B3Params()};
    const int modern_height{SYNTHETIC_H + 1};

    modern::ModernOutput out{OwnerVector()};
    BOOST_CHECK(modern::CheckPolicyOutput(out, modern_height, params) ==
                modern::PolicyOutputCheck::OK);

    // Unknown types and unactivated versions are invalid, never ignored.
    out.policy_type = 2; // first unassigned type
    BOOST_CHECK(modern::CheckPolicyOutput(out, modern_height, params) ==
                modern::PolicyOutputCheck::UNKNOWN_POLICY);
    out.policy_type = 0xFFFF;
    BOOST_CHECK(modern::CheckPolicyOutput(out, modern_height, params) ==
                modern::PolicyOutputCheck::UNKNOWN_POLICY);
    out = OwnerVector();
    out.policy_version = 0;
    BOOST_CHECK(modern::CheckPolicyOutput(out, modern_height, params) ==
                modern::PolicyOutputCheck::UNKNOWN_POLICY);
    out.policy_version = 2;
    BOOST_CHECK(modern::CheckPolicyOutput(out, modern_height, params) ==
                modern::PolicyOutputCheck::UNKNOWN_POLICY);
}

BOOST_AUTO_TEST_CASE(policy_outputs_are_modern_era_only)
{
    const Consensus::Params params{B3Params()};
    const modern::ModernOutput out{OwnerVector()};

    BOOST_CHECK(modern::CheckPolicyOutput(out, SYNTHETIC_H + 1, params) ==
                modern::PolicyOutputCheck::OK);
    BOOST_CHECK(modern::CheckPolicyOutput(out, SYNTHETIC_H, params) ==
                modern::PolicyOutputCheck::NOT_MODERN_ERA);
    BOOST_CHECK(modern::CheckPolicyOutput(out, 0, params) ==
                modern::PolicyOutputCheck::NOT_MODERN_ERA);
}

BOOST_AUTO_TEST_CASE(amounts_and_params_are_bounded)
{
    const Consensus::Params params{B3Params()};
    const int modern_height{SYNTHETIC_H + 1};

    modern::ModernOutput out{OwnerVector()};
    out.amount = -1;
    BOOST_CHECK(modern::CheckPolicyOutput(out, modern_height, params) ==
                modern::PolicyOutputCheck::BAD_AMOUNT);
    out.amount = MAX_MONEY + 1;
    BOOST_CHECK(modern::CheckPolicyOutput(out, modern_height, params) ==
                modern::PolicyOutputCheck::BAD_AMOUNT);
    out.amount = MAX_MONEY;
    BOOST_CHECK(modern::CheckPolicyOutput(out, modern_height, params) ==
                modern::PolicyOutputCheck::OK);

    // Consensus-visible parameters stay small, and v1 of both policies
    // carries none at all.
    out = OwnerVector();
    out.policy_params.assign(modern::MAX_POLICY_PARAMS_SIZE + 1, 0x00);
    BOOST_CHECK(modern::CheckPolicyOutput(out, modern_height, params) ==
                modern::PolicyOutputCheck::PARAMS_TOO_LARGE);
    out.policy_params.assign(1, 0x00);
    BOOST_CHECK(modern::CheckPolicyOutput(out, modern_height, params) ==
                modern::PolicyOutputCheck::BAD_POLICY_PARAMS);
}

BOOST_AUTO_TEST_CASE(legacy_lock_is_native_only)
{
    const Consensus::Params params{B3Params()};
    const int modern_height{SYNTHETIC_H + 1};

    modern::ModernOutput out{OwnerVector()};
    out.policy_type = static_cast<uint16_t>(modern::PolicyType::LEGACY_LOCK);
    BOOST_CHECK(modern::CheckPolicyOutput(out, modern_height, params) ==
                modern::PolicyOutputCheck::BAD_ASSET);
    out.asset = modern::NativeAsset();
    BOOST_CHECK(modern::CheckPolicyOutput(out, modern_height, params) ==
                modern::PolicyOutputCheck::OK);
}

BOOST_AUTO_TEST_CASE(pre_h_utxos_view_as_native_legacy_lock)
{
    const Consensus::Params params{B3Params()};
    const CScript legacy_script{CScript() << OP_DUP << OP_HASH160
                                          << std::vector<unsigned char>(20, 0xb3)
                                          << OP_EQUALVERIFY << OP_CHECKSIG};
    const Coin coin{CTxOut{7'654'321, legacy_script}, /*nHeightIn=*/123,
                    /*fCoinBaseIn=*/false};

    const modern::ModernOutput viewed{modern::ViewLegacyCoin(coin)};
    BOOST_CHECK(viewed.asset == modern::NativeAsset());
    BOOST_CHECK_EQUAL(viewed.amount, coin.out.nValue);
    BOOST_CHECK_EQUAL(viewed.policy_type, static_cast<uint16_t>(modern::PolicyType::LEGACY_LOCK));
    BOOST_CHECK_EQUAL(viewed.policy_version, modern::POLICY_VERSION_V1);
    BOOST_CHECK(viewed.policy_commitment == modern::LegacyLockCommitment(legacy_script));
    BOOST_CHECK(viewed.policy_params.empty());

    // The view is pure and deterministic, and validates in the modern era.
    BOOST_CHECK(modern::ViewLegacyCoin(coin) == viewed);
    BOOST_CHECK(modern::CheckPolicyOutput(viewed, SYNTHETIC_H + 1, params) ==
                modern::PolicyOutputCheck::OK);

    // The coin itself is untouched by viewing: same bytes before and after.
    DataStream before;
    before << coin;
    (void)modern::ViewLegacyCoin(coin);
    DataStream after;
    after << coin;
    BOOST_CHECK(std::equal(before.begin(), before.end(), after.begin(), after.end()) &&
                before.size() == after.size());
}

BOOST_AUTO_TEST_SUITE_END()
