// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chainparams.h>
#include <bridge/proof.h>
#include <common/messages.h>
#include <consensus/era.h>
#include <core_io.h>
#include <flowmesh/auth.h>
#include <flowmesh/market.h>
#include <flowmesh/seat_id.h>
#include <key_io.h>
#include <modern/asset.h>
#include <modern/asset_output.h>
#include <modern/asset_validation.h>
#include <modern/bridge_asset.h>
#include <modern/bridge_binding.h>
#include <modern/chain_domain.h>
#include <modern/fn_pod.h>
#include <modern/flowmesh_seat.h>
#include <random.h>
#include <node/bridge_state.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <rpc/util.h>
#include <sync.h>
#include <util/check.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <support/cleanse.h>
#include <wallet/coincontrol.h>
#include <wallet/receive.h>
#include <wallet/rpc/flowmesh.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <univalue.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wallet {

FlowMeshDepositAdmission CheckFlowMeshDepositAdmission(
    const bool market_bootstrap, const bool flowmesh_rules_active,
    const bool market_established, const bool base_asset_deposit,
    const bool runtime_ready, const bool runtime_paused)
{
    if (market_bootstrap) {
        if (market_established) {
            return FlowMeshDepositAdmission::BOOTSTRAP_MARKET_ALREADY_ESTABLISHED;
        }
        if (!base_asset_deposit) {
            return FlowMeshDepositAdmission::BOOTSTRAP_REQUIRES_BASE_ASSET;
        }
        return FlowMeshDepositAdmission::MARKET_BOOTSTRAP;
    }
    if (!flowmesh_rules_active) {
        return FlowMeshDepositAdmission::RULES_INACTIVE;
    }
    if (!market_established) {
        return FlowMeshDepositAdmission::MARKET_NOT_ESTABLISHED;
    }
    if (!runtime_ready) {
        return FlowMeshDepositAdmission::RUNTIME_UNAVAILABLE;
    }
    if (runtime_paused) {
        return FlowMeshDepositAdmission::MARKET_PAUSED;
    }
    return FlowMeshDepositAdmission::USER_DEPOSIT;
}

namespace {

struct AssetRpcOptions {
    bool broadcast{true};
    bool market_bootstrap{false};
    CCoinControl coin_control{};
};

struct FlowMeshSeatRpcOptions {
    AssetRpcOptions transaction;
    std::optional<COutPoint> fn_outpoint;
    std::optional<std::array<unsigned char, bls::PUBKEY_SIZE>> bls_pubkey;
};

struct ChainSnapshot {
    uint256 tip_hash{};
    int next_height{0};
    std::optional<uint32_t> fn_issued_before{};
    bool pending_fn_pod{false};
};

struct ParsedBridgePayload {
    bridge::BridgeRecordV1 decoded{};
    CMpaRecord record{};
};

struct WalletAssetCoin {
    COutPoint outpoint{};
    modern::ModernOutput output{};
};

static const char* BridgeRecordKindName(
    const bridge::BridgeRecordKindV1 kind)
{
    switch (kind) {
    case bridge::BridgeRecordKindV1::BOOTSTRAP:
        return "bootstrap";
    case bridge::BridgeRecordKindV1::UPDATE:
        return "update";
    case bridge::BridgeRecordKindV1::MINT:
        return "mint";
    case bridge::BridgeRecordKindV1::EXECUTION_BACKFILL:
        return "execution-backfill";
    case bridge::BridgeRecordKindV1::MANAGED_WITHDRAWAL:
        return "managed-withdrawal";
    case bridge::BridgeRecordKindV1::BRIDGE_BURN:
        return "bridge-burn";
    }
    return "unknown";
}

static ParsedBridgePayload ParseCanonicalBridgePayload(const UniValue& value)
{
    const std::vector<unsigned char> payload{
        ParseHexV(value, "bridge_payload")};
    const auto decoded{bridge::DecodeBridgeRecordV1(payload)};
    if (!decoded) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "bridge_payload is not a canonical type-10 v1 payload");
    }
    const auto canonical{bridge::EncodeBridgeRecordV1(*decoded)};
    if (!canonical || *canonical != payload) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "bridge_payload has a non-canonical encoding");
    }
    const auto record{bridge::MakeBridgeMpaRecord(*decoded)};
    if (!record) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "bridge_payload cannot be encoded as type-10 v1");
    }
    return ParsedBridgePayload{*decoded, *record};
}

static bridge::EthAddress ParseEthereumRecipient(const UniValue& value)
{
    if (!value.isStr()) {
        throw JSONRPCError(RPC_TYPE_ERROR,
                           "ethereum_recipient must be a hex address");
    }
    std::string encoded{value.get_str()};
    if (encoded.starts_with("0x") || encoded.starts_with("0X")) {
        encoded.erase(0, 2);
    }
    if (encoded.size() != 40 || !IsHex(encoded)) {
        throw JSONRPCError(
            RPC_INVALID_ADDRESS_OR_KEY,
            "ethereum_recipient must be exactly 20 bytes of hexadecimal");
    }
    const std::vector<unsigned char> bytes{ParseHex(encoded)};
    bridge::EthAddress address{};
    std::copy(bytes.begin(), bytes.end(), address.begin());
    if (bridge::EthAddressIsNull(address)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           "ethereum_recipient cannot be the zero address");
    }
    return address;
}

static CAmount AssetUnitsFromValue(const UniValue& value, const std::string& name)
{
    if (!value.isNum()) {
        throw JSONRPCError(RPC_TYPE_ERROR, name + " must be an integer number of asset units");
    }
    try {
        const int64_t units{value.getInt<int64_t>()};
        if (units < 1 || units > MAX_MONEY) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               name + " must be in the range [1, MAX_MONEY]");
        }
        return units;
    } catch (const UniValue::type_error&) {
        throw JSONRPCError(RPC_TYPE_ERROR,
                           name + " must be an integer number of asset units");
    }
}

static uint8_t AssetDecimalsFromValue(const UniValue& value)
{
    if (!value.isNum()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "decimals must be an integer");
    }
    try {
        const int decimals{value.getInt<int>()};
        if (decimals < 0 || decimals > modern::ASSET_MAX_DECIMALS) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                strprintf("decimals must be in the range [0, %u]",
                          modern::ASSET_MAX_DECIMALS));
        }
        return static_cast<uint8_t>(decimals);
    } catch (const UniValue::type_error&) {
        throw JSONRPCError(RPC_TYPE_ERROR, "decimals must be an integer");
    }
}

static AssetRpcOptions ParseAssetRpcOptions(const UniValue& value,
                                            const bool flowmesh_seat = false,
                                            const bool flowmesh_deposit = false)
{
    AssetRpcOptions parsed;
    // Asset and FN inputs are confirmed by default. A caller may explicitly
    // opt into trusted unconfirmed wallet change with minconf=0.
    parsed.coin_control.m_min_depth = 1;

    if (value.isNull()) return parsed;
    if (!value.isObject()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "options must be an object");
    }

    std::set<std::string> allowed{
        "broadcast", "fee_rate", "replaceable", "minconf", "include_unsafe"};
    if (flowmesh_seat) {
        allowed.insert("fn_txid");
        allowed.insert("fn_vout");
        allowed.insert("bls_pubkey");
    }
    if (flowmesh_deposit) allowed.insert("market_bootstrap");
    for (const std::string& key : value.getKeys()) {
        if (!allowed.contains(key)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               strprintf("Unknown option '%s'", key));
        }
    }

    if (value.exists("broadcast")) parsed.broadcast = value["broadcast"].get_bool();
    if (value.exists("market_bootstrap")) {
        parsed.market_bootstrap = value["market_bootstrap"].get_bool();
    }
    if (value.exists("replaceable")) {
        parsed.coin_control.m_signal_bip125_rbf = value["replaceable"].get_bool();
    }
    if (value.exists("include_unsafe")) {
        parsed.coin_control.m_include_unsafe_inputs = value["include_unsafe"].get_bool();
    }
    if (value.exists("minconf")) {
        const int minconf{value["minconf"].getInt<int>()};
        if (minconf < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "minconf cannot be negative");
        }
        parsed.coin_control.m_min_depth = minconf;
    }
    if (value.exists("fee_rate")) {
        // Match send/walletcreatefundedpsbt: the JSON value is atomic units
        // per vB, represented internally as atomic units per kvB.
        parsed.coin_control.m_feerate =
            CFeeRate{AmountFromValue(value["fee_rate"], /*decimals=*/3)};
        // Keep the wallet and relay minimum-fee checks in force. An explicit
        // rate chooses the price; it does not opt this safety-oriented RPC
        // out of local policy.
        parsed.coin_control.fOverrideFeeRate = false;
    }
    return parsed;
}

static FlowMeshSeatRpcOptions ParseFlowMeshSeatRpcOptions(
    const UniValue& value)
{
    FlowMeshSeatRpcOptions out;
    out.transaction = ParseAssetRpcOptions(value, /*flowmesh_seat=*/true);
    if (value.isNull()) return out;

    const bool has_txid{value.exists("fn_txid")};
    const bool has_vout{value.exists("fn_vout")};
    if (has_txid != has_vout) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "fn_txid and fn_vout must be provided together");
    }
    if (has_txid) {
        const Txid txid{Txid::FromUint256(ParseHashV(value["fn_txid"],
                                                     "fn_txid"))};
        const int64_t vout{value["fn_vout"].getInt<int64_t>()};
        if (vout < 0 || vout > std::numeric_limits<uint32_t>::max()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "fn_vout is outside the uint32 range");
        }
        out.fn_outpoint = COutPoint{txid, static_cast<uint32_t>(vout)};
    }
    if (value.exists("bls_pubkey")) {
        const std::vector<unsigned char> bytes{
            ParseHexV(value["bls_pubkey"], "bls_pubkey")};
        if (bytes.size() != bls::PUBKEY_SIZE) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "bls_pubkey must be exactly 48 bytes");
        }
        const auto decoded{bls::PublicKey::Decode(bytes)};
        if (!decoded) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "bls_pubkey is not a canonical BLS public key");
        }
        out.bls_pubkey = decoded->Compressed();
    }
    return out;
}

