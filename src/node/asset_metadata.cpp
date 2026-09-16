// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/asset_metadata.h>

#include <chain.h>
#include <consensus/era.h>
#include <dbwrapper.h>
#include <modern/chain_domain.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <util/log.h>
#include <util/threadnames.h>
#include <validation.h>

#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace node {
namespace {
constexpr uint8_t DB_IDENTITY{'M'};
constexpr uint8_t DB_CURSOR{'C'};
constexpr uint8_t DB_PROOF{'P'};
constexpr uint32_t DB_VERSION{1};
constexpr unsigned int SCAN_BATCH_BLOCKS{32};
constexpr size_t DB_CACHE_BYTES{1 << 20};

std::optional<uint256> ChainDomain(const Consensus::Params& params)
{
    return params.legacy_final_hash
        ? modern::ModernChainDomain(params.hashGenesisBlock, *params.legacy_final_hash)
        : std::nullopt;
}

int FirstAssetHeight(const Consensus::Params& params)
{
    if (!params.asset_activation_height || *params.asset_activation_height < 0 ||
        !Consensus::AssetRulesActive(*params.asset_activation_height, params)) return -1;
    return *params.asset_activation_height;
}

template <typename K, typename V>
bool ReadExact(CDBWrapper& db, const K& key, V& value)
{
    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    it->Seek(key);
    K actual_key;
    return it->Valid() && it->GetKeyExact(actual_key) && actual_key == key &&
           it->GetValueExact(value) && it->StatusOK();
}
} // namespace

AssetMetadataCache::AssetMetadataCache(ChainstateManager& chainman, const fs::path& datadir)
    : m_chainman{chainman}, m_datadir{datadir},
      m_domain{ChainDomain(chainman.GetConsensus())},
      m_first_height{FirstAssetHeight(chainman.GetConsensus())}
{
}

AssetMetadataCache::~AssetMetadataCache()
{
    Stop();
}

bool AssetMetadataCache::Start()
{
    std::lock_guard lock{m_lifecycle_mutex};
    if (m_thread.joinable() || !m_domain || m_first_height < 0) return true;
    m_interrupt.reset();
    try {
        m_thread = std::thread([this] {
            // This optional cache must not let an exception escape its thread
            // (unlike a consensus index, it must never AbortNode or terminate).
            try {
                util::ThreadRename("asset-metadata");
                Run();
            } catch (const std::exception& e) {
                LogWarning("Asset metadata discovery stopped; undiscovered precision remains unknown: %s", e.what());
            } catch (...) {
                LogWarning("Asset metadata discovery stopped; undiscovered precision remains unknown");
            }
        });
    } catch (const std::exception& e) {
        LogWarning("Unable to start optional asset metadata discovery: %s", e.what());
        return false;
    }
    return true;
}

void AssetMetadataCache::Stop()
{
    std::lock_guard lock{m_lifecycle_mutex};
    m_interrupt();
    if (m_thread.joinable()) m_thread.join();
}

std::optional<modern::AssetMetadataProof> AssetMetadataCache::Get(const uint256& asset) const
{
    LOCK(m_mutex);
    const auto it{m_proofs.find(asset)};
    if (it == m_proofs.end()) return std::nullopt;
    return it->second;
}

bool AssetMetadataCache::Remember(const uint256& asset, const modern::AssetMetadataProof& proof)
{
    if (!m_domain || !modern::VerifyAssetMetadataProof(*m_domain, asset, proof)) return false;
    LOCK(m_mutex);
    if (m_proofs.emplace(asset, proof).second) {
        m_generation.fetch_add(1, std::memory_order_release);
    }
    return true;
}

void AssetMetadataCache::Load(CDBWrapper& db, Cursor& cursor)
{
    const auto expected_identity{std::pair{DB_VERSION, std::pair{*m_domain, m_first_height}}};
    if (db.IsEmpty()) {
        db.Write(DB_IDENTITY, expected_identity);
        return;
    }
    auto identity{expected_identity};
    if (!ReadExact(db, DB_IDENTITY, identity) || identity != expected_identity) {
        throw std::runtime_error("asset metadata cache has an unknown format or chain scope");
    }

    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    it->Seek(DB_PROOF);
    unsigned int loaded{0};
    while (it->Valid() && !m_interrupt) {
        uint8_t prefix;
        if (!it->GetKey(prefix)) throw std::runtime_error("unreadable asset metadata cache key");
        if (prefix != DB_PROOF) break;
        std::pair<uint8_t, uint256> key;
        modern::AssetMetadataProof proof;
        if (!it->GetKeyExact(key) || !it->GetValueExact(proof) || !Remember(key.second, proof)) {
            throw std::runtime_error("asset metadata cache contains an invalid issuance proof");
        }
        it->Next();
        // Loading an existing cache is background work too, with bounded
        // interruption checks and no long-held metadata mutex.
        if (++loaded % 1024 == 0) m_interrupt.sleep_for(std::chrono::milliseconds{1});
    }
    if (!it->StatusOK()) throw std::runtime_error("asset metadata cache iterator failed");
    if (m_interrupt) return;

    if (db.Exists(DB_CURSOR)) {
        Cursor saved;
        if (!ReadExact(db, DB_CURSOR, saved) || saved.height < m_first_height - 1 ||
            (saved.height == m_first_height - 1 ? !saved.hash.IsNull() : saved.hash.IsNull())) {
            throw std::runtime_error("asset metadata cache has an invalid scan cursor");
        }
        cursor = saved;
    }
}

