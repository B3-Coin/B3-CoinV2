// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/ledger/state_commitment.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <set>
#include <utility>
#include <vector>

namespace flowmesh2::ledger {

namespace {

//! MerkleRoot of `keyed` values sorted by key; INVALID_INPUT if a key repeats.
template <class Key>
Result<uint256> SortedRoot(std::vector<std::pair<Key, uint256>> keyed, const char* repeated)
{
    std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const auto dup{std::adjacent_find(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first == b.first; })};
    if (dup != keyed.end()) return Error{ErrorCode::INVALID_INPUT, repeated};
    std::vector<uint256> values;
    values.reserve(keyed.size());
    for (const auto& entry : keyed) values.push_back(entry.second);
    return Ok(MerkleRoot(values));
}

} // namespace

Result<StateRootsV0> CommitV0(const LedgerState& ledger, std::span<const MarketDescriptor> markets,
                              std::span<const OrderLeafV0> orders)
{
    StateRootsV0 roots;

    // The ledger keeps both trees' levels (M2-LED-10): same roots as
    // MerkleRoot of every leaf value, from the changes since its last Seal.
    const Result<uint256> accounts{ledger.AccountsRootV0()};
    if (!accounts.has_value()) return accounts.error();
    roots.accounts = *accounts;

    std::vector<std::pair<OrderId, uint256>> order_leaves;
    order_leaves.reserve(orders.size());
    for (const OrderLeafV0& order : orders) order_leaves.emplace_back(order.order, order.value);
    const Result<uint256> orders_root{SortedRoot(std::move(order_leaves), "repeated OrderId in the orders component")};
    if (!orders_root.has_value()) return orders_root.error();
    roots.orders = *orders_root;

    std::vector<std::pair<MarketId, uint256>> market_leaves;
    market_leaves.reserve(markets.size());
    for (const MarketDescriptor& market : markets) {
        const Result<void> valid{ValidateMarket(market)};
        if (!valid.has_value()) return valid.error();
        market_leaves.emplace_back(market.Id(), MarketLeafValue(market));
    }
    const Result<uint256> markets_root{SortedRoot(std::move(market_leaves), "repeated MarketId in the markets component")};
    if (!markets_root.has_value()) return markets_root.error();
    roots.markets = *markets_root;

    roots.nullifiers = ledger.NullifiersRootV0();

    std::vector<uint256> totals;
    totals.reserve(ledger.Totals().size());
    for (const auto& [asset, asset_totals] : ledger.Totals()) totals.push_back(TotalsLeafV0{asset, asset_totals}.LeafValue());
    roots.totals = MerkleRoot(totals);

    if (ledger.Profile().withdrawals_enabled) {
        roots.version = ExpectedCommitmentVersion(ledger.Profile());
        std::vector<uint256> withdrawals;
        withdrawals.reserve(ledger.Withdrawals().size());
        for (const auto& [id, record] : ledger.Withdrawals()) {
            if (id != record.id || !record.IsCanonical()) return Error{ErrorCode::INVARIANT, "noncanonical withdrawal leaf"};
            withdrawals.push_back(record.LeafValue());
        }
        roots.withdrawals = MerkleRoot(withdrawals);
    } else if (!ledger.Withdrawals().empty()) {
        return Error{ErrorCode::INVARIANT, "withdrawals in a legacy profile"};
    }
    return Ok(roots);
}

OrderLeafV0 OrderLeafOf(const RestingOrderLeafV0& leaf) { return OrderLeafV0{leaf.order, leaf.LeafValue()}; }

Result<StateRootsV0> CommitTradingStateV0(const LedgerState& ledger, std::span<const MarketDescriptor> markets,
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
    return CommitV0(ledger, markets, leaves);
}

Result<AccountWitnessV0> ProveAccountV0(const LedgerState& ledger, const AccountId& account)
{
    if (!ledger.HasAccount(account)) return Error{ErrorCode::INVALID_INPUT, "unknown account (v0 has no absence proof)"};
    Result<AccountLeafV0> leaf{ledger.Leaf(account)};
    if (!leaf.has_value()) return leaf.error();
    Result<MerkleProof> proof{ledger.ProveAccountLeafV0(account)};
    if (!proof.has_value()) return proof.error();
    return Ok(AccountWitnessV0{std::move(leaf).value(), std::move(proof).value()});
}

Result<WithdrawalWitness> ProveWithdrawal(const LedgerState& ledger, const ActionId& id)
{
    if (!ledger.Profile().withdrawals_enabled) return Error{ErrorCode::INVALID_INPUT, "withdrawal proofs disabled"};
    const auto found{ledger.Withdrawals().find(id)};
    if (found == ledger.Withdrawals().end()) return Error{ErrorCode::INVALID_INPUT, "unknown withdrawal"};
    std::vector<uint256> leaves;
    leaves.reserve(ledger.Withdrawals().size());
    for (const auto& [key, record] : ledger.Withdrawals()) {
        if (key != record.id || !record.IsCanonical()) return Error{ErrorCode::INVARIANT, "noncanonical withdrawal leaf"};
        leaves.push_back(record.LeafValue());
    }
    const auto index{static_cast<uint64_t>(std::distance(ledger.Withdrawals().begin(), found))};
    auto proof{MerkleProve(leaves, index)};
    if (!proof.has_value()) return proof.error();
    return Ok(WithdrawalWitness{found->second, std::move(proof).value()});
}

Result<uint256> CommitV1(const LedgerState&)
{
    return NotImplemented("M2-LED-02"); // TODO(rebirth:M2-LED-02)
}

Result<uint256> LiabilitySumRoot(const LedgerState&, const AssetId&)
{
    return NotImplemented("M2-LED-03"); // TODO(rebirth:M2-LED-03)
}

Result<Atoms> ExitValue(const LedgerState&, const AccountId&, const AssetId&)
{
    return NotImplemented("M2-LED-04"); // TODO(rebirth:M2-LED-04)
}

} // namespace flowmesh2::ledger
