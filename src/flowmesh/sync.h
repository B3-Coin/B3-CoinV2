// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_FLOWMESH_SYNC_H
#define B3COIN_FLOWMESH_SYNC_H

#include <flowmesh/auth.h>
#include <flowmesh/batch.h>
#include <flowmesh/certificate.h>
#include <flowmesh/deposit.h>
#include <flowmesh/microblock.h>
#include <flowmesh/pool.h>
#include <flowmesh/recovery.h>
#include <flowmesh/state.h>
#include <key.h>
#include <pubkey.h>
#include <serialize.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace flowmesh {

/**
 * FlowMesh node orchestration: propose -> independently re-execute ->
 * attest -> certify -> durably persist -> commit, plus catch-up for
 * nodes that fall behind. Transport-agnostic: every handler consumes one
 * typed message and returns what should be sent.
 *
 * SAFETY ORDERING (non-negotiable):
 *  - a lock is durably journaled BEFORE its attestation leaves the node
 *    (journal failure => do not sign);
 *  - a certified entry is durably persisted BEFORE it becomes the live
 *    authoritative state (persist failure => FAIL-STOP: the node halts
 *    FlowMesh participation; the durable tip never trails the live tip);
 *  - an anchor relied on by committed history that stops being canonical
 *    on B3 halts unsafe FlowMesh progression (deep-reorg treatment
 *    beyond halting is an OWNER DECISION).
 * None of this touches B3: FlowMesh halting is always local.
 *
 * Messages tolerate duplicates, reordering, staleness and garbage: a
 * handler either acts once or deterministically refuses; nothing throws
 * on untrusted content and hard bounds cap all buffered work.
 */

//! Proposal ENVELOPE: authorship and recovery metadata around a
//! proposer-free candidate. The candidate hash covers execution content
//! only, so a replacement proposer in a later round can carry an
//! existing locked candidate under the SAME hash; this envelope proves
//! who is proposing it in which round.
struct ProposalMsg {
    MicroblockCore mb;
    uint32_t round{0};
    XOnlyPubKey proposer;
    std::array<unsigned char, 64> proposer_sig{};

    SERIALIZE_METHODS(ProposalMsg, obj)
    {
        READWRITE(obj.mb, obj.round, obj.proposer, obj.proposer_sig);
    }
};

inline uint256 ProposalDigest(const uint256& domain, const uint64_t sequence,
                              const uint32_t round, const uint256& microblock_hash)
{
    HashWriter h;
    h << std::string{"b3/flowmesh/propose/v1"} << domain << sequence << round << microblock_hash;
    return h.GetHash();
}

inline bool SignProposal(const CKey& key, const uint256& domain, ProposalMsg& msg)
{
    msg.proposer = XOnlyPubKey{key.GetPubKey()};
    return key.SignSchnorr(ProposalDigest(domain, msg.mb.sequence, msg.round, msg.mb.GetHash()),
                           msg.proposer_sig, nullptr, uint256::ZERO);
}

inline bool VerifyProposal(const ProposalMsg& msg, const uint256& domain)
{
    if (!msg.proposer.IsFullyValid()) return false;
    return msg.proposer.VerifySchnorr(
        ProposalDigest(domain, msg.mb.sequence, msg.round, msg.mb.GetHash()), msg.proposer_sig);
}

struct AttestationMsg {
    uint64_t sequence{0};
    uint256 microblock_hash;
    Attestation attestation;

    SERIALIZE_METHODS(AttestationMsg, obj)
    {
        READWRITE(obj.sequence, obj.microblock_hash, obj.attestation);
    }
};

//! One finalized log entry: the microblock plus its certificate.
struct CertifiedEntry {
    MicroblockCore mb;
    MicroblockCertificate cert;

    SERIALIZE_METHODS(CertifiedEntry, obj) { READWRITE(obj.mb, obj.cert); }
};

struct CatchupRequest {
    uint64_t from_sequence{0};

    SERIALIZE_METHODS(CatchupRequest, obj) { READWRITE(obj.from_sequence); }
};

//! Durable destination for committed entries. MUST return success only
//! after the entry is durably recorded — the node treats failure as
//! fail-stop and will NOT advance its live state past it.
class CommitSink
{
public:
    virtual ~CommitSink() = default;
    [[nodiscard]] virtual bool OnCommit(const CertifiedEntry& entry) = 0;
};