static CScript GetOwnerScript(CWallet& wallet, const UniValue& address,
                              const std::string& label, std::string& encoded)
    EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    CTxDestination destination;
    if (address.isNull()) {
        const auto generated{
            wallet.GetNewDestination(wallet.m_default_address_type, label)};
        if (!generated) {
            throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT,
                               util::ErrorString(generated).original);
        }
        destination = *generated;
    } else {
        destination = DecodeDestination(address.get_str());
        if (!IsValidDestination(destination)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                               "Invalid owner address");
        }
    }
    encoded = EncodeDestination(destination);
    const CScript owner_script{GetScriptForDestination(destination)};
    if (ScriptRequiresInactiveB3Witness(wallet, owner_script)) {
        throw JSONRPCError(
            RPC_INVALID_ADDRESS_OR_KEY,
            "B3 witness addresses are not active in this release; use a legacy owner address");
    }
    return owner_script;
}

static ChainSnapshot SnapshotChainState(CWallet& wallet,
                                        const bool inspect_fn_pool)
{
    std::string error;
    const auto node_snapshot{
        wallet.chain().modernCreationSnapshot(inspect_fn_pool, error)};
    if (!node_snapshot) {
        throw JSONRPCError(RPC_MISC_ERROR,
                           error.empty() ? "Modern chain snapshot is unavailable"
                                         : error);
    }
    ChainSnapshot snapshot{node_snapshot->tip_hash,
                           node_snapshot->next_height,
                           node_snapshot->fn_issued_before,
                           node_snapshot->pending_fn_pod};

    if (!inspect_fn_pool) return snapshot;
    const Consensus::Params& params{Params().GetConsensus()};
    if (!Consensus::FnPodRulesActive(snapshot.next_height, params)) {
        throw JSONRPCError(RPC_MISC_ERROR,
                           "Modern FN PoD creation is not active for the next block");
    }
    if (!snapshot.fn_issued_before) {
        throw JSONRPCError(RPC_DATABASE_ERROR,
                           "The branch-local FN issuance counter is unavailable");
    }

    return snapshot;
}

static void RecheckChainSnapshot(CWallet& wallet,
                                 const ChainSnapshot& expected,
                                 const bool inspect_fn_pool)
{
    const ChainSnapshot current{SnapshotChainState(wallet, inspect_fn_pool)};
    if (current.tip_hash != expected.tip_hash ||
        current.next_height != expected.next_height ||
        current.fn_issued_before != expected.fn_issued_before) {
        throw JSONRPCError(RPC_VERIFY_REJECTED,
                           "The chain tip or FN slot changed while the transaction was being built; retry");
    }
    if (inspect_fn_pool && current.pending_fn_pod) {
        throw JSONRPCError(RPC_VERIFY_REJECTED,
                           "A modern FN creation for the current slot is already pending");
    }
}

static node::BridgeTxAuthorization PrevalidateBridgeTransaction(
    CWallet& wallet, const CTransaction& tx, const ChainSnapshot& snapshot)
{
    node::BridgeTxAuthorization authorization;
    std::string error;
    const interfaces::BridgePrevalidationResult result{
        wallet.chain().prevalidateBridgeTransaction(
            tx, snapshot.tip_hash, snapshot.next_height, authorization, error)};
    switch (result) {
    case interfaces::BridgePrevalidationResult::VALID:
        return authorization;
    case interfaces::BridgePrevalidationResult::TIP_CHANGED:
    case interfaces::BridgePrevalidationResult::REJECTED:
        throw JSONRPCError(RPC_VERIFY_REJECTED, error);
    case interfaces::BridgePrevalidationResult::RULES_INACTIVE:
        throw JSONRPCError(RPC_MISC_ERROR, error);
    case interfaces::BridgePrevalidationResult::STATE_UNAVAILABLE:
        throw JSONRPCError(RPC_DATABASE_ERROR, error);
    }
    NONFATAL_UNREACHABLE();
}

static std::vector<WalletAssetCoin> AvailableWalletAssetCoins(
    const CWallet& wallet, const modern::AssetId& asset,
    const uint16_t required_policy, const CCoinControl& coin_control,
    CAmount* unavailable_witness_amount = nullptr)
    EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    std::vector<WalletAssetCoin> result;

    for (const auto& [outpoint, txo] : wallet.GetTXOs()) {
        const CWalletTx& wtx{txo.GetWalletTx()};
        const CTxOut& txout{txo.GetTxOut()};
        const int depth{wallet.GetTxDepthInMainChain(wtx)};
        if (depth < coin_control.m_min_depth || depth < 0) continue;
        if (depth == 0) {
            if (!wtx.InMempool()) continue;
            if (!coin_control.m_include_unsafe_inputs &&
                !CachedTxIsTrusted(wallet, wtx)) {
                continue;
            }
        }
        if (wallet.IsTxImmatureCoinBase(wtx) || wallet.IsLockedCoin(outpoint) ||
            wallet.IsSpent(outpoint)) {
            continue;
        }

        if (AssetSigningContextForWalletTransaction(wtx) !=
            AssetSigningContext::OWNER_SUFFIX) {
            continue;
        }

        std::string parse_error;
        const auto parsed{modern::ParseAssetOutput(txout, parse_error)};
        if (!parsed || parsed->asset != asset ||
            parsed->policy_type != required_policy) {
            continue;
        }
        if (!wallet.IsMine(txout, AssetSigningContext::OWNER_SUFFIX)) continue;
        const std::optional<CScript> owner_script{
            modern::AssetOwnerScript(txout)};
        if (!owner_script) continue;
        if (ScriptRequiresInactiveB3Witness(wallet, *owner_script)) {
            if (unavailable_witness_amount) {
                if (*unavailable_witness_amount >
                    MAX_MONEY - parsed->amount) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "Wallet asset balance exceeds the supported range");
                }
                *unavailable_witness_amount += parsed->amount;
            }
            continue;
        }
        result.push_back(WalletAssetCoin{outpoint, *parsed});
    }

    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.output.amount != b.output.amount) {
            return a.output.amount < b.output.amount;
        }
        return a.outpoint < b.outpoint;
    });
    return result;
}

static CAmount SelectAssetInputs(CWallet& wallet, const modern::AssetId& asset,
                                 const uint16_t policy, const CAmount target,
                                 CCoinControl& coin_control)
    EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    CAmount unavailable_witness_amount{0};
    const auto coins{AvailableWalletAssetCoins(
        wallet, asset, policy, coin_control, &unavailable_witness_amount)};

    // Prefer the smallest single UTXO which covers the send, otherwise use
    // the largest values first to minimize input count.
    for (const WalletAssetCoin& coin : coins) {
        if (coin.output.amount >= target) {
            coin_control.Select(coin.outpoint);
            return coin.output.amount;
        }
    }

    CAmount selected{0};
    for (auto it{coins.rbegin()}; it != coins.rend() && selected < target; ++it) {
        if (selected > MAX_MONEY - it->output.amount) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "Wallet asset balance exceeds the supported range");
        }
        coin_control.Select(it->outpoint);
        selected += it->output.amount;
    }
    if (selected < target) {
        if (unavailable_witness_amount >= target - selected) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "The wallet has enough asset units only in witness-owned outputs. B3 witness addresses are not active in this release; use legacy-owned asset outputs");
        }
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                           "Insufficient confirmed asset balance");
    }
    return selected;
}

static WalletAssetCoin SelectFlowMeshSeatFn(
    CWallet& wallet, const modern::AssetId& fn_asset,
    const FlowMeshSeatRpcOptions& options, CCoinControl& coin_control)
    EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    std::vector<WalletAssetCoin> coins{AvailableWalletAssetCoins(
        wallet, fn_asset, static_cast<uint16_t>(modern::PolicyType::FN),
        coin_control)};
    coins.erase(std::remove_if(coins.begin(), coins.end(), [](const auto& coin) {
                    return coin.output.amount != 1 ||
                           (coin.output.policy_version !=
                                modern::POLICY_VERSION_V1 &&
                            coin.output.policy_version !=
                                modern::FN_SEAT_POLICY_VERSION_V2);
                }),
                coins.end());

    if (options.fn_outpoint) {
        const auto it{std::find_if(coins.begin(), coins.end(),
                                   [&](const auto& coin) {
                                       return coin.outpoint ==
                                              *options.fn_outpoint;
                                   })};
        if (it == coins.end()) {
            throw JSONRPCError(
                RPC_WALLET_ERROR,
                "The requested FN output is unavailable, unconfirmed, locked, or not spendable by this wallet");
        }
        coin_control.Select(it->outpoint);
        return *it;
    }

    // Preserve already-active seats by default. An explicit outpoint is
    // required to rotate/rebind FN-v2; automatic selection takes the oldest
    // ordinary FN-v1 output.
    const auto it{std::find_if(coins.begin(), coins.end(), [](const auto& coin) {
        return coin.output.policy_version == modern::POLICY_VERSION_V1;
    })};
    if (it == coins.end()) {
        throw JSONRPCError(
            RPC_WALLET_INSUFFICIENT_FUNDS,
            "No confirmed spendable unbound FN Coin is available; specify an FN-v2 outpoint explicitly to rotate an existing seat");
    }
    coin_control.Select(it->outpoint);
    return *it;
}

