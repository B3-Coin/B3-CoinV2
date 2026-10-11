// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <addresstype.h>
#include <chainparams.h>
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
#include <util/chaintype.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <wallet/coincontrol.h>
#include <wallet/rpc/finality_fresh_key.h>
#include <wallet/rpc/finality_recovery.h>
#include <wallet/rpc/finality_recovery_status.h>
#include <wallet/rpc/util.h>
#include <crypto/bls.h>
#include <random.h>
#include <modern/finality_key.h>
#include <modern/finality_types.h>
#include <modern/metadata_cell.h>
#include <modern/mpa.h>
#include <modern/policy.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <univalue.h>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <span>

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
    obj.pushKV("finality_signing", status.finality_signing);
    obj.pushKV("last_signed_height", status.last_signed_height);
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
        "included in weight at Set0 bootstrap or through a later certified set rotation. It is\n"
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

RPCHelpMan releasestake()
{
    return RPCHelpMan{
        "releasestake",
        "Release STAKE outputs back into ordinary spendable B3 (unstake).\n"
        "Spends the given STAKE outputs of this wallet, or all of its confirmed ones when none are\n"
        "given, to one address of this wallet, paying the fee from the released amount. Nothing is\n"
        "restaked. Weight already frozen in an epoch snapshot can stay in force until that snapshot\n"
        "is replaced. Unconfirmed stakes cannot be released yet. Requires an unlocked wallet.\n",
        {
            {"outpoints", RPCArg::Type::ARR, RPCArg::DefaultHint{"every confirmed STAKE output of this wallet"}, "The STAKE outputs to release",
                {
                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        {
                            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The stake transaction id"},
                            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO, "The STAKE output index"},
                        },
                    },
                },
            },
            {"address", RPCArg::Type::STR, RPCArg::DefaultHint{"a new legacy address of this wallet"}, "The wallet address that receives the released B3"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "txid", "the release transaction id"},
                {RPCResult::Type::NUM, "released_outputs", "the number of STAKE outputs spent"},
                {RPCResult::Type::STR_AMOUNT, "principal", "the total staked principal released"},
                {RPCResult::Type::STR_AMOUNT, "fee", "the fee paid from the principal"},
                {RPCResult::Type::STR_AMOUNT, "amount", "the ordinary B3 received"},
                {RPCResult::Type::STR, "address", "the wallet address that received it"},
            }},
        RPCExamples{HelpExampleCli("releasestake", "") +
                    HelpExampleCli("releasestake", "'[{\"txid\":\"mytxid\",\"vout\":0}]'") +
                    HelpExampleRpc("releasestake", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;
            pwallet->BlockUntilSyncedToCurrentChain();

            LOCK(pwallet->cs_wallet);
            EnsureWalletIsUnlocked(*pwallet);

            // The wallet's unspent, well-formed STAKE outputs and their depth.
            std::map<COutPoint, std::pair<CAmount, int>> stakes;
            for (const auto& [txid, wtx] : pwallet->mapWallet) {
                for (size_t i{0}; i < wtx.tx->vout.size(); ++i) {
                    const CTxOut& out{wtx.tx->vout[i]};
                    if (!modern::ClaimsStakeMagic(out.scriptPubKey) || !pwallet->IsMine(out)) continue;
                    const COutPoint outpoint{txid, static_cast<uint32_t>(i)};
                    if (pwallet->IsSpent(outpoint)) continue;
                    std::string error;
                    if (!modern::ParseStakeOutput(out, error)) continue;
                    stakes.emplace(outpoint, std::make_pair(out.nValue, pwallet->GetTxDepthInMainChain(wtx)));
                }
            }

            std::vector<COutPoint> selected;
            if (request.params[0].isNull() || request.params[0].empty()) {
                for (const auto& [outpoint, value] : stakes) {
                    if (value.second > 0 && !pwallet->IsLockedCoin(outpoint)) selected.push_back(outpoint);
                }
                if (selected.empty()) throw JSONRPCError(RPC_WALLET_ERROR, "This wallet has no confirmed STAKE outputs to release");
            } else {
                for (const UniValue& item : request.params[0].getValues()) {
                    const COutPoint outpoint{Txid::FromUint256(ParseHashO(item, "txid")), item.find_value("vout").getInt<uint32_t>()};
                    const auto found{stakes.find(outpoint)};
                    if (found == stakes.end()) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s is not an unspent STAKE output of this wallet", outpoint.ToString()));
                    }
                    if (found->second.second <= 0) {
                        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("STAKE output %s is not confirmed yet", outpoint.ToString()));
                    }
                    if (pwallet->IsLockedCoin(outpoint)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, strprintf("STAKE output %s is locked; unlock it first (lockunspent)", outpoint.ToString()));
                    }
                    if (std::find(selected.begin(), selected.end(), outpoint) != selected.end()) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("STAKE output %s is listed twice", outpoint.ToString()));
                    }
                    selected.push_back(outpoint);
                }
            }

            CTxDestination destination;
            if (!request.params[1].isNull()) {
                destination = DecodeDestination(request.params[1].get_str());
                if (!IsValidDestination(destination)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
                // Releasing returns principal to this wallet; sending it elsewhere is an ordinary send.
                if (!pwallet->IsMine(destination)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "The address does not belong to this wallet");
                }
            } else {
                const auto fresh{pwallet->GetNewDestination(OutputType::LEGACY, "b3-stake-release")};
                if (!fresh) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(fresh).original);
                destination = *fresh;
            }

            CAmount principal{0};
            CCoinControl coin_control;
            coin_control.m_include_stake = true;
            coin_control.m_allow_other_inputs = false;
            for (const COutPoint& outpoint : selected) {
                principal += stakes.at(outpoint).first;
                coin_control.Select(outpoint);
            }
            std::vector<CRecipient> recipients{{destination, principal, /*fSubtractFeeFromAmount=*/true}};
            auto res{CreateTransaction(*pwallet, recipients, /*change_pos=*/std::nullopt, coin_control, /*sign=*/true)};
            if (!res) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(res).original);
            const CTransactionRef& tx{res->tx};
            pwallet->CommitTransaction(tx, {{"b3", "unstake"}}, /*orderForm=*/{});

            UniValue obj(UniValue::VOBJ);
            obj.pushKV("txid", tx->GetHash().GetHex());
            obj.pushKV("released_outputs", static_cast<int>(selected.size()));
            obj.pushKV("principal", ValueFromAmount(principal));
            obj.pushKV("fee", ValueFromAmount(res->fee));
            obj.pushKV("amount", ValueFromAmount(principal - res->fee));
            obj.pushKV("address", EncodeDestination(destination));
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
                {RPCResult::Type::NUM, "active_weight", "this validator's epoch-frozen block-production/finality weight at the next height, in whole modern B3; once a set is in force, newly ACTIVE stake enters only through a later certified rotation"},
                {RPCResult::Type::NUM, "total_active_weight", "the epoch-frozen validator set's total weight at the next height, in whole modern B3"},
                {RPCResult::Type::OBJ, "staking", "the node's staking loop",
                 {
                     {RPCResult::Type::BOOL, "available", "a staking loop exists in this node"},
                     {RPCResult::Type::BOOL, "running", "whether the loop is running"},
                     {RPCResult::Type::STR, "state", "human-readable loop state"},
                     {RPCResult::Type::STR, "last_error", /*optional=*/true, "the last error the loop hit"},
                     {RPCResult::Type::STR_HEX, "validator_key", /*optional=*/true, "the key the loop stakes with"},
                     {RPCResult::Type::BOOL, "finality_signing", "whether the node-global loop currently holds a BLS signing key"},
                     {RPCResult::Type::NUM, "last_signed_height", "the latest height signed by the node-global finality signer, or -1"},
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


//! Build, fund, sign and commit a FINALITY_KEY binding transaction (bind,
//! rotate or revoke). The MPA evidence lives outside the transaction's signed
//! identity (txid excludes it), but it must be present during funding so the
//! wallet prices its verification-cost vsize. Its own BIP340 + PoP
//! authenticate it.
static UniValue SubmitFinalityKeyTx(CWallet& wallet, const CKey& identity, const std::array<unsigned char, 32>& vk,
                                    const bls::SecretKey* bls_key, const uint32_t seq, const uint256& domain,
                                    const std::string& action) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    modern::FinalityKeyParams params;
    params.bls_pubkey = bls_key ? bls_key->GetPublicKey().Compressed() : modern::BlsPubkeyBytes{};
    params.seq = seq;
    uint256 commitment;
    std::copy(vk.begin(), vk.end(), commitment.begin());
    const auto cell{modern::MakeMetadataCellScript(static_cast<uint16_t>(modern::PolicyType::FINALITY_KEY),
                                                   modern::POLICY_VERSION_V1, commitment, params.Encode())};
    if (!cell) throw JSONRPCError(RPC_INTERNAL_ERROR, "cell construction failed");
    modern::FinalityKeyEvidence ev;
    ev.validator_key = vk;
    ev.bls_pubkey = params.bls_pubkey;
    ev.seq = seq;
    const uint256 digest{modern::FinalityBindDigest(domain, ev.validator_key, ev.bls_pubkey, seq)};
    uint256 aux{GetRandHash()};
    if (!identity.SignSchnorr(digest, ev.bip340_sig, nullptr, aux)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "BIP340 identity authorization failed");
    }
    if (bls_key) ev.pop = bls_key->SignPoP().Compressed();

    CMpaRecord record;
    record.payload_type = modern::MPA_TYPE_FINALITY_KEY_EVIDENCE;
    record.payload_version = modern::MPA_VERSION_V1;
    const auto enc{ev.Encode()};
    record.payload.assign(enc.begin(), enc.end());

    std::vector<CRecipient> recipients{{CNoDestination{*cell}, 0, /*fSubtractFeeFromAmount=*/false}};
    CCoinControl coin_control;
    const ModernTransactionOptions modern_options{
        .mpa = {record},
        .native_disintegration = 0};
    auto res{CreateTransaction(wallet, recipients, /*change_pos=*/std::nullopt,
                               coin_control, /*sign=*/true, modern_options)};
    if (!res) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, util::ErrorString(res).original);
    const CTransactionRef tx{res->tx};
    wallet.CommitTransaction(tx, {{"b3", "finality-key-" + action}}, /*orderForm=*/{});

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("txid", tx->GetHash().GetHex());
    obj.pushKV("ptxid", tx->GetPtxid().GetHex());
    obj.pushKV("action", action);
    obj.pushKV("validator_key", HexStr(vk));
    obj.pushKV("seq", static_cast<uint64_t>(seq));
    obj.pushKV("bls_pubkey", HexStr(params.bls_pubkey));
    obj.pushKV("status", "UNCONFIRMED");
    return obj;
}

struct PendingFinalityKeyTx {
    Txid txid;
    uint32_t seq{0};
};

