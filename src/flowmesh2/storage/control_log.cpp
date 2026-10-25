// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <flowmesh2/storage/control_log.h>

#include <flowmesh2/core/codec.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/seat_set.h>
#include <flowmesh2/core/params.h>
#include <flowmesh2/storage/commit_log.h>
#include <streams.h>
#include <util/chaintype.h>

#include <utility>

namespace flowmesh2::storage {
// TODO(rebirth:M4-STO-08) private control journal of the regtest view change;
// the general VIEW_STATE and NEW_VIEW_ACCEPTED records of M4-STO-07 replace it.
namespace {
constexpr uint8_t CONTROL_RECORD_VERSION{1};
constexpr size_t CONTROL_INTENT_MAX{1 + CONTROL_KEY_BYTES + 5 + MAX_CONTROL_MESSAGE_BYTES};
constexpr size_t SIGNED_CONTROL_BYTES{1 + CONTROL_KEY_BYTES + 32 + bls::SIGNATURE_SIZE + 8};
Error Invalid(std::string_view detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }

Result<void> VerifyControlSignature(const bls::PublicKey& key, const uint256& digest, const VoteSignature& bytes)
{
    const auto signature{bls::Signature::Decode(bytes)};
    if (!signature || !bls::Verify(key, std::span{digest.data(), digest.size()}, *signature)) {
        return Error{ErrorCode::VERIFY_FAILED, "CONTROL_SIGNATURE"};
    }
    return Ok();
}
} // namespace

uint256 ControlIntentRecord::Digest() const
{
    // PREPARE starts with 0x01; this control transcript starts with 0xc1.
    // The complete phase/key and exact unsigned body are covered.
    HashWriter writer{TaggedWriter(HashTag::PREPARE)};
    writer << uint8_t{0xc1} << key;
    WriteCompactSize(writer, message.size());
    writer.write(MakeByteSpan(message));
    return writer.GetSHA256();
}

std::vector<unsigned char> ControlIntentRecord::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter writer{out, 0};
    writer << CONTROL_RECORD_VERSION << key;
    WriteCappedVector(writer, message, MAX_CONTROL_MESSAGE_BYTES);
    return out;
}

Result<ControlIntentRecord> ControlIntentRecord::Decode(std::span<const unsigned char> bytes)
{
    return DecodeExactWith(bytes, CONTROL_INTENT_MAX, [](SpanReader& reader) {
        uint8_t version{0};
        ControlIntentRecord record;
        reader >> version;
        if (version != CONTROL_RECORD_VERSION) throw std::ios_base::failure("control record version");
        reader >> record.key;
        ReadCappedVector(reader, record.message, MAX_CONTROL_MESSAGE_BYTES);
        return record;
    });
}

std::vector<unsigned char> SignedControlRecord::Encode() const
{
    std::vector<unsigned char> out;
    VectorWriter{out, 0, CONTROL_RECORD_VERSION, key, digest, signature, intent_seq};
    return out;
}
Result<SignedControlRecord> SignedControlRecord::Decode(std::span<const unsigned char> bytes)
{
    return DecodeExactWith(bytes, SIGNED_CONTROL_BYTES, [](SpanReader& reader) {
        uint8_t version{0};
        SignedControlRecord record;
        reader >> version;
        if (version != CONTROL_RECORD_VERSION) throw std::ios_base::failure("control record version");
        reader >> record.key >> record.digest >> record.signature >> record.intent_seq;
        return record;
    });
}
std::vector<unsigned char> AcceptedControlView::Encode() const { return ControlIntentRecord{key, message}.Encode(); }
Result<AcceptedControlView> AcceptedControlView::Decode(std::span<const unsigned char> bytes)
{
    auto record{ControlIntentRecord::Decode(bytes)};
    if (!record.has_value()) return record.error();
    return Ok(AcceptedControlView{record->key, std::move(record->message)});
}

