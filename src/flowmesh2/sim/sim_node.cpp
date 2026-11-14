// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/sim/sim_node.h>

#include <algorithm>

#include <crypto/bls.h>
#include <crypto/sha256.h>
#include <flowmesh2/core/facts.h>
#include <flowmesh2/core/hashes.h>
#include <flowmesh2/core/state_root.h>

#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace flowmesh2::sim {

namespace {

//! The error codes that fence the node before the head moves (O-90).
bool FencesNode(ErrorCode code);

} // namespace

uint256 SimLabelHash(std::string_view label)
{
    uint256 out;
    CSHA256{}.Write(reinterpret_cast<const unsigned char*>(label.data()), label.size()).Finalize(out.data());
    return out;
}

NetworkId SimNetwork()
{
    NetworkId network;
    network.chain = ChainType::REGTEST;
    network.magic = {0xf2, 0x51, 0x4d, 0x01};
    network.modern_chain_domain = SimLabelHash("fm2/sim/domain");
    return network;
}

NetworkId SimForeignNetwork()
{
    NetworkId network{SimNetwork()};
    network.magic = {0xf2, 0x51, 0x4d, 0x02};
    return network;
}

uint256 SimBlockHash(uint32_t height) { return SimLabelHash("fm2/sim/block/" + std::to_string(height)); }

AssetId SimAsset(std::string_view name)
{
    return AssetId{Tagged(HashTag::TEST_ASSET, std::span{reinterpret_cast<const unsigned char*>(name.data()), name.size()})};
}

MarketDescriptor SimMarketM1() { return MarketDescriptor{AssetId::Native(), SimAsset("rUSD"), 1'000'000, 1}; }
MarketDescriptor SimMarketM2() { return MarketDescriptor{SimAsset("TOK"), SimAsset("rUSD"), 1, 10}; }

//! What Create fixes before the node exists: the journals, the seats, the chain and the genesis.
struct SimNode::Parts {
    TraceRecorder* trace{nullptr};
    SimNodeConfig config;
    ProfileParams profile;
    NetworkId network;
    uint256 domain;
    std::vector<MarketDescriptor> markets;
    std::unique_ptr<storage::MemoryJournal> signer_journal;
    std::unique_ptr<storage::MemoryJournal> commit_journal;
    std::optional<SeatSet> seats;
    uint256 ikm;
    AuthorityVersion authority;
    std::unique_ptr<dev::FakeChainAdapter> fake;
    std::unique_ptr<TracingChainAdapter> traced;
    std::optional<state::HeadView> genesis;
};

SimNode::SimNode(Parts&& parts)
    : m_trace{*parts.trace},
      m_profile{parts.profile},
      m_network{parts.network},
      m_domain{parts.domain},
      m_faults{parts.config.faults},
      m_markets{std::move(parts.markets)},
      m_signer_journal{std::move(parts.signer_journal)},
      m_commit_journal{std::move(parts.commit_journal)},
      m_seats{std::move(*parts.seats)},
      m_genesis_authority{parts.authority},
      m_fake{std::move(parts.fake)},
      m_traced{std::move(parts.traced)},
      m_genesis{std::move(*parts.genesis)},
      m_config{parts.config},
      m_seat_count{parts.config.seat_count},
      m_self_seat{parts.config.self_seat},
      m_ikm{parts.ikm},
      m_mesh{parts.config.mesh}
{
}

SimNode::~SimNode() = default;

