// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <modern/asset_validation.h>

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/fn_params.h>
#include <consensus/modern_pos_params.h>
#include <consensus/params.h>
#include <crypto/common.h>
#include <modern/asset_output.h>
#include <modern/fn_pod.h>
#include <modern/mpa.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr int LEGACY_FINAL_HEIGHT{100};
constexpr int FN_GENESIS_HEIGHT{LEGACY_FINAL_HEIGHT + 1};
constexpr int FN_POD_ACTIVATION_HEIGHT{130};
constexpr int ASSET_ACTIVATION_HEIGHT{140};
constexpr CAmount NATIVE_INPUT_VALUE{100'000 * KILO_COIN};

const uint256 TEST_GENESIS{
    "00000000000000000000000000000000000000000000000000000000000000b1"};
const uint256 TEST_X{
    "00000000000000000000000000000000000000000000000000000000000000b2"};

CScript OwnerScript(const unsigned char fill = 0x41)
{
    return CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, fill)
                     << OP_EQUALVERIFY << OP_CHECKSIG;
}

Consensus::FnGenesisRight ManifestRight(const uint32_t number)
{
    Consensus::FnGenesisRight right;
    WriteBE32(right.pod_id.begin(), number);
    right.recipient_key_hash.fill(static_cast<unsigned char>(number));
    return right;
}

Consensus::Params TestParams()
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = FN_GENESIS_HEIGHT;
    params.hashGenesisBlock = TEST_GENESIS;
    params.legacy_final_hash = TEST_X;
    params.modern_pos.emplace();
    params.fn_genesis_rights_root = uint256::ONE;
    params.fn_genesis_manifest.push_back(ManifestRight(1));
    params.fn_pod_activation_height = FN_POD_ACTIVATION_HEIGHT;
    params.asset_activation_height = ASSET_ACTIVATION_HEIGHT;
    return params;
}

modern::AssetId FnAsset(const Consensus::Params& params)
{
    const auto asset{modern::ConfiguredFnAssetId(params)};
    BOOST_REQUIRE(asset.has_value());
    return *asset;
}

COutPoint Prevout(const uint32_t number)
{
    uint256 hash;
    WriteBE32(hash.begin(), number);
    return COutPoint{Txid::FromUint256(hash), number};
}

Coin NativeCoin(const CAmount amount = NATIVE_INPUT_VALUE)
{
    return Coin{CTxOut{amount, CScript() << OP_TRUE}, LEGACY_FINAL_HEIGHT,
                /*coinbase=*/false};
}

CTxOut FnOutput(const Consensus::Params& params, const CAmount amount,
                const unsigned char owner_fill = 0x41)
{
    const auto output{modern::MakeAssetOwnerOutput(
        FnAsset(params), amount, modern::PolicyType::FN, OwnerScript(owner_fill))};
    BOOST_REQUIRE(output.has_value());
    return *output;
}

CMutableTransaction PodTransaction(const Consensus::Params& params,
                                   const uint32_t created_before,
                                   const CAmount native_gap,
                                   const uint32_t declared_output_index = 1)
{
    BOOST_REQUIRE(native_gap >= 0);
    BOOST_REQUIRE(native_gap <= NATIVE_INPUT_VALUE);
    CMutableTransaction tx;
    tx.vin.emplace_back(Prevout(created_before + 1));
    tx.vout.emplace_back(NATIVE_INPUT_VALUE - native_gap, CScript() << OP_TRUE);
    tx.vout.push_back(FnOutput(params, 1));
    tx.mpa.push_back(modern::MakeModernFnPodRecord(created_before,
                                                   declared_output_index));
    return tx;
}

bool CheckPod(const CMutableTransaction& tx, const Consensus::Params& params,
              const int height, const uint32_t issued_before,
              modern::AssetTransactionEffects& effects, std::string& error)
{
    const std::vector<Coin> coins{NativeCoin()};
    const CTransaction transaction{tx};
    const CAmount native_gap{NATIVE_INPUT_VALUE - transaction.GetValueOut()};
    error.clear();
    return modern::CheckAssetTransaction(
        transaction, coins, height, params,
        modern::AssetTransactionContext{issued_before, native_gap}, effects, error);
}

std::string ConservationError(const modern::AssetCheck check)
{
    return "asset conservation failure (code " +
           std::to_string(static_cast<int>(check)) + ")";
}

} // namespace

BOOST_AUTO_TEST_SUITE(modern_fn_pod_tests)

