// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_service.h>

#include <chain.h>
#include <consensus/era.h>
#include <flowmesh/production_wire.h>
#include <modern/asset_output.h>
#include <modern/asset_validation.h>
#include <modern/chain_domain.h>
#include <modern/flowmesh_seat.h>
#include <modern/fn.h>
#include <node/flowmesh_anchor.h>
#include <node/flowmesh_checkpoint_index.h>
#include <node/fn_seat_index.h>
#include <primitives/block.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace {

bls::SecretKey SeatKey(const unsigned char tag)
{
    std::array<unsigned char, 32> ikm{};
    ikm.fill(tag);
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key);
    return *key;
}

struct FlowMeshServiceSetup : TestingSetup {
    node::FlowMeshService service;

    FlowMeshServiceSetup()
        : TestingSetup(ChainType::MAIN),
          service{*m_node.chainman, m_args.GetDataDirNet() / "fn-operator-test"}
    {
        // Mainnet's schedule is configured, but the isolated chain is at
        // genesis: no markets, transaction broadcast, peers or signing work.
        BOOST_REQUIRE(m_node.peerman);
        std::string error;
        BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
        BOOST_REQUIRE(service.Running());
    }
};

// Exercise the real service reconciliation policy with chain-derived market
// and seat indexes. The synthetic blocks need only pass those indexes' own
// validation; no runtime chain policy or service startup hook is substituted.
struct FlowMeshServiceRestartSetup : TestingSetup {
    const modern::AssetId asset{uint256{uint8_t{0x31}}};
    uint256 domain;
    flowmesh::MarketId market;
    flowmesh::VaultId vault;
    flowmesh::ActiveFnBlsSeatSet seats;
    std::vector<bls::SecretKey> secrets;
    CBlockIndex* original_tip{nullptr};

