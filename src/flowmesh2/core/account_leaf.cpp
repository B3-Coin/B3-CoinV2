// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/account_leaf.h>

#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/hashes.h>
#include <serialize.h>
#include <streams.h>

#include <algorithm>
#include <ios>

namespace flowmesh2 {

namespace {

const AssetBalance* FindBalance(const std::vector<AssetBalance>& balances, const AssetId& asset)
{
    const auto it{std::lower_bound(balances.begin(), balances.end(), asset,
                                   [](const AssetBalance& b, const AssetId& a) { return b.asset < a; })};
    return it != balances.end() && it->asset == asset ? &*it : nullptr;
}

} // namespace

size_t AccountLeafV0::MaxEncodedSize(const ProfileParams& profile)
{
    const size_t nonces{profile.nonce_window};
    return 32 + GetSizeOfCompactSize(MAX_LEAF_ASSETS) + MAX_LEAF_ASSETS * AssetBalance::ENCODED_BYTES +
           GetSizeOfCompactSize(nonces) + nonces * 8 + 4;
}

std::vector<unsigned char> AccountLeafV0::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer << account;
    WriteCompactSize(writer, balances.size());
    for (const auto& balance : balances) writer << balance;
    WriteCompactSize(writer, used_nonces.size());
    for (const uint64_t nonce : used_nonces) writer << nonce;
    writer << open_orders;
    return out;
}

bool AccountLeafV0::IsCanonical(const ProfileParams& profile) const
{
    if (balances.size() > MAX_LEAF_ASSETS || used_nonces.size() > profile.nonce_window) return false;
    for (size_t i{0}; i < balances.size(); ++i) {
        const AssetBalance& b{balances[i]};
        if (!InAtomRange(b.available) || !InAtomRange(b.reserved)) return false;
        if (b.available == 0 && b.reserved == 0) return false;
        if (i > 0 && !(balances[i - 1].asset < b.asset)) return false;
    }
    for (size_t i{1}; i < used_nonces.size(); ++i) {
        if (used_nonces[i - 1] >= used_nonces[i]) return false;
    }
    return open_orders <= profile.max_open_orders_per_account;
}

Result<AccountLeafV0> AccountLeafV0::Decode(std::span<const unsigned char> bytes, const ProfileParams& profile)
{
    return DecodeExactWith(bytes, MaxEncodedSize(profile), [&profile](SpanReader& reader) {
        AccountLeafV0 leaf;
        reader >> leaf.account;
        ReadCappedVector(reader, leaf.balances, MAX_LEAF_ASSETS);
        ReadCappedVector(reader, leaf.used_nonces, profile.nonce_window);
        reader >> leaf.open_orders;
        if (!leaf.IsCanonical(profile)) throw std::ios_base::failure("AccountLeafV0 is not canonical");
        return leaf;
    });
}

uint256 AccountLeafV0::LeafValue() const { return Tagged(HashTag::STATE_ACCOUNTS, Encode()); }

Atoms AccountLeafV0::Available(const AssetId& asset) const
{
    const AssetBalance* b{FindBalance(balances, asset)};
    return b ? b->available : 0;
}

Atoms AccountLeafV0::Reserved(const AssetId& asset) const
{
    const AssetBalance* b{FindBalance(balances, asset)};
    return b ? b->reserved : 0;
}

bool AccountLeafV0::NonceUsed(uint64_t nonce) const
{
    return std::binary_search(used_nonces.begin(), used_nonces.end(), nonce);
}

} // namespace flowmesh2
