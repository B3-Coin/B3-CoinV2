// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_JOURNAL_H
#define B3_FLOWMESH2_STORAGE_JOURNAL_H

#include <flowmesh2/core/result.h>
#include <flowmesh2/storage/record.h>
#include <sync.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

/** The journal interface (slice R8; ARCHITECTURE.md sections 4 and 7).
 *
 * A Journal is an append-only sequence of framed records (storage/record.h)
 * of one role and one instance. Records get sequence numbers 1, 2, 3, ...
 * in append order. Append() only queues a record; Barrier() makes every
 * record appended before it durable and returns the durable sequence number.
 * Several appends followed by one barrier form a group commit.
 *
 * The contract every caller relies on:
 *
 * - Append(type, payload) -> Result<seq>.
 *   FENCED after a fatal error. ORDER_VIOLATION (JOURNAL_ROLE_MISMATCH) for a
 *   type this role does not hold, including unregistered types. OVERSIZE for
 *   a payload above MAX_RECORD_PAYLOAD_BYTES. A refused append consumes no
 *   sequence number and changes nothing; only STORAGE_FATAL (an append that
 *   the implementation could not take) also makes the journal fatal.
 * - Barrier() -> Result<durable_seq>.
 *   Writes and commits everything appended so far, then returns that
 *   sequence number. ANY failure is STORAGE_FATAL and sticky: the journal is
 *   fatal from then on and nothing is ever retried (DUR-8). A barrier of a
 *   fatal journal is STORAGE_FATAL again. Barriers of one journal are issued
 *   by one thread (section 4); a second Barrier while one is running is
 *   ORDER_VIOLATION (JOURNAL_BARRIER_CONCURRENT) and changes nothing.
 * - Status() reads {appended_seq, durable_seq, fatal} atomically;
 *   DurableSeq() and IsFatal() read one field each.
 * - ReadAll() returns the durable records of the journal.
 *
 * After a fatal error Append is FENCED, Barrier is STORAGE_FATAL and the
 * durability tokens (storage/durable.h) can no longer be minted; the node
 * fences admission and sealing (section 7.1).
 *
 * Locking: one Mutex per journal guards the sequence numbers, the chain, the
 * unwritten bytes and the fatal flag. Append holds it while it frames the
 * record. Barrier takes the unwritten bytes and the target under the lock,
 * releases it, writes and commits WITHOUT the lock (no fm2 lock is held
 * across disk work), then re-locks to record the result. Observer callbacks
 * run without the lock.
 *
 * The implementations are closed: only MemoryJournal and FileJournal can
 * construct the base (Journal::ImplKey), so the durable sequence number that
 * tokens are minted against is always this class's own bookkeeping.
 */
namespace flowmesh2::storage {

class MemoryJournal;
class FileJournal;

// Static error details of the journal (ErrorCode in parentheses).
inline constexpr std::string_view JOURNAL_FENCED{"JOURNAL_FENCED"};                        //!< (FENCED) the journal is fatal
inline constexpr std::string_view JOURNAL_FATAL{"JOURNAL_FATAL"};                          //!< (STORAGE_FATAL) barrier of a fatal journal
inline constexpr std::string_view JOURNAL_ROLE_MISMATCH{"JOURNAL_ROLE_MISMATCH"};          //!< (ORDER_VIOLATION) type not held by the role
inline constexpr std::string_view JOURNAL_BARRIER_CONCURRENT{"JOURNAL_BARRIER_CONCURRENT"}; //!< (ORDER_VIOLATION) a barrier is running

/** Sees every accepted append and every barrier of a journal, in order.
 * Callbacks run on the calling thread without the journal lock, so they may
 * call back into the journal. Used by tests and by the simulator's trace
 * (R14, section 14.4 P3).
 */
class BarrierObserver
{
public:
    virtual ~BarrierObserver() = default;
    //! After a record was accepted with sequence number `seq`.
    virtual void OnAppend(JournalRole role, uint64_t instance_id, RecordType type, uint64_t seq) = 0;
    //! A barrier for every record up to `target_seq` starts its write and commit.
    virtual void OnBarrierStart(JournalRole role, uint64_t instance_id, uint64_t target_seq) = 0;
    //! That barrier finished; `ok` false means the journal is now fatal.
    virtual void OnBarrierEnd(JournalRole role, uint64_t instance_id, uint64_t target_seq, bool ok) = 0;
};

struct JournalStatus {
    uint64_t appended_seq{0}; //!< sequence number of the last accepted record (0 if none)
    uint64_t durable_seq{0};  //!< every record up to this one is durable
    bool fatal{false};        //!< sticky: a write or commit failed

    friend bool operator==(const JournalStatus&, const JournalStatus&) = default;
};

class Journal
{
public:
    virtual ~Journal() = default;
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;

    JournalRole Role() const { return m_role; }
    uint64_t InstanceId() const { return m_instance_id; }

    //! Queue one record; see the file comment for every refusal.
    Result<uint64_t> Append(RecordType type, std::span<const unsigned char> payload) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    //! Make every queued record durable; STORAGE_FATAL (sticky) on any failure.
    Result<uint64_t> Barrier() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    JournalStatus Status() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    uint64_t DurableSeq() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex) { return Status().durable_seq; }
    bool IsFatal() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex) { return Status().fatal; }

    //! The durable records, in order (FileJournal: stub M4-STO-01).
    virtual Result<std::vector<JournalRecord>> ReadAll() const = 0;

protected:
    //! Only the two implementations can construct a Journal.
    class ImplKey
    {
        ImplKey() = default;
        friend class MemoryJournal;
        friend class FileJournal;
    };

    Journal(ImplKey, JournalRole role, uint64_t instance_id, BarrierObserver* observer);

    /** Called with the journal lock held, before a framed record of `size`
     * bytes is queued at byte `offset` of the journal image; `append_number`
     * counts the Append calls that got this far (1-based). A pure check of
     * the implementation's limits: no I/O, no lock, no callback. Any error
     * refuses the append and consumes nothing; STORAGE_FATAL also makes the
     * journal fatal.
     */
    virtual Result<void> AdmitAppend(uint64_t append_number, uint64_t offset, size_t size) const = 0;

    /** Called by Barrier WITHOUT the journal lock: write `bytes` at byte
     * `offset` of the journal image and make the image durable up to
     * offset + bytes.size(). `barrier_number` counts Barrier calls that got
     * this far (1-based). Called for every barrier, also with no new bytes.
     * Any error makes the journal fatal; Barrier reports STORAGE_FATAL with
     * the error's detail. Never called concurrently for one journal.
     */
    virtual Result<void> Persist(uint64_t barrier_number, uint64_t offset, std::span<const unsigned char> bytes) = 0;

private:
    const JournalRole m_role;
    const uint64_t m_instance_id;
    BarrierObserver* const m_observer;

    mutable Mutex m_mutex;
    uint256 m_chain GUARDED_BY(m_mutex);
    uint64_t m_appended_seq GUARDED_BY(m_mutex){0};
    uint64_t m_durable_seq GUARDED_BY(m_mutex){0};
    bool m_fatal GUARDED_BY(m_mutex){false};
    uint64_t m_append_calls GUARDED_BY(m_mutex){0};
    uint64_t m_barrier_calls GUARDED_BY(m_mutex){0};
    bool m_barrier_running GUARDED_BY(m_mutex){false};
    //! Image offset of m_unwritten[0]; everything before it was handed to Persist.
    uint64_t m_unwritten_offset GUARDED_BY(m_mutex){0};
    std::vector<unsigned char> m_unwritten GUARDED_BY(m_mutex);
};

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_JOURNAL_H
