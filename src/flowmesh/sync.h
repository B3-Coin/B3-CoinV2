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
#include <memory>
#include <cstdint>
#include <ios>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace node {
class FlowMeshStore;
struct ValidatorRuntime;
} // namespace node

namespace flowmesh {

class MeshNode;
namespace detail {
struct SigningNodeFactory;
} // namespace detail
namespace test_only {
struct SigningBridge;
} // namespace test_only

/**
 * FlowMesh node orchestration: propose -> independently re-execute ->
 * attest -> certify -> durably persist -> commit, plus catch-up.
 * Transport-agnostic; nothing here touches B3.
 *
 * PROVISIONAL FINALITY MODEL — OWNER DECISION REQUIRED: this prototype
 * commits state on a valid threshold certificate. Whether a certificate
 * IS irreversible FlowMesh finality has NOT been ratified by the owner;
 * the layer stays activation-unwired and this model is an
 * implementation placeholder, not settled production law. Likewise,
 * PERMANENT SPLIT LOCKS MAY HALT FLOWMESH INDEFINITELY (recovery.h):
 * no cross-round unlock rule exists, by owner-decision boundary.
 *
 * SAFETY ORDERING (non-negotiable):
 *  - guards (round, lock, proposer, anchors) run BEFORE a proposal may
 *    consume execution work or candidate-cache capacity;
 *  - a lock is durably journaled (compare-and-set) BEFORE its
 *    attestation leaves the node; journal failure or conflict => do
 *    not sign;
 *  - required B3 anchors are revalidated immediately before SIGNING and
 *    immediately before COMMIT;
 *  - a certified entry is durably persisted BEFORE it becomes the live
 *    authoritative state (persist failure => fail-stop);
 *  - a SIGNING validator requires full durable state (sink + lock
 *    journal); observers may run without them but cannot attest.
 *
 * THREADING: MeshNode is NOT thread-safe — all handlers run in one
 * execution context per node (the eventual network wiring owns the
 * serialization). The durable store serializes its own writers.
 */

//! Bounded evidence codec shared by proposals and certified entries:
//! one credential per signed body action, counts and sizes checked
//! before allocation.
template <typename Stream>
void SerializeEvidence(Stream& s, const std::vector<std::vector<unsigned char>>& credentials)
{
    WriteCompactSize(s, credentials.size());
    for (const std::vector<unsigned char>& credential : credentials) {
        WriteCompactSize(s, credential.size());
        if (!credential.empty()) {
            s.write(std::as_bytes(std::span{credential.data(), credential.size()}));
        }
    }
}
template <typename Stream>
void UnserializeEvidence(Stream& s, std::vector<std::vector<unsigned char>>& credentials)
{
    const uint64_t n{ReadCompactSize(s)};
    if (n > MAX_MICROBLOCK_ACTIONS) {
        throw std::ios_base::failure("flowmesh evidence list too large");
    }
    credentials.clear();
    credentials.reserve(n);
    for (uint64_t i{0}; i < n; ++i) {
        const uint64_t len{ReadCompactSize(s)};
        if (len > MAX_ACTION_CREDENTIAL_SIZE) {
            throw std::ios_base::failure("flowmesh evidence credential too large");
        }
        std::vector<unsigned char> credential(len);
        if (len > 0) {
            s.read(std::as_writable_bytes(std::span{credential.data(), credential.size()}));
        }
        credentials.push_back(std::move(credential));
    }
}

//! Proposal ENVELOPE: authorship, recovery round, and authentication
//! EVIDENCE around a proposer-free, evidence-free candidate. The
//! candidate hash covers execution content only.
struct ProposalMsg {
    MicroblockCore mb;
    uint32_t round{0};
    XOnlyPubKey proposer;
    std::array<unsigned char, 64> proposer_sig{};
    //! One credential per signed body action, in body order.
    std::vector<std::vector<unsigned char>> credentials;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << mb << round << proposer << proposer_sig;
        SerializeEvidence(s, credentials);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> mb >> round >> proposer >> proposer_sig;
        UnserializeEvidence(s, credentials);
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

//! One certified log entry: the microblock, its certificate, and the
//! authentication evidence its body was admitted under (needed so
//! replay/catch-up can re-verify admission; NOT part of the microblock
//! identity the certificate signs).
struct CertifiedEntry {
    MicroblockCore mb;
    MicroblockCertificate cert;
    std::vector<std::vector<unsigned char>> credentials;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << mb << cert;
        SerializeEvidence(s, credentials);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> mb >> cert;
        UnserializeEvidence(s, credentials);
    }
};

struct CatchupRequest {
    uint64_t from_sequence{0};

