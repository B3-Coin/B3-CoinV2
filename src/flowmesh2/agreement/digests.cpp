// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/agreement/digests.h>

#include <flowmesh2/core/hashes.h>
#include <serialize.h>

#include <vector>

namespace flowmesh2::agreement {

uint256 PrepareDigest(uint64_t view, const uint256& header_hash)
{
    HashWriter writer{TaggedWriter(HashTag::PREPARE)};
    writer << view << header_hash;
    return writer.GetSHA256();
}

uint256 ViewChangeDigest(const uint256& domain, uint64_t epoch, uint64_t slot, uint64_t new_view, uint32_t seat,
                         const std::optional<PreparedCert>& prepared)
{
    HashWriter writer{TaggedWriter(HashTag::AGREEMENT)};
    writer << static_cast<uint8_t>(StatementKind::VIEW_CHANGE) << domain << epoch << slot << new_view << seat
           << static_cast<uint8_t>(prepared.has_value() ? 1 : 0);
    if (prepared.has_value()) writer << prepared->view << prepared->cert.Hash();
    return writer.GetSHA256();
}

uint256 ViewChangeDigest(const uint256& domain, const ViewChange& report)
{
    return ViewChangeDigest(domain, report.epoch, report.slot, report.new_view, report.seat, report.prepared);
}

uint256 NewViewDigest(const uint256& domain, uint64_t epoch, uint64_t slot, uint64_t view, uint32_t seat, const uint256& selected,
                      std::span<const uint256> report_digests)
{
    HashWriter writer{TaggedWriter(HashTag::AGREEMENT)};
    writer << static_cast<uint8_t>(StatementKind::NEW_VIEW) << domain << epoch << slot << view << seat << selected;
    WriteCompactSize(writer, report_digests.size());
    for (const uint256& digest : report_digests) writer << digest;
    return writer.GetSHA256();
}

uint256 NewViewDigest(const uint256& domain, const NewView& message)
{
    std::vector<uint256> digests;
    digests.reserve(message.reports.size());
    for (const ViewChange& report : message.reports) digests.push_back(ViewChangeDigest(domain, report));
    return NewViewDigest(domain, message.epoch, message.slot, message.view, message.seat, message.selected, digests);
}

uint256 HighWaterDigest(const uint256& domain, uint32_t subject, uint64_t epoch, uint64_t slot, uint64_t view)
{
    HashWriter writer{TaggedWriter(HashTag::AGREEMENT)};
    writer << static_cast<uint8_t>(StatementKind::HIGH_WATER) << domain << subject << epoch << slot << view;
    return writer.GetSHA256();
}

uint256 LeaderSeed(const uint256& seat_set_root, uint64_t epoch, uint64_t term)
{
    HashWriter writer{TaggedWriter(HashTag::AGREEMENT)};
    writer << static_cast<uint8_t>(StatementKind::LEADER_SEED) << seat_set_root << epoch << term;
    return writer.GetSHA256();
}

} // namespace flowmesh2::agreement
