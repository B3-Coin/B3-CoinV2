// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <addresstype.h>
#include <consensus/amount.h>
#include <core_io.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <modern/stake.h>
#include <outputtype.h>
#include <pubkey.h>
#include <rpc/util.h>
#include <script/script.h>
#include <script/solver.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <wallet/coincontrol.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <univalue.h>

#include <array>
#include <optional>

namespace wallet {

namespace {

std::array<unsigned char, 32> XOnlyBytes(const CPubKey& pubkey)
{
    const XOnlyPubKey xonly{pubkey};
    std::array<unsigned char, 32> out{};
    std::copy(xonly.begin(), xonly.end(), out.begin());
    return out;
}

UniValue StatusToJSON(const interfaces::StakingStatus& status)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("available", status.available);
    obj.pushKV("running", status.running);
    obj.pushKV("state", status.state);
    if (!status.last_error.empty()) obj.pushKV("last_error", status.last_error);
    if (status.validator_key) obj.pushKV("validator_key", HexStr(*status.validator_key));
    obj.pushKV("blocks_produced", status.blocks_produced);
    if (!status.last_block_hash.IsNull()) obj.pushKV("last_block_hash", status.last_block_hash.GetHex());
    if (status.next_block_time > 0) obj.pushKV("next_block_time", status.next_block_time);
    return obj;
}

} // namespace

RPCHelpMan createstake()
{
    return RPCHelpMan{
        "createstake",
        "Lock an amount of B3 as a STAKE output for this wallet's validator key (Modern PoS V1).\n"
        "The validator key is created on first use and held by the wallet. The output becomes\n"
        "PENDING when confirmed and ACTIVE once buried STAKE_ACTIVATION_DEPTH blocks deep; it is\n"
        "never auto-selected for ordinary spends (unstaking is an explicit spend of the outpoint).\n"
        "Requires the modern era (after the legacy boundary H) and an unlocked wallet.\n",
        {
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "The amount to stake in " + CURRENCY_UNIT + " (at least the network's minimum stake)"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "txid", "the stake transaction id"},
                {RPCResult::Type::NUM, "vout", "the index of the STAKE output"},
                {RPCResult::Type::STR_AMOUNT, "amount", "the staked principal"},
                {RPCResult::Type::STR_HEX, "validator_key", "this wallet's x-only validator key"},
                {RPCResult::Type::STR, "owner_address", "the wallet address that owns (and can unstake) the principal"},
                {RPCResult::Type::STR, "status", "UNCONFIRMED until mined; then PENDING until active"},
                {RPCResult::Type::NUM, "activation_depth", "blocks after inclusion until the stake is ACTIVE"},
            }},
        RPCExamples{HelpExampleCli("createstake", "1000") + HelpExampleRpc("createstake", "1000")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;
            pwallet->BlockUntilSyncedToCurrentChain();

            LOCK(pwallet->cs_wallet);
            EnsureWalletIsUnlocked(*pwallet);

            const CAmount amount{AmountFromValue(request.params[0])};
            const interfaces::StakingStatus status{pwallet->chain().stakingStatus(std::nullopt)};
            if (!status.min_stake_amount) {
                throw JSONRPCError(RPC_MISC_ERROR, "The minimum stake is not configured on this network; STAKE outputs cannot be created");
            }
            if (amount < *status.min_stake_amount) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Amount below the minimum stake of %s %s", FormatMoney(*status.min_stake_amount), CURRENCY_UNIT));
            }
            if (status.next_block_phase == "legacy") {
                throw JSONRPCError(RPC_MISC_ERROR, "STAKE outputs exist only in the modern era (after the legacy boundary H)");
            }

            const auto validator{pwallet->GetOrCreateValidatorKey()};
            if (!validator) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(validator).original);
            const auto owner{pwallet->GetNewDestination(OutputType::LEGACY, "b3-stake-owner")};
            if (!owner) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(owner).original);
            if (!std::holds_alternative<PKHash>(*owner)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "The wallet cannot produce a legacy (P2PKH) owner address for the stake");
            }
            const CScript stake_script{modern::MakeStakeScript(XOnlyBytes(*validator), GetScriptForDestination(*owner))};

            std::vector<CRecipient> recipients{{CNoDestination{stake_script}, amount, /*fSubtractFeeFromAmount=*/false}};
            CCoinControl coin_control;
            auto res{CreateTransaction(*pwallet, recipients, /*change_pos=*/std::nullopt, coin_control, /*sign=*/true)};
            if (!res) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, util::ErrorString(res).original);
            const CTransactionRef& tx{res->tx};
            pwallet->CommitTransaction(tx, {{"b3", "stake"}}, /*orderForm=*/{});

            int vout{-1};
            for (size_t i{0}; i < tx->vout.size(); ++i) {
                if (tx->vout[i].scriptPubKey == stake_script) vout = static_cast<int>(i);
            }
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("txid", tx->GetHash().GetHex());
            obj.pushKV("vout", vout);
            obj.pushKV("amount", ValueFromAmount(amount));
            obj.pushKV("validator_key", HexStr(XOnlyBytes(*validator)));
            obj.pushKV("owner_address", EncodeDestination(*owner));
            obj.pushKV("status", "UNCONFIRMED");
            obj.pushKV("activation_depth", modern::STAKE_ACTIVATION_DEPTH);
            return obj;
        },
    };
}

