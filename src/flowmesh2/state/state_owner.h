// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STATE_STATE_OWNER_H
#define B3_FLOWMESH2_STATE_STATE_OWNER_H

#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/slot.h>
#include <flowmesh2/engine/engine.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/storage/durable.h>
#include <flowmesh2/storage/record.h>
#include <sync.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

/** The single-writer state owner (slice R9; ARCHITECTURE.md sections 3, 4
 * and 8; [CC] ownership boundary 2).
 *
 * One StateOwner holds the committed head of one FlowMesh instance as an
 * immutable shared_ptr<const HeadView>. It is the only code that changes
 * that head, and it changes it only by a pointer swap in Apply, under its
 * Bitcoin Mutex. The lock protects the head pointer and nothing else: it is
 * held only to copy or swap the pointer, never while a slot executes, a
 * hash is computed or anything waits. A reader (Head) gets one snapshot in
 * which header, certificate hash and state belong to one slot.
 *
 * - Preview(parent, inputs) const: refuses a CANDIDATE parent (pipelining,
 *   stub M4-AGR-10) and an unknown parent kind (INVALID_INPUT,
 *   PARENT_KIND_UNKNOWN); copies the head pointer under the lock; STALE_PARENT
 *   (PARENT_NOT_HEAD) unless `parent` names that head (height and header
 *   hash); then BuildCandidate (state/candidate.h) WITHOUT the lock. The
 *   head is never changed. REAL-REPLACE M2-LED-10: every Preview copies
 *   the whole state (copy-on-write replaces it).
 * - Reexecute(header, body) const: a follower's check of a proposal or a
 *   decided slot: ReexecuteOn on the head, i.e. Preview with the inputs the
 *   header names plus exact header and canonical-body equality (VERIFY_FAILED
 *   otherwise; STALE_PARENT unless the header names the head).
 * - Apply(candidate, decision, current), the single writer, in this order:
 *   STALE_PARENT (PARENT_NOT_HEAD) unless the candidate's header names the
 *   current head; VERIFY_FAILED (storage::TOKEN_OTHER_LOG) for a token of
 *   another commit journal instance than the one the owner was created
 *   for; VERIFY_FAILED (DECISION_OTHER_HEADER) unless the token's slot and
 *   header hash are the candidate header's; AUTHORITY_CHANGED
 *   (AUTHORITY_SUPERSEDED) unless the candidate header's AuthorityVersion
 *   (epoch, seat set root, profile hash) equals `current`, the authority
 *   the caller holds now (check 3 of ARCHITECTURE.md section 8; the caller
 *   fences admission and sealing). Then the new head is {candidate header,
 *   the certificate hash the token names (the one the DECISION record
 *   holds), candidate state}, installed by a compare-and-swap of the head
 *   pointer: if another Apply changed the head meanwhile, STALE_PARENT. On
 *   any refusal the head and the candidate are unchanged. Applying a second
 *   candidate for an already applied slot is STALE_PARENT (N13).
 * - Head(): the current head; never null.
 * - Restore(base, records): install a head rebuilt from a snapshot and the
 *   DECISION records after it. Stub M4-STO-03; ReplayDecisions
 *   (state/replay.h) is the real, pure part of recovery. Restore installs
 *   ReplayResult::head (the last APPLIED slot); a decided slot without
 *   APPLIED is applied only after check 3 passes again under the current
 *   authority, and a refusal leaves the head there and the node fenced
 *   (O-89).
 *
 * Create(profile, genesis, commit_journal_instance) takes a GENESIS head
 * only (height 0, zero parent hash, parent certificate hash and certificate
 * hash; INVALID_INPUT, OWNER_NOT_GENESIS), of this profile (INVALID_INPUT,
 * OWNER_PROFILE_MISMATCH), whose header names the state's seat set
 * (INVALID_INPUT, OWNER_SEAT_SET_MISMATCH) and commits to the state
 * (VERIFY_FAILED, OWNER_STATE_ROOT_MISMATCH); Engine::Create(profile)'s
 * errors come first. It does not re-run the slot-boundary invariants
 * (EngineState::CheckInvariants): the engine checks the parent's held fees
 * at the start of every slot and every invariant on every candidate (phase
 * 9), so a broken initial state can never produce a candidate (tested by an
 * injected failure). Any other starting head is Restore's job.
 *
 * Determinism: Preview, Reexecute and Apply depend only on the head and
 * their arguments; no clock, I/O or randomness. Threading: any thread may
 * call any method; Apply calls are linearised by the compare-and-swap.
 */
namespace flowmesh2::state {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view OWNER_NOT_GENESIS{"initial head is not a genesis head"};                    //!< (INVALID_INPUT)
inline constexpr std::string_view OWNER_PROFILE_MISMATCH{"initial head of another profile"};                   //!< (INVALID_INPUT)
inline constexpr std::string_view OWNER_SEAT_SET_MISMATCH{"initial header names another seat set"};            //!< (INVALID_INPUT)
inline constexpr std::string_view OWNER_STATE_ROOT_MISMATCH{"initial header does not commit to its state"};    //!< (VERIFY_FAILED)
inline constexpr std::string_view DECISION_OTHER_HEADER{"decision token for another header"};                 //!< (VERIFY_FAILED)
inline constexpr std::string_view AUTHORITY_SUPERSEDED{"candidate authority is not the current authority"};   //!< (AUTHORITY_CHANGED)

class StateOwner
{
public:
    //! See the file comment. The owner applies only decisions of journal `commit_journal_instance`.
    static Result<std::unique_ptr<StateOwner>> Create(const ProfileParams& profile, const HeadView& genesis,
                                                      uint64_t commit_journal_instance);

    StateOwner(const StateOwner&) = delete;
    StateOwner& operator=(const StateOwner&) = delete;

    const ProfileParams& Profile() const { return m_engine.Profile(); }
    uint64_t CommitJournalInstance() const { return m_commit_journal_instance; }

    //! The committed head (one snapshot: header, certificate hash and state of one slot).
    std::shared_ptr<const HeadView> Head() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Execute `inputs` on the committed head `parent`; pure. See the file comment.
    Result<Candidate> Preview(const ParentRef& parent, const SlotInputs& inputs) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Re-execute `header` and `body` on the head; pure. See the file comment.
    Result<Candidate> Reexecute(const SlotHeaderV2& header, const SlotBody& body) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Install `candidate` as the head under a durable decision. Returns the new head.
    Result<std::shared_ptr<const HeadView>> Apply(Candidate&& candidate, const DurableDecision& decision,
                                                  const AuthorityVersion& current) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! Snapshot `base` plus the commit journal `records` after it. Stub: M4-STO-03.
    Result<void> Restore(const HeadView& base, std::span<const storage::JournalRecord> records) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    StateOwner(engine::Engine engine, std::shared_ptr<const HeadView> genesis, uint64_t commit_journal_instance);

    const engine::Engine m_engine;
    const uint64_t m_commit_journal_instance;
    mutable Mutex m_mutex;
    std::shared_ptr<const HeadView> m_head GUARDED_BY(m_mutex);
};

} // namespace flowmesh2::state

#endif // B3_FLOWMESH2_STATE_STATE_OWNER_H
