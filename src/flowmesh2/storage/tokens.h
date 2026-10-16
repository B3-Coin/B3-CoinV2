// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef B3_FLOWMESH2_STORAGE_TOKENS_H
#define B3_FLOWMESH2_STORAGE_TOKENS_H

#include <crypto/bls.h>
#include <flowmesh2/storage/record.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <ios>
#include <tuple>

/** Keys, record references and pending tokens (slice R8; ARCHITECTURE.md 7.2).
 *
 * Every Append* of the signer and commit logs returns a Pending<Type>: the
 * journal instance, the record's sequence number and the key of what the
 * record says (its reference). A Pending proves nothing about durability.
 * Only Journal::Confirm turns it into a Durable token (storage/durable.h), and
 * only once the journal's durable sequence number has reached it.
 *
 * A Pending is copyable but cannot be forged: its only constructor is
 * private, and SignerLog and CommitLog, which build it from the record they
 * just appended, are its only friends. So a Durable token's reference is
 * always what was actually written at that sequence number.
 *
 * Keys:
 * - ProposalKey{epoch, slot, view}: one accepted proposal per key.
 * - VoteKey{domain, epoch, slot, view, phase}: maps to exactly one digest
 *   forever (equivocation rule, section 7.2). A new view is a new key, so a
 *   re-vote after a view change is allowed.
 * VotePhase is an append-only registry: PREPARE=1 (M4-AGR-02), COMMIT=2
 * (the slot header's CommitDigest, the only phase the outline signs).
 *
 * References (what a Pending and a Durable token are bound to):
 *   ACCEPTED_PROPOSAL  ProposalRef{key, header_hash}
 *   INTENT             VoteRef{key, digest}
 *   SIGNED_VOTE        SignedVoteRef{key, digest, signature}
 *   DECISION           DecisionRef{slot, header_hash}
 *   APPLIED            AppliedRef{slot, header_hash, state_root}
 * SlotOf and DigestOf give the slot and the digest (a vote's digest, or the
 * header hash of a decision) of any reference.
 *
 * Header-only and deterministic.
 */
namespace flowmesh2::storage {

class SignerLog;
class CommitLog;

//! Vote phases. APPEND-ONLY.
enum class VotePhase : uint8_t {
    PREPARE = 1, //!< PBFT prepare (M4-AGR-02); its digest is not defined in the outline
    COMMIT = 2,  //!< signs SlotHeaderV2::CommitDigest()
};

constexpr bool IsKnown(VotePhase phase) { return phase == VotePhase::PREPARE || phase == VotePhase::COMMIT; }

//! The 96-byte compressed BLS signature of a vote.
using VoteSignature = std::array<unsigned char, bls::SIGNATURE_SIZE>;

struct ProposalKey {
    uint64_t epoch{0};
    uint64_t slot{0};
    uint64_t view{0};

    friend bool operator==(const ProposalKey&, const ProposalKey&) = default;
    friend auto operator<=>(const ProposalKey&, const ProposalKey&) = default;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << epoch << slot << view;
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> epoch >> slot >> view;
    }
};

//! Encoded as domain (32) | u64 epoch | u64 slot | u64 view | u8 phase: 57 bytes.
struct VoteKey {
    uint256 domain;
    uint64_t epoch{0};
    uint64_t slot{0};
    uint64_t view{0};
    VotePhase phase{VotePhase::COMMIT};

    ProposalKey Proposal() const { return ProposalKey{epoch, slot, view}; }

    friend bool operator==(const VoteKey&, const VoteKey&) = default;
    friend bool operator<(const VoteKey& a, const VoteKey& b)
    {
        return std::tie(a.domain, a.epoch, a.slot, a.view, a.phase) < std::tie(b.domain, b.epoch, b.slot, b.view, b.phase);
    }

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << domain << epoch << slot << view << static_cast<uint8_t>(phase);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint8_t phase_byte{0};
        s >> domain >> epoch >> slot >> view >> phase_byte;
        phase = static_cast<VotePhase>(phase_byte);
        if (!IsKnown(phase)) throw std::ios_base::failure("unknown vote phase");
    }
};

