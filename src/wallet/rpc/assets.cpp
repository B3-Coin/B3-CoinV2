// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chainparams.h>
#include <bridge/admission.h>
#include <bridge/eip191.h>
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
#include <modern/bridge_forwarder.h>
#include <modern/bridge_v2.h>
#include <modern/chain_domain.h>
#include <modern/fn_pod.h>
#include <modern/flowmesh_seat.h>
#include <modern/withdrawal_v2.h>
#include <random.h>
#include <node/bridge_state.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <rpc/util.h>
#include <sync.h>
#include <util/check.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <support/cleanse.h>
#include <wallet/coincontrol.h>
#include <wallet/receive.h>
#include <wallet/rpc/assets.h>
#include <wallet/rpc/flowmesh.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <univalue.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace wallet {

CAmount AssetUnitsFromValue(const UniValue& value, const std::string& name)
{
    if (!value.isNum() && !value.isStr()) {
        throw JSONRPCError(RPC_TYPE_ERROR,
                           name + " must be an integer number or string of asset units");
    }
    // Parse the original decimal characters, never a floating-point value or
    // an amount scaled by display precision. Quoted and JSON-number inputs
    // have the same canonical, unsigned integer grammar.
    const std::string& text{value.getValStr()};
    if (text.empty() || (text.size() > 1 && text.front() == '0') ||
        !std::all_of(text.begin(), text.end(), [](const char digit) {
            return digit >= '0' && digit <= '9';
        })) {
        throw JSONRPCError(RPC_TYPE_ERROR,
                           name + " must contain only canonical integer asset units, without a sign, fraction, or exponent");
    }
    CAmount units{0};
    const auto [end, error]{std::from_chars(text.data(), text.data() + text.size(), units)};
    if (error == std::errc::result_out_of_range || units < 1 || units > MAX_MONEY) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           name + " must be in the range [1, MAX_MONEY]");
    }
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw JSONRPCError(RPC_TYPE_ERROR,
                           name + " must be an integer number or string of asset units");
    }
    return units;
}

std::optional<CMpaRecord> BuildBridgeWithdrawalMpaRecord(
    const Consensus::BridgeWithdrawalMode mode, const uint256& registry_id,
    const uint32_t burn_output_index, const uint64_t raw_amount,
    const bridge::EthAddress& ethereum_recipient)
{
    switch (mode) {
    case Consensus::BridgeWithdrawalMode::MANAGED_V1:
        return bridge::MakeBridgeMpaRecord(bridge::BridgeRecordV1{
            bridge::BridgeRecordKindV1::MANAGED_WITHDRAWAL,
            bridge::BridgeManagedWithdrawalV1{
                registry_id, burn_output_index, raw_amount,
                ethereum_recipient}});
    case Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1:
        return bridge::MakeBridgeMpaRecord(bridge::BridgeRecordV1{
            bridge::BridgeRecordKindV1::BRIDGE_BURN,
            bridge::BridgeBurnV1{registry_id, burn_output_index, raw_amount,
                                 ethereum_recipient}});
    }
    return std::nullopt;
}

/** BuildBridgeWithdrawalMpaRecord with the optional route (d3) exit block of
 * a bridge V2 burn (node changes section 7.2 item 5). Without `exit` it is
 * the five-argument builder, byte for byte. With it the record is a kind 6
 * BRIDGE_BURN only, encoded under the V2 record set, the only one that
 * carries an exit block; consensus checks the block and that
 * ethereum_recipient is its exit forwarder. */
static std::optional<CMpaRecord> BuildBridgeWithdrawalMpaRecord(
    const Consensus::BridgeWithdrawalMode mode, const uint256& registry_id,
    const uint32_t burn_output_index, const uint64_t raw_amount,
    const bridge::EthAddress& ethereum_recipient,
    const std::optional<modern::ExitForwarderParamsV2>& exit)
{
    if (!exit) {
        return BuildBridgeWithdrawalMpaRecord(mode, registry_id,
                                              burn_output_index, raw_amount,
                                              ethereum_recipient);
    }
    if (mode != Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1) {
        return std::nullopt;
    }
    return bridge::MakeBridgeMpaRecord(
        bridge::BridgeRecordV1{
            bridge::BridgeRecordKindV1::BRIDGE_BURN,
            bridge::BridgeBurnV1{registry_id, burn_output_index, raw_amount,
                                 ethereum_recipient, *exit}},
        bridge::BRIDGE_MPT_LIMITS_V2);
}

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
    //! burnasset only: allow a plain burn of the bridge V2 bUSD asset.
    bool allow_unredeemable_burn{false};
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
    case bridge::BridgeRecordKindV1::ETH_STATE_OBSERVATION:
        return "eth-state-observation";
    case bridge::BridgeRecordKindV1::FINALITY_CONFLICT:
        return "finality-conflict";
    case bridge::BridgeRecordKindV1::VAULT_SWEEP:
        return "vault-sweep";
    }
    return "unknown";
}

static ParsedBridgePayload ParseCanonicalBridgePayload(const UniValue& value)
{
    // The bridge V2 receipt-proof limits (node changes section 4: 8,192-byte
    // terminal receipt node, 16 KiB proof) only widen V1's, and the encoding
    // is the same, so the wallet parses with them at every height. Consensus
    // still selects the limits by height: below V a record that needs the V2
    // limits is malformed, and bridge prevalidation refuses it.
    const std::vector<unsigned char> payload{
        ParseHexV(value, "bridge_payload")};
    const auto decoded{
        bridge::DecodeBridgeRecordV1(payload, bridge::BRIDGE_MPT_LIMITS_V2)};
    if (!decoded) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "bridge_payload is not a canonical type-10 v1 payload");
    }
    const auto canonical{
        bridge::EncodeBridgeRecordV1(*decoded, bridge::BRIDGE_MPT_LIMITS_V2)};
    if (!canonical || *canonical != payload) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "bridge_payload has a non-canonical encoding");
    }
    const auto record{
        bridge::MakeBridgeMpaRecord(*decoded, bridge::BRIDGE_MPT_LIMITS_V2)};
    if (!record) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "bridge_payload cannot be encoded as type-10 v1");
    }
    return ParsedBridgePayload{*decoded, *record};
}

//! A non-zero 20-byte EVM address, hex with an optional 0x prefix; `name`
//! names the field in the errors.
static bridge::EthAddress ParseEthAddressField(const UniValue& value,
                                               const std::string& name)
{
    if (!value.isStr()) {
        throw JSONRPCError(RPC_TYPE_ERROR, name + " must be a hex address");
    }
    std::string encoded{value.get_str()};
    if (encoded.starts_with("0x") || encoded.starts_with("0X")) {
        encoded.erase(0, 2);
    }
    if (encoded.size() != 40 || !IsHex(encoded)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           name + " must be exactly 20 bytes of hexadecimal");
    }
    const std::vector<unsigned char> bytes{ParseHex(encoded)};
    bridge::EthAddress address{};
    std::copy(bytes.begin(), bytes.end(), address.begin());
    if (bridge::EthAddressIsNull(address)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           name + " cannot be the zero address");
    }
    return address;
}

