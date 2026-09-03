// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_production_store.h>

#include <consensus/flowmesh_params.h>
#include <crypto/common.h>
#include <flowmesh/production_wire.h>
#include <streams.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace node {

bool FlowMeshHandoffConnectionMature(
    const ProductionB3Connection& connection,
    const int32_t canonical_tip_height)
{
    return connection.height >= 0 &&
           canonical_tip_height >= connection.height &&
           static_cast<int64_t>(canonical_tip_height) - connection.height >=
               Consensus::FLOWMESH_ANCHOR_DEPTH;
}

namespace {

constexpr uint8_t KEY_MARKER{'m'};
constexpr uint8_t KEY_ENTRY{'e'};
constexpr uint8_t KEY_CONNECTION{'c'};
constexpr uint8_t KEY_LOCK{'l'};
constexpr uint8_t KEY_LOCKED_CANDIDATE{'p'};
constexpr uint8_t KEY_LEGACY_SNAPSHOT{'s'};

constexpr size_t MAX_DISK_ENTRY_BYTES{
    flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES +
    flowmesh::FLOWMESH_BLS_CERTIFICATE_MAX_SIZE +
    modern::FLOWMESH_MAX_CHECKPOINT_EFFECTS *
        (modern::FLOWMESH_DEPOSIT_ACCEPTANCE_V1_SIZE + 3) +
    flowmesh::FLOWMESH_MAX_WITHDRAWAL_SETTLEMENTS_PER_ENTRY *
        (modern::FLOWMESH_WITHDRAWAL_RECEIPT_V1_SIZE + 32 + 32 + 4 + 32 + 3) +
    64};
constexpr size_t MAX_DISK_CONNECTION_BYTES{
    modern::FLOWMESH_CHECKPOINT_RECORD_MAX_SIZE + 16 + 4 + 32};
constexpr size_t MAX_DISK_LOCKED_CANDIDATE_BYTES{
    flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES +
    flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS *
        (flowmesh::FLOWMESH_ACTION_MAX_BYTES + 9) +
    18};

// Ordered namespaces use fixed-width big-endian integers. LevelDB compares
// raw key bytes; ordinary little-endian integer serialization would place
// sequence 256 before sequence 1 and break contiguous/replay scans.
struct SequenceKeyType {
    uint8_t prefix{0};
    uint64_t sequence{0};

    SERIALIZE_METHODS(SequenceKeyType, obj)
    {
        READWRITE(obj.prefix,
                  Using<BigEndianFormatter<8>>(obj.sequence));
    }

    friend bool operator==(const SequenceKeyType&,
                           const SequenceKeyType&) = default;
};

using EntryKeyType = SequenceKeyType;
using ConnectionKeyType = SequenceKeyType;

struct LockKeyType {
    uint8_t prefix{KEY_LOCK};
    uint64_t epoch{0};
    uint64_t sequence{0};

    SERIALIZE_METHODS(LockKeyType, obj)
    {
        READWRITE(obj.prefix, Using<BigEndianFormatter<8>>(obj.epoch),
                  Using<BigEndianFormatter<8>>(obj.sequence));
    }

    friend bool operator==(const LockKeyType&, const LockKeyType&) = default;
    friend bool operator<(const LockKeyType& a, const LockKeyType& b)
    {
        return a.prefix < b.prefix ||
               (a.prefix == b.prefix &&
                (a.epoch < b.epoch ||
                 (a.epoch == b.epoch && a.sequence < b.sequence)));
    }
};

EntryKeyType EntryKey(const uint64_t sequence)
{
    return {KEY_ENTRY, sequence};
}

ConnectionKeyType ConnectionKey(const uint64_t sequence)
{
    return {KEY_CONNECTION, sequence};
}

LockKeyType LockKey(const flowmesh::ProductionSignPosition& position)
{
    return {KEY_LOCK, position.epoch, position.sequence};
}

LockKeyType LockedCandidateKey(
    const flowmesh::ProductionSignPosition& position)
{
    return {KEY_LOCKED_CANDIDATE, position.epoch, position.sequence};
}

bool SameSemanticAction(const flowmesh::Action& semantic,
                        const flowmesh::Action& evidence)
{
    if (!semantic.credential.empty() || !semantic.ShapeIsCanonical() ||
        !evidence.ShapeIsCanonical()) {
        return false;
    }
    flowmesh::Action stripped{evidence};
    stripped.credential.clear();
    const auto semantic_bytes{
        flowmesh::EncodeProductionActionPayload(semantic)};
    const auto stripped_bytes{
        flowmesh::EncodeProductionActionPayload(stripped)};
    return semantic_bytes && stripped_bytes &&
           *semantic_bytes == *stripped_bytes;
}

bool LockedCandidateEvidenceMatches(
    const flowmesh::ProductionEntryCore& entry,
    const std::span<const flowmesh::Action> evidence)
{
    if (entry.actions.size() != evidence.size()) return false;
    for (size_t i{0}; i < evidence.size(); ++i) {
        if (!SameSemanticAction(entry.actions[i], evidence[i])) return false;
        if (!evidence[i].IsDeposit() && evidence[i].credential.empty()) {
            return false;
        }
    }
    return true;
}

struct DiskLockedCandidate {
    std::vector<unsigned char> entry_bytes;
    std::vector<std::vector<unsigned char>> evidence_bytes;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        WriteCompactSize(s, entry_bytes.size());
        if (!entry_bytes.empty()) {
            s.write(std::as_bytes(
                std::span{entry_bytes.data(), entry_bytes.size()}));
        }
        WriteCompactSize(s, evidence_bytes.size());
        for (const auto& evidence : evidence_bytes) {
            WriteCompactSize(s, evidence.size());
            if (!evidence.empty()) {
                s.write(std::as_bytes(
                    std::span{evidence.data(), evidence.size()}));
            }
        }
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        if (s.size() > MAX_DISK_LOCKED_CANDIDATE_BYTES) {
            throw std::ios_base::failure(
                "FlowMesh v3 locked candidate exceeds bound");
        }
        const uint64_t entry_size{ReadCompactSize(s)};
        if (entry_size == 0 ||
            entry_size > flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES) {
            throw std::ios_base::failure(
                "FlowMesh v3 locked candidate entry exceeds bound");
        }
        entry_bytes.resize(entry_size);
        s.read(std::as_writable_bytes(
            std::span{entry_bytes.data(), entry_bytes.size()}));

        const uint64_t evidence_count{ReadCompactSize(s)};
        if (evidence_count > flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_ACTIONS) {
            throw std::ios_base::failure(
                "FlowMesh v3 locked candidate evidence count exceeds bound");
        }
        evidence_bytes.clear();
        evidence_bytes.reserve(evidence_count);
        for (uint64_t i{0}; i < evidence_count; ++i) {
            const uint64_t evidence_size{ReadCompactSize(s)};
            if (evidence_size == 0 ||
                evidence_size > flowmesh::FLOWMESH_ACTION_MAX_BYTES) {
                throw std::ios_base::failure(
                    "FlowMesh v3 locked candidate evidence exceeds bound");
            }
            auto& evidence{evidence_bytes.emplace_back(evidence_size)};
            s.read(std::as_writable_bytes(
                std::span{evidence.data(), evidence.size()}));
        }
    }
};

struct DiskEntry {
    uint64_t verified_epoch{0};
    std::vector<unsigned char> entry_bytes;
    std::vector<unsigned char> certificate_bytes;
    std::vector<std::vector<unsigned char>> effect_bytes;
    std::vector<flowmesh::WithdrawalSettlementFactV1> settlements;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << verified_epoch;
        WriteCompactSize(s, entry_bytes.size());
        if (!entry_bytes.empty()) {
            s.write(std::as_bytes(std::span{entry_bytes.data(), entry_bytes.size()}));
        }
        WriteCompactSize(s, certificate_bytes.size());
        if (!certificate_bytes.empty()) {
            s.write(std::as_bytes(
                std::span{certificate_bytes.data(), certificate_bytes.size()}));
        }
        WriteCompactSize(s, effect_bytes.size());
        for (const auto& effect : effect_bytes) {
            WriteCompactSize(s, effect.size());
            if (!effect.empty()) {
                s.write(std::as_bytes(
                    std::span{effect.data(), effect.size()}));
            }
        }
        WriteCompactSize(s, settlements.size());
        for (const auto& settlement : settlements) {
            const auto encoded{modern::EncodeFlowMeshEffectV1(
                modern::FlowMeshEffectV1{settlement.receipt})};
            if (!encoded ||
                encoded->size() !=
                    modern::FLOWMESH_WITHDRAWAL_RECEIPT_V1_SIZE) {
                throw std::ios_base::failure(
                    "FlowMesh v3 settlement receipt is invalid");
            }
            s.write(std::as_bytes(
                std::span{encoded->data(), encoded->size()}));
            s << settlement.checkpoint_id << settlement.transaction_id
              << settlement.connected_height << settlement.connected_block;
        }
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        if (s.size() > MAX_DISK_ENTRY_BYTES) {
            throw std::ios_base::failure("FlowMesh v3 disk entry exceeds bound");
        }
        s >> verified_epoch;
        const uint64_t entry_size{ReadCompactSize(s)};
        if (entry_size == 0 ||
            entry_size > flowmesh::FLOWMESH_V1_MAX_MICROBLOCK_BYTES) {
            throw std::ios_base::failure("FlowMesh v3 entry body exceeds bound");
        }
        entry_bytes.resize(entry_size);
        s.read(std::as_writable_bytes(
            std::span{entry_bytes.data(), entry_bytes.size()}));

        const uint64_t certificate_size{ReadCompactSize(s)};
        if (certificate_size <
                flowmesh::FLOWMESH_BLS_CERTIFICATE_FIXED_SIZE + 1 ||
            certificate_size > flowmesh::FLOWMESH_BLS_CERTIFICATE_MAX_SIZE) {
            throw std::ios_base::failure("FlowMesh v3 certificate exceeds bound");
        }
        certificate_bytes.resize(certificate_size);
        s.read(std::as_writable_bytes(
            std::span{certificate_bytes.data(), certificate_bytes.size()}));

        const uint64_t effect_count{ReadCompactSize(s)};
        if (effect_count > modern::FLOWMESH_MAX_CHECKPOINT_EFFECTS) {
            throw std::ios_base::failure("FlowMesh v3 effect count exceeds bound");
        }
        effect_bytes.clear();
        effect_bytes.reserve(effect_count);
        for (uint64_t i{0}; i < effect_count; ++i) {
            const uint64_t effect_size{ReadCompactSize(s)};
            if (effect_size != modern::FLOWMESH_DEPOSIT_ACCEPTANCE_V1_SIZE &&
                effect_size != modern::FLOWMESH_WITHDRAWAL_RECEIPT_V1_SIZE) {
                throw std::ios_base::failure("FlowMesh v3 effect size is invalid");
            }
            auto& effect{effect_bytes.emplace_back(effect_size)};
            s.read(std::as_writable_bytes(
                std::span{effect.data(), effect.size()}));
        }