BOOST_AUTO_TEST_CASE(type_six_codec_is_exactly_eight_network_order_bytes)
{
    const modern::ModernFnPodActionV1 expected{0x01020304U, 0xa0b0c0d0U};
    const std::vector<unsigned char> encoded{
        modern::EncodeModernFnPodPayload(expected)};
    const std::vector<unsigned char> exact{
        0x01, 0x02, 0x03, 0x04, 0xa0, 0xb0, 0xc0, 0xd0};

    BOOST_CHECK_EQUAL(encoded.size(), modern::MODERN_FN_POD_ACTION_V1_SIZE);
    BOOST_CHECK(encoded == exact);

    const CMpaRecord record{modern::MakeModernFnPodRecord(expected.created_before,
                                                          expected.output_index)};
    BOOST_CHECK_EQUAL(record.payload_type, modern::CREATION_ACTION_MODERN_FN_POD);
    BOOST_CHECK_EQUAL(record.payload_version,
                      modern::MODERN_FN_POD_ACTION_VERSION_V1);
    BOOST_CHECK(record.payload == exact);

    modern::ModernFnPodActionV1 decoded;
    std::string error;
    BOOST_CHECK(modern::DecodeModernFnPodRecord(record, decoded, error));
    BOOST_CHECK(decoded == expected);
    BOOST_CHECK(error.empty());
}

BOOST_AUTO_TEST_CASE(codec_rejects_malformed_payload_type_and_version)
{
    const CMpaRecord valid{modern::MakeModernFnPodRecord(7, 2)};
    modern::ModernFnPodActionV1 decoded;
    std::string error;

    for (const size_t malformed_size : {size_t{0}, size_t{7}, size_t{9}}) {
        CMpaRecord malformed{valid};
        malformed.payload.resize(malformed_size);
        BOOST_CHECK(!modern::DecodeModernFnPodRecord(malformed, decoded, error));
        BOOST_CHECK_EQUAL(error, "modern FN PoD declaration has the wrong size");
    }

    CMpaRecord wrong_type{valid};
    wrong_type.payload_type = modern::CREATION_ACTION_ASSET_ISSUANCE;
    BOOST_CHECK(!modern::DecodeModernFnPodRecord(wrong_type, decoded, error));
    BOOST_CHECK_EQUAL(error, "not a modern FN PoD declaration");

    CMpaRecord wrong_version{valid};
    wrong_version.payload_version = 2;
    BOOST_CHECK(!modern::DecodeModernFnPodRecord(wrong_version, decoded, error));
    BOOST_CHECK_EQUAL(error, "not a modern FN PoD declaration");
}

BOOST_AUTO_TEST_CASE(type_six_activates_only_at_its_pinned_height)
{
    const Consensus::Params params{TestParams()};
    CMutableTransaction tx{PodTransaction(
        params, 0, modern::RequiredFnPodDisintegration(0))};
    std::string error;

    BOOST_CHECK(!modern::CheckTransactionMpa(
        CTransaction{tx}, params, FN_POD_ACTIVATION_HEIGHT - 1, error));
    BOOST_CHECK_EQUAL(error, "mpa-inactive-type");
    BOOST_CHECK(modern::CheckTransactionMpa(
        CTransaction{tx}, params, FN_POD_ACTIVATION_HEIGHT, error));

    modern::AssetTransactionEffects effects;
    BOOST_CHECK(!CheckPod(tx, params, FN_POD_ACTIVATION_HEIGHT - 1, 0,
                          effects, error));
    BOOST_CHECK_EQUAL(error, "modern FN PoD creation is not active");
    BOOST_CHECK(CheckPod(tx, params, FN_POD_ACTIVATION_HEIGHT, 0, effects, error));

    tx.mpa[0].payload_version = 2;
    BOOST_CHECK(!modern::CheckTransactionMpa(
        CTransaction{tx}, params, FN_POD_ACTIVATION_HEIGHT, error));
    BOOST_CHECK_EQUAL(error, "mpa-unknown-type");
}

BOOST_AUTO_TEST_CASE(declaration_binds_the_exact_amount_one_fn_vout)
{
    const Consensus::Params params{TestParams()};
    const CAmount required{modern::RequiredFnPodDisintegration(0)};
    modern::AssetTransactionEffects effects;
    std::string error;

    CMutableTransaction out_of_range{PodTransaction(params, 0, required, 2)};
    BOOST_CHECK(!CheckPod(out_of_range, params, FN_POD_ACTIVATION_HEIGHT, 0,
                          effects, error));
    BOOST_CHECK_EQUAL(error, "modern FN PoD output index is out of range");

    CMutableTransaction names_native{PodTransaction(params, 0, required, 0)};
    BOOST_CHECK(!CheckPod(names_native, params, FN_POD_ACTIVATION_HEIGHT, 0,
                          effects, error));
    BOOST_CHECK_EQUAL(
        error, "modern FN PoD declaration does not name an amount-1 FN owner output");

    CMutableTransaction amount_two{PodTransaction(params, 0, required)};
    amount_two.vout[1] = FnOutput(params, 2);
    BOOST_CHECK(!CheckPod(amount_two, params, FN_POD_ACTIVATION_HEIGHT, 0,
                          effects, error));
    BOOST_CHECK_EQUAL(
        error, "modern FN PoD declaration does not name an amount-1 FN owner output");
}

