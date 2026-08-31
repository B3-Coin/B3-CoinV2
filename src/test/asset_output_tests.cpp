// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <modern/asset_output.h>

#include <coins.h>
#include <consensus/amount.h>
#include <crypto/common.h>
#include <modern/policy.h>
#include <modern/asset_validation.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <primitives/transaction.h>
#include <script/script.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

modern::AssetId TestAsset()
{
    modern::AssetId asset;
    for (size_t i{0}; i < asset.size(); ++i) asset.begin()[i] = static_cast<unsigned char>(i + 1);
    return asset;
}

CScript TestOwner(const unsigned char fill = 0x42)
{
    return CScript() << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, fill)
                     << OP_EQUALVERIFY << OP_CHECKSIG;
}

std::vector<unsigned char> Payload(const CTxOut& out)
{
    CScript::const_iterator pc{out.scriptPubKey.begin()};
    opcodetype opcode;
    std::vector<unsigned char> data;
    BOOST_REQUIRE(out.scriptPubKey.GetOp(pc, opcode, data));
    return data;
}

CTxOut OwnerFromPayload(const std::vector<unsigned char>& payload, const CScript& owner = TestOwner())
{
    CScript script;
    script << payload << OP_DROP;
    script.insert(script.end(), owner.begin(), owner.end());
    return CTxOut{0, script};
}

CTxOut VaultFromPayload(const std::vector<unsigned char>& payload, const CAmount native_value = 0)
{
    return CTxOut{native_value, CScript() << payload << OP_DROP << OP_FALSE};
}

uint256 TestVaultId()
{
    uint256 id;
    for (size_t i{0}; i < id.size(); ++i) id.begin()[i] = static_cast<unsigned char>(0x80 + i);
    return id;
}

uint256 TestAccountId()
{
    uint256 id;
    for (size_t i{0}; i < id.size(); ++i) id.begin()[i] = static_cast<unsigned char>(0x40 + i);
    return id;
}

Consensus::Params FlowMeshParams()
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = 101;
    params.transition_pow_length = 10;
    params.legacy_final_hash = uint256::ONE;
    params.modern_pos.emplace();
    params.fn_pod_activation_height = 120;
    params.asset_activation_height = 130;
    params.flowmesh_activation_height = 130 + Consensus::FLOWMESH_ANCHOR_DEPTH;
    return params;
}

} // namespace

BOOST_AUTO_TEST_SUITE(asset_output_tests)

