// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/journal.h>

#include <algorithm>
#include <utility>

namespace flowmesh2::storage {

Journal::Journal(ImplKey, JournalRole role, uint64_t instance_id, BarrierObserver* observer, const ScanResult* recovered)
    : m_role{role}, m_instance_id{instance_id}, m_observer{observer},
      m_chain{recovered == nullptr ? JournalGenesisChain(role, instance_id) : recovered->last_chain},
      m_appended_seq{recovered == nullptr ? 0 : recovered->records.size()},
      m_durable_seq{m_appended_seq},
      m_durable_offset{recovered == nullptr ? 0 : recovered->end_offset},
      m_unwritten_offset{m_durable_offset}
{
}

Result<uint64_t> Journal::Append(RecordType type, std::span<const unsigned char> payload)
{
    uint64_t seq{0};
    {
        LOCK(m_mutex);
        if (m_fatal) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
        if (!RoleAccepts(m_role, type)) return Error{ErrorCode::ORDER_VIOLATION, JOURNAL_ROLE_MISMATCH};
        Result<EncodedRecord> encoded{EncodeRecord(m_chain, type, payload)};
        if (!encoded.has_value()) return encoded.error();
        ++m_append_calls;
        const uint64_t offset{m_unwritten_offset + m_unwritten.size()};
        if (Result<void> admitted{AdmitAppend(m_append_calls, offset, encoded->bytes.size())}; !admitted.has_value()) {
            if (admitted.error().code == ErrorCode::STORAGE_FATAL) m_fatal = true;
            return admitted.error();
        }
        m_unwritten.insert(m_unwritten.end(), encoded->bytes.begin(), encoded->bytes.end());
        m_chain = encoded->chain;
        seq = ++m_appended_seq;
    }
    if (m_observer != nullptr) m_observer->OnAppend(m_role, m_instance_id, type, seq);
    return Ok(seq);
}

Result<uint64_t> Journal::Barrier()
{
    uint64_t barrier_number{0};
    uint64_t target{0};
    uint64_t offset{0};
    std::vector<unsigned char> bytes;
    {
        LOCK(m_mutex);
        if (m_fatal) return Error{ErrorCode::STORAGE_FATAL, JOURNAL_FATAL};
        if (m_barrier_running) return Error{ErrorCode::ORDER_VIOLATION, JOURNAL_BARRIER_CONCURRENT};
        m_barrier_running = true;
        barrier_number = ++m_barrier_calls;
        target = m_appended_seq;
        offset = m_unwritten_offset;
        bytes.swap(m_unwritten);
        m_unwritten_offset += bytes.size();
    }
    if (m_observer != nullptr) m_observer->OnBarrierStart(m_role, m_instance_id, target);
    // Disk (or simulated) work, without the journal lock. Never retried.
    const Result<void> persisted{Persist(barrier_number, offset, bytes)};
    {
        LOCK(m_mutex);
        m_barrier_running = false;
        if (persisted.has_value()) {
            m_durable_seq = std::max(m_durable_seq, target);
            m_durable_offset = offset + bytes.size();
        } else {
            m_fatal = true;
        }
    }
    if (m_observer != nullptr) m_observer->OnBarrierEnd(m_role, m_instance_id, target, persisted.has_value());
    if (!persisted.has_value()) return Error{ErrorCode::STORAGE_FATAL, persisted.error().detail};
    return Ok(target);
}

uint64_t Journal::DurableImageSize() const
{
    LOCK(m_mutex);
    return m_durable_offset;
}

JournalStatus Journal::Status() const
{
    LOCK(m_mutex);
    return JournalStatus{m_appended_seq, m_durable_seq, m_fatal};
}

} // namespace flowmesh2::storage
