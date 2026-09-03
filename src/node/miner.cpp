// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/miner.h>

#include <chain.h>
#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/era.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <key.h>
#include <logging.h>
#include <modern/fn.h>
#include <modern/fn_genesis_validation.h>
#include <modern/fn_pod.h>
#include <modern/finality_certificate.h>
#include <modern/payload_root.h>
#include <modern/pos_v1.h>
#include <node/bridge_state.h>
#include <node/finality_binding_index.h>
#include <node/finality_signature.h>
#include <node/finality_tracker.h>
#include <node/flowmesh_checkpoint_index.h>
#include <node/flowmesh_vault_index.h>
#include <node/fn_seat_index.h>
#include <node/stake_tracker.h>
#include <node/context.h>
#include <node/kernel_notifications.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <streams.h>
#include <util/moneystr.h>
#include <util/signalinterrupt.h>
#include <util/time.h>
#include <validation.h>

#include <algorithm>
#include <utility>
#include <numeric>

namespace node {

int64_t GetMinimumTime(const CBlockIndex* pindexPrev, const int64_t difficulty_adjustment_interval)
{
    int64_t min_time{pindexPrev->GetMedianTimePast() + 1};
    // Height of block to be mined.
    const int height{pindexPrev->nHeight + 1};
    // Account for BIP94 timewarp rule on all networks. This makes future
    // activation safer.
    if (height % difficulty_adjustment_interval == 0) {
        min_time = std::max<int64_t>(min_time, pindexPrev->GetBlockTime() - MAX_TIMEWARP);
    }
    return min_time;
}

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime{std::max<int64_t>(GetMinimumTime(pindexPrev, consensusParams.DifficultyAdjustmentInterval()),
                                       TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()))};

    if (nOldTime < nNewTime) {
        pblock->nTime = nNewTime;
    }

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks) {
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);
    }

    return nNewTime - nOldTime;
}

void RegenerateCommitments(CBlock& block, ChainstateManager& chainman)
{
    CMutableTransaction tx{*block.vtx.at(0)};
    tx.vout.erase(tx.vout.begin() + GetWitnessCommitmentIndex(block));
    block.vtx.at(0) = MakeTransactionRef(tx);

    const CBlockIndex* prev_block = WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(block.hashPrevBlock));
    chainman.GenerateCoinbaseCommitment(block, prev_block);

    block.hashMerkleRoot = BlockMerkleRoot(block);
}

static BlockAssembler::Options ClampOptions(BlockAssembler::Options options)
{
    // Apply DEFAULT_BLOCK_RESERVED_WEIGHT when the caller left it unset.
    options.block_reserved_weight = std::clamp<size_t>(options.block_reserved_weight.value_or(DEFAULT_BLOCK_RESERVED_WEIGHT), MINIMUM_BLOCK_RESERVED_WEIGHT, MAX_BLOCK_WEIGHT);
    options.coinbase_output_max_additional_sigops = std::clamp<size_t>(options.coinbase_output_max_additional_sigops, 0, MAX_BLOCK_SIGOPS_COST);
    // Limit weight to between block_reserved_weight and MAX_BLOCK_WEIGHT for sanity:
    // block_reserved_weight can safely exceed -blockmaxweight, but the rest of the block template will be empty.
    options.nBlockMaxWeight = std::clamp<size_t>(options.nBlockMaxWeight, *options.block_reserved_weight, MAX_BLOCK_WEIGHT);
    return options;
}

BlockAssembler::BlockAssembler(Chainstate& chainstate, const CTxMemPool* mempool, const Options& options)
    : chainparams{chainstate.m_chainman.GetParams()},
      m_mempool{options.use_mempool ? mempool : nullptr},
      m_chainstate{chainstate},
      m_options{ClampOptions(options)}
{
}

void ApplyArgsManOptions(const ArgsManager& args, BlockAssembler::Options& options)
{
    // Block resource limits
    options.nBlockMaxWeight = args.GetIntArg("-blockmaxweight", options.nBlockMaxWeight);
    if (const auto blockmintxfee{args.GetArg("-blockmintxfee")}) {
        if (const auto parsed{ParseMoney(*blockmintxfee)}) options.blockMinFeeRate = CFeeRate{*parsed};
    }
    options.print_modified_fee = args.GetBoolArg("-printpriority", options.print_modified_fee);
    if (!options.block_reserved_weight) {
        options.block_reserved_weight = args.GetIntArg("-blockreservedweight");
    }
}

