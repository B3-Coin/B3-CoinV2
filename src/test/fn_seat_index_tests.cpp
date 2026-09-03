// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <chain.h>
#include <consensus/era.h>
#include <crypto/bls.h>
#include <flowmesh/seat_id.h>
#include <modern/asset_output.h>
#include <modern/asset_validation.h>
#include <modern/chain_domain.h>
#include <modern/flowmesh_seat.h>
#include <modern/fn.h>
#include <node/fn_seat_index.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <uint256.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
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

bls::SecretKey Key(const uint32_t id)
{
    std::array<unsigned char, 32> ikm{};
    ikm[0] = static_cast<unsigned char>(id);
    ikm[15] = static_cast<unsigned char>(id >> 8);
    ikm[31] = 0x42;
    const auto key{bls::SecretKey::FromIKM(ikm)};
    BOOST_REQUIRE(key.has_value());
    return *key;
}

CScript OwnerScript(const unsigned char fill)
{
    return CScript() << OP_DUP << OP_HASH160
                     << std::vector<unsigned char>(20, fill)
                     << OP_EQUALVERIFY << OP_CHECKSIG;
}

modern::AssetId FnAsset(const Consensus::Params& params)
{
    const auto asset{modern::ConfiguredFnAssetId(params)};
    BOOST_REQUIRE(asset.has_value());
    return *asset;
}

CTxOut SeatOutput(const Consensus::Params& params, const bls::SecretKey& key,
                  const unsigned char owner)
{
    const auto out{modern::MakeFlowMeshSeatOutput(
        FnAsset(params), OwnerScript(owner), key.GetPublicKey())};
    BOOST_REQUIRE(out.has_value());
    return *out;
}

CTxOut FnV1Output(const Consensus::Params& params,
                  const unsigned char owner = 0x71)
{
    const auto out{modern::MakeAssetOwnerOutput(
        FnAsset(params), 1, modern::PolicyType::FN, OwnerScript(owner))};
    BOOST_REQUIRE(out.has_value());
    return *out;
}

CTransactionRef SeatTx(const Consensus::Params& params,
                       const std::vector<bls::SecretKey>& keys,
                       const std::vector<COutPoint>& spends = {},
                       const uint32_t salt = 1)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = salt;
    for (const COutPoint& outpoint : spends) tx.vin.emplace_back(outpoint);
    for (size_t i{0}; i < keys.size(); ++i) {
        tx.vout.push_back(SeatOutput(
            params, keys[i], static_cast<unsigned char>(0x30 + (salt + i) % 90)));
        const auto pop{keys[i].SignPoP().Compressed()};
        tx.mpa.push_back(modern::MakeFlowMeshSeatBindingRecord(
            static_cast<uint32_t>(i), pop));
    }
    return MakeTransactionRef(std::move(tx));
}

CTransactionRef EndSeatTx(const Consensus::Params& params,
                          const COutPoint& spend, const uint32_t salt)
{
    CMutableTransaction tx;
    tx.version = 2;
    tx.nLockTime = salt;
    tx.vin.emplace_back(spend);
    tx.vout.push_back(FnV1Output(params, static_cast<unsigned char>(salt)));
    return MakeTransactionRef(std::move(tx));
}

CBlock Block(std::initializer_list<CTransactionRef> txs)
{
    CBlock block;
    block.vtx.assign(txs.begin(), txs.end());
    return block;
}

node::FnSeatBlockDelta Verify(const node::FnSeatIndex& index,
                              const CBlock& block, const int height,
                              const uint256& hash,
                              const Consensus::Params& params)
{
    node::FnSeatBlockDelta delta;
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.VerifyBlock(block, height, hash, params, delta,
                                            error), error);
    return delta;
}

void Connect(node::FnSeatIndex& index, const node::FnSeatBlockDelta& delta)
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
            indexes[height].pprev = height == 0 ? nullptr : &indexes[height - 1];
        }
        chain.SetTip(indexes.back());
    }
};

} // namespace

BOOST_AUTO_TEST_SUITE(fn_seat_index_tests)

