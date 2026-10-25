// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/state/replay.h>

#include <flowmesh2/engine/engine.h>
#include <flowmesh2/storage/commit_log.h>

#include <cstddef>
#include <map>
#include <utility>

namespace flowmesh2::state {

namespace {

constexpr Error Order(std::string_view detail) { return Error{ErrorCode::ORDER_VIOLATION, detail}; }

} // namespace

Result<ReplayResult> ReplayDecisions(const HeadView& genesis, std::span<const storage::JournalRecord> records)
{
    const ProfileParams& profile{genesis.State().Profile()};
    Result<engine::Engine> created{engine::Engine::Create(profile)};
    if (!created.has_value()) return created.error();
    const engine::Engine engine{std::move(created).value()};
    // A base above genesis is a snapshot head: the records must decide and apply it (O-95).
    const uint64_t base_height{genesis.Header().height};

    HeadView head{genesis};         // after the last decided slot
    HeadView applied_head{genesis}; // after the last applied slot: the committed head
    std::map<uint64_t, storage::SnapshotMark> marks;
    // Heads of decided slots above the committed head, by height (normally at most one).
    std::map<uint64_t, HeadView> unapplied;
    std::vector<ReplayedSlot> slots;
    uint64_t duplicate_decisions{0};
    uint64_t duplicate_applied{0};
    uint64_t last_applied_slot{0};
    for (size_t i{0}; i < records.size(); ++i) {
        const storage::JournalRecord& record{records[i]};
        if (record.seq != i + 1) return Order(REPLAY_SEQ);
        switch (record.type) {
        case storage::RecordType::DECISION: {
            Result<storage::DecisionRecord> decision{storage::DecisionRecord::Decode(record.payload, profile)};
            if (!decision.has_value()) return decision.error();
            const SlotHeaderV2& header{decision->header};
            if (decision->certificate.header_hash != header.Hash()) {
                return Error{ErrorCode::VERIFY_FAILED, storage::DECISION_CERT_HEADER};
            }
            if (header.height == 0) return Order(REPLAY_DECISION_BELOW_BASE);
            if (header.height <= base_height && header.height == slots.size() + 1) {
                // Below a snapshot base: no state to re-execute on, so the headers must chain into the base.
                const uint256 cert_hash{decision->certificate.Hash()};
                if (!slots.empty() && (header.parent_hash != slots.back().header.Hash() || header.parent_cert_hash != slots.back().cert_hash)) {
                    return Error{ErrorCode::VERIFY_FAILED, REPLAY_BASE_CHAIN};
                }
                if (header.height == base_height && (!(header == genesis.Header()) || cert_hash != genesis.CertHash())) {
                    return Error{ErrorCode::VERIFY_FAILED, REPLAY_BASE_CHAIN};
                }
                slots.push_back(ReplayedSlot{header, cert_hash, record.seq, std::nullopt});
                break;
            }
            if (header.height <= base_height && header.height > slots.size()) return Order(REPLAY_DECISION_GAP);
            if (slots.size() < base_height) return Error{ErrorCode::INVALID_INPUT, REPLAY_BASE_UNKNOWN};
            if (header.height <= head.Header().height) {
                // Heights 1..head are replayed, so slots[height - 1] is that height.
                if (!(slots[header.height - 1].header == header)) {
                    return Error{ErrorCode::EQUIVOCATION_REFUSED, storage::DECISION_CONFLICT};
                }
                ++duplicate_decisions; // applied once: the first record counts
                break;
            }
            if (header.height - 1 != head.Header().height) return Order(REPLAY_DECISION_GAP);
            const Result<Candidate> candidate{ReexecuteOn(engine, head, header, decision->body)};
            if (!candidate.has_value()) return candidate.error();
            const uint256 cert_hash{decision->certificate.Hash()};
            Result<HeadView> next{HeadView::Make(header, cert_hash, candidate->Next())};
            if (!next.has_value()) return next.error();
            head = std::move(next).value();
            unapplied.emplace(header.height, head);
            slots.push_back(ReplayedSlot{header, cert_hash, record.seq, std::nullopt});
            break;
        }
        case storage::RecordType::APPLIED: {
            const Result<storage::AppliedRecord> applied{storage::AppliedRecord::Decode(record.payload)};
            if (!applied.has_value()) return applied.error();
            if (applied->slot == 0 || applied->slot > slots.size()) return Order(REPLAY_APPLIED_UNKNOWN);
            ReplayedSlot& slot{slots[applied->slot - 1]};
            if (applied->header_hash != slot.header.Hash() || applied->state_root != slot.header.state_root) {
                return Error{ErrorCode::VERIFY_FAILED, REPLAY_APPLIED_MISMATCH};
            }
            if (slot.applied_seq.has_value()) {
                ++duplicate_applied;
                break;
            }
            // The next unapplied slot only: no APPLIED below the committed head, none that skips a slot.
            if (applied->slot != last_applied_slot + 1) return Order(REPLAY_APPLIED_OUT_OF_ORDER);
            slot.applied_seq = record.seq;
            last_applied_slot = applied->slot;
            if (applied->slot <= base_height) break; // at or below the base: the committed head is already the base
            const auto pending{unapplied.find(applied->slot)};
            // Every decided slot above the committed head is in `unapplied`.
            if (pending == unapplied.end()) return Error{ErrorCode::INVARIANT, "replayed slot without its head"};
            applied_head = std::move(pending->second);
            unapplied.erase(pending);
            break;
        }
        case storage::RecordType::SNAPSHOT_MARK: {
            // A mark names an applied slot; it never moves either head.
            const Result<storage::SnapshotMark> mark{storage::SnapshotMark::Decode(record.payload)};
            if (!mark.has_value()) return mark.error();
            if (mark->slot == 0 || mark->slot > last_applied_slot) return Order(REPLAY_MARK_UNAPPLIED);
            const SlotHeaderV2& header{slots[mark->slot - 1].header};
            if (mark->header_hash != header.Hash() || mark->state_root != header.state_root) {
                return Error{ErrorCode::VERIFY_FAILED, REPLAY_MARK_MISMATCH};
            }
            marks.try_emplace(mark->slot, *mark);
            break;
        }
        case storage::RecordType::PREPARED_QC: {
            // A preparation never advances the committed or decided head.
            // RestoreVerifiedGenesis authenticates the actual PREPARE quorum.
            const auto prepared{storage::PreparedQcRecord::Decode(record.payload, profile)};
            if (!prepared.has_value()) return prepared.error();
            const auto& header{prepared->header};
            if (prepared->slot == 0 || prepared->slot != header.height || prepared->epoch != header.authority_epoch ||
                prepared->header_hash != header.Hash() || !storage::BodyMatchesHeader(header, prepared->body)) {
                return Error{ErrorCode::VERIFY_FAILED, "prepared record differs from its value"};
            }
            const auto certificate{SlotCertificate::Decode(prepared->qc)};
            if (!certificate.has_value()) return certificate.error();
            if (certificate->header_hash != header.Hash()) return Error{ErrorCode::VERIFY_FAILED, "prepared proof names another value"};
            if (header.height <= slots.size()) {
                if (slots[header.height - 1].header != header) return Error{ErrorCode::EQUIVOCATION_REFUSED, "prepared record conflicts with decision"};
            } else if (header.height <= base_height) {
                // A preparation below a snapshot base, before its decision: nothing to check it against.
            } else {
                if (header.height - 1 != head.Header().height) return Order(REPLAY_DECISION_GAP);
                const auto candidate{ReexecuteOn(engine, head, header, prepared->body)};
                if (!candidate.has_value()) return candidate.error();
            }
            break;
        }
        default:
            if (!storage::IsKnown(record.type)) return Error{ErrorCode::DECODE, storage::RECORD_TYPE};
            return Order(storage::RECORD_ROLE);
        }
    }
    if (slots.size() < base_height || last_applied_slot < base_height) return Error{ErrorCode::INVALID_INPUT, REPLAY_BASE_UNKNOWN};
    std::vector<storage::SnapshotMark> mark_list;
    for (const auto& [slot, mark] : marks) mark_list.push_back(mark);
    return Ok(ReplayResult{std::move(slots), std::move(applied_head), std::move(head), duplicate_decisions, duplicate_applied,
                           std::move(mark_list)});
}

} // namespace flowmesh2::state