Result<std::unique_ptr<SimNode>> SimNode::Create(const SimNodeConfig& config, TraceRecorder& trace)
{
    if (const Result<void> plan{CheckFaultPlan(config.faults)}; !plan.has_value()) return plan.error();
    if ((config.faults.fail_commit_barrier_at.has_value() && *config.faults.fail_commit_barrier_at == 0) ||
        (config.faults.bump_authority_at.has_value() && *config.faults.bump_authority_at == 0) ||
        (config.faults.bump_authority_before_apply_at.has_value() && *config.faults.bump_authority_before_apply_at == 0) ||
        (config.faults.crash_before_slot.has_value() && *config.faults.crash_before_slot == 0)) {
        return Error{ErrorCode::INVALID_INPUT, SIM_FAULT_SLOT};
    }
    if (config.seat_count == 0 || config.self_seat >= config.seat_count) return Error{ErrorCode::INVALID_INPUT, SIM_SEAT_RANGE};
    if (config.seat_count > 1 && config.mesh == nullptr) return Error{ErrorCode::INVALID_INPUT, SIM_MESH_MISSING};
    Parts parts;
    parts.trace = &trace;
    parts.config = config;
    Result<ProfileParams> profile{DevRegtestProfile(ChainType::REGTEST)};
    if (!profile.has_value()) return profile.error();
    parts.profile = *profile;
    parts.network = SimNetwork();
    parts.domain = DomainHash(parts.network, FM_VERSION);
    parts.markets = {SimMarketM1(), SimMarketM2()};

    // Journals (both observed by the trace). B-DECISION of slot s is the
    // commit journal's barrier 2s - 1 (see sim/faults.h).
    storage::MemoryJournalFaults commit_faults;
    if (config.faults.fail_commit_barrier_at.has_value()) {
        const uint64_t slot{*config.faults.fail_commit_barrier_at};
        if (slot > std::numeric_limits<uint64_t>::max() / 2) return Error{ErrorCode::INVALID_INPUT, SIM_FAULT_SLOT};
        commit_faults.fail_barrier = 2 * slot - 1;
    }
    Result<std::unique_ptr<storage::MemoryJournal>> signer_journal{
        storage::MemoryJournal::Create(storage::JournalRole::SIGNER, config.signer_journal_instance, {}, &trace)};
    if (!signer_journal.has_value()) return signer_journal.error();
    parts.signer_journal = std::move(signer_journal).value();
    Result<std::unique_ptr<storage::MemoryJournal>> commit_journal{
        storage::MemoryJournal::Create(storage::JournalRole::COMMIT, config.commit_journal_instance, commit_faults, &trace)};
    if (!commit_journal.has_value()) return commit_journal.error();
    parts.commit_journal = std::move(commit_journal).value();

    // The seats: TEST IKM SHA256("fm2/sim/seat<i>") for every seat; the PoPs
    // build the seat set (k = 1 keeps "fm2/sim/seat0" byte for byte). The
    // simulator derives every seat's key only to build the set; the node signs
    // with seat self_seat's through its SeatSigner.
    std::vector<SeatRegistration> registrations;
    for (uint32_t seat{0}; seat < config.seat_count; ++seat) {
        const uint256 ikm{SimLabelHash("fm2/sim/seat" + std::to_string(seat))};
        if (seat == config.self_seat) parts.ikm = ikm;
        const std::optional<bls::SecretKey> key{bls::SecretKey::FromIKM(std::span<const unsigned char>{ikm.data(), ikm.size()})};
        if (!key.has_value()) return Error{ErrorCode::INVALID_INPUT, SIM_BAD_KEY};
        registrations.push_back(SeatRegistration{key->GetPublicKey(), key->SignPoP()});
    }
    Result<SeatSet> seats{SeatSet::FromPoP(registrations)};
    if (!seats.has_value()) return seats.error();
    parts.seats = std::move(seats).value();
    parts.authority = AuthorityVersion{0, parts.seats->Root(), ProfileHash(parts.profile)};

    // The chain, seen through the trace.
    Result<std::unique_ptr<dev::FakeChainAdapter>> fake{
        dev::FakeChainAdapter::Create(parts.network, parts.authority, *parts.seats, SimAnchor(SIM_BASE_HEIGHT))};
    if (!fake.has_value()) return fake.error();
    parts.fake = std::move(fake).value();
    parts.traced = std::make_unique<TracingChainAdapter>(*parts.fake, trace);

    Result<state::HeadView> genesis{state::GenesisHead(parts.profile, parts.markets, parts.domain, parts.authority,
                                                       SimAnchor(SIM_BASE_HEIGHT), FactCursor{SIM_BASE_HEIGHT + 1, 0})};
    if (!genesis.has_value()) return genesis.error();
    parts.genesis = std::move(genesis).value();

    std::unique_ptr<SimNode> node{new SimNode{std::move(parts)}};
    if (const Result<void> built{node->BuildComponents(false)}; !built.has_value()) return built.error();
    return Ok(std::move(node));
}

