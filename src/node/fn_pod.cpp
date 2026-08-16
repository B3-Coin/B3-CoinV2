// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/fn_pod.h>

#include <chain.h>
#include <compat/endian.h>
#include <consensus/era.h>
#include <legacy/consensus.h>
#include <node/blockstorage.h>
#include <primitives/block.h>
#include <script/script.h>
#include <tinyformat.h>
#include <undo.h>
#include <validation.h>

#include <algorithm>
#include <set>

namespace node {

namespace {
constexpr CAmount MARKER_VALUE{1'000'000}; // the customary 1-B3 output (audit only)
constexpr uint8_t DB_POD_RECORD{'p'};
constexpr uint8_t DB_MARKER{'m'};

//! Big-endian height so leveldb iterates records in (height, txid) order.
struct PodKey {
    int32_t height{0};
    Txid txid{};
    SERIALIZE_METHODS(PodKey, obj)
    {
        uint8_t prefix{DB_POD_RECORD};
        READWRITE(prefix);
        uint32_t be{htobe32_internal(static_cast<uint32_t>(obj.height))};
        READWRITE(be);
        SER_READ(obj, obj.height = static_cast<int32_t>(be32toh_internal(be)));
        READWRITE(obj.txid);
    }
};
} // namespace

bool IsSupportedFundingScript(const std::span<const unsigned char> s)
{
    // Frozen byte-exact historical key forms; never a policy matcher.
    // P2PKH: OP_DUP OP_HASH160 <20> OP_EQUALVERIFY OP_CHECKSIG
    if (s.size() == 25 && s[0] == OP_DUP && s[1] == OP_HASH160 && s[2] == 0x14 &&
        s[23] == OP_EQUALVERIFY && s[24] == OP_CHECKSIG) {
        return true;
    }
    // P2PK: <33-byte key> OP_CHECKSIG | <65-byte key> OP_CHECKSIG
    if (s.size() == 35 && s[0] == 33 && (s[1] == 0x02 || s[1] == 0x03) &&
        s[34] == OP_CHECKSIG) {
        return true;
    }
    if (s.size() == 67 && s[0] == 65 && s[1] == 0x04 && s[66] == OP_CHECKSIG) {
        return true;
    }
    return false;
}

std::optional<PodRecord> ClassifyPod(const CTransaction& tx,
                                     const std::vector<CTxOut>& spent_prevouts,
                                     const int height, const Consensus::Params& params)
{
    // The consensus PoD detector, verbatim: non-coinbase, non-coinstake,
    // input/output gap at or above the height's collateral tier.
    if (tx.IsCoinBase() || tx.IsCoinStake()) return std::nullopt;
    if (spent_prevouts.size() != tx.vin.size()) return std::nullopt;

    CAmount value_in{0};
    for (const CTxOut& prev : spent_prevouts) value_in += prev.nValue;
    const CAmount gap{value_in - tx.GetValueOut()};
    const CAmount tier{legacy::GetFNCollateral(height, params)};
    if (gap < tier) return std::nullopt;

    PodRecord record;
    record.pod_id = tx.GetHash();
    record.height = height;
    record.disintegrated = gap;
    record.tier = tier;

    // Distinct raw funding scripts, deduplicated and canonically ordered.
    std::set<std::vector<unsigned char>> distinct;
    for (const CTxOut& prev : spent_prevouts) {
        distinct.emplace(prev.scriptPubKey.begin(), prev.scriptPubKey.end());
    }
    record.funding_scripts.assign(distinct.begin(), distinct.end());

    record.claimable = std::ranges::all_of(record.funding_scripts, [](const auto& script) {
        return IsSupportedFundingScript(script);
    });
    record.reason = record.claimable ? PodClaimability::SUPPORTED
                                     : PodClaimability::UNSUPPORTED_FUNDING_SCRIPT;

    // Audit metadata only.
    for (uint32_t n{0}; n < tx.vout.size(); ++n) {
        if (tx.vout[n].nValue == MARKER_VALUE) record.marker_vouts.push_back(n);
    }
    return record;
}

std::vector<PodRecord> DerivePodRecords(const CBlock& block, const CBlockUndo& undo,
                                        const int height, const Consensus::Params& params)
{
    std::vector<PodRecord> records;
    for (size_t i{1}; i < block.vtx.size(); ++i) {
        const CTransaction& tx{*block.vtx[i]};
        if (i - 1 >= undo.vtxundo.size()) break; // malformed undo: stay total
        std::vector<CTxOut> prevouts;
        prevouts.reserve(undo.vtxundo[i - 1].vprevout.size());
        for (const Coin& coin : undo.vtxundo[i - 1].vprevout) prevouts.push_back(coin.out);
        if (auto record{ClassifyPod(tx, prevouts, height, params)}) {
            records.push_back(std::move(*record));
        }
    }
    return records;
}

PodDB::PodDB(DBParams db_params) : m_db{std::move(db_params)} {}

std::optional<PodDB::Marker> PodDB::ReadMarker() const
{
    Marker marker;
    if (!m_db.Read(DB_MARKER, marker)) return std::nullopt;
    return marker;
}

void PodDB::WriteHeight(const int height, const uint256& block_hash,
                        const std::vector<PodRecord>& records)
{
    CDBBatch batch{m_db};
    for (const PodRecord& record : records) {
        batch.Write(PodKey{.height = height, .txid = record.pod_id}, record);
    }
    batch.Write(DB_MARKER, Marker{.height = height, .hash = block_hash});
    m_db.WriteBatch(batch, /*fSync=*/true);
}

void PodDB::RewindTo(const int height, const uint256& block_hash)
{
    CDBBatch batch{m_db};
    std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    for (it->Seek(PodKey{.height = height + 1, .txid = Txid{}}); it->Valid(); it->Next()) {
        PodKey key;
        if (!it->GetKey(key)) break;
        batch.Erase(key);
    }
    batch.Write(DB_MARKER, Marker{.height = height, .hash = block_hash});
    m_db.WriteBatch(batch, /*fSync=*/true);
}

std::vector<PodRecord> PodDB::ReadAll()
{
    std::vector<PodRecord> records;
    std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    for (it->Seek(PodKey{.height = 0, .txid = Txid{}}); it->Valid(); it->Next()) {
        PodKey key;
        if (!it->GetKey(key)) break;
        PodRecord record;
        if (!it->GetValue(record)) {
            throw std::runtime_error("PodDB: undecodable record");
        }
        records.push_back(std::move(record));
    }
    return records;
}

bool SyncPodRecords(ChainstateManager& chainman, PodDB& db, std::string& error)
{
    LOCK(cs_main);
    const Consensus::Params& params{chainman.GetConsensus()};
    const CChain& chain{chainman.ActiveChain()};

    const std::optional<int> final_height{Consensus::LegacyFinalHeight(params)};
    // Records exist only for heights <= H; without a configured boundary
    // the legacy era has no end yet and records may grow with the chain.
    const int target{final_height ? std::min(chain.Height(), *final_height) : chain.Height()};

    // Recover the marker position; rewind if its block left the chain.
    int synced{0};
    if (const auto marker{db.ReadMarker()}) {
        if (marker->version != PodDB::FORMAT_VERSION) {
            error = "fnpod database has an unknown format version";
            return false;
        }
        synced = marker->height;
        const CBlockIndex* at{chain[marker->height]};
        if (!at || at->GetBlockHash() != marker->hash) {
            // The marker's block left the active chain (a legacy-era
            // reorganization before the boundary is pinned). Only one
            // (height, hash) pair is stored, so recovery re-derives from
            // genesis: deterministic, never guessing, and cheap relative
            // to how rare pre-pin reorgs are.
            db.RewindTo(0, chain[0]->GetBlockHash());
            synced = 0;
        }
    }

    for (int height{synced + 1}; height <= target; ++height) {
        const CBlockIndex* pindex{chain[height]};
        if (!pindex) break;
        CBlock block;
        if (!chainman.m_blockman.ReadBlock(block, *pindex)) {
            error = strprintf("block data unavailable at height %d (pruned before "
                              "PoD derivation; reindex required)", height);
            return false;
        }
        CBlockUndo undo;
        if (block.vtx.size() > 1 && !chainman.m_blockman.ReadBlockUndo(undo, *pindex)) {
            error = strprintf("undo data unavailable at height %d (pruned before "
                              "PoD derivation; reindex required)", height);
            return false;
        }
        db.WriteHeight(height, pindex->GetBlockHash(),
                       DerivePodRecords(block, undo, height, params));
    }
    return true;
}

PodCapacityReport BuildPodCapacityReport(const std::vector<PodRecord>& records)
{
    PodCapacityReport report;
    report.total_qualifying = records.size();
    for (const PodRecord& record : records) {
        ++report.by_reason[static_cast<uint8_t>(record.reason)];
        if (!record.claimable) continue;
        ++report.claimable;
        report.max_distinct_funding_scripts =
            std::max(report.max_distinct_funding_scripts, record.funding_scripts.size());
    }
    // Worst-case one-authorization size: compact index (<=3) + key length
    // prefix + 65-byte uncompressed key + sig length prefix + 72-byte DER.
    constexpr size_t WORST_AUTH{3 + 1 + 65 + 1 + 72};
    report.max_authorization_payload = report.max_distinct_funding_scripts * WORST_AUTH;
    // One push per authorization (each <= 520 by construction); whole proof
    // script bounded by MAX_SCRIPT_SIZE = 10,000 bytes including the tag.
    constexpr size_t MAX_PROOF_SCRIPT{10'000};
    constexpr size_t TAG_OVERHEAD{1 + 1 + 36}; // OP_RETURN + push + "B3FP"||pod_id
    const size_t per_auth{WORST_AUTH + 3};     // + push opcode overhead
    report.fits_b3fp_carrier =
        TAG_OVERHEAD + report.max_distinct_funding_scripts * per_auth <= MAX_PROOF_SCRIPT;
    return report;
}

} // namespace node