BOOST_AUTO_TEST_CASE(owner_and_fn_roundtrip_with_frozen_wire_fields)
{
    const modern::AssetId asset{TestAsset()};
    const CScript owner{TestOwner()};
    constexpr CAmount amount{0x0001020304050607};

    for (const modern::PolicyType policy : {modern::PolicyType::OWNER, modern::PolicyType::FN}) {
        const auto made{modern::MakeAssetOwnerOutput(asset, amount, policy, owner)};
        BOOST_REQUIRE(made.has_value());
        BOOST_CHECK_EQUAL(made->nValue, 0);
        BOOST_CHECK(modern::ClaimsAssetOutput(*made));
        BOOST_CHECK(modern::IsAssetOwnerOutput(*made));
        BOOST_CHECK(!modern::IsAssetBurnOutput(*made));

        const std::vector<unsigned char> payload{Payload(*made)};
        BOOST_REQUIRE_EQUAL(payload.size(), modern::ASSET_OUTPUT_HEADER_SIZE);
        BOOST_CHECK(std::equal(modern::ASSET_OUTPUT_MAGIC.begin(),
                               modern::ASSET_OUTPUT_MAGIC.end(), payload.begin()));
        BOOST_CHECK(std::equal(asset.begin(), asset.end(), payload.begin() + 4));
        BOOST_CHECK_EQUAL(ReadBE64(payload.data() + 36), static_cast<uint64_t>(amount));
        BOOST_CHECK_EQUAL(ReadBE16(payload.data() + 44), static_cast<uint16_t>(policy));
        BOOST_CHECK_EQUAL(ReadBE16(payload.data() + 46), modern::POLICY_VERSION_V1);

        std::string error;
        const auto parsed{modern::ParseAssetOutput(*made, error)};
        BOOST_REQUIRE_MESSAGE(parsed.has_value(), error);
        BOOST_CHECK(parsed->asset == asset);
        BOOST_CHECK_EQUAL(parsed->amount, amount);
        BOOST_CHECK_EQUAL(parsed->policy_type, static_cast<uint16_t>(policy));
        BOOST_CHECK_EQUAL(parsed->policy_version, modern::POLICY_VERSION_V1);
        BOOST_CHECK(parsed->policy_params.empty());
        BOOST_CHECK(parsed->policy_commitment == modern::AssetOwnerCommitment(owner));
        BOOST_REQUIRE(modern::AssetOwnerScript(*made).has_value());
        BOOST_CHECK(*modern::AssetOwnerScript(*made) == owner);

        // A complete ModernOutput can be re-carried only when its derived
        // commitment is the exact owner-script hash.
        BOOST_REQUIRE(modern::MakeAssetOwnerOutput(*parsed, owner).has_value());
        modern::ModernOutput wrong_commitment{*parsed};
        wrong_commitment.policy_commitment = uint256::ONE;
        BOOST_CHECK(!modern::MakeAssetOwnerOutput(wrong_commitment, owner));
    }
}

BOOST_AUTO_TEST_CASE(claims_fail_closed_and_require_a_minimal_exact_grammar)
{
    const auto valid{modern::MakeAssetOwnerOutput(TestAsset(), 7, TestOwner())};
    BOOST_REQUIRE(valid.has_value());
    const std::vector<unsigned char> payload{Payload(*valid)};
    std::string error;

    BOOST_CHECK(!modern::ClaimsAssetOutput(CScript() << OP_TRUE));
    BOOST_CHECK(modern::CheckAssetOutput(CTxOut{0, CScript() << OP_TRUE}, error));

    // A truncated push still claims once its visible data starts B3A1.
    CScript truncated;
    truncated.push_back(static_cast<unsigned char>(modern::ASSET_OUTPUT_HEADER_SIZE));
    truncated.insert(truncated.end(), modern::ASSET_OUTPUT_MAGIC.begin(),
                     modern::ASSET_OUTPUT_MAGIC.end());
    BOOST_CHECK(modern::ClaimsAssetOutput(truncated));
    BOOST_CHECK(!modern::ParseAssetOutput(CTxOut{0, truncated}, error));

    // PUSHDATA1 carries the right bytes but is non-minimal for 48 bytes.
    CScript nonminimal;
    nonminimal.push_back(OP_PUSHDATA1);
    nonminimal.push_back(static_cast<unsigned char>(payload.size()));
    nonminimal.insert(nonminimal.end(), payload.begin(), payload.end());
    nonminimal << OP_DROP;
    const CScript owner{TestOwner()};
    nonminimal.insert(nonminimal.end(), owner.begin(), owner.end());
    BOOST_REQUIRE(modern::ClaimsAssetOutput(nonminimal));
    BOOST_CHECK(!modern::ParseAssetOutput(CTxOut{0, nonminimal}, error));
    BOOST_CHECK_EQUAL(error, "asset payload not minimally encoded");

    BOOST_CHECK(!modern::ParseAssetOutput(CTxOut{0, CScript() << payload << OP_TRUE}, error));
    BOOST_CHECK_EQUAL(error, "asset payload not followed by OP_DROP");
    BOOST_CHECK(!modern::ParseAssetOutput(OwnerFromPayload(payload, CScript{}), error));
    BOOST_CHECK_EQUAL(error, "asset owner suffix is empty");

    std::vector<unsigned char> with_params{payload};
    with_params.push_back(0x00);
    BOOST_CHECK(!modern::ParseAssetOutput(OwnerFromPayload(with_params), error));
    BOOST_CHECK_EQUAL(error, "asset output v1 policy params must be empty");

    std::vector<unsigned char> oversized{payload};
    oversized.resize(modern::ASSET_OUTPUT_MAX_PAYLOAD_SIZE + 1, 0x00);
    BOOST_REQUIRE(modern::ClaimsAssetOutput(OwnerFromPayload(oversized)));
    BOOST_CHECK(!modern::ParseAssetOutput(OwnerFromPayload(oversized), error));
    BOOST_CHECK_EQUAL(error, "asset payload has the wrong size or magic");

    std::vector<unsigned char> wrong_version{payload};
    WriteBE16(wrong_version.data() + 46, modern::POLICY_VERSION_V1 + 1);
    BOOST_CHECK(!modern::ParseAssetOutput(OwnerFromPayload(wrong_version), error));
    BOOST_CHECK_EQUAL(error, "asset output policy version is not v1");

    CMutableTransaction tx;
    tx.vout.emplace_back(0, CScript() << OP_TRUE);
    tx.vout.push_back(*valid);
    BOOST_CHECK(modern::CheckAssetOutputs(CTransaction{tx}, error));
    tx.vout.emplace_back(0, truncated);
    BOOST_CHECK(!modern::CheckAssetOutputs(CTransaction{tx}, error));
}

