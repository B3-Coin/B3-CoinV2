// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/storage/memory_journal.h>

#include <utility>

namespace flowmesh2::storage {

MemoryJournal::MemoryJournal(JournalRole role, uint64_t instance_id, MemoryJournalFaults faults, BarrierObserver* observer)
    : Journal(ImplKey{}, role, instance_id, observer), m_faults{faults}
{
}

Result<std::unique_ptr<MemoryJournal>> MemoryJournal::Create(JournalRole role, uint64_t instance_id,
                                                             MemoryJournalFaults faults, BarrierObserver* observer)
{
    if (!IsKnown(role)) return Error{ErrorCode::INVALID_INPUT, "unknown journal role"};
    return Ok(std::unique_ptr<MemoryJournal>{new MemoryJournal{role, instance_id, faults, observer}});
}

std::vector<unsigned char> MemoryJournal::DurableImage() const
{
    LOCK(m_image_mutex);
    return m_image;
}

Result<std::vector<JournalRecord>> MemoryJournal::ReadAll() const
{
    const std::vector<unsigned char> image{DurableImage()};
    Result<ScanResult> scanned{ScanRecords(image, Role(), InstanceId())};
    if (!scanned.has_value()) return scanned.error();
    return Ok(std::move(scanned->records));
}

Result<void> MemoryJournal::AdmitAppend(uint64_t append_number, uint64_t /*offset*/, size_t /*size*/) const
{
    if (m_faults.fail_append != 0 && append_number == m_faults.fail_append) {
        return Error{ErrorCode::STORAGE_FATAL, MEMORY_JOURNAL_APPEND_FAULT};
    }
    return Ok();
}

Result<void> MemoryJournal::Persist(uint64_t barrier_number, uint64_t offset, std::span<const unsigned char> bytes)
{
    if (m_faults.fail_barrier != 0 && barrier_number == m_faults.fail_barrier) {
        return Error{ErrorCode::STORAGE_FATAL, MEMORY_JOURNAL_BARRIER_FAULT};
    }
    LOCK(m_image_mutex);
    // The base journal hands over consecutive byte ranges, one barrier at a time.
    if (offset != m_image.size()) return Error{ErrorCode::STORAGE_FATAL, "memory journal image offset mismatch"};
    m_image.insert(m_image.end(), bytes.begin(), bytes.end());
    return Ok();
}

} // namespace flowmesh2::storage