BOOST_AUTO_TEST_CASE(a2_activation_requires_the_complete_a3_runway)
{
    const COutPoint vector_outpoint{
        Txid::FromUint256(Filled(0x22)), 0x01020304U};
    BOOST_CHECK_EQUAL(
        flowmesh::ComputeFlowMeshSeatId(Filled(0x11), vector_outpoint).GetHex(),
        "8cd2932f7479a936d0e8bd28f5bc1deb36ad1e667759b011a9c447e42ff182f4");
    Consensus::Params params{Params()};
    const auto tx{SeatTx(params, {Key(1)})};
    const CBlock block{Block({tx})};
    node::FnSeatIndex index;
    node::FnSeatBlockDelta delta;
    std::string error;

    BOOST_CHECK(!index.VerifyBlock(block, A2 - 1, Filled(0x10), params,
                                   delta, error));
    BOOST_CHECK_EQUAL(error, "FlowMesh FN-seat index is not active");
    BOOST_CHECK(index.VerifyBlock(block, A2, Filled(0x11), params, delta,
                                  error));

    params.flowmesh_activation_height = A3 - 1;
    BOOST_CHECK(!index.VerifyBlock(block, A2, Filled(0x12), params, delta,
                                   error));
    BOOST_CHECK_EQUAL(error, "FlowMesh FN-seat schedule is unavailable");
}

BOOST_AUTO_TEST_CASE(duplicate_keys_and_atomic_rotations_follow_block_order)
{
    const Consensus::Params params{Params()};
    const bls::SecretKey a{Key(2)};
    const bls::SecretKey b{Key(3)};
    node::FnSeatIndex index;

    // Two outputs in one transaction cannot claim the same live key.
    {
        const auto duplicate{SeatTx(params, {a, a}, {}, 2)};
        node::FnSeatBlockDelta delta;
        std::string error;
        BOOST_CHECK(!index.VerifyBlock(Block({duplicate}), A2, Filled(0x20),
                                       params, delta, error));
        BOOST_CHECK_EQUAL(error, "duplicate live FlowMesh FN-seat BLS key");
    }

    const auto genesis{SeatTx(params, {a, b}, {}, 3)};
    Connect(index, Verify(index, Block({genesis}), A2, Filled(0x21), params));
    const COutPoint a_old{genesis->GetHash(), 0};
    const COutPoint b_old{genesis->GetHash(), 1};
    BOOST_REQUIRE_EQUAL(index.Size(), 2U);

    // A parent-state duplicate remains invalid until its owning outpoint is
    // actually spent.
    {
        const auto duplicate{SeatTx(params, {a}, {}, 4)};
        node::FnSeatBlockDelta delta;
        std::string error;
        BOOST_CHECK(!index.VerifyBlock(Block({duplicate}), A2 + 1,
                                       Filled(0x22), params, delta, error));
        BOOST_CHECK_EQUAL(error, "duplicate live FlowMesh FN-seat BLS key");
    }

    // Inputs release first, so both a same-key rotation and a two-key swap are
    // atomic and valid.
    const auto rotate{SeatTx(params, {a}, {a_old}, 5)};
    Connect(index, Verify(index, Block({rotate}), A2 + 1, Filled(0x23), params));
    const COutPoint a_rotated{rotate->GetHash(), 0};
    BOOST_CHECK(index.OwnerOf(a.GetPublicKey().Compressed()) == a_rotated);

    const auto swap{SeatTx(params, {b, a}, {a_rotated, b_old}, 6)};
    Connect(index, Verify(index, Block({swap}), A2 + 2, Filled(0x24), params));
    const COutPoint b_swapped{swap->GetHash(), 0};
    const COutPoint a_swapped{swap->GetHash(), 1};
    BOOST_CHECK(index.OwnerOf(b.GetPublicKey().Compressed()) == b_swapped);
    BOOST_CHECK(index.OwnerOf(a.GetPublicKey().Compressed()) == a_swapped);
}

