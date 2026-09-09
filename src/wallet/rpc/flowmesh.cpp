// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <consensus/flowmesh_params.h>
#include <core_io.h>
#include <flowmesh/auth.h>
#include <interfaces/chain.h>
#include <key_io.h>
#include <modern/asset_output.h>
#include <modern/mpa.h>
#include <policy/policy.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <rpc/util.h>
#include <util/moneystr.h>
#include <wallet/coincontrol.h>
#include <wallet/rpc/flowmesh.h>
#include <wallet/rpc/util.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

#include <univalue.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace wallet {
namespace {

using MarketStatus = interfaces::FlowMeshMarketStatus;

static bool ParseBroadcastOption(const UniValue& options)
{
    if (options.isNull()) return true;
    if (!options.isObject()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "options must be an object");
    }
    for (const std::string& key : options.getKeys()) {
        if (key != "broadcast") {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "Unknown option '" + key + "'");
        }
    }
    if (!options.exists("broadcast")) return true;
    if (!options["broadcast"].isBool()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "broadcast must be a boolean");
    }
    return options["broadcast"].get_bool();
}

static uint256 ParseMarketId(const UniValue& value)
{
    const uint256 market{ParseHashV(value, "market_id")};
    if (market.IsNull()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "market_id cannot be zero");
    }
    return market;
}

static flowmesh::MarketDataQuery ParseMarketDataQuery(const UniValue& options)
{
    flowmesh::MarketDataQuery query;
    if (options.isNull()) return query;
    if (!options.isObject()) throw JSONRPCError(RPC_TYPE_ERROR, "options must be an object");
    const std::set<std::string> allowed{
        "limit", "curve_limit", "before_sequence", "curve_cursor", "expected_head", "known_head"};
    for (const auto& key : options.getKeys()) {
        if (!allowed.contains(key)) throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown option '" + key + "'");
    }
    const auto integer = [&](const std::string& name, const uint64_t minimum, const uint64_t maximum) {
        const UniValue& value{options[name]};
        if (!value.isNum()) throw JSONRPCError(RPC_TYPE_ERROR, name + " must be an integer");
        uint64_t number;
        try {
            number = value.getInt<uint64_t>();
        } catch (const UniValue::type_error&) {
            throw JSONRPCError(RPC_TYPE_ERROR, name + " must be a nonnegative integer");
        }
        if (number < minimum || number > maximum) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, name + " is outside its supported range");
        }
        return number;
    };
    if (options.exists("limit")) query.limit = integer("limit", 1, flowmesh::MARKET_DATA_MAX_HISTORY);
    if (options.exists("curve_limit")) query.curve_limit = integer("curve_limit", 1, flowmesh::MARKET_DATA_MAX_CURVES);
    if (options.exists("before_sequence")) query.before_sequence = integer("before_sequence", 0, std::numeric_limits<uint64_t>::max());
    for (const auto* name : {"expected_head", "known_head"}) {
        if (!options.exists(name)) continue;
        if (!options[name].isStr()) throw JSONRPCError(RPC_TYPE_ERROR, std::string{name} + " must be a hash string");
        const uint256 head{ParseHashV(options[name], name)};
        if (std::string{name} == "expected_head") query.expected_head = head;
        else query.known_head = head;
    }
    if (options.exists("curve_cursor")) {
        if (!options["curve_cursor"].isStr()) throw JSONRPCError(RPC_TYPE_ERROR, "curve_cursor must be a string");
        const std::string cursor{options["curve_cursor"].get_str()};
        if (cursor.size() != 68 || (cursor.substr(0, 4) != "bid:" && cursor.substr(0, 4) != "ask:")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "curve_cursor must be bid:<account_id> or ask:<account_id>");
        }
        query.curve_cursor = flowmesh::ClearingEngine::CurveKey{
            cursor.starts_with("bid:") ? flowmesh::ClearingEngine::Side::BID : flowmesh::ClearingEngine::Side::ASK,
            ParseHashV(UniValue{cursor.substr(4)}, "curve_cursor")};
        if (!query.expected_head) throw JSONRPCError(RPC_INVALID_PARAMETER, "curve_cursor requires expected_head");
    }
    if (query.known_head && (query.curve_cursor || query.before_sequence)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "known_head cannot be combined with pagination cursors");
    }
    return query;
}

static UniValue MarketCurvesJson(const std::vector<flowmesh::ClearingEngine::CurveView>& curves)
{
    UniValue out{UniValue::VARR};
    for (const auto& curve : curves) {
        UniValue item{UniValue::VOBJ};
        item.pushKV("account_id", curve.account_id.GetHex());
        item.pushKV("side", curve.side == flowmesh::ClearingEngine::Side::BID ? "bid" : "ask");
        item.pushKV("status", curve.filled_quantity ? "partially-filled" : "open");
        item.pushKV("filled_quantity", curve.filled_quantity);
        item.pushKV("remaining_quantity", curve.remaining_quantity);
        item.pushKV("reserved_amount", curve.reserved_amount);
        UniValue points{UniValue::VARR};
        for (const auto& point : curve.points) {
            UniValue row{UniValue::VOBJ};
            row.pushKV("price", point.price);
            row.pushKV("quantity", point.qty);
            points.push_back(std::move(row));
        }
        item.pushKV("points", std::move(points));
        out.push_back(std::move(item));
    }
    return out;
}

static RPCResult MarketCurveResult()
{
    using T = RPCResult::Type;
    return RPCResult{T::OBJ, "", "Certified standing curve; identity is account and side, not a price-time order", {
        {T::STR_HEX, "account_id", "Curve owner"},
        {T::STR, "side", "bid or ask"},
        {T::STR, "status", "open or partially-filled"},
        {T::NUM, "filled_quantity", "Base units consumed from this current curve"},
        {T::NUM, "remaining_quantity", "Maximum original curve quantity minus consumed quantity"},
        {T::NUM, "reserved_amount", "B3 atoms for bids, base units for asks"},
        {T::ARR, "points", "Original curve breakpoints; effective quantity is max(evaluate(points, price)-filled_quantity, 0)", {
            {T::OBJ, "", "Breakpoint", {
                {T::NUM, "price", "B3 atoms per base atomic unit"},
                {T::NUM, "quantity", "Original base atomic units at this price"},
            }},
        }},
    }};
}

static UniValue CertifiedMarketDataJson(const flowmesh::MarketData& data)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("domain", data.domain.GetHex());
    out.pushKV("market_id", data.market_id.GetHex());
    out.pushKV("base_asset_id", data.base_asset_id.GetHex());
    out.pushKV("quote_asset", "B3");
    out.pushKV("execution_config_id", data.execution_config_id.GetHex());
    out.pushKV("matching_model", "uniform-price-curve-auction");
    out.pushKV("quantity_lot_raw", 1);
    out.pushKV("price_tick_raw", 1);
    out.pushKV("unchanged", data.unchanged);
    const auto& source{data.snapshot};
    UniValue snapshot{UniValue::VOBJ};
    snapshot.pushKV("certified", source.certified);
    snapshot.pushKV("next_microblock_sequence", source.next_microblock_sequence);
    snapshot.pushKV("last_microblock_hash", source.last_microblock_hash.GetHex());
    snapshot.pushKV("state_root", source.state_root.GetHex());
    snapshot.pushKV("epoch", source.epoch);
    snapshot.pushKV("anchor_height", source.anchor_height);
    snapshot.pushKV("anchor_hash", source.anchor_hash.GetHex());
    snapshot.pushKV("active_seats", static_cast<uint64_t>(source.active_seats));
    snapshot.pushKV("quorum_required", static_cast<uint64_t>(source.quorum_required));
    snapshot.pushKV("running", source.running);
    snapshot.pushKV("paused", source.paused);
    snapshot.pushKV("observer_only", source.observer_only);
    snapshot.pushKV("pending_handoff", source.pending_handoff);
    snapshot.pushKV("halt", source.halt);
    snapshot.pushKV("error", source.error);
    snapshot.pushKV("pending_actions", static_cast<uint64_t>(source.pending_actions));
    snapshot.pushKV("checkpoint_status_known", false);
    if (source.local_observed_at) snapshot.pushKV("local_observed_at", *source.local_observed_at);
    const auto& diagnostics{source.runtime};
    UniValue runtime{UniValue::VOBJ};
    runtime.pushKV("round", diagnostics.round);
    runtime.pushKV("candidate_count", static_cast<uint64_t>(diagnostics.candidate_count));
    runtime.pushKV("max_verified_attestations", static_cast<uint64_t>(diagnostics.max_verified_attestations));
    runtime.pushKV("active_catchup_requests", static_cast<uint64_t>(diagnostics.active_catchup_requests));
    runtime.pushKV("proposals_missing_evidence", diagnostics.proposals_missing_evidence);
    runtime.pushKV("proposals_rejected_round", diagnostics.proposals_rejected_round);
    runtime.pushKV("attestations_without_candidate", diagnostics.attestations_without_candidate);
    if (diagnostics.last_message_observed_at) runtime.pushKV("last_message_observed_at", *diagnostics.last_message_observed_at);
    if (diagnostics.local_locked_candidate) runtime.pushKV("local_locked_candidate", diagnostics.local_locked_candidate->GetHex());
    snapshot.pushKV("runtime", std::move(runtime));
    out.pushKV("snapshot", std::move(snapshot));
    if (data.unchanged) return out;

    UniValue liquidity{UniValue::VOBJ};
    liquidity.pushKV("curves", MarketCurvesJson(data.liquidity.curves));
    liquidity.pushKV("total_curves", static_cast<uint64_t>(data.liquidity.total_curves));
    liquidity.pushKV("complete", data.liquidity.complete);
    if (data.liquidity.next_cursor) {
        liquidity.pushKV("next_cursor", std::string{data.liquidity.next_cursor->first == flowmesh::ClearingEngine::Side::BID ? "bid:" : "ask:"} + data.liquidity.next_cursor->second.GetHex());
    }
    out.pushKV("liquidity", std::move(liquidity));
    if (data.account) {
        const auto& source_account{*data.account};
        UniValue account{UniValue::VOBJ};
        account.pushKV("account_id", source_account.account_id.GetHex());
        account.pushKV("next_sequence", source_account.next_sequence);
        account.pushKV("base_available", source_account.base_available);
        account.pushKV("base_reserved", source_account.base_reserved);
        account.pushKV("b3_available_atoms", source_account.b3_available_atoms);
        account.pushKV("b3_reserved_atoms", source_account.b3_reserved_atoms);
        account.pushKV("curves", MarketCurvesJson(source_account.curves));
        out.pushKV("account", std::move(account));
    }
    UniValue history{UniValue::VOBJ};
    history.pushKV("available", data.history.available);
    history.pushKV("scope", "bounded-certified-log-cache");
    history.pushKV("truncated", data.history.truncated);
    if (data.history.oldest_retained_sequence) history.pushKV("oldest_retained_sequence", *data.history.oldest_retained_sequence);
    if (data.history.next_before_sequence) history.pushKV("next_before_sequence", *data.history.next_before_sequence);
    UniValue entries{UniValue::VARR};
    for (const auto& entry : data.history.entries) {
        UniValue item{UniValue::VOBJ};
        item.pushKV("sequence", entry.sequence);
        item.pushKV("microblock_hash", entry.microblock_hash.GetHex());
        item.pushKV("epoch", entry.epoch);
        item.pushKV("anchor_height", entry.anchor_height);
        item.pushKV("anchor_hash", entry.anchor_hash.GetHex());
        item.pushKV("kind", entry.handoff ? "handoff" : "execution");
        item.pushKV("cleared", entry.cleared);
        item.pushKV("price", entry.price);
        item.pushKV("quantity", entry.quantity);
        item.pushKV("notional_atoms", entry.notional_atoms);
        item.pushKV("fee_atoms", entry.fee_atoms);
        const auto fill{data.account ? std::find_if(entry.account_fills.begin(), entry.account_fills.end(),
            [&](const auto& candidate) { return candidate.account_id == data.account->account_id; }) : entry.account_fills.end()};
        const bool found{fill != entry.account_fills.end()};
        item.pushKV("account_fills_known", data.account.has_value() && (entry.account_fills_complete || found));
        item.pushKV("account_bid_fill", found ? fill->bid_quantity : CAmount{0});
        item.pushKV("account_ask_fill", found ? fill->ask_quantity : CAmount{0});
        entries.push_back(std::move(item));
    }
    history.pushKV("entries", std::move(entries));
    out.pushKV("history", std::move(history));
    return out;
}

