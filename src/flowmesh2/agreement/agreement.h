// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_AGREEMENT_AGREEMENT_H
#define B3_FLOWMESH2_AGREEMENT_AGREEMENT_H

#include <flowmesh2/core/authority.h>
#include <flowmesh2/core/body.h>
#include <flowmesh2/core/certificate.h>
#include <flowmesh2/core/result.h>
#include <flowmesh2/state/candidate.h>
#include <flowmesh2/storage/durable.h>

#include <cstdint>
#include <optional>
#include <span>

/** The agreement interface (slice R10; ARCHITECTURE.md sections 3, 7.3 and
 * 8; [CC] ownership boundary 3).
 *
 * An Agreement certifies a slot once: the exact parent, the exact original
 * action bytes (through actions_root) and the resulting commitments, all
 * covered by the header's CommitDigest. Implementations: the REGTEST k = 1
 * dev certifier (dev/dev_certifier.h, fm2_dev) and PBFT (agreement/pbft.h,
 * the stub M4-AGR-02).
 *
 * The contract every caller relies on (the sealing thread owns an Agreement;
 * no method takes a lock of the caller):
 *
 * - Propose(candidate, authority): the leader asks to certify `candidate`,
 *   built under `authority` (the candidate header's AuthorityVersion).
 *   Before anything is written the agreement checks the authority with the
 *   chain adapter (check 1: AUTHORITY_CHANGED, nothing written); after the
 *   intent barrier and immediately before signing it checks again (check 2:
 *   AUTHORITY_CHANGED, the INTENT is durable but no signature exists; the
 *   one-digest-per-VoteKey rule keeps refusing another digest for that key).
 *   Storage failures are STORAGE_FATAL (sticky) and a fenced journal is
 *   FENCED; nothing is retried. Ok means the agreement has started (PBFT) or
 *   finished (k = 1); the outcome is read with TakeDecision.
 * - OnMessage(from_seat, bytes): one encoded message (agreement/messages.h)
 *   from an authenticated seat session. Errors are the message's (DECODE,
 *   OVERSIZE, INVALID_INPUT, VERIFY_FAILED, EQUIVOCATION_REFUSED) or the
 *   agreement's (STORAGE_FATAL, FENCED); none of them changes durable state
 *   except by the barrier rules.
 * - OnTimeout(now_ms): time enters only here, as a parameter (section 5).
 * - TakeDecision(): the next decided slot, exactly once: a Decision whose
 *   DurableDecision proves the DECISION record (header, certificate, body)
 *   passed B-DECISION. The caller applies it (StateOwner::Apply, check 3),
 *   then writes APPLIED. nullopt when nothing new is decided.
 *
 * MessageSink is where an agreement sends its encoded messages; the seat
 * mesh implements it (R13, M4-NET-01). A vote may only be sent once its
 * SIGNED_VOTE record is durable: build it from an OutboundVote
 * (agreement/outbound.h).
 */
namespace flowmesh2::agreement {

//! A decided slot: the durable DECISION and what its record holds besides the header.
struct Decision {
    DurableDecision decision; //!< names the slot, the header hash and the certificate hash
    SlotCertificate certificate;
    SlotBody body;
};

class MessageSink
{
public:
    virtual ~MessageSink() = default;
    //! Queue one encoded message to `seat`; BUSY when its lane is full (nothing queued).
    virtual Result<void> Send(uint32_t seat, std::span<const unsigned char> message) = 0;
};

class Agreement
{
public:
    virtual ~Agreement() = default;

    virtual Result<void> Propose(const state::Candidate& candidate, const AuthorityVersion& authority) = 0;
    virtual Result<void> OnMessage(uint32_t from_seat, std::span<const unsigned char> bytes) = 0;
    virtual Result<void> OnTimeout(uint64_t now_ms) = 0;
    virtual std::optional<Decision> TakeDecision() = 0;
};

} // namespace flowmesh2::agreement

#endif // B3_FLOWMESH2_AGREEMENT_AGREEMENT_H