BOOST_AUTO_TEST_CASE(block_order_release_claim_and_same_block_child)
{
    const Consensus::Params params{Params()};
    const bls::SecretKey key{Key(4)};

    // A prior transaction may release the key for a later transaction.
    node::FnSeatIndex ordered;
    const auto initial{SeatTx(params, {key}, {}, 7)};
    Connect(ordered, Verify(ordered, Block({initial}), A2, Filled(0x30), params));
    const COutPoint old{initial->GetHash(), 0};
    const auto release{EndSeatTx(params, old, 8)};
    const auto claim{SeatTx(params, {key}, {}, 9)};
    Connect(ordered, Verify(ordered, Block({release, claim}), A2 + 1,
                            Filled(0x31), params));
    const COutPoint claimed{claim->GetHash(), 0};
    BOOST_CHECK(ordered.OwnerOf(key.GetPublicKey().Compressed()) == claimed);

    // A future release cannot retroactively rescue an earlier duplicate.
    node::FnSeatIndex reversed;
    Connect(reversed, Verify(reversed, Block({initial}), A2, Filled(0x32), params));
    node::FnSeatBlockDelta rejected;
    std::string error;
    BOOST_CHECK(!reversed.VerifyBlock(Block({claim, release}), A2 + 1,
                                      Filled(0x33), params, rejected, error));
    BOOST_CHECK_EQUAL(error, "duplicate live FlowMesh FN-seat BLS key");

    // A child sees and can spend a parent created earlier in the same block;
    // the temporary parent cancels from the durable delta.
    node::FnSeatIndex child_index;
    const auto parent{SeatTx(params, {key}, {}, 10)};
    const auto child{SeatTx(params, {key},
                            {COutPoint{parent->GetHash(), 0}}, 11)};
    const auto delta{Verify(child_index, Block({parent, child}), A2,
                            Filled(0x34), params)};
    BOOST_CHECK(delta.removed.empty());
    BOOST_REQUIRE_EQUAL(delta.added.size(), 1U);
    BOOST_CHECK(delta.added[0].outpoint == COutPoint(child->GetHash(), 0));
    Connect(child_index, delta);
    BOOST_CHECK_EQUAL(child_index.Size(), 1U);
}

BOOST_AUTO_TEST_CASE(fn_v1_end_empty_block_undo_and_replay_are_exact)
{
    const Consensus::Params params{Params()};
    const bls::SecretKey key{Key(5)};
    const auto create{SeatTx(params, {key}, {}, 12)};
    const auto end{EndSeatTx(params, COutPoint{create->GetHash(), 0}, 13)};
    const CBlock create_block{Block({create})};
    const CBlock empty_block{};
    const CBlock end_block{Block({end})};
    const uint256 h0{Filled(0x40)};
    const uint256 h1{Filled(0x41)};
    const uint256 h2{Filled(0x42)};

    node::FnSeatIndex incremental;
    const auto d0{Verify(incremental, create_block, A2, h0, params)};
    Connect(incremental, d0);
    const auto d1{Verify(incremental, empty_block, A2 + 1, h1, params)};
    BOOST_CHECK(d1.added.empty() && d1.removed.empty());
    Connect(incremental, d1);
    const auto d2{Verify(incremental, end_block, A2 + 2, h2, params)};
    Connect(incremental, d2);
    BOOST_CHECK(incremental.All().empty());
    BOOST_REQUIRE_EQUAL(incremental.History().size(), 3U);

    // Clean replay derives byte-identical state and deltas.
    node::FnSeatIndex replay;
    Connect(replay, Verify(replay, create_block, A2, h0, params));
    Connect(replay, Verify(replay, empty_block, A2 + 1, h1, params));
    Connect(replay, Verify(replay, end_block, A2 + 2, h2, params));
    BOOST_CHECK(replay.All() == incremental.All());
    BOOST_CHECK(replay.History() == incremental.History());

    std::string error;
    BOOST_REQUIRE(incremental.DisconnectBlock(A2 + 2, h2, error));
    BOOST_CHECK_EQUAL(incremental.Size(), 1U);
    const auto before_empty{incremental.All()};
    BOOST_REQUIRE(incremental.DisconnectBlock(A2 + 1, h1, error));
    BOOST_CHECK(incremental.All() == before_empty);
    BOOST_REQUIRE(incremental.DisconnectBlock(A2, h0, error));
    BOOST_CHECK(incremental.All().empty());
    BOOST_CHECK(incremental.History().empty());
}