static bls::SecretKey GenerateFlowMeshSeatKey()
{
    CKeyingMaterial ikm(32);
    GetStrongRandBytes(ikm);
    const auto key{bls::SecretKey::FromIKM(
        std::span<const unsigned char>{ikm.data(), ikm.size()})};
    memory_cleanse(ikm.data(), ikm.size());
    if (!key) {
        throw JSONRPCError(RPC_INTERNAL_ERROR,
                           "Unable to generate a FlowMesh BLS key");
    }
    return *key;
}

static COutPoint SelectIssuanceAnchor(CWallet& wallet,
                                      CCoinControl& coin_control)
    EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    const CoinsResult available{AvailableCoins(wallet, &coin_control)};
    auto coins{available.All()};
    std::sort(coins.begin(), coins.end(), [](const COutput& a, const COutput& b) {
        return a.outpoint < b.outpoint;
    });
    for (const COutput& coin : coins) {
        if (!coin.solvable || coin.txout.nValue <= 0) continue;
        coin_control.Select(coin.outpoint);
        return coin.outpoint;
    }
    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                       "No confirmed native B3 input is available as the issuance anchor");
}

static CRecipient AssetRecipient(const CTxOut& output)
{
    return CRecipient{CNoDestination{output.scriptPubKey}, output.nValue,
                      /*fSubtractFeeFromAmount=*/false};
}

static CMpaRecord AssetIssuanceRecord(const modern::AssetGenesisV1& genesis)
{
    const modern::CreationAction action{
        modern::MakeAssetIssuanceAction(genesis)};
    return CMpaRecord{action.action_type, action.action_version, action.payload};
}

static UniValue FinishAssetTransaction(
    const JSONRPCRequest& request, CWallet& wallet,
    const CreatedTransactionResult& created, const AssetRpcOptions& options,
    const ChainSnapshot& snapshot, const bool inspect_fn_pool,
    const CAmount disintegration, const std::string& kind)
{
    RecheckChainSnapshot(wallet, snapshot, inspect_fn_pool);
    const CTransactionRef& tx{created.tx};
    const std::string hex{EncodeHexTx(*tx)};
    if (options.broadcast) {
        wallet.CommitTransaction(tx,
                                 {{"b3", kind},
                                  {"b3_network_fee", FormatMoney(created.fee)},
                                  {"b3_disintegration", FormatMoney(disintegration)}},
                                 /*orderForm=*/{});
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("txid", tx->GetHash().GetHex());
    result.pushKV("ptxid", tx->GetPtxid().GetHex());
    result.pushKV("hex", hex);
    result.pushKV("network_fee", ValueFromAmount(created.fee));
    result.pushKV("disintegration", ValueFromAmount(disintegration));
    result.pushKV("broadcast", options.broadcast);
    return result;
}

static RPCResult AssetTransactionResult(
    std::string description, std::vector<RPCResult> asset_fields)
{
    std::vector<RPCResult> fields;
    fields.reserve(6 + asset_fields.size());
    fields.emplace_back(RPCResult::Type::STR_HEX, "txid", "Transaction id");
    fields.emplace_back(RPCResult::Type::STR_HEX, "ptxid", "Payload-aware transaction id");
    fields.emplace_back(RPCResult::Type::STR_HEX, "hex", "Serialized transaction");
    fields.emplace_back(RPCResult::Type::STR_AMOUNT, "network_fee", "Native B3 network fee");
    fields.emplace_back(RPCResult::Type::STR_AMOUNT, "disintegration", "Native B3 amount permanently destroyed");
    fields.emplace_back(RPCResult::Type::BOOL, "broadcast", "Whether the transaction was broadcast and added to the wallet");
    for (RPCResult& field : asset_fields) {
        fields.push_back(std::move(field));
    }
    return RPCResult{RPCResult::Type::OBJ, "", std::move(description),
                     std::move(fields)};
}

static void EnsureSigningWallet(const CWallet& wallet)
{
    EnsureWalletIsUnlocked(wallet);
    if (wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
        throw JSONRPCError(RPC_WALLET_ERROR,
                           "Private keys are disabled for this wallet");
    }
}

static modern::AssetId ParseAssetId(const UniValue& value)
{
    const modern::AssetId asset{ParseHashV(value, "asset_id")};
    if (asset == modern::NativeAsset()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "asset_id cannot be the native B3 asset");
    }
    return asset;
}

static modern::AssetId ParseAssetOrNative(const UniValue& value,
                                          const std::string& name)
{
    if (!value.isStr()) {
        throw JSONRPCError(RPC_TYPE_ERROR,
                           name + " must be an asset id or 'B3'");
    }
    const std::string text{value.get_str()};
    if (text == "B3" || text == "b3" || text == "native") {
        return modern::NativeAsset();
    }
    return ParseHashV(value, name);
}

static CAmount FlowMeshDepositAmount(const UniValue& value,
                                     const modern::AssetId& asset)
{
    if (asset == modern::NativeAsset()) {
        const CAmount amount{AmountFromValue(value)};
        if (amount <= 0 || amount > MAX_MONEY) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "native deposit amount is out of range");
        }
        return amount;
    }
    return AssetUnitsFromValue(value, "amount");
}

static uint16_t AssetOwnerPolicy(const modern::AssetId& asset,
                                 const Consensus::Params& params,
                                 const int next_height)
{
    const auto fn_asset{modern::ConfiguredFnAssetId(params)};
    if (fn_asset && asset == *fn_asset) {
        if (!Consensus::FnRulesActive(next_height, params)) {
            throw JSONRPCError(RPC_MISC_ERROR,
                               "FN transfers are not active for the next block");
        }
        return static_cast<uint16_t>(modern::PolicyType::FN);
    }
    if (!Consensus::AssetRulesActive(next_height, params)) {
        throw JSONRPCError(RPC_MISC_ERROR,
                           "Colored assets are not active for the next block");
    }
    return static_cast<uint16_t>(modern::PolicyType::OWNER);
}

static const Consensus::BridgeAssetParams& RequireBridgeForNextBlock(
    const int next_height)
{
    const Consensus::Params& params{Params().GetConsensus()};
    if (!params.busd_bridge ||
        !Consensus::BridgeMintParamsReady(*params.busd_bridge)) {
        throw JSONRPCError(
            RPC_MISC_ERROR,
            "The bridge is disabled because its production safety pins are incomplete");
    }
    if (!Consensus::BridgeRulesActive(next_height, params)) {
        throw JSONRPCError(RPC_MISC_ERROR,
                           "Bridge rules are not active for the next block");
    }
    return *params.busd_bridge;
}

static CRecipient BridgeBindingRecipient(const CMpaRecord& record)
{
    const auto output{modern::MakeBridgeBindingOutput(record)};
    if (!output) {
        throw JSONRPCError(RPC_INTERNAL_ERROR,
                           "Unable to construct the signed bridge-record binding cell");
    }
    return AssetRecipient(*output);
}

static CRecipient BridgeSelfPayment(CWallet& wallet,
                                    const std::string& label)
    EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    const auto destination{
        wallet.GetNewDestination(wallet.m_default_address_type, label)};
    if (!destination) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT,
                           util::ErrorString(destination).original);
    }
    const CScript script{GetScriptForDestination(*destination)};
    const CAmount amount{std::max<CAmount>(
        1, GetDustThreshold(CTxOut{0, script},
                            wallet.chain().relayDustFee()))};
    return CRecipient{*destination, amount,
                      /*fSubtractFeeFromAmount=*/false};
}

} // namespace