//! Find this validator's wallet-owned transition which has not confirmed (or
//! been abandoned/conflicted) yet. Wallet transactions retain their MPA across
//! restart, so this also closes the restart-before-confirmation duplicate-bind
//! case which chain-only status cannot see.
static std::optional<PendingFinalityKeyTx> FindPendingFinalityKeyTx(
    const CWallet& wallet,
    const std::array<unsigned char, 32>& validator_key) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    for (const auto& [txid, wtx] : wallet.mapWallet) {
        if (!wtx.isUnconfirmed() || !wtx.tx->HasMpa()) continue;
        std::vector<modern::FinalityKeyPair> pairs;
        std::string error;
        if (!modern::MatchFinalityKeyPairs(*wtx.tx, pairs, error)) continue;
        for (const auto& pair : pairs) {
            if (pair.evidence.validator_key == validator_key) {
                return PendingFinalityKeyTx{txid, pair.params.seq};
            }
        }
    }
    return std::nullopt;
}

static void RejectPendingFinalityKeyTx(const CWallet& wallet,
                                       const std::array<unsigned char, 32>& validator_key)
    EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    const auto pending{FindPendingFinalityKeyTx(wallet, validator_key)};
    if (!pending) return;
    throw JSONRPCError(
        RPC_WALLET_ERROR,
        strprintf("A FINALITY_KEY transaction for this validator is already unconfirmed "
                  "(txid %s, sequence %u); wait for confirmation, rebroadcast it, or "
                  "abandon it before creating another",
                  pending->txid.ToString(), pending->seq));
}

//! Common preamble: unlocked wallet, validator key, chain finality status.
struct FinalityRpcContext {
    CPubKey validator_pubkey;
    std::array<unsigned char, 32> vk{};
    interfaces::FinalityStatus status;
};
static FinalityRpcContext FinalityContext(CWallet& wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    FinalityRpcContext ctx;
    const auto validator{wallet.GetOrCreateValidatorKey()};
    if (!validator) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(validator).original);
    ctx.validator_pubkey = *validator;
    ctx.vk = XOnlyBytes(*validator);
    ctx.status = wallet.chain().finalityStatus(ctx.vk);
    if (!ctx.status.configured) {
        throw JSONRPCError(RPC_MISC_ERROR, "Modern-PoS finality is not configured on this network (no pinned boundary / rules)");
    }
    if (ctx.status.chain_domain.IsNull()) {
        throw JSONRPCError(RPC_MISC_ERROR, "The modern chain domain is not derivable (boundary unpinned)");
    }
    return ctx;
}

RPCHelpMan importfinalitykey()
{
    return RPCHelpMan{
        "importfinalitykey",
        "Import an INDEPENDENTLY generated BLS finality consensus secret key (32-byte big-endian\n"
        "scalar, 0 < sk < r) into this wallet. The key is stored under the wallet's ordinary\n"
        "descriptor storage and encryption -- no separate keystore -- and takes precedence over the\n"
        "deterministic derivation: the next bindfinalitykey binds it, and startstaking arms the signer\n"
        "with it when it matches the on-chain binding. Requires an unlocked wallet. The secret is\n"
        "never returned by any RPC.\n",
        {
            {"blssecret", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "the 32-byte BLS secret scalar (hex)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
                  {
                      {RPCResult::Type::STR_HEX, "bls_pubkey", "the imported key's BLS public key"},
                      {RPCResult::Type::BOOL, "imported", ""},
                  }},
        RPCExamples{HelpExampleCli("importfinalitykey", "\"<hex>\"") + HelpExampleRpc("importfinalitykey", "\"<hex>\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;
            LOCK(pwallet->cs_wallet);
            EnsureWalletIsUnlocked(*pwallet);
            const auto bytes{ParseHexV(request.params[0], "blssecret")};
            if (bytes.size() != 32) throw JSONRPCError(RPC_INVALID_PARAMETER, "blssecret must be exactly 32 bytes of hex");
            const auto key{bls::SecretKey::FromBytes(bytes)};
            if (!key) throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid BLS secret: must satisfy 0 < sk < r (32 big-endian bytes)");
            const auto imported{pwallet->ImportFinalityBlsKey(*key)};
            if (!imported) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(imported).original);
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("bls_pubkey", HexStr(imported->Compressed()));
            obj.pushKV("imported", true);
            return obj;
        },
    };
}

RPCHelpMan bindfinalitykey()
{
    return RPCHelpMan{
        "bindfinalitykey",
        "Bind (or rotate to) this wallet's BLS finality consensus key (Modern PoS V1, FINALITY_KEY policy).\n"
        "By default the BLS key is derived deterministically from the wallet's validator identity key and the\n"
        "binding sequence, so a restored wallet re-derives it. An independently imported finality key takes\n"
        "precedence for the next bind or rotation. The transaction carries the\n"
        "FINALITY_KEY cell plus the BIP340 identity authorization and BLS proof-of-possession as Modern\n"
        "Payload Area evidence. The first binding uses sequence 0; an existing binding is rotated to the next\n"
        "sequence with a fresh BLS key. Takes effect at the next epoch snapshot boundary. Requires an\n"
        "unlocked wallet. Private BLS material is never returned.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
                  {
                      {RPCResult::Type::STR_HEX, "txid", "the binding transaction"},
                      {RPCResult::Type::STR_HEX, "ptxid", "its full-evidence identifier"},
                      {RPCResult::Type::STR, "action", "bind | rotate"},
                      {RPCResult::Type::STR_HEX, "validator_key", "this wallet's x-only validator key"},
                      {RPCResult::Type::NUM, "seq", "the new binding sequence"},
                      {RPCResult::Type::STR_HEX, "bls_pubkey", "the BLS public key now being bound"},
                      {RPCResult::Type::STR, "status", "UNCONFIRMED"},
                  }},
        RPCExamples{HelpExampleCli("bindfinalitykey", "") + HelpExampleRpc("bindfinalitykey", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;
            pwallet->BlockUntilSyncedToCurrentChain();
            LOCK(pwallet->cs_wallet);
            EnsureWalletIsUnlocked(*pwallet);
            const FinalityRpcContext ctx{FinalityContext(*pwallet)};
            RejectPendingFinalityKeyTx(*pwallet, ctx.vk);
            if (ctx.status.bound && ctx.status.binding_seq == std::numeric_limits<uint32_t>::max()) {
                throw JSONRPCError(RPC_MISC_ERROR, "The binding sequence is exhausted");
            }
            const uint32_t seq{ctx.status.bound ? ctx.status.binding_seq + 1 : 0};
            const auto identity{pwallet->GetValidatorSecret()};
            if (!identity) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(identity).original);
            // One resolution rule (wallet): an imported independent key takes
            // precedence over the deterministic derivation for a fresh bind.
            const auto bls_key{pwallet->ResolveFinalityBlsKey(seq, /*bound_bls_pubkey=*/nullptr)};
            if (!bls_key) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(bls_key).original);
            const std::string action{ctx.status.bound && !ctx.status.revoked ? "rotate" : "bind"};
            if (action == "rotate") {
                const auto pk{bls_key->GetPublicKey().Compressed()};
                if (ctx.status.binding_bls_pubkey == std::vector<unsigned char>(pk.begin(), pk.end())) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       "the resolved BLS key is already bound; import a different key "
                                       "(importfinalitykey) before rotating");
                }
            }
            return SubmitFinalityKeyTx(*pwallet, *identity, ctx.vk, &*bls_key, seq, ctx.status.chain_domain, action);
        },
    };
}

RPCHelpMan revokefinalitykey()
{
    return RPCHelpMan{
        "revokefinalitykey",
        "Revoke this wallet's bound BLS finality key (zero-key FINALITY_KEY transition at the next\n"
        "sequence). The validator leaves the validator set at the next epoch snapshot boundary and is no\n"
        "longer block-eligible from then on. Requires an unlocked wallet and an existing binding.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
                  {
                      {RPCResult::Type::STR_HEX, "txid", "the revocation transaction"},
                      {RPCResult::Type::STR_HEX, "ptxid", "its full-evidence identifier"},
                      {RPCResult::Type::STR, "action", "revoke"},
                      {RPCResult::Type::STR_HEX, "validator_key", "this wallet's x-only validator key"},
                      {RPCResult::Type::NUM, "seq", "the revocation sequence"},
                      {RPCResult::Type::STR_HEX, "bls_pubkey", "all zero (revocation)"},
                      {RPCResult::Type::STR, "status", "UNCONFIRMED"},
                  }},
        RPCExamples{HelpExampleCli("revokefinalitykey", "") + HelpExampleRpc("revokefinalitykey", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;
            pwallet->BlockUntilSyncedToCurrentChain();
            LOCK(pwallet->cs_wallet);
            EnsureWalletIsUnlocked(*pwallet);
            const FinalityRpcContext ctx{FinalityContext(*pwallet)};
            RejectPendingFinalityKeyTx(*pwallet, ctx.vk);
            if (!ctx.status.bound || ctx.status.revoked) {
                throw JSONRPCError(RPC_MISC_ERROR, "This validator has no active FINALITY_KEY binding to revoke");
            }
            if (ctx.status.binding_seq == std::numeric_limits<uint32_t>::max()) {
                throw JSONRPCError(RPC_MISC_ERROR, "The binding sequence is exhausted");
            }
            const auto identity{pwallet->GetValidatorSecret()};
            if (!identity) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(identity).original);
            return SubmitFinalityKeyTx(*pwallet, *identity, ctx.vk, /*bls_key=*/nullptr, ctx.status.binding_seq + 1,
                                       ctx.status.chain_domain, "revoke");
        },
    };
}

// ---------------------------------------------------------------------------
// B-37 per-epoch fresh-lineage keys (bridge V2 stage S3, regtest-only knob
// -b3freshkeyheight; b3-bridge-v2-finality-dependency.md section 6.4). The
// wallet signs bindings with the validator identity; the BLS secrets live in
// the node's fresh key store (wallet/rpc/finality_fresh_key.h).