Result<void> ControlLog::CheckKey(const ControlKey& key, bool own) const
{
    if (key.domain != m_config.domain || key.profile_hash != m_config.profile_hash || key.seat_set_root != m_config.seat_set_root ||
        key.epoch != m_config.epoch || key.seat >= m_config.seat_count || (own && key.seat != m_config.self_seat)) {
        return Invalid("CONTROL_CONFIG");
    }
    if (key.slot == 0 || key.parent_hash.IsNull() || key.view == 0) return Invalid("CONTROL_KEY");
    if (!IsKnown(key.phase)) return Invalid("CONTROL_PHASE");
    if (key.view > MAX_CONTROL_VIEW) return Error{ErrorCode::FENCED, "CONTROL_VIEW_BOUND"};
    if (key.phase == ControlPhase::NEW_VIEW && key.seat != (key.slot % m_config.seat_count + key.view % m_config.seat_count) % m_config.seat_count) {
        return Invalid("CONTROL_NEW_VIEW_LEADER");
    }
    return Ok();
}

Result<void> ControlLog::CheckTransition(const ControlIntentRecord& record) const
{
    if (auto key{CheckKey(record.key, true)}; !key.has_value()) return key.error();
    if (record.message.empty() || record.message.size() > MAX_CONTROL_MESSAGE_BYTES) return Invalid("CONTROL_MESSAGE_SIZE");
    const auto it{m_views.find(record.key.slot)};
    if (it == m_views.end()) {
        if (m_views.size() >= MAX_CONTROL_INSTANCES) return Error{ErrorCode::FENCED, "CONTROL_INSTANCE_BOUND"};
        return Ok();
    }
    const auto& previous{it->second};
    if (previous.parent_hash != record.key.parent_hash) return Error{ErrorCode::EQUIVOCATION_REFUSED, "CONTROL_PARENT_CONFLICT"};
    if (record.key.view < previous.view || (record.key.phase == ControlPhase::VIEW_CHANGE && record.key.view == previous.view)) {
        return Error{ErrorCode::ORDER_VIOLATION, "CONTROL_VIEW_BACKWARDS"};
    }
    return Ok();
}

void ControlLog::InstallIntent(const ControlIntentRecord& record, uint64_t seq)
{
    m_intents.emplace(record.key, Entry{record, seq, std::nullopt, 0});
    auto [it, inserted]{m_views.try_emplace(record.key.slot, ControlView{record.key.parent_hash, 0, false, std::nullopt})};
    auto& state{it->second};
    if (record.key.view > state.view) {
        state.view = record.key.view;
        state.changing = true;
        state.accepted.reset();
    }
}

Result<void> ControlLog::CheckAccepted(const AcceptedControlView& record) const
{
    if (auto key{CheckKey(record.key, false)}; !key.has_value()) return key.error();
    if (record.key.phase != ControlPhase::NEW_VIEW) return Invalid("CONTROL_ACCEPTED_PHASE");
    if (record.message.empty() || record.message.size() > MAX_CONTROL_MESSAGE_BYTES) return Invalid("CONTROL_MESSAGE_SIZE");
    const auto it{m_views.find(record.key.slot)};
    if (it == m_views.end()) {
        if (m_views.size() >= MAX_CONTROL_INSTANCES) return Error{ErrorCode::FENCED, "CONTROL_INSTANCE_BOUND"};
        return Ok();
    }
    if (it->second.parent_hash != record.key.parent_hash) return Error{ErrorCode::EQUIVOCATION_REFUSED, "CONTROL_PARENT_CONFLICT"};
    if (record.key.view < it->second.view) return Error{ErrorCode::ORDER_VIOLATION, "CONTROL_VIEW_BACKWARDS"};
    return Ok();
}

void ControlLog::InstallAccepted(const AcceptedControlView& record)
{
    auto [it, inserted]{m_views.try_emplace(record.key.slot, ControlView{record.key.parent_hash, 0, false, std::nullopt})};
    it->second.view = record.key.view;
    it->second.changing = false;
    it->second.accepted = record;
}