        const uint64_t settlement_count{ReadCompactSize(s)};
        if (settlement_count >
            flowmesh::FLOWMESH_MAX_WITHDRAWAL_SETTLEMENTS_PER_ENTRY) {
            throw std::ios_base::failure(
                "FlowMesh v3 settlement count exceeds bound");
        }
        settlements.clear();
        settlements.reserve(settlement_count);
        for (uint64_t i{0}; i < settlement_count; ++i) {
            std::vector<unsigned char> receipt_bytes(
                modern::FLOWMESH_WITHDRAWAL_RECEIPT_V1_SIZE);
            s.read(std::as_writable_bytes(std::span{
                receipt_bytes.data(), receipt_bytes.size()}));
            const auto effect{
                modern::DecodeFlowMeshEffectV1(receipt_bytes)};
            const auto* receipt{
                effect ? std::get_if<modern::FlowMeshWithdrawalReceiptV1>(
                             &*effect)
                       : nullptr};
            if (receipt == nullptr) {
                throw std::ios_base::failure(
                    "FlowMesh v3 settlement receipt is malformed");
            }
            flowmesh::WithdrawalSettlementFactV1 settlement;
            settlement.receipt = *receipt;
            s >> settlement.checkpoint_id >> settlement.transaction_id
              >> settlement.connected_height >> settlement.connected_block;
            if (!flowmesh::WithdrawalSettlementFactIsCanonical(settlement) ||
                (!settlements.empty() &&
                 !(settlements.back().receipt.receipt_id <
                   settlement.receipt.receipt_id))) {
                throw std::ios_base::failure(
                    "FlowMesh v3 settlements are not canonical");
            }
            settlements.push_back(std::move(settlement));
        }
    }
};

struct DiskConnection {
    int32_t connected_height{-1};
    uint256 connected_block;
    std::vector<unsigned char> checkpoint_bytes;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << connected_height << connected_block;
        WriteCompactSize(s, checkpoint_bytes.size());
        if (!checkpoint_bytes.empty()) {
            s.write(std::as_bytes(
                std::span{checkpoint_bytes.data(), checkpoint_bytes.size()}));
        }
    }

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        if (s.size() > MAX_DISK_CONNECTION_BYTES) {
            throw std::ios_base::failure("FlowMesh v3 checkpoint exceeds bound");
        }
        s >> connected_height >> connected_block;
        if (connected_height < 0 || connected_block.IsNull()) {
            throw std::ios_base::failure(
                "FlowMesh v3 checkpoint connection is invalid");
        }
        const uint64_t size{ReadCompactSize(s)};
        if (size < modern::FLOWMESH_CHECKPOINT_RECORD_MIN_SIZE ||
            size > modern::FLOWMESH_CHECKPOINT_RECORD_MAX_SIZE) {
            throw std::ios_base::failure("FlowMesh v3 checkpoint size is invalid");
        }
        checkpoint_bytes.resize(size);
        s.read(std::as_writable_bytes(
            std::span{checkpoint_bytes.data(), checkpoint_bytes.size()}));
    }
};

enum class ReadResult : uint8_t { FOUND, NOT_FOUND, ERROR };

template <typename K, typename V>
ReadResult ReadStrict(CDBWrapper& db, const K& key, V& value)
{
    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    it->Seek(key);
    if (!it->Valid()) {
        return it->StatusOK() ? ReadResult::NOT_FOUND : ReadResult::ERROR;
    }
    K stored_key;
    if (!it->GetKeyExact(stored_key) || !(stored_key == key)) {
        return it->StatusOK() ? ReadResult::NOT_FOUND : ReadResult::ERROR;
    }
    if (!it->GetValueExact(value)) return ReadResult::ERROR;
    return ReadResult::FOUND;
}

std::optional<DiskLockedCandidate> MakeDiskLockedCandidate(
    const flowmesh::ProductionEntryCore& entry,
    const std::span<const flowmesh::Action> authenticated_evidence)
{
    const auto entry_bytes{flowmesh::EncodeProductionEntry(entry)};
    if (!entry_bytes ||
        !LockedCandidateEvidenceMatches(entry, authenticated_evidence)) {
        return std::nullopt;
    }
    DiskLockedCandidate out;
    out.entry_bytes = *entry_bytes;
    out.evidence_bytes.reserve(authenticated_evidence.size());
    size_t total{out.entry_bytes.size() + 18};
    for (const flowmesh::Action& action : authenticated_evidence) {
        const auto encoded{flowmesh::EncodeProductionActionPayload(action)};
        if (!encoded ||
            encoded->size() > MAX_DISK_LOCKED_CANDIDATE_BYTES - total) {
            return std::nullopt;
        }
        total += encoded->size() + 9;
        if (total > MAX_DISK_LOCKED_CANDIDATE_BYTES) return std::nullopt;
        out.evidence_bytes.push_back(*encoded);
    }
    return out;
}

std::optional<StoredLockedProductionCandidate> DecodeDiskLockedCandidate(
    const DiskLockedCandidate& disk)
{
    const auto entry{flowmesh::DecodeProductionEntry(disk.entry_bytes)};
    if (!entry || disk.evidence_bytes.size() != entry->actions.size()) {
        return std::nullopt;
    }
    std::vector<flowmesh::Action> evidence;
    evidence.reserve(disk.evidence_bytes.size());
    for (const auto& bytes : disk.evidence_bytes) {
        const auto action{flowmesh::DecodeProductionActionPayload(bytes)};
        if (!action) return std::nullopt;
        evidence.push_back(*action);
    }
    if (!LockedCandidateEvidenceMatches(*entry, evidence)) {
        return std::nullopt;
    }
    return StoredLockedProductionCandidate{*entry, std::move(evidence)};
}

std::optional<flowmesh::AnchorRef> SeatAnchor(
    const flowmesh::ActiveFnBlsSeatSet& seats)
{
    if (seats.anchor_height >
            static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
        seats.anchor_hash.IsNull()) {
        return std::nullopt;
    }
    return flowmesh::AnchorRef{
        static_cast<int32_t>(seats.anchor_height), seats.anchor_hash};
}

bool MarkerShapeIsValid(const FlowMeshProductionStore::Marker& marker)
{
    if (marker.version != FlowMeshProductionStore::FORMAT_VERSION ||
        marker.domain.IsNull() || marker.market_id.IsNull() ||
        marker.current_anchor.IsNull() || marker.current_seat_set_hash.IsNull() ||
        marker.state_root.IsNull()) {
        return false;
    }
    if (marker.next_sequence == 0 && !marker.last_microblock_hash.IsNull()) {
        return false;
    }
    if (marker.next_sequence == 0 && marker.next_effect_index != 0) {
        return false;
    }
    if (marker.next_sequence > 0 && marker.last_microblock_hash.IsNull()) {
        return false;
    }
    return true;
}

bool MarkerMatchesSeatSet(const FlowMeshProductionStore::Marker& marker,
                          const flowmesh::ActiveFnBlsSeatSet& seats)
{
    const auto anchor{SeatAnchor(seats)};
    return anchor &&
           flowmesh::CheckActiveFnBlsSeatSet(marker.domain, seats) ==
               flowmesh::BlsSeatSetCheck::OK &&
           seats.market_id == marker.market_id &&
           seats.epoch == marker.current_epoch &&
           seats.set_hash == marker.current_seat_set_hash &&
           *anchor == marker.current_anchor;
}

ReadResult ReadMarkerStrict(CDBWrapper& db,
                            FlowMeshProductionStore::Marker& marker)
{
    return ReadStrict(db, KEY_MARKER, marker);
}

std::optional<flowmesh::BlsMicroblockCertificate> DecodeCertificateEnvelope(
    const std::span<const unsigned char> bytes)
{
    if (bytes.size() < flowmesh::FLOWMESH_BLS_CERTIFICATE_FIXED_SIZE + 1 ||
        bytes.size() > flowmesh::FLOWMESH_BLS_CERTIFICATE_MAX_SIZE) {
        return std::nullopt;
    }
    const size_t bitmap_size{
        bytes.size() - flowmesh::FLOWMESH_BLS_CERTIFICATE_FIXED_SIZE};
    if (bitmap_size == 0 ||
        bitmap_size > flowmesh::FLOWMESH_MAX_SIGNER_BITMAP_BYTES) {
        return std::nullopt;
    }
    flowmesh::BlsMicroblockCertificate certificate;
    certificate.seat_epoch = ReadBE64(bytes.data());
    certificate.sequence = ReadBE64(bytes.data() + 8);
    std::copy(bytes.begin() + 16, bytes.begin() + 48,
              certificate.microblock_hash.begin());
    certificate.signer_bitmap.assign(bytes.begin() + 48,
                                     bytes.begin() + 48 + bitmap_size);
    std::copy(bytes.begin() + 48 + bitmap_size, bytes.end(),
              certificate.aggregate_signature.begin());
    return certificate;
}

std::optional<flowmesh::ProductionEntryCore> DecodeDiskEntryShape(
    const DiskEntry& disk)
{
    const auto entry{flowmesh::DecodeProductionEntry(disk.entry_bytes)};
    const auto certificate{DecodeCertificateEnvelope(disk.certificate_bytes)};
    if (!entry || !certificate || disk.verified_epoch != entry->epoch ||
        certificate->seat_epoch != entry->epoch ||
        certificate->sequence != entry->sequence ||
        certificate->microblock_hash != entry->GetHash()) {
        return std::nullopt;
    }
    if (disk.effect_bytes.size() != entry->effect_count) return std::nullopt;
    std::vector<modern::FlowMeshEffectV1> effects;
    effects.reserve(disk.effect_bytes.size());
    for (const auto& bytes : disk.effect_bytes) {
        const auto effect{modern::DecodeFlowMeshEffectV1(bytes)};
        if (!effect) return std::nullopt;
        const bool matching_context{std::visit(
            [&](const auto& typed) {
                return typed.market_id == entry->market_id &&
                       typed.epoch == entry->epoch &&
                       typed.sequence == entry->sequence;
            },
            *effect)};
        if (!matching_context) return std::nullopt;
        effects.push_back(*effect);
    }
    const auto effect_root{
        modern::ComputeFlowMeshEffectRoot(entry->effect_start, effects)};
    if (!effect_root || *effect_root != entry->effect_root) {
        return std::nullopt;
    }
    if (disk.settlements.size() >
            flowmesh::FLOWMESH_MAX_WITHDRAWAL_SETTLEMENTS_PER_ENTRY ||
        (!disk.settlements.empty() &&
         (entry->kind != static_cast<uint8_t>(
                             flowmesh::ProductionEntryKind::EXECUTION) ||
          !entry->actions.empty()))) {
        return std::nullopt;
    }
    std::set<Txid> settlement_transactions;
    for (size_t i{0}; i < disk.settlements.size(); ++i) {
        const auto& settlement{disk.settlements[i]};
        if (!flowmesh::WithdrawalSettlementFactIsCanonical(settlement) ||
            settlement.receipt.market_id != entry->market_id ||
            settlement.connected_height > entry->anchor.height ||
            (i > 0 &&
             !(disk.settlements[i - 1].receipt.receipt_id <
               settlement.receipt.receipt_id)) ||
            !settlement_transactions.insert(settlement.transaction_id).second) {
            return std::nullopt;
        }
    }
    return entry;
}