BOOST_AUTO_TEST_CASE(owner_suffix_is_any_nonempty_script)
{
    BOOST_CHECK(!modern::MakeAssetOwnerOutput(TestAsset(), 1, CScript{}));

    // The owner is a script, not a carrier-selected address class. A bare
    // 2-of-2 multisig suffix is preserved byte-for-byte and its exact bytes
    // determine the commitment.
    std::vector<unsigned char> pubkey_a(33, 0x11);
    std::vector<unsigned char> pubkey_b(33, 0x22);
    pubkey_a[0] = 0x02;
    pubkey_b[0] = 0x03;
    const CScript multisig{CScript() << OP_2 << pubkey_a << pubkey_b << OP_2
                                     << OP_CHECKMULTISIG};
    const auto made{modern::MakeAssetOwnerOutput(
        TestAsset(), 2, modern::PolicyType::FN, multisig)};
    BOOST_REQUIRE(made.has_value());
    std::string error;
    const auto parsed{modern::ParseAssetOutput(*made, error)};
    BOOST_REQUIRE_MESSAGE(parsed.has_value(), error);
    BOOST_REQUIRE(modern::AssetOwnerScript(*made).has_value());
    BOOST_CHECK(*modern::AssetOwnerScript(*made) == multisig);
    BOOST_CHECK(parsed->policy_commitment == modern::AssetOwnerCommitment(multisig));

    // Even a minimal one-op script is a carrier-valid owner suffix; spending
    // semantics are the ordinary script interpreter's concern.
    BOOST_CHECK(modern::MakeAssetOwnerOutput(TestAsset(), 1, CScript() << OP_TRUE));
    BOOST_CHECK(!modern::MakeAssetOwnerOutput(
        TestAsset(), 1, modern::PolicyType::BURN, TestOwner()));

    // One carrier cannot be hidden inside another carrier.  Keeping the
    // envelope single-layer makes parsing, signing and script execution use
    // exactly the same owner script.
    const auto inner{modern::MakeAssetOwnerOutput(TestAsset(), 1, TestOwner())};
    BOOST_REQUIRE(inner.has_value());
    BOOST_CHECK(!modern::MakeAssetOwnerOutput(
        TestAsset(), 1, modern::PolicyType::OWNER, inner->scriptPubKey));

    const auto outer{OwnerFromPayload(Payload(*inner), inner->scriptPubKey)};
    std::string nested_error;
    BOOST_CHECK(modern::ClaimsAssetOutput(outer));
    BOOST_CHECK(!modern::ParseAssetOutput(outer, nested_error));
    BOOST_CHECK_EQUAL(nested_error,
                      "asset owner suffix cannot be another asset carrier");
}

