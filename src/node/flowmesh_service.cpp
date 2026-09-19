// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_service.h>

#include <chain.h>
#include <consensus/era.h>
#include <flowmesh/fee_allocation.h>
#include <hash.h>
#include <logging.h>
#include <modern/asset_output.h>
#include <modern/chain_domain.h>
#include <modern/mpa.h>
#include <net_processing.h>
#include <node/flowmesh_anchor.h>
#include <node/flowmesh_checkpoint_index.h>
#include <node/flowmesh_production_store.h>
#include <node/flowmesh_vault_index.h>
#include <node/fn_seat_index.h>
#include <script/script.h>
#include <sync.h>
#include <univalue.h>
#include <util/int128.h>
#include <util/thread.h>
#include <validation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <utility>

namespace node {

std::optional<flowmesh::ActiveFnBlsSeatSet> ResolveFlowMeshClientSeats(
    ChainstateManager& chainman, const flowmesh::ClientEvidencePins& pins,
    const flowmesh::ProductionEntryCore& entry, std::string& error)
{
    if (!flowmesh::CheckClientEvidencePins(pins) || entry.domain != pins.domain ||
        entry.market_id != pins.market_id) {
        error = "FlowMesh client market/domain pins do not match the entry";
        return std::nullopt;
    }
    std::vector<flowmesh::BlsSeatBinding> bindings;
    flowmesh::AnchorRef seat_anchor;
    {
        LOCK(::cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        const CBlockIndex* tip{chainstate.m_chain.Tip()};
        const auto& params{chainman.GetConsensus()};
        const auto domain{params.legacy_final_hash
            ? modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)
            : std::nullopt};
        if (!tip || !domain || *domain != pins.domain ||
            !Consensus::FlowMeshRulesActive(tip->nHeight, params)) {
            error = "FlowMesh client B3 chain/domain is unavailable";
            return std::nullopt;
        }
        auto& seats{chainstate.ModernFnSeats()};
        auto& vaults{chainstate.ModernFlowMeshVaults()};
        auto& checkpoints{chainstate.ModernFlowMeshCheckpoints()};
        if (!seats.Sync(chainstate.m_chain, chainstate.m_blockman, params, *tip) ||
            !vaults.Sync(chainstate.m_chain, chainstate.m_blockman, params, *tip) ||
            !checkpoints.Sync(chainstate.m_chain, chainstate.m_blockman, params,
                              seats.Index(), vaults.Index(), *tip)) {
            error = "FlowMesh client mandatory B3 indexes are unavailable";
            return std::nullopt;
        }
        const CBlockIndex* anchor{entry.anchor.height >= 0 ? chainstate.m_chain[entry.anchor.height] : nullptr};
        if (!anchor || anchor->GetBlockHash() != entry.anchor.hash ||
            tip->nHeight - entry.anchor.height < Consensus::FLOWMESH_ANCHOR_DEPTH) {
            error = "FlowMesh client production anchor is not canonical and deep";
            return std::nullopt;
        }
        const auto market{vaults.Index().MarketAt(pins.market_id, *anchor)};
        if (!market || market->base_asset != pins.base_asset || market->vault_id != pins.vault_id) {
            error = "FlowMesh client market is not established at its anchor";
            return std::nullopt;
        }
        auto authority{checkpoints.Index().Head(pins.market_id)};
        // Historical action evidence may use an earlier connected epoch.
        // Bound this metadata walk; it never downloads/replays microblocks.
        size_t traversed{0};
        while (authority && authority->core.sequence > entry.sequence) {
            if (++traversed > 4096) {
                error = "FlowMesh client historical authority exceeds lookup bound";
                return std::nullopt;
            }
            authority = authority->core.previous_checkpoint_id.IsNull()
                ? std::nullopt : checkpoints.Index().Get(authority->core.previous_checkpoint_id);
        }
        uint64_t epoch{0};
        uint256 expected_set;
        if (authority) {
            if (authority->core.sequence == entry.sequence &&
                authority->core.microblock_hash != entry.GetHash()) {
                error = "FlowMesh client entry conflicts with the connected checkpoint";
                return std::nullopt;
            }
            if (authority->core.handoff && authority->core.sequence < entry.sequence) {
                if (!FlowMeshHandoffConnectionMature(
                        {authority->connected_height, authority->connected_block}, tip->nHeight)) {
                    error = "FlowMesh client incoming epoch handoff is not mature";
                    return std::nullopt;
                }
                epoch = authority->core.handoff->next_epoch;
                seat_anchor = {static_cast<int32_t>(authority->core.handoff->next_anchor.height),
                               authority->core.handoff->next_anchor.block_hash};
                expected_set = authority->core.handoff->next_seat_set_hash;
            } else {
                epoch = authority->core.epoch;
                seat_anchor = {static_cast<int32_t>(authority->core.anchor.height), authority->core.anchor.block_hash};
                expected_set = authority->core.seat_set_hash;
            }
        } else {
            const auto bootstrap{seats.Index().EarliestFlowMeshReadySnapshot(
                chainstate.m_chain, market->created_height, tip->nHeight, params, error)};
            if (!bootstrap) return std::nullopt;
            seat_anchor = {bootstrap->anchor_height, bootstrap->anchor_hash};
        }
        if (entry.epoch != epoch || (!expected_set.IsNull() && entry.seat_set_hash != expected_set)) {
            error = "FlowMesh client epoch has no matching connected B3 authority";
            return std::nullopt;
        }
        const CBlockIndex* seat_index{seat_anchor.height >= 0 ? chainstate.m_chain[seat_anchor.height] : nullptr};
        if (!seat_index || seat_index->GetBlockHash() != seat_anchor.hash) {
            error = "FlowMesh client seat anchor is no longer canonical";
            return std::nullopt;
        }
        const auto snapshot{seats.AnchoredSnapshot(chainstate.m_chain, *seat_index, tip->nHeight, params, error)};
        if (!snapshot || !snapshot->FlowMeshReady()) return std::nullopt;
        bindings.reserve(snapshot->members.size());
        for (const auto& member : snapshot->members) {
            bindings.push_back({member.outpoint, member.bls_pubkey, member.proof_of_possession});
        }
    }
    flowmesh::BlsSeatSetCheck check;
    auto out{flowmesh::BuildActiveFnBlsSeatSet(pins.domain, pins.market_id, entry.epoch,
        static_cast<uint64_t>(seat_anchor.height), seat_anchor.hash, bindings, check)};
    if (!out || out->set_hash != entry.seat_set_hash) {
        error = "FlowMesh client certificate names a different anchored seat set";
        return std::nullopt;
    }
    return out;
}

uint256 FlowMeshSeatKeysFingerprint(
    std::vector<std::array<unsigned char, bls::PUBKEY_SIZE>> public_keys)
{
    std::sort(public_keys.begin(), public_keys.end());
    public_keys.erase(std::unique(public_keys.begin(), public_keys.end()), public_keys.end());
    HashWriter writer{TaggedHash("B3/FlowMesh/ArmedSeatKeys/v1")};
    writer << public_keys;
    return writer.GetHash();
}

namespace {

constexpr size_t FLOWMESH_MARKET_DB_CACHE_BYTES{size_t{4} << 20};
constexpr std::chrono::milliseconds FLOWMESH_TICK_INTERVAL{250};

void ReconciliationTrace(const char* stage, uint64_t generation,
                         const uint256& tip = {})
{
    if (!LogAcceptCategory(BCLog::BENCH, BCLog::Level::Debug)) return;
    // Fixed-size metadata and a process-lifetime cap, including one explicit
    // terminal marker. The marker means later reconciliation spans are unknown.
    static std::atomic<uint64_t> trace_count{0};
    const auto count{trace_count.fetch_add(1, std::memory_order_relaxed)};
    if (count > 32768) return;
    UniValue event{UniValue::VOBJ};
    event.pushKV("monotonic_us", std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    event.pushKV("stage", count == 32768 ? "trace_limit_reached" : stage);
    event.pushKV("generation", generation);
    event.pushKV("tip", tip.GetHex());
    LogDebug(BCLog::BENCH, "FlowMeshServiceTrace %s\n", event.write());
}

bool SameMembers(const flowmesh::ActiveFnBlsSeatSet& a,
                 const flowmesh::ActiveFnBlsSeatSet& b)
{
    if (a.members.size() != b.members.size()) return false;
    for (size_t i{0}; i < a.members.size(); ++i) {
        if (a.members[i].seat_id != b.members[i].seat_id ||
            a.members[i].outpoint != b.members[i].outpoint ||
            a.members[i].key.Key().Compressed() !=
                b.members[i].key.Key().Compressed()) {
            return false;
        }
    }
    return true;
}

uint256 FlowMeshEffectId(const modern::FlowMeshEffectV1& effect)
{
    if (const auto* deposit{
            std::get_if<modern::FlowMeshDepositAcceptanceV1>(&effect)}) {
        return deposit->acceptance_id;
    }
    return std::get<modern::FlowMeshWithdrawalReceiptV1>(effect).receipt_id;
}

bool AppendLiveVaultInput(Chainstate& chainstate,
                          const FlowMeshVaultRecord& record,
                          std::vector<FlowMeshVaultInput>& inputs)
    EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    const Coin& coin{chainstate.CoinsTip().AccessCoin(record.outpoint)};
    if (coin.IsSpent()) return false;
    inputs.push_back({record, coin.out});
    return true;
}

} // namespace

struct FlowMeshService::Impl final : public FlowMeshRuntimeChain,
                                     public FlowMeshRuntimeKeyProvider,
                                     public ProductionSeatSetSource {
    struct MarketResources {
        FlowMeshServiceMarket metadata;
        FlowMeshMarketRecord chain_record;
        std::unique_ptr<ChainDepositVerifier> deposits;
        std::unique_ptr<FlowMeshProductionStore> store;
        bool installed{false};
        bool ready{false};
    };

    using SeatKey =
        std::tuple<flowmesh::MarketId, uint64_t, uint256>;

    explicit Impl(ChainstateManager& chainman_in, fs::path datadir_in,
                  FlowMeshServiceTransport transport_in,
                  std::set<flowmesh::MarketId> preagreement_markets_in)
        : chainman{chainman_in}, datadir{std::move(datadir_in)},
          transport{std::move(transport_in)},
          preagreement_markets{std::move(preagreement_markets_in)},
          anchors{chainman, FLOWMESH_ANCHOR_DEPTH}
    {
    }

    ChainstateManager& chainman;
    fs::path datadir;
    const FlowMeshServiceTransport transport;
    const std::set<flowmesh::MarketId> preagreement_markets;
    std::shared_ptr<FlowMeshNetService> network;
    ChainAnchorPolicy anchors;
    SteadyFlowMeshRuntimeClock clock;

    mutable std::mutex mutex;
    mutable std::mutex refresh_mutex;
    mutable std::mutex reconciled_tip_mutex;
    std::condition_variable ticker_cv;
    PeerManager* peerman{nullptr};
    std::shared_ptr<FlowMeshRuntime> runtime;
    std::map<flowmesh::MarketId, std::unique_ptr<MarketResources>> markets;
    mutable std::map<SeatKey, flowmesh::ActiveFnBlsSeatSet> seat_sets;
    std::vector<bls::SecretKey> local_keys;
    std::vector<std::array<unsigned char, bls::PUBKEY_SIZE>> local_public_keys;
    uint256 local_key_fingerprint{FlowMeshSeatKeysFingerprint({})};
    std::thread ticker;
    bool enabled{false};
    bool running{false};
    bool stopping{false};
    std::atomic<bool> chain_reconciling{false};
    std::atomic<uint64_t> delivery_generation{0};
    uint256 reconciled_tip;

    // Caller holds mutex; neither this snapshot nor the CAS token contains secrets.
    FlowMeshSeatKeyStatus SeatKeyStatusLocked() const
    {
        FlowMeshSeatKeyStatus out;
        out.enabled = enabled;
        out.running = running && !stopping;
        out.armed_pubkeys = local_public_keys;
        out.fingerprint = local_key_fingerprint;
        return out;
    }

    int32_t TipHeight() const override
    {
        LOCK(::cs_main);
        return chainman.ActiveChain().Height();
    }

    bool RulesActiveAtTip() const
    {
        LOCK(::cs_main);
        const CBlockIndex* tip{chainman.ActiveChain().Tip()};
        return tip != nullptr && Consensus::FlowMeshRulesActive(
                                     tip->nHeight, chainman.GetConsensus());
    }

    bool ReconciledTipMatches(const uint256& expected_tip) const
    {
        if (expected_tip.IsNull()) return false;
        {
            std::lock_guard<std::mutex> lock{reconciled_tip_mutex};
            if (reconciled_tip != expected_tip) return false;
        }
        LOCK(::cs_main);
        const CBlockIndex* tip{chainman.ActiveChain().Tip()};
        return tip != nullptr && tip->GetBlockHash() == expected_tip &&
               Consensus::FlowMeshRulesActive(tip->nHeight,
                                               chainman.GetConsensus());
    }

    bool ReconciledAtTip() const
    {
        if (chain_reconciling.load(std::memory_order_acquire)) return false;
        uint256 expected_tip;
        {
            std::lock_guard<std::mutex> lock{reconciled_tip_mutex};
            expected_tip = reconciled_tip;
        }
        return ReconciledTipMatches(expected_tip);
    }

    // Local admission epoch, not a chain or signing commitment. A critical
    // handler can detect reconciliation that both started and ended while
    // it was executing, even if the gate has reopened by its final check.
    uint64_t DeliveryGeneration() const override
    {
        return delivery_generation.load(std::memory_order_acquire);
    }

    void SetChainReconciling(bool value)
    {
        delivery_generation.fetch_add(1, std::memory_order_acq_rel);
        chain_reconciling.store(value, std::memory_order_release);
        delivery_generation.fetch_add(1, std::memory_order_release);
        ReconciliationTrace(value ? "gate_closed" : "gate_opened", DeliveryGeneration());
    }

    bool Acceptable(const flowmesh::AnchorRef& anchor) const override
    {
        if (chain_reconciling.load(std::memory_order_acquire) ||
            anchor.height < 0 || anchor.hash.IsNull()) {
            return false;
        }
        uint256 expected_tip;
        {
            std::lock_guard<std::mutex> lock{reconciled_tip_mutex};
            expected_tip = reconciled_tip;
        }
        if (expected_tip.IsNull()) return false;
        LOCK(::cs_main);
        const CChain& active{chainman.ActiveChain()};
        const CBlockIndex* tip{active.Tip()};
        if (tip == nullptr ||
            !Consensus::FlowMeshRulesActive(tip->nHeight,
                                            chainman.GetConsensus()) ||
            tip->GetBlockHash() != expected_tip) {
            return false;
        }
        const CBlockIndex* index{
            chainman.m_blockman.LookupBlockIndex(anchor.hash)};
        return index != nullptr && index->nHeight == anchor.height &&
               active.Contains(index) &&
               active.Height() - index->nHeight >= FLOWMESH_ANCHOR_DEPTH;
    }

    bool StillCanonical(const flowmesh::AnchorRef& anchor) const override
    {
        return anchors.StillCanonical(anchor);
    }

    flowmesh::AnchorRef Current() const override { return anchors.Current(); }

    void RememberSeatSet(const flowmesh::ActiveFnBlsSeatSet& seats) const
    {
        std::lock_guard<std::mutex> lock{mutex};
        seat_sets.insert_or_assign(
            SeatKey{seats.market_id, seats.epoch, seats.set_hash}, seats);
    }

    std::optional<flowmesh::ActiveFnBlsSeatSet> KnownSeatSet(
        const flowmesh::MarketId& market_id, const uint64_t epoch,
        const uint256& set_hash) const
    {
        std::lock_guard<std::mutex> lock{mutex};
        const auto it{seat_sets.find(SeatKey{market_id, epoch, set_hash})};
        return it == seat_sets.end()
                   ? std::nullopt
                   : std::optional<flowmesh::ActiveFnBlsSeatSet>{it->second};
    }

    bool SyncIndexesLocked(Chainstate& chainstate,
                           const CBlockIndex& tip,
                           std::string& error) const
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        const Consensus::Params& params{chainman.GetConsensus()};
        FnSeatTracker& seats{chainstate.ModernFnSeats()};
        if (!seats.Sync(chainstate.m_chain, chainstate.m_blockman, params,
                        tip)) {
            error = "FlowMesh FN-seat history is unavailable";
            return false;
        }
        FlowMeshVaultTracker& vaults{chainstate.ModernFlowMeshVaults()};
        if (!vaults.Sync(chainstate.m_chain, chainstate.m_blockman, params,
                         tip)) {
            error = "FlowMesh vault history is unavailable";
            return false;
        }
        FlowMeshCheckpointTracker& checkpoints{
            chainstate.ModernFlowMeshCheckpoints()};
        if (!checkpoints.Sync(chainstate.m_chain, chainstate.m_blockman,
                              params, seats.Index(), vaults.Index(), tip)) {
            error = "FlowMesh checkpoint history is unavailable";
            return false;
        }
        return true;
    }

