// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/asset_metadata.h>

#include <chain.h>
#include <consensus/block_codec.h>
#include <consensus/merkle.h>
#include <dbwrapper.h>
#include <modern/chain_domain.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <test/util/logging.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace node {
struct AssetMetadataCachePersistenceTestAccess {
    static std::pair<int, uint256> Load(AssetMetadataCache& cache, CDBWrapper& db)
    {
        AssetMetadataCache::Cursor cursor{cache.m_first_height - 1, {}};
        cache.Load(db, cursor);
        return {cursor.height, cursor.hash};
    }
};
} // namespace node

namespace {
using Proof = modern::AssetMetadataProof;
using Cache = node::AssetMetadataCache;
using Access = node::AssetMetadataCachePersistenceTestAccess;
constexpr uint8_t DB_IDENTITY{'M'};
constexpr uint8_t DB_CURSOR{'C'};
constexpr uint8_t DB_PROOF{'P'};

Proof TestProof(const uint8_t tag = 1, const uint8_t decimals = 6)
{
    return {COutPoint{Txid::FromUint256(uint256{tag}), 7}, 1'000'000'000'000, decimals};
}

bool WaitUntil(const std::function<bool()>& predicate)
{
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{5}};
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return false;
}

struct CacheSetup : TestingSetup {
    uint256 domain;
    int first_height;

    CacheSetup()
        : TestingSetup{ChainType::REGTEST,
                       {.extra_args = {"-b3modernregtest", "-b3flowmeshtest", "-b3corridorlength=130"}}}
    {
        const auto& params{m_node.chainman->GetConsensus()};
        domain = *modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash);
        first_height = *params.asset_activation_height;
    }

    uint256 Asset(const Proof& proof) const
    {
        return modern::AssetIdV1(domain, proof.issuance_prevout,
            modern::AssetGenesisCommitment({.max_supply = proof.max_supply, .decimals = proof.decimals}));
    }

    fs::path CachePath() const
    {
        return m_path_root / "indexes" / "asset_metadata" / fs::PathFromString(domain.GetHex()) / "db";
    }

    std::unique_ptr<CDBWrapper> MemoryDB() const
    {
        auto db{std::make_unique<CDBWrapper>(DBParams{
            .path = {}, .cache_bytes = 1 << 20, .memory_only = true})};
        Identity(*db);
        return db;
    }

    void Identity(CDBWrapper& db) const
    {
        db.Write(DB_IDENTITY, std::pair{uint32_t{1}, std::pair{domain, first_height}});
    }
};

// These synthetic index entries model blocks already accepted by validation.
// The test exercises the real block-file codec, background scan, and saved
// cursor, not consensus issuance admission (covered by asset_validation_tests).
struct ScanSetup : CacheSetup {
    CBlockIndex* original_tip;
    CBlockIndex* activation_parent;
    uint32_t nonce{0};

    ScanSetup()
    {
        original_tip = WITH_LOCK(cs_main, return m_node.chainman->ActiveChain().Tip());
        BOOST_REQUIRE(original_tip);
        activation_parent = original_tip;
        for (int height{1}; height < first_height; ++height) {
            // No historical file data: proves the scan does not read the
            // legacy/corridor prefix before asset activation.
            activation_parent = Append(activation_parent, {}, false);
        }
    }

    ~ScanSetup()
    {
        LOCK(cs_main);
        m_node.chainman->ActiveChain().SetTip(*original_tip);
    }

    CTransactionRef Issuance(const Proof& proof) const
    {
        CMutableTransaction tx;
        tx.version = 2;
        tx.vin.emplace_back(proof.issuance_prevout);
        const auto action{modern::MakeAssetIssuanceAction(
            {.max_supply = proof.max_supply, .decimals = proof.decimals})};
        tx.mpa.emplace_back(action.action_type, action.action_version, action.payload);
        return MakeTransactionRef(std::move(tx));
    }