std::optional<std::vector<modern::FlowMeshEffectV1>> DecodeDiskEffects(
    const DiskEntry& disk)
{
    std::vector<modern::FlowMeshEffectV1> out;
    out.reserve(disk.effect_bytes.size());
    for (const auto& bytes : disk.effect_bytes) {
        const auto effect{modern::DecodeFlowMeshEffectV1(bytes)};
        if (!effect) return std::nullopt;
        out.push_back(*effect);
    }
    return out;
}

std::optional<StoredProductionEntry> DecodeDiskEntry(
    const DiskEntry& disk, const flowmesh::ActiveFnBlsSeatSet& seats)
{
    const auto entry{DecodeDiskEntryShape(disk)};
    if (!entry) return std::nullopt;
    const auto certificate{flowmesh::DecodeBlsMicroblockCertificate(
        disk.certificate_bytes, seats.Size())};
    if (!certificate ||
        flowmesh::CheckProductionEntryCertificate(*entry, seats, *certificate) !=
            flowmesh::BlsCertificateCheck::OK) {
        return std::nullopt;
    }
    const auto effects{DecodeDiskEffects(disk)};
    if (!effects) return std::nullopt;
    return StoredProductionEntry{disk.verified_epoch, *entry, *certificate,
                                 *effects, disk.settlements};
}

std::optional<DiskEntry> MakeDiskEntry(
    const flowmesh::ProductionEntryCore& entry,
    const flowmesh::BlsMicroblockCertificate& certificate,
    const size_t seat_count,
    const std::span<const modern::FlowMeshEffectV1> effects,
    const std::span<const flowmesh::WithdrawalSettlementFactV1> settlements)
{
    if (effects.size() != entry.effect_count ||
        settlements.size() >
            flowmesh::FLOWMESH_MAX_WITHDRAWAL_SETTLEMENTS_PER_ENTRY ||
        (!settlements.empty() &&
         (entry.kind != static_cast<uint8_t>(
                            flowmesh::ProductionEntryKind::EXECUTION) ||
          !entry.actions.empty()))) {
        return std::nullopt;
    }
    const auto entry_bytes{flowmesh::EncodeProductionEntry(entry)};
    const auto certificate_bytes{
        flowmesh::EncodeBlsMicroblockCertificate(certificate, seat_count)};
    if (!entry_bytes || !certificate_bytes) return std::nullopt;
    std::vector<std::vector<unsigned char>> effect_bytes;
    effect_bytes.reserve(effects.size());
    for (const auto& effect : effects) {
        const auto bytes{modern::EncodeFlowMeshEffectV1(effect)};
        if (!bytes) return std::nullopt;
        effect_bytes.push_back(*bytes);
    }
    const auto effect_root{
        modern::ComputeFlowMeshEffectRoot(entry.effect_start, effects)};
    if (!effect_root || *effect_root != entry.effect_root) return std::nullopt;
    std::set<Txid> settlement_transactions;
    for (size_t i{0}; i < settlements.size(); ++i) {
        const auto& settlement{settlements[i]};
        if (!flowmesh::WithdrawalSettlementFactIsCanonical(settlement) ||
            settlement.receipt.market_id != entry.market_id ||
            settlement.connected_height > entry.anchor.height ||
            (i > 0 &&
             !(settlements[i - 1].receipt.receipt_id <
               settlement.receipt.receipt_id)) ||
            !settlement_transactions.insert(settlement.transaction_id).second) {
            return std::nullopt;
        }
    }
    return DiskEntry{entry.epoch, *entry_bytes, *certificate_bytes,
                     std::move(effect_bytes),
                     std::vector<flowmesh::WithdrawalSettlementFactV1>{
                         settlements.begin(), settlements.end()}};
}

bool CheckpointCoreMatchesEntry(
    const modern::FlowMeshCheckpointCoreV1& core,
    const flowmesh::ProductionEntryCore& entry)
{
    if (entry.anchor.height < 0 || core.domain != entry.domain ||
        core.market_id != entry.market_id || core.epoch != entry.epoch ||
        core.sequence != entry.sequence ||
        core.microblock_hash != entry.GetHash() ||
        core.production_anchor.height !=
            static_cast<uint64_t>(entry.anchor.height) ||
        core.production_anchor.block_hash != entry.anchor.hash ||
        core.seat_set_hash != entry.seat_set_hash ||
        core.parent_hash != entry.parent_hash ||
        core.previous_state_root != entry.previous_state_root ||
        core.actions_root != entry.actions_root ||
        core.result_root != entry.result_root ||
        core.state_root != entry.state_root ||
        core.effect_start != entry.effect_start ||
        core.effect_count != entry.effect_count ||
        core.effect_root != entry.effect_root) {
        return false;
    }
    const bool handoff{
        entry.kind == static_cast<uint8_t>(
                          flowmesh::ProductionEntryKind::EPOCH_HANDOFF)};
    if (!handoff) {
        return core.kind == modern::FlowMeshCheckpointKind::EXECUTION &&
               !core.handoff;
    }
    return core.kind == modern::FlowMeshCheckpointKind::EPOCH_HANDOFF &&
           core.handoff && core.handoff->next_epoch == entry.next_epoch &&
           entry.next_anchor.height >= 0 &&
           core.handoff->next_anchor.height ==
               static_cast<uint64_t>(entry.next_anchor.height) &&
           core.handoff->next_anchor.block_hash == entry.next_anchor.hash &&
           core.handoff->next_seat_set_hash == entry.next_seat_set_hash;
}

bool CheckpointMatchesEntry(
    const modern::FlowMeshCheckpointRecordV1& checkpoint,
    const StoredProductionEntry& stored,
    const flowmesh::ActiveFnBlsSeatSet& seats)
{
    const auto seat_anchor{SeatAnchor(seats)};
    return seat_anchor &&
           checkpoint.core.anchor.height ==
               static_cast<uint64_t>(seat_anchor->height) &&
           checkpoint.core.anchor.block_hash == seat_anchor->hash &&
           checkpoint.certificate == stored.certificate &&
           CheckpointCoreMatchesEntry(checkpoint.core, stored.entry);
}

std::optional<modern::FlowMeshCheckpointRecordV1> DecodeConnectionEnvelope(
    const DiskConnection& connection)
{
    return modern::DecodeFlowMeshCheckpointEnvelopeV1(
        connection.checkpoint_bytes);
}

std::optional<DiskConnection> MakeDiskConnection(
    const modern::FlowMeshCheckpointRecordV1& checkpoint,
    const size_t seat_count, const ProductionB3Connection& connection)
{
    if (connection.height < 0 || connection.block_hash.IsNull()) {
        return std::nullopt;
    }
    const auto bytes{
        modern::EncodeFlowMeshCheckpointRecordV1(checkpoint, seat_count)};
    if (!bytes) return std::nullopt;
    return DiskConnection{connection.height, connection.block_hash, *bytes};
}

bool ValidateCheckpointConnection(
    const DiskConnection& connection, const DiskEntry& disk_entry,
    const flowmesh::ProductionEntryCore& entry,
    const uint256& expected_previous_checkpoint,
    modern::FlowMeshCheckpointId& checkpoint_id_out,
    std::string& error)
{
    const auto checkpoint{DecodeConnectionEnvelope(connection)};
    const auto disk_certificate{
        DecodeCertificateEnvelope(disk_entry.certificate_bytes)};
    if (!checkpoint || !disk_certificate ||
        checkpoint->certificate != *disk_certificate ||
        !CheckpointCoreMatchesEntry(checkpoint->core, entry)) {
        error = "FlowMesh v3 checkpoint does not match its stored entry";
        return false;
    }
    if (checkpoint->core.previous_checkpoint_id != expected_previous_checkpoint) {
        error = "FlowMesh v3 checkpoint does not extend the checkpoint head";
        return false;
    }
    const auto id{modern::FlowMeshCheckpointIdV1(checkpoint->core)};
    if (!id) {
        error = "FlowMesh v3 checkpoint core is non-canonical";
        return false;
    }
    checkpoint_id_out = *id;
    return true;
}

bool NamespaceIsEmpty(CDBWrapper& db, std::string& error)
{
    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        uint8_t prefix{0};
        if (!it->GetKey(prefix)) continue;
        if (prefix == KEY_MARKER || prefix == KEY_ENTRY ||
            prefix == KEY_CONNECTION || prefix == KEY_LOCK ||
            prefix == KEY_LOCKED_CANDIDATE ||
            prefix == KEY_LEGACY_SNAPSHOT) {
            error = "FlowMesh v3 data exists without a marker";
            return false;
        }
    }
    if (!it->StatusOK()) {
        error = "FlowMesh v3 freshness scan failed";
        return false;
    }
    return true;
}

bool ValidateConnectionNamespace(CDBWrapper& db, const uint64_t next_sequence,
                                 std::string& error)
{
    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    std::optional<uint64_t> previous;
    for (it->Seek(uint8_t{KEY_CONNECTION}); it->Valid(); it->Next()) {
        uint8_t prefix{0};
        if (!it->GetKey(prefix) || prefix != KEY_CONNECTION) break;
        ConnectionKeyType key;
        DiskConnection connection;
        if (!it->GetKeyExact(key) || !it->GetValueExact(connection) ||
            key.prefix != KEY_CONNECTION || key.sequence >= next_sequence ||
            (previous && key.sequence <= *previous) ||
            !DecodeConnectionEnvelope(connection)) {
            error = "FlowMesh v3 checkpoint namespace is malformed";
            return false;
        }
        previous = key.sequence;
    }
    if (!it->StatusOK()) {
        error = "FlowMesh v3 checkpoint namespace scan failed";
        return false;
    }
    return true;
}

