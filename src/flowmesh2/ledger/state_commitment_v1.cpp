// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/ledger/state_commitment_v1.h>

#include <flowmesh2/core/smt.h>
#include <serialize.h>
#include <streams.h>

#include <algorithm>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace flowmesh2::ledger {

namespace {

uint256 PathOf(SmtComponent component, const uint256& key)
{
    return SmtKeyPath(component, std::span<const unsigned char>{key.data(), 32});
}

//! The tree of `leaves` in any order; INVALID_INPUT (`repeated`) if two share a path.
Result<SmtTree> TreeOf(std::vector<SmtLeaf> leaves, const char* repeated)
{
    std::sort(leaves.begin(), leaves.end(), [](const SmtLeaf& a, const SmtLeaf& b) { return SmtPathLess(a.path, b.path); });
    const auto dup{std::adjacent_find(leaves.begin(), leaves.end(), [](const SmtLeaf& a, const SmtLeaf& b) { return a.path == b.path; })};
    if (dup != leaves.end()) return Error{ErrorCode::INVALID_INPUT, repeated};
    return SmtTree::Build(leaves);
}

} // namespace

Result<SmtTree> AccountsTreeV1(const LedgerState& ledger)
{
    std::vector<SmtLeaf> leaves;
    leaves.reserve(ledger.Accounts().size());
    for (const auto& entry : ledger.Accounts()) {
        const Result<AccountLeafV0> leaf{ledger.Leaf(entry.first)};
        if (!leaf.has_value()) return Error{ErrorCode::INVARIANT, leaf.error().detail};
        leaves.push_back(SmtLeaf{AccountPathV1(entry.first), leaf->LeafValue()});
    }
    // Distinct AccountIds give distinct paths unless SHA-256 collides.
    return TreeOf(std::move(leaves), "two accounts share a v1 path");
}

Result<void> UpdateAccountsTreeV1(SmtTree& tree, const LedgerState& before, const LedgerState& after)
{
    SmtTree next{tree};
    std::optional<Error> broken;
    after.ForEachChangedAccount(before, [&](const AccountId& account, const AccountState*, const AccountState* now) {
        if (broken.has_value()) return;
        if (now == nullptr) {
            next.Erase(AccountPathV1(account));
            return;
        }
        const Result<AccountLeafV0> leaf{after.Leaf(account)};
        if (!leaf.has_value()) {
            broken = Error{ErrorCode::INVARIANT, leaf.error().detail};
            return;
        }
        next.Set(AccountPathV1(account), leaf->LeafValue());
    });
    if (broken.has_value()) return *broken;
    tree = std::move(next);
    return Ok();
}

uint256 NullifierPathV1(const Outpoint& outpoint)
{
    std::vector<unsigned char> key;
    VectorWriter writer{key, 0};
    writer << outpoint;
    return SmtKeyPath(SmtComponent::NULLIFIERS, key);
}

SmtTree NullifiersTreeV1(const LedgerState& ledger)
{
    std::vector<SmtLeaf> leaves;
    leaves.reserve(ledger.Nullifiers().size());
    for (const Outpoint& outpoint : ledger.Nullifiers()) leaves.push_back(SmtLeaf{NullifierPathV1(outpoint), NullifierLeafValue(outpoint)});
    // A set of distinct outpoints: distinct paths unless SHA-256 collides.
    return std::move(TreeOf(std::move(leaves), "two nullifiers share a v1 path")).value();
}

