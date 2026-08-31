// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <coins.h>
#include <consensus/era.h>
#include <consensus/params.h>
#include <crypto/bls.h>
#include <crypto/common.h>
#include <modern/asset_output.h>
#include <modern/asset_validation.h>
#include <modern/flowmesh_seat.h>
#include <modern/mpa.h>
#include <modern/payload_cost.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr int H{100};
constexpr int M{111};
constexpr int A1{120};
constexpr int A2{130};
constexpr int A3{A2 + Consensus::FLOWMESH_ANCHOR_DEPTH};

Consensus::FnGenesisRight ManifestRight()
{
    Consensus::FnGenesisRight right;
    right.pod_id.begin()[31] = 1;
    right.recipient_key_hash.fill(0x21);
    return right;
}

Consensus::Params Params()
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = H + 1;
    params.transition_pow_length = 10;
    params.hashGenesisBlock = uint256::ONE;
    params.legacy_final_hash = uint256::ONE;
    params.modern_pos.emplace();
    params.fn_genesis_rights_root = uint256::ONE;
    params.fn_genesis_manifest.push_back(ManifestRight());
    params.fn_pod_activation_height = A1;
    params.asset_activation_height = A2;
    params.flowmesh_activation_height = A3;
    return params;
}

bls::SecretKey Key(const unsigned char id)
{
    std::array<unsigned char, 32> ikm{};
    ikm[0] = id;
    ikm[31] = 0x42;
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key.has_value());
    return *key;
}

CScript OwnerScript(const unsigned char fill = 0x31)
{
    return CScript() << OP_DUP << OP_HASH160
                     << std::vector<unsigned char>(20, fill)
                     << OP_EQUALVERIFY << OP_CHECKSIG;
}

modern::AssetId FnAsset(const Consensus::Params& params)
{
    const auto asset{modern::ConfiguredFnAssetId(params)};
    BOOST_REQUIRE(asset.has_value());
    return *asset;
}

CTxOut SeatOutput(const Consensus::Params& params, const bls::SecretKey& key,
                  const CAmount amount = 1, const unsigned char owner_fill = 0x31)
{
    modern::ModernOutput out;
    out.asset = FnAsset(params);
    out.amount = amount;
    out.policy_type = static_cast<uint16_t>(modern::PolicyType::FN);
    out.policy_version = modern::FN_SEAT_POLICY_VERSION_V2;
    out.policy_commitment = modern::AssetOwnerCommitment(OwnerScript(owner_fill));
    const auto pubkey{key.GetPublicKey().Compressed()};
    out.policy_params.assign(pubkey.begin(), pubkey.end());
    const auto made{modern::MakeAssetOwnerOutput(out, OwnerScript(owner_fill))};
    BOOST_REQUIRE(made.has_value());
    return *made;
}

CTxOut FnV1Output(const Consensus::Params& params, const unsigned char owner_fill = 0x32)
{
    const auto made{modern::MakeAssetOwnerOutput(FnAsset(params), 1,
                                                  modern::PolicyType::FN,
                                                  OwnerScript(owner_fill))};
    BOOST_REQUIRE(made.has_value());
    return *made;
}

CMpaRecord BindingRecord(const uint32_t output_index, const bls::SecretKey& key)
{
    const auto pop{key.SignPoP().Compressed()};
    return modern::MakeFlowMeshSeatBindingRecord(output_index, pop);
}

CMutableTransaction SeatTransaction(const Consensus::Params& params,
                                    const bls::SecretKey& key)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    tx.vout.emplace_back(0, CScript() << OP_TRUE);
    tx.vout.push_back(SeatOutput(params, key));
    tx.mpa.push_back(BindingRecord(1, key));
    return tx;
}

bool CheckMpa(const CMutableTransaction& tx, const Consensus::Params& params,
              const int height, std::string& error)
{
    error.clear();
    return modern::CheckTransactionMpa(CTransaction{tx}, params, height, error);
}

} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_seat_binding_tests)

