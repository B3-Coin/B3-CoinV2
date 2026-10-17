// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/short_id.h>

namespace flowmesh2::net {

Result<agreement::ShortId> ShortIdCodec::Encode(const ActionId& /*id*/) const
{
    return NotImplemented("M4-NET-02"); // TODO(rebirth:M4-NET-02)
}

Result<std::vector<ActionId>> ShortIdCodec::Decode(const agreement::ShortId& /*short_id*/, std::span<const ActionId> /*known*/) const
{
    return NotImplemented("M4-NET-02"); // TODO(rebirth:M4-NET-02)
}

} // namespace flowmesh2::net
