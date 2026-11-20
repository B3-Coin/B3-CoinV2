// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/sim/cluster.h>

#include <flowmesh2/net/sync.h>

#include <crypto/sha256.h>
#include <flowmesh2/agreement/messages.h>

#include <algorithm>
#include <string>
#include <utility>

namespace flowmesh2::sim {

namespace {

uint256 FabricSeedKey(uint64_t seed)
{
    const std::string label{"fm2/sim/fabric/" + std::to_string(seed)};
    uint256 out;
    CSHA256{}.Write(reinterpret_cast<const unsigned char*>(label.data()), label.size()).Finalize(out.data());
    return out;
}

} // namespace

SimCluster::SimCluster(std::vector<std::unique_ptr<SimNode>> nodes, net::InProcessMesh mesh, const FaultPlan& faults)
    : m_nodes{std::move(nodes)}, m_mesh{std::move(mesh)}, m_faults{faults}, m_rng{FabricSeedKey(faults.fabric_seed)}
{
}

Result<std::unique_ptr<SimCluster>> SimCluster::Create(size_t nodes, const SimNodeConfig& config, TraceRecorder& trace)
{
    if (nodes == 0) return Error{ErrorCode::INVALID_INPUT, CLUSTER_EMPTY};
    if (nodes > MAX_SEATS) return Error{ErrorCode::OVERSIZE, "more nodes than seats"};
    if (const Result<void> plan{CheckFaultPlan(config.faults)}; !plan.has_value()) return plan.error();
    net::InProcessMesh mesh;
    if (nodes > 1) {
        Result<ProfileParams> profile{DevRegtestProfile(ChainType::REGTEST)};
        if (!profile.has_value()) return profile.error();
        const net::MeshConfig prototype{0, static_cast<uint32_t>(nodes), net::MaxBulkFrameBytes(*profile), *profile};
        Result<net::InProcessMesh> created{net::InProcessSeatMesh::Create(prototype, static_cast<uint32_t>(nodes))};
        if (!created.has_value()) return created.error();
        mesh = std::move(created).value();
    }
    std::vector<std::unique_ptr<SimNode>> all;
    for (uint32_t i{0}; i < nodes; ++i) {
        SimNodeConfig own{config};
        own.seat_count = static_cast<uint32_t>(nodes);
        own.self_seat = i;
        own.mesh = nodes > 1 ? mesh.ends[i].get() : nullptr;
        Result<std::unique_ptr<SimNode>> node{SimNode::Create(own, trace)};
        if (!node.has_value()) return node.error();
        all.push_back(std::move(node).value());
    }
    return Ok(std::unique_ptr<SimCluster>{new SimCluster{std::move(all), std::move(mesh), config.faults}});
}

Result<std::unique_ptr<SimCluster>> SimCluster::CreateTwins(size_t, size_t, const SimNodeConfig&, TraceRecorder&)
{
    return NotImplemented("M4-SIM-03"); // TODO(rebirth:M4-SIM-03) Twins and mutation fuzzing
}

std::optional<uint32_t> SimCluster::Leader()
{
    for (uint32_t i{0}; i < m_nodes.size(); ++i) {
        if (m_nodes[i]->IsLeader()) return i;
    }
    return std::nullopt;
}

bool SimCluster::Partitioned(uint32_t from, uint32_t to, uint64_t now_ms) const
{
    if (m_faults.partition.empty() || m_healed) return false;
    const std::optional<uint64_t> heal{m_heal_at_ms.has_value() ? m_heal_at_ms : m_faults.partition_heal_at_ms};
    if (heal.has_value() && now_ms >= *heal) return false;
    const auto group_of = [this](uint32_t seat) -> int {
        for (size_t g{0}; g < m_faults.partition.size(); ++g) {
            if (m_faults.partition[g].contains(seat)) return static_cast<int>(g);
        }
        return -1 - static_cast<int>(seat); // a seat in no group is alone
    };
    return group_of(from) != group_of(to);
}

uint64_t SimCluster::DelayFor(uint32_t from, uint32_t to) const
{
    uint64_t delay{0};
    for (const DelayRule& rule : m_faults.delay) {
        if (rule.from_seat == from && rule.to_seat == to) delay += rule.delay_ms;
    }
    return delay;
}

bool SimCluster::Dropped(uint32_t from, uint32_t to)
{
    for (const DropRule& rule : m_faults.drop) {
        if (rule.from_seat != from || rule.to_seat != to || rule.per_mille == 0) continue;
        if (m_rng.randrange(uint32_t{1000}) < rule.per_mille) return true; // both unsigned: the draw's type follows its bound
    }
    return false;
}

Result<PumpReport> SimCluster::Pump(uint64_t now_ms)
{
    if (m_nodes.size() == 1) return Error{ErrorCode::INVALID_INPUT, CLUSTER_ONE_NODE};
    if (now_ms < m_now_ms) return Error{ErrorCode::INVALID_INPUT, CLUSTER_TIME_BACKWARDS};
    m_now_ms = now_ms;
    PumpReport report;
    // New frames: dropped, or scheduled by the fabric's rules (in queueing order, so the RNG draws are deterministic).
    for (net::InProcessFrame& frame : m_mesh.fabric->Drain()) {
        if (Partitioned(frame.from, frame.to, now_ms) || Dropped(frame.from, frame.to)) {
            ++report.dropped;
            continue;
        }
        m_delayed.push_back(Delayed{now_ms + DelayFor(frame.from, frame.to), std::move(frame)});
    }
    // Due frames, in (time, queueing) order.
    std::stable_sort(m_delayed.begin(), m_delayed.end(), [](const Delayed& a, const Delayed& b) {
        return std::tie(a.deliver_at_ms, a.frame.seq) < std::tie(b.deliver_at_ms, b.frame.seq);
    });
    std::vector<Delayed> later;
    for (Delayed& item : m_delayed) {
        if (item.deliver_at_ms > now_ms) {
            later.push_back(std::move(item));
            continue;
        }
        ++report.delivered;
        const net::InboundFrame inbound{item.frame.from, item.frame.lane, std::move(item.frame.bytes)};
        if (const Result<void> handled{m_nodes[item.frame.to]->OnFrame(inbound)}; !handled.has_value()) {
            ++report.refused;
        }
    }
    m_delayed = std::move(later);
    // Time, with each seat's skew; then every decision reached.
    for (uint32_t i{0}; i < m_nodes.size(); ++i) {
        int64_t skew{0};
        for (const SkewRule& rule : m_faults.skew) {
            if (rule.seat == i) skew += rule.skew_ms;
        }
        const uint64_t local{skew >= 0 ? now_ms + static_cast<uint64_t>(skew)
                                       : (static_cast<uint64_t>(-skew) > now_ms ? 0 : now_ms - static_cast<uint64_t>(-skew))};
        (void)m_nodes[i]->Tick(local); // a fenced or refusing node is visible through its status
    }
    for (uint32_t i{0}; i < m_nodes.size(); ++i) {
        Result<std::optional<SealReport>> completed{m_nodes[i]->Complete()};
        if (completed.has_value() && completed->has_value()) {
            report.sealed.push_back(**completed);
            report.sealed_by.push_back(i);
        }
    }
    return Ok(std::move(report));
}

Result<void> SimCluster::RestartNode(uint32_t index)
{
    if (index >= m_nodes.size()) return Error{ErrorCode::INVALID_INPUT, CLUSTER_NODE_RANGE};
    // What was in flight to or from the node is lost with its process.
    std::vector<Delayed> kept;
    for (Delayed& item : m_delayed) {
        if (item.frame.to != index && item.frame.from != index) kept.push_back(std::move(item));
    }
    m_delayed = std::move(kept);
    if (m_mesh.fabric) {
        while (m_mesh.fabric->Poll(index).has_value()) {
        }
    }
    return m_nodes[index]->Restart();
}

} // namespace flowmesh2::sim