//! Read access to finalized history (serving catch-up after a restart,
//! when in-memory history is gone). Node-side: the certified log store.
class CatchupSource
{
public:
    virtual ~CatchupSource() = default;
    virtual std::optional<CertifiedEntry> EntryAt(uint64_t sequence) const = 0;
};

//! Bound on simultaneously buffered executed candidates per sequence —
//! a proposer-equivocation DoS cap, not a protocol parameter.
inline constexpr size_t MAX_CANDIDATES_PER_SEQUENCE{8};

enum class MeshHalt : uint8_t {
    NONE = 0,
    INVALID_CONFIG = 1,     // nonsensical quorum/wiring; never participated
    PERSIST_FAILED = 2,     // durable record failed; live tip must not advance
    LOCK_JOURNAL_FAILED = 3, // could not journal a lock; must not sign
    ANCHOR_INVALIDATED = 4, // committed history relies on a non-canonical B3 anchor
};

class MeshNode
{
public:
    struct Config {
        uint256 domain;
        //! Active FN seats and the certificate threshold. Seat lifecycle
        //! and the fault bound behind the threshold are OWNER DECISIONS;
        //! both arrive here as explicit inputs and are shape-validated
        //! (ValidQuorumConfig) — a nonsensical quorum halts the node.
        std::set<XOnlyPubKey> seats;
        uint64_t threshold{0};
        const ProposerSchedule* schedule{nullptr};
        const ActionAuthenticator* auth{nullptr};
        const DepositVerifier* deposits{nullptr};
        const AnchorPolicy* anchors{nullptr};
        CommitSink* sink{nullptr};
        //! Serves catch-up beyond in-memory history (e.g. after restart).
        const CatchupSource* history{nullptr};
        //! Present only on validator seats; observers validate and track
        //! but never attest. A seat REQUIRES a lock journal: without
        //! durable lock state a restart could sign conflicting
        //! candidates, so a journal-less seat config is invalid.
        std::optional<CKey> seat_key;
        LockJournal* lock_journal{nullptr};
    };

    //! Restart restore: `genesis` reconstructed via the store (or a
    //! fresh genesis state), `last_hash` the certified tip hash, and
    //! `restored_locks` the journaled safety-critical locks.
    MeshNode(Config config, FlowMeshState genesis, const uint256& last_hash = {},
             const std::map<uint64_t, uint256>& restored_locks = {})
        : m_config{std::move(config)}, m_state{std::move(genesis)}, m_last_hash{last_hash}
    {
        m_guard.ImportLocks(restored_locks);
        if (!ValidQuorumConfig(m_config.seats.size(), m_config.threshold) ||
            m_config.schedule == nullptr || m_config.auth == nullptr ||
            m_config.anchors == nullptr ||
            (m_config.seat_key.has_value() && m_config.lock_journal == nullptr)) {
            m_halt = MeshHalt::INVALID_CONFIG;
        }
    }

    const FlowMeshState& State() const { return m_state; }
    uint64_t Sequence() const { return m_state.ledger.Slot(); }
    const uint256& LastHash() const { return m_last_hash; }
    ActionPool& Pool() { return m_pool; }
    const std::vector<AttestationEquivocation>& Evidence() const { return m_evidence; }
    uint32_t CurrentRound() const { return m_guard.CurrentRound(Sequence()); }
    MeshHalt Halt() const { return m_halt; }
    bool Halted() const { return m_halt != MeshHalt::NONE; }

    bool SubmitAction(const Action& action)
    {
        if (Halted()) return false;
        return m_pool.Add(action);
    }

    //! Local liveness timeout for the current sequence: accept the next
    //! round's proposer from now on. Timing policy is deliberately the
    //! caller's (wall clocks never enter deterministic state).
    void NoteTimeout()
    {
        if (Halted()) return;
        m_guard.NoteTimeout(Sequence());
    }