    std::optional<flowmesh::ActiveFnBlsSeatSet> BuildSeatSetAt(
        const flowmesh::MarketId& market_id, const uint64_t epoch,
        const flowmesh::AnchorRef& anchor, std::string& error) const
    {
        if (anchor.height < 0 || anchor.hash.IsNull()) {
            error = "FlowMesh seat anchor is null";
            return std::nullopt;
        }
        std::optional<flowmesh::ActiveFnBlsSeatSet> out;
        {
            LOCK(::cs_main);
            Chainstate& chainstate{chainman.ActiveChainstate()};
            const CBlockIndex* tip{chainstate.m_chain.Tip()};
            const CBlockIndex* anchor_index{
                chainstate.m_blockman.LookupBlockIndex(anchor.hash)};
            if (tip == nullptr || anchor_index == nullptr ||
                anchor_index->nHeight != anchor.height ||
                chainstate.m_chain[anchor.height] != anchor_index ||
                !SyncIndexesLocked(chainstate, *tip, error)) {
                if (error.empty()) {
                    error = "FlowMesh seat anchor is not on the active chain";
                }
                return std::nullopt;
            }
            const Consensus::Params& params{chainman.GetConsensus()};
            const auto snapshot{chainstate.ModernFnSeats().AnchoredSnapshot(
                chainstate.m_chain, *anchor_index, tip->nHeight, params,
                error)};
            if (!snapshot || !snapshot->FlowMeshReady()) {
                if (error.empty()) {
                    error = "FlowMesh has fewer than four anchor-final seats";
                }
                return std::nullopt;
            }
            const auto domain{
                params.legacy_final_hash
                    ? modern::ModernChainDomain(params.hashGenesisBlock,
                                                *params.legacy_final_hash)
                    : std::nullopt};
            if (!domain) {
                error = "FlowMesh chain domain is unavailable";
                return std::nullopt;
            }
            std::vector<flowmesh::BlsSeatBinding> bindings;
            bindings.reserve(snapshot->members.size());
            for (const FnSeatRecord& member : snapshot->members) {
                bindings.push_back({member.outpoint, member.bls_pubkey,
                                    member.proof_of_possession});
            }
            flowmesh::BlsSeatSetCheck check;
            out = flowmesh::BuildActiveFnBlsSeatSet(
                *domain, market_id, epoch,
                static_cast<uint64_t>(anchor.height), anchor.hash, bindings,
                check);
            if (!out) {
                error = std::string{"FlowMesh active seat set is invalid: "} +
                        flowmesh::BlsSeatSetCheckName(check);
                return std::nullopt;
            }
        }
        RememberSeatSet(*out);
        return out;
    }

