// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/staking.h>

#include <chain.h>
#include <consensus/era.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <logging.h>
#include <modern/stake.h>
#include <node/miner.h>
#include <node/stake_tracker.h>
#include <primitives/block.h>
#include <pubkey.h>
#include <util/strencodings.h>
#include <util/thread.h>
#include <util/time.h>
#include <validation.h>

#include <exception>
#include <memory>

namespace node {

namespace {
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
} // namespace

StakingLoop::StakingLoop(ChainstateManager& chainman, CTxMemPool* mempool)
    : m_chainman{chainman}, m_mempool{mempool} {}

StakingLoop::~StakingLoop()
{
    Stop();
}

bool StakingLoop::Start(const CKey& validator_key, const CScript& coinbase_script, std::string& error)
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
        m_key = validator_key;
        const XOnlyPubKey xonly{validator_key.GetPubKey()};
        std::copy(xonly.begin(), xonly.end(), m_validator.begin());
        m_coinbase_script = coinbase_script;
        m_running = true;
        m_stop = false;
        m_state = "starting";
        m_last_error.clear();
        m_next_block_time = 0;
    }
    m_thread = std::thread(&util::TraceThread, "b3staking", [this] { ThreadLoop(); });
    return true;
}

void StakingLoop::Stop()
{
    {
        LOCK(m_mutex);
        m_stop = true;
        m_cv.notify_all();
    }
    if (m_thread.joinable()) m_thread.join();
    LOCK(m_mutex);
    m_running = false;
    m_state = "stopped";
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
    if (!key || !params.legacy_b3coin || !Consensus::LegacyBoundaryPinned(params)) return;
    // One stake universe: the weights the validation rule will apply to the
    // next block (whole modern B3, bound + ACTIVE stake).
    const auto weights{chainstate.ModernEligibilityWeights(*key, *tip)};
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
    {
        LOCK(m_mutex);
        key = m_key;
        validator = m_validator;
        coinbase_script = m_coinbase_script;
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
        {
            LOCK(::cs_main);
            const CBlockIndex* tip{m_chainman.ActiveChain().Tip()};
            if (!tip) {
                if (!SleepUnlessStopped(std::chrono::seconds{1})) break;
                continue;
            }
            tip_hash = tip->GetBlockHash();
            next_height = tip->nHeight + 1;
        }
        if (!params.legacy_b3coin || !params.modern_pos || !Consensus::LegacyBoundaryPinned(params) ||
            Consensus::GetConsensusPhase(next_height, params) != Consensus::ConsensusPhase::MODERN_POS) {
            SetState(strprintf("waiting: the next block (height %d) is not a modern-PoS block (phase %s%s)",
                               next_height, PhaseName(params, next_height),
                               params.legacy_b3coin && !params.modern_pos ? ", modern-PoS rules not configured" : ""));
            if (!SleepUnlessStopped(std::chrono::seconds{10})) break;
            continue;
        }

        // Probe: the deterministic template tells us this validator's round
        // and the exact timestamp it must carry. Skip the template's own
        // validity check here -- before the round time the header is
        // legitimately "too new".
        BlockAssembler::Options options;
        options.coinbase_output_script = coinbase_script;
        options.modern_pos_validator_key = validator;
        options.test_block_validity = false;
        int64_t block_time{0};
        try {
            const auto tmpl{BlockAssembler(m_chainman.ActiveChainstate(), m_mempool, options).CreateNewBlock()};
            block_time = tmpl->block.GetBlockTime();
        } catch (const std::exception& e) {
            SetState(strprintf("waiting: %s", e.what()), e.what());
            if (!SleepUnlessStopped(std::chrono::seconds{10})) break;
            continue;
        }
        WITH_LOCK(m_mutex, m_next_block_time = block_time);

        // Pace: the network refuses a block before its forced timestamp, so
        // wait for it (or for the tip to move, which changes everything).
        bool tip_changed{false};
        SetState(strprintf("waiting for round time %d (height %d)", block_time, next_height));
        while (TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()) < block_time) {
            if (!SleepUnlessStopped(std::chrono::milliseconds{250})) return;
            if (WITH_LOCK(::cs_main, return m_chainman.ActiveChain().Tip()->GetBlockHash()) != tip_hash) {
                tip_changed = true;
                break;
            }
        }
        if (tip_changed) continue;
        if (WITH_LOCK(m_mutex, return m_stop)) break;

        // Produce: fresh template (latest transactions), final merkle root,
        // validator signature over the block hash, submit.
        try {
            options.test_block_validity = true;
            const auto tmpl{BlockAssembler(m_chainman.ActiveChainstate(), m_mempool, options).CreateNewBlock()};
            CBlock block{tmpl->block};
            if (block.hashPrevBlock != tip_hash) continue; // tip moved under us
            block.hashMerkleRoot = BlockMerkleRoot(block);
            if (!BlockAssembler::SignModernPosBlock(block, key, params)) {
                SetState("waiting: block signing failed", "block signing failed");
                if (!SleepUnlessStopped(std::chrono::seconds{5})) break;
                continue;
            }
            const uint256 hash{block.GetHash()};
            bool new_block{false};
            const auto shared_block{std::make_shared<const CBlock>(std::move(block))};
            if (!m_chainman.ProcessNewBlock(shared_block, /*force_processing=*/true, /*min_pow_checked=*/true, &new_block)) {
                SetState("waiting: produced block was rejected", strprintf("block %s rejected", hash.ToString()));
                if (!SleepUnlessStopped(std::chrono::seconds{2})) break;
                continue;
            }
            {
                LOCK(m_mutex);
                ++m_blocks_produced;
                m_last_block_hash = hash;
                m_state = "producing";
            }
            LogInfo("staking: produced modern-PoS block %s at height %d\n", hash.ToString(), next_height);
        } catch (const std::exception& e) {
            SetState(strprintf("waiting: %s", e.what()), e.what());
            if (!SleepUnlessStopped(std::chrono::seconds{5})) break;
            continue;
        }
    }
    LOCK(m_mutex);
    m_running = false;
    m_state = "stopped";
}

} // namespace node