void BlockAssembler::resetBlock()
{
    // Reserve space for fixed-size block header, txs count, and coinbase tx.
    nBlockWeight = *Assert(m_options.block_reserved_weight);
    nBlockSigOpsCost = m_options.coinbase_output_max_additional_sigops;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
    m_fn_pod_issued_total.reset();
}

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock()
{
    const auto time_start{SteadyClock::now()};

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());
    CBlock* const pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction. It is skipped by the
    // getblocktemplate RPC and mining interface consumers must not use it.
    pblock->vtx.emplace_back();

    LOCK(::cs_main);
    CBlockIndex* pindexPrev = m_chainstate.m_chain.Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    pblock->nVersion = m_chainstate.m_chainman.m_versionbitscache.ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.MineBlocksOnDemand()) {
        pblock->nVersion = gArgs.GetIntArg("-blockversion", pblock->nVersion);
    }
    // B3: block production is era- and phase-aware. Legacy-era production is
    // the historical staker's job and is not supported here; MODERN-era
    // blocks must carry the codec marker so their body serializes with the
    // modern codec and their identity stays in the modern hash domain.
    const Consensus::Params& b3_consensus{chainparams.GetConsensus()};
    const bool b3_modern{b3_consensus.legacy_b3coin &&
                         Consensus::GetB3Era(nHeight, b3_consensus) == Consensus::B3Era::MODERN};
    const bool b3_corridor{b3_consensus.legacy_b3coin &&
                           Consensus::GetConsensusPhase(nHeight, b3_consensus) ==
                               Consensus::ConsensusPhase::TRANSITION_POW};
    const bool b3_bridge_active{
        b3_modern && Consensus::BridgeRulesActive(nHeight, b3_consensus)};
    const bool b3_bridge_withdrawals_active{
        b3_modern &&
        Consensus::BridgeWithdrawalRulesActive(nHeight, b3_consensus)};
    if (b3_consensus.legacy_b3coin && !b3_modern) {
        throw std::runtime_error("legacy-era B3 block production is not supported");
    }
    // X-distribution PAUSE (owner ruling 2026-08-23): with H configured and
    // X unset, no post-H block may be produced -- a blank-X node must never
    // enter the corridor. Validation refuses such blocks too; this is the
    // production-side half of the same fail-closed rule.
    if (b3_modern && Consensus::LegacyBoundaryHeightOnly(b3_consensus)) {
        throw std::runtime_error("legacy boundary hash X is not pinned; post-H block production is refused");
    }
    if (b3_modern) {
        pblock->nVersion = static_cast<int32_t>(Consensus::B3_BLOCK_CODEC_V2_VERSION);
    }
    if (b3_corridor && (!b3_consensus.transition_pow_bits || !IsCanonicalCompactBits(*b3_consensus.transition_pow_bits))) {
        throw std::runtime_error("temporary-PoW corridor difficulty is not configured (or not canonical compact bits)");
    }
    if (b3_corridor && !b3_consensus.transition_pow_reward) {
        throw std::runtime_error("temporary-PoW corridor reward is not configured");
    }
    if (b3_modern && Consensus::FnPodRulesActive(nHeight, b3_consensus)) {
        if (!b3_consensus.fn_pod_activation_height ||
            pindexPrev->nHeight < *b3_consensus.fn_pod_activation_height) {
            m_fn_pod_issued_total = 0;
        } else if (pindexPrev->m_fn_pod_issued_total_known) {
            m_fn_pod_issued_total = pindexPrev->m_fn_pod_issued_total;
        } else {
            throw std::runtime_error("modern FN PoD counter for the parent is unavailable");
        }
        if (!modern::ModernFnCapacity(b3_consensus)) {
            throw std::runtime_error("historical FN count exceeds the lifetime cap");
        }
    }
    if (b3_modern && Consensus::FlowMeshRulesActive(nHeight, b3_consensus)) {
        node::FnSeatTracker& seats{m_chainstate.ModernFnSeats()};
        if (!seats.Sync(m_chainstate.m_chain, m_chainstate.m_blockman,
                        b3_consensus, *pindexPrev)) {
            throw std::runtime_error(
                "FlowMesh FN-seat index is unavailable for block assembly");
        }
        node::FlowMeshCheckpointTracker& checkpoints{
            m_chainstate.ModernFlowMeshCheckpoints()};
        node::FlowMeshVaultTracker& vaults{
            m_chainstate.ModernFlowMeshVaults()};
        if (!vaults.Sync(m_chainstate.m_chain, m_chainstate.m_blockman,
                         b3_consensus, *pindexPrev)) {
            throw std::runtime_error(
                "FlowMesh vault index is unavailable for block assembly");
        }
        if (!checkpoints.Sync(m_chainstate.m_chain,
                              m_chainstate.m_blockman, b3_consensus,
                              seats.Index(), vaults.Index(), *pindexPrev)) {
            throw std::runtime_error(
                "FlowMesh checkpoint index is unavailable for block assembly");
        }
    }
    BridgeBurnReadiness bridge_burn_readiness{
        b3_bridge_withdrawals_active ? BridgeBurnReadiness::READY
                                     : BridgeBurnReadiness::UNAVAILABLE};
    if (b3_bridge_active) {
        node::BridgeStateTracker& bridge{m_chainstate.ModernBridgeState()};
        if (!bridge.Sync(m_chainstate.m_chain, m_chainstate.m_blockman,
                         b3_consensus, *pindexPrev)) {
            throw std::runtime_error(
                "bridge state is unavailable for block assembly");
        }
        if (b3_bridge_withdrawals_active &&
            b3_consensus.busd_bridge->withdrawal_mode ==
                Consensus::BridgeWithdrawalMode::DECENTRALIZED_VERIFIER_V1 &&
            b3_consensus.busd_bridge->decentralized_withdrawal) {
            bridge_burn_readiness = BridgeBurnReadiness::UNAVAILABLE;
            node::FinalityTracker& finality{m_chainstate.ModernFinality()};
            if (finality.Sync(m_chainstate.m_chain,
                              m_chainstate.m_blockman, b3_consensus,
                              *pindexPrev, &bridge.Index())) {
                const node::FinalityTracker::State projected{
                    finality.Projected(nHeight, b3_consensus)};
                bridge_burn_readiness =
                    node::BridgeWithdrawalValidatorSetsReady(
                        projected,
                        *b3_consensus.busd_bridge
                             ->decentralized_withdrawal)
                        ? BridgeBurnReadiness::READY
                        : BridgeBurnReadiness::NOT_READY;
            }
        }
    }

    // The historical FN supply is a mandatory one-time coinbase event in the
    // first corridor block. Resolve and reserve it before mempool selection so
    // ordinary transactions can never crowd the pinned outputs out.
    std::vector<CTxOut> fn_genesis_outputs;
    if (b3_modern && b3_consensus.hard_fork_height &&
        nHeight == *b3_consensus.hard_fork_height &&
        (b3_consensus.fn_genesis_required ||
         modern::HasFnGenesisConfigurationIntent(b3_consensus))) {
        std::string fn_error;
        auto expected{modern::ExpectedFnGenesisOutputs(b3_consensus, fn_error)};
        if (!expected) {
            throw std::runtime_error("invalid FN Genesis configuration: " + fn_error);
        }
        fn_genesis_outputs = std::move(*expected);
        const uint64_t fn_weight{static_cast<uint64_t>(GetSerializeSize(fn_genesis_outputs)) *
                                 WITNESS_SCALE_FACTOR};
        if (fn_weight > m_options.nBlockMaxWeight - nBlockWeight) {
            throw std::runtime_error("FN Genesis outputs exceed the configured block weight");
        }
        uint64_t fn_sigops_cost{0};
        for (const CTxOut& out : fn_genesis_outputs) {
            fn_sigops_cost += static_cast<uint64_t>(
                out.scriptPubKey.GetSigOpCount(/*fAccurate=*/false)) *
                WITNESS_SCALE_FACTOR;
        }
        if (fn_sigops_cost > MAX_BLOCK_SIGOPS_COST - nBlockSigOpsCost) {
            throw std::runtime_error("FN Genesis outputs exceed the block sigop limit");
        }
        nBlockWeight += fn_weight;
        nBlockSigOpsCost += fn_sigops_cost;
    }
    // Modern-PoS production (frozen V1 spec §3-§5): fully deterministic —
    // resolve the seed and the validator's weights, find the smallest
    // eligible recovery round, and force the exact round timestamp. The
    // caller signs after finalizing the merkle root.
    const bool b3_modern_pos{b3_modern && !b3_corridor};
    int64_t pos_round{0};
    if (b3_modern_pos) {
        if (!b3_consensus.modern_pos) {
            throw std::runtime_error("modern-PoS parameters are not configured");
        }
        if (!m_options.modern_pos_validator_key) {
            throw std::runtime_error("modern-PoS production requires a validator key");
        }
        const auto domain{modern::ModernChainDomain(b3_consensus.hashGenesisBlock,
                                                    b3_consensus.legacy_final_hash.value_or(uint256{}))};
        if (!domain) {
            throw std::runtime_error("modern chain domain is not pinned");
        }
        const uint256 seed{Consensus::GetConsensusPhase(pindexPrev->nHeight, b3_consensus) ==
                                   Consensus::ConsensusPhase::MODERN_POS
                               ? pindexPrev->m_modern_pos_digest
                               : modern::ModernPosGenesisSeed(*domain, pindexPrev->GetBlockHash())};
        if (seed.IsNull()) {
            throw std::runtime_error("modern-PoS seed for the parent is unavailable");
        }
        // One stake universe: the same (w, W) validation will apply, from the
        // validator set in force at this height (bound + ACTIVE stake).
        const auto weights{m_chainstate.ModernEligibilityWeights(*m_options.modern_pos_validator_key, *pindexPrev)};
        if (!weights) {
            throw std::runtime_error("modern-PoS validator set is unavailable");
        }
        const auto [w, W]{*weights};
        if (w <= 0) {
            throw std::runtime_error("validator is not in the active validator set (no bound, active stake)");
        }
        constexpr int64_t MAX_PRODUCTION_ROUNDS{100'000};
        bool eligible{false};
        for (; pos_round < MAX_PRODUCTION_ROUNDS; ++pos_round) {
            const uint256 digest{modern::ModernPosEligibilityDigest(
                *domain, seed, static_cast<uint32_t>(nHeight), static_cast<uint32_t>(pos_round),
                *m_options.modern_pos_validator_key)};
            if (modern::ModernPosEligible(digest, w, W, pos_round, *b3_consensus.modern_pos)) {
                eligible = true;
                break;
            }
        }
        if (!eligible) throw std::runtime_error("no eligible modern-PoS round found");
    }

    // The bridge freshness rule commits to the candidate block time. Modern
    // PoS time is already known from the selected recovery round, so expose
    // that exact value during chunk selection rather than a wall-clock value
    // that would be replaced after selection.
    pblock->nTime = b3_modern_pos
                        ? static_cast<uint32_t>(modern::ModernPosBlockTime(
                              pindexPrev->GetBlockTime(), pos_round,
                              *b3_consensus.modern_pos))
                        : TicksSinceEpoch<std::chrono::seconds>(NodeClock::now());
    if (!b3_modern_pos && b3_bridge_active) {
        // Freeze the exact transition-PoW time before bridge selection. A
        // later wall-clock refresh could otherwise make a mint cross the
        // light-client freshness boundary after it had been admitted.
        UpdateTime(pblock, b3_consensus, pindexPrev);
        if (b3_corridor) {
            pblock->nTime = static_cast<uint32_t>(std::max<int64_t>(
                pblock->nTime,
                pindexPrev->GetBlockTime() +
                    b3_consensus.transition_pow_min_spacing));
        }
    }
    m_lock_time_cutoff = pindexPrev->GetMedianTimePast();

    std::unique_ptr<BridgeBlockPreview> bridge_preview;
    if (m_mempool && b3_bridge_active) {
        uint256 preview_block_id;
        preview_block_id.begin()[0] = 1; // scratch identity; never persisted
        std::string error;
        bridge_preview = m_chainstate.ModernBridgeState()
                             .Index()
                             .BeginBlockPreview(
                                 nHeight, pblock->GetBlockTime(),
                                 preview_block_id, b3_consensus, error);
        if (!bridge_preview) {
            throw std::runtime_error(
                "bridge preview is unavailable for block assembly: " +
                error);
        }
    }

    // A binding can be valid by itself against the confirmed tip yet conflict
    // with another unconfirmed binding selected earlier for this block (same
    // validator sequence or reused BLS key). Keep the exact consensus state
    // machine as a candidate-local preview so one toxic mempool entry cannot
    // make every block template fail final validation.
    std::optional<FinalityBindingOverlay> finality_binding_preview;
    if (m_mempool && b3_modern) {
        const auto domain{modern::ModernChainDomain(
            b3_consensus.hashGenesisBlock,
            b3_consensus.legacy_final_hash.value_or(uint256{}))};
        if (!domain) {
            throw std::runtime_error(
                "modern chain domain is unavailable for finality-key block assembly");
        }
        node::FinalityBindingTracker& bindings{
            m_chainstate.ModernFinalityBindings()};
        if (!bindings.Sync(m_chainstate.m_chain, m_chainstate.m_blockman,
                           b3_consensus, *pindexPrev)) {
            throw std::runtime_error(
                "finality-key binding index is unavailable for block assembly");
        }
        finality_binding_preview.emplace(bindings.Index(), nHeight, *domain);
    }

    if (m_mempool) {
        LOCK(m_mempool->cs);
        m_mempool->StartBlockBuilding();
        addChunks(bridge_preview.get(), bridge_burn_readiness,
                  finality_binding_preview ? &*finality_binding_preview : nullptr);
        m_mempool->StopBlockBuilding();
    }

    const auto time_1{SteadyClock::now()};

    m_last_block_num_txs = nBlockTx;
    m_last_block_weight = nBlockWeight;

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;

    // Construct coinbase transaction struct in parallel
    CoinbaseTx& coinbase_tx{pblocktemplate->m_coinbase_tx};
    coinbase_tx.version = coinbaseTx.version;

    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vin[0].nSequence = CTxIn::MAX_SEQUENCE_NONFINAL; // Make sure timelock is enforced.
    coinbase_tx.sequence = coinbaseTx.vin[0].nSequence;

    // Add an output that spends the full coinbase reward.
    coinbaseTx.vout.resize(1);
    coinbaseTx.vout[0].scriptPubKey = m_options.coinbase_output_script;
    // Block subsidy + fees. Corridor blocks claim fees plus the configured
    // corridor reward; modern-PoS blocks claim fees plus the configured
    // sealed-supply-derived modern reward, matching the unconditional
    // consensus cap. Only non-B3 chains use the stock subsidy schedule.
    CAmount modern_subsidy{0};
    CAmount treasury_share{0};
    if (b3_modern_pos && b3_consensus.modern_pos) {
        const auto m_height{Consensus::ModernPosStartHeight(b3_consensus)};
        modern_subsidy = Consensus::ModernBlockSubsidy(nHeight, m_height.value_or(nHeight),
                                                       *b3_consensus.modern_pos);
        treasury_share = Consensus::ModernTreasuryShare(modern_subsidy, *b3_consensus.modern_pos);
    }
    const CAmount block_reward{b3_corridor  ? nFees + *b3_consensus.transition_pow_reward
                               : b3_modern_pos ? nFees + modern_subsidy
                                               : nFees + GetBlockSubsidy(nHeight, chainparams.GetConsensus())};
    coinbaseTx.vout[0].nValue = block_reward - treasury_share;
    if (treasury_share > 0) {
        // OD-2 treasury split: the ruled share of the SUBSIDY pays the
        // pinned treasury script; the producer keeps the rest plus fees.
        const CTxOut treasury_output{
            treasury_share, CScript{b3_consensus.modern_pos->treasury_script.begin(),
                                    b3_consensus.modern_pos->treasury_script.end()}};
        coinbaseTx.vout.push_back(treasury_output);
        coinbase_tx.required_outputs.push_back(treasury_output);
    }
    if (!fn_genesis_outputs.empty()) {
        coinbaseTx.vout.insert(coinbaseTx.vout.end(), fn_genesis_outputs.begin(),
                               fn_genesis_outputs.end());
        coinbase_tx.required_outputs.insert(coinbase_tx.required_outputs.end(),
                                            fn_genesis_outputs.begin(),
                                            fn_genesis_outputs.end());
    }
    coinbase_tx.block_reward_remaining = block_reward - treasury_share;

    // Start the coinbase scriptSig with the block height as required by BIP34.
    // Mining clients are expected to append extra data to this prefix, so
    // increasing its length would reduce the space they can use and may break
    // existing clients.
    coinbaseTx.vin[0].scriptSig = CScript() << nHeight;
    if (b3_modern_pos) {
        // The validator declaration (V1 spec §5): a direct 32-byte push of
        // the x-only key immediately after the BIP34 height push, merkle-
        // committed through the coinbase.
        coinbaseTx.vin[0].scriptSig
            << std::vector<unsigned char>(m_options.modern_pos_validator_key->begin(),
                                          m_options.modern_pos_validator_key->end());
    }
    if (m_options.include_dummy_extranonce) {
        // For blocks at heights <= 16, the BIP34-encoded height alone is only
        // one byte. Consensus requires coinbase scriptSigs to be at least two
        // bytes long (bad-cb-length), so tests and regtest include a dummy
        // extraNonce (OP_0)
        coinbaseTx.vin[0].scriptSig << OP_0;
    }
    coinbase_tx.script_sig_prefix = coinbaseTx.vin[0].scriptSig;
    Assert(nHeight > 0);
    coinbaseTx.nLockTime = static_cast<uint32_t>(nHeight - 1);
    coinbase_tx.lock_time = coinbaseTx.nLockTime;

    // FINALITY_CERT inclusion (plan Commit 16): when the local signature pool
    // holds a quorum certificate for a checkpoint above the finalized height,
    // include it in the coinbase -- cell + type-4 record, judged first with
    // the IDENTICAL consensus rule so no invalid certificate is ever emitted.
    // Without a quorum nothing is included: blocks never depend on
    // certificates and the epoch simply extends (frozen behaviour).
    if (b3_modern_pos) {
        node::FinalityTracker& finality{m_chainstate.ModernFinality()};
        const node::BridgeStateIndex* bridge_index{nullptr};
        if (Consensus::BridgeRulesActive(pindexPrev->nHeight, b3_consensus)) {
            node::BridgeStateTracker& bridge{
                m_chainstate.ModernBridgeState()};
            if (bridge.Sync(m_chainstate.m_chain, m_chainstate.m_blockman,
                            b3_consensus, *pindexPrev)) {
                bridge_index = &bridge.Index();
            }
        }
        if (finality.Sync(m_chainstate.m_chain, m_chainstate.m_blockman,
                          b3_consensus, *pindexPrev, bridge_index)) {
            if (auto best{m_chainstate.FinalitySignatures().BestCertificate(finality, m_chainstate.m_chain,
                                                                            b3_consensus, bridge_index)}) {
                std::string cert_error;
                if (finality.JudgeCandidateCertificate(best->first, best->second, *pindexPrev, b3_consensus,
                                                       cert_error, bridge_index)) {
                    const auto [payload, cell] = modern::BuildFinalityCertificate(best->first, best->second);
                    const CTxOut certificate_output{0, cell};
                    coinbaseTx.vout.push_back(certificate_output);
                    coinbase_tx.required_outputs.push_back(certificate_output);
                    CMpaRecord record;
                    record.payload_type = modern::MPA_TYPE_FINALITY_CERTIFICATE;
                    record.payload_version = modern::MPA_VERSION_V1;
                    record.payload = payload;
                    coinbaseTx.mpa = {record};
                    LogInfo("CreateNewBlock(): including finality certificate for checkpoint %d (epoch %d)\n",
                            best->first.height, best->first.epoch);
                } else {
                    LogDebug(BCLog::VALIDATION, "CreateNewBlock(): pooled certificate not includable (%s)\n",
                             cert_error);
                }
            }
        }
    }

    // MODERN_PAYLOAD_ROOT (plan Commit 7): when the candidate block carries any
    // Modern Payload Area -- a transaction's or the coinbase's own (finality
    // certificate) -- the coinbase must commit to all sections with exactly one
    // root cell. The root depends only on the MPA sections and positions,
    // never on the coinbase outputs, so it can be computed before the cell is
    // appended.
    if (b3_modern) {
        bool any_mpa{!coinbaseTx.mpa.empty()};
        for (size_t i = 1; !any_mpa && i < pblock->vtx.size(); ++i) {
            if (pblock->vtx[i] && pblock->vtx[i]->HasMpa()) any_mpa = true;
        }
        if (any_mpa) {
            CBlock probe{*pblock};
            probe.vtx[0] = MakeTransactionRef(CTransaction{coinbaseTx});
            const CTxOut root_output{
                0, modern::MakePayloadRootCellScript(modern::ComputePayloadRoot(probe))};
            coinbaseTx.vout.push_back(root_output);
            coinbase_tx.required_outputs.push_back(root_output);
        }
    }
    if (!coinbaseTx.mpa.empty()) {
        DataStream section;
        SerializeMpaSection(section, coinbaseTx.mpa);
        coinbase_tx.mpa_section.assign(UCharCast(section.data()),
                                       UCharCast(section.data()) + section.size());
    }
    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));
    m_chainstate.m_chainman.GenerateCoinbaseCommitment(*pblock, pindexPrev);

    const CTransactionRef& final_coinbase{pblock->vtx[0]};
    if (final_coinbase->HasWitness()) {
        const auto& witness_stack{final_coinbase->vin[0].scriptWitness.stack};
        // Consensus requires the coinbase witness stack to have exactly one
        // element of 32 bytes.
        Assert(witness_stack.size() == 1 && witness_stack[0].size() == 32);
        coinbase_tx.witness = uint256(witness_stack[0]);
    }
    if (const int witness_index = GetWitnessCommitmentIndex(*pblock); witness_index != NO_WITNESS_COMMITMENT) {
        Assert(witness_index >= 0 && static_cast<size_t>(witness_index) < final_coinbase->vout.size());
        coinbase_tx.required_outputs.push_back(final_coinbase->vout[witness_index]);
    }

    LogInfo("CreateNewBlock(): block weight: %u txs: %u fees: %ld sigops %d\n", GetBlockWeight(*pblock), nBlockTx, nFees, nBlockSigOpsCost);

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    if (b3_modern_pos) {
        // Deterministic PoS header: the exact round timestamp, the enforced
        // sentinel bits, nNonce 0, and a zero placeholder signature so the
        // template's own validity check (which skips signature cryptography)
        // passes the contextual size rule; the caller re-signs after the
        // merkle root is final.
        pblock->nTime = static_cast<uint32_t>(
            modern::ModernPosBlockTime(pindexPrev->GetBlockTime(), pos_round, *b3_consensus.modern_pos));
        pblock->nBits = b3_consensus.modern_pos->sentinel_bits;
        pblock->vchBlockSig.assign(modern::MODERN_POS_SIG_SIZE, 0x00);
    } else {
        // Bridge-active block assembly already froze the exact preview time
        // above. Preserve it through final validation.
        if (!b3_bridge_active) {
            UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
        }
        if (b3_corridor) {
            // Corridor pacing: never earlier than parent + minimum spacing
            // (the network refuses it); the future bound then paces
            // acceptance in real time.
            pblock->nTime = static_cast<uint32_t>(std::max<int64_t>(
                pblock->nTime, pindexPrev->GetBlockTime() + b3_consensus.transition_pow_min_spacing));
        }
        pblock->nBits = b3_corridor ? *b3_consensus.transition_pow_bits
                                    : GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
    }
    pblock->nNonce         = 0;

    if (m_options.test_block_validity) {
        // if nHeight <= 16, and include_dummy_extranonce=false this will fail due to bad-cb-length.
        if (BlockValidationState state{TestBlockValidity(m_chainstate, *pblock, /*check_pow=*/false, /*check_merkle_root=*/false)}; !state.IsValid()) {
            throw std::runtime_error(strprintf("TestBlockValidity failed: %s", state.ToString()));
        }
    }
    const auto time_2{SteadyClock::now()};

    LogDebug(BCLog::BENCH, "CreateNewBlock() chunks: %.2fms, validity: %.2fms (total %.2fms)\n",
             Ticks<MillisecondsDouble>(time_1 - time_start),
             Ticks<MillisecondsDouble>(time_2 - time_1),
             Ticks<MillisecondsDouble>(time_2 - time_start));

    return std::move(pblocktemplate);
}

