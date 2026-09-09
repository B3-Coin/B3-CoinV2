// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshtrading.h>
#include <qt/b3flowmeshmarketdata.h>
#include <core_io.h>
#include <flowmesh/market.h>
#include <key_io.h>
#include <modern/asset_output.h>
#include <modern/flowmesh_checkpoint.h>
#include <modern/flowmesh_vault_proof.h>
#include <modern/mpa.h>
#include <node/transaction.h>
#include <policy/policy.h>
#include <rpc/util.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace B3FlowMeshTrading {
namespace {
[[noreturn]] void Fail(const char* message) { throw std::runtime_error{message}; }
const UniValue& Field(const UniValue& value, const char* name) {
    if (!value.isObject()) Fail("Malformed FlowMesh response.");
    return value.find_value(name);
}
QString Text(const UniValue& value) {
    if (!value.isStr() || value.get_str().size() > 1024) Fail("Invalid FlowMesh text field.");
    return QString::fromStdString(value.get_str());
}
uint256 Hash(const QString& text, bool zero = false) {
    const auto hash{uint256::FromHex(text.toStdString())};
    if (text.size() != 64 || !hash || (!zero && hash->IsNull())) Fail("Invalid FlowMesh identifier.");
    return *hash;
}
QString Id(const UniValue& value) { const auto text{Text(value).toLower()}; Hash(text); return text; }
bool Flag(const UniValue& value) { if (!value.isBool()) Fail("Invalid FlowMesh status flag."); return value.get_bool(); }
int64_t Integer(const UniValue& value) {
    if (!value.isNum()) Fail("Expected an exact integer, not a quoted or floating-point amount.");
    const auto n{value.getInt<int64_t>()};
    if (n < 0) Fail("Negative FlowMesh count or amount.");
    return n;
}
CAmount Money(const UniValue& value, bool native) {
    const CAmount n{native ? AmountFromValue(value) : Integer(value)};
    if (!MoneyRange(n)) Fail("FlowMesh amount is out of range.");
    return n;
}
void Positive(CAmount n) { if (n <= 0 || !MoneyRange(n)) Fail("Enter a positive amount within the consensus range."); }
void Address(const QString& text) {
    if (text.size() > 256 || !IsValidDestination(DecodeDestination(text.toStdString()))) Fail("Enter a valid B3 destination address.");
}
bool Sequenced(Operation op) { return op == Operation::Order || op == Operation::Cancel || op == Operation::Withdraw; }
bool Settlement(Operation op) { return op == Operation::Checkpoint || op == Operation::Vault; }
void Owned(const CTxOut& output, const std::function<bool(const CTxDestination&)>& owned) {
    const auto owner{modern::AssetOwnerScript(output)};
    CTxDestination destination;
    if (!ExtractDestination(owner ? *owner : output.scriptPubKey, destination) || !owned(destination)) {
        Fail("An unreviewed output does not belong to the captured wallet.");
    }
}
void Pool(const CTxOut& output, const uint256& asset, const uint256& vault, uint16_t shard) {
    const auto parsed{modern::ParseAssetOutput(output)};
    const auto params{parsed ? modern::ParseVaultParams(parsed->policy_params) : std::nullopt};
    if (!parsed || !params || parsed->asset != asset || parsed->policy_commitment != vault ||
        parsed->policy_type != static_cast<uint16_t>(modern::PolicyType::DEX_VAULT) ||
        params->kind != modern::VAULT_KIND_POOL_CHANGE || params->account || params->shard != shard) {
        Fail("The prepared vault change differs from the certified operation.");
    }
}
} // namespace