BOOST_AUTO_TEST_CASE(explicit_burn_roundtrip_and_form_separation)
{
    const auto burn{modern::MakeAssetBurnOutput(TestAsset(), 99)};
    BOOST_REQUIRE(burn.has_value());
    BOOST_REQUIRE(!burn->scriptPubKey.empty());
    BOOST_CHECK_NE(burn->scriptPubKey[0], OP_RETURN);
    BOOST_CHECK(!burn->scriptPubKey.IsUnspendable());
    BOOST_CHECK(modern::ClaimsAssetOutput(*burn));
    BOOST_CHECK(modern::IsAssetBurnOutput(*burn));
    BOOST_CHECK(!modern::IsAssetOwnerOutput(*burn));
    BOOST_CHECK(!modern::AssetOwnerScript(*burn));

    std::string error;
    const auto parsed{modern::ParseAssetOutput(*burn, error)};
    BOOST_REQUIRE_MESSAGE(parsed.has_value(), error);
    BOOST_CHECK(parsed->asset == TestAsset());
    BOOST_CHECK_EQUAL(parsed->amount, 99);
    BOOST_CHECK_EQUAL(parsed->policy_type, static_cast<uint16_t>(modern::PolicyType::BURN));
    BOOST_CHECK_EQUAL(parsed->policy_version, modern::POLICY_VERSION_V1);
    BOOST_CHECK(parsed->policy_commitment.IsNull());
    BOOST_CHECK(parsed->policy_params.empty());
    BOOST_CHECK(modern::MakeAssetBurnOutput(*parsed).has_value());
    modern::ModernOutput nonnull_burn{*parsed};
    nonnull_burn.policy_commitment = uint256::ONE;
    BOOST_CHECK(!modern::MakeAssetBurnOutput(nonnull_burn));

    const std::vector<unsigned char> burn_payload{Payload(*burn)};
    CScript::const_iterator pc{burn->scriptPubKey.begin()};
    opcodetype opcode;
    std::vector<unsigned char> data;
    BOOST_REQUIRE(burn->scriptPubKey.GetOp(pc, opcode, data));
    BOOST_REQUIRE(burn->scriptPubKey.GetOp(pc, opcode, data));
    BOOST_CHECK_EQUAL(opcode, OP_DROP);
    BOOST_REQUIRE(burn->scriptPubKey.GetOp(pc, opcode, data));
    BOOST_CHECK_EQUAL(opcode, OP_FALSE);
    BOOST_CHECK(pc == burn->scriptPubKey.end());

    CScript trailing{burn->scriptPubKey};
    trailing << OP_TRUE;
    BOOST_CHECK(!modern::ParseAssetOutput(CTxOut{0, trailing}, error));
    BOOST_CHECK_EQUAL(error,
                      "asset BURN policy requires the exact OP_FALSE terminator");

    std::vector<unsigned char> owner_payload{burn_payload};
    WriteBE16(owner_payload.data() + 44, static_cast<uint16_t>(modern::PolicyType::OWNER));
    // The retired OP_RETURN draft remains a namespace claim so it is rejected
    // fail-closed, but it is never an accepted asset carrier.
    const CTxOut op_return_draft{0, CScript() << OP_RETURN << burn_payload};
    BOOST_CHECK(modern::ClaimsAssetOutput(op_return_draft));
    BOOST_CHECK(!modern::ParseAssetOutput(
        op_return_draft, error));
    BOOST_CHECK_EQUAL(error, "OP_RETURN is not an asset output carrier");
    // The policy field, not the suffix opcode, supplies the semantics. An
    // OWNER sent to OP_FALSE remains an (unspendable) OWNER output and is not
    // silently reclassified as an explicit BURN.
    const CTxOut false_owner{0, CScript() << owner_payload << OP_DROP << OP_FALSE};
    BOOST_REQUIRE(modern::ParseAssetOutput(false_owner, error));
    BOOST_CHECK(modern::IsAssetOwnerOutput(false_owner));
    BOOST_CHECK(!modern::IsAssetBurnOutput(false_owner));
    BOOST_CHECK(!modern::ParseAssetOutput(OwnerFromPayload(burn_payload), error));

    // A BURN-policy Modern output is standard even when OP_RETURN data relay
    // is disabled; the unrelated data-carrier budget must not govern it.
    CMutableTransaction burn_tx;
    burn_tx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    burn_tx.vout.push_back(*burn);
    std::string standardness_error;
    BOOST_CHECK(IsStandardTx(CTransaction{burn_tx}, /*max_datacarrier_bytes=*/0,
                             /*permit_bare_multisig=*/true,
                             CFeeRate{DUST_RELAY_TX_FEE}, standardness_error,
                             /*enable_asset_owner=*/true));
}