RPCHelpMan getstakinginfo()
{
    return RPCHelpMan{
        "getstakinginfo",
        "Staking status: this wallet's validator key, the node's staking loop, and every STAKE\n"
        "output owned by this wallet with its PENDING/ACTIVE state.\n",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "validator_key", /*optional=*/true, "this wallet's x-only validator key (absent until created)"},
                {RPCResult::Type::NUM, "tip_height", "the active chain height"},
                {RPCResult::Type::STR, "next_block_phase", "legacy | corridor | modern_pos (the rules governing the next block)"},
                {RPCResult::Type::BOOL, "modern_pos_active", "whether the next block is a modern-PoS block with rules configured"},
                {RPCResult::Type::STR_AMOUNT, "min_stake_amount", /*optional=*/true, "the network's minimum stake"},
                {RPCResult::Type::NUM, "activation_depth", "blocks after inclusion until a stake is ACTIVE"},
                {RPCResult::Type::NUM, "active_weight", "this validator's block-production weight at the next height, in whole modern B3 (bound + ACTIVE stake; the finality weight)"},
                {RPCResult::Type::NUM, "total_active_weight", "the validator set's total weight at the next height, in whole modern B3"},
                {RPCResult::Type::OBJ, "staking", "the node's staking loop",
                 {
                     {RPCResult::Type::BOOL, "available", "a staking loop exists in this node"},
                     {RPCResult::Type::BOOL, "running", "whether the loop is running"},
                     {RPCResult::Type::STR, "state", "human-readable loop state"},
                     {RPCResult::Type::STR, "last_error", /*optional=*/true, "the last error the loop hit"},
                     {RPCResult::Type::STR_HEX, "validator_key", /*optional=*/true, "the key the loop stakes with"},
                     {RPCResult::Type::NUM, "blocks_produced", "blocks produced since the loop started"},
                     {RPCResult::Type::STR_HEX, "last_block_hash", /*optional=*/true, "the last block produced"},
                     {RPCResult::Type::NUM_TIME, "next_block_time", /*optional=*/true, "forced timestamp of the next block this validator may produce"},
                 }},
                {RPCResult::Type::ARR, "stakes", "this wallet's unspent STAKE outputs",
                 {
                     {RPCResult::Type::OBJ, "", "",
                      {
                          {RPCResult::Type::STR_HEX, "txid", ""},
                          {RPCResult::Type::NUM, "vout", ""},
                          {RPCResult::Type::STR_AMOUNT, "amount", ""},
                          {RPCResult::Type::STR_HEX, "validator_key", "the validator key the output stakes for"},
                          {RPCResult::Type::STR, "owner_address", /*optional=*/true, "the owner address"},
                          {RPCResult::Type::NUM, "confirmations", ""},
                          {RPCResult::Type::NUM, "created_height", /*optional=*/true, "inclusion height (confirmed outputs)"},
                          {RPCResult::Type::STR, "status", "UNCONFIRMED | PENDING | ACTIVE"},
                          {RPCResult::Type::NUM, "active_at_height", /*optional=*/true, "first height at which the stake counts"},
                      }},
                 }},
                {RPCResult::Type::STR_AMOUNT, "active", "sum of this wallet's ACTIVE stake"},
                {RPCResult::Type::STR_AMOUNT, "pending", "sum of this wallet's PENDING stake"},
                {RPCResult::Type::STR_AMOUNT, "unconfirmed", "sum of this wallet's unconfirmed stake"},
            }},
        RPCExamples{HelpExampleCli("getstakinginfo", "") + HelpExampleRpc("getstakinginfo", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;
            pwallet->BlockUntilSyncedToCurrentChain();

            LOCK(pwallet->cs_wallet);
            const std::optional<CPubKey> validator{pwallet->GetValidatorPubKey()};
            std::optional<std::array<unsigned char, 32>> validator_bytes;
            if (validator) validator_bytes = XOnlyBytes(*validator);
            const interfaces::StakingStatus status{pwallet->chain().stakingStatus(validator_bytes)};

            UniValue obj(UniValue::VOBJ);
            if (validator_bytes) obj.pushKV("validator_key", HexStr(*validator_bytes));
            obj.pushKV("tip_height", status.tip_height);
            obj.pushKV("next_block_phase", status.next_block_phase);
            obj.pushKV("modern_pos_active", status.modern_pos_active);
            if (status.min_stake_amount) obj.pushKV("min_stake_amount", ValueFromAmount(*status.min_stake_amount));
            obj.pushKV("activation_depth", status.stake_activation_depth);
            obj.pushKV("active_weight", status.active_weight);
            obj.pushKV("total_active_weight", status.total_active_weight);
            obj.pushKV("staking", StatusToJSON(status));

            UniValue stakes(UniValue::VARR);
            CAmount active{0}, pending{0}, unconfirmed{0};
            for (const auto& [txid, wtx] : pwallet->mapWallet) {
                for (size_t i{0}; i < wtx.tx->vout.size(); ++i) {
                    const CTxOut& out{wtx.tx->vout[i]};
                    if (!modern::ClaimsStakeMagic(out.scriptPubKey) || !pwallet->IsMine(out)) continue;
                    const COutPoint outpoint{txid, static_cast<uint32_t>(i)};
                    if (pwallet->IsSpent(outpoint)) continue;
                    std::string error;
                    const auto view{modern::ParseStakeOutput(out, error)};
                    if (!view) continue;
                    const int depth{pwallet->GetTxDepthInMainChain(wtx)};
                    UniValue entry(UniValue::VOBJ);
                    entry.pushKV("txid", txid.GetHex());
                    entry.pushKV("vout", static_cast<int>(i));
                    entry.pushKV("amount", ValueFromAmount(out.nValue));
                    entry.pushKV("validator_key", HexStr(view->validator_key));
                    CTxDestination owner_dest;
                    if (ExtractDestination(view->owner_script, owner_dest)) entry.pushKV("owner_address", EncodeDestination(owner_dest));
                    entry.pushKV("confirmations", depth);
                    if (depth <= 0) {
                        entry.pushKV("status", "UNCONFIRMED");
                        unconfirmed += out.nValue;
                    } else {
                        const int created_height{status.tip_height - depth + 1};
                        const int active_at{created_height + modern::STAKE_ACTIVATION_DEPTH};
                        const bool is_active{modern::IsStakeMature(created_height, status.tip_height + 1)};
                        entry.pushKV("created_height", created_height);
                        entry.pushKV("status", is_active ? "ACTIVE" : "PENDING");
                        entry.pushKV("active_at_height", active_at);
                        (is_active ? active : pending) += out.nValue;
                    }
                    stakes.push_back(std::move(entry));
                }
            }
            obj.pushKV("stakes", std::move(stakes));
            obj.pushKV("active", ValueFromAmount(active));
            obj.pushKV("pending", ValueFromAmount(pending));
            obj.pushKV("unconfirmed", ValueFromAmount(unconfirmed));
            return obj;
        },
    };
}

