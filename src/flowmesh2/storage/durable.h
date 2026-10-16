// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_DURABLE_H
#define B3_FLOWMESH2_STORAGE_DURABLE_H

#include <cstdint>

/** Durability tokens (ARCHITECTURE.md section 7.2).
 *
 * A Durable<Kind> token proves that a journal record of that kind reached its
 * barrier. Journal::Confirm(pending) will mint it, and only when
 * DurableSeq() >= seq and the journal is not fatal (slice R8). The token is
 * the only way to take the step that its barrier protects:
 *
 *   DurableIntent      SeatSigner::Sign, the only BLS signing path    (B-INTENT)
 *   DurableSignedVote  building an OutboundVote, the only way to send  (B-VOTE)
 *   DurableDecision    StateOwner::Apply and CommitLog::AppendApplied  (B-DECISION)
 *   DurableApplied     ReadService::Publish                            (B-APPLIED)
 *
 * A token cannot be forged: it has no default constructor and no copy, and
 * its only constructor is private with class Journal as its only friend. It
 * can only be moved, into the call it unlocks. The compile-fail control
 * test/compile_fail/forge_token.cpp (CTest flowmesh2_negative_compile)
 * proves that constructing one anywhere else does not compile.
 *
 * Slice R2 adds the type only, so the control has a real target. Nothing can
 * mint a token until slice R8 adds Journal and Journal::Confirm. R8 also binds
 * each token to its slot, view and digest or header hash (section 7.2) and
 * defines what a moved-from token means; in R2 a token carries the journal
 * instance and the record sequence number.
 *
 * Header-only and deterministic: no allocation, I/O, clock or lock.
 */
namespace flowmesh2 {

class Journal; // slice R8: the only class that mints tokens

//! Which barrier a token proves. Values follow RecordType where they match.
enum class DurableKind : uint8_t {
    INTENT = 1,
    SIGNED_VOTE = 2,
    DECISION = 3,
    APPLIED = 4,
};

template <DurableKind K>
class [[nodiscard]] Durable
{
public:
    static constexpr DurableKind KIND{K};

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

private:
    friend class Journal;
    constexpr Durable(uint64_t journal_instance, uint64_t seq) : m_journal_instance{journal_instance}, m_seq{seq} {}

    uint64_t m_journal_instance;
    uint64_t m_seq;
};

using DurableIntent = Durable<DurableKind::INTENT>;
using DurableSignedVote = Durable<DurableKind::SIGNED_VOTE>;
using DurableDecision = Durable<DurableKind::DECISION>;
using DurableApplied = Durable<DurableKind::APPLIED>;

} // namespace flowmesh2

#endif // B3_FLOWMESH2_STORAGE_DURABLE_H