Market ParseMarket(const UniValue& value)
{
    Market m;
    m.id = Id(Field(value, "market_id")); m.base = Id(Field(value, "base_asset_id"));
    m.vault = Id(Field(value, "vault_id")); m.domain = Id(Field(value, "domain"));
    m.config = Id(Field(value, "execution_config_id"));
    const auto market{flowmesh::ComputeFlowMeshMarketId(Hash(m.domain), Hash(m.base))};
    const auto vault{flowmesh::ComputeFlowMeshVaultId(Hash(m.domain), Hash(m.id))};
    if (!market || *market != Hash(m.id) || !vault || *vault != Hash(m.vault) || Text(Field(value, "quote_asset")) != QStringLiteral("B3")) {
        Fail("Market, base asset and vault are not bound to the same chain domain.");
    }
    const bool available{Flag(Field(value, "available"))}, running{Flag(Field(value, "running"))};
    const bool paused{Flag(Field(value, "paused"))}, handoff{Flag(Field(value, "pending_handoff"))};
    const QString halt{Text(Field(value, "halt"))}, error{Text(Field(value, "error"))};
    m.ready = available && running && !paused && !handoff && halt == QStringLiteral("none") && error.isEmpty();
    // A certified genesis/handoff checkpoint can be precisely what clears a
    // pause. Connected vault effects likewise remain publishable while paused;
    // proof validity and actual fees are independently checked by mempool acceptance.
    m.publish_ready = available && running;
    m.reason = !available || !running ? QStringLiteral("Market runtime is unavailable.") : paused ? QStringLiteral("Market paused: validator quorum is not ready.") :
        handoff ? QStringLiteral("Validator handoff is pending.") : halt != QStringLiteral("none") ? QStringLiteral("Market halted: ") + halt : error;
    m.checkpoint_pending = Flag(Field(value, "checkpoint_pending"));
    if (m.checkpoint_pending) m.checkpoint = Id(Field(value, "pending_checkpoint_id"));
    const auto& account{Field(value, "account")};
    if (!account.isNull()) {
        m.has_account = true; m.account = Id(Field(account, "account_id"));
        m.sequence = Integer(Field(account, "next_sequence"));
        m.base_available = Money(Field(account, "base_available"), false);
        m.base_reserved = Money(Field(account, "base_reserved"), false);
        m.b3_available = Money(Field(account, "b3_available"), true);
        m.b3_reserved = Money(Field(account, "b3_reserved"), true);
    }
    return m;
}

std::vector<Market> ParseMarkets(const UniValue& value)
{
    if (!value.isArray() || value.size() > 1000) Fail("Invalid or oversized FlowMesh market list.");
    std::vector<Market> markets; std::set<QString> ids;
    for (const auto& entry : value.getValues()) {
        auto m{ParseMarket(entry)};
        if (!ids.insert(m.id).second) Fail("Duplicate FlowMesh market.");
        markets.push_back(std::move(m));
    }
    return markets;
}

Market ReadMarket(const QString& id, const RpcCall& rpc)
{
    for (const auto& m : ParseMarkets(rpc("listflowmeshmarkets", UniValue{UniValue::VARR}))) {
        if (m.id != id) continue;
        if (!m.has_account) return m;
        UniValue params{UniValue::VARR}; params.push_back(id.toStdString());
        const auto fresh{ParseMarket(rpc("getflowmeshbalance", params))};
        if (fresh.id != m.id || fresh.account != m.account) Fail("Wallet or market changed during refresh.");
        return fresh;
    }
    Fail("The selected market is no longer reported by this node.");
}

void CheckSameMarket(const Market& reviewed, const Market& fresh, bool sequence, bool settlement)
{
    if (!(settlement ? fresh.publish_ready : fresh.ready)) Fail("The market is not ready for this operation; refresh its runtime status.");
    if (reviewed.id != fresh.id || reviewed.base != fresh.base || reviewed.vault != fresh.vault ||
        reviewed.domain != fresh.domain || reviewed.config != fresh.config ||
        (reviewed.has_account && (!fresh.has_account || reviewed.account != fresh.account))) Fail("The reviewed wallet/market binding changed.");
    if (sequence && (!fresh.has_account || reviewed.sequence != fresh.sequence)) Fail("The account sequence changed. Review the action again; nothing was retried.");
}

void CheckSynced(const UniValue& chain)
{
    if (Flag(Field(chain, "initialblockdownload")) || Integer(Field(chain, "headers")) > Integer(Field(chain, "blocks"))) {
        Fail("Wait for the B3 node to finish synchronizing.");
    }
}

