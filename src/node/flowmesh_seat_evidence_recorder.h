// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FLOWMESH_SEAT_EVIDENCE_RECORDER_H
#define B3COIN_NODE_FLOWMESH_SEAT_EVIDENCE_RECORDER_H

#include <flowmesh/production_engine.h>
#include <flowmesh/seat_evidence.h>
#include <logging.h>
#include <node/flowmesh_production_store.h>
#include <uint256.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace node {

// The runtime's recorder of seat-misbehaviour evidence, over any market type
// with the members it uses (FlowMeshRuntime's own Market in production).

// Seat-misbehaviour evidence (flowmesh/seat_evidence.h). Information only:
// recording never changes a relay, verification, signing, halt or commit
// decision, and a failure here never blocks the market.

//! Detection needs a misbehaving seat. Per market and minute, at most this
//! many detections are checked, recorded or not; only one that verifies is
//! written (one synced store batch each). The pace is spent before any
//! verification, so a detection beyond it costs only its id and is counted
//! as unrecorded.
inline constexpr size_t SEAT_EVIDENCE_CHECKS_PER_MINUTE{8};

template <typename Market>
void SetSeatEvidence(Market& market, const SeatEvidenceLog& log)
{
    auto retained{std::make_shared<std::vector<flowmesh::SeatEvidence>>()};
    retained->reserve(log.retained.size());
    market.seat_evidence_ids.clear();
    for (const auto& item : log.retained) {
        retained->push_back(item.evidence);
        market.seat_evidence_ids.insert(item.evidence.Id());
    }
    market.seat_evidence_recorded = log.recorded;
    market.seat_evidence = std::move(retained);
}

//! The anchored seat set `statement` names: this market's own set, which was
//! validated when it was installed (`validated`), or else the chain's, which
//! the check validates. Null when the chain has none.
template <typename Market>
const flowmesh::ActiveFnBlsSeatSet* EvidenceSeats(Market& market, const flowmesh::SeatEvidenceStatement& statement,
                                                  std::optional<flowmesh::ActiveFnBlsSeatSet>& storage,
                                                  bool& validated)
{
    validated = market.seats.epoch == statement.seat_epoch && market.seats.set_hash == statement.seat_set_hash;
    if (validated) return &market.seats;
    ++market.seat_evidence_work.chain_lookups;
    storage = market.chain->SeatSet(market.domain, market.market_id, statement.seat_epoch, statement.seat_set_hash);
    return storage ? &*storage : nullptr;
}