Result<void> SimNode::BuildComponents(bool restart)
{
    // Logs: fresh over empty journals, rebuilt from history on a restart (M4-STO-01).
    Result<storage::SignerLog> signer_log{restart ? storage::SignerLog::Recover(*m_signer_journal, m_profile)
                                                  : storage::SignerLog::Create(*m_signer_journal)};
    if (!signer_log.has_value()) return signer_log.error();
    m_signer_log = std::make_unique<storage::SignerLog>(std::move(signer_log).value());
    Result<storage::CommitLog> commit_log{restart ? storage::CommitLog::Recover(*m_commit_journal, m_profile)
                                                  : storage::CommitLog::Create(*m_commit_journal)};
    if (!commit_log.has_value()) return commit_log.error();
    m_commit_log = std::make_unique<storage::CommitLog>(std::move(commit_log).value());
    // Recorded evidence (M4-AGR-07): a restarted node that recorded any stays halted.
    Result<agreement::EvidenceStore> evidence{agreement::EvidenceStore::Create(m_profile, *m_traced, *m_commit_log)};
    if (!evidence.has_value()) return evidence.error();
    m_evidence = std::make_unique<agreement::EvidenceStore>(std::move(evidence).value());

    // The signer of this seat.
    Result<storage::SeatSigner> signer{storage::SeatSigner::FromIKM(std::span<const unsigned char>{m_ikm.data(), m_ikm.size()}, *m_signer_log)};
    if (!signer.has_value()) return signer.error();
    m_signer = std::make_unique<storage::SeatSigner>(std::move(signer).value());
    m_signer->SetSignObserver(&m_trace);
    if (!(m_seats.Keys()[m_self_seat].Key() == m_signer->PublicKey())) return Error{ErrorCode::INVALID_INPUT, SIM_BAD_KEY};

    // State: the genesis, or the committed head replayed from the commit journal on a restart.
    Result<std::unique_ptr<state::StateOwner>> owner{state::StateOwner::Create(m_profile, m_genesis, m_commit_journal->InstanceId())};
    if (!owner.has_value()) return owner.error();
    m_owner = std::move(owner).value();
    if (restart) {
        Result<std::vector<storage::JournalRecord>> records{m_commit_journal->ReadAll()};
        if (!records.has_value()) return records.error();
        if (const Result<void> restored{m_owner->Restore(m_genesis, *records)}; !restored.has_value()) return restored.error();
    }
    const std::shared_ptr<const state::HeadView> head{m_owner->Head()};

    // Reads, pool, budget, admission, lanes and the API.
    const read::ReadLimits limits{8, read::CertifiedEnvelope::MAX_ENCODED_BYTES, 8, m_profile.max_delay_slots};
    Result<std::unique_ptr<read::ReadService>> read{read::ReadService::Create(limits, m_profile)};
    if (!read.has_value()) return read.error();
    m_read = std::move(read).value();
    Result<std::unique_ptr<admission::ActionPool>> pool{
        admission::ActionPool::Create(admission::PoolLimits::FromProfile(m_profile), m_profile, head->Header().height)};
    if (!pool.has_value()) return pool.error();
    m_pool = std::move(pool).value();
    Result<std::unique_ptr<dev::UnlimitedDevBudget>> budget{dev::UnlimitedDevBudget::Create(m_profile)};
    if (!budget.has_value()) return budget.error();
    m_budget = std::move(budget).value();
    Result<std::unique_ptr<admission::Admission>> admission{admission::Admission::Create(m_profile, *m_owner, *m_read, *m_pool, *m_budget)};
    if (!admission.has_value()) return admission.error();
    m_admission = std::move(admission).value();
    Result<std::unique_ptr<net::LaneSet>> lanes{net::LaneSet::Create(net::ProvisionalLaneLimits(m_profile))};
    if (!lanes.has_value()) return lanes.error();
    m_lanes = std::move(lanes).value();
    Result<std::unique_ptr<net::InProcessApi>> api{
        net::InProcessApi::Create(m_profile, *m_lanes, *m_admission, *m_pool, *m_read, net::ApiLimits{m_config.max_replies})};
    if (!api.has_value()) return api.error();
    m_api = std::move(api).value();

    // The agreement: the k = 1 dev certifier, or PBFT over the mesh.
    m_certifier.reset();
    m_pbft.reset();
    m_proposed.reset();
    m_proposed_ids.clear();
    if (m_seat_count == 1) {
        Result<std::unique_ptr<dev::DevSingleSeatCertifier>> certifier{
            dev::DevSingleSeatCertifier::Create(m_profile, *m_traced, m_seats, *m_signer, *m_signer_log, *m_commit_log)};
        if (!certifier.has_value()) return certifier.error();
        m_certifier = std::move(certifier).value();
        return Ok();
    }
    m_rules = std::make_unique<agreement::PbftSafetyRules>();
    agreement::PbftConfig config{m_config.pbft};
    config.self_seat = m_self_seat;
    config.start_ms = m_now_ms;
    const agreement::PbftContext context{*m_traced, *m_owner, *m_signer, *m_signer_log, *m_commit_log, *m_rules, *m_mesh, *m_mesh, *m_evidence};
    Result<std::unique_ptr<agreement::PbftAgreement>> pbft{agreement::PbftAgreement::Create(m_profile, config, context)};
    if (!pbft.has_value()) return pbft.error();
    m_pbft = std::move(pbft).value();
    return Ok();
}

