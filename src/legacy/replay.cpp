// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <legacy/replay.h>

#include <coins.h>
#include <consensus/amount.h>
#include <consensus/block_codec.h>
#include <consensus/merkle.h>
#include <dbwrapper.h>
#include <legacy/codec.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <tinyformat.h>

#include <exception>
#include <utility>

namespace {

constexpr uint8_t DB_REPLAY_COIN{'C'};
constexpr uint8_t DB_REPLAY_MARKER{'R'};

struct ReplayCoinKey {
    COutPoint* outpoint;
    uint8_t key{DB_REPLAY_COIN};
    explicit ReplayCoinKey(const COutPoint* ptr) : outpoint(const_cast<COutPoint*>(ptr)) {}

    SERIALIZE_METHODS(ReplayCoinKey, obj) { READWRITE(obj.key, obj.outpoint->hash, VARINT(obj.outpoint->n)); }
};

} // namespace

namespace legacy {

bool DecodeLegacyBlock(const std::span<const std::byte> raw, CBlock& block, std::string& error)
{
    try {
        SpanReader reader{raw};
        reader >> TX_LEGACY(block);
    } catch (const std::exception& e) {
        error = strprintf("undecodable legacy block: %s", e.what());
        return false;
    }
    return true;
}

TrustedReplay::TrustedReplay(const Consensus::Params& params, const int final_height,
                             std::map<int, uint256> checkpoints)
    : m_params{params}, m_final_height{final_height}, m_checkpoints{std::move(checkpoints)}
{
}

void TrustedReplay::ResumeAt(const int next_height, const uint256& tip_hash)
{
    m_next_height = next_height;
    m_tip_hash = tip_hash;
}

bool TrustedReplay::ApplyBlock(const CBlock& block, CCoinsViewCache& view, std::string& error)
{
    const int height{m_next_height};
    if (height > m_final_height) {
        error = strprintf("height %d is beyond the finalized legacy boundary %d", height, m_final_height);
        return false;
    }

    // Only the legacy codec exists at or below the boundary.
    if (Consensus::HasB3BlockCodecV2(block.nVersion)) {
        error = strprintf("modern codec marker below the legacy boundary at height %d", height);
        return false;
    }
    const uint256 hash{block.GetMarkerHash(m_params)};

    // Previous-block linkage, anchored on the configured genesis.
    if (height == 0) {
        if (!block.hashPrevBlock.IsNull() || hash != m_params.hashGenesisBlock) {
            error = "block at height 0 is not the configured genesis";
            return false;
        }
    } else if (block.hashPrevBlock != m_tip_hash) {
        error = strprintf("broken linkage at height %d: prev %s, tip %s",
                          height, block.hashPrevBlock.ToString(), m_tip_hash.ToString());
        return false;
    }

    // Configured checkpoint hashes.
    if (const auto it{m_checkpoints.find(height)}; it != m_checkpoints.end() && it->second != hash) {
        error = strprintf("checkpoint mismatch at height %d: got %s, want %s",
                          height, hash.ToString(), it->second.ToString());
        return false;
    }

    // Structural shape needed for deterministic classification.
    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase()) {
        error = strprintf("first transaction is not a coinbase at height %d", height);
        return false;
    }
    for (size_t i{1}; i < block.vtx.size(); ++i) {
        if (block.vtx[i]->IsCoinBase()) {
            error = strprintf("unexpected extra coinbase at height %d", height);
            return false;
        }
        // Historical identity requires the legacy transaction encoding.
        if (!block.vtx[i]->IsLegacyEncoded()) {
            error = strprintf("non-legacy transaction encoding at height %d", height);
            return false;
        }
    }
    if (!block.vtx[0]->IsLegacyEncoded()) {
        error = strprintf("non-legacy coinbase encoding at height %d", height);
        return false;
    }

    // Merkle integrity; a mutated root means duplicated transactions.
    bool mutated{false};
    if (BlockMerkleRoot(block, &mutated) != block.hashMerkleRoot) {
        error = strprintf("merkle root mismatch at height %d", height);
        return false;
    }
    if (mutated) {
        error = strprintf("duplicated transactions at height %d", height);
        return false;
    }