RPCHelpMan issueasset()
{
    return RPCHelpMan{
        "issueasset",
        "Issue the complete fixed supply of a new simple-v1 colored asset. "
        "The first selected native input permanently anchors its chain-bound asset id.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"max_supply", RPCArg::Type::NUM, RPCArg::Optional::NO,
             "Full fixed supply in integer asset units"},
            {"decimals", RPCArg::Type::NUM, RPCArg::Optional::NO,
             "Display decimals (0-18); amounts remain integer units"},
            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
             "Owner address (default: a new wallet address)"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED,
             "Transaction options", {
                 {"broadcast", RPCArg::Type::BOOL, RPCArg::Default{true},
                  "Broadcast and add to this wallet"},
                 {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED,
                  "Fee rate in atomic units per vB"},
                 {"replaceable", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED,
                  "Signal BIP125 replaceability"},
                 {"minconf", RPCArg::Type::NUM, RPCArg::Default{1},
                  "Minimum confirmations for selected inputs"},
                 {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false},
                  "Allow unsafe wallet inputs"},
             }},
        },
        AssetTransactionResult("Created transaction", {
            {RPCResult::Type::STR_HEX, "asset_id", "New chain-bound asset id"},
            {RPCResult::Type::NUM, "max_supply", "Full fixed supply in integer asset units"},
            {RPCResult::Type::NUM, "decimals", "Display decimals"},
            {RPCResult::Type::STR, "owner_address", "Address owning the issued supply"},
            {RPCResult::Type::STR_AMOUNT, "treasury_fee", "Native B3 issuance fee paid to the treasury"},
        }),
        RPCExamples{HelpExampleCli("issueasset", "1000000 2")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();

            const CAmount max_supply{
                AssetUnitsFromValue(request.params[0], "max_supply")};
            const uint8_t decimals{AssetDecimalsFromValue(request.params[1])};
            const AssetRpcOptions options{
                ParseAssetRpcOptions(request.params[3])};
            const ChainSnapshot snapshot{SnapshotChainState(*wallet, false)};
            const Consensus::Params& params{Params().GetConsensus()};
            if (!Consensus::AssetRulesActive(snapshot.next_height, params)) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    "Colored-asset issuance is not active for the next block");
            }
            if (!params.modern_pos || params.modern_pos->treasury_script.empty()) {
                throw JSONRPCError(RPC_MISC_ERROR,
                                   "The asset treasury script is not configured");
            }
            const auto domain{modern::ModernChainDomain(
                params.hashGenesisBlock,
                params.legacy_final_hash.value_or(uint256{}))};
            if (!domain) {
                throw JSONRPCError(RPC_MISC_ERROR,
                                   "The modern chain domain is not pinned");
            }

            CreatedTransactionResult created{nullptr, 0, std::nullopt, {}};
            modern::AssetId asset;
            std::string owner_address;
            {
                LOCK(wallet->cs_wallet);
                EnsureSigningWallet(*wallet);
                CCoinControl coin_control{options.coin_control};
                // The issuance anchor must be confirmed and is forced to vin[0].
                coin_control.m_min_depth = std::max(1, coin_control.m_min_depth);
                const COutPoint anchor{
                    SelectIssuanceAnchor(*wallet, coin_control)};
                const modern::AssetGenesisV1 genesis{
                    .max_supply = static_cast<uint64_t>(max_supply),
                    .decimals = decimals,
                    .issuance_mode = modern::ASSET_ISSUANCE_MODE_GENESIS_FIXED,
                    .mode_params = {}};
                asset = modern::AssetIdV1(
                    *domain, anchor, modern::AssetGenesisCommitment(genesis));
                if (const auto fn_asset{modern::ConfiguredFnAssetId(params)};
                    asset == modern::NativeAsset() ||
                    (fn_asset && asset == *fn_asset)) {
                    throw JSONRPCError(RPC_VERIFY_ERROR,
                                       "Derived asset id collides with a reserved id");
                }

                const CScript owner{GetOwnerScript(
                    *wallet, request.params[2], "b3-asset-owner",
                    owner_address)};
                const auto minted{modern::MakeAssetOwnerOutput(
                    asset, max_supply, modern::PolicyType::OWNER, owner)};
                if (!minted) {
                    throw JSONRPCError(RPC_INTERNAL_ERROR,
                                       "Unable to encode the asset owner output");
                }
                const CTxOut treasury{
                    modern::ASSET_ISSUANCE_TREASURY_FEE,
                    CScript{params.modern_pos->treasury_script.begin(),
                            params.modern_pos->treasury_script.end()}};
                const std::vector<CRecipient> recipients{
                    AssetRecipient(*minted), AssetRecipient(treasury)};
                const ModernTransactionOptions modern_options{
                    .mpa = {AssetIssuanceRecord(genesis)},
                    .native_disintegration = 0};
                auto result{CreateTransaction(
                    *wallet, recipients,
                    /*change_pos=*/static_cast<unsigned int>(recipients.size()),
                    coin_control, /*sign=*/true, modern_options)};
                if (!result) {
                    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                                       util::ErrorString(result).original);
                }
                if (result->tx->vin.empty() ||
                    result->tx->vin[0].prevout != anchor) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "Wallet did not retain the selected issuance anchor at vin[0]");
                }
                created = std::move(*result);
            }

            UniValue result{FinishAssetTransaction(
                request, *wallet, created, options, snapshot, false,
                /*disintegration=*/0, "asset-issuance")};
            result.pushKV("asset_id", asset.GetHex());
            result.pushKV("max_supply", max_supply);
            result.pushKV("decimals", decimals);
            result.pushKV("owner_address", owner_address);
            result.pushKV("treasury_fee",
                          ValueFromAmount(
                              modern::ASSET_ISSUANCE_TREASURY_FEE));
            return result;
        }};
}

static UniValue TransferOrBurnAsset(const JSONRPCRequest& request,
                                    const bool burn)
{
    const std::shared_ptr<CWallet> wallet{GetWalletForJSONRPCRequest(request)};
    if (!wallet) return UniValue::VNULL;
    wallet->BlockUntilSyncedToCurrentChain();

    const modern::AssetId asset{ParseAssetId(request.params[0])};
    const CAmount amount{AssetUnitsFromValue(request.params[1], "amount")};
    const int options_index{burn ? 2 : 3};
    const AssetRpcOptions options{
        ParseAssetRpcOptions(request.params[options_index])};
    const ChainSnapshot snapshot{SnapshotChainState(*wallet, false)};
    const Consensus::Params& params{Params().GetConsensus()};
    const uint16_t owner_policy{
        AssetOwnerPolicy(asset, params, snapshot.next_height)};

    CreatedTransactionResult created{nullptr, 0, std::nullopt, {}};
    std::string owner_address;
    CAmount asset_change{0};
    {
        LOCK(wallet->cs_wallet);
        EnsureSigningWallet(*wallet);
        CCoinControl coin_control{options.coin_control};
        const CAmount selected{SelectAssetInputs(
            *wallet, asset, owner_policy, amount, coin_control)};
        asset_change = selected - amount;

        std::vector<CRecipient> recipients;
        if (burn) {
            const auto burned{modern::MakeAssetBurnOutput(asset, amount)};
            if (!burned) {
                throw JSONRPCError(RPC_INTERNAL_ERROR,
                                   "Unable to encode the asset burn output");
            }
            recipients.push_back(AssetRecipient(*burned));
        } else {
            const CScript owner{GetOwnerScript(
                *wallet, request.params[2], "b3-asset-receive",
                owner_address)};
            const auto sent{modern::MakeAssetOwnerOutput(
                asset, amount, owner_policy, owner)};
            if (!sent) {
                throw JSONRPCError(RPC_INTERNAL_ERROR,
                                   "Unable to encode the asset owner output");
            }
            recipients.push_back(AssetRecipient(*sent));
        }

        if (asset_change > 0) {
            std::string unused;
            const CScript change_owner{GetOwnerScript(
                *wallet, UniValue{}, "b3-asset-change", unused)};
            const auto change{modern::MakeAssetOwnerOutput(
                asset, asset_change, owner_policy, change_owner)};
            if (!change) {
                throw JSONRPCError(RPC_INTERNAL_ERROR,
                                   "Unable to encode the asset change output");
            }
            recipients.push_back(AssetRecipient(*change));
        }

        auto result{CreateTransaction(
            *wallet, recipients,
            /*change_pos=*/static_cast<unsigned int>(recipients.size()),
            coin_control, /*sign=*/true)};
        if (!result) {
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                               util::ErrorString(result).original);
        }
        created = std::move(*result);
    }

    UniValue result{FinishAssetTransaction(
        request, *wallet, created, options, snapshot, false,
        /*disintegration=*/0, burn ? "asset-burn" : "asset-transfer")};
    result.pushKV("asset_id", asset.GetHex());
    result.pushKV("amount", amount);
    result.pushKV("asset_change", asset_change);
    if (!burn) result.pushKV("owner_address", owner_address);
    return result;
}

RPCHelpMan sendasset()
{
    return RPCHelpMan{
        "sendasset",
        "Send integer units of a colored asset or FN Coin. Native B3 inputs "
        "pay the network fee; asset change returns to a fresh wallet owner.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"asset_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "32-byte chain-bound asset id"},
            {"amount", RPCArg::Type::NUM, RPCArg::Optional::NO,
             "Integer asset units"},
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO,
             "Recipient owner address"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED,
             "Transaction options", {
                 {"broadcast", RPCArg::Type::BOOL, RPCArg::Default{true},
                  "Broadcast and add to this wallet"},
                 {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED,
                  "Fee rate in atomic units per vB"},
                 {"replaceable", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED,
                  "Signal BIP125 replaceability"},
                 {"minconf", RPCArg::Type::NUM, RPCArg::Default{1},
                  "Minimum confirmations for selected inputs"},
                 {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false},
                  "Allow unsafe wallet inputs"},
             }},
        },
        AssetTransactionResult("Created transaction", {
            {RPCResult::Type::STR_HEX, "asset_id", "Transferred asset id"},
            {RPCResult::Type::NUM, "amount", "Integer asset units sent"},
            {RPCResult::Type::NUM, "asset_change", "Integer asset units returned to this wallet"},
            {RPCResult::Type::STR, "owner_address", "Recipient owner address"},
        }),
        RPCExamples{HelpExampleCli("sendasset", "\"asset_id\" 10 \"address\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            return TransferOrBurnAsset(request, false);
        }};
}

RPCHelpMan burnasset()
{
    return RPCHelpMan{
        "burnasset",
        "Permanently burn integer units of a colored asset or FN Coin.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"asset_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "32-byte chain-bound asset id"},
            {"amount", RPCArg::Type::NUM, RPCArg::Optional::NO,
             "Integer asset units"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED,
             "Transaction options", {
                 {"broadcast", RPCArg::Type::BOOL, RPCArg::Default{true},
                  "Broadcast and add to this wallet"},
                 {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED,
                  "Fee rate in atomic units per vB"},
                 {"replaceable", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED,
                  "Signal BIP125 replaceability"},
                 {"minconf", RPCArg::Type::NUM, RPCArg::Default{1},
                  "Minimum confirmations for selected inputs"},
                 {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false},
                  "Allow unsafe wallet inputs"},
             }},
        },
        AssetTransactionResult("Created transaction", {
            {RPCResult::Type::STR_HEX, "asset_id", "Burned asset id"},
            {RPCResult::Type::NUM, "amount", "Integer asset units burned"},
            {RPCResult::Type::NUM, "asset_change", "Integer asset units returned to this wallet"},
        }),
        RPCExamples{HelpExampleCli("burnasset", "\"asset_id\" 10")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            return TransferOrBurnAsset(request, true);
        }};
}