namespace {

bool FencesNode(ErrorCode code)
{
    switch (code) {
    case ErrorCode::STORAGE_FATAL:
    case ErrorCode::FENCED:
    case ErrorCode::AUTHORITY_CHANGED:
    case ErrorCode::EQUIVOCATION_REFUSED:
    case ErrorCode::INVARIANT:
    case ErrorCode::ORDER_VIOLATION:
        return true;
    default:
        return false;
    }
}

} // namespace

void SimNode::Fence(const Error& error)
{
    if (!m_admission->Fenced()) m_trace.Record("node.fence", std::string{ToString(error.code)});
    m_admission->Fence();
}

Error SimNode::FenceOn(const Error& error)
{
    if (FencesNode(error.code)) Fence(error);
    return error;
}

Error SimNode::FenceAfterApply(const Error& error)
{
    // The head already moved: the pool, the read service or the commit log
    // would disagree with it on every later call, so the node stops (O-90).
    Fence(error);
    return error;
}

Result<SealReport> SimNode::SealSlot()
{
    const std::shared_ptr<const state::HeadView> head{m_owner->Head()};
    const uint64_t height{head->Header().height + 1};
    m_trace.BeginSlot(height);
    Result<SealReport> sealed{SealNext(head, height)};
    m_trace.EndSlot(height, sealed.has_value() ? std::string_view{"OK"} : ToString(sealed.error().code));
    return sealed;
}

Result<SealReport> SimNode::SealNext(const std::shared_ptr<const state::HeadView>& head, uint64_t height)
{
    if (m_seat_count != 1) return Error{ErrorCode::INVALID_INPUT, SIM_MULTI_SEAT};
    Result<std::pair<state::Candidate, std::vector<ActionId>>> prepared{Prepare(head, height)};
    if (!prepared.has_value()) return prepared.error();
    state::Candidate& candidate{prepared->first};

    // 5. Agreement: checks 1 and 2, B-INTENT, sign, B-VOTE, B-DECISION.
    if (const Result<void> proposed{m_certifier->Propose(candidate, candidate.Header().Authority())}; !proposed.has_value()) {
        return FenceOn(proposed.error());
    }
    std::optional<agreement::Decision> decision{m_certifier->TakeDecision()};
    if (!decision.has_value()) return FenceOn(Error{ErrorCode::INVARIANT, SIM_NO_DECISION});

    // The plan's authority bump after the decision is durable (check 3 refuses it, O-89).
    if (m_faults.bump_authority_before_apply_at == height) {
        const AuthorityVersion bumped{m_fake->BumpAuthority()};
        m_trace.Record("fault.bump_authority_before_apply", "epoch=" + std::to_string(bumped.epoch));
    }
    return Finish(height, std::move(candidate), std::move(*decision), std::move(prepared->second));
}