    std::optional<flowmesh::AnchorRef> UniqueBootstrapAnchor(
        const FlowMeshMarketRecord& record, std::string& error) const
    {
        LOCK(::cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        const CBlockIndex* tip{chainstate.m_chain.Tip()};
        if (tip == nullptr || !SyncIndexesLocked(chainstate, *tip, error)) {
            if (error.empty()) {
                error = "FlowMesh bootstrap history is unavailable";
            }
            return std::nullopt;
        }
        const CBlockIndex* creation{
            record.created_height >= 0
                ? chainstate.m_chain[record.created_height]
                : nullptr};
        if (creation == nullptr ||
            creation->GetBlockHash() != record.created_block) {
            error = "FlowMesh market creation is not canonical";
            return std::nullopt;
        }
        const auto canonical_record{
            chainstate.ModernFlowMeshVaults().Index().MarketAt(
                record.market_id, *creation)};
        if (!canonical_record || !(*canonical_record == record)) {
            error = "FlowMesh market creation history is inconsistent";
            return std::nullopt;
        }
        const auto snapshot{
            chainstate.ModernFnSeats().Index().EarliestFlowMeshReadySnapshot(
                chainstate.m_chain, record.created_height, tip->nHeight,
                chainman.GetConsensus(), error)};
        if (!snapshot) return std::nullopt;
        return flowmesh::AnchorRef{snapshot->anchor_height,
                                   snapshot->anchor_hash};
    }

    std::optional<flowmesh::ActiveFnBlsSeatSet> SeatSet(
        const uint256& domain, const flowmesh::MarketId& market_id,
        const uint64_t epoch, const uint256& set_hash) const override
    {
        const auto expected_domain{ChainDomain()};
        if (!expected_domain || domain != *expected_domain) return std::nullopt;
        if (const auto known{KnownSeatSet(market_id, epoch, set_hash)}) {
            return known;
        }

        std::vector<std::pair<uint64_t, flowmesh::AnchorRef>> candidates;
        {
            LOCK(::cs_main);
            Chainstate& chainstate{chainman.ActiveChainstate()};
            const CBlockIndex* tip{chainstate.m_chain.Tip()};
            std::string error;
            if (tip == nullptr || !SyncIndexesLocked(chainstate, *tip, error)) {
                return std::nullopt;
            }
            const FlowMeshCheckpointIndex& index{
                chainstate.ModernFlowMeshCheckpoints().Index()};
            auto cursor{index.Head(market_id)};
            std::set<modern::FlowMeshCheckpointId> seen;
            while (cursor && seen.insert(cursor->checkpoint_id).second) {
                if (cursor->core.epoch == epoch &&
                    cursor->core.seat_set_hash == set_hash) {
                    candidates.emplace_back(
                        epoch,
                        flowmesh::AnchorRef{
                            static_cast<int32_t>(cursor->core.anchor.height),
                            cursor->core.anchor.block_hash});
                    break;
                }
                if (cursor->core.handoff &&
                    cursor->core.handoff->next_epoch == epoch &&
                    cursor->core.handoff->next_seat_set_hash == set_hash) {
                    candidates.emplace_back(
                        epoch,
                        flowmesh::AnchorRef{
                            static_cast<int32_t>(
                                cursor->core.handoff->next_anchor.height),
                            cursor->core.handoff->next_anchor.block_hash});
                    break;
                }
                if (cursor->core.previous_checkpoint_id.IsNull()) break;
                cursor = index.Get(cursor->core.previous_checkpoint_id);
            }
        }
        if (candidates.empty()) return std::nullopt;
        std::string error;
        auto built{BuildSeatSetAt(market_id, candidates.front().first,
                                  candidates.front().second, error)};
        if (!built || built->set_hash != set_hash) return std::nullopt;
        return built;
    }

    std::optional<flowmesh::ActiveFnBlsSeatSet> GetSeatSet(
        const uint256& domain, const flowmesh::MarketId& market_id,
        const uint64_t epoch, const uint256& seat_set_hash) const override
    {
        return SeatSet(domain, market_id, epoch, seat_set_hash);
    }

    std::optional<flowmesh::ActiveFnBlsSeatSet> SeatSetForSequence(
        const uint256& domain, const flowmesh::MarketId& market_id,
        const uint64_t sequence) const override
    {
        const auto expected_domain{ChainDomain()};
        if (!expected_domain || domain != *expected_domain) return std::nullopt;

        std::optional<std::pair<uint64_t, uint256>> identity;
        {
            LOCK(::cs_main);
            Chainstate& chainstate{chainman.ActiveChainstate()};
            const CBlockIndex* tip{chainstate.m_chain.Tip()};
            std::string error;
            if (tip == nullptr || !SyncIndexesLocked(chainstate, *tip, error)) {
                return std::nullopt;
            }
            const FlowMeshCheckpointIndex& index{
                chainstate.ModernFlowMeshCheckpoints().Index()};
            auto cursor{index.Head(market_id)};
            std::set<modern::FlowMeshCheckpointId> seen;
            while (cursor && seen.insert(cursor->checkpoint_id).second) {
                if (cursor->core.sequence <= sequence) {
                    if (cursor->core.handoff &&
                        cursor->core.sequence < sequence) {
                        identity = std::pair{
                            cursor->core.handoff->next_epoch,
                            cursor->core.handoff->next_seat_set_hash};
                    } else {
                        identity = std::pair{cursor->core.epoch,
                                             cursor->core.seat_set_hash};
                    }
                    break;
                }
                if (cursor->core.previous_checkpoint_id.IsNull()) break;
                cursor = index.Get(cursor->core.previous_checkpoint_id);
            }
        }
        if (identity) {
            return SeatSet(domain, market_id, identity->first,
                           identity->second);
        }

        // Before the first connected checkpoint every durable entry is in
        // epoch zero. There is exactly one cached epoch-zero bootstrap set.
        std::lock_guard<std::mutex> lock{mutex};
        for (const auto& [key, seats] : seat_sets) {
            if (std::get<0>(key) == market_id && std::get<1>(key) == 0) {
                return seats;
            }
        }
        return std::nullopt;
    }

    FlowMeshSeatTransition SeatTransition(
        const uint256& domain, const flowmesh::MarketId& market_id,
        const flowmesh::ActiveFnBlsSeatSet& current) const override
    {
        if (!ReconciledAtTip()) {
            return {FlowMeshSeatTransitionKind::PAUSED, std::nullopt};
        }
        const auto expected_domain{ChainDomain()};
        if (!expected_domain || domain != *expected_domain ||
            market_id != current.market_id) {
            return {FlowMeshSeatTransitionKind::PAUSED, std::nullopt};
        }
        // Sequence zero is produced immediately, but no later execution may
        // begin until its deterministic genesis checkpoint is connected.
        if (GenesisNotProduced(market_id)) return {};
        if (GenesisCheckpointPending(market_id)) {
            return {FlowMeshSeatTransitionKind::PAUSED, std::nullopt};
        }
        // A connected type-9 withdrawal is retired inside one dedicated,
        // certified settlement entry. Do not let later state build on it
        // until that exact entry is anchored by type 8.
        if (SettlementCheckpointPending(market_id)) {
            return {FlowMeshSeatTransitionKind::PAUSED, std::nullopt};
        }
        // Settlement has priority over committee rotation. Otherwise a
        // handoff could advance the production anchor past a connected
        // type-9 withdrawal without retiring its FlowMesh liability.
        const auto settlement_required{
            SettlementExecutionRequired(market_id)};
        if (!settlement_required) {
            return {FlowMeshSeatTransitionKind::PAUSED, std::nullopt};
        }
        if (*settlement_required) return {};
        if (current.epoch == std::numeric_limits<uint64_t>::max()) {
            return {FlowMeshSeatTransitionKind::PAUSED, std::nullopt};
        }
        const flowmesh::AnchorRef anchor{anchors.Current()};
        std::string error;
        const auto next{BuildSeatSetAt(market_id, current.epoch + 1, anchor,
                                       error)};
        if (!next) {
            return {FlowMeshSeatTransitionKind::PAUSED, std::nullopt};
        }
        if (SameMembers(current, *next)) return {};
        // Ordinary checkpoints are asynchronous and do not throttle trading.
        // A membership change is the one place where the outgoing committee
        // must drain every required checkpoint before signing its handoff.
        if (CheckpointPending(market_id)) {
            return {FlowMeshSeatTransitionKind::PAUSED, std::nullopt};
        }
        return {FlowMeshSeatTransitionKind::HANDOFF, *next};
    }

    std::optional<FlowMeshRuntimeConnectedHandoff>
    ConnectedHandoffCheckpoint(
        const uint256& domain, const flowmesh::MarketId& market_id,
        const uint256& microblock_hash) const override
    {
        const auto expected_domain{ChainDomain()};
        if (!expected_domain || domain != *expected_domain) return std::nullopt;
        LOCK(::cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        const CBlockIndex* tip{chainstate.m_chain.Tip()};
        std::string error;
        if (tip == nullptr || !SyncIndexesLocked(chainstate, *tip, error)) {
            return std::nullopt;
        }
        const FlowMeshCheckpointIndex& index{
            chainstate.ModernFlowMeshCheckpoints().Index()};
        auto cursor{index.Head(market_id)};
        std::set<modern::FlowMeshCheckpointId> seen;
        while (cursor && seen.insert(cursor->checkpoint_id).second) {
            if (cursor->core.microblock_hash == microblock_hash &&
                cursor->core.kind ==
                    modern::FlowMeshCheckpointKind::EPOCH_HANDOFF) {
                const ProductionB3Connection connection{
                    cursor->connected_height, cursor->connected_block};
                if (!FlowMeshHandoffConnectionMature(connection,
                                                      tip->nHeight)) {
                    return std::nullopt;
                }
                return FlowMeshRuntimeConnectedHandoff{
                    cursor->core, connection};
            }
            if (cursor->core.previous_checkpoint_id.IsNull()) break;
            cursor = index.Get(cursor->core.previous_checkpoint_id);
        }
        return std::nullopt;
    }

    std::optional<FlowMeshHistoricalRewardSeat> HistoricalRewardSeat(
        const uint256& domain, const flowmesh::MarketId& market_id,
        const flowmesh::AccountId& reward_account) const override
    {
        const auto expected_domain{ChainDomain()};
        if (!expected_domain || domain != *expected_domain) return std::nullopt;
        std::vector<flowmesh::ActiveFnBlsSeatSet> known;
        {
            std::lock_guard<std::mutex> lock{mutex};
            for (const auto& [key, seats] : seat_sets) {
                if (std::get<0>(key) == market_id) known.push_back(seats);
            }
        }
        for (const auto& seats : known) {
            for (const auto& member : seats.members) {
                const flowmesh::FlowMeshFeeSeat fee_seat{
                    member.seat_id, member.key.Key().Compressed()};
                if (flowmesh::FlowMeshSeatRewardAccount(
                        market_id, seats.epoch, fee_seat) == reward_account) {
                    return FlowMeshHistoricalRewardSeat{seats, member};
                }
            }
        }
        return std::nullopt;
    }

    std::vector<bls::SecretKey> LocalSeatKeys(
        const flowmesh::MarketId&,
        const flowmesh::ActiveFnBlsSeatSet&) const override
    {
        if (!ReconciledAtTip()) return {};
        if (transport.mode == "independent") {
            std::shared_ptr<FlowMeshNetService> active_network;
            {
                std::lock_guard<std::mutex> lock{mutex};
                active_network = network;
            }
            // Transport failure is not permission to start a new signing history.
            if (!active_network || !active_network->Snapshot().running) return {};
        }
        std::lock_guard<std::mutex> lock{mutex};
        if (!running || stopping) return {};
        return local_keys;
    }

    std::optional<uint256> ChainDomain() const
    {
        const Consensus::Params& params{chainman.GetConsensus()};
        return params.legacy_final_hash
                   ? modern::ModernChainDomain(params.hashGenesisBlock,
                                               *params.legacy_final_hash)
                   : std::nullopt;
    }

    std::optional<uint256> TreasuryOwnerCommitment() const
    {
        const Consensus::Params& params{chainman.GetConsensus()};
        if (!params.modern_pos || params.modern_pos->treasury_script.empty()) {
            return std::nullopt;
        }
        const CScript script{params.modern_pos->treasury_script.begin(),
                             params.modern_pos->treasury_script.end()};
        const uint256 commitment{modern::AssetOwnerCommitment(script)};
        return commitment.IsNull() ? std::nullopt
                                   : std::optional<uint256>{commitment};
    }

    bool CheckpointPending(const flowmesh::MarketId& market_id) const
    {
        FlowMeshProductionStore* store{nullptr};
        {
            std::lock_guard<std::mutex> lock{mutex};
            const auto it{markets.find(market_id)};
            if (it == markets.end() || !it->second->ready ||
                !it->second->store) {
                return false;
            }
            store = it->second->store.get();
        }
        std::optional<FlowMeshProductionStore::Marker> marker;
        std::string error;
        if (!store->ReadMarker(marker, error) || !marker) return true;
        const auto seats{SeatSet(marker->domain, marker->market_id,
                                 marker->current_epoch,
                                 marker->current_seat_set_hash)};
        if (!seats) return true;
        std::optional<ProductionCheckpointCandidate> candidate;
        return !store->NextCheckpointCandidate(*seats, candidate, error) ||
               candidate.has_value();
    }

    std::optional<FlowMeshProductionStore::Marker> MarketMarker(
        const flowmesh::MarketId& market_id) const
    {
        FlowMeshProductionStore* store{nullptr};
        {
            std::lock_guard<std::mutex> lock{mutex};
            const auto it{markets.find(market_id)};
            if (it == markets.end() || !it->second->ready ||
                !it->second->store) {
                return std::nullopt;
            }
            store = it->second->store.get();
        }
        std::optional<FlowMeshProductionStore::Marker> marker;
        std::string error;
        if (!store->ReadMarker(marker, error)) return std::nullopt;
        return marker;
    }

    bool GenesisNotProduced(const flowmesh::MarketId& market_id) const
    {
        const auto marker{MarketMarker(market_id)};
        return !marker || marker->next_sequence == 0;
    }

    bool GenesisCheckpointPending(const flowmesh::MarketId& market_id) const
    {
        const auto marker{MarketMarker(market_id)};
        return !marker || (marker->next_sequence > 0 &&
                           marker->last_b3_checkpoint.IsNull());
    }

    bool SettlementCheckpointPending(
        const flowmesh::MarketId& market_id) const
    {
        FlowMeshProductionStore* store{nullptr};
        {
            std::lock_guard<std::mutex> lock{mutex};
            const auto it{markets.find(market_id)};
            if (it == markets.end() || !it->second->ready ||
                !it->second->store) {
                return true;
            }
            store = it->second->store.get();
        }
        std::optional<FlowMeshProductionStore::Marker> marker;
        std::string error;
        if (!store->ReadMarker(marker, error) || !marker) return true;
        if (marker->next_sequence == 0) return false;
        const uint64_t last_sequence{marker->next_sequence - 1};
        const auto seats{SeatSetForSequence(marker->domain, market_id,
                                             last_sequence)};
        if (!seats) return true;
        std::optional<StoredProductionEntry> stored;
        if (!store->ReadEntry(last_sequence, *seats, stored, error) ||
            !stored) {
            return true;
        }
        if (stored->settlements.empty()) return false;
        if (marker->last_b3_checkpoint.IsNull()) return true;

        LOCK(::cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        const CBlockIndex* tip{chainstate.m_chain.Tip()};
        if (tip == nullptr || !SyncIndexesLocked(chainstate, *tip, error)) {
            return true;
        }
        const auto connected{chainstate.ModernFlowMeshCheckpoints()
                                 .Index()
                                 .Get(marker->last_b3_checkpoint)};
        return !connected || connected->core.market_id != market_id ||
               connected->core.sequence < last_sequence;
    }

    std::optional<bool> SettlementExecutionRequired(
        const flowmesh::MarketId& market_id) const
    {
        FlowMeshProductionStore* store{nullptr};
        const ChainDepositVerifier* chain_facts{nullptr};
        {
            std::lock_guard<std::mutex> lock{mutex};
            const auto it{markets.find(market_id)};
            if (it == markets.end() || !it->second->ready ||
                !it->second->store || !it->second->deposits) {
                return std::nullopt;
            }
            store = it->second->store.get();
            chain_facts = it->second->deposits.get();
        }
        std::optional<FlowMeshProductionStore::Marker> marker;
        std::string error;
        if (!store->ReadMarker(marker, error) || !marker) return std::nullopt;
        if (marker->next_sequence == 0) return false;
        const uint64_t last_sequence{marker->next_sequence - 1};
        const auto seats{SeatSetForSequence(marker->domain, market_id,
                                             last_sequence)};
        if (!seats) return std::nullopt;
        std::optional<StoredProductionEntry> stored;
        if (!store->ReadEntry(last_sequence, *seats, stored, error) ||
            !stored) {
            return std::nullopt;
        }
        const flowmesh::AnchorRef through{anchors.Current()};
        const auto plan{chain_facts->PlanWithdrawalSettlements(
            std::optional<flowmesh::AnchorRef>{stored->entry.anchor}, through)};
        if (!plan) return std::nullopt;
        return plan->count != 0;
    }

    FlowMeshRelayResult Relay(FlowMeshRuntimeRelay relay) const
    {
        FlowMeshRelayResult result;
        PeerManager* target{nullptr};
        std::shared_ptr<FlowMeshNetService> independent;
        {
            std::lock_guard<std::mutex> lock{mutex};
            if (!running || stopping) {
                result.no_peer_reason = FlowMeshDeliveryAdmission::STOPPED;
                result.reason = "FlowMesh service is stopping or not running";
                return result;
            }
            target = peerman;
            independent = network;
        }
        if (!ReconciledAtTip()) {
            result.no_peer_reason = FlowMeshDeliveryAdmission::RECONCILING;
            result.reason = "B3 tip is not yet reconciled; retain and revalidate the exact object before retry";
            return result;
        }
        // Disjoint connection IDs keep directed replies on their originating
        // transport. Both transports still feed the same execution/signing state.
        if (transport.IndependentEnabled() && (!relay.peer || *relay.peer <= -2)) {
            if (independent) {
                result = independent->Relay(relay);
            } else {
                result.no_peer_reason = FlowMeshDeliveryAdmission::STOPPED;
                result.reason = "Independent FlowMesh network is not available";
            }
        }
        if (transport.LegacyEnabled() && target && (!relay.peer || *relay.peer >= 0)) {
            auto legacy{target->RelayFlowMeshMessage(relay.message, relay.peer, relay.exclude_peer)};
            result.peers.insert(result.peers.end(), legacy.peers.begin(), legacy.peers.end());
            if (result.reason.empty()) {
                result.no_peer_reason = legacy.no_peer_reason;
                result.reason = std::move(legacy.reason);
            }
        }
        return result;
    }

    void CancelDelivery(uint64_t delivery_id) const
    {
        std::shared_ptr<FlowMeshNetService> independent;
        {
            std::lock_guard<std::mutex> lock{mutex};
            independent = network;
        }
        if (independent) independent->Cancel(delivery_id);
    }

    bool RefreshMarkets(uint256& sampled_tip, std::string& error);
    bool InstallMarket(const FlowMeshMarketRecord& record,
                       std::string& error);
    bool ReconcileConnectedCheckpoints(std::string& error);
    bool ReconcileStoreConnections(FlowMeshProductionStore& store,
                                   bool& rolled_back,
                                   std::string& error);
    bool ReconcileAllStoreConnections(std::string& error);

    void TickerLoop()
    {
        util::ThreadRename("flowmesh-tick");
        std::unique_lock<std::mutex> lock{mutex};
        while (!stopping) {
            ticker_cv.wait_for(lock, FLOWMESH_TICK_INTERVAL,
                               [&] { return stopping; });
            if (stopping) break;
            std::shared_ptr<FlowMeshRuntime> active{runtime};
            lock.unlock();
            // Maintenance must continue while B3 reconciliation closes the
            // signing gate: expire deferred ingress and cancel stale queued
            // copies. Runtime production/retries still check chain validity.
            if (active) {
                active->NotifyTick();
            }
            lock.lock();
        }
    }
};

bool FlowMeshService::Impl::ReconcileStoreConnections(
    FlowMeshProductionStore& store, bool& rolled_back, std::string& error)
{
    std::vector<int32_t> heights;
    if (!store.ConnectedB3Heights(heights, error)) return false;

    std::map<int32_t, uint256> canonical_blocks;
    uint256 snapshot_tip;
    {
        LOCK(::cs_main);
        const CChain& chain{chainman.ActiveChain()};
        const CBlockIndex* tip{chain.Tip()};
        if (tip) snapshot_tip = tip->GetBlockHash();
        for (const int32_t height : heights) {
            const CBlockIndex* active{height >= 0 ? chain[height] : nullptr};
            canonical_blocks.emplace(
                height, active ? active->GetBlockHash() : uint256{});
        }
    }
    if (!store.ReconcileCheckpointConnections(canonical_blocks, rolled_back,
                                               error)) {
        return false;
    }

    // Never resume the runtime using a snapshot that changed while the durable
    // batch was being checked/written. A downward rollback remains safe; the
    // next tip callback will reconcile the newer chain before unpausing.
    {
        LOCK(::cs_main);
        const CBlockIndex* tip{chainman.ActiveChain().Tip()};
        const uint256 current_tip{tip ? tip->GetBlockHash() : uint256{}};
        if (current_tip != snapshot_tip) {
            error = "B3 tip changed during FlowMesh checkpoint reconciliation";
            return false;
        }
    }
    return true;
}

bool FlowMeshService::Impl::ReconcileAllStoreConnections(std::string& error)
{
    uint256 initial_tip;
    {
        LOCK(::cs_main);
        const CBlockIndex* tip{chainman.ActiveChain().Tip()};
        if (tip) initial_tip = tip->GetBlockHash();
    }
    std::vector<std::pair<flowmesh::MarketId, FlowMeshProductionStore*>> stores;
    {
        std::lock_guard<std::mutex> lock{mutex};
        for (const auto& [id, resources] : markets) {
            if (resources->store) stores.emplace_back(id, resources->store.get());
        }
    }
    for (const auto& [id, store] : stores) {
        bool rolled_back{false};
        if (!ReconcileStoreConnections(*store, rolled_back, error)) {
            if (error.empty()) {
                error = "FlowMesh durable checkpoint reconciliation failed";
            }
            return false;
        }
        if (rolled_back) {
            LogWarning("FlowMesh market %s rolled back disconnected B3 checkpoint state\n",
                       id.GetHex());
        }
    }
    uint256 final_tip;
    {
        LOCK(::cs_main);
        const CBlockIndex* tip{chainman.ActiveChain().Tip()};
        final_tip = tip ? tip->GetBlockHash() : uint256{};
        if (final_tip != initial_tip) {
            error = "B3 tip changed while FlowMesh markets were reconciling";
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lock{reconciled_tip_mutex};
        reconciled_tip = final_tip;
    }
    return true;
}

bool FlowMeshService::Impl::InstallMarket(
    const FlowMeshMarketRecord& record, std::string& error)
{
    const auto domain{ChainDomain()};
    const auto treasury{TreasuryOwnerCommitment()};
    const auto expected_market{
        domain ? flowmesh::ComputeFlowMeshMarketId(*domain, record.base_asset)
               : std::nullopt};
    const auto expected_vault{
        expected_market && domain
            ? flowmesh::ComputeFlowMeshVaultId(*domain, *expected_market)
            : std::nullopt};
    if (!domain || !treasury || !expected_market || !expected_vault ||
        *expected_market != record.market_id ||
        *expected_vault != record.vault_id) {
        error = "FlowMesh market registry identity is inconsistent";
        return false;
    }

    auto metadata_state{flowmesh::FlowMeshState{
        record.vault_id, record.base_asset, modern::NativeAsset(),
        flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS}};
    FlowMeshServiceMarket metadata{
        *domain, record.market_id, record.vault_id, record.base_asset,
        modern::NativeAsset(), metadata_state.ConfigId()};

    MarketResources* resources{nullptr};
    {
        std::lock_guard<std::mutex> lock{mutex};
        auto [it, inserted]{markets.try_emplace(
            record.market_id, std::make_unique<MarketResources>())};
        resources = it->second.get();
        if (inserted) {
            resources->metadata = metadata;
            resources->chain_record = record;
            Chainstate* active_chainstate{nullptr};
            {
                LOCK(::cs_main);
                active_chainstate = &chainman.ActiveChainstate();
            }
            resources->deposits = std::make_unique<ChainDepositVerifier>(
                *active_chainstate, record.base_asset,
                record.market_id);
        } else if (!(resources->metadata == metadata) ||
                   !(resources->chain_record == record)) {
            error = "FlowMesh market identity changed after discovery";
            return false;
        }
        if (resources->ready) return true;
    }

    // Open the database before choosing a bootstrap anchor. Every fresh or
    // epoch-zero store must use the same chain-derived anchor: the earliest
    // post-market block with four active seats. Discovery time is irrelevant.
    const bool preagreement{preagreement_markets.contains(record.market_id)};
    auto store{std::make_unique<FlowMeshProductionStore>(DBParams{
        .path = datadir / fs::PathFromString(record.market_id.GetHex()),
        .cache_bytes = FLOWMESH_MARKET_DB_CACHE_BYTES}, preagreement)};
    bool fresh{false};
    if (!store->CheckForMarket(*domain, record.market_id, fresh, error)) {
        return false;
    }
    bool rolled_back{false};
    if (!ReconcileStoreConnections(*store, rolled_back, error)) return false;
    if (rolled_back) {
        LogWarning("FlowMesh market %s restored its pre-reorg checkpoint marker during startup\n",
                   record.market_id.GetHex());
    }
    std::optional<FlowMeshProductionStore::Marker> disk_marker;
    if (!store->ReadMarker(disk_marker, error)) return false;
    const auto unique_bootstrap_anchor{UniqueBootstrapAnchor(record, error)};
    if (disk_marker && !unique_bootstrap_anchor) return false;
    if (disk_marker && unique_bootstrap_anchor &&
        disk_marker->current_epoch == 0 &&
        disk_marker->current_anchor != *unique_bootstrap_anchor) {
        error = "FlowMesh epoch-zero store has a non-canonical bootstrap anchor";
        return false;
    }
    const flowmesh::AnchorRef bootstrap_anchor{
        disk_marker ? disk_marker->current_anchor
                    : unique_bootstrap_anchor.value_or(flowmesh::AnchorRef{})};
    const uint64_t bootstrap_epoch{disk_marker ? disk_marker->current_epoch : 0};
    auto bootstrap_seats{
        unique_bootstrap_anchor
            ? BuildSeatSetAt(record.market_id, bootstrap_epoch,
                             bootstrap_anchor, error)
            : std::nullopt};
    if (!bootstrap_seats) {
        // A market is visible before it is runnable when fewer than four
        // seats are anchor-final. Install one explicit paused shell so status
        // and wallet APIs remain honest, then retry on later tips.
        std::shared_ptr<FlowMeshRuntime> active;
        bool install_shell{false};
        {
            std::lock_guard<std::mutex> lock{mutex};
            active = runtime;
            install_shell = !resources->installed;
        }
        if (active && install_shell) {
            FlowMeshRuntimeMarketConfig config{
                .domain = *domain,
                .market_id = record.market_id,
                .treasury_owner_commitment = *treasury,
                .active_seats = {},
                .state = metadata_state,
                .last_microblock_hash = {},
                .readiness =
                    FlowMeshRuntimeMarketReadiness::INSUFFICIENT_SEATS};
            std::string add_error;
            if (!active->AddMarket(std::move(config), add_error)) {
                error = add_error;
                return false;
            }
            std::lock_guard<std::mutex> lock{mutex};
            resources->installed = true;
        }
        error.clear();
        return true;
    }
    if (disk_marker &&
        (bootstrap_seats->set_hash != disk_marker->current_seat_set_hash ||
         bootstrap_seats->epoch != disk_marker->current_epoch)) {
        error = "FlowMesh store marker has a different canonical seat set";
        return false;
    }
    if (preagreement && (fresh || (disk_marker && !disk_marker->agreement_bootstrap_complete))) {
        // A missing local directory is not evidence of a globally fresh
        // market. Require explicit selection AND no canonical B3 checkpoint.
        // Repeat this check after an interrupted bootstrap as well. Hold the
        // chain snapshot through the first durable mode binding.
        LOCK(::cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        const CBlockIndex* tip{chainstate.m_chain.Tip()};
        if (!tip || !SyncIndexesLocked(chainstate, *tip, error)) return false;
        if (chainstate.ModernFlowMeshCheckpoints().Index().Head(record.market_id)) {
            error = "FlowMesh preagreement requires a fresh market without B3 checkpoint history; existing markets cannot migrate";
            return false;
        }
        if (!store->OpenForMarket(*domain, record.market_id, *bootstrap_seats,
                                 metadata_state.Root(), error)) return false;
    } else {
        if (!store->OpenForMarket(*domain, record.market_id, *bootstrap_seats,
                                 metadata_state.Root(), error)) return false;
    }

    uint256 last_hash;
    if (disk_marker && disk_marker->next_sequence > 0) {
        flowmesh::ProductionAnchorContext anchors_context{
            TipHeight(), std::nullopt, &anchors};
        if (!store->Replay(metadata_state, last_hash, *this, anchors_context,
                           *treasury, resources->deposits.get(), error)) {
            return false;
        }
    }
    std::optional<FlowMeshProductionStore::Marker> marker;
    if (!store->ReadMarker(marker, error) || !marker) {
        if (error.empty()) error = "FlowMesh store marker is unavailable";
        return false;
    }
    auto active_seats{SeatSet(marker->domain, marker->market_id,
                              marker->current_epoch,
                              marker->current_seat_set_hash)};
    if (!active_seats) {
        error = "FlowMesh store current seat set is unavailable";
        return false;
    }

    FlowMeshRuntimeMarketConfig config{
        .domain = *domain,
        .market_id = record.market_id,
        .treasury_owner_commitment = *treasury,
        .active_seats = *active_seats,
        .state = metadata_state,
        .next_sequence = marker->next_sequence,
        .next_effect_index = marker->next_effect_index,
        .last_microblock_hash = marker->last_microblock_hash,
        .store = store.get(),
        .deposits = resources->deposits.get(),
        .preagreement = preagreement,
        .agreement_path = datadir / fs::PathFromString(record.market_id.GetHex()) / "agreement",
        .agreement_identity = marker->agreement_identity,
        .agreement_allow_create = preagreement && !marker->agreement_bootstrap_complete};

    std::shared_ptr<FlowMeshRuntime> active;
    {
        std::lock_guard<std::mutex> lock{mutex};
        active = runtime;
        resources->store = std::move(store);
    }
    if (!active || !active->AddMarket(std::move(config), error)) return false;
    bool observer{true};
    {
        std::lock_guard<std::mutex> lock{mutex};
        resources->installed = true;
        resources->ready = true;
        observer = local_keys.empty();
    }
    LogInfo("FlowMesh market %s started (%s/B3, epoch=%d, observer=%d)\n",
            record.market_id.GetHex(), record.base_asset.GetHex(),
            active_seats->epoch, observer);
    return true;
}

bool FlowMeshService::Impl::RefreshMarkets(uint256& sampled_tip,
                                           std::string& error)
{
    sampled_tip.SetNull();
    std::lock_guard<std::mutex> refresh_lock{refresh_mutex};
    {
        std::lock_guard<std::mutex> lock{mutex};
        if (!running || stopping) return true;
    }
    const Consensus::Params& params{chainman.GetConsensus()};
    if (!Consensus::FlowMeshSeatBindingScheduleConfigured(params)) return true;

    flowmesh::AnchorRef discovery_anchor;
    std::vector<FlowMeshMarketRecord> discovered;
    {
        LOCK(::cs_main);
        Chainstate& chainstate{chainman.ActiveChainstate()};
        const CBlockIndex* tip{chainstate.m_chain.Tip()};
        if (tip == nullptr || !Consensus::FlowMeshRulesActive(tip->nHeight,
                                                               params)) {
            return true;
        }
        sampled_tip = tip->GetBlockHash();
        if (!SyncIndexesLocked(chainstate, *tip, error)) return false;
        const int anchor_height{tip->nHeight - FLOWMESH_ANCHOR_DEPTH};
        const CBlockIndex* anchor{
            anchor_height >= 0 ? chainstate.m_chain[anchor_height] : nullptr};
        if (anchor == nullptr) return true;
        discovery_anchor = {anchor->nHeight, anchor->GetBlockHash()};
        discovered =
            chainstate.ModernFlowMeshVaults().Index().MarketsAt(*anchor);
    }
    std::map<flowmesh::MarketId, FlowMeshMarketRecord> canonical;
    for (const FlowMeshMarketRecord& record : discovered) {
        canonical.emplace(record.market_id, record);
    }
    {
        std::lock_guard<std::mutex> lock{mutex};
        for (const auto& [id, resources] : markets) {
            const auto it{canonical.find(id)};
            if (it == canonical.end() ||
                !(resources->chain_record == it->second)) {
                error = "installed FlowMesh market is absent from the canonical anchor";
                return false;
            }
        }
    }
    for (const FlowMeshMarketRecord& record : discovered) {
        if (!InstallMarket(record, error)) return false;
    }
    {
        LOCK(::cs_main);
        const CBlockIndex* tip{chainman.ActiveChain().Tip()};
        if (tip == nullptr || tip->GetBlockHash() != sampled_tip) {
            error = "B3 tip changed while FlowMesh markets were refreshing";
            return false;
        }
    }
    return true;
}

bool FlowMeshService::Impl::ReconcileConnectedCheckpoints(std::string& error)
{
    std::vector<flowmesh::MarketId> ids;
    {
        std::lock_guard<std::mutex> lock{mutex};
        for (const auto& [id, resources] : markets) {
            if (resources->ready && resources->store) ids.push_back(id);
        }
    }
    for (const auto& id : ids) {
        FlowMeshProductionStore* store{nullptr};
        {
            std::lock_guard<std::mutex> lock{mutex};
            store = markets.at(id)->store.get();
        }
        // A block may connect several ordinary checkpoints. Bound work even
        // though v1 normally publishes one earliest entry at a time.
        for (size_t count{0}; count < 64; ++count) {
            std::optional<FlowMeshProductionStore::Marker> marker;
            if (!store->ReadMarker(marker, error) || !marker) {
                if (error.empty()) error = "FlowMesh production marker is unavailable";
                return false;
            }
            const auto seats{SeatSet(marker->domain, marker->market_id,
                                     marker->current_epoch,
                                     marker->current_seat_set_hash)};
            if (!seats) {
                error = "FlowMesh checkpoint reconciliation cannot resolve the active seat set";
                return false;
            }
            std::optional<ProductionCheckpointCandidate> candidate;
            if (!store->NextCheckpointCandidate(*seats, candidate, error)) {
                return false;
            }
            if (!candidate) {
                break;
            }
            const auto record{flowmesh::BuildProductionCheckpointRecord(
                candidate->stored.entry, candidate->stored.certificate,
                *seats, candidate->previous_checkpoint_id)};
            const auto checkpoint_id{
                record ? modern::FlowMeshCheckpointIdV1(record->core)
                       : std::nullopt};
            if (!record || !checkpoint_id) {
                error = "FlowMesh checkpoint candidate is not canonical";
                return false;
            }

            std::optional<FlowMeshConnectedCheckpoint> connected;
            int32_t canonical_tip_height{-1};
            {
                LOCK(::cs_main);
                Chainstate& chainstate{chainman.ActiveChainstate()};
                const CBlockIndex* tip{chainstate.m_chain.Tip()};
                if (tip == nullptr) {
                    error = "FlowMesh checkpoint reconciliation has no active B3 tip";
                } else if (SyncIndexesLocked(chainstate, *tip, error)) {
                    canonical_tip_height = tip->nHeight;
                    connected = chainstate.ModernFlowMeshCheckpoints()
                                    .Index()
                                    .Get(*checkpoint_id);
                }
            }
            if (!error.empty() && !connected) return false;
            if (!connected) break;
            if (record->core.kind ==
                modern::FlowMeshCheckpointKind::EXECUTION) {
                if (!store->MarkExecutionCheckpointConnected(
                        *record, *seats,
                        ProductionB3Connection{connected->connected_height,
                                               connected->connected_block},
                        error)) {
                    LogWarning("FlowMesh checkpoint reconciliation failed for %s: %s\n",
                               id.GetHex(), error);
                    return false;
                }
                continue;
            }
            const auto next{SeatSet(
                marker->domain, marker->market_id,
                candidate->stored.entry.next_epoch,
                candidate->stored.entry.next_seat_set_hash)};
            if (!next) {
                error = "FlowMesh handoff reconciliation cannot resolve the incoming seat set";
                return false;
            }
            const ProductionB3Connection connection{
                connected->connected_height, connected->connected_block};
            // The outgoing committee remains the durable owner during this
            // window. If the publication is shallowly reorganized, there are
            // no incoming-epoch entries to unwind and the exact handoff can be
            // republished safely on the replacement chain.
            if (!FlowMeshHandoffConnectionMature(connection,
                                                  canonical_tip_height)) {
                break;
            }
            if (!store->MarkHandoffCheckpointConnected(
                    *record, *seats, *next, connection,
                    canonical_tip_height, error)) {
                LogWarning("FlowMesh handoff reconciliation failed for %s: %s\n",
                           id.GetHex(), error);
                return false;
            }
            break; // runtime observes and switches the marker on its tick
        }
    }
    return true;
}

bool ParseFlowMeshPreagreementMarkets(const std::vector<std::string>& values,
                                     std::set<flowmesh::MarketId>& out,
                                     std::string& error)
{
    std::set<flowmesh::MarketId> parsed;
    for (const std::string& value : values) {
        const auto market{uint256::FromHex(value)};
        if (value.size() != 64 || !market || market->IsNull()) {
            error = "-flowmeshpreagreementmarket requires an exact nonzero 64-hex market ID";
            return false;
        }
        parsed.insert(*market);
    }
    out = std::move(parsed);
    return true;
}

FlowMeshService::FlowMeshService(ChainstateManager& chainman, fs::path datadir,
                                 FlowMeshServiceTransport transport,
                                 std::set<flowmesh::MarketId> preagreement_markets)
    : m_impl{std::make_unique<Impl>(chainman, std::move(datadir), std::move(transport),
                                  std::move(preagreement_markets))}
{
}

FlowMeshService::~FlowMeshService() { Stop(); }

bool FlowMeshService::Start(PeerManager& peerman, std::string& error)
{
    if (!m_impl->transport.LegacyEnabled() && !m_impl->transport.IndependentEnabled()) {
        error = "Invalid FlowMesh transport; use legacy, dual or independent";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        if (m_impl->running) return true;
        m_impl->enabled =
            Consensus::FlowMeshSeatBindingScheduleConfigured(
                m_impl->chainman.GetConsensus());
        if (!m_impl->enabled) {
            LogInfo("FlowMesh service dormant: A2/A3 schedule is not complete\n");
            return true;
        }
        m_impl->peerman = &peerman;
        m_impl->stopping = false;
        m_impl->SetChainReconciling(true);
    }

    FlowMeshRuntimeConfig config;
    config.chain = m_impl.get();
    config.keys = m_impl.get();
    config.clock = &m_impl->clock;
    config.relay = [impl = m_impl.get()](FlowMeshRuntimeRelay relay) {
        return impl->Relay(std::move(relay));
    };
    config.cancel_delivery = [impl = m_impl.get()](uint64_t delivery_id) {
        impl->CancelDelivery(delivery_id);
    };
    auto runtime{std::make_shared<FlowMeshRuntime>(
        std::move(config), std::vector<FlowMeshRuntimeMarketConfig>{})};
    if (!runtime->Start(error)) {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        m_impl->peerman = nullptr;
        return false;
    }
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        m_impl->runtime = runtime;
        m_impl->running = true;
        m_impl->ticker =
            std::thread{&FlowMeshService::Impl::TickerLoop, m_impl.get()};
    }
    uint256 market_tip;
    if (!m_impl->RefreshMarkets(market_tip, error)) {
        Stop();
        return false;
    }
    error.clear();
    if (!m_impl->ReconcileAllStoreConnections(error) ||
        !m_impl->ReconcileConnectedCheckpoints(error)) {
        Stop();
        return false;
    }
    error.clear();
    if (!m_impl->ReconcileAllStoreConnections(error)) {
        Stop();
        return false;
    }
    if (m_impl->transport.IndependentEnabled()) {
        auto network_config{m_impl->transport.network};
        network_config.delivery_callback = [weak_runtime = std::weak_ptr<FlowMeshRuntime>{runtime}](const FlowMeshDeliveryEvent& event) {
            const auto active{weak_runtime.lock()};
            return active && active->NotifyDeliveryEvent(event);
        };
        auto network{std::make_shared<FlowMeshNetService>(std::move(network_config), *this)};
        {
            std::lock_guard<std::mutex> lock{m_impl->mutex};
            m_impl->network = network;
        }
        std::string net_error;
        if (!network->Start(net_error)) {
            // Never stop B3 or silently change routing policy on overlay failure.
            LogWarning("Independent FlowMesh network unavailable: %s\n", net_error);
        }
    }
    if (!m_impl->RulesActiveAtTip()) {
        error.clear();
        LogInfo("FlowMesh production service waiting for its A3 activation height\n");
        return true;
    }
    if (!m_impl->ReconciledTipMatches(market_tip)) {
        error.clear();
        LogInfo("FlowMesh production service waiting for a stable reconciled B3 tip\n");
        return true;
    }
    m_impl->SetChainReconciling(false);
    if (!m_impl->ReconciledAtTip()) {
        m_impl->SetChainReconciling(true);
        LogInfo("FlowMesh production service waiting: B3 tip changed before startup completed\n");
        return true;
    }
    runtime->NotifyTick();
    LogInfo("FlowMesh production service started (transport=%s)\n", m_impl->transport.mode);
    return true;
}

void FlowMeshService::Stop()
{
    std::shared_ptr<FlowMeshRuntime> runtime;
    std::shared_ptr<FlowMeshNetService> network;
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        if (!m_impl->running && !m_impl->runtime) return;
        m_impl->stopping = true;
        m_impl->ticker_cv.notify_all();
        runtime = m_impl->runtime;
        network = m_impl->network;
    }
    if (network) network->Stop();
    if (m_impl->ticker.joinable()) m_impl->ticker.join();
    if (runtime) runtime->Stop();
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        m_impl->runtime.reset();
        m_impl->network.reset();
        m_impl->running = false;
        m_impl->peerman = nullptr;
    }
}

bool FlowMeshService::Enabled() const
{
    std::lock_guard<std::mutex> lock{m_impl->mutex};
    return m_impl->enabled;
}

bool FlowMeshService::Running() const
{
    std::lock_guard<std::mutex> lock{m_impl->mutex};
    return m_impl->running && !m_impl->stopping;
}

bool FlowMeshService::LegacyTransportEnabled() const
{
    return m_impl->transport.LegacyEnabled();
}

std::string FlowMeshService::TransportMode() const
{
    return m_impl->transport.mode;
}

FlowMeshNetSnapshot FlowMeshService::NetworkSnapshot() const
{
    std::shared_ptr<FlowMeshNetService> network;
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        network = m_impl->network;
    }
    return network ? network->Snapshot() : FlowMeshNetSnapshot{};
}

FlowMeshNetConnectResult FlowMeshService::AddNetworkPeer(const std::string& peer)
{
    std::shared_ptr<FlowMeshNetService> network;
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        if (!m_impl->running || m_impl->stopping) {
            FlowMeshNetConnectResult result;
            result.error = "FlowMesh service is not running";
            return result;
        }
        network = m_impl->network;
    }
    // Keep the transport alive without holding a service/chain lock across
    // admission. AddPeer checks transport shutdown under its own queue lock.
    if (network) return network->AddPeer(peer);
    FlowMeshNetConnectResult result;
    result.error = "Independent FlowMesh transport is not configured";
    return result;
}