void AssetMetadataCache::Run()
{
    Cursor cursor{m_first_height - 1, {}};
    std::unique_ptr<CDBWrapper> db;
    try {
        db = std::make_unique<CDBWrapper>(DBParams{
            .path = m_datadir / "indexes" / "asset_metadata" / fs::PathFromString(m_domain->GetHex()) / "db",
            .cache_bytes = DB_CACHE_BYTES,
        });
        Load(*db, cursor);
    } catch (const std::exception& e) {
        // Never retain an advanced cursor after incomplete/corrupt loading:
        // independently verified rows may remain in memory, but rescan all
        // modern blocks to rediscover any omitted facts. Leave the DB intact.
        LogWarning("Asset metadata cache unavailable; discovering in memory: %s", e.what());
        db.reset();
        cursor = {m_first_height - 1, {}};
    }

    std::optional<uint256> last_missing_block;
    while (!m_interrupt) {
        const Cursor previous_cursor{cursor};
        std::map<uint256, modern::AssetMetadataProof> discovered;
        unsigned int processed{0};
        bool missing_block{false};
        for (; processed < SCAN_BATCH_BLOCKS && !m_interrupt; ++processed) {
            const CBlockIndex* next{nullptr};
            {
                LOCK(cs_main);
                const CChain& chain{m_chainman.ValidatedChainstate().m_chain};
                if (cursor.height >= m_first_height) {
                    const auto* saved{m_chainman.m_blockman.LookupBlockIndex(cursor.hash)};
                    const auto* fork{saved && saved->nHeight == cursor.height ? chain.FindFork(saved) : nullptr};
                    if (!fork || fork->nHeight < m_first_height) {
                        cursor = {m_first_height - 1, {}};
                    } else {
                        cursor = {fork->nHeight, fork->GetBlockHash()};
                    }
                }
                if (cursor.height >= chain.Height()) break;
                next = chain[cursor.height + 1];
                if (!next || !next->IsValid(BLOCK_VALID_SCRIPTS)) break;
                missing_block = !(next->nStatus & BLOCK_HAVE_DATA);
            }

            // BlockManager snapshots file positions under cs_main internally;
            // actual block I/O and proof decoding happen outside that lock.
            CBlock block;
            if (missing_block || !m_chainman.m_blockman.ReadBlock(block, *next)) {
                if (last_missing_block != next->GetBlockHash()) {
                    LogWarning("Asset metadata discovery waiting for block %s at height %d; undiscovered precision remains unknown",
                               next->GetBlockHash().GetHex(), next->nHeight);
                    last_missing_block = next->GetBlockHash();
                }
                missing_block = true;
                break; // Never skip a missing/pruned block and advance the cursor.
            }
            last_missing_block.reset();
            std::map<uint256, modern::AssetMetadataProof> block_proofs;
            for (const auto& tx : block.vtx) {
                if (m_interrupt) break;
                if (tx->mpa.empty()) continue;
                uint256 asset;
                modern::AssetMetadataProof proof;
                std::string error;
                if (modern::DecodeAssetPrecisionProof(*tx, *m_domain, asset, proof, error)) {
                    block_proofs.emplace(asset, std::move(proof));
                }
            }
            if (m_interrupt) break;
            {
                LOCK(cs_main);
                const CChain& chain{m_chainman.ValidatedChainstate().m_chain};
                // The branch may have changed during disk I/O. Retry from the
                // cursor before learning this block or claiming scan progress.
                if (chain[next->nHeight] != next || !next->IsValid(BLOCK_VALID_SCRIPTS)) continue;
            }
            for (const auto& [asset, proof] : block_proofs) {
                if (Remember(asset, proof)) discovered.emplace(asset, proof);
            }
            cursor = {next->nHeight, next->GetBlockHash()};
        }

        if (db && (cursor != previous_cursor || !discovered.empty())) {
            try {
                // Cursor and every fact discovered before it are one atomic
                // batch. A crash may cause replay, never a cursor-only skip.
                CDBBatch batch{*db};
                for (const auto& [asset, proof] : discovered) batch.Write(std::pair{DB_PROOF, asset}, proof);
                batch.Write(DB_CURSOR, cursor);
                db->WriteBatch(batch);
            } catch (const std::exception& e) {
                LogWarning("Asset metadata cache persistence stopped; continuing in memory: %s", e.what());
                db.reset();
            }
        }
        // Yield between bounded catch-up batches; sleep interruptibly at the
        // tip. A missing block pauses discovery, not validation or wallet I/O.
        m_interrupt.sleep_for(missing_block ? std::chrono::milliseconds{10'000}
                              : processed == SCAN_BATCH_BLOCKS ? std::chrono::milliseconds{10}
                                                              : std::chrono::milliseconds{1'000});
    }
}

} // namespace node