bool ValidateLockNamespace(CDBWrapper& db,
                           const FlowMeshProductionStore::Marker& marker,
                           const bool pending_handoff, std::string& error)
{
    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    std::optional<LockKeyType> previous;
    for (it->Seek(uint8_t{KEY_LOCK}); it->Valid(); it->Next()) {
        uint8_t prefix{0};
        if (!it->GetKey(prefix) || prefix != KEY_LOCK) break;
        LockKeyType key;
        uint256 hash;
        if (!it->GetKeyExact(key) || !it->GetValueExact(hash) ||
            key.prefix != KEY_LOCK || hash.IsNull() ||
            (previous && !(*previous < key))) {
            error = "FlowMesh v3 lock namespace is malformed";
            return false;
        }
        const uint64_t epoch{key.epoch};
        const uint64_t sequence{key.sequence};
        if (epoch > marker.current_epoch || sequence > marker.next_sequence ||
            (sequence == marker.next_sequence &&
             (epoch != marker.current_epoch || pending_handoff))) {
            error = "FlowMesh v3 lock lies beyond the authoritative marker";
            return false;
        }
        if (sequence < marker.next_sequence) {
            DiskEntry entry;
            const auto stored{ReadStrict(db, EntryKey(sequence), entry)};
            const auto shape{stored == ReadResult::FOUND
                                 ? DecodeDiskEntryShape(entry)
                                 : std::nullopt};
            if (!shape || shape->epoch != epoch ||
                shape->GetHash() != hash) {
                error = "FlowMesh v3 lock disagrees with its committed entry";
                return false;
            }
        } else {
            DiskLockedCandidate disk;
            const auto stored{ReadStrict(
                db, LockedCandidateKey({epoch, sequence}), disk)};
            const auto candidate{stored == ReadResult::FOUND
                                     ? DecodeDiskLockedCandidate(disk)
                                     : std::nullopt};
            if (!candidate || candidate->entry.GetHash() != hash ||
                candidate->entry.domain != marker.domain ||
                candidate->entry.market_id != marker.market_id ||
                candidate->entry.epoch != marker.current_epoch ||
                candidate->entry.seat_set_hash !=
                    marker.current_seat_set_hash ||
                candidate->entry.sequence != marker.next_sequence ||
                candidate->entry.parent_hash != marker.last_microblock_hash ||
                candidate->entry.previous_state_root != marker.state_root ||
                candidate->entry.effect_start != marker.next_effect_index) {
                error = "FlowMesh v3 current lock has no exact candidate";
                return false;
            }
        }
        previous = key;
    }
    if (!it->StatusOK()) {
        error = "FlowMesh v3 lock namespace scan failed";
        return false;
    }
    return true;
}

bool ValidateLockedCandidateNamespace(
    CDBWrapper& db, const FlowMeshProductionStore::Marker& marker,
    const bool pending_handoff, std::string& error)
{
    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    size_t count{0};
    for (it->Seek(uint8_t{KEY_LOCKED_CANDIDATE}); it->Valid(); it->Next()) {
        uint8_t prefix{0};
        if (!it->GetKey(prefix) || prefix != KEY_LOCKED_CANDIDATE) break;
        LockKeyType key;
        DiskLockedCandidate disk;
        const auto candidate{it->GetKeyExact(key) && it->GetValueExact(disk)
                                 ? DecodeDiskLockedCandidate(disk)
                                 : std::nullopt};
        if (!candidate || key.prefix != KEY_LOCKED_CANDIDATE ||
            ++count != 1 || pending_handoff ||
            key.epoch != marker.current_epoch ||
            key.sequence != marker.next_sequence ||
            candidate->entry.domain != marker.domain ||
            candidate->entry.market_id != marker.market_id ||
            candidate->entry.epoch != key.epoch ||
            candidate->entry.seat_set_hash != marker.current_seat_set_hash ||
            candidate->entry.sequence != key.sequence ||
            candidate->entry.parent_hash != marker.last_microblock_hash ||
            candidate->entry.previous_state_root != marker.state_root ||
            candidate->entry.effect_start != marker.next_effect_index) {
            error = "FlowMesh v3 locked-candidate namespace is malformed";
            return false;
        }
        uint256 locked_hash;
        if (ReadStrict(db, LockKey({key.epoch, key.sequence}), locked_hash) !=
                ReadResult::FOUND ||
            locked_hash != candidate->entry.GetHash()) {
            error = "FlowMesh v3 locked candidate has no matching lock";
            return false;
        }
    }
    if (!it->StatusOK()) {
        error = "FlowMesh v3 locked-candidate namespace scan failed";
        return false;
    }
    return true;
}

bool ValidateStorage(CDBWrapper& db,
                     const FlowMeshProductionStore::Marker& marker,
                     std::string& error)
{
    if (!MarkerShapeIsValid(marker)) {
        error = marker.version == FlowMeshProductionStore::FORMAT_VERSION
                    ? "FlowMesh v3 marker is malformed"
                    : "FlowMesh store is not format v3; migration is unsupported";
        return false;
    }
    {
        std::unique_ptr<CDBIterator> legacy{db.NewIterator()};
        legacy->Seek(uint8_t{KEY_LEGACY_SNAPSHOT});
        if (legacy->Valid()) {
            uint8_t prefix{0};
            if (legacy->GetKey(prefix) && prefix == KEY_LEGACY_SNAPSHOT) {
                error = "FlowMesh v3 refused a legacy snapshot namespace";
                return false;
            }
        }
        if (!legacy->StatusOK()) {
            error = "FlowMesh v3 legacy namespace scan failed";
            return false;
        }
    }
    if (!ValidateConnectionNamespace(db, marker.next_sequence, error)) return false;

    uint64_t expected_sequence{0};
    uint64_t next_effect_index{0};
    uint256 last_hash;
    uint256 running_state_root;
    uint256 checkpoint_head;
    uint64_t active_epoch{0};
    uint256 active_set_hash;
    std::optional<flowmesh::AnchorRef> active_anchor_from_handoff;
    bool have_active_context{false};
    bool pending_handoff{false};

    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    for (it->Seek(uint8_t{KEY_ENTRY}); it->Valid(); it->Next()) {
        uint8_t prefix{0};
        if (!it->GetKey(prefix) || prefix != KEY_ENTRY) break;
        EntryKeyType key;
        DiskEntry disk;
        if (!it->GetKeyExact(key) || !it->GetValueExact(disk) ||
            key.prefix != KEY_ENTRY || key.sequence != expected_sequence ||
            key.sequence >= marker.next_sequence) {
            error = "FlowMesh v3 entry namespace is malformed or non-contiguous";
            return false;
        }
        const auto entry{DecodeDiskEntryShape(disk)};
        if (!entry || entry->domain != marker.domain ||
            entry->market_id != marker.market_id ||
            entry->sequence != expected_sequence ||
            entry->effect_start != next_effect_index ||
            entry->parent_hash != last_hash ||
            (expected_sequence > 0 &&
             entry->previous_state_root != running_state_root)) {
            error = "FlowMesh v3 entry chain is malformed";
            return false;
        }
        if (!have_active_context) {
            active_epoch = entry->epoch;
            active_set_hash = entry->seat_set_hash;
            have_active_context = true;
        }
        if (pending_handoff || entry->epoch != active_epoch ||
            entry->seat_set_hash != active_set_hash) {
            error = "FlowMesh v3 entry crosses an unconnected or wrong epoch";
            return false;
        }

        DiskConnection connection;
        const ReadResult connected{
            ReadStrict(db, ConnectionKey(expected_sequence), connection)};
        if (connected == ReadResult::ERROR) {
            error = "FlowMesh v3 checkpoint record is corrupt";
            return false;
        }
        if (connected == ReadResult::FOUND) {
            modern::FlowMeshCheckpointId next_checkpoint;
            if (!ValidateCheckpointConnection(connection, disk, *entry,
                                               checkpoint_head,
                                               next_checkpoint, error)) {
                return false;
            }
            checkpoint_head = next_checkpoint;
        }

        const bool handoff{
            entry->kind == static_cast<uint8_t>(
                               flowmesh::ProductionEntryKind::EPOCH_HANDOFF)};
        if (handoff) {
            if (connected == ReadResult::FOUND) {
                active_epoch = entry->next_epoch;
                active_set_hash = entry->next_seat_set_hash;
                active_anchor_from_handoff = entry->next_anchor;
            } else {
                pending_handoff = true;
                if (expected_sequence + 1 != marker.next_sequence) {
                    error = "FlowMesh v3 has entries after an unconnected handoff";
                    return false;
                }
            }
        }
        running_state_root = entry->state_root;
        last_hash = entry->GetHash();
        next_effect_index += entry->effect_count;
        ++expected_sequence;
    }
    if (!it->StatusOK()) {
        error = "FlowMesh v3 entry namespace scan failed";
        return false;
    }
    if (expected_sequence != marker.next_sequence) {
        error = "FlowMesh v3 marker points past a missing entry";
        return false;
    }
    if (expected_sequence == 0) {
        if (!marker.last_microblock_hash.IsNull() ||
            !marker.last_b3_checkpoint.IsNull() ||
            marker.next_effect_index != 0) {
            error = "FlowMesh v3 empty log has a nonempty head";
            return false;
        }
    } else if (marker.last_microblock_hash != last_hash ||
               marker.state_root != running_state_root ||
               marker.current_epoch != active_epoch ||
               marker.current_seat_set_hash != active_set_hash ||
               marker.next_effect_index != next_effect_index ||
               marker.last_b3_checkpoint != checkpoint_head ||
               (active_anchor_from_handoff &&
                marker.current_anchor != *active_anchor_from_handoff)) {
        error = "FlowMesh v3 marker disagrees with its durable history";
        return false;
    }
    return ValidateLockNamespace(db, marker, pending_handoff, error) &&
           ValidateLockedCandidateNamespace(db, marker, pending_handoff,
                                            error);
}

bool ReadLastEntryShape(CDBWrapper& db,
                        const FlowMeshProductionStore::Marker& marker,
                        std::optional<flowmesh::ProductionEntryCore>& out,
                        std::string& error)
{
    out.reset();
    if (marker.next_sequence == 0) return true;
    DiskEntry disk;
    if (ReadStrict(db, EntryKey(marker.next_sequence - 1), disk) !=
        ReadResult::FOUND) {
        error = "FlowMesh v3 tip entry is missing or corrupt";
        return false;
    }
    out = DecodeDiskEntryShape(disk);
    if (!out) {
        error = "FlowMesh v3 tip entry is malformed";
        return false;
    }
    return true;
}

bool MarkerHasPendingHandoff(CDBWrapper& db,
                             const FlowMeshProductionStore::Marker& marker,
                             bool& pending, std::string& error)
{
    pending = false;
    std::optional<flowmesh::ProductionEntryCore> tip;
    if (!ReadLastEntryShape(db, marker, tip, error)) return false;
    if (!tip || tip->kind != static_cast<uint8_t>(
                                 flowmesh::ProductionEntryKind::EPOCH_HANDOFF)) {
        return true;
    }
    DiskConnection connection;
    const ReadResult result{
        ReadStrict(db, ConnectionKey(tip->sequence), connection)};
    if (result == ReadResult::ERROR) {
        error = "FlowMesh v3 handoff connection record is corrupt";
        return false;
    }
    pending = result == ReadResult::NOT_FOUND;
    return true;
}

bool PendingLockAllowsAppend(CDBWrapper& db,
                             const flowmesh::ProductionEntryCore& entry,
                             std::string& error)
{
    const flowmesh::ProductionSignPosition position{entry.epoch,
                                                     entry.sequence};
    uint256 locked_hash;
    const ReadResult lock_result{ReadStrict(db, LockKey(position), locked_hash)};
    DiskLockedCandidate disk;
    const ReadResult candidate_result{
        ReadStrict(db, LockedCandidateKey(position), disk)};
    if (lock_result == ReadResult::ERROR ||
        candidate_result == ReadResult::ERROR) {
        error = "FlowMesh v3 pending signing record is corrupt";
        return false;
    }
    if (lock_result == ReadResult::NOT_FOUND) {
        if (candidate_result != ReadResult::NOT_FOUND) {
            error = "FlowMesh v3 has an orphan locked candidate";
            return false;
        }
        return true;
    }
    const auto candidate{candidate_result == ReadResult::FOUND
                             ? DecodeDiskLockedCandidate(disk)
                             : std::nullopt};
    const auto entry_bytes{flowmesh::EncodeProductionEntry(entry)};
    if (!candidate || locked_hash != entry.GetHash() ||
        candidate->entry.GetHash() != locked_hash || !entry_bytes ||
        disk.entry_bytes != *entry_bytes) {
        error = "FlowMesh v3 certificate conflicts with the permanent signing lock";
        return false;
    }
    return true;
}