bool BlockAssembler::TestChunkBlockLimits(FeePerWeight chunk_feerate, int64_t chunk_sigops_cost) const
{
    if (nBlockWeight + chunk_feerate.size >= m_options.nBlockMaxWeight) {
        return false;
    }
    if (nBlockSigOpsCost + chunk_sigops_cost >= MAX_BLOCK_SIGOPS_COST) {
        return false;
    }
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
bool BlockAssembler::TestChunkTransactions(
    const std::vector<CTxMemPoolEntryRef>& txs,
    std::optional<uint32_t>& resulting_fn_pod_total,
    const FinalityBindingOverlay* const finality_binding_preview,
    std::optional<FinalityBindingOverlay>& resulting_finality_binding_preview) const
{
    resulting_fn_pod_total = m_fn_pod_issued_total;
    resulting_finality_binding_preview.reset();
    const bool witness_active{DeploymentActiveAfter(
        m_chainstate.m_chain.Tip(), m_chainstate.m_chainman,
        Consensus::DEPLOYMENT_SEGWIT)};
    for (const auto tx : txs) {
        if (!IsFinalTx(tx.get().GetTx(), nHeight, m_lock_time_cutoff)) {
            return false;
        }
        const CTransaction& transaction{tx.get().GetTx()};
        // The mempool rejects these while witness commitments are inactive,
        // but retain a production-side guard for entries restored by an older
        // binary or injected through test/debug code. One stale entry must not
        // poison every block template.
        if (!witness_active && transaction.HasWitness()) {
            return false;
        }
        const bool has_finality_key_evidence{std::any_of(
            transaction.mpa.begin(), transaction.mpa.end(),
            [](const CMpaRecord& record) {
                return record.payload_type ==
                       modern::MPA_TYPE_FINALITY_KEY_EVIDENCE;
            })};
        if (finality_binding_preview != nullptr && has_finality_key_evidence) {
            if (!resulting_finality_binding_preview) {
                resulting_finality_binding_preview.emplace(
                    *finality_binding_preview);
            }
            std::vector<FinalityBindingIndex::Transition> transitions;
            std::string error;
            if (!resulting_finality_binding_preview->ApplyTransaction(
                    transaction, transitions, error)) {
                LogDebug(BCLog::VALIDATION,
                         "CreateNewBlock(): skipping FINALITY_KEY conflict in %s (%s)\n",
                         transaction.GetHash().ToString(), error);
                return false;
            }
        }
        const size_t declarations{modern::CountModernFnPodDeclarations(transaction)};
        if (declarations == 0) continue;
        if (declarations != 1 || !resulting_fn_pod_total) return false;

        modern::ModernFnPodActionV1 action;
        bool decoded{false};
        for (const CMpaRecord& record : transaction.mpa) {
            if (record.payload_type != modern::CREATION_ACTION_MODERN_FN_POD) continue;
            std::string error;
            if (!modern::DecodeModernFnPodRecord(record, action, error)) return false;
            decoded = true;
        }
        const auto capacity{modern::ModernFnCapacity(chainparams.GetConsensus())};
        if (!decoded || !capacity || action.created_before != *resulting_fn_pod_total ||
            *resulting_fn_pod_total >= *capacity) {
            return false;
        }
        ++*resulting_fn_pod_total;
    }
    if (Consensus::FlowMeshRulesActive(nHeight,
                                       chainparams.GetConsensus())) {
        CBlock candidate;
        // The block template still has a null coinbase placeholder during
        // selection. Only ordinary transactions can carry type 8/9 here.
        for (size_t i{1}; i < pblocktemplate->block.vtx.size(); ++i) {
            if (pblocktemplate->block.vtx[i]) {
                candidate.vtx.push_back(pblocktemplate->block.vtx[i]);
            }
        }
        for (const auto& entry : txs) {
            candidate.vtx.push_back(entry.get().GetSharedTx());
        }
        uint256 candidate_id;
        candidate_id.begin()[0] = 1; // scratch identity; never persisted
        node::FlowMeshCheckpointBlockDelta delta;
        std::string error;
        const node::FnSeatTracker& seats{m_chainstate.ModernFnSeats()};
        const node::FlowMeshCheckpointTracker& checkpoints{
            m_chainstate.ModernFlowMeshCheckpoints()};
        const node::FlowMeshVaultTracker& vaults{
            m_chainstate.ModernFlowMeshVaults()};
        if (!checkpoints.Index().VerifyBlock(
                candidate, nHeight, candidate_id, m_chainstate.m_chain,
                chainparams.GetConsensus(), seats.Index(), vaults.Index(),
                delta, error)) {
            return false;
        }
    }
    return true;
}

void BlockAssembler::AddToBlock(const CTxMemPoolEntry& entry)
{
    pblocktemplate->block.vtx.emplace_back(entry.GetSharedTx());
    pblocktemplate->vTxFees.push_back(entry.GetFee());
    pblocktemplate->vTxSigOpsCost.push_back(entry.GetSigOpCost());
    nBlockWeight += entry.GetTxWeight();
    ++nBlockTx;
    nBlockSigOpsCost += entry.GetSigOpCost();
    nFees += entry.GetFee();

    if (m_options.print_modified_fee) {
        LogInfo("fee rate %s txid %s\n",
                  CFeeRate(entry.GetModifiedFee(), entry.GetTxSize()).ToString(),
                  entry.GetTx().GetHash().ToString());
    }
}

void BlockAssembler::addChunks(
    BridgeBlockPreview* const bridge_preview,
    const BridgeBurnReadiness bridge_burn_readiness,
    FinalityBindingOverlay* const finality_binding_preview)
{
    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    constexpr int32_t BLOCK_FULL_ENOUGH_WEIGHT_DELTA = 4000;
    int64_t nConsecutiveFailed = 0;

    std::vector<CTxMemPoolEntry::CTxMemPoolEntryRef> selected_transactions;
    selected_transactions.reserve(MAX_CLUSTER_COUNT_LIMIT);
    FeePerWeight chunk_feerate;

    // This fills selected_transactions
    chunk_feerate = m_mempool->GetBlockBuilderChunk(selected_transactions);
    FeePerVSize chunk_feerate_vsize = ToFeePerVSize(chunk_feerate);

    while (selected_transactions.size() > 0) {
        // Check to see if min fee rate is still respected.
        if (chunk_feerate_vsize << m_options.blockMinFeeRate.GetFeePerVSize()) {
            // Everything else we might consider has a lower feerate
            return;
        }

        int64_t chunk_sig_ops = 0;
        for (const auto& tx : selected_transactions) {
            chunk_sig_ops += tx.get().GetSigOpCost();
        }

        // Check to see if this chunk will fit. Bridge preview is deliberately
        // last: a successful atomic append is followed immediately by
        // inclusion, while a rejected chunk leaves the accepted preview
        // prefix unchanged.
        std::optional<uint32_t> resulting_fn_pod_total;
        std::optional<FinalityBindingOverlay> resulting_finality_binding_preview;
        bool chunk_accepted{
            TestChunkBlockLimits(chunk_feerate, chunk_sig_ops) &&
            TestChunkTransactions(selected_transactions,
                                  resulting_fn_pod_total,
                                  finality_binding_preview,
                                  resulting_finality_binding_preview)};
        if (chunk_accepted && bridge_preview != nullptr) {
            std::vector<CTransactionRef> bridge_transactions;
            bridge_transactions.reserve(selected_transactions.size());
            for (const auto& entry : selected_transactions) {
                bridge_transactions.push_back(entry.get().GetSharedTx());
            }
            std::string error;
            chunk_accepted = std::all_of(
                bridge_transactions.begin(), bridge_transactions.end(),
                [&](const CTransactionRef& tx) {
                    return CheckBridgeBurnReadiness(
                        *tx, bridge_burn_readiness, error);
                });
            if (chunk_accepted) {
                chunk_accepted = bridge_preview->TryAppend(
                    bridge_transactions, error);
            }
        }
        if (!chunk_accepted) {
            // This chunk won't fit, so we skip it and will try the next best one.
            m_mempool->SkipBuilderChunk();
            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockWeight +
                    BLOCK_FULL_ENOUGH_WEIGHT_DELTA > m_options.nBlockMaxWeight) {
                // Give up if we're close to full and haven't succeeded in a while
                return;
            }
        } else {
            m_mempool->IncludeBuilderChunk();
            m_fn_pod_issued_total = resulting_fn_pod_total;
            if (finality_binding_preview != nullptr &&
                resulting_finality_binding_preview) {
                *finality_binding_preview =
                    std::move(*resulting_finality_binding_preview);
            }

            // This chunk will fit, so add it to the block.
            nConsecutiveFailed = 0;
            for (const auto& tx : selected_transactions) {
                AddToBlock(tx);
            }
            pblocktemplate->m_package_feerates.emplace_back(chunk_feerate_vsize);
        }

        selected_transactions.clear();
        chunk_feerate = m_mempool->GetBlockBuilderChunk(selected_transactions);
        chunk_feerate_vsize = ToFeePerVSize(chunk_feerate);
    }
}

