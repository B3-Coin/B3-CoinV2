// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chainparams.h>
#include <core_io.h>
#include <rpc/util.h>
#include <sync.h>
#include <univalue.h>
#include <wallet/asset_metadata.h>
#include <wallet/rpc/util.h>
#include <wallet/wallet.h>

namespace wallet {

RPCHelpMan setassetmetadata()
{
    return RPCHelpMan{
        "setassetmetadata",
        "Store a cosmetic name and ticker for a simple-v1 asset in this wallet.\n"
        "The raw issuance transaction must contain exactly one valid genesis record whose\n"
        "first input and immutable rules derive the requested asset id on this chain.\n"
        "This verifies precision, not current chain inclusion, signatures, backing or redemption.\n"
        "Labels are local to this wallet, are not published, and must be imported separately\n"
        "by recipients. Configured B3, FN, bUSD and tUSD identities cannot be overridden.\n"
        "No transaction is created or broadcast, and wallet unlocking is not required.\n",
        {
            {"asset_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Full chain-bound asset id"},
            {"name", RPCArg::Type::STR, RPCArg::Optional::NO, "Local display name: 1-64 safe ASCII characters"},
            {"ticker", RPCArg::Type::STR, RPCArg::Optional::NO, "Local ticker: 1-12 safe ASCII characters"},
            {"issuance_hex", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Raw issuance transaction, at most 1,000,000 bytes; not a transfer transaction"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Stored local metadata", {
            {RPCResult::Type::STR_HEX, "asset_id", "Asset id"},
            {RPCResult::Type::STR, "name", "Wallet-local display name"},
            {RPCResult::Type::STR, "ticker", "Wallet-local ticker"},
            {RPCResult::Type::NUM, "decimals", "Verified immutable precision"},
            {RPCResult::Type::BOOL, "precision_known", "Always true for an accepted proof"},
            {RPCResult::Type::STR, "metadata_source", "local-registry"},
            {RPCResult::Type::BOOL, "test_only", "Whether this is a configured test-only asset"},
        }},
        RPCExamples{HelpExampleCli("setassetmetadata", "\"asset_id\" \"Example Token\" \"EXM\" \"issuance_hex\"")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const auto wallet{GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            const uint256 asset{ParseHashV(request.params[0], "asset_id")};
            const auto domain{AssetMetadataDomain(Params().GetConsensus())};
            if (!domain) throw JSONRPCError(RPC_MISC_ERROR, "The modern chain domain is not pinned");
            if (ConfiguredAssetMetadata(Params().GetConsensus(), asset)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Configured asset metadata cannot be overridden");
            }
            LocalAssetMetadata metadata{request.params[1].get_str(), request.params[2].get_str(), {}};
            std::string error;
            if (!ValidAssetMetadataLabels(metadata.name, metadata.ticker, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }
            const std::string& hex{request.params[3].get_str()};
            if (hex.size() > 2'000'000) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Issuance proof exceeds 1,000,000 bytes");
            }
            CMutableTransaction tx;
            if (!DecodeHexTx(tx, hex)) throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Unable to decode issuance transaction");
            uint256 derived;
            if (!DecodeAssetMetadataProof(CTransaction{std::move(tx)}, *domain, derived, metadata.proof, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }
            if (derived != asset) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Issuance proof does not match this chain and asset id");
            }
            {
                LOCK(wallet->cs_wallet);
                if (!wallet->SetAssetMetadata(asset, metadata, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, error);
                }
            }
            wallet->NotifyStatusChanged(wallet.get());
            UniValue result{UniValue::VOBJ};
            result.pushKV("asset_id", asset.GetHex());
            result.pushKV("name", metadata.name);
            result.pushKV("ticker", metadata.ticker);
            result.pushKV("decimals", static_cast<int>(metadata.proof.decimals));
            result.pushKV("precision_known", true);
            result.pushKV("metadata_source", "local-registry");
            result.pushKV("test_only", false);
            return result;
        }};
}

RPCHelpMan clearassetmetadata()
{
    return RPCHelpMan{
        "clearassetmetadata",
        "Remove this wallet's local asset label and imported precision proof.\n"
        "Built-in metadata and precision learned from wallet issuance history remain available.\n"
        "This does not change or transfer assets and requires no wallet unlocking.\n",
        {{"asset_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Full asset id"}},
        RPCResult{RPCResult::Type::BOOL, "", "True when a local record was removed; false when none existed"},
        RPCExamples{HelpExampleCli("clearassetmetadata", "\"asset_id\"")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const auto wallet{GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            const uint256 asset{ParseHashV(request.params[0], "asset_id")};
            std::string error;
            bool removed;
            {
                LOCK(wallet->cs_wallet);
                removed = wallet->ClearAssetMetadata(asset, error);
            }
            if (!error.empty()) throw JSONRPCError(RPC_WALLET_ERROR, error);
            if (removed) wallet->NotifyStatusChanged(wallet.get());
            return removed;
        }};
}

} // namespace wallet