flowmesh::ProductionAnchorContext AnchorsForNextEntry(
    CDBWrapper& db, const FlowMeshProductionStore::Marker& marker,
    const flowmesh::ProductionAnchorContext& supplied, std::string& error)
{
    flowmesh::ProductionAnchorContext out{supplied};
    if (marker.next_sequence == 0) return out;
    std::optional<flowmesh::ProductionEntryCore> tip;
    if (!ReadLastEntryShape(db, marker, tip, error)) {
        out.policy = nullptr;
        return out;
    }
    out.previous_anchor = tip->anchor;
    return out;
}

bool ReadStoredEntry(CDBWrapper& db, const uint64_t sequence,
                     const flowmesh::ActiveFnBlsSeatSet& seats,
                     StoredProductionEntry& out, std::string& error)
{
    DiskEntry disk;
    const ReadResult result{ReadStrict(db, EntryKey(sequence), disk)};
    if (result == ReadResult::NOT_FOUND) {
        error = "FlowMesh v3 entry is missing";
        return false;
    }
    if (result == ReadResult::ERROR) {
        error = "FlowMesh v3 entry is corrupt";
        return false;
    }
    const auto decoded{DecodeDiskEntry(disk, seats)};
    if (!decoded) {
        error = "FlowMesh v3 entry or certificate failed strict decoding";
        return false;
    }
    out = *decoded;
    return true;
}

bool FindLastConnectionSequence(CDBWrapper& db,
                                std::optional<uint64_t>& sequence_out,
                                std::string& error)
{
    sequence_out.reset();
    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    for (it->Seek(uint8_t{KEY_CONNECTION}); it->Valid(); it->Next()) {
        uint8_t prefix{0};
        if (!it->GetKey(prefix) || prefix != KEY_CONNECTION) break;
        ConnectionKeyType key;
        DiskConnection value;
        if (!it->GetKeyExact(key) || !it->GetValueExact(value) ||
            !DecodeConnectionEnvelope(value)) {
            error = "FlowMesh v3 checkpoint namespace is corrupt";
            return false;
        }
        if (key.prefix != KEY_CONNECTION) {
            error = "FlowMesh v3 checkpoint namespace key is malformed";
            return false;
        }
        sequence_out = key.sequence;
    }
    if (!it->StatusOK()) {
        error = "FlowMesh v3 checkpoint namespace scan failed";
        return false;
    }
    return true;
}

struct ConnectionRecord {
    uint64_t sequence{0};
    DiskConnection disk;
    modern::FlowMeshCheckpointRecordV1 checkpoint;
    modern::FlowMeshCheckpointId checkpoint_id;
};

bool ReadConnectionRecords(CDBWrapper& db,
                           std::vector<ConnectionRecord>& out,
                           std::string& error)
{
    out.clear();
    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    for (it->Seek(uint8_t{KEY_CONNECTION}); it->Valid(); it->Next()) {
        uint8_t prefix{0};
        if (!it->GetKey(prefix) || prefix != KEY_CONNECTION) break;
        ConnectionKeyType key;
        DiskConnection disk;
        if (!it->GetKeyExact(key) || !it->GetValueExact(disk) ||
            key.prefix != KEY_CONNECTION) {
            error = "FlowMesh v3 checkpoint namespace is corrupt";
            return false;
        }
        const auto checkpoint{DecodeConnectionEnvelope(disk)};
        const auto checkpoint_id{
            checkpoint ? modern::FlowMeshCheckpointIdV1(checkpoint->core)
                       : std::nullopt};
        if (!checkpoint || !checkpoint_id || disk.connected_height < 0 ||
            disk.connected_block.IsNull()) {
            error = "FlowMesh v3 checkpoint connection is malformed";
            return false;
        }
        out.push_back(ConnectionRecord{key.sequence, std::move(disk),
                                       *checkpoint, *checkpoint_id});
    }
    if (!it->StatusOK()) {
        error = "FlowMesh v3 checkpoint namespace scan failed";
        return false;
    }
    return true;
}

} // namespace

FlowMeshProductionStore::FlowMeshProductionStore(DBParams db_params)
    : m_db{std::move(db_params)}
{
}

bool FlowMeshProductionStore::ReadMarker(std::optional<Marker>& out,
                                         std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    out.reset();
    Marker marker;
    switch (ReadMarkerStrict(m_db, marker)) {
    case ReadResult::NOT_FOUND: return true;
    case ReadResult::ERROR:
        error = "FlowMesh v3 marker is corrupt or unreadable";
        return false;
    case ReadResult::FOUND: break;
    }
    if (!MarkerShapeIsValid(marker)) {
        error = marker.version == FORMAT_VERSION
                    ? "FlowMesh v3 marker is malformed"
                    : "FlowMesh store is not format v3; migration is unsupported";
        return false;
    }
    out = marker;
    return true;
}