    // Mechanical UTXO transition, atomic per block: nothing reaches `view`
    // unless every transaction applies.
    CCoinsViewCache block_view(&view);
    uint32_t tx_offset{static_cast<uint32_t>(GetSerializeSize(CBlockHeader{})) +
                       static_cast<uint32_t>(GetSizeOfCompactSize(block.vtx.size()))};
    for (const CTransactionRef& ptx : block.vtx) {
        const CTransaction& tx{*ptx};
        const Txid txid{tx.GetHash()};

        CAmount value_in{0};
        if (!tx.IsCoinBase()) {
            for (const CTxIn& txin : tx.vin) {
                // Missing prevouts and duplicate spends (within a
                // transaction, a block, or against history) fail here.
                if (!block_view.HaveCoin(txin.prevout)) {
                    error = strprintf("missing or already-spent input %s:%u of %s at height %d",
                                      txin.prevout.hash.ToString(), txin.prevout.n,
                                      txid.ToString(), height);
                    return false;
                }
                const Coin& coin{block_view.AccessCoin(txin.prevout)};
                if (!MoneyRange(coin.out.nValue) || coin.out.nValue > MAX_MONEY - value_in) {
                    error = strprintf("input value overflow in %s at height %d", txid.ToString(), height);
                    return false;
                }
                value_in += coin.out.nValue;
                block_view.SpendCoin(txin.prevout);
            }
        }

        CAmount value_out{0};
        for (const CTxOut& txout : tx.vout) {
            if (txout.nValue < 0 || !MoneyRange(txout.nValue) || txout.nValue > MAX_MONEY - value_out) {
                error = strprintf("output value overflow in %s at height %d", txid.ToString(), height);
                return false;
            }
            value_out += txout.nValue;
        }
        // Rewards are attested, not validated, so coinbase and coinstake may
        // create value; a plain transaction creating value is internally
        // inconsistent data.
        if (!tx.IsCoinBase() && !tx.IsCoinStake() && value_out > value_in) {
            error = strprintf("plain transaction %s creates value at height %d", txid.ToString(), height);
            return false;
        }

        // Exact output creation, preserving txid, vout index, amount,
        // scriptPubKey, height, coinbase/coinstake class, transaction time
        // and in-block offset. Overwrites mirror historical pre-BIP30
        // behavior deterministically.
        AddCoins(block_view, tx, height, /*check=*/true, tx_offset);
        tx_offset += static_cast<uint32_t>(GetSerializeSize(TX_LEGACY(tx)));
    }
    block_view.Flush();

    m_tip_hash = hash;
    ++m_next_height;
    return true;
}

bool TrustedReplay::ApplyRawBlock(const std::span<const std::byte> raw, CCoinsViewCache& view,
                                  std::string& error)
{
    CBlock block;
    if (!DecodeLegacyBlock(raw, block, error)) return false;
    return ApplyBlock(block, view, error);
}

ReplayDB::ReplayDB(DBParams db_params) : m_db{std::make_unique<CDBWrapper>(std::move(db_params))}
{
    Marker marker;
    if (m_db->Read(DB_REPLAY_MARKER, marker)) m_marker = marker;
}

ReplayDB::~ReplayDB() = default;

std::optional<ReplayDB::Marker> ReplayDB::ReadMarker() const
{
    return m_marker;
}

bool ReplayDB::HasAnyCoins() const
{
    std::unique_ptr<CDBIterator> cursor{m_db->NewIterator()};
    // Coin keys start with the prefix byte; seek to the smallest such key.
    const std::pair<uint8_t, uint256> start{DB_REPLAY_COIN, uint256{}};
    cursor->Seek(start);
    if (!cursor->Valid()) return false;
    std::pair<uint8_t, uint256> key;
    return cursor->GetKey(key) && key.first == DB_REPLAY_COIN;
}

void ReplayDB::WriteMarker(const Marker& marker)
{
    m_db->Write(DB_REPLAY_MARKER, marker, /*fSync=*/true);
    m_marker = marker;
}

std::optional<Coin> ReplayDB::GetCoin(const COutPoint& outpoint) const
{
    Coin coin;
    if (!m_db->Read(ReplayCoinKey{&outpoint}, coin)) return std::nullopt;
    return coin;
}

uint256 ReplayDB::GetBestBlock() const
{
    return m_marker ? m_marker->hash : uint256{};
}