Result<StateRootsV1> CommitV1(const LedgerState& ledger, const SmtTree& accounts, std::span<const MarketDescriptor> markets,
                              std::span<const OrderLeafV0> orders)
{
    StateRootsV1 roots;
    roots.accounts = accounts.Root();

    std::vector<SmtLeaf> order_leaves;
    order_leaves.reserve(orders.size());
    for (const OrderLeafV0& order : orders) order_leaves.push_back(SmtLeaf{PathOf(SmtComponent::ORDERS, order.order.id), order.value});
    const Result<SmtTree> orders_tree{TreeOf(std::move(order_leaves), "repeated OrderId in the orders component")};
    if (!orders_tree.has_value()) return orders_tree.error();
    roots.orders = orders_tree->Root();

    std::vector<SmtLeaf> market_leaves;
    market_leaves.reserve(markets.size());
    for (const MarketDescriptor& market : markets) {
        const Result<void> valid{ValidateMarket(market)};
        if (!valid.has_value()) return valid.error();
        market_leaves.push_back(SmtLeaf{PathOf(SmtComponent::MARKETS, market.Id().id), MarketLeafValue(market)});
    }
    const Result<SmtTree> markets_tree{TreeOf(std::move(market_leaves), "repeated MarketId in the markets component")};
    if (!markets_tree.has_value()) return markets_tree.error();
    roots.markets = markets_tree->Root();

    roots.nullifiers = NullifiersTreeV1(ledger).Root();

    std::vector<SmtLeaf> totals;
    totals.reserve(ledger.Totals().size());
    for (const auto& [asset, asset_totals] : ledger.Totals()) {
        totals.push_back(SmtLeaf{PathOf(SmtComponent::TOTALS, asset.id), TotalsLeafV0{asset, asset_totals}.LeafValue()});
    }
    roots.totals = std::move(TreeOf(std::move(totals), "two assets share a v1 path")).value().Root();

    if (!ledger.Profile().withdrawals_enabled && !ledger.Withdrawals().empty()) {
        return Error{ErrorCode::INVARIANT, "withdrawals in a legacy profile"};
    }
    std::vector<SmtLeaf> withdrawals;
    withdrawals.reserve(ledger.Withdrawals().size());
    for (const auto& [id, record] : ledger.Withdrawals()) {
        if (id != record.id || !record.IsCanonical()) return Error{ErrorCode::INVARIANT, "noncanonical withdrawal leaf"};
        withdrawals.push_back(SmtLeaf{PathOf(SmtComponent::WITHDRAWALS, id.id), record.LeafValue()});
    }
    roots.withdrawals = std::move(TreeOf(std::move(withdrawals), "two withdrawals share a v1 path")).value().Root();
    return Ok(roots);
}

Result<StateRootsV1> CommitTradingStateV1(const LedgerState& ledger, const SmtTree& accounts,
                                          std::span<const MarketDescriptor> markets,
                                          std::span<const RestingOrderLeafV0> orders)
{
    std::set<MarketId> listed;
    for (const MarketDescriptor& market : markets) listed.insert(market.Id());
    std::vector<OrderLeafV0> leaves;
    leaves.reserve(orders.size());
    for (const RestingOrderLeafV0& order : orders) {
        if (!order.IsCanonical()) return Error{ErrorCode::INVALID_INPUT, "order leaf is not canonical"};
        if (!listed.contains(order.market)) return Error{ErrorCode::INVALID_INPUT, "order rests in an unlisted market"};
        if (!ledger.HasAccount(order.owner)) return Error{ErrorCode::INVALID_INPUT, "order owner has no account"};
        leaves.push_back(OrderLeafOf(order));
    }
    return CommitV1(ledger, accounts, markets, leaves);
}

Result<AccountProofV1> ProveAccountV1(const LedgerState& ledger, const SmtTree& accounts, const AccountId& account)
{
    AccountProofV1 out;
    out.account = account;
    const uint256 path{AccountPathV1(account)};
    const std::optional<uint256> committed{accounts.Find(path)};
    if (ledger.HasAccount(account)) {
        Result<AccountLeafV0> leaf{ledger.Leaf(account)};
        if (!leaf.has_value()) return Error{ErrorCode::INVARIANT, leaf.error().detail};
        if (committed != leaf->LeafValue()) return Error{ErrorCode::INVARIANT, "the accounts tree is not the ledger's"};
        out.leaf = std::move(leaf).value();
    } else if (committed.has_value()) {
        return Error{ErrorCode::INVARIANT, "the accounts tree is not the ledger's"};
    }
    out.proof = accounts.Prove(path);
    return Ok(std::move(out));
}

} // namespace flowmesh2::ledger
