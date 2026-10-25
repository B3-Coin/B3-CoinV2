// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <flowmesh2/agreement/control_messages.h>
#include <flowmesh2/agreement/pbft.h>
#include <flowmesh2/core/state_root.h>
#include <util/chaintype.h>

namespace flowmesh2::agreement {
namespace {
Error Invalid(std::string_view detail) { return Error{ErrorCode::VERIFY_FAILED, detail}; }

Result<void> VerifyCertificate(const SlotCertificate& cert, const SlotHeaderV2& header, const SeatSet& seats, const uint256& digest)
{
    if (cert.header_hash != header.Hash()) return Invalid("CONTROL_CERT_HEADER");
    if (const auto format{CertificateDigest(header, cert)}; !format.has_value()) return format.error();
    auto indices{cert.SignerIndices(seats.Size())};
    if (!indices.has_value()) return indices.error();
    if (indices->size() < seats.Quorum()) return Invalid("CONTROL_CERT_QUORUM");
    std::vector<bls::VerifiedPublicKey> keys;
    for (const auto seat : *indices) keys.push_back(seats.Keys()[seat]);
    const auto aggregate{bls::Signature::Decode(cert.aggregate)};
    if (!aggregate || !bls::FastAggregateVerify(keys, std::span{digest.data(), digest.size()}, *aggregate)) return Invalid("CONTROL_CERT_SIGNATURE");
    return Ok();
}

Result<void> VerifyInstance(const storage::ControlKey& key, const SlotHeaderV2& parent, const SeatSet& seats,
                            const ProfileParams& profile, storage::ControlPhase phase)
{
    if (!RegtestPbftSeatCount(seats.Size())) return Invalid("CONTROL_SEAT_COUNT");
    const auto expected{profile.source_range_commitment ? StableQNetworkRegtestProfile(ChainType::REGTEST) : StableQViewRegtestProfile(ChainType::REGTEST)};
    if (!expected.has_value()) return expected.error();
    if (profile != *expected || ProfileHash(profile) != parent.profile_hash || parent.commitment_version != ExpectedCommitmentVersion(profile)) return Invalid("CONTROL_STABLE_PROFILE");
    if (parent.height == UINT64_MAX || key.domain != parent.domain || key.profile_hash != parent.profile_hash ||
        key.seat_set_root != seats.Root() || key.seat_set_root != parent.seat_set_root || key.epoch != parent.authority_epoch ||
        key.slot != parent.height + 1 || key.parent_hash != parent.Hash() || key.seat >= seats.Size() || key.view == 0 ||
        key.view > storage::MAX_CONTROL_VIEW || key.phase != phase) return Invalid("CONTROL_INSTANCE");
    return Ok();
}

Result<void> VerifyPreparedHeader(const SlotHeaderV2& header, const storage::ControlKey& key, const SlotHeaderV2& parent)
{
    if (header.domain != key.domain || header.profile_hash != key.profile_hash || header.authority_epoch != key.epoch ||
        header.seat_set_root != key.seat_set_root || header.height != key.slot || header.parent_hash != key.parent_hash ||
        header.commitment_version != COMMITMENT_VERSION_VIEW_BOUND || header.facts_from != parent.facts_to ||
        header.facts_to < header.facts_from || header.anchor.height < parent.anchor.height ||
        (parent.height == 0 ? !header.parent_cert_hash.IsNull() : header.parent_cert_hash != parent.Hash())) return Invalid("CONTROL_PROOF_INSTANCE");
    return Ok();
}

template<class T>
Result<storage::ControlIntentRecord> Transcript(const T& message, const ProfileParams& profile)
{
    auto unsigned_message{message};
    unsigned_message.signature.fill(0);
    auto bytes{EncodeMessage(Message{unsigned_message}, profile)};
    if (!bytes.has_value()) return bytes.error();
    if (bytes->size() > storage::MAX_CONTROL_MESSAGE_BYTES) return Error{ErrorCode::OVERSIZE, "CONTROL_MESSAGE_SIZE"};
    return Ok(storage::ControlIntentRecord{message.key, std::move(*bytes)});
}

template<class T>
Result<void> VerifySignature(const T& message, const SeatSet& seats, const ProfileParams& profile)
{
    auto transcript{Transcript(message, profile)};
    if (!transcript.has_value()) return transcript.error();
    const auto digest{transcript->Digest()};
    const auto signature{bls::Signature::Decode(message.signature)};
    if (!signature || !bls::Verify(seats.Keys()[message.key.seat].Key(), std::span{digest.data(), digest.size()}, *signature)) {
        return Invalid("CONTROL_MESSAGE_SIGNATURE");
    }
    return Ok();
}
} // namespace

Result<storage::ControlIntentRecord> ControlIntent(const ControlReport& report, const ProfileParams& profile) { return Transcript(report, profile); }
Result<storage::ControlIntentRecord> ControlIntent(const ControlNewView& view, const ProfileParams& profile) { return Transcript(view, profile); }

Result<void> VerifyControlReportEvidence(const ControlReport& report, const SlotHeaderV2& parent, const SeatSet& seats, const ProfileParams& profile)
{
    if (auto instance{VerifyInstance(report.key, parent, seats, profile, storage::ControlPhase::VIEW_CHANGE)}; !instance.has_value()) return instance.error();
    if (report.prepared) {
        if (report.prepared->view >= report.key.view) return Invalid("CONTROL_PREPARED_FROM_FUTURE");
        if (report.prepared->certificate.view != report.prepared->view) return Invalid("CONTROL_PREPARED_VIEW");
        if (auto bound{VerifyPreparedHeader(report.prepared->header, report.key, parent)}; !bound.has_value()) return bound.error();
        if (auto cert{VerifyCertificate(report.prepared->certificate, report.prepared->header, seats,
                                        PbftPrepareDigest(report.prepared->header, report.prepared->view))}; !cert.has_value()) return cert.error();
    }
    if (report.decision) {
        if (auto bound{VerifyPreparedHeader(report.decision->header, report.key, parent)}; !bound.has_value()) return bound.error();
        const auto digest{CertificateDigest(report.decision->header, report.decision->cert)};
        if (!digest.has_value()) return digest.error();
        if (auto cert{VerifyCertificate(report.decision->cert, report.decision->header, seats, *digest)}; !cert.has_value()) return cert.error();
    }
    return Ok();
}

Result<void> VerifyControlReport(const ControlReport& report, const SlotHeaderV2& parent, const SeatSet& seats, const ProfileParams& profile)
{
    if (auto proof{VerifyControlReportEvidence(report, parent, seats, profile)}; !proof.has_value()) return proof.error();
    return VerifySignature(report, seats, profile);
}

Result<ControlSelection> VerifyControlNewViewEvidence(const ControlNewView& view, const SlotHeaderV2& parent,
                                             const SeatSet& seats, const ProfileParams& profile)
{
    if (auto instance{VerifyInstance(view.key, parent, seats, profile, storage::ControlPhase::NEW_VIEW)}; !instance.has_value()) return instance.error();
    if (view.key.seat != (view.key.slot % seats.Size() + view.key.view % seats.Size()) % seats.Size()) return Invalid("CONTROL_NEW_VIEW_LEADER");
    if (view.reports.size() < seats.Quorum() || view.reports.size() > seats.Size()) return Invalid("CONTROL_REPORT_QUORUM");
    ControlSelection selected;
    for (size_t i{0}; i < view.reports.size(); ++i) {
        const auto& report{view.reports[i]};
        if (report.key.view != view.key.view || (i && report.key.seat <= view.reports[i - 1].key.seat)) return Invalid("CONTROL_REPORT_ORDER_OR_TARGET");
        if (auto valid{VerifyControlReport(report, parent, seats, profile)}; !valid.has_value()) return valid.error();
        if (report.prepared) {
            if (!selected.highest || report.prepared->view > selected.highest->view) selected.highest = report.prepared;
            else if (report.prepared->view == selected.highest->view && report.prepared->header.Hash() != selected.highest->header.Hash()) {
                return Error{ErrorCode::EQUIVOCATION_REFUSED, "CONTROL_EQUAL_VIEW_PREPARED_CONFLICT"};
            }
        }
        if (report.decision) {
            if (selected.decision && selected.decision->header.Hash() != report.decision->header.Hash()) {
                return Error{ErrorCode::EQUIVOCATION_REFUSED, "CONTROL_CONFLICTING_DECISION"};
            }
            selected.decision = report.decision;
        }
    }
    if (selected.decision) {
        if (view.selected != selected.decision->header.Hash()) return Invalid("CONTROL_SELECTED_DECISION");
    } else if (selected.highest && view.selected != selected.highest->header.Hash()) return Invalid("CONTROL_SELECTED_HIGHEST");
    if (!view.selected_proposal || view.selected_proposal->view != view.key.view ||
        view.selected_proposal->header.Hash() != view.selected) return Invalid("CONTROL_SELECTED_BODY");
    if (auto body{CheckProposal(*view.selected_proposal)}; !body.has_value()) return body.error();
    if (auto instance{VerifyPreparedHeader(view.selected_proposal->header, view.key, parent)}; !instance.has_value()) return instance.error();
    return Ok(std::move(selected));
}

Result<ControlSelection> VerifyControlNewView(const ControlNewView& view, const SlotHeaderV2& parent,
                                             const SeatSet& seats, const ProfileParams& profile)
{
    auto proof{VerifyControlNewViewEvidence(view, parent, seats, profile)};
    if (!proof.has_value()) return proof.error();
    if (auto signature{VerifySignature(view, seats, profile)}; !signature.has_value()) return signature.error();
    return proof;
}

} // namespace flowmesh2::agreement
