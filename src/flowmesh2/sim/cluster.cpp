// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/sim/cluster.h>

#include <utility>

namespace flowmesh2::sim {

Result<std::unique_ptr<SimCluster>> SimCluster::Create(size_t nodes, const SimNodeConfig& config, TraceRecorder& trace)
{
    if (nodes == 0) return Error{ErrorCode::INVALID_INPUT, CLUSTER_EMPTY};
    if (nodes > 1) {
        return NotImplemented("M4-SIM-01"); // TODO(rebirth:M4-SIM-01) k > 1 seats, PBFT and an in-process mesh between the nodes
    }
    Result<std::unique_ptr<SimNode>> node{SimNode::Create(config, trace)};
    if (!node.has_value()) return node.error();
    std::vector<std::unique_ptr<SimNode>> all;
    all.push_back(std::move(node).value());
    return Ok(std::unique_ptr<SimCluster>{new SimCluster{std::move(all)}});
}

Result<std::unique_ptr<SimCluster>> SimCluster::CreateTwins(size_t, size_t, const SimNodeConfig&, TraceRecorder&)
{
    return NotImplemented("M4-SIM-03"); // TODO(rebirth:M4-SIM-03) Twins and mutation fuzzing
}

} // namespace flowmesh2::sim
