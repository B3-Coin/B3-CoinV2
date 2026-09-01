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
        "The BLS key is derived deterministically from the wallet's validator identity key and the binding\n"
        "sequence, so nothing new is stored and a restored wallet re-derives it. The transaction carries the\n"
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
            const uint32_t seq{ctx.status.bound ? ctx.status.binding_seq + 1 : 0};
            if (ctx.status.bound && ctx.status.binding_seq == std::numeric_limits<uint32_t>::max()) {
                throw JSONRPCError(RPC_MISC_ERROR, "The binding sequence is exhausted");
            }
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
            const auto identity{pwallet->GetValidatorSecret()};
            if (!identity) throw JSONRPCError(RPC_WALLET_ERROR, util::ErrorString(identity).original);
            return SubmitFinalityKeyTx(*pwallet, *identity, ctx.vk, /*bls_key=*/nullptr, ctx.status.binding_seq + 1,
                                       ctx.status.chain_domain, "revoke");
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
        "is never returned.\n",
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
                           {RPCResult::Type::NUM, "weight", "this validator's weight (whole modern B3)"},
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
                           {RPCResult::Type::BOOL, "armed", "a BLS key is loaded in the staking loop"},
                           {RPCResult::Type::NUM, "last_signed_height", ""},
                           {RPCResult::Type::NUM, "pool_checkpoints", "checkpoints tracked by the local signature pool"},
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
                const uint32_t next_seq{st.bound ? st.binding_seq + 1 : 0};
                if (const auto next{pwallet->ResolveFinalityBlsKey(next_seq, /*bound_bls_pubkey=*/nullptr)}) {
                    binding.pushKV("next_bls_pubkey", HexStr(next->GetPublicKey().Compressed()));
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
            obj.pushKV("signing", signing);
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
                {RPCResult::Type::BOOL, "finality_signing", "whether the finality signer was armed with this validator's bound BLS key"},
                {RPCResult::Type::STR, "finality_note", /*optional=*/true, "why the finality signer is not armed"},
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

            // Resolve the optional finality key before entering the node. The
            // node installs it atomically with this validator key so concurrent
            // calls from different wallets cannot cross their signing keys.
            std::optional<bls::SecretKey> finality_key;
            std::string finality_note;
            {
                const interfaces::FinalityStatus fstatus{pwallet->chain().finalityStatus(XOnlyBytes(*validator))};
                if (fstatus.configured && fstatus.bound && !fstatus.revoked) {
                    // Derived or imported: whichever wallet key matches the binding.
                    const auto bls_key{pwallet->ResolveFinalityBlsKey(fstatus.binding_seq, &fstatus.binding_bls_pubkey)};
                    if (bls_key) {
                        finality_key = *bls_key;
                    } else {
                        finality_note = util::ErrorString(bls_key).original;
                    }
                } else if (fstatus.configured) {
                    finality_note = fstatus.revoked ? "the FINALITY_KEY binding is revoked"
                                                    : "no FINALITY_KEY binding (bindfinalitykey)";
                }
            }

            std::string error;
            if (!pwallet->chain().startStaking(*secret, GetScriptForDestination(*rewards),
                                               finality_key, error)) {
                throw JSONRPCError(RPC_MISC_ERROR, error);
            }
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("running", true);
            obj.pushKV("validator_key", HexStr(XOnlyBytes(*validator)));
            obj.pushKV("rewards_address", EncodeDestination(*rewards));
            obj.pushKV("finality_signing", finality_key.has_value());
            if (!finality_note.empty()) obj.pushKV("finality_note", finality_note);
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