BOOST_AUTO_TEST_CASE(burn_policy_output_never_enters_the_modern_utxo_set)
{
    const auto owner{modern::MakeAssetOwnerOutput(TestAsset(), 98, TestOwner())};
    const auto burn{modern::MakeAssetBurnOutput(TestAsset(), 1)};
    BOOST_REQUIRE(owner.has_value());
    BOOST_REQUIRE(burn.has_value());

    CMutableTransaction mtx;
    mtx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    mtx.vout = {*owner, *burn};
    const CTransaction tx{mtx};

    CCoinsView modern_base;
    CCoinsViewCache modern_view{&modern_base};
    AddCoins(modern_view, tx, 810'001, /*check=*/false, /*nTxOffset=*/0,
             /*exclude_modern_cells=*/true);
    BOOST_CHECK(modern_view.HaveCoin(COutPoint{tx.GetHash(), 0}));
    BOOST_CHECK(!modern_view.HaveCoin(COutPoint{tx.GetHash(), 1}));

    // The same bytes in sealed history are ordinary script outputs. The
    // modern-only exclusion cannot rewrite legacy UTXO semantics.
    CCoinsView legacy_base;
    CCoinsViewCache legacy_view{&legacy_base};
    AddCoins(legacy_view, tx, 810'000, /*check=*/false, /*nTxOffset=*/0,
             /*exclude_modern_cells=*/false);
    BOOST_CHECK(legacy_view.HaveCoin(COutPoint{tx.GetHash(), 1}));
}

BOOST_AUTO_TEST_CASE(native_amount_and_native_value_are_rejected)
{
    const modern::AssetId asset{TestAsset()};
    const CScript owner{TestOwner()};
    BOOST_CHECK(!modern::MakeAssetOwnerOutput(modern::NativeAsset(), 1, owner));
    BOOST_CHECK(!modern::MakeAssetBurnOutput(modern::NativeAsset(), 1));
    BOOST_CHECK(!modern::MakeAssetOwnerOutput(asset, 0, owner));
    BOOST_CHECK(!modern::MakeAssetOwnerOutput(asset, -1, owner));
    BOOST_CHECK(!modern::MakeAssetOwnerOutput(asset, MAX_MONEY + 1, owner));
    BOOST_CHECK(!modern::MakeAssetBurnOutput(asset, 0));
    BOOST_CHECK(!modern::MakeAssetBurnOutput(asset, MAX_MONEY + 1));

    const auto valid{modern::MakeAssetOwnerOutput(asset, MAX_MONEY, owner)};
    BOOST_REQUIRE(valid.has_value());
    std::string error;
    BOOST_REQUIRE(modern::ParseAssetOutput(*valid, error));

    CTxOut nonzero_value{*valid};
    nonzero_value.nValue = 1;
    BOOST_CHECK(!modern::ParseAssetOutput(nonzero_value, error));
    BOOST_CHECK_EQUAL(error, "asset output must have zero native value");

    std::vector<unsigned char> payload{Payload(*valid)};
    std::fill(payload.begin() + 4, payload.begin() + 36, 0x00);
    BOOST_CHECK(!modern::ParseAssetOutput(OwnerFromPayload(payload, owner), error));
    BOOST_CHECK_EQUAL(error, "asset output must carry a non-native asset");

    payload = Payload(*valid);
    WriteBE64(payload.data() + 36, 0);
    BOOST_CHECK(!modern::ParseAssetOutput(OwnerFromPayload(payload, owner), error));
    BOOST_CHECK_EQUAL(error, "asset amount outside [1, MAX_MONEY]");
    WriteBE64(payload.data() + 36, static_cast<uint64_t>(MAX_MONEY) + 1);
    BOOST_CHECK(!modern::ParseAssetOutput(OwnerFromPayload(payload, owner), error));
}

