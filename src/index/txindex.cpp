// Copyright (c) 2017-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <index/txindex.h>

#include <common/args.h>
#include <consensus/block_codec.h>
#include <dbwrapper.h>
#include <flatfile.h>
#include <index/base.h>
#include <index/disktxpos.h>
#include <interfaces/chain.h>
#include <legacy/codec.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <uint256.h>
#include <util/fs.h>
#include <util/log.h>
#include <validation.h>

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <utility>
#include <vector>

constexpr uint8_t DB_TXINDEX{'t'};

std::unique_ptr<TxIndex> g_txindex;


/** Access to the txindex database (indexes/txindex/) */
class TxIndex::DB : public BaseIndex::DB
{
public:
    explicit DB(size_t n_cache_size, bool f_memory = false, bool f_wipe = false);

    /// Read the disk location of the transaction data with the given hash. Returns false if the
    /// transaction hash is not indexed.
    bool ReadTxPos(const Txid& txid, CDiskTxPos& pos) const;

    /// Write a batch of transaction positions to the DB.
    void WriteTxs(const std::vector<std::pair<Txid, CDiskTxPos>>& v_pos);
};

TxIndex::DB::DB(size_t n_cache_size, bool f_memory, bool f_wipe) :
    BaseIndex::DB(gArgs.GetDataDirNet() / "indexes" / "txindex", n_cache_size, f_memory, f_wipe)
{}

bool TxIndex::DB::ReadTxPos(const Txid& txid, CDiskTxPos& pos) const
{
    return Read(std::make_pair(DB_TXINDEX, txid.ToUint256()), pos);
}

void TxIndex::DB::WriteTxs(const std::vector<std::pair<Txid, CDiskTxPos>>& v_pos)
{
    CDBBatch batch(*this);
    for (const auto& [txid, pos] : v_pos) {
        batch.Write(std::make_pair(DB_TXINDEX, txid.ToUint256()), pos);
    }
    WriteBatch(batch);
}

TxIndex::TxIndex(std::unique_ptr<interfaces::Chain> chain, size_t n_cache_size, bool f_memory, bool f_wipe)
    : BaseIndex(std::move(chain), "txindex"), m_db(std::make_unique<TxIndex::DB>(n_cache_size, f_memory, f_wipe))
{}

TxIndex::~TxIndex() = default;

bool TxIndex::CustomAppend(const interfaces::BlockInfo& block)
{
    // Exclude genesis block transaction because outputs are not spendable.
    if (block.height == 0) return true;

    assert(block.data);
    CDiskTxPos pos({block.file_number, block.data_pos}, GetSizeOfCompactSize(block.data->vtx.size()));
    // Offsets must measure the bytes actually on disk: a legacy-codec B3
    // block stores nTime transactions without witness framing, selected by
    // the block's permanent codec marker exactly as the storage layer does.
    const bool legacy_codec_block{m_chainstate->m_chainman.GetConsensus().legacy_b3coin &&
                                  !Consensus::HasB3BlockCodecV2(block.data->nVersion)};
    std::vector<std::pair<Txid, CDiskTxPos>> vPos;
    vPos.reserve(block.data->vtx.size());
    for (const auto& tx : block.data->vtx) {
        vPos.emplace_back(tx->GetHash(), pos);
        pos.nTxOffset += legacy_codec_block ? ::GetSerializeSize(legacy::TX_LEGACY(*tx))
                                            : ::GetSerializeSize(TX_MODERN(*tx));
    }
    m_db->WriteTxs(vPos);
    return true;
}

BaseIndex::DB& TxIndex::GetDB() const { return *m_db; }

bool TxIndex::FindTx(const Txid& tx_hash, uint256& block_hash, CTransactionRef& tx) const
{
    CDiskTxPos postx;
    if (!m_db->ReadTxPos(tx_hash, postx)) {
        return false;
    }

    AutoFile file{m_chainstate->m_blockman.OpenBlockFile(postx, true)};
    if (file.IsNull()) {
        LogError("OpenBlockFile failed");
        return false;
    }
    const Consensus::Params& consensus{m_chainstate->m_chainman.GetConsensus()};
    CBlockHeader header;
    try {
        file >> header;
        file.seek(postx.nTxOffset, SEEK_CUR);
        // The header's permanent codec marker selects the transaction codec,
        // mirroring the write path; provenance makes GetHash() the
        // historical txid for legacy-decoded transactions.
        if (consensus.legacy_b3coin && !Consensus::HasB3BlockCodecV2(header.nVersion)) {
            file >> legacy::TX_LEGACY(tx);
        } else {
            // Marker-modern blocks store the full payload form. Reading with
            // the witness-only codec rejects flag 0x02 and offsets every
            // later transaction after an MPA-bearing one.
            file >> TX_MODERN(tx);
        }
    } catch (const std::exception& e) {
        LogError("Deserialize or I/O error - %s", e.what());
        return false;
    }
    if (tx->GetHash() != tx_hash) {
        LogError("txid mismatch");
        return false;
    }
    // Marker-derived identity, the same rule as every other identity site:
    // a marker-modern block's hash is SHA256d even on a legacy-B3 chain.
    block_hash = header.GetMarkerHash(consensus);
    return true;
}