inline constexpr size_t VOTE_KEY_BYTES{57};

struct ProposalRef {
    ProposalKey key;
    uint256 header_hash;
    friend bool operator==(const ProposalRef&, const ProposalRef&) = default;
};
struct VoteRef {
    VoteKey key;
    uint256 digest;
    friend bool operator==(const VoteRef&, const VoteRef&) = default;
};
struct SignedVoteRef {
    VoteKey key;
    uint256 digest;
    VoteSignature signature{};
    friend bool operator==(const SignedVoteRef&, const SignedVoteRef&) = default;
};
struct DecisionRef {
    uint64_t slot{0};
    uint256 header_hash;
    friend bool operator==(const DecisionRef&, const DecisionRef&) = default;
};
struct AppliedRef {
    uint64_t slot{0};
    uint256 header_hash;
    uint256 state_root;
    friend bool operator==(const AppliedRef&, const AppliedRef&) = default;
};

constexpr uint64_t SlotOf(const ProposalRef& r) { return r.key.slot; }
constexpr uint64_t SlotOf(const VoteRef& r) { return r.key.slot; }
constexpr uint64_t SlotOf(const SignedVoteRef& r) { return r.key.slot; }
constexpr uint64_t SlotOf(const DecisionRef& r) { return r.slot; }
constexpr uint64_t SlotOf(const AppliedRef& r) { return r.slot; }
constexpr const uint256& DigestOf(const ProposalRef& r) { return r.header_hash; }
constexpr const uint256& DigestOf(const VoteRef& r) { return r.digest; }
constexpr const uint256& DigestOf(const SignedVoteRef& r) { return r.digest; }
constexpr const uint256& DigestOf(const DecisionRef& r) { return r.header_hash; }
constexpr const uint256& DigestOf(const AppliedRef& r) { return r.header_hash; }

//! The reference type of each record type that has a Pending.
template <RecordType T>
struct RecordRefOf; // only the five specialisations below exist
template <>
struct RecordRefOf<RecordType::ACCEPTED_PROPOSAL> {
    using type = ProposalRef;
};
template <>
struct RecordRefOf<RecordType::INTENT> {
    using type = VoteRef;
};
template <>
struct RecordRefOf<RecordType::SIGNED_VOTE> {
    using type = SignedVoteRef;
};
template <>
struct RecordRefOf<RecordType::DECISION> {
    using type = DecisionRef;
};
template <>
struct RecordRefOf<RecordType::APPLIED> {
    using type = AppliedRef;
};

//! A record appended to a journal but not yet known to be durable.
template <RecordType T>
class Pending
{
public:
    using Ref = typename RecordRefOf<T>::type;
    static constexpr RecordType TYPE{T};

    Pending() = delete;

    uint64_t JournalInstance() const { return m_journal_instance; }
    uint64_t Seq() const { return m_seq; }
    const Ref& Key() const { return m_key; }

    friend bool operator==(const Pending&, const Pending&) = default;

private:
    friend class SignerLog;
    friend class CommitLog;
    Pending(uint64_t journal_instance, uint64_t seq, const Ref& key) : m_journal_instance{journal_instance}, m_seq{seq}, m_key{key} {}

    uint64_t m_journal_instance;
    uint64_t m_seq;
    Ref m_key;
};

using PendingProposal = Pending<RecordType::ACCEPTED_PROPOSAL>;
using PendingIntent = Pending<RecordType::INTENT>;
using PendingSignedVote = Pending<RecordType::SIGNED_VOTE>;
using PendingDecision = Pending<RecordType::DECISION>;
using PendingApplied = Pending<RecordType::APPLIED>;

} // namespace flowmesh2::storage

#endif // B3_FLOWMESH2_STORAGE_TOKENS_H