BOOST_AUTO_TEST_CASE(dex_vault_v2_roundtrips_exact_raw_and_semantic_params)
{
    const uint256 vault_id{TestVaultId()};
    const uint256 account{TestAccountId()};
    const uint16_t shard{modern::FlowMeshUserDepositShard(vault_id, account)};

    const auto native{modern::MakeDexVaultOutput(
        modern::NativeAsset(), 123, vault_id, modern::VAULT_KIND_USER_DEPOSIT,
        shard, account)};
    BOOST_REQUIRE(native.has_value());
    BOOST_CHECK_EQUAL(native->nValue, 123);
    BOOST_CHECK_NE(native->scriptPubKey[0], OP_RETURN);
    BOOST_CHECK(!native->scriptPubKey.IsUnspendable());
    BOOST_CHECK(modern::IsDexVaultOutput(*native));
    BOOST_CHECK(!modern::IsAssetBurnOutput(*native));
    BOOST_CHECK(!modern::IsAssetOwnerOutput(*native));

    const std::vector<unsigned char> user_wire{Payload(*native)};
    BOOST_REQUIRE_EQUAL(user_wire.size(), modern::ASSET_OUTPUT_HEADER_SIZE + 67);
    BOOST_CHECK(std::equal(vault_id.begin(), vault_id.end(),
                           user_wire.begin() + modern::ASSET_OUTPUT_HEADER_SIZE));
    const size_t semantic{modern::ASSET_OUTPUT_HEADER_SIZE + 32};
    BOOST_CHECK_EQUAL(user_wire[semantic], modern::VAULT_KIND_USER_DEPOSIT);
    BOOST_CHECK_EQUAL(ReadLE16(user_wire.data() + semantic + 1), shard);
    BOOST_CHECK(std::equal(account.begin(), account.end(), user_wire.begin() + semantic + 3));

    std::string error;
    const auto parsed_native{modern::ParseAssetOutput(*native, error)};
    BOOST_REQUIRE_MESSAGE(parsed_native.has_value(), error);
    BOOST_CHECK(parsed_native->asset == modern::NativeAsset());
    BOOST_CHECK_EQUAL(parsed_native->amount, 123);
    BOOST_CHECK(parsed_native->policy_commitment == vault_id);
    BOOST_REQUIRE_EQUAL(parsed_native->policy_params.size(),
                        modern::VAULT_USER_DEPOSIT_PARAMS_SIZE);
    BOOST_CHECK(std::equal(parsed_native->policy_params.begin(),
                           parsed_native->policy_params.end(), user_wire.begin() + semantic));

    const auto colored{modern::MakeDexVaultOutput(
        TestAsset(), 456, vault_id, modern::VAULT_KIND_POOL_CHANGE, 17)};
    BOOST_REQUIRE(colored.has_value());
    BOOST_CHECK_EQUAL(colored->nValue, 0);
    const std::vector<unsigned char> pool_wire{Payload(*colored)};
    BOOST_REQUIRE_EQUAL(pool_wire.size(), modern::ASSET_OUTPUT_HEADER_SIZE + 35);
    const auto parsed_colored{modern::ParseAssetOutput(*colored, error)};
    BOOST_REQUIRE_MESSAGE(parsed_colored.has_value(), error);
    BOOST_CHECK(parsed_colored->asset == TestAsset());
    BOOST_CHECK(parsed_colored->policy_commitment == vault_id);
    BOOST_REQUIRE_EQUAL(parsed_colored->policy_params.size(),
                        modern::VAULT_POOL_CHANGE_PARAMS_SIZE);
    BOOST_CHECK_EQUAL(parsed_colored->policy_params[0], modern::VAULT_KIND_POOL_CHANGE);
    BOOST_CHECK_EQUAL(ReadLE16(parsed_colored->policy_params.data() + 1), 17);

    CScript::const_iterator pc{colored->scriptPubKey.begin()};
    opcodetype opcode;
    std::vector<unsigned char> data;
    BOOST_REQUIRE(colored->scriptPubKey.GetOp(pc, opcode, data));
    BOOST_REQUIRE(colored->scriptPubKey.GetOp(pc, opcode, data));
    BOOST_CHECK_EQUAL(opcode, OP_DROP);
    BOOST_REQUIRE(colored->scriptPubKey.GetOp(pc, opcode, data));
    BOOST_CHECK_EQUAL(opcode, OP_FALSE);
    BOOST_CHECK(pc == colored->scriptPubKey.end());
}