static bridge::EthAddress ParseEthereumRecipient(const UniValue& value)
{
    return ParseEthAddressField(value, "ethereum_recipient");
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

//! `bridge_withdraw` admits bridgewithdraw's `arbitrum` key; its route (d3)
//! object is parsed by ParseArbitrumExitRoute, not here.
static AssetRpcOptions ParseAssetRpcOptions(const UniValue& value,
                                            const bool flowmesh_seat = false,
                                            const bool flowmesh_deposit = false,
                                            const bool asset_burn = false,
                                            const bool bridge_withdraw = false)
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
    if (asset_burn) allowed.insert("allow_unredeemable_burn");
    if (bridge_withdraw) allowed.insert("arbitrum");
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
    if (value.exists("allow_unredeemable_burn")) {
        parsed.allow_unredeemable_burn =
            value["allow_unredeemable_burn"].get_bool();
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
    const CAmount disintegration, const std::string& kind,
    const std::optional<uint256>& ordinary_deposit_market = std::nullopt)
{
    if (ordinary_deposit_market) {
        CHECK_NONFATAL(!inspect_fn_pool && kind == "flowmesh-deposit");
        std::string error;
        if (!wallet.chain().checkFlowMeshDepositTransaction(
                created.tx, *ordinary_deposit_market, error)) {
            throw JSONRPCError(RPC_VERIFY_REJECTED, error);
        }
    } else {
        // Issuance, FN slots, bridge operations and explicit market bootstrap
        // retain their existing exact-snapshot guard.
        RecheckChainSnapshot(wallet, snapshot, inspect_fn_pool);
    }
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

} // namespace

uint16_t AssetOwnerPolicy(const modern::AssetId& asset,
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
    const auto bridge_asset{modern::ConfiguredBridgeAssetId(params)};
    if (bridge_asset && asset == *bridge_asset) {
        if (!Consensus::BridgeRulesActive(next_height, params)) {
            throw JSONRPCError(
                RPC_MISC_ERROR,
                "Bridge asset transfers are not active for the next block");
        }
        return static_cast<uint16_t>(modern::PolicyType::OWNER);
    }
    if (!Consensus::AssetRulesActive(next_height, params)) {
        throw JSONRPCError(RPC_MISC_ERROR,
                           "Colored assets are not active for the next block");
    }
    return static_cast<uint16_t>(modern::PolicyType::OWNER);
}

namespace {

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

/**
 * B3's bridge V2 deposit status (interfaces::Chain::bridgeV2DepositStatus):
 * returns only on VALID; every other status, an unexpected one included,
 * throws (fail closed). A sticky conflict, a broken fresh lineage or a
 * latest VERIFIER_E showing an impaired verifier closes deposits for good;
 * `closed_consequence` tells the caller what a deposit made then would mean.
 */
static void RequireBridgeV2DepositsOpen(CWallet& wallet,
                                        const std::string& closed_consequence)
{
    std::string error;
    const interfaces::BridgePrevalidationResult status{
        wallet.chain().bridgeV2DepositStatus(error)};
    switch (status) {
    case interfaces::BridgePrevalidationResult::VALID:
        return;
    case interfaces::BridgePrevalidationResult::RULES_INACTIVE:
        throw JSONRPCError(RPC_MISC_ERROR, error);
    case interfaces::BridgePrevalidationResult::STATE_UNAVAILABLE:
        throw JSONRPCError(RPC_DATABASE_ERROR, error);
    case interfaces::BridgePrevalidationResult::REJECTED:
        throw JSONRPCError(RPC_VERIFY_REJECTED,
                           "bridge V2 deposits are closed for good: " + error +
                               "; " + closed_consequence);
    case interfaces::BridgePrevalidationResult::TIP_CHANGED:
        throw JSONRPCError(RPC_VERIFY_REJECTED, error);
    }
    NONFATAL_UNREACHABLE();
}

/**
 * The RECIPIENT_V1 word of this wallet's legacy P2PKH `address`, or of a new
 * legacy address labelled `label` when `address` is null; `destination`
 * receives the address. Every bUSD V2 deposit route mints to this word.
 */
static std::array<unsigned char, 32> WalletBridgeRecipientV1(
    CWallet& wallet, const UniValue& address, const std::string& label,
    CTxDestination& destination)
{
    {
        LOCK(wallet.cs_wallet);
        if (address.isNull()) {
            const auto generated{
                wallet.GetNewDestination(OutputType::LEGACY, label)};
            if (!generated) {
                throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT,
                                   util::ErrorString(generated).original);
            }
            destination = *generated;
        } else {
            destination = DecodeDestination(address.get_str());
            if (!IsValidDestination(destination)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                                   "Invalid B3 address");
            }
        }
        if (!std::holds_alternative<PKHash>(destination)) {
            throw JSONRPCError(
                RPC_INVALID_ADDRESS_OR_KEY,
                "A bridge deposit recipient must be a legacy P2PKH address (RECIPIENT_V1)");
        }
        if (!wallet.IsMine(destination)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                               "The address is not owned by this wallet");
        }
    }
    const PKHash& key_hash{std::get<PKHash>(destination)};
    bridge::RecipientV1 recipient;
    std::copy(key_hash.begin(), key_hash.end(),
              recipient.pubkey_hash.begin());
    const std::array<unsigned char, 32> recipient_word{
        bridge::EncodeRecipientV1(recipient)};
    // The frozen codec refuses an all-zero key hash. No wallet key has one,
    // but never hand out a word B3 would not mint to.
    if (!bridge::DecodeRecipientV1(recipient_word)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           "The address cannot be encoded as RECIPIENT_V1");
    }
    return recipient_word;
}

// ------------------------------------------------- bridge V2 forwarders
//
// Wallet policy for the route (b) deposit forwarder F_d and the route (d3)
// exit forwarder F_x (contracts spec sections 7 and 8; node changes section
// 11.2). Consensus checks only a burn's exit block against the pinned exit
// forwarder; F_d is never a mint condition.

//! B3DepositForwarderFactory.MIN_FLUSH_RAW: the smallest flush (1 USDT).
static constexpr uint64_t BRIDGE_FORWARDER_MIN_FLUSH_RAW{1'000'000};
//! Default relayer fee per forwarder transaction, raw six-decimal units
//! (provisional [I]).
static constexpr uint64_t BRIDGE_FORWARDER_DEFAULT_FEE_PER_TX{1'000'000};
//! LayerZero endpoint id of Ethereum: the dstEid of a route (b) OFT send.
static constexpr uint32_t BRIDGE_FORWARDER_ETHEREUM_EID{30101};
//! Default F_x feeCap: release and forward in one B3RelayBatch call, the F_x
//! fee paying both (B-15; provisional [I]).
static constexpr uint64_t BRIDGE_EXIT_DEFAULT_FEE_CAP{
    2 * BRIDGE_FORWARDER_DEFAULT_FEE_PER_TX};

//! A canonical unsigned integer in [min, max]: a JSON number or a string,
//! AssetUnitsFromValue's grammar (no sign, fraction, exponent or leading
//! zero) except that zero is admitted when `min` is zero.
static uint64_t BridgeUnsignedFromValue(const UniValue& value,
                                        const std::string& name,
                                        const uint64_t min, const uint64_t max)
{
    if (!value.isNum() && !value.isStr()) {
        throw JSONRPCError(RPC_TYPE_ERROR,
                           name + " must be an integer number or string");
    }
    const std::string& text{value.getValStr()};
    if (text.empty() || (text.size() > 1 && text.front() == '0') ||
        !std::all_of(text.begin(), text.end(), [](const char digit) {
            return digit >= '0' && digit <= '9';
        })) {
        throw JSONRPCError(RPC_TYPE_ERROR,
                           name + " must contain only canonical integer digits, without a sign, fraction, or exponent");
    }
    uint64_t out{0};
    const auto [end, error]{
        std::from_chars(text.data(), text.data() + text.size(), out)};
    if (error != std::errc{} || end != text.data() + text.size() ||
        out < min || out > max) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           strprintf("%s must be in the range [%u, %u]", name,
                                     min, max));
    }
    return out;
}

/**
 * OI-6 (arbitrum proposal section 20): an address holding a bridge V2 role
 * (launch or successor vault, origin token, verifier, forwarder factory) is
 * never a withdrawal recipient, an exit `to`, a refund EOA or a derived
 * forwarder, because those contracts give nothing back for funds sent to
 * them.
 */
static void RequireUnreservedBridgeV2Address(
    const Consensus::BridgeV2Params& v2, const Consensus::BridgeEthAddress& address,
    const std::string& name)
{
    if (const auto reason{modern::BridgeV2ReservedEthAddressReason(v2, address)}) {
        throw JSONRPCError(
            RPC_INVALID_ADDRESS_OR_KEY,
            strprintf("%s is %s: funds sent there are lost, the bridge V2 contracts give no claim back (OI-6)",
                      name, std::string{*reason}));
    }
}

/**
 * The EIP-191 proof that `refund_eoa` is an Ethereum EOA the user controls
 * (contracts spec section 7.3; bridge/eip191.h): `signature` must be the
 * 65-byte r || s || v personal_sign signature of RefundEoaMessageV2 under
 * this chain's modern domain, recovering exactly `refund_eoa`. Returns the
 * message; every error ends with it, verbatim, so the user can sign it.
 */
static std::string RequireRefundEoaProof(const Consensus::Params& params,
                                         const Consensus::BridgeEthAddress& refund_eoa,
                                         const UniValue& signature,
                                         const std::string& name)
{
    const auto domain{modern::ModernChainDomain(
        params.hashGenesisBlock, params.legacy_final_hash.value_or(uint256{}))};
    if (!domain) {
        throw JSONRPCError(RPC_MISC_ERROR,
                           "The modern chain domain is not pinned");
    }
    const std::string message{bridge::RefundEoaMessageV2(*domain, refund_eoa)};
    const std::string how{
        " Sign exactly this text (lines separated by \\n, no trailing newline) "
        "as an EIP-191 personal_sign message with the refund EOA's key; a Safe "
        "or an ERC-4337 account cannot:\n" + message};
    if (!signature.isStr()) {
        throw JSONRPCError(RPC_TYPE_ERROR,
                           name + " must be a hex string." + how);
    }
    std::string encoded{signature.get_str()};
    if (encoded.starts_with("0x") || encoded.starts_with("0X")) {
        encoded.erase(0, 2);
    }
    if (encoded.size() != 2 * bridge::EIP191_SIGNATURE_SIZE || !IsHex(encoded)) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            name + " must be exactly 65 bytes of hexadecimal (r || s || v)." + how);
    }
    const std::vector<unsigned char> bytes{ParseHex(encoded)};
    const auto signer{bridge::RecoverEip191SignerV2(message, bytes)};
    if (!signer || *signer != refund_eoa) {
        throw JSONRPCError(
            RPC_INVALID_ADDRESS_OR_KEY,
            name + " is not the refund EOA's EIP-191 signature of the refund EOA message." + how);
    }
    return message;
}

static UniValue ExitForwarderParamsJson(const modern::ExitForwarderParamsV2& p)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("dst_eid", p.dst_eid);
    out.pushKV("to", "0x" + HexStr(p.to));
    out.pushKV("max_bridge_fee_bps", p.max_bridge_fee_bps);
    out.pushKV("fee_cap", p.fee_cap);
    out.pushKV("refund_eoa", "0x" + HexStr(p.refund_eoa));
    return out;
}

//! bridgewithdraw's route (d3): the exit forwarder F_x of options.arbitrum.
struct ArbitrumExitRoute {
    modern::ExitForwarderParamsV2 params{};
    Consensus::BridgeEthAddress recipient{}; // the Arbitrum address
    Consensus::BridgeEthAddress account{};   // F_x, the burn's recipient
    uint256 salt{};
};

