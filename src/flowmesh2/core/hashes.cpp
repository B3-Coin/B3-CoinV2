// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/hashes.h>

#include <util/check.h>

#include <array>
#include <cstddef>
#include <string>

namespace flowmesh2 {
namespace {

constexpr std::array<HashTagInfo, 29> HASH_TAGS{{
    {HashTag::NET_DOMAIN, "DOMAIN", "B3/FM2/DOMAIN"},
    {HashTag::ACCOUNT, "ACCOUNT", "B3/FM2/ACCOUNT"},
    {HashTag::ACTION, "ACTION", "B3/FM2/ACTION"},
    {HashTag::ACTION_LEAF, "ACTION_LEAF", "B3/FM2/ACTION_LEAF"},
    {HashTag::BODY, "BODY", "B3/FM2/BODY"},
    {HashTag::FACT, "FACT", "B3/FM2/FACT"},
    {HashTag::FACTS, "FACTS", "B3/FM2/FACTS"},
    {HashTag::MARKET, "MARKET", "B3/FM2/MARKET"},
    {HashTag::ORDER, "ORDER", "B3/FM2/ORDER"},
    {HashTag::SLOT, "SLOT", "B3/FM2/SLOT"},
    {HashTag::COMMIT, "COMMIT", "B3/FM2/COMMIT"},
    {HashTag::PREPARE, "PREPARE", "B3/FM2/PREPARE"},
    {HashTag::CERT, "CERT", "B3/FM2/CERT"},
    {HashTag::OUTCOME, "OUTCOME", "B3/FM2/OUTCOME"},
    {HashTag::SEATSET, "SEATSET", "B3/FM2/SEATSET"},
    {HashTag::PROFILE, "PROFILE", "B3/FM2/PROFILE"},
    {HashTag::STATE, "STATE", "B3/FM2/STATE"},
    {HashTag::STATE_ACCOUNTS, "STATE_ACCOUNTS", "B3/FM2/STATE_ACCOUNTS"},
    {HashTag::STATE_ORDERS, "STATE_ORDERS", "B3/FM2/STATE_ORDERS"},
    {HashTag::STATE_MARKETS, "STATE_MARKETS", "B3/FM2/STATE_MARKETS"},
    {HashTag::STATE_NULLIFIERS, "STATE_NULLIFIERS", "B3/FM2/STATE_NULLIFIERS"},
    {HashTag::STATE_TOTALS, "STATE_TOTALS", "B3/FM2/STATE_TOTALS"},
    {HashTag::MERKLE_LEAF, "MERKLE_LEAF", "B3/FM2/MERKLE_LEAF"},
    {HashTag::MERKLE_NODE, "MERKLE_NODE", "B3/FM2/MERKLE_NODE"},
    {HashTag::MERKLE_EMPTY, "MERKLE_EMPTY", "B3/FM2/MERKLE_EMPTY"},
    {HashTag::MERKLE_ROOT, "MERKLE_ROOT", "B3/FM2/MERKLE_ROOT"},
    {HashTag::PROTOCOL_ACCOUNT, "PROTOCOL_ACCOUNT", "B3/FM2/PROTOCOL_ACCOUNT"},
    {HashTag::JOURNAL, "JOURNAL", "B3/FM2/JOURNAL"},
    {HashTag::TEST_ASSET, "TEST_ASSET", "B3/FM2/TEST_ASSET"},
}};

// Append-only shape: contiguous from 1, unique names, text = prefix + name.
constexpr bool RegistryIsWellFormed()
{
    for (size_t i{0}; i < HASH_TAGS.size(); ++i) {
        const auto& info{HASH_TAGS[i]};
        if (static_cast<size_t>(info.tag) != i + 1) return false;
        if (info.name.empty()) return false;
        if (info.text.size() != HASH_TAG_PREFIX.size() + info.name.size()) return false;
        if (!info.text.starts_with(HASH_TAG_PREFIX) || !info.text.ends_with(info.name)) return false;
        for (size_t j{0}; j < i; ++j) {
            if (HASH_TAGS[j].name == info.name) return false;
        }
    }
    return true;
}
static_assert(RegistryIsWellFormed(), "HashTag registry must be contiguous from 1 with unique prefixed names");

size_t IndexOf(HashTag tag)
{
    const size_t index{static_cast<size_t>(tag) - 1};
    Assert(static_cast<size_t>(tag) >= 1 && index < HASH_TAGS.size());
    return index;
}

//! One primed writer per tag, built once (thread-safe static initialisation).
const std::array<HashWriter, HASH_TAGS.size()>& PrimedWriters()
{
    static const std::array<HashWriter, HASH_TAGS.size()> writers{[] {
        std::array<HashWriter, HASH_TAGS.size()> out;
        for (size_t i{0}; i < HASH_TAGS.size(); ++i) {
            out[i] = TaggedHash(std::string{HASH_TAGS[i].text});
        }
        return out;
    }()};
    return writers;
}

} // namespace

std::span<const HashTagInfo> HashTagRegistry() { return HASH_TAGS; }

bool IsKnown(HashTag tag)
{
    const auto value{static_cast<size_t>(tag)};
    return value >= 1 && value <= HASH_TAGS.size();
}

std::string_view TagText(HashTag tag)
{
    if (!IsKnown(tag)) return std::string_view{};
    return HASH_TAGS[static_cast<size_t>(tag) - 1].text;
}

HashWriter TaggedWriter(HashTag tag) { return PrimedWriters()[IndexOf(tag)]; }

uint256 Tagged(HashTag tag, std::span<const unsigned char> bytes)
{
    HashWriter writer{TaggedWriter(tag)};
    writer.write(std::as_bytes(bytes));
    return writer.GetSHA256();
}

} // namespace flowmesh2