static COutPoint ParseOutPoint(const UniValue& txid_value,
                               const UniValue& vout_value)
{
    const Txid txid{Txid::FromUint256(ParseHashV(txid_value, "txid"))};
    const int64_t vout{vout_value.getInt<int64_t>()};
    if (vout < 0 || vout > std::numeric_limits<uint32_t>::max()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           "vout is outside the uint32 range");
    }
    return COutPoint{txid, static_cast<uint32_t>(vout)};
}

static CAmount ExactPositiveAmount(const UniValue& value,
                                   const std::string& name)
{
    if (!value.isNum()) {
        throw JSONRPCError(RPC_TYPE_ERROR,
                           name + " must be an integer");
    }
    try {
        const int64_t amount{value.getInt<int64_t>()};
        if (amount <= 0 || amount > MAX_MONEY) {
            throw JSONRPCError(
                RPC_INVALID_PARAMETER,
                name + " must be in the range [1, MAX_MONEY]");
        }
        return amount;
    } catch (const UniValue::type_error&) {
        throw JSONRPCError(RPC_TYPE_ERROR,
                           name + " must be an integer");
    }
}

static uint64_t OptionalSequence(const UniValue& value,
                                 const uint64_t fallback)
{
    if (value.isNull()) return fallback;
    if (!value.isNum()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "sequence must be an integer");
    }
    try {
        const int64_t sequence{value.getInt<int64_t>()};
        if (sequence < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "sequence cannot be negative");
        }
        return static_cast<uint64_t>(sequence);
    } catch (const UniValue::type_error&) {
        throw JSONRPCError(RPC_TYPE_ERROR, "sequence must be an integer");
    }
}

static modern::AssetId ParseAssetOrB3(const UniValue& value)
{
    if (!value.isStr()) {
        throw JSONRPCError(RPC_TYPE_ERROR,
                           "asset must be an asset id or 'B3'");
    }
    const std::string text{value.get_str()};
    if (text == "B3" || text == "b3" || text == "native") {
        return modern::NativeAsset();
    }
    return ParseHashV(value, "asset");
}

static UniValue MarketStatusJson(const MarketStatus& status)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("available", status.available);
    out.pushKV("running", status.running);
    out.pushKV("domain", status.domain.GetHex());
    out.pushKV("market_id", status.market_id.GetHex());
    out.pushKV("vault_id", status.vault_id.GetHex());
    out.pushKV("base_asset_id", status.base_asset.GetHex());
    out.pushKV("quote_asset", "B3");
    out.pushKV("execution_config_id", status.execution_config_id.GetHex());
    out.pushKV("epoch", status.epoch);
    out.pushKV("next_microblock_sequence",
               status.next_microblock_sequence);
    out.pushKV("next_effect_index", status.next_effect_index);
    out.pushKV("round", status.round);
    out.pushKV("last_microblock_hash",
               status.last_microblock_hash.GetHex());
    out.pushKV("state_root", status.state_root.GetHex());
    out.pushKV("pending_actions",
               static_cast<uint64_t>(status.pending_actions));
    out.pushKV("observer_only", status.observer_only);
    out.pushKV("paused", status.paused);
    out.pushKV("pending_handoff", status.pending_handoff);
    out.pushKV("checkpoint_pending", status.checkpoint_pending);
    if (status.checkpoint_pending) {
        out.pushKV("pending_checkpoint_id",
                   status.pending_checkpoint_id.GetHex());
        out.pushKV("pending_checkpoint_sequence",
                   status.pending_checkpoint_sequence);
        out.pushKV("pending_checkpoint_effect_count",
                   status.pending_checkpoint_effect_count);
        out.pushKV("genesis_checkpoint_required",
                   status.pending_checkpoint_sequence == 0);
    }
    out.pushKV("halt", status.halt);
    out.pushKV("error", status.error);
    if (status.account_id) {
        UniValue account{UniValue::VOBJ};
        account.pushKV("account_id", status.account_id->GetHex());
        account.pushKV("next_sequence", status.next_account_sequence);
        account.pushKV("slot", status.slot);
        account.pushKV("base_available", status.base_available);
        account.pushKV("base_reserved", status.base_reserved);
        account.pushKV("b3_available",
                       ValueFromAmount(status.b3_available));
        account.pushKV("b3_reserved",
                       ValueFromAmount(status.b3_reserved));
        out.pushKV("account", std::move(account));
    }
    return out;
}

static std::optional<flowmesh::AccountId> ExistingWalletAccount(
    const CWallet& wallet)
{
    LOCK(wallet.cs_wallet);
    const auto pubkey{wallet.GetFlowMeshAccountPubKey()};
    return pubkey ? std::optional<flowmesh::AccountId>{
                        flowmesh::AccountForKey(XOnlyPubKey{*pubkey})}
                  : std::nullopt;
}

struct WalletActionContext {
    CKey secret;
    flowmesh::AccountId account;
    MarketStatus market;
};

static WalletActionContext GetWalletActionContext(CWallet& wallet,
                                                   const uint256& market_id)
{
    CKey secret;
    flowmesh::AccountId account;
    {
        LOCK(wallet.cs_wallet);
        EnsureWalletIsUnlocked(wallet);
        if (wallet.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               "Private keys are disabled for this wallet");
        }
        const auto pubkey{wallet.GetOrCreateFlowMeshAccountKey()};
        if (!pubkey) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               util::ErrorString(pubkey).original);
        }
        account = flowmesh::AccountForKey(XOnlyPubKey{*pubkey});
        const auto key{wallet.GetFlowMeshAccountSecret()};
        if (!key) {
            throw JSONRPCError(RPC_WALLET_ERROR,
                               util::ErrorString(key).original);
        }
        secret = *key;
    }

    const auto status{wallet.chain().flowMeshMarketStatus(market_id, account)};
    if (!status || !status->available || !status->running) {
        throw JSONRPCError(RPC_MISC_ERROR,
                           "FlowMesh market is not running in this node");
    }
    if (status->paused) {
        throw JSONRPCError(RPC_MISC_ERROR,
                           "FlowMesh market is paused (at least four active seats are required)");
    }
    if (!status->account_id || *status->account_id != account ||
        status->domain.IsNull() || status->execution_config_id.IsNull()) {
        throw JSONRPCError(RPC_INTERNAL_ERROR,
                           "FlowMesh account or market binding is unavailable");
    }
    return {std::move(secret), account, *status};
}

static UniValue SubmitSignedAction(CWallet& wallet,
                                   WalletActionContext& context,
                                   flowmesh::Action action)
{
    if (!flowmesh::SignAction(context.secret, context.market.domain,
                              context.market.execution_config_id, action) ||
        !action.ShapeIsCanonical()) {
        throw JSONRPCError(RPC_INTERNAL_ERROR,
                           "Unable to sign the canonical FlowMesh action");
    }
    std::string error;
    if (!wallet.chain().submitFlowMeshAction(context.market.market_id,
                                              action, error)) {
        throw JSONRPCError(RPC_MISC_ERROR,
                           error.empty() ? "FlowMesh action was not accepted"
                                         : error);
    }
    UniValue out{UniValue::VOBJ};
    out.pushKV("accepted", true);
    out.pushKV("market_id", context.market.market_id.GetHex());
    out.pushKV("account_id", context.account.GetHex());
    out.pushKV("sequence", action.sequence);
    out.pushKV("action_id", action.Id().GetHex());
    return out;
}

static std::vector<flowmesh::ClearingEngine::Breakpoint> LimitCurve(
    const std::string& side, const CAmount price, const CAmount quantity)
{
    if (side == "bid") {
        if (price >= MAX_MONEY) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "bid price must be below MAX_MONEY");
        }
        return {{price, quantity}, {price + 1, 0}};
    }
    if (side == "ask") {
        if (price == 0) return {{0, quantity}};
        return {{price - 1, 0}, {price, quantity}};
    }
    throw JSONRPCError(RPC_INVALID_PARAMETER,
                       "side must be 'bid' or 'ask'");
}

static std::string QueueActionName(const uint8_t type)
{
    switch (static_cast<flowmesh::ActionType>(type)) {
    case flowmesh::ActionType::SUBMIT_BID: return "bid";
    case flowmesh::ActionType::SUBMIT_ASK: return "ask";
    case flowmesh::ActionType::CANCEL_BID: return "cancel-bid";
    case flowmesh::ActionType::CANCEL_ASK: return "cancel-ask";
    case flowmesh::ActionType::WITHDRAW: return "withdrawal-request";
    case flowmesh::ActionType::DEPOSIT: return "deposit";
    default: return "action";
    }
}