    CBlockIndex* Append(CBlockIndex* parent, const std::vector<CTransactionRef>& transactions,
                        const bool have_data = true)
    {
        CBlock block;
        block.nVersion = Consensus::WithB3BlockCodecV2(2);
        block.hashPrevBlock = parent->GetBlockHash();
        block.nTime = parent->nTime + 1;
        block.nNonce = ++nonce;
        block.vtx = transactions;
        block.hashMerkleRoot = BlockMerkleRoot(block);
        const auto pos{have_data ? m_node.chainman->m_blockman.WriteBlock(block, parent->nHeight + 1)
                                 : FlatFilePos{}};
        if (have_data) BOOST_REQUIRE(!pos.IsNull());
        LOCK(cs_main);
        auto* index{m_node.chainman->m_blockman.InsertBlockIndex(block.GetHash())};
        BOOST_REQUIRE(index);
        index->nHeight = parent->nHeight + 1;
        index->pprev = parent;
        index->nTime = block.nTime;
        index->nStatus = BLOCK_VALID_SCRIPTS;
        if (have_data) {
            index->nStatus |= BLOCK_HAVE_DATA;
            index->nFile = pos.nFile;
            index->nDataPos = pos.nPos;
        }
        index->BuildSkip();
        m_node.chainman->ActiveChain().SetTip(*index);
        return index;
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(asset_metadata_cache_tests, CacheSetup)

BOOST_AUTO_TEST_CASE(persisted_proofs_are_verified_and_duplicate_load_does_not_refresh)
{
    auto db{MemoryDB()};
    const auto proof{TestProof()};
    const auto asset{Asset(proof)};
    db->Write(std::pair{DB_PROOF, asset}, proof);
    db->Write(DB_CURSOR, std::pair{first_height + 10, uint256{uint8_t{0x42}}});
    Cache cache{*m_node.chainman, m_path_root};
    BOOST_CHECK(!cache.Get(asset));
    BOOST_CHECK_EQUAL(cache.Generation(), 0U);
    const auto cursor{Access::Load(cache, *db)};
    BOOST_CHECK_EQUAL(cursor.first, first_height + 10);
    BOOST_REQUIRE(cache.Get(asset));
    BOOST_CHECK_EQUAL(cache.Get(asset)->decimals, 6U);
    BOOST_CHECK_EQUAL(cache.Generation(), 1U);
    Access::Load(cache, *db);
    BOOST_CHECK_EQUAL(cache.Generation(), 1U);
    BOOST_CHECK(!cache.Get(uint256{uint8_t{0x77}}));
}

BOOST_AUTO_TEST_CASE(invalid_or_noncanonical_persisted_proofs_are_rejected)
{
    const auto proof{TestProof()};
    const auto asset{Asset(proof)};
    for (unsigned int variant{0}; variant < 6; ++variant) {
        auto db{MemoryDB()};
        switch (variant) {
        case 0: {
            auto wrong{proof};
            ++wrong.decimals;
            db->Write(std::pair{DB_PROOF, asset}, wrong);
            break;
        }
        case 1:
            db->Write(std::pair{DB_PROOF, uint256{uint8_t{0x78}}}, proof);
            break;
        case 2:
            db->Write(std::pair{DB_PROOF, asset}, std::pair{proof, uint8_t{0}});
            break;
        case 3:
            db->Write(std::pair{std::pair{DB_PROOF, asset}, uint8_t{0}}, proof);
            break;
        case 4:
            db->Write(DB_PROOF, proof); // truncated key must not be skipped by Seek.
            break;
        case 5:
            db->Write(DB_IDENTITY, std::pair{uint32_t{1}, std::pair{uint256{uint8_t{0x79}}, first_height}});
            db->Write(std::pair{DB_PROOF, asset}, proof);
            break;
        }
        Cache cache{*m_node.chainman, m_path_root};
        BOOST_CHECK_THROW(Access::Load(cache, *db), std::runtime_error);
        BOOST_CHECK(!cache.Get(asset));
        BOOST_CHECK_EQUAL(cache.Generation(), 0U);
    }
}

BOOST_AUTO_TEST_CASE(invalid_saved_cursor_never_authorizes_skipping_history)
{
    for (const auto& cursor : {std::pair{first_height - 2, uint256{}},
                               std::pair{first_height - 1, uint256{uint8_t{0x31}}},
                               std::pair{first_height, uint256{}}}) {
        auto db{MemoryDB()};
        db->Write(DB_CURSOR, cursor);
        Cache cache{*m_node.chainman, m_path_root};
        BOOST_CHECK_THROW(Access::Load(cache, *db), std::runtime_error);
    }
    auto db{MemoryDB()};
    db->Write(DB_CURSOR, std::pair{std::pair{first_height, uint256{uint8_t{0x31}}}, uint8_t{0}});
    Cache cache{*m_node.chainman, m_path_root};
    BOOST_CHECK_THROW(Access::Load(cache, *db), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_FIXTURE_TEST_SUITE(asset_metadata_scan_tests, ScanSetup)

BOOST_AUTO_TEST_CASE(background_scan_restart_reorg_and_future_block)
{
    const auto original{TestProof(1)};
    const auto replacement{TestProof(2, 3)};
    const auto future{TestProof(3, 8)};
    Append(activation_parent, {Issuance(original)});
    {
        Cache cache{*m_node.chainman, m_path_root};
        BOOST_REQUIRE(cache.Start());
        BOOST_REQUIRE(cache.Start());
        BOOST_REQUIRE(WaitUntil([&] { return cache.Get(Asset(original)).has_value(); }));
        cache.Stop();
        cache.Stop();
    }
    CBlockIndex* replacement_tip{Append(activation_parent, {Issuance(replacement)})};
    {
        Cache restarted{*m_node.chainman, m_path_root};
        BOOST_REQUIRE(restarted.Start());
        BOOST_REQUIRE(WaitUntil([&] { return restarted.Get(Asset(replacement)).has_value(); }));
        // Precision is immutable knowledge; the old proof is not a claim of
        // current chain membership and survives the cursor's branch rewind.
        BOOST_CHECK(restarted.Get(Asset(original)));
        BOOST_CHECK_EQUAL(restarted.Generation(), 2U);
        Append(replacement_tip, {Issuance(future)});
        BOOST_REQUIRE(WaitUntil([&] { return restarted.Get(Asset(future)).has_value(); }));
        restarted.Stop();
        BOOST_CHECK_EQUAL(restarted.Generation(), 3U);
    }
    CDBWrapper db{DBParams{.path = CachePath(), .cache_bytes = 1 << 20}};
    Cache loaded{*m_node.chainman, m_path_root};
    const auto cursor{Access::Load(loaded, db)};
    BOOST_CHECK_EQUAL(cursor.first, first_height + 1);
    BOOST_CHECK(loaded.Get(Asset(original)));
    BOOST_CHECK(loaded.Get(Asset(replacement)));
    BOOST_CHECK(loaded.Get(Asset(future)));
}

BOOST_AUTO_TEST_CASE(missing_activation_block_does_not_skip_to_later_proof)
{
    auto* missing{Append(activation_parent, {}, false)};
    const auto later{TestProof(4)};
    Append(missing, {Issuance(later)});
    std::atomic<bool> waiting{false};
    DebugLogHelper log{"Asset metadata discovery waiting for block", [&](const std::string* line) {
        if (line) waiting = true;
        return true;
    }};
    Cache cache{*m_node.chainman, m_path_root};
    BOOST_REQUIRE(cache.Start());
    BOOST_REQUIRE(WaitUntil([&] { return waiting.load(); }));
    BOOST_CHECK(!cache.Get(Asset(later)));
    cache.Stop(); // Interrupts the ten-second missing-data backoff.
    CDBWrapper db{DBParams{.path = CachePath(), .cache_bytes = 1 << 20}};
    BOOST_CHECK(!db.Exists(DB_CURSOR));
}

BOOST_AUTO_TEST_CASE(corrupt_cache_falls_back_to_validated_block_scan)
{
    const auto proof{TestProof(5)};
    const auto asset{Asset(proof)};
    Append(activation_parent, {Issuance(proof)});
    {
        CDBWrapper db{DBParams{.path = CachePath(), .cache_bytes = 1 << 20}};
        Identity(db);
        auto corrupt{proof};
        ++corrupt.decimals;
        db.Write(std::pair{DB_PROOF, asset}, corrupt);
        db.Write(DB_CURSOR, std::pair{first_height + 100, uint256{uint8_t{0x56}}});
    }
    Cache cache{*m_node.chainman, m_path_root};
    BOOST_REQUIRE(cache.Start());
    BOOST_REQUIRE(WaitUntil([&] { return cache.Get(asset).has_value(); }));
    BOOST_CHECK_EQUAL(cache.Get(asset)->decimals, proof.decimals);
    cache.Stop();
    // The optional cache's error must not request node shutdown or rewrite
    // corrupt files. They can be examined/recovered separately by the owner.
    BOOST_CHECK(!m_interrupt);
    CDBWrapper db{DBParams{.path = CachePath(), .cache_bytes = 1 << 20}};
    Proof persisted;
    BOOST_REQUIRE(db.Read(std::pair{DB_PROOF, asset}, persisted));
    BOOST_CHECK_EQUAL(persisted.decimals, proof.decimals + 1);
}

BOOST_AUTO_TEST_SUITE_END()
