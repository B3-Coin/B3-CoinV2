// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_DEV_DEV_CERTIFIER_H
#define B3_FLOWMESH2_DEV_DEV_CERTIFIER_H

#ifndef FM2_DEV_COMPONENTS
#error "dev/dev_certifier.h is a REGTEST-only dev component: only the fm2_dev target (FM2_DEV_COMPONENTS) and its users may include it"
#endif

#include <flowmesh2/agreement/agreement.h>
#include <flowmesh2/chain/chain_adapter.h>
#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/seat_signer.h>
#include <flowmesh2/storage/signer_log.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

/** DevSingleSeatCertifier: the REGTEST k = 1 certifier (slice R10; O-10;
 * ARCHITECTURE.md sections 7.3 and 8). DEV ONLY: it lives in fm2_dev,
 * compiles only with FM2_DEV_COMPONENTS, and guard rule 3 keeps its symbols
 * out of fm2_core and fm2_verify. One seat is NOT a quorum of a real seat
 * set (FN-10 asks for k >= 4); a certificate it makes is "certified" only
 * against the trusted dev seat set. PBFT is M4-AGR-02.
 *
 * Create(profile, adapter, seats, signer, signer_log, commit_log) refuses:
 * - a profile that Validate refuses (its error; a non-REGTEST profile is
 *   UNSUPPORTED_NETWORK) and an adapter whose network is not REGTEST
 *   (UNSUPPORTED_NETWORK, DEV_NETWORK);
 * - a seat set of k != 1 (INVALID_INPUT, DEV_SEAT_COUNT);
 * - a signer whose key is not the seat's (INVALID_INPUT, DEV_SEAT_KEY).
 * The references must outlive the certifier; the sealing thread owns it.
 *
 * Propose(candidate, authority), in this exact order (the barrier map of
 * section 7.3; each step's error is returned and nothing after it runs):
 *  0. FENCED (DEV_FENCED) if either journal is fatal, so a fenced node
 *     writes no new ACCEPTED_PROPOSAL or INTENT; ORDER_VIOLATION
 *     (DEV_DECISION_PENDING) while a decision has not been taken (one slot
 *     in flight); INVALID_INPUT (DEV_CANDIDATE_AUTHORITY) unless `authority`
 *     is the candidate header's AuthorityVersion; INVALID_INPUT
 *     (DEV_SEAT_SET) unless the header names this certifier's seat set;
 *     EQUIVOCATION_REFUSED (DEV_HEIGHT_DECIDED) if the commit log already
 *     holds a decision for the header's height with another header hash
 *     (CommitLog::DecisionAt). A decided height stays decided, also when
 *     check 3 refused to apply it, so a new epoch (a new VoteKey) never
 *     leads to a second signature for that height (O-89). The same header
 *     again is not refused here: it is idempotent all the way through.
 *  1. adapter.CheckAuthority(authority)               authority check 1
 *  2. signer_log.AppendAcceptedProposal({epoch, height, view 0}, header, body)
 *  3. signer_log.AppendIntent({domain, epoch, height, view 0, COMMIT},
 *                             header.CommitDigest(), proposal)
 *  4. signer_log.Barrier()                            B-INTENT
 *  5. signer_log.Confirm(intent) -> DurableIntent
 *  6. adapter.CheckAuthority(authority)               authority check 2
 *  7. signer.Sign(DurableIntent)                      the only BLS vote path
 *  8. signer_log.AppendSignedVote(intent, signature)
 *  9. signer_log.Barrier()                            B-VOTE
 * 10. signer_log.Confirm(vote) -> DurableSignedVote -> OutboundVote
 * 11. certificate {header.Hash(), bitmap 0x01, aggregate of the one
 *     signature}; it must pass bls::FastAggregateVerify with the seat's key
 *     over CommitDigest (INVARIANT, DEV_CERTIFICATE otherwise)
 * 12. commit_log.AppendDecision(header, certificate, body)
 * 13. commit_log.Barrier()                            B-DECISION
 * 14. commit_log.Confirm(decision) -> DurableDecision
 * Ok means the slot is decided: TakeDecision() returns Decision{token,
 * certificate, body} once. A failed barrier is STORAGE_FATAL and sticky (no
 * later step runs, so a failed B-VOTE leaves no DECISION). After a check-2
 * refusal the INTENT is durable but no signature exists, and the signer
 * log keeps refusing another digest for that (epoch, slot, view)
 * (EQUIVOCATION_REFUSED). Authority check 3 is StateOwner::Apply; the
 * caller then writes APPLIED (B-APPLIED). Nothing checks the authority
 * between check 2 and DECISION, so an authority change there leaves a
 * durable DECISION that check 3 refuses: the height is decided but not
 * applied, the node fences, and step 0 refuses any other header for that
 * height (O-89; the cross-epoch rule of M4-AGR-01, the proof M10-SPEC-01).
 *
 * OnMessage: Unsupported (M4-AGR-02): with one seat there is nothing to
 * receive. OnTimeout: Ok and no effect: Propose decides or fails
 * synchronously, so a timeout never has pending work (no view change with
 * one seat).
 */