namespace {

//! The chain's fresh key status for this wallet's validator (and the first
//! claims of `probe_keys`); refuses unless fresh-key mode is configured and
//! derivable at the tip.
interfaces::FreshFinalityKeyStatus RequireFreshKeyStatus(CWallet& wallet, const std::array<unsigned char, 32>& vk,
                                                         const std::vector<std::array<unsigned char, 48>>& probe_keys = {})
{
    interfaces::FreshFinalityKeyStatus status{wallet.chain().freshFinalityKeyStatus(vk, probe_keys)};
    if (!status.configured) {
        throw JSONRPCError(RPC_MISC_ERROR, "Fresh-lineage per-epoch keys (B-37) are not configured on this chain "
                                           "(regtest-only -b3freshkeyheight)");
    }
    if (status.chain_domain.IsNull()) {
        throw JSONRPCError(RPC_MISC_ERROR, "The modern chain domain is not derivable (boundary unpinned)");
    }
    if (!status.available) {
        throw JSONRPCError(RPC_MISC_ERROR, "The fresh key state is unavailable at the tip (finality state not derivable)");
    }
    return status;
}

//! The probed claim of `pk` in `status` (FreshFinalityKeyStatus::probe_claims).
const interfaces::FreshFinalityKeyClaim* ProbedClaim(const interfaces::FreshFinalityKeyStatus& status,
                                                     const modern::BlsPubkeyBytes& pk)
{
    for (const interfaces::FreshFinalityKeyClaim& claim : status.probe_claims) {
        if (claim.bls_pubkey == pk) return &claim;
    }
    return nullptr;
}

//! The height at which `pk` was claimed first by a binding other than this
//! validator's own fresh binding (so this validator's binding is inert).
std::optional<int> ClaimedElsewhereAt(const interfaces::FreshFinalityKeyStatus& status,
                                      const modern::BlsPubkeyBytes& pk)
{
    const interfaces::FreshFinalityKeyClaim* claim{ProbedClaim(status, pk)};
    if (!claim || !claim->first_height || claim->own_epoch) return std::nullopt;
    return claim->first_height;
}

//! "epoch:<n>" -> n.
std::optional<uint64_t> ParseStoredKeyEpoch(const std::string& reuse_key)
{
    constexpr std::string_view prefix{"epoch:"};
    if (reuse_key.size() <= prefix.size() || reuse_key.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    return ToIntegral<uint64_t>(std::string_view{reuse_key}.substr(prefix.size()));
}

} // namespace

RPCHelpMan bindfreshfinalitykey()
{
    return RPCHelpMan{
        "bindfreshfinalitykey",
        "Bind a fresh-lineage committee key for the next binding window (bridge V2 per-epoch keys, B-37;\n"
        "regtest only: -b3freshkeyheight). The BLS key is generated from fresh randomness into this node's\n"
        "fresh key store (<datadir>/<net>/finality_signer, outside the wallet file): no wallet backup holds it\n"
        "or can re-derive it. The staking loop's finality signer signs fresh-lineage votes only with these keys\n"
        "and erases each one once its committees are done and deep. The transaction carries a FINALITY_KEY\n"
        "policy-version-2 cell and the MPA type-12 evidence (BIP340 identity authorization and BLS proof of\n"
        "possession). The next block's window counts exactly one target epoch: 0 (the genesis binding, serving\n"
        "Set'_0 and Set'_1) from R_key to R_f-1, then e+2 during fresh epoch e. Refused when this node's store\n"
        "already holds a key for that epoch: two bindings of one validator for one epoch void its seat. A seat\n"
        "also needs the validator's V1 FINALITY_KEY binding and active stake. A held key for that epoch is\n"
        "replaced (erased first) only when it can never seat this validator anyway: no binding transaction was\n"
        "ever recorded for it (so it was never published), or its key was claimed first by another binding (this\n"
        "validator's binding of it is inert). Requires an unlocked wallet and an existing validator key.\n",
        {
            {"epoch", RPCArg::Type::NUM, RPCArg::DefaultHint{"the next binding window's epoch"},
             "The target fresh epoch; any value other than the one the next block's window counts is refused"},
            {"options", RPCArg::Type::OBJ_NAMED_PARAMS, RPCArg::Optional::OMITTED, "",
             {
                 {"broadcast", RPCArg::Type::BOOL, RPCArg::Default{true},
                  "Commit and broadcast the transaction; false returns the signed hex instead (a generated key stays "
                  "in the store with this binding transaction recorded)"},
                 {"reuse_key", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
                  "Regtest drills only, requires broadcast=false: bind an existing key instead of a new one, \"v1\" "
                  "(this wallet's key of its V1 FINALITY_KEY binding) or \"epoch:<n>\" (the key this node's store "
                  "holds for fresh epoch n). The mempool refuses either once that key is on chain "
                  "(fresh-key-ever-bound)"},
             }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
                  {
                      {RPCResult::Type::STR_HEX, "txid", "the binding transaction"},
                      {RPCResult::Type::STR_HEX, "ptxid", "its full-evidence identifier"},
                      {RPCResult::Type::STR_HEX, "hex", /*optional=*/true, "the signed transaction (broadcast=false only)"},
                      {RPCResult::Type::STR_HEX, "validator_key", "this wallet's x-only validator key"},
                      {RPCResult::Type::NUM, "epoch", "the target fresh epoch"},
                      {RPCResult::Type::STR_HEX, "bls_pubkey", "the fresh BLS public key being bound"},
                      {RPCResult::Type::STR, "key_source", "generated (a new key in this node's fresh key store), v1 or stored (reuse_key drills)"},
                      {RPCResult::Type::NUM, "window_start", "first height of the binding window that counts this epoch"},
                      {RPCResult::Type::STR, "status", "UNCONFIRMED (in the local mempool), NOT_IN_MEMPOOL (committed to the wallet, refused by the local mempool for now) or NOT_BROADCAST"},
                      {RPCResult::Type::ARR, "replaced", /*optional=*/true, "held keys for this epoch that were erased and replaced",
                       {{RPCResult::Type::STR, "", "<bls_pubkey>: <why it could never seat this validator>"}}},
                      {RPCResult::Type::ARR, "warnings", /*optional=*/true, "",
                       {{RPCResult::Type::STR, "", "a condition the binding alone does not cure"}}},
                  }},
        RPCExamples{HelpExampleCli("bindfreshfinalitykey", "") +
                    HelpExampleCli("-named bindfreshfinalitykey", "broadcast=false") +
                    HelpExampleRpc("bindfreshfinalitykey", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;
            pwallet->BlockUntilSyncedToCurrentChain();
            LOCK(pwallet->cs_wallet);
            EnsureWalletIsUnlocked(*pwallet);

            bool broadcast{true};
            std::optional<std::string> reuse_key;
            if (!request.params[1].isNull()) {
                const UniValue& options{request.params[1].get_obj()};
                RPCTypeCheckObj(options,
                                {
                                    {"broadcast", UniValueType(UniValue::VBOOL)},
                                    {"reuse_key", UniValueType(UniValue::VSTR)},
                                },
                                /*fAllowNull=*/true, /*fStrict=*/true);
                if (options.exists("broadcast")) broadcast = options["broadcast"].get_bool();
                if (options.exists("reuse_key")) reuse_key = options["reuse_key"].get_str();
            }
            if (reuse_key) {
                if (Params().GetChainType() != ChainType::REGTEST) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "reuse_key is regtest-only");
                }
                if (broadcast) throw JSONRPCError(RPC_INVALID_PARAMETER, "reuse_key requires broadcast=false");
            }

            // Refuse before touching the wallet: on a chain without key mode
            // (every shipped network) nothing is created or written, and a
            // missing validator key is not created here (a seat needs a
            // stake and a V1 binding made with it anyway).
            const std::optional<CPubKey> validator{pwallet->GetValidatorPubKey()};
            const std::array<unsigned char, 32> vk{validator ? XOnlyBytes(*validator) : std::array<unsigned char, 32>{}};
            interfaces::FreshFinalityKeyStatus status{RequireFreshKeyStatus(*pwallet, vk)};
            if (!validator) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "This wallet has no validator key yet (createstake or startstaking creates one)");
            }
            const auto identity{pwallet->GetValidatorSecret()};
            if (!identity) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(identity).original);
            if (!status.next_binding_epoch || !status.window_start) {
                const std::string why{status.next_binding_window == "not-active"
                                          ? strprintf("fresh bindings start at height %d (R_key)", status.key_height.value_or(-1))
                                          : std::string{"the fresh lineage is not bootstrapped or is broken"}};
                throw JSONRPCError(RPC_MISC_ERROR, strprintf("No fresh binding window is open for the next block "
                                                             "(height %d, %s): %s",
                                                             status.tip_height + 1, status.next_binding_window, why));
            }
            const uint64_t epoch{*status.next_binding_epoch};
            if (!request.params[0].isNull()) {
                const int64_t requested{request.params[0].getInt<int64_t>()};
                if (requested < 0 || static_cast<uint64_t>(requested) != epoch) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       strprintf("epoch %d is not the next binding window: a binding included in the "
                                                 "next block (height %d) counts only for fresh epoch %u",
                                                 requested, status.tip_height + 1, epoch));
                }
            }

            // The key: a new one in the fresh key store, or (drills) an existing one.
            node::FreshKeyStore store;
            std::optional<bls::SecretKey> key;
            std::string key_source;
            std::vector<std::string> replaced;
            if (!reuse_key) {
                // A held key for this epoch blocks a new one (two bindings
                // void the seat) unless it can never seat this validator:
                // never published (no binding transaction was recorded: this
                // RPC records it before anything leaves the node), or its key
                // was claimed first elsewhere (this validator's binding of it
                // is inert, and only a holder of its secret could have made
                // that claim). Probe the chain for the held keys' claims.
                std::vector<std::array<unsigned char, 48>> held_keys;
                {
                    node::FreshKeyStore probe;
                    std::string error;
                    if (probe.Open(status.signer_directory, status.chain_domain, vk, error)) {
                        for (const node::FreshKeyRecord& record : probe.Records()) {
                            if (record.Held() && record.target_epoch == epoch) held_keys.push_back(record.bls_pubkey);
                        }
                    }
                }
                if (!held_keys.empty()) {
                    status = RequireFreshKeyStatus(*pwallet, vk, held_keys);
                    if (status.next_binding_epoch != epoch) {
                        throw JSONRPCError(RPC_MISC_ERROR, "The binding window moved while this call ran; retry");
                    }
                }
                const FreshKeyReplacePolicy replace{[&status](const node::FreshKeyRecord& record) -> std::optional<std::string> {
                    if (record.bind_txid.IsNull()) {
                        return std::string{"no binding transaction was ever recorded for it, so it was never published"};
                    }
                    if (const auto height{ClaimedElsewhereAt(status, record.bls_pubkey)}) {
                        return strprintf("its key was claimed first by another binding at height %d, so binding "
                                         "transaction %s can never seat this validator",
                                         *height, record.bind_txid.GetHex());
                    }
                    return std::nullopt;
                }};
                auto generated{GenerateFreshFinalityKey(store, status.signer_directory, status.chain_domain, vk, epoch,
                                                        replace, &replaced)};
                if (!generated) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(generated).original);
                for (const std::string& line : replaced) {
                    pwallet->WalletLogPrintf("bindfreshfinalitykey: replaced fresh key %s\n", line);
                }
                key = std::move(*generated);
                key_source = "generated";
            } else if (*reuse_key == "v1") {
                const interfaces::FinalityStatus v1{pwallet->chain().finalityStatus(vk)};
                if (!v1.bound || v1.revoked) {
                    throw JSONRPCError(RPC_MISC_ERROR, "This validator has no active V1 FINALITY_KEY binding to reuse");
                }
                auto resolved{pwallet->ResolveFinalityBlsKey(v1.binding_seq, &v1.binding_bls_pubkey)};
                if (!resolved) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(resolved).original);
                key = std::move(*resolved);
                key_source = "v1";
            } else if (const auto stored_epoch{ParseStoredKeyEpoch(*reuse_key)}) {
                std::string error;
                if (!store.Open(status.signer_directory, status.chain_domain, vk, error)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, strprintf("cannot open the fresh key store: %s", error));
                }
                for (const node::FreshKeyRecord& record : store.Records()) {
                    if (record.target_epoch != *stored_epoch || !record.Held()) continue;
                    key = store.SecretFor(record.bls_pubkey);
                    if (key) break;
                }
                if (!key) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER,
                                       strprintf("this node's fresh key store holds no key for fresh epoch %u",
                                                 *stored_epoch));
                }
                key_source = "stored";
            } else {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "reuse_key must be \"v1\" or \"epoch:<n>\"");
            }
            const modern::BlsPubkeyBytes pubkey{key->GetPublicKey().Compressed()};
            // A generated key that never reaches a transaction is useless:
            // erase it, so that a retry may generate another for this epoch.
            // A signer pass may have rewritten the store meanwhile: reopen
            // once and retry. If it still fails, the key stays HELD without a
            // binding transaction, which the next call replaces anyway.
            const auto erase_unused{[&] {
                std::string error;
                if (key_source != "generated" || store.Erase(pubkey, error)) return;
                if (store.Open(status.signer_directory, status.chain_domain, vk, error) && store.Erase(pubkey, error)) {
                    return;
                }
                pwallet->WalletLogPrintf("bindfreshfinalitykey: cannot erase unused fresh key %s: %s\n",
                                         HexStr(pubkey), error);
            }};

            auto parts{MakeFreshFinalityKeyBinding(*identity, status.chain_domain, *key, epoch)};
            if (!parts) {
                erase_unused();
                throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(parts).original);
            }
            // As for V1 bindings: the MPA evidence sits outside the txid but
            // must be present while funding so its verification-cost vsize is paid.
            std::vector<CRecipient> recipients{{CNoDestination{parts->cell}, 0, /*fSubtractFeeFromAmount=*/false}};
            CCoinControl coin_control;
            const ModernTransactionOptions modern_options{
                .mpa = {parts->record},
                .native_disintegration = 0};
            auto res{CreateTransaction(*pwallet, recipients, /*change_pos=*/std::nullopt, coin_control, /*sign=*/true,
                                       modern_options)};
            if (!res) {
                erase_unused();
                throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, util::ErrorString(res).original);
            }
            const CTransactionRef tx{res->tx};

            UniValue warnings{UniValue::VARR};
            if (key_source == "generated") {
                // Recorded before anything is public (broadcast or hex), so a
                // held key without a binding transaction was never published
                // and may be replaced. A signer pass may have rewritten the
                // store meanwhile: reopen once and retry; otherwise publish
                // nothing.
                std::string error;
                const uint256 txid{tx->GetHash().ToUint256()};
                if (!store.SetBindTxid(pubkey, txid, error) &&
                    !(store.Open(status.signer_directory, status.chain_domain, vk, error) &&
                      store.SetBindTxid(pubkey, txid, error))) {
                    const std::string record_error{error};
                    erase_unused();
                    throw JSONRPCError(RPC_WALLET_ERROR,
                                       strprintf("cannot record the binding transaction in the fresh key store (%s); "
                                                 "nothing was broadcast",
                                                 record_error));
                }
            }
            {
                const interfaces::FinalityStatus v1{pwallet->chain().finalityStatus(vk)};
                if (!v1.bound || v1.revoked) {
                    warnings.push_back("this validator has no active V1 FINALITY_KEY binding: a fresh binding seats "
                                       "it only together with one (bindfinalitykey) and active stake");
                }
            }

            UniValue obj(UniValue::VOBJ);
            obj.pushKV("txid", tx->GetHash().GetHex());
            obj.pushKV("ptxid", tx->GetPtxid().GetHex());
            std::string tx_status{"NOT_BROADCAST"};
            if (broadcast) {
                pwallet->CommitTransaction(tx, {{"b3", "fresh-finality-key"}}, /*orderForm=*/{});
                tx_status = pwallet->chain().isInMempool(tx->GetHash()) ? "UNCONFIRMED" : "NOT_IN_MEMPOOL";
            } else {
                obj.pushKV("hex", EncodeHexTx(*tx));
            }
            obj.pushKV("validator_key", HexStr(vk));
            obj.pushKV("epoch", epoch);
            obj.pushKV("bls_pubkey", HexStr(pubkey));
            obj.pushKV("key_source", key_source);
            obj.pushKV("window_start", *status.window_start);
            obj.pushKV("status", tx_status);
            if (!replaced.empty()) {
                UniValue replaced_arr{UniValue::VARR};
                for (const std::string& line : replaced) replaced_arr.push_back(line);
                obj.pushKV("replaced", std::move(replaced_arr));
            }
            if (!warnings.empty()) obj.pushKV("warnings", std::move(warnings));
            return obj;
        },
    };
}