BOOST_AUTO_TEST_CASE(authorization_is_exactly_one_fn_unit)
{
    const Consensus::Params params{TestParams()};
    const CAmount required{modern::RequiredFnPodDisintegration(0)};
    modern::AssetTransactionEffects effects;
    std::string error;

    CMutableTransaction exact{PodTransaction(params, 0, required)};
    BOOST_CHECK(CheckPod(exact, params, FN_POD_ACTIVATION_HEIGHT, 0, effects, error));
    BOOST_CHECK_EQUAL(effects.fn_pod_creations, 1U);

    CMutableTransaction extra{exact};
    extra.vout.push_back(FnOutput(params, 1, 0x42));
    BOOST_CHECK(!CheckPod(extra, params, FN_POD_ACTIVATION_HEIGHT, 0,
                          effects, error));
    BOOST_CHECK_EQUAL(error,
                      ConservationError(modern::AssetCheck::UNAUTHORIZED_MINT));

    CMutableTransaction wrong_slot{exact};
    wrong_slot.mpa[0] = modern::MakeModernFnPodRecord(1, 1);
    BOOST_CHECK(!CheckPod(wrong_slot, params, FN_POD_ACTIVATION_HEIGHT, 0,
                          effects, error));
    BOOST_CHECK_EQUAL(error,
                      "modern FN PoD declaration names the wrong creation slot");
}

BOOST_AUTO_TEST_CASE(disintegration_is_separate_from_the_ordinary_native_fee)
{
    const Consensus::Params params{TestParams()};
    const CAmount required{modern::RequiredFnPodDisintegration(0)};
    constexpr CAmount ordinary_fee{17 * KILO_COIN};
    modern::AssetTransactionEffects effects;
    std::string error;

    CMutableTransaction under{PodTransaction(params, 0, required - 1)};
    BOOST_CHECK(!CheckPod(under, params, FN_POD_ACTIVATION_HEIGHT, 0,
                          effects, error));
    BOOST_CHECK_EQUAL(
        error, "modern FN PoD accounting gap is below the required disintegration");

    CMutableTransaction exact{PodTransaction(params, 0, required)};
    BOOST_CHECK(CheckPod(exact, params, FN_POD_ACTIVATION_HEIGHT, 0, effects, error));
    BOOST_CHECK_EQUAL(effects.fn_pod_disintegration, required);
    BOOST_CHECK_EQUAL(required - effects.fn_pod_disintegration, 0);

    CMutableTransaction with_fee{PodTransaction(params, 0, required + ordinary_fee)};
    BOOST_CHECK(CheckPod(with_fee, params, FN_POD_ACTIVATION_HEIGHT, 0,
                         effects, error));
    BOOST_CHECK_EQUAL(effects.fn_pod_disintegration, required);
    BOOST_CHECK_EQUAL((required + ordinary_fee) - effects.fn_pod_disintegration,
                      ordinary_fee);
}

BOOST_AUTO_TEST_CASE(historical_manifest_reduces_and_can_exhaust_capacity)
{
    Consensus::Params params{TestParams()};
    params.fn_genesis_manifest.clear();
    for (uint32_t i{0}; i < 4'500; ++i) {
        params.fn_genesis_manifest.push_back(ManifestRight(i + 1));
    }
    BOOST_REQUIRE_EQUAL(modern::ModernFnCapacity(params).value(), 500U);

    modern::AssetTransactionEffects effects;
    std::string error;
    const uint32_t last_slot{499};
    CMutableTransaction last{PodTransaction(
        params, last_slot, modern::RequiredFnPodDisintegration(last_slot))};
    BOOST_CHECK(CheckPod(last, params, FN_POD_ACTIVATION_HEIGHT, last_slot,
                         effects, error));

    const uint32_t exhausted_slot{500};
    CMutableTransaction exhausted{PodTransaction(
        params, exhausted_slot,
        modern::RequiredFnPodDisintegration(exhausted_slot))};
    BOOST_CHECK(!CheckPod(exhausted, params, FN_POD_ACTIVATION_HEIGHT,
                          exhausted_slot, effects, error));
    BOOST_CHECK_EQUAL(error, "FN lifetime issuance cap is exhausted");

    params.fn_genesis_manifest.resize(Consensus::MAX_FN_EVER_ISSUED + 1);
    BOOST_CHECK(!modern::ModernFnCapacity(params).has_value());
}

BOOST_AUTO_TEST_CASE(tier_boundaries_are_priced_by_the_declared_modern_slot)
{
    const Consensus::Params params{TestParams()};
    const std::array<std::pair<uint32_t, CAmount>, 4> cases{{
        {499, 15'000 * KILO_COIN},
        {500, 30'000 * KILO_COIN},
        {999, 30'000 * KILO_COIN},
        {1'000, 60'000 * KILO_COIN},
    }};

    for (const auto& [slot, required] : cases) {
        BOOST_CHECK_EQUAL(modern::RequiredFnPodDisintegration(slot), required);
        CMutableTransaction tx{PodTransaction(params, slot, required)};
        modern::AssetTransactionEffects effects;
        std::string error;
        BOOST_CHECK_MESSAGE(
            CheckPod(tx, params, FN_POD_ACTIVATION_HEIGHT, slot, effects, error),
            "slot " << slot << ": " << error);
        BOOST_CHECK_EQUAL(effects.fn_pod_disintegration, required);
    }
}

BOOST_AUTO_TEST_SUITE_END()
