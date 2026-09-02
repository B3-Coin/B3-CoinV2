// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TXDB_H
#define BITCOIN_TXDB_H

#include <coins.h>
#include <dbwrapper.h>
#include <kernel/caches.h>
#include <kernel/cs_main.h>
#include <sync.h>
#include <util/fs.h>

#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class COutPoint;
class uint256;

//! User-controlled performance and debug options.
struct CoinsViewOptions {
    //! Maximum database write batch size in bytes.
    size_t batch_write_bytes{DEFAULT_DB_CACHE_BATCH};
    //! If non-zero, randomly exit when the database is flushed with (1/ratio) probability.
    int simulate_crash_ratio{0};
};

/** CCoinsView backed by the coin database (chainstate/) */
class CCoinsViewDB final : public CCoinsView
{
protected:
    DBParams m_db_params;
    CoinsViewOptions m_options;
    //! Prevents CompactFull() from using m_db while ResizeCache() replaces it.
    Mutex m_db_mutex;
    std::unique_ptr<CDBWrapper> m_db;
    std::shared_future<void> m_compaction;
    //! Maintain the versioned B3 validation marker atomically with DB_BEST_BLOCK.
    bool m_b3_validation_schema_v1_enabled{false};
public:
    explicit CCoinsViewDB(DBParams db_params, CoinsViewOptions options);
    ~CCoinsViewDB() override;

    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
    bool HaveCoin(const COutPoint &outpoint) const override;
    uint256 GetBestBlock() const override;
    std::vector<uint256> GetHeadBlocks() const override;
    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& hashBlock) override;
    std::unique_ptr<CCoinsViewCursor> Cursor() const override;

    //! Whether an unsupported database format is used.
    bool NeedsUpgrade();

    /**
     * Whether this coins database was verified under B3 post-H validation
     * schema v1 at exactly its current best block. A missing/stale marker also
     * disables automatic marker advancement until Mark...() succeeds.
     */
    bool B3ValidationSchemaV1Current() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * Synchronously mark the current best block as schema-v1 verified and
     * enable atomic marker advancement in subsequent BatchWrite calls.
     */
    bool MarkB3ValidationSchemaV1Current() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    /**
     * Synchronously revoke schema-v1 trust. Subsequent BatchWrite calls must
     * not carry the marker to another tip until Mark...() succeeds again.
     */
    bool ClearB3ValidationSchemaV1() EXCLUSIVE_LOCKS_REQUIRED(cs_main);

    size_t EstimateSize() const override;

    //! Dynamically alter the underlying leveldb cache size.
    void ResizeCache(size_t new_cache_size) EXCLUSIVE_LOCKS_REQUIRED(cs_main, !m_db_mutex);

    //! Perform a full compaction of the underlying LevelDB on a one-shot background thread.
    std::shared_future<void> CompactFull() EXCLUSIVE_LOCKS_REQUIRED(cs_main, !m_db_mutex);

    //! Return an underlying LevelDB property value, if available.
    std::optional<std::string> GetDBProperty(const std::string& property);
};

#endif // BITCOIN_TXDB_H