static CRecipient ExactOutputRecipient(const CTxOut& output)
{
    return CRecipient{CNoDestination{output.scriptPubKey}, output.nValue,
                      /*fSubtractFeeFromAmount=*/false};
}

static uint256 FlowMeshEffectId(const modern::FlowMeshEffectV1& effect)
{
    if (const auto* deposit{
            std::get_if<modern::FlowMeshDepositAcceptanceV1>(&effect)}) {
        return deposit->acceptance_id;
    }
    return std::get<modern::FlowMeshWithdrawalReceiptV1>(effect).receipt_id;
}

static UniValue VaultOperationJson(
    const interfaces::FlowMeshVaultOperation& operation)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("effect_id", FlowMeshEffectId(operation.effect).GetHex());
    out.pushKV("market_id", operation.market_id.GetHex());
    out.pushKV("checkpoint_id", operation.checkpoint_id.GetHex());
    out.pushKV("vault_inputs",
               static_cast<uint64_t>(operation.inputs.size()));
    if (const auto* deposit{
            std::get_if<modern::FlowMeshDepositAcceptanceV1>(
                &operation.effect)}) {
        out.pushKV("kind", "deposit-sweep");
        out.pushKV("epoch", deposit->epoch);
        out.pushKV("sequence", deposit->sequence);
        out.pushKV("account_id", deposit->account.GetHex());
        out.pushKV("asset", deposit->asset == modern::NativeAsset()
                                ? "B3"
                                : deposit->asset.GetHex());
        out.pushKV("amount", deposit->asset == modern::NativeAsset()
                                 ? ValueFromAmount(deposit->amount)
                                 : UniValue{deposit->amount});
        out.pushKV("vault_id", deposit->vault_id.GetHex());
        out.pushKV("deposit_txid", deposit->deposit_outpoint.hash.GetHex());
        out.pushKV("deposit_vout", deposit->deposit_outpoint.n);
        out.pushKV("shard", deposit->shard);
    } else {
        const auto& receipt{
            std::get<modern::FlowMeshWithdrawalReceiptV1>(operation.effect)};
        out.pushKV("kind", "withdrawal");
        out.pushKV("epoch", receipt.epoch);
        out.pushKV("sequence", receipt.sequence);
        out.pushKV("account_id", receipt.account.GetHex());
        out.pushKV("asset", receipt.asset == modern::NativeAsset()
                                ? "B3"
                                : receipt.asset.GetHex());
        out.pushKV("amount", receipt.asset == modern::NativeAsset()
                                 ? ValueFromAmount(receipt.amount)
                                 : UniValue{receipt.amount});
        out.pushKV("vault_id", receipt.vault_id.GetHex());
        out.pushKV("destination_owner_commitment",
                   receipt.destination_owner_commitment.GetHex());
        out.pushKV("change_shard", receipt.deterministic_change_shard);
    }
    return out;
}

static RPCResult MarketStatusResult(std::string description)
{
    return RPCResult{RPCResult::Type::OBJ, "", std::move(description), {
        {RPCResult::Type::BOOL, "available", "Whether this node recognizes the market"},
        {RPCResult::Type::BOOL, "running", "Whether the market service is running"},
        {RPCResult::Type::STR_HEX, "domain", "Modern chain domain"},
        {RPCResult::Type::STR_HEX, "market_id", "FlowMesh market id"},
        {RPCResult::Type::STR_HEX, "vault_id", "FlowMesh vault id"},
        {RPCResult::Type::STR_HEX, "base_asset_id", "Market base-asset id"},
        {RPCResult::Type::STR, "quote_asset", "Market quote asset"},
        {RPCResult::Type::STR_HEX, "execution_config_id", "Pinned execution configuration id"},
        {RPCResult::Type::NUM, "epoch", "Current FlowMesh epoch"},
        {RPCResult::Type::NUM, "next_microblock_sequence", "Next microblock sequence"},
        {RPCResult::Type::NUM, "next_effect_index", "Next certified effect index"},
        {RPCResult::Type::NUM, "round", "Current consensus round"},
        {RPCResult::Type::STR_HEX, "last_microblock_hash", "Last certified microblock hash"},
        {RPCResult::Type::STR_HEX, "state_root", "Current certified state root"},
        {RPCResult::Type::NUM, "pending_actions", "Actions waiting to be processed"},
        {RPCResult::Type::BOOL, "observer_only", "Whether this node has no active validator seat"},
        {RPCResult::Type::BOOL, "paused", "Whether trading is paused"},
        {RPCResult::Type::BOOL, "pending_handoff", "Whether a validator-set handoff is pending"},
        {RPCResult::Type::BOOL, "checkpoint_pending", "Whether a certified checkpoint awaits publication"},
        {RPCResult::Type::STR_HEX, "pending_checkpoint_id", /*optional=*/true, "Pending checkpoint id"},
        {RPCResult::Type::NUM, "pending_checkpoint_sequence", /*optional=*/true, "Pending checkpoint sequence"},
        {RPCResult::Type::NUM, "pending_checkpoint_effect_count", /*optional=*/true, "Effects committed by the pending checkpoint"},
        {RPCResult::Type::BOOL, "genesis_checkpoint_required", /*optional=*/true, "Whether the pending checkpoint is the market genesis checkpoint"},
        {RPCResult::Type::STR, "halt", "none or the fail-closed halt reason"},
        {RPCResult::Type::STR, "error", "Most recent market error"},
        {RPCResult::Type::OBJ, "account", /*optional=*/true, "This wallet's account state", {
            {RPCResult::Type::STR_HEX, "account_id", "FlowMesh account id"},
            {RPCResult::Type::NUM, "next_sequence", "Next account action sequence"},
            {RPCResult::Type::NUM, "slot", "Assigned deterministic account slot"},
            {RPCResult::Type::NUM, "base_available", "Available base-asset units"},
            {RPCResult::Type::NUM, "base_reserved", "Reserved base-asset units"},
            {RPCResult::Type::STR_AMOUNT, "b3_available", "Available native B3"},
            {RPCResult::Type::STR_AMOUNT, "b3_reserved", "Reserved native B3"},
        }},
    }};
}

static RPCResult VaultOperationResult(std::string description)
{
    return RPCResult{RPCResult::Type::OBJ, "", std::move(description), {
        {RPCResult::Type::STR_HEX, "effect_id", "Certified effect id"},
        {RPCResult::Type::STR_HEX, "market_id", "FlowMesh market id"},
        {RPCResult::Type::STR_HEX, "checkpoint_id", "Authorizing checkpoint id"},
        {RPCResult::Type::NUM, "vault_inputs", "Number of required keyless vault inputs"},
        {RPCResult::Type::STR, "kind", "deposit-sweep or withdrawal"},
        {RPCResult::Type::NUM, "epoch", "Effect epoch"},
        {RPCResult::Type::NUM, "sequence", "Effect sequence"},
        {RPCResult::Type::STR_HEX, "account_id", "FlowMesh account id"},
        {RPCResult::Type::STR, "asset", "B3 or the colored-asset id"},
        {RPCResult::Type::NUM, "amount", "B3 decimal amount or exact colored-asset units"},
        {RPCResult::Type::STR_HEX, "vault_id", "FlowMesh vault id"},
        {RPCResult::Type::STR_HEX, "deposit_txid", /*optional=*/true, "Deposit transaction id for a deposit sweep"},
        {RPCResult::Type::NUM, "deposit_vout", /*optional=*/true, "Deposit output index for a deposit sweep"},
        {RPCResult::Type::NUM, "shard", /*optional=*/true, "Vault shard for a deposit sweep"},
        {RPCResult::Type::STR_HEX, "destination_owner_commitment", /*optional=*/true, "Destination owner commitment for a withdrawal"},
        {RPCResult::Type::NUM, "change_shard", /*optional=*/true, "Deterministic vault-change shard for a withdrawal"},
    }};
}

static RPCResult AcceptedActionResult(
    std::string description, std::vector<RPCResult> action_fields)
{
    std::vector<RPCResult> fields;
    fields.reserve(5 + action_fields.size());
    fields.emplace_back(RPCResult::Type::BOOL, "accepted", "Whether the action was accepted");
    fields.emplace_back(RPCResult::Type::STR_HEX, "market_id", "FlowMesh market id");
    fields.emplace_back(RPCResult::Type::STR_HEX, "account_id", "FlowMesh account id");
    fields.emplace_back(RPCResult::Type::NUM, "sequence", "Account action sequence");
    fields.emplace_back(RPCResult::Type::STR_HEX, "action_id", "Signed action id");
    for (RPCResult& field : action_fields) {
        fields.push_back(std::move(field));
    }
    return RPCResult{RPCResult::Type::OBJ, "", std::move(description),
                     std::move(fields)};
}

} // namespace

UniValue FlowMeshVaultOperationToJSON(
    const interfaces::FlowMeshVaultOperation& operation)
{
    return VaultOperationJson(operation);
}

UniValue FlowMeshValidatorStatusToJSON(
    const interfaces::FlowMeshValidatorStatus& status,
    std::vector<std::array<unsigned char, bls::PUBKEY_SIZE>> wallet_public_keys)
{
    std::sort(wallet_public_keys.begin(), wallet_public_keys.end());
    wallet_public_keys.erase(std::unique(wallet_public_keys.begin(), wallet_public_keys.end()), wallet_public_keys.end());
    UniValue armed{UniValue::VARR};
    std::set<std::array<unsigned char, bls::PUBKEY_SIZE>> armed_keys{
        status.armed_pubkeys.begin(), status.armed_pubkeys.end()};
    for (const auto& key : armed_keys) armed.push_back(HexStr(key));
    UniValue wallet_keys{UniValue::VARR};
    uint64_t wallet_armed{0};
    for (const auto& key : wallet_public_keys) {
        wallet_keys.push_back(HexStr(key));
        if (armed_keys.contains(key)) ++wallet_armed;
    }
    UniValue out{UniValue::VOBJ};
    out.pushKV("scope", "node-global");
    out.pushKV("service_available", status.available);
    out.pushKV("service_enabled", status.enabled);
    out.pushKV("service_running", status.running);
    out.pushKV("armed", !armed_keys.empty());
    out.pushKV("armed_keys_fingerprint", status.fingerprint.GetHex());
    out.pushKV("armed_key_count", static_cast<uint64_t>(armed_keys.size()));
    out.pushKV("armed_bls_pubkeys", std::move(armed));
    out.pushKV("wallet_bls_pubkeys", std::move(wallet_keys));
    out.pushKV("wallet_key_count", static_cast<uint64_t>(wallet_public_keys.size()));
    out.pushKV("wallet_armed_key_count", wallet_armed);
    out.pushKV("wallet_all_keys_armed", !wallet_public_keys.empty() && wallet_armed == wallet_public_keys.size());
    out.pushKV("armed_is_signing_proof", false);
    out.pushKV("warning", "Armed keys are node-global and do not prove active seat membership, a signature, or a certified microblock. Market status is a separate observation.");
    return out;
}

