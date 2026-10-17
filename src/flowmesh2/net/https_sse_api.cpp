// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/net/https_sse_api.h>

namespace flowmesh2::net {

Result<std::unique_ptr<ClientApi>> HttpsSseApi::Create(const HttpsSseConfig& /*config*/, ClientApi& /*backend*/)
{
    return NotImplemented("M5-API-01"); // TODO(rebirth:M5-API-01)
}

} // namespace flowmesh2::net