BOOST_AUTO_TEST_CASE(type_seven_codec_is_exact_and_network_ordered)
{
    modern::FlowMeshSeatBindingV1 binding;
    binding.output_index = 0x01020304U;
    for (size_t i{0}; i < binding.pop.size(); ++i) {
        binding.pop[i] = static_cast<unsigned char>(i);
    }
    const std::vector<unsigned char> encoded{
        modern::EncodeFlowMeshSeatBindingPayload(binding)};
    BOOST_REQUIRE_EQUAL(encoded.size(), 100U);
    BOOST_CHECK_EQUAL(encoded[0], 0x01);
    BOOST_CHECK_EQUAL(encoded[1], 0x02);
    BOOST_CHECK_EQUAL(encoded[2], 0x03);
    BOOST_CHECK_EQUAL(encoded[3], 0x04);
    BOOST_CHECK(std::equal(binding.pop.begin(), binding.pop.end(), encoded.begin() + 4));

    const CMpaRecord record{modern::MakeFlowMeshSeatBindingRecord(
        binding.output_index, binding.pop)};
    BOOST_CHECK_EQUAL(record.payload_type,
                      modern::CREATION_ACTION_FLOWMESH_SEAT_BINDING);
    BOOST_CHECK_EQUAL(record.payload_version,
                      modern::FLOWMESH_SEAT_BINDING_ACTION_VERSION_V1);
    BOOST_CHECK(record.payload == encoded);

    modern::FlowMeshSeatBindingV1 decoded;
    std::string error;
    BOOST_CHECK(modern::DecodeFlowMeshSeatBindingRecord(record, decoded, error));
    BOOST_CHECK(decoded == binding);

    for (const size_t size : {size_t{0}, size_t{99}, size_t{101}}) {
        CMpaRecord malformed{record};
        malformed.payload.resize(size);
        BOOST_CHECK(!modern::DecodeFlowMeshSeatBindingRecord(malformed, decoded, error));
        BOOST_CHECK_EQUAL(error, "FlowMesh seat-binding record has the wrong size");
    }
}

BOOST_AUTO_TEST_CASE(fn_v2_reuses_the_owner_carrier_without_op_return)
{
    const Consensus::Params params{Params()};
    const bls::SecretKey key{Key(1)};
    const CScript owner{OwnerScript()};
    const auto made{modern::MakeFlowMeshSeatOutput(FnAsset(params), owner,
                                                   key.GetPublicKey())};
    BOOST_REQUIRE(made.has_value());
    BOOST_CHECK(!made->scriptPubKey.empty());
    BOOST_CHECK(made->scriptPubKey[0] != OP_RETURN);
    BOOST_CHECK(modern::AssetOwnerScript(*made) == owner);

    std::string error;
    const auto parsed{modern::ParseAssetOutput(*made, error)};
    BOOST_REQUIRE(parsed.has_value());
    BOOST_CHECK_EQUAL(parsed->policy_type,
                      static_cast<uint16_t>(modern::PolicyType::FN));
    BOOST_CHECK_EQUAL(parsed->policy_version, modern::FN_SEAT_POLICY_VERSION_V2);
    BOOST_CHECK_EQUAL(parsed->amount, 1);
    BOOST_CHECK_EQUAL(parsed->policy_params.size(), bls::PUBKEY_SIZE);
    BOOST_CHECK(parsed->policy_commitment == modern::AssetOwnerCommitment(owner));
    BOOST_CHECK(modern::CheckPolicyOutput(*parsed, A2 - 1, params) ==
                modern::PolicyOutputCheck::UNKNOWN_POLICY);
    BOOST_CHECK(modern::CheckPolicyOutput(*parsed, A2, params) ==
                modern::PolicyOutputCheck::OK);

    const CTxOut ordinary{FnV1Output(params)};
    const auto v1{modern::ParseAssetOutput(ordinary, error)};
    BOOST_REQUIRE(v1.has_value());
    BOOST_CHECK_EQUAL(v1->policy_version, modern::POLICY_VERSION_V1);
    BOOST_CHECK(v1->policy_params.empty());
}