BOOST_AUTO_TEST_CASE(dex_vault_v2_rejects_noncanonical_wire_value_and_suffix)
{
    const uint256 vault_id{TestVaultId()};
    const uint256 account{TestAccountId()};
    const uint16_t shard{modern::FlowMeshUserDepositShard(vault_id, account)};
    const auto user{modern::MakeDexVaultOutput(
        TestAsset(), 50, vault_id, modern::VAULT_KIND_USER_DEPOSIT, shard, account)};
    const auto native{modern::MakeDexVaultOutput(
        modern::NativeAsset(), 50, vault_id, modern::VAULT_KIND_POOL_CHANGE, 9)};
    BOOST_REQUIRE(user.has_value());
    BOOST_REQUIRE(native.has_value());
    std::string error;

    CTxOut wrong_value{*user};
    wrong_value.nValue = 1;
    BOOST_CHECK(!modern::ParseAssetOutput(wrong_value, error));
    CTxOut wrong_native_value{*native};
    wrong_native_value.nValue = 49;
    BOOST_CHECK(!modern::ParseAssetOutput(wrong_native_value, error));

    std::vector<unsigned char> payload{Payload(*user)};
    const size_t wire{modern::ASSET_OUTPUT_HEADER_SIZE};
    const size_t semantic{wire + 32};

    std::vector<unsigned char> malformed{payload};
    std::fill(malformed.begin() + wire, malformed.begin() + semantic, 0);
    BOOST_CHECK(!modern::ParseAssetOutput(VaultFromPayload(malformed), error));

    malformed = payload;
    malformed[semantic] = 0xff;
    BOOST_CHECK(!modern::ParseAssetOutput(VaultFromPayload(malformed), error));

    malformed = payload;
    WriteLE16(malformed.data() + semantic + 1, static_cast<uint16_t>((shard + 1) % 256));
    BOOST_CHECK(!modern::ParseAssetOutput(VaultFromPayload(malformed), error));

    malformed = payload;
    std::fill(malformed.begin() + semantic + 3, malformed.end(), 0);
    BOOST_CHECK(!modern::ParseAssetOutput(VaultFromPayload(malformed), error));

    uint256 wrong_account{account};
    for (unsigned int byte{1};
         modern::FlowMeshUserDepositShard(vault_id, wrong_account) == shard;
         ++byte) {
        BOOST_REQUIRE_LE(byte, 255U);
        wrong_account.begin()[0] = static_cast<unsigned char>(byte);
    }
    malformed = payload;
    std::copy(wrong_account.begin(), wrong_account.end(), malformed.begin() + semantic + 3);
    BOOST_CHECK(!modern::ParseAssetOutput(VaultFromPayload(malformed), error));

    malformed = payload;
    WriteLE16(malformed.data() + semantic + 1, 256);
    BOOST_CHECK(!modern::ParseAssetOutput(VaultFromPayload(malformed), error));

    malformed = payload;
    malformed.pop_back();
    BOOST_CHECK(!modern::ParseAssetOutput(VaultFromPayload(malformed), error));

    malformed = payload;
    WriteBE16(malformed.data() + 46, modern::POLICY_VERSION_V1);
    BOOST_CHECK(!modern::ParseAssetOutput(VaultFromPayload(malformed), error));

    const CTxOut wrong_suffix{0, CScript() << payload << OP_DROP << OP_TRUE};
    BOOST_CHECK(!modern::ParseAssetOutput(wrong_suffix, error));
    CScript trailing{user->scriptPubKey};
    trailing << OP_TRUE;
    BOOST_CHECK(!modern::ParseAssetOutput(CTxOut{0, trailing}, error));

    BOOST_CHECK(!modern::MakeDexVaultOutput(
        TestAsset(), 1, uint256{}, modern::VAULT_KIND_POOL_CHANGE, 0));
    BOOST_CHECK(!modern::MakeDexVaultOutput(
        TestAsset(), 1, vault_id, modern::VAULT_KIND_USER_DEPOSIT,
        static_cast<uint16_t>((shard + 1) % 256), account));
}