bool FlowMeshProductionStore::ConnectedB3Heights(std::vector<int32_t>& out,
                                                  std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    out.clear();
    Marker marker;
    const ReadResult marker_result{ReadMarkerStrict(m_db, marker)};
    if (marker_result == ReadResult::NOT_FOUND) {
        return NamespaceIsEmpty(m_db, error);
    }
    if (marker_result == ReadResult::ERROR ||
        !ValidateStorage(m_db, marker, error)) {
        if (error.empty()) error = "FlowMesh v3 marker is corrupt or unreadable";
        return false;
    }
    std::vector<ConnectionRecord> records;
    if (!ReadConnectionRecords(m_db, records, error)) return false;
    for (const ConnectionRecord& record : records) {
        if (out.empty() || out.back() != record.disk.connected_height) {
            out.push_back(record.disk.connected_height);
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return true;
}

bool FlowMeshProductionStore::ReconcileCheckpointConnections(
    const std::map<int32_t, uint256>& canonical_blocks, bool& rolled_back,
    std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    rolled_back = false;
    const bool ready_before{m_ready};
    Marker marker;
    const ReadResult marker_result{ReadMarkerStrict(m_db, marker)};
    if (marker_result == ReadResult::NOT_FOUND) {
        if (!NamespaceIsEmpty(m_db, error)) {
            m_ready = false;
            return false;
        }
        return true;
    }
    if (marker_result == ReadResult::ERROR ||
        !ValidateStorage(m_db, marker, error)) {
        m_ready = false;
        if (error.empty()) error = "FlowMesh v3 marker is corrupt or unreadable";
        return false;
    }

    std::vector<ConnectionRecord> records;
    if (!ReadConnectionRecords(m_db, records, error)) {
        m_ready = false;
        return false;
    }
    std::optional<size_t> first_disconnected;
    for (size_t i{0}; i < records.size(); ++i) {
        const auto observed{canonical_blocks.find(
            records[i].disk.connected_height)};
        if (observed == canonical_blocks.end()) {
            m_ready = false;
            error = "FlowMesh v3 canonical B3 snapshot is incomplete";
            return false;
        }
        if (observed->second != records[i].disk.connected_block) {
            first_disconnected = i;
            break;
        }
    }
    if (!first_disconnected) {
        m_ready = ready_before;
        return true;
    }

    Marker next{marker};
    next.last_b3_checkpoint =
        records[*first_disconnected].checkpoint.core.previous_checkpoint_id;

    // The first removed handoff is the exact boundary whose outgoing marker
    // must be restored. Entries after it were certified by the now-orphaned
    // incoming committee and cannot be discarded or reinterpreted safely.
    for (size_t i{*first_disconnected}; i < records.size(); ++i) {
        const auto& core{records[i].checkpoint.core};
        if (core.kind != modern::FlowMeshCheckpointKind::EPOCH_HANDOFF) {
            continue;
        }
        if (marker.next_sequence != records[i].sequence + 1 ||
            core.anchor.height >
                static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) ||
            core.anchor.block_hash.IsNull() || core.seat_set_hash.IsNull()) {
            m_ready = false;
            error = "FlowMesh v3 cannot safely roll back a handoff after incoming-epoch entries";
            return false;
        }
        next.current_epoch = core.epoch;
        next.current_anchor = {
            static_cast<int32_t>(core.anchor.height), core.anchor.block_hash};
        next.current_seat_set_hash = core.seat_set_hash;
        break;
    }

    try {
        CDBBatch batch{m_db};
        for (size_t i{*first_disconnected}; i < records.size(); ++i) {
            batch.Erase(ConnectionKey(records[i].sequence));
        }
        batch.Write(KEY_MARKER, next);
        m_db.WriteBatch(batch, /*fSync=*/true);
    } catch (const std::exception& e) {
        m_ready = false;
        error = std::string{"FlowMesh v3 checkpoint rollback failed: "} +
                e.what();
        return false;
    }
    if (!ValidateStorage(m_db, next, error)) {
        m_ready = false;
        if (error.empty()) {
            error = "FlowMesh v3 checkpoint rollback produced invalid storage";
        }
        return false;
    }
    rolled_back = true;
    m_ready = ready_before;
    return true;
}

bool FlowMeshProductionStore::CheckForMarket(
    const uint256& domain, const flowmesh::MarketId& market_id,
    bool& fresh_out, std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    fresh_out = false;
    if (domain.IsNull() || market_id.IsNull()) {
        error = "FlowMesh v3 refused a null domain or market";
        return false;
    }
    Marker marker;
    const ReadResult result{ReadMarkerStrict(m_db, marker)};
    if (result == ReadResult::ERROR) {
        error = "FlowMesh v3 marker is corrupt or unreadable";
        return false;
    }
    if (result == ReadResult::NOT_FOUND) {
        if (!NamespaceIsEmpty(m_db, error)) return false;
        fresh_out = true;
        return true;
    }
    if (marker.version != FORMAT_VERSION) {
        error = "FlowMesh store is not format v3; migration is unsupported";
        return false;
    }
    if (marker.domain != domain || marker.market_id != market_id) {
        error = "FlowMesh v3 store belongs to a different domain or market";
        return false;
    }
    return ValidateStorage(m_db, marker, error);
}

bool FlowMeshProductionStore::OpenForMarket(
    const uint256& domain, const flowmesh::MarketId& market_id,
    const flowmesh::ActiveFnBlsSeatSet& initial_seats,
    const uint256& initial_state_root, std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    m_open = false;
    m_ready = false;
    if (domain.IsNull() || market_id.IsNull() || initial_state_root.IsNull()) {
        error = "FlowMesh v3 refused a null initial binding";
        return false;
    }
    const auto initial_anchor{SeatAnchor(initial_seats)};
    if (!initial_anchor || initial_seats.market_id != market_id ||
        flowmesh::CheckActiveFnBlsSeatSet(domain, initial_seats) !=
            flowmesh::BlsSeatSetCheck::OK) {
        error = "FlowMesh v3 refused an invalid initial seat set";
        return false;
    }

    Marker marker;
    const ReadResult result{ReadMarkerStrict(m_db, marker)};
    if (result == ReadResult::ERROR) {
        error = "FlowMesh v3 marker is corrupt or unreadable";
        return false;
    }
    if (result == ReadResult::NOT_FOUND) {
        if (!NamespaceIsEmpty(m_db, error)) return false;
        marker.domain = domain;
        marker.market_id = market_id;
        marker.current_epoch = initial_seats.epoch;
        marker.current_anchor = *initial_anchor;
        marker.current_seat_set_hash = initial_seats.set_hash;
        marker.state_root = initial_state_root;
        try {
            CDBBatch batch{m_db};
            batch.Write(KEY_MARKER, marker);
            m_db.WriteBatch(batch, /*fSync=*/true);
        } catch (const std::exception& e) {
            error = std::string{"FlowMesh v3 initialization failed: "} + e.what();
            return false;
        }
        m_open = true;
        m_ready = true;
        return true;
    }
    if (marker.version != FORMAT_VERSION) {
        error = "FlowMesh store is not format v3; migration is unsupported";
        return false;
    }
    if (marker.domain != domain || marker.market_id != market_id) {
        error = "FlowMesh v3 store belongs to a different domain or market";
        return false;
    }
    if (!ValidateStorage(m_db, marker, error)) return false;
    if (marker.next_sequence == 0 &&
        (!MarkerMatchesSeatSet(marker, initial_seats) ||
         marker.state_root != initial_state_root)) {
        error = "FlowMesh v3 empty store has a different initial binding";
        return false;
    }
    m_open = true;
    m_ready = marker.next_sequence == 0;
    return true;
}

bool FlowMeshProductionStore::AppendExecution(
    const flowmesh::ProductionEntryCore& entry,
    const flowmesh::BlsMicroblockCertificate& certificate,
    const flowmesh::ActiveFnBlsSeatSet& active_seats,
    const flowmesh::FlowMeshState& current_state,
    const flowmesh::ProductionAnchorContext& anchor_context,
    const uint256& treasury_owner_commitment,
    const flowmesh::DepositVerifier* deposits,
    flowmesh::FlowMeshState& next_state_out, std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    if (!m_open || !m_ready) {
        error = "FlowMesh v3 store has not completed startup replay";
        return false;
    }
    Marker marker;
    if (ReadMarkerStrict(m_db, marker) != ReadResult::FOUND ||
        !MarkerShapeIsValid(marker)) {
        error = "FlowMesh v3 marker is missing or corrupt";
        return false;
    }
    bool pending{false};
    if (!MarkerHasPendingHandoff(m_db, marker, pending, error)) return false;
    if (pending) {
        error = "FlowMesh v3 cannot execute before the handoff checkpoint connects";
        return false;
    }
    if (!MarkerMatchesSeatSet(marker, active_seats) ||
        marker.state_root != current_state.Root()) {
        error = "FlowMesh v3 execution does not match the active marker state";
        return false;
    }
    if (marker.next_sequence == std::numeric_limits<uint64_t>::max()) {
        error = "FlowMesh v3 sequence space is exhausted";
        return false;
    }
    if (!PendingLockAllowsAppend(m_db, entry, error)) return false;
    flowmesh::ProductionAnchorContext exact_anchors{
        AnchorsForNextEntry(m_db, marker, anchor_context, error)};
    if (exact_anchors.policy == nullptr) {
        if (error.empty()) error = "FlowMesh v3 execution requires an anchor policy";
        return false;
    }
    flowmesh::ProductionEpochGate gate{marker.domain, marker.market_id,
                                       active_seats};
    flowmesh::ProductionEntryCheck check;
    const auto executed{flowmesh::ExecuteProductionEntry(
        current_state, entry, marker.domain, marker.market_id, active_seats,
        gate, marker.next_sequence, marker.next_effect_index,
        marker.last_microblock_hash,
        exact_anchors, treasury_owner_commitment, deposits, check)};
    if (!executed) {
        error = std::string{"FlowMesh v3 execution entry failed: "} +
                flowmesh::ProductionEntryCheckName(check);
        return false;
    }
    if (flowmesh::CheckProductionEntryCertificate(entry, active_seats,
                                                   certificate) !=
        flowmesh::BlsCertificateCheck::OK) {
        error = "FlowMesh v3 execution certificate is invalid";
        return false;
    }
    const auto disk{MakeDiskEntry(entry, certificate, active_seats.Size(),
                                  executed->effects,
                                  executed->settlements)};
    if (!disk) {
        error = "FlowMesh v3 execution record is not encodable";
        return false;
    }

    Marker next{marker};
    ++next.next_sequence;
    next.next_effect_index += entry.effect_count;
    next.last_microblock_hash = entry.GetHash();
    next.state_root = entry.state_root;
    try {
        CDBBatch batch{m_db};
        batch.Write(EntryKey(entry.sequence), *disk);
        batch.Erase(LockedCandidateKey({entry.epoch, entry.sequence}));
        batch.Write(KEY_MARKER, next);
        m_db.WriteBatch(batch, /*fSync=*/true);
    } catch (const std::exception& e) {
        error = std::string{"FlowMesh v3 execution append failed: "} + e.what();
        return false;
    }
    next_state_out = executed->next_state;
    return true;
}

bool FlowMeshProductionStore::AppendHandoff(
    const flowmesh::ProductionEntryCore& handoff,
    const flowmesh::BlsMicroblockCertificate& certificate,
    const flowmesh::ActiveFnBlsSeatSet& outgoing_seats,
    const flowmesh::ActiveFnBlsSeatSet& next_seats,
    const flowmesh::FlowMeshState& current_state,
    const flowmesh::ProductionAnchorContext& anchor_context,
    std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    if (!m_open || !m_ready) {
        error = "FlowMesh v3 store has not completed startup replay";
        return false;
    }
    Marker marker;
    if (ReadMarkerStrict(m_db, marker) != ReadResult::FOUND ||
        !MarkerShapeIsValid(marker)) {
        error = "FlowMesh v3 marker is missing or corrupt";
        return false;
    }
    bool pending{false};
    if (!MarkerHasPendingHandoff(m_db, marker, pending, error)) return false;
    if (pending) {
        error = "FlowMesh v3 already has a pending handoff";
        return false;
    }
    if (!MarkerMatchesSeatSet(marker, outgoing_seats) ||
        marker.state_root != current_state.Root() ||
        marker.next_sequence == std::numeric_limits<uint64_t>::max()) {
        error = "FlowMesh v3 handoff does not match the active marker";
        return false;
    }
    if (!PendingLockAllowsAppend(m_db, handoff, error)) return false;
    if (handoff.anchor.height < 0 ||
        handoff.next_anchor.height <= handoff.anchor.height) {
        error = "FlowMesh v3 handoff cannot be represented by a canonical B3 checkpoint";
        return false;
    }
    flowmesh::ProductionAnchorContext exact_anchors{
        AnchorsForNextEntry(m_db, marker, anchor_context, error)};
    if (exact_anchors.policy == nullptr) {
        if (error.empty()) error = "FlowMesh v3 handoff requires an anchor policy";
        return false;
    }
    flowmesh::ProductionEpochGate gate{marker.domain, marker.market_id,
                                       outgoing_seats};
    const flowmesh::ProductionEntryCheck check{gate.StageHandoff(
        current_state, handoff, outgoing_seats, next_seats, certificate,
        marker.next_sequence, marker.next_effect_index,
        marker.last_microblock_hash, exact_anchors)};
    if (check != flowmesh::ProductionEntryCheck::OK) {
        error = std::string{"FlowMesh v3 handoff failed: "} +
                flowmesh::ProductionEntryCheckName(check);
        return false;
    }
    const auto disk{MakeDiskEntry(
        handoff, certificate, outgoing_seats.Size(),
        std::span<const modern::FlowMeshEffectV1>{},
        std::span<const flowmesh::WithdrawalSettlementFactV1>{})};
    if (!disk) {
        error = "FlowMesh v3 handoff record is not encodable";
        return false;
    }
    Marker next{marker};
    ++next.next_sequence;
    next.next_effect_index += handoff.effect_count;
    next.last_microblock_hash = handoff.GetHash();
    next.state_root = handoff.state_root;
    // Epoch/anchor/set deliberately remain outgoing until the checkpoint
    // connection record and transition marker commit together.
    try {
        CDBBatch batch{m_db};
        batch.Write(EntryKey(handoff.sequence), *disk);
        batch.Erase(LockedCandidateKey({handoff.epoch, handoff.sequence}));
        batch.Write(KEY_MARKER, next);
        m_db.WriteBatch(batch, /*fSync=*/true);
    } catch (const std::exception& e) {
        error = std::string{"FlowMesh v3 handoff append failed: "} + e.what();
        return false;
    }
    return true;
}

bool FlowMeshProductionStore::MarkExecutionCheckpointConnected(
    const modern::FlowMeshCheckpointRecordV1& checkpoint,
    const flowmesh::ActiveFnBlsSeatSet& active_seats,
    const ProductionB3Connection& connection, std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    if (!m_open || !m_ready) {
        error = "FlowMesh v3 store has not completed startup replay";
        return false;
    }
    Marker marker;
    if (ReadMarkerStrict(m_db, marker) != ReadResult::FOUND ||
        !MarkerShapeIsValid(marker) || !MarkerMatchesSeatSet(marker, active_seats)) {
        error = "FlowMesh v3 checkpoint does not match the active marker";
        return false;
    }
    if (checkpoint.core.kind != modern::FlowMeshCheckpointKind::EXECUTION ||
        checkpoint.core.sequence >= marker.next_sequence ||
        checkpoint.core.previous_checkpoint_id != marker.last_b3_checkpoint) {
        error = "FlowMesh v3 execution checkpoint is stale or out of order";
        return false;
    }
    StoredProductionEntry stored;
    if (!ReadStoredEntry(m_db, checkpoint.core.sequence, active_seats, stored,
                         error)) {
        return false;
    }
    if (!CheckpointMatchesEntry(checkpoint, stored, active_seats)) {
        error = "FlowMesh v3 checkpoint does not match its entry";
        return false;
    }
    std::optional<uint64_t> last_connection;
    if (!FindLastConnectionSequence(m_db, last_connection, error)) return false;
    if (last_connection && checkpoint.core.sequence <= *last_connection) {
        error = "FlowMesh v3 checkpoint sequence is not increasing";
        return false;
    }
    DiskConnection existing;
    if (ReadStrict(m_db, ConnectionKey(checkpoint.core.sequence), existing) !=
        ReadResult::NOT_FOUND) {
        error = "FlowMesh v3 checkpoint slot is occupied or corrupt";
        return false;
    }
    const auto disk{
        MakeDiskConnection(checkpoint, active_seats.Size(), connection)};
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(checkpoint.core)};
    if (!disk || !checkpoint_id) {
        error = "FlowMesh v3 execution checkpoint is not canonical";
        return false;
    }
    Marker next{marker};
    next.last_b3_checkpoint = *checkpoint_id;
    try {
        CDBBatch batch{m_db};
        batch.Write(ConnectionKey(checkpoint.core.sequence), *disk);
        batch.Write(KEY_MARKER, next);
        m_db.WriteBatch(batch, /*fSync=*/true);
    } catch (const std::exception& e) {
        error = std::string{"FlowMesh v3 checkpoint commit failed: "} + e.what();
        return false;
    }
    return true;
}