BOOST_AUTO_TEST_CASE(a3_schedule_is_complete_ordered_and_fail_closed)
{
    Consensus::Params params{Params()};
    BOOST_CHECK_EQUAL(Consensus::ModernPosStartHeight(params).value_or(0), M);
    BOOST_CHECK(Consensus::FlowMeshSeatBindingScheduleConfigured(params));
    BOOST_CHECK(!Consensus::FlowMeshSeatBindingRulesActive(A2 - 1, params));
    BOOST_CHECK(Consensus::FlowMeshSeatBindingRulesActive(A2, params));
    BOOST_CHECK(!Consensus::FlowMeshRulesActive(A3 - 1, params));
    BOOST_CHECK(Consensus::FlowMeshRulesActive(A3, params));

    params.flowmesh_activation_height.reset();
    BOOST_CHECK(!Consensus::FlowMeshSeatBindingScheduleConfigured(params));
    BOOST_CHECK(!Consensus::FlowMeshSeatBindingRulesActive(A2, params));
    params.flowmesh_activation_height = A2 + Consensus::FLOWMESH_ANCHOR_DEPTH - 1;
    BOOST_CHECK(!Consensus::FlowMeshSeatBindingScheduleConfigured(params));
    params.flowmesh_activation_height = A3;
    BOOST_CHECK(Consensus::FlowMeshSeatBindingScheduleConfigured(params));
}

BOOST_AUTO_TEST_CASE(valid_binding_activates_exactly_at_a2_and_has_cost)
{
    const Consensus::Params params{Params()};
    const bls::SecretKey key{Key(2)};
    const CMutableTransaction tx{SeatTransaction(params, key)};
    std::string error;

    BOOST_CHECK(!CheckMpa(tx, params, A2 - 1, error));
    BOOST_CHECK_EQUAL(error, "mpa-inactive-type");
    BOOST_CHECK(CheckMpa(tx, params, A2, error));
    BOOST_CHECK_EQUAL(modern::PayloadVerifyCost(CTransaction{tx}),
                      modern::FLOWMESH_SEAT_BINDING_VERIFY_COST);
    BOOST_CHECK_EQUAL(modern::FLOWMESH_SEAT_BINDING_VERIFY_COST, 700);

    BOOST_CHECK(modern::GetPayloadTypeStatus(
                    modern::CREATION_ACTION_FLOWMESH_SEAT_BINDING, 1, params, A2) ==
                modern::PayloadTypeStatus::ACTIVE);
    BOOST_CHECK(modern::GetPayloadTypeStatus(
                    modern::CREATION_ACTION_FLOWMESH_SEAT_BINDING, 2, params, A2) ==
                modern::PayloadTypeStatus::UNKNOWN);
    BOOST_CHECK(modern::PayloadSizeAllowed(
        modern::CREATION_ACTION_FLOWMESH_SEAT_BINDING, 100));
    BOOST_CHECK(!modern::PayloadSizeAllowed(
        modern::CREATION_ACTION_FLOWMESH_SEAT_BINDING, 99));
}

BOOST_AUTO_TEST_CASE(binding_rejects_cross_key_wrong_output_and_bad_amount)
{
    const Consensus::Params params{Params()};
    const bls::SecretKey seat_key{Key(3)};
    const bls::SecretKey wrong_key{Key(4)};
    std::string error;

    CMutableTransaction cross_key{SeatTransaction(params, seat_key)};
    cross_key.mpa[0] = BindingRecord(1, wrong_key);
    BOOST_CHECK(!CheckMpa(cross_key, params, A2, error));
    BOOST_CHECK_EQUAL(error, "invalid FlowMesh seat proof of possession");

    CMutableTransaction wrong_output{SeatTransaction(params, seat_key)};
    wrong_output.mpa[0] = BindingRecord(0, seat_key);
    BOOST_CHECK(!CheckMpa(wrong_output, params, A2, error));
    BOOST_CHECK_EQUAL(error, "orphan FlowMesh seat-binding record");

    CMutableTransaction amount_two{SeatTransaction(params, seat_key)};
    amount_two.vout[1] = SeatOutput(params, seat_key, 2);
    BOOST_CHECK(!CheckMpa(amount_two, params, A2, error));
    BOOST_CHECK_EQUAL(error, "FlowMesh seat output amount is not 1");
}

