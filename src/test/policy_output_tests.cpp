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

BOOST_AUTO_TEST_CASE(stake_v1_structure_is_enforced)
{
    const Consensus::Params params{B3Params()};
    const int modern_height{SYNTHETIC_H + 1};

    modern::ModernOutput out;
    out.asset = modern::NativeAsset();
    out.amount = 5'000'000;
    out.policy_type = static_cast<uint16_t>(modern::PolicyType::STAKE);
    out.policy_version = modern::POLICY_VERSION_V1;
    out.policy_commitment =
        uint256{"00000000000000000000000000000000000000000000000000000000000000c7"};
    out.policy_params.assign(modern::STAKE_PARAMS_SIZE, 0x00);
    for (size_t i{0}; i < modern::STAKE_PARAMS_KEY_SIZE; ++i) out.policy_params[i] = 0x42;

    BOOST_CHECK(modern::CheckPolicyOutput(out, modern_height, params) ==
                modern::PolicyOutputCheck::OK);

    // Regression: an activated STAKE output must never fall through the
    // structural switch unchecked.
    modern::ModernOutput bad{out};
    bad.asset = uint256{"00000000000000000000000000000000000000000000000000000000000000a5"};
    BOOST_CHECK(modern::CheckPolicyOutput(bad, modern_height, params) ==
                modern::PolicyOutputCheck::BAD_ASSET);
    bad = out;
    bad.policy_commitment = uint256{};
    BOOST_CHECK(modern::CheckPolicyOutput(bad, modern_height, params) ==
                modern::PolicyOutputCheck::BAD_POLICY_PARAMS);
    bad = out;
    bad.policy_params.resize(modern::STAKE_PARAMS_SIZE - 1);
    BOOST_CHECK(modern::CheckPolicyOutput(bad, modern_height, params) ==
                modern::PolicyOutputCheck::BAD_POLICY_PARAMS);
    bad = out;
    bad.policy_params[modern::STAKE_PARAMS_SIZE - 1] = 0x01; // reserved must stay zero
    BOOST_CHECK(modern::CheckPolicyOutput(bad, modern_height, params) ==
                modern::PolicyOutputCheck::BAD_POLICY_PARAMS);
}


