// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_agreement.h>

#include <flowmesh/production_engine.h>
#include <streams.h>

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace node {
namespace {
using namespace flowmesh;
using Bytes = std::vector<unsigned char>;
constexpr uint32_t MAX_VIEWS{128};
constexpr size_t MAX_CANDIDATES{32};
constexpr size_t MAX_RECORDS{16384};
constexpr size_t MAX_SLOT_BYTES{128U * 1024U * 1024U};
constexpr size_t MAX_EVIDENCE_BYTES{16U * 1024U * 1024U};
constexpr uint8_t MARKER_KEY{'m'};

bool SameContext(const PreagreementContext& a, const PreagreementContext& b)
{
    return a.domain == b.domain && a.market_id == b.market_id && a.epoch == b.epoch &&
        a.seat_set_hash == b.seat_set_hash && a.sequence == b.sequence &&
        a.parent_hash == b.parent_hash && a.previous_state_root == b.previous_state_root &&
        a.execution_config_id == b.execution_config_id;
}

struct DiskContext {
    PreagreementContext value;
    SERIALIZE_METHODS(DiskContext, obj)
    {
        READWRITE(obj.value.domain, obj.value.market_id, obj.value.epoch,
                  obj.value.seat_set_hash, obj.value.sequence, obj.value.parent_hash,
                  obj.value.previous_state_root, obj.value.execution_config_id);
    }
};

struct SlotKey {
    uint8_t prefix{'s'};
    uint64_t epoch{0};
    uint64_t sequence{0};
    SERIALIZE_METHODS(SlotKey, obj)
    {
        READWRITE(obj.prefix, Using<BigEndianFormatter<8>>(obj.epoch),
                  Using<BigEndianFormatter<8>>(obj.sequence));
    }
    friend bool operator==(const SlotKey&, const SlotKey&) = default;
};

struct Marker {
    uint32_t version{1};
    uint256 identity;
    DiskContext active;
    SERIALIZE_METHODS(Marker, obj) { READWRITE(obj.version, obj.identity, obj.active); }
};

template <typename Stream> void PutBytes(Stream& stream, const Bytes& bytes)
{
    WriteCompactSize(stream, bytes.size());
    stream.write(std::as_bytes(std::span{bytes}));
}
template <typename Stream> Bytes GetBytes(Stream& stream, const size_t limit)
{
    const auto count{ReadCompactSize(stream)};
    if (count > limit || count > stream.size()) throw std::ios_base::failure("agreement record size");
    Bytes bytes(count);
    stream.read(std::as_writable_bytes(std::span{bytes}));
    return bytes;
}
template <typename Stream> size_t Count(Stream& stream, const size_t limit)
{
    const auto count{ReadCompactSize(stream)};
    if (count > limit) throw std::ios_base::failure("agreement record count");
    return count;
}

struct Candidate { Bytes entry; Bytes evidence; };
struct SignedRecord { Bytes intent; Bytes signed_bytes; };
struct Slot {
    DiskContext context;
    uint32_t view{0};
    bool changing{false};
    Bytes highest;
    std::map<uint256, Candidate> candidates;
    std::map<uint32_t, Bytes> proposals;
    std::vector<SignedRecord> records;
    Bytes decision;

    template <typename Stream> void Serialize(Stream& s) const
    {
        s << context << view << changing;
        PutBytes(s, highest);
        WriteCompactSize(s, candidates.size());
        for (const auto& [hash, candidate] : candidates) {
            s << hash; PutBytes(s, candidate.entry); PutBytes(s, candidate.evidence);
        }
        WriteCompactSize(s, proposals.size());
        for (const auto& [v, proposal] : proposals) { s << v; PutBytes(s, proposal); }
        WriteCompactSize(s, records.size());
        for (const auto& record : records) { PutBytes(s, record.intent); PutBytes(s, record.signed_bytes); }
        PutBytes(s, decision);
    }
    template <typename Stream> void Unserialize(Stream& s)
    {
        if (s.size() > MAX_SLOT_BYTES) throw std::ios_base::failure("agreement slot exceeds limit");
        s >> context >> view >> changing;
        highest = GetBytes(s, AGREEMENT_MAX_PROOF_BYTES);
        for (size_t i{0}, n{Count(s, MAX_CANDIDATES)}; i < n; ++i) {
            uint256 hash; s >> hash;
            Candidate candidate{GetBytes(s, FLOWMESH_V1_MAX_MICROBLOCK_BYTES), GetBytes(s, MAX_EVIDENCE_BYTES)};
            if (!candidates.emplace(hash, std::move(candidate)).second) throw std::ios_base::failure("duplicate agreement candidate");
        }
        for (size_t i{0}, n{Count(s, MAX_VIEWS)}; i < n; ++i) {
            uint32_t v; s >> v;
            if (!proposals.emplace(v, GetBytes(s, AGREEMENT_MAX_BYTES)).second) throw std::ios_base::failure("duplicate agreement proposal");
        }
        for (size_t i{0}, n{Count(s, MAX_RECORDS)}; i < n; ++i) {
            records.push_back({GetBytes(s, AGREEMENT_MAX_BYTES), GetBytes(s, AGREEMENT_MAX_BYTES)});
        }
        decision = GetBytes(s, AGREEMENT_MAX_BYTES);
    }
};

template <typename K, typename V> bool ReadExact(CDBWrapper& db, const K& key, V& value)
{
    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    it->Seek(key);
    K found;
    return it->Valid() && it->GetKeyExact(found) && found == key &&
        it->GetValueExact(value) && it->StatusOK();
}

bool ContextValid(const PreagreementContext& context, const ActiveFnBlsSeatSet& seats)
{
    // The digest helper also checks parent/sequence and every mandatory field.
    return PreagreementVoteDigest(context, PreagreementPhase::PREPARE, 0, context.market_id, 0) &&
        context.market_id == seats.market_id && context.epoch == seats.epoch &&
        context.seat_set_hash == seats.set_hash &&
        CheckActiveFnBlsSeatSet(context.domain, seats) == BlsSeatSetCheck::OK;
}

bool CompleteProofFits(const ActiveFnBlsSeatSet& seats)
{
    const size_t q{FlowMeshBlsThreshold(seats.Size())};
    // Complete later-view COMMIT: Q prepares, Q commits, Q reports, each
    // carrying its own Q-prepared proof. Never sample or truncate the roster.
    return seats.Size() <= FLOWMESH_MAX_ACTIVE_FN_SEATS &&
        q * q + 3 * q <= PREAGREEMENT_MAX_PROOF_SIGNATURES;
}

bool EntryMatches(const Bytes& bytes, const PreagreementContext& context, const uint256& hash)
{
    const auto entry{DecodeProductionEntry(bytes)};
    return entry && entry->GetHash() == hash && entry->domain == context.domain &&
        entry->market_id == context.market_id && entry->epoch == context.epoch &&
        entry->seat_set_hash == context.seat_set_hash && entry->sequence == context.sequence &&
        entry->parent_hash == context.parent_hash && entry->previous_state_root == context.previous_state_root;
}

using RecordKey = std::tuple<AgreementStage, uint32_t, uint32_t>;
RecordKey Key(const AgreementMessage& message) { return {message.stage, message.view, message.seat_index}; }
using VoteKey = std::pair<uint32_t, uint256>;
struct Votes {
    std::map<uint32_t, bls::Signature> prepares;
    std::map<uint32_t, bls::Signature> commits;
    std::optional<PreagreementPreparedCertificate> prepared;
    std::optional<PreagreementNewViewProof> new_view;
};

template <typename T> std::vector<IndexedBlsSignature> QuorumVotes(const T& votes, const size_t quorum)
{
    std::vector<IndexedBlsSignature> result;
    for (const auto& [seat, signature] : votes) {
        result.push_back({seat, signature});
        if (result.size() == quorum) break;
    }
    return result;
}

} // namespace

struct FlowMeshAgreement::Impl {
    CDBWrapper db;
    FlowMeshAgreementCallbacks callbacks;
    Marker marker;
    Slot slot;
    ActiveFnBlsSeatSet seats;
    bool opened{false};
    bool halted{false};
    std::string last_error;
    std::map<VoteKey, Votes> votes;
    std::map<uint32_t, std::map<uint32_t, PreagreementViewChange>> reports;
    std::map<uint32_t, AgreementMessage> pending_proposals;
    std::optional<AgreementMessage> pending_decision;
    std::map<RecordKey, size_t> record_index;
    std::set<RecordKey> received;
    size_t received_bytes{0};
    size_t retry_cursor{0};
    uint32_t proposal_retry_cursor{0};
    std::set<uint256> usable;
    std::optional<uint256> preferred;
    std::optional<uint256> required;

    Impl(DBParams params, FlowMeshAgreementCallbacks c) : db(params), callbacks(std::move(c)) {}
    const PreagreementContext& Context() const { return slot.context.value; }
    size_t Quorum() const { return FlowMeshBlsThreshold(seats.Size()); }
    bool Stop(const std::string& why, std::string& error)
    {
        halted = true; last_error = why; error = why; return false;
    }
    bool Ready(std::string& error)
    {
        if (!opened || halted) { error = halted ? last_error : "agreement journal is not open"; return false; }
        error.clear(); return true;
    }
    void Crash(const FlowMeshAgreementCrashPoint point) { if (callbacks.crash) callbacks.crash(point); }
    void Persist()
    {
        if (GetSerializeSize(slot) > MAX_SLOT_BYTES) throw std::runtime_error("agreement durable slot resource limit");
        CDBBatch batch{db};
        batch.Write(MARKER_KEY, marker);
        batch.Write(SlotKey{'s', Context().epoch, Context().sequence}, slot);
        db.WriteBatch(batch, true);
    }
    std::optional<PreagreementPreparedCertificate> Highest() const
    {
        return slot.highest.empty() ? std::nullopt : DecodePreagreementPrepared(slot.highest);
    }
    bool IsDecided() const { return !slot.decision.empty(); }
    std::map<uint32_t, bls::SecretKey> Keys() const
    {
        std::map<uint32_t, bls::SecretKey> result;
        if (!callbacks.local_keys) return result;
        for (const auto& key : callbacks.local_keys()) {
            const auto pub{key.GetPublicKey()};
            for (uint32_t i{0}; i < seats.Size(); ++i) {
                if (pub == seats.members[i].key.Key()) { result.emplace(i, key); break; }
            }
        }
        return result;
    }
    bool ValidMessage(const AgreementMessage& message, const ActiveFnBlsSeatSet& authority,
                      const PreagreementContext& context, const bool unsigned_intent = false) const
    {
        if (!SameContext(message.context, context) || message.view >= MAX_VIEWS ||
            !EncodeAgreementMessage(message)) return false;
        if (message.stage != AgreementStage::DECISION && message.seat_index >= authority.Size()) return false;
        if (message.stage != AgreementStage::DECISION && !unsigned_intent &&
            !CheckAgreementMessageSignature(message, authority)) return false;
        if (message.stage == AgreementStage::PROPOSAL) {
            if (ProductionProposerSeatIndex(context.sequence, message.view, authority.Size()) != message.seat_index ||
                !EntryMatches(message.entry_bytes, context, message.candidate)) return false;
            if (message.view && (!message.new_view ||
                CheckPreagreementNewView(context, authority, *message.new_view) != PreagreementCheck::OK)) return false;
        }
        if (message.prepared && CheckPreagreementPrepared(context, authority, *message.prepared) != PreagreementCheck::OK) return false;
        if (message.new_view && CheckPreagreementNewView(context, authority, *message.new_view) != PreagreementCheck::OK) return false;
        if (message.stage == AgreementStage::DECISION && (!message.decision ||
            !EntryMatches(message.entry_bytes, context, message.candidate) ||
            CheckPreagreementCommit(context, authority, *message.decision) != PreagreementCheck::OK)) return false;
        return true;
    }
    bool ValidateSlot(const Slot& candidate, const ActiveFnBlsSeatSet& authority, std::string& error) const
    {
        const auto& context{candidate.context.value};
        if (!ContextValid(context, authority) || candidate.view >= MAX_VIEWS) { error = "invalid agreement context/view"; return false; }
        if (!candidate.highest.empty()) {
            const auto highest{DecodePreagreementPrepared(candidate.highest)};
            if (!highest || highest->view > candidate.view ||
                CheckPreagreementPrepared(context, authority, *highest) != PreagreementCheck::OK) {
                error = "invalid durable prepared certificate"; return false;
            }
        }
        for (const auto& [hash, item] : candidate.candidates) {
            if (!EntryMatches(item.entry, context, hash)) { error = "invalid durable candidate"; return false; }
        }
        for (const auto& [view, bytes] : candidate.proposals) {
            const auto message{DecodeAgreementMessage(bytes)};
            if (!message || message->stage != AgreementStage::PROPOSAL || view != message->view ||
                view > candidate.view || !ValidMessage(*message, authority, context) ||
                !candidate.candidates.contains(message->candidate)) {
                error = "invalid durable proposal"; return false;
            }
        }
        std::set<RecordKey> unique;
        uint32_t abandoned_view{0};
        std::optional<PreagreementPreparedCertificate> prior_commit;
        for (const auto& record : candidate.records) {
            auto intent{DecodeAgreementMessage(record.intent)};
            if (!intent || intent->stage == AgreementStage::DECISION || intent->view > candidate.view ||
                intent->signature != std::array<unsigned char, bls::SIGNATURE_SIZE>{} ||
                !ValidMessage(*intent, authority, context, true) || !unique.insert(Key(*intent)).second) {
                error = "invalid or conflicting agreement signing intent"; return false;
            }
            if (intent->stage == AgreementStage::VIEW_CHANGE) {
                if (intent->view < abandoned_view || (prior_commit &&
                    (!intent->prepared || intent->prepared->view < prior_commit->view))) {
                    error = "durable view change conceals earlier local preparation"; return false;
                }
                abandoned_view = intent->view;
            } else if (intent->view < abandoned_view) {
                error = "durable vote created after abandoning its view"; return false;
            }
            if (intent->stage == AgreementStage::PREPARE || intent->stage == AgreementStage::COMMIT) {
                const auto proposal{candidate.proposals.find(intent->view)};
                const auto decoded{proposal == candidate.proposals.end() ? std::nullopt : DecodeAgreementMessage(proposal->second)};
                if (!decoded || decoded->candidate != intent->candidate) { error = "vote without durable justified proposal"; return false; }
            }
            if (intent->stage == AgreementStage::COMMIT) {
                if (!intent->prepared) { error = "commit intent without prepared proof"; return false; }
                prior_commit = intent->prepared;
            }
            if (!record.signed_bytes.empty()) {
                auto signed_message{DecodeAgreementMessage(record.signed_bytes)};
                if (!signed_message || !ValidMessage(*signed_message, authority, context)) {
                    error = "invalid durable signature"; return false;
                }
                signed_message->signature.fill(0);
                if (EncodeAgreementMessage(*signed_message) != std::optional<Bytes>{record.intent}) {
                    error = "signed agreement bytes differ from durable intent"; return false;
                }
            } else if (intent->view < candidate.view && intent->stage != AgreementStage::VIEW_CHANGE) {
                error = "incomplete old-view intent after view advancement"; return false;
            }
        }
        if (!candidate.decision.empty()) {
            const auto decision{DecodeAgreementMessage(candidate.decision)};
            if (!decision || decision->stage != AgreementStage::DECISION ||
                !ValidMessage(*decision, authority, context) || !candidate.candidates.contains(decision->candidate)) {
                error = "invalid durable decision"; return false;
            }
        }
        return true;
    }
    void ClearMemory()
    {
        votes.clear(); reports.clear(); pending_proposals.clear(); pending_decision.reset();
        record_index.clear(); received.clear(); usable.clear(); preferred.reset(); required.reset();
        received_bytes = 0; retry_cursor = 0; proposal_retry_cursor = 0;
    }
    void Track(const AgreementMessage& message)
    {
        if (!received.insert(Key(message)).second) {
            // COMMIT signs the vote, not its optional proof attachments. A
            // relay may deliver the same share first without its preparation
            // or new-view proof. Retain newly verified evidence, without
            // counting the seat again or replacing its first candidate.
            if (message.stage != AgreementStage::COMMIT) return;
            const auto found{votes.find({message.view, message.candidate})};
            if (found == votes.end() || !found->second.commits.contains(message.seat_index)) return;
            auto& bucket{found->second};
            const bool prepared{message.prepared && !bucket.prepared};
            const bool new_view{message.new_view && !bucket.new_view};
            if (!prepared && !new_view) return;
            // Conservatively charge the complete enriched message. Each
            // bounded bucket can acquire each attachment at most once.
            const size_t bytes{EncodeAgreementMessage(message)->size()};
            if (bytes > MAX_SLOT_BYTES - received_bytes) throw std::runtime_error("agreement tracked proof resource limit");
            received_bytes += bytes;
            if (prepared) bucket.prepared = message.prepared;
            if (new_view) bucket.new_view = message.new_view;
            return;
        }
        received_bytes += EncodeAgreementMessage(message)->size();
        if (received.size() > MAX_RECORDS || received_bytes > MAX_SLOT_BYTES) throw std::runtime_error("agreement tracked message resource limit");
        if (message.stage == AgreementStage::VIEW_CHANGE) {
            reports[message.view].emplace(message.seat_index,
                PreagreementViewChange{message.view, message.seat_index, message.prepared, message.signature});
            return;
        }
        if (message.stage == AgreementStage::PROPOSAL) {
            pending_proposals.try_emplace(message.view, message);
            if (message.new_view) votes[{message.view, message.candidate}].new_view = message.new_view;
            return;
        }
        if (message.stage != AgreementStage::PREPARE && message.stage != AgreementStage::COMMIT) return;
        const auto signature{bls::Signature::Decode(message.signature)};
        if (!signature) throw std::runtime_error("invalid locally durable BLS signature");
        auto& bucket{votes[{message.view, message.candidate}]};
        (message.stage == AgreementStage::PREPARE ? bucket.prepares : bucket.commits).emplace(message.seat_index, *signature);
        if (message.prepared) bucket.prepared = message.prepared;
        if (message.new_view) bucket.new_view = message.new_view;
    }
    void RestoreMemory()
    {
        ClearMemory();
        for (const auto& [view, bytes] : slot.proposals) Track(*DecodeAgreementMessage(bytes));
        if (const auto highest{Highest()}) votes[{highest->view, highest->candidate}].prepared = highest;
        for (size_t i{0}; i < slot.records.size(); ++i) {
            const auto intent{DecodeAgreementMessage(slot.records[i].intent)};
            record_index.emplace(Key(*intent), i);
            if (!slot.records[i].signed_bytes.empty()) Track(*DecodeAgreementMessage(slot.records[i].signed_bytes));
        }
    }
    bool CandidateReady(const uint256& hash)
    {
        const auto found{slot.candidates.find(hash)};
        if (found == slot.candidates.end()) { required = hash; return false; }
        if (usable.contains(hash)) return true;
        if (!callbacks.validate_candidate) return false;
        const auto restored{callbacks.validate_candidate(found->second.entry, std::span<const unsigned char>{found->second.evidence})};
        if (!restored) { required = hash; return false; }
        if (restored->size() > MAX_EVIDENCE_BYTES) throw std::runtime_error("agreement evidence resource limit");
        if (*restored != found->second.evidence) {
            found->second.evidence = *restored;
            Persist();
        }
        usable.insert(hash);
        if (required == hash) required.reset();
        return true;
    }
    bool RememberCandidate(const Bytes& bytes, const uint256& hash)
    {
        if (!EntryMatches(bytes, Context(), hash)) return false;
        if (slot.candidates.contains(hash)) return CandidateReady(hash);
        if (!callbacks.validate_candidate) return false;
        const auto evidence{callbacks.validate_candidate(bytes, std::nullopt)};
        if (!evidence) { required = hash; return false; }
        if (slot.candidates.size() >= MAX_CANDIDATES || evidence->size() > MAX_EVIDENCE_BYTES) {
            throw std::runtime_error("agreement candidate/evidence resource limit");
        }
        slot.candidates.emplace(hash, Candidate{bytes, *evidence});
        Persist();
        usable.insert(hash);
        if (!preferred) preferred = hash;
        if (required == hash) required.reset();
        return true;
    }
    bool RememberProposalCandidate(const AgreementMessage& proposal)
    {
        if (!RememberCandidate(proposal.entry_bytes, proposal.candidate)) return false;
        // An older authenticated proposal remains the exact fetch response for
        // a highest-prepared candidate. Retain it for restart/rebroadcast once
        // its evidence becomes usable, without reopening that view's votes.
        if (proposal.view < slot.view && !slot.proposals.contains(proposal.view)) {
            slot.proposals.emplace(proposal.view, *EncodeAgreementMessage(proposal));
            Persist();
        }
        return true;
    }
    void RetryOldProposalCandidates()
    {
        // A complete decision is recovered first by Pump. An unrelated old
        // body must not consume the caller's final candidate-cache position
        // while that decision is waiting for its evidence.
        if (IsDecided() || pending_decision) return;
        // Exact duplicate ingress is intentionally coalesced. Evidence/anchor
        // recovery therefore uses the bounded, authenticated original bodies.
        // Prefer the hash needed for progress, and also rotate through other
        // old bodies so replacing `required` cannot strand one indefinitely.
        // At most two extra candidate validations occur per local Retry, not
        // per received vote. No candidate, signature or proof is reconstructed.
        std::optional<uint256> retried;
        if (required && !slot.candidates.contains(*required)) {
            const auto needed{*required};
            const auto proposal{std::find_if(pending_proposals.begin(), pending_proposals.end(),
                [&](const auto& item) { return item.first < slot.view && item.second.candidate == needed; })};
            if (proposal != pending_proposals.end()) {
                retried = needed;
                RememberProposalCandidate(proposal->second);
            }
        }
        auto proposal{pending_proposals.upper_bound(proposal_retry_cursor)};
        for (size_t visited{0}; visited < std::min<size_t>(pending_proposals.size(), 4); ++visited) {
            if (proposal == pending_proposals.end()) proposal = pending_proposals.begin();
            const auto& [view, message]{*proposal++};
            proposal_retry_cursor = view;
            if (view >= slot.view || slot.proposals.contains(view) || retried == message.candidate) continue;
            // Keep an unavailable preferred hash as the integration's signal;
            // a background candidate must not replace that recovery request.
            const auto needed{required};
            RememberProposalCandidate(message);
            if (needed && !slot.candidates.contains(*needed)) required = needed;
            break;
        }
    }
    bool Send(const AgreementMessage& message)
    {
        return callbacks.publish && callbacks.publish(message);
    }
    std::optional<AgreementMessage> Sign(AgreementMessage intent, const bls::SecretKey& key)
    {
        intent.signature.fill(0);
        const RecordKey record_key{Key(intent)};
        auto found{record_index.find(record_key)};
        size_t index;
        if (found == record_index.end()) {
            if (slot.records.size() >= MAX_RECORDS) throw std::runtime_error("agreement signing record resource limit");
            if (intent.view < slot.view || IsDecided()) return std::nullopt;
            const auto bytes{EncodeAgreementMessage(intent)};
            if (!bytes || !ValidMessage(intent, seats, Context(), true)) throw std::runtime_error("invalid local agreement intent");
            index = slot.records.size();
            slot.records.push_back({*bytes, {}});
            record_index.emplace(record_key, index);
            Persist(); // The exact message and all associated evidence precede signing.
            Crash(FlowMeshAgreementCrashPoint::AFTER_INTENT_PERSIST);
        } else {
            index = found->second;
            intent = *DecodeAgreementMessage(slot.records[index].intent);
            if (!slot.records[index].signed_bytes.empty()) return DecodeAgreementMessage(slot.records[index].signed_bytes);
            if (intent.view < slot.view) return std::nullopt;
        }
        const auto digest{AgreementMessageDigest(intent)};
        if (!digest) throw std::runtime_error("agreement intent has no signing digest");
        intent.signature = key.Sign(std::span<const unsigned char>{digest->begin(), 32}).Compressed();
        Crash(FlowMeshAgreementCrashPoint::AFTER_SIGNATURE);
        const auto bytes{EncodeAgreementMessage(intent)};
        if (!bytes) throw std::runtime_error("signed agreement encoding failed");
        slot.records[index].signed_bytes = *bytes;
        Persist(); // No publication, including loopback, occurs before this sync.
        Crash(FlowMeshAgreementCrashPoint::AFTER_SIGNED_PERSIST);
        Track(intent);
        Send(intent);
        return intent;
    }
    bool MoveView(const uint32_t target, const bool changing)
    {
        if (target <= slot.view) return true;
        if (target >= MAX_VIEWS) throw std::runtime_error("agreement view resource limit reached");
        // Every earlier intent is completed before creating a view change.
        for (const auto& record : slot.records) {
            if (record.signed_bytes.empty()) return false;
        }
        slot.view = target;
        slot.changing = changing;
        Persist();
        return true;
    }
    void LearnPrepared(const PreagreementPreparedCertificate& prepared)
    {
        const auto highest{Highest()};
        if (highest && highest->view == prepared.view && highest->candidate != prepared.candidate) {
            throw std::runtime_error("conflicting authenticated prepared quorums");
        }
        if (!highest || prepared.view > highest->view) {
            if (prepared.view > slot.view) return;
            const auto bytes{EncodePreagreementPrepared(prepared)};
            if (!bytes) throw std::runtime_error("prepared proof resource limit");
            slot.highest = *bytes;
            Persist();
        }
    }
    bool SaveDecision(const AgreementMessage& message)
    {
        if (IsDecided()) {
            const auto existing{DecodeAgreementMessage(slot.decision)};
            if (existing->candidate != message.candidate) throw std::runtime_error("conflicting commit decisions");
            return true;
        }
        if (!RememberCandidate(message.entry_bytes, message.candidate)) {
            pending_decision = message; return false;
        }
        const auto encoded{EncodeAgreementMessage(message)};
        if (!encoded) throw std::runtime_error("decision proof resource limit");
        Crash(FlowMeshAgreementCrashPoint::BEFORE_DECISION_PERSIST);
        slot.decision = *encoded;
        Persist();
        Crash(FlowMeshAgreementCrashPoint::AFTER_DECISION_PERSIST);
        required.reset();
        Send(message);
        return true;
    }
    bool Pump()
    {
        if (IsDecided()) return true;
        const auto keys{Keys()};
        // Resume a crashed exact intent before any new signing or view move.
        const size_t existing_records{slot.records.size()};
        for (size_t i{0}; i < existing_records; ++i) {
            if (!slot.records[i].signed_bytes.empty()) continue;
            const auto intent{DecodeAgreementMessage(slot.records[i].intent)};
            if (intent->stage != AgreementStage::VIEW_CHANGE && !CandidateReady(intent->candidate)) continue;
            if (const auto key{keys.find(intent->seat_index)}; key != keys.end()) Sign(*intent, key->second);
        }
        if (pending_decision && SaveDecision(*pending_decision)) { pending_decision.reset(); return true; }

        if (slot.changing) {
            for (const auto& [seat, key] : keys) {
                AgreementMessage report;
                report.stage = AgreementStage::VIEW_CHANGE; report.context = Context();
                report.view = slot.view; report.seat_index = seat; report.prepared = Highest();
                if (report.prepared) report.candidate = report.prepared->candidate;
                Sign(report, key);
            }
        }

        // A scheduled leader may only propose a nonzero view using Q reports.
        const auto leader{callbacks.leader ? callbacks.leader(slot.view) :
            std::optional<uint32_t>{ProductionProposerSeatIndex(Context().sequence, slot.view, seats.Size())}};
        if (leader && keys.contains(*leader) && !pending_proposals.contains(slot.view)) {
            std::optional<PreagreementNewViewProof> proof;
            std::optional<uint256> selection{preferred};
            if (!selection && !slot.candidates.empty()) selection = slot.candidates.begin()->first;
            if (slot.view != 0) {
                auto found{reports.find(slot.view)};
                if (found == reports.end() || found->second.size() < Quorum()) selection.reset();
                else {
                    PreagreementNewViewProof next; next.view = slot.view;
                    std::optional<PreagreementPreparedCertificate> highest;
                    for (const auto& [seat, report] : found->second) {
                        next.reports.push_back(report);
                        if (report.prepared && (!highest || report.prepared->view > highest->view)) highest = report.prepared;
                        if (next.reports.size() == Quorum()) break;
                    }
                    if (highest) selection = highest->candidate;
                    if (selection) { next.candidate = *selection; proof = std::move(next); }
                }
            }
            if (selection && CandidateReady(*selection) &&
                (!proof || CheckPreagreementNewView(Context(), seats, *proof) == PreagreementCheck::OK)) {
                AgreementMessage proposal;
                proposal.stage = AgreementStage::PROPOSAL; proposal.context = Context();
                proposal.view = slot.view; proposal.candidate = *selection;
                proposal.seat_index = *leader; proposal.entry_bytes = slot.candidates.at(*selection).entry;
                proposal.new_view = proof;
                Sign(proposal, keys.at(*leader));
            }
        }

        if (const auto found{pending_proposals.find(slot.view)}; found != pending_proposals.end()) {
            const auto proposal{found->second};
            if (RememberCandidate(proposal.entry_bytes, proposal.candidate)) {
                if (!slot.proposals.contains(slot.view)) {
                    slot.proposals.emplace(slot.view, *EncodeAgreementMessage(proposal));
                    slot.changing = false;
                    Persist();
                }
                for (const auto& [seat, key] : keys) {
                    AgreementMessage vote;
                    vote.stage = AgreementStage::PREPARE; vote.context = Context();
                    vote.view = slot.view; vote.candidate = proposal.candidate; vote.seat_index = seat;
                    Sign(vote, key);
                }
            }
        }

        // Q PREPARE is durable preparation; only Q COMMIT is a decision.
        for (auto& [position, bucket] : votes) {
            const auto& [view, hash]{position};
            if (!bucket.prepared && bucket.prepares.size() >= Quorum()) {
                bucket.prepared = PreagreementPreparedCertificate{view, hash, QuorumVotes(bucket.prepares, Quorum())};
            }
            if (!bucket.prepared) continue;
            LearnPrepared(*bucket.prepared);
            const auto proposal{slot.proposals.find(view)};
            if (view == slot.view && proposal != slot.proposals.end() && CandidateReady(hash)) {
                const auto accepted{DecodeAgreementMessage(proposal->second)};
                if (accepted->candidate == hash) {
                    for (const auto& [seat, key] : keys) {
                        AgreementMessage commit;
                        commit.stage = AgreementStage::COMMIT; commit.context = Context();
                        commit.view = view; commit.candidate = hash; commit.seat_index = seat;
                        commit.prepared = bucket.prepared; commit.new_view = accepted->new_view;
                        Sign(commit, key);
                    }
                }
            }
            if (bucket.commits.size() < Quorum() || (view && !bucket.new_view)) continue;
            PreagreementCommitCertificate certificate{*bucket.prepared, QuorumVotes(bucket.commits, Quorum()), bucket.new_view};
            if (CheckPreagreementCommit(Context(), seats, certificate) != PreagreementCheck::OK) continue;
            if (!CandidateReady(hash)) continue;
            AgreementMessage decision;
            decision.stage = AgreementStage::DECISION; decision.context = Context();
            decision.view = view; decision.candidate = hash; decision.seat_index = AGREEMENT_NO_SEAT;
            decision.entry_bytes = slot.candidates.at(hash).entry; decision.decision = std::move(certificate);
            SaveDecision(decision);
            return true;
        }
        return true;
    }
};

FlowMeshAgreement::FlowMeshAgreement(DBParams params, FlowMeshAgreementCallbacks callbacks)
    : m_impl(std::make_unique<Impl>(std::move(params), std::move(callbacks))) {}
FlowMeshAgreement::~FlowMeshAgreement() = default;

bool FlowMeshAgreement::Open(const PreagreementContext& context, const ActiveFnBlsSeatSet& seats,
                            const uint256& identity, const bool allow_bootstrap, std::string& error)
{
    auto& s{*m_impl};
    if (s.opened || s.halted) return s.Stop("agreement journal cannot be reopened in place", error);
    try {
        if (!CompleteProofFits(seats)) return s.Stop("full anchored roster exceeds agreement proof resource budget (maximum 92 seats)", error);
        if (identity.IsNull() || !ContextValid(context, seats)) return s.Stop("invalid agreement bootstrap identity/context", error);
        if (!s.db.Exists(MARKER_KEY)) {
            if (!allow_bootstrap || !s.db.IsEmpty()) return s.Stop("missing agreement journal; fresh bootstrap not authorized", error);
            s.marker.identity = identity; s.marker.active.value = context;
            s.slot.context.value = context; s.seats = seats;
            s.Persist();
        } else {
            if (!ReadExact(s.db, MARKER_KEY, s.marker) || s.marker.version != 1 || s.marker.identity != identity ||
                s.marker.active.value.domain != context.domain || s.marker.active.value.market_id != context.market_id ||
                s.marker.active.value.execution_config_id != context.execution_config_id) {
                return s.Stop("agreement journal identity/context mismatch", error);
            }
            const auto& active{s.marker.active.value};
            if (!ReadExact(s.db, SlotKey{'s', active.epoch, active.sequence}, s.slot) || !SameContext(active, s.Context())) {
                return s.Stop("missing or corrupt active agreement slot", error);
            }
            auto historical{active.seat_set_hash == seats.set_hash ? std::optional<ActiveFnBlsSeatSet>{seats} :
                (s.callbacks.seat_set ? s.callbacks.seat_set(active) : std::nullopt)};
            if (!historical || !s.ValidateSlot(s.slot, *historical, error)) return s.Stop("agreement journal validation: " + error, error);
            s.seats = *historical;
            // Check every namespace/key and historical context. Histories are
            // append-only slots; unknown namespaces are not silently ignored.
            std::unique_ptr<CDBIterator> it{s.db.NewIterator()};
            it->SeekToFirst();
            uint64_t last_sequence{0}; bool first{true}; bool found_active{false};
            while (it->Valid()) {
                uint8_t prefix;
                if (it->GetKeyExact(prefix) && prefix == MARKER_KEY) { it->Next(); continue; }
                SlotKey key; Slot old;
                if (!it->GetKeyExact(key) || key.prefix != 's' || !it->GetValueExact(old) ||
                    key.epoch != old.context.value.epoch || key.sequence != old.context.value.sequence ||
                    old.context.value.domain != context.domain || old.context.value.market_id != context.market_id ||
                    old.context.value.execution_config_id != context.execution_config_id || key.sequence > active.sequence ||
                    (!first && key.sequence <= last_sequence)) return s.Stop("corrupt agreement slot history", error);
                first = false; last_sequence = key.sequence;
                found_active |= key.epoch == active.epoch && key.sequence == active.sequence;
                const auto authority{old.context.value.seat_set_hash == s.seats.set_hash ? std::optional<ActiveFnBlsSeatSet>{s.seats} :
                    (s.callbacks.seat_set ? s.callbacks.seat_set(old.context.value) : std::nullopt)};
                if (!authority || !s.ValidateSlot(old, *authority, error)) return s.Stop("invalid agreement history: " + error, error);
                it->Next();
            }
            if (!it->StatusOK() || !found_active) return s.Stop("incomplete agreement slot history", error);
        }
        s.opened = true;
        s.RestoreMemory();
        if (!SameContext(s.Context(), context)) return Advance(context, seats, error);
        error.clear(); return true;
    } catch (const std::exception& e) { return s.Stop(std::string{"agreement open failed: "} + e.what(), error); }
}

bool FlowMeshAgreement::Advance(const PreagreementContext& context, const ActiveFnBlsSeatSet& seats, std::string& error)
{
    auto& s{*m_impl}; if (!s.Ready(error)) return false;
    try {
        if (!CompleteProofFits(seats)) return s.Stop("next full anchored roster exceeds agreement proof resource budget (maximum 92 seats)", error);
        if (SameContext(context, s.Context())) return true;
        if (!ContextValid(context, seats) || context.domain != s.Context().domain ||
            context.market_id != s.Context().market_id || context.execution_config_id != s.Context().execution_config_id ||
            context.sequence <= s.Context().sequence || context.epoch < s.Context().epoch) {
            return s.Stop("agreement advancement does not match certified forward history", error);
        }
        if (context.sequence == s.Context().sequence + 1 && s.IsDecided()) {
            const auto decision{DecodeAgreementMessage(s.slot.decision)};
            if (context.parent_hash != decision->candidate) return s.Stop("certified history contradicts durable agreement decision", error);
        }
        if (s.db.Exists(SlotKey{'s', context.epoch, context.sequence})) return s.Stop("agreement advancement would overwrite slot history", error);
        s.slot = Slot{}; s.slot.context.value = context; s.marker.active.value = context; s.seats = seats;
        s.Persist(); s.RestoreMemory(); return true;
    } catch (const std::exception& e) { return s.Stop(std::string{"agreement advancement failed: "} + e.what(), error); }
}

bool FlowMeshAgreement::SubmitCandidate(const std::span<const unsigned char> entry, std::string& error)
{
    auto& s{*m_impl}; if (!s.Ready(error)) return false;
    try {
        s.usable.clear();
        const auto decoded{DecodeProductionEntry(entry)};
        if (!decoded || !EntryMatches(Bytes{entry.begin(), entry.end()}, s.Context(), decoded->GetHash())) {
            error = "candidate does not match pinned agreement context"; return false;
        }
        if (s.IsDecided()) return decoded->GetHash() == *DecidedCandidate();
        s.RememberCandidate(Bytes{entry.begin(), entry.end()}, decoded->GetHash());
        return s.Pump();
    } catch (const std::exception& e) { return s.Stop(std::string{"agreement candidate failure: "} + e.what(), error); }
}

bool FlowMeshAgreement::Receive(const AgreementMessage& message, std::string& error)
{
    auto& s{*m_impl}; if (!s.Ready(error)) return false;
    try {
        s.usable.clear();
        if (!s.ValidMessage(message, s.seats, s.Context())) { error = "invalid agreement message"; return false; }
        if (message.stage == AgreementStage::DECISION) { s.SaveDecision(message); return true; }
        if (s.IsDecided()) return true;
        const bool fresh{!s.received.contains(Key(message))};
        bool enrichment{false};
        if (!fresh && message.stage == AgreementStage::COMMIT) {
            const auto bucket{s.votes.find({message.view, message.candidate})};
            enrichment = bucket != s.votes.end() && bucket->second.commits.contains(message.seat_index) &&
                ((message.prepared && !bucket->second.prepared) ||
                 (message.new_view && !bucket->second.new_view));
        }
        // Refuse bounded-cache exhaustion at ingress. A verified duplicate
        // carrying new attachments must not turn resource pressure into a
        // permanent journal halt inside Track(). Unchanged retries cost zero.
        if ((fresh && s.received.size() >= MAX_RECORDS) ||
            ((fresh || enrichment) && EncodeAgreementMessage(message)->size() > MAX_SLOT_BYTES - s.received_bytes)) {
            error = "agreement incoming message resource limit"; return false;
        }
        if (message.view > s.slot.view && message.stage != AgreementStage::VIEW_CHANGE) {
            if (!message.new_view) { error = "higher agreement view lacks quorum new-view proof"; return false; }
            s.Pump();
            if (!s.MoveView(message.view, false)) { error = "waiting for durable signing intent recovery"; return false; }
        }
        // Old authenticated proposals can supply the candidate needed by a
        // later highest-prepared report, but never resurrect old-view votes.
        if (message.stage == AgreementStage::PROPOSAL) {
            if (const auto old{s.pending_proposals.find(message.view)}; old != s.pending_proposals.end() &&
                old->second.candidate != message.candidate) { error = "leader equivocated in agreement view"; return false; }
            s.RememberProposalCandidate(message);
        }
        s.Track(message);
        if (message.prepared && message.stage == AgreementStage::COMMIT) s.LearnPrepared(*message.prepared);
        if (message.stage == AgreementStage::VIEW_CHANGE && message.view > s.slot.view &&
            s.reports[message.view].size() >= s.seats.Size() - s.Quorum() + 1) {
            s.Pump();
            if (!s.MoveView(message.view, true)) return true;
        }
        return s.Pump();
    } catch (const std::exception& e) { return s.Stop(std::string{"agreement receive failure: "} + e.what(), error); }
}

bool FlowMeshAgreement::Timeout(std::string& error)
{
    auto& s{*m_impl}; if (!s.Ready(error)) return false;
    try {
        s.usable.clear();
        s.Pump();
        if (!s.IsDecided() && s.MoveView(s.slot.view + 1, true)) s.Pump();
        return true;
    } catch (const std::exception& e) { return s.Stop(std::string{"agreement timeout failure: "} + e.what(), error); }
}

bool FlowMeshAgreement::Retry(std::string& error)
{
    auto& s{*m_impl}; if (!s.Ready(error)) return false;
    try {
        // Evidence and anchors can become available again between retries.
        s.usable.clear();
        s.RetryOldProposalCandidates();
        s.Pump();
        // Retain/rebroadcast the authenticated proposal even on a follower.
        // Its entry bytes are the fetch path for a hidden highest-prepared
        // candidate when the original leader is no longer available.
        std::vector<const Bytes*> retry;
        for (const auto& record : s.slot.records) retry.push_back(&record.signed_bytes);
        for (const auto& [view, proposal] : s.slot.proposals) retry.push_back(&proposal);
        if (s.IsDecided()) retry.push_back(&s.slot.decision);
        const size_t count{retry.size()};
        size_t sent_bytes{0};
        for (size_t visited{0}; visited < std::min<size_t>(count, 64); ++visited) {
            s.retry_cursor %= count;
            const auto& bytes{*retry[s.retry_cursor]};
            if (bytes.empty()) { ++s.retry_cursor; continue; }
            if (sent_bytes && bytes.size() > AGREEMENT_MAX_BYTES - sent_bytes) break;
            if (!s.Send(*DecodeAgreementMessage(bytes))) break;
            sent_bytes += bytes.size();
            ++s.retry_cursor;
        }
        return true;
    } catch (const std::exception& e) { return s.Stop(std::string{"agreement retry failure: "} + e.what(), error); }
}

uint32_t FlowMeshAgreement::View() const { return m_impl->slot.view; }
const PreagreementContext& FlowMeshAgreement::Context() const { return m_impl->slot.context.value; }
bool FlowMeshAgreement::Halted() const { return m_impl->halted; }
const std::string& FlowMeshAgreement::LastError() const { return m_impl->last_error; }
std::optional<uint256> FlowMeshAgreement::RequiredCandidateHash() const { return m_impl->required; }
std::optional<uint256> FlowMeshAgreement::DecidedCandidate() const
{
    if (!m_impl->opened || m_impl->halted || m_impl->slot.decision.empty()) return std::nullopt;
    const auto decision{DecodeAgreementMessage(m_impl->slot.decision)};
    return decision ? std::optional<uint256>{decision->candidate} : std::nullopt;
}
std::optional<Bytes> FlowMeshAgreement::CandidateBytes(const uint256& hash) const
{
    const auto found{m_impl->slot.candidates.find(hash)};
    return found == m_impl->slot.candidates.end() ? std::nullopt : std::optional<Bytes>{found->second.entry};
}
std::optional<Bytes> FlowMeshAgreement::RestoreCandidateBlob(const uint256& hash) const
{
    const auto found{m_impl->slot.candidates.find(hash)};
    return found == m_impl->slot.candidates.end() ? std::nullopt : std::optional<Bytes>{found->second.evidence};
}

} // namespace node
