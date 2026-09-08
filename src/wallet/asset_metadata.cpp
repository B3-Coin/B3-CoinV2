// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/asset_metadata.h>

#include <chainparams.h>
#include <modern/asset.h>
#include <modern/asset_validation.h>
#include <modern/bridge_asset.h>
#include <modern/chain_domain.h>
#include <sync.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <algorithm>
#include <string_view>

namespace wallet {
namespace {
modern::AssetGenesisV1 Genesis(const AssetMetadataProof& proof)
{
    return modern::AssetGenesisV1{.max_supply = proof.max_supply, .decimals = proof.decimals};
}

bool ReservedLabel(const std::string& text)
{
    std::string lower;
    for (char c : text) {
        if (c == ' ' || c == '.' || c == '-' || c == '_') continue;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        lower.push_back(c);
    }
    return lower == "b3" || lower == "fn" || lower == "busd" || lower == "tusd" ||
           lower == "fncoin" || lower == "bridgedusd" || lower == "testusd";
}
} // namespace

std::optional<uint256> AssetMetadataDomain(const Consensus::Params& params)
{
    return modern::ModernChainDomain(params.hashGenesisBlock,
                                    params.legacy_final_hash.value_or(uint256{}));
}

bool AssetMetadataProofMatches(const uint256& domain, const uint256& asset,
                               const AssetMetadataProof& proof)
{
    const auto genesis{Genesis(proof)};
    return !domain.IsNull() && !asset.IsNull() && !proof.issuance_prevout.IsNull() &&
           modern::AssetGenesisValid(genesis) &&
           modern::AssetIdV1(domain, proof.issuance_prevout,
                             modern::AssetGenesisCommitment(genesis)) == asset;
}

bool DecodeAssetMetadataProof(const CTransaction& tx, const uint256& domain,
                              uint256& asset, AssetMetadataProof& proof, std::string& error)
{
    if (domain.IsNull() || tx.vin.empty() || tx.vin[0].prevout.IsNull()) {
        error = "Issuance proof requires a pinned chain domain and a non-null first input";
        return false;
    }
    const CMpaRecord* issuance{nullptr};
    for (const auto& record : tx.mpa) {
        if (record.payload_type != modern::CREATION_ACTION_ASSET_ISSUANCE) continue;
        if (issuance) {
            error = "Issuance proof has multiple genesis records";
            return false;
        }
        issuance = &record;
    }
    if (!issuance) {
        error = "Issuance proof has no genesis record";
        return false;
    }
    modern::AssetGenesisV1 genesis;
    if (!modern::DecodeAssetIssuanceAction(
            {issuance->payload_type, issuance->payload_version, issuance->payload}, genesis, error)) return false;
    if (!modern::AssetGenesisValid(genesis)) {
        error = "Issuance proof has invalid or unsupported genesis rules";
        return false;
    }
    proof = {tx.vin[0].prevout, genesis.max_supply, genesis.decimals};
    asset = modern::AssetIdV1(domain, proof.issuance_prevout,
                              modern::AssetGenesisCommitment(genesis));
    if (!AssetMetadataProofMatches(domain, asset, proof)) {
        error = "Issuance proof does not identify a simple-v1 asset";
        return false;
    }
    return true;
}

bool ValidAssetMetadataLabels(const std::string& name, const std::string& ticker,
                              std::string& error)
{
    const auto alnum = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    const auto safe = [&](const std::string& value, size_t limit, bool spaces) {
        return !value.empty() && value.size() <= limit && alnum(value.front()) &&
               alnum(value.back()) && std::all_of(value.begin(), value.end(), [&](unsigned char c) {
                   return alnum(c) || c == '-' || c == '_' || c == '.' || (spaces && c == ' ');
               });
    };
    if (!safe(name, 64, true) || !safe(ticker, 12, false)) {
        error = "Name (1-64 ASCII characters) and ticker (1-12) must start/end with letters or digits; only letters, digits, '.', '-', '_' and internal name spaces are allowed";
        return false;
    }
    if (ReservedLabel(name) || ReservedLabel(ticker)) {
        error = "B3, FN, bUSD, tUSD and their configured display names are reserved";
        return false;
    }
    return true;
}

std::optional<WalletAssetMetadata> ConfiguredAssetMetadata(const Consensus::Params& params,
                                                          const uint256& asset)
{
    if (asset == modern::NativeAsset()) return WalletAssetMetadata{"B3", "B3", 9, "consensus", false};
    if (const auto fn{modern::ConfiguredFnAssetId(params)}; fn && asset == *fn) {
        return WalletAssetMetadata{"FN Coin", "FN", 0, "consensus", false};
    }
    if (const auto bridge{modern::ConfiguredDecentralizedBridgeAssetId(params)}; bridge && asset == *bridge) {
        return WalletAssetMetadata{"Bridged USD", "bUSD", 6, "consensus", false};
    }
    // Reviewed mainnet label. Precision is verified from the immutable genesis
    // preimage on this chain, never inferred from the label or raw balance.
    static const uint256 test_usd{*uint256::FromHex("43d4555d04fdb78726381db4e8340c6f59e5761f2945d634aef0a5d4a3c3a299")};
    static const AssetMetadataProof proof{
        COutPoint{Txid::FromUint256(*uint256::FromHex("59b3897ca1ce201118644247833cb4eba68cc70849a3f593b94d61da46ba150c")), 0},
        1'000'000'000'000, 6};
    if (asset == test_usd) {
        const auto domain{AssetMetadataDomain(params)};
        if (domain && AssetMetadataProofMatches(*domain, asset, proof)) {
            return WalletAssetMetadata{"Test USD", "tUSD", 6, "bundled-registry", true};
        }
    }
    return std::nullopt;
}

void CWallet::LearnAssetMetadata(const CTransaction& tx)
{
    AssertLockHeld(cs_wallet);
    if (tx.mpa.empty()) return;
    const auto domain{AssetMetadataDomain(Params().GetConsensus())};
    if (!domain) return;
    uint256 asset;
    AssetMetadataProof proof;
    std::string error;
    if (DecodeAssetMetadataProof(tx, *domain, asset, proof, error)) {
        m_asset_genesis.try_emplace(asset, std::move(proof));
    }
}

WalletAssetMetadata CWallet::GetAssetMetadata(const uint256& asset) const
{
    AssertLockHeld(cs_wallet);
    if (auto configured{ConfiguredAssetMetadata(Params().GetConsensus(), asset)}) return *configured;
    if (const auto it{m_asset_metadata.find(asset)}; it != m_asset_metadata.end()) {
        return {it->second.name, it->second.ticker, it->second.proof.decimals, "local-registry", false};
    }
    if (const auto it{m_asset_genesis.find(asset)}; it != m_asset_genesis.end()) {
        return {{}, {}, it->second.decimals, "wallet-issuance", false};
    }
    return {};
}

bool CWallet::LoadAssetMetadata(const uint256& domain, const uint256& asset,
                                const LocalAssetMetadata& metadata, std::string& error)
{
    AssertLockHeld(cs_wallet);
    if (!ValidAssetMetadataLabels(metadata.name, metadata.ticker, error)) return false;
    if (!AssetMetadataProofMatches(domain, asset, metadata.proof)) {
        error = "Stored asset metadata proof does not match its chain and asset id";
        return false;
    }
    const auto current_domain{AssetMetadataDomain(Params().GetConsensus())};
    // A wallet opened on another chain must never reuse its labels or precision.
    if (!current_domain || domain != *current_domain) return true;
    if (ConfiguredAssetMetadata(Params().GetConsensus(), asset)) {
        error = "Configured asset metadata cannot be overridden";
        return false;
    }
    m_asset_metadata.insert_or_assign(asset, metadata);
    return true;
}

bool CWallet::SetAssetMetadata(const uint256& asset, const LocalAssetMetadata& metadata, std::string& error)
{
    AssertLockHeld(cs_wallet);
    const auto domain{AssetMetadataDomain(Params().GetConsensus())};
    if (!domain || !AssetMetadataProofMatches(*domain, asset, metadata.proof)) {
        error = "Issuance proof does not match this chain and asset id";
        return false;
    }
    if (ConfiguredAssetMetadata(Params().GetConsensus(), asset)) {
        error = "Configured asset metadata cannot be overridden";
        return false;
    }
    if (!ValidAssetMetadataLabels(metadata.name, metadata.ticker, error)) return false;
    WalletBatch batch{GetDatabase()};
    if (!batch.WriteAssetMetadata(*domain, asset, metadata)) {
        error = "Unable to persist asset metadata";
        return false;
    }
    m_asset_metadata.insert_or_assign(asset, metadata);
    return true;
}

bool CWallet::ClearAssetMetadata(const uint256& asset, std::string& error)
{
    AssertLockHeld(cs_wallet);
    const auto it{m_asset_metadata.find(asset)};
    if (it == m_asset_metadata.end()) return false;
    const auto domain{AssetMetadataDomain(Params().GetConsensus())};
    if (!domain || !WalletBatch{GetDatabase()}.EraseAssetMetadata(*domain, asset)) {
        error = "Unable to erase asset metadata";
        return false;
    }
    m_asset_metadata.erase(it);
    return true;
}

} // namespace wallet