Result<std::pair<state::Candidate, std::vector<ActionId>>> SimNode::Prepare(const std::shared_ptr<const state::HeadView>& head, uint64_t height)
{
    // 0. A fenced node seals nothing: no poll, take, preview, signature or write (O-89).
    if (m_admission->Fenced()) return Error{ErrorCode::FENCED, SIM_FENCED};

    // 1. Facts.
    const FactCursor from{head->Header().facts_to};
    Result<chain::PollReply> polled{m_traced->Poll(from, MAX_SLOT_FACTS)};
    if (!polled.has_value()) return FenceOn(polled.error());
    m_trace.Record("chain.poll", "from=" + std::to_string(from.height) + ":" + std::to_string(from.index) + "," +
                                     std::string{chain::ToString(polled->Status())});
    switch (polled->Status()) {
    case chain::PollStatus::FACTS:
        break;
    case chain::PollStatus::BUSY:
        return Error{ErrorCode::BUSY, SIM_POLL_BUSY};
    case chain::PollStatus::NEED_DATA:
        return Error{ErrorCode::NEED_DATA, SIM_POLL_NEED_DATA};
    case chain::PollStatus::AUTHORITY_CHANGED:
        return FenceOn(Error{ErrorCode::AUTHORITY_CHANGED, SIM_POLL_AUTHORITY});
    }
    const chain::FactBatch& batch{*polled->Batch()};

    // 2. Actions, minus the withheld ones (the filter runs without the pool lock).
    const admission::WithholdFilter withhold{[this](const ActionV2& action) {
        return m_faults.withhold_from_proposals.contains({action.Account(), action.nonce});
    }};
    Result<admission::TakenSlot> taken{m_pool->TakeForSlot(height, m_profile.max_actions_per_slot, withhold)};
    if (!taken.has_value()) return FenceOn(taken.error());
    m_trace.Record("pool.take", "slot=" + std::to_string(height) + ",n=" + std::to_string(taken->originals.size()));

    // 3. Preview on the committed head.
    state::SlotInputs inputs;
    inputs.authority = batch.authority;
    inputs.anchor = batch.anchor;
    inputs.facts_from = batch.from;
    inputs.facts_to = batch.to;
    for (const Fact& fact : batch.facts) {
        Result<std::vector<unsigned char>> bytes{EncodeFact(fact)};
        if (!bytes.has_value()) return FenceOn(bytes.error());
        inputs.body.facts.push_back(std::move(bytes).value());
    }
    inputs.body.originals = taken->originals;
    // An engine error that one action causes (O-34) is not fenced: the slot is
    // retried, and excluding that action is M4-ADM-03.
    Result<state::Candidate> candidate{m_owner->Preview(state::ParentRef::Committed(*head), inputs)};
    if (!candidate.has_value()) return FenceOn(candidate.error());
    m_trace.Record("state.preview", "slot=" + std::to_string(height) + ",header=" + candidate->Header().Hash().GetHex());

    // 4. The plan's authority bump (N11), between Preview and Propose.
    if (m_faults.bump_authority_at == height) {
        const AuthorityVersion bumped{m_fake->BumpAuthority()};
        m_trace.Record("fault.bump_authority", "epoch=" + std::to_string(bumped.epoch));
    }
    // The commit-barrier fault is exact only after two commit barriers per earlier slot (sim/faults.h);
    // once it fired (the journal is fatal) the plan is spent and there is nothing left to map.
    if (m_faults.fail_commit_barrier_at == height && !m_commit_journal->IsFatal() &&
        m_trace.BarrierCount(storage::JournalRole::COMMIT) != 2 * (height - 1)) {
        return FenceOn(Error{ErrorCode::INVARIANT, SIM_FAULT_MAPPING});
    }
    return Ok(std::pair{std::move(candidate).value(), taken->ids});
}