RPCHelpMan getfreshfinalitykeys()
{
    const std::vector<RPCResult> binding_doc{
        {RPCResult::Type::NUM, "epoch", "target fresh epoch"},
        {RPCResult::Type::STR_HEX, "bls_pubkey", "the bound BLS public key"},
        {RPCResult::Type::NUM, "height", "including block"},
        {RPCResult::Type::BOOL, "counted", "included inside the window that counts its epoch"},
        {RPCResult::Type::BOOL, "void", "two or more counted bindings of this validator name this epoch: no seat"},
    };
    return RPCHelpMan{
        "getfreshfinalitykeys",
        "This wallet validator's fresh-lineage per-epoch keys (bridge V2 B-37; regtest -b3freshkeyheight): the\n"
        "records of this node's fresh key store (public data only; a secret never leaves the store) joined with\n"
        "the chain's fresh bindings and committees, the chain bindings of this validator that the store does not\n"
        "hold, committee seats without a held key, and alerts. A binding the store does not hold means someone\n"
        "else holds this validator's identity key (an old wallet backup, or a leak). Returns only\n"
        "configured=false without the knob.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
                  {
                      {RPCResult::Type::BOOL, "configured", "Whether fresh-lineage per-epoch keys are configured on this chain"},
                      {RPCResult::Type::STR_HEX, "validator_key", /*optional=*/true, "this wallet's x-only validator key"},
                      {RPCResult::Type::NUM, "key_height", /*optional=*/true, "R_key, the first height that accepts a fresh binding"},
                      {RPCResult::Type::NUM, "activation_height", /*optional=*/true, "R_f, the fresh lineage's first block"},
                      {RPCResult::Type::STR_HEX, "chain_domain", /*optional=*/true, "the modern chain domain"},
                      {RPCResult::Type::NUM, "tip_height", /*optional=*/true, "active tip height"},
                      {RPCResult::Type::BOOL, "state_available", /*optional=*/true, "whether the fresh key state is derivable at the tip"},
                      {RPCResult::Type::STR, "next_binding_window", /*optional=*/true, "open, not-active (below R_key) or lineage-unavailable: the next block's binding window"},
                      {RPCResult::Type::NUM, "next_binding_epoch", /*optional=*/true, "the target epoch a binding in the next block counts for"},
                      {RPCResult::Type::NUM, "window_start", /*optional=*/true, "first height of that window"},
                      {RPCResult::Type::BOOL, "bootstrapped", /*optional=*/true, "whether the fresh lineage exists at the tip"},
                      {RPCResult::Type::NUM, "epoch", /*optional=*/true, "current fresh epoch"},
                      {RPCResult::Type::BOOL, "lineage_broken", /*optional=*/true, "whether the fresh lineage broke for good"},
                      {RPCResult::Type::STR, "store_path", /*optional=*/true, "this validator's fresh key store file"},
                      {RPCResult::Type::STR, "store_error", /*optional=*/true, "why the fresh key store cannot be read"},
                      {RPCResult::Type::ARR, "keys", /*optional=*/true, "the store's keys, in creation order",
                       {{RPCResult::Type::OBJ, "", "",
                         {
                             {RPCResult::Type::NUM, "epoch", "target fresh epoch"},
                             {RPCResult::Type::STR_HEX, "bls_pubkey", "BLS public key"},
                             {RPCResult::Type::STR, "state", "held, or erased (the secret is gone for good)"},
                             {RPCResult::Type::STR_HEX, "bind_txid", /*optional=*/true, "the binding transaction recorded by bindfreshfinalitykey"},
                             {RPCResult::Type::NUM, "confirmations", /*optional=*/true, "confirmations of that transaction (when this wallet has it)"},
                             {RPCResult::Type::NUM, "binding_height", /*optional=*/true, "height of the chain binding of this key (target epoch >= epoch-2)"},
                             {RPCResult::Type::BOOL, "counted", /*optional=*/true, "that binding is inside its window"},
                             {RPCResult::Type::BOOL, "void", /*optional=*/true, "another counted binding of this validator names the same epoch"},
                             {RPCResult::Type::NUM, "claimed_elsewhere_height", /*optional=*/true, "the key was claimed first by another binding (another validator's, or a V1 FINALITY_KEY binding) at this height: this validator's binding of it is inert"},
                             {RPCResult::Type::ARR, "seated_epochs", "retained committees (epoch-1 .. epoch+1) that seat this validator on this key",
                              {{RPCResult::Type::NUM, "", "fresh epoch"}}},
                         }}}},
                      {RPCResult::Type::ARR, "foreign_bindings", /*optional=*/true, "chain bindings of this validator (target epoch >= epoch-2) whose key the store does not hold",
                       {{RPCResult::Type::OBJ, "", "", binding_doc}}},
                      {RPCResult::Type::ARR, "missing_seats", /*optional=*/true, "seats of this validator in Set'_{epoch-1}, Set'_epoch and Set'_{epoch+1} whose key the store does not hold (a Set'_{epoch-1} key erased by policy is not missing)",
                       {{RPCResult::Type::OBJ, "", "",
                         {
                             {RPCResult::Type::NUM, "epoch", "fresh epoch of the committee"},
                             {RPCResult::Type::STR_HEX, "bls_pubkey", "the seated key"},
                         }}}},
                      {RPCResult::Type::ARR, "alerts", /*optional=*/true, "",
                       {{RPCResult::Type::STR, "", "a foreign binding, a void seat, a store key claimed first elsewhere or an unreadable store"}}},
                  }},
        RPCExamples{HelpExampleCli("getfreshfinalitykeys", "") + HelpExampleRpc("getfreshfinalitykeys", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;
            pwallet->BlockUntilSyncedToCurrentChain();
            LOCK(pwallet->cs_wallet);
            const auto validator{pwallet->GetValidatorPubKey()};
            if (!validator) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "This wallet has no validator key yet (createstake or startstaking creates one)");
            }
            const std::array<unsigned char, 32> vk{XOnlyBytes(*validator)};
            interfaces::FreshFinalityKeyStatus status{pwallet->chain().freshFinalityKeyStatus(vk, {})};
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("configured", status.configured);
            if (!status.configured) return obj;
            // The store first (public records only), then the chain view once
            // more with the store's keys probed for a first claim elsewhere.
            std::vector<node::FreshKeyRecord> records;
            std::string store_error;
            if (status.chain_domain.IsNull()) {
                store_error = "the modern chain domain is not derivable";
            } else {
                node::FreshKeyStore store;
                if (store.Open(status.signer_directory, status.chain_domain, vk, store_error)) records = store.Records();
            }
            if (!records.empty()) {
                std::vector<std::array<unsigned char, 48>> probe;
                for (const node::FreshKeyRecord& record : records) probe.push_back(record.bls_pubkey);
                status = pwallet->chain().freshFinalityKeyStatus(vk, probe);
            }
            obj.pushKV("validator_key", HexStr(vk));
            if (status.key_height) obj.pushKV("key_height", *status.key_height);
            obj.pushKV("activation_height", status.activation_height);
            if (!status.chain_domain.IsNull()) obj.pushKV("chain_domain", status.chain_domain.GetHex());
            obj.pushKV("tip_height", status.tip_height);
            obj.pushKV("state_available", status.available);
            obj.pushKV("next_binding_window", status.next_binding_window);
            if (status.next_binding_epoch) obj.pushKV("next_binding_epoch", *status.next_binding_epoch);
            if (status.window_start) obj.pushKV("window_start", *status.window_start);
            obj.pushKV("bootstrapped", status.bootstrapped);
            if (status.bootstrapped) {
                obj.pushKV("epoch", status.epoch);
                obj.pushKV("lineage_broken", status.broken);
            }

            if (!status.chain_domain.IsNull()) {
                obj.pushKV("store_path", fs::PathToString(node::FreshKeyStore::StatePath(
                                             status.signer_directory, status.chain_domain, vk)));
            }
            UniValue alerts{UniValue::VARR};
            if (!store_error.empty()) {
                obj.pushKV("store_error", store_error);
                alerts.push_back(strprintf("cannot read the fresh key store: %s", store_error));
            }
            const auto record_of{[&records](const modern::BlsPubkeyBytes& pk) -> const node::FreshKeyRecord* {
                for (const node::FreshKeyRecord& record : records) {
                    if (record.bls_pubkey == pk) return &record;
                }
                return nullptr;
            }};
            const auto binding_of{[&status](const modern::BlsPubkeyBytes& pk) -> const interfaces::FreshFinalityKeyBinding* {
                for (const interfaces::FreshFinalityKeyBinding& binding : status.bindings) {
                    if (binding.bls_pubkey == pk) return &binding;
                }
                return nullptr;
            }};

            UniValue keys{UniValue::VARR};
            for (const node::FreshKeyRecord& record : records) {
                UniValue entry(UniValue::VOBJ);
                entry.pushKV("epoch", record.target_epoch);
                entry.pushKV("bls_pubkey", HexStr(record.bls_pubkey));
                entry.pushKV("state", node::FreshKeyStateName(record.state));
                if (!record.bind_txid.IsNull()) {
                    entry.pushKV("bind_txid", record.bind_txid.GetHex());
                    if (const CWalletTx* wtx{pwallet->GetWalletTx(Txid::FromUint256(record.bind_txid))}) {
                        entry.pushKV("confirmations", pwallet->GetTxDepthInMainChain(*wtx));
                    }
                }
                if (const interfaces::FreshFinalityKeyBinding* binding{binding_of(record.bls_pubkey)}) {
                    entry.pushKV("binding_height", binding->height);
                    entry.pushKV("counted", binding->counted);
                    entry.pushKV("void", binding->voided);
                }
                if (const auto elsewhere{ClaimedElsewhereAt(status, record.bls_pubkey)}) {
                    entry.pushKV("claimed_elsewhere_height", *elsewhere);
                    if (record.Held()) alerts.push_back(strprintf(
                        "fresh key %s of fresh epoch %u was claimed first by another binding at height %d: this validator's binding of it is inert (it seats nothing), and only a holder of the key's secret could have made that claim; bindfreshfinalitykey replaces it while that epoch's binding window is open",
                        HexStr(record.bls_pubkey), record.target_epoch, *elsewhere));
                }
                UniValue seated{UniValue::VARR};
                for (const interfaces::FreshFinalityKeySeat& seat : status.seats) {
                    if (seat.bls_pubkey == record.bls_pubkey) seated.push_back(seat.epoch);
                }
                entry.pushKV("seated_epochs", std::move(seated));
                keys.push_back(std::move(entry));
            }
            obj.pushKV("keys", std::move(keys));

            UniValue foreign{UniValue::VARR};
            std::map<uint64_t, std::vector<modern::BlsPubkeyBytes>> voided;
            for (const interfaces::FreshFinalityKeyBinding& binding : status.bindings) {
                if (binding.voided) voided[binding.epoch].push_back(binding.bls_pubkey);
                if (!store_error.empty() || record_of(binding.bls_pubkey)) continue;
                UniValue entry(UniValue::VOBJ);
                entry.pushKV("epoch", binding.epoch);
                entry.pushKV("bls_pubkey", HexStr(binding.bls_pubkey));
                entry.pushKV("height", binding.height);
                entry.pushKV("counted", binding.counted);
                entry.pushKV("void", binding.voided);
                foreign.push_back(std::move(entry));
                alerts.push_back(strprintf(
                    "fresh key %s bound for fresh epoch %u at height %d under this validator's identity is not in this node's fresh key store: an old wallet backup or a leaked identity key is binding keys (consider restaking under a new identity)",
                    HexStr(binding.bls_pubkey), binding.epoch, binding.height));
            }
            for (const auto& [voided_epoch, pubkeys] : voided) {
                std::string keys_text;
                for (const modern::BlsPubkeyBytes& pk : pubkeys) keys_text += (keys_text.empty() ? "" : ", ") + HexStr(pk);
                alerts.push_back(strprintf(
                    "fresh-lineage seat for fresh epoch %u is void: %u fresh bindings of this validator name it (%s); an old wallet backup or a leaked identity key may be binding keys",
                    voided_epoch, pubkeys.size(), keys_text));
            }
            obj.pushKV("foreign_bindings", std::move(foreign));

            // Seats this node must be able to sign for (the signer's rule):
            // a Set'_{e-1} key the store erased went by policy; not missing.
            UniValue missing{UniValue::VARR};
            if (store_error.empty()) {
                for (const interfaces::FreshFinalityKeySeat& seat : status.seats) {
                    const node::FreshKeyRecord* record{record_of(seat.bls_pubkey)};
                    if (record && record->Held()) continue;
                    if (record && seat.epoch + 1 == status.epoch) continue;
                    UniValue entry(UniValue::VOBJ);
                    entry.pushKV("epoch", seat.epoch);
                    entry.pushKV("bls_pubkey", HexStr(seat.bls_pubkey));
                    missing.push_back(std::move(entry));
                }
            }
            obj.pushKV("missing_seats", std::move(missing));
            obj.pushKV("alerts", std::move(alerts));
            return obj;
        },
    };
}