namespace {

RPCResult FlowMeshValidatorStatusResult(const bool include_markets,
                                       const bool legacy_fields)
{
    std::vector<RPCResult> fields{
        {RPCResult::Type::STR, "scope", "Always node-global; changing keys affects every wallet"},
        {RPCResult::Type::BOOL, "service_available", "Whether this node has a FlowMesh service"},
        {RPCResult::Type::BOOL, "service_enabled", "Whether the service's activation schedule is configured"},
        {RPCResult::Type::BOOL, "service_running", "Whether the service worker is running"},
        {RPCResult::Type::BOOL, "armed", "At least one FN BLS key is loaded; not proof of signing"},
        {RPCResult::Type::STR_HEX, "armed_keys_fingerprint", "CAS token for the exact sorted unique public-key set"},
        {RPCResult::Type::NUM, "armed_key_count", "Number of unique node-global armed keys"},
        {RPCResult::Type::ARR, "armed_bls_pubkeys", "Sorted unique node-global public keys", {
            {RPCResult::Type::STR_HEX, "", "48-byte BLS public key"}}},
        {RPCResult::Type::ARR, "wallet_bls_pubkeys", "Public keys stored in the selected wallet; no secrets are read", {
            {RPCResult::Type::STR_HEX, "", "48-byte BLS public key"}}},
        {RPCResult::Type::NUM, "wallet_key_count", "Unique stored wallet FN BLS keys; not a count of owned or eligible seats"},
        {RPCResult::Type::NUM, "wallet_armed_key_count", "Stored wallet keys also present in the armed snapshot"},
        {RPCResult::Type::BOOL, "wallet_all_keys_armed", "All stored wallet keys are armed, false if none are stored"},
        {RPCResult::Type::BOOL, "armed_is_signing_proof", "Always false"},
        {RPCResult::Type::STR, "warning", "Status interpretation and node-global scope"},
    };
    if (include_markets) {
        fields.emplace_back(RPCResult::Type::ARR, "markets", "Separately sampled node-wide market status; not proof of a local signature",
                            std::vector<RPCResult>{MarketStatusResult("Market status")});
    }
    if (legacy_fields) {
        fields.emplace_back(RPCResult::Type::BOOL, "running", "Legacy alias for armed, not service_running or proof of signing");
        fields.emplace_back(RPCResult::Type::NUM, "armed_keys", "Legacy alias for armed_key_count");
    }
    return RPCResult{RPCResult::Type::OBJ, "", "Public FN validator key status", std::move(fields)};
}

std::optional<uint256> ExpectedArmedKeys(const UniValue& value)
{
    return value.isNull() ? std::nullopt
                          : std::optional<uint256>{ParseHashV(value, "expected_armed_keys_fingerprint")};
}

} // namespace