RPCHelpMan createfncoin()
{
    return RPCHelpMan{
        "createfncoin",
        "Create the next proof-free modern FN Coin by destroying the native "
        "B3 amount pinned for the current lifetime slot.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
             "FN owner address (default: a new wallet address)"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED,
             "Transaction options", {
                 {"broadcast", RPCArg::Type::BOOL, RPCArg::Default{true},
                  "Broadcast and add to this wallet"},
                 {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED,
                  "Fee rate in atomic units per vB"},
                 {"replaceable", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED,
                  "Signal BIP125 replaceability"},
                 {"minconf", RPCArg::Type::NUM, RPCArg::Default{1},
                  "Minimum confirmations for selected inputs"},
                 {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false},
                  "Allow unsafe wallet inputs"},
             }},
        },
        AssetTransactionResult("Created transaction", {
            {RPCResult::Type::STR_HEX, "asset_id", "Chain-scoped FN asset id"},
            {RPCResult::Type::NUM, "amount", "FN Coin amount created"},
            {RPCResult::Type::STR, "owner_address", "FN owner address"},
            {RPCResult::Type::NUM, "created_before", "Modern FN Coins created before this transaction"},
            {RPCResult::Type::NUM, "capacity", "Remaining-era FN creation capacity"},
            {RPCResult::Type::NUM, "tier", "Proof-of-disintegration price tier"},
        }),
        RPCExamples{HelpExampleCli("createfncoin", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();

            const AssetRpcOptions options{
                ParseAssetRpcOptions(request.params[1])};
            const ChainSnapshot snapshot{SnapshotChainState(*wallet, true)};
            if (snapshot.pending_fn_pod) {
                throw JSONRPCError(
                    RPC_VERIFY_REJECTED,
                    "A modern FN creation for the current slot is already pending");
            }
            const Consensus::Params& params{Params().GetConsensus()};
            const auto capacity{modern::ModernFnCapacity(params)};
            if (!capacity || *snapshot.fn_issued_before >= *capacity) {
                throw JSONRPCError(RPC_MISC_ERROR,
                                   "The lifetime FN Coin cap is exhausted");
            }
            const auto fn_asset{modern::ConfiguredFnAssetId(params)};
            if (!fn_asset) {
                throw JSONRPCError(RPC_MISC_ERROR,
                                   "The chain-scoped FN asset id is unavailable");
            }
            const CAmount disintegration{modern::RequiredFnPodDisintegration(
                *snapshot.fn_issued_before)};

            CreatedTransactionResult created{nullptr, 0, std::nullopt, {}};
            std::string owner_address;
            {
                LOCK(wallet->cs_wallet);
                EnsureSigningWallet(*wallet);
                CCoinControl coin_control{options.coin_control};
                const CScript owner{GetOwnerScript(
                    *wallet, request.params[0], "b3-fn-owner",
                    owner_address)};
                const auto fn_output{modern::MakeAssetOwnerOutput(
                    *fn_asset, 1, modern::PolicyType::FN, owner)};
                if (!fn_output) {
                    throw JSONRPCError(RPC_INTERNAL_ERROR,
                                       "Unable to encode the FN owner output");
                }
                const std::vector<CRecipient> recipients{
                    AssetRecipient(*fn_output)};
                const ModernTransactionOptions modern_options{
                    .mpa = {modern::MakeModernFnPodRecord(
                        *snapshot.fn_issued_before,
                        /*output_index=*/0)},
                    .native_disintegration = disintegration};
                auto result{CreateTransaction(
                    *wallet, recipients,
                    /*change_pos=*/static_cast<unsigned int>(recipients.size()),
                    coin_control, /*sign=*/true, modern_options)};
                if (!result) {
                    throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS,
                                       util::ErrorString(result).original);
                }
                created = std::move(*result);
            }

            UniValue result{FinishAssetTransaction(
                request, *wallet, created, options, snapshot, true,
                disintegration, "fn-pod")};
            result.pushKV("asset_id", fn_asset->GetHex());
            result.pushKV("amount", 1);
            result.pushKV("owner_address", owner_address);
            result.pushKV("created_before", *snapshot.fn_issued_before);
            result.pushKV("capacity", *capacity);
            result.pushKV("tier", *snapshot.fn_issued_before / 500 + 1);
            return result;
        }};
}

RPCHelpMan importflowmeshkey()
{
    return RPCHelpMan{
        "importflowmeshkey",
        "Import an independently generated 32-byte BLS secret for an FN "
        "FlowMesh seat. The secret is stored in the wallet's encrypted "
        "opaque key collection and is never returned.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"blssecret", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "32-byte big-endian BLS secret scalar"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Imported FlowMesh key", {
            {RPCResult::Type::STR_HEX, "bls_pubkey", "48-byte BLS public key"},
            {RPCResult::Type::STR_HEX, "proof_of_possession", "96-byte BLS proof of possession"},
        }},
        RPCExamples{HelpExampleCli("importflowmeshkey", "\"<hex>\"")},
        [&](const RPCHelpMan& self,
            const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            const std::vector<unsigned char> bytes{
                ParseHexV(request.params[0], "blssecret")};
            if (bytes.size() != 32) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "blssecret must be exactly 32 bytes");
            }
            const auto key{bls::SecretKey::FromBytes(bytes)};
            if (!key) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "invalid BLS secret: must satisfy 0 < sk < r");
            }
            LOCK(wallet->cs_wallet);
            EnsureSigningWallet(*wallet);
            const auto imported{wallet->ImportFlowMeshBlsKey(*key)};
            if (!imported) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   util::ErrorString(imported).original);
            }
            UniValue result{UniValue::VOBJ};
            result.pushKV("bls_pubkey", HexStr(imported->Compressed()));
            result.pushKV("proof_of_possession",
                          HexStr(key->SignPoP().Compressed()));
            return result;
        }};
}

RPCHelpMan bindflowmeshseat()
{
    return RPCHelpMan{
        "bindflowmeshseat",
        "Pre-bind one amount-1 FN Coin as a FlowMesh seat at A2. By default "
        "the wallet selects an unbound FN Coin, creates and securely stores "
        "a fresh BLS key, and keeps FN ownership at a new wallet address. "
        "Full trading begins at A3 after the required 30-block runway.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
             "FN seat owner address (default: a new wallet address)"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED,
             "Seat and transaction options", {
                 {"fn_txid", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED,
                  "Specific FN outpoint transaction id (requires fn_vout)"},
                 {"fn_vout", RPCArg::Type::NUM, RPCArg::Optional::OMITTED,
                  "Specific FN output index (requires fn_txid)"},
                 {"bls_pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED,
                  "Use a previously imported 48-byte FlowMesh BLS public key"},
                 {"broadcast", RPCArg::Type::BOOL, RPCArg::Default{true},
                  "Broadcast and add to this wallet"},
                 {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED,
                  "Fee rate in atomic units per vB"},
                 {"replaceable", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED,
                  "Signal BIP125 replaceability"},
                 {"minconf", RPCArg::Type::NUM, RPCArg::Default{1},
                  "Minimum confirmations for the FN input"},
                 {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false},
                  "Allow unsafe wallet inputs"},
             }},
        },
        AssetTransactionResult("Created seat-binding transaction", {
            {RPCResult::Type::STR_HEX, "fn_input_txid", "Transaction id of the FN Coin being bound or rotated"},
            {RPCResult::Type::NUM, "fn_input_vout", "Output index of the FN Coin being bound or rotated"},
            {RPCResult::Type::STR_HEX, "seat_txid", "Seat-binding transaction id"},
            {RPCResult::Type::NUM, "seat_vout", "Seat output index"},
            {RPCResult::Type::STR_HEX, "seat_id", "Chain-bound FlowMesh seat id"},
            {RPCResult::Type::STR_HEX, "bls_pubkey", "48-byte BLS public key"},
            {RPCResult::Type::STR, "owner_address", "FN seat owner address"},
            {RPCResult::Type::BOOL, "rotation", "Whether this transaction rotates an existing seat"},
        }),
        RPCExamples{HelpExampleCli("bindflowmeshseat", "")},
        [&](const RPCHelpMan& self,
            const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();

            const FlowMeshSeatRpcOptions options{
                ParseFlowMeshSeatRpcOptions(request.params[1])};
            const ChainSnapshot snapshot{SnapshotChainState(*wallet, false)};
            const Consensus::Params& params{Params().GetConsensus()};
            if (!Consensus::FlowMeshSeatBindingRulesActive(
                    snapshot.next_height, params)) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    "FlowMesh seat pre-binding is not active for the next block");
            }
            const auto fn_asset{modern::ConfiguredFnAssetId(params)};
            const auto domain{modern::ModernChainDomain(
                params.hashGenesisBlock,
                params.legacy_final_hash.value_or(uint256{}))};
            if (!fn_asset || !domain) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    "The chain-scoped FN or FlowMesh domain is unavailable");
            }

            CreatedTransactionResult created{nullptr, 0, std::nullopt, {}};
            std::string owner_address;
            WalletAssetCoin previous;
            std::array<unsigned char, bls::PUBKEY_SIZE> public_key{};
            {
                LOCK(wallet->cs_wallet);
                EnsureSigningWallet(*wallet);
                CCoinControl coin_control{options.transaction.coin_control};
                previous = SelectFlowMeshSeatFn(
                    *wallet, *fn_asset, options, coin_control);

                std::optional<bls::SecretKey> seat_key;
                if (options.bls_pubkey) {
                    const auto stored{
                        wallet->GetFlowMeshBlsKey(*options.bls_pubkey)};
                    if (!stored) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            util::ErrorString(stored).original);
                    }
                    seat_key = *stored;
                } else {
                    seat_key = GenerateFlowMeshSeatKey();
                    const auto imported{
                        wallet->ImportFlowMeshBlsKey(*seat_key)};
                    if (!imported) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            util::ErrorString(imported).original);
                    }
                }
                public_key = seat_key->GetPublicKey().Compressed();

                const CScript owner{GetOwnerScript(
                    *wallet, request.params[0], "b3-flowmesh-seat-owner",
                    owner_address)};
                const auto seat_output{modern::MakeFlowMeshSeatOutput(
                    *fn_asset, owner, seat_key->GetPublicKey())};
                if (!seat_output) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "Unable to encode the FlowMesh FN-seat output");
                }
                const auto pop{seat_key->SignPoP().Compressed()};
                const std::vector<CRecipient> recipients{
                    AssetRecipient(*seat_output)};
                const ModernTransactionOptions modern_options{
                    .mpa = {modern::MakeFlowMeshSeatBindingRecord(
                        /*output_index=*/0, pop)},
                    .native_disintegration = 0};
                auto result{CreateTransaction(
                    *wallet, recipients,
                    /*change_pos=*/static_cast<unsigned int>(recipients.size()),
                    coin_control, /*sign=*/true, modern_options)};
                if (!result) {
                    throw JSONRPCError(
                        RPC_WALLET_INSUFFICIENT_FUNDS,
                        util::ErrorString(result).original);
                }
                created = std::move(*result);
            }

            UniValue result{FinishAssetTransaction(
                request, *wallet, created, options.transaction, snapshot,
                false, /*disintegration=*/0, "flowmesh-seat-binding")};
            const COutPoint seat_outpoint{created.tx->GetHash(), 0};
            result.pushKV("fn_input_txid", previous.outpoint.hash.GetHex());
            result.pushKV("fn_input_vout", previous.outpoint.n);
            result.pushKV("seat_txid", seat_outpoint.hash.GetHex());
            result.pushKV("seat_vout", seat_outpoint.n);
            result.pushKV("seat_id",
                          flowmesh::ComputeFlowMeshSeatId(*domain,
                                                         seat_outpoint)
                              .GetHex());
            result.pushKV("bls_pubkey", HexStr(public_key));
            result.pushKV("owner_address", owner_address);
            result.pushKV("rotation",
                          previous.output.policy_version ==
                              modern::FN_SEAT_POLICY_VERSION_V2);
            return result;
        }};
}

