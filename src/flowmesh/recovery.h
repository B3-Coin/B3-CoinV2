// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_RECOVERY_H
#define B3COIN_FLOWMESH_RECOVERY_H

#include <pubkey.h>
#include <uint256.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace flowmesh {

/**
 * Minimal leader-recovery framework.
 *
 * The naive rule "leader timeout -> next leader" is NOT assumed safe:
 * competing proposals for one sequence can split attestations so no
 * certificate forms, and without a discipline on re-attestation two
 * certificates could form. This header isolates the smallest explicit
 * mechanism that is safe by construction, without importing a full BFT
 * protocol:
 *
 *   ROUNDS.  A sequence may be proposed in rounds 0, 1, 2, ... — round r
 *   has exactly one schedule-determined eligible proposer. A validator
 *   advances its local round for a sequence only on its own timeout
 *   (when to time out is LOCAL LIVENESS POLICY, deliberately outside
 *   the deterministic core: nothing about state depends on it).
 *
 *   LOCK.  Once a validator attests hash H at sequence s, it may attest
 *   ONLY H at s ever again (any round). Together with the certificate
 *   threshold (certificate.h: 2t - k > f) this gives uniqueness: two
 *   certificates at one sequence would share an honest attester, and an
 *   honest attester never signs two hashes at one sequence.
 *
 *   LIVENESS — HONEST LIMIT.  A split round simply fails to certify; a
 *   later round's proposer may re-propose a locked candidate (locks
 *   permit re-attesting the same hash, and attestations for one hash
 *   combine across rounds). BUT rounds do NOT necessarily converge
 *   split locks: PERMANENT SPLIT LOCKS MAY HALT FLOWMESH INDEFINITELY
 *   (e.g. k=4, f=1, t=3 with honest locks split 2/1 and the Byzantine
 *   seat withholding). Resolving that requires a cross-round
 *   unlock/view-change rule — an OWNER DECISION deliberately not
 *   implemented here; permanent locking is the fail-safe default, and
 *   B3 is unaffected by any FlowMesh stall.
 *
 * OWNER DECISIONS exposed, not hidden: the proposer schedule itself,
 * the fault bound f (hence threshold t), and the timeout policy.
 * RoundRobinSchedule below is a provisional default pending
 * ratification, not a settled protocol rule.
 */
class ProposerSchedule
{
public:
    virtual ~ProposerSchedule() = default;
    //! The single seat eligible to propose `sequence` in `round`, or
    //! nullopt if the schedule cannot answer (e.g. empty seat set).
    virtual std::optional<XOnlyPubKey> ProposerAt(uint64_t sequence, uint32_t round) const = 0;
};

//! Deterministic round-robin over the canonically sorted seat set:
//! seats[(sequence + round) % k]. No randomness, no grinding surface;
//! the seat set is public so secret leader election buys nothing here.
//! PROVISIONAL DEFAULT — the schedule rule is an owner decision.
class RoundRobinSchedule final : public ProposerSchedule
{
public:
    explicit RoundRobinSchedule(std::vector<XOnlyPubKey> seats) : m_seats{std::move(seats)}
    {
        std::sort(m_seats.begin(), m_seats.end());
        m_seats.erase(std::unique(m_seats.begin(), m_seats.end()), m_seats.end());
    }

    std::optional<XOnlyPubKey> ProposerAt(const uint64_t sequence,
                                          const uint32_t round) const override
    {
        if (m_seats.empty()) return std::nullopt;
        return m_seats[(sequence + round) % m_seats.size()];
    }

private:
    std::vector<XOnlyPubKey> m_seats;
};

/**
 * Durable write-ahead journal for safety-critical lock state. A
 * validator MUST persist its lock BEFORE an attestation leaves the
 * process: a restart may not erase what the validator has signed, or a
 * conflicting candidate could be signed for the same sequence after
 * recovery. Storage failure means DO NOT SIGN (non-participation, never
 * unsafe participation).
 */
class LockJournal
{
public:
    virtual ~LockJournal() = default;
    [[nodiscard]] virtual bool WriteLock(uint64_t sequence, const uint256& microblock_hash) = 0;
    //! Certification through `sequence` makes its lock obsolete.
    [[nodiscard]] virtual bool ClearLocksThrough(uint64_t sequence) = 0;
};

enum class AttestDecision : uint8_t {
    ATTEST = 0,
    WRONG_ROUND = 1,     // proposal round is not this validator's current round
    WRONG_PROPOSER = 2,  // proposer is not the round's scheduled seat
    LOCK_CONFLICT = 3,   // validator is locked on a different hash at this sequence
};

/**
 * Validator-local attestation guard enforcing the round and lock rules.
 * Everything here is a pure function of what this validator has done and
 * seen; only round advancement is (deliberately) driven by local timing.
 */
class AttestationGuard
{
public:
    uint32_t CurrentRound(const uint64_t sequence) const
    {
        const auto it{m_rounds.find(sequence)};
        return it == m_rounds.end() ? 0 : it->second;
    }

    std::optional<uint256> LockedHash(const uint64_t sequence) const
    {
        const auto it{m_locked.find(sequence)};
        if (it == m_locked.end()) return std::nullopt;
        return it->second;
    }

    AttestDecision Consider(const ProposerSchedule& schedule, const uint64_t sequence,
                            const uint32_t round, const XOnlyPubKey& proposer,
                            const uint256& microblock_hash) const
    {
        if (round != CurrentRound(sequence)) return AttestDecision::WRONG_ROUND;
        const std::optional<XOnlyPubKey> scheduled{schedule.ProposerAt(sequence, round)};
        if (!scheduled || !(*scheduled == proposer)) return AttestDecision::WRONG_PROPOSER;
        const std::optional<uint256> locked{LockedHash(sequence)};
        if (locked && *locked != microblock_hash) return AttestDecision::LOCK_CONFLICT;
        return AttestDecision::ATTEST;
    }

    //! Record that this validator attested `microblock_hash` at
    //! `sequence` — the lock is permanent for the sequence.
    void NoteAttested(const uint64_t sequence, const uint256& microblock_hash)
    {
        m_locked.emplace(sequence, microblock_hash);
    }

    //! Local timeout: no certificate for `sequence` arrived in time.
    //! Advances this validator's round so the next scheduled proposer
    //! becomes acceptable. Timing policy is local liveness, not state.
    void NoteTimeout(const uint64_t sequence) { ++m_rounds[sequence]; }

    //! A certificate for `sequence` was accepted: per-sequence recovery
    //! bookkeeping is complete and can be dropped.
    void NoteCertified(const uint64_t sequence)
    {
        m_rounds.erase(sequence);
        m_locked.erase(sequence);
    }

    //! Restart restore: import the durably journaled locks so the
    //! validator cannot sign a conflicting candidate after recovery.
    void ImportLocks(const std::map<uint64_t, uint256>& locks)
    {
        for (const auto& [sequence, hash] : locks) m_locked.emplace(sequence, hash);
    }

    const std::map<uint64_t, uint256>& Locks() const { return m_locked; }

private:
    std::map<uint64_t, uint32_t> m_rounds;
    std::map<uint64_t, uint256> m_locked;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_RECOVERY_H
