// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/snapshot_mark.h>

#include <flowmesh2/core/codec.h>
#include <streams.h>

namespace flowmesh2::storage {

std::vector<unsigned char> SnapshotMark::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter{out, 0, slot, header_hash, state_root};
    return out;
}

Result<SnapshotMark> SnapshotMark::Decode(std::span<const unsigned char> bytes)
{
    return DecodeExactWith(bytes, ENCODED_BYTES, [](SpanReader& reader) {
        SnapshotMark mark;
        reader >> mark.slot >> mark.header_hash >> mark.state_root;
        return mark;
    });
}

} // namespace flowmesh2::storage
