// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_LEGACY_REPLAY_H
#define B3COIN_LEGACY_REPLAY_H

#include <coins.h>
#include <consensus/params.h>
#include <serialize.h>
#include <uint256.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>

class CBlock;
class CBlockUndo;
class CDBWrapper;
struct DBParams;

namespace legacy {

//! Safely decode a raw legacy-encoded block. Returns false (with `error`
//! set) instead of throwing on malformed bytes.
bool DecodeLegacyBlock(std::span<const std::byte> raw, CBlock& block, std::string& error);

/**
 * Trusted mechanical replay of the legacy era (heights <= the finalized
 * legacy boundary H). A distinct engine, not a set of bypass flags inside
 * the modern ConnectBlock path.
 *
 * Once the boundary (H, X) is pinned, the boundary hash attests the entire
 * legacy prefix, so replay only performs mechanical processing:
 *
 *  - safe legacy block/transaction decoding (ApplyRawBlock);
 *  - previous-block hash linkage, anchored on the configured genesis;
 *  - configured checkpoint hashes;
 *  - transaction Merkle roots, including duplicate-transaction mutation;
 *  - referenced UTXO existence, duplicate-spend detection, exact input
 *    removal and exact output creation;
 *  - overflow-safe value accounting;
 *  - preservation of historical txid, vout index, amount, scriptPubKey,
 *    creation height, coinbase/coinstake classification, transaction time
 *    and in-block offset (maturity- and kernel-relevant metadata).
 *
 * It deliberately skips historical script/signature validation, PoW, the
 * PoS kernel, stake modifiers, rewards, difficulty, timestamps/MTP and
 * chainwork fork choice. Malformed or internally inconsistent data is
 * still rejected. There is no UTXO snapshot and no hard-coded UTXO root:
 * the set is reconstructed exclusively by applying blocks.
 *
 * Blocks apply strictly in height order and atomically: a failed block
 * leaves the target view and the replay position untouched, which makes
 * crash-safe forward resumption (ResumeAt) straightforward for a caller
 * that persists the view.
 */
class TrustedReplay
{
public:
    //! `final_height` is H, the last legacy height; blocks above it are
    //! refused. `checkpoints` maps heights to required legacy block hashes.
    TrustedReplay(const Consensus::Params& params, int final_height,
                  std::map<int, uint256> checkpoints);

    //! Resume forward replay: `next_height` is the height to be applied
    //! next and `tip_hash` the hash of its parent.
    void ResumeAt(int next_height, const uint256& tip_hash);

    //! Mechanically apply the next block in height order to `view`.
    //! All-or-nothing: on failure `error` is set and neither the view nor
    //! the replay position changes.
    //!
    //! When `undo` is provided, the spent coins are captured per
    //! transaction in the standard CBlockUndo layout (one entry per
    //! transaction after the coinbase, inputs in order), so a caller that
    //! connects blocks through replay can disconnect them through the
    //! ordinary undo path. On failure the partially filled contents of
    //! `undo` are meaningless and must be discarded.
    bool ApplyBlock(const CBlock& block, CCoinsViewCache& view, std::string& error,
                    CBlockUndo* undo = nullptr);

    //! Safely decode a raw legacy-encoded block, then apply it.
    bool ApplyRawBlock(std::span<const std::byte> raw, CCoinsViewCache& view, std::string& error);

    int NextHeight() const { return m_next_height; }
    const uint256& TipHash() const { return m_tip_hash; }

    int FinalHeight() const { return m_final_height; }

private:
    const Consensus::Params& m_params;
    const int m_final_height;
    const std::map<int, uint256> m_checkpoints;
    int m_next_height{0};
    uint256 m_tip_hash{};
};

/**
 * Durable coins storage for trusted replay. The UTXO entries and a single
 * replay marker — format version, last completely applied height and hash,
 * and the completion state — live in one database, and every block commits
 * its coin changes together with its marker in ONE atomic batch write. A
 * crash therefore either persists a block completely (coins and marker) or
 * not at all; a partial block can never be observed.
 */
class ReplayDB final : public CCoinsView
{
public:
    static constexpr int32_t FORMAT_VERSION{1};

    struct Marker {
        int32_t version{FORMAT_VERSION};
        int32_t height{-1};
        uint256 hash{};
        bool completed{false};
        SERIALIZE_METHODS(Marker, obj) { READWRITE(obj.version, obj.height, obj.hash, obj.completed); }
    };

    explicit ReplayDB(DBParams db_params);
    ~ReplayDB();

    //! The persisted marker, if any.
    std::optional<Marker> ReadMarker() const;
    //! Whether any UTXO entry exists (used to detect a marker-less database
    //! that nevertheless holds coins — an inconsistent state).
    bool HasAnyCoins() const;
    //! Synchronously persist a marker on its own (completion state).
    void WriteMarker(const Marker& marker);
    //! The marker committed together with the next BatchWrite.
    void SetPendingMarker(const Marker& marker) { m_pending = marker; }

    std::optional<Coin> GetCoin(const COutPoint& outpoint) const override;
    uint256 GetBestBlock() const override;
    void BatchWrite(CoinsViewCacheCursor& cursor, const uint256& hashBlock) override;

private:
    std::unique_ptr<CDBWrapper> m_db;
    std::optional<Marker> m_marker;
    std::optional<Marker> m_pending;
};

/**
 * Atomically resumable trusted replay: TrustedReplay's mechanical checks
 * with per-block durable commits into a ReplayDB. Load() restores the
 * position exactly from the marker and never guesses — a marker-less
 * database with coins, an unknown format version, or a marker that
 * disagrees with the configured boundary all fail safely instead.
 */
class PersistentReplay
{
public:
    PersistentReplay(const Consensus::Params& params, int final_height,
                     std::map<int, uint256> checkpoints, ReplayDB& db);

    //! Restore the replay position from the persisted marker. Must succeed
    //! before blocks can be applied.
    bool Load(std::string& error);

    //! Apply the next block and durably commit its coins and marker in one
    //! atomic batch. On failure nothing changes and restart resumes at
    //! exactly the same block.
    bool ApplyBlock(const CBlock& block, std::string& error);
    bool ApplyRawBlock(std::span<const std::byte> raw, std::string& error);

    //! Persist the completion state once every block through the finalized
    //! boundary has been applied.
    bool Finish(std::string& error);

    int NextHeight() const { return m_replay.NextHeight(); }
    const uint256& TipHash() const { return m_replay.TipHash(); }
    bool Completed() const { return m_completed; }

private:
    ReplayDB& m_db;
    TrustedReplay m_replay;
    bool m_loaded{false};
    bool m_completed{false};
};

} // namespace legacy

#endif // B3COIN_LEGACY_REPLAY_H