    FlowMeshServiceRestartSetup()
        : TestingSetup(ChainType::REGTEST,
                       {.extra_args = {"-b3modernregtest", "-b3flowmeshtest",
                                       "-b3corridorlength=130"}})
    {
        auto& chainman{*m_node.chainman};
        const auto& params{chainman.GetConsensus()};
        BOOST_REQUIRE(Consensus::FlowMeshSeatBindingScheduleConfigured(params));
        domain = *modern::ModernChainDomain(params.hashGenesisBlock,
                                            *params.legacy_final_hash);
        market = *flowmesh::ComputeFlowMeshMarketId(domain, asset);
        vault = *flowmesh::ComputeFlowMeshVaultId(domain, market);
        const auto fn_asset{modern::ConfiguredFnAssetId(params)};
        BOOST_REQUIRE(fn_asset);

        CMutableTransaction bindings;
        bindings.version = 2;
        const CScript owner{CScript() << OP_DUP << OP_HASH160
                                      << std::vector<unsigned char>(20, 0x51)
                                      << OP_EQUALVERIFY << OP_CHECKSIG};
        for (uint32_t i{0}; i < 4; ++i) {
            secrets.push_back(SeatKey(20 + i));
            const auto output{modern::MakeFlowMeshSeatOutput(
                *fn_asset, owner, secrets.back().GetPublicKey())};
            BOOST_REQUIRE(output);
            bindings.vout.push_back(*output);
            bindings.mpa.push_back(modern::MakeFlowMeshSeatBindingRecord(
                i, secrets.back().SignPoP().Compressed()));
        }
        const uint256 account{uint8_t{0x41}};
        const auto deposit{modern::MakeDexVaultOutput(
            asset, 1, vault, modern::VAULT_KIND_USER_DEPOSIT,
            modern::FlowMeshUserDepositShard(vault, account), account)};
        BOOST_REQUIRE(deposit);
        CMutableTransaction bootstrap;
        bootstrap.version = 2;
        bootstrap.vout.push_back(*deposit);

        LOCK(::cs_main);
        auto& chainstate{chainman.ActiveChainstate()};
        auto& chain{chainstate.m_chain};
        original_tip = chain.Tip();
        BOOST_REQUIRE(original_tip);
        auto& seat_tracker{chainstate.ModernFnSeats()};
        auto& vault_tracker{chainstate.ModernFlowMeshVaults()};
        auto& checkpoint_tracker{chainstate.ModernFlowMeshCheckpoints()};
        BOOST_REQUIRE(seat_tracker.Sync(chain, chainman.m_blockman, params,
                                         *original_tip));
        BOOST_REQUIRE(vault_tracker.Sync(chain, chainman.m_blockman, params,
                                          *original_tip));
        BOOST_REQUIRE(checkpoint_tracker.Sync(
            chain, chainman.m_blockman, params, seat_tracker.Index(),
            vault_tracker.Index(), *original_tip));
        for (int height{1}; height <= *params.flowmesh_activation_height + 1;
             ++height) {
            CBlock block;
            block.nVersion = 2;
            block.hashPrevBlock = chain.Tip()->GetBlockHash();
            block.nTime = original_tip->nTime + height;
            block.nNonce = height;
            if (height == *params.asset_activation_height) {
                block.vtx = {MakeTransactionRef(bindings),
                             MakeTransactionRef(bootstrap)};
            }
            auto* index{chainman.m_blockman.InsertBlockIndex(block.GetHash())};
            BOOST_REQUIRE(index);
            index->nHeight = height;
            index->pprev = chain.Tip();
            index->nTime = block.nTime;
            index->BuildSkip();
            chain.SetTip(*index);
            seat_tracker.BlockConnected(block, *index, params);
            vault_tracker.BlockConnected(block, *index, params);
            checkpoint_tracker.BlockConnected(
                block, *index, chain, params, seat_tracker.Index(),
                vault_tracker.Index());
            BOOST_REQUIRE(seat_tracker.Synced(index->GetBlockHash()));
            BOOST_REQUIRE(vault_tracker.Synced(index->GetBlockHash()));
            BOOST_REQUIRE(checkpoint_tracker.Synced(index->GetBlockHash()));
        }
        const auto* anchor{chain[*params.asset_activation_height]};
        const auto snapshot{seat_tracker.Index().SnapshotAt(*anchor)};
        BOOST_REQUIRE(snapshot);
        std::vector<flowmesh::BlsSeatBinding> seat_bindings;
        for (const auto& member : snapshot->members) {
            seat_bindings.push_back({member.outpoint, member.bls_pubkey,
                                     member.proof_of_possession});
        }
        flowmesh::BlsSeatSetCheck check;
        const auto active{flowmesh::BuildActiveFnBlsSeatSet(
            domain, market, 0, anchor->nHeight, anchor->GetBlockHash(),
            seat_bindings, check)};
        BOOST_REQUIRE_MESSAGE(active, flowmesh::BlsSeatSetCheckName(check));
        seats = *active;
        std::sort(secrets.begin(), secrets.end(), [&](const auto& a, const auto& b) {
            const auto index_of = [&](const auto& key) {
                return std::find_if(seats.members.begin(), seats.members.end(),
                    [&](const auto& member) { return member.key.Key() == key.GetPublicKey(); });
            };
            return index_of(a) < index_of(b);
        });
    }

    ~FlowMeshServiceRestartSetup()
    {
        LOCK(::cs_main);
        m_node.chainman->ActiveChain().SetTip(*original_tip);
    }

    fs::path ServicePath() const
    {
        return m_args.GetDataDirNet() / "flowmesh_service_restart";
    }

    fs::path StorePath() const
    {
        return ServicePath() / fs::PathFromString(market.GetHex());
    }

    flowmesh::FlowMeshState InitialState() const
    {
        return {vault, asset, modern::NativeAsset(),
                flowmesh::FLOWMESH_V1_MAX_CURVE_POINTS};
    }

    flowmesh::ProductionEntryCore Genesis(const int anchor_offset = 0) const
    {
        const auto& params{m_node.chainman->GetConsensus()};
        const CScript treasury_script{params.modern_pos->treasury_script.begin(),
                                       params.modern_pos->treasury_script.end()};
        node::ChainAnchorPolicy anchors{*m_node.chainman,
                                         Consensus::FLOWMESH_ANCHOR_DEPTH};
        flowmesh::AnchorRef anchor;
        int height;
        {
            LOCK(::cs_main);
            height = m_node.chainman->ActiveChain().Height();
            const auto* index{m_node.chainman->ActiveChain()[
                static_cast<int>(seats.anchor_height) + anchor_offset]};
            anchor = {index->nHeight, index->GetBlockHash()};
        }
        const auto initial{InitialState()};
        flowmesh::ProductionEpochGate gate{domain, market, seats};
        flowmesh::ProductionEntryCheck check;
        const auto built{flowmesh::BuildProductionExecutionEntry(
            initial, domain, market, seats, gate, 0, 0, {}, anchor,
            {height, std::nullopt, &anchors},
            modern::AssetOwnerCommitment(treasury_script), {}, nullptr, check)};
        BOOST_REQUIRE_MESSAGE(built, flowmesh::ProductionEntryCheckName(check));
        return built->entry;
    }