std::vector<FlowMeshRuntimeDeliverySnapshot> FlowMeshService::DeliverySnapshots(
    std::optional<flowmesh::MarketId> market_id) const
{
    std::shared_ptr<FlowMeshRuntime> runtime;
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        runtime = m_impl->runtime;
    }
    return runtime ? runtime->DeliverySnapshots(market_id)
                   : std::vector<FlowMeshRuntimeDeliverySnapshot>{};
}

std::vector<FlowMeshServiceMarket> FlowMeshService::Markets() const
{
    std::lock_guard<std::mutex> lock{m_impl->mutex};
    std::vector<FlowMeshServiceMarket> out;
    out.reserve(m_impl->markets.size());
    for (const auto& [id, resources] : m_impl->markets) {
        (void)id;
        out.push_back(resources->metadata);
    }
    return out;
}

std::optional<FlowMeshServiceMarket> FlowMeshService::Market(
    const flowmesh::MarketId& market_id) const
{
    std::lock_guard<std::mutex> lock{m_impl->mutex};
    const auto it{m_impl->markets.find(market_id)};
    return it == m_impl->markets.end()
               ? std::nullopt
               : std::optional<FlowMeshServiceMarket>{it->second->metadata};
}

std::optional<FlowMeshRuntimeMarketStatus> FlowMeshService::MarketStatus(
    const flowmesh::MarketId& market_id) const
{
    std::shared_ptr<FlowMeshRuntime> runtime;
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        runtime = m_impl->runtime;
    }
    auto status{runtime ? runtime->MarketStatus(market_id) : std::nullopt};
    if (!status) return std::nullopt;
    if (!m_impl->RulesActiveAtTip()) {
        status->paused = true;
        status->error = "FlowMesh service is below its activation height";
    } else if (!m_impl->ReconciledAtTip()) {
        status->paused = true;
        status->error = "FlowMesh service is reconciling the B3 tip";
    }
    return status;
}