RPCHelpMan flowmeshdeposit()
{
    return RPCHelpMan{
        "flowmeshdeposit",
        "Deposit either the market's colored asset or native B3 into its "
        "keyless FlowMesh vault for this wallet's independent trading "
        "account. No OP_RETURN is used. Native amounts use normal B3 decimal "
        "units; colored-asset amounts are exact integer base units.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"base_asset_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "The market's 32-byte colored asset id"},
            {"deposit_asset", RPCArg::Type::STR, RPCArg::Optional::NO,
             "The base asset id, or 'B3' for native quote funds"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO,
             "B3 decimal amount, or integer colored-asset units"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED,
             "Transaction options", {
                 {"broadcast", RPCArg::Type::BOOL, RPCArg::Default{true},
                  "Broadcast and add to this wallet"},
                 {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED,
                  "Fee rate in atomic units per vB"},
                 {"replaceable", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED,
                  "Signal BIP125 replaceability"},
                 {"minconf", RPCArg::Type::NUM, RPCArg::Default{1},
                  "Minimum confirmations for selected inputs"},
                 {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false},
                  "Allow unsafe wallet inputs"},
                 {"market_bootstrap", RPCArg::Type::BOOL, RPCArg::Default{false},
                  "Explicitly create this market's first colored deposit while its runtime is unavailable or paused. The output is keyless and cannot be recovered until a qualifying FlowMesh seat quorum activates the market"},
             }},
        },
        AssetTransactionResult("Created vault-deposit transaction", {
            {RPCResult::Type::STR_HEX, "deposit_txid", "Vault-deposit transaction id"},
            {RPCResult::Type::NUM, "deposit_vout", "Vault-deposit output index"},
            {RPCResult::Type::STR_HEX, "market_id", "FlowMesh market id"},
            {RPCResult::Type::STR_HEX, "vault_id", "FlowMesh vault id"},
            {RPCResult::Type::STR_HEX, "account_id", "Wallet FlowMesh account id"},
            {RPCResult::Type::STR_HEX, "asset_id", "Deposited asset id; zero denotes native B3"},
            {RPCResult::Type::NUM, "amount", "Deposited amount (B3 decimal amount or exact colored-asset units)"},
            {RPCResult::Type::NUM, "asset_change", "Colored-asset units returned to this wallet, or zero for B3"},
            {RPCResult::Type::NUM, "shard", "Deterministic vault shard"},
            {RPCResult::Type::BOOL, "market_bootstrap", "Whether this is the explicit first colored deposit that establishes the market"},
            {RPCResult::Type::NUM, "anchor_confirmations_required", "Required FlowMesh anchor depth"},
        }),
        RPCExamples{HelpExampleCli(
            "flowmeshdeposit", "\"<asset_id>\" \"B3\" 25")},
        [&](const RPCHelpMan& self,
            const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();

            const modern::AssetId base{ParseAssetId(request.params[0])};
            const modern::AssetId deposit_asset{
                ParseAssetOrNative(request.params[1], "deposit_asset")};
            if (deposit_asset != base &&
                deposit_asset != modern::NativeAsset()) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "deposit_asset must be this market's base asset or B3");
            }
            const CAmount amount{
                FlowMeshDepositAmount(request.params[2], deposit_asset)};
            const AssetRpcOptions options{
                ParseAssetRpcOptions(request.params[3],
                                     /*flowmesh_seat=*/false,
                                     /*flowmesh_deposit=*/true)};
            const ChainSnapshot snapshot{SnapshotChainState(*wallet, false)};
            const Consensus::Params& params{Params().GetConsensus()};
            if (!Consensus::FlowMeshVaultPreparationRulesActive(
                    snapshot.next_height, params)) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    "FlowMesh vault preparation is not active for the next block");
            }
            const auto fn_asset{modern::ConfiguredFnAssetId(params)};
            if (fn_asset && base == *fn_asset) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "FlowMesh simple-v1 markets require a colored asset, not FN Coin");
            }
            const auto domain{modern::ModernChainDomain(
                params.hashGenesisBlock,
                params.legacy_final_hash.value_or(uint256{}))};
            if (!domain) {
                throw JSONRPCError(RPC_MISC_ERROR,
                                   "The modern chain domain is unavailable");
            }
            const auto market{flowmesh::ComputeFlowMeshMarketId(*domain, base)};
            const auto vault{market ? flowmesh::ComputeFlowMeshVaultId(
                                          *domain, *market)
                                    : std::nullopt};
            if (!market || !vault) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "Unable to derive this FlowMesh market");
            }

            auto require_deposit_admission = [&] {
                const bool established{
                    wallet->chain().flowMeshMarketEstablished(*market)};
                const auto status{wallet->chain().flowMeshMarketStatus(
                    *market, /*account_id=*/std::nullopt)};
                const bool runtime_ready{
                    status && status->available && status->running &&
                    status->halt == "none" && !status->domain.IsNull() &&
                    status->market_id == *market &&
                    status->vault_id == *vault &&
                    status->base_asset == base &&
                    status->quote_asset == modern::NativeAsset()};
                const FlowMeshDepositAdmission admission{
                    CheckFlowMeshDepositAdmission(
                        options.market_bootstrap,
                        Consensus::FlowMeshRulesActive(snapshot.next_height,
                                                       params),
                        established, deposit_asset == base, runtime_ready,
                        status && status->paused)};
                switch (admission) {
                case FlowMeshDepositAdmission::USER_DEPOSIT:
                case FlowMeshDepositAdmission::MARKET_BOOTSTRAP:
                    return;
                case FlowMeshDepositAdmission::RULES_INACTIVE:
                    throw JSONRPCError(
                        RPC_MISC_ERROR,
                        "FlowMesh user deposits are not active for the next block");
                case FlowMeshDepositAdmission::MARKET_NOT_ESTABLISHED:
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "This market is not established; use market_bootstrap=true to explicitly create its first keyless colored deposit");
                case FlowMeshDepositAdmission::RUNTIME_UNAVAILABLE:
                    throw JSONRPCError(
                        RPC_MISC_ERROR,
                        "FlowMesh market runtime is not ready in this node");
                case FlowMeshDepositAdmission::MARKET_PAUSED:
                    throw JSONRPCError(
                        RPC_MISC_ERROR,
                        "FlowMesh market is paused; user deposits are refused until at least four active seats are available");
                case FlowMeshDepositAdmission::BOOTSTRAP_REQUIRES_BASE_ASSET:
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "market_bootstrap can only create the market's first colored-asset deposit");
                case FlowMeshDepositAdmission::BOOTSTRAP_MARKET_ALREADY_ESTABLISHED:
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "This FlowMesh market is already established; remove market_bootstrap and wait until its runtime is unpaused");
                }
                NONFATAL_UNREACHABLE();
            };
            require_deposit_admission();

            CreatedTransactionResult created{nullptr, 0, std::nullopt, {}};
            flowmesh::AccountId account;
            CAmount asset_change{0};
            uint16_t shard{0};
            {
                LOCK(wallet->cs_wallet);
                EnsureSigningWallet(*wallet);
                const auto account_pubkey{
                    wallet->GetOrCreateFlowMeshAccountKey()};
                if (!account_pubkey) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        util::ErrorString(account_pubkey).original);
                }
                account = flowmesh::AccountForKey(
                    XOnlyPubKey{*account_pubkey});
                shard = modern::FlowMeshUserDepositShard(*vault, account);

                CCoinControl coin_control{options.coin_control};
                if (deposit_asset != modern::NativeAsset()) {
                    const CAmount selected{SelectAssetInputs(
                        *wallet, deposit_asset,
                        static_cast<uint16_t>(modern::PolicyType::OWNER),
                        amount, coin_control)};
                    asset_change = selected - amount;
                }
                const auto vault_output{modern::MakeDexVaultOutput(
                    deposit_asset, amount, *vault,
                    modern::VAULT_KIND_USER_DEPOSIT, shard, account)};
                if (!vault_output) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "Unable to encode the FlowMesh vault deposit");
                }
                std::vector<CRecipient> recipients{
                    AssetRecipient(*vault_output)};
                if (asset_change > 0) {
                    std::string unused;
                    const CScript change_owner{GetOwnerScript(
                        *wallet, UniValue{}, "b3-flowmesh-deposit-change",
                        unused)};
                    const auto change{modern::MakeAssetOwnerOutput(
                        deposit_asset, asset_change,
                        modern::PolicyType::OWNER, change_owner)};
                    if (!change) {
                        throw JSONRPCError(
                            RPC_INTERNAL_ERROR,
                            "Unable to encode colored-asset deposit change");
                    }
                    recipients.push_back(AssetRecipient(*change));
                }
                auto result{CreateTransaction(
                    *wallet, recipients,
                    /*change_pos=*/static_cast<unsigned int>(recipients.size()),
                    coin_control, /*sign=*/true)};
                if (!result) {
                    throw JSONRPCError(
                        RPC_WALLET_INSUFFICIENT_FUNDS,
                        util::ErrorString(result).original);
                }
                created = std::move(*result);
            }

            // Runtime readiness can change independently of wallet
            // construction. Recheck immediately before the common
            // finish/broadcast path so the RPC never knowingly publishes
            // into a paused market.
            require_deposit_admission();

            UniValue result{FinishAssetTransaction(
                request, *wallet, created, options, snapshot, false,
                /*disintegration=*/0, "flowmesh-deposit")};
            const COutPoint deposit_outpoint{created.tx->GetHash(), 0};
            result.pushKV("deposit_txid", deposit_outpoint.hash.GetHex());
            result.pushKV("deposit_vout", deposit_outpoint.n);
            result.pushKV("market_id", market->GetHex());
            result.pushKV("vault_id", vault->GetHex());
            result.pushKV("account_id", account.GetHex());
            result.pushKV("asset_id", deposit_asset.GetHex());
            if (deposit_asset == modern::NativeAsset()) {
                result.pushKV("amount", ValueFromAmount(amount));
            } else {
                result.pushKV("amount", amount);
            }
            result.pushKV("asset_change", asset_change);
            result.pushKV("shard", shard);
            result.pushKV("market_bootstrap", options.market_bootstrap);
            result.pushKV("anchor_confirmations_required",
                          Consensus::FLOWMESH_ANCHOR_DEPTH);
            return result;
        }};
}

