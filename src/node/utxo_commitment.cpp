// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/utxo_commitment.h>

#include <coins.h>
#include <hash.h>
#include <serialize.h>
#include <streams.h>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

namespace node {

namespace {
//! Canonical byte equality over the exact coin contents (value, script,
//! height, coinbase/coinstake flags, legacy nTime/nTxOffset).
bool CoinBytesEqual(const Coin& a, const Coin& b)
{
    DataStream sa;
    DataStream sb;
    a.Serialize(sa);
    b.Serialize(sb);
    return sa.size() == sb.size() && std::equal(sa.begin(), sa.end(), sb.begin());
}

void SortByOutpoint(std::vector<UtxoEntry>& entries)
{
    std::sort(entries.begin(), entries.end(),
              [](const UtxoEntry& x, const UtxoEntry& y) { return x.outpoint < y.outpoint; });
}
} // namespace

uint256 UtxoSetCommitment(const std::vector<UtxoEntry>& sorted_entries)
{
    HashWriter h{};
    h << std::string{"b3/utxo-commitment/v1"};
    h << uint64_t{sorted_entries.size()};
    for (const auto& e : sorted_entries) {
        h << e.outpoint;
        e.coin.Serialize(h);
    }
    return h.GetSHA256();
}

std::vector<UtxoEntry> EnumerateUtxos(const CCoinsView& view)
{
    std::vector<UtxoEntry> entries;
    std::unique_ptr<CCoinsViewCursor> cursor{view.Cursor()};
    if (!cursor) return entries; // view does not support enumeration
    for (; cursor->Valid(); cursor->Next()) {
        UtxoEntry e;
        if (cursor->GetKey(e.outpoint) && cursor->GetValue(e.coin)) {
            entries.push_back(std::move(e));
        }
    }
    SortByOutpoint(entries);
    return entries;
}

UtxoComparison CompareUtxoSets(std::vector<UtxoEntry> a, std::vector<UtxoEntry> b)
{
    SortByOutpoint(a);
    SortByOutpoint(b);

    UtxoComparison cmp;
    cmp.commitment_a = UtxoSetCommitment(a);
    cmp.commitment_b = UtxoSetCommitment(b);
    cmp.count_a = a.size();
    cmp.count_b = b.size();

    size_t i{0};
    size_t j{0};
    while (i < a.size() && j < b.size()) {
        if (a[i].outpoint == b[j].outpoint) {
            if (!CoinBytesEqual(a[i].coin, b[j].coin)) {
                cmp.mismatches.push_back({a[i].outpoint, a[i].coin, b[j].coin});
            }
            ++i;
            ++j;
        } else if (a[i].outpoint < b[j].outpoint) {
            cmp.mismatches.push_back({a[i].outpoint, a[i].coin, std::nullopt}); // only in a
            ++i;
        } else {
            cmp.mismatches.push_back({b[j].outpoint, std::nullopt, b[j].coin}); // only in b
            ++j;
        }
    }
    for (; i < a.size(); ++i) cmp.mismatches.push_back({a[i].outpoint, a[i].coin, std::nullopt});
    for (; j < b.size(); ++j) cmp.mismatches.push_back({b[j].outpoint, std::nullopt, b[j].coin});
    return cmp;
}

UtxoComparison CompareUtxoViews(const CCoinsView& a, const CCoinsView& b)
{
    return CompareUtxoSets(EnumerateUtxos(a), EnumerateUtxos(b));
}

} // namespace node