Request Parameters(const Action& a)
{
    Hash(a.market.id); Hash(a.market.base); Hash(a.market.vault); Hash(a.market.domain);
    if (!(Settlement(a.operation) ? a.market.publish_ready : a.market.ready)) Fail("The market is not ready for this wallet action.");
    if (Sequenced(a.operation) && (!a.market.has_account || a.market.sequence > INT64_MAX)) Fail("A certified wallet account and valid sequence are required.");
    Request r; r.params.push_back(a.market.id.toStdString());
    UniValue options{UniValue::VOBJ}; options.pushKV("broadcast", false);
    switch (a.operation) {
    case Operation::Order:
        if (a.side != QStringLiteral("bid") && a.side != QStringLiteral("ask")) Fail("Choose bid or ask.");
        Positive(a.price); Positive(a.amount);
        if (a.price > MAX_MONEY / a.amount) Fail("The order's B3 notional exceeds the consensus range.");
        if (a.side == QStringLiteral("bid") && (a.price >= MAX_MONEY || a.price > MAX_MONEY / a.amount || a.price * a.amount > a.market.b3_available)) Fail("The bid exceeds the available certified B3 balance or price range.");
        if (a.side == QStringLiteral("ask") && a.amount > a.market.base_available) Fail("The ask exceeds the available certified asset balance.");
        r.method = "submitflowmeshorder"; r.params.push_back(a.side.toStdString()); r.params.push_back(a.price); r.params.push_back(a.amount); r.params.push_back(a.market.sequence); break;
    case Operation::Cancel:
        if (a.side != QStringLiteral("bid") && a.side != QStringLiteral("ask")) Fail("Choose bid or ask.");
        r.method = "cancelflowmeshorder"; r.params.push_back(a.side.toStdString()); r.params.push_back(a.market.sequence); break;
    case Operation::Withdraw:
        Positive(a.amount); Address(a.destination);
        if (a.amount > (a.native ? a.market.b3_available : a.market.base_available)) Fail("The withdrawal exceeds the available certified balance.");
        r.method = "requestflowmeshwithdrawal"; r.params.push_back(a.native ? "B3" : a.market.base.toStdString());
        r.params.push_back(a.native ? ValueFromAmount(a.amount) : UniValue{a.amount}); r.params.push_back(a.destination.toStdString()); r.params.push_back(a.market.sequence); break;
    case Operation::Deposit:
        Positive(a.amount); r.method = "flowmeshdeposit"; r.params.clear(); r.params.setArray(); r.params.push_back(a.market.base.toStdString());
        r.params.push_back(a.native ? "B3" : a.market.base.toStdString()); r.params.push_back(a.native ? ValueFromAmount(a.amount) : UniValue{a.amount});
        options.pushKV("minconf", 1); options.pushKV("include_unsafe", false); options.pushKV("replaceable", false); r.params.push_back(options); break;
    case Operation::Admit:
        Hash(a.txid); if (!a.market.has_account) Fail("Create a wallet vault deposit first.");
        r.method = "submitflowmeshdeposit"; r.params.push_back(a.txid.toStdString()); r.params.push_back(a.vout); break;
    case Operation::Checkpoint:
        if (!a.market.checkpoint_pending) Fail("No certified checkpoint is awaiting publication.");
        r.method = "createflowmeshcheckpoint"; r.params.push_back(options); break;
    case Operation::Vault:
        if (Id(Field(a.effect, "market_id")) != a.market.id || Id(Field(a.effect, "account_id")) != a.market.account) Fail("The effect is not bound to this wallet and market.");
        if (Text(Field(a.effect, "asset")) != QStringLiteral("B3") && Text(Field(a.effect, "asset")) != a.market.base) Fail("The effect asset does not belong to this market.");
        r.method = "createflowmeshvaulttx"; r.params.clear(); r.params.setArray(); r.params.push_back(Id(Field(a.effect, "effect_id")).toStdString());
        if (Text(Field(a.effect, "kind")) == QStringLiteral("withdrawal")) { Address(a.destination); r.params.push_back(a.destination.toStdString()); }
        else if (Text(Field(a.effect, "kind")) == QStringLiteral("deposit-sweep")) r.params.push_back(UniValue{});
        else Fail("Unknown certified vault effect kind.");
        r.params.push_back(options); break;
    }
    return r;
}

UniValue DispatchApproved(const Action& a, bool approved, const std::function<bool()>& cancelled, const RpcCall& rpc)
{
    if (!approved || cancelled()) Fail("Action cancelled; nothing was submitted.");
    auto current{a}; current.market = ReadMarket(a.market.id, rpc);
    CheckSameMarket(a.market, current.market, Sequenced(a.operation), Settlement(a.operation));
    if (a.operation == Operation::Checkpoint && a.market.checkpoint != current.market.checkpoint) Fail("The pending checkpoint changed. Inspect the new certificate before preparing it.");
    const auto request{Parameters(current)};
    if (cancelled()) Fail("Action cancelled; nothing was submitted.");
    return rpc(request.method, request.params);
}

