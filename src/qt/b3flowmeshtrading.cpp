// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.
#include <qt/b3flowmeshtrading.h>
#include <flowmesh/microblock.h>
#include <qt/b3flowmeshmarketdata.h>
#include <core_io.h>
#include <flowmesh/batch.h>
#include <flowmesh/market.h>
#include <flowmesh/p2p.h>
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
#include <algorithm>
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
    const auto& verification{Field(value, "verification")};
    if (!verification.isNull()) {
        const auto source{Text(Field(verification, "source"))};
        if (source != QStringLiteral("local_engine") && source != QStringLiteral("remote_endpoint")) Fail("Unsupported market verification source.");
        if (source == QStringLiteral("remote_endpoint")) {
            m.remote = true;
            m.ready = m.ready && Flag(Field(verification, "certificate_verified")) && Flag(Field(verification, "account_state_verified")) && !Text(Field(verification, "endpoint")).isEmpty();
            m.publish_ready = m.publish_ready && Flag(Field(verification, "certificate_verified")) && Flag(Field(verification, "account_state_verified"));
        }
    }
    m.reason = !available || !running ? QStringLiteral("Market service is unavailable.") : paused ? QStringLiteral("Market paused: inspect endpoint readiness and checkpoint progress.") :
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
        // Remote discovery intentionally has no account/state proof. Read only
        // the selected market, including first-deposit wallets with no account.
        if (!m.has_account && !m.remote) return m;
        UniValue params{UniValue::VARR}; params.push_back(id.toStdString());
        const auto fresh{ParseMarket(rpc("getflowmeshbalance", params))};
        if (fresh.id != m.id || fresh.base != m.base || fresh.vault != m.vault || fresh.domain != m.domain || fresh.config != m.config || fresh.remote != m.remote || (m.has_account && fresh.account != m.account)) Fail("Wallet or market changed during refresh.");
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
    case Operation::Order: {
        if (a.side != QStringLiteral("bid") && a.side != QStringLiteral("ask")) Fail("Choose bid or ask.");
        if (!a.ladder.empty()) {
            // A ladder: 2-4 distinct prices as one curve; its reservation
            // must fit this side's available plus already reserved balance.
            const bool bid{a.side == QStringLiteral("bid")};
            if (a.ladder.size() < 2 || a.ladder.size() > flowmesh::FLOWMESH_LADDER_MAX_STEPS) Fail("A ladder order has 2 to 4 prices.");
            std::vector<flowmesh::LadderStep> steps;
            UniValue list{UniValue::VARR};
            for (const auto& [price, quantity] : a.ladder) {
                Positive(price); Positive(quantity);
                steps.push_back({price, quantity});
                UniValue step{UniValue::VOBJ}; step.pushKV("price", price); step.pushKV("quantity", quantity);
                list.push_back(std::move(step));
            }
            const auto reservation{flowmesh::LadderReservation(bid, steps)};
            const auto budget{B3FlowMeshMarketData::ReplacementBudget(bid ? a.market.b3_available : a.market.base_available,
                                                                     bid ? a.market.b3_reserved : a.market.base_reserved)};
            if (!(bid ? flowmesh::MakeLadderBidCurve(steps) : flowmesh::MakeLadderAskCurve(steps)) || !reservation) Fail("The ladder's prices must differ and its total must stay within the consensus range.");
            if (!budget || *reservation > *budget) Fail("The ladder exceeds this side's available plus already reserved certified balance.");
            r.method = "submitflowmeshladder"; r.params.push_back(a.side.toStdString()); r.params.push_back(list); r.params.push_back(a.market.sequence);
            if (a.submit_wait_ms > 0) r.params.push_back(a.submit_wait_ms);
            break;
        }
        Positive(a.price); Positive(a.amount);
        if (a.price > MAX_MONEY / a.amount) Fail("The order's B3 notional exceeds the consensus range.");
        const bool bid{a.side == QStringLiteral("bid")};
        const auto budget{B3FlowMeshMarketData::ReplacementBudget(bid ? a.market.b3_available : a.market.base_available,
                                                                 bid ? a.market.b3_reserved : a.market.base_reserved)};
        if (!budget || (bid ? a.price >= MAX_MONEY || a.price * a.amount > *budget : a.amount > *budget)) Fail("The replacement exceeds this side's available plus already reserved certified balance or price range.");
        r.method = "submitflowmeshorder"; r.params.push_back(a.side.toStdString()); r.params.push_back(a.price); r.params.push_back(a.amount); r.params.push_back(a.market.sequence);
        if (a.submit_wait_ms > 0) r.params.push_back(a.submit_wait_ms);
        break;
    }
    case Operation::Cancel:
        if (a.side != QStringLiteral("bid") && a.side != QStringLiteral("ask")) Fail("Choose bid or ask.");
        r.method = "cancelflowmeshorder"; r.params.push_back(a.side.toStdString()); r.params.push_back(a.market.sequence);
        if (a.submit_wait_ms > 0) r.params.push_back(a.submit_wait_ms);
        break;
    case Operation::Withdraw:
        Positive(a.amount); Address(a.destination);
        if (a.amount > (a.native ? a.market.b3_available : a.market.base_available)) Fail("The withdrawal exceeds the available certified balance.");
        r.method = "requestflowmeshwithdrawal"; r.params.push_back(a.native ? "B3" : a.market.base.toStdString());
        r.params.push_back(a.native ? ValueFromAmount(a.amount) : UniValue{a.amount}); r.params.push_back(a.destination.toStdString()); r.params.push_back(a.market.sequence);
        if (a.submit_wait_ms > 0) r.params.push_back(a.submit_wait_ms);
        break;
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

