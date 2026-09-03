// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chain.h>
#include <consensus/era.h>
#include <flowmesh/market.h>
#include <flowmesh/production_engine.h>
#include <modern/asset_output.h>
#include <modern/chain_domain.h>
#include <node/flowmesh_vault_index.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr int H{100};
constexpr int A1{120};
constexpr int A2{130};
constexpr int A3{A2 + Consensus::FLOWMESH_ANCHOR_DEPTH};

uint256 Filled(const unsigned char value)
{
    uint256 out;
    std::fill(out.begin(), out.end(), value);
    return out;
}

uint256 Numbered(const uint32_t number)
{
    uint256 out;
    out.begin()[0] = static_cast<unsigned char>(number);
    out.begin()[1] = static_cast<unsigned char>(number >> 8);
    out.begin()[2] = static_cast<unsigned char>(number >> 16);
    out.begin()[3] = static_cast<unsigned char>(number >> 24);
    out.begin()[31] = 0xf1;
    return out;
}

Consensus::FnGenesisRight ManifestRight()
{
    Consensus::FnGenesisRight right;
    right.pod_id.begin()[31] = 1;
    right.recipient_key_hash.fill(0x21);
    return right;
}

Consensus::Params Params()
{
    Consensus::Params params{};
    params.legacy_b3coin = true;
    params.hard_fork_height = H + 1;
    params.transition_pow_length = 10;
    params.hashGenesisBlock = Filled(0x01);
    params.legacy_final_hash = Filled(0x02);
    params.modern_pos.emplace();
    params.fn_genesis_rights_root = Filled(0x03);
    params.fn_genesis_manifest.push_back(ManifestRight());
    params.fn_pod_activation_height = A1;
    params.asset_activation_height = A2;
    params.flowmesh_activation_height = A3;
    return params;
}

uint256 Domain(const Consensus::Params& params)
{
    const auto domain{modern::ModernChainDomain(params.hashGenesisBlock,
                                                 *params.legacy_final_hash)};
    BOOST_REQUIRE(domain.has_value());
    return *domain;
}

struct MarketFixture {
    uint256 domain;
    modern::AssetId base;
    flowmesh::MarketId market;
    flowmesh::VaultId vault;
};

MarketFixture Market(const Consensus::Params& params,
                     const unsigned char base_fill = 0x31)
{
    MarketFixture out;
    out.domain = Domain(params);
    out.base = Filled(base_fill);
    const auto market{flowmesh::ComputeFlowMeshMarketId(out.domain, out.base)};
    BOOST_REQUIRE(market.has_value());
    out.market = *market;
    const auto vault{flowmesh::ComputeFlowMeshVaultId(out.domain, out.market)};
    BOOST_REQUIRE(vault.has_value());
    out.vault = *vault;
    return out;
}

CTxOut VaultOutput(const modern::AssetId& asset, const CAmount amount,
                   const flowmesh::VaultId& vault, const uint8_t kind,
                   const uint16_t shard,
                   const flowmesh::AccountId& account = {})
{
    const auto output{modern::MakeDexVaultOutput(asset, amount, vault, kind,
                                                  shard, account)};
    BOOST_REQUIRE(output.has_value());
    return *output;
}

CTxOut UserDeposit(const MarketFixture& market,
                   const modern::AssetId& asset, const CAmount amount,
                   const flowmesh::AccountId& account)
{
    return VaultOutput(asset, amount, market.vault,
                       modern::VAULT_KIND_USER_DEPOSIT,
                       modern::FlowMeshUserDepositShard(market.vault, account),
                       account);
}

CTransactionRef Tx(const std::vector<CTxOut>& outputs,
                   const std::vector<COutPoint>& spends = {},
                   const uint32_t salt = 1)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = salt;
    for (const COutPoint& outpoint : spends) tx.vin.emplace_back(outpoint);
    tx.vout = outputs;
    return MakeTransactionRef(std::move(tx));
}

CBlock Block(std::initializer_list<CTransactionRef> txs)
{
    CBlock block;
    block.vtx.assign(txs.begin(), txs.end());
    return block;
}

node::FlowMeshVaultBlockDelta Verify(
    const node::FlowMeshVaultIndex& index, const CBlock& block,
    const int height, const uint256& hash, const Consensus::Params& params)
{
    node::FlowMeshVaultBlockDelta delta;
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.VerifyBlock(block, height, hash, params, delta,
                                            error), error);
    return delta;
}