Result<SealReport> SimNode::Finish(uint64_t height, state::Candidate candidate, agreement::Decision decision, std::vector<ActionId> taken)
{
    // 6. Apply under the authority read now (check 3).
    const AuthorityVersion current{m_traced->CurrentAuthority()};
    state::Candidate kept{candidate};
    std::vector<OutcomeV2> outcomes{candidate.Outcomes()};
    Result<state::AppliedSlot> applied{m_owner->Apply(std::move(candidate), decision.decision, current)};
    if (!applied.has_value()) return FenceOn(applied.error());
    const std::shared_ptr<const state::HeadView> next{applied->head};
    m_trace.Record("state.apply", "slot=" + std::to_string(height) + ",root=" + next->Header().state_root.GetHex());

    // 7. B-APPLIED.
    // From here on the head has moved: every error fences the node (O-90).
    Result<storage::PendingApplied> pending{m_commit_log->AppendApplied(applied->receipt)};
    if (!pending.has_value()) return FenceAfterApply(pending.error());
    if (const Result<uint64_t> barrier{m_commit_log->Barrier()}; !barrier.has_value()) return FenceAfterApply(barrier.error());
    Result<DurableApplied> token{m_commit_log->Confirm(*pending)};
    if (!token.has_value()) return FenceAfterApply(token.error());

    // 8. Publication, after B-APPLIED.
    Result<std::shared_ptr<const read::SlotView>> view{read::SlotView::Build(*next, decision.certificate, outcomes)};
    if (!view.has_value()) return FenceAfterApply(view.error());
    if (const Result<void> published{m_read->Publish(*token, *view)}; !published.has_value()) return FenceAfterApply(published.error());
    m_trace.Record("read.publish", "slot=" + std::to_string(height));

    // 9. The pool learns of the slot last.
    Result<admission::CommitReport> report{m_pool->OnCommitted(*token, *next, outcomes, decision.body)};
    if (!report.has_value()) return FenceAfterApply(report.error());
    m_trace.Record("pool.on_committed", "slot=" + std::to_string(height) + ",included=" + std::to_string(report->included) +
                                            ",recorded=" + std::to_string(report->recorded) + ",nonce_used=" +
                                            std::to_string(report->nonce_used) + ",nonce_too_low=" +
                                            std::to_string(report->nonce_too_low) + ",expired=" + std::to_string(report->expired) +
                                            ",returned=" + std::to_string(report->returned));

    SealReport out{height, next->HeaderHash(), next->Header().action_count, next->Header().outcome_count};
    m_sealed.push_back(SealedSlot{height, next, std::move(kept), std::move(decision), std::move(outcomes), std::move(view).value(),
                                  *report, std::move(taken)});
    return Ok(out);
}

// ---- Multi-seat (k > 1) --------------------------------------------------------------

bool SimNode::IsLeader() { return m_pbft != nullptr && m_pbft->IsLeader(); }

std::optional<agreement::PbftStatus> SimNode::AgreementStatus()
{
    if (m_pbft == nullptr) return std::nullopt;
    return m_pbft->Status();
}

Result<uint64_t> SimNode::ProposeSlot()
{
    if (m_pbft == nullptr) return Error{ErrorCode::INVALID_INPUT, SIM_MULTI_SEAT};
    if (!m_pbft->IsLeader()) return Error{ErrorCode::INVALID_INPUT, SIM_NOT_LEADER};
    const std::shared_ptr<const state::HeadView> head{m_owner->Head()};
    const uint64_t height{head->Header().height + 1};
    m_trace.Record("slot.propose", "slot=" + std::to_string(height) + ",seat=" + std::to_string(m_self_seat));
    Result<std::pair<state::Candidate, std::vector<ActionId>>> prepared{Prepare(head, height)};
    if (!prepared.has_value()) return prepared.error();
    if (const Result<void> proposed{m_pbft->Propose(prepared->first, prepared->first.Header().Authority())}; !proposed.has_value()) {
        return FenceOn(proposed.error());
    }
    m_proposed = std::move(prepared->first);
    m_proposed_ids = std::move(prepared->second);
    return Ok(height);
}