//! DEX_VAULT v2 params (owner ruling 2026-08-22): one keyless vault policy,
//! two kinds — USER_DEPOSIT carries the FlowMesh account id (35 bytes),
//! VAULT_POOL_CHANGE carries none (3 bytes) and can never credit anyone.
//! The v1 shard-only form is retired.
BOOST_AUTO_TEST_CASE(dex_vault_v2_params_distinguish_user_deposit_from_pool_change)
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = 1001;
    params.test_only_asset_policies_active = true;
    const uint256 vault{uint256{"00000000000000000000000000000000000000000000000000000000000000f1"}};
    const uint256 account{uint256{"00000000000000000000000000000000000000000000000000000000000000a1"}};

    modern::ModernOutput out;
    out.asset = uint256::ONE;
    out.amount = 500;
    out.policy_type = static_cast<uint16_t>(modern::PolicyType::DEX_VAULT);
    out.policy_version = modern::DEX_VAULT_POLICY_VERSION_V2;
    out.policy_commitment = vault;

    out.policy_params = modern::MakeVaultParams(modern::VAULT_KIND_POOL_CHANGE, 7);
    BOOST_CHECK(modern::CheckPolicyOutput(out, 1001, params) == modern::PolicyOutputCheck::OK);
    {
        const auto vp{modern::ParseVaultParams(out.policy_params)};
        BOOST_REQUIRE(vp.has_value());
        BOOST_CHECK_EQUAL(vp->kind, modern::VAULT_KIND_POOL_CHANGE);
        BOOST_CHECK_EQUAL(vp->shard, 7U);
        BOOST_CHECK(!vp->account.has_value());
    }
    const uint16_t deposit_shard{modern::FlowMeshUserDepositShard(vault, account)};
    out.policy_params = modern::MakeVaultParams(modern::VAULT_KIND_USER_DEPOSIT,
                                                deposit_shard, account);
    BOOST_CHECK(modern::CheckPolicyOutput(out, 1001, params) == modern::PolicyOutputCheck::OK);
    {
        const auto vp{modern::ParseVaultParams(out.policy_params)};
        BOOST_REQUIRE(vp.has_value());
        BOOST_CHECK_EQUAL(vp->kind, modern::VAULT_KIND_USER_DEPOSIT);
        BOOST_CHECK_EQUAL(vp->shard, deposit_shard);
        BOOST_REQUIRE(vp->account.has_value());
        BOOST_CHECK(*vp->account == account);
    }
    // Malformed: a USER_DEPOSIT with a null account, wrong sizes, unknown
    // kind, and the retired v1 two-byte form.
    out.policy_params = modern::MakeVaultParams(modern::VAULT_KIND_USER_DEPOSIT,
                                                deposit_shard, uint256{});
    BOOST_CHECK(modern::CheckPolicyOutput(out, 1001, params) != modern::PolicyOutputCheck::OK);
    out.policy_params = {modern::VAULT_KIND_POOL_CHANGE, 0x00}; // too short
    BOOST_CHECK(modern::CheckPolicyOutput(out, 1001, params) != modern::PolicyOutputCheck::OK);
    out.policy_params = modern::MakeVaultParams(modern::VAULT_KIND_POOL_CHANGE, 0);
    out.policy_params.push_back(0x00); // pool change with trailing byte
    BOOST_CHECK(modern::CheckPolicyOutput(out, 1001, params) != modern::PolicyOutputCheck::OK);
    out.policy_params = modern::MakeVaultParams(9, 0); // unknown kind
    BOOST_CHECK(modern::CheckPolicyOutput(out, 1001, params) != modern::PolicyOutputCheck::OK);
    out.policy_params = {0x00, 0x00}; // retired v1 shard-only
    BOOST_CHECK(modern::CheckPolicyOutput(out, 1001, params) != modern::PolicyOutputCheck::OK);
    // And v1 DEX_VAULT is no longer an activated version at all.
    out.policy_version = modern::POLICY_VERSION_V1;
    out.policy_params = {0x00, 0x00};
    BOOST_CHECK(!modern::IsActivatedPolicy(out.policy_type, out.policy_version, /*assets_active=*/true));
    BOOST_CHECK(modern::IsActivatedPolicy(out.policy_type,
                                           modern::DEX_VAULT_POLICY_VERSION_V2,
                                           /*assets_active=*/false,
                                           /*fn_active=*/false,
                                           /*dex_vault_active=*/true));
    BOOST_CHECK(!modern::IsActivatedPolicy(out.policy_type,
                                            modern::DEX_VAULT_POLICY_VERSION_V2,
                                            /*assets_active=*/true,
                                            /*fn_active=*/false,
                                            /*dex_vault_active=*/false));

    // A complete same-release schedule opens keyless vault preparation with
    // colored assets at A2; their proof-authorized spends still wait for A3.
    params.test_only_asset_policies_active = false;
    params.legacy_final_hash = uint256::ONE;
    params.modern_pos.emplace();
    params.fn_pod_activation_height = 1010;
    params.asset_activation_height = 1020;
    params.flowmesh_activation_height = 1020 + Consensus::FLOWMESH_ANCHOR_DEPTH;
    out.policy_version = modern::DEX_VAULT_POLICY_VERSION_V2;
    out.policy_params = modern::MakeVaultParams(modern::VAULT_KIND_POOL_CHANGE, 7);
    BOOST_REQUIRE(Consensus::AssetRulesActive(1020, params));
    BOOST_CHECK(modern::CheckPolicyOutput(out, 1019, params) ==
                modern::PolicyOutputCheck::UNKNOWN_POLICY);
    BOOST_CHECK(modern::CheckPolicyOutput(out, 1020, params) ==
                modern::PolicyOutputCheck::OK);
}

BOOST_AUTO_TEST_SUITE_END()