RPCHelpMan getflowmeshvalidatorinfo()
{
    return RPCHelpMan{
        "getflowmeshvalidatorinfo",
        "Read the selected wallet's stored FN BLS public keys and a locked snapshot of the node-global armed keys. "
        "Does not unlock, create keys, sign, or broadcast. Armed is not proof of seat eligibility or actual signing.\n",
        {},
        FlowMeshValidatorStatusResult(/*include_markets=*/true, /*legacy_fields=*/false),
        RPCExamples{HelpExampleCli("getflowmeshvalidatorinfo", "")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const auto wallet{GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            std::vector<std::array<unsigned char, bls::PUBKEY_SIZE>> public_keys;
            {
                LOCK(wallet->cs_wallet);
                public_keys = wallet->ListFlowMeshBlsPubkeys();
            }
            UniValue out{FlowMeshValidatorStatusToJSON(wallet->chain().flowMeshValidatorStatus(), std::move(public_keys))};
            UniValue markets{UniValue::VARR};
            for (const auto& market : wallet->chain().flowMeshMarkets(std::nullopt)) {
                markets.push_back(MarketStatusJson(market));
            }
            out.pushKV("markets", std::move(markets));
            return out;
        }};
}

RPCHelpMan listflowmeshmarkets()
{
    return RPCHelpMan{
        "listflowmeshmarkets",
        "List every anchor-final simple-v1 colored-asset/B3 FlowMesh market.\n",
        {},
        RPCResult{RPCResult::Type::ARR, "", "Markets", {
            MarketStatusResult("Market status"),
        }},
        RPCExamples{HelpExampleCli("listflowmeshmarkets", "")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            const auto account{ExistingWalletAccount(*wallet)};
            UniValue out{UniValue::VARR};
            for (const auto& status : wallet->chain().flowMeshMarkets(account)) {
                out.push_back(MarketStatusJson(status));
            }
            return out;
        }};
}

RPCHelpMan getflowmeshmarketdata()
{
    using T = RPCResult::Type;
    return RPCHelpMan{
        "getflowmeshmarketdata",
        "Read certified standing curves, exact auction clearing history and this wallet's account without creating keys, signing or submitting actions. "
        "FlowMesh uses one uniform-price curve auction per microblock, not price-time priority. "
        "All prices are integer B3 atoms per base atomic unit and all quantities are integer base units; B3 has nine decimals. "
        "History contains at most the latest 256 certified microblocks, rebuilt during normal verified startup replay. "
        "Account fills are explicitly marked unknown if outside the bounded per-entry account cache. "
        "Microblocks contain no execution timestamp: chart actual cleared entries against sequence. "
        "local_observed_at is only this process's observation time and is absent for replayed heads. "
        "An old head on an idle market does not prove quorum failure. Standing curve identity is account and side; the v1 state does not retain submission ids or original submission sequences. "
        "Use expected_head when continuing curve pages; a changed head rejects pagination. "
        "known_head returns current lightweight status with unchanged=true when immutable data is unchanged. "
        "Checkpoint status remains available through listflowmeshmarkets; this bounded query does not scan the checkpoint backlog.\n",
        {
            {"market_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "Market id"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "Bounded query options", {
                {"limit", RPCArg::Type::NUM, RPCArg::Default{50}, "History entries, 1-100"},
                {"before_sequence", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Exclusive history sequence cursor; newest first"},
                {"curve_limit", RPCArg::Type::NUM, RPCArg::Default{128}, "Standing curves, 1-128"},
                {"curve_cursor", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Exclusive bid:<account_id> or ask:<account_id> cursor; requires expected_head"},
                {"expected_head", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Reject if the certified head has changed"},
                {"known_head", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Omit heavy data when the head matches; cannot be combined with pagination cursors"},
            }},
        },
        RPCResult{T::OBJ, "", "Bounded certified market snapshot", {
            {T::STR_HEX, "domain", "FlowMesh domain binding"},
            {T::STR_HEX, "market_id", "Market id"},
            {T::STR_HEX, "base_asset_id", "Base asset id"},
            {T::STR, "quote_asset", "B3"},
            {T::STR_HEX, "execution_config_id", "Immutable execution configuration"},
            {T::STR, "matching_model", "uniform-price-curve-auction"},
            {T::NUM, "quantity_lot_raw", "Current limit-order RPC input granularity in base units"},
            {T::NUM, "price_tick_raw", "Current limit-order RPC input granularity in B3 atoms per base unit"},
            {T::BOOL, "unchanged", "Heavy data omitted because known_head matches"},
            {T::OBJ, "base_metadata", /*optional=*/true, "Verified display precision and cosmetic labels; omitted on unchanged responses; does not certify backing", {
                {T::BOOL, "known", "Immutable display precision is verified"},
                {T::NUM, "decimals", /*optional=*/true, "Verified display precision; omitted when unknown"},
                {T::STR, "ticker", "Display ticker or empty"},
                {T::STR, "name", "Display name or empty"},
                {T::STR, "source", "Metadata provenance"},
                {T::BOOL, "test_only", "Configured unbacked test asset"},
            }},
            {T::OBJ, "snapshot", "Atomic certified state plus current local service status; seat count does not prove online quorum", {
                {T::BOOL, "certified", "At least one threshold-certified microblock is available"},
                {T::NUM, "next_microblock_sequence", "Next global sequence"},
                {T::STR_HEX, "last_microblock_hash", "Certified head or zero before genesis"},
                {T::STR_HEX, "state_root", "Root belonging to this snapshot"},
                {T::NUM, "epoch", "Active epoch"},
                {T::NUM, "anchor_height", "Head's B3 anchor height; -1 before genesis"},
                {T::STR_HEX, "anchor_hash", "Head's B3 anchor, not an execution timestamp"},
                {T::NUM, "active_seats", "Canonical active-seat count"},
                {T::NUM, "quorum_required", "Certificate signature threshold"},
                {T::BOOL, "running", "Local service is running"},
                {T::BOOL, "paused", "Local execution is paused"},
                {T::BOOL, "observer_only", "Local node has no active signing seat"},
                {T::BOOL, "pending_handoff", "Committee handoff is pending"},
                {T::STR, "halt", "Local fail-closed halt state"},
                {T::STR, "error", "Local service error"},
                {T::NUM, "pending_actions", "Uncertified local queued actions; not fills"},
                {T::BOOL, "checkpoint_status_known", "False: use listflowmeshmarkets for checkpoint backlog"},
                {T::NUM, "local_observed_at", /*optional=*/true, "Local Unix seconds when this process committed the head; not certified time"},
                {T::OBJ, "runtime", "Bounded local observations for the current epoch/next sequence, not consensus or proof of live quorum; counters reset at certified advancement", {
                    {T::NUM, "round", "Current local proposal round"},
                    {T::NUM, "candidate_count", "Locally cached candidates at the current signing position"},
                    {T::NUM, "max_verified_attestations", "Largest verified attestation count for one cached candidate"},
                    {T::NUM, "active_catchup_requests", "Current bounded peer catch-up requests"},
                    {T::NUM, "proposals_missing_evidence", "Observed proposals missing locally available action evidence"},
                    {T::NUM, "proposals_rejected_round", "Observed proposals rejected for local round mismatch; not proof that their signatures were valid"},
                    {T::NUM, "attestations_without_candidate", "Decoded attestation frames naming an in-range seat received with no cached candidate; these are not verified votes"},
                    {T::NUM, "last_message_observed_at", /*optional=*/true, "Local Unix observation time of market traffic, not certified time"},
                    {T::STR_HEX, "local_locked_candidate", /*optional=*/true, "Cached local safety-lock candidate at the current signing position; no journal is read by this RPC"},
                }},
            }},
            {T::OBJ, "liquidity", /*optional=*/true, "Account-ordered certified curve page; aggregate depth is complete only when complete=true", {
                {T::NUM, "total_curves", "Total standing curves"},
                {T::BOOL, "complete", "This response contains the entire current curve set"},
                {T::STR, "next_cursor", /*optional=*/true, "Continue with this cursor and the same expected_head"},
                {T::ARR, "curves", "Standing curves", {MarketCurveResult()}},
            }},
            {T::OBJ, "account", /*optional=*/true, "Selected wallet's existing certified account; never creates an account", {
                {T::STR_HEX, "account_id", "Account identity"},
                {T::NUM, "next_sequence", "Next certified account action sequence"},
                {T::NUM, "base_available", "Available base atomic units"},
                {T::NUM, "base_reserved", "Reserved base atomic units"},
                {T::NUM, "b3_available_atoms", "Available B3 atoms"},
                {T::NUM, "b3_reserved_atoms", "Reserved B3 atoms"},
                {T::ARR, "curves", "At most the account's current bid and ask", {MarketCurveResult()}},
            }},
            {T::OBJ, "history", /*optional=*/true, "Verified execution-derived history, newest first", {
                {T::BOOL, "available", "Verified history cache is ready"},
                {T::STR, "scope", "bounded-certified-log-cache"},
                {T::BOOL, "truncated", "Older certified entries have fallen outside the retained cache"},
                {T::NUM, "oldest_retained_sequence", /*optional=*/true, "Oldest available microblock"},
                {T::NUM, "next_before_sequence", /*optional=*/true, "Exclusive cursor for another retained page"},
                {T::ARR, "entries", "Actual certified microblocks, including executions without a match", {
                    {T::OBJ, "", "Certified execution or handoff", {
                        {T::NUM, "sequence", "Global microblock sequence"},
                        {T::STR_HEX, "microblock_hash", "Certified microblock id"},
                        {T::NUM, "epoch", "Certifying epoch"},
                        {T::NUM, "anchor_height", "B3 production anchor height"},
                        {T::STR_HEX, "anchor_hash", "B3 production anchor hash"},
                        {T::STR, "kind", "execution or handoff"},
                        {T::BOOL, "cleared", "An actual nonzero match occurred"},
                        {T::NUM, "price", "Uniform clearing price in B3 atoms per base unit; zero when not cleared"},
                        {T::NUM, "quantity", "Matched base atomic units"},
                        {T::NUM, "notional_atoms", "Actual matched native B3 atoms"},
                        {T::NUM, "fee_atoms", "Exact total protocol fee in B3 atoms"},
                        {T::BOOL, "account_fills_known", "This account's fill quantities are complete; false means unknown, not no fill"},
                        {T::NUM, "account_bid_fill", "Base units bought by this account; consult account_fills_known"},
                        {T::NUM, "account_ask_fill", "Base units sold by this account; consult account_fills_known"},
                    }},
                }},
            }},
        }},
        RPCExamples{HelpExampleCli("getflowmeshmarketdata", "\"<market_id>\" '{\"limit\":50}'")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const auto wallet{GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            const uint256 market_id{ParseMarketId(request.params[0])};
            const auto query{ParseMarketDataQuery(request.params[1])};
            const auto account{ExistingWalletAccount(*wallet)};
            std::string error;
            const auto data{wallet->chain().flowMeshMarketData(market_id, account, query, error)};
            if (!data) throw JSONRPCError(RPC_MISC_ERROR, error.empty() ? "FlowMesh market data is unavailable" : error);
            UniValue out{CertifiedMarketDataJson(*data)};
            if (data->unchanged) return out;
            UniValue metadata{UniValue::VOBJ};
            {
                LOCK(wallet->cs_wallet);
                const auto asset{wallet->GetAssetMetadata(data->base_asset_id)};
                metadata.pushKV("known", asset.decimals.has_value());
                if (asset.decimals) metadata.pushKV("decimals", *asset.decimals);
                metadata.pushKV("ticker", asset.ticker);
                metadata.pushKV("name", asset.display_name);
                metadata.pushKV("source", asset.source);
                metadata.pushKV("test_only", asset.test_only);
            }
            out.pushKV("base_metadata", std::move(metadata));
            return out;
        }};
}

RPCHelpMan getflowmeshbalance()
{
    return RPCHelpMan{
        "getflowmeshbalance",
        "Return this wallet's certified FlowMesh balance for one market.\n",
        {{"market_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
          "32-byte market id"}},
        MarketStatusResult("Market and account status"),
        RPCExamples{HelpExampleCli("getflowmeshbalance", "\"<market_id>\"")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            const uint256 market_id{ParseMarketId(request.params[0])};
            const auto account{ExistingWalletAccount(*wallet)};
            if (!account) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "This wallet has no FlowMesh account yet; create a vault deposit first");
            }
            const auto status{
                wallet->chain().flowMeshMarketStatus(market_id, account)};
            if (!status) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "Unknown FlowMesh market");
            }
            return MarketStatusJson(*status);
        }};
}

RPCHelpMan listflowmeshvaultoperations()
{
    return RPCHelpMan{
        "listflowmeshvaultoperations",
        "List connected, unconsumed FlowMesh effects that can currently be "
        "published as type-9 vault transactions. This is how a publisher "
        "discovers the effect_id accepted by createflowmeshvaulttx. Publish "
        "same-market/asset withdrawals one at a time, wait for confirmation, "
        "then refresh this list because their pool inputs may overlap.\n",
        {{"market_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED,
          "Optional 32-byte market id filter"}},
        RPCResult{RPCResult::Type::ARR, "", "Publishable operations", {
            VaultOperationResult("Certified vault operation"),
        }},
        RPCExamples{
            HelpExampleCli("listflowmeshvaultoperations", "") +
            HelpExampleCli("listflowmeshvaultoperations",
                           "\"<market_id>\"")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            const std::optional<uint256> market_id{
                request.params.size() > 0 && !request.params[0].isNull()
                    ? std::optional<uint256>{ParseMarketId(request.params[0])}
                    : std::nullopt};
            std::string error;
            const auto operations{wallet->chain().flowMeshVaultOperations(
                market_id, error)};
            if (!error.empty()) {
                throw JSONRPCError(RPC_MISC_ERROR, error);
            }
            UniValue out{UniValue::VARR};
            for (const auto& operation : operations) {
                out.push_back(FlowMeshVaultOperationToJSON(operation));
            }
            return out;
        }};
}

RPCHelpMan submitflowmeshdeposit()
{
    return RPCHelpMan{
        "submitflowmeshdeposit",
        "Submit a confirmed keyless vault output to FlowMesh after its 30-block anchor depth.\n",
        {
            {"market_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "32-byte market id"},
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "Vault-deposit transaction id"},
            {"vout", RPCArg::Type::NUM, RPCArg::Optional::NO,
             "Vault-deposit output index"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Accepted deposit action", {
            {RPCResult::Type::BOOL, "accepted", "Whether the deposit action was accepted"},
            {RPCResult::Type::STR, "kind", "Action kind"},
            {RPCResult::Type::STR_HEX, "market_id", "FlowMesh market id"},
            {RPCResult::Type::STR_HEX, "deposit_txid", "Vault-deposit transaction id"},
            {RPCResult::Type::NUM, "deposit_vout", "Vault-deposit output index"},
            {RPCResult::Type::STR_HEX, "action_id", "Deposit action id"},
        }},
        RPCExamples{HelpExampleCli(
            "submitflowmeshdeposit", "\"<market_id>\" \"<txid>\" 0")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            const uint256 market_id{ParseMarketId(request.params[0])};
            const COutPoint outpoint{
                ParseOutPoint(request.params[1], request.params[2])};

            const auto account{ExistingWalletAccount(*wallet)};
            if (!account) {
                throw JSONRPCError(RPC_WALLET_ERROR,
                                   "This wallet has no FlowMesh account key");
            }
            const auto status{
                wallet->chain().flowMeshMarketStatus(market_id, account)};
            if (!status || !status->available || !status->running) {
                throw JSONRPCError(RPC_MISC_ERROR,
                                   "FlowMesh market is not running in this node");
            }
            if (status->paused) {
                throw JSONRPCError(RPC_MISC_ERROR,
                                   "FlowMesh market is paused");
            }

            {
                LOCK(wallet->cs_wallet);
                const CWalletTx* wtx{wallet->GetWalletTx(outpoint.hash)};
                if (!wtx || outpoint.n >= wtx->tx->vout.size()) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "Deposit transaction/output is not known to this wallet");
                }
                const int depth{wallet->GetTxDepthInMainChain(*wtx)};
                if (depth <= Consensus::FLOWMESH_ANCHOR_DEPTH) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        strprintf("Deposit has %d confirmations; it needs %d so its creation block is 30 blocks deep",
                                  depth,
                                  Consensus::FLOWMESH_ANCHOR_DEPTH + 1));
                }
                std::string parse_error;
                const auto output{modern::ParseAssetOutput(
                    wtx->tx->vout[outpoint.n], parse_error)};
                const auto params{output ? modern::ParseVaultParams(
                                               output->policy_params)
                                         : std::nullopt};
                if (!output || !params ||
                    output->policy_type != static_cast<uint16_t>(
                        modern::PolicyType::DEX_VAULT) ||
                    params->kind != modern::VAULT_KIND_USER_DEPOSIT ||
                    !params->account || *params->account != *account ||
                    output->policy_commitment != status->vault_id ||
                    (output->asset != status->base_asset &&
                     output->asset != modern::NativeAsset())) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "Output is not this wallet's USER_DEPOSIT for the selected market");
                }
            }

            flowmesh::Action action;
            action.type = static_cast<uint8_t>(flowmesh::ActionType::DEPOSIT);
            action.outpoint = outpoint;
            std::string error;
            if (!wallet->chain().submitFlowMeshAction(market_id, action,
                                                       error)) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    error.empty() ? "FlowMesh deposit was not accepted" : error);
            }
            UniValue out{UniValue::VOBJ};
            out.pushKV("accepted", true);
            out.pushKV("kind", QueueActionName(action.type));
            out.pushKV("market_id", market_id.GetHex());
            out.pushKV("deposit_txid", outpoint.hash.GetHex());
            out.pushKV("deposit_vout", outpoint.n);
            out.pushKV("action_id", action.Id().GetHex());
            return out;
        }};
}

