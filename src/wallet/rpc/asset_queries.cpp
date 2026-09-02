// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/wallet.h>

#include <chainparams.h>
#include <key_io.h>
#include <modern/asset_output.h>
#include <modern/asset_validation.h>
#include <modern/policy.h>
#include <rpc/util.h>
#include <script/script.h>
#include <univalue.h>
#include <wallet/receive.h>
#include <wallet/rpc/util.h>
#include <wallet/scriptpubkeyman.h>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wallet {
namespace {

struct WalletAssetBucket {
    modern::AssetId asset_id;
    bool is_fn{false};
    CAmount confirmed{0};
    CAmount unconfirmed{0};
    CAmount spendable{0};
    std::vector<UniValue> utxos;
};

} // namespace

RPCHelpMan getwalletassets()
{
    return RPCHelpMan{
        "getwalletassets",
        "Return this wallet's unspent, trusted post-transition FN Coin and simple-v1 asset outputs.\n"
        "Asset amounts are exact integer base units; FN Coin has zero decimals.\n",
        {
            {"asset_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Only return this 32-byte asset id"},
            {"minconf", RPCArg::Type::NUM, RPCArg::Default{0}, "Minimum confirmations"},
            {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false}, "Include unconfirmed outputs the wallet does not consider safe"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::ARR, "assets", "Wallet asset balances and UTXOs", {
                {RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR_HEX, "asset_id", "Asset identifier"},
                    {RPCResult::Type::STR, "kind", "fn or colored"},
                    {RPCResult::Type::NUM, "confirmed", "Confirmed unspent amount"},
                    {RPCResult::Type::NUM, "unconfirmed", "Unconfirmed unspent amount"},
                    {RPCResult::Type::NUM, "spendable", "Amount currently mature, safe, unlocked, and signable by this wallet"},
                    {RPCResult::Type::ARR, "utxos", "Matching unspent outputs", {
                        {RPCResult::Type::OBJ, "", "", {
                            {RPCResult::Type::STR_HEX, "txid", "Transaction id"},
                            {RPCResult::Type::NUM, "vout", "Output index"},
                            {RPCResult::Type::NUM, "amount", "Exact integer asset amount"},
                            {RPCResult::Type::STR, "policy", "fn or owner"},
                            {RPCResult::Type::NUM, "policy_version", "Asset policy version"},
                            {RPCResult::Type::STR, "owner_address", /*optional=*/true, "Owner address when the owner script has a standard destination"},
                            {RPCResult::Type::STR_HEX, "owner_script", "Exact owner authorization script"},
                            {RPCResult::Type::NUM, "confirmations", "Depth in the active chain"},
                            {RPCResult::Type::BOOL, "coinbase", "Whether the creating transaction is coinbase"},
                            {RPCResult::Type::BOOL, "mature", "Whether coinbase/coinstake maturity has elapsed"},
                            {RPCResult::Type::BOOL, "locked", "Whether the wallet has locked this outpoint"},
                            {RPCResult::Type::BOOL, "spendable", "Whether this wallet can currently select and sign the output"},
                            {RPCResult::Type::BOOL, "safe", "Whether the wallet considers the creating transaction safe"},
                        }},
                    }},
                }},
            }},
        }},
        RPCExamples{
            HelpExampleCli("getwalletassets", "") +
            HelpExampleCli("getwalletassets", "\"asset_id\" 1") +
            HelpExampleRpc("getwalletassets", "\"asset_id\", 0, false")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<const CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;

            std::optional<modern::AssetId> filter;
            if (!request.params[0].isNull()) {
                filter = ParseHashV(request.params[0], "asset_id");
                if (*filter == modern::NativeAsset()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "asset_id must identify a non-native asset");
                }
            }
            const int min_depth{
                request.params[1].isNull() ? 0 : request.params[1].getInt<int>()};
            if (min_depth < 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "minconf must be non-negative");
            }
            const bool include_unsafe{
                !request.params[2].isNull() && request.params[2].get_bool()};

            wallet->BlockUntilSyncedToCurrentChain();
            LOCK(wallet->cs_wallet);

            const Consensus::Params& consensus{Params().GetConsensus()};
            const std::optional<modern::AssetId> fn_asset{
                modern::ConfiguredFnAssetId(consensus)};
            std::map<modern::AssetId, WalletAssetBucket> buckets;
            std::set<Txid> trusted_parents;

            for (const auto& [outpoint, txo] : wallet->GetTXOs()) {
                const CWalletTx& wtx{txo.GetWalletTx()};
                const CTxOut& output{txo.GetTxOut()};
                if (wallet->IsSpent(outpoint)) continue;

                const int depth{wallet->GetTxDepthInMainChain(wtx)};
                if (depth < min_depth || depth < 0 ||
                    (depth == 0 && !wtx.InMempool())) {
                    continue;
                }

                bool safe{CachedTxIsTrusted(*wallet, wtx, trusted_parents)};
                if (depth == 0 &&
                    (wtx.mapValue.contains("replaces_txid") ||
                     wtx.mapValue.contains("replaced_by_txid"))) {
                    safe = false;
                }
                if (!include_unsafe && !safe) continue;

                if (AssetSigningContextForWalletTransaction(wtx) !=
                    AssetSigningContext::OWNER_SUFFIX) {
                    continue;
                }
                const std::optional<modern::ModernOutput> parsed{
                    modern::ParseAssetOutput(output)};
                const std::optional<CScript> owner_script{
                    modern::AssetOwnerScript(output)};
                if (!parsed || !owner_script) continue;
                if (filter && parsed->asset != *filter) continue;

                const bool is_fn{fn_asset && parsed->asset == *fn_asset};
                WalletAssetBucket& bucket{buckets.try_emplace(
                    parsed->asset,
                    WalletAssetBucket{parsed->asset, is_fn, 0, 0, 0, {}})
                                                .first->second};

                const bool mature{!wallet->IsTxImmatureCoinBase(wtx)};
                const bool locked{wallet->IsLockedCoin(outpoint)};
                const bool currently_spendable{
                    WalletCanSpendScriptNow(*wallet, *owner_script)};
                const bool spendable{
                    safe && mature && !locked && currently_spendable};

                if (depth > 0) {
                    bucket.confirmed += parsed->amount;
                } else {
                    bucket.unconfirmed += parsed->amount;
                }
                if (spendable) bucket.spendable += parsed->amount;

                UniValue utxo{UniValue::VOBJ};
                utxo.pushKV("txid", outpoint.hash.GetHex());
                utxo.pushKV("vout", outpoint.n);
                utxo.pushKV("amount", parsed->amount);
                utxo.pushKV("policy", is_fn ? "fn" : "owner");
                utxo.pushKV("policy_version", parsed->policy_version);
                CTxDestination owner;
                if (ExtractDestination(*owner_script, owner)) {
                    utxo.pushKV("owner_address", EncodeDestination(owner));
                }
                utxo.pushKV("owner_script", HexStr(*owner_script));
                utxo.pushKV("confirmations", depth);
                utxo.pushKV("coinbase", wtx.IsCoinBase());
                utxo.pushKV("mature", mature);
                utxo.pushKV("locked", locked);
                utxo.pushKV("spendable", spendable);
                utxo.pushKV("safe", safe);
                bucket.utxos.push_back(std::move(utxo));
            }

            UniValue assets{UniValue::VARR};
            for (auto& [asset_id, bucket] : buckets) {
                UniValue entry{UniValue::VOBJ};
                entry.pushKV("asset_id", asset_id.GetHex());
                entry.pushKV("kind", bucket.is_fn ? "fn" : "colored");
                entry.pushKV("confirmed", bucket.confirmed);
                entry.pushKV("unconfirmed", bucket.unconfirmed);
                entry.pushKV("spendable", bucket.spendable);
                UniValue utxos{UniValue::VARR};
                for (UniValue& utxo : bucket.utxos) {
                    utxos.push_back(std::move(utxo));
                }
                entry.pushKV("utxos", std::move(utxos));
                assets.push_back(std::move(entry));
            }

            UniValue result{UniValue::VOBJ};
            result.pushKV("assets", std::move(assets));
            return result;
        },
    };
}

} // namespace wallet