BOOST_AUTO_TEST_CASE(dex_vault_prepares_at_a2_and_remains_a_utxo)
{
    Consensus::Params params{FlowMeshParams()};
    params.test_only_asset_policies_active = true; // must not bypass B3A1's schedule gate
    const int A2{*params.asset_activation_height};
    const auto vault{modern::MakeDexVaultOutput(
        TestAsset(), 99, TestVaultId(), modern::VAULT_KIND_POOL_CHANGE, 4)};
    const auto native_vault{modern::MakeDexVaultOutput(
        modern::NativeAsset(), 25, TestVaultId(), modern::VAULT_KIND_POOL_CHANGE, 5)};
    const auto burn{modern::MakeAssetBurnOutput(TestAsset(), 1)};
    BOOST_REQUIRE(vault.has_value());
    BOOST_REQUIRE(native_vault.has_value());
    BOOST_REQUIRE(burn.has_value());

    std::string error;
    BOOST_CHECK(!modern::ViewAssetAwareOutput(*vault, A2 - 1, params, error));
    BOOST_REQUIRE_MESSAGE(modern::ViewAssetAwareOutput(*vault, A2, params, error), error);
    BOOST_REQUIRE_MESSAGE(modern::ViewAssetAwareOutput(*native_vault, A2, params, error), error);

    CMutableTransaction mtx;
    mtx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    mtx.vout = {*vault, *native_vault, *burn};
    const CTransaction tx{mtx};
    CCoinsView base;
    CCoinsViewCache view{&base};
    AddCoins(view, tx, A2, /*check=*/false, /*nTxOffset=*/0,
             /*exclude_modern_cells=*/true);
    BOOST_CHECK(view.HaveCoin(COutPoint{tx.GetHash(), 0}));
    BOOST_CHECK(view.HaveCoin(COutPoint{tx.GetHash(), 1}));
    BOOST_CHECK(!view.HaveCoin(COutPoint{tx.GetHash(), 2}));

    std::string standardness_error;
    BOOST_CHECK(IsStandardTx(tx, /*max_datacarrier_bytes=*/0,
                             /*permit_bare_multisig=*/true,
                             CFeeRate{DUST_RELAY_TX_FEE}, standardness_error,
                             /*enable_asset_owner=*/true));
}

BOOST_AUTO_TEST_SUITE_END()