    /**
     * Verify that every B3 anchor relied on by committed FlowMesh
     * history is still canonical. On violation the node halts FlowMesh
     * progression (fail-safe floor; deep-reorg recovery policy is an
     * OWNER DECISION). Call on B3 tip changes; also consulted before
     * proposing. Never touches B3 state.
     */
    bool RecheckCommittedAnchors()
    {
        if (Halted()) return !Halted();
        if (m_config.anchors == nullptr) return true;
        for (const auto& [key, seq] : m_committed_anchors) {
            const AnchorRef anchor{key.first, key.second};
            if (!m_config.anchors->StillCanonical(anchor)) {
                m_halt = MeshHalt::ANCHOR_INVALIDATED;
                return false;
            }
        }
        return true;
    }

    /**
     * If this node's seat is the scheduled proposer for the current
     * (sequence, local round), produce a signed proposal envelope. A
     * validator locked on a hash re-proposes THAT candidate (same
     * proposer-free hash) rather than building a new one — and because
     * identity excludes the proposer, any later scheduled proposer
     * holding the candidate can do the same, so recovery can continue a
     * safe locked candidate.
     */
    std::optional<ProposalMsg> TryPropose(const size_t max_actions = MAX_MICROBLOCK_ACTIONS)
    {
        if (Halted() || !m_config.seat_key) return std::nullopt;
        if (!RecheckCommittedAnchors()) return std::nullopt;
        const uint64_t seq{Sequence()};
        const uint32_t round{m_guard.CurrentRound(seq)};
        const std::optional<XOnlyPubKey> scheduled{m_config.schedule->ProposerAt(seq, round)};
        const XOnlyPubKey me{XOnlyPubKey{m_config.seat_key->GetPubKey()}};
        if (!scheduled || !(*scheduled == me)) return std::nullopt;

        std::optional<MicroblockCore> mb;
        if (const std::optional<uint256> locked{m_guard.LockedHash(seq)}) {
            const auto it{m_candidates.find(*locked)};
            if (it == m_candidates.end()) return std::nullopt; // cannot reproduce; stand down
            mb = it->second.mb;
        } else {
            FlowMeshState next{m_state};
            BatchResult result;
            mb = BuildMicroblock(m_state, m_config.domain, m_last_hash,
                                 m_config.anchors->Current(),
                                 m_pool.SelectBatch(m_state, max_actions), *m_config.auth,
                                 m_config.deposits, next, result);
            if (!mb) return std::nullopt; // fatal execution failure: propose nothing
            RememberCandidate(*mb, std::move(next), std::move(result));
        }

        ProposalMsg msg;
        msg.mb = *mb;
        msg.round = round;
        if (!SignProposal(*m_config.seat_key, m_config.domain, msg)) return std::nullopt;
        return msg;
    }

    /**
     * Validate and independently re-execute a proposal. The envelope
     * must be signed by the round's scheduled proposer; the candidate
     * must re-execute exactly. A seat whose guard permits it journals
     * its lock durably and only then attests; observers only track.
     */
    std::optional<AttestationMsg> HandleProposal(const ProposalMsg& msg)
    {
        if (Halted()) return std::nullopt;
        const uint64_t seq{Sequence()};
        if (msg.mb.sequence != seq) return std::nullopt;
        if (!msg.mb.ShapeIsValid()) return std::nullopt;
        if (!m_config.anchors->Acceptable(msg.mb.anchor)) return std::nullopt;
        // Envelope authorization: the round's scheduled proposer signed
        // this exact (candidate, round).
        const std::optional<XOnlyPubKey> scheduled{
            m_config.schedule->ProposerAt(seq, msg.round)};
        if (!scheduled || !(*scheduled == msg.proposer)) return std::nullopt;
        if (!VerifyProposal(msg, m_config.domain)) return std::nullopt;

        const uint256 hash{msg.mb.GetHash()};
        if (m_candidates.size() >= MAX_CANDIDATES_PER_SEQUENCE &&
            m_candidates.count(hash) == 0) {
            return std::nullopt;
        }
        if (m_candidates.count(hash) == 0) {
            FlowMeshState next{m_state};
            BatchResult result;
            if (ExecuteCandidate(m_state, m_config.domain, m_last_hash, msg.mb, *m_config.auth,
                                 m_config.deposits, next, result) != CandidateError::NONE) {
                return std::nullopt;
            }
            RememberCandidate(msg.mb, std::move(next), std::move(result));
        }

        if (!m_config.seat_key) return std::nullopt; // observer
        if (m_guard.Consider(*m_config.schedule, seq, msg.round, msg.proposer, hash) !=
            AttestDecision::ATTEST) {
            return std::nullopt;
        }
        // SAFETY ORDER: journal the lock durably BEFORE signing. A
        // journal failure means we must not sign — halt participation.
        if (!m_config.lock_journal->WriteLock(seq, hash)) {
            m_halt = MeshHalt::LOCK_JOURNAL_FAILED;
            return std::nullopt;
        }
        const std::optional<Attestation> att{
            SignAttestation(*m_config.seat_key, m_config.domain, seq, hash)};
        if (!att) return std::nullopt;
        m_guard.NoteAttested(seq, hash);
        return AttestationMsg{seq, hash, *att};
    }

