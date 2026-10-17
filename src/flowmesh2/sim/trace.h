// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_SIM_TRACE_H
#define B3_FLOWMESH2_SIM_TRACE_H

#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/storage/journal.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/storage/seat_signer.h>
#include <sync.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/** The simulator's trace (slice R14; ARCHITECTURE.md 14.4 P3 and P6).
 *
 * A TraceRecorder collects one run's events in order:
 * - as the storage::BarrierObserver of the signer and commit journals:
 *   every accepted append (`signer.append` / `commit.append`, with the
 *   record type and sequence number) and every barrier
 *   (`<role>.barrier.start` with its target sequence, `<role>.barrier.end`
 *   with ok or fail);
 * - as the storage::SignObserver of the seat signer: every vote signature
 *   (`bls.sign`, with its slot and view);
 * - from TracingChainAdapter: every read of the chain authority
 *   (`authority.check`, with the epoch). The dev certifier's checks 1 and 2
 *   (ChainAdapter::CheckAuthority) and the node's read before Apply (check
 *   3) are the only readers during a slot;
 * - from the node and the scenario, through Record: `slot.begin`,
 *   `chain.poll`, `pool.take`, `state.preview`, `state.apply`,
 *   `read.publish`, `pool.on_committed`, `slot.end`, fault and client
 *   events.
 * Each event is stamped with the slot being sealed (BeginSlot .. EndSlot;
 * 0 outside a slot). A journal callback does not name its record's slot:
 * the P3 check decodes the durable record of each append's sequence number
 * to prove it belongs to that slot.
 *
 * The canonical trace is every event's Line() followed by '\n'; Digest()
 * is SHA256("fm2/sim/trace/1\n" || canonical trace). Lines hold registry
 * names, decimal numbers and hashes only: no wall-clock time, path or
 * pointer, so one seed gives one digest on every run and host.
 *
 * Locking: one Mutex guards the events; it is held only to append or copy,
 * never across a call into another object (ARCHITECTURE.md section 4).
 */
namespace flowmesh2::sim {

//! One trace event; see the file comment.
struct TraceEvent {
    std::string kind;   //!< "<component>.<action>", e.g. "signer.append"
    uint64_t slot{0};   //!< the slot being sealed (0 outside a slot)
    std::string detail; //!< canonical details
    std::optional<storage::RecordType> record; //!< journal appends: the record type
    uint64_t seq{0};    //!< journal appends: the record's sequence; barriers: the target sequence
    bool ok{true};      //!< barrier ends: whether the barrier succeeded

    //! "<slot>|<kind>|<detail>": one line of the canonical trace.
    std::string Line() const;
    //! The kind with the record type or barrier result the P3 order names, e.g.
    //! "signer.append:INTENT" or "commit.barrier.end:ok".
    std::string Step() const;

    friend bool operator==(const TraceEvent&, const TraceEvent&) = default;
};

class TraceRecorder final : public storage::BarrierObserver, public storage::SignObserver
{
public:
    TraceRecorder() = default;
    TraceRecorder(const TraceRecorder&) = delete;
    TraceRecorder& operator=(const TraceRecorder&) = delete;

    // storage::BarrierObserver.
    void OnAppend(storage::JournalRole role, uint64_t instance_id, storage::RecordType type, uint64_t seq) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void OnBarrierStart(storage::JournalRole role, uint64_t instance_id, uint64_t target_seq) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void OnBarrierEnd(storage::JournalRole role, uint64_t instance_id, uint64_t target_seq, bool ok) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    // storage::SignObserver.
    void OnSigned(uint64_t slot, uint64_t view, const uint256& digest) override EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! A chain authority read (TracingChainAdapter).
    void Authority(const AuthorityVersion& authority) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! `slot.begin`; later events carry `slot` until EndSlot.
    void BeginSlot(uint64_t slot) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! `slot.end` with `result` ("OK" or an ErrorCode name); later events carry slot 0.
    void EndSlot(uint64_t slot, std::string_view result) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Any other event.
    void Record(std::string_view kind, std::string detail) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    std::vector<TraceEvent> Events() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    size_t Size() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! The canonical trace (see the file comment).
    std::string Canonical() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! SHA256("fm2/sim/trace/1\n" || Canonical()).
    uint256 Digest() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Barrier starts seen for `role` so far.
    uint64_t BarrierCount(storage::JournalRole role) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    void Push(TraceEvent event) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    mutable Mutex m_mutex;
    std::vector<TraceEvent> m_events GUARDED_BY(m_mutex);
    uint64_t m_slot GUARDED_BY(m_mutex){0};
    std::map<storage::JournalRole, uint64_t> m_barriers GUARDED_BY(m_mutex);
};

//! "signer" or "commit" (the role's lower-case name); "journal" for an unregistered role.
std::string_view RolePrefix(storage::JournalRole role);

/** A ChainAdapter that forwards every call to `inner` and reports each
 * CurrentAuthority() read to `trace` as `authority.check`. Both must
 * outlive it. ChainAdapter::CheckAuthority reads CurrentAuthority, so the
 * dev certifier's authority checks pass through here.
 */
class TracingChainAdapter final : public chain::ChainAdapter
{
public:
    TracingChainAdapter(chain::ChainAdapter& inner, TraceRecorder& trace) : m_inner{inner}, m_trace{trace} {}

    NetworkId Network() const override { return m_inner.Network(); }
    AuthorityVersion CurrentAuthority() const override;
    Result<chain::PollReply> Poll(const FactCursor& after, size_t max_facts) override { return m_inner.Poll(after, max_facts); }
    Result<void> VerifyAnchor(const Anchor& candidate, const Anchor& last_certified) const override
    {
        return m_inner.VerifyAnchor(candidate, last_certified);
    }
    Result<SeatSet> SeatSetAt(uint64_t epoch) const override { return m_inner.SeatSetAt(epoch); }

private:
    chain::ChainAdapter& m_inner;
    TraceRecorder& m_trace;
};

} // namespace flowmesh2::sim

#endif // B3_FLOWMESH2_SIM_TRACE_H