RPCHelpMan getfinalityinfo()
{
    return RPCHelpMan{
        "getfinalityinfo",
        "This wallet's Modern-PoS finality view: the validator's FINALITY_KEY binding and derived BLS\n"
        "public keys, eligibility and weight in the active validator set, the epoch state machine, the\n"
        "latest finalized checkpoint, the persisted pin and the local signing status. Private BLS material\n"
        "is never returned. Recovery diagnostics are a cached observation from this wallet's running signer,\n"
        "not a fresh disk-journal inspection or permission to reset its lock. Normal orphan-lock recovery\n"
        "requires a strictly newer certificate included on the active chain using the exact locked epoch\n"
        "and signing set. A higher tip, timeout, or successor-epoch certificate is not that proof.\n"
        "Separately, getfinalityrecoveryinfo and setfinalityrecovery expose an explicit operator-trusted\n"
        "emergency exception. It is not normal certificate recovery and does not revoke old signatures.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
                  {
                      {RPCResult::Type::STR_HEX, "validator_key", "this wallet's x-only validator key"},
                      {RPCResult::Type::OBJ, "binding", "the on-chain FINALITY_KEY binding",
                       {
                           {RPCResult::Type::BOOL, "bound", ""},
                           {RPCResult::Type::BOOL, "revoked", ""},
                           {RPCResult::Type::NUM, "seq", /*optional=*/true, "current binding sequence"},
                           {RPCResult::Type::STR_HEX, "bls_pubkey", /*optional=*/true, "the bound BLS public key"},
                           {RPCResult::Type::NUM, "since_height", /*optional=*/true, ""},
                           {RPCResult::Type::BOOL, "key_is_ours", /*optional=*/true, "the bound key matches this wallet's derived or imported key"},
                           {RPCResult::Type::STR_HEX, "imported_bls_pubkey", /*optional=*/true, "the independently imported BLS key, if any"},
                           {RPCResult::Type::STR_HEX, "next_bls_pubkey", /*optional=*/true, "the key a rotation/bind would bind next"},
                       }},
                      {RPCResult::Type::OBJ, "validator_set", "the set in force",
                       {
                           {RPCResult::Type::BOOL, "member", "this validator is in the current set (block-eligible)"},
                           {RPCResult::Type::NUM, "weight", "this validator's epoch-frozen current-set weight (whole modern B3)"},
                           {RPCResult::Type::NUM, "total_weight", ""},
                           {RPCResult::Type::NUM, "quorum_weight", ""},
                           {RPCResult::Type::NUM, "size", ""},
                       }},
                      {RPCResult::Type::OBJ, "epoch", "the epoch state machine",
                       {
                           {RPCResult::Type::BOOL, "active", "the chain is inside the modern-PoS phase"},
                           {RPCResult::Type::BOOL, "bootstrapped", ""},
                           {RPCResult::Type::NUM, "epoch", ""},
                           {RPCResult::Type::NUM, "epoch_start", ""},
                           {RPCResult::Type::BOOL, "handover_certified", ""},
                           {RPCResult::Type::BOOL, "lineage_broken", ""},
                           {RPCResult::Type::STR_HEX, "current_set_hash", /*optional=*/true, ""},
                           {RPCResult::Type::STR_HEX, "next_set_hash", /*optional=*/true, ""},
                       }},
                      {RPCResult::Type::OBJ, "finalized", /*optional=*/true, "the latest finalized checkpoint",
                       {
                           {RPCResult::Type::NUM, "height", ""},
                           {RPCResult::Type::STR_HEX, "hash", ""},
                           {RPCResult::Type::NUM, "epoch", ""},
                       }},
                      {RPCResult::Type::OBJ, "pin", /*optional=*/true, "the persisted finality pin",
                       {
                           {RPCResult::Type::NUM, "height", ""},
                           {RPCResult::Type::STR_HEX, "hash", ""},
                       }},
                      {RPCResult::Type::OBJ, "signing", "local finality signing",
                       {
                           {RPCResult::Type::BOOL, "armed", "the local finality signer is enabled; last_signed_height shows its actual progress"},
                           {RPCResult::Type::NUM, "last_signed_height", ""},
                           {RPCResult::Type::NUM, "pool_checkpoints", "checkpoints tracked by the local signature pool"},
                           {RPCResult::Type::STR, "last_error", /*optional=*/true, "the staking loop's last reported error"},
                           {RPCResult::Type::OBJ, "recovery", "read-only snapshot; unavailable does not mean safe to sign",
                            {
                                {RPCResult::Type::BOOL, "available", "a matching running signer has observed a synced chain tip"},
                                {RPCResult::Type::STR, "state", "diagnostic state, including blocked_on_orphan_vote, waiting_for_locked_height, journal_missing, signer_error, lock_matches_chain, or an unavailable reason"},
                                {RPCResult::Type::STR, "source", /*optional=*/true, "running_signer_snapshot; never reads or recreates a journal"},
                                {RPCResult::Type::OBJ, "observed_tip", /*optional=*/true, "chain tip used for this cached observation",
                                 {{RPCResult::Type::NUM, "height", ""}, {RPCResult::Type::STR_HEX, "hash", "B3 display order"}}},
                                {RPCResult::Type::BOOL, "journal_open", /*optional=*/true, "the signer already has an open journal"},
                                {RPCResult::Type::BOOL, "journal_present", /*optional=*/true, "the open signer has a loaded public journal record"},
                                {RPCResult::Type::BOOL, "permanent_error", /*optional=*/true, "the signer requires its durable-state error to be resolved; certificate checks alone cannot bypass it"},
                                {RPCResult::Type::BOOL, "blocked_on_orphan_vote", /*optional=*/true, "the observed active-chain block at the lock height differs from the locked block; false is not an assertion that the signer is armed"},
                                {RPCResult::Type::OBJ, "last_vote", /*optional=*/true, "last vote recorded in the journal, which may precede a later certified ancestry anchor; hashes/digest use B3 display order",
                                 {
                                     {RPCResult::Type::NUM, "height", ""},
                                     {RPCResult::Type::STR_HEX, "hash", "signed block hash"},
                                     {RPCResult::Type::STR_HEX, "digest", "exact signed-object digest"},
                                 }},
                                {RPCResult::Type::OBJ, "lock", /*optional=*/true, "the existing ancestry lock; hashes use B3 display order",
                                 {
                                     {RPCResult::Type::NUM, "height", ""},
                                     {RPCResult::Type::STR_HEX, "hash", "locked block hash"},
                                     {RPCResult::Type::STR_HEX, "digest", "locked signed-object digest"},
                                     {RPCResult::Type::NUM, "epoch", ""},
                                     {RPCResult::Type::STR_HEX, "signing_set_hash", ""},
                                     {RPCResult::Type::STR_HEX, "successor_set_hash", ""},
                                     {RPCResult::Type::STR_HEX, "current_chain_hash", /*optional=*/true, "observed active-chain block at this height; absent if that height is not available"},
                                 }},
                                {RPCResult::Type::OBJ, "required_proof", /*optional=*/true, "normal protocol proof for changing an orphan lock; the separate operator exception is described by getfinalityrecoveryinfo",
                                 {
                                     {RPCResult::Type::STR, "type", "newer_included_same_epoch_same_set_certificate"},
                                     {RPCResult::Type::NUM, "checkpoint_height_strictly_greater_than", "the locked height; equality is insufficient"},
                                     {RPCResult::Type::NUM, "epoch", "required exact signing epoch"},
                                     {RPCResult::Type::STR_HEX, "signing_set_hash", "required exact signing set"},
                                 }},
                                {RPCResult::Type::OBJ, "latest_certificate_checks", /*optional=*/true, "checks on the observed latest certificate, not authorization to change a journal; false if absent or not derivable",
                                 {
                                     {RPCResult::Type::BOOL, "included_on_active_chain", ""},
                                     {RPCResult::Type::BOOL, "strictly_newer", ""},
                                     {RPCResult::Type::BOOL, "same_epoch", ""},
                                     {RPCResult::Type::BOOL, "same_signing_set", ""},
                                     {RPCResult::Type::BOOL, "checkpoint_reconstructible", ""},
                                 }},
                                {RPCResult::Type::OBJ, "observed_finalized", /*optional=*/true, "latest finalized checkpoint at observed_tip, which may lag the top-level finalized view",
                                 {
                                     {RPCResult::Type::NUM, "height", ""},
                                     {RPCResult::Type::STR_HEX, "hash", ""},
                                     {RPCResult::Type::NUM, "epoch", ""},
                                     {RPCResult::Type::NUM, "included_at_height", ""},
                                     {RPCResult::Type::STR_HEX, "signing_set_hash", /*optional=*/true, "present only while the signing set can be reconstructed"},
                                 }},
                            }},
                           {RPCResult::Type::ARR, "snapshot_keys", "public keys required by the frozen current, previous and prepared next validator sets",
                            {
                                {RPCResult::Type::OBJ, "", "",
                                 {
                                     {RPCResult::Type::NUM, "epoch", "the snapshot epoch"},
                                     {RPCResult::Type::NUM, "binding_seq", "the binding sequence recorded by this snapshot"},
                                     {RPCResult::Type::STR_HEX, "bls_pubkey", "the exact BLS public key required by this snapshot"},
                                     {RPCResult::Type::BOOL, "current", "this is the validator's current-set key"},
                                     {RPCResult::Type::BOOL, "key_available", /*optional=*/true, "whether this unlocked wallet can resolve the exact snapshot key; does not indicate that it is loaded in the staking loop"},
                                     {RPCResult::Type::STR, "key_error", /*optional=*/true, "why the unlocked wallet could not resolve this snapshot key"},
                                 }},
                            }},
                       }},
                  }},
        RPCExamples{HelpExampleCli("getfinalityinfo", "") + HelpExampleRpc("getfinalityinfo", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
            if (!pwallet) return UniValue::VNULL;
            pwallet->BlockUntilSyncedToCurrentChain();
            LOCK(pwallet->cs_wallet);
            const FinalityRpcContext ctx{FinalityContext(*pwallet)};
            const interfaces::FinalityStatus& st{ctx.status};

            UniValue obj(UniValue::VOBJ);
            obj.pushKV("validator_key", HexStr(ctx.vk));
            UniValue binding(UniValue::VOBJ);
            binding.pushKV("bound", st.bound);
            binding.pushKV("revoked", st.revoked);
            if (st.bound) {
                binding.pushKV("seq", static_cast<uint64_t>(st.binding_seq));
                binding.pushKV("bls_pubkey", HexStr(st.binding_bls_pubkey));
                binding.pushKV("since_height", st.binding_height);
            }
            // Key material needs the identity secret: only with an unlocked wallet.
            if (!pwallet->IsLocked()) {
                if (st.bound && !st.revoked) {
                    binding.pushKV("key_is_ours",
                                   pwallet->ResolveFinalityBlsKey(st.binding_seq, &st.binding_bls_pubkey).has_value());
                }
                if (pwallet->HasImportedFinalityBlsKey()) {
                    if (const auto imported{pwallet->GetImportedFinalityBlsKey()}) {
                        binding.pushKV("imported_bls_pubkey", HexStr(imported->GetPublicKey().Compressed()));
                    }
                }
                if (!st.bound || st.binding_seq < std::numeric_limits<uint32_t>::max()) {
                    const uint32_t next_seq{st.bound ? st.binding_seq + 1 : 0};
                    if (const auto next{pwallet->ResolveFinalityBlsKey(
                            next_seq, /*bound_bls_pubkey=*/nullptr)}) {
                        binding.pushKV("next_bls_pubkey",
                                       HexStr(next->GetPublicKey().Compressed()));
                    }
                }
            }
            obj.pushKV("binding", binding);
            UniValue set(UniValue::VOBJ);
            set.pushKV("member", st.in_current_set);
            set.pushKV("weight", st.member_weight);
            set.pushKV("total_weight", st.total_weight);
            set.pushKV("quorum_weight", st.quorum_weight);
            set.pushKV("size", st.set_size);
            obj.pushKV("validator_set", set);
            UniValue epoch(UniValue::VOBJ);
            epoch.pushKV("active", st.active);
            epoch.pushKV("bootstrapped", st.bootstrapped);
            epoch.pushKV("epoch", st.epoch);
            epoch.pushKV("epoch_start", st.epoch_start);
            epoch.pushKV("handover_certified", st.handover_certified);
            epoch.pushKV("lineage_broken", st.lineage_broken);
            if (!st.current_set_hash.IsNull()) epoch.pushKV("current_set_hash", st.current_set_hash.GetHex());
            if (!st.next_set_hash.IsNull()) epoch.pushKV("next_set_hash", st.next_set_hash.GetHex());
            obj.pushKV("epoch", epoch);
            if (st.finalized_height) {
                UniValue fin(UniValue::VOBJ);
                fin.pushKV("height", *st.finalized_height);
                fin.pushKV("hash", st.finalized_hash.GetHex());
                fin.pushKV("epoch", st.finalized_epoch);
                obj.pushKV("finalized", fin);
            }
            if (st.pin_height) {
                UniValue pin(UniValue::VOBJ);
                pin.pushKV("height", *st.pin_height);
                pin.pushKV("hash", st.pin_hash.GetHex());
                obj.pushKV("pin", pin);
            }
            const interfaces::StakingStatus staking{pwallet->chain().stakingStatus(ctx.vk)};
            UniValue signing(UniValue::VOBJ);
            signing.pushKV("armed", staking.finality_signing);
            signing.pushKV("last_signed_height", staking.last_signed_height);
            signing.pushKV("pool_checkpoints", st.pool_checkpoints);
            if (!staking.last_error.empty()) signing.pushKV("last_error", staking.last_error);
            signing.pushKV("recovery", FinalityRecoveryStatusToJSON(staking, ctx.vk));
            UniValue snapshot_keys(UniValue::VARR);
            for (const auto& snapshot : st.signing_keys) {
                UniValue snapshot_key(UniValue::VOBJ);
                snapshot_key.pushKV("epoch", snapshot.epoch);
                snapshot_key.pushKV("binding_seq", static_cast<uint64_t>(snapshot.binding_seq));
                snapshot_key.pushKV("bls_pubkey", HexStr(snapshot.bls_pubkey));
                snapshot_key.pushKV("current", st.in_current_set && snapshot.epoch == st.epoch);
                if (!pwallet->IsLocked()) {
                    const auto key{pwallet->ResolveFinalityBlsKey(snapshot.binding_seq, &snapshot.bls_pubkey)};
                    snapshot_key.pushKV("key_available", key.has_value());
                    if (!key) snapshot_key.pushKV("key_error", util::ErrorString(key).original);
                }
                snapshot_keys.push_back(std::move(snapshot_key));
            }
            signing.pushKV("snapshot_keys", std::move(snapshot_keys));
            obj.pushKV("signing", signing);
            return obj;
        },
    };
}