RPCHelpMan submitflowmeshorder()
{
    return RPCHelpMan{
        "submitflowmeshorder",
        "Place a signed limit bid or ask in a colored-asset/B3 market. Price is integer native-B3 atomic units per colored-asset unit.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"market_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "32-byte market id"},
            {"side", RPCArg::Type::STR, RPCArg::Optional::NO,
             "'bid' or 'ask'"},
            {"price", RPCArg::Type::NUM, RPCArg::Optional::NO,
             "Limit price in B3 atomic units per base unit"},
            {"quantity", RPCArg::Type::NUM, RPCArg::Optional::NO,
             "Exact integer base-asset quantity"},
            {"sequence", RPCArg::Type::NUM, RPCArg::Optional::OMITTED,
             "Account sequence (default: current certified next sequence)"},
        },
        AcceptedActionResult("Accepted signed order", {
            {RPCResult::Type::STR, "side", "bid or ask"},
            {RPCResult::Type::NUM, "price", "Limit price in B3 atomic units per base unit"},
            {RPCResult::Type::NUM, "quantity", "Exact base-asset quantity"},
        }),
        RPCExamples{HelpExampleCli(
            "submitflowmeshorder", "\"<market_id>\" bid 10000 500")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            const uint256 market_id{ParseMarketId(request.params[0])};
            const std::string side{request.params[1].get_str()};
            const CAmount price{ExactPositiveAmount(request.params[2], "price")};
            const CAmount quantity{
                ExactPositiveAmount(request.params[3], "quantity")};
            WalletActionContext context{
                GetWalletActionContext(*wallet, market_id)};
            flowmesh::Action action;
            action.signer = context.account;
            action.sequence = OptionalSequence(
                request.params[4], context.market.next_account_sequence);
            action.type = static_cast<uint8_t>(
                side == "bid" ? flowmesh::ActionType::SUBMIT_BID
                              : side == "ask"
                                    ? flowmesh::ActionType::SUBMIT_ASK
                                    : flowmesh::ActionType::SPOT_TO_FUTURES);
            action.curve = LimitCurve(side, price, quantity);
            UniValue out{SubmitSignedAction(*wallet, context,
                                             std::move(action))};
            out.pushKV("side", side);
            out.pushKV("price", price);
            out.pushKV("quantity", quantity);
            return out;
        }};
}

RPCHelpMan cancelflowmeshorder()
{
    return RPCHelpMan{
        "cancelflowmeshorder",
        "Cancel this wallet's standing bid or ask.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"market_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "32-byte market id"},
            {"side", RPCArg::Type::STR, RPCArg::Optional::NO,
             "'bid' or 'ask'"},
            {"sequence", RPCArg::Type::NUM, RPCArg::Optional::OMITTED,
             "Account sequence (default: current certified next sequence)"},
        },
        AcceptedActionResult("Accepted signed cancellation", {
            {RPCResult::Type::STR, "side", "bid or ask"},
        }),
        RPCExamples{HelpExampleCli(
            "cancelflowmeshorder", "\"<market_id>\" ask")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            const uint256 market_id{ParseMarketId(request.params[0])};
            const std::string side{request.params[1].get_str()};
            if (side != "bid" && side != "ask") {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "side must be 'bid' or 'ask'");
            }
            WalletActionContext context{
                GetWalletActionContext(*wallet, market_id)};
            flowmesh::Action action;
            action.signer = context.account;
            action.sequence = OptionalSequence(
                request.params[2], context.market.next_account_sequence);
            action.type = static_cast<uint8_t>(
                side == "bid" ? flowmesh::ActionType::CANCEL_BID
                              : flowmesh::ActionType::CANCEL_ASK);
            UniValue out{SubmitSignedAction(*wallet, context,
                                             std::move(action))};
            out.pushKV("side", side);
            return out;
        }};
}

RPCHelpMan requestflowmeshwithdrawal()
{
    return RPCHelpMan{
        "requestflowmeshwithdrawal",
        "Create a signed FlowMesh withdrawal request. This creates a certified request; a later connected checkpoint and type-9 vault transaction perform the on-chain payout.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"market_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "32-byte market id"},
            {"asset", RPCArg::Type::STR, RPCArg::Optional::NO,
             "Base asset id, or 'B3'"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO,
             "B3 decimal amount or exact integer colored-asset units"},
            {"destination", RPCArg::Type::STR, RPCArg::Optional::NO,
             "Owner address for the eventual payout"},
            {"sequence", RPCArg::Type::NUM, RPCArg::Optional::OMITTED,
             "Account sequence (default: current certified next sequence)"},
        },
        AcceptedActionResult("Accepted signed request", {
            {RPCResult::Type::STR, "asset", "B3 or the colored-asset id"},
            {RPCResult::Type::NUM, "amount", "B3 decimal amount or exact colored-asset units"},
            {RPCResult::Type::STR, "destination", "Destination owner address"},
            {RPCResult::Type::STR_HEX, "destination_owner_commitment", "Destination owner commitment"},
            {RPCResult::Type::STR, "status", "Withdrawal request status"},
        }),
        RPCExamples{HelpExampleCli(
            "requestflowmeshwithdrawal",
            "\"<market_id>\" B3 5 \"<address>\"")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            const uint256 market_id{ParseMarketId(request.params[0])};
            const modern::AssetId asset{ParseAssetOrB3(request.params[1])};
            const CAmount amount{
                asset == modern::NativeAsset()
                    ? AmountFromValue(request.params[2])
                    : ExactPositiveAmount(request.params[2], "amount")};
            if (amount <= 0 || amount > MAX_MONEY) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "amount is out of range");
            }
            const CTxDestination destination{
                DecodeDestination(request.params[3].get_str())};
            if (!IsValidDestination(destination)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                                   "Invalid destination address");
            }
            const CScript destination_script{
                GetScriptForDestination(destination)};
            WalletActionContext context{
                GetWalletActionContext(*wallet, market_id)};
            if (asset != modern::NativeAsset() &&
                asset != context.market.base_asset) {
                throw JSONRPCError(
                    RPC_INVALID_PARAMETER,
                    "asset must be this market's base asset or B3");
            }
            flowmesh::Action action;
            action.signer = context.account;
            action.sequence = OptionalSequence(
                request.params[4], context.market.next_account_sequence);
            action.type = static_cast<uint8_t>(flowmesh::ActionType::WITHDRAW);
            action.asset = asset;
            action.amount = amount;
            action.destination =
                modern::AssetOwnerCommitment(destination_script);
            UniValue out{SubmitSignedAction(*wallet, context,
                                             std::move(action))};
            out.pushKV("asset", asset == modern::NativeAsset()
                                    ? "B3"
                                    : asset.GetHex());
            out.pushKV("amount", asset == modern::NativeAsset()
                                     ? ValueFromAmount(amount)
                                     : UniValue{amount});
            out.pushKV("destination", request.params[3].get_str());
            out.pushKV("destination_owner_commitment",
                       modern::AssetOwnerCommitment(destination_script)
                           .GetHex());
            out.pushKV("status", "requested");
            return out;
        }};
}

RPCHelpMan startflowmeshvalidator()
{
    return RPCHelpMan{
        "startflowmeshvalidator",
        "Replace the NODE-GLOBAL armed FN keys with every FN-seat BLS key held by this wallet. "
        "This also replaces any keys loaded by another wallet. Keys remain in node memory; secrets are never returned. "
        "Supply the fingerprint from getflowmeshvalidatorinfo to reject a stale approval atomically. "
        "Omitting it preserves the legacy unconditional behavior. Loaded keys do not prove signing.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {{"expected_armed_keys_fingerprint", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED,
          "Expected node-global armed-key snapshot; reject without replacing keys if it changed"}},
        FlowMeshValidatorStatusResult(/*include_markets=*/false, /*legacy_fields=*/true),
        RPCExamples{HelpExampleCli("startflowmeshvalidator", "")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            const auto expected{ExpectedArmedKeys(request.params[0])};
            std::vector<bls::SecretKey> keys;
            std::vector<std::array<unsigned char, bls::PUBKEY_SIZE>> public_keys;
            {
                LOCK(wallet->cs_wallet);
                EnsureWalletIsUnlocked(*wallet);
                public_keys = wallet->ListFlowMeshBlsPubkeys();
                for (const auto& pubkey : public_keys) {
                    const auto key{wallet->GetFlowMeshBlsKey(pubkey)};
                    if (!key) {
                        throw JSONRPCError(RPC_WALLET_ERROR,
                                           util::ErrorString(key).original);
                    }
                    keys.push_back(*key);
                }
            }
            if (keys.empty()) {
                throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "This wallet has no FlowMesh BLS seat keys; bind a seat first");
            }
            std::string error;
            interfaces::FlowMeshValidatorStatus status;
            if (!wallet->chain().armFlowMeshSeatKeys(keys, error, expected, &status)) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    error.empty() ? "Unable to start FlowMesh validator"
                                  : error);
            }
            UniValue out{FlowMeshValidatorStatusToJSON(status, std::move(public_keys))};
            out.pushKV("running", !status.armed_pubkeys.empty());
            out.pushKV("armed_keys", static_cast<uint64_t>(status.armed_pubkeys.size()));
            return out;
        }};
}