    void Retain(const flowmesh::ProductionEntryCore& entry)
    {
        node::FlowMeshProductionStore store{DBParams{
            .path = StorePath(), .cache_bytes = size_t{1} << 20}};
        std::string error;
        BOOST_REQUIRE_MESSAGE(store.OpenForMarket(
            domain, market, seats, InitialState().Root(), error), error);
        BOOST_REQUIRE(store.LockCandidate(entry, {}) ==
                      flowmesh::ProductionLockResult::LOCKED);
    }

    void CheckLock(const flowmesh::ProductionEntryCore& entry,
                   const bool committed)
    {
        node::FlowMeshProductionStore store{DBParams{
            .path = StorePath(), .cache_bytes = size_t{1} << 20}};
        std::string error;
        std::optional<uint256> locked;
        BOOST_REQUIRE(store.ReadLock({0, 0}, locked, error));
        BOOST_REQUIRE(locked);
        BOOST_CHECK(*locked == entry.GetHash());
        std::optional<node::StoredLockedProductionCandidate> retained;
        BOOST_REQUIRE(store.ReadLockedCandidate({0, 0}, retained, error));
        BOOST_CHECK_EQUAL(retained.has_value(), !committed);
        if (retained) BOOST_CHECK(retained->entry.GetHash() == entry.GetHash());
        std::optional<node::FlowMeshProductionStore::Marker> marker;
        BOOST_REQUIRE(store.ReadMarker(marker, error));
        BOOST_REQUIRE(marker);
        BOOST_CHECK_EQUAL(marker->next_sequence, committed ? 1U : 0U);
        if (committed) BOOST_CHECK(marker->last_microblock_hash == entry.GetHash());
    }

    static bool WaitUntil(const std::function<bool()>& predicate)
    {
        const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{5}};
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        return false;
    }
};

} // namespace

BOOST_FIXTURE_TEST_SUITE(flowmesh_service_tests, FlowMeshServiceSetup)

BOOST_AUTO_TEST_CASE(public_snapshot_and_fingerprint_are_read_only_and_canonical)
{
    const auto empty{service.SeatKeyStatus()};
    BOOST_CHECK(empty.enabled);
    BOOST_CHECK(empty.running);
    BOOST_CHECK(empty.armed_pubkeys.empty());
    BOOST_CHECK(!empty.fingerprint.IsNull());
    BOOST_CHECK(empty.fingerprint == node::FlowMeshSeatKeysFingerprint({}));
    BOOST_CHECK(service.SeatKeyStatus().fingerprint == empty.fingerprint);
    BOOST_CHECK(service.Markets().empty());

    const auto a{SeatKey(1).GetPublicKey().Compressed()};
    const auto b{SeatKey(2).GetPublicKey().Compressed()};
    BOOST_CHECK(node::FlowMeshSeatKeysFingerprint({a, b}) ==
                node::FlowMeshSeatKeysFingerprint({b, a, a, b}));
    BOOST_CHECK(node::FlowMeshSeatKeysFingerprint({a, b}) !=
                node::FlowMeshSeatKeysFingerprint({a}));
    BOOST_CHECK(node::FlowMeshSeatKeysFingerprint({a}) != empty.fingerprint);
}

BOOST_AUTO_TEST_CASE(stale_start_and_stop_preserve_the_other_wallet_keys)
{
    const auto empty{service.SeatKeyStatus()};
    const auto a{SeatKey(3)};
    const auto b{SeatKey(4)};
    std::string error;
    node::FlowMeshSeatKeyStatus started;
    BOOST_REQUIRE(service.ArmSeatKeys({b, a, b}, error, empty.fingerprint, &started));
    BOOST_CHECK_EQUAL(started.armed_pubkeys.size(), 2U);
    BOOST_CHECK(started.fingerprint == node::FlowMeshSeatKeysFingerprint(
        {a.GetPublicKey().Compressed(), b.GetPublicKey().Compressed()}));

    node::FlowMeshSeatKeyStatus untouched{empty};
    BOOST_CHECK(!service.ArmSeatKeys({SeatKey(5)}, error, empty.fingerprint, &untouched));
    BOOST_CHECK(error.find("changed") != std::string::npos);
    BOOST_CHECK(untouched.fingerprint == empty.fingerprint);
    BOOST_CHECK(service.SeatKeyStatus().fingerprint == started.fingerprint);
    error.clear();
    BOOST_CHECK(!service.DisarmSeatKeys(error, empty.fingerprint, &untouched));
    BOOST_CHECK(error.find("changed") != std::string::npos);
    BOOST_CHECK(untouched.fingerprint == empty.fingerprint);
    BOOST_CHECK(service.SeatKeyStatus().fingerprint == started.fingerprint);

    node::FlowMeshSeatKeyStatus stopped;
    BOOST_REQUIRE(service.DisarmSeatKeys(error, started.fingerprint, &stopped));
    BOOST_CHECK(stopped.running); // Worker can run as an observer without keys.
    BOOST_CHECK(stopped.armed_pubkeys.empty());
    BOOST_CHECK(stopped.fingerprint == empty.fingerprint);
}

