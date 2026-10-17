// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/sim/trace.h>

#include <crypto/sha256.h>

#include <string>
#include <utility>

namespace flowmesh2::sim {

std::string TraceEvent::Line() const
{
    std::string line{std::to_string(slot)};
    line += '|';
    line += kind;
    line += '|';
    line += detail;
    return line;
}

std::string TraceEvent::Step() const
{
    std::string step{kind};
    if (record.has_value()) {
        step += ':';
        step += storage::ToString(*record);
    }
    if (kind.ends_with(".barrier.end")) step += ok ? ":ok" : ":fail";
    return step;
}

std::string_view RolePrefix(storage::JournalRole role)
{
    switch (role) {
    case storage::JournalRole::SIGNER:
        return "signer";
    case storage::JournalRole::COMMIT:
        return "commit";
    }
    return "journal";
}

void TraceRecorder::Push(TraceEvent event)
{
    LOCK(m_mutex);
    event.slot = m_slot;
    m_events.push_back(std::move(event));
}

void TraceRecorder::OnAppend(storage::JournalRole role, uint64_t instance_id, storage::RecordType type, uint64_t seq)
{
    TraceEvent event;
    event.kind = std::string{RolePrefix(role)} + ".append";
    event.detail = std::string{storage::ToString(type)} + ",instance=" + std::to_string(instance_id) + ",seq=" + std::to_string(seq);
    event.record = type;
    event.seq = seq;
    Push(std::move(event));
}

void TraceRecorder::OnBarrierStart(storage::JournalRole role, uint64_t instance_id, uint64_t target_seq)
{
    {
        LOCK(m_mutex);
        ++m_barriers[role];
    }
    TraceEvent event;
    event.kind = std::string{RolePrefix(role)} + ".barrier.start";
    event.detail = "instance=" + std::to_string(instance_id) + ",target=" + std::to_string(target_seq);
    event.seq = target_seq;
    Push(std::move(event));
}

void TraceRecorder::OnBarrierEnd(storage::JournalRole role, uint64_t instance_id, uint64_t target_seq, bool ok)
{
    TraceEvent event;
    event.kind = std::string{RolePrefix(role)} + ".barrier.end";
    event.detail = "instance=" + std::to_string(instance_id) + ",target=" + std::to_string(target_seq) + (ok ? ",ok" : ",fail");
    event.seq = target_seq;
    event.ok = ok;
    Push(std::move(event));
}

void TraceRecorder::OnSigned(uint64_t slot, uint64_t view, const uint256& digest)
{
    TraceEvent event;
    event.kind = "bls.sign";
    event.detail = "slot=" + std::to_string(slot) + ",view=" + std::to_string(view) + ",digest=" + digest.GetHex();
    Push(std::move(event));
}

void TraceRecorder::Authority(const AuthorityVersion& authority)
{
    TraceEvent event;
    event.kind = "authority.check";
    event.detail = "epoch=" + std::to_string(authority.epoch) + ",seats=" + authority.seat_set_root.GetHex();
    Push(std::move(event));
}

void TraceRecorder::BeginSlot(uint64_t slot)
{
    {
        LOCK(m_mutex);
        m_slot = slot;
    }
    Record("slot.begin", "slot=" + std::to_string(slot));
}

void TraceRecorder::EndSlot(uint64_t slot, std::string_view result)
{
    Record("slot.end", "slot=" + std::to_string(slot) + "," + std::string{result});
    LOCK(m_mutex);
    m_slot = 0;
}

void TraceRecorder::Record(std::string_view kind, std::string detail)
{
    TraceEvent event;
    event.kind = std::string{kind};
    event.detail = std::move(detail);
    Push(std::move(event));
}

std::vector<TraceEvent> TraceRecorder::Events() const
{
    LOCK(m_mutex);
    return m_events;
}

size_t TraceRecorder::Size() const
{
    LOCK(m_mutex);
    return m_events.size();
}

std::string TraceRecorder::Canonical() const
{
    std::string out;
    for (const TraceEvent& event : Events()) {
        out += event.Line();
        out += '\n';
    }
    return out;
}

uint256 TraceRecorder::Digest() const
{
    static constexpr std::string_view PREFIX{"fm2/sim/trace/1\n"};
    const std::string canonical{Canonical()};
    uint256 out;
    CSHA256{}
        .Write(reinterpret_cast<const unsigned char*>(PREFIX.data()), PREFIX.size())
        .Write(reinterpret_cast<const unsigned char*>(canonical.data()), canonical.size())
        .Finalize(out.data());
    return out;
}

uint64_t TraceRecorder::BarrierCount(storage::JournalRole role) const
{
    LOCK(m_mutex);
    const auto it{m_barriers.find(role)};
    return it == m_barriers.end() ? 0 : it->second;
}

AuthorityVersion TracingChainAdapter::CurrentAuthority() const
{
    const AuthorityVersion authority{m_inner.CurrentAuthority()};
    m_trace.Authority(authority);
    return authority;
}

} // namespace flowmesh2::sim