BOOST_AUTO_TEST_CASE(anchor_snapshot_and_set_hash_bind_every_field)
{
    const Consensus::Params params{Params()};
    const uint256 domain{Domain(params)};
    const uint256 market{Filled(0x55)};
    std::vector<bls::SecretKey> keys;
    for (uint32_t i{0}; i < 5; ++i) keys.push_back(Key(20 + i));
    const auto seats{SeatTx(params, keys, {}, 20)};

    SyntheticChain chain{A3 - 1};
    node::FnSeatIndex index;
    Connect(index, Verify(index, Block({seats}), A2,
                          chain.hashes[A2], params));
    for (int height{A2 + 1}; height < A3; ++height) {
        Connect(index, Verify(index, CBlock{}, height, chain.hashes[height],
                              params));
    }

    std::string error;
    const auto snapshot{index.AnchoredSnapshot(
        chain.chain, chain.indexes[A2], A3, params, error)};
    BOOST_REQUIRE_MESSAGE(snapshot.has_value(), error);
    BOOST_CHECK(snapshot->FlowMeshReady());
    BOOST_REQUIRE_EQUAL(snapshot->members.size(), 5U);
    for (size_t i{1}; i < snapshot->members.size(); ++i) {
        BOOST_CHECK(snapshot->members[i - 1].seat_id < snapshot->members[i].seat_id ||
                    (snapshot->members[i - 1].seat_id == snapshot->members[i].seat_id &&
                     snapshot->members[i - 1].outpoint < snapshot->members[i].outpoint));
    }

    // A3's first eligible snapshot is exactly A3-30 == A2.
    BOOST_CHECK_EQUAL(snapshot->anchor_height, A2);
    BOOST_CHECK(snapshot->anchor_hash == chain.hashes[A2]);
    BOOST_CHECK(!index.AnchoredSnapshot(chain.chain, chain.indexes[A2 + 1],
                                        A3, params, error));
    BOOST_CHECK_EQUAL(error, "FlowMesh seat anchor is too shallow");
    uint256 side_hash{Filled(0x99)};
    CBlockIndex side;
    side.nHeight = A2;
    side.phashBlock = &side_hash;
    side.pprev = &chain.indexes[A2 - 1];
    BOOST_CHECK(!index.AnchoredSnapshot(chain.chain, side, A3, params, error));
    BOOST_CHECK_EQUAL(error, "FlowMesh seat anchor is not on the active chain");

    const auto base{snapshot->SetHash(domain, market, 9)};
    BOOST_REQUIRE(base.has_value());
    BOOST_CHECK_EQUAL(
        base->GetHex(),
        "cc249a85463cc584ea13b16f1494c20b4eaa8d11a31dda8c3e0ed5402a859652");
    BOOST_CHECK(snapshot->SetHash(domain, Filled(0x56), 9) != base);
    BOOST_CHECK(snapshot->SetHash(domain, market, 10) != base);

    auto changed_anchor{*snapshot};
    ++changed_anchor.anchor_height;
    BOOST_CHECK(changed_anchor.SetHash(domain, market, 9) != base);
    changed_anchor = *snapshot;
    changed_anchor.anchor_hash = Filled(0x57);
    BOOST_CHECK(changed_anchor.SetHash(domain, market, 9) != base);

    auto changed_count{*snapshot};
    changed_count.members.pop_back(); // count 5->4 also changes threshold 4->3
    BOOST_CHECK_EQUAL(flowmesh::FlowMeshSeatSetThreshold(5), 4U);
    BOOST_CHECK_EQUAL(flowmesh::FlowMeshSeatSetThreshold(4), 3U);
    BOOST_CHECK(changed_count.SetHash(domain, market, 9) != base);

    auto changed_outpoint{*snapshot};
    ++changed_outpoint.members[0].outpoint.n;
    changed_outpoint.members[0].seat_id = flowmesh::ComputeFlowMeshSeatId(
        domain, changed_outpoint.members[0].outpoint);
    std::sort(changed_outpoint.members.begin(), changed_outpoint.members.end(),
              [](const node::FnSeatRecord& a, const node::FnSeatRecord& b) {
                  return a.seat_id < b.seat_id ||
                         (a.seat_id == b.seat_id && a.outpoint < b.outpoint);
              });
    BOOST_CHECK(changed_outpoint.SetHash(domain, market, 9) != base);

    auto changed_key{*snapshot};
    changed_key.members[0].bls_pubkey[0] ^= 1;
    BOOST_CHECK(changed_key.SetHash(domain, market, 9) != base);
    auto reversed{snapshot->SetMembers()};
    std::reverse(reversed.begin(), reversed.end());
    BOOST_CHECK(!flowmesh::ComputeFlowMeshSeatSetHash(
        domain, market, 9, A2, snapshot->anchor_hash, reversed));

    // The domain is also structural: changing it invalidates every derived
    // SeatId instead of producing a reusable commitment.
    BOOST_CHECK(!snapshot->SetHash(Filled(0x58), market, 9));
}

