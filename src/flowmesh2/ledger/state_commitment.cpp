// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/ledger/state_commitment.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace flowmesh2::ledger {

namespace {

//! Leaf values of the accounts tree in ascending AccountId order (the map order).
Result<std::vector<uint256>> AccountLeafValues(const LedgerState& ledger)
{
    std::vector<uint256> values;
    values.reserve(ledger.Accounts().size());
    for (const auto& entry : ledger.Accounts()) {
        const Result<AccountLeafV0> leaf{ledger.Leaf(entry.first)};
        if (!leaf.has_value()) return leaf.error();
        values.push_back(leaf->LeafValue());
    }
    return Ok(std::move(values));
}

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

    const Result<std::vector<uint256>> accounts{AccountLeafValues(ledger)};
    if (!accounts.has_value()) return accounts.error();
    roots.accounts = MerkleRoot(*accounts);

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

    std::vector<uint256> nullifiers;
    nullifiers.reserve(ledger.Nullifiers().size());
    for (const Outpoint& outpoint : ledger.Nullifiers()) nullifiers.push_back(NullifierLeafValue(outpoint));
    roots.nullifiers = MerkleRoot(nullifiers);

    std::vector<uint256> totals;
    totals.reserve(ledger.Totals().size());
    for (const auto& [asset, asset_totals] : ledger.Totals()) totals.push_back(TotalsLeafV0{asset, asset_totals}.LeafValue());
    roots.totals = MerkleRoot(totals);

    return Ok(roots);
}

Result<AccountWitnessV0> ProveAccountV0(const LedgerState& ledger, const AccountId& account)
{
    const auto it{ledger.Accounts().find(account)};
    if (it == ledger.Accounts().end()) return Error{ErrorCode::INVALID_INPUT, "unknown account (v0 has no absence proof)"};
    Result<AccountLeafV0> leaf{ledger.Leaf(account)};
    if (!leaf.has_value()) return leaf.error();
    const Result<std::vector<uint256>> values{AccountLeafValues(ledger)};
    if (!values.has_value()) return values.error();
    const auto index{static_cast<uint64_t>(std::distance(ledger.Accounts().begin(), it))};
    Result<MerkleProof> proof{MerkleProve(*values, index)};
    if (!proof.has_value()) return proof.error();
    return Ok(AccountWitnessV0{std::move(leaf).value(), std::move(proof).value()});
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