int SubmissionWaitMs(const Operation operation, const bool remote, const bool wallet_was_unlocked, const int wait_ms)
{
    return remote && wallet_was_unlocked && Sequenced(operation) && wait_ms > 0 ? wait_ms : 0;
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

namespace {
QString BaseAmount(CAmount amount, std::optional<int> decimals, const QString& ticker)
{
    if (!decimals) return QStringLiteral("%1 raw units").arg(amount);
    return B3FlowMeshMarketData::FormatAmount(amount, *decimals) + QLatin1Char(' ') + (ticker.isEmpty() ? QStringLiteral("token") : ticker);
}
QString SideTitle(bool bid, bool inverse)
{
    // Inverse view: a canonical bid spends B3 (Sell B3), an ask receives it.
    if (inverse) return bid ? QStringLiteral("Sell B3") : QStringLiteral("Buy B3");
    return bid ? QStringLiteral("Buy") : QStringLiteral("Sell");
}
QString OrderTitle(bool bid, CAmount price, CAmount quantity, std::optional<int> decimals, const QString& ticker, bool inverse)
{
    const QString side{SideTitle(bid, inverse && decimals)};
    if (!decimals) return QStringLiteral("%1 %2 @ %3 B3 atoms per raw unit").arg(side, BaseAmount(quantity, decimals, ticker)).arg(price);
    const QString token{ticker.isEmpty() ? QStringLiteral("token") : ticker};
    // A short label: the shared display formatter. The review and the saved
    // request's details show the exact signed limit.
    const QString limit{B3FlowMeshMarketData::FormatDisplayPrice(price, *decimals, inverse)};
    if (inverse) {
        return QStringLiteral("%1 · %2 %3 · limit %4 %5/B3").arg(side, bid ? QStringLiteral("receive up to") : QStringLiteral("spend up to"),
            BaseAmount(quantity, decimals, ticker), limit, token);
    }
    return QStringLiteral("%1 %2 @ %3 B3/%4").arg(side, BaseAmount(quantity, decimals, ticker), limit, token);
}
//! The marketable-limit disclosure, in the review's own orientation and units.
QString MarketableDisclosure(const Action& a)
{
    if (!a.marketable) return {};
    const auto& m{*a.marketable};
    const QString token{a.display_ticker.isEmpty() ? QStringLiteral("token") : a.display_ticker};
    const QString units{a.inverse_display ? QStringLiteral("%1 / B3").arg(token) : QStringLiteral("B3 / %1").arg(token)};
    const QString depth{a.display_decimals ? QString{B3FlowMeshMarketData::FormatAmount(m.visible, *a.display_decimals) + QLatin1Char(' ') + token}
                                           : QString{QString::number(m.visible) + QStringLiteral(" raw base-asset units")}};
    return QStringLiteral("\nMarketable limit, not a market order: priced from the best certified opposite order at %1 %2 with at most %3% slippage. "
                          "The auction clears at one uniform price, never worse than the limit above. Certified opposite depth at or better than that limit: %4 "
                          "(visible depth, not a fill estimate). Any unfilled part stays as a standing order until you cancel it: FlowMesh has no market or immediate-or-cancel orders.")
        .arg(m.reference_display, units, B3FlowMeshMarketData::FormatSlippage(m.slippage_bps), depth);
}
//! What a new order replaces on its side, in the review's orientation.
QString ReplacesDisclosure(const Action& a)
{
    if (!a.replaces) return {};
    const auto& r{*a.replaces};
    const QString side{SideTitle(a.side == QStringLiteral("bid"), a.inverse_display)};
    const auto amount = [&](CAmount n) { return BaseAmount(n, a.display_decimals, a.display_ticker); };
    const QString current{r.limit ? QStringLiteral("%1 order (%2 left; %3 already filled)").arg(side, amount(r.remaining), amount(r.filled))
                          : r.ladder_prices ? QStringLiteral("%1 ladder of %2 prices (%3 left)").arg(side).arg(r.ladder_prices).arg(amount(r.remaining))
                                            : QStringLiteral("%1 curve of %2 points (%3 left)").arg(side).arg(r.points).arg(amount(r.remaining))};
    return QStringLiteral("\nReplaces your current %1. The new quantity counts from zero; the reservation is adjusted by the difference.").arg(current);
}

//! The review of a ladder order: every price with its quantity, the exact
//! reservation, and what a ladder is (one order on its side).
QString DescribeLadder(const Action& a, const QString& prefix)
{
    const bool bid{a.side == QStringLiteral("bid")};
    std::vector<flowmesh::LadderStep> steps;
    for (const auto& [price, quantity] : a.ladder) { Positive(price); Positive(quantity); steps.push_back({price, quantity}); }
    const auto reservation{flowmesh::LadderReservation(bid, steps)};
    if (a.ladder.size() < 2 || !reservation) Fail("Invalid ladder order.");
    if (a.inverse_display && !a.display_decimals) Fail("Verified asset precision is required for the inverse order review.");
    // Best first: a bid's highest price, an ask's lowest.
    std::sort(steps.begin(), steps.end(), [&](const auto& x, const auto& y) { return bid ? x.price > y.price : x.price < y.price; });
    const QString token{a.display_ticker.isEmpty() ? QStringLiteral("token") : a.display_ticker};
    const auto amount = [&](CAmount n) { return BaseAmount(n, a.display_decimals, a.display_ticker); };
    const auto price = [&](CAmount p) {
        if (!a.display_decimals) return QStringLiteral("%1 B3 atoms per raw unit").arg(p);
        return a.inverse_display ? QStringLiteral("%1 %2 / B3").arg(B3FlowMeshMarketData::ExactInversePrice(p, *a.display_decimals), token)
                                 : QStringLiteral("%1 B3 / %2").arg(B3FlowMeshMarketData::FormatPrice(p, *a.display_decimals), token);
    };
    CAmount total{0};
    QString lines;
    for (const auto& step : steps) {
        total += step.qty;
        lines += a.inverse_display
                     ? QStringLiteral("\n  • %1 %2 at %3 %4 (exact)").arg(bid ? QStringLiteral("receive up to") : QStringLiteral("spend up to"), amount(step.qty),
                                                                          bid ? QStringLiteral("no less than") : QStringLiteral("no more than"), price(step.price))
                     : QStringLiteral("\n  • %1 at %2").arg(amount(step.qty), price(step.price));
    }
    QString review{prefix + QStringLiteral("Ladder %1 of %2 prices, ONE standing order on this side:%3\nTotal quantity: %4.")
                                .arg(SideTitle(bid, a.inverse_display)).arg(steps.size()).arg(lines, amount(total))};
    review += bid ? QStringLiteral("\nExact B3 reservation: %1 B3, the sum of quantity x price (as separate limit orders would reserve).").arg(QString::fromStdString(FormatMoney(*reservation)))
                  : QStringLiteral("\nReserves %1 (the sum of the quantities). The B3 received depends on the auction fills, before the allocated B3 fee.").arg(amount(*reservation));
    review += QStringLiteral("\nExact canonical steps (B3 atoms per token atom : token atoms):");
    for (const auto& step : steps) review += QStringLiteral(" %1 : %2;").arg(step.price).arg(step.qty);
    if (a.display_limit_adjusted) review += QStringLiteral("\nAt least one entered price was adjusted only in your favor to the exact executable price listed above; those exact prices are what will be signed.");
    review += QStringLiteral("\nAccount sequence: %1. One uniform-price curve auction per microblock, not price-time priority. Accepted is not filled. No network fee for this request.").arg(a.market.sequence);
    review += QStringLiteral("\nA ladder is one order: a price cannot be cancelled or edited alone, and cancelling or replacing this side removes every price. Wallets before this version show it only as a curve.");
    return review + ReplacesDisclosure(a);
}
} // namespace

QString Describe(const Action& a)
{
    const QString prefix{QStringLiteral("Market: %1\nFull base asset ID: %2\nWallet account: %3\n").arg(a.market.id, a.market.base, a.market.has_account ? a.market.account : QStringLiteral("created during deposit preparation"))};
    QString base_amount = QString::number(a.amount) + QStringLiteral(" raw base-asset units");
    if (a.display_decimals) base_amount = B3FlowMeshMarketData::FormatAmount(a.amount, *a.display_decimals) + QLatin1Char(' ') + a.display_ticker + QStringLiteral(" (%1 atomic units)").arg(a.amount);
    QString amount{base_amount};
    if (a.native) amount = QString::fromStdString(FormatMoney(a.amount)) + QStringLiteral(" B3");
    switch (a.operation) {
    case Operation::Order: {
        if (!a.ladder.empty()) return DescribeLadder(a, prefix);
        Positive(a.price); Positive(a.amount);
        if (a.price > MAX_MONEY / a.amount) Fail("The order's B3 notional exceeds the consensus range.");
        if (a.inverse_display) {
            if (!a.display_decimals) Fail("Verified asset precision is required for the inverse order review.");
            const bool buy{B3FlowMeshMarketData::DisplayBuy(a.side, true)};
            const auto executable{B3FlowMeshMarketData::ExactInversePrice(a.price, *a.display_decimals)};
            QString review{prefix + QStringLiteral("%1 B3: %2 %3.\nExecutable limit: %4 %5 %6 / B3 (gross, before fee).\n%7\nThe exact order quantity is in %6, NOT B3. This is a quantity cap, not a guaranteed fill. Partial fills and a better clearing price change the B3 amount.\n%8\nCanonical market: %6/B3. Canonical side: %9; account sequence: %10. This replaces your existing order on that side.\nUniform-price curve auction; accepted is not filled. No network fee for this request.")
                .arg(buy ? QStringLiteral("Buy") : QStringLiteral("Sell"), buy ? QStringLiteral("Spend up to") : QStringLiteral("Receive up to"), base_amount,
                     buy ? QStringLiteral("no more than") : QStringLiteral("no less than"), executable, a.display_ticker,
                     (buy ? QStringLiteral("Estimated gross B3 at the limit: %1 B3, IF the full token cap fills. Better clearing can increase gross B3; partial fills reduce it.")
                          : QStringLiteral("Maximum B3 reservation/spend: %1 B3. Better clearing may spend less; no exact B3 sale is promised.")).arg(QString::fromStdString(FormatMoney(a.price * a.amount))),
                     buy ? QStringLiteral("Actual trading fee asset: B3. The whole-auction 0.01% fee is allocated from matched B3 proceeds and deducted from the B3 you receive; your allocated fee and net B3 are unknown before execution. The price limit above is gross, not fee-inclusive.")
                         : QStringLiteral("Actual trading fee asset: B3, paid from the counterparty's B3 received, not deducted from your token receipt. The exact whole-auction fee allocation is unknown before execution."),
                     a.side, QString::number(a.market.sequence))};
            review += QStringLiteral("\nExact canonical limit: %1 B3 atoms per token atom; quantity cap: %2 token atoms.").arg(a.price).arg(a.amount);
            if (a.display_limit_adjusted) review += QStringLiteral("\nYour entered limit was %1 %2 / B3. It was adjusted only in your favor to the executable limit %3 %2 / B3 shown above; that exact canonical limit is what will be signed.").arg(a.entered_display_limit, a.display_ticker, executable);
            return review + ReplacesDisclosure(a) + MarketableDisclosure(a);
        }
        QString price = QString::number(a.price) + QStringLiteral(" B3 atoms per raw base unit");
        if (a.display_decimals) price = B3FlowMeshMarketData::FormatPrice(a.price, *a.display_decimals) + QStringLiteral(" B3 / ") + a.display_ticker;
        return prefix + QStringLiteral("Limit %1: %2 at %3.\nExact limit notional: %4 B3\nAccount sequence: %5\nOne uniform-price curve auction, not price-time priority. Accepted is not filled. Protocol fee is 0.01% of matched notional deducted from seller proceeds; actual allocation depends on certified fills. No network fee for this request.").arg(a.side == QStringLiteral("bid") ? QStringLiteral("Buy") : QStringLiteral("Sell"), base_amount, price, QString::fromStdString(FormatMoney(a.price * a.amount)), QString::number(a.market.sequence)) + ReplacesDisclosure(a) + MarketableDisclosure(a);
    }
    case Operation::Cancel: return prefix + QStringLiteral("Cancel this account's standing %1 (%2); sequence %3. Cancellation is not certified until processed.").arg(
        a.inverse_display ? (B3FlowMeshMarketData::DisplayBuy(a.side, true) ? QStringLiteral("Buy B3") : QStringLiteral("Sell B3")) : a.side,
        a.side, QString::number(a.market.sequence));
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
    ParseReceipt(value, a.market.id);
    if (Sequenced(a.operation) && (Id(Field(value, "account_id")) != a.market.account || Integer(Field(value, "sequence")) != static_cast<int64_t>(a.market.sequence))) Fail("The accepted action differs from the reviewed wallet or sequence; inspect state before retrying.");
}

Receipt ParseReceipt(const UniValue& value, const QString& market, const QString& expected_action)
{
    if (Id(Field(value, "market_id")) != market) Fail("Action receipt belongs to another market; outcome remains unknown.");
    Receipt r; r.action_id = Id(Field(value, "action_id")); r.market = market;
    if (!Field(value, "account_id").isNull()) r.account = Id(Field(value, "account_id"));
    if (!expected_action.isEmpty() && r.action_id != expected_action) Fail("Action receipt does not match the saved action ID.");
    const bool accepted{Flag(Field(value, "accepted"))};
    if (Field(value, "receipt_state").isNull()) {
        // Older local RPC acceptance meant queue admission, never execution.
        r.state = accepted ? QStringLiteral("queued") : QStringLiteral("unknown");
        return r;
    }
    r.state = Text(Field(value, "receipt_state")); r.reason = Text(Field(value, "reason")); r.endpoint = Text(Field(value, "endpoint"));
    r.certificate_verified = Flag(Field(value, "certificate_verified")); r.outcome_verified = Flag(Field(value, "outcome_verified"));
    const bool admitted{r.state == QStringLiteral("queued") || r.state == QStringLiteral("admitted") || r.state == QStringLiteral("certified_inclusion")};
    if ((!admitted && r.state != QStringLiteral("unknown") && r.state != QStringLiteral("rejected")) || accepted != admitted) Fail("Inconsistent action receipt state.");
    if (!Field(value, "microblock_hash").isNull()) {
        r.microblock_hash = Id(Field(value, "microblock_hash")); r.microblock_sequence = Integer(Field(value, "microblock_sequence"));
    }
    if (r.state == QStringLiteral("certified_inclusion") && (!r.certificate_verified || r.microblock_hash.isEmpty())) Fail("Action inclusion lacks verified certificate evidence.");
    if (!Field(value, "reason_code").isNull()) r.reason_code = Text(Field(value, "reason_code"));
    if (!Field(value, "stage").isNull()) r.stage = Text(Field(value, "stage"));
    if (!Field(value, "outcome").isNull()) r.outcome = ParseOutcome(Field(value, "outcome"));
    if (r.outcome_verified != r.outcome.has_value()) Fail("A verified outcome flag and its outcome disagree.");
    if (r.outcome) {
        if (r.outcome->status == QStringLiteral("not_executed")) {
            // Proven never to execute: a rejection naming the consumer.
            if (r.state != QStringLiteral("rejected") || r.outcome->consumed_by.isEmpty()) Fail("A not-executed outcome lacks its consuming action.");
        } else if (!r.Included() || r.outcome->microblock_hash != r.microblock_hash) {
            Fail("An execution outcome cannot precede verified action inclusion.");
        }
    }
    return r;
}

Outcome ParseOutcome(const UniValue& value)
{
    Outcome o;
    o.status = Text(Field(value, "status"));
    if (o.status != QStringLiteral("applied") && o.status != QStringLiteral("rejected") &&
        o.status != QStringLiteral("included_not_executed") && o.status != QStringLiteral("not_executed")) Fail("Unknown verified outcome status.");
    o.final = Flag(Field(value, "final"));
    o.reason_code = Text(Field(value, "reason_code"));
    o.reason_text = Text(Field(value, "reason_text"));
    o.evidence = Text(Field(value, "evidence"));
    o.microblock_sequence = Integer(Field(value, "microblock_sequence"));
    o.microblock_hash = Id(Field(value, "microblock_hash"));
    if (!Field(value, "fill").isNull()) {
        const auto& fill{Field(value, "fill")};
        Outcome::Fill f;
        f.side = Text(Field(fill, "side"));
        if (f.side != QStringLiteral("bid") && f.side != QStringLiteral("ask")) Fail("Unknown verified fill side.");
        f.cleared = Flag(Field(fill, "cleared")); f.superseded_in_entry = Flag(Field(fill, "superseded_in_entry"));
        f.quantity = Money(Field(fill, "quantity"), false); f.price = Money(Field(fill, "price"), false);
        f.quote_atoms = Money(Field(fill, "quote_atoms"), false); f.fee_atoms = Money(Field(fill, "fee_atoms"), false);
        f.net_quote_atoms = Money(Field(fill, "net_quote_atoms"), false);
        o.fill = f;
    }
    if (!Field(value, "withdrawal").isNull()) o.withdrawal_amount = Money(Field(Field(value, "withdrawal"), "amount"), false);
    if (!Field(value, "deposit").isNull()) o.deposit_amount = Money(Field(Field(value, "deposit"), "amount"), false);
    if (!Field(value, "consumed_by").isNull()) {
        o.consumed_by = Id(Field(Field(value, "consumed_by"), "action_id"));
        o.consumed_in_sequence = Integer(Field(Field(value, "consumed_by"), "microblock_sequence"));
    }
    // An unrecognised reason is displayed as such and never changes finality.
    static const std::set<QString> known{QStringLiteral("APPLIED"), QStringLiteral("REJECTED_BY_STATE"),
        QStringLiteral("INCLUDED_BAD_SEQUENCE"), QStringLiteral("INCLUDED_EQUIVOCATION"), QStringLiteral("INCLUDED_MALFORMED"),
        QStringLiteral("DEPOSIT_NOT_CREDITED"), QStringLiteral("NOT_EXECUTED_SEQUENCE_CONSUMED")};
    if (!known.contains(o.reason_code)) {
        o.final = false;
        o.reason_text = QStringLiteral("Unrecognised reason; outcome unchanged.");
    }
    return o;
}

QString DescribeOutcome(const Outcome& o)
{
    if (o.status == QStringLiteral("not_executed")) {
        // "Never executes" only when final (an unrecognised reason is not).
        return QStringLiteral("%1: its account sequence was used by another certified request (%2…) in batch #%3.%4")
            .arg(o.final ? QStringLiteral("Never executes") : QStringLiteral("Reported not executed"), o.consumed_by.left(16),
                 QString::number(o.consumed_in_sequence), o.final ? QString{} : QStringLiteral(" Not final."));
    }
    QString text;
    if (o.status == QStringLiteral("applied")) text = QStringLiteral("Executed in certified batch #%1.").arg(o.microblock_sequence);
    else if (o.status == QStringLiteral("rejected")) text = QStringLiteral("Not executed in certified batch #%1: %2").arg(o.microblock_sequence).arg(o.reason_text);
    else text = QStringLiteral("Included in certified batch #%1 but not executed: %2").arg(o.microblock_sequence).arg(o.reason_text);
    if (o.fill) {
        if (o.fill->superseded_in_entry) text += QStringLiteral(" Replaced before clearing in the same batch; no fill.");
        else if (o.fill->quantity == 0) text += QStringLiteral(" No fill in that batch; the order rests on the book.");
        else text += QStringLiteral(" Filled %1 at %2 (quote %3, fee %4). Any remainder rests on the book.")
            .arg(o.fill->quantity).arg(o.fill->price).arg(o.fill->quote_atoms).arg(o.fill->fee_atoms);
    }
    if (o.withdrawal_amount) text += QStringLiteral(" Withdrawal of %1 requested; a certified checkpoint and a vault transaction must follow.").arg(*o.withdrawal_amount);
    if (o.deposit_amount) text += QStringLiteral(" Deposit of %1 credited.").arg(*o.deposit_amount);
    if (!o.final) text += QStringLiteral(" Not final.");
    return text;
}

QString DescribeReceipt(const Receipt& r)
{
    QString text;
    if (r.NotExecuted()) text = DescribeOutcome(*r.outcome);
    else if (r.Included()) text = QStringLiteral("Exact action inclusion verified in certified microblock #%1. %2")
        .arg(r.microblock_sequence).arg(r.outcome ? DescribeOutcome(*r.outcome) : QStringLiteral("Execution outcome is unknown: inclusion alone does not prove a fill, successful withdrawal or B3 payout."));
    else if (r.no_resubmit) text = QStringLiteral("Previously certified; fresh proof currently unavailable. The retained verified record prevents resubmission; this is not a never-established submission outcome.");
    else if (r.state == QStringLiteral("admitted")) text = QStringLiteral("Endpoint reports pool admission; not certified inclusion or execution.");
    else if (r.state == QStringLiteral("queued")) text = QStringLiteral("Request queued; pool admission, certified inclusion and execution are not yet proven.");
    else if (r.state == QStringLiteral("rejected")) text = QStringLiteral("Endpoint reports rejection; this does not prove that another endpoint did not already accept the exact action.");
    else text = QStringLiteral("Submission outcome has never been established by verified inclusion. Do not create or re-sign a replacement request to retry it.");
    if (r.no_resubmit) text += QStringLiteral("\nPreviously certified: retained no-resubmit protection is active. A fresh verification label requires current evidence; this instruction will not be resent.");
    text += QStringLiteral("\nAction ID: %1").arg(r.action_id);
    if (!r.endpoint.isEmpty()) text += QStringLiteral("\nEndpoint: %1").arg(r.endpoint);
    if (!r.reason.isEmpty()) text += QStringLiteral("\n%1").arg(r.reason);
    return text;
}


StageView DescribeStage(const Receipt& r, bool local, RequestKind kind, std::optional<int> decimals, const QString& ticker)
{
    StageView v;
    if (r.MayStillExecute() && (r.NotExecuted() || r.Included())) {
        // Verified, but not final: the exact bytes may still execute (or a
        // deposit still be credited) later. Never "not executed", never settled.
        const auto& o{*r.outcome};
        const QString reason{o.reason_text.isEmpty() ? o.reason_code : o.reason_text};
        v.reached = r.Included() ? Stage::OutcomeVerified : Stage::Signed; v.flag = StageFlag::NotFinal;
        v.status = r.Included() ? QStringLiteral("Included in certified batch #%1, not executed yet · not final, may still execute (%2)").arg(QString::number(o.microblock_sequence), reason)
                                : QStringLiteral("Not executed yet · not final, may still execute (%1)").arg(reason);
        v.outcome = QStringLiteral("not executed yet · may still execute");
        return v;
    }
    if (r.NotExecuted()) {
        // Proven and final: a different certified action consumed its sequence.
        v.reached = Stage::Signed; v.flag = StageFlag::NotExecuted; v.terminal = r.Terminal(); v.settled = true;
        v.status = QStringLiteral("Not executed (its account sequence was used by another certified request)");
        v.outcome = QStringLiteral("✕ not executed");
        return v;
    }
    if (r.Included() && r.outcome_verified && r.outcome) {
        const auto& o{*r.outcome};
        v.reached = Stage::OutcomeVerified; v.terminal = r.Terminal(); v.settled = true;
        if (o.status == QStringLiteral("applied")) {
            v.status = QStringLiteral("Executed in certified batch #%1").arg(o.microblock_sequence);
            if (kind == RequestKind::Cancel) v.outcome = QStringLiteral("✓ cancelled");
            else if (kind == RequestKind::Order && o.fill) {
                if (o.fill->superseded_in_entry) v.outcome = QStringLiteral("✓ replaced before clearing · no fill");
                else if (o.fill->quantity == 0) v.outcome = QStringLiteral("✓ no fill yet · resting");
                else v.outcome = QStringLiteral("✓ filled %1").arg(BaseAmount(o.fill->quantity, decimals, ticker));
            } else v.outcome = QStringLiteral("✓ applied");
        } else {
            // Final here: a non-final one is shown above as "may still execute".
            v.flag = StageFlag::NotExecuted;
            v.status = (o.status == QStringLiteral("rejected") ? QStringLiteral("Not executed (%1)") : QStringLiteral("Included but not executed (%1)"))
                           .arg(o.reason_text.isEmpty() ? o.reason_code : o.reason_text);
            v.outcome = QStringLiteral("✕ not executed");
        }
        // Only an applied outcome with an unrecognised reason reaches this.
        if (!o.final) v.status += QStringLiteral(" · not final");
        return v;
    }
    if (r.Included()) {
        v.reached = Stage::Certified; v.settled = true;
        v.status = QStringLiteral("Included in certified batch #%1 · applied or rejected is not reported").arg(r.microblock_sequence);
        v.outcome = QStringLiteral("not reported");
        return v;
    }
    if (r.no_resubmit) {
        v.reached = Stage::Certified; v.flag = StageFlag::CertifiedEarlier; v.settled = true;
        v.status = QStringLiteral("Certified earlier · fresh proof pending · never resent");
        v.outcome = QStringLiteral("not reported");
        return v;
    }
    // Before verified inclusion the receipt state decides; the backend stage
    // only tells a retained, never-sent instruction from a possibly sent one.
    if (r.state == QStringLiteral("admitted")) {
        v.reached = Stage::Admitted;
        v.status = local ? QStringLiteral("Admitted by your node") : QStringLiteral("Endpoint reports admission (unverified)");
    } else if (r.state == QStringLiteral("queued")) {
        v.reached = Stage::Sent;
        v.status = local ? QStringLiteral("Queued by your node") : QStringLiteral("Endpoint queued it (reported)");
    } else if (r.state == QStringLiteral("rejected")) {
        v.reached = Stage::Signed; v.flag = StageFlag::Refused;
        v.status = QStringLiteral("Refusal reported · treated as unknown until reviewed");
    } else if (r.stage == QStringLiteral("signed")) {
        v.reached = Stage::Signed;
        v.status = QStringLiteral("Signed and saved · not sent yet");
    } else {
        v.reached = r.stage == QStringLiteral("sent") ? Stage::Sent : Stage::Signed; v.flag = StageFlag::Uncertain;
        v.status = local ? QStringLiteral("Outcome unknown · never re-signed") : QStringLiteral("Delivery unconfirmed · outcome unknown · never re-signed");
    }
    return v;
}

QString StageName(Stage stage)
{
    switch (stage) {
    case Stage::Signing: return QStringLiteral("Signing");
    case Stage::Signed: return QStringLiteral("Signed");
    case Stage::Sent: return QStringLiteral("Sent");
    case Stage::Admitted: return QStringLiteral("Admitted");
    case Stage::Certified: return QStringLiteral("Certified");
    case Stage::OutcomeVerified: return QStringLiteral("Outcome verified");
    }
    return {};
}

RequestKind KindOf(Operation operation)
{
    return operation == Operation::Order ? RequestKind::Order : operation == Operation::Cancel ? RequestKind::Cancel : RequestKind::Other;
}

RequestKind KindOf(const SavedAction& action)
{
    return action.type <= 1 ? RequestKind::Order : action.type <= 3 ? RequestKind::Cancel : RequestKind::Other;
}

QString ShortTitle(const Action& a)
{
    const bool bid{a.side == QStringLiteral("bid")};
    const QString native{QString::fromStdString(FormatMoney(a.amount)) + QStringLiteral(" B3")};
    switch (a.operation) {
    case Operation::Order:
        if (!a.ladder.empty()) return QStringLiteral("%1 ladder · %2 prices · %3").arg(SideTitle(bid, a.inverse_display)).arg(a.ladder.size()).arg(BaseAmount(a.amount, a.display_decimals, a.display_ticker));
        return OrderTitle(bid, a.price, a.amount, a.display_decimals, a.display_ticker, a.inverse_display);
    case Operation::Cancel: return QStringLiteral("Cancel %1").arg(SideTitle(bid, a.inverse_display));
    case Operation::Withdraw: return QStringLiteral("Withdraw %1").arg(a.native ? native : BaseAmount(a.amount, a.display_decimals, a.display_ticker));
    case Operation::Deposit: return QStringLiteral("Deposit %1").arg(a.native ? native : BaseAmount(a.amount, a.display_decimals, a.display_ticker));
    case Operation::Admit: return QStringLiteral("Admit deposit %1…:%2").arg(a.txid.left(12)).arg(a.vout);
    case Operation::Checkpoint: return QStringLiteral("Publish checkpoint");
    case Operation::Vault: return QStringLiteral("Publish vault transaction");
    }
    return {};
}

QString ShortTitle(const SavedAction& a, std::optional<int> decimals, const QString& ticker, bool inverse)
{
    const bool bid{a.type == 0 || a.type == 2};
    if (a.type == 2 || a.type == 3) return QStringLiteral("Cancel %1").arg(SideTitle(bid, inverse));
    if (a.type <= 1) {
        // The same exact limit shapes DescribeSavedAction recognises; any
        // other curve is named, never re-priced.
        const auto& p{a.canonical_points};
        if (p.size() == 2 && p[0].price < MAX_MONEY && p[1].price == p[0].price + 1 &&
            (bid ? p[0].quantity > 0 && p[1].quantity == 0 : p[0].quantity == 0 && p[1].quantity > 0)) {
            return OrderTitle(bid, bid ? p[0].price : p[1].price, bid ? p[0].quantity : p[1].quantity, decimals, ticker, inverse);
        }
        std::vector<B3FlowMeshMarketData::Point> points;
        for (const auto& point : p) points.push_back({point.price, point.quantity});
        if (const auto steps{B3FlowMeshMarketData::LadderStepsOf(bid ? QStringLiteral("bid") : QStringLiteral("ask"), points)}; steps && steps->size() >= 2) {
            return QStringLiteral("%1 ladder · %2 prices%3").arg(SideTitle(bid, inverse)).arg(steps->size()).arg(a.sequence ? QStringLiteral(" · sequence %1").arg(*a.sequence) : QString{});
        }
        return QStringLiteral("%1 order%2").arg(SideTitle(bid, inverse), a.sequence ? QStringLiteral(" · sequence %1").arg(*a.sequence) : QString{});
    }
    if (a.type == 4) return QStringLiteral("Withdrawal request");
    if (a.type == 5) return QStringLiteral("Deposit admission");
    return QStringLiteral("Request (type %1)").arg(a.type);
}

SavedActions ParseSavedActions(const UniValue& value)
{
    if (Text(Field(value, "source")) != QStringLiteral("local-retained-outbox")) Fail("Unexpected saved-action source.");
    const auto& rows{Field(value, "actions")};
    if (!rows.isArray() || rows.size() > 512) Fail("Saved-action list exceeds its local bound.");
    SavedActions out;
    if (!Field(value, "account_id").isNull()) out.account = Id(Field(value, "account_id"));
    if (out.account.isEmpty() && rows.size()) Fail("Saved instructions lack a wallet account.");
    for (const auto& row : rows.getValues()) {
        SavedAction action;
        action.market = Id(Field(row, "market_id")); action.domain = Id(Field(row, "domain"));
        action.config = Id(Field(row, "execution_config_id")); action.account = Id(Field(row, "account_id"));
        if (action.account != out.account) Fail("Saved instruction belongs to another wallet account.");
        const QString id{Id(Field(row, "action_id"))};
        if (std::any_of(out.actions.begin(), out.actions.end(), [&](const auto& other) { return other.market == action.market && other.receipt.action_id == id; })) Fail("Duplicate saved instruction identity.");
        action.receipt = ParseReceipt(Field(row, "receipt"), action.market, id);
        if (Id(Field(Field(row, "receipt"), "account_id")) != action.account) Fail("Saved receipt belongs to another wallet account.");
        action.receipt.no_resubmit = Flag(Field(row, "previously_certified")) || action.receipt.Included();
        const auto type{Integer(Field(row, "action_type"))};
        if (type > 8) Fail("Unsupported saved instruction type.");
        action.type = type;
        const auto& side{Field(row, "canonical_side")};
        const auto& points{Field(row, "canonical_points")};
        if (!side.isNull() || !points.isNull()) {
            action.canonical_side = Text(side);
            const QString expected{type == 0 || type == 2 ? QStringLiteral("bid") : type == 1 || type == 3 ? QStringLiteral("ask") : QString{}};
            if (expected.isEmpty() || action.canonical_side != expected || !points.isArray() || points.size() > flowmesh::MAX_ACTION_CURVE_POINTS ||
                (type >= 2 && points.size())) Fail("Saved canonical presentation does not match its original action type.");
            for (const auto& point : points.getValues()) action.canonical_points.push_back({Money(Field(point, "price"), false), Money(Field(point, "quantity"), false)});
        }
        if (!Field(row, "sequence").isNull()) action.sequence = Integer(Field(row, "sequence"));
        if ((type == 5) == action.sequence.has_value()) Fail("Saved instruction sequence does not match its original type.");
        action.signed_bytes_sha256 = Id(Field(row, "signed_bytes_sha256"));
        action.signed_bytes_size = Integer(Field(row, "signed_bytes_size"));
        if (action.signed_bytes_size == 0 || action.signed_bytes_size > flowmesh::FLOWMESH_ACTION_MAX_BYTES) Fail("Saved instruction size exceeds its codec bound.");
        action.initial_submission_ms = Integer(Field(row, "initial_submission_ms"));
        action.may_have_been_sent = Flag(Field(row, "may_have_been_sent"));
        out.actions.push_back(std::move(action));
    }
    std::sort(out.actions.begin(), out.actions.end(), [](const auto& a, const auto& b) {
        return a.initial_submission_ms != b.initial_submission_ms ? a.initial_submission_ms > b.initial_submission_ms
            : std::pair{a.market, a.receipt.action_id} < std::pair{b.market, b.receipt.action_id};
    });
    return out;
}

QString DescribeSavedAction(const SavedAction& action, std::optional<int> decimals, const QString& ticker)
{
    QString description{QStringLiteral("Market: %1\nAccount: %2\nOriginal action type: %3 · Original account sequence: %4\nOriginal signed payload: %5 bytes · SHA-256: %6\n%7")
        .arg(action.market, action.account).arg(action.type)
        .arg(action.sequence ? QString::number(*action.sequence) : QStringLiteral("not sequenced (deposit)"))
        .arg(action.signed_bytes_size).arg(action.signed_bytes_sha256, DescribeReceipt(action.receipt))};
    if (action.type <= 3) {
        // Action type itself has always fixed the economic direction. Old
        // records need no rewrite, presentation tag, re-signing or fresh send.
        const bool bid{action.type == 0 || action.type == 2};
        const bool cancel{action.type >= 2};
        const QString token{decimals && !ticker.isEmpty() ? ticker : QStringLiteral("configured token")};
        description += QStringLiteral("\n%1%2 B3 = %3 %4; canonical %5. This is the original instruction, not a conversion or a new order.")
            .arg(cancel ? QStringLiteral("Cancel ") : QString{}, bid ? QStringLiteral("Sell") : QStringLiteral("Buy"),
                 bid ? QStringLiteral("buy") : QStringLiteral("sell"), token, bid ? QStringLiteral("bid") : QStringLiteral("ask"));
        const auto& points{action.canonical_points};
        if (!cancel && points.size() == 2 && points[0].price < MAX_MONEY && points[1].price == points[0].price + 1 &&
            (bid ? points[0].quantity > 0 && points[1].quantity == 0 : points[0].quantity == 0 && points[1].quantity > 0)) {
            const CAmount price{bid ? points[0].price : points[1].price}, quantity{bid ? points[0].quantity : points[1].quantity};
            description += QStringLiteral("\nOriginal canonical limit: %1 B3 atoms per token atom; %2 token atoms %3.").arg(price).arg(quantity).arg(bid ? QStringLiteral("to receive, at most") : QStringLiteral("to spend, at most"));
            if (decimals) description += QStringLiteral("\n%1 up to %2 %3; original gross limit %4 %3 / B3. No exact B3 fill is promised.")
                .arg(bid ? QStringLiteral("Receive") : QStringLiteral("Spend"), B3FlowMeshMarketData::FormatAmount(quantity, *decimals), token, B3FlowMeshMarketData::ExactInversePrice(price, *decimals));
            if (const auto notional{B3FlowMeshMarketData::Notional(price, quantity)}) description += (bid ? QStringLiteral("\nOriginal maximum B3 reservation/spend: %1 B3.") : QStringLiteral("\nGross B3 at the limit if fully filled: %1 B3, before the allocated B3 fee.")).arg(B3FlowMeshMarketData::FormatAmount(*notional, 9));
        } else if (!points.empty()) {
            description += QStringLiteral("\nOriginal canonical curve (price: B3 atoms per token atom; quantity: token atoms):");
            for (const auto& point : points) description += QStringLiteral("\n%1 : %2").arg(point.price).arg(point.quantity);
        } else if (!cancel) description += QStringLiteral("\nOriginal price/quantity details unavailable from this backend; no amount or price is guessed.");
        if (!cancel) description += QStringLiteral("\nActual trading fee asset: B3. This receipt does not report your allocated executed fee; no fee amount is inferred from inclusion.");
    }
    return description;
}

Request ReceiptParameters(const QString& market, const QString& action_id, bool retry, int wait_ms)
{
    Hash(market); Hash(action_id);
    Request r{retry ? "retryflowmeshaction" : "getflowmeshactionstatus"};
    r.params.push_back(market.toStdString()); r.params.push_back(action_id.toStdString());
    if (!retry && wait_ms > 0) r.params.push_back(wait_ms);
    return r;
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