Result<ControlLog> ControlLog::Create(Journal& journal, const ControlConfig& config)
{
    if (journal.Role() != JournalRole::SIGNER) return Error{ErrorCode::ORDER_VIOLATION, LOG_ROLE};
    const auto status{journal.Status()};
    if (status.fatal) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (status.appended_seq != status.durable_seq) return Error{ErrorCode::ORDER_VIOLATION, LOG_PENDING_RECORDS};
    if (config.domain.IsNull() || config.profile_hash.IsNull() || config.seat_set_root.IsNull() || config.seat_count == 0 ||
        config.seat_count > MAX_SEATS || config.self_seat >= config.seat_count) return Invalid("CONTROL_CONFIG");
    const auto profile{StableQViewRegtestProfile(ChainType::REGTEST)};
    if (!profile.has_value()) return profile.error();
    const auto network_profile{StableQNetworkRegtestProfile(ChainType::REGTEST)};
    if (!network_profile.has_value()) return network_profile.error();
    if ((config.profile_hash != ProfileHash(*profile) && config.profile_hash != ProfileHash(*network_profile)) ||
        config.epoch != 0 || !RegtestPbftSeatCount(config.seat_count)) return Invalid("CONTROL_REGTEST_VIEW_PROFILE");
    auto records{journal.ReadAll()};
    if (!records.has_value()) return records.error();
    if (records->size() != status.durable_seq) return Error{ErrorCode::ORDER_VIOLATION, LOG_REPLAY_SEQUENCE};
    ControlLog log{journal, config};
    uint64_t expected{0};
    for (const auto& record : *records) {
        if (record.seq != ++expected) return Error{ErrorCode::ORDER_VIOLATION, LOG_REPLAY_SEQUENCE};
        if (record.payload.size() > MAX_CONTROL_HISTORY_BYTES - log.m_history_bytes) return Error{ErrorCode::FENCED, "CONTROL_HISTORY_BOUND"};
        log.m_history_bytes += record.payload.size();
        if (record.type == RecordType::CONTROL_INTENT) {
            auto intent{ControlIntentRecord::Decode(record.payload)};
            if (!intent.has_value()) return intent.error();
            if (const auto known{log.m_intents.find(intent->key)}; known != log.m_intents.end()) {
                if (known->second.intent != *intent) return Error{ErrorCode::EQUIVOCATION_REFUSED, "CONTROL_INTENT_CONFLICT"};
                continue;
            }
            if (auto valid{log.CheckTransition(*intent)}; !valid.has_value()) return valid.error();
            log.InstallIntent(*intent, record.seq);
        } else if (record.type == RecordType::SIGNED_CONTROL) {
            auto vote{SignedControlRecord::Decode(record.payload)};
            if (!vote.has_value()) return vote.error();
            const auto it{log.m_intents.find(vote->key)};
            if (it == log.m_intents.end() || it->second.intent_seq != vote->intent_seq || vote->intent_seq >= record.seq ||
                it->second.intent.Digest() != vote->digest) return Error{ErrorCode::VERIFY_FAILED, "CONTROL_INTENT_UNKNOWN"};
            if (auto verified{VerifyControlSignature(config.signer_key, vote->digest, vote->signature)}; !verified.has_value()) return verified.error();
            if (it->second.signature) {
                if (*it->second.signature != vote->signature) return Error{ErrorCode::EQUIVOCATION_REFUSED, "CONTROL_SIGNATURE_CONFLICT"};
            } else {
                it->second.signature = vote->signature;
                it->second.signed_seq = record.seq;
            }
        } else if (record.type == RecordType::NEW_VIEW_ACCEPTED) {
            auto accepted{AcceptedControlView::Decode(record.payload)};
            if (!accepted.has_value()) return accepted.error();
            const auto key{std::pair{accepted->key.slot, accepted->key.view}};
            if (const auto it{log.m_accepted.find(key)}; it != log.m_accepted.end()) {
                if (it->second.first.key != accepted->key || it->second.first.message != accepted->message) {
                    return Error{ErrorCode::EQUIVOCATION_REFUSED, "CONTROL_NEW_VIEW_CONFLICT"};
                }
                continue;
            }
            if (auto valid{log.CheckAccepted(*accepted)}; !valid.has_value()) return valid.error();
            log.m_accepted.emplace(key, std::pair{*accepted, record.seq});
            log.InstallAccepted(*accepted);
        } else {
            return Error{ErrorCode::ORDER_VIOLATION, "CONTROL_RECORD_ROLE"};
        }
    }
    if (journal.Status() != status) return Error{ErrorCode::ORDER_VIOLATION, LOG_REPLAY_CHANGED};
    return Ok(std::move(log));
}

