// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/staking.h>

#include <arith_uint256.h>
#include <chain.h>
#include <consensus/era.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <hash.h>
#include <logging.h>
#include <modern/chain_domain.h>
#include <modern/pos_v1.h>
#include <modern/stake.h>
#include <net_processing.h>
#include <node/bridge_state.h>
#include <node/finality_signature.h>
#include <node/finality_tracker.h>
#include <node/miner.h>
#include <node/stake_tracker.h>
#include <primitives/block.h>
#include <pubkey.h>
#include <support/cleanse.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <util/thread.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <vector>

namespace node {

namespace {
constexpr const char* PREFERRED_PROPOSER_TAG{"B3/MODERN/POS/PREFERRED/V1"};
std::string PhaseName(const Consensus::Params& params, const int height)
{
    if (!params.legacy_b3coin) return "modern";
    switch (Consensus::GetConsensusPhase(height, params)) {
    case Consensus::ConsensusPhase::LEGACY_POS: return "legacy";
    case Consensus::ConsensusPhase::TRANSITION_POW: return "corridor";
    case Consensus::ConsensusPhase::MODERN_POS: return "modern_pos";
    }
    return "unknown";
}

uint256 PreferredProposerDraw(const uint256& chain_domain,
                              const uint256& seed, const uint256& set_hash,
                              const uint32_t height, const uint32_t round,
                              const uint32_t rank)
{
    HashWriter writer{TaggedHash(PREFERRED_PROPOSER_TAG)};
    writer << chain_domain << seed << set_hash << height << round << rank;
    return writer.GetSHA256();
}

uint64_t HashModulo(const uint256& value, const uint64_t modulus)
{
    Assume(modulus > 0);
    const arith_uint256 number{UintToArith256(value)};
    const arith_uint256 divisor{modulus};
    return (number - (number / divisor) * divisor).GetLow64();
}
} // namespace

PreferredProposerPlan ComputePreferredProposerPlan(
    const uint256& chain_domain, const uint256& seed, const int height,
    const int64_t round, const ValidatorSetSnapshot& set,
    const std::array<unsigned char, 32>& validator_key,
    const Consensus::ModernPosParams& pos)
{
    if (chain_domain.IsNull() || seed.IsNull() || height <= 0 || round < 0 ||
        round > std::numeric_limits<uint32_t>::max() || !pos.Valid() ||
        pos.round_seconds > std::numeric_limits<uint32_t>::max() ||
        set.TotalWeight() == 0 ||
        set.TotalWeight() > static_cast<uint64_t>(std::numeric_limits<CAmount>::max())) {
        return {};
    }

    struct Candidate {
        modern::PosValidatorKey key;
        uint64_t weight;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(set.Size());
    const CAmount total_weight{static_cast<CAmount>(set.TotalWeight())};
    const uint32_t h{static_cast<uint32_t>(height)};
    const uint32_t r{static_cast<uint32_t>(round)};
    for (const auto& member : set.Members()) {
        if (member.weight == 0 ||
            member.weight > static_cast<uint64_t>(std::numeric_limits<CAmount>::max())) {
            return {};
        }
        const CAmount weight{static_cast<CAmount>(member.weight)};
        const uint256 digest{modern::ModernPosEligibilityDigest(
            chain_domain, seed, h, r, member.validator_key)};
        if (!modern::ModernPosEligible(digest, weight, total_weight, round,
                                       pos)) {
            continue;
        }
        candidates.push_back({member.validator_key, member.weight});
    }
    if (candidates.empty()) {
        return {PreferredProposerAction::WAIT_NEXT_ROUND};
    }

    const uint32_t eligible_count{static_cast<uint32_t>(candidates.size())};
    const bool local_eligible{std::ranges::any_of(
        candidates, [&](const Candidate& candidate) {
            return candidate.key == validator_key;
        })};
    if (!local_eligible) {
        return {PreferredProposerAction::WAIT_NEXT_ROUND, 0,
                eligible_count};
    }

    // Mainnet's 30-second recovery round becomes unique five-second backup
    // windows. Keep one full window free at the end for propagation. Eligible
    // validators that are not drawn into one of the available windows defer
    // to the next consensus recovery round, where the order is freshly
    // derived.
    const auto round_duration{std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::seconds{pos.round_seconds})};
    const auto step{std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::seconds{std::min<int64_t>(5, pos.round_seconds)})};
    const auto whole_steps{round_duration / step};
    const uint32_t capacity{static_cast<uint32_t>(std::max<int64_t>(
        1, whole_steps - 1))};