void ReplayDB::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& hashBlock)
{
    // One block, one batch: the coin changes and the replay marker become
    // durable together or not at all.
    if (!m_pending || m_pending->hash != hashBlock) {
        throw std::logic_error("replay marker/chainstate disagreement in BatchWrite");
    }
    CDBBatch batch{*m_db};
    for (auto it{cursor.Begin()}; it != cursor.End();) {
        if (it->second.IsDirty()) {
            ReplayCoinKey entry{&it->first};
            if (it->second.coin.IsSpent()) {
                batch.Erase(entry);
            } else {
                batch.Write(entry, it->second.coin);
            }
        }
        it = cursor.NextAndMaybeErase(*it);
    }
    batch.Write(DB_REPLAY_MARKER, *m_pending);
    m_db->WriteBatch(batch, /*fSync=*/true);
    m_marker = m_pending;
    m_pending.reset();
}

PersistentReplay::PersistentReplay(const Consensus::Params& params, const int final_height,
                                   std::map<int, uint256> checkpoints, ReplayDB& db)
    : m_db{db}, m_replay{params, final_height, std::move(checkpoints)}
{
}

bool PersistentReplay::Load(std::string& error)
{
    const std::optional<ReplayDB::Marker> marker{m_db.ReadMarker()};
    if (!marker) {
        // Recovery never guesses: a database holding coins without a marker
        // is inconsistent, not resumable.
        if (m_db.HasAnyCoins()) {
            error = "replay database holds coins but no marker";
            return false;
        }
        m_replay.ResumeAt(0, uint256{});
        m_loaded = true;
        return true;
    }
    if (marker->version != ReplayDB::FORMAT_VERSION) {
        error = strprintf("unsupported replay format version %d (expected %d)",
                          marker->version, ReplayDB::FORMAT_VERSION);
        return false;
    }
    if (marker->height > m_replay.FinalHeight()) {
        error = strprintf("replay marker height %d disagrees with the configured boundary %d",
                          marker->height, m_replay.FinalHeight());
        return false;
    }
    if (marker->completed && marker->height != m_replay.FinalHeight()) {
        error = strprintf("replay marked complete at height %d but the boundary is %d",
                          marker->height, m_replay.FinalHeight());
        return false;
    }
    m_replay.ResumeAt(marker->height + 1, marker->hash);
    m_completed = marker->completed;
    m_loaded = true;
    return true;
}

bool PersistentReplay::ApplyBlock(const CBlock& block, std::string& error)
{
    if (!m_loaded) {
        error = "replay position not loaded";
        return false;
    }
    if (m_completed) {
        error = "replay already completed";
        return false;
    }

    const int height{m_replay.NextHeight()};
    const uint256 prev_tip{m_replay.TipHash()};
    CCoinsViewCache cache{&m_db};
    if (!m_replay.ApplyBlock(block, cache, error)) return false;

    const ReplayDB::Marker marker{.version = ReplayDB::FORMAT_VERSION,
                                  .height = height,
                                  .hash = m_replay.TipHash(),
                                  .completed = false};
    try {
        m_db.SetPendingMarker(marker);
        cache.SetBestBlock(marker.hash);
        cache.Flush(); // one atomic batch: coins + marker
    } catch (const std::exception& e) {
        // Nothing durable changed; rewind so restartless retry stays exact.
        m_replay.ResumeAt(height, prev_tip);
        error = strprintf("failed to commit block at height %d: %s", height, e.what());
        return false;
    }
    return true;
}

bool PersistentReplay::ApplyRawBlock(const std::span<const std::byte> raw, std::string& error)
{
    CBlock block;
    if (!DecodeLegacyBlock(raw, block, error)) return false;
    return ApplyBlock(block, error);
}

bool PersistentReplay::Finish(std::string& error)
{
    if (!m_loaded) {
        error = "replay position not loaded";
        return false;
    }
    if (m_completed) return true;
    if (m_replay.NextHeight() <= m_replay.FinalHeight()) {
        error = strprintf("replay is at height %d, boundary %d not yet reached",
                          m_replay.NextHeight(), m_replay.FinalHeight());
        return false;
    }
    ReplayDB::Marker marker{.version = ReplayDB::FORMAT_VERSION,
                            .height = m_replay.FinalHeight(),
                            .hash = m_replay.TipHash(),
                            .completed = true};
    try {
        m_db.WriteMarker(marker);
    } catch (const std::exception& e) {
        error = strprintf("failed to persist completion: %s", e.what());
        return false;
    }
    m_completed = true;
    return true;
}

} // namespace legacy