void AddMerkleRootAndCoinbase(CBlock& block, CTransactionRef coinbase, uint32_t version, uint32_t timestamp, uint32_t nonce)
{
    if (block.vtx.size() == 0) {
        block.vtx.emplace_back(coinbase);
    } else {
        block.vtx[0] = coinbase;
    }
    block.nVersion = version;
    block.nTime = timestamp;
    block.nNonce = nonce;
    block.hashMerkleRoot = BlockMerkleRoot(block);

    // Reset cached checks
    block.m_checked_witness_commitment = false;
    block.m_checked_merkle_root = false;
    block.fChecked = false;
}

void InterruptWait(KernelNotifications& kernel_notifications, bool& interrupt_wait)
{
    LOCK(kernel_notifications.m_tip_block_mutex);
    interrupt_wait = true;
    kernel_notifications.m_tip_block_cv.notify_all();
}

std::unique_ptr<CBlockTemplate> WaitAndCreateNewBlock(ChainstateManager& chainman,
                                                      KernelNotifications& kernel_notifications,
                                                      CTxMemPool* mempool,
                                                      const std::unique_ptr<CBlockTemplate>& block_template,
                                                      const BlockWaitOptions& options,
                                                      const BlockAssembler::Options& assemble_options,
                                                      bool& interrupt_wait)
{
    // Delay calculating the current template fees, just in case a new block
    // comes in before the next tick.
    CAmount current_fees = -1;

    // Alternate waiting for a new tip and checking if fees have risen.
    // The latter check is expensive so we only run it once per second.
    auto now{NodeClock::now()};
    const auto deadline = now + options.timeout;
    const MillisecondsDouble tick{1000};
    const bool allow_min_difficulty{chainman.GetParams().GetConsensus().fPowAllowMinDifficultyBlocks};

    do {
        bool tip_changed{false};
        {
            WAIT_LOCK(kernel_notifications.m_tip_block_mutex, lock);
            // Note that wait_until() checks the predicate before waiting
            kernel_notifications.m_tip_block_cv.wait_until(lock, std::min(now + tick, deadline), [&]() EXCLUSIVE_LOCKS_REQUIRED(kernel_notifications.m_tip_block_mutex) {
                AssertLockHeld(kernel_notifications.m_tip_block_mutex);
                const auto tip_block{kernel_notifications.TipBlock()};
                // We assume tip_block is set, because this is an instance
                // method on BlockTemplate and no template could have been
                // generated before a tip exists.
                tip_changed = Assume(tip_block) && tip_block != block_template->block.hashPrevBlock;
                return tip_changed || chainman.m_interrupt || interrupt_wait;
            });
            if (interrupt_wait) {
                interrupt_wait = false;
                return nullptr;
            }
        }

        if (chainman.m_interrupt) return nullptr;
        // At this point the tip changed, a full tick went by or we reached
        // the deadline.

        // Must release m_tip_block_mutex before locking cs_main, to avoid deadlocks.
        LOCK(::cs_main);

        // On test networks return a minimum difficulty block after 20 minutes
        if (!tip_changed && allow_min_difficulty) {
            const NodeClock::time_point tip_time{std::chrono::seconds{chainman.ActiveChain().Tip()->GetBlockTime()}};
            if (now > tip_time + 20min) {
                tip_changed = true;
            }
        }

        /**
         * We determine if fees increased compared to the previous template by generating
         * a fresh template. There may be more efficient ways to determine how much
         * (approximate) fees for the next block increased, perhaps more so after
         * Cluster Mempool.
         *
         * We'll also create a new template if the tip changed during this iteration.
         */
        if (options.fee_threshold < MAX_MONEY || tip_changed) {
            auto new_tmpl{BlockAssembler{
                chainman.ActiveChainstate(),
                mempool,
                assemble_options}
                              .CreateNewBlock()};

            // If the tip changed, return the new template regardless of its fees.
            if (tip_changed) return new_tmpl;

            // Calculate the original template total fees if we haven't already
            if (current_fees == -1) {
                current_fees = std::accumulate(block_template->vTxFees.begin(), block_template->vTxFees.end(), CAmount{0});
            }

            // Check if fees increased enough to return the new template
            const CAmount new_fees = std::accumulate(new_tmpl->vTxFees.begin(), new_tmpl->vTxFees.end(), CAmount{0});
            Assume(options.fee_threshold != MAX_MONEY);
            if (new_fees >= current_fees + options.fee_threshold) return new_tmpl;
        }

        now = NodeClock::now();
    } while (now < deadline);

    return nullptr;
}

