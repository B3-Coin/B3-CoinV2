// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STATE_REPLAY_H
#define B3_FLOWMESH2_STATE_REPLAY_H

#include <flowmesh2/core/body.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/outcome.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/storage/record.h>
#include <flowmesh2/storage/snapshot_mark.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

/** Decision replay (slice R9; ARCHITECTURE.md section 7.4).
 *
 * ReplayDecisions(genesis, records) rebuilds the committed heads of a
 * FlowMesh instance from its genesis head and the records of its COMMIT
 * journal (Journal::ReadAll; storage/commit_log.h writes them). It
 * re-executes every DECISION body exactly once per height, with the same
 * code the state owner uses (ReexecuteOn: BuildCandidate plus exact header
 * and canonical-body equality), and returns every replayed header, the
 * committed head and the decided head. Pure and deterministic: it takes no
 * lock and touches no file; installing the result into a StateOwner is
 * Restore (stub M4-STO-03).
 *
 * The base is the genesis head or, since M4-STO-03 (O-95), a committed head
 * restored from a snapshot (state::SnapshotHead). Records are always the
 * whole journal: segments are never deleted. Below a snapshot base there is
 * no state to re-execute on, so those DECISION records are checked as a
 * chain instead: heights 1, 2, ... in order, each naming the previous
 * header's hash and certificate hash as its parent (VERIFY_FAILED,
 * REPLAY_BASE_CHAIN), and the one at the base height exactly the base
 * header with the base's certificate hash (REPLAY_BASE_CHAIN). The records
 * must decide every height up to the base and apply the base (INVALID_INPUT,
 * REPLAY_BASE_UNKNOWN), so a snapshot of state that was never committed
 * cannot be a base. Above the base everything is re-executed as from
 * genesis.
 *
 * Committed versus decided (O-89): a slot is committed state only once its
 * APPLIED record is durable, because APPLIED is written only after Apply
 * passed check 3. So `head` is the head after the last APPLIED slot, which
 * is the live state owner's head, and `decided_head` the head after the
 * last DECISION. They differ only by a tail of decided slots without
 * APPLIED (a crash between B-DECISION and B-APPLIED, or check 3 refused
 * the slot); those slots stay decided for their height, and recovery must
 * run check 3 on them under the current authority before it writes APPLIED
 * (Restore, M4-STO-03).
 *
 * Rules, the first failure returned:
 * - records are the whole journal, numbered 1, 2, 3, ... (ORDER_VIOLATION,
 *   REPLAY_SEQ); Engine::Create of the base state's profile comes first;
 * - DECISION: exact decode (DecisionRecord, its DECODE/OVERSIZE errors);
 *   VERIFY_FAILED (storage::DECISION_CERT_HEADER) unless the certificate
 *   names the header. At the next height it is re-executed on the replayed
 *   head (STALE_PARENT if it names another parent, VERIFY_FAILED if the
 *   header or body differ, any refusal of the validity predicate or the
 *   engine), and the new head takes the record's certificate hash. At a
 *   height already replayed, the same header is a duplicate and is skipped:
 *   it is applied once (also with another certificate; the first one
 *   counts, as in the commit log); another header is EQUIVOCATION_REFUSED
 *   (storage::DECISION_CONFLICT). Height 0 is ORDER_VIOLATION
 *   (REPLAY_DECISION_BELOW_BASE), a height that skips one ORDER_VIOLATION
 *   (REPLAY_DECISION_GAP).
 * - APPLIED: exact decode; ORDER_VIOLATION (REPLAY_APPLIED_UNKNOWN) unless
 *   its slot was replayed before it; VERIFY_FAILED (REPLAY_APPLIED_MISMATCH)
 *   unless header hash and state root are the replayed slot's; a repeat for
 *   an applied slot is skipped; otherwise it must name the next slot after
 *   the committed head (slot 1 first, no slot skipped; ORDER_VIOLATION,
 *   REPLAY_APPLIED_OUT_OF_ORDER), which then becomes the committed head.
 * - PREPARED_QC: exact full-value decode, binding and reexecution. It never
 *   advances either head. Cryptographic authentication is the certified
 *   StateOwner wrapper's responsibility (view0 only in this regtest slice).
 * - SNAPSHOT_MARK: exact decode (storage::SnapshotMark); ORDER_VIOLATION
 *   (REPLAY_MARK_UNAPPLIED) unless its slot is 1 or more and applied
 *   before the mark; VERIFY_FAILED (REPLAY_MARK_MISMATCH) unless header hash
 *   and state root are that slot's. It never moves either head; `marks`
 *   lists each marked slot once (the first record), ascending.
 * - Observer (M4-STO-03, O-71): ReplayDecisions(base, records, observer)
 *   calls `observer` once for every slot above the base when its APPLIED
 *   record makes it committed, in height order, with the slot's head, its
 *   certificate (of the first DECISION record), canonical body and outcomes
 *   in emission order: what the read service and the action pool were
 *   given when the slot was sealed. An observer error ends the replay and is
 *   returned. The calls happen while the replay runs, before the result is
 *   known to be valid, so a caller that builds anything from them discards
 *   it when the replay (or Restore) refuses.
 * - Any other type: a signer-journal type is ORDER_VIOLATION
 *   (storage::RECORD_ROLE), an unregistered one DECODE (storage::RECORD_TYPE).
 *
 * A decided slot without an APPLIED record is re-executed like any other;
 * its applied_seq is empty and it is above `head`, so recovery knows that
 * check 3 must pass and APPLIED must be written before the slot is
 * published (M4-STO-03, M4-STO-01).
 */