/**
 * Parses options.arbitrum of a bridge V2 burn of `amount` raw units: p =
 * {pinned dst_eid, word(recipient), max_bridge_fee_bps, fee_cap,
 * refund_eoa}, its F_x under the pinned exit forwarder, and the wallet's
 * refusals (a reserved recipient, refund EOA or F_x, an unproven refund EOA,
 * an amount a forward at the full fee would empty). Consensus re-checks p
 * and F_x (bridge_state.cpp, VerifyBridgeBurnV2).
 */
static ArbitrumExitRoute ParseArbitrumExitRoute(const Consensus::Params& params,
                                                const UniValue& value,
                                                const uint64_t amount)
{
    // The caller checked ExitForwarderRouteUnavailableReasonV2.
    const Consensus::BridgeV2Params& v2{*Assert(params.bridge_v2)};
    const Consensus::BridgeV2ExitForwarderPins& pins{*Assert(v2.exit_forwarder)};
    if (!value.isObject()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "options.arbitrum must be an object");
    }
    const std::set<std::string> allowed{"recipient", "refund_eoa",
                                        "refund_eoa_signature", "fee_cap",
                                        "max_bridge_fee_bps"};
    for (const std::string& key : value.getKeys()) {
        if (!allowed.contains(key)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               strprintf("Unknown arbitrum option '%s'", key));
        }
    }
    for (const char* key : {"recipient", "refund_eoa", "refund_eoa_signature"}) {
        if (!value.exists(key)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               strprintf("options.arbitrum.%s is required", key));
        }
    }
    ArbitrumExitRoute out;
    out.recipient = ParseEthAddressField(value["recipient"], "arbitrum.recipient");
    const Consensus::BridgeEthAddress refund_eoa{
        ParseEthAddressField(value["refund_eoa"], "arbitrum.refund_eoa")};
    const uint64_t fee_cap{
        value.exists("fee_cap")
            ? BridgeUnsignedFromValue(value["fee_cap"], "arbitrum.fee_cap", 0,
                                      std::numeric_limits<uint64_t>::max())
            : BRIDGE_EXIT_DEFAULT_FEE_CAP};
    const uint64_t max_bridge_fee_bps{
        value.exists("max_bridge_fee_bps")
            ? BridgeUnsignedFromValue(value["max_bridge_fee_bps"],
                                      "arbitrum.max_bridge_fee_bps", 0,
                                      modern::FORWARDER_MAX_BRIDGE_FEE_BPS)
            : 0};
    // forward() sends balance - fee and needs it non-zero: at the full fee
    // a release of `amount` must still leave something to send.
    if (amount <= fee_cap) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            "amount must exceed arbitrum.fee_cap: a forward at the full fee must still send something");
    }
    RequireUnreservedBridgeV2Address(v2, out.recipient, "arbitrum.recipient");
    RequireUnreservedBridgeV2Address(v2, refund_eoa, "arbitrum.refund_eoa");
    RequireRefundEoaProof(params, refund_eoa, value["refund_eoa_signature"],
                          "arbitrum.refund_eoa_signature");

    out.params = modern::ExitForwarderParamsV2{
        .dst_eid = pins.dst_eid,
        .to = modern::ForwarderAddressWord(out.recipient),
        .max_bridge_fee_bps = static_cast<uint16_t>(max_bridge_fee_bps),
        .fee_cap = fee_cap,
        .refund_eoa = refund_eoa,
    };
    if (!modern::ExitForwarderParamsValidV2(out.params, pins.dst_eid)) {
        throw JSONRPCError(
            RPC_INVALID_PARAMETER,
            "The arbitrum exit parameters are invalid for the pinned exit forwarder");
    }
    out.account = modern::ExitForwarderAddressV2(pins, out.params);
    out.salt = modern::ExitForwarderSaltV2(out.params);
    RequireUnreservedBridgeV2Address(v2, out.account,
                                     "The derived exit forwarder");
    return out;
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
        ParseAssetRpcOptions(request.params[options_index],
                             /*flowmesh_seat=*/false,
                             /*flowmesh_deposit=*/false, /*asset_burn=*/burn)};
    const ChainSnapshot snapshot{SnapshotChainState(*wallet, false)};
    const Consensus::Params& params{Params().GetConsensus()};
    // Node changes section 7.5: a plain burn of bUSD V2 appends no leaf V2,
    // so it destroys the holder's claim on the Vault V2 reserve for good.
    // bridgewithdraw is the redeeming burn; this one needs an explicit
    // opt-in. Without a bridge V2 envelope (every shipped network) there is
    // no bUSD V2 asset id and nothing changes.
    if (burn && !options.allow_unredeemable_burn) {
        const auto busd_v2{modern::ConfiguredBridgeV2AssetId(params)};
        if (busd_v2 && asset == *busd_v2) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                "burnasset would destroy bridge V2 bUSD without a withdrawal "
                "leaf, giving up its claim on the Vault V2 reserve; use "
                "bridgewithdraw to redeem it, or set "
                "options.allow_unredeemable_burn=true");
        }
    }
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
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO,
             "Positive integer atomic asset units, as a JSON number or string; no display-decimal scaling"},
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
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO,
             "Positive integer atomic asset units, as a JSON number or string; no display-decimal scaling"},
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
                 {"allow_unredeemable_burn", RPCArg::Type::BOOL, RPCArg::Default{false},
                  "Allow a plain burn of the bridge V2 bUSD asset (regtest only). Such a burn creates no "
                  "withdrawal leaf and gives up its claim on the Vault V2 reserve; bridgewithdraw redeems "
                  "it instead"},
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
            // ParseHexV includes malformed input in its error. Never echo a
            // secret or allocate an unbounded decoded buffer on this path.
            const auto& text{request.params[0].get_str()};
            if (text.size() != 2 * bls::SECRET_SIZE || !IsHex(text)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "blssecret must be exactly 32 bytes of hex");
            }
            const auto bytes{ParseHex(text)};
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

