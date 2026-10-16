// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/body.h>

#include <flowmesh2/core/action.h>
#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/merkle.h>
#include <serialize.h>
#include <streams.h>

#include <cstdint>
#include <utility>

namespace flowmesh2 {

namespace {

template <class Stream>
void WriteByteStrings(Stream& s, const std::vector<std::vector<unsigned char>>& items)
{
    WriteCompactSize(s, items.size());
    for (const auto& item : items) {
        WriteCompactSize(s, item.size());
        s.write(MakeByteSpan(item));
    }
}

//! Count checked against max_items before anything is allocated; each item
//! checked against max_item_bytes before its bytes are allocated.
template <class Stream>
void ReadByteStrings(Stream& s, std::vector<std::vector<unsigned char>>& out, size_t max_items, size_t max_item_bytes)
{
    const uint64_t count{ReadCompactSize(s, /*range_check=*/false)};
    if (count > max_items) throw CapExceeded{"SlotBody: item count exceeds cap"};
    out.clear();
    out.reserve(static_cast<size_t>(count));
    for (uint64_t i{0}; i < count; ++i) {
        std::vector<unsigned char> item;
        ReadCappedVector(s, item, max_item_bytes);
        out.push_back(std::move(item));
    }
}

size_t MaxListBytes(size_t max_items, size_t max_item_bytes)
{
    return GetSizeOfCompactSize(max_items) + max_items * (GetSizeOfCompactSize(max_item_bytes) + max_item_bytes);
}

std::vector<uint256> Leaves(const std::vector<std::vector<unsigned char>>& items, uint256 (*leaf)(std::span<const unsigned char>))
{
    std::vector<uint256> leaves;
    leaves.reserve(items.size());
    for (const auto& item : items) leaves.push_back(leaf(item));
    return leaves;
}

} // namespace

uint256 ActionLeaf(std::span<const unsigned char> exact_bytes) { return Tagged(HashTag::ACTION_LEAF, exact_bytes); }

uint256 FactLeaf(std::span<const unsigned char> fact_bytes) { return Tagged(HashTag::FACT, fact_bytes); }

std::vector<unsigned char> SlotBody::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    WriteByteStrings(writer, facts);
    WriteByteStrings(writer, originals);
    return out;
}

size_t SlotBody::MaxEncodedSize(const ProfileParams& profile)
{
    return MaxListBytes(MAX_SLOT_FACTS, MAX_FACT_BYTES) +
           MaxListBytes(profile.max_actions_per_slot, ActionV2::MaxEncodedSize(profile));
}

Result<SlotBody> SlotBody::Decode(std::span<const unsigned char> bytes, const ProfileParams& profile)
{
    const size_t max_originals{profile.max_actions_per_slot};
    const size_t max_original_bytes{ActionV2::MaxEncodedSize(profile)};
    return DecodeExactWith(bytes, MaxEncodedSize(profile), [&](SpanReader& reader) {
        SlotBody body;
        ReadByteStrings(reader, body.facts, MAX_SLOT_FACTS, MAX_FACT_BYTES);
        ReadByteStrings(reader, body.originals, max_originals, max_original_bytes);
        return body;
    });
}

uint256 SlotBody::BodyHash() const { return Tagged(HashTag::BODY, Encode()); }

uint256 SlotBody::ActionsRoot() const { return MerkleRoot(Leaves(originals, &ActionLeaf)); }

uint256 SlotBody::FactsRoot() const { return MerkleRoot(Leaves(facts, &FactLeaf)); }

} // namespace flowmesh2