    /**
     * Record one attestation. When a tracked candidate reaches the
     * threshold, the certificate is assembled and the entry is durably
     * persisted, then committed live. Conflicting attestations by one
     * seat become recorded equivocation evidence.
     */
    std::optional<CertifiedEntry> HandleAttestation(const AttestationMsg& msg)
    {
        if (Halted()) return std::nullopt;
        const uint64_t seq{Sequence()};
        if (msg.sequence != seq) return std::nullopt;
        if (m_config.seats.count(msg.attestation.validator) == 0) return std::nullopt;
        if (!VerifyAttestation(msg.attestation, m_config.domain, seq, msg.microblock_hash)) {
            return std::nullopt;
        }

        for (const auto& [other_hash, atts] : m_attestations) {
            if (other_hash == msg.microblock_hash) continue;
            const auto prior{atts.find(msg.attestation.validator)};
            if (prior != atts.end()) {
                if (const auto evidence{DetectEquivocation(m_config.domain, seq, other_hash,
                                                           prior->second, msg.microblock_hash,
                                                           msg.attestation)}) {
                    m_evidence.push_back(*evidence);
                }
                return std::nullopt;
            }
        }

        auto& per_hash{m_attestations[msg.microblock_hash]};
        per_hash.emplace(msg.attestation.validator, msg.attestation);
        if (per_hash.size() < m_config.threshold) return std::nullopt;

        const auto candidate{m_candidates.find(msg.microblock_hash)};
        if (candidate == m_candidates.end()) return std::nullopt;

        std::vector<Attestation> atts;
        atts.reserve(per_hash.size());
        for (const auto& [key, att] : per_hash) atts.push_back(att);
        CertifiedEntry entry{candidate->second.mb,
                             AssembleCertificate(msg.microblock_hash, seq, std::move(atts))};
        if (!Commit(entry, candidate->second.next)) return std::nullopt;
        return entry;
    }

    /**
     * Accept an already-certified entry (broadcast or catch-up). Returns
     * true when the entry is committed or already known; false when it
     * cannot be used (ahead of us — request catch-up; invalid; or this
     * node halted). Certified entries REVALIDATE their anchors: the
     * anchor must still be canonical on this node's B3 view (depth-free
     * — old anchors are legitimately deep, but never off-chain).
     */
    bool HandleCertified(const CertifiedEntry& entry)
    {
        if (Halted()) return false;
        const uint64_t seq{Sequence()};
        if (entry.mb.sequence < seq) return true; // stale; already final locally
        if (entry.mb.sequence > seq) return false; // missing parents: catch up first
        const uint256 hash{entry.mb.GetHash()};
        if (entry.cert.microblock_hash != hash || entry.cert.sequence != seq) return false;
        if (CheckCertificate(entry.cert, m_config.domain, m_config.seats, m_config.threshold) !=
            CertificateCheck::OK) {
            return false;
        }
        if (!m_config.anchors->StillCanonical(entry.mb.anchor)) return false;
        auto candidate{m_candidates.find(hash)};
        if (candidate == m_candidates.end()) {
            FlowMeshState next{m_state};
            BatchResult result;
            if (ExecuteCandidate(m_state, m_config.domain, m_last_hash, entry.mb, *m_config.auth,
                                 m_config.deposits, next, result) != CandidateError::NONE) {
                return false;
            }
            RememberCandidate(entry.mb, std::move(next), std::move(result));
            candidate = m_candidates.find(hash);
        }
        return Commit(entry, candidate->second.next);
    }

