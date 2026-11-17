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
    NetworkId network;
    uint256 domain;
    std::vector<MarketDescriptor> markets;
    std::unique_ptr<storage::MemoryJournal> signer_journal;
    std::unique_ptr<storage::MemoryJournal> commit_journal;
    AuthorityVersion authority;
    std::unique_ptr<dev::FakeChainAdapter> fake;
    std::unique_ptr<TracingChainAdapter> traced;
    std::unique_ptr<dev::UnlimitedDevBudget> budget;
};

SimNode::SimNode(Parts&& parts)
    : m_trace{*parts.trace},
      m_network{parts.network},
      m_domain{parts.domain},
      m_faults{parts.config.faults},
      m_markets{std::move(parts.markets)},
      m_signer_journal{std::move(parts.signer_journal)},
      m_commit_journal{std::move(parts.commit_journal)},
      m_genesis_authority{parts.authority},
      m_fake{std::move(parts.fake)},
      m_traced{std::move(parts.traced)},
      m_budget{std::move(parts.budget)},
      m_hooks{*this}
{
}

SimNode::~SimNode() = default;

Result<std::unique_ptr<SimNode>> SimNode::Create(const SimNodeConfig& config, TraceRecorder& trace)
{
    if (const Result<void> plan{CheckFaultPlan(config.faults)}; !plan.has_value()) return plan.error();
    if ((config.faults.fail_commit_barrier_at.has_value() && *config.faults.fail_commit_barrier_at == 0) ||
        (config.faults.bump_authority_at.has_value() && *config.faults.bump_authority_at == 0) ||
        (config.faults.bump_authority_before_apply_at.has_value() && *config.faults.bump_authority_before_apply_at == 0) ||
        (config.faults.crash_after_decision_at.has_value() && *config.faults.crash_after_decision_at == 0) ||
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
    uint256 ikm;
    for (uint32_t seat{0}; seat < config.seat_count; ++seat) {
        const uint256 seat_ikm{SimLabelHash("fm2/sim/seat" + std::to_string(seat))};
        if (seat == config.self_seat) ikm = seat_ikm;
        const std::optional<bls::SecretKey> key{bls::SecretKey::FromIKM(std::span<const unsigned char>{seat_ikm.data(), seat_ikm.size()})};
        if (!key.has_value()) return Error{ErrorCode::INVALID_INPUT, SIM_BAD_KEY};
        registrations.push_back(SeatRegistration{key->GetPublicKey(), key->SignPoP()});
    }
    Result<SeatSet> seats{SeatSet::FromPoP(registrations)};
    if (!seats.has_value()) return seats.error();
    parts.authority = AuthorityVersion{0, seats->Root(), ProfileHash(*profile)};

    // The chain, seen through the trace.
    Result<std::unique_ptr<dev::FakeChainAdapter>> fake{
        dev::FakeChainAdapter::Create(parts.network, parts.authority, *seats, SimAnchor(SIM_BASE_HEIGHT))};
    if (!fake.has_value()) return fake.error();
    parts.fake = std::move(fake).value();
    parts.traced = std::make_unique<TracingChainAdapter>(*parts.fake, trace);

    Result<state::HeadView> genesis{state::GenesisHead(*profile, parts.markets, parts.domain, parts.authority, SimAnchor(SIM_BASE_HEIGHT),
                                                       FactCursor{SIM_BASE_HEIGHT + 1, 0})};
    if (!genesis.has_value()) return genesis.error();

    Result<std::unique_ptr<dev::UnlimitedDevBudget>> budget{dev::UnlimitedDevBudget::Create(*profile)};
    if (!budget.has_value()) return budget.error();
    parts.budget = std::move(budget).value();

    std::unique_ptr<SimNode> node{new SimNode{std::move(parts)}};

    // The seat over the simulator's parts.
    seat::SeatNodeConfig seat_config;
    seat_config.profile = *profile;
    seat_config.seat_count = config.seat_count;
    seat_config.self_seat = config.self_seat;
    seat_config.ikm.assign(ikm.begin(), ikm.end());
    seat_config.max_replies = config.max_replies;
    seat_config.pbft = config.pbft;
    seat_config.streams = config.streams;
    seat::SeatNodeParts seat_parts;
    seat_parts.chain = node->m_traced.get();
    seat_parts.signer_journal = node->m_signer_journal.get();
    seat_parts.commit_journal = node->m_commit_journal.get();
    seat_parts.budget = node->m_budget.get();
    seat_parts.mesh = config.mesh;
    seat_parts.sign_observer = &trace;
    seat_parts.hooks = &node->m_hooks;
    seat_parts.agreement = [](const seat::AgreementParts& p) -> Result<std::unique_ptr<agreement::Agreement>> {
        Result<std::unique_ptr<dev::DevSingleSeatCertifier>> certifier{
            dev::DevSingleSeatCertifier::Create(p.profile, p.chain, p.seats, p.signer, p.signer_log, p.commit_log)};
        if (!certifier.has_value()) return certifier.error();
        return Ok(std::unique_ptr<agreement::Agreement>{std::move(certifier).value()});
    };
    Result<std::unique_ptr<seat::SeatNode>> seat_node{seat::SeatNode::Create(seat_config, seat_parts, *seats, *genesis)};
    if (!seat_node.has_value()) return seat_node.error();
    node->m_node = std::move(seat_node).value();
    return Ok(std::move(node));
}

// ---- The seat's hooks: the trace and the fault plan ----------------------------------

void SimNode::Hooks::Record(std::string_view kind, std::string detail) { m_node.m_trace.Record(kind, std::move(detail)); }
void SimNode::Hooks::BeginSlot(uint64_t height) { m_node.m_trace.BeginSlot(height); }
void SimNode::Hooks::EndSlot(uint64_t height, std::string_view result) { m_node.m_trace.EndSlot(height, result); }

bool SimNode::Hooks::Withhold(const ActionV2& action)
{
    return m_node.m_faults.withhold_from_proposals.contains({action.Account(), action.nonce});
}

Result<void> SimNode::Hooks::AfterPreview(uint64_t height)
{
    // 4. The plan's authority bump (N11), between Preview and Propose.
    if (m_node.m_faults.bump_authority_at == height) {
        const AuthorityVersion bumped{m_node.m_fake->BumpAuthority()};
        m_node.m_trace.Record("fault.bump_authority", "epoch=" + std::to_string(bumped.epoch));
    }
    // The commit-barrier fault is exact only after two commit barriers per earlier slot (sim/faults.h);
    // once it fired (the journal is fatal) the plan is spent and there is nothing left to map.
    if (m_node.m_faults.fail_commit_barrier_at == height && !m_node.m_commit_journal->IsFatal() &&
        m_node.m_trace.BarrierCount(storage::JournalRole::COMMIT) != 2 * (height - 1)) {
        return Error{ErrorCode::INVARIANT, SIM_FAULT_MAPPING};
    }
    return Ok();
}

Result<void> SimNode::Hooks::AfterDecision(uint64_t height)
{
    // The plan's authority bump after the decision is durable (check 3 refuses it, O-89).
    if (m_node.m_faults.bump_authority_before_apply_at == height) {
        const AuthorityVersion bumped{m_node.m_fake->BumpAuthority()};
        m_node.m_trace.Record("fault.bump_authority_before_apply", "epoch=" + std::to_string(bumped.epoch));
    }
    // The plan's crash: the DECISION is durable, nothing is applied, the node stops (a restart recovers it).
    if (m_node.m_faults.crash_after_decision_at == height) {
        m_node.m_trace.Record("fault.crash_after_decision", "slot=" + std::to_string(height));
        return Error{ErrorCode::STORAGE_FATAL, SIM_CRASH_AFTER_DECISION};
    }
    return Ok();
}

void SimNode::Hooks::OnSealed(SealedSlot&& sealed) { m_node.m_sealed.push_back(std::move(sealed)); }

// ---- The wrapper's own methods ----------------------------------------------------------

Result<void> SimNode::Restart()
{
    m_trace.Record("node.restart", "seat=" + std::to_string(m_node->SelfSeat()));
    return m_node->Restart();
}

Result<std::shared_ptr<const state::HeadView>> SimNode::ReapplySealed(uint64_t height)
{
    for (const SealedSlot& slot : m_sealed) {
        if (slot.height != height) continue;
        state::Candidate again{slot.candidate};
        Result<state::AppliedSlot> applied{m_node->Owner().Apply(std::move(again), slot.decision.decision, m_traced->CurrentAuthority())};
        if (!applied.has_value()) return applied.error();
        return Ok(std::move(applied->head));
    }
    return Error{ErrorCode::INVALID_INPUT, SIM_NOT_SEALED};
}

} // namespace flowmesh2::sim