std::optional<flowmesh::FlowMeshState> FlowMeshService::StateSnapshot(
    const flowmesh::MarketId& market_id) const
{
    std::shared_ptr<FlowMeshRuntime> runtime;
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        runtime = m_impl->runtime;
    }
    return runtime ? runtime->StateSnapshot(market_id) : std::nullopt;
}

std::optional<flowmesh::MarketData> FlowMeshService::MarketData(
    const flowmesh::MarketId& market_id,
    const std::optional<flowmesh::AccountId>& account,
    const flowmesh::MarketDataQuery& query, std::string& error) const
{
    std::shared_ptr<FlowMeshRuntime> runtime;
    bool running{false};
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        runtime = m_impl->runtime;
        running = m_impl->running && !m_impl->stopping;
    }
    if (!runtime) {
        error = "FlowMesh runtime is unavailable";
        return std::nullopt;
    }
    auto out{runtime->MarketData(market_id, account, query, error)};
    if (out) {
        out->snapshot.running = running;
        const bool rules_active{m_impl->RulesActiveAtTip()};
        const bool reconciled{m_impl->ReconciledAtTip()};
        out->snapshot.chain_reconciling = running && rules_active && !reconciled;
        if (!running || !rules_active || !reconciled) {
            out->snapshot.paused = true;
            if (out->snapshot.error.empty()) out->snapshot.error = "FlowMesh service is not active at the current B3 tip";
        }
    }
    return out;
}