BOOST_AUTO_TEST_CASE(unique_bootstrap_snapshot_uses_first_ready_post_market_block)
{
    const Consensus::Params params{Params()};
    SyntheticChain chain{A3 + 6};
    node::FnSeatIndex index;
    std::vector<bls::SecretKey> keys;
    for (uint32_t i{0}; i < 4; ++i) keys.push_back(Key(40 + i));

    Connect(index, Verify(index, Block({SeatTx(
                              params, {keys[0], keys[1], keys[2]}, {}, 40)}),
                          A2, chain.hashes[A2], params));
    for (int height{A2 + 1}; height <= A3 + 6; ++height) {
        const CBlock block{
            height == A2 + 3
                ? Block({SeatTx(params, {keys[3]}, {}, 41)})
                : CBlock{}};
        Connect(index, Verify(index, block, height, chain.hashes[height],
                              params));
    }

    std::string error;
    // A market established at A2+2 waits for the fourth seat at A2+3.
    const auto first_ready{index.EarliestFlowMeshReadySnapshot(
        chain.chain, A2 + 2, A3 + 3, params, error)};
    BOOST_REQUIRE_MESSAGE(first_ready.has_value(), error);
    BOOST_CHECK_EQUAL(first_ready->anchor_height, A2 + 3);
    BOOST_CHECK(first_ready->anchor_hash == chain.hashes[A2 + 3]);

    // If the market is established later while the same committee is already
    // ready, its own creation block is the unique lower bound.
    const auto market_later{index.EarliestFlowMeshReadySnapshot(
        chain.chain, A2 + 4, A3 + 4, params, error)};
    BOOST_REQUIRE_MESSAGE(market_later.has_value(), error);
    BOOST_CHECK_EQUAL(market_later->anchor_height, A2 + 4);

    // The selected readiness transition is unusable until it is 30 deep.
    BOOST_CHECK(!index.EarliestFlowMeshReadySnapshot(
        chain.chain, A2 + 2, A3 + 2, params, error));
    BOOST_CHECK_EQUAL(
        error, "FlowMesh has fewer than four anchor-final bootstrap seats");
}

BOOST_AUTO_TEST_CASE(five_thousand_member_boundary_is_canonical_and_bounded)
{
    const Consensus::Params params{Params()};
    const uint256 domain{Domain(params)};
    SyntheticChain chain{A2};
    node::FnSeatBlockDelta delta;
    delta.height = A2;
    delta.block_hash = chain.hashes[A2];
    delta.added.reserve(5000);
    for (uint32_t i{0}; i < 5000; ++i) {
        node::FnSeatRecord record;
        uint256 txid;
        txid.begin()[0] = static_cast<unsigned char>((i + 1) & 0xff);
        txid.begin()[1] = static_cast<unsigned char>(((i + 1) >> 8) & 0xff);
        record.outpoint = COutPoint{Txid::FromUint256(txid), i};
        record.seat_id = flowmesh::ComputeFlowMeshSeatId(domain, record.outpoint);
        WriteBE32(record.bls_pubkey.data(), i + 1);
        record.created_height = A2;
        record.created_block = delta.block_hash;
        delta.added.push_back(record);
    }
    node::FnSeatIndex index;
    std::string error;
    BOOST_REQUIRE_MESSAGE(index.ConnectBlock(delta, error), error);
    BOOST_CHECK_EQUAL(index.Size(), 5000U);
    const auto snapshot{index.SnapshotAt(chain.indexes[A2])};
    BOOST_REQUIRE(snapshot.has_value());
    BOOST_REQUIRE_EQUAL(snapshot->members.size(), 5000U);
    BOOST_CHECK(snapshot->SetHash(domain, Filled(0x60), 1).has_value());

    node::FnSeatRecord extra;
    extra.outpoint = COutPoint{Txid::FromUint256(Filled(0x61)), 5000};
    extra.seat_id = flowmesh::ComputeFlowMeshSeatId(domain, extra.outpoint);
    extra.bls_pubkey.fill(0xff);
    node::FnSeatBlockDelta overflow;
    overflow.height = A2 + 1;
    overflow.block_hash = Filled(0x62);
    overflow.added.push_back(extra);
    BOOST_CHECK(!index.ConnectBlock(overflow, error));
    BOOST_CHECK_EQUAL(error, "FlowMesh FN-seat delta exceeds the live-seat cap");
    BOOST_CHECK_EQUAL(index.Size(), 5000U); // failure was atomic
}

BOOST_AUTO_TEST_SUITE_END()
