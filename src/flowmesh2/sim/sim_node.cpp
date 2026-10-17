// Copyright (c) 2026 The B3 developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <flowmesh2/sim/sim_node.h>

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

//! Everything Create builds, in dependency order, before the node exists.
struct SimNode::Parts {
    TraceRecorder* trace{nullptr};
    ProfileParams profile;
    NetworkId network;
    uint256 domain;
    FaultPlan faults;
    std::vector<MarketDescriptor> markets;
    std::unique_ptr<storage::MemoryJournal> signer_journal;
    std::unique_ptr<storage::MemoryJournal> commit_journal;
    std::unique_ptr<storage::SignerLog> signer_log;
    std::unique_ptr<storage::CommitLog> commit_log;
    std::unique_ptr<storage::SeatSigner> signer;
    std::optional<SeatSet> seats;
    AuthorityVersion authority;
    std::unique_ptr<dev::FakeChainAdapter> fake;
    std::unique_ptr<TracingChainAdapter> traced;
    std::optional<state::HeadView> genesis;
    std::unique_ptr<state::StateOwner> owner;
    std::unique_ptr<read::ReadService> read;
    std::unique_ptr<admission::ActionPool> pool;
    std::unique_ptr<dev::UnlimitedDevBudget> budget;
    std::unique_ptr<admission::Admission> admission;
    std::unique_ptr<net::LaneSet> lanes;
    std::unique_ptr<net::InProcessApi> api;
    std::unique_ptr<dev::DevSingleSeatCertifier> certifier;
};

SimNode::SimNode(Parts&& parts)
    : m_trace{*parts.trace},
      m_profile{parts.profile},
      m_network{parts.network},
      m_domain{parts.domain},
      m_faults{std::move(parts.faults)},
      m_markets{std::move(parts.markets)},
      m_signer_journal{std::move(parts.signer_journal)},
      m_commit_journal{std::move(parts.commit_journal)},
      m_signer_log{std::move(parts.signer_log)},
      m_commit_log{std::move(parts.commit_log)},
      m_signer{std::move(parts.signer)},
      m_seats{std::move(*parts.seats)},
      m_genesis_authority{parts.authority},
      m_fake{std::move(parts.fake)},
      m_traced{std::move(parts.traced)},
      m_genesis{std::move(*parts.genesis)},
      m_owner{std::move(parts.owner)},
      m_read{std::move(parts.read)},
      m_pool{std::move(parts.pool)},
      m_budget{std::move(parts.budget)},
      m_admission{std::move(parts.admission)},
      m_lanes{std::move(parts.lanes)},
      m_api{std::move(parts.api)},
      m_certifier{std::move(parts.certifier)}
{
}

SimNode::~SimNode() = default;