    // Draw a deterministic stake-weighted permutation without replacement.
    // Sorting first removes all dependence on snapshot insertion order. Each
    // rank hashes the immutable round inputs plus that rank, maps the result
    // into the remaining stake interval, selects its owner, then removes it.
    // Only ranks that can actually send in this round need to be drawn; this
    // bounds the work to five linear passes on mainnet even at the maximum
    // validator-set size. This is advisory scheduling only; eligibility and
    // block validity remain exactly the existing consensus rules.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  return a.key < b.key;
              });
    uint64_t remaining_weight{0};
    for (const Candidate& candidate : candidates) {
        remaining_weight += candidate.weight;
    }
    for (uint32_t rank{0}; rank < capacity && !candidates.empty(); ++rank) {
        uint64_t draw{HashModulo(
            PreferredProposerDraw(chain_domain, seed, set.SetHash(), h, r,
                                  rank),
            remaining_weight)};
        auto selected{candidates.begin()};
        for (; selected != candidates.end(); ++selected) {
            if (draw < selected->weight) break;
            draw -= selected->weight;
        }
        if (selected == candidates.end()) return {};
        if (selected->key == validator_key) {
            const auto delay{step * rank};
            return {PreferredProposerAction::SCHEDULE, rank,
                    eligible_count, delay, step};
        }
        remaining_weight -= selected->weight;
        candidates.erase(selected);
    }
    return {PreferredProposerAction::WAIT_NEXT_ROUND, capacity,
            eligible_count};
}

StakingLoop::StakingLoop(ChainstateManager& chainman, CTxMemPool* mempool,
                         fs::path finality_signer_dir)
    : m_chainman{chainman}, m_mempool{mempool},
      m_finality_signer_dir{std::move(finality_signer_dir)} {}

bool StakingLoop::SetFinalityKey(const bls::SecretKey& key, const std::array<unsigned char, 32>& validator_key,
                                 std::string& error)
{
    LOCK(m_lifecycle_mutex);
    LOCK(m_mutex);
    if (m_running) {
        error = "cannot change the finality key while the staking loop is running";
        return false;
    }
    m_bls_key = key;
    m_validator = validator_key;
    m_finality_signing_failed = false;
    return true;
}

bool StakingLoop::ClearFinalityKey(std::string& error)
{
    LOCK(m_lifecycle_mutex);
    LOCK(m_mutex);
    if (m_running) {
        error = "cannot change the finality key while the staking loop is running";
        return false;
    }
    m_bls_key.reset();
    return true;
}

StakingLoop::~StakingLoop()
{
    Stop();
}

bool StakingLoop::Start(const CKey& validator_key, const CScript& coinbase_script, std::string& error)
{
    LOCK(m_lifecycle_mutex);
    return StartImpl(validator_key, coinbase_script, /*finality_key=*/nullptr, error);
}

bool StakingLoop::StartWithFinalityKey(const CKey& validator_key, const CScript& coinbase_script,
                                       const std::optional<bls::SecretKey>& finality_key,
                                       std::string& error)
{
    LOCK(m_lifecycle_mutex);
    return StartImpl(validator_key, coinbase_script, &finality_key, error);
}

