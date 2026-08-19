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

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace flowmesh {

/**
 * FlowMesh node orchestration: propose -> independently re-execute ->
 * attest -> certify -> commit, plus catch-up for nodes that fall
 * behind. Transport-agnostic by design: every handler consumes one
 * typed message and returns what should be sent, so the same logic is
 * driven by in-process tests today and by real P2P plumbing later.
 * Nothing here touches B3 consensus — FlowMesh stalling can never stall
 * the base chain.
 *
 * Messages tolerate duplicates, reordering, staleness and garbage: a
 * handler either acts once or deterministically refuses; nothing throws
 * on untrusted content and hard bounds cap all buffered work.
 */

struct ProposalMsg {
    MicroblockCore mb;
    //! Recovery round this proposal claims (recovery.h); metadata, not
    //! part of the microblock identity.
    uint32_t round{0};

    SERIALIZE_METHODS(ProposalMsg, obj) { READWRITE(obj.mb, obj.round); }
};

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

//! Where committed entries go (persistence lives node-side; the sink
//! keeps this layer free of database dependencies).
class CommitSink
{
public:
    virtual ~CommitSink() = default;
    virtual void OnCommit(const CertifiedEntry& entry) = 0;
};

//! Bound on simultaneously buffered executed candidates per sequence —
//! a proposer-equivocation DoS cap, not a protocol parameter.
inline constexpr size_t MAX_CANDIDATES_PER_SEQUENCE{8};

class MeshNode
{
public:
    struct Config {
        uint256 domain;
        //! Active FN seats and the certificate threshold. Seat lifecycle
        //! and the fault bound behind the threshold are OWNER DECISIONS;
        //! both arrive here as explicit inputs.
        std::set<XOnlyPubKey> seats;
        uint64_t threshold{0};
        const ProposerSchedule* schedule{nullptr};
        const ActionAuthenticator* auth{nullptr};
        const DepositVerifier* deposits{nullptr};
        const AnchorPolicy* anchors{nullptr};
        CommitSink* sink{nullptr};
        //! Present only on validator seats; observers validate and track
        //! but never attest.
        std::optional<CKey> seat_key;
    };

    MeshNode(Config config, FlowMeshState genesis)
        : m_config{std::move(config)}, m_state{std::move(genesis)}
    {
    }

    const FlowMeshState& State() const { return m_state; }
    uint64_t Sequence() const { return m_state.ledger.Slot(); }
    const uint256& LastHash() const { return m_last_hash; }
    ActionPool& Pool() { return m_pool; }
    const std::vector<AttestationEquivocation>& Evidence() const { return m_evidence; }
    uint32_t CurrentRound() const { return m_guard.CurrentRound(Sequence()); }

    bool SubmitAction(const Action& action) { return m_pool.Add(action); }

    //! Local liveness timeout for the current sequence: accept the next
    //! round's proposer from now on. Timing policy is deliberately the
    //! caller's (wall clocks never enter deterministic state).
    void NoteTimeout() { m_guard.NoteTimeout(Sequence()); }

    //! If this node's seat is the scheduled proposer for the current
    //! (sequence, local round), build a proposal from the pool.
    std::optional<ProposalMsg> TryPropose(const size_t max_actions = MAX_MICROBLOCK_ACTIONS)
    {
        if (!m_config.seat_key || m_config.schedule == nullptr ||
            m_config.anchors == nullptr || m_config.auth == nullptr) {
            return std::nullopt;
        }
        const uint64_t seq{Sequence()};
        const uint32_t round{m_guard.CurrentRound(seq)};
        const std::optional<XOnlyPubKey> scheduled{m_config.schedule->ProposerAt(seq, round)};
        const XOnlyPubKey me{XOnlyPubKey{m_config.seat_key->GetPubKey()}};
        if (!scheduled || !(*scheduled == me)) return std::nullopt;

        // If this validator is locked on a hash for this sequence (an
        // earlier round attested but failed to certify), re-propose that
        // exact microblock rather than building a new one.
        if (const std::optional<uint256> locked{m_guard.LockedHash(seq)}) {
            const auto it{m_candidates.find(*locked)};
            if (it != m_candidates.end()) return ProposalMsg{it->second.mb, round};
        }

        FlowMeshState next{m_state};
        BatchResult result;
        const MicroblockCore mb{BuildMicroblock(
            m_state, m_config.domain, m_last_hash, m_config.anchors->Current(),
            m_pool.SelectBatch(m_state, max_actions), me, *m_config.auth, m_config.deposits,
            next, result)};
        RememberCandidate(mb, std::move(next), std::move(result));
        return ProposalMsg{mb, round};
    }

    /**
     * Validate and independently re-execute a proposal. A seat whose
     * guard permits it attests; observers only track the candidate.
     * Refusals are silent and stateless — duplicates and garbage cost
     * one bounded validation.
     */
    std::optional<AttestationMsg> HandleProposal(const ProposalMsg& msg)
    {
        if (m_config.schedule == nullptr || m_config.anchors == nullptr ||
            m_config.auth == nullptr) {
            return std::nullopt;
        }
        const uint64_t seq{Sequence()};
        if (msg.mb.sequence != seq) return std::nullopt;
        if (!msg.mb.ShapeIsValid()) return std::nullopt;
        if (!m_config.anchors->Acceptable(msg.mb.anchor)) return std::nullopt;
        if (m_candidates.size() >= MAX_CANDIDATES_PER_SEQUENCE &&
            m_candidates.count(msg.mb.GetHash()) == 0) {
            return std::nullopt;
        }

        const uint256 hash{msg.mb.GetHash()};
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
        if (m_guard.Consider(*m_config.schedule, seq, msg.round, msg.mb.proposer, hash) !=
            AttestDecision::ATTEST) {
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
     * threshold, the certificate is assembled, the state commits, and
     * the finalized entry is returned for broadcast. Conflicting
     * attestations by one seat become recorded equivocation evidence.
     */
    std::optional<CertifiedEntry> HandleAttestation(const AttestationMsg& msg)
    {
        const uint64_t seq{Sequence()};
        if (msg.sequence != seq) return std::nullopt;
        if (m_config.seats.count(msg.attestation.validator) == 0) return std::nullopt;
        if (!VerifyAttestation(msg.attestation, m_config.domain, seq, msg.microblock_hash)) {
            return std::nullopt;
        }

        // Cross-hash equivocation by one seat: record evidence, keep the
        // first attestation, ignore the later one.
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
        Commit(entry, candidate->second.next);
        return entry;
    }

    /**
     * Accept an already-certified entry (broadcast or catch-up). Returns
     * true when the entry is committed or already known; false when it
     * cannot be used yet (ahead of us — request catch-up) or is invalid.
     */
    bool HandleCertified(const CertifiedEntry& entry)
    {
        const uint64_t seq{Sequence()};
        if (entry.mb.sequence < seq) return true; // stale; already final locally
        if (entry.mb.sequence > seq) return false; // missing parents: catch up first
        if (m_config.auth == nullptr) return false;
        const uint256 hash{entry.mb.GetHash()};
        if (entry.cert.microblock_hash != hash || entry.cert.sequence != seq) return false;
        if (CheckCertificate(entry.cert, m_config.domain, m_config.seats, m_config.threshold) !=
            CertificateCheck::OK) {
            return false;
        }
        // A certified entry's anchor was accepted by the certifying
        // quorum; a lagging node re-checks execution, not local anchor
        // preference (its own B3 view may trail).
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
        Commit(entry, candidate->second.next);
        return true;
    }

    //! Ask for missing history when a peer references a future sequence.
    std::optional<CatchupRequest> MaybeRequestCatchup(const uint64_t observed_sequence) const
    {
        if (observed_sequence <= Sequence()) return std::nullopt;
        return CatchupRequest{Sequence()};
    }

    //! Serve finalized history from `from_sequence` (bounded).
    std::vector<CertifiedEntry> HandleCatchupRequest(const CatchupRequest& req,
                                                     const size_t max_entries = 256) const
    {
        std::vector<CertifiedEntry> out;
        for (uint64_t s{req.from_sequence}; s < m_history.size() && out.size() < max_entries;
             ++s) {
            out.push_back(m_history[static_cast<size_t>(s)]);
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

    void Commit(const CertifiedEntry& entry, const FlowMeshState& next)
    {
        const uint64_t seq{entry.mb.sequence};
        m_state = next;
        m_last_hash = entry.mb.GetHash();
        m_history.push_back(entry);
        m_guard.NoteCertified(seq);
        m_pool.PruneCommitted(m_state);
        m_candidates.clear();
        m_attestations.clear();
        if (m_config.sink != nullptr) m_config.sink->OnCommit(entry);
    }

    Config m_config;
    FlowMeshState m_state;
    uint256 m_last_hash;
    AttestationGuard m_guard;
    ActionPool m_pool;
    //! Executed candidates and gathered attestations for the CURRENT
    //! sequence only; both reset on every commit.
    std::map<uint256, Candidate> m_candidates;
    std::map<uint256, std::map<XOnlyPubKey, Attestation>> m_attestations;
    //! Finalized history (also persisted via the CommitSink node-side).
    std::vector<CertifiedEntry> m_history;
    std::vector<AttestationEquivocation> m_evidence;
};

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_SYNC_H