RPCHelpMan stopflowmeshvalidator()
{
    return RPCHelpMan{
        "stopflowmeshvalidator",
        "Remove ALL node-global armed FN keys, including keys loaded by other wallets. "
        "Supply the fingerprint from getflowmeshvalidatorinfo to reject a stale approval atomically. "
        "Omitting it preserves the legacy unconditional behavior.\n",
        {{"expected_armed_keys_fingerprint", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED,
          "Expected node-global armed-key snapshot; reject without clearing keys if it changed"}},
        FlowMeshValidatorStatusResult(/*include_markets=*/false, /*legacy_fields=*/true),
        RPCExamples{HelpExampleCli("stopflowmeshvalidator", "")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            const auto expected{ExpectedArmedKeys(request.params[0])};
            std::vector<std::array<unsigned char, bls::PUBKEY_SIZE>> public_keys;
            {
                LOCK(wallet->cs_wallet);
                public_keys = wallet->ListFlowMeshBlsPubkeys();
            }
            std::string error;
            interfaces::FlowMeshValidatorStatus status;
            if (!wallet->chain().disarmFlowMeshSeatKeys(error, expected, &status)) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    error.empty() ? "Unable to stop FlowMesh validator"
                                  : error);
            }
            UniValue out{FlowMeshValidatorStatusToJSON(status, std::move(public_keys))};
            out.pushKV("running", false);
            out.pushKV("armed_keys", static_cast<uint64_t>(status.armed_pubkeys.size()));
            return out;
        }};
}

RPCHelpMan createflowmeshcheckpoint()
{
    return RPCHelpMan{
        "createflowmeshcheckpoint",
        "Create the service-selected next certified FlowMesh entry as one "
        "type-8 MPA record in an ordinary wallet-funded B3 transaction. "
        "By default the signed transaction is committed and broadcast. "
        "Set options.broadcast=false to return the signed transaction for "
        "review without committing or broadcasting it; send its exact hex "
        "with sendrawtransaction to publish it. Prepared inputs are not "
        "reserved and may be spent before publication.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"market_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "32-byte market id"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED,
             "Transaction options", {
                 {"broadcast", RPCArg::Type::BOOL, RPCArg::Default{true},
                  "Commit and broadcast the signed transaction"},
             }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Signed checkpoint transaction", {
            {RPCResult::Type::STR_HEX, "txid", "Transaction id"},
            {RPCResult::Type::STR_HEX, "ptxid", "Payload-aware transaction id"},
            {RPCResult::Type::STR_HEX, "hex", "Serialized transaction"},
            {RPCResult::Type::STR_HEX, "market_id", "FlowMesh market id"},
            {RPCResult::Type::STR_HEX, "checkpoint_id", "Checkpoint id"},
            {RPCResult::Type::NUM, "sequence", "Checkpoint sequence"},
            {RPCResult::Type::NUM, "effect_count", "Effects committed by the checkpoint"},
            {RPCResult::Type::STR_AMOUNT, "network_fee", "Native B3 network fee"},
            {RPCResult::Type::BOOL, "broadcast", "Whether the transaction was broadcast"},
        }},
        RPCExamples{
            HelpExampleCli("createflowmeshcheckpoint", "\"<market_id>\"") +
            HelpExampleCli("createflowmeshcheckpoint",
                           "\"<market_id>\" '{\"broadcast\":false}'")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();
            const bool broadcast{ParseBroadcastOption(request.params[1])};
            const uint256 market_id{ParseMarketId(request.params[0])};
            std::string service_error;
            const auto pending{wallet->chain().nextFlowMeshCheckpoint(
                market_id, service_error)};
            if (!pending) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    service_error.empty()
                        ? "No certified FlowMesh entry is awaiting a checkpoint"
                        : service_error);
            }
            if (pending->record.payload_type !=
                    modern::MPA_TYPE_FLOWMESH_CHECKPOINT ||
                pending->record.payload_version != modern::MPA_VERSION_V1) {
                throw JSONRPCError(
                    RPC_INTERNAL_ERROR,
                    "FlowMesh service returned a non-type-8 checkpoint record");
            }

            CreatedTransactionResult created{nullptr, 0, std::nullopt, {}};
            {
                LOCK(wallet->cs_wallet);
                EnsureWalletIsUnlocked(*wallet);
                if (wallet->IsWalletFlagSet(
                        WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "Private keys are disabled for this wallet");
                }
                const auto destination{wallet->GetNewDestination(
                    wallet->m_default_address_type,
                    "b3-flowmesh-checkpoint")};
                if (!destination) {
                    throw JSONRPCError(
                        RPC_WALLET_KEYPOOL_RAN_OUT,
                        util::ErrorString(destination).original);
                }
                const CScript script{GetScriptForDestination(*destination)};
                const CAmount self_amount{std::max<CAmount>(
                    1, GetDustThreshold(CTxOut{0, script},
                                        wallet->chain().relayDustFee()))};
                const std::vector<CRecipient> recipients{
                    CRecipient{*destination, self_amount,
                               /*fSubtractFeeFromAmount=*/false}};
                CCoinControl coin_control;
                coin_control.m_min_depth = 1;
                const ModernTransactionOptions modern_options{
                    .mpa = {pending->record}};
                auto result{CreateTransaction(
                    *wallet, recipients,
                    /*change_pos=*/static_cast<unsigned int>(
                        recipients.size()),
                    coin_control, /*sign=*/true, modern_options)};
                if (!result) {
                    throw JSONRPCError(
                        RPC_WALLET_INSUFFICIENT_FUNDS,
                        util::ErrorString(result).original);
                }
                created = std::move(*result);
            }
            if (broadcast) {
                wallet->CommitTransaction(
                    created.tx,
                    {{"b3", "flowmesh-checkpoint"},
                     {"b3_network_fee", FormatMoney(created.fee)}},
                    /*orderForm=*/{});
            }

            UniValue out{UniValue::VOBJ};
            out.pushKV("txid", created.tx->GetHash().GetHex());
            out.pushKV("ptxid", created.tx->GetPtxid().GetHex());
            out.pushKV("hex", EncodeHexTx(*created.tx));
            out.pushKV("market_id", market_id.GetHex());
            out.pushKV("checkpoint_id", pending->checkpoint_id.GetHex());
            out.pushKV("sequence", pending->sequence);
            out.pushKV("effect_count", pending->effect_count);
            out.pushKV("network_fee", ValueFromAmount(created.fee));
            out.pushKV("broadcast", broadcast);
            return out;
        }};
}