Result<void> SimNode::OnFrame(const net::InboundFrame& frame)
{
    if (m_pbft == nullptr) return Error{ErrorCode::INVALID_INPUT, SIM_MULTI_SEAT};
    if (frame.lane != net::Lane::CRITICAL && frame.lane != net::Lane::PROPOSAL) return Ok();
    const Result<void> handled{m_pbft->OnMessage(frame.from_seat, frame.bytes)};
    if (!handled.has_value()) {
        if (handled.error().code == ErrorCode::STORAGE_FATAL || handled.error().code == ErrorCode::FENCED) Fence(handled.error());
        m_trace.Record("pbft.refuse", "seat=" + std::to_string(m_self_seat) + ",from=" + std::to_string(frame.from_seat) + "," +
                                          std::string{ToString(handled.error().code)} + "," + std::string{handled.error().detail});
        return handled.error();
    }
    return Ok();
}

Result<void> SimNode::Tick(uint64_t now_ms)
{
    if (m_pbft == nullptr) return Error{ErrorCode::INVALID_INPUT, SIM_MULTI_SEAT};
    m_now_ms = std::max(m_now_ms, now_ms);
    const Result<void> ticked{m_pbft->OnTimeout(now_ms)};
    if (!ticked.has_value() && (ticked.error().code == ErrorCode::STORAGE_FATAL || ticked.error().code == ErrorCode::FENCED)) {
        Fence(ticked.error());
    }
    return ticked;
}

Result<std::optional<SealReport>> SimNode::Complete()
{
    if (m_pbft == nullptr) return Error{ErrorCode::INVALID_INPUT, SIM_MULTI_SEAT};
    std::optional<agreement::Decision> decision{m_pbft->TakeDecision()};
    if (!decision.has_value()) return Ok(std::optional<SealReport>{});
    const uint64_t height{decision->header.height};
    m_trace.BeginSlot(height);
    std::optional<state::Candidate> candidate;
    std::vector<ActionId> taken;
    if (m_proposed.has_value() && m_proposed->Header().Hash() == decision->header.Hash()) {
        candidate = std::move(*m_proposed);
        taken = std::move(m_proposed_ids);
    } else {
        // A follower (or a leader whose proposal lost to another view): execute the decided slot.
        Result<state::Candidate> reexecuted{m_owner->Reexecute(decision->header, decision->body)};
        if (!reexecuted.has_value()) {
            m_trace.EndSlot(height, ToString(reexecuted.error().code));
            return FenceOn(reexecuted.error());
        }
        candidate = std::move(reexecuted).value();
    }
    m_proposed.reset();
    m_proposed_ids.clear();
    Result<SealReport> finished{Finish(height, std::move(*candidate), std::move(*decision), std::move(taken))};
    m_trace.EndSlot(height, finished.has_value() ? std::string_view{"OK"} : ToString(finished.error().code));
    if (!finished.has_value()) return finished.error();
    return Ok(std::optional<SealReport>{*finished});
}

Result<void> SimNode::Restart()
{
    m_trace.Record("node.restart", "seat=" + std::to_string(m_self_seat));
    return BuildComponents(true);
}

Result<net::PumpReport> SimNode::Pump(size_t max) { return m_api->Pump(max); }

Result<std::shared_ptr<const state::HeadView>> SimNode::ReapplySealed(uint64_t height)
{
    for (const SealedSlot& slot : m_sealed) {
        if (slot.height != height) continue;
        state::Candidate again{slot.candidate};
        Result<state::AppliedSlot> applied{m_owner->Apply(std::move(again), slot.decision.decision, m_traced->CurrentAuthority())};
        if (!applied.has_value()) return applied.error();
        return Ok(std::move(applied->head));
    }
    return Error{ErrorCode::INVALID_INPUT, SIM_NOT_SEALED};
}

} // namespace flowmesh2::sim