    SERIALIZE_METHODS(CatchupRequest, obj) { READWRITE(obj.from_sequence); }
};

//! Durable destination for committed entries. MUST return success only
//! after the entry is durably recorded.
class CommitSink
{
public:
    virtual ~CommitSink() = default;
    [[nodiscard]] virtual bool OnCommit(const CertifiedEntry& entry) = 0;
};

//! Read access to certified history (serving catch-up after restart).
class CatchupSource
{
public:
    virtual ~CatchupSource() = default;
    virtual std::optional<CertifiedEntry> EntryAt(uint64_t sequence) const = 0;
};

//! Bound on simultaneously buffered executed candidates per sequence —
//! a DoS cap, not a protocol parameter. Only proposals that passed
//! every admission guard may consume a slot.
inline constexpr size_t MAX_CANDIDATES_PER_SEQUENCE{8};

enum class MeshHalt : uint8_t {
    NONE = 0,
    INVALID_CONFIG = 1,      // nonsensical quorum/wiring; never participated
    PERSIST_FAILED = 2,      // durable record failed; live tip must not advance
    LOCK_JOURNAL_FAILED = 3, // could not journal (or CAS-conflicted) a lock
    ANCHOR_INVALIDATED = 4,  // committed history relies on a non-canonical B3 anchor
};

class MeshNode
{
public:
    struct Config {
        uint256 domain;
        //! The immutable market/execution configuration this node runs
        //! (state.h ComputeExecutionConfigId); must match the state's
        //! own id, and the authenticator must be bound to it.
        uint256 market_config_id;
        //! Seats and threshold: shape-validated; the production fault
        //! bound f (hence threshold), schedule and timeout policy are
        //! OWNER DECISIONS supplied here explicitly.
        std::set<XOnlyPubKey> seats;
        uint64_t threshold{0};
        const ProposerSchedule* schedule{nullptr};
        const ActionAuthenticator* auth{nullptr};
        const DepositVerifier* deposits{nullptr};
        const AnchorPolicy* anchors{nullptr};
        CommitSink* sink{nullptr};
        const CatchupSource* history{nullptr};
        //! SIGNING VALIDATOR LIFECYCLE: a seat key REQUIRES both a
        //! durable commit sink and a durable lock journal — a node
        //! without full signing durability may only observe. Prefer
        //! node::StartValidator (flowmesh_store.h), which also restores
        //! tip/state/locks/anchor history; hand-built signing configs
        //! without durable state are refused here.
        std::optional<CKey> seat_key;
        LockJournal* lock_journal{nullptr};
    };

    /**
     * PUBLIC CONSTRUCTION IS OBSERVER-ONLY (Codex item 3): signing
     * capability cannot be obtained here. A config carrying a seat key
     * or lock journal is refused outright (INVALID_CONFIG, and the
     * signing material is stripped so the instance can never sign), and
     * no restore maps are accepted — a signing validator exists ONLY
     * through node::StartValidator, which restores state, tip, locks
     * and anchor dependencies from the durable store itself.
     */
    MeshNode(Config config, FlowMeshState genesis, const uint256& last_hash = {})
        : MeshNode{[&] {
                       if (config.seat_key.has_value() || config.lock_journal != nullptr) {
                           config.seat_key.reset();
                           config.lock_journal = nullptr;
                           config.market_config_id = uint256{}; // force INVALID_CONFIG
                       }
                       return std::move(config);
                   }(),
                   std::move(genesis), last_hash, {}, {}}
    {
    }

private:
    //! The FULL constructor (restore maps, signing capability): reachable
    //! only via node::StartValidator (production) or the loudly-named
    //! test bridge. Enforces the signing lifecycle and the market
    //! binding.
    MeshNode(Config config, FlowMeshState genesis, const uint256& last_hash,
             const std::map<uint64_t, uint256>& restored_locks,
             const std::map<std::pair<int32_t, uint256>, uint64_t>& restored_anchors)
        : m_config{std::move(config)}, m_state{std::move(genesis)}, m_last_hash{last_hash},
          m_pool{m_config.auth}, m_committed_anchors{restored_anchors}
    {
        m_guard.ImportLocks(restored_locks);
        if (!ValidQuorumConfig(m_config.seats.size(), m_config.threshold) ||
            m_config.schedule == nullptr || m_config.auth == nullptr ||
            m_config.anchors == nullptr ||
            m_config.market_config_id != m_state.ConfigId() ||
            // The authenticator must PROVE it judges this node's domain
            // and this state's execution configuration — a Market-B
            // authenticator inside a Market-A node is refused here
            // explicitly, never left to downstream accidents.
            m_config.auth->DomainId() != m_config.domain ||
            m_config.auth->ExecConfigId() != m_state.ConfigId() ||
            (m_config.seat_key.has_value() &&
             (m_config.lock_journal == nullptr || m_config.sink == nullptr))) {
            m_halt = MeshHalt::INVALID_CONFIG;
        }
    }

