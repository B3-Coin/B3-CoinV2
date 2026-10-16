// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/outcome.h>

#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/hashes.h>
#include <serialize.h>
#include <streams.h>

#include <ios>

namespace flowmesh2 {

namespace {
//! source + subject + account + nonce + code + remaining_lots.
constexpr size_t OUTCOME_FIXED_BYTES{1 + 32 + 32 + 8 + 2 + 8};
} // namespace

bool IsKnown(OutcomeSource source)
{
    return source == OutcomeSource::FACT || source == OutcomeSource::ACTION || source == OutcomeSource::SLOT;
}

std::string_view ToString(OutcomeSource source)
{
    switch (source) {
    case OutcomeSource::FACT: return "FACT";
    case OutcomeSource::ACTION: return "ACTION";
    case OutcomeSource::SLOT: return "SLOT";
    }
    return "UNKNOWN";
}

size_t OutcomeV2::MaxFills(const ProfileParams& profile) { return profile.max_fills_per_order; }

size_t OutcomeV2::MaxDeltas(const ProfileParams& profile)
{
    return 2 * (static_cast<size_t>(profile.max_fills_per_order) + 1);
}

size_t OutcomeV2::MaxEncodedSize(const ProfileParams& profile)
{
    const size_t fills{MaxFills(profile)};
    const size_t deltas{MaxDeltas(profile)};
    return OUTCOME_FIXED_BYTES + GetSizeOfCompactSize(fills) + fills * Fill::ENCODED_BYTES +
           GetSizeOfCompactSize(deltas) + deltas * BalanceDelta::ENCODED_BYTES;
}

std::vector<unsigned char> OutcomeV2::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer << static_cast<uint8_t>(source) << subject << account << nonce << static_cast<uint16_t>(code)
           << remaining_lots;
    WriteCompactSize(writer, fills.size());
    for (const auto& fill : fills) writer << fill;
    WriteCompactSize(writer, deltas.size());
    for (const auto& delta : deltas) writer << delta;
    return out;
}

Result<OutcomeV2> OutcomeV2::Decode(std::span<const unsigned char> bytes, const ProfileParams& profile)
{
    const size_t max_fills{MaxFills(profile)};
    const size_t max_deltas{MaxDeltas(profile)};
    return DecodeExactWith(bytes, MaxEncodedSize(profile), [max_fills, max_deltas](SpanReader& reader) {
        OutcomeV2 outcome;
        uint8_t source_byte{0};
        uint16_t code_value{0};
        reader >> source_byte >> outcome.subject >> outcome.account >> outcome.nonce >> code_value >>
            outcome.remaining_lots;
        outcome.source = static_cast<OutcomeSource>(source_byte);
        if (!IsKnown(outcome.source)) throw std::ios_base::failure("unknown OutcomeSource");
        outcome.code = static_cast<OutcomeCode>(code_value);
        if (!IsKnown(outcome.code)) throw std::ios_base::failure("unknown OutcomeCode");
        ReadCappedVector(reader, outcome.fills, max_fills);
        ReadCappedVector(reader, outcome.deltas, max_deltas);
        return outcome;
    });
}

uint256 OutcomeV2::LeafHash() const { return Tagged(HashTag::OUTCOME, Encode()); }

} // namespace flowmesh2