BOOST_AUTO_TEST_CASE(binding_requires_an_exact_bijection)
{
    const Consensus::Params params{Params()};
    const bls::SecretKey key{Key(5)};
    std::string error;

    CMutableTransaction missing{SeatTransaction(params, key)};
    missing.mpa.clear();
    BOOST_CHECK(!CheckMpa(missing, params, A2, error));
    BOOST_CHECK_EQUAL(error, "FlowMesh seat output is missing binding evidence");

    CMutableTransaction orphan{SeatTransaction(params, key)};
    orphan.vout[1] = FnV1Output(params);
    BOOST_CHECK(!CheckMpa(orphan, params, A2, error));
    BOOST_CHECK_EQUAL(error, "orphan FlowMesh seat-binding record");

    CMutableTransaction duplicate{SeatTransaction(params, key)};
    duplicate.mpa.push_back(duplicate.mpa.front());
    BOOST_CHECK(!modern::CheckFlowMeshSeatBindings(CTransaction{duplicate}, A2,
                                                    params, error));
    BOOST_CHECK_EQUAL(error, "duplicate FlowMesh seat-binding record");
    BOOST_CHECK(!CheckMpa(duplicate, params, A2, error));
    BOOST_CHECK_EQUAL(error, "mpa-record-order");
}

BOOST_AUTO_TEST_CASE(noncanonical_and_infinity_bls_keys_fail_structural_parse)
{
    const Consensus::Params params{Params()};
    modern::ModernOutput out;
    out.asset = FnAsset(params);
    out.amount = 1;
    out.policy_type = static_cast<uint16_t>(modern::PolicyType::FN);
    out.policy_version = modern::FN_SEAT_POLICY_VERSION_V2;
    out.policy_commitment = modern::AssetOwnerCommitment(OwnerScript());
    out.policy_params.assign(bls::PUBKEY_SIZE, 0x00);

    BOOST_CHECK(!modern::MakeAssetOwnerOutput(out, OwnerScript()).has_value());
    const std::vector<unsigned char> payload{modern::asset_output_detail::Payload(out)};
    CScript raw;
    raw << payload << OP_DROP;
    const CScript owner{OwnerScript()};
    raw.insert(raw.end(), owner.begin(), owner.end());
    std::string error;
    BOOST_CHECK(!modern::ParseAssetOutput(CTxOut{0, raw}, error));
    BOOST_CHECK_EQUAL(error,
                      "spendable asset carrier has an invalid OWNER/FN policy shape");

    // Canonical compressed infinity is forbidden by the consensus BLS wrapper.
    out.policy_params.assign(bls::PUBKEY_SIZE, 0x00);
    out.policy_params[0] = 0xc0;
    BOOST_CHECK(!modern::MakeAssetOwnerOutput(out, OwnerScript()).has_value());
}

BOOST_AUTO_TEST_CASE(spending_a_seat_to_fn_v1_ends_it_without_new_evidence)
{
    const Consensus::Params params{Params()};
    const bls::SecretKey key{Key(6)};
    const CTxOut seat{SeatOutput(params, key)};

    CMutableTransaction spend;
    spend.version = 2;
    spend.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    spend.vout.push_back(FnV1Output(params));
    std::string error;
    BOOST_CHECK(CheckMpa(spend, params, A2, error));

    const std::vector<Coin> prevs{Coin{seat, A2, /*coinbase=*/false}};
    modern::AssetTransactionEffects effects;
    BOOST_CHECK(modern::CheckAssetTransaction(
        CTransaction{spend}, prevs, A2, params,
        modern::AssetTransactionContext{std::nullopt, 0}, effects, error));
    BOOST_CHECK_EQUAL(effects.fn_pod_creations, 0U);
}

BOOST_AUTO_TEST_SUITE_END()