    friend struct detail::SigningNodeFactory;

public:
    const FlowMeshState& State() const { return m_state; }
    uint64_t Sequence() const { return m_state.Slot(); }
    const uint256& LastHash() const { return m_last_hash; }
    ActionPool& Pool() { return m_pool; }
    const std::vector<AttestationEquivocation>& Evidence() const { return m_evidence; }
    uint32_t CurrentRound() const { return m_guard.CurrentRound(Sequence()); }
    MeshHalt Halt() const { return m_halt; }
    bool Halted() const { return m_halt != MeshHalt::NONE; }
    const std::map<std::pair<int32_t, uint256>, uint64_t>& CommittedAnchors() const
    {
        return m_committed_anchors;
    }

    bool SubmitAction(const Action& action)
    {
        if (Halted()) return false;
        return m_pool.Add(action);
    }

    void NoteTimeout()
    {
        if (Halted()) return;
        m_guard.NoteTimeout(Sequence());
    }

    /**
     * Verify that every B3 anchor relied on by committed FlowMesh
     * history is still canonical; halt otherwise (fail-safe floor;
     * deep-reorg treatment beyond halting is an OWNER DECISION).
     * Consulted before proposing, before SIGNING, and before COMMIT.
     */
    bool RecheckCommittedAnchors()
    {
        if (Halted()) return false;
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

    std::optional<ProposalMsg> TryPropose(const size_t max_actions = MAX_MICROBLOCK_ACTIONS)
    {
        if (Halted() || !m_config.seat_key) return std::nullopt;
        if (!RecheckCommittedAnchors()) return std::nullopt;
        const uint64_t seq{Sequence()};
        const uint32_t round{m_guard.CurrentRound(seq)};
        const std::optional<XOnlyPubKey> scheduled{m_config.schedule->ProposerAt(seq, round)};
        const XOnlyPubKey me{XOnlyPubKey{m_config.seat_key->GetPubKey()}};
        if (!scheduled || !(*scheduled == me)) return std::nullopt;

        ProposalMsg msg;
        msg.round = round;
        if (const std::optional<uint256> locked{m_guard.LockedHash(seq)}) {
            const auto it{m_candidates.find(*locked)};
            if (it == m_candidates.end()) return std::nullopt; // cannot reproduce; stand down
            msg.mb = it->second.mb;
            msg.credentials = it->second.credentials;
        } else {
            FlowMeshState next{m_state};
            BatchResult result;
            std::vector<std::vector<unsigned char>> credentials;
            const std::optional<MicroblockCore> mb{BuildMicroblock(
                m_state, m_config.domain, m_last_hash, m_config.anchors->Current(),
                m_pool.SelectBatch(m_state, max_actions), m_config.deposits, next, result,
                credentials)};
            if (!mb) return std::nullopt;
            RememberCandidate(*mb, credentials, std::move(next), std::move(result));
            msg.mb = *mb;
            msg.credentials = std::move(credentials);
        }
        if (!SignProposal(*m_config.seat_key, m_config.domain, msg)) return std::nullopt;
        return msg;
    }

    /**
     * Proposal admission, in the mandated guard order:
     *   shape -> sequence -> round eligibility -> safety lock ->
     *   proposer/envelope -> B3 anchors -> evidence -> ONLY THEN
     *   execute/cache. Rejected proposals consume no candidate-cache
     *   capacity. A seat that passes every guard journals its lock
     *   (compare-and-set) durably and only then attests.
     */
    std::optional<AttestationMsg> HandleProposal(const ProposalMsg& msg)
    {
        if (Halted()) return std::nullopt;
        const uint64_t seq{Sequence()};
        if (msg.mb.sequence != seq) return std::nullopt;
        if (!msg.mb.ShapeIsValid()) return std::nullopt;

        const uint256 hash{msg.mb.GetHash()};
        // Round / lock / proposer guards BEFORE any execution or cache.
        if (m_guard.Consider(*m_config.schedule, seq, msg.round, msg.proposer, hash) !=
            AttestDecision::ATTEST) {
            return std::nullopt;
        }
        if (!VerifyProposal(msg, m_config.domain)) return std::nullopt;
        // Cheap proposal-anchor gate (pre-cache DoS guard); the FULL
        // anchor recheck runs after execution, immediately before
        // signing (Codex item 5).
        if (!m_config.anchors->Acceptable(msg.mb.anchor)) return std::nullopt;
        // Evidence: every signed body action must authenticate.
        if (!VerifyActionEvidence(msg.mb, msg.credentials, *m_config.auth)) return std::nullopt;

        if (m_candidates.count(hash) == 0) {
            if (m_candidates.size() >= MAX_CANDIDATES_PER_SEQUENCE) return std::nullopt;
            FlowMeshState next{m_state};
            BatchResult result;
            if (ExecuteCandidate(m_state, m_config.domain, m_last_hash, msg.mb,
                                 m_config.deposits, next, result) != CandidateError::NONE) {
                return std::nullopt;
            }
            RememberCandidate(msg.mb, msg.credentials, std::move(next), std::move(result));
        }

        if (!m_config.seat_key) return std::nullopt; // observer
        // IMMEDIATELY before signing (after candidate execution):
        // revalidate every required B3 anchor. The committed dependency
        // set must remain canonical, and this candidate's own anchor
        // must still satisfy the FULL acceptability policy (canonical
        // AND sufficiently buried per the owner-supplied depth) — mere
        // canonicality is not enough to sign against.
        if (!RecheckCommittedAnchors()) return std::nullopt;
        if (!m_config.anchors->Acceptable(msg.mb.anchor)) {
            if (!m_config.anchors->StillCanonical(msg.mb.anchor)) {
                m_halt = MeshHalt::ANCHOR_INVALIDATED; // orphaned: halt
            }
            return std::nullopt; // unburied: refuse to sign (retriable)
        }
        // SAFETY ORDER: durable compare-and-set lock BEFORE signing.
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
                             AssembleCertificate(msg.microblock_hash, seq, std::move(atts)),
                             candidate->second.credentials};
        if (!Commit(entry, candidate->second.next)) return std::nullopt;
        return entry;
    }

