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
