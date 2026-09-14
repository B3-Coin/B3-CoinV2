// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_NODE_ASSET_METADATA_H
#define BITCOIN_NODE_ASSET_METADATA_H

#include <modern/asset_metadata.h>
#include <sync.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/threadinterrupt.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

class CDBWrapper;
class ChainstateManager;

namespace node {

/**
 * Optional node-wide knowledge of immutable simple-v1 issuance preimages.
 *
 * A bounded background scan starts at asset activation, using the validated
 * chainstate, not an unvalidated assumeutxo tip. Cache failure never stops
 * validation. Missing historical blocks leave undiscovered precision unknown.
 * A verified preimage remains useful after a reorg: it proves an AssetId's
 * precision, NOT current chain inclusion, a display label, or monetary backing.
 *
 * Get() and Generation() do not access disk, cs_main, or wallet callbacks, so
 * wallet callers may use them while holding cs_wallet. Stop() must be called
 * outside cs_main, before the referenced ChainstateManager is destroyed.
 */
class AssetMetadataCache
{
public:
    AssetMetadataCache(ChainstateManager& chainman, const fs::path& datadir);
    ~AssetMetadataCache();

    //! Starts the worker without opening the database or scanning on the caller.
    //! A chain without a configured asset schedule remains dormant (success).
    bool Start() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Stop() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    std::optional<modern::AssetMetadataProof> Get(const uint256& asset) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    uint64_t Generation() const { return m_generation.load(std::memory_order_acquire); }

private:
    friend struct AssetMetadataCacheTestAccess;
    friend struct AssetMetadataCachePersistenceTestAccess;
    struct Cursor {
        int height{-1};
        uint256 hash;

        SERIALIZE_METHODS(Cursor, obj) { READWRITE(obj.height, obj.hash); }
        bool operator==(const Cursor&) const = default;
    };

    //! The test seam and every production insertion use the same proof check.
    bool Remember(const uint256& asset, const modern::AssetMetadataProof& proof) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Load(CDBWrapper& db, Cursor& cursor) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Run() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    ChainstateManager& m_chainman;
    const fs::path m_datadir;
    const std::optional<uint256> m_domain;
    const int m_first_height;
    mutable Mutex m_mutex;
    std::map<uint256, modern::AssetMetadataProof> m_proofs GUARDED_BY(m_mutex);
    std::atomic<uint64_t> m_generation{0};
    // Serializes Start/Stop only. The worker and wallet lookups never acquire it.
    std::mutex m_lifecycle_mutex;
    CThreadInterrupt m_interrupt;
    std::thread m_thread;
};

} // namespace node

#endif // BITCOIN_NODE_ASSET_METADATA_H