    //! Ask for missing history when a peer references a future sequence.
    std::optional<CatchupRequest> MaybeRequestCatchup(const uint64_t observed_sequence) const
    {
        if (observed_sequence <= Sequence()) return std::nullopt;
        return CatchupRequest{Sequence()};
    }

    //! Serve finalized history from `from_sequence` (bounded): from
    //! in-memory history when present, else from the durable source.
    std::vector<CertifiedEntry> HandleCatchupRequest(const CatchupRequest& req,
                                                     const size_t max_entries = 256) const
    {
        std::vector<CertifiedEntry> out;
        for (uint64_t s{req.from_sequence}; out.size() < max_entries; ++s) {
            if (s < m_history_base + m_history.size() && s >= m_history_base) {
                out.push_back(m_history[static_cast<size_t>(s - m_history_base)]);
                continue;
            }
            if (m_config.history != nullptr) {
                if (const auto entry{m_config.history->EntryAt(s)}) {
                    out.push_back(*entry);
                    continue;
                }
            }
            break;
        }
        return out;
    }

    //! Apply a catch-up response in order; returns entries committed.
    size_t HandleCatchupResponse(const std::vector<CertifiedEntry>& entries)
    {
        size_t committed{0};
        for (const CertifiedEntry& entry : entries) {
            if (entry.mb.sequence != Sequence()) continue;
            if (!HandleCertified(entry)) break;
            ++committed;
        }
        return committed;
    }

private:
    struct Candidate {
        MicroblockCore mb;
        FlowMeshState next;
        BatchResult result;
    };

    void RememberCandidate(const MicroblockCore& mb, FlowMeshState&& next, BatchResult&& result)
    {
        m_candidates.emplace(mb.GetHash(), Candidate{mb, std::move(next), std::move(result)});
    }

    /**
     * SAFETY ORDER: durably persist FIRST; only a successfully persisted
     * entry may become live state. On persist failure the node halts
     * (fail-stop) with its live tip still at the previous certified
     * position — the durable tip never trails the live tip, so the node
     * can never keep voting on top of an unpersisted transition.
     */
    [[nodiscard]] bool Commit(const CertifiedEntry& entry, const FlowMeshState& next)
    {
        const uint64_t seq{entry.mb.sequence};
        if (m_config.sink != nullptr && !m_config.sink->OnCommit(entry)) {
            m_halt = MeshHalt::PERSIST_FAILED;
            return false;
        }
        m_state = next;
        m_last_hash = entry.mb.GetHash();
        if (m_history.empty()) m_history_base = seq;
        m_history.push_back(entry);
        m_guard.NoteCertified(seq);
        if (m_config.lock_journal != nullptr) {
            // Lock now obsolete; a failure here is storage trouble —
            // stop participating, but the committed state is durable.
            if (!m_config.lock_journal->ClearLocksThrough(seq)) {
                m_halt = MeshHalt::LOCK_JOURNAL_FAILED;
            }
        }
        if (!entry.mb.anchor.IsNull()) {
            m_committed_anchors[{entry.mb.anchor.height, entry.mb.anchor.hash}] = seq;
        }
        m_pool.PruneCommitted(m_state);
        m_candidates.clear();
        m_attestations.clear();
        return true;
    }

    Config m_config;
    FlowMeshState m_state;
    uint256 m_last_hash;
    AttestationGuard m_guard;
    ActionPool m_pool;
    MeshHalt m_halt{MeshHalt::NONE};
    //! Executed candidates and gathered attestations for the CURRENT
    //! sequence only; both reset on every commit.
    std::map<uint256, Candidate> m_candidates;
    std::map<uint256, std::map<XOnlyPubKey, Attestation>> m_attestations;
    //! Finalized history committed during THIS process lifetime (older
    //! history serves from the durable CatchupSource).
    std::vector<CertifiedEntry> m_history;
    uint64_t m_history_base{0};
    //! Every B3 anchor committed history relies on -> newest sequence.
    std::map<std::pair<int32_t, uint256>, uint64_t> m_committed_anchors;
    std::vector<AttestationEquivocation> m_evidence;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_SYNC_H