std::optional<flowmesh::ClientStateEvidence> FlowMeshService::ClientSnapshot(
    const flowmesh::MarketId& market_id, std::string& error) const
{
    std::shared_ptr<FlowMeshRuntime> runtime;
    {
        std::lock_guard lock{m_impl->mutex};
        runtime = m_impl->runtime;
    }
    if (!runtime) { error = "FlowMesh runtime is unavailable"; return std::nullopt; }
    return runtime->ClientSnapshot(market_id, error);
}

std::optional<std::vector<unsigned char>> FlowMeshService::ClientCertifiedEntry(
    const flowmesh::MarketId& market_id, const uint64_t sequence, std::string& error) const
{
    std::shared_ptr<FlowMeshRuntime> runtime;
    {
        std::lock_guard lock{m_impl->mutex};
        runtime = m_impl->runtime;
    }
    if (!runtime) { error = "FlowMesh runtime is unavailable"; return std::nullopt; }
    return runtime->ClientCertifiedEntry(market_id, sequence, error);
}

flowmesh::ClientEventPage FlowMeshService::ClientEvents(
    const std::optional<flowmesh::ClientEventCursor>& after,
    const std::optional<flowmesh::MarketId>& market,
    const std::optional<flowmesh::AccountId>& account, const size_t limit) const
{
    std::shared_ptr<FlowMeshRuntime> runtime;
    {
        std::lock_guard lock{m_impl->mutex};
        runtime = m_impl->runtime;
    }
    if (runtime) return runtime->ClientEvents(after, market, account, limit);
    flowmesh::ClientEventPage out;
    out.gap = true;
    return out;
}

std::optional<flowmesh::ClientEvent> FlowMeshService::ClientActionStatus(
    const flowmesh::MarketId& market_id, const uint256& action_id) const
{
    std::shared_ptr<FlowMeshRuntime> runtime;
    {
        std::lock_guard lock{m_impl->mutex};
        runtime = m_impl->runtime;
    }
    return runtime ? runtime->ClientActionStatus(market_id, action_id) : std::nullopt;
}

bool FlowMeshService::SubmitLocalAction(const flowmesh::MarketId& market_id,
                                        const flowmesh::Action& action,
                                        std::string& error)
{
    if (!m_impl->RulesActiveAtTip()) {
        error = "FlowMesh service is below its activation height";
        return false;
    }
    if (!m_impl->ReconciledAtTip()) {
        error = "FlowMesh service is reconciling the B3 tip";
        return false;
    }
    std::shared_ptr<FlowMeshRuntime> runtime;
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        if (!m_impl->running || m_impl->stopping || !m_impl->runtime ||
            m_impl->chain_reconciling.load(std::memory_order_acquire)) {
            error = "FlowMesh service is not running";
            return false;
        }
        const auto it{m_impl->markets.find(market_id)};
        if (it == m_impl->markets.end() || !it->second->ready) {
            error = "FlowMesh market is not ready";
            return false;
        }
        runtime = m_impl->runtime;
    }
    const auto status{runtime->MarketStatus(market_id)};
    if (!status || m_impl->GenesisNotProduced(market_id) ||
        m_impl->GenesisCheckpointPending(market_id)) {
        error = "FlowMesh genesis checkpoint is not yet certified and connected";
        return false;
    }
    const flowmesh::QueueResult result{
        runtime->SubmitLocalAction(market_id, action)};
    if (result != flowmesh::QueueResult::ACCEPTED) {
        switch (result) {
        case flowmesh::QueueResult::MALFORMED:
            error = "FlowMesh action is malformed";
            break;
        case flowmesh::QueueResult::RATE_LIMITED:
            error = "FlowMesh action is rate limited";
            break;
        case flowmesh::QueueResult::PEER_LIMIT:
        case flowmesh::QueueResult::MARKET_LIMIT:
        case flowmesh::QueueResult::GLOBAL_LIMIT:
            error = "FlowMesh action queue is full or unavailable";
            break;
        case flowmesh::QueueResult::RECONCILING:
            error = "FlowMesh action admission is waiting for B3 reconciliation";
            break;
        case flowmesh::QueueResult::STOPPED:
            error = "FlowMesh service is stopped";
            break;
        case flowmesh::QueueResult::ACCEPTED: break;
        }
        return false;
    }
    return true;
}

