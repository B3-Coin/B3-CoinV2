// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <flowmesh/preagreement.h>

#include <crypto/common.h>
#include <hash.h>

#include <array>
#include <map>
#include <span>
#include <utility>

namespace flowmesh {
namespace {

bool ContextWellFormed(const PreagreementContext& context)
{
    return !context.domain.IsNull() && !context.market_id.IsNull() &&
        !context.seat_set_hash.IsNull() && !context.previous_state_root.IsNull() &&
        !context.execution_config_id.IsNull() &&
        (context.sequence == 0 ? context.parent_hash.IsNull() : !context.parent_hash.IsNull());
}

void WriteContext(HashWriter& writer, const PreagreementContext& context)
{
    std::array<unsigned char, 16> numbers{};
    WriteBE64(numbers.data(), context.epoch);
    WriteBE64(numbers.data() + 8, context.sequence);
    writer << std::span<const unsigned char>{context.domain.begin(), 32}
           << std::span<const unsigned char>{context.market_id.begin(), 32}
           << std::span<const unsigned char>{numbers.data(), 8}
           << std::span<const unsigned char>{context.seat_set_hash.begin(), 32}
           << std::span<const unsigned char>{numbers.data() + 8, 8}
           << std::span<const unsigned char>{context.parent_hash.begin(), 32}
           << std::span<const unsigned char>{context.previous_state_root.begin(), 32}
           << std::span<const unsigned char>{context.execution_config_id.begin(), 32};
}

void WriteViewCandidate(HashWriter& writer, const uint32_t view, const uint256& candidate)
{
    std::array<unsigned char, 4> number{};
    WriteBE32(number.data(), view);
    writer << std::span<const unsigned char>{number}
           << std::span<const unsigned char>{candidate.begin(), 32};
}

PreagreementCheck CheckContext(const PreagreementContext& context, const ActiveFnBlsSeatSet& seats)
{
    if (!ContextWellFormed(context)) return PreagreementCheck::INVALID_CONTEXT;
    if (context.market_id != seats.market_id || context.epoch != seats.epoch ||
        context.seat_set_hash != seats.set_hash ||
        CheckActiveFnBlsSeatSet(context.domain, seats) != BlsSeatSetCheck::OK) {
        return PreagreementCheck::INVALID_SEAT_SET;
    }
    return PreagreementCheck::OK;
}

// Count the complete object before any cryptographic verification. No proof
// contains another new-view proof, so there is no recursive depth to consume.
bool AddBudget(size_t& used, const size_t count)
{
    if (count > PREAGREEMENT_MAX_PROOF_SIGNATURES - used) return false;
    used += count;
    return true;
}

bool NewViewBudget(const PreagreementNewViewProof& proof, size_t& used)
{
    if (!AddBudget(used, proof.reports.size())) return false;
    for (const auto& report : proof.reports) {
        if (report.prepared && !AddBudget(used, report.prepared->votes.size())) return false;
    }
    return true;
}

bool VerifySignature(const ActiveFnBlsSeatSet& seats, const uint32_t seat,
                     const std::optional<uint256>& digest,
                     const std::array<unsigned char, bls::SIGNATURE_SIZE>& bytes)
{
    if (!digest || seat >= seats.Size()) return false;
    const auto signature{bls::Signature::Decode(bytes)};
    return signature && bls::Verify(seats.members[seat].key.Key(),
        std::span<const unsigned char>{digest->begin(), 32}, *signature);
}

PreagreementCheck CheckVotes(const PreagreementContext& context, const ActiveFnBlsSeatSet& seats,
                            const PreagreementPhase phase, const uint32_t view,
                            const uint256& candidate, const std::vector<IndexedBlsSignature>& votes)
{
    if (candidate.IsNull()) return PreagreementCheck::INVALID_CANDIDATE;
    if (votes.size() != FlowMeshBlsThreshold(seats.Size())) return PreagreementCheck::WRONG_QUORUM;
    // Shape is checked before signature work and caller-selected order is not normalized.
    for (size_t i{0}; i < votes.size(); ++i) {
        if (votes[i].seat_index >= seats.Size() ||
            (i && votes[i - 1].seat_index >= votes[i].seat_index)) {
            return PreagreementCheck::NON_CANONICAL_SIGNERS;
        }
    }
    for (const auto& vote : votes) {
        if (!VerifySignature(seats, vote.seat_index,
                PreagreementVoteDigest(context, phase, view, candidate, vote.seat_index), vote.signature.Compressed())) {
            return PreagreementCheck::BAD_SIGNATURE;
        }
    }
    return PreagreementCheck::OK;
}

PreagreementCheck CheckNewViewInner(const PreagreementContext& context, const ActiveFnBlsSeatSet& seats,
                                   const PreagreementNewViewProof& proof)
{
    if (proof.view == 0) return PreagreementCheck::INVALID_VIEW;
    if (proof.candidate.IsNull()) return PreagreementCheck::INVALID_CANDIDATE;
    if (proof.reports.size() != FlowMeshBlsThreshold(seats.Size())) return PreagreementCheck::WRONG_QUORUM;
    for (size_t i{0}; i < proof.reports.size(); ++i) {
        const auto& report{proof.reports[i]};
        if (report.view != proof.view || (report.prepared && report.prepared->view >= proof.view)) {
            return PreagreementCheck::INVALID_VIEW;
        }
        if (report.seat_index >= seats.Size() ||
            (i && proof.reports[i - 1].seat_index >= report.seat_index)) {
            return PreagreementCheck::NON_CANONICAL_SIGNERS;
        }
    }
    std::optional<std::pair<uint32_t, uint256>> highest;
    std::map<uint32_t, uint256> prepared_by_view;
    for (const auto& report : proof.reports) {
        if (report.prepared) {
            const auto& prepared{*report.prepared};
            const auto check{CheckVotes(context, seats, PreagreementPhase::PREPARE,
                                        prepared.view, prepared.candidate, prepared.votes)};
            if (check != PreagreementCheck::OK) return check;
            const auto [entry, inserted]{prepared_by_view.emplace(prepared.view, prepared.candidate)};
            if (!inserted && entry->second != prepared.candidate) return PreagreementCheck::CONFLICTING_PREPARED;
            if (!highest || highest->first < prepared.view) {
                highest = {prepared.view, prepared.candidate};
            }
        }
        if (!VerifySignature(seats, report.seat_index,
                PreagreementViewChangeDigest(context, report), report.signature)) {
            return PreagreementCheck::BAD_SIGNATURE;
        }
    }
    if (highest && highest->second != proof.candidate) return PreagreementCheck::UNSAFE_NEW_VIEW_CANDIDATE;
    return PreagreementCheck::OK;
}

} // namespace

const char* PreagreementCheckName(const PreagreementCheck check)
{
    switch (check) {
    case PreagreementCheck::OK: return "ok-proof-only-no-signing-authority";
    case PreagreementCheck::INVALID_CONTEXT: return "invalid-context";
    case PreagreementCheck::INVALID_SEAT_SET: return "invalid-seat-set";
    case PreagreementCheck::INVALID_CANDIDATE: return "invalid-candidate";
    case PreagreementCheck::WRONG_QUORUM: return "wrong-quorum";
    case PreagreementCheck::NON_CANONICAL_SIGNERS: return "non-canonical-signers";
    case PreagreementCheck::BAD_SIGNATURE: return "bad-signature";
    case PreagreementCheck::INVALID_VIEW: return "invalid-view";
    case PreagreementCheck::MISSING_NEW_VIEW: return "missing-new-view";
    case PreagreementCheck::WRONG_NEW_VIEW: return "wrong-new-view";
    case PreagreementCheck::CONFLICTING_PREPARED: return "conflicting-prepared";
    case PreagreementCheck::UNSAFE_NEW_VIEW_CANDIDATE: return "unsafe-new-view-candidate";
    case PreagreementCheck::PROOF_TOO_LARGE: return "proof-too-large";
    }
    return "unknown";
}

std::optional<uint256> PreagreementVoteDigest(
    const PreagreementContext& context, const PreagreementPhase phase,
    const uint32_t view, const uint256& candidate, const uint32_t seat_index)
{
    if (!ContextWellFormed(context) || candidate.IsNull() ||
        (phase != PreagreementPhase::PREPARE && phase != PreagreementPhase::COMMIT)) return std::nullopt;
    HashWriter writer{TaggedHash(phase == PreagreementPhase::PREPARE
        ? "B3/FLOWMESH/PREAGREE/PREPARE/V1" : "B3/FLOWMESH/PREAGREE/COMMIT/V1")};
    WriteContext(writer, context);
    WriteViewCandidate(writer, view, candidate);
    std::array<unsigned char, 4> index{};
    WriteBE32(index.data(), seat_index);
    writer << std::span<const unsigned char>{index};
    return writer.GetSHA256();
}

std::optional<uint256> PreagreementViewChangeDigest(
    const PreagreementContext& context, const PreagreementViewChange& report)
{
    if (!ContextWellFormed(context) || report.view == 0 ||
        (report.prepared && (report.prepared->candidate.IsNull() ||
            report.prepared->view >= report.view ||
            report.prepared->votes.size() > PREAGREEMENT_MAX_PROOF_SIGNATURES))) return std::nullopt;
    HashWriter writer{TaggedHash("B3/FLOWMESH/PREAGREE/VIEWCHANGE/V1")};
    WriteContext(writer, context);
    std::array<unsigned char, 9> fields{};
    WriteBE32(fields.data(), report.view);
    WriteBE32(fields.data() + 4, report.seat_index);
    fields[8] = report.prepared.has_value();
    writer << std::span<const unsigned char>{fields};
    if (report.prepared) {
        const auto& prepared{*report.prepared};
        WriteViewCandidate(writer, prepared.view, prepared.candidate);
        std::array<unsigned char, 4> count{};
        WriteBE32(count.data(), static_cast<uint32_t>(prepared.votes.size()));
        writer << std::span<const unsigned char>{count};
        for (const auto& vote : prepared.votes) {
            std::array<unsigned char, 4> index{};
            WriteBE32(index.data(), vote.seat_index);
            writer << std::span<const unsigned char>{index}
                   << std::span<const unsigned char>{vote.signature.Compressed()};
        }
    }
    return writer.GetSHA256();
}

PreagreementCheck CheckPreagreementPrepared(
    const PreagreementContext& context, const ActiveFnBlsSeatSet& seats,
    const PreagreementPreparedCertificate& certificate)
{
    if (certificate.votes.size() > PREAGREEMENT_MAX_PROOF_SIGNATURES) return PreagreementCheck::PROOF_TOO_LARGE;
    const auto check{CheckContext(context, seats)};
    return check == PreagreementCheck::OK
        ? CheckVotes(context, seats, PreagreementPhase::PREPARE, certificate.view, certificate.candidate, certificate.votes)
        : check;
}

PreagreementCheck CheckPreagreementNewView(
    const PreagreementContext& context, const ActiveFnBlsSeatSet& seats,
    const PreagreementNewViewProof& proof)
{
    size_t used{0};
    if (!NewViewBudget(proof, used)) return PreagreementCheck::PROOF_TOO_LARGE;
    const auto check{CheckContext(context, seats)};
    return check == PreagreementCheck::OK ? CheckNewViewInner(context, seats, proof) : check;
}

PreagreementCheck CheckPreagreementCommit(
    const PreagreementContext& context, const ActiveFnBlsSeatSet& seats,
    const PreagreementCommitCertificate& certificate)
{
    size_t used{0};
    if (!AddBudget(used, certificate.prepared.votes.size()) || !AddBudget(used, certificate.commits.size()) ||
        (certificate.new_view && !NewViewBudget(*certificate.new_view, used))) return PreagreementCheck::PROOF_TOO_LARGE;
    auto check{CheckContext(context, seats)};
    if (check != PreagreementCheck::OK) return check;
    const auto& prepared{certificate.prepared};
    if (prepared.view == 0) {
        if (certificate.new_view) return PreagreementCheck::WRONG_NEW_VIEW;
    } else {
        if (!certificate.new_view) return PreagreementCheck::MISSING_NEW_VIEW;
        if (certificate.new_view->view != prepared.view || certificate.new_view->candidate != prepared.candidate) {
            return PreagreementCheck::WRONG_NEW_VIEW;
        }
        check = CheckNewViewInner(context, seats, *certificate.new_view);
        if (check != PreagreementCheck::OK) return check;
    }
    check = CheckVotes(context, seats, PreagreementPhase::PREPARE, prepared.view, prepared.candidate, prepared.votes);
    return check == PreagreementCheck::OK
        ? CheckVotes(context, seats, PreagreementPhase::COMMIT, prepared.view, prepared.candidate, certificate.commits)
        : check;
}

} // namespace flowmesh