RPCHelpMan exportbridgebootstrapidentity()
{
    return RPCHelpMan{
        "exportbridgebootstrapidentity",
        "Export the public proof package needed to place this wallet's "
        "confirmed FINALITY_KEY identity in the immutable four-member "
        "Ethereum bridge bootstrap manifest. This command can be used before "
        "Modern PoS starts. It returns a BLS proof of possession and a fresh "
        "BIP340 proof of the already-confirmed validator-to-BLS binding; no "
        "private key is returned. Requires an unlocked wallet.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Public bootstrap identity package",
                  {
                      {RPCResult::Type::STR_HEX, "validator_key", "Wallet's x-only validator identity"},
                      {RPCResult::Type::STR_HEX, "bls_pubkey", "Confirmed compressed BLS public key"},
                      {RPCResult::Type::STR_HEX, "proof_of_possession", "Public 96-byte BLS proof of possession"},
                      {RPCResult::Type::NUM, "binding_seq", "Confirmed FINALITY_KEY binding sequence"},
                      {RPCResult::Type::NUM, "binding_height", "Height at which the binding became active"},
                      {RPCResult::Type::STR_HEX, "binding_bip340_sig", "Public BIP340 proof binding validator_key, BLS key and sequence to this chain"},
                      {RPCResult::Type::STR_HEX, "chain_domain", "B3 modern chain domain in Ethereum/wire byte order"},
                  }},
        RPCExamples{HelpExampleCli("exportbridgebootstrapidentity", "") +
                    HelpExampleRpc("exportbridgebootstrapidentity", "")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();

            LOCK(wallet->cs_wallet);
            EnsureWalletIsUnlocked(*wallet);
            const FinalityRpcContext ctx{FinalityContext(*wallet)};
            const interfaces::FinalityStatus& status{ctx.status};
            if (!status.bound || status.revoked || status.binding_height < 0) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    status.revoked
                        ? "This validator's FINALITY_KEY binding is revoked"
                        : "This validator has no confirmed FINALITY_KEY binding");
            }
            const auto key{wallet->ResolveFinalityBlsKey(
                status.binding_seq, &status.binding_bls_pubkey)};
            if (!key) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   util::ErrorString(key).original);
            }
            const auto identity{wallet->GetValidatorSecret()};
            if (!identity) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   util::ErrorString(identity).original);
            }

            const uint256 binding_digest{modern::FinalityBindDigest(
                status.chain_domain, ctx.vk, key->GetPublicKey().Compressed(),
                status.binding_seq)};
            std::array<unsigned char, modern::BIP340_SIG_SIZE>
                binding_signature{};
            const uint256 binding_aux{GetRandHash()};
            if (!identity->SignSchnorr(binding_digest, binding_signature,
                                       nullptr, binding_aux)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "BIP340 bootstrap-manifest binding proof failed");
            }

            UniValue result{UniValue::VOBJ};
            result.pushKV("validator_key", HexStr(ctx.vk));
            result.pushKV("bls_pubkey",
                          HexStr(key->GetPublicKey().Compressed()));
            result.pushKV("proof_of_possession",
                          HexStr(key->SignPoP().Compressed()));
            result.pushKV("binding_seq",
                          static_cast<uint64_t>(status.binding_seq));
            result.pushKV("binding_height", status.binding_height);
            result.pushKV("binding_bip340_sig",
                          HexStr(binding_signature));
            result.pushKV("chain_domain", HexStr(status.chain_domain));
            return result;
        },
    };
}

