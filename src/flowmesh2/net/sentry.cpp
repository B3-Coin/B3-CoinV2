// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/sentry.h>

namespace flowmesh2::net {

Result<FanoutReport> SentryFanout::Fanout(std::span<const unsigned char> /*frame*/)
{
    return NotImplemented("M4-NET-03"); // TODO(rebirth:M4-NET-03)
}

} // namespace flowmesh2::net