BOOST_AUTO_TEST_CASE(omitted_guard_retains_legacy_replacement_and_clear)
{
    std::string error;
    BOOST_REQUIRE(service.ArmSeatKeys({SeatKey(6)}, error));
    const auto replacement{SeatKey(7)};
    BOOST_REQUIRE(service.ArmSeatKeys({replacement}, error));
    BOOST_CHECK(service.SeatKeyStatus().fingerprint ==
                node::FlowMeshSeatKeysFingerprint({replacement.GetPublicKey().Compressed()}));
    service.DisarmSeatKeys();
    BOOST_CHECK(service.SeatKeyStatus().armed_pubkeys.empty());
}

BOOST_AUTO_TEST_CASE(two_concurrent_approvals_cannot_both_replace_the_same_snapshot)
{
    const auto expected{service.SeatKeyStatus().fingerprint};
    const auto a{SeatKey(8)};
    const auto b{SeatKey(9)};
    bool accepted_a{false};
    bool accepted_b{false};
    std::string error_a;
    std::string error_b;
    std::thread first{[&] { accepted_a = service.ArmSeatKeys({a}, error_a, expected); }};
    std::thread second{[&] { accepted_b = service.ArmSeatKeys({b}, error_b, expected); }};
    first.join();
    second.join();
    BOOST_CHECK(accepted_a != accepted_b);
    const auto winner{accepted_a ? a.GetPublicKey().Compressed() : b.GetPublicKey().Compressed()};
    BOOST_CHECK(service.SeatKeyStatus().fingerprint == node::FlowMeshSeatKeysFingerprint({winner}));
}

