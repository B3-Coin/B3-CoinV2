// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_TEST_UTIL_FLOWMESH_H
#define B3COIN_TEST_UTIL_FLOWMESH_H

#include <flowmesh/state.h>
#include <flowmesh/sync.h>

#include <map>
#include <memory>
#include <utility>

//! TEST-ONLY definitions of the bridges forward-declared (as friends)
//! in the production FlowMesh headers. This header lives under
//! src/test/util and is compiled ONLY into test and bench targets: a
//! production build contains no callable API that can fabricate state
//! funding or construct a signing MeshNode outside node::StartValidator.

namespace flowmesh {
namespace test_only {

struct StateFunding {
    static bool Fund(FlowMeshState& state, const AccountId& account, const AssetId& asset,
                     const CAmount amount)
    {
        return state.ledger.Deposit(account, asset, amount);
    }
};

struct SigningBridge {
    static std::unique_ptr<MeshNode> UnsafeMake(
        MeshNode::Config config, FlowMeshState genesis, const uint256& last_hash = {},
        const std::map<uint64_t, uint256>& restored_locks = {},
        const std::map<std::pair<int32_t, uint256>, uint64_t>& restored_anchors = {})
    {
        return detail::SigningNodeFactory::Make(std::move(config), std::move(genesis),
                                                last_hash, restored_locks, restored_anchors);
    }
};

} // namespace test_only
} // namespace flowmesh

#endif // B3COIN_TEST_UTIL_FLOWMESH_H