QString Describe(const Action& a)
{
    const QString prefix{QStringLiteral("Market: %1\nFull base asset ID: %2\nWallet account: %3\n").arg(a.market.id, a.market.base, a.market.has_account ? a.market.account : QStringLiteral("created during deposit preparation"))};
    QString base_amount = QString::number(a.amount) + QStringLiteral(" raw base-asset units");
    if (a.display_decimals) base_amount = B3FlowMeshMarketData::FormatAmount(a.amount, *a.display_decimals) + QLatin1Char(' ') + a.display_ticker + QStringLiteral(" (%1 atomic units)").arg(a.amount);
    QString amount{base_amount};
    if (a.native) amount = QString::fromStdString(FormatMoney(a.amount)) + QStringLiteral(" B3");
    switch (a.operation) {
    case Operation::Order: {
        Positive(a.price); Positive(a.amount);
        if (a.price > MAX_MONEY / a.amount) Fail("The order's B3 notional exceeds the consensus range.");
        QString price = QString::number(a.price) + QStringLiteral(" B3 atoms per raw base unit");
        if (a.display_decimals) price = B3FlowMeshMarketData::FormatPrice(a.price, *a.display_decimals) + QStringLiteral(" B3 / ") + a.display_ticker;
        return prefix + QStringLiteral("Limit %1: %2 at %3.\nExact limit notional: %4 B3\nAccount sequence: %5\nOne uniform-price curve auction, not price-time priority. Accepted is not filled. Protocol fee is 0.01% of matched notional deducted from seller proceeds; actual allocation depends on certified fills. No network fee for this request.").arg(a.side == QStringLiteral("bid") ? QStringLiteral("Buy") : QStringLiteral("Sell"), base_amount, price, QString::fromStdString(FormatMoney(a.price * a.amount)), QString::number(a.market.sequence));
    }
    case Operation::Cancel: return prefix + QStringLiteral("Cancel this account's standing %1; sequence %2. Cancellation is not certified until processed.").arg(a.side, QString::number(a.market.sequence));
    case Operation::Deposit: return prefix + QStringLiteral("Deposit %1 into a KEYLESS vault. Funds may remain locked if the validator quorum fails. Preparation creates a wallet trading-account key if needed; back up this wallet even if you cancel. Admission needs 31 confirmations.").arg(amount);
    case Operation::Admit: return prefix + QStringLiteral("Admit existing deposit %1:%2. Must have at least 31 confirmations. Admission is not completed settlement.").arg(a.txid).arg(a.vout);
    case Operation::Withdraw: return prefix + QStringLiteral("Request withdrawal of %1 to %2; sequence %3. This is NOT an on-chain payout. A certified checkpoint and a separately fee-funded vault transaction must follow.").arg(amount, a.destination, QString::number(a.market.sequence));
    case Operation::Checkpoint: return prefix + QStringLiteral("Publish reviewed pending checkpoint: %1. Native B3 pays the network fee.").arg(a.market.checkpoint);
    case Operation::Vault: return prefix + QStringLiteral("Publish certified %1\nEffect: %2\nAsset: %3\nExact amount: %4\nDestination: %5\nWait for confirmation before publishing another overlapping vault effect.").arg(Text(Field(a.effect,"kind")), Id(Field(a.effect,"effect_id")), Text(Field(a.effect,"asset")), QString::fromStdString(Field(a.effect,"amount").write()), a.destination);
    }
    return prefix;
}

void CheckActionResult(const UniValue& value, const Action& a)
{
    if (!Flag(Field(value, "accepted")) || Id(Field(value, "market_id")) != a.market.id) Fail("The node did not confirm the expected action. Its outcome may be unknown; do not retry.");
    Id(Field(value, "action_id"));
    if (Sequenced(a.operation) && (Id(Field(value, "account_id")) != a.market.account || Integer(Field(value, "sequence")) != static_cast<int64_t>(a.market.sequence))) Fail("The accepted action differs from the reviewed wallet or sequence; inspect state before retrying.");
}