bool FlowMeshProductionStore::MarkHandoffCheckpointConnected(
    const modern::FlowMeshCheckpointRecordV1& checkpoint,
    const flowmesh::ActiveFnBlsSeatSet& outgoing_seats,
    const flowmesh::ActiveFnBlsSeatSet& next_seats,
    const ProductionB3Connection& connection,
    const int32_t canonical_tip_height, std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    if (!m_open || !m_ready) {
        error = "FlowMesh v3 store has not completed startup replay";
        return false;
    }
    if (!FlowMeshHandoffConnectionMature(connection,
                                         canonical_tip_height)) {
        error = "FlowMesh v3 handoff publication is not deep enough";
        return false;
    }
    Marker marker;
    if (ReadMarkerStrict(m_db, marker) != ReadResult::FOUND ||
        !MarkerShapeIsValid(marker) ||
        !MarkerMatchesSeatSet(marker, outgoing_seats) ||
        checkpoint.core.kind != modern::FlowMeshCheckpointKind::EPOCH_HANDOFF ||
        marker.next_sequence == 0 ||
        checkpoint.core.sequence != marker.next_sequence - 1 ||
        checkpoint.core.previous_checkpoint_id != marker.last_b3_checkpoint) {
        error = "FlowMesh v3 handoff checkpoint is stale or out of order";
        return false;
    }
    StoredProductionEntry stored;
    if (!ReadStoredEntry(m_db, checkpoint.core.sequence, outgoing_seats,
                         stored, error)) {
        return false;
    }
    if (stored.entry.kind != static_cast<uint8_t>(
                                 flowmesh::ProductionEntryKind::EPOCH_HANDOFF) ||
        marker.last_microblock_hash != stored.entry.GetHash() ||
        marker.state_root != stored.entry.state_root ||
        !CheckpointMatchesEntry(checkpoint, stored, outgoing_seats)) {
        error = "FlowMesh v3 checkpoint does not match its handoff";
        return false;
    }
    const auto next_anchor{SeatAnchor(next_seats)};
    if (!next_anchor ||
        flowmesh::CheckActiveFnBlsSeatSet(marker.domain, next_seats) !=
            flowmesh::BlsSeatSetCheck::OK ||
        next_seats.market_id != marker.market_id ||
        next_seats.epoch != stored.entry.next_epoch ||
        next_seats.set_hash != stored.entry.next_seat_set_hash ||
        *next_anchor != stored.entry.next_anchor) {
        error = "FlowMesh v3 checkpoint names a different next seat set";
        return false;
    }
    std::optional<uint64_t> last_connection;
    if (!FindLastConnectionSequence(m_db, last_connection, error)) return false;
    if (last_connection && checkpoint.core.sequence <= *last_connection) {
        error = "FlowMesh v3 checkpoint sequence is not increasing";
        return false;
    }
    DiskConnection existing;
    if (ReadStrict(m_db, ConnectionKey(checkpoint.core.sequence), existing) !=
        ReadResult::NOT_FOUND) {
        error = "FlowMesh v3 handoff checkpoint slot is occupied or corrupt";
        return false;
    }
    const auto disk{
        MakeDiskConnection(checkpoint, outgoing_seats.Size(), connection)};
    const auto checkpoint_id{modern::FlowMeshCheckpointIdV1(checkpoint.core)};
    if (!disk || !checkpoint_id) {
        error = "FlowMesh v3 handoff checkpoint is not canonical";
        return false;
    }

    Marker next{marker};
    next.current_epoch = next_seats.epoch;
    next.current_anchor = *next_anchor;
    next.current_seat_set_hash = next_seats.set_hash;
    next.last_b3_checkpoint = *checkpoint_id;
    try {
        CDBBatch batch{m_db};
        batch.Write(ConnectionKey(checkpoint.core.sequence), *disk);
        batch.Write(KEY_MARKER, next);
        m_db.WriteBatch(batch, /*fSync=*/true);
    } catch (const std::exception& e) {
        error = std::string{"FlowMesh v3 handoff transition failed: "} + e.what();
        return false;
    }
    return true;
}

bool FlowMeshProductionStore::ReadEntry(
    const uint64_t sequence,
    const flowmesh::ActiveFnBlsSeatSet& active_seats,
    std::optional<StoredProductionEntry>& out, std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    out.reset();
    if (!m_open) {
        error = "FlowMesh v3 store is not open";
        return false;
    }
    Marker marker;
    if (ReadMarkerStrict(m_db, marker) != ReadResult::FOUND ||
        sequence >= marker.next_sequence ||
        flowmesh::CheckActiveFnBlsSeatSet(marker.domain, active_seats) !=
            flowmesh::BlsSeatSetCheck::OK) {
        error = "FlowMesh v3 entry request is outside the active database";
        return false;
    }
    StoredProductionEntry stored;
    if (!ReadStoredEntry(m_db, sequence, active_seats, stored, error)) return false;
    out = std::move(stored);
    return true;
}

bool FlowMeshProductionStore::NextCheckpointCandidate(
    const flowmesh::ActiveFnBlsSeatSet& active_seats,
    std::optional<ProductionCheckpointCandidate>& out, std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    out.reset();
    if (!m_open || !m_ready) {
        error = "FlowMesh v3 store has not completed startup replay";
        return false;
    }
    Marker marker;
    if (ReadMarkerStrict(m_db, marker) != ReadResult::FOUND ||
        !MarkerShapeIsValid(marker) ||
        !MarkerMatchesSeatSet(marker, active_seats)) {
        error = "FlowMesh v3 checkpoint backlog does not match the active marker";
        return false;
    }
    std::optional<uint64_t> last_connection;
    if (!FindLastConnectionSequence(m_db, last_connection, error)) return false;
    const uint64_t start{last_connection ? *last_connection + 1 : 0};
    for (uint64_t sequence{start}; sequence < marker.next_sequence;
         ++sequence) {
        StoredProductionEntry stored;
        if (!ReadStoredEntry(m_db, sequence, active_seats, stored, error)) {
            return false;
        }
        const bool handoff{
            stored.entry.kind == static_cast<uint8_t>(
                                     flowmesh::ProductionEntryKind::EPOCH_HANDOFF)};
        const bool market_genesis{!last_connection && sequence == 0};
        if (!market_genesis && !handoff && stored.entry.effect_count == 0 &&
            stored.settlements.empty()) {
            continue;
        }
        out = ProductionCheckpointCandidate{
            std::move(stored), marker.last_b3_checkpoint};
        return true;
    }
    return true;
}

bool FlowMeshProductionStore::Replay(
    flowmesh::FlowMeshState& state, uint256& last_hash,
    const ProductionSeatSetSource& seat_sets,
    const flowmesh::ProductionAnchorContext& anchor_context,
    const uint256& treasury_owner_commitment,
    const flowmesh::DepositVerifier* deposits, std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    if (!m_open) {
        error = "FlowMesh v3 store is not open";
        return false;
    }
    // Replay is the signing/append readiness boundary. Any failure below
    // leaves the store fail-closed until a complete replay succeeds.
    m_ready = false;
    Marker marker;
    if (ReadMarkerStrict(m_db, marker) != ReadResult::FOUND ||
        !ValidateStorage(m_db, marker, error)) {
        if (error.empty()) error = "FlowMesh v3 marker is missing or corrupt";
        return false;
    }
    flowmesh::FlowMeshState working{state};
    uint256 running_hash;
    uint64_t next_effect_index{0};
    uint256 checkpoint_head;
    std::optional<flowmesh::ActiveFnBlsSeatSet> active_seats;
    flowmesh::ProductionAnchorContext anchors{anchor_context};

    for (uint64_t sequence{0}; sequence < marker.next_sequence; ++sequence) {
        DiskEntry disk;
        if (ReadStrict(m_db, EntryKey(sequence), disk) != ReadResult::FOUND) {
            error = "FlowMesh v3 replay entry is missing or corrupt";
            return false;
        }
        const auto shape{DecodeDiskEntryShape(disk)};
        if (!shape) {
            error = "FlowMesh v3 replay entry failed strict decoding";
            return false;
        }
        if (!active_seats || active_seats->epoch != shape->epoch ||
            active_seats->set_hash != shape->seat_set_hash) {
            active_seats = seat_sets.GetSeatSet(
                marker.domain, marker.market_id, shape->epoch,
                shape->seat_set_hash);
        }
        if (!active_seats ||
            flowmesh::CheckActiveFnBlsSeatSet(marker.domain, *active_seats) !=
                flowmesh::BlsSeatSetCheck::OK ||
            active_seats->market_id != marker.market_id) {
            error = "FlowMesh v3 replay cannot resolve the active seat set";
            return false;
        }
        const auto stored{DecodeDiskEntry(disk, *active_seats)};
        if (!stored) {
            error = "FlowMesh v3 replay certificate is invalid";
            return false;
        }

        flowmesh::ProductionEpochGate gate{marker.domain, marker.market_id,
                                           *active_seats};
        if (stored->entry.kind == static_cast<uint8_t>(
                                      flowmesh::ProductionEntryKind::EXECUTION)) {
            flowmesh::ProductionEntryCheck check;
            const auto executed{flowmesh::ExecuteProductionEntry(
                working, stored->entry, marker.domain, marker.market_id,
                *active_seats, gate, sequence, next_effect_index,
                running_hash, anchors,
                treasury_owner_commitment, deposits, check)};
            if (!executed) {
                error = std::string{"FlowMesh v3 replay execution failed: "} +
                        flowmesh::ProductionEntryCheckName(check);
                return false;
            }
            if (executed->effects != stored->effects ||
                executed->settlements != stored->settlements) {
                error = "FlowMesh v3 replay effects/settlements differ from deterministic execution";
                return false;
            }
            working = executed->next_state;
        } else {
            const auto next_seats{seat_sets.GetSeatSet(
                marker.domain, marker.market_id, stored->entry.next_epoch,
                stored->entry.next_seat_set_hash)};
            if (!next_seats) {
                error = "FlowMesh v3 replay cannot resolve the handoff seat set";
                return false;
            }
            const flowmesh::ProductionEntryCheck check{gate.StageHandoff(
                working, stored->entry, *active_seats, *next_seats,
                stored->certificate, sequence, next_effect_index,
                running_hash, anchors)};
            if (check != flowmesh::ProductionEntryCheck::OK) {
                error = std::string{"FlowMesh v3 replay handoff failed: "} +
                        flowmesh::ProductionEntryCheckName(check);
                return false;
            }

            DiskConnection connection;
            const ReadResult connected{
                ReadStrict(m_db, ConnectionKey(sequence), connection)};
            if (connected == ReadResult::ERROR) {
                error = "FlowMesh v3 replay checkpoint is corrupt";
                return false;
            }
            if (connected == ReadResult::FOUND) {
                const auto checkpoint{modern::DecodeFlowMeshCheckpointRecordV1(
                    connection.checkpoint_bytes, active_seats->Size())};
                const auto next_checkpoint{
                    checkpoint ? modern::FlowMeshCheckpointIdV1(checkpoint->core)
                               : std::nullopt};
                if (!checkpoint ||
                    !CheckpointMatchesEntry(*checkpoint, *stored,
                                            *active_seats) ||
                    checkpoint->core.previous_checkpoint_id != checkpoint_head ||
                    !next_checkpoint) {
                    error = "FlowMesh v3 replay handoff checkpoint is invalid";
                    return false;
                }
                checkpoint_head = *next_checkpoint;
                active_seats = *next_seats;
            } else if (sequence + 1 != marker.next_sequence) {
                error = "FlowMesh v3 replay found an unconnected historical handoff";
                return false;
            }
        }

        // Ordinary execution checkpoints are optional and update the same
        // monotonic checkpoint chain when present.
        if (stored->entry.kind == static_cast<uint8_t>(
                                      flowmesh::ProductionEntryKind::EXECUTION)) {
            DiskConnection connection;
            const ReadResult connected{
                ReadStrict(m_db, ConnectionKey(sequence), connection)};
            if (connected == ReadResult::ERROR) {
                error = "FlowMesh v3 replay checkpoint is corrupt";
                return false;
            }
            if (connected == ReadResult::FOUND) {
                const auto checkpoint{modern::DecodeFlowMeshCheckpointRecordV1(
                    connection.checkpoint_bytes, active_seats->Size())};
                const auto id{checkpoint
                                  ? modern::FlowMeshCheckpointIdV1(checkpoint->core)
                                  : std::nullopt};
                if (!checkpoint || !id ||
                    !CheckpointMatchesEntry(*checkpoint, *stored,
                                            *active_seats) ||
                    checkpoint->core.previous_checkpoint_id != checkpoint_head) {
                    error = "FlowMesh v3 replay execution checkpoint is invalid";
                    return false;
                }
                checkpoint_head = *id;
            }
        }
        running_hash = stored->entry.GetHash();
        next_effect_index += stored->entry.effect_count;
        anchors.previous_anchor = stored->entry.anchor;
    }

    if (marker.next_sequence == 0) {
        active_seats = seat_sets.GetSeatSet(
            marker.domain, marker.market_id, marker.current_epoch,
            marker.current_seat_set_hash);
    }
    if (!active_seats || !MarkerMatchesSeatSet(marker, *active_seats) ||
        working.Root() != marker.state_root ||
        running_hash != marker.last_microblock_hash ||
        next_effect_index != marker.next_effect_index ||
        checkpoint_head != marker.last_b3_checkpoint) {
        error = "FlowMesh v3 replay result disagrees with the marker";
        return false;
    }
    state = std::move(working);
    last_hash = running_hash;
    m_ready = true;
    return true;
}