void Connect(node::FlowMeshVaultIndex& index,
             const node::FlowMeshVaultBlockDelta& delta)
{
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
}

struct SyntheticChain {
    std::vector<uint256> hashes;
    std::vector<CBlockIndex> indexes;
    CChain chain;

    explicit SyntheticChain(const int tip_height)
        : hashes(static_cast<size_t>(tip_height + 1)),
          indexes(static_cast<size_t>(tip_height + 1))
    {
        for (int height{0}; height <= tip_height; ++height) {
            hashes[height] = Filled(static_cast<unsigned char>(height + 1));
            indexes[height].nHeight = height;
            indexes[height].phashBlock = &hashes[height];
            indexes[height].pprev =
                height == 0 ? nullptr : &indexes[height - 1];
        }
        chain.SetTip(indexes.back());
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(flowmesh_vault_index_tests)

BOOST_AUTO_TEST_CASE(pre_a3_withdrawal_interval_is_provably_empty)
{
    Consensus::Params params{Params()};
    BOOST_CHECK(node::FlowMeshIntervalProvablyHasNoWithdrawals(
        A2, A3 - 1, params));
    BOOST_CHECK(!node::FlowMeshIntervalProvablyHasNoWithdrawals(
        A2, A3, params));
    BOOST_CHECK(!node::FlowMeshIntervalProvablyHasNoWithdrawals(
        A3, A3 - 1, params));
    params.flowmesh_activation_height.reset();
    BOOST_CHECK(!node::FlowMeshIntervalProvablyHasNoWithdrawals(
        A2, A3 - 1, params));
}

BOOST_AUTO_TEST_CASE(a2_preparation_requires_the_complete_schedule)
{
    Consensus::Params params{Params()};
    const MarketFixture market{Market(params)};
    const flowmesh::AccountId account{Filled(0x41)};
    const CBlock block{Block({Tx({UserDeposit(
        market, market.base, 25, account)})})};
    node::FlowMeshVaultIndex index;
    node::FlowMeshVaultBlockDelta delta;
    std::string error;

    BOOST_CHECK(!index.VerifyBlock(block, A2 - 1, Filled(0x10), params,
                                   delta, error));
    BOOST_CHECK_EQUAL(error, "FlowMesh vault history is not active");
    BOOST_CHECK(index.VerifyBlock(block, A2, Filled(0x11), params, delta,
                                  error));

    params.flowmesh_activation_height.reset();
    BOOST_CHECK(!index.VerifyBlock(block, A2, Filled(0x12), params, delta,
                                   error));
    BOOST_CHECK_EQUAL(error, "FlowMesh vault history is not active");
    params.flowmesh_activation_height = A3 - 1;
    BOOST_CHECK(!index.VerifyBlock(block, A2, Filled(0x13), params, delta,
                                   error));
}

BOOST_AUTO_TEST_CASE(strict_block_order_and_same_block_children)
{
    const Consensus::Params params{Params()};
    const MarketFixture market{Market(params)};
    const flowmesh::AccountId alice{Filled(0x42)};
    const flowmesh::AccountId bob{Filled(0x43)};
    node::FlowMeshVaultIndex index;

    const auto parent{Tx({UserDeposit(market, market.base, 100, alice)}, {},
                         10)};
    const COutPoint parent_out{parent->GetHash(), 0};
    const auto first{Verify(index, Block({parent}), A3, Filled(0x20), params)};
    BOOST_REQUIRE_EQUAL(first.markets_added.size(), 1U);
    Connect(index, first);

    const auto child{Tx({UserDeposit(market, modern::NativeAsset(), 75, bob)},
                        {parent_out}, 11)};
    const COutPoint child_out{child->GetHash(), 0};
    const auto delta{Verify(index, Block({child}), A3 + 1, Filled(0x21),
                            params)};
    BOOST_REQUIRE_EQUAL(delta.removed.size(), 1U);
    BOOST_REQUIRE_EQUAL(delta.added.size(), 1U);
    BOOST_CHECK(delta.added[0].outpoint == child_out);
    BOOST_CHECK(delta.added[0].asset == modern::NativeAsset());
    BOOST_CHECK_EQUAL(delta.added[0].amount, 75);
    BOOST_CHECK(delta.added[0].account == bob);
    BOOST_CHECK_EQUAL(delta.added[0].kind,
                      modern::VAULT_KIND_USER_DEPOSIT);
    BOOST_CHECK_EQUAL(delta.added[0].shard,
                      modern::FlowMeshUserDepositShard(market.vault, bob));
    Connect(index, delta);
    BOOST_CHECK(!index.Get(parent_out));
    BOOST_CHECK(index.Get(child_out).has_value());
    BOOST_CHECK(index.Market(market.market).has_value());

    // Market creation is not durable until its first colored deposit survives
    // the establishing block boundary.
    node::FlowMeshVaultIndex transient;
    node::FlowMeshVaultBlockDelta rejected;
    std::string error;
    BOOST_CHECK(!transient.VerifyBlock(Block({parent, child}), A3,
                                       Filled(0x2f), params, rejected, error));
    BOOST_CHECK_EQUAL(
        error,
        "FlowMesh market-establishing deposit is not live at the block boundary");

    // An earlier transaction releases the existing output before a later
    // vault creation becomes visible in this same block.
    const auto pool{Tx({VaultOutput(
        market.base, 60, market.vault, modern::VAULT_KIND_POOL_CHANGE, 7)},
                       {child_out}, 12)};
    const auto ordered{Verify(index, Block({pool}), A3 + 2, Filled(0x22),
                              params)};
    BOOST_REQUIRE_EQUAL(ordered.removed.size(), 1U);
    BOOST_REQUIRE_EQUAL(ordered.added.size(), 1U);
    BOOST_CHECK(ordered.removed[0].outpoint == child_out);
    BOOST_CHECK_EQUAL(ordered.added[0].kind,
                      modern::VAULT_KIND_POOL_CHANGE);
    Connect(index, ordered);
}

BOOST_AUTO_TEST_CASE(as_of_anchor_tracks_spends_empty_blocks_and_forks)
{
    const Consensus::Params params{Params()};
    const MarketFixture market{Market(params)};
    const flowmesh::AccountId account{Filled(0x44)};
    SyntheticChain chain{A3 + 2};
    node::FlowMeshVaultIndex index;

    const auto create{Tx({UserDeposit(market, market.base, 90, account)}, {},
                         20)};
    const COutPoint deposit{create->GetHash(), 0};
    const auto spend{Tx({CTxOut{0, CScript() << OP_TRUE}}, {deposit}, 21)};
    Connect(index, Verify(index, Block({create}), A3, chain.hashes[A3],
                          params));
    Connect(index, Verify(index, CBlock{}, A3 + 1, chain.hashes[A3 + 1],
                          params));
    Connect(index, Verify(index, Block({spend}), A3 + 2,
                          chain.hashes[A3 + 2], params));

    BOOST_CHECK(!index.Get(deposit));
    BOOST_CHECK(index.LookupAt(deposit, chain.indexes[A3]).has_value());
    BOOST_CHECK(index.LookupAt(deposit, chain.indexes[A3 + 1]).has_value());
    BOOST_CHECK(!index.LookupAt(deposit, chain.indexes[A3 + 2]));

    uint256 side_hash{Filled(0xee)};
    CBlockIndex side;
    side.nHeight = A3 + 1;
    side.phashBlock = &side_hash;
    side.pprev = &chain.indexes[A3];
    BOOST_CHECK(!index.LookupAt(deposit, side));

    // Exact undo, including the empty block, restores the prior states; a
    // clean replay produces byte-identical immutable history.
    const auto complete_history{index.History()};
    node::FlowMeshVaultIndex replay;
    Connect(replay, Verify(replay, Block({create}), A3, chain.hashes[A3],
                           params));
    Connect(replay, Verify(replay, CBlock{}, A3 + 1,
                           chain.hashes[A3 + 1], params));
    Connect(replay, Verify(replay, Block({spend}), A3 + 2,
                           chain.hashes[A3 + 2], params));
    BOOST_CHECK(replay.History() == complete_history);
    BOOST_CHECK(replay.All() == index.All());

    std::string error;
    BOOST_REQUIRE(index.DisconnectBlock(A3 + 2, chain.hashes[A3 + 2],
                                        error));
    BOOST_CHECK(index.Get(deposit).has_value());
    const auto before_empty{index.All()};
    BOOST_REQUIRE(index.DisconnectBlock(A3 + 1, chain.hashes[A3 + 1],
                                        error));
    BOOST_CHECK(index.All() == before_empty);
    BOOST_REQUIRE(index.DisconnectBlock(A3, chain.hashes[A3], error));
    BOOST_CHECK(index.All().empty());
    BOOST_CHECK(index.History().empty());
}

BOOST_AUTO_TEST_CASE(withdrawal_capacity_is_top64_anchor_exact_and_reorg_safe)
{
    const Consensus::Params params{Params()};
    const MarketFixture market{Market(params)};
    SyntheticChain chain{A2 + 3};
    node::FlowMeshVaultIndex index;

    node::FlowMeshVaultBlockDelta fragmented;
    // Pool custody is indexed during the A2 preparation runway. The first
    // A3 production slots anchor into this range, so capacity must be usable
    // without waiting an unintended additional 30 blocks.
    fragmented.height = A2;
    fragmented.block_hash = chain.hashes[A2];
    for (uint32_t i{0}; i < 65; ++i) {
        fragmented.added.push_back(node::FlowMeshVaultRecord{
            COutPoint{Txid::FromUint256(Numbered(i + 1)), 0}, market.base, 1,
            market.vault, modern::VAULT_KIND_POOL_CHANGE,
            static_cast<uint16_t>(i), std::nullopt, A2,
            chain.hashes[A2]});
    }
    Connect(index, fragmented);

    const auto fragmented_inputs{index.LargestWithdrawalInputsAt(
        market.vault, market.base, chain.indexes[A2])};
    BOOST_REQUIRE(fragmented_inputs.has_value());
    BOOST_REQUIRE_EQUAL(
        fragmented_inputs->size(),
        flowmesh::FLOWMESH_MAX_WITHDRAWAL_VAULT_INPUTS);
    BOOST_CHECK_EQUAL(*index.WithdrawalCapacityAt(
                          market.vault, market.base, chain.indexes[A2]),
                      64);

    // The high-value output is deliberately introduced after every dust
    // outpoint. Amount-first selection must still put it first; the old
    // outpoint-first publisher would exhaust 64 dust inputs before seeing it.
    node::FlowMeshVaultBlockDelta large;
    large.height = A2 + 1;
    large.block_hash = chain.hashes[A2 + 1];
    const node::FlowMeshVaultRecord large_record{
        COutPoint{Txid::FromUint256(Numbered(1000)), 0}, market.base, 100,
        market.vault, modern::VAULT_KIND_POOL_CHANGE, 200, std::nullopt,
        A2 + 1, chain.hashes[A2 + 1]};
    large.added.push_back(large_record);
    Connect(index, large);
    BOOST_REQUIRE_EQUAL(index.All().begin()->second.amount, 1);

    const auto ordered{index.LargestWithdrawalInputsAt(
        market.vault, market.base, chain.indexes[A2 + 1])};
    BOOST_REQUIRE(ordered.has_value());
    BOOST_REQUIRE_EQUAL(
        ordered->size(), flowmesh::FLOWMESH_MAX_WITHDRAWAL_VAULT_INPUTS);
    BOOST_CHECK(ordered->front() == large_record);
    BOOST_CHECK_EQUAL(*index.WithdrawalCapacityAt(
                          market.vault, market.base,
                          chain.indexes[A2 + 1]),
                      163);
    // The prior anchor remains byte-for-byte independent of later changes.
    BOOST_CHECK_EQUAL(*index.WithdrawalCapacityAt(
                          market.vault, market.base, chain.indexes[A2]),
                      64);

    uint256 side_hash{Filled(0xee)};
    CBlockIndex side;
    side.nHeight = A2 + 1;
    side.phashBlock = &side_hash;
    side.pprev = &chain.indexes[A2];
    BOOST_CHECK(!index.WithdrawalCapacityAt(market.vault, market.base, side));

    // Two already-certified receipts total the admitted capacity: 60 + 103.
    // Publishing the first spends only the largest (100) input and returns
    // deterministic change 40. Even while its FlowMesh liability awaits the
    // anchor-derived settlement entry, the second receipt remains exactly
    // constructible from at most 64 current pool inputs.
    const uint256 first_receipt{Numbered(2000)};
    const node::FlowMeshVaultRecord change_record{
        COutPoint{Txid::FromUint256(Numbered(1001)), 0}, market.base, 40,
        market.vault, modern::VAULT_KIND_POOL_CHANGE,
        flowmesh::ComputeProductionWithdrawalChangeShard(market.vault,
                                                          first_receipt),
        std::nullopt, A2 + 2, chain.hashes[A2 + 2]};
    node::FlowMeshVaultBlockDelta spent;
    spent.height = A2 + 2;
    spent.block_hash = chain.hashes[A2 + 2];
    spent.removed.push_back(large_record);
    spent.added.push_back(change_record);
    Connect(index, spent);
    BOOST_CHECK_EQUAL(*index.WithdrawalCapacityAt(
                          market.vault, market.base,
                          chain.indexes[A2 + 2]),
                      103);
    const auto second_inputs{index.LargestWithdrawalInputsAt(
        market.vault, market.base, chain.indexes[A2 + 2])};
    BOOST_REQUIRE(second_inputs.has_value());
    CAmount second_total{0};
    size_t second_count{0};
    for (const node::FlowMeshVaultRecord& record : *second_inputs) {
        second_total += record.amount;
        ++second_count;
        if (second_total >= 103) break;
    }
    BOOST_CHECK_EQUAL(second_count,
                      flowmesh::FLOWMESH_MAX_WITHDRAWAL_VAULT_INPUTS);
    BOOST_CHECK_EQUAL(second_total, 103);
    BOOST_CHECK(second_inputs->front() == change_record);

    node::FlowMeshVaultBlockDelta second_spent;
    second_spent.height = A2 + 3;
    second_spent.block_hash = chain.hashes[A2 + 3];
    second_spent.removed.assign(second_inputs->begin(),
                                second_inputs->begin() + second_count);
    Connect(index, second_spent);
    BOOST_CHECK_EQUAL(*index.WithdrawalCapacityAt(
                          market.vault, market.base,
                          chain.indexes[A2 + 3]),
                      2);
    // Historical reconstruction recovers the later-spent large output.
    BOOST_CHECK_EQUAL(*index.WithdrawalCapacityAt(
                          market.vault, market.base,
                          chain.indexes[A2 + 1]),
                      163);

    std::string error;
    BOOST_REQUIRE(index.DisconnectBlock(A2 + 3, chain.hashes[A2 + 3],
                                        error));
    const auto second_replayed{index.LargestWithdrawalInputsAt(
        market.vault, market.base, chain.indexes[A2 + 2])};
    BOOST_REQUIRE(second_replayed.has_value());
    BOOST_CHECK(*second_replayed == *second_inputs);
    BOOST_REQUIRE(index.DisconnectBlock(A2 + 2, chain.hashes[A2 + 2],
                                        error));
    const auto replayed{index.LargestWithdrawalInputsAt(
        market.vault, market.base, chain.indexes[A2 + 1])};
    BOOST_REQUIRE(replayed.has_value());
    BOOST_CHECK(*replayed == *ordered);
}

BOOST_AUTO_TEST_CASE(deposit_resolution_derives_every_fact_from_chain_history)
{
    const Consensus::Params params{Params()};
    const MarketFixture market{Market(params)};
    const MarketFixture other_market{Market(params, 0x32)};
    const flowmesh::AccountId alice{Filled(0x45)};
    const flowmesh::AccountId bob{Filled(0x46)};
    SyntheticChain chain{A3 + 1};
    node::FlowMeshVaultIndex index;

    const auto outputs{Tx({
        UserDeposit(market, market.base, 101, alice),
        UserDeposit(market, modern::NativeAsset(), 202, bob),
        VaultOutput(market.base, 303, market.vault,
                    modern::VAULT_KIND_POOL_CHANGE, 9),
        UserDeposit(other_market, other_market.base, 404, alice),
    }, {}, 30)};
    Connect(index, Verify(index, Block({outputs}), A2, chain.hashes[A2],
                          params));
    for (int height{A2 + 1}; height <= A3 + 1; ++height) {
        Connect(index, Verify(index, CBlock{}, height, chain.hashes[height],
                              params));
    }

    const auto colored{index.ResolveDepositAt(
        COutPoint{outputs->GetHash(), 0}, chain.indexes[A3 + 1], market.domain,
        market.base, market.market, A2, A3)};
    BOOST_REQUIRE(colored.has_value());
    BOOST_CHECK(colored->asset == market.base);
    BOOST_CHECK_EQUAL(colored->amount, 101);
    BOOST_CHECK(colored->account == alice);

    // A2 is deliberately a preparation runway: deposits are resolvable from
    // their exact pre-A3 anchor, although trading remains disabled until A3.
    const auto prepared{index.ResolveDepositAt(
        COutPoint{outputs->GetHash(), 0}, chain.indexes[A2], market.domain,
        market.base, market.market, A2, A3)};
    BOOST_REQUIRE(prepared.has_value());
    BOOST_CHECK_EQUAL(prepared->amount, 101);

    const auto native{index.ResolveDepositAt(
        COutPoint{outputs->GetHash(), 1}, chain.indexes[A3 + 1], market.domain,
        market.base, market.market, A2, A3)};
    BOOST_REQUIRE(native.has_value());
    BOOST_CHECK(native->asset == modern::NativeAsset());
    BOOST_CHECK_EQUAL(native->amount, 202);
    BOOST_CHECK(native->account == bob);

    // The verifier is market-bound, recognizes USER_DEPOSIT only, and never
    // accepts an unrelated chain asset merely because it uses a vault carrier.
    BOOST_CHECK(!index.ResolveDepositAt(
        COutPoint{outputs->GetHash(), 0}, chain.indexes[A3 + 1], market.domain,
        market.base, other_market.market, A2, A3));
    BOOST_CHECK(!index.ResolveDepositAt(
        COutPoint{outputs->GetHash(), 2}, chain.indexes[A3 + 1], market.domain,
        market.base, market.market, A2, A3));
    BOOST_CHECK(!index.ResolveDepositAt(
        COutPoint{outputs->GetHash(), 3}, chain.indexes[A3 + 1], market.domain,
        market.base, market.market, A2, A3));
    BOOST_CHECK(!index.ResolveDepositAt(
        COutPoint{outputs->GetHash(), 0}, chain.indexes[A2 - 1], market.domain,
        market.base, market.market, A2, A3));

    node::FlowMeshVaultBlockDelta rejected;
    std::string error;
    const auto wrong_asset{Tx(
        {UserDeposit(market, Filled(0x77), 505, alice)}, {}, 31)};
    BOOST_CHECK(!index.VerifyBlock(Block({wrong_asset}), A3 + 2,
                                   Filled(0x78), params, rejected, error));
    BOOST_CHECK_EQUAL(
        error, "colored FlowMesh deposit names the wrong vault");
}

BOOST_AUTO_TEST_CASE(deposit_resolution_rejects_noncanonical_or_malformed_records)
{
    const Consensus::Params params{Params()};
    const MarketFixture market{Market(params)};
    const flowmesh::AccountId account{Filled(0x47)};
    SyntheticChain chain{A3};

    auto make_record = [&](const uint32_t n) {
        node::FlowMeshVaultRecord record;
        record.outpoint = COutPoint{Txid::FromUint256(Filled(0x80)), n};
        record.asset = market.base;
        record.amount = 12;
        record.vault_id = market.vault;
        record.kind = modern::VAULT_KIND_USER_DEPOSIT;
        record.shard = modern::FlowMeshUserDepositShard(market.vault, account);
        record.account = account;
        record.created_height = A3;
        record.created_block = chain.hashes[A3];
        return record;
    };
    auto resolves = [&](node::FlowMeshVaultRecord record,
                        const uint256& delta_hash = uint256{}) {
        node::FlowMeshVaultIndex index;
        node::FlowMeshVaultBlockDelta delta;
        delta.height = A3;
        delta.block_hash = delta_hash.IsNull() ? chain.hashes[A3] : delta_hash;
        delta.added.push_back(record);
        delta.markets_added.push_back(node::FlowMeshMarketRecord{
            market.base, market.market, market.vault, record.outpoint,
            record.created_height, record.created_block});
        std::string error;
        if (!index.ConnectBlock(delta, error)) return false;
        return index.ResolveDepositAt(record.outpoint, chain.indexes[A3],
                                      market.domain, market.base,
                                      market.market, A2, A3).has_value();
    };

    BOOST_CHECK(resolves(make_record(0)));
    auto bad{make_record(1)};
    bad.shard ^= 1;
    BOOST_CHECK(!resolves(bad));
    bad = make_record(2);
    bad.amount = 0;
    BOOST_CHECK(!resolves(bad));
    bad = make_record(3);
    bad.account.reset();
    BOOST_CHECK(!resolves(bad));
    bad = make_record(4);
    bad.created_height = A3 - 1;
    BOOST_CHECK(!resolves(bad));
    bad = make_record(5);
    bad.created_block = Filled(0x99);
    BOOST_CHECK(!resolves(bad));

    // An otherwise plausible record in a side-block delta is not canonical
    // for the requested active anchor.
    bad = make_record(6);
    bad.created_block = Filled(0x98);
    BOOST_CHECK(!resolves(bad, Filled(0x98)));
}

BOOST_AUTO_TEST_SUITE_END()