FlowMeshSeatKeyStatus FlowMeshService::SeatKeyStatus() const
{
    std::lock_guard<std::mutex> lock{m_impl->mutex};
    return m_impl->SeatKeyStatusLocked();
}

bool FlowMeshService::ArmSeatKeys(std::vector<bls::SecretKey> keys,
                                  std::string& error,
                                  const std::optional<uint256>& expected_fingerprint,
                                  FlowMeshSeatKeyStatus* result)
{
    if (m_impl->transport.IndependentEnabled() &&
        m_impl->transport.network.role != "validator") {
        error = "An observer/sentry transport cannot arm FN signing keys; configure flowmeshrole=validator first";
        return false;
    }
    if (m_impl->transport.mode == "independent" && !NetworkSnapshot().running) {
        error = "Independent FlowMesh transport is unavailable; inspect getflowmeshnetworkinfo before arming";
        return false;
    }
    if (keys.empty()) {
        error = "no FlowMesh BLS seat key was supplied";
        return false;
    }
    std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) {
        return a.Bytes() < b.Bytes();
    });
    keys.erase(std::unique(keys.begin(), keys.end(),
                           [](const auto& a, const auto& b) {
                               return a.Bytes() == b.Bytes();
                           }),
               keys.end());
    // Derive public metadata before taking the runtime mutex. UI polling
    // must never repeat BLS multiplication while blocking market work.
    std::vector<std::array<unsigned char, bls::PUBKEY_SIZE>> public_keys;
    public_keys.reserve(keys.size());
    for (const auto& key : keys) public_keys.push_back(key.GetPublicKey().Compressed());
    std::sort(public_keys.begin(), public_keys.end());
    if (std::adjacent_find(public_keys.begin(), public_keys.end()) != public_keys.end()) {
        error = "distinct FlowMesh secret keys produced a duplicate public key";
        return false;
    }
    const uint256 fingerprint{FlowMeshSeatKeysFingerprint(public_keys)};
    std::shared_ptr<FlowMeshRuntime> runtime;
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        if (!m_impl->running || m_impl->stopping) {
            error = "FlowMesh service is not running";
            return false;
        }
        if (expected_fingerprint &&
            m_impl->local_key_fingerprint != *expected_fingerprint) {
            error = "FlowMesh armed keys changed; refresh and explicitly confirm the node-global operation again";
            return false;
        }
        m_impl->local_keys = std::move(keys);
        m_impl->local_public_keys = std::move(public_keys);
        m_impl->local_key_fingerprint = fingerprint;
        if (result) *result = m_impl->SeatKeyStatusLocked();
        runtime = m_impl->runtime;
    }
    if (runtime && m_impl->ReconciledAtTip()) {
        runtime->NotifyTick();
    }
    return true;
}

void FlowMeshService::DisarmSeatKeys()
{
    std::string error;
    DisarmSeatKeys(error, std::nullopt);
}

bool FlowMeshService::DisarmSeatKeys(
    std::string& error, const std::optional<uint256>& expected_fingerprint,
    FlowMeshSeatKeyStatus* result)
{
    std::lock_guard<std::mutex> lock{m_impl->mutex};
    if (expected_fingerprint &&
        m_impl->local_key_fingerprint != *expected_fingerprint) {
        error = "FlowMesh armed keys changed; refresh and explicitly confirm the node-global operation again";
        return false;
    }
    m_impl->local_keys.clear();
    m_impl->local_public_keys.clear();
    m_impl->local_key_fingerprint = FlowMeshSeatKeysFingerprint({});
    if (result) *result = m_impl->SeatKeyStatusLocked();
    return true;
}

std::optional<FlowMeshPendingCheckpoint> FlowMeshService::NextCheckpointMpa(
    const flowmesh::MarketId& market_id, std::string& error) const
{
    if (!m_impl->ReconciledAtTip()) {
        error = "FlowMesh service is not active at the current B3 tip";
        return std::nullopt;
    }
    FlowMeshProductionStore* store{nullptr};
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        const auto it{m_impl->markets.find(market_id)};
        if (!m_impl->running || m_impl->stopping ||
            it == m_impl->markets.end() || !it->second->ready ||
            !it->second->store) {
            error = "FlowMesh market is not running";
            return std::nullopt;
        }
        store = it->second->store.get();
    }
    std::optional<FlowMeshProductionStore::Marker> marker;
    if (!store->ReadMarker(marker, error) || !marker) {
        if (error.empty()) error = "FlowMesh store marker is unavailable";
        return std::nullopt;
    }
    const auto seats{m_impl->SeatSet(marker->domain, marker->market_id,
                                     marker->current_epoch,
                                     marker->current_seat_set_hash)};
    if (!seats) {
        error = "FlowMesh checkpoint seat set is unavailable";
        return std::nullopt;
    }
    std::optional<ProductionCheckpointCandidate> candidate;
    if (!store->NextCheckpointCandidate(*seats, candidate, error) ||
        !candidate) {
        return std::nullopt;
    }
    const auto checkpoint{flowmesh::BuildProductionCheckpointRecord(
        candidate->stored.entry, candidate->stored.certificate, *seats,
        candidate->previous_checkpoint_id)};
    const auto checkpoint_id{
        checkpoint ? modern::FlowMeshCheckpointIdV1(checkpoint->core)
                   : std::nullopt};
    const auto payload{
        checkpoint ? modern::EncodeFlowMeshCheckpointRecordV1(
                         *checkpoint, seats->Size())
                   : std::nullopt};
    if (!checkpoint || !checkpoint_id || !payload) {
        error = "FlowMesh checkpoint cannot be encoded";
        return std::nullopt;
    }
    return FlowMeshPendingCheckpoint{
        CMpaRecord{modern::MPA_TYPE_FLOWMESH_CHECKPOINT,
                   modern::MPA_VERSION_V1, *payload},
        *checkpoint_id, checkpoint->core.sequence,
        checkpoint->core.effect_count};
}

std::optional<FlowMeshVaultOperation> FlowMeshService::VaultOperation(
    const uint256& effect_id, std::string& error) const
{
    if (!m_impl->ReconciledAtTip()) {
        error = "FlowMesh service is not active at the current B3 tip";
        return std::nullopt;
    }
    if (effect_id.IsNull()) {
        error = "FlowMesh effect id is null";
        return std::nullopt;
    }

    struct LocalMarket {
        flowmesh::MarketId id;
        FlowMeshProductionStore* store{nullptr};
    };
    std::vector<LocalMarket> local_markets;
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        if (!m_impl->running || m_impl->stopping) {
            error = "FlowMesh service is not running";
            return std::nullopt;
        }
        for (const auto& [id, resources] : m_impl->markets) {
            if (resources->ready && resources->store) {
                local_markets.push_back({id, resources->store.get()});
            }
        }
    }

    struct ConnectedEntry {
        FlowMeshProductionStore* store{nullptr};
        FlowMeshConnectedCheckpoint checkpoint;
    };
    std::vector<ConnectedEntry> connected;
    {
        LOCK(::cs_main);
        Chainstate& chainstate{m_impl->chainman.ActiveChainstate()};
        const CBlockIndex* tip{chainstate.m_chain.Tip()};
        if (tip == nullptr ||
            !m_impl->SyncIndexesLocked(chainstate, *tip, error)) {
            if (error.empty()) error = "FlowMesh chain indexes are unavailable";
            return std::nullopt;
        }
        const FlowMeshCheckpointIndex& index{
            chainstate.ModernFlowMeshCheckpoints().Index()};
        for (const LocalMarket& local : local_markets) {
            auto cursor{index.Head(local.id)};
            std::set<modern::FlowMeshCheckpointId> seen;
            while (cursor && seen.insert(cursor->checkpoint_id).second) {
                if (cursor->core.kind ==
                        modern::FlowMeshCheckpointKind::EXECUTION &&
                    cursor->core.effect_count > 0) {
                    connected.push_back({local.store, *cursor});
                }
                if (cursor->core.previous_checkpoint_id.IsNull()) break;
                cursor = index.Get(cursor->core.previous_checkpoint_id);
            }
        }
    }

    struct FoundEffect {
        FlowMeshConnectedCheckpoint checkpoint;
        modern::FlowMeshEffectV1 effect;
        uint32_t leaf_index{0};
        std::vector<uint256> branch;
    };
    std::optional<FoundEffect> found;
    for (const ConnectedEntry& item : connected) {
        const auto& core{item.checkpoint.core};
        const auto seats{m_impl->SeatSet(core.domain, core.market_id,
                                         core.epoch, core.seat_set_hash)};
        if (!seats) continue;
        std::optional<StoredProductionEntry> stored;
        std::string read_error;
        if (!item.store->ReadEntry(core.sequence, *seats, stored,
                                   read_error) ||
            !stored || stored->entry.GetHash() != core.microblock_hash ||
            stored->effects.size() != core.effect_count) {
            continue;
        }
        for (uint32_t leaf{0}; leaf < stored->effects.size(); ++leaf) {
            if (FlowMeshEffectId(stored->effects[leaf]) != effect_id) continue;
            const auto branch{modern::BuildFlowMeshEffectBranch(
                stored->entry.effect_start, stored->effects, leaf)};
            if (!branch) {
                error = "FlowMesh effect branch cannot be reconstructed";
                return std::nullopt;
            }
            if (found) {
                error = "FlowMesh effect id occurs more than once";
                return std::nullopt;
            }
            found = FoundEffect{item.checkpoint, stored->effects[leaf], leaf,
                                *branch};
        }
    }
    if (!found) {
        error = "FlowMesh effect is not available in connected local history";
        return std::nullopt;
    }

    modern::FlowMeshVaultProofV1 proof;
    proof.kind = std::holds_alternative<modern::FlowMeshDepositAcceptanceV1>(
                     found->effect)
                     ? modern::FlowMeshVaultProofKind::DEPOSIT_SWEEP
                     : modern::FlowMeshVaultProofKind::WITHDRAWAL;
    proof.checkpoint_id = found->checkpoint.checkpoint_id;
    proof.effect = found->effect;
    proof.leaf_index = found->leaf_index;
    proof.branch = found->branch;
    const auto payload{modern::EncodeFlowMeshVaultProofV1(proof)};
    if (!payload) {
        error = "FlowMesh type-9 proof cannot be encoded";
        return std::nullopt;
    }

    std::vector<FlowMeshVaultInput> inputs;
    {
        LOCK(::cs_main);
        Chainstate& chainstate{m_impl->chainman.ActiveChainstate()};
        const CBlockIndex* tip{chainstate.m_chain.Tip()};
        if (tip == nullptr ||
            !m_impl->SyncIndexesLocked(chainstate, *tip, error)) {
            if (error.empty()) error = "FlowMesh chain indexes are unavailable";
            return std::nullopt;
        }
        const FlowMeshCheckpointIndex& checkpoints{
            chainstate.ModernFlowMeshCheckpoints().Index()};
        const auto still_connected{checkpoints.Get(proof.checkpoint_id)};
        if (!still_connected ||
            still_connected->core != found->checkpoint.core ||
            !checkpoints.VerifyVaultProof(proof, error)) {
            if (error.empty()) {
                error = "FlowMesh effect checkpoint is no longer connected";
            }
            return std::nullopt;
        }
        const auto nullifier{FlowMeshNullifierForProof(proof)};
        if (!nullifier || checkpoints.IsNullified(*nullifier)) {
            error = "FlowMesh vault effect is already nullified";
            return std::nullopt;
        }

        const FlowMeshVaultIndex& vaults{
            chainstate.ModernFlowMeshVaults().Index()};

        if (const auto* deposit{
                std::get_if<modern::FlowMeshDepositAcceptanceV1>(
                    &found->effect)}) {
            const auto record{vaults.Get(deposit->deposit_outpoint)};
            if (!record ||
                record->vault_id != deposit->vault_id ||
                record->kind != modern::VAULT_KIND_USER_DEPOSIT ||
                !record->account || *record->account != deposit->account ||
                record->asset != deposit->asset ||
                record->amount != deposit->amount ||
                record->shard != deposit->shard ||
                !AppendLiveVaultInput(chainstate, *record, inputs)) {
                error = "FlowMesh accepted deposit is no longer a live exact vault input";
                return std::nullopt;
            }
        } else {
            const auto& receipt{
                std::get<modern::FlowMeshWithdrawalReceiptV1>(found->effect)};
            const auto candidates{vaults.LargestWithdrawalInputsAt(
                receipt.vault_id, receipt.asset, *tip)};
            if (!candidates) {
                error = "FlowMesh withdrawal capacity index is unavailable";
                return std::nullopt;
            }
            util::Signed128 selected{0};
            for (const FlowMeshVaultRecord& record : *candidates) {
                if (!AppendLiveVaultInput(chainstate, record, inputs)) {
                    error = "FlowMesh selected vault input is not live";
                    return std::nullopt;
                }
                selected += record.amount;
                if (selected >= receipt.amount) break;
            }
            if (selected < receipt.amount) {
                error = "FlowMesh vault has insufficient live liquidity for the receipt";
                return std::nullopt;
            }
        }
    }

    return FlowMeshVaultOperation{
        found->checkpoint.core.market_id, found->checkpoint.checkpoint_id,
        CMpaRecord{modern::MPA_TYPE_FLOWMESH_VAULT_PROOF,
                   modern::MPA_VERSION_V1, *payload},
        found->effect, std::move(inputs)};
}