std::optional<BlockRef> GetTip(ChainstateManager& chainman)
{
    LOCK(::cs_main);
    CBlockIndex* tip{chainman.ActiveChain().Tip()};
    if (!tip) return {};
    return BlockRef{tip->GetBlockHash(), tip->nHeight};
}

bool CooldownIfHeadersAhead(ChainstateManager& chainman, KernelNotifications& kernel_notifications, const BlockRef& last_tip, bool& interrupt_mining)
{
    uint256 last_tip_hash{last_tip.hash};

    while (const std::optional<int> remaining = chainman.BlocksAheadOfTip()) {
        const int cooldown_seconds = std::clamp(*remaining, 3, 20);
        const auto cooldown_deadline{MockableSteadyClock::now() + std::chrono::seconds{cooldown_seconds}};

        {
            WAIT_LOCK(kernel_notifications.m_tip_block_mutex, lock);
            kernel_notifications.m_tip_block_cv.wait_until(lock, cooldown_deadline, [&]() EXCLUSIVE_LOCKS_REQUIRED(kernel_notifications.m_tip_block_mutex) {
                const auto tip_block = kernel_notifications.TipBlock();
                return chainman.m_interrupt || interrupt_mining || (tip_block && *tip_block != last_tip_hash);
            });
            if (chainman.m_interrupt || interrupt_mining) {
                interrupt_mining = false;
                return false;
            }

            // If the tip changed during the wait, extend the deadline
            const auto tip_block = kernel_notifications.TipBlock();
            if (tip_block && *tip_block != last_tip_hash) {
                last_tip_hash = *tip_block;
                continue;
            }
        }

        // No tip change and the cooldown window has expired.
        if (MockableSteadyClock::now() >= cooldown_deadline) break;
    }

    return true;
}