RPCHelpMan signbridgebootstrap()
{
    return RPCHelpMan{
        "signbridgebootstrap",
        "Sign the one-time Ethereum bridge handoff from the published four-key "
        "bootstrap committee to B3's exact Set_0 snapshot. The message is "
        "derived entirely from the active chain: block M-1, zero withdrawal "
        "root, the canonical Set_0 hash, and epoch zero. No caller-supplied "
        "digest is accepted and no BLS private key is returned. The command "
        "is available only while the node can reproduce Set_0 and requires "
        "this wallet's exact confirmed, non-revoked FINALITY_KEY binding named "
        "in the immutable four-key deployment manifest.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"bls_pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "Exact 48-byte compressed BLS key from the deployed bootstrap manifest"},
            {"binding_seq", RPCArg::Type::NUM, RPCArg::Optional::NO,
             "Exact confirmed FINALITY_KEY binding sequence from that manifest"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Exact bootstrap signature package",
                  {
                      {RPCResult::Type::STR_HEX, "validator_key", "Wallet's x-only validator identity"},
                      {RPCResult::Type::STR_HEX, "bls_pubkey", "Bound compressed BLS public key"},
                      {RPCResult::Type::STR_HEX, "proof_of_possession", "Public 96-byte BLS proof of possession for the manifest"},
                      {RPCResult::Type::NUM, "binding_seq", "Exact binding sequence used"},
                      {RPCResult::Type::NUM, "binding_height", "Height at which the binding became active"},
                      {RPCResult::Type::STR_HEX, "binding_bip340_sig", "Fresh public BIP340 proof binding validator_key, BLS key and sequence to this chain"},
                      {RPCResult::Type::STR_HEX, "chain_domain", "B3 modern chain domain in Ethereum/wire byte order"},
                      {RPCResult::Type::NUM, "snapshot_height", "Exact B3 snapshot height M-1"},
                      {RPCResult::Type::STR_HEX, "snapshot_block_hash", "Active-chain block-hash bytes at M-1 in the order used by the Ethereum FinalizedBlock"},
                      {RPCResult::Type::STR_HEX, "snapshot_block_hash_b3", "Active-chain block hash at M-1 in ordinary B3 display order"},
                      {RPCResult::Type::STR_HEX, "set0_hash", "Keccak commitment to the canonical Set_0 header in Ethereum/wire byte order"},
                      {RPCResult::Type::STR_HEX, "set0_header", "Canonical 110-byte Set_0 header"},
                      {RPCResult::Type::STR_HEX, "finalized_block", "Canonical 112-byte bootstrap FinalizedBlock"},
                      {RPCResult::Type::STR_HEX, "digest", "B3/FINALITY/V1 digest bytes that were signed"},
                      {RPCResult::Type::STR_HEX, "signature", "Compressed 96-byte BLS signature"},
                      {RPCResult::Type::NUM, "finality_pin_height", "Finalized descendant height that made the M-1 snapshot safe to sign"},
                      {RPCResult::Type::STR_HEX, "finality_pin_hash", "Finality-pin hash bytes in Ethereum/wire order"},
                      {RPCResult::Type::STR_HEX, "finality_pin_hash_b3", "Finality-pin hash in ordinary B3 display order"},
                  }},
        RPCExamples{HelpExampleCli("signbridgebootstrap", "\"<manifest_bls_pubkey>\" 0") +
                    HelpExampleRpc("signbridgebootstrap", "\"<manifest_bls_pubkey>\", 0")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();

            LOCK(wallet->cs_wallet);
            EnsureWalletIsUnlocked(*wallet);
            const FinalityRpcContext ctx{FinalityContext(*wallet)};
            const interfaces::FinalityStatus& status{ctx.status};
            if (!status.bootstrap_snapshot_height ||
                status.bootstrap_snapshot_hash.IsNull() ||
                status.bootstrap_set_hash.IsNull()) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    "The exact Set_0 bridge snapshot is not available yet");
            }
            if (!status.pin_height ||
                *status.pin_height <= *status.bootstrap_snapshot_height) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    "Set_0 is not safe to sign until B3 finality has pinned a descendant of block M-1");
            }
            const auto set0{modern::ValidatorSetHeader::Decode(
                status.bootstrap_set_header)};
            if (!set0 || set0->epoch != 0 ||
                modern::ValidatorSetHash(*set0) != status.bootstrap_set_hash) {
                throw JSONRPCError(
                    RPC_INTERNAL_ERROR,
                    "The node returned an inconsistent Set_0 bridge snapshot");
            }
            if (!status.bound || status.revoked) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    status.revoked
                        ? "This validator's FINALITY_KEY binding is revoked"
                        : "This validator has no confirmed FINALITY_KEY binding");
            }
            const std::vector<unsigned char> expected_pubkey{
                ParseHexV(request.params[0], "bls_pubkey")};
            if (expected_pubkey.size() != modern::BLS_PUBKEY_SIZE ||
                !bls::PublicKey::Decode(expected_pubkey)) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "bls_pubkey must be one valid 48-byte compressed BLS public key");
            }
            const uint32_t binding_seq{
                request.params[1].getInt<uint32_t>()};
            if (binding_seq != status.binding_seq ||
                expected_pubkey != status.binding_bls_pubkey) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "The requested bootstrap key and sequence do not match this validator's current confirmed binding");
            }
            if (status.binding_height < 0 ||
                status.binding_height > *status.bootstrap_snapshot_height) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    "The requested bootstrap key was not bound by the M-1 snapshot");
            }
            const auto key{wallet->ResolveFinalityBlsKey(
                binding_seq, &expected_pubkey)};
            if (!key) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   util::ErrorString(key).original);
            }
            const auto identity{wallet->GetValidatorSecret()};
            if (!identity) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   util::ErrorString(identity).original);
            }

            const uint256 binding_digest{modern::FinalityBindDigest(
                status.chain_domain, ctx.vk, key->GetPublicKey().Compressed(),
                binding_seq)};
            std::array<unsigned char, modern::BIP340_SIG_SIZE>
                binding_signature{};
            const uint256 binding_aux{GetRandHash()};
            if (!identity->SignSchnorr(binding_digest, binding_signature,
                                       nullptr, binding_aux)) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "BIP340 bootstrap-manifest binding proof failed");
            }

            modern::FinalizedBlock snapshot;
            snapshot.height =
                static_cast<uint64_t>(*status.bootstrap_snapshot_height);
            snapshot.block_hash = status.bootstrap_snapshot_hash;
            snapshot.withdrawal_root = uint256{};
            snapshot.validator_set_hash = status.bootstrap_set_hash;
            snapshot.epoch = 0;
            const uint256 digest{
                modern::FinalityDigest(status.chain_domain, snapshot)};
            const auto signature{
                key->Sign(std::span<const unsigned char>{digest.begin(), 32})
                    .Compressed()};
            const auto snapshot_bytes{snapshot.Encode()};

            UniValue result{UniValue::VOBJ};
            result.pushKV("validator_key", HexStr(ctx.vk));
            result.pushKV("bls_pubkey",
                          HexStr(key->GetPublicKey().Compressed()));
            result.pushKV("proof_of_possession",
                          HexStr(key->SignPoP().Compressed()));
            result.pushKV("binding_seq", static_cast<uint64_t>(binding_seq));
            result.pushKV("binding_height", status.binding_height);
            result.pushKV("binding_bip340_sig",
                          HexStr(binding_signature));
            result.pushKV("chain_domain", HexStr(status.chain_domain));
            result.pushKV("snapshot_height",
                          *status.bootstrap_snapshot_height);
            result.pushKV("snapshot_block_hash",
                          HexStr(status.bootstrap_snapshot_hash));
            result.pushKV("snapshot_block_hash_b3",
                          status.bootstrap_snapshot_hash.GetHex());
            result.pushKV("set0_hash", HexStr(status.bootstrap_set_hash));
            result.pushKV("set0_header",
                          HexStr(status.bootstrap_set_header));
            result.pushKV("finalized_block", HexStr(snapshot_bytes));
            result.pushKV("digest", HexStr(digest));
            result.pushKV("signature", HexStr(signature));
            result.pushKV("finality_pin_height", *status.pin_height);
            result.pushKV("finality_pin_hash", HexStr(status.pin_hash));
            result.pushKV("finality_pin_hash_b3", status.pin_hash.GetHex());
            return result;
        },
    };
}