bool StakingLoop::StartImpl(const CKey& validator_key, const CScript& coinbase_script,
                            const std::optional<bls::SecretKey>* finality_key,
                            std::string& error)
{
    if (!validator_key.IsValid()) {
        error = "invalid validator key";
        return false;
    }
    if (coinbase_script.empty()) {
        error = "empty coinbase script";
        return false;
    }
    {
        LOCK(m_mutex);
        if (m_running) {
            error = "staking is already running";
            return false;
        }
    }

    // Join a finished thread from a previous run before reusing the object.
    if (m_thread.joinable()) m_thread.join();
    {
        LOCK(m_mutex);
        const XOnlyPubKey xonly{validator_key.GetPubKey()};
        std::array<unsigned char, 32> validator{};
        std::copy(xonly.begin(), xonly.end(), validator.begin());

        if (finality_key != nullptr) {
            // This is an explicit replacement, including nullopt when the
            // current wallet has no usable live binding.
            m_bls_key = *finality_key;
        } else if (m_bls_key && m_validator != validator) {
            // Preserve the old arm-then-start API only for the validator it
            // was armed for. Never carry a BLS secret across identities.
            m_bls_key.reset();
        }
        m_key = validator_key;
        m_validator = validator;
        m_coinbase_script = coinbase_script;
        m_running = true;
        m_stop = false;
        m_state = "starting";
        m_last_error.clear();
        m_last_signed_height = -1;
        m_finality_signing_failed = false;
        m_next_block_time = 0;
        try {
            m_thread = std::thread(&util::TraceThread, "b3staking", [this] { ThreadLoop(); });
        } catch (const std::exception& e) {
            m_running = false;
            m_state = "stopped";
            m_key = CKey{};
            m_bls_key.reset();
            memory_cleanse(m_validator.data(), m_validator.size());
            if (!m_coinbase_script.empty()) {
                memory_cleanse(m_coinbase_script.data(), m_coinbase_script.size());
            }
            m_coinbase_script.clear();
            error = std::string{"unable to start staking thread: "} + e.what();
            return false;
        }
    }
    return true;
}

void StakingLoop::Stop()
{
    LOCK(m_lifecycle_mutex);
    {
        LOCK(m_mutex);
        m_stop = true;
        m_cv.notify_all();
    }
    if (m_thread.joinable()) m_thread.join();
    LOCK(m_mutex);
    m_running = false;
    m_state = "stopped";
    // Start() copies signing material into the node so staking can continue
    // after the wallet is re-locked. Stop() is the end of that authorization:
    // forget every copied key and its associated public routing data before a
    // different wallet can start this node-global loop.
    m_key = CKey{};
    m_bls_key.reset();
    memory_cleanse(m_validator.data(), m_validator.size());
    if (!m_coinbase_script.empty()) {
        memory_cleanse(m_coinbase_script.data(), m_coinbase_script.size());
    }
    m_coinbase_script.clear();
    m_next_block_time = 0;
}

bool StakingLoop::SleepUnlessStopped(const std::chrono::milliseconds d)
{
    WAIT_LOCK(m_mutex, lock);
    return !m_cv.wait_for(lock, d, [this]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_stop; });
}

void StakingLoop::SetState(const std::string& state, const std::string& error)
{
    LOCK(m_mutex);
    if (m_state != state) LogDebug(BCLog::VALIDATION, "staking: %s\n", state);
    m_state = state;
    if (!error.empty()) m_last_error = error;
}

void StakingLoop::FillChainFacts(interfaces::StakingStatus& status, const std::optional<std::array<unsigned char, 32>>& key)
{
    const Consensus::Params& params{m_chainman.GetConsensus()};
    status.min_stake_amount = params.min_stake_amount;
    status.stake_activation_depth = modern::STAKE_ACTIVATION_DEPTH;
    LOCK(::cs_main);
    Chainstate& chainstate{m_chainman.ActiveChainstate()};
    const CBlockIndex* tip{chainstate.m_chain.Tip()};
    if (!tip) return;
    status.tip_height = tip->nHeight;
    const int next_height{tip->nHeight + 1};
    status.next_block_phase = PhaseName(params, next_height);
    status.modern_pos_active = params.legacy_b3coin && params.modern_pos.has_value() &&
                               Consensus::LegacyBoundaryPinned(params) &&
                               Consensus::GetConsensusPhase(next_height, params) == Consensus::ConsensusPhase::MODERN_POS;
    if (!params.legacy_b3coin || !Consensus::LegacyBoundaryPinned(params)) return;
    // One stake universe: the epoch-frozen weights the validation rule will
    // apply to the next block (whole modern B3). Once an epoch set is in
    // force, newly ACTIVE stake is picked up only by a later certified set
    // rotation, never mid-epoch.
    const auto weights{chainstate.ModernEligibilityWeights(key, *tip)};
    if (!weights) return;
    status.active_weight = weights->first;
    status.total_active_weight = weights->second;
}