flowmesh::ProductionLockResult FlowMeshProductionStore::LockCandidate(
    const flowmesh::ProductionEntryCore& entry,
    const std::span<const flowmesh::Action> authenticated_evidence)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    const uint256 entry_hash{entry.GetHash()};
    if (!m_open || !m_ready || entry_hash.IsNull()) {
        return flowmesh::ProductionLockResult::STORAGE_FAILURE;
    }
    const auto disk{MakeDiskLockedCandidate(entry, authenticated_evidence)};
    if (!disk) return flowmesh::ProductionLockResult::STORAGE_FAILURE;
    Marker marker;
    if (ReadMarkerStrict(m_db, marker) != ReadResult::FOUND ||
        !MarkerShapeIsValid(marker)) {
        return flowmesh::ProductionLockResult::STORAGE_FAILURE;
    }
    bool pending{false};
    std::string error;
    if (!MarkerHasPendingHandoff(m_db, marker, pending, error) || pending ||
        entry.domain != marker.domain || entry.market_id != marker.market_id ||
        entry.epoch != marker.current_epoch ||
        entry.seat_set_hash != marker.current_seat_set_hash ||
        entry.sequence != marker.next_sequence ||
        entry.parent_hash != marker.last_microblock_hash ||
        entry.previous_state_root != marker.state_root ||
        entry.effect_start != marker.next_effect_index) {
        return flowmesh::ProductionLockResult::STORAGE_FAILURE;
    }
    const flowmesh::ProductionSignPosition position{entry.epoch,
                                                     entry.sequence};
    uint256 existing;
    switch (ReadStrict(m_db, LockKey(position), existing)) {
    case ReadResult::FOUND: {
        if (existing != entry_hash) {
            return flowmesh::ProductionLockResult::CONFLICT;
        }
        DiskLockedCandidate existing_disk;
        const auto retained{
            ReadStrict(m_db, LockedCandidateKey(position), existing_disk) ==
                    ReadResult::FOUND
                ? DecodeDiskLockedCandidate(existing_disk)
                : std::nullopt};
        return retained && retained->entry.GetHash() == existing &&
                       retained->entry.epoch == position.epoch &&
                       retained->entry.sequence == position.sequence
                   ? flowmesh::ProductionLockResult::ALREADY_LOCKED_SAME
                   : flowmesh::ProductionLockResult::STORAGE_FAILURE;
    }
    case ReadResult::ERROR:
        return flowmesh::ProductionLockResult::STORAGE_FAILURE;
    case ReadResult::NOT_FOUND:
        break;
    }
    DiskLockedCandidate orphan;
    if (ReadStrict(m_db, LockedCandidateKey(position), orphan) !=
        ReadResult::NOT_FOUND) {
        return flowmesh::ProductionLockResult::STORAGE_FAILURE;
    }
    try {
        CDBBatch batch{m_db};
        batch.Write(LockKey(position), entry_hash);
        batch.Write(LockedCandidateKey(position), *disk);
        m_db.WriteBatch(batch, /*fSync=*/true);
    } catch (const std::exception&) {
        return flowmesh::ProductionLockResult::STORAGE_FAILURE;
    }
    return flowmesh::ProductionLockResult::LOCKED;
}

flowmesh::ProductionLockResult FlowMeshProductionStore::LockOnce(
    const flowmesh::ProductionSignPosition& position,
    const uint256& entry_hash)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    if (!m_open || !m_ready || entry_hash.IsNull()) {
        return flowmesh::ProductionLockResult::STORAGE_FAILURE;
    }
    Marker marker;
    if (ReadMarkerStrict(m_db, marker) != ReadResult::FOUND ||
        !MarkerShapeIsValid(marker)) {
        return flowmesh::ProductionLockResult::STORAGE_FAILURE;
    }
    bool pending{false};
    std::string error;
    if (!MarkerHasPendingHandoff(m_db, marker, pending, error) || pending ||
        position.epoch != marker.current_epoch ||
        position.sequence != marker.next_sequence) {
        return flowmesh::ProductionLockResult::STORAGE_FAILURE;
    }
    uint256 existing;
    const ReadResult result{ReadStrict(m_db, LockKey(position), existing)};
    if (result == ReadResult::ERROR || result == ReadResult::NOT_FOUND) {
        // A real production store may never create a hash-only first lock.
        return flowmesh::ProductionLockResult::STORAGE_FAILURE;
    }
    if (existing != entry_hash) return flowmesh::ProductionLockResult::CONFLICT;
    DiskLockedCandidate disk;
    const auto retained{
        ReadStrict(m_db, LockedCandidateKey(position), disk) ==
                ReadResult::FOUND
            ? DecodeDiskLockedCandidate(disk)
            : std::nullopt};
    return retained && retained->entry.GetHash() == existing &&
                   retained->entry.epoch == position.epoch &&
                   retained->entry.sequence == position.sequence &&
                   retained->entry.domain == marker.domain &&
                   retained->entry.market_id == marker.market_id &&
                   retained->entry.seat_set_hash ==
                       marker.current_seat_set_hash &&
                   retained->entry.parent_hash == marker.last_microblock_hash &&
                   retained->entry.previous_state_root == marker.state_root &&
                   retained->entry.effect_start == marker.next_effect_index
               ? flowmesh::ProductionLockResult::ALREADY_LOCKED_SAME
               : flowmesh::ProductionLockResult::STORAGE_FAILURE;
}

bool FlowMeshProductionStore::ReadLockedCandidate(
    const flowmesh::ProductionSignPosition& position,
    std::optional<StoredLockedProductionCandidate>& out, std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    out.reset();
    DiskLockedCandidate disk;
    const ReadResult candidate_result{
        ReadStrict(m_db, LockedCandidateKey(position), disk)};
    uint256 locked_hash;
    const ReadResult lock_result{ReadStrict(m_db, LockKey(position),
                                            locked_hash)};
    if (candidate_result == ReadResult::ERROR ||
        lock_result == ReadResult::ERROR) {
        error = "FlowMesh v3 locked candidate is corrupt or unreadable";
        return false;
    }
    if (candidate_result == ReadResult::NOT_FOUND &&
        lock_result == ReadResult::NOT_FOUND) {
        return true;
    }
    if (candidate_result == ReadResult::NOT_FOUND &&
        lock_result == ReadResult::FOUND) {
        Marker marker;
        if (ReadMarkerStrict(m_db, marker) == ReadResult::FOUND &&
            MarkerShapeIsValid(marker) &&
            position.sequence < marker.next_sequence) {
            // Committed positions retain their permanent hash lock, while
            // the bulky restart-only candidate evidence is erased.
            return true;
        }
    }
    const auto candidate{candidate_result == ReadResult::FOUND
                             ? DecodeDiskLockedCandidate(disk)
                             : std::nullopt};
    if (!candidate || lock_result != ReadResult::FOUND ||
        locked_hash.IsNull() || candidate->entry.GetHash() != locked_hash ||
        candidate->entry.epoch != position.epoch ||
        candidate->entry.sequence != position.sequence) {
        error = "FlowMesh v3 lock and retained candidate disagree";
        return false;
    }
    out = *candidate;
    return true;
}

bool FlowMeshProductionStore::ReadLock(
    const flowmesh::ProductionSignPosition& position,
    std::optional<uint256>& out, std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_mutex};
    out.reset();
    uint256 hash;
    switch (ReadStrict(m_db, LockKey(position), hash)) {
    case ReadResult::NOT_FOUND: return true;
    case ReadResult::ERROR:
        error = "FlowMesh v3 lock is corrupt or unreadable";
        return false;
    case ReadResult::FOUND:
        if (hash.IsNull()) {
            error = "FlowMesh v3 lock contains a null hash";
            return false;
        }
        out = hash;
        return true;
    }
    return false;
}

} // namespace node
