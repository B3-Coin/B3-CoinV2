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
    if (genesis.Header().height != 0) return Error{ErrorCode::INVALID_INPUT, REPLAY_BASE_NOT_GENESIS};

    HeadView head{genesis};         // after the last decided slot
    HeadView applied_head{genesis}; // after the last applied slot: the committed head
    // Heads of decided slots above the committed head, by height (normally at most one).
    std::map<uint64_t, HeadView> unapplied;
    std::vector<ReplayedSlot> slots;
    uint64_t duplicate_decisions{0};
    uint64_t duplicate_applied{0};
    uint64_t last_applied_slot{genesis.Header().height};
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
            if (applied->slot == 0 || applied->slot > head.Header().height) return Order(REPLAY_APPLIED_UNKNOWN);
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
            const auto pending{unapplied.find(applied->slot)};
            // Every decided slot above the committed head is in `unapplied`.
            if (pending == unapplied.end()) return Error{ErrorCode::INVARIANT, "replayed slot without its head"};
            applied_head = std::move(pending->second);
            unapplied.erase(pending);
            break;
        }
        case storage::RecordType::SNAPSHOT_MARK:
            // Snapshot boundaries: the mark record, its snapshot and a replay that starts there.
            return NotImplemented("M4-STO-03"); // TODO(rebirth:M4-STO-03)
        case storage::RecordType::PREPARED_QC:
            // Durable PBFT view state in the commit journal.
            return NotImplemented("M4-STO-07"); // TODO(rebirth:M4-STO-07)
        default:
            if (!storage::IsKnown(record.type)) return Error{ErrorCode::DECODE, storage::RECORD_TYPE};
            return Order(storage::RECORD_ROLE);
        }
    }
    return Ok(ReplayResult{std::move(slots), std::move(applied_head), std::move(head), duplicate_decisions, duplicate_applied});
}

} // namespace flowmesh2::state