RPCHelpMan exportflowmeshkey()
{
    return RPCHelpMan{
        "exportflowmeshkey",
        "Export exactly one existing FN FlowMesh BLS secret held by the selected wallet. "
        "The plaintext secret authorizes FN votes and historical FN rewards. "
        "This does not export or migrate the signer journal, stop an existing signer, "
        "or authorize running a duplicate signer. Keep the result private. "
        "An encrypted wallet must be fully unlocked; a locked wallet with armed staking keys is insufficient.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"bls_pubkey", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "Exactly one canonical 48-byte compressed FN BLS public key (96 hex characters)"},
            {"ack_risk", RPCArg::Type::BOOL, RPCArg::Optional::NO,
             "Must be true: the plaintext secret authorizes FN votes and historical FN rewards; export does not migrate the signer journal or authorize duplicate signing"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Sensitive plaintext key export", {
            {RPCResult::Type::STR_HEX, "bls_pubkey", "Selected 48-byte FN BLS public key"},
            {RPCResult::Type::STR_HEX, "blssecret", "Sensitive 32-byte big-endian BLS secret scalar"},
            {RPCResult::Type::STR, "warning", "Key authority and signer-journal warning"},
        }},
        RPCExamples{HelpExampleCli("exportflowmeshkey", "\"<bls_pubkey>\" true")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const auto wallet{GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            if (!request.params[1].isBool()) {
                throw JSONRPCError(RPC_TYPE_ERROR, "ack_risk must be a boolean");
            }
            if (!request.params[1].get_bool()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "Set ack_risk=true to acknowledge plaintext FN key export and signer-journal risks");
            }
            // Never echo malformed input: a caller may accidentally paste a secret.
            const auto& text{request.params[0].get_str()};
            if (text.size() != 2 * bls::PUBKEY_SIZE || !IsHex(text)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "bls_pubkey must be exactly 96 hex characters");
            }
            const auto pubkey{bls::PublicKey::Decode(ParseHex(text))};
            if (!pubkey) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "bls_pubkey must be a canonical compressed BLS public key");
            }
            LOCK(wallet->cs_wallet);
            EnsureSigningWallet(*wallet);
            if (!wallet->HasFlowMeshBlsKey(pubkey->Compressed())) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "This wallet has no matching FlowMesh BLS key");
            }
            const auto key{wallet->GetFlowMeshBlsKey(pubkey->Compressed())};
            if (!key) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "Unable to read the selected FlowMesh BLS key");
            }
            struct ExportBytes {
                std::array<unsigned char, bls::SECRET_SIZE> value;
                ~ExportBytes() { memory_cleanse(value.data(), value.size()); }
            } secret{key->Bytes()};
            UniValue result{UniValue::VOBJ};
            result.pushKV("bls_pubkey", HexStr(pubkey->Compressed()));
            result.pushKV("blssecret", HexStr(secret.value));
            result.pushKV("warning", "This plaintext key authorizes FN votes and historical FN rewards. Export does not migrate the signer journal, stop an existing signer, or authorize running a duplicate signer. Keep the result private.");
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
             "Number or string: decimal B3 units for native deposits, or positive integer atomic units for colored assets (no display-decimal scaling)"},
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
                        status && !status->error.empty()
                            ? "FlowMesh market is paused: " + status->error
                            : "FlowMesh market is paused; user deposits are temporarily unavailable");
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
                /*disintegration=*/0, "flowmesh-deposit",
                options.market_bootstrap ? std::nullopt : market)};
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
        "Ethereum light-client update, execution-backfill, or (from the bridge "
        "V2 activation height, regtest only) Ethereum state-observation (kind "
        "9), finality-conflict (kind 13) or vault-sweep (kind 16) type-10 "
        "record. A finality-conflict "
        "record is evidence of a digest-V2 certificate, signed by the fresh "
        "lineage's current or next committee, that differs from B3's statement "
        "at its checkpoint: once a block holds it B3 refuses bridge V2 burns "
        "and closes deposits for good. A vault-sweep record (B-23, from a "
        "pinned successor's migration height; getbridgev2info successor.sweep "
        "gives its payload) appends one leaf V2 moving the retired launch "
        "vault entry's reserve, capped by the wind-down headroom of its latest "
        "reserve observation, to the successor vault; prove that leaf with "
        "getbridgewithdrawalproofv2 at binding_vout. "
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
            {RPCResult::Type::STR, "record_kind", "bootstrap, update, execution-backfill, eth-state-observation, finality-conflict, or vault-sweep"},
            {RPCResult::Type::NUM, "binding_vout", "Signed B3MC bridge binding output index"},
            {RPCResult::Type::NUM, "sweep_amount_raw", /*optional=*/true, "vault-sweep only: raw six-decimal amount the sweep leaf moves from the launch vault to the successor vault"},
            {RPCResult::Type::STR, "proof_rpc_after_confirmation", /*optional=*/true, "vault-sweep only: copyable proof lookup of the sweep leaf (its source is binding_vout); replace <certificate_block> with the inclusion block of an accepted fresh-lineage certificate covering it"},
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
                    bridge::BridgeRecordKindV1::EXECUTION_BACKFILL &&
                parsed.decoded.kind !=
                    bridge::BridgeRecordKindV1::ETH_STATE_OBSERVATION &&
                parsed.decoded.kind !=
                    bridge::BridgeRecordKindV1::FINALITY_CONFLICT &&
                parsed.decoded.kind !=
                    bridge::BridgeRecordKindV1::VAULT_SWEEP) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "submitbridgecarrier accepts only bootstrap, update, or execution-backfill records, "
                    "and eth-state-observation, finality-conflict or vault-sweep records from the bridge V2 "
                    "activation height");
            }
            const bool sweep{parsed.decoded.kind ==
                             bridge::BridgeRecordKindV1::VAULT_SWEEP};

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
            // A vault sweep (B-23) appends exactly one consensus-derived
            // leaf V2 whose source is the binding output; every other
            // carrier authorizes nothing.
            const std::optional<node::BridgeWithdrawalV2Request>& swept{
                authorization.withdrawal_v2};
            if (authorization.mint || authorization.withdrawal ||
                authorization.decentralized_withdrawal ||
                (sweep ? !swept || !swept->sweep ||
                             swept->burn_output_index != 1 ||
                             swept->transaction_id != created.tx->GetHash()
                       : swept.has_value())) {
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
            if (sweep) {
                result.pushKV("sweep_amount_raw", swept->withdrawal.amount);
                result.pushKV("proof_rpc_after_confirmation",
                              "getbridgewithdrawalproofv2 " +
                                  created.tx->GetHash().GetHex() + " " +
                                  std::to_string(swept->burn_output_index) +
                                  " <certificate_block>");
            }
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
        "and mint caps before broadcast. From the bridge V2 activation height "
        "(regtest only) the payload must name the V2 vault entry id and the "
        "command mints bUSD V2 for a deposit into the pinned Vault V2; from a "
        "pinned B-23 successor's migration height it may also name the "
        "successor's entry id for a deposit into the successor vault (the "
        "same bUSD V2 asset); below V nothing changes. A signed B3MC policy-9 "
        "cell binds the "
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
            {RPCResult::Type::STR_HEX, "asset_id", "Configured chain-bound bUSD asset id (the vault-bound bUSD V2 asset id from the bridge V2 activation height)"},
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
            // Bridge V2 rides on the V1 bridge tracker and gates (V >= A3).
            const Consensus::BridgeAssetParams& bridge_params{
                RequireBridgeForNextBlock(snapshot.next_height)};
            const Consensus::Params& params{Params().GetConsensus()};
            // From V every kind 3 record is a V2 mint (node changes section
            // 3.4): it names the V2 vault entry id and mints the vault-bound
            // bUSD V2 asset. The V1 approval window governs only V1 mints.
            const bool bridge_v2{
                params.BridgeV2RulesActive(snapshot.next_height)};
            if (!bridge_v2 && bridge_params.approval_last_height &&
                snapshot.next_height > *bridge_params.approval_last_height) {
                throw JSONRPCError(RPC_MISC_ERROR,
                                   "New bridge mints are no longer approved");
            }
            const auto asset{bridge_v2
                                 ? modern::ConfiguredBridgeV2AssetId(params)
                                 : modern::ConfiguredBridgeAssetId(params)};
            // A V2 mint names the launch entry (also once it is retired:
            // late deposits into it still mint and are swept) or, from h_m,
            // the B-23 successor (node changes section 7.6).
            const bool registry_ok{
                bridge_v2
                    ? modern::ResolveBridgeV2Entry(params, mint->registry_id,
                                                   snapshot.next_height)
                          .has_value()
                    : modern::ConfiguredBridgeRegistryId(params) ==
                          std::optional<uint256>{mint->registry_id}};
            if (!asset || !registry_ok) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    bridge_v2
                        ? "The MINT payload does not name the active bridge V2 vault entry"
                        : "The MINT payload does not name the active bridge registry");
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
            if (!authorization.mint || authorization.HasWithdrawal() ||
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

RPCHelpMan getbridgedepositrecipient()
{
    return RPCHelpMan{
        "getbridgedepositrecipient",
        "Return the route (a) instructions for a bridge V2 deposit that "
        "mints bUSD V2 to this wallet (regtest only, from the bridge V2 "
        "activation height): approve the Vault V2 for the token amount, "
        "then call its deposit(uint256 amount, bytes32 b3Recipient) from an "
        "externally owned account with recipient_v1 as b3Recipient. A relayer "
        "proves the Deposit event and claimbridgedeposit mints it. From a "
        "pinned B-23 successor's migration height the target is the "
        "successor vault (getbridgev2info deposit_vault), never the retired "
        "launch vault. Refused "
        "once B3 has closed bridge V2 deposits for good (getbridgev2info "
        "deposits: a sticky conflict, a broken fresh lineage, or a latest "
        "VERIFIER_E showing an impaired verifier or Verifier V2 "
        "depositsClosed): B3 still mints every proven deposit, because the "
        "vault cannot refund, but bUSD V2 minted after that can never be "
        "redeemed. Nothing is signed or broadcast.\n",
        {
            {"address", RPCArg::Type::STR,
             RPCArg::DefaultHint{"a new legacy address labelled b3-busd-bridge-receive"},
             "This wallet's legacy P2PKH address that receives the bUSD V2"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Deposit instructions", {
            {RPCResult::Type::STR, "address", "B3 P2PKH address that receives the bUSD V2"},
            {RPCResult::Type::STR_HEX, "recipient_v1", "The deposit's b3Recipient: RECIPIENT_V1 bytes32, 0x-prefixed (11 zero bytes, version 0x01, the 20-byte P2PKH key hash)"},
            {RPCResult::Type::STR_HEX, "vault", "Vault V2 address, 0x-prefixed: the deposit target (the entry that takes deposits for the next block)"},
            {RPCResult::Type::STR_HEX, "token", "Origin token address the vault takes, 0x-prefixed"},
            {RPCResult::Type::NUM, "origin_chain_id", "Ethereum chain id of the vault"},
            {RPCResult::Type::STR_HEX, "asset_id", "bUSD V2 AssetId minted for the deposit (display order)"},
            {RPCResult::Type::NUM, "max_deposit_raw", "Largest admitted deposit in raw six-decimal token units"},
            {RPCResult::Type::STR, "deposit_signature", "Vault V2 deposit function signature"},
        }},
        RPCExamples{HelpExampleCli("getbridgedepositrecipient", "") +
                    HelpExampleRpc("getbridgedepositrecipient", "\"<b3_address>\"")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();

            // B3's deposit status first, so a refusal consumes no address.
            // Only VALID continues; every other status, an unexpected one
            // included, throws (fail closed).
            RequireBridgeV2DepositsOpen(
                *wallet,
                "a deposit made now mints bUSD V2 that can never be redeemed");
            const Consensus::Params& params{Params().GetConsensus()};
            const auto asset{modern::ConfiguredBridgeV2AssetId(params)};
            if (!params.bridge_v2 || !asset) {
                throw JSONRPCError(RPC_MISC_ERROR,
                                   "Bridge V2 is not configured on this chain");
            }
            // The entry that takes deposits for the next block: the B-23
            // successor from h_m, whose shared AssetId mints the same bUSD
            // V2 (node changes section 7.6).
            const std::optional<int> tip_height{wallet->chain().getHeight()};
            const Consensus::BridgeV2VaultEntry* entry_ptr{
                params.bridge_v2->Entry(modern::ActiveBridgeV2Role(
                    params, tip_height ? *tip_height + 1 : 0))};
            if (entry_ptr == nullptr) {
                throw JSONRPCError(RPC_MISC_ERROR,
                                   "The bridge V2 deposit vault entry is unavailable");
            }
            const Consensus::BridgeV2VaultEntry& entry{*entry_ptr};

            CTxDestination destination;
            const std::array<unsigned char, 32> recipient_word{
                WalletBridgeRecipientV1(*wallet, request.params[0],
                                        "b3-busd-bridge-receive",
                                        destination)};

            UniValue result{UniValue::VOBJ};
            result.pushKV("address", EncodeDestination(destination));
            result.pushKV("recipient_v1", "0x" + HexStr(recipient_word));
            result.pushKV("vault", "0x" + HexStr(entry.vault_address));
            result.pushKV("token", "0x" + HexStr(entry.token_address));
            result.pushKV("origin_chain_id", entry.origin_chain_id);
            result.pushKV("asset_id", asset->GetHex());
            result.pushKV("max_deposit_raw", entry.max_deposit_raw);
            result.pushKV("deposit_signature", "deposit(uint256,bytes32)");
            return result;
        }};
}

RPCHelpMan bridgeforwarderaddress()
{
    return RPCHelpMan{
        "bridgeforwarderaddress",
        "Derive a fresh route (b) deposit forwarder F_d for a bridge V2 deposit "
        "that mints bUSD V2 to this wallet (regtest only, from the bridge V2 "
        "activation height, with a pinned deposit forwarder factory; contracts "
        "spec section 7). The user sends USDT0 on Arbitrum with the OFT send() "
        "of oft_send, amountLD the delivery and minAmountLD = amountLD; "
        "LayerZero credits the forwarder with USDT on Ethereum by plain "
        "transfer. A relayer, or anyone from a codeless externally owned "
        "account, then deploys it (deploy(params)) and flushes it "
        "(flush(params, fee)) into the launch Vault V2 with exactly one "
        "deposit(amount, b3Recipient) per flush, every fee within the "
        "lifetime fee_cap; claimbridgedeposit mints the proven Deposit. The "
        "forwarder is the CREATE2 address of B3Forwarder under the pinned "
        "factory and init-code hash, so it can be paid before it exists. "
        "Before not_after only refund_eoa can take funds back "
        "(refund(params, token, to, amount)); after it anyone may "
        "returnToOwner(params, fee) them to refund_eoa. refund_eoa must be an "
        "Ethereum externally owned account this user controls, proven by an "
        "EIP-191 personal_sign signature of refund_eoa_message (an error names "
        "the exact text): Safes and ERC-4337 accounts cannot sign it. One "
        "forwarder carries at most the vault's max_deposit_raw (OI-2), so its "
        "first flush deposits the whole delivery whatever fee within fee_cap "
        "the flusher takes; fee_cap = fee_per_tx x 4 pays deploy, two flushes "
        "(one for a repeat arrival) and a returnToOwner. A larger delivery "
        "waits for a zero-fee flush, a refund or returnToOwner after "
        "not_after: split it across forwarders. not_after is at most 30 days "
        "out and, once the bridge V2 light client is bootstrapped, ends 24 "
        "hours before its horizon (getbridgev2info light_client.horizon_time); "
        "within 7 days of the horizon no forwarder is derived (node changes "
        "section 12.3). Each call takes a new address and a fresh not_after by "
        "default, hence a fresh forwarder: never reuse one once flushed. Hand "
        "params to the relayer, or call publish(params) on the factory. "
        "Refused, like getbridgedepositrecipient, once B3 has closed bridge V2 "
        "deposits for good, and from a pinned B-23 successor's migration "
        "height (the factory deposits only into the launch vault). Never send "
        "USDT or USDT0 to the vault directly (OI-6). Nothing is signed or "
        "broadcast.\n",
        {
            {"refund_eoa", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "Ethereum externally owned account this user controls, 20 bytes with optional 0x prefix: the only account that may refund the forwarder, and the one returnToOwner pays"},
            {"refund_eoa_signature", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "65-byte r || s || v EIP-191 personal_sign signature by refund_eoa of refund_eoa_message, with optional 0x prefix"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED,
             "Derivation options", {
                 {"address", RPCArg::Type::STR,
                  RPCArg::DefaultHint{"a new legacy address labelled b3-busd-bridge-forwarder"},
                  "This wallet's legacy P2PKH address that receives the bUSD V2"},
                 {"amount", RPCArg::Type::NUM,
                  RPCArg::DefaultHint{"the vault's max_deposit_raw"},
                  "Raw six-decimal USDT the user expects to deliver: at least 1000000 (MIN_FLUSH_RAW) and at most the vault's max_deposit_raw (OI-2)"},
                 {"fee_per_tx", RPCArg::Type::NUM,
                  RPCArg::Default{BRIDGE_FORWARDER_DEFAULT_FEE_PER_TX},
                  "Relayer fee per forwarder transaction in raw six-decimal units (provisional)"},
                 {"not_after", RPCArg::Type::NUM,
                  RPCArg::DefaultHint{"now + 30 days"},
                  "Unix time in seconds until which the forwarder may be flushed: after now, at most now + 30 days and, with a bootstrapped light client, at most its horizon - 24 hours"},
             }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Deposit forwarder instructions", {
            {RPCResult::Type::STR, "address", "B3 P2PKH address that receives the bUSD V2"},
            {RPCResult::Type::STR_HEX, "recipient_v1", "params.b3_recipient: RECIPIENT_V1 bytes32, 0x-prefixed (11 zero bytes, version 0x01, the 20-byte P2PKH key hash)"},
            {RPCResult::Type::STR_HEX, "forwarder", "The deposit forwarder F_d = factory.accountOf(params), 0x-prefixed: the OFT send's recipient"},
            {RPCResult::Type::STR_HEX, "factory", "Pinned B3DepositForwarderFactory address, 0x-prefixed"},
            {RPCResult::Type::STR_HEX, "init_code_hash", "Pinned FORWARDER_INIT_CODE_HASH, keccak256 of the B3Forwarder creation code (raw bytes)"},
            {RPCResult::Type::STR_HEX, "vault", "Launch Vault V2 address the forwarder deposits into, 0x-prefixed"},
            {RPCResult::Type::STR_HEX, "token", "Origin token address the forwarder flushes (USDT on Ethereum), 0x-prefixed"},
            {RPCResult::Type::OBJ, "params", "DepositParams p for deploy, flush, refund, returnToOwner and publish", {
                {RPCResult::Type::STR_HEX, "vault", "p.vault"},
                {RPCResult::Type::STR_HEX, "b3_recipient", "p.b3Recipient (RECIPIENT_V1)"},
                {RPCResult::Type::STR_HEX, "refund_eoa", "p.refundEOA"},
                {RPCResult::Type::NUM, "fee_cap", "p.feeCap: lifetime bound on every fee the forwarder pays (raw six-decimal units)"},
                {RPCResult::Type::NUM, "not_after", "p.notAfter: Unix time until which flush works; returnToOwner only after it"},
            }},
            {RPCResult::Type::STR_HEX, "salt", "depositSalt(p), the CREATE2 salt (raw bytes)"},
            {RPCResult::Type::NUM, "amount_raw", "The expected delivery, at most max_deposit_raw"},
            {RPCResult::Type::NUM, "fee_per_tx", "The fee per forwarder transaction fee_cap is sized for"},
            {RPCResult::Type::NUM, "max_deposit_raw", "Largest delivery one forwarder carries: the vault's MAX_DEPOSIT_RAW, which one flush deposits"},
            {RPCResult::Type::NUM, "min_flush_raw", "Smallest flush (the factory's MIN_FLUSH_RAW)"},
            {RPCResult::Type::NUM, "horizon_time", /*optional=*/true, "With a bootstrapped bridge V2 light client: its estimated horizon (getbridgev2info light_client.horizon_time), which caps not_after at horizon_time - 24 hours"},
            {RPCResult::Type::OBJ, "oft_send", "Route (b) SendParam fields for the Arbitrum USDT0 OFT send(): amountLD is the delivery and minAmountLD = amountLD", {
                {RPCResult::Type::NUM, "dst_eid", "LayerZero endpoint id of Ethereum (30101)"},
                {RPCResult::Type::STR_HEX, "to", "The forwarder as a bytes32 word, 0x-prefixed"},
                {RPCResult::Type::STR, "compose_msg", "Empty: route (b) never composes"},
                {RPCResult::Type::STR, "oft_cmd", "Empty"},
            }},
            {RPCResult::Type::STR, "refund_eoa_message", "The exact EIP-191 text refund_eoa signed"},
            {RPCResult::Type::ARR, "warnings", "What this forwarder must never be used for", {
                {RPCResult::Type::STR, "", "Warning"},
            }},
        }},
        RPCExamples{HelpExampleCli("bridgeforwarderaddress",
                                   "\"0x<refund_eoa>\" \"0x<signature>\"") +
                    HelpExampleRpc("bridgeforwarderaddress",
                                   "\"0x<refund_eoa>\", \"0x<signature>\", {\"amount\": 25000000000}")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();

            const UniValue& options{request.params[2]};
            if (!options.isNull()) {
                if (!options.isObject()) {
                    throw JSONRPCError(RPC_TYPE_ERROR, "options must be an object");
                }
                const std::set<std::string> allowed{"address", "amount",
                                                    "fee_per_tx", "not_after"};
                for (const std::string& key : options.getKeys()) {
                    if (!allowed.contains(key)) {
                        throw JSONRPCError(RPC_INVALID_PARAMETER,
                                           strprintf("Unknown option '%s'", key));
                    }
                }
            }

            // Every refusal comes before step 8, so none consumes an address.
            // 1. A pinned deposit forwarder whose vault takes deposits for
            // the next block. The factory's VAULT is the launch vault; a
            // B-23 successor pins no factory of its own.
            const Consensus::Params& params{Params().GetConsensus()};
            const std::optional<int> tip_height{wallet->chain().getHeight()};
            if (const auto reason{modern::DepositForwarderUnavailableReasonV2(
                    params, tip_height ? *tip_height + 1 : 0)}) {
                throw JSONRPCError(RPC_MISC_ERROR, std::string{*reason});
            }
            const Consensus::BridgeV2Params& v2{*params.bridge_v2};
            const Consensus::BridgeV2DepositForwarderPins& pins{*v2.deposit_forwarder};
            const Consensus::BridgeV2VaultEntry& entry{v2.vault};
            const uint64_t max_deposit_raw{
                static_cast<uint64_t>(entry.max_deposit_raw)};

            // 2. B3's deposit status.
            RequireBridgeV2DepositsOpen(
                *wallet,
                "a forwarder deposit made now mints bUSD V2 that can never be redeemed");

            // 3. The light-client horizon (node changes section 12.3): no
            // forwarder within 7 days of it; notAfter ends 24 hours before
            // it. Unknown until the V2 store is bootstrapped.
            const int64_t now{GetTime()};
            if (now <= 0) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "The node clock is unset");
            }
            std::optional<interfaces::BridgeV2LightClientHead> head;
            {
                std::string error;
                switch (wallet->chain().bridgeV2LightClientHead(head, error)) {
                case interfaces::BridgePrevalidationResult::VALID:
                    break;
                case interfaces::BridgePrevalidationResult::STATE_UNAVAILABLE:
                    throw JSONRPCError(RPC_DATABASE_ERROR, error);
                case interfaces::BridgePrevalidationResult::RULES_INACTIVE:
                case interfaces::BridgePrevalidationResult::REJECTED:
                case interfaces::BridgePrevalidationResult::TIP_CHANGED:
                    throw JSONRPCError(RPC_MISC_ERROR, error);
                }
            }
            const std::optional<int64_t> horizon_time{
                head ? std::optional{modern::BridgeV2LightClientHorizonTime(
                           v2.light_client.fork_schedule_valid_through_epoch,
                           head->finalized_slot, head->finalized_execution_timestamp)}
                     : std::nullopt};
            const auto latest_not_after{
                modern::DepositForwarderLatestNotAfterV2(now, horizon_time)};
            if (!latest_not_after) {
                throw JSONRPCError(
                    RPC_VERIFY_REJECTED,
                    strprintf("The bridge V2 light-client horizon (estimated at %d) is less than 7 days away: a forwarder deposit made now could wait for the extension hard fork, so none is derived (node changes section 12.3)",
                              horizon_time.value_or(0)));
            }

            // 4. The refund EOA: never a bridge V2 contract (OI-6).
            const Consensus::BridgeEthAddress refund_eoa{
                ParseEthAddressField(request.params[0], "refund_eoa")};
            RequireUnreservedBridgeV2Address(v2, refund_eoa, "refund_eoa");

            // 5. ... and an EOA the user controls (EIP-191).
            const std::string message{RequireRefundEoaProof(
                params, refund_eoa, request.params[1], "refund_eoa_signature")};

            // 6. One delivery of at most max_deposit_raw (OI-2), so a single
            // flush deposits it whatever fee within the cap is taken, and the
            // lifetime fee cap fee_per_tx x 4.
            const uint64_t amount{
                options["amount"].isNull()
                    ? max_deposit_raw
                    : BridgeUnsignedFromValue(options["amount"], "amount",
                                              BRIDGE_FORWARDER_MIN_FLUSH_RAW,
                                              static_cast<uint64_t>(MAX_MONEY))};
            if (amount > max_deposit_raw) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    strprintf("amount exceeds max_deposit_raw (%u): one forwarder carries one vault deposit, so a larger delivery would wait for a zero-fee flush, a refund or returnToOwner (OI-2); split it across several forwarders",
                              max_deposit_raw));
            }
            const uint64_t fee_per_tx{
                options["fee_per_tx"].isNull()
                    ? BRIDGE_FORWARDER_DEFAULT_FEE_PER_TX
                    : BridgeUnsignedFromValue(options["fee_per_tx"], "fee_per_tx",
                                              1, static_cast<uint64_t>(MAX_MONEY))};
            // fee_per_tx <= MAX_MONEY, so fee_per_tx x 4 always fits uint64.
            const uint64_t fee_cap{
                *CHECK_NONFATAL(modern::DepositForwarderFeeCapV2(fee_per_tx))};

            // 7. notAfter: a fresh one per derivation, at most the latest.
            const uint64_t latest{static_cast<uint64_t>(*latest_not_after)};
            const uint64_t not_after{
                options["not_after"].isNull()
                    ? latest
                    : BridgeUnsignedFromValue(options["not_after"], "not_after",
                                              static_cast<uint64_t>(now) + 1,
                                              latest)};

            // 8. The B3 recipient: a new address by default, so a fresh F_d.
            CTxDestination destination;
            const std::array<unsigned char, 32> recipient_word{
                WalletBridgeRecipientV1(*wallet, options["address"],
                                        "b3-busd-bridge-forwarder",
                                        destination)};

            // 9. F_d = accountOf(p) under the pinned factory.
            const modern::DepositForwarderParamsV2 p{
                .vault = entry.vault_address,
                .b3_recipient = recipient_word,
                .refund_eoa = refund_eoa,
                .fee_cap = fee_cap,
                .not_after = not_after,
            };
            if (!modern::DepositForwarderParamsValidV2(
                    p, entry.vault_address, entry.token_address, pins.factory)) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "The deposit forwarder parameters are invalid for the pinned factory");
            }
            const Consensus::BridgeEthAddress forwarder{
                modern::DepositForwarderAddressV2(pins, p)};
            RequireUnreservedBridgeV2Address(v2, forwarder,
                                             "The derived deposit forwarder");

            UniValue params_json{UniValue::VOBJ};
            params_json.pushKV("vault", "0x" + HexStr(p.vault));
            params_json.pushKV("b3_recipient", "0x" + HexStr(p.b3_recipient));
            params_json.pushKV("refund_eoa", "0x" + HexStr(p.refund_eoa));
            params_json.pushKV("fee_cap", p.fee_cap);
            params_json.pushKV("not_after", p.not_after);

            UniValue oft_send{UniValue::VOBJ};
            oft_send.pushKV("dst_eid", BRIDGE_FORWARDER_ETHEREUM_EID);
            oft_send.pushKV("to", "0x" + HexStr(modern::ForwarderAddressWord(forwarder)));
            oft_send.pushKV("compose_msg", "");
            oft_send.pushKV("oft_cmd", "");

            UniValue warnings{UniValue::VARR};
            warnings.push_back(
                "Never send USDT or USDT0 to Vault V2, its token or a forwarder factory directly: a plain transfer mints nothing and is lost (OI-6). Only an OFT send to the forwarder, or a vault deposit(amount, b3Recipient) call, mints bUSD V2");
            warnings.push_back(
                "Use this forwarder for one deposit of at most max_deposit_raw and derive a fresh one for the next. Its fee cap pays deploy, two flushes (one for a repeat arrival) and a returnToOwner; what no paid flush moves before not_after returns to refund_eoa after it (OI-2)");
            if (!horizon_time) {
                warnings.push_back(
                    "The bridge V2 light client is not bootstrapped yet, so not_after is not capped at its horizon; B3 mints the flushed deposit once the light client covers it");
            }
            if (amount < BRIDGE_FORWARDER_MIN_FLUSH_RAW + fee_per_tx) {
                warnings.push_back(
                    "A delivery of amount_raw leaves less than min_flush_raw after a fee_per_tx fee, so a relayer cannot flush it at that fee; flush it yourself with a lower fee, refund it, or wait for returnToOwner");
            }

            UniValue result{UniValue::VOBJ};
            result.pushKV("address", EncodeDestination(destination));
            result.pushKV("recipient_v1", "0x" + HexStr(recipient_word));
            result.pushKV("forwarder", "0x" + HexStr(forwarder));
            result.pushKV("factory", "0x" + HexStr(pins.factory));
            result.pushKV("init_code_hash", "0x" + HexStr(pins.init_code_hash));
            result.pushKV("vault", "0x" + HexStr(entry.vault_address));
            result.pushKV("token", "0x" + HexStr(entry.token_address));
            result.pushKV("params", std::move(params_json));
            result.pushKV("salt", "0x" + HexStr(modern::DepositForwarderSaltV2(p)));
            result.pushKV("amount_raw", amount);
            result.pushKV("fee_per_tx", fee_per_tx);
            result.pushKV("max_deposit_raw", max_deposit_raw);
            result.pushKV("min_flush_raw", BRIDGE_FORWARDER_MIN_FLUSH_RAW);
            if (horizon_time) result.pushKV("horizon_time", *horizon_time);
            result.pushKV("oft_send", std::move(oft_send));
            result.pushKV("refund_eoa_message", message);
            result.pushKV("warnings", std::move(warnings));
            return result;
        }};
}

