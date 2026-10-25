// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_MEMORY_JOURNAL_H
#define B3_FLOWMESH2_STORAGE_MEMORY_JOURNAL_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/storage/journal.h>
#include <flowmesh2/storage/record.h>
#include <sync.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

/** An in-memory journal with a fault plan (slice R8).
 *
 * Behaves exactly like a file journal whose disk never loses a committed
 * byte: Append queues framed records, Barrier moves them into the durable
 * image, and ReadAll() scans that image with ScanRecords, so the framing is
 * exercised end to end. What a crash would leave behind is DurableImage().
 *
 * Fault plan (for tests, the simulator's FaultPlan and R9 replay tests):
 * - fail_append = n: the n-th Append call that reaches the write step
 *   (after the fence, role and size checks) fails with STORAGE_FATAL
 *   (MEMORY_JOURNAL_APPEND_FAULT); nothing is queued and the journal is
 *   fatal from then on.
 * - fail_barrier = n: the n-th Barrier call fails with STORAGE_FATAL
 *   (MEMORY_JOURNAL_BARRIER_FAULT) without touching the durable image; the
 *   journal is fatal from then on.
 * 0 means never. Counting is 1-based and per journal.
 *
 * An optional BarrierObserver sees every accepted append and every barrier
 * (journal.h). Not deterministic code by directory rule (storage/), but this
 * class does no I/O and reads no clock.
 */
namespace flowmesh2::storage {

inline constexpr std::string_view MEMORY_JOURNAL_APPEND_FAULT{"MEMORY_JOURNAL_APPEND_FAULT"};
inline constexpr std::string_view MEMORY_JOURNAL_BARRIER_FAULT{"MEMORY_JOURNAL_BARRIER_FAULT"};

struct MemoryJournalFaults {
    uint64_t fail_append{0};  //!< 1-based Append call to fail; 0 = never
    uint64_t fail_barrier{0}; //!< 1-based Barrier call to fail; 0 = never
};

class MemoryJournal final : public Journal
{
public:
    //! INVALID_INPUT for an unknown role.
    static Result<std::unique_ptr<MemoryJournal>> Create(JournalRole role, uint64_t instance_id,
                                                         MemoryJournalFaults faults = {},
                                                         BarrierObserver* observer = nullptr);

    //! The bytes that survived the last successful barrier (what a crash leaves).
    std::vector<unsigned char> DurableImage() const EXCLUSIVE_LOCKS_REQUIRED(!m_image_mutex);

    //! ScanRecords over DurableImage(): only durable records, in order.
    Result<std::vector<JournalRecord>> ReadAll() const override EXCLUSIVE_LOCKS_REQUIRED(!m_image_mutex);

private:
    MemoryJournal(JournalRole role, uint64_t instance_id, MemoryJournalFaults faults, BarrierObserver* observer);

    Result<uint64_t> AdmitAppend(uint64_t append_number, uint64_t offset, size_t size) const override;
    Result<void> Persist(uint64_t barrier_number, uint64_t offset, std::span<const unsigned char> bytes) override
        EXCLUSIVE_LOCKS_REQUIRED(!m_image_mutex);

    const MemoryJournalFaults m_faults;
    //! Never held together with the base journal's lock (Persist runs without it).
    mutable Mutex m_image_mutex;
    std::vector<unsigned char> m_image GUARDED_BY(m_image_mutex);
};

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_MEMORY_JOURNAL_H
