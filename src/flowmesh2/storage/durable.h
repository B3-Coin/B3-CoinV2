// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_DURABLE_H
#define B3_FLOWMESH2_STORAGE_DURABLE_H

#include <flowmesh2/storage/record.h>
#include <flowmesh2/storage/tokens.h>
#include <uint256.h>

#include <cstdint>

/** Durability tokens (ARCHITECTURE.md section 7.2).
 *
 * A Durable<Kind> token proves that a journal record of that kind reached its
 * barrier. storage::Journal::Confirm(pending) mints it, and only when the
 * pending record belongs to that journal, the journal is not fatal and its
 * durable sequence number has reached the record (slice R8). The token is
 * the only way to take the step that its barrier protects:
 *
 *   DurableIntent      SeatSigner::Sign, the only BLS signing path    (B-INTENT)
 *   DurableSignedVote  building an OutboundVote, the only way to send  (B-VOTE)
 *   DurableDecision    StateOwner::Apply and CommitLog::AppendApplied  (B-DECISION)
 *   DurableApplied     ReadService::Publish                            (B-APPLIED)
 *
 * A token cannot be forged: it has no default constructor and no copy, and
 * its only constructor is private with storage::Journal as its only friend.
 * It can only be moved, into the call it unlocks. The compile-fail control
 * test/compile_fail/forge_token.cpp (CTest flowmesh2_negative_compile)
 * proves that constructing one anywhere else does not compile.
 *
 * Binding (slice R8): a token carries the journal instance, the record's
 * sequence number and the record's reference (storage/tokens.h): Slot(),
 * Digest() (the vote digest of INTENT and SIGNED_VOTE, the header hash of
 * DECISION and APPLIED), View() for the two vote kinds, and Binding() for
 * the whole reference (a SIGNED_VOTE's includes its signature, an APPLIED's
 * its state root).
 *
 * Moved-from tokens: a move copies the binding, and the source keeps it. A
 * token is a proof that a record is durable, which stays true; it is not a
 * single-use capability. Single use is not needed for safety: SignerLog
 * refuses a second digest for a VoteKey before anything is signed, BLS
 * signing is deterministic, so signing one intent twice gives the same
 * signature, and the logs make repeated appends of the same record
 * idempotent. Move-only keeps ownership explicit.
 *
 * Header-only and deterministic: no allocation, I/O, clock or lock.
 */
namespace flowmesh2 {

namespace storage {
class Journal; // the only class that mints tokens
} // namespace storage

//! Which barrier a token proves. Values equal the RecordType of the record.
enum class DurableKind : uint8_t {
    INTENT = 1,
    SIGNED_VOTE = 2,
    DECISION = 3,
    APPLIED = 4,
};

constexpr storage::RecordType RecordTypeOf(DurableKind kind) { return static_cast<storage::RecordType>(kind); }
constexpr DurableKind DurableKindOf(storage::RecordType type) { return static_cast<DurableKind>(type); }
//! True for the four record types that a token can prove.
constexpr bool HasDurableKind(storage::RecordType type)
{
    return type == storage::RecordType::INTENT || type == storage::RecordType::SIGNED_VOTE ||
           type == storage::RecordType::DECISION || type == storage::RecordType::APPLIED;
}
static_assert(RecordTypeOf(DurableKind::INTENT) == storage::RecordType::INTENT);
static_assert(RecordTypeOf(DurableKind::SIGNED_VOTE) == storage::RecordType::SIGNED_VOTE);
static_assert(RecordTypeOf(DurableKind::DECISION) == storage::RecordType::DECISION);
static_assert(RecordTypeOf(DurableKind::APPLIED) == storage::RecordType::APPLIED);

template <DurableKind K>
class [[nodiscard]] Durable
{
public:
    static constexpr DurableKind KIND{K};
    using Ref = typename storage::RecordRefOf<RecordTypeOf(K)>::type;

    Durable() = delete;
    Durable(const Durable&) = delete;
    Durable& operator=(const Durable&) = delete;
    Durable(Durable&&) noexcept = default;
    //! A token is handed on, never overwritten in place.
    Durable& operator=(Durable&&) = delete;
    ~Durable() = default;

    //! The journal instance that confirmed the record.
    uint64_t JournalInstance() const { return m_journal_instance; }
    //! The record's sequence number in that journal.
    uint64_t Seq() const { return m_seq; }
    //! The record's reference (storage/tokens.h).
    const Ref& Binding() const { return m_ref; }
    //! The slot (height) the record is about.
    uint64_t Slot() const { return storage::SlotOf(m_ref); }
    //! The vote digest (INTENT, SIGNED_VOTE) or the header hash (DECISION, APPLIED).
    const uint256& Digest() const { return storage::DigestOf(m_ref); }
    //! The view of a vote.
    uint64_t View() const
        requires(K == DurableKind::INTENT || K == DurableKind::SIGNED_VOTE)
    {
        return m_ref.key.view;
    }

private:
    friend class storage::Journal;
    Durable(uint64_t journal_instance, uint64_t seq, const Ref& ref) : m_journal_instance{journal_instance}, m_seq{seq}, m_ref{ref} {}

    uint64_t m_journal_instance;
    uint64_t m_seq;
    Ref m_ref;
};

using DurableIntent = Durable<DurableKind::INTENT>;
using DurableSignedVote = Durable<DurableKind::SIGNED_VOTE>;
using DurableDecision = Durable<DurableKind::DECISION>;
using DurableApplied = Durable<DurableKind::APPLIED>;

} // namespace flowmesh2

#endif // B3_FLOWMESH2_STORAGE_DURABLE_H
