// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_SIM_CLUSTER_H
#define B3_FLOWMESH2_SIM_CLUSTER_H

#ifndef FM2_DEV_COMPONENTS
#error "sim/cluster.h uses the REGTEST dev components: only targets that link fm2_dev (FM2_DEV_COMPONENTS) may include it"
#endif

#include <flowmesh2/core/result.h>
#include <flowmesh2/net/in_process_mesh.h>
#include <flowmesh2/sim/faults.h>
#include <flowmesh2/sim/sim_node.h>
#include <flowmesh2/sim/trace.h>
#include <random.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

/** A simulated cluster (slice R14; multi-node since M4-SIM-01/02;
 * ARCHITECTURE.md section 14).
 *
 * Create(n, config, trace): n nodes over one in-process mesh. Node i signs
 * as seat i of the n simulator seats and gets `config` with seat_count n,
 * self_seat i and its mesh end; with n = 1 it is the k = 1 dev certifier's
 * world the walking skeleton runs on (no mesh). The network and clock
 * faults of `config.faults` (M4-SIM-02) are the cluster's: DropRule (a
 * seeded fraction of frames from one seat to another vanish; the seed is
 * faults.fabric_seed), DelayRule (frames from one seat to another arrive
 * delay_ms later), partition (frames between seats of different groups
 * vanish until partition_heal_at_ms; a seat in no group is in its own),
 * SkewRule (that seat's agreement sees now + skew_ms), and crash_before_slot
 * with crash_seat (the driver asks RestartNode when the cluster reaches that
 * slot; the cluster does not do it by itself, so a script decides the
 * moment). Everything is a function of (config, the driver's calls), so a
 * seed replays byte-identically.
 *
 * Pump(now_ms) is one step of simulated time: it drains the fabric, drops
 * or schedules each frame, delivers every frame due by now_ms to its
 * receiver's SimNode::OnFrame (refusals are counted, not fatal: a peer's
 * bad frame is the peer's fault), ticks every node's agreement with its
 * skewed time and completes every decision reached (SimNode::Complete).
 * Frames a step produces wait for the next step, so a step is bounded.
 *
 * CreateTwins is M4-SIM-03 (a stub). Create(0) is INVALID_INPUT (CLUSTER_EMPTY).
 */
namespace flowmesh2::sim {

inline constexpr std::string_view CLUSTER_EMPTY{"a cluster needs at least one node"};       //!< (INVALID_INPUT)
inline constexpr std::string_view CLUSTER_ONE_NODE{"a one-node cluster has no mesh to pump"}; //!< (INVALID_INPUT)
inline constexpr std::string_view CLUSTER_NODE_RANGE{"node index outside the cluster"};     //!< (INVALID_INPUT)
inline constexpr std::string_view CLUSTER_TIME_BACKWARDS{"pump time moved backwards"};       //!< (INVALID_INPUT)

//! What one Pump did.
struct PumpReport {
    size_t delivered{0};
    size_t dropped{0};
    size_t refused{0}; //!< frames a receiver's agreement refused
    std::vector<SealReport> sealed; //!< decisions completed, by node index order
    std::vector<uint32_t> sealed_by;
};

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
    //! The fabric (nullptr for one node).
    net::InProcessFabric* Fabric() { return m_mesh.fabric.get(); }
    const FaultPlan& Faults() const { return m_faults; }
    //! The node that leads the current view, if any says so.
    std::optional<uint32_t> Leader();

    //! One step of simulated time (see the file comment). INVALID_INPUT for one node or time moving backwards.
    Result<PumpReport> Pump(uint64_t now_ms);
    //! Crash and restart node `index` (SimNode::Restart); its queued and in-flight frames are dropped.
    Result<void> RestartNode(uint32_t index);
    //! Heal the partition now (frames cross groups again), whatever partition_heal_at_ms says.
    void HealPartition() { m_healed = true; }
    //! Arm the partition again until `heal_at_ms` (a script's slot under partition).
    void SetPartitionHeal(uint64_t heal_at_ms)
    {
        m_healed = false;
        m_heal_at_ms = heal_at_ms;
    }

private:
    struct Delayed {
        uint64_t deliver_at_ms{0};
        net::InProcessFrame frame;
    };

    SimCluster(std::vector<std::unique_ptr<SimNode>> nodes, net::InProcessMesh mesh, const FaultPlan& faults);

    bool Partitioned(uint32_t from, uint32_t to, uint64_t now_ms) const;
    uint64_t DelayFor(uint32_t from, uint32_t to) const;
    bool Dropped(uint32_t from, uint32_t to);

    std::vector<std::unique_ptr<SimNode>> m_nodes;
    net::InProcessMesh m_mesh;
    const FaultPlan m_faults;
    FastRandomContext m_rng;
    std::vector<Delayed> m_delayed;
    uint64_t m_now_ms{0};
    bool m_healed{false};
    std::optional<uint64_t> m_heal_at_ms; //!< overrides the plan's partition_heal_at_ms when set
};

} // namespace flowmesh2::sim

#endif // B3_FLOWMESH2_SIM_CLUSTER_H