Result<std::unique_ptr<SimNode>> SimNode::Create(const SimNodeConfig& config, TraceRecorder& trace)
{
    if (const Result<void> plan{CheckFaultPlan(config.faults)}; !plan.has_value()) return plan.error();
    if ((config.faults.fail_commit_barrier_at.has_value() && *config.faults.fail_commit_barrier_at == 0) ||
        (config.faults.bump_authority_at.has_value() && *config.faults.bump_authority_at == 0) ||
        (config.faults.bump_authority_before_apply_at.has_value() && *config.faults.bump_authority_before_apply_at == 0)) {
        return Error{ErrorCode::INVALID_INPUT, SIM_FAULT_SLOT};
    }
    Parts parts;
    parts.trace = &trace;
    parts.faults = config.faults;
    Result<ProfileParams> profile{DevRegtestProfile(ChainType::REGTEST)};
    if (!profile.has_value()) return profile.error();
    parts.profile = *profile;
    parts.network = SimNetwork();
    parts.domain = DomainHash(parts.network, FM_VERSION);
    parts.markets = {SimMarketM1(), SimMarketM2()};

    // Journals (both observed by the trace) and their logs. B-DECISION of
    // slot s is the commit journal's barrier 2s - 1 (see sim/faults.h).
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
    Result<storage::SignerLog> signer_log{storage::SignerLog::Create(*parts.signer_journal)};
    if (!signer_log.has_value()) return signer_log.error();
    parts.signer_log = std::make_unique<storage::SignerLog>(std::move(signer_log).value());
    Result<storage::CommitLog> commit_log{storage::CommitLog::Create(*parts.commit_journal)};
    if (!commit_log.has_value()) return commit_log.error();
    parts.commit_log = std::make_unique<storage::CommitLog>(std::move(commit_log).value());

    // The one seat: TEST IKM; its PoP builds the seat set (no secret key here).
    const uint256 ikm{SimLabelHash("fm2/sim/seat0")};
    Result<storage::SeatSigner> signer{storage::SeatSigner::FromIKM(std::span<const unsigned char>{ikm.data(), ikm.size()}, *parts.signer_log)};
    if (!signer.has_value()) return signer.error();
    parts.signer = std::make_unique<storage::SeatSigner>(std::move(signer).value());
    parts.signer->SetSignObserver(&trace);
    const std::optional<bls::Signature> pop{bls::Signature::Decode(parts.signer->ProofOfPossession())};
    if (!pop.has_value()) return Error{ErrorCode::INVALID_INPUT, SIM_BAD_KEY};
    const std::vector<SeatRegistration> registrations{SeatRegistration{parts.signer->PublicKey(), *pop}};
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

    // State, reads, pool, budget, admission, lanes and the API.
    Result<state::HeadView> genesis{state::GenesisHead(parts.profile, parts.markets, parts.domain, parts.authority,
                                                       SimAnchor(SIM_BASE_HEIGHT), FactCursor{SIM_BASE_HEIGHT + 1, 0})};
    if (!genesis.has_value()) return genesis.error();
    parts.genesis = std::move(genesis).value();
    Result<std::unique_ptr<state::StateOwner>> owner{
        state::StateOwner::Create(parts.profile, *parts.genesis, parts.commit_journal->InstanceId())};
    if (!owner.has_value()) return owner.error();
    parts.owner = std::move(owner).value();
    const read::ReadLimits limits{8, read::CertifiedEnvelope::MAX_ENCODED_BYTES, 8, parts.profile.max_delay_slots};
    Result<std::unique_ptr<read::ReadService>> read{read::ReadService::Create(limits, parts.profile)};
    if (!read.has_value()) return read.error();
    parts.read = std::move(read).value();
    Result<std::unique_ptr<admission::ActionPool>> pool{
        admission::ActionPool::Create(admission::PoolLimits::FromProfile(parts.profile), parts.profile, 0)};
    if (!pool.has_value()) return pool.error();
    parts.pool = std::move(pool).value();
    Result<std::unique_ptr<dev::UnlimitedDevBudget>> budget{dev::UnlimitedDevBudget::Create(parts.profile)};
    if (!budget.has_value()) return budget.error();
    parts.budget = std::move(budget).value();
    Result<std::unique_ptr<admission::Admission>> admission{
        admission::Admission::Create(parts.profile, *parts.owner, *parts.read, *parts.pool, *parts.budget)};
    if (!admission.has_value()) return admission.error();
    parts.admission = std::move(admission).value();
    Result<std::unique_ptr<net::LaneSet>> lanes{net::LaneSet::Create(net::ProvisionalLaneLimits(parts.profile))};
    if (!lanes.has_value()) return lanes.error();
    parts.lanes = std::move(lanes).value();
    Result<std::unique_ptr<net::InProcessApi>> api{net::InProcessApi::Create(parts.profile, *parts.lanes, *parts.admission, *parts.pool,
                                                                             *parts.read, net::ApiLimits{config.max_replies})};
    if (!api.has_value()) return api.error();
    parts.api = std::move(api).value();

    // The k = 1 dev certifier over the traced adapter.
    Result<std::unique_ptr<dev::DevSingleSeatCertifier>> certifier{dev::DevSingleSeatCertifier::Create(
        parts.profile, *parts.traced, *parts.seats, *parts.signer, *parts.signer_log, *parts.commit_log)};
    if (!certifier.has_value()) return certifier.error();
    parts.certifier = std::move(certifier).value();

    return Ok(std::unique_ptr<SimNode>{new SimNode{std::move(parts)}});
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

    // 5. Agreement: checks 1 and 2, B-INTENT, sign, B-VOTE, B-DECISION.
    if (const Result<void> proposed{m_certifier->Propose(*candidate, inputs.authority)}; !proposed.has_value()) {
        return FenceOn(proposed.error());
    }
    std::optional<agreement::Decision> decision{m_certifier->TakeDecision()};
    if (!decision.has_value()) return FenceOn(Error{ErrorCode::INVARIANT, SIM_NO_DECISION});

    // The plan's authority bump after the decision is durable (check 3 refuses it, O-89).
    if (m_faults.bump_authority_before_apply_at == height) {
        const AuthorityVersion bumped{m_fake->BumpAuthority()};
        m_trace.Record("fault.bump_authority_before_apply", "epoch=" + std::to_string(bumped.epoch));
    }

    // 6. Apply under the authority read now (check 3).
    const AuthorityVersion current{m_traced->CurrentAuthority()};
    state::Candidate kept{*candidate};
    std::vector<OutcomeV2> outcomes{candidate->Outcomes()};
    Result<state::AppliedSlot> applied{m_owner->Apply(std::move(*candidate), decision->decision, current)};
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
    Result<std::shared_ptr<const read::SlotView>> view{read::SlotView::Build(*next, decision->certificate, outcomes)};
    if (!view.has_value()) return FenceAfterApply(view.error());
    if (const Result<void> published{m_read->Publish(*token, *view)}; !published.has_value()) return FenceAfterApply(published.error());
    m_trace.Record("read.publish", "slot=" + std::to_string(height));

    // 9. The pool learns of the slot last.
    Result<admission::CommitReport> report{m_pool->OnCommitted(*token, *next, outcomes, decision->body)};
    if (!report.has_value()) return FenceAfterApply(report.error());
    m_trace.Record("pool.on_committed", "slot=" + std::to_string(height) + ",included=" + std::to_string(report->included) +
                                            ",recorded=" + std::to_string(report->recorded) + ",nonce_used=" +
                                            std::to_string(report->nonce_used) + ",nonce_too_low=" +
                                            std::to_string(report->nonce_too_low) + ",expired=" + std::to_string(report->expired) +
                                            ",returned=" + std::to_string(report->returned));

    SealReport out{height, next->HeaderHash(), next->Header().action_count, next->Header().outcome_count};
    m_sealed.push_back(SealedSlot{height, next, std::move(kept), std::move(*decision), std::move(outcomes), std::move(view).value(),
                                  *report, taken->ids});
    return Ok(out);
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