std::optional<BlockRef> WaitTipChanged(ChainstateManager& chainman, KernelNotifications& kernel_notifications, const uint256& current_tip, MillisecondsDouble& timeout, bool& interrupt)
{
    Assume(timeout >= 0ms); // No internal callers should use a negative timeout
    if (timeout < 0ms) timeout = 0ms;
    if (timeout > std::chrono::years{100}) timeout = std::chrono::years{100}; // Upper bound to avoid UB in std::chrono
    auto deadline{std::chrono::steady_clock::now() + timeout};
    {
        WAIT_LOCK(kernel_notifications.m_tip_block_mutex, lock);
        // For callers convenience, wait longer than the provided timeout
        // during startup for the tip to be non-null. That way this function
        // always returns valid tip information when possible and only
        // returns null when shutting down, not when timing out.
        kernel_notifications.m_tip_block_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(kernel_notifications.m_tip_block_mutex) {
            return kernel_notifications.TipBlock() || chainman.m_interrupt || interrupt;
        });
        if (chainman.m_interrupt || interrupt) {
            interrupt = false;
            return {};
        }
        // At this point TipBlock is set, so continue to wait until it is
        // different then `current_tip` provided by caller.
        kernel_notifications.m_tip_block_cv.wait_until(lock, deadline, [&]() EXCLUSIVE_LOCKS_REQUIRED(kernel_notifications.m_tip_block_mutex) {
            return Assume(kernel_notifications.TipBlock()) != current_tip || chainman.m_interrupt || interrupt;
        });
        if (chainman.m_interrupt || interrupt) {
            interrupt = false;
            return {};
        }
    }

    // Must release m_tip_block_mutex before getTip() locks cs_main, to
    // avoid deadlocks.
    return GetTip(chainman);
}

bool BlockAssembler::SignModernPosBlock(CBlock& block, const CKey& validator_key,
                                        const Consensus::Params& params)
{
    const auto domain{modern::ModernChainDomain(params.hashGenesisBlock,
                                                params.legacy_final_hash.value_or(uint256{}))};
    if (!domain || !validator_key.IsValid()) return false;
    const uint256 sig_hash{modern::ModernPosSignatureHash(*domain, block.GetHash())};
    block.vchBlockSig.assign(modern::MODERN_POS_SIG_SIZE, 0x00);
    return validator_key.SignSchnorr(sig_hash, block.vchBlockSig, /*merkle_root=*/nullptr, uint256{});
}

} // namespace node