RPCHelpMan startstaking()
{
    return RPCHelpMan{
        "startstaking",
        "Start the node's automatic Modern PoS staking loop with this wallet's validator key\n"
        "(created on first use). Block fees are paid to a fresh address of this wallet. The loop\n"
        "keeps a copy of the validator key until stopstaking, so re-locking the wallet does not\n"
        "interrupt staking. Requires an unlocked wallet.\n",
        {},
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::BOOL, "running", ""},
                {RPCResult::Type::STR_HEX, "validator_key", "the x-only validator key now staking"},
                {RPCResult::Type::STR, "rewards_address", "where produced-block fees are paid"},
            }},
        RPCExamples{HelpExampleCli("startstaking", "") + HelpExampleRpc("startstaking", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;
            pwallet->BlockUntilSyncedToCurrentChain();

            LOCK(pwallet->cs_wallet);
            EnsureWalletIsUnlocked(*pwallet);
            const auto validator{pwallet->GetOrCreateValidatorKey()};
            if (!validator) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(validator).original);
            const auto secret{pwallet->GetValidatorSecret()};
            if (!secret) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(secret).original);
            const auto rewards{pwallet->GetNewDestination(pwallet->m_default_address_type, "b3-staking-rewards")};
            if (!rewards) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(rewards).original);

            std::string error;
            if (!pwallet->chain().startStaking(*secret, GetScriptForDestination(*rewards), error)) {
                throw JSONRPCError(RPC_MISC_ERROR, error);
            }
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("running", true);
            obj.pushKV("validator_key", HexStr(XOnlyBytes(*validator)));
            obj.pushKV("rewards_address", EncodeDestination(*rewards));
            return obj;
        },
    };
}

RPCHelpMan stopstaking()
{
    return RPCHelpMan{
        "stopstaking",
        "Stop the node's automatic staking loop (if running).\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "", {{RPCResult::Type::BOOL, "running", ""}}},
        RPCExamples{HelpExampleCli("stopstaking", "") + HelpExampleRpc("stopstaking", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;
            pwallet->chain().stopStaking();
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("running", false);
            return obj;
        },
    };
}

} // namespace wallet
