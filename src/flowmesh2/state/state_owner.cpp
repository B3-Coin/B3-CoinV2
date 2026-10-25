// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/state/state_owner.h>

#include <flowmesh2/core/state_root.h>
#include <flowmesh2/storage/journal.h>
#include <flowmesh2/storage/commit_log.h>
#include <flowmesh2/storage/control_log.h>
#include <flowmesh2/core/merkle.h>
#include <flowmesh2/agreement/pbft.h>

#include <algorithm>
#include <numeric>
#include <utility>
#include <vector>

namespace flowmesh2::state {

namespace {

constexpr Error Invalid(std::string_view detail) { return Error{ErrorCode::INVALID_INPUT, detail}; }
constexpr Error Stale() { return Error{ErrorCode::STALE_PARENT, PARENT_NOT_HEAD}; }

uint8_t CommitmentVersion(const ProfileParams& profile)
{
    return ExpectedCommitmentVersion(profile);
}

//! Verification only: no secret key or dependency on the separate client library.
Result<void> CheckRestoreCertificate(const SlotHeaderV2& header, const SlotCertificate& cert, const SeatSet& seats, const uint256& digest)
{
    if (cert.header_hash != header.Hash() || header.seat_set_root != seats.Root()) {
        return Error{ErrorCode::VERIFY_FAILED, RESTORE_CERTIFICATE};
    }
    const auto indices{cert.SignerIndices(seats.Size())};
    if (!indices.has_value() || indices->size() < seats.Quorum()) {
        return Error{ErrorCode::VERIFY_FAILED, RESTORE_CERTIFICATE};
    }
    const auto signature{bls::Signature::Decode(cert.aggregate)};
    if (!signature) return Error{ErrorCode::VERIFY_FAILED, RESTORE_CERTIFICATE};
    std::vector<bls::VerifiedPublicKey> signers;
    signers.reserve(indices->size());
    for (const uint32_t index : *indices) signers.push_back(seats.Keys()[index]);
    if (!bls::FastAggregateVerify(signers, std::span<const unsigned char>{digest.begin(), digest.end()}, *signature)) {
        return Error{ErrorCode::VERIFY_FAILED, RESTORE_CERTIFICATE};
    }
    return Ok();
}

Result<void> CheckRestoreHead(const HeadView& head, const HeadView& genesis, const ProfileParams& profile)
{
    const SlotHeaderV2& header{head.Header()};
    if (header.version != SLOT_HEADER_VERSION || header.domain != genesis.Header().domain ||
        header.profile_hash != ProfileHash(profile) || !(head.State().Profile() == profile) ||
        header.authority_epoch != 0 || header.seat_set_root != genesis.Header().seat_set_root ||
        head.State().seat_set_root != header.seat_set_root) return Error{ErrorCode::VERIFY_FAILED, RESTORE_AUTHORITY};
    const uint8_t version{CommitmentVersion(profile)};
    if (header.commitment_version != version) return Error{ErrorCode::VERIFY_FAILED, OWNER_COMMITMENT_VERSION};
    const auto roots{head.State().engine.Commit()};
    if (!roots.has_value()) return roots.error();
    if (roots->version != version) return Error{ErrorCode::VERIFY_FAILED, OWNER_COMMITMENT_VERSION};
    if (roots->Root() != header.state_root) return Error{ErrorCode::VERIFY_FAILED, OWNER_STATE_ROOT_MISMATCH};
    return head.State().engine.CheckInvariants();
}

//! The instance-bound chain, exact bodies and quorum certificates of every DECISION and PREPARED_QC in `records`.
Result<void> VerifyRestoreRecords(const SlotHeaderV2& base, std::span<const storage::JournalRecord> records, const SeatSet& seats,
                                  uint64_t commit_journal_instance, const ProfileParams& profile)
{
    uint256 chain{storage::JournalGenesisChain(storage::JournalRole::COMMIT, commit_journal_instance)};
    uint64_t expected_seq{0};
    for (const auto& record : records) {
        if (record.seq != ++expected_seq) return Error{ErrorCode::ORDER_VIOLATION, REPLAY_SEQ};
        if (record.payload.size() > storage::MAX_RECORD_PAYLOAD_BYTES) return Error{ErrorCode::OVERSIZE, storage::RECORD_LENGTH};
        if (!storage::IsKnown(record.type)) return Error{ErrorCode::DECODE, storage::RECORD_TYPE};
        if (!storage::RoleAccepts(storage::JournalRole::COMMIT, record.type)) return Error{ErrorCode::ORDER_VIOLATION, storage::RECORD_ROLE};
        chain = storage::NextChain(chain, record.type, record.payload);
        if (chain != record.chain) return Error{ErrorCode::VERIFY_FAILED, RESTORE_RECORD_CHAIN};
        if (record.type != storage::RecordType::DECISION && record.type != storage::RecordType::PREPARED_QC) continue;
        SlotHeaderV2 header;
        SlotBody body;
        SlotCertificate certificate;
        uint256 digest;
        if (record.type == storage::RecordType::DECISION) {
            const auto decision{storage::DecisionRecord::Decode(record.payload, profile)};
            if (!decision.has_value()) return decision.error();
            header = decision->header;
            body = decision->body;
            certificate = decision->certificate;
            const auto signed_digest{CertificateDigest(header, certificate)};
            if (!signed_digest.has_value()) return signed_digest.error();
            digest = *signed_digest;
        } else {
            const auto prepared{storage::PreparedQcRecord::Decode(record.payload, profile)};
            if (!prepared.has_value()) return prepared.error();
            header = prepared->header;
            body = prepared->body;
            if (prepared->epoch != header.authority_epoch || prepared->slot != header.height ||
                prepared->header_hash != header.Hash() ||
                (profile.view_bound_commits ? prepared->view > storage::MAX_CONTROL_VIEW : prepared->view != 0)) {
                return Error{ErrorCode::VERIFY_FAILED, RESTORE_AUTHORITY};
            }
            const auto proof{SlotCertificate::Decode(prepared->qc)};
            if (!proof.has_value()) return proof.error();
            certificate = *proof;
            if (header.commitment_version == COMMITMENT_VERSION_VIEW_BOUND && certificate.view != prepared->view) {
                return Error{ErrorCode::VERIFY_FAILED, RESTORE_CERTIFICATE};
            }
            if (const auto format{CertificateDigest(header, certificate)}; !format.has_value()) return format.error();
            digest = agreement::PbftPrepareDigest(header, prepared->view);
        }
        if (header.version != SLOT_HEADER_VERSION || header.domain != base.domain ||
            header.profile_hash != base.profile_hash || header.authority_epoch != 0 ||
            header.seat_set_root != seats.Root()) return Error{ErrorCode::VERIFY_FAILED, RESTORE_AUTHORITY};
        if (header.commitment_version != CommitmentVersion(profile)) return Error{ErrorCode::VERIFY_FAILED, OWNER_COMMITMENT_VERSION};
        if (!storage::BodyMatchesHeader(header, body)) return Error{ErrorCode::VERIFY_FAILED, storage::BODY_HEADER_MISMATCH};
        if (const auto verified{CheckRestoreCertificate(header, certificate, seats, digest)}; !verified.has_value()) return verified.error();
    }

    return Ok();
}

} // namespace

StateOwner::StateOwner(engine::Engine engine, std::shared_ptr<const HeadView> genesis, uint64_t commit_journal_instance)
    : m_engine{std::move(engine)}, m_commit_journal_instance{commit_journal_instance},
      m_configured_genesis{genesis}, m_head{std::move(genesis)}
{
}

Result<std::unique_ptr<StateOwner>> StateOwner::Create(const ProfileParams& profile, const HeadView& genesis,
                                                       uint64_t commit_journal_instance)
{
    Result<engine::Engine> engine{engine::Engine::Create(profile)};
    if (!engine.has_value()) return engine.error();
    const SlotHeaderV2& header{genesis.Header()};
    if (header.height != 0 || !header.parent_hash.IsNull() || !header.parent_cert_hash.IsNull() || !genesis.CertHash().IsNull()) {
        return Invalid(OWNER_NOT_GENESIS);
    }
    if (header.profile_hash != ProfileHash(profile) || !(genesis.State().Profile() == profile)) return Invalid(OWNER_PROFILE_MISMATCH);
    if (header.seat_set_root != genesis.State().seat_set_root) return Invalid(OWNER_SEAT_SET_MISMATCH);
    const uint8_t version{CommitmentVersion(profile)};
    if (header.commitment_version != version) return Error{ErrorCode::VERIFY_FAILED, OWNER_COMMITMENT_VERSION};
    const Result<StateRootsV0> roots{genesis.State().engine.Commit()};
    if (!roots.has_value()) return roots.error();
    if (roots->version != version) return Error{ErrorCode::VERIFY_FAILED, OWNER_COMMITMENT_VERSION};
    if (roots->Root() != header.state_root) return Error{ErrorCode::VERIFY_FAILED, OWNER_STATE_ROOT_MISMATCH};
    std::unique_ptr<StateOwner> owner{
        new StateOwner{std::move(engine).value(), std::make_shared<const HeadView>(genesis), commit_journal_instance}};
    return Ok(std::move(owner));
}

std::shared_ptr<const HeadView> StateOwner::Head() const
{
    LOCK(m_mutex);
    return m_head;
}

Result<Candidate> StateOwner::Preview(const ParentRef& parent, const SlotInputs& inputs) const
{
    if (parent.kind == ParentKind::CANDIDATE) {
        // Pipelining: building on an uncommitted candidate (plan section 13: deferred until a measured need).
        return NotImplemented("M4-AGR-10"); // TODO(rebirth:M4-AGR-10)
    }
    if (parent.kind != ParentKind::COMMITTED) return Invalid(PARENT_KIND_UNKNOWN);
    const std::shared_ptr<const HeadView> head{Head()}; // the only work under the lock: a pointer copy
    if (parent.height != head->Header().height || parent.header_hash != head->HeaderHash()) return Stale();
    // Executes without the lock. The candidate shares the ledger and copies the books (REAL-REPLACE M2-LED-10).
    return BuildCandidate(m_engine, *head, inputs);
}

Result<Candidate> StateOwner::Reexecute(const SlotHeaderV2& header, const SlotBody& body) const
{
    const std::shared_ptr<const HeadView> head{Head()};
    return ReexecuteOn(m_engine, *head, header, body);
}

Result<AppliedSlot> StateOwner::Apply(Candidate&& candidate, const DurableDecision& decision, const AuthorityVersion& current)
{
    const SlotHeaderV2& header{candidate.Header()};
    const std::shared_ptr<const HeadView> head{Head()};
    // A candidate always has a height >= 1 (ValidateSlotInputs gives parent height + 1).
    if (header.height == 0 || header.height - 1 != head->Header().height || header.parent_hash != head->HeaderHash()) {
        return Stale();
    }
    if (decision.JournalInstance() != m_commit_journal_instance) return Error{ErrorCode::VERIFY_FAILED, storage::TOKEN_OTHER_LOG};
    if (decision.Slot() != header.height || decision.Digest() != header.Hash()) {
        return Error{ErrorCode::VERIFY_FAILED, DECISION_OTHER_HEADER};
    }
    if (!(header.Authority() == current)) return Error{ErrorCode::AUTHORITY_CHANGED, AUTHORITY_SUPERSEDED};
    Result<HeadView> next{HeadView::Make(header, decision.Binding().cert_hash, candidate.Next())};
    if (!next.has_value()) return next.error();
    auto installed{std::make_shared<const HeadView>(std::move(next).value())};
    {
        LOCK(m_mutex);
        if (m_head != head) return Stale(); // another Apply moved the head after the checks
        m_head = installed;
    }
    ApplyReceipt receipt{decision.JournalInstance(), decision.Seq(), header.height, decision.Digest(), installed->Header().state_root};
    return Ok(AppliedSlot{std::move(installed), std::move(receipt)});
}

Result<ReplayResult> StateOwner::RestoreVerifiedGenesis(const HeadView& base,
                                                       std::span<const storage::JournalRecord> records, const SeatSet& seats)
{
    return RestoreGenesis(base, records, seats, ReplayObserver{});
}

Result<ReplayResult> StateOwner::RestoreGenesis(const HeadView& base, std::span<const storage::JournalRecord> records,
                                                const SeatSet& seats, const ReplayObserver& observer)
{
    const auto before{Head()};
    const HeadView& configured{*m_configured_genesis};
    if (before->Header() != configured.Header() || before->CertHash() != configured.CertHash() ||
        !(before->State() == configured.State())) return Stale();
    if (base.Header() != configured.Header() || base.CertHash() != configured.CertHash() ||
        !(base.State() == configured.State())) return Error{ErrorCode::VERIFY_FAILED, RESTORE_BASE_MISMATCH};
    if (seats.Size() == 0 || seats.Size() > MAX_SEATS || seats.Root() != base.Header().seat_set_root ||
        base.Header().authority_epoch != 0 || base.Header().domain.IsNull()) {
        return Error{ErrorCode::VERIFY_FAILED, RESTORE_AUTHORITY};
    }
    // Reconstruct the canonical empty-header fields as well as checking the
    // configured state's own commitment. No cryptographic work holds a lock.
    const auto canonical{GenesisHeader(base.Header().domain, base.Header().Authority(), base.Header().anchor,
                                       base.Header().facts_from, base.Header().state_root, CommitmentVersion(Profile()))};
    if (!canonical.has_value() || *canonical != base.Header()) return Error{ErrorCode::VERIFY_FAILED, RESTORE_BASE_MISMATCH};
    if (const auto checked{CheckRestoreHead(base, configured, Profile())}; !checked.has_value()) return checked.error();

    if (const auto verified{VerifyRestoreRecords(base.Header(), records, seats, m_commit_journal_instance, Profile())}; !verified.has_value()) {
        return verified.error();
    }
    return Install(before, base, records, observer);
}

Result<ReplayResult> StateOwner::Install(const std::shared_ptr<const HeadView>& before, const HeadView& base,
                                         std::span<const storage::JournalRecord> records, const ReplayObserver& observer)
{
    const HeadView& configured{*m_configured_genesis};
    Result<ReplayResult> replayed{ReplayDecisions(base, records, observer)};
    if (!replayed.has_value()) return replayed.error();
    if (base.Header().height != 0) {
        // A snapshot base is a restart point only once its mark is durable (O-95).
        const storage::SnapshotMark mark{base.Header().height, base.HeaderHash(), base.Header().state_root};
        if (std::find(replayed->marks.begin(), replayed->marks.end(), mark) == replayed->marks.end()) {
            return Error{ErrorCode::VERIFY_FAILED, RESTORE_BASE_UNMARKED};
        }
    }
    for (const HeadView* head : {&replayed->head, &replayed->decided_head}) {
        if (const auto checked{CheckRestoreHead(*head, configured, Profile())}; !checked.has_value()) return checked.error();
    }
    auto installed{std::make_shared<const HeadView>(replayed->head)};
    {
        LOCK(m_mutex);
        if (m_head != before) return Stale();
        m_head = std::move(installed);
    }
    return Ok(std::move(replayed).value());
}

Result<ReplayResult> StateOwner::Restore(const HeadView& base, std::span<const storage::JournalRecord> records, const SeatSet& seats,
                                         const ReplayObserver& observer)
{
    if (base.Header().height == 0) return RestoreGenesis(base, records, seats, observer);
    const auto before{Head()};
    const HeadView& configured{*m_configured_genesis};
    if (before->Header() != configured.Header() || before->CertHash() != configured.CertHash() ||
        !(before->State() == configured.State())) return Stale();
    if (seats.Size() == 0 || seats.Size() > MAX_SEATS || seats.Root() != base.Header().seat_set_root) {
        return Error{ErrorCode::VERIFY_FAILED, RESTORE_AUTHORITY};
    }
    // The snapshot's own state: this instance's domain, profile, epoch and seats, its commitment and invariants (O-54).
    if (const auto checked{CheckRestoreHead(base, configured, Profile())}; !checked.has_value()) return checked.error();
    if (const auto verified{VerifyRestoreRecords(base.Header(), records, seats, m_commit_journal_instance, Profile())}; !verified.has_value()) {
        return verified.error();
    }
    return Install(before, base, records, observer);
}

bool IsInputRefusal(ErrorCode code)
{
    switch (code) {
    case ErrorCode::NOT_IMPLEMENTED:
    case ErrorCode::UNSUPPORTED:
    case ErrorCode::DECODE:
    case ErrorCode::OVERSIZE:
    case ErrorCode::ARITHMETIC_OVERFLOW:
    case ErrorCode::VERIFY_FAILED:
    case ErrorCode::INVALID_INPUT:
        return true;
    case ErrorCode::UNSUPPORTED_NETWORK:
    case ErrorCode::INVARIANT:
    case ErrorCode::ORDER_VIOLATION:
    case ErrorCode::EQUIVOCATION_REFUSED:
    case ErrorCode::STORAGE_FATAL:
    case ErrorCode::FENCED:
    case ErrorCode::AUTHORITY_CHANGED:
    case ErrorCode::STALE_PARENT:
    case ErrorCode::BUSY:
    case ErrorCode::NEED_DATA:
        return false;
    }
    return false;
}

Result<PreviewSelection> PreviewExcludingRefused(const StateOwner& owner, const ParentRef& parent, const SlotInputs& inputs)
{
    Result<Candidate> full{owner.Preview(parent, inputs)};
    if (full.has_value()) return Ok(PreviewSelection{std::move(full).value(), {}});
    if (!IsInputRefusal(full.error().code) || inputs.body.originals.empty()) return full.error();

    std::vector<size_t> kept(inputs.body.originals.size());
    std::iota(kept.begin(), kept.end(), size_t{0});
    SlotInputs trial{inputs};
    const auto preview = [&](size_t count) {
        trial.body.originals.clear();
        for (size_t i{0}; i < count; ++i) trial.body.originals.push_back(inputs.body.originals[kept[i]]);
        return owner.Preview(parent, trial);
    };
    // The facts and the slot alone must preview, or no action is to blame.
    if (const Result<Candidate> none{preview(0)}; !none.has_value()) return full.error();

    std::vector<size_t> excluded;
    while (true) {
        // Prefix `low` of `kept` previews and prefix `high` fails with an input refusal.
        size_t low{0}, high{kept.size()};
        while (high - low > 1) {
            const size_t middle{low + (high - low) / 2};
            const Result<Candidate> tried{preview(middle)};
            if (tried.has_value()) {
                low = middle;
            } else if (IsInputRefusal(tried.error().code)) {
                high = middle;
            } else {
                return tried.error();
            }
        }
        excluded.push_back(kept[high - 1]);
        kept.erase(kept.begin() + static_cast<std::ptrdiff_t>(high - 1));
        Result<Candidate> again{preview(kept.size())};
        if (again.has_value()) {
            std::sort(excluded.begin(), excluded.end());
            return Ok(PreviewSelection{std::move(again).value(), std::move(excluded)});
        }
        // `kept` is never empty here: prefix 0 previews.
        if (!IsInputRefusal(again.error().code)) return again.error();
    }
}

} // namespace flowmesh2::state