B3AssetTransfer::Prepared ParsePrepared(const UniValue& value, const Action& a, const std::function<bool(const CTxDestination&)>& owned)
{
    Parameters(a);
    if (Flag(Field(value, "broadcast")) || Id(Field(value, "market_id")) != a.market.id) Fail("Preparation unexpectedly broadcast or selected another market.");
    const QString txid{Id(Field(value,"txid"))}; const auto& encoded{Field(value,"hex")};
    if (!encoded.isStr() || encoded.get_str().empty() || encoded.get_str().size() > 2 * MAX_STANDARD_TX_WEIGHT || !IsHex(encoded.get_str())) Fail("Invalid prepared transaction bytes.");
    CMutableTransaction mutable_tx;
    if (!DecodeHexTx(mutable_tx, encoded.get_str())) Fail("Cannot decode the prepared FlowMesh transaction.");
    const CTransaction tx{mutable_tx};
    if (tx.GetHash().GetHex() != txid.toStdString() || tx.IsCoinBase() || tx.vin.empty() || tx.vout.empty()) Fail("Prepared transaction identity or structure is invalid.");
    const CAmount fee{Money(Field(value,"network_fee"), true)};
    if (fee > node::DEFAULT_MAX_RAW_TX_FEE_RATE.GetFee(GetVirtualTransactionSize(tx))) Fail("Prepared transaction exceeds the network fee cap.");
    const size_t keyless{a.operation == Operation::Vault ? static_cast<size_t>(Integer(Field(value,"vault_inputs"))) : 0};
    if ((a.operation == Operation::Vault && keyless == 0) || keyless > 64 || keyless >= tx.vin.size()) Fail("Invalid keyless/native fee-input count.");
    for (size_t i{0}; i < tx.vin.size(); ++i) {
        const auto& input{tx.vin[i]}; const bool empty{input.scriptSig.empty() && input.scriptWitness.IsNull()};
        if (input.prevout.IsNull() || (i < keyless ? !empty : empty)) Fail("Unexpected unsigned or keyless input in prepared transaction.");
    }
    for (const auto& output : tx.vout) if (!MoneyRange(output.nValue)) Fail("Invalid prepared output amount.");
    const uint256 asset{a.native ? modern::NativeAsset() : Hash(a.market.base)};
    if (a.operation == Operation::Deposit) {
        const auto output{modern::ParseAssetOutput(tx.vout[0])};
        const auto params{output ? modern::ParseVaultParams(output->policy_params) : std::nullopt};
        const auto account{Hash(Id(Field(value,"account_id")))};
        if (!tx.mpa.empty() || !output || !params || output->asset != asset || output->amount != a.amount ||
            output->policy_type != static_cast<uint16_t>(modern::PolicyType::DEX_VAULT) || output->policy_commitment != Hash(a.market.vault) ||
            params->kind != modern::VAULT_KIND_USER_DEPOSIT || !params->account || *params->account != account ||
            (a.market.has_account && account != Hash(a.market.account)) || params->shard != modern::FlowMeshUserDepositShard(Hash(a.market.vault), account) ||
            Id(Field(value,"vault_id")) != a.market.vault || Hash(Text(Field(value,"asset_id")), true) != asset ||
            Money(Field(value,"amount"), a.native) != a.amount || Id(Field(value,"deposit_txid")) != txid || Integer(Field(value,"deposit_vout")) != 0 ||
            Money(Field(value,"disintegration"), true) != 0) Fail("Prepared deposit differs from the reviewed keyless vault, amount, asset or account.");
        if (!Field(value,"market_bootstrap").isNull() && Flag(Field(value,"market_bootstrap"))) Fail("Market bootstrapping is not authorized by this panel.");
        CAmount change{0};
        for (size_t i{1}; i < tx.vout.size(); ++i) {
            Owned(tx.vout[i], owned);
            if (const auto parsed{modern::ParseAssetOutput(tx.vout[i])}) {
                if (a.native || parsed->asset != asset || parsed->policy_type != static_cast<uint16_t>(modern::PolicyType::OWNER) || change > MAX_MONEY - parsed->amount) Fail("Unexpected deposit asset change.");
                change += parsed->amount;
            }
        }
        if (Integer(Field(value,"asset_change")) != change) Fail("Prepared deposit change does not match the response.");
    } else if (a.operation == Operation::Checkpoint) {
        if (tx.mpa.size() != 1 || tx.mpa[0].payload_type != modern::MPA_TYPE_FLOWMESH_CHECKPOINT || tx.mpa[0].payload_version != modern::MPA_VERSION_V1) Fail("Expected one checkpoint proof.");
        const auto checkpoint{modern::DecodeFlowMeshCheckpointEnvelopeV1(tx.mpa[0].payload)};
        const auto id{checkpoint ? modern::FlowMeshCheckpointIdV1(checkpoint->core) : std::nullopt};
        if (!checkpoint || !id || *id != Hash(a.market.checkpoint) || Id(Field(value,"checkpoint_id")) != a.market.checkpoint || checkpoint->core.market_id != Hash(a.market.id) || checkpoint->core.domain != Hash(a.market.domain)) Fail("The prepared checkpoint is not the reviewed pending certificate.");
        for (const auto& output : tx.vout) { if (modern::ClaimsAssetOutput(output.scriptPubKey)) Fail("Unexpected asset output in a checkpoint transaction."); Owned(output, owned); }
    } else if (a.operation == Operation::Vault) {
        if (tx.mpa.size() != 1 || tx.mpa[0].payload_type != modern::MPA_TYPE_FLOWMESH_VAULT_PROOF || tx.mpa[0].payload_version != modern::MPA_VERSION_V1) Fail("Expected one connected vault proof.");
        const auto proof{modern::DecodeFlowMeshVaultProofV1(tx.mpa[0].payload)};
        if (!proof || proof->checkpoint_id != Hash(Id(Field(a.effect,"checkpoint_id"))) || Id(Field(value,"effect_id")) != Id(Field(a.effect,"effect_id")) || Text(Field(value,"operation")) != Text(Field(a.effect,"kind"))) Fail("Prepared vault proof differs from the reviewed effect.");
        size_t wallet_change{1};
        std::visit([&](const auto& effect) {
            if (effect.market_id != Hash(a.market.id) || effect.account != Hash(a.market.account) || effect.vault_id != Hash(a.market.vault) ||
                effect.asset != (Text(Field(a.effect,"asset")) == QStringLiteral("B3") ? modern::NativeAsset() : Hash(a.market.base)) ||
                effect.amount != Money(Field(a.effect,"amount"), effect.asset == modern::NativeAsset())) Fail("Certified effect differs from the reviewed wallet, amount, asset or vault.");
            using Effect = std::decay_t<decltype(effect)>;
            if constexpr (std::is_same_v<Effect, modern::FlowMeshDepositAcceptanceV1>) {
                if (effect.acceptance_id != Hash(Id(Field(a.effect,"effect_id"))) || keyless != 1 || tx.vin[0].prevout != effect.deposit_outpoint) Fail("Unexpected deposit sweep input or effect.");
                Pool(tx.vout[0], effect.asset, effect.vault_id, effect.shard);
                if (modern::ParseAssetOutput(tx.vout[0])->amount != effect.amount) Fail("Deposit sweep amount mismatch.");
            } else {
                if (effect.receipt_id != Hash(Id(Field(a.effect,"effect_id")))) Fail("Withdrawal receipt mismatch.");
                const CScript destination{GetScriptForDestination(DecodeDestination(a.destination.toStdString()))};
                if (effect.destination_owner_commitment != modern::AssetOwnerCommitment(destination)) Fail("Destination differs from the certified withdrawal.");
                if (effect.asset == modern::NativeAsset()) {
                    if (tx.vout[0].nValue != effect.amount || tx.vout[0].scriptPubKey != destination) Fail("Native payout differs from the review.");
                } else {
                    const auto payout{modern::ParseAssetOutput(tx.vout[0])}; const auto owner{modern::AssetOwnerScript(tx.vout[0])};
                    if (!payout || !owner || payout->asset != effect.asset || payout->amount != effect.amount || *owner != destination) Fail("Asset payout differs from the review.");
                }
                if (tx.vout.size() > 1 && modern::ClaimsAssetOutput(tx.vout[1].scriptPubKey)) { Pool(tx.vout[1], effect.asset, effect.vault_id, effect.deterministic_change_shard); wallet_change = 2; }
            }
        }, proof->effect);
        for (size_t i{wallet_change}; i < tx.vout.size(); ++i) { if (modern::ClaimsAssetOutput(tx.vout[i].scriptPubKey)) Fail("Unexpected additional asset output in vault transaction."); Owned(tx.vout[i], owned); }
    } else Fail("This action does not prepare an on-chain transaction.");
    return {txid, encoded.get_str(), fee, a.amount, a.destination};
}
} // namespace B3FlowMeshTrading