namespace flowmesh2::dev {

// Static details (ErrorCode in parentheses).
inline constexpr std::string_view DEV_NETWORK{"the dev certifier is REGTEST only"};                         //!< (UNSUPPORTED_NETWORK)
inline constexpr std::string_view DEV_SEAT_COUNT{"the dev certifier needs exactly one seat"};              //!< (INVALID_INPUT)
inline constexpr std::string_view DEV_SEAT_KEY{"signer key is not the seat's key"};                        //!< (INVALID_INPUT)
inline constexpr std::string_view DEV_FENCED{"a journal of the certifier is fatal"};                       //!< (FENCED)
inline constexpr std::string_view DEV_DECISION_PENDING{"the previous decision was not taken"};             //!< (ORDER_VIOLATION)
inline constexpr std::string_view DEV_CANDIDATE_AUTHORITY{"authority is not the candidate header's"};      //!< (INVALID_INPUT)
inline constexpr std::string_view DEV_SEAT_SET{"candidate header names another seat set"};                 //!< (INVALID_INPUT)
inline constexpr std::string_view DEV_CERTIFICATE{"the one-seat certificate does not verify"};             //!< (INVARIANT)
inline constexpr std::string_view DEV_HEIGHT_DECIDED{"the commit log decided this height with another header"}; //!< (EQUIVOCATION_REFUSED)

class DevSingleSeatCertifier final : public agreement::Agreement
{
public:
    static Result<std::unique_ptr<DevSingleSeatCertifier>> Create(const ProfileParams& profile, chain::ChainAdapter& adapter,
                                                                  const SeatSet& seats, storage::SeatSigner& signer,
                                                                  storage::SignerLog& signer_log, storage::CommitLog& commit_log);

    Result<void> Propose(const state::Candidate& candidate, const AuthorityVersion& authority) override;
    //! Unsupported: M4-AGR-02.
    Result<void> OnMessage(uint32_t from_seat, std::span<const unsigned char> bytes) override;
    //! Ok, no effect.
    Result<void> OnTimeout(uint64_t now_ms) override;
    std::optional<agreement::Decision> TakeDecision() override;

    //! The seat set this certifier signs for (k = 1).
    const SeatSet& Seats() const { return m_seats; }

private:
    DevSingleSeatCertifier(chain::ChainAdapter& adapter, const SeatSet& seats, storage::SeatSigner& signer,
                           storage::SignerLog& signer_log, storage::CommitLog& commit_log);

    chain::ChainAdapter& m_adapter;
    const SeatSet m_seats;
    storage::SeatSigner& m_signer;
    storage::SignerLog& m_signer_log;
    storage::CommitLog& m_commit_log;
    std::optional<agreement::Decision> m_decision;
};

} // namespace flowmesh2::dev

#endif // B3_FLOWMESH2_DEV_DEV_CERTIFIER_H