RPCHelpMan bridgewithdraw()
{
    return RPCHelpMan{
        "bridgewithdraw",
        "Burn exact integer bUSD units and create the Ethereum USDT release "
        "request selected by the pinned bridge mode. In decentralized mode the "
        "request is a BRIDGE_BURN leaf and consensus first requires the current "
        "and next B3 validator sets to meet the Ethereum verifier thresholds. "
        "Its final withdrawal id and leaf are assigned only when a block "
        "confirms it. Use the returned proof RPC command after confirmation; "
        "it resolves the stable transaction id and burn output against the "
        "active chain. "
        "In managed mode it remains a managed-v1 operator request. "
        "From the bridge V2 activation height (regtest only) the command burns "
        "the vault-bound bUSD V2 asset instead: the BRIDGE_BURN record names "
        "the V2 vault entry (from a pinned B-23 successor's migration height "
        "the successor's: the retired launch entry takes no new burns), needs "
        "the bridge V2 burn height, and confirms as "
        "a leaf V2 of the V2 withdrawal tree whose root fresh-lineage "
        "certificates carry. Consensus then applies the full V2 burn gate "
        "(rows C1-C4 and E1-E8): a live, unbroken fresh finality lineage whose "
        "current and next committees meet the Verifier V2 bounds and whose "
        "finality keeps up, no sticky conflict, and fresh connected kind 9 "
        "observations (a LIVE VERIFIER_E on B3's lineage, TOKEN_FLAGS showing "
        "a token that pays, and RESERVE_E), also at the burn's position after "
        "earlier records of its block. Each burn must also fit the vault "
        "entry's reserve and the wind-down bound of the latest RESERVE_E, and "
        "the mempool refuses a burn whose gate closes within its margin. "
        "getbridgev2info burn_gate names the row that refuses a burn. Prove "
        "it with getbridgewithdrawalproofv2 against an accepted certificate. "
        "Under bridge V2 the recipient is never a bridge V2 contract (OI-6). "
        "Route (d3), bridge V2 with a pinned exit forwarder only: "
        "options.arbitrum replaces ethereum_recipient, and the leaf pays the "
        "exit forwarder F_x = accountOf(p) of p = {pinned dst_eid, "
        "word(arbitrum.recipient), max_bridge_fee_bps, fee_cap, refund_eoa}, "
        "which the burn record's exit block carries and consensus checks. A "
        "relayer releases and forwards it through the USDT0 OFT adapter in "
        "one B3RelayBatch call, taking at most fee_cap; only refund_eoa can "
        "withdraw an unforwarded balance on Ethereum. USDT0 delivers to a "
        "blocked Arbitrum address frozen: check isBlocked(recipient) on "
        "Arbitrum first. The burn and "
        "recipient are bound together by the canonical type-10 record and its "
        "signed B3MC policy-9 output. No OP_RETURN is used.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"amount", RPCArg::Type::NUM, RPCArg::Optional::NO,
             "Integer raw bUSD units to burn (six decimal places)"},
            {"ethereum_recipient", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED,
             "Exact 20-byte Ethereum recipient, with optional 0x prefix (route d1). Required unless options.arbitrum is given, and refused with it except as null or \"\" (omitted)"},
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
                 {"arbitrum", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED,
                  "Route (d3): withdraw to USDT0 on Arbitrum through the exit forwarder F_x (bridge V2 with a pinned exit forwarder only)", {
                      {"recipient", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
                       "20-byte Arbitrum address that receives the USDT0, with optional 0x prefix"},
                      {"refund_eoa", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
                       "Ethereum externally owned account this user controls: the only account that may withdraw an unforwarded F_x balance on Ethereum"},
                      {"refund_eoa_signature", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
                       "65-byte r || s || v EIP-191 personal_sign signature by refund_eoa of the refund EOA message (bridgeforwarderaddress refund_eoa_message; an error names the exact text)"},
                      {"fee_cap", RPCArg::Type::NUM, RPCArg::Default{BRIDGE_EXIT_DEFAULT_FEE_CAP},
                       "Largest relayer fee the forward may take, raw six-decimal units; it pays the release and the forward (provisional). amount must exceed it"},
                      {"max_bridge_fee_bps", RPCArg::Type::NUM, RPCArg::Default{0},
                       "OFT slippage bound in basis points, at most 50: minAmountLD = amount - amount x bps / 10000"},
                  }},
             },
             // The nested arbitrum object has no one-line rendering.
             RPCArgOptions{.oneline_description = "options"}},
        },
        AssetTransactionResult("Created bridge withdrawal request", {
            {RPCResult::Type::STR_HEX, "asset_id", "Configured chain-bound bUSD asset id (the vault-bound bUSD V2 asset id from the bridge V2 activation height)"},
            {RPCResult::Type::NUM, "amount", "Integer bUSD units burned"},
            {RPCResult::Type::NUM, "asset_change", "Integer bUSD units returned to this wallet"},
            {RPCResult::Type::STR_HEX, "ethereum_recipient", "Bound Ethereum recipient (route d3: the exit forwarder F_x)"},
            {RPCResult::Type::STR, "route", "ethereum (route d1: the leaf pays ethereum_recipient) or arbitrum (route d3: the leaf pays the exit forwarder F_x)"},
            {RPCResult::Type::OBJ, "exit", /*optional=*/true, "Route d3 only: the exit block the burn record carries",
             {
                 {RPCResult::Type::OBJ, "params", "ExitParams p for B3OftExitFactory.forward and withdrawOnEthereum",
                  {
                      {RPCResult::Type::NUM, "dst_eid", "p.dstEid: the pinned exit factory's DST_EID"},
                      {RPCResult::Type::STR_HEX, "to", "p.to: the Arbitrum recipient as a bytes32 word"},
                      {RPCResult::Type::NUM, "max_bridge_fee_bps", "p.maxBridgeFeeBps"},
                      {RPCResult::Type::NUM, "fee_cap", "p.feeCap: largest relayer fee one forward may take (raw six-decimal units)"},
                      {RPCResult::Type::STR_HEX, "refund_eoa", "p.refundEOA"},
                  }},
                 {RPCResult::Type::STR_HEX, "recipient", "The Arbitrum recipient"},
                 {RPCResult::Type::STR_HEX, "factory", "Pinned B3OftExitFactory address"},
                 {RPCResult::Type::STR_HEX, "account", "F_x = factory.accountOf(p), equal to ethereum_recipient"},
                 {RPCResult::Type::STR_HEX, "salt", "exitSalt(p), the CREATE2 salt (raw bytes)"},
             }},
            {RPCResult::Type::NUM, "burn_vout", "Exact bUSD BURN output index"},
            {RPCResult::Type::NUM, "binding_vout", "Signed B3MC bridge binding output index"},
            {RPCResult::Type::STR, "withdrawal_mode", "managed-v1, decentralized-verifier-v1, or decentralized-verifier-v2 (from the bridge V2 activation height)"},
            {RPCResult::Type::STR, "withdrawal_status", /*optional=*/true, "pending-confirmation for a new decentralized (V1 or V2) burn"},
            {RPCResult::Type::STR, "proof_rpc_after_confirmation", /*optional=*/true, "Copyable authoritative proof lookup using txid and burn_vout; for a V2 burn, replace <certificate_block> with the inclusion block of an accepted fresh-lineage certificate covering the burn"},
        }),
        RPCExamples{
            HelpExampleCli(
                "bridgewithdraw", "1000000 \"0x00112233445566778899aabbccddeeff00112233\"") +
            "Route (d3), with named arguments (positionally, pass \"\" as ethereum_recipient):\n" +
            HelpExampleCliNamed(
                "bridgewithdraw",
                {{"amount", 25000000},
                 {"options", UniValue{[] {
                      UniValue arbitrum{UniValue::VOBJ};
                      arbitrum.pushKV("recipient", "0x<arbitrum address>");
                      arbitrum.pushKV("refund_eoa", "0x<ethereum EOA>");
                      arbitrum.pushKV("refund_eoa_signature", "0x<65-byte signature>");
                      UniValue options{UniValue::VOBJ};
                      options.pushKV("arbitrum", std::move(arbitrum));
                      return options;
                  }()}}})},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const CAmount amount{
                AssetUnitsFromValue(request.params[0], "amount")};
            // Route (d1) names ethereum_recipient; route (d3) names
            // options.arbitrum and its exit forwarder becomes the recipient.
            // With options.arbitrum an empty ethereum_recipient counts as
            // omitted: the CLI passes that argument as a string, so
            // positionally "" is the only way to skip it.
            const bool arbitrum{request.params[2].isObject() &&
                                request.params[2].exists("arbitrum")};
            const bool recipient_omitted{
                request.params[1].isNull() ||
                (arbitrum && request.params[1].isStr() &&
                 request.params[1].get_str().empty())};
            if (arbitrum && !recipient_omitted) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "ethereum_recipient and options.arbitrum are mutually exclusive: route (d3) pays its exit forwarder");
            }
            if (!arbitrum && recipient_omitted) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "ethereum_recipient is required unless options.arbitrum is given");
            }
            bridge::EthAddress ethereum_recipient{};
            if (!arbitrum) {
                ethereum_recipient = ParseEthereumRecipient(request.params[1]);
            }
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            const AssetRpcOptions options{ParseAssetRpcOptions(
                request.params[2], /*flowmesh_seat=*/false,
                /*flowmesh_deposit=*/false, /*asset_burn=*/false,
                /*bridge_withdraw=*/true)};
            const ChainSnapshot snapshot{SnapshotChainState(*wallet, false)};
            // Route (d3) needs a pinned exit forwarder and V2 burn rules for
            // the next block; refused before anything else is read.
            if (arbitrum) {
                if (const auto reason{modern::ExitForwarderRouteUnavailableReasonV2(
                        Params().GetConsensus(), snapshot.next_height)}) {
                    throw JSONRPCError(RPC_MISC_ERROR, std::string{*reason});
                }
            }
            // Bridge V2 rides on the V1 bridge tracker and gates (V >= A3).
            const Consensus::BridgeAssetParams& bridge_params{
                RequireBridgeForNextBlock(snapshot.next_height)};
            const Consensus::BridgeWithdrawalMode withdrawal_mode{
                *bridge_params.withdrawal_mode};
            const Consensus::Params& params{Params().GetConsensus()};
            // From V every BRIDGE_BURN is a V2 burn and managed withdrawals
            // are closed (node changes section 3.4): W2 replaces the V1
            // withdrawal gate, the kind 6 record names the V2 vault entry and
            // the burn is of the vault-bound bUSD V2 asset. The V1 store and
            // withdrawal tree are frozen from V, whatever the V1 mode.
            const bool bridge_v2{
                params.BridgeV2RulesActive(snapshot.next_height)};
            if (bridge_v2) {
                if (!params.BridgeV2BurnRulesActive(snapshot.next_height)) {
                    throw JSONRPCError(
                        RPC_MISC_ERROR,
                        "Bridge V2 withdrawals are disabled for the next block");
                }
            } else if (!Consensus::BridgeWithdrawalRulesActive(
                           snapshot.next_height, params)) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    "Bridge withdrawals are disabled for the next block");
            }
            const bool decentralized{
                !bridge_v2 &&
                withdrawal_mode ==
                    Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1};
            const bool managed{!bridge_v2 && !decentralized};
            const auto asset{bridge_v2
                                 ? modern::ConfiguredBridgeV2AssetId(params)
                                 : modern::ConfiguredBridgeAssetId(params)};
            // A V2 burn names the entry that takes burns: the launch entry,
            // or from h_m the B-23 successor (the retired entry refuses
            // burns; node changes section 7.6).
            const Consensus::BridgeV2EntryRole v2_role{
                modern::ActiveBridgeV2Role(params, snapshot.next_height)};
            const Consensus::BridgeV2VaultEntry* v2_entry{
                bridge_v2 ? params.bridge_v2->Entry(v2_role) : nullptr};
            const auto registry{bridge_v2
                                    ? modern::ConfiguredBridgeV2EntryId(params, v2_role)
                                    : modern::ConfiguredBridgeRegistryId(params)};
            if (!asset || !registry || (bridge_v2 && v2_entry == nullptr)) {
                throw JSONRPCError(RPC_MISC_ERROR,
                                   "The active bridge identity is unavailable");
            }
            // Route (d3) exists only under bridge V2 (W2 reached, checked
            // above) with a pinned exit forwarder; its F_x is the burn's
            // recipient. Under V2 neither route may pay a bridge V2 contract
            // (OI-6).
            std::optional<ArbitrumExitRoute> exit_route;
            if (arbitrum) {
                CHECK_NONFATAL(bridge_v2);
                exit_route = ParseArbitrumExitRoute(
                    params, request.params[2]["arbitrum"],
                    static_cast<uint64_t>(amount));
                ethereum_recipient = exit_route->account;
            } else if (bridge_v2) {
                RequireUnreservedBridgeV2Address(
                    *params.bridge_v2, ethereum_recipient, "ethereum_recipient");
            }
            // A V2 burn reuses the kind 6 BRIDGE_BURN record layout; a route
            // (d3) burn appends its exit block (V2 record set).
            const auto record{BuildBridgeWithdrawalMpaRecord(
                bridge_v2 ? Consensus::BridgeWithdrawalMode::
                                DECENTRALIZED_VERIFIER_V1
                          : withdrawal_mode,
                *registry, /*burn_output_index=*/0,
                static_cast<uint64_t>(amount), ethereum_recipient,
                exit_route ? std::optional{exit_route->params} : std::nullopt)};
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
            const auto decentralized_leaf{
                authorization.decentralized_withdrawal
                    ? modern::BridgeWithdrawalLeafV1(
                          authorization.decentralized_withdrawal->withdrawal)
                    : std::nullopt};
            // The consensus-derived leaf V2 must be exactly this burn: the
            // next id is assigned at confirmation, everything else is fixed
            // by the V2 entry, the request and the next height.
            const std::optional<node::BridgeWithdrawalV2Request>&
                withdrawal_v2{authorization.withdrawal_v2};
            const auto leaf_v2{
                withdrawal_v2
                    ? modern::BridgeWithdrawalLeafV2(withdrawal_v2->withdrawal)
                    : std::nullopt};
            if (authorization.mint ||
                (bridge_v2 &&
                 (authorization.withdrawal ||
                  authorization.decentralized_withdrawal ||
                  !withdrawal_v2 ||
                  withdrawal_v2->burn_output_index != 0 ||
                  withdrawal_v2->sweep ||
                  withdrawal_v2->entry != static_cast<uint8_t>(v2_role) ||
                  withdrawal_v2->withdrawal.asset_id != *asset ||
                  withdrawal_v2->withdrawal.destination_chain_id !=
                      v2_entry->origin_chain_id ||
                  withdrawal_v2->withdrawal.vault !=
                      v2_entry->vault_address ||
                  withdrawal_v2->withdrawal.token !=
                      v2_entry->token_address ||
                  withdrawal_v2->withdrawal.amount != amount ||
                  withdrawal_v2->withdrawal.recipient != ethereum_recipient ||
                  withdrawal_v2->withdrawal.b3_height !=
                      static_cast<uint64_t>(snapshot.next_height) ||
                  withdrawal_v2->transaction_id != created.tx->GetHash() ||
                  !leaf_v2 || *leaf_v2 != withdrawal_v2->leaf)) ||
                (decentralized &&
                 (authorization.withdrawal ||
                  authorization.withdrawal_v2 ||
                  !authorization.decentralized_withdrawal ||
                  authorization.decentralized_withdrawal->burn_output_index !=
                      0 ||
                  authorization.decentralized_withdrawal->withdrawal.asset_id !=
                      *asset ||
                  authorization.decentralized_withdrawal->withdrawal
                          .origin_chain_id !=
                      bridge_params.asset.origin_chain_id ||
                  authorization.decentralized_withdrawal->withdrawal
                          .origin_token !=
                      bridge_params.asset.token_address ||
                  authorization.decentralized_withdrawal->withdrawal.amount !=
                      amount ||
                  authorization.decentralized_withdrawal->withdrawal.recipient !=
                      ethereum_recipient ||
                  authorization.decentralized_withdrawal->withdrawal.b3_height !=
                      static_cast<uint64_t>(snapshot.next_height) ||
                  authorization.decentralized_withdrawal->transaction_id !=
                      created.tx->GetHash() ||
                  !decentralized_leaf ||
                  *decentralized_leaf !=
                      authorization.decentralized_withdrawal->leaf)) ||
                (managed &&
                 (authorization.decentralized_withdrawal ||
                  authorization.withdrawal_v2 ||
                  !authorization.withdrawal ||
                  authorization.withdrawal->burn_output_index != 0 ||
                  authorization.withdrawal->asset != *asset ||
                  authorization.withdrawal->amount != amount ||
                  authorization.withdrawal->ethereum_recipient !=
                      ethereum_recipient))) {
                throw JSONRPCError(
                    RPC_VERIFY_REJECTED,
                    "Bridge prevalidation did not authorize the exact withdrawal burn and recipient");
            }
            UniValue result{FinishAssetTransaction(
                request, *wallet, created, options, snapshot, false,
                /*disintegration=*/0,
                bridge_v2       ? "bridge-decentralized-withdrawal-v2"
                : decentralized ? "bridge-decentralized-withdrawal"
                                : "bridge-managed-withdrawal")};
            result.pushKV("asset_id", asset->GetHex());
            result.pushKV("amount", amount);
            result.pushKV("asset_change", asset_change);
            result.pushKV("ethereum_recipient", HexStr(ethereum_recipient));
            result.pushKV("route", exit_route ? "arbitrum" : "ethereum");
            if (exit_route) {
                UniValue exit_json{UniValue::VOBJ};
                exit_json.pushKV("params", ExitForwarderParamsJson(exit_route->params));
                exit_json.pushKV("recipient", "0x" + HexStr(exit_route->recipient));
                exit_json.pushKV("factory", "0x" + HexStr(params.bridge_v2->exit_forwarder->factory));
                exit_json.pushKV("account", "0x" + HexStr(exit_route->account));
                exit_json.pushKV("salt", "0x" + HexStr(exit_route->salt));
                result.pushKV("exit", std::move(exit_json));
            }
            result.pushKV("burn_vout", 0);
            result.pushKV("binding_vout", 1);
            result.pushKV("withdrawal_mode",
                          bridge_v2       ? "decentralized-verifier-v2"
                          : decentralized ? "decentralized-verifier-v1"
                                          : "managed-v1");
            if (withdrawal_v2) {
                // The certificate is chosen by the relayer after the burn
                // confirms: any accepted fresh-lineage certificate whose
                // height covers it (getfinalitylineagecertificate).
                result.pushKV("withdrawal_status", "pending-confirmation");
                result.pushKV(
                    "proof_rpc_after_confirmation",
                    "getbridgewithdrawalproofv2 " +
                        created.tx->GetHash().GetHex() + " " +
                        std::to_string(withdrawal_v2->burn_output_index) +
                        " <certificate_block>");
            } else if (authorization.decentralized_withdrawal) {
                result.pushKV("withdrawal_status", "pending-confirmation");
                result.pushKV(
                    "proof_rpc_after_confirmation",
                    "getbridgewithdrawalproof " +
                        created.tx->GetHash().GetHex() + " " +
                        std::to_string(authorization
                                           .decentralized_withdrawal
                                           ->burn_output_index));
            }
            return result;
        }};
}

} // namespace wallet