RPCHelpMan submitbridgecarrier()
{
    return RPCHelpMan{
        "submitbridgecarrier",
        "Fund, sign, and optionally broadcast one canonical bridge bootstrap, "
        "Ethereum light-client update, or execution-backfill type-10 record. "
        "The payload must be produced by an external bridge proof builder. "
        "Mint and managed-withdrawal records are rejected by this command. "
        "A mandatory zero-valued B3MC policy-9 output binds the exact MPA "
        "payload to the wallet signature; no OP_RETURN is used.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"bridge_payload", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "Canonical type-10 v1 payload bytes, without the outer MPA frame"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED,
             "Transaction options", {
                 {"broadcast", RPCArg::Type::BOOL, RPCArg::Default{true},
                  "Broadcast and add to this wallet"},
                 {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED,
                  "Fee rate in atomic units per vB"},
                 {"replaceable", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED,
                  "Signal BIP125 replaceability"},
                 {"minconf", RPCArg::Type::NUM, RPCArg::Default{1},
                  "Minimum confirmations for selected native inputs"},
                 {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false},
                  "Allow unsafe wallet inputs"},
             }},
        },
        AssetTransactionResult("Created bridge carrier transaction", {
            {RPCResult::Type::STR, "record_kind", "bootstrap, update, or execution-backfill"},
            {RPCResult::Type::NUM, "binding_vout", "Signed B3MC bridge binding output index"},
        }),
        RPCExamples{HelpExampleCli(
            "submitbridgecarrier", "\"<canonical_payload_hex>\"")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const ParsedBridgePayload parsed{
                ParseCanonicalBridgePayload(request.params[0])};
            if (parsed.decoded.kind !=
                    bridge::BridgeRecordKindV1::BOOTSTRAP &&
                parsed.decoded.kind != bridge::BridgeRecordKindV1::UPDATE &&
                parsed.decoded.kind !=
                    bridge::BridgeRecordKindV1::EXECUTION_BACKFILL) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "submitbridgecarrier accepts only bootstrap, update, or execution-backfill records");
            }

            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            const AssetRpcOptions options{
                ParseAssetRpcOptions(request.params[1])};
            const ChainSnapshot snapshot{SnapshotChainState(*wallet, false)};
            (void)RequireBridgeForNextBlock(snapshot.next_height);

            CreatedTransactionResult created{nullptr, 0, std::nullopt, {}};
            {
                LOCK(wallet->cs_wallet);
                EnsureSigningWallet(*wallet);
                std::vector<CRecipient> recipients;
                recipients.push_back(BridgeSelfPayment(
                    *wallet, "b3-bridge-carrier"));
                recipients.push_back(BridgeBindingRecipient(parsed.record));
                const ModernTransactionOptions modern_options{
                    .mpa = {parsed.record}};
                auto result{CreateTransaction(
                    *wallet, recipients,
                    /*change_pos=*/static_cast<unsigned int>(recipients.size()),
                    options.coin_control, /*sign=*/true, modern_options)};
                if (!result) {
                    throw JSONRPCError(
                        RPC_WALLET_INSUFFICIENT_FUNDS,
                        util::ErrorString(result).original);
                }
                created = std::move(*result);
            }

            const node::BridgeTxAuthorization authorization{
                PrevalidateBridgeTransaction(*wallet, *created.tx, snapshot)};
            if (authorization.mint || authorization.withdrawal) {
                throw JSONRPCError(
                    RPC_INTERNAL_ERROR,
                    "A bridge carrier unexpectedly produced an asset authorization");
            }
            UniValue result{FinishAssetTransaction(
                request, *wallet, created, options, snapshot, false,
                /*disintegration=*/0, "bridge-carrier")};
            result.pushKV("record_kind",
                          BridgeRecordKindName(parsed.decoded.kind));
            result.pushKV("binding_vout", 1);
            return result;
        }};
}

RPCHelpMan claimbridgedeposit()
{
    return RPCHelpMan{
        "claimbridgedeposit",
        "Create the exact bUSD OWNER output authorized by one canonical "
        "type-10 Ethereum deposit proof. The external proof builder must set "
        "output_index to zero and supply the canonical payload. The amount is "
        "an integer number of raw six-decimal bUSD units. This command checks "
        "the proof, finalized bridge state, replay nullifier, recipient, amount, "
        "and mint caps before broadcast. A signed B3MC policy-9 cell binds the "
        "MPA payload; no OP_RETURN is used.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"bridge_payload", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "Canonical type-10 v1 MINT payload bytes"},
            {"amount", RPCArg::Type::NUM, RPCArg::Optional::NO,
             "Exact integer bUSD units authorized by the Ethereum deposit"},
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO,
             "Exact B3 P2PKH recipient encoded in the Ethereum deposit"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED,
             "Transaction options", {
                 {"broadcast", RPCArg::Type::BOOL, RPCArg::Default{true},
                  "Broadcast and add to this wallet"},
                 {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED,
                  "Fee rate in atomic units per vB"},
                 {"replaceable", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED,
                  "Signal BIP125 replaceability"},
                 {"minconf", RPCArg::Type::NUM, RPCArg::Default{1},
                  "Minimum confirmations for selected native inputs"},
                 {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false},
                  "Allow unsafe wallet inputs"},
             }},
        },
        AssetTransactionResult("Created bridge deposit-mint transaction", {
            {RPCResult::Type::STR_HEX, "asset_id", "Configured chain-bound bUSD asset id"},
            {RPCResult::Type::NUM, "amount", "Integer bUSD units minted"},
            {RPCResult::Type::STR, "owner_address", "Authorized B3 recipient"},
            {RPCResult::Type::NUM, "mint_vout", "Authorized bUSD output index (zero)"},
            {RPCResult::Type::NUM, "binding_vout", "Signed B3MC bridge binding output index"},
        }),
        RPCExamples{HelpExampleCli(
            "claimbridgedeposit",
            "\"<canonical_mint_payload_hex>\" 1000000 \"<b3_address>\"")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const ParsedBridgePayload parsed{
                ParseCanonicalBridgePayload(request.params[0])};
            if (parsed.decoded.kind != bridge::BridgeRecordKindV1::MINT) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "bridge_payload is not a MINT record");
            }
            const auto* mint{
                std::get_if<bridge::BridgeMintV1>(&parsed.decoded.payload)};
            if (mint == nullptr || mint->output_index != 0) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "The wallet mint path requires bridge output_index zero");
            }
            const CAmount amount{
                AssetUnitsFromValue(request.params[1], "amount")};

            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            const AssetRpcOptions options{
                ParseAssetRpcOptions(request.params[3])};
            const ChainSnapshot snapshot{SnapshotChainState(*wallet, false)};
            const Consensus::BridgeAssetParams& bridge_params{
                RequireBridgeForNextBlock(snapshot.next_height)};
            if (bridge_params.approval_last_height &&
                snapshot.next_height > *bridge_params.approval_last_height) {
                throw JSONRPCError(RPC_MISC_ERROR,
                                   "New bridge mints are no longer approved");
            }
            const Consensus::Params& params{Params().GetConsensus()};
            const auto asset{modern::ConfiguredBridgeAssetId(params)};
            const auto registry{modern::ConfiguredBridgeRegistryId(params)};
            if (!asset || !registry || mint->registry_id != *registry) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "The MINT payload does not name the active bridge registry");
            }

            CreatedTransactionResult created{nullptr, 0, std::nullopt, {}};
            std::string owner_address;
            CScript owner_script;
            {
                LOCK(wallet->cs_wallet);
                EnsureSigningWallet(*wallet);
                owner_script = GetOwnerScript(
                    *wallet, request.params[2], "b3-busd-bridge-receive",
                    owner_address);
                const auto minted{modern::MakeAssetOwnerOutput(
                    *asset, amount, modern::PolicyType::OWNER, owner_script)};
                if (!minted) {
                    throw JSONRPCError(RPC_INTERNAL_ERROR,
                                       "Unable to encode the bUSD owner output");
                }
                const std::vector<CRecipient> recipients{
                    AssetRecipient(*minted),
                    BridgeBindingRecipient(parsed.record),
                };
                const ModernTransactionOptions modern_options{
                    .mpa = {parsed.record}};
                auto result{CreateTransaction(
                    *wallet, recipients,
                    /*change_pos=*/static_cast<unsigned int>(recipients.size()),
                    options.coin_control, /*sign=*/true, modern_options)};
                if (!result) {
                    throw JSONRPCError(
                        RPC_WALLET_INSUFFICIENT_FUNDS,
                        util::ErrorString(result).original);
                }
                created = std::move(*result);
            }

            const node::BridgeTxAuthorization authorization{
                PrevalidateBridgeTransaction(*wallet, *created.tx, snapshot)};
            if (!authorization.mint || authorization.withdrawal ||
                authorization.mint->output_index != 0 ||
                authorization.mint->authorization.asset != *asset ||
                authorization.mint->authorization.amount != amount ||
                authorization.mint->authorization.recipient_script !=
                    owner_script) {
                throw JSONRPCError(
                    RPC_VERIFY_REJECTED,
                    "Bridge prevalidation did not authorize the exact requested bUSD output");
            }
            UniValue result{FinishAssetTransaction(
                request, *wallet, created, options, snapshot, false,
                /*disintegration=*/0, "bridge-deposit-mint")};
            result.pushKV("asset_id", asset->GetHex());
            result.pushKV("amount", amount);
            result.pushKV("owner_address", owner_address);
            result.pushKV("mint_vout", 0);
            result.pushKV("binding_vout", 1);
            return result;
        }};
}