namespace flowmesh2::state {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view REPLAY_BASE_UNKNOWN{"records do not decide and apply the replay base"}; //!< (INVALID_INPUT)
inline constexpr std::string_view REPLAY_BASE_CHAIN{"decisions do not chain into the replay base"};        //!< (VERIFY_FAILED)
inline constexpr std::string_view REPLAY_MARK_UNAPPLIED{"snapshot mark of a slot not applied"};            //!< (ORDER_VIOLATION)
inline constexpr std::string_view REPLAY_MARK_MISMATCH{"snapshot mark differs from its slot"};             //!< (VERIFY_FAILED)
inline constexpr std::string_view REPLAY_SEQ{"records are not the whole journal in order"};                 //!< (ORDER_VIOLATION)
inline constexpr std::string_view REPLAY_DECISION_BELOW_BASE{"decision at the base height"};                //!< (ORDER_VIOLATION)
inline constexpr std::string_view REPLAY_DECISION_GAP{"decision skips a height"};                           //!< (ORDER_VIOLATION)
inline constexpr std::string_view REPLAY_APPLIED_UNKNOWN{"applied record before its decision"};             //!< (ORDER_VIOLATION)
inline constexpr std::string_view REPLAY_APPLIED_MISMATCH{"applied record differs from its decision"};      //!< (VERIFY_FAILED)
inline constexpr std::string_view REPLAY_APPLIED_OUT_OF_ORDER{"applied records out of slot order"};         //!< (ORDER_VIOLATION)

//! One replayed slot.
struct ReplayedSlot {
    SlotHeaderV2 header;                 //!< the decided header, reproduced by re-execution
    uint256 cert_hash;                   //!< hash of the certificate of its first DECISION record
    uint64_t decision_seq{0};            //!< journal sequence number of that record
    std::optional<uint64_t> applied_seq; //!< sequence number of its APPLIED record, if any

    friend bool operator==(const ReplayedSlot&, const ReplayedSlot&) = default;
};

struct ReplayResult {
    std::vector<ReplayedSlot> slots; //!< one per height 1, 2, ..., in order
    //! The committed head: after the last slot with an APPLIED record (the genesis head if none); the live state owner's head.
    HeadView head;
    //! After the last decided slot; differs from `head` only when decided slots lack APPLIED (they follow `head`).
    HeadView decided_head;
    uint64_t duplicate_decisions{0}; //!< DECISION records skipped as duplicates
    uint64_t duplicate_applied{0};   //!< APPLIED records skipped as repeats
    std::vector<storage::SnapshotMark> marks; //!< SNAPSHOT_MARK records, one per marked slot, ascending
};

//! A slot the replay just made committed (its APPLIED record): see the file comment.
struct ReplayCommitted {
    const HeadView& head;               //!< the committed head after the slot
    const SlotCertificate& certificate; //!< of its first DECISION record
    const SlotBody& body;               //!< the certified canonical body
    std::span<const OutcomeV2> outcomes; //!< emission order
};
using ReplayObserver = std::function<Result<void>(const ReplayCommitted&)>;

//! See the file comment.
Result<ReplayResult> ReplayDecisions(const HeadView& genesis, std::span<const storage::JournalRecord> records);
//! The same, calling `observer` for every slot above the base as it becomes committed (an empty observer is never called).
Result<ReplayResult> ReplayDecisions(const HeadView& genesis, std::span<const storage::JournalRecord> records,
                                     const ReplayObserver& observer);

} // namespace flowmesh2::state

#endif // B3_FLOWMESH2_STATE_REPLAY_H