BOOST_AUTO_TEST_CASE(empty_or_stopped_service_cannot_arm_and_failure_keeps_result)
{
    std::string error;
    const auto empty{service.SeatKeyStatus()};
    node::FlowMeshSeatKeyStatus result{empty};
    BOOST_CHECK(!service.ArmSeatKeys({}, error, empty.fingerprint, &result));
    BOOST_CHECK(service.SeatKeyStatus().armed_pubkeys.empty());
    BOOST_CHECK(result.running);
    service.Stop();
    BOOST_CHECK(!service.ArmSeatKeys({SeatKey(10)}, error, empty.fingerprint, &result));
    BOOST_CHECK(!service.SeatKeyStatus().running);
    BOOST_CHECK(service.SeatKeyStatus().armed_pubkeys.empty());
    BOOST_CHECK(result.running); // Failure never fills an alleged success snapshot.
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(flowmesh_service_startup_tests, FlowMeshServiceRestartSetup)

BOOST_AUTO_TEST_CASE(retained_candidate_survives_service_start_and_resumes_exact_vote)
{
    const auto entry{Genesis()};
    Retain(entry);
    CheckLock(entry, /*committed=*/false);
    std::string error;
    {
        node::FlowMeshService service{*m_node.chainman, ServicePath()};
        // Start holds its live reconciliation gate closed while AddMarket
        // restores this journal. Previously this valid candidate aborted the
        // entire service with "cannot re-execute its retained signing candidate".
        BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
        BOOST_CHECK(service.Running());
        BOOST_REQUIRE(WaitUntil([&] {
            const auto status{service.MarketStatus(market)};
            const auto data{service.MarketData(market, std::nullopt, {}, error)};
            return status && !status->paused &&
                   status->halt == node::FlowMeshRuntimeHalt::NONE && data &&
                   data->snapshot.runtime.local_locked_candidate == entry.GetHash();
        }));
        BOOST_CHECK_EQUAL(service.MarketStatus(market)->next_sequence, 0U);
        BOOST_CHECK(service.MarketStatus(market)->observer_only);
    }
    CheckLock(entry, /*committed=*/false);

    {
        node::FlowMeshService restarted{*m_node.chainman, ServicePath()};
        BOOST_REQUIRE_MESSAGE(restarted.Start(*m_node.peerman, error), error);
        BOOST_REQUIRE_MESSAGE(restarted.ArmSeatKeys(secrets, error), error);
        // Real local proposal/attestation handling reaches quorum only for
        // the exact retained hash. Reopening must not discard its lock or
        // manufacture a replacement candidate at the newer current anchor.
        BOOST_REQUIRE(WaitUntil([&] {
            const auto status{restarted.MarketStatus(market)};
            return status && status->next_sequence == 1 &&
                   status->halt == node::FlowMeshRuntimeHalt::NONE;
        }));
        BOOST_CHECK(restarted.MarketStatus(market)->last_microblock_hash == entry.GetHash());
    }
    CheckLock(entry, /*committed=*/true);
}

BOOST_AUTO_TEST_CASE(retained_service_lock_rejects_conflicting_proposal_after_restart)
{
    const auto entry{Genesis()};
    const auto conflict{Genesis(/*anchor_offset=*/1)};
    BOOST_REQUIRE(entry.GetHash() != conflict.GetHash());
    Retain(entry);
    std::string error;
    {
        node::FlowMeshService service{*m_node.chainman, ServicePath()};
        BOOST_REQUIRE_MESSAGE(service.Start(*m_node.peerman, error), error);
        BOOST_REQUIRE(WaitUntil([&] {
            const auto status{service.MarketStatus(market)};
            return status && !status->paused &&
                   status->halt == node::FlowMeshRuntimeHalt::NONE;
        }));
        // Seat one cannot produce the round-zero proposal itself. Give it
        // a fully valid competing proposal from seat zero at the same slot.
        BOOST_REQUIRE_MESSAGE(service.ArmSeatKeys({secrets[1]}, error), error);
        flowmesh::ProductionProposalEnvelope proposal;
        proposal.entry = conflict;
        proposal.round = service.MarketStatus(market)->round;
        proposal.proposer_seat_index = flowmesh::ProductionProposerSeatIndex(
            0, proposal.round, seats.Size());
        const auto digest{flowmesh::ProductionProposalDigest(conflict, proposal.round)};
        proposal.proposer_signature = secrets[proposal.proposer_seat_index]
            .Sign(std::span<const unsigned char>{digest.begin(), 32}).Compressed();
        const auto payload{flowmesh::EncodeProductionProposalPayload(proposal)};
        BOOST_REQUIRE(payload);
        flowmesh::WireMessage wire;
        wire.kind = flowmesh::WireMessageKind::PROPOSAL;
        wire.header = {flowmesh::FLOWMESH_WIRE_VERSION_V1, market, 0, 0};
        wire.payload = *payload;
        BOOST_REQUIRE(service.EnqueueWireMessage(7, wire) == flowmesh::QueueResult::ACCEPTED);
        BOOST_REQUIRE(WaitUntil([&] {
            const auto data{service.MarketData(market, std::nullopt, {}, error)};
            return data && data->snapshot.runtime.proposals_conflicting_lock > 0;
        }));
        BOOST_CHECK(service.Running());
        BOOST_CHECK_EQUAL(service.MarketStatus(market)->next_sequence, 0U);
        BOOST_CHECK(service.MarketStatus(market)->halt == node::FlowMeshRuntimeHalt::NONE);
        const auto data{service.MarketData(market, std::nullopt, {}, error)};
        BOOST_REQUIRE(data);
        BOOST_CHECK(data->snapshot.runtime.local_locked_candidate == entry.GetHash());
        BOOST_CHECK_EQUAL(data->snapshot.runtime.max_verified_attestations, 0U);
        // The competing remote body cannot disable the worker. Once quorum
        // keys are available, the original retained candidate still completes.
        BOOST_REQUIRE_MESSAGE(service.ArmSeatKeys(secrets, error), error);
        BOOST_REQUIRE(WaitUntil([&] {
            const auto status{service.MarketStatus(market)};
            return status && status->next_sequence == 1 &&
                   status->halt == node::FlowMeshRuntimeHalt::NONE;
        }));
        BOOST_CHECK(service.MarketStatus(market)->last_microblock_hash == entry.GetHash());
    }
    CheckLock(entry, /*committed=*/true);
}

BOOST_AUTO_TEST_SUITE_END()