RPCHelpMan createflowmeshvaulttx()
{
    return RPCHelpMan{
        "createflowmeshvaulttx",
        "Create one certified type-9 FlowMesh deposit sweep or withdrawal. "
        "The service selects the connected proof and exact keyless vault "
        "inputs; this wallet supplies and signs a separate native-B3 fee "
        "input. A destination is required only for a withdrawal. "
        "By default the signed transaction is committed and broadcast. "
        "Set options.broadcast=false to return the signed transaction for "
        "review without committing or broadcasting it; send its exact hex "
        "with sendrawtransaction to publish it. Prepared inputs are not "
        "reserved and may be spent before publication.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"effect_id", RPCArg::Type::STR_HEX, RPCArg::Optional::NO,
             "Certified deposit-acceptance or withdrawal-receipt id"},
            {"destination", RPCArg::Type::STR, RPCArg::Optional::OMITTED,
             "Exact owner address committed by a withdrawal receipt"},
            {"options", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED,
             "Transaction options", {
                 {"broadcast", RPCArg::Type::BOOL, RPCArg::Default{true},
                  "Commit and broadcast the signed transaction"},
             }},
        },
        RPCResult{RPCResult::Type::OBJ, "", "Signed vault transaction", {
            {RPCResult::Type::STR_HEX, "txid", "Transaction id"},
            {RPCResult::Type::STR_HEX, "ptxid", "Payload-aware transaction id"},
            {RPCResult::Type::STR_HEX, "hex", "Serialized transaction"},
            {RPCResult::Type::STR, "operation", "deposit-sweep or withdrawal"},
            {RPCResult::Type::STR_HEX, "effect_id", "Certified effect id"},
            {RPCResult::Type::STR_HEX, "checkpoint_id", "Authorizing checkpoint id"},
            {RPCResult::Type::STR_HEX, "market_id", "FlowMesh market id"},
            {RPCResult::Type::STR, "asset", "B3 or the colored-asset id"},
            {RPCResult::Type::NUM, "amount", "B3 decimal amount or exact colored-asset units"},
            {RPCResult::Type::NUM, "vault_inputs", "Number of consumed keyless vault inputs"},
            {RPCResult::Type::STR_AMOUNT, "network_fee", "Native B3 network fee"},
            {RPCResult::Type::BOOL, "broadcast", "Whether the transaction was broadcast"},
        }},
        RPCExamples{
            HelpExampleCli("createflowmeshvaulttx", "\"<acceptance_id>\"") +
            HelpExampleCli("createflowmeshvaulttx",
                           "\"<receipt_id>\" \"<address>\"") +
            HelpExampleCli("-named createflowmeshvaulttx",
                           "effect_id=\"<acceptance_id>\" options='{\"broadcast\":false}'")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const std::shared_ptr<CWallet> wallet{
                GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            wallet->BlockUntilSyncedToCurrentChain();

            const bool broadcast{ParseBroadcastOption(request.params[2])};
            const uint256 effect_id{ParseHashV(request.params[0],
                                               "effect_id")};
            if (effect_id.IsNull()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   "effect_id cannot be zero");
            }
            std::string service_error;
            const auto operation{wallet->chain().flowMeshVaultOperation(
                effect_id, service_error)};
            if (!operation) {
                throw JSONRPCError(
                    RPC_MISC_ERROR,
                    service_error.empty()
                        ? "No connected, unspent FlowMesh effect has this id"
                        : service_error);
            }
            if (FlowMeshEffectId(operation->effect) != effect_id ||
                operation->market_id.IsNull() ||
                operation->checkpoint_id.IsNull() ||
                operation->inputs.empty() || operation->inputs.size() > 64 ||
                operation->record.payload_type !=
                    modern::MPA_TYPE_FLOWMESH_VAULT_PROOF ||
                operation->record.payload_version != modern::MPA_VERSION_V1) {
                throw JSONRPCError(
                    RPC_INTERNAL_ERROR,
                    "FlowMesh service returned a malformed vault operation");
            }

            const bool has_destination{request.params.size() > 1 &&
                                       !request.params[1].isNull()};
            std::vector<CTxOut> fixed_outputs;
            std::string operation_kind;
            modern::AssetId operation_asset;
            CAmount operation_amount{0};
            flowmesh::VaultId operation_vault;

            if (const auto* deposit{
                    std::get_if<modern::FlowMeshDepositAcceptanceV1>(
                        &operation->effect)}) {
                if (has_destination) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "destination must be omitted for a deposit sweep");
                }
                if (operation->inputs.size() != 1 ||
                    deposit->market_id != operation->market_id ||
                    deposit->amount <= 0 || deposit->amount > MAX_MONEY ||
                    deposit->vault_id.IsNull()) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "FlowMesh service returned an invalid deposit sweep");
                }
                const auto pool{modern::MakeDexVaultOutput(
                    deposit->asset, deposit->amount, deposit->vault_id,
                    modern::VAULT_KIND_POOL_CHANGE, deposit->shard)};
                if (!pool) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "Unable to encode the certified deposit pool output");
                }
                fixed_outputs.push_back(*pool);
                operation_kind = "deposit-sweep";
                operation_asset = deposit->asset;
                operation_amount = deposit->amount;
                operation_vault = deposit->vault_id;
            } else {
                const auto& receipt{
                    std::get<modern::FlowMeshWithdrawalReceiptV1>(
                        operation->effect)};
                if (!has_destination) {
                    throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "destination is required for a withdrawal receipt");
                }
                const CTxDestination destination{
                    DecodeDestination(request.params[1].get_str())};
                if (!IsValidDestination(destination)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                                       "Invalid destination address");
                }
                const CScript destination_script{
                    GetScriptForDestination(destination)};
                if (modern::AssetOwnerCommitment(destination_script) !=
                    receipt.destination_owner_commitment) {
                    throw JSONRPCError(
                        RPC_INVALID_ADDRESS_OR_KEY,
                        "Destination does not match the certified withdrawal request");
                }
                if (receipt.market_id != operation->market_id ||
                    receipt.amount <= 0 || receipt.amount > MAX_MONEY ||
                    receipt.vault_id.IsNull()) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "FlowMesh service returned an invalid withdrawal receipt");
                }

                if (receipt.asset == modern::NativeAsset()) {
                    // An ordinary post-H native output projects as OWNER and
                    // commits to this exact script. Native B3 never uses the
                    // B3A1 colored-asset carrier.
                    fixed_outputs.emplace_back(receipt.amount,
                                               destination_script);
                } else {
                    const auto payout{modern::MakeAssetOwnerOutput(
                        receipt.asset, receipt.amount, destination_script)};
                    if (!payout) {
                        throw JSONRPCError(
                            RPC_INTERNAL_ERROR,
                            "Unable to encode the certified asset payout");
                    }
                    fixed_outputs.push_back(*payout);
                }
                operation_kind = "withdrawal";
                operation_asset = receipt.asset;
                operation_amount = receipt.amount;
                operation_vault = receipt.vault_id;
            }

            std::set<COutPoint> unique_inputs;
            CAmount vault_input_total{0};
            for (const auto& input : operation->inputs) {
                if (input.outpoint.IsNull() ||
                    !unique_inputs.insert(input.outpoint).second) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "FlowMesh service returned duplicate or null vault inputs");
                }
                std::string parse_error;
                const auto output{
                    modern::ParseAssetOutput(input.txout, parse_error)};
                const auto params{output ? modern::ParseVaultParams(
                                               output->policy_params)
                                         : std::nullopt};
                if (!output || !params ||
                    output->policy_type != static_cast<uint16_t>(
                        modern::PolicyType::DEX_VAULT) ||
                    output->asset != operation_asset ||
                    output->policy_commitment != operation_vault ||
                    output->amount <= 0 ||
                    vault_input_total > MAX_MONEY - output->amount) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "FlowMesh service returned a mismatched vault input");
                }
                if (operation_kind == "deposit-sweep") {
                    const auto& deposit{
                        std::get<modern::FlowMeshDepositAcceptanceV1>(
                            operation->effect)};
                    if (input.outpoint != deposit.deposit_outpoint ||
                        params->kind != modern::VAULT_KIND_USER_DEPOSIT ||
                        !params->account || *params->account != deposit.account ||
                        params->shard != deposit.shard ||
                        output->amount != deposit.amount) {
                        throw JSONRPCError(
                            RPC_INTERNAL_ERROR,
                            "FlowMesh service selected the wrong deposit input");
                    }
                } else if (params->kind !=
                               modern::VAULT_KIND_POOL_CHANGE ||
                           params->account) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "FlowMesh service selected a non-pool withdrawal input");
                }
                vault_input_total += output->amount;
            }

            if (operation_kind == "deposit-sweep") {
                if (vault_input_total != operation_amount) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "Deposit input amount does not match its acceptance");
                }
            } else {
                if (vault_input_total < operation_amount) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "Selected vault inputs do not cover the withdrawal");
                }
                const CAmount vault_change{vault_input_total -
                                           operation_amount};
                if (vault_change > 0) {
                    const auto& receipt{
                        std::get<modern::FlowMeshWithdrawalReceiptV1>(
                            operation->effect)};
                    const auto change{modern::MakeDexVaultOutput(
                        receipt.asset, vault_change, receipt.vault_id,
                        modern::VAULT_KIND_POOL_CHANGE,
                        receipt.deterministic_change_shard)};
                    if (!change) {
                        throw JSONRPCError(
                            RPC_INTERNAL_ERROR,
                            "Unable to encode deterministic vault change");
                    }
                    fixed_outputs.push_back(*change);
                }
            }

            CreatedTransactionResult created{nullptr, 0, std::nullopt, {}};
            CTransactionRef signed_tx;
            {
                LOCK(wallet->cs_wallet);
                EnsureWalletIsUnlocked(*wallet);
                if (wallet->IsWalletFlagSet(
                        WALLET_FLAG_DISABLE_PRIVATE_KEYS)) {
                    throw JSONRPCError(
                        RPC_WALLET_ERROR,
                        "Private keys are disabled for this wallet");
                }

                CCoinControl coin_control;
                coin_control.m_allow_other_inputs = true;
                coin_control.m_min_depth = 1;
                for (const auto& input : operation->inputs) {
                    PreselectedInput& selected{
                        coin_control.Select(input.outpoint)};
                    selected.SetTxOut(input.txout);
                    // A keyless input has only outpoint, empty script length,
                    // and sequence: 41 non-witness bytes = 164 weight units.
                    selected.SetInputWeight(164);
                    selected.SetScriptSig(CScript{});
                    selected.SetScriptWitness(CScriptWitness{});
                }

                std::vector<CRecipient> recipients;
                recipients.reserve(fixed_outputs.size());
                for (const CTxOut& output : fixed_outputs) {
                    recipients.push_back(ExactOutputRecipient(output));
                }
                const ModernTransactionOptions modern_options{
                    .mpa = {operation->record}};
                auto result{CreateTransaction(
                    *wallet, recipients,
                    /*change_pos=*/static_cast<unsigned int>(
                        recipients.size()),
                    coin_control, /*sign=*/false, modern_options)};
                if (!result) {
                    throw JSONRPCError(
                        RPC_WALLET_INSUFFICIENT_FUNDS,
                        util::ErrorString(result).original);
                }
                created = std::move(*result);

                if (created.tx->vin.size() <= operation->inputs.size()) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "Vault transaction lacks a separate native fee input");
                }
                for (size_t i{0}; i < operation->inputs.size(); ++i) {
                    if (created.tx->vin[i].prevout !=
                            operation->inputs[i].outpoint ||
                        !created.tx->vin[i].scriptSig.empty() ||
                        !created.tx->vin[i].scriptWitness.IsNull()) {
                        throw JSONRPCError(
                            RPC_INTERNAL_ERROR,
                            "Wallet did not preserve the ordered keyless vault inputs");
                    }
                }

                CMutableTransaction mutable_tx{*created.tx};
                std::map<COutPoint, Coin> coins;
                for (const CTxIn& input : mutable_tx.vin) {
                    coins[input.prevout];
                }
                wallet->chain().findCoins(coins);
                for (const auto& input : operation->inputs) {
                    const auto it{coins.find(input.outpoint)};
                    if (it == coins.end() || it->second.IsSpent() ||
                        it->second.out != input.txout) {
                        throw JSONRPCError(
                            RPC_VERIFY_REJECTED,
                            "A selected vault input changed or was spent; retry");
                    }
                }

                std::map<int, bilingual_str> input_errors;
                const bool complete{wallet->SignTransaction(
                    mutable_tx, coins, SIGHASH_DEFAULT, input_errors)};
                if (complete) {
                    throw JSONRPCError(
                        RPC_INTERNAL_ERROR,
                        "Keyless vault inputs were unexpectedly signed");
                }
                for (size_t i{0}; i < mutable_tx.vin.size(); ++i) {
                    const bool vault_input{i < operation->inputs.size()};
                    const bool empty{
                        mutable_tx.vin[i].scriptSig.empty() &&
                        mutable_tx.vin[i].scriptWitness.IsNull()};
                    if ((vault_input && !empty) ||
                        (!vault_input && input_errors.contains(i))) {
                        throw JSONRPCError(
                            RPC_WALLET_ERROR,
                            "Wallet could not sign every native fee input while preserving keyless custody");
                    }
                }
                signed_tx = MakeTransactionRef(std::move(mutable_tx));
            }

            if (broadcast) {
                wallet->CommitTransaction(
                    signed_tx,
                    {{"b3", "flowmesh-" + operation_kind},
                     {"b3_network_fee", FormatMoney(created.fee)}},
                    /*orderForm=*/{});
            }

            UniValue out{UniValue::VOBJ};
            out.pushKV("txid", signed_tx->GetHash().GetHex());
            out.pushKV("ptxid", signed_tx->GetPtxid().GetHex());
            out.pushKV("hex", EncodeHexTx(*signed_tx));
            out.pushKV("operation", operation_kind);
            out.pushKV("effect_id", effect_id.GetHex());
            out.pushKV("checkpoint_id",
                       operation->checkpoint_id.GetHex());
            out.pushKV("market_id", operation->market_id.GetHex());
            out.pushKV("asset", operation_asset == modern::NativeAsset()
                                    ? "B3"
                                    : operation_asset.GetHex());
            out.pushKV("amount", operation_asset == modern::NativeAsset()
                                     ? ValueFromAmount(operation_amount)
                                     : UniValue{operation_amount});
            out.pushKV("vault_inputs",
                       static_cast<uint64_t>(operation->inputs.size()));
            out.pushKV("network_fee", ValueFromAmount(created.fee));
            out.pushKV("broadcast", broadcast);
            return out;
        }};
}

} // namespace wallet
