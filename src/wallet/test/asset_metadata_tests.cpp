// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/asset_metadata.h>

#include <chainparams.h>
#include <consensus/era.h>
#include <core_io.h>
#include <interfaces/wallet.h>
#include <key.h>
#include <modern/asset.h>
#include <modern/asset_output.h>
#include <modern/asset_validation.h>
#include <modern/bridge_asset.h>
#include <streams.h>
#include <rpc/request.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <test/util/setup_common.h>
#include <wallet/context.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <boost/test/unit_test.hpp>
#include <boost/signals2/connection.hpp>

#include <algorithm>

namespace wallet {
RPCHelpMan setassetmetadata();
RPCHelpMan clearassetmetadata();
RPCHelpMan getwalletassets();
namespace {
AssetMetadataProof TestProof(uint8_t decimals = 6)
{
    return {COutPoint{Txid::FromUint256(uint256::ONE), 7}, 1'000'000'000'000, decimals};
}

uint256 ProofId(const AssetMetadataProof& proof)
{
    return modern::AssetIdV1(AssetMetadataDomain(Params().GetConsensus()).value(),
                             proof.issuance_prevout,
                             modern::AssetGenesisCommitment({.max_supply = proof.max_supply,
                                                              .decimals = proof.decimals}));
}

CMutableTransaction Issuance(const AssetMetadataProof& proof)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.vin.emplace_back(proof.issuance_prevout);
    const auto action{modern::MakeAssetIssuanceAction({.max_supply = proof.max_supply,
                                                     .decimals = proof.decimals})};
    tx.mpa.emplace_back(action.action_type, action.action_version, action.payload);
    const CScript owner{CScript{} << OP_TRUE};
    tx.vout.push_back(modern::MakeAssetOwnerOutput(ProofId(proof), proof.max_supply,
                                                  modern::PolicyType::OWNER, owner).value());
    return tx;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(asset_metadata_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(precision_is_bound_to_genesis_chain_and_anchor)
{
    const auto domain{AssetMetadataDomain(Params().GetConsensus()).value()};
    for (uint8_t decimals : {0, 6, 18}) {
        const auto proof{TestProof(decimals)};
        const auto asset{ProofId(proof)};
        BOOST_CHECK(AssetMetadataProofMatches(domain, asset, proof));
        auto changed{proof};
        ++changed.decimals;
        BOOST_CHECK(!AssetMetadataProofMatches(domain, asset, changed));
        changed = proof;
        ++changed.max_supply;
        BOOST_CHECK(!AssetMetadataProofMatches(domain, asset, changed));
        changed = proof;
        ++changed.issuance_prevout.n;
        BOOST_CHECK(!AssetMetadataProofMatches(domain, asset, changed));
        BOOST_CHECK(!AssetMetadataProofMatches(uint256::ONE, asset, proof));
        BOOST_CHECK(!AssetMetadataProofMatches(domain, uint256::ZERO, proof));
        uint256 decoded_id;
        AssetMetadataProof decoded;
        std::string error;
        BOOST_REQUIRE(DecodeAssetMetadataProof(CTransaction{Issuance(proof)}, domain,
                                                decoded_id, decoded, error));
        BOOST_CHECK(decoded_id == asset);
        BOOST_CHECK_EQUAL(decoded.max_supply, proof.max_supply);
        BOOST_CHECK_EQUAL(decoded.decimals, proof.decimals);
    }
}

BOOST_AUTO_TEST_CASE(reject_ambiguous_malformed_and_nonissuance_proofs)
{
    const auto domain{AssetMetadataDomain(Params().GetConsensus()).value()};
    const auto valid{Issuance(TestProof())};
    uint256 asset;
    AssetMetadataProof proof;
    std::string error;
    auto check_invalid = [&](const CMutableTransaction& tx) {
        BOOST_CHECK(!DecodeAssetMetadataProof(CTransaction{tx}, domain, asset, proof, error));
    };
    auto tx{valid};
    tx.mpa.clear(); // A recipient's transfer does not carry issuance precision.
    check_invalid(tx);
    tx = valid;
    tx.mpa.push_back(tx.mpa.front());
    check_invalid(tx);
    tx = valid;
    tx.mpa.front().payload.push_back(0);
    check_invalid(tx);
    tx = valid;
    tx.mpa.front().payload_version = 2;
    check_invalid(tx);
    tx = valid;
    tx.mpa.front().payload[8] = 19;
    check_invalid(tx);
    tx = valid;
    tx.mpa.front().payload[9] = modern::ASSET_ISSUANCE_MODE_AUTHORITY_MINT;
    check_invalid(tx);
    tx = valid;
    std::fill_n(tx.mpa.front().payload.begin(), 8, 0); // Zero supply.
    check_invalid(tx);
    tx = valid;
    tx.vin.clear();
    check_invalid(tx);
    tx = valid;
    tx.vin.front().prevout.SetNull();
    check_invalid(tx);
}

BOOST_AUTO_TEST_CASE(labels_are_bounded_plain_text_and_cannot_spoof_configured_assets)
{
    std::string error;
    BOOST_CHECK(ValidAssetMetadataLabels("Example Token", "EXM", error));
    BOOST_CHECK(ValidAssetMetadataLabels(std::string(64, 'A'), std::string(12, 'A'), error));
    for (const std::string name : {"", " token", "token ", "<b>token</b>", "token\nname", "FN Coin", "Test-USD", "Bridged USD"}) {
        BOOST_CHECK(!ValidAssetMetadataLabels(name, "EXM", error));
    }
    BOOST_CHECK(!ValidAssetMetadataLabels(std::string(65, 'A'), "EXM", error));
    BOOST_CHECK(!ValidAssetMetadataLabels("Example", std::string(13, 'A'), error));
    BOOST_CHECK(!ValidAssetMetadataLabels("Ex\xc3\xa4mple", "EXM", error));
    for (const std::string ticker : {"B3", "fn", "BUSD", "TuSd", "b.u.s.d", "X Y"}) {
        BOOST_CHECK(!ValidAssetMetadataLabels("Example", ticker, error));
    }
}

BOOST_AUTO_TEST_CASE(reviewed_registry_precision_is_verified_and_chain_scoped)
{
    const auto asset{*uint256::FromHex("43d4555d04fdb78726381db4e8340c6f59e5761f2945d634aef0a5d4a3c3a299")};
    auto params{Params().GetConsensus()};
    const auto metadata{ConfiguredAssetMetadata(params, asset)};
    BOOST_REQUIRE(metadata);
    BOOST_CHECK_EQUAL(metadata->display_name, "Test USD");
    BOOST_CHECK_EQUAL(metadata->ticker, "tUSD");
    BOOST_CHECK_EQUAL(metadata->decimals.value(), 6);
    BOOST_CHECK_EQUAL(metadata->source, "bundled-registry");
    BOOST_CHECK(metadata->test_only);
    CWallet wallet{nullptr, "reserved", CreateMockableWalletDatabase()};
    const AssetMetadataProof proof{
        COutPoint{Txid::FromUint256(*uint256::FromHex("59b3897ca1ce201118644247833cb4eba68cc70849a3f593b94d61da46ba150c")), 0},
        1'000'000'000'000, 6};
    {
        LOCK(wallet.cs_wallet);
        std::string error;
        BOOST_CHECK(!wallet.SetAssetMetadata(asset, {"Other Name", "OTH", proof}, error));
        BOOST_CHECK_EQUAL(wallet.GetAssetMetadata(asset).display_name, "Test USD");
        BOOST_CHECK(!wallet.ClearAssetMetadata(asset, error));
    }
    params.hashGenesisBlock = uint256::ONE;
    BOOST_CHECK(!ConfiguredAssetMetadata(params, asset));
    BOOST_REQUIRE(ConfiguredAssetMetadata(Params().GetConsensus(), modern::NativeAsset()));
}

BOOST_AUTO_TEST_CASE(wallet_learning_local_import_restart_clear_and_write_failure)
{
    const auto proof{TestProof()};
    const auto asset{ProofId(proof)};
    CWallet issuer{nullptr, "issuer", CreateMockableWalletDatabase()};
    BOOST_REQUIRE(WalletBatch{issuer.GetDatabase()}.WriteWalletFlags(WALLET_FLAG_DESCRIPTORS));
    const auto tx{MakeTransactionRef(Issuance(proof))};
    BOOST_REQUIRE(issuer.AddToWallet(tx, TxStateInactive{}));
    {
        LOCK(issuer.cs_wallet);
        const auto metadata{issuer.GetAssetMetadata(asset)};
        BOOST_CHECK_EQUAL(metadata.decimals.value(), 6);
        BOOST_CHECK(metadata.display_name.empty());
        BOOST_CHECK_EQUAL(metadata.source, "wallet-issuance");
        BOOST_CHECK_EQUAL(issuer.m_asset_genesis.size(), 1);
        // Re-observing or reorging the same issuance cannot mutate its precision.
        issuer.LearnAssetMetadata(*tx);
        BOOST_CHECK_EQUAL(issuer.m_asset_genesis.size(), 1);
    }
    CWallet restored_issuer{nullptr, "restored-issuer", DuplicateMockDatabase(issuer.GetDatabase())};
    BOOST_REQUIRE(WalletBatch{restored_issuer.GetDatabase()}.LoadWallet(&restored_issuer) == DBErrors::LOAD_OK);
    {
        LOCK(restored_issuer.cs_wallet);
        BOOST_CHECK_EQUAL(restored_issuer.GetAssetMetadata(asset).decimals.value(), 6);
    }

    CWallet recipient{nullptr, "recipient", CreateMockableWalletDatabase()};
    BOOST_REQUIRE(WalletBatch{recipient.GetDatabase()}.WriteWalletFlags(WALLET_FLAG_DESCRIPTORS));
    LocalAssetMetadata entry{"Example Token", "EXM", proof};
    std::string error;
    {
        LOCK(recipient.cs_wallet);
        BOOST_CHECK(!recipient.GetAssetMetadata(asset).decimals);
        BOOST_REQUIRE(recipient.SetAssetMetadata(asset, entry, error));
        const auto metadata{recipient.GetAssetMetadata(asset)};
        BOOST_CHECK_EQUAL(metadata.display_name, "Example Token");
        BOOST_CHECK_EQUAL(metadata.decimals.value(), 6);
        BOOST_CHECK_EQUAL(metadata.source, "local-registry");
        auto bad{entry};
        bad.proof.decimals = 2;
        BOOST_CHECK(!recipient.SetAssetMetadata(asset, bad, error));
        BOOST_CHECK_EQUAL(recipient.GetAssetMetadata(asset).decimals.value(), 6);
    }
    CWallet restored{nullptr, "restored", DuplicateMockDatabase(recipient.GetDatabase())};
    BOOST_REQUIRE(WalletBatch{restored.GetDatabase()}.LoadWallet(&restored) == DBErrors::LOAD_OK);
    {
        LOCK(restored.cs_wallet);
        BOOST_CHECK_EQUAL(restored.GetAssetMetadata(asset).display_name, "Example Token");
        BOOST_CHECK_EQUAL(restored.GetAssetMetadata(asset).decimals.value(), 6);
        GetMockableDatabase(restored).m_pass = false;
        auto renamed{entry};
        renamed.name = "Other Token";
        BOOST_CHECK(!restored.SetAssetMetadata(asset, renamed, error));
        BOOST_CHECK_EQUAL(restored.GetAssetMetadata(asset).display_name, "Example Token");
        BOOST_CHECK(!restored.ClearAssetMetadata(asset, error));
        BOOST_CHECK_EQUAL(restored.GetAssetMetadata(asset).display_name, "Example Token");
        GetMockableDatabase(restored).m_pass = true;
        error.clear();
        BOOST_REQUIRE(restored.ClearAssetMetadata(asset, error));
        BOOST_CHECK(!restored.GetAssetMetadata(asset).decimals);
        BOOST_CHECK(!restored.ClearAssetMetadata(asset, error));
    }
    CWallet cleared{nullptr, "cleared", DuplicateMockDatabase(restored.GetDatabase())};
    BOOST_REQUIRE(WalletBatch{cleared.GetDatabase()}.LoadWallet(&cleared) == DBErrors::LOAD_OK);
    LOCK(cleared.cs_wallet);
    BOOST_CHECK(!cleared.GetAssetMetadata(asset).decimals);
}

BOOST_AUTO_TEST_CASE(corrupt_precision_record_is_ignored_without_loading_bad_metadata)
{
    CWallet stored{nullptr, "stored", CreateMockableWalletDatabase()};
    WalletBatch batch{stored.GetDatabase()};
    BOOST_REQUIRE(batch.WriteWalletFlags(WALLET_FLAG_DESCRIPTORS));
    auto proof{TestProof()};
    const auto asset{ProofId(proof)};
    ++proof.decimals;
    BOOST_REQUIRE(batch.WriteAssetMetadata(AssetMetadataDomain(Params().GetConsensus()).value(),
                                           asset, {"Example", "EXM", proof}));
    CWallet loaded{nullptr, "loaded", DuplicateMockDatabase(stored.GetDatabase())};
    BOOST_CHECK(WalletBatch{loaded.GetDatabase()}.LoadWallet(&loaded) == DBErrors::NONCRITICAL_ERROR);
    LOCK(loaded.cs_wallet);
    BOOST_CHECK(!loaded.GetAssetMetadata(asset).decimals);
}

BOOST_AUTO_TEST_CASE(other_chain_registry_records_are_not_used)
{
    CWallet wallet{nullptr, "other-chain", CreateMockableWalletDatabase()};
    const auto proof{TestProof()};
    const uint256 other_domain{uint256::ONE};
    const auto other_id{modern::AssetIdV1(other_domain, proof.issuance_prevout,
        modern::AssetGenesisCommitment({.max_supply = proof.max_supply, .decimals = proof.decimals}))};
    std::string error;
    LOCK(wallet.cs_wallet);
    BOOST_REQUIRE(wallet.LoadAssetMetadata(other_domain, other_id, {"Example", "EXM", proof}, error));
    BOOST_CHECK(!wallet.GetAssetMetadata(other_id).decimals);
    BOOST_CHECK(wallet.m_asset_metadata.empty());
}

BOOST_AUTO_TEST_SUITE_END()

namespace {
struct AssetMetadataRpcSetup : WalletTestingSetup {
    WalletContext context;
    std::shared_ptr<CWallet> wallet_alias;

    AssetMetadataRpcSetup() : wallet_alias{&m_wallet, [](CWallet*) {}}
    {
        context.chain = m_node.chain.get();
        WITH_LOCK(context.wallets_mutex, context.wallets.push_back(wallet_alias));
    }

    UniValue Call(RPCHelpMan method, UniValue params = UniValue{UniValue::VARR})
    {
        JSONRPCRequest request;
        request.context = &context;
        request.strMethod = method.m_name;
        request.params = std::move(params);
        return method.HandleRequest(request);
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(asset_metadata_rpc_tests, AssetMetadataRpcSetup)

BOOST_AUTO_TEST_CASE(local_registry_rpc_matches_wallet_interface_and_notifies_without_new_tip)
{
    const auto proof{TestProof()};
    const auto asset{ProofId(proof)};
    CKey key;
    key.MakeNewKey(true);
    const CScript owner{GetScriptForDestination(PKHash{key.GetPubKey()})};
    CMutableTransaction transfer;
    transfer.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 8});
    transfer.vout.push_back(modern::MakeAssetOwnerOutput(asset, 12'345'678,
                                                       modern::PolicyType::OWNER, owner).value());
    const auto block{uint256{2}};
    const int height{Consensus::LegacyFinalHeight(Params().GetConsensus()).value() + 10};
    {
        LOCK(m_wallet.cs_wallet);
        auto* legacy{m_wallet.GetOrCreateLegacyDataSPKM()};
        BOOST_REQUIRE(legacy);
        BOOST_REQUIRE(legacy->LoadKey(key, key.GetPubKey()));
        m_wallet.CacheNewScriptPubKeys({owner}, legacy);
        BOOST_REQUIRE(m_wallet.AddToWallet(MakeTransactionRef(transfer), TxStateConfirmed{block, height, 1}));
        m_wallet.SetLastBlockProcessed(height, block);
    }
    int notifications{0};
    boost::signals2::scoped_connection notification{
        m_wallet.NotifyStatusChanged.connect([&](CWallet*) { ++notifications; })};
    UniValue filter{UniValue::VARR};
    filter.push_back(asset.GetHex());
    const auto before{Call(getwalletassets(), filter)};
    BOOST_REQUIRE_EQUAL(before["assets"].size(), 1);
    BOOST_CHECK(!before["assets"][0]["precision_known"].get_bool());
    BOOST_CHECK_EQUAL(before["assets"][0]["confirmed"].getInt<int64_t>(), 12'345'678);

    UniValue params{UniValue::VARR};
    params.push_back(asset.GetHex());
    params.push_back("Example Token");
    params.push_back("EXM");
    params.push_back(EncodeHexTx(CTransaction{Issuance(proof)}));
    const auto result{Call(setassetmetadata(), params)};
    BOOST_CHECK_EQUAL(result["decimals"].getInt<int>(), 6);
    BOOST_CHECK_EQUAL(notifications, 1);
    const auto assets{Call(getwalletassets(), filter)["assets"]};
    BOOST_REQUIRE_EQUAL(assets.size(), 1);
    BOOST_CHECK(assets[0]["precision_known"].get_bool());
    BOOST_CHECK_EQUAL(assets[0]["name"].get_str(), "Example Token");
    BOOST_CHECK_EQUAL(assets[0]["ticker"].get_str(), "EXM");
    BOOST_CHECK_EQUAL(assets[0]["decimals"].getInt<int>(), 6);
    BOOST_CHECK_EQUAL(assets[0]["metadata_source"].get_str(), "local-registry");
    BOOST_CHECK(!assets[0]["test_only"].get_bool());
    BOOST_CHECK_EQUAL(assets[0]["confirmed"].getInt<int64_t>(), 12'345'678);
    const auto interface{interfaces::MakeWallet(context, wallet_alias)};
    const auto balances{interface->getAssetBalances()};
    BOOST_REQUIRE_EQUAL(balances.size(), 1);
    BOOST_CHECK_EQUAL(balances[0].display_name, assets[0]["name"].get_str());
    BOOST_CHECK_EQUAL(balances[0].ticker, assets[0]["ticker"].get_str());
    BOOST_CHECK_EQUAL(balances[0].decimals.value(), assets[0]["decimals"].getInt<int>());
    BOOST_CHECK_EQUAL(balances[0].metadata_source, assets[0]["metadata_source"].get_str());

    UniValue wrong{UniValue::VARR};
    wrong.push_back(asset.GetHex());
    wrong.push_back("Other Token");
    wrong.push_back("OTH");
    wrong.push_back(EncodeHexTx(CTransaction{Issuance(TestProof(2))}));
    try {
        Call(setassetmetadata(), wrong);
        BOOST_FAIL("wrong proof accepted");
    } catch (const UniValue& error) {
        BOOST_CHECK_EQUAL(error["code"].getInt<int>(), RPC_INVALID_PARAMETER);
    }
    BOOST_CHECK_EQUAL(notifications, 1);
    BOOST_CHECK(Call(clearassetmetadata(), filter).get_bool());
    BOOST_CHECK_EQUAL(notifications, 2);
    BOOST_CHECK(!Call(clearassetmetadata(), filter).get_bool());
    BOOST_CHECK_EQUAL(notifications, 2);
    BOOST_CHECK(!Call(getwalletassets(), filter)["assets"][0]["precision_known"].get_bool());
    BOOST_CHECK(!interface->getAssetBalances()[0].decimals);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