RPCHelpMan getfinalityrecoveryinfo()
{
    return RPCHelpMan{
        "getfinalityrecoveryinfo",
        "Read this wallet's cached finality signer observation and public recovery configuration.\n"
        "Does not unlock the wallet, create a validator key, open a journal, or authorize recovery.\n"
        "Normal recovery uses a newer included same-epoch, same-set certificate. The separate\n"
        "operator-trusted exception requires an exact incident manifest and accepted anchor.\n"
        "A configured manifest is not evidence that recovery has applied. After startstaking,\n"
        "inspect the observed lock and subsequent last_vote to confirm actual progress.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
                  {
                      {RPCResult::Type::STR_HEX, "validator_key", /*optional=*/true, "this wallet's existing public validator key"},
                      {RPCResult::Type::OBJ, "signer", "cached observation, with the same fields as getfinalityinfo signing.recovery; not a new journal inspection",
                       {{RPCResult::Type::ELISION, "", ""}}},
                      {RPCResult::Type::OBJ, "operator_recovery", "explicit emergency exception, not a quorum proof",
                       {
                           {RPCResult::Type::BOOL, "supported", "the node supports operator recovery configuration"},
                           {RPCResult::Type::BOOL, "staking_running", "stop staking before changing the plan"},
                           {RPCResult::Type::BOOL, "configured_for_wallet", "this wallet has a configured plan; does not mean applied"},
                           {RPCResult::Type::BOOL, "configured_for_other_wallet", "another validator has a plan; this wallet cannot replace or clear it"},
                           {RPCResult::Type::STR, "persistence", "RPC configuration is memory-only; startup options remain unchanged"},
                           {RPCResult::Type::OBJ, "manifest", /*optional=*/true, "this wallet's public manifest, using the setfinalityrecovery schema",
                            {{RPCResult::Type::ELISION, "", ""}}},
                           {RPCResult::Type::STR, "warning", "limitations of this operator trust exception"},
                       }},
                  }},
        RPCExamples{HelpExampleCli("getfinalityrecoveryinfo", "") + HelpExampleRpc("getfinalityrecoveryinfo", "")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> pwallet{GetWalletForJSONRPCRequest(request)};
            if (!pwallet) return UniValue::VNULL;
            LOCK(pwallet->cs_wallet);
            const auto validator{pwallet->GetValidatorPubKey()};
            std::optional<std::array<unsigned char, 32>> vk;
            if (validator) vk = XOnlyBytes(*validator);
            UniValue obj{UniValue::VOBJ};
            if (vk) {
                obj.pushKV("validator_key", HexStr(*vk));
                obj.pushKV("signer", FinalityRecoveryStatusToJSON(pwallet->chain().stakingStatus(vk), *vk));
            } else {
                UniValue signer{UniValue::VOBJ};
                signer.pushKV("available", false);
                signer.pushKV("state", "wallet_has_no_validator_key");
                obj.pushKV("signer", std::move(signer));
            }
            obj.pushKV("operator_recovery", FinalityRecoveryControlToJSON(pwallet->chain().finalityRecoveryControl(), vk));
            return obj;
        },
    };
}

RPCHelpMan setfinalityrecovery()
{
    return RPCHelpMan{
        "setfinalityrecovery",
        "Configure an explicit operator-trusted, one-time orphan-lock recovery for this wallet's\n"
        "existing validator. Requires an unlocked wallet and stopped staking. The node checks the\n"
        "intact journal against the exact incident; missing/deleted journals cannot be recovered.\n"
        "This call only configures memory: it does not change the journal or sign. Run startstaking\n"
        "afterward; the signer rechecks the anchor, epoch, sets and depth before moving the lock\n"
        "forward, preserving the old vote. The anchor must be in the incident epoch, which must\n"
        "still be current or immediately previous. On mainnet subsequent checkpoints wait 20 blocks.\n"
        "WARNING: the manifest is operator trust, not a quorum proof. Old signatures remain valid\n"
        "and this exception can weaken finality safety. Independently agree the exact incident and\n"
        "anchor before accepting it. Twenty blocks does not eliminate reorg risk. No default anchor\n"
        "is chosen. Configuration is lost on restart; any startup recovery options are unchanged.\n",
        {
            {"manifest", RPCArg::Type::OBJ, RPCArg::Optional::NO, "public incident and agreed anchor; exact fields only, no private keys or file paths",
             {
                 {"version", RPCArg::Type::NUM, RPCArg::Optional::NO, "integer 1"},
                 {"chain_domain", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "64 hex characters, B3 display order"},
                 {"validator_key", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "this wallet's exact 32-byte x-only public key, hex"},
                 {"incident_height", RPCArg::Type::NUM, RPCArg::Optional::NO, "existing orphan vote and lock height"},
                 {"incident_block_hash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "exact orphan hash, B3 display order"},
                 {"incident_epoch", RPCArg::Type::NUM, RPCArg::Optional::NO, "exact journal epoch"},
                 {"incident_signing_set_hash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "exact locked signing set, B3 display order"},
                 {"incident_successor_set_hash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "exact locked successor set, B3 display order"},
                 {"anchor_height", RPCArg::Type::NUM, RPCArg::Optional::NO, "agreed scheduled checkpoint strictly above the incident in that same retained epoch"},
                 {"anchor_block_hash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "agreed active-chain checkpoint hash, B3 display order"},
             }},
            {"accepted_anchor", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "repeat the exact 64-hex anchor_block_hash to explicitly accept this trust exception"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "configuration only, never an assertion of recovered signing",
                  {
                      {RPCResult::Type::BOOL, "configured", "true when the pending plan was accepted"},
                      {RPCResult::Type::BOOL, "recovered", "always false: this RPC never applies recovery"},
                      {RPCResult::Type::STR, "state", "configured_for_next_startstaking"},
                      {RPCResult::Type::BOOL, "journal_unchanged", "always true"},
                      {RPCResult::Type::STR_HEX, "validator_key", "the targeted public validator"},
                      {RPCResult::Type::NUM, "anchor_height", "the approved anchor height"},
                      {RPCResult::Type::STR_HEX, "anchor_block_hash", "the approved anchor hash"},
                  }},
        RPCExamples{HelpExampleCli("setfinalityrecovery", "'<manifest JSON>' '<accepted anchor hash>'")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> pwallet{GetWalletForJSONRPCRequest(request)};
            if (!pwallet) return UniValue::VNULL;
            LOCK(pwallet->cs_wallet);
            EnsureWalletIsUnlocked(*pwallet);
            const auto validator{pwallet->GetValidatorPubKey()};
            if (!validator) throw JSONRPCError(RPC_WALLET_ERROR, "This wallet has no existing validator key; recovery does not create one");
            const auto vk{XOnlyBytes(*validator)};
            std::string error;
            Consensus::FinalitySignerRecovery plan;
            if (!ParseWalletFinalityRecoveryManifest(request.params[0], request.params[1], vk, plan, error)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, error);
            }
            if (!CheckWalletFinalityRecoveryControl(pwallet->chain().finalityRecoveryControl(), vk, error) ||
                !pwallet->chain().setFinalityRecovery(vk, plan, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            UniValue obj{UniValue::VOBJ};
            obj.pushKV("configured", true);
            obj.pushKV("recovered", false);
            obj.pushKV("state", "configured_for_next_startstaking");
            obj.pushKV("journal_unchanged", true);
            obj.pushKV("validator_key", HexStr(vk));
            obj.pushKV("anchor_height", plan.anchor_height);
            obj.pushKV("anchor_block_hash", plan.anchor_block_hash.GetHex());
            return obj;
        },
    };
}

RPCHelpMan clearfinalityrecovery()
{
    return RPCHelpMan{
        "clearfinalityrecovery",
        "Clear this wallet's pending operator-recovery configuration from node memory. Requires an\n"
        "unlocked wallet with an existing validator key and stopped staking. Refuses to clear another\n"
        "validator's plan. This does not undo an applied recovery, rewind or erase a journal, or remove\n"
        "startup recovery options. If options remain in the configuration file they are read at restart.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
                  {
                      {RPCResult::Type::BOOL, "configured", "false after successful clearing"},
                      {RPCResult::Type::BOOL, "journal_unchanged", "always true"},
                      {RPCResult::Type::BOOL, "startup_options_unchanged", "always true; this RPC never edits configuration files"},
                  }},
        RPCExamples{HelpExampleCli("clearfinalityrecovery", "") + HelpExampleRpc("clearfinalityrecovery", "")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> pwallet{GetWalletForJSONRPCRequest(request)};
            if (!pwallet) return UniValue::VNULL;
            LOCK(pwallet->cs_wallet);
            EnsureWalletIsUnlocked(*pwallet);
            const auto validator{pwallet->GetValidatorPubKey()};
            if (!validator) throw JSONRPCError(RPC_WALLET_ERROR, "This wallet has no existing validator key; recovery does not create one");
            const auto vk{XOnlyBytes(*validator)};
            std::string error;
            if (!CheckWalletFinalityRecoveryControl(pwallet->chain().finalityRecoveryControl(), vk, error) ||
                !pwallet->chain().clearFinalityRecovery(vk, error)) {
                throw JSONRPCError(RPC_WALLET_ERROR, error);
            }
            UniValue obj{UniValue::VOBJ};
            obj.pushKV("configured", false);
            obj.pushKV("journal_unchanged", true);
            obj.pushKV("startup_options_unchanged", true);
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
                {RPCResult::Type::BOOL, "finality_signing", "whether at least one matching BLS key was loaded; getfinalityinfo reports the signer's actual progress and errors"},
                {RPCResult::Type::STR, "finality_note", /*optional=*/true, "binding or snapshot keys that could not be resolved, including unavailable previous or next keys"},
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

            // Resolve the snapshot keys before entering the node. The
            // node installs them atomically with this validator key so concurrent
            // calls from different wallets cannot cross their signing keys.
            const interfaces::FinalityStatus fstatus{pwallet->chain().finalityStatus(XOnlyBytes(*validator))};
            std::vector<std::string> finality_notes;
            const auto finality_keys{pwallet->ResolveFinalitySigningKeys(fstatus, finality_notes)};

            std::string error;
            if (!pwallet->chain().startStaking(*secret, GetScriptForDestination(*rewards),
                                               finality_keys, error)) {
                throw JSONRPCError(RPC_MISC_ERROR, error);
            }
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("running", true);
            obj.pushKV("validator_key", HexStr(XOnlyBytes(*validator)));
            obj.pushKV("rewards_address", EncodeDestination(*rewards));
            obj.pushKV("finality_signing", !finality_keys.empty());
            if (!finality_notes.empty()) obj.pushKV("finality_note", util::Join(finality_notes, "; "));
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