RPCHelpMan bridgewithdraw()
{
    return RPCHelpMan{
        "bridgewithdraw",
        "Burn exact integer bUSD units and create a managed-v1 Ethereum USDT "
        "release request for one nonzero 20-byte Ethereum recipient. The burn "
        "and recipient are bound together by the canonical type-10 record and "
        "its signed B3MC policy-9 output. The operator may release reserves only "
        "after this request is confirmed under the published managed-v1 rules. "
        "No OP_RETURN is used.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"amount", RPCArg::Type::NUM, RPCArg::Optional::NO,
             "Integer raw bUSD units to burn (six decimal places)"},
            {"ethereum_recipient", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "Exact 20-byte Ethereum recipient, with optional 0x prefix"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED,
             "Transaction options", {
                 {"broadcast", RPCArg::Type::BOOL, RPCArg::Default{true},
                  "Broadcast and add to this wallet"},
                 {"fee_rate", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED,
                  "Fee rate in atomic units per vB"},
                 {"replaceable", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED,
                  "Signal BIP125 replaceability"},
                 {"minconf", RPCArg::Type::NUM, RPCArg::Default{1},
                  "Minimum confirmations for selected inputs"},
                 {"include_unsafe", RPCArg::Type::BOOL, RPCArg::Default{false},
                  "Allow unsafe wallet inputs"},
             }},
        },
        AssetTransactionResult("Created managed bridge withdrawal request", {
            {RPCResult::Type::STR_HEX, "asset_id", "Configured chain-bound bUSD asset id"},
            {RPCResult::Type::NUM, "amount", "Integer bUSD units burned"},
            {RPCResult::Type::NUM, "asset_change", "Integer bUSD units returned to this wallet"},
            {RPCResult::Type::STR_HEX, "ethereum_recipient", "Bound Ethereum recipient"},
            {RPCResult::Type::NUM, "burn_vout", "Exact bUSD BURN output index"},
            {RPCResult::Type::NUM, "binding_vout", "Signed B3MC bridge binding output index"},
            {RPCResult::Type::STR, "withdrawal_mode", "managed-v1"},
        }),
        RPCExamples{HelpExampleCli(
            "bridgewithdraw", "1000000 \"0x00112233445566778899aabbccddeeff00112233\"")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const CAmount amount{
                AssetUnitsFromValue(request.params[0], "amount")};
            const bridge::EthAddress ethereum_recipient{
                ParseEthereumRecipient(request.params[1])};
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            const AssetRpcOptions options{
                ParseAssetRpcOptions(request.params[2])};
            const ChainSnapshot snapshot{SnapshotChainState(*wallet, false)};
            const Consensus::BridgeAssetParams& bridge_params{
                RequireBridgeForNextBlock(snapshot.next_height)};
            if (*bridge_params.withdrawal_mode !=
                Consensus::BridgeWithdrawalMode::MANAGED_V1) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    "Managed-v1 bridge withdrawals are not enabled");
            }
            const Consensus::Params& params{Params().GetConsensus()};
            const auto asset{modern::ConfiguredBridgeAssetId(params)};
            const auto registry{modern::ConfiguredBridgeRegistryId(params)};
            if (!asset || !registry) {
                throw JSONRPCError(RPC_MISC_ERROR,
                                   "The active bridge identity is unavailable");
            }
            const bridge::BridgeManagedWithdrawalV1 withdrawal{
                *registry, /*burn_output_index=*/0,
                static_cast<uint64_t>(amount), ethereum_recipient};
            const auto record{bridge::MakeBridgeMpaRecord(
                bridge::BridgeRecordV1{
                    bridge::BridgeRecordKindV1::MANAGED_WITHDRAWAL,
                    withdrawal})};
            if (!record) {
                throw JSONRPCError(RPC_INTERNAL_ERROR,
                                   "Unable to encode the withdrawal record");
            }

            CreatedTransactionResult created{nullptr, 0, std::nullopt, {}};
            CAmount asset_change{0};
            {
                LOCK(wallet->cs_wallet);
                EnsureSigningWallet(*wallet);
                CCoinControl coin_control{options.coin_control};
                const CAmount selected{SelectAssetInputs(
                    *wallet, *asset,
                    static_cast<uint16_t>(modern::PolicyType::OWNER),
                    amount, coin_control)};
                asset_change = selected - amount;
                const auto burned{modern::MakeAssetBurnOutput(*asset, amount)};
                if (!burned) {
                    throw JSONRPCError(RPC_INTERNAL_ERROR,
                                       "Unable to encode the bUSD BURN output");
                }
                std::vector<CRecipient> recipients{
                    AssetRecipient(*burned), BridgeBindingRecipient(*record)};
                if (asset_change > 0) {
                    std::string unused;
                    const CScript change_owner{GetOwnerScript(
                        *wallet, UniValue{}, "b3-busd-withdrawal-change",
                        unused)};
                    const auto change{modern::MakeAssetOwnerOutput(
                        *asset, asset_change, modern::PolicyType::OWNER,
                        change_owner)};
                    if (!change) {
                        throw JSONRPCError(
                            RPC_INTERNAL_ERROR,
                            "Unable to encode the bUSD withdrawal change output");
                    }
                    recipients.push_back(AssetRecipient(*change));
                }
                const ModernTransactionOptions modern_options{
                    .mpa = {*record}};
                auto result{CreateTransaction(
                    *wallet, recipients,
                    /*change_pos=*/static_cast<unsigned int>(recipients.size()),
                    coin_control, /*sign=*/true, modern_options)};
                if (!result) {
                    throw JSONRPCError(
                        RPC_WALLET_INSUFFICIENT_FUNDS,
                        util::ErrorString(result).original);
                }
                created = std::move(*result);
            }

            const node::BridgeTxAuthorization authorization{
                PrevalidateBridgeTransaction(*wallet, *created.tx, snapshot)};
            if (authorization.mint || !authorization.withdrawal ||
                authorization.withdrawal->burn_output_index != 0 ||
                authorization.withdrawal->asset != *asset ||
                authorization.withdrawal->amount != amount ||
                authorization.withdrawal->ethereum_recipient !=
                    ethereum_recipient) {
                throw JSONRPCError(
                    RPC_VERIFY_REJECTED,
                    "Bridge prevalidation did not authorize the exact withdrawal burn and recipient");
            }
            UniValue result{FinishAssetTransaction(
                request, *wallet, created, options, snapshot, false,
                /*disintegration=*/0, "bridge-managed-withdrawal")};
            result.pushKV("asset_id", asset->GetHex());
            result.pushKV("amount", amount);
            result.pushKV("asset_change", asset_change);
            result.pushKV("ethereum_recipient", HexStr(ethereum_recipient));
            result.pushKV("burn_vout", 0);
            result.pushKV("binding_vout", 1);
            result.pushKV("withdrawal_mode", "managed-v1");
            return result;
        }};
}

} // namespace wallet