    bool HandleCertified(const CertifiedEntry& entry)
    {
        if (Halted()) return false;
        const uint64_t seq{Sequence()};
        if (entry.mb.sequence < seq) return true;  // stale; already final locally
        if (entry.mb.sequence > seq) return false; // missing parents: catch up first
        const uint256 hash{entry.mb.GetHash()};
        if (entry.cert.microblock_hash != hash || entry.cert.sequence != seq) return false;
        if (CheckCertificate(entry.cert, m_config.domain, m_config.seats, m_config.threshold) !=
            CertificateCheck::OK) {
            return false;
        }
        // Full policy on the entry's own anchor (older anchors are only
        // ever MORE buried, so legitimate certified history passes).
        if (!m_config.anchors->Acceptable(entry.mb.anchor)) return false;
        if (!VerifyActionEvidence(entry.mb, entry.credentials, *m_config.auth)) return false;
        auto candidate{m_candidates.find(hash)};
        if (candidate == m_candidates.end()) {
            FlowMeshState next{m_state};
            BatchResult result;
            if (ExecuteCandidate(m_state, m_config.domain, m_last_hash, entry.mb,
                                 m_config.deposits, next, result) != CandidateError::NONE) {
                return false;
            }
            RememberCandidate(entry.mb, entry.credentials, std::move(next), std::move(result));
            candidate = m_candidates.find(hash);
        }
        return Commit(entry, candidate->second.next);
    }

    std::optional<CatchupRequest> MaybeRequestCatchup(const uint64_t observed_sequence) const
    {
        if (observed_sequence <= Sequence()) return std::nullopt;
        return CatchupRequest{Sequence()};
    }

