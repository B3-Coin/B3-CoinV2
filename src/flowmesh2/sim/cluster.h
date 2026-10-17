// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_SIM_CLUSTER_H
#define B3_FLOWMESH2_SIM_CLUSTER_H

#ifndef FM2_DEV_COMPONENTS
#error "sim/cluster.h uses the REGTEST dev components: only targets that link fm2_dev (FM2_DEV_COMPONENTS) may include it"
#endif

#include <flowmesh2/core/result.h>
#include <flowmesh2/sim/sim_node.h>
#include <flowmesh2/sim/trace.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

/** A simulated cluster (slice R14; ARCHITECTURE.md 2.1 sim/ row).
 *
 * Real in the outline: a cluster of ONE node (Create(1, ...)), the k = 1
 * dev certifier's world, which the walking skeleton runs on.
 *
 * Stubs:
 * - Create(n > 1): NotImplemented(M4-SIM-01). Several nodes need a seat set
 *   of k > 1 seats, PBFT (M4-AGR-02) and an in-process seat mesh between
 *   them (InProcessSeatMesh, M4-SIM-01 too); the multi-node DST of N = 4
 *   and 7 runs on it.
 * - CreateTwins(...): NotImplemented(M4-SIM-03). Twins (two nodes sharing a
 *   seat key, to provoke equivocation) and mutation fuzzing.
 * Create(0) is INVALID_INPUT (CLUSTER_EMPTY). Neither stub builds anything
 * before it answers.
 */
namespace flowmesh2::sim {

inline constexpr std::string_view CLUSTER_EMPTY{"a cluster needs at least one node"}; //!< (INVALID_INPUT)

class SimCluster
{
public:
    static Result<std::unique_ptr<SimCluster>> Create(size_t nodes, const SimNodeConfig& config, TraceRecorder& trace);
    //! Twins and mutation fuzzing. Stub: NotImplemented(M4-SIM-03).
    static Result<std::unique_ptr<SimCluster>> CreateTwins(size_t nodes, size_t twin_pairs, const SimNodeConfig& config,
                                                           TraceRecorder& trace);

    SimCluster(const SimCluster&) = delete;
    SimCluster& operator=(const SimCluster&) = delete;

    size_t Size() const { return m_nodes.size(); }
    //! Node `index` (index < Size()).
    SimNode& Node(size_t index) { return *m_nodes.at(index); }

private:
    explicit SimCluster(std::vector<std::unique_ptr<SimNode>> nodes) : m_nodes{std::move(nodes)} {}

    std::vector<std::unique_ptr<SimNode>> m_nodes;
};

} // namespace flowmesh2::sim

#endif // B3_FLOWMESH2_SIM_CLUSTER_H
