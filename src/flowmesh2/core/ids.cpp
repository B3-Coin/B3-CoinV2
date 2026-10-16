// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/core/ids.h>

#include <flowmesh2/core/hashes.h>

#include <span>

namespace flowmesh2 {

AccountId AccountId::FromKey(const XOnlyPubKey& key)
{
    return AccountId{Tagged(HashTag::ACCOUNT, std::span<const unsigned char>{key.data(), key.size()})};
}

} // namespace flowmesh2