    //! Serving catch-up REVALIDATES anchors first: a node whose
    //! committed history relies on an orphaned anchor halts and serves
    //! nothing rather than propagating suspect history.
    std::vector<CertifiedEntry> HandleCatchupRequest(const CatchupRequest& req,
                                                     const size_t max_entries = 256)
    {
        if (!RecheckCommittedAnchors()) return {};
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
        std::vector<std::vector<unsigned char>> credentials;
        FlowMeshState next;
        BatchResult result;
    };

    void RememberCandidate(const MicroblockCore& mb,
                           const std::vector<std::vector<unsigned char>>& credentials,
                           FlowMeshState&& next, BatchResult&& result)
    {
        m_candidates.emplace(mb.GetHash(),
                             Candidate{mb, credentials, std::move(next), std::move(result)});
    }

    /**
     * SAFETY ORDER: revalidate every required B3 anchor immediately
     * before commit; durably persist FIRST; only a successfully
     * persisted entry becomes live state. Failure halts (fail-stop)
     * with the live tip unchanged.
     */
    [[nodiscard]] bool Commit(const CertifiedEntry& entry, const FlowMeshState& next)
    {
        const uint64_t seq{entry.mb.sequence};
        if (!RecheckCommittedAnchors()) return false;
        if (!m_config.anchors->Acceptable(entry.mb.anchor)) {
            if (!m_config.anchors->StillCanonical(entry.mb.anchor)) {
                m_halt = MeshHalt::ANCHOR_INVALIDATED; // orphaned: halt
            }
            return false; // unburied: refuse this commit (retriable)
        }
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
    std::map<uint256, Candidate> m_candidates;
    std::map<uint256, std::map<XOnlyPubKey, Attestation>> m_attestations;
    std::vector<CertifiedEntry> m_history;
    uint64_t m_history_base{0};
    //! Every B3 anchor committed history relies on -> newest sequence.
    //! Restored on startup (node::StartValidator) so restart cannot
    //! forget the dependencies needed to detect a later B3 reorg.
    std::map<std::pair<int32_t, uint256>, uint64_t> m_committed_anchors;
    std::vector<AttestationEquivocation> m_evidence;
};

} // namespace flowmesh

namespace node {
//! Declared here so it can be the signing factory's friend; defined in
//! node/flowmesh_store.cpp (see flowmesh_store.h for documentation).
//! Takes only the IMMUTABLE market configuration — the genesis state is
//! constructed internally as the canonical empty state, so fabricated
//! balances/custody/nonces cannot be injected at production startup.
bool StartValidator(FlowMeshStore& store, flowmesh::MeshNode::Config config,
                    const uint256& vault_commitment, const uint256& base_asset,
                    const uint256& quote_asset, size_t max_k, ValidatorRuntime& out,
                    std::string& error);
} // namespace node

namespace flowmesh {

namespace detail {
//! The ONLY gateway to the full (signing-capable, restore-accepting)
//! MeshNode constructor, itself PRIVATE: its sole callers are
//! node::StartValidator (the production lifecycle) and the loudly
//! named test bridge. No other code can reach signing construction.
struct SigningNodeFactory {
private:
    static std::unique_ptr<MeshNode> Make(
        MeshNode::Config config, FlowMeshState genesis, const uint256& last_hash,
        const std::map<uint64_t, uint256>& restored_locks,
        const std::map<std::pair<int32_t, uint256>, uint64_t>& restored_anchors)
    {
        return std::unique_ptr<MeshNode>{new MeshNode{std::move(config), std::move(genesis),
                                                      last_hash, restored_locks,
                                                      restored_anchors}};
    }

    friend bool ::node::StartValidator(::node::FlowMeshStore& store, MeshNode::Config config,
                                       const uint256& vault_commitment,
                                       const uint256& base_asset, const uint256& quote_asset,
                                       size_t max_k, ::node::ValidatorRuntime& out,
                                       std::string& error);
    friend struct ::flowmesh::test_only::SigningBridge;
};
} // namespace detail

// test_only::SigningBridge is DECLARED as the factory's second friend
// but DEFINED only under src/test/util/ — a production build contains
// no callable API that can construct a signing MeshNode outside
// node::StartValidator.

} // namespace flowmesh

#endif // B3COIN_FLOWMESH_SYNC_H