Result<PendingControlIntent> ControlLog::AppendIntent(const ControlIntentRecord& record)
{
    if (IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (record.message.size() > MAX_CONTROL_MESSAGE_BYTES) return Error{ErrorCode::OVERSIZE, "CONTROL_MESSAGE_SIZE"};
    const auto digest{record.Digest()};
    if (const auto it{m_intents.find(record.key)}; it != m_intents.end()) {
        if (it->second.intent != record) return Error{ErrorCode::EQUIVOCATION_REFUSED, "CONTROL_INTENT_CONFLICT"};
        return Ok(PendingControlIntent{JournalInstance(), it->second.intent_seq, ControlRef{record.key, digest}});
    }
    if (auto valid{CheckTransition(record)}; !valid.has_value()) return valid.error();
    const auto bytes{record.Encode()};
    if (bytes.size() > MAX_CONTROL_HISTORY_BYTES - m_history_bytes) return Error{ErrorCode::FENCED, "CONTROL_HISTORY_BOUND"};
    auto seq{m_journal->Append(RecordType::CONTROL_INTENT, bytes)};
    if (!seq.has_value()) return seq.error();
    m_history_bytes += bytes.size();
    InstallIntent(record, *seq);
    return Ok(PendingControlIntent{JournalInstance(), *seq, ControlRef{record.key, digest}});
}

bool ControlLog::Knows(const DurableControlIntent& intent) const
{
    const auto it{m_intents.find(intent.Binding().key)};
    return intent.JournalInstance() == JournalInstance() && it != m_intents.end() && it->second.intent_seq == intent.Seq() &&
           it->second.intent.Digest() == intent.Digest();
}

Result<void> ControlLog::MaySign(const DurableControlIntent& intent) const
{
    if (IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (!Knows(intent)) return Error{ErrorCode::VERIFY_FAILED, "CONTROL_INTENT_UNKNOWN"};
    const auto& entry{m_intents.find(intent.Binding().key)->second};
    if (entry.signature) return Ok(); // deterministic retransmission of an issued signature
    const auto view{m_views.find(intent.Slot())};
    if (view == m_views.end() || view->second.view != intent.View() ||
        (intent.Binding().key.phase == ControlPhase::VIEW_CHANGE && !view->second.changing)) {
        return Error{ErrorCode::ORDER_VIOLATION, "CONTROL_ABANDONED_VIEW"};
    }
    return Ok();
}

Result<void> ControlLog::AllowsVote(const uint256& domain, uint64_t epoch, uint64_t slot, uint64_t view) const
{
    if (IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (domain != m_config.domain || epoch != m_config.epoch) return Invalid("CONTROL_VOTE_INSTANCE");
    const auto it{m_views.find(slot)};
    if (it == m_views.end()) return view == 0 ? Ok() : Result<void>{Error{ErrorCode::ORDER_VIOLATION, "CONTROL_NEW_VIEW_NOT_ACCEPTED"}};
    if (it->second.view != view || it->second.changing || !it->second.accepted) {
        return Error{ErrorCode::ORDER_VIOLATION, "CONTROL_VIEW_NOT_ACTIVE"};
    }
    const auto accepted{m_accepted.find(std::pair{slot, view})};
    if (accepted == m_accepted.end() || accepted->second.second > m_journal->DurableSeq()) {
        return Error{ErrorCode::ORDER_VIOLATION, "CONTROL_VIEW_NOT_DURABLE"};
    }
    return Ok();
}

Result<PendingSignedControl> ControlLog::AppendSigned(const DurableControlIntent& intent, const VoteSignature& signature)
{
    if (IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (!Knows(intent)) return Error{ErrorCode::VERIFY_FAILED, "CONTROL_INTENT_UNKNOWN"};
    if (auto verified{VerifyControlSignature(m_config.signer_key, intent.Digest(), signature)}; !verified.has_value()) return verified.error();
    auto& entry{m_intents.find(intent.Binding().key)->second};
    const SignedControlRef ref{intent.Binding().key, intent.Digest(), signature};
    if (entry.signature) {
        if (*entry.signature != signature) return Error{ErrorCode::EQUIVOCATION_REFUSED, "CONTROL_SIGNATURE_CONFLICT"};
        return Ok(PendingSignedControl{JournalInstance(), entry.signed_seq, ref});
    }
    const auto bytes{SignedControlRecord{intent.Binding().key, intent.Digest(), signature, intent.Seq()}.Encode()};
    if (bytes.size() > MAX_CONTROL_HISTORY_BYTES - m_history_bytes) return Error{ErrorCode::FENCED, "CONTROL_HISTORY_BOUND"};
    auto seq{m_journal->Append(RecordType::SIGNED_CONTROL, bytes)};
    if (!seq.has_value()) return seq.error();
    m_history_bytes += bytes.size();
    entry.signature = signature;
    entry.signed_seq = *seq;
    return Ok(PendingSignedControl{JournalInstance(), *seq, ref});
}

Result<uint64_t> ControlLog::AcceptNewView(const AcceptedControlView& record)
{
    if (IsFatal()) return Error{ErrorCode::FENCED, JOURNAL_FENCED};
    if (record.message.size() > MAX_CONTROL_MESSAGE_BYTES) return Error{ErrorCode::OVERSIZE, "CONTROL_MESSAGE_SIZE"};
    const auto key{std::pair{record.key.slot, record.key.view}};
    if (const auto it{m_accepted.find(key)}; it != m_accepted.end()) {
        if (it->second.first.key != record.key || it->second.first.message != record.message) {
            return Error{ErrorCode::EQUIVOCATION_REFUSED, "CONTROL_NEW_VIEW_CONFLICT"};
        }
        return Ok(it->second.second);
    }
    if (auto valid{CheckAccepted(record)}; !valid.has_value()) return valid.error();
    const auto bytes{record.Encode()};
    if (bytes.size() > MAX_CONTROL_HISTORY_BYTES - m_history_bytes) return Error{ErrorCode::FENCED, "CONTROL_HISTORY_BOUND"};
    auto seq{m_journal->Append(RecordType::NEW_VIEW_ACCEPTED, bytes)};
    if (!seq.has_value()) return seq.error();
    m_history_bytes += bytes.size();
    m_accepted.emplace(key, std::pair{record, *seq});
    InstallAccepted(record);
    return seq;
}

std::optional<ControlIntentRecord> ControlLog::IntentAt(const ControlKey& key) const
{
    const auto it{m_intents.find(key)};
    return it == m_intents.end() ? std::nullopt : std::optional{it->second.intent};
}
std::optional<PendingSignedControl> ControlLog::SignedAt(const ControlKey& key) const
{
    const auto it{m_intents.find(key)};
    if (it == m_intents.end() || !it->second.signature) return std::nullopt;
    return PendingSignedControl{JournalInstance(), it->second.signed_seq, SignedControlRef{key, it->second.intent.Digest(), *it->second.signature}};
}
std::optional<ControlView> ControlLog::ViewAt(uint64_t slot) const
{
    const auto it{m_views.find(slot)};
    return it == m_views.end() ? std::nullopt : std::optional{it->second};
}

std::optional<AcceptedControlView> ControlLog::AcceptedAt(uint64_t slot, uint64_t view) const
{
    const auto it{m_accepted.find(std::pair{slot, view})};
    if (it == m_accepted.end() || it->second.second > m_journal->DurableSeq()) return std::nullopt;
    return it->second.first;
}

} // namespace flowmesh2::storage