std::vector<FlowMeshVaultOperation> FlowMeshService::VaultOperations(
    const std::optional<flowmesh::MarketId> market_id,
    std::string& error) const
{
    if (!m_impl->ReconciledAtTip()) {
        error = "FlowMesh service is not active at the current B3 tip";
        return {};
    }
    if (market_id && market_id->IsNull()) {
        error = "FlowMesh market id is null";
        return {};
    }
    struct LocalMarket {
        flowmesh::MarketId id;
        FlowMeshProductionStore* store{nullptr};
    };
    std::vector<LocalMarket> local_markets;
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        if (!m_impl->running || m_impl->stopping) {
            error = "FlowMesh service is not running";
            return {};
        }
        for (const auto& [id, resources] : m_impl->markets) {
            if ((!market_id || id == *market_id) && resources->ready &&
                resources->store) {
                local_markets.push_back({id, resources->store.get()});
            }
        }
    }

    struct ConnectedEntry {
        FlowMeshProductionStore* store{nullptr};
        FlowMeshConnectedCheckpoint checkpoint;
    };
    std::vector<ConnectedEntry> connected;
    {
        LOCK(::cs_main);
        Chainstate& chainstate{m_impl->chainman.ActiveChainstate()};
        const CBlockIndex* tip{chainstate.m_chain.Tip()};
        if (tip == nullptr ||
            !m_impl->SyncIndexesLocked(chainstate, *tip, error)) {
            if (error.empty()) error = "FlowMesh chain indexes are unavailable";
            return {};
        }
        const FlowMeshCheckpointIndex& index{
            chainstate.ModernFlowMeshCheckpoints().Index()};
        for (const LocalMarket& local : local_markets) {
            auto cursor{index.Head(local.id)};
            std::set<modern::FlowMeshCheckpointId> seen;
            while (cursor && seen.insert(cursor->checkpoint_id).second) {
                if (cursor->core.kind ==
                        modern::FlowMeshCheckpointKind::EXECUTION &&
                    cursor->core.effect_count > 0) {
                    connected.push_back({local.store, *cursor});
                }
                if (cursor->core.previous_checkpoint_id.IsNull()) break;
                cursor = index.Get(cursor->core.previous_checkpoint_id);
            }
        }
    }

    std::set<std::pair<flowmesh::MarketId, uint256>> effect_ids;
    for (const ConnectedEntry& item : connected) {
        const auto& core{item.checkpoint.core};
        const auto seats{m_impl->SeatSet(core.domain, core.market_id,
                                         core.epoch, core.seat_set_hash)};
        if (!seats) continue;
        std::optional<StoredProductionEntry> stored;
        std::string read_error;
        if (!item.store->ReadEntry(core.sequence, *seats, stored,
                                   read_error) ||
            !stored || stored->entry.GetHash() != core.microblock_hash ||
            stored->effects.size() != core.effect_count) {
            continue;
        }
        for (const auto& effect : stored->effects) {
            effect_ids.emplace(core.market_id, FlowMeshEffectId(effect));
        }
    }

    std::vector<FlowMeshVaultOperation> out;
    out.reserve(effect_ids.size());
    for (const auto& [id, effect_id] : effect_ids) {
        std::string operation_error;
        auto operation{VaultOperation(effect_id, operation_error)};
        if (operation && operation->market_id == id) {
            out.push_back(std::move(*operation));
            continue;
        }
        // Listing is intentionally actionable: already-consumed effects and
        // effects whose live vault liquidity is not currently constructible
        // are omitted rather than presented as transactions the wallet can
        // relay. Structural/index errors still fail the query closed.
        if (operation_error ==
                "FlowMesh vault effect is already nullified" ||
            operation_error ==
                "FlowMesh accepted deposit is no longer a live exact vault input" ||
            operation_error ==
                "FlowMesh vault has insufficient live liquidity for the receipt" ||
            operation_error ==
                "FlowMesh withdrawal needs more than 64 vault inputs" ||
            operation_error ==
                "FlowMesh selected vault input is not live") {
            continue;
        }
        error = operation_error.empty()
                    ? "FlowMesh vault-operation discovery was inconsistent"
                    : std::move(operation_error);
        return {};
    }
    error.clear();
    return out;
}

flowmesh::QueueResult FlowMeshService::EnqueueWireMessage(
    const flowmesh::WirePeerId peer, flowmesh::WireMessage message)
{
    std::shared_ptr<FlowMeshRuntime> runtime;
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        if (!m_impl->running || m_impl->stopping) return flowmesh::QueueResult::STOPPED;
        if (m_impl->chain_reconciling.load(std::memory_order_acquire)) return flowmesh::QueueResult::RECONCILING;
        runtime = m_impl->runtime;
    }
    if (!m_impl->ReconciledAtTip()) return flowmesh::QueueResult::RECONCILING;
    return runtime ? runtime->EnqueueWireMessage(peer, std::move(message))
                   : flowmesh::QueueResult::STOPPED;
}

void FlowMeshService::FlowMeshPeerConnected(const flowmesh::WirePeerId peer)
{
    std::shared_ptr<FlowMeshRuntime> runtime;
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        runtime = m_impl->runtime;
    }
    if (runtime) runtime->FlowMeshPeerConnected(peer);
}

void FlowMeshService::FlowMeshPeerDisconnected(
    const flowmesh::WirePeerId peer)
{
    std::shared_ptr<FlowMeshRuntime> runtime;
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        runtime = m_impl->runtime;
    }
    if (runtime) runtime->FlowMeshPeerDisconnected(peer);
}

void FlowMeshService::BlockDisconnected(
    const std::shared_ptr<const CBlock>&, const CBlockIndex*)
{
    // This callback precedes UpdatedBlockTip on the validation-interface
    // queue. Stop admitting/ticking production at the earliest available
    // disconnect signal; UpdatedBlockTip performs the exact durable rollback.
    m_impl->SetChainReconciling(true);
}

void FlowMeshService::ReconcileAfterInitialBlockDownload()
{
    if (!Running() || m_impl->chainman.IsInitialBlockDownload()) return;

    const CBlockIndex* tip;
    {
        LOCK(::cs_main);
        tip = m_impl->chainman.ActiveChain().Tip();
    }
    UpdatedBlockTip(tip, nullptr, /*initial_download=*/false);
}

void FlowMeshService::UpdatedBlockTip(const CBlockIndex* new_tip,
                                      const CBlockIndex*,
                                      const bool initial_download)
{
    if (!Running()) return;
    m_impl->SetChainReconciling(true);
    if (initial_download) return;
    const uint256 callback_tip{new_tip ? new_tip->GetBlockHash() : uint256{}};
    if (!m_impl->RulesActiveAtTip()) {
        LogInfo("FlowMesh remains paused below its A3 activation height\n");
        return;
    }

    std::shared_ptr<FlowMeshRuntime> runtime;
    {
        std::lock_guard<std::mutex> lock{m_impl->mutex};
        runtime = m_impl->runtime;
    }
    ReconciliationTrace("idle_wait_started", m_impl->DeliveryGeneration(), callback_tip);
    if (runtime && !runtime->WaitForIdle(std::chrono::seconds{5})) {
        ReconciliationTrace("idle_wait_timeout", m_impl->DeliveryGeneration(), callback_tip);
        LogWarning("FlowMesh remains paused: runtime did not quiesce for B3 checkpoint reconciliation\n");
        return;
    }
    ReconciliationTrace("idle_wait_completed", m_impl->DeliveryGeneration(), callback_tip);

    std::string error;
    if (!m_impl->ReconcileAllStoreConnections(error)) {
        ReconciliationTrace("store_reconciliation_failed", m_impl->DeliveryGeneration(), callback_tip);
        LogWarning("FlowMesh remains paused: durable checkpoint reconciliation failed: %s\n",
                   error);
        return;
    }
    ReconciliationTrace("store_reconciled", m_impl->DeliveryGeneration(), callback_tip);
    error.clear();
    uint256 market_tip;
    if (!m_impl->RefreshMarkets(market_tip, error)) {
        ReconciliationTrace("market_refresh_failed", m_impl->DeliveryGeneration(), callback_tip);
        LogWarning("FlowMesh market refresh failed: %s\n", error);
        return;
    }
    ReconciliationTrace("markets_refreshed", m_impl->DeliveryGeneration(), callback_tip);
    error.clear();
    if (!m_impl->ReconcileConnectedCheckpoints(error)) {
        ReconciliationTrace("checkpoint_reconciliation_failed", m_impl->DeliveryGeneration(), callback_tip);
        LogWarning("FlowMesh remains paused: connected checkpoint reconciliation failed: %s\n",
                   error);
        return;
    }
    ReconciliationTrace("checkpoints_reconciled", m_impl->DeliveryGeneration(), callback_tip);
    error.clear();
    if (!m_impl->ReconcileAllStoreConnections(error)) {
        ReconciliationTrace("final_store_reconciliation_failed", m_impl->DeliveryGeneration(), callback_tip);
        LogWarning("FlowMesh remains paused: B3 tip changed after checkpoint connection: %s\n",
                   error);
        return;
    }
    ReconciliationTrace("final_store_reconciled", m_impl->DeliveryGeneration(), callback_tip);
    if (market_tip != callback_tip ||
        !m_impl->ReconciledTipMatches(callback_tip)) {
        LogInfo("FlowMesh remains paused: B3 tip changed during reconciliation\n");
        return;
    }
    m_impl->SetChainReconciling(false);
    if (!m_impl->ReconciledAtTip()) {
        m_impl->SetChainReconciling(true);
        LogInfo("FlowMesh remains paused: B3 tip changed before reconciliation completed\n");
        return;
    }
    if (runtime) runtime->NotifyTick();
}

} // namespace node