interfaces::StakingStatus StakingLoop::Status(const std::optional<std::array<unsigned char, 32>>& validator_key)
{
    interfaces::StakingStatus status;
    status.available = true;
    std::optional<std::array<unsigned char, 32>> key{validator_key};
    {
        LOCK(m_mutex);
        status.running = m_running;
        status.state = m_state;
        status.last_error = m_last_error;
        status.blocks_produced = m_blocks_produced;
        status.last_block_hash = m_last_block_hash;
        status.next_block_time = m_next_block_time;
        status.finality_signing =
            m_bls_key.has_value() && !m_finality_signing_failed;
        status.last_signed_height = m_last_signed_height;
        if (m_running) {
            status.validator_key = m_validator;
            if (!key) key = m_validator;
        }
    }
    FillChainFacts(status, key);
    return status;
}

void StakingLoop::ThreadLoop()
{
    CKey key;
    std::array<unsigned char, 32> validator{};
    CScript coinbase_script;
    FinalitySigner signer;
    {
        LOCK(m_mutex);
        key = m_key;
        validator = m_validator;
        coinbase_script = m_coinbase_script;
        if (m_bls_key) {
            const Consensus::Params& params{m_chainman.GetConsensus()};
            const auto domain{
                params.legacy_final_hash
                    ? modern::ModernChainDomain(
                          params.hashGenesisBlock,
                          *params.legacy_final_hash)
                    : std::nullopt};
            std::string error;
            if (!domain || !signer.SetKeyPersistent(
                               *m_bls_key, validator,
                               domain.value_or(uint256{}),
                               m_finality_signer_dir, error)) {
                if (error.empty()) error = "chain domain is not configured";
                m_finality_signing_failed = true;
                m_last_error = strprintf(
                    "finality signing disabled safely: %s", error);
            } else {
                m_last_signed_height = signer.LastSignedHeight();
            }
        }
    }
    const Consensus::Params& params{m_chainman.GetConsensus()};

    while (true) {
        if (WITH_LOCK(m_mutex, return m_stop)) break;

        if (m_chainman.IsInitialBlockDownload()) {
            SetState("waiting: initial block download");
            if (!SleepUnlessStopped(std::chrono::seconds{5})) break;
            continue;
        }

        uint256 tip_hash;
        int next_height{0};
        bool have_tip{false};
        {
            LOCK(::cs_main);
            const CBlockIndex* tip{m_chainman.ActiveChain().Tip()};
            if (tip) {
                tip_hash = tip->GetBlockHash();
                next_height = tip->nHeight + 1;
                have_tip = true;
            }
        }
        if (!have_tip) {
            if (!SleepUnlessStopped(std::chrono::seconds{1})) break;
            continue;
        }

        // Finality signing (liveness): sign every scheduled checkpoint we
        // are eligible for at the current tip, self-aggregate, relay. The
        // signer refuses everything the spec forbids (wrong key, repeat,
        // shallow, below the finalized checkpoint), so this is idempotent.
        if (signer.HasKey()) {
            std::vector<FinalitySig> sigs;
            {
                LOCK(::cs_main);
                Chainstate& chainstate{m_chainman.ActiveChainstate()};
                const CBlockIndex* tip{chainstate.m_chain.Tip()};
                FinalityTracker& tracker{chainstate.ModernFinality()};
                const BridgeStateIndex* bridge_index{nullptr};
                if (tip && Consensus::BridgeRulesActive(tip->nHeight,
                                                        params)) {
                    BridgeStateTracker& bridge{chainstate.ModernBridgeState()};
                    if (bridge.Sync(chainstate.m_chain, chainstate.m_blockman,
                                    params, *tip)) {
                        bridge_index = &bridge.Index();
                    }
                }
                if (tip && tracker.Sync(chainstate.m_chain,
                                        chainstate.m_blockman, params, *tip,
                                        bridge_index)) {
                    sigs = signer.MaybeSign(
                        tracker, chainstate.m_chain, params,
                        chainstate.FinalitySignatures(), bridge_index);
                }
            }
            // This is the local anti-repeat watermark, not merely the last
            // signature selected for relay. A valid old checkpoint can be
            // deliberately discarded by the bounded pool while still
            // advancing the signer, and RPC status must reflect that.
            WITH_LOCK(m_mutex,
                      m_last_signed_height = signer.LastSignedHeight());
            {
                LOCK(m_mutex);
                if (!signer.LastError().empty()) {
                    m_finality_signing_failed = true;
                    m_last_error = strprintf(
                        "finality signing disabled safely: %s",
                        signer.LastError());
                } else if (m_finality_signing_failed) {
                    // A branch-lock wait is recoverable only when a newer
                    // included quorum certificate supplies the lock-change
                    // proof. The signer rechecks that proof every loop.
                    m_finality_signing_failed = false;
                    if (m_last_error.starts_with(
                            "finality signing disabled safely:")) {
                        m_last_error.clear();
                    }
                }
            }
            if (!sigs.empty()) {
                LogInfo("staking: signed %d finality checkpoint(s) up to height %d\n", sigs.size(),
                        signer.LastSignedHeight());
                if (m_peerman) m_peerman->RelayFinalitySignatures(sigs);
            }
        }
        if (!params.legacy_b3coin || !params.modern_pos || !Consensus::LegacyBoundaryPinned(params) ||
            Consensus::GetConsensusPhase(next_height, params) != Consensus::ConsensusPhase::MODERN_POS) {
            SetState(strprintf("waiting: the next block (height %d) is not a modern-PoS block (phase %s%s)",
                               next_height, PhaseName(params, next_height),
                               params.legacy_b3coin && !params.modern_pos ? ", modern-PoS rules not configured" : ""));
            if (!SleepUnlessStopped(std::chrono::seconds{10})) break;
            continue;
        }

        // Read the immutable set and seed governing the next height. This is
        // local scheduling state only; received blocks still pass through the
        // unchanged consensus validation and fork-choice paths.
        std::shared_ptr<const ValidatorSetSnapshot> set;
        uint256 chain_domain;
        uint256 seed;
        int64_t parent_time{0};
        bool tip_changed{false};
        std::string coordination_error;
        try {
            LOCK(::cs_main);
            Chainstate& chainstate{m_chainman.ActiveChainstate()};
            const CBlockIndex* tip{chainstate.m_chain.Tip()};
            if (!tip || tip->GetBlockHash() != tip_hash) {
                tip_changed = true;
            } else {
                parent_time = tip->GetBlockTime();
                const auto domain{modern::ModernChainDomain(
                    params.hashGenesisBlock,
                    params.legacy_final_hash.value_or(uint256{}))};
                if (!domain) {
                    coordination_error = "modern chain domain is unavailable";
                } else {
                    chain_domain = *domain;
                    seed = Consensus::GetConsensusPhase(tip->nHeight, params) ==
                                   Consensus::ConsensusPhase::MODERN_POS
                               ? tip->m_modern_pos_digest
                               : modern::ModernPosGenesisSeed(
                                     chain_domain, tip->GetBlockHash());
                    // Synchronizes the finality/bridge trackers, then exposes
                    // the exact frozen set the assembler already uses.
                    const auto weights{
                        chainstate.ModernEligibilityWeights(validator, *tip)};
                    if (!weights) {
                        coordination_error =
                            "modern-PoS validator set is unavailable";
                    } else if (weights->first <= 0) {
                        coordination_error =
                            "validator is not in the active validator set (no bound, active stake)";
                    } else {
                        set = chainstate.ModernFinality().SetInForceAt(
                            next_height, params);
                        if (!set) {
                            coordination_error =
                                "modern-PoS validator set is unavailable";
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            coordination_error = e.what();
        }
        if (tip_changed) continue;
        if (!coordination_error.empty() || !set || seed.IsNull()) {
            if (coordination_error.empty()) {
                coordination_error = "modern-PoS eligibility seed is unavailable";
            }
            SetState(strprintf("waiting: %s", coordination_error),
                     coordination_error);
            if (!SleepUnlessStopped(std::chrono::seconds{2})) break;
            continue;
        }

        const Consensus::ModernPosParams& pos{*params.modern_pos};
        constexpr int64_t MAX_HEADER_TIME{
            std::numeric_limits<uint32_t>::max()};
        if (parent_time < 0 || parent_time > MAX_HEADER_TIME ||
            pos.block_interval_seconds > MAX_HEADER_TIME - parent_time ||
            pos.round_seconds > MAX_HEADER_TIME) {
            SetState("waiting: modern-PoS timestamp parameters are out of range",
                     "modern-PoS timestamp parameters are out of range");
            if (!SleepUnlessStopped(std::chrono::seconds{2})) break;
            continue;
        }
        const int64_t round_ms{pos.round_seconds * int64_t{1000}};
        const int64_t first_round_time_seconds{
            parent_time + pos.block_interval_seconds};
        const int64_t first_round_time_ms{
            first_round_time_seconds * int64_t{1000}};
        const int64_t now_ms{
            TicksSinceEpoch<std::chrono::milliseconds>(NodeClock::now())};
        const int64_t round{now_ms <= first_round_time_ms
                                ? 0
                                : (now_ms - first_round_time_ms) / round_ms};
        if (round < 0 || round > std::numeric_limits<uint32_t>::max()) {
            SetState("waiting: modern-PoS recovery round is out of range",
                     "modern-PoS recovery round is out of range");
            if (!SleepUnlessStopped(std::chrono::seconds{2})) break;
            continue;
        }
        if (round >
            (MAX_HEADER_TIME - first_round_time_seconds) /
                pos.round_seconds) {
            SetState("waiting: modern-PoS round timestamp is out of range",
                     "modern-PoS round timestamp is out of range");
            if (!SleepUnlessStopped(std::chrono::seconds{2})) break;
            continue;
        }

        PreferredProposerPlan proposer_plan;
        try {
            proposer_plan = ComputePreferredProposerPlan(
                chain_domain, seed, next_height, round, *set, validator, pos);
        } catch (const std::exception& e) {
            SetState(strprintf("waiting: proposer coordination failed: %s",
                               e.what()),
                     e.what());
            if (!SleepUnlessStopped(std::chrono::seconds{2})) break;
            continue;
        }

        const int64_t round_time_ms{first_round_time_ms + round * round_ms};
        const int64_t next_round_time_ms{round_time_ms + round_ms};
        enum class WaitResult { TIME_REACHED, TIP_CHANGED, STOPPED };
        const auto tip_is_current = [&] {
            return WITH_LOCK(
                ::cs_main,
                const CBlockIndex* current{m_chainman.ActiveChain().Tip()};
                return current && current->GetBlockHash() == tip_hash);
        };
        const auto wait_until_or_tip_change = [&](const int64_t target_ms) {
            while (TicksSinceEpoch<std::chrono::milliseconds>(
                       NodeClock::now()) < target_ms) {
                if (!SleepUnlessStopped(std::chrono::milliseconds{250})) {
                    return WaitResult::STOPPED;
                }
                if (!tip_is_current()) return WaitResult::TIP_CHANGED;
            }
            return WaitResult::TIME_REACHED;
        };

        if (proposer_plan.action == PreferredProposerAction::UNAVAILABLE) {
            WITH_LOCK(m_mutex, m_next_block_time = 0);
            SetState("waiting: proposer coordination inputs are unavailable",
                     "proposer coordination inputs are unavailable");
            if (!SleepUnlessStopped(std::chrono::seconds{2})) break;
            continue;
        }
        if (proposer_plan.action ==
            PreferredProposerAction::WAIT_NEXT_ROUND) {
            WITH_LOCK(m_mutex, m_next_block_time = 0);
            SetState(strprintf(
                "waiting: no proposer slot in recovery round %d at height %d; reranking next round",
                round, next_height));
            const WaitResult waited{
                wait_until_or_tip_change(next_round_time_ms)};
            if (waited == WaitResult::STOPPED) return;
            continue;
        }

        const int64_t send_time_ms{
            round_time_ms + proposer_plan.delay.count()};
        const int64_t send_deadline_ms{std::min(
            next_round_time_ms,
            send_time_ms + proposer_plan.window.count())};
        if (send_time_ms >= next_round_time_ms || now_ms >= send_deadline_ms) {
            WITH_LOCK(m_mutex, m_next_block_time = 0);
            SetState(strprintf(
                "waiting: proposer window expired in recovery round %d at height %d; reranking next round",
                round, next_height));
            const WaitResult waited{
                wait_until_or_tip_change(next_round_time_ms)};
            if (waited == WaitResult::STOPPED) return;
            continue;
        }

        WITH_LOCK(m_mutex, m_next_block_time = send_time_ms / 1000);
        SetState(strprintf(
            "waiting: proposer %d of %d in recovery round %d at height %d (send delay %d ms)",
            proposer_plan.rank + 1, proposer_plan.eligible_count, round,
            next_height, proposer_plan.delay.count()));
        const WaitResult waited{wait_until_or_tip_change(send_time_ms)};
        if (waited == WaitResult::STOPPED) return;
        if (waited == WaitResult::TIP_CHANGED) continue;
        if (!tip_is_current() ||
            TicksSinceEpoch<std::chrono::milliseconds>(NodeClock::now()) >=
                send_deadline_ms) {
            continue;
        }

        // Produce only inside this validator's unique advisory window. The
        // assembler rechecks ordinary eligibility for the requested existing
        // recovery round; validation never sees the local scheduling option.
        try {
            BlockAssembler::Options options;
            options.coinbase_output_script = coinbase_script;
            options.modern_pos_validator_key = validator;
            options.modern_pos_round = static_cast<uint32_t>(round);
            options.test_block_validity = true;
            const auto tmpl{BlockAssembler(m_chainman.ActiveChainstate(),
                                           m_mempool, options)
                                .CreateNewBlock()};
            CBlock block{tmpl->block};
            if (block.hashPrevBlock != tip_hash || !tip_is_current() ||
                TicksSinceEpoch<std::chrono::milliseconds>(NodeClock::now()) >=
                    send_deadline_ms) {
                continue;
            }
            block.hashMerkleRoot = BlockMerkleRoot(block);
            if (!BlockAssembler::SignModernPosBlock(block, key, params)) {
                SetState("waiting: block signing failed", "block signing failed");
                if (!SleepUnlessStopped(std::chrono::seconds{2})) break;
                continue;
            }
            const uint256 hash{block.GetHash()};
            if (!tip_is_current() ||
                TicksSinceEpoch<std::chrono::milliseconds>(NodeClock::now()) >=
                    send_deadline_ms) {
                continue;
            }
            bool new_block{false};
            const auto shared_block{
                std::make_shared<const CBlock>(std::move(block))};
            if (!m_chainman.ProcessNewBlock(
                    shared_block, /*force_processing=*/true,
                    /*min_pow_checked=*/true, &new_block)) {
                SetState("waiting: produced block was rejected",
                         strprintf("block %s rejected", hash.ToString()));
                if (!SleepUnlessStopped(std::chrono::seconds{2})) break;
                continue;
            }
            {
                LOCK(m_mutex);
                ++m_blocks_produced;
                m_last_block_hash = hash;
                m_state = "producing";
            }
            LogInfo("staking: produced modern-PoS block %s at height %d as proposer %d of %d\n",
                    hash.ToString(), next_height, proposer_plan.rank + 1,
                    proposer_plan.eligible_count);
        } catch (const std::exception& e) {
            SetState(strprintf("waiting: %s", e.what()), e.what());
            if (!SleepUnlessStopped(std::chrono::seconds{2})) break;
            continue;
        }
    }
    LOCK(m_mutex);
    m_running = false;
    m_state = "stopped";
}

} // namespace node