//! Keeps `evidence` in the market's store when this node's own chain
//! verifies it; never a record that others could not check. The caller
//! names which statement signatures it has itself just verified over
//! exactly the context each statement signs (see SeatEvidencePrechecked);
//! those are not verified again.
template <typename Market>
void RecordSeatEvidence(Market& market, const flowmesh::SeatEvidence& evidence,
                        const bool first_signature_verified, const bool second_signature_verified)
{
    if (market.store == nullptr || market.seat_evidence_ids.contains(evidence.Id())) return;
    // The pace first: past it, a detection costs no seat-set read, set
    // validation or signature check, and never a write.
    const auto now{market.clock->Now()};
    if (now < market.seat_evidence_window || now - market.seat_evidence_window >= std::chrono::minutes{1}) {
        market.seat_evidence_window = now;
        market.seat_evidence_window_checks = 0;
    }
    if (market.seat_evidence_window_checks >= SEAT_EVIDENCE_CHECKS_PER_MINUTE) {
        ++market.seat_evidence_unrecorded;
        return;
    }
    // Spent by every checked detection, recorded or not.
    ++market.seat_evidence_window_checks;
    ++market.seat_evidence_work.checks;
    flowmesh::SeatEvidencePrechecked prechecked;
    prechecked.first_signature = first_signature_verified;
    prechecked.second_signature = second_signature_verified;
    std::optional<flowmesh::ActiveFnBlsSeatSet> first_storage, second_storage;
    const auto* first_seats{EvidenceSeats(market, evidence.first, first_storage, prechecked.first_seats)};
    const auto* second_seats{first_seats == nullptr
                                 ? nullptr
                                 : EvidenceSeats(market, evidence.second, second_storage, prechecked.second_seats)};
    flowmesh::SeatEvidenceWork work;
    const auto check{first_seats && second_seats
                         ? flowmesh::CheckSeatEvidence(evidence, *first_seats, *second_seats, prechecked, &work)
                         : flowmesh::SeatEvidenceCheck::WRONG_SEAT_SET};
    market.seat_evidence_work.seat_set_checks += work.seat_set_checks;
    market.seat_evidence_work.signature_checks += work.signature_checks;
    if (check != flowmesh::SeatEvidenceCheck::OK) {
        ++market.seat_evidence_unrecorded;
        return;
    }
    // Only a verified conflict reaches the synced write, within the pace.
    bool inserted{false};
    std::string error;
    SeatEvidenceLog log;
    if (!market.store->RecordSeatEvidence(evidence, inserted, error) ||
        !market.store->ReadSeatEvidence(log, error)) {
        ++market.seat_evidence_unrecorded;
        LogWarning("FlowMesh market %s could not record seat-misbehaviour evidence: %s\n",
                   market.market_id.GetHex(), error);
        return;
    }
    SetSeatEvidence(market, log);
    if (!inserted) return;
    LogWarning("FlowMesh market %s recorded seat-misbehaviour evidence %s (%s at sequence %d); "
               "information only, see getflowmeshhealth\n",
               market.market_id.GetHex(), evidence.Id().GetHex(),
               flowmesh::SeatEvidenceKindName(evidence.kind), evidence.first.sequence);
}

//! `seat` attested `first_hash` at this slot and now `second_hash`, whose
//! signature has just verified.
template <typename Market>
void RecordAttestationEquivocation(Market& market, const uint32_t seat, const uint256& first_hash,
                                   const uint256& second_hash, const bls::Signature& second_signature)
{
    // One record per seat and slot: a third candidate adds nothing new.
    if (!market.seat_evidence_slot_seats.insert(seat).second) return;
    const auto first{market.candidates.find(first_hash)};
    const auto second{market.candidates.find(second_hash)};
    const auto votes{market.attestations.find(first_hash)};
    if (first == market.candidates.end() || second == market.candidates.end() ||
        votes == market.attestations.end()) return;
    const auto vote{votes->second.find(seat)};
    if (vote == votes->second.end()) return;
    // HandleAttestation verified both votes under this market's seat set, over
    // each candidate's certificate context: the stored one when it was
    // accepted, the second just now. Those are exactly the statements below
    // when both contexts name this market and that set, so nothing is
    // verified again; anything else is not recorded.
    const auto first_context{flowmesh::ProductionCertificateContext(first->second.entry)};
    const auto second_context{flowmesh::ProductionCertificateContext(second->second.entry)};
    const auto names_seats = [&](const flowmesh::BlsCertificateContext& context) {
        return context.domain == market.domain && context.market_id == market.market_id &&
               context.seat_epoch == market.seats.epoch && context.seat_set_hash == market.seats.set_hash;
    };
    if (!names_seats(first_context) || !names_seats(second_context) || seat >= market.seats.Size()) {
        ++market.seat_evidence_unrecorded;
        return;
    }
    RecordSeatEvidence(market, flowmesh::SeatEvidence{
        flowmesh::SeatEvidenceKind::ATTESTATION_EQUIVOCATION, market.domain, market.market_id,
        flowmesh::SeatAttestationStatement(first_context, seat, vote->second.signature),
        flowmesh::SeatAttestationStatement(second_context, seat, second_signature),
        market.seats.members[seat].key.Key().Compressed()},
        /*first_signature_verified=*/true, /*second_signature_verified=*/true);
}

} // namespace node

#endif // B3COIN_NODE_FLOWMESH_SEAT_EVIDENCE_RECORDER_H
