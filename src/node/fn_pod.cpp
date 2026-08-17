// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/fn_pod.h>

#include <chain.h>
#include <compat/endian.h>
#include <consensus/era.h>
#include <legacy/consensus.h>
#include <modern/fn.h>
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
        if (prefix != DB_POD_RECORD) {
            throw std::ios_base::failure("unexpected fnpod database key prefix");
        }
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

bool DerivePodRecords(const CBlock& block, const CBlockUndo& undo, const int height,
                      const Consensus::Params& params, std::vector<PodRecord>& out,
                      std::string& error)
{
    error.clear(); // never leave a stale message from an earlier attempt
    // FAIL CLOSED on undo inconsistency (corrective ruling 2026-08-17):
    // a partial record set must never be returned or persisted. Undo
    // data carries exactly one entry per non-coinbase transaction, and
    // each entry exactly one spent coin per input.
    const size_t expected{block.vtx.empty() ? 0 : block.vtx.size() - 1};
    if (undo.vtxundo.size() != expected) {
        error = strprintf("undo data mismatched at height %d: %d entries for %d "
                          "non-coinbase transactions",
                          height, undo.vtxundo.size(), expected);
        return false;
    }
    // Validation here is STRUCTURAL plus obviously-invalid Coins (spent/
    // null entries, out-of-range amounts, unsafe summation). Disk
    // checksums detect stored-byte corruption; validated ConnectBlock
    // undo data or TrustedReplay reconstruction supplies SEMANTIC
    // provenance — never this function.
    std::vector<PodRecord> records;
    for (size_t i{1}; i < block.vtx.size(); ++i) {
        const CTransaction& tx{*block.vtx[i]};
        const auto& prevout_coins{undo.vtxundo[i - 1].vprevout};
        if (prevout_coins.size() != tx.vin.size()) {
            error = strprintf("undo data mismatched at height %d: transaction %d has %d "
                              "spent coins for %d inputs",
                              height, i, prevout_coins.size(), tx.vin.size());
            return false;
        }
        std::vector<CTxOut> prevouts;
        prevouts.reserve(prevout_coins.size());
        CAmount value_in{0};
        for (const Coin& coin : prevout_coins) {
            if (coin.IsSpent()) {
                error = strprintf("undo data invalid at height %d: spent/null coin in "
                                  "transaction %d", height, i);
                return false;
            }
            if (coin.out.nValue < 0 || coin.out.nValue > MAX_MONEY) {
                error = strprintf("undo data invalid at height %d: coin amount out of "
                                  "range in transaction %d", height, i);
                return false;
            }
            // PRE-ADD guard: reject before the addition can overflow —
            // every addend passed MoneyRange, so MAX_MONEY - value_in is
            // itself safe to compute.
            if (coin.out.nValue > MAX_MONEY - value_in) {
                error = strprintf("undo data invalid at height %d: input value sum out "
                                  "of range in transaction %d", height, i);
                return false;
            }
            value_in += coin.out.nValue;
            prevouts.push_back(coin.out);
        }
        if (auto record{ClassifyPod(tx, prevouts, height, params)}) {
            records.push_back(std::move(*record));
        }
    }
    // Single atomic publication: on ANY failure above, `out` was never
    // touched — the caller's vector is left completely unchanged.
    out = std::move(records);
    return true;
}

PodDB::PodDB(DBParams db_params) : m_db{std::move(db_params)} {}

bool PodDB::ScanRecordsStrict(
    const std::function<void(const int height, const Txid& txid, PodRecord&&)>& fn,
    std::string& error, const std::optional<int> max_height)
{
    error.clear();
    // Walk the COMPLETE record namespace from the RAW one-byte prefix —
    // a malformed key that sorts before the first canonical height key
    // is caught, not skipped. Every key and value must decode with
    // EXACT/full consumption (typed GetKeyExact/GetValueExact, which
    // also apply the normal value deobfuscation), the decoded record
    // must match its key, heights must be sane, and iterator
    // termination must be a clean end-of-range.
    std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    for (it->Seek(uint8_t{DB_POD_RECORD}); it->Valid(); it->Next()) {
        uint8_t prefix{0};
        if (!it->GetKey(prefix) || prefix != DB_POD_RECORD) {
            error = "fnpod database contains an unexpected key";
            return false;
        }
        PodKey key;
        if (!it->GetKeyExact(key)) {
            error = "fnpod database contains an undecodable record key";
            return false;
        }
        PodRecord record;
        if (!it->GetValueExact(record)) {
            error = "fnpod database contains an undecodable record";
            return false;
        }
        if (record.height != key.height || !(record.pod_id == key.txid)) {
            error = "fnpod database record does not match its key";
            return false;
        }
        if (record.height < 0) {
            error = "fnpod database record has a negative height";
            return false;
        }
        if (max_height && record.height > *max_height) {
            error = "fnpod database record lies above the marker height";
            return false;
        }
        fn(key.height, key.txid, std::move(record));
    }
    if (!it->StatusOK()) {
        error = "fnpod database iterator failed (I/O or checksum error)";
        return false;
    }
    return true;
}

PodDB::MarkerRead PodDB::ReadMarkerChecked(Marker& out)
{
    // Exact-consumption marker read: the prefix byte is inspected with
    // the ordinary GetKey, then the FULL key must be exactly the one
    // marker byte — a key beginning with 'm' but carrying trailing
    // bytes (e.g. "m\0") is CORRUPT, never MISSING. The value must
    // decode with complete consumption too.
    std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    it->Seek(uint8_t{DB_MARKER});
    if (!it->Valid()) return it->StatusOK() ? MarkerRead::MISSING : MarkerRead::CORRUPT;
    uint8_t prefix{0};
    if (!it->GetKey(prefix)) return MarkerRead::CORRUPT; // empty/unreadable key
    if (prefix != DB_MARKER) return MarkerRead::MISSING; // first key >= 'm' is not the marker
    uint8_t exact_key{0};
    if (!it->GetKeyExact(exact_key)) return MarkerRead::CORRUPT; // 'm' + trailing bytes
    Marker decoded;
    if (!it->GetValueExact(decoded)) return MarkerRead::CORRUPT;
    // Coexistence check: even with a canonical 'm' present, an EXTRA
    // marker-prefixed key (e.g. "m\0") is corruption. The decoded
    // marker is not published until this passes.
    it->Next();
    if (!it->StatusOK()) return MarkerRead::CORRUPT;
    if (it->Valid()) {
        uint8_t next_prefix{0};
        if (!it->GetKey(next_prefix)) return MarkerRead::CORRUPT;
        if (next_prefix == DB_MARKER) return MarkerRead::CORRUPT;
    }
    out = decoded;
    return MarkerRead::OK;
}

std::optional<PodDB::Marker> PodDB::ReadMarker()
{
    Marker marker;
    switch (ReadMarkerChecked(marker)) {
    case MarkerRead::OK: return marker;
    case MarkerRead::MISSING: return std::nullopt;
    case MarkerRead::CORRUPT: break;
    }
    // Corruption must never be flattened into "missing".
    throw std::runtime_error("PodDB: undecodable marker");
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

bool PodDB::RewindTo(const int height, const uint256& block_hash, std::string& error)
{
    error.clear();
    // Honor the public contract: validate the EXISTING marker and the
    // FULL record namespace strictly BEFORE committing any deletion or
    // the new marker. Every rejection below leaves the marker and all
    // records unchanged. This method REWINDS only — it never advances
    // the marker.
    Marker existing;
    switch (ReadMarkerChecked(existing)) {
    case MarkerRead::OK:
        break;
    case MarkerRead::MISSING:
        error = "fnpod database has no marker to rewind";
        return false;
    case MarkerRead::CORRUPT:
        error = "fnpod database marker is undecodable";
        return false;
    }
    if (existing.version != FORMAT_VERSION) {
        error = "fnpod database has an unknown format version";
        return false;
    }
    if (existing.height < 0) {
        error = "fnpod database marker has a negative height";
        return false;
    }
    if (height < 0) {
        error = "rewind target height is negative";
        return false;
    }
    if (height > existing.height) {
        error = "rewind target lies above the marker; RewindTo never advances";
        return false;
    }
    if (height == existing.height && block_hash != existing.hash) {
        error = "rewind target hash contradicts the marker at the same height";
        return false;
    }
    std::vector<PodKey> above;
    if (!ScanRecordsStrict(
            [&](const int record_height, const Txid& txid, PodRecord&&) {
                if (record_height > height) {
                    above.push_back(PodKey{.height = record_height, .txid = txid});
                }
            },
            error, existing.height)) {
        return false;
    }
    CDBBatch batch{m_db};
    for (const PodKey& key : above) batch.Erase(key);
    batch.Write(DB_MARKER, Marker{.height = height, .hash = block_hash});
    m_db.WriteBatch(batch, /*fSync=*/true);
    return true;
}

std::vector<PodRecord> PodDB::ReadAll()
{
    // Strict full-namespace scan; THROWS on any violation — a partial
    // set is never returned.
    std::vector<PodRecord> records;
    std::string error;
    if (!ScanRecordsStrict(
            [&](const int, const Txid&, PodRecord&& record) {
                records.push_back(std::move(record));
            },
            error)) {
        throw std::runtime_error("PodDB: " + error);
    }
    return records;
}

bool SyncPodRecords(ChainstateManager& chainman, PodDB& db, std::string& error)
{
    error.clear(); // never leave a stale message from an earlier attempt
    LOCK(cs_main);
    const Consensus::Params& params{chainman.GetConsensus()};
    const CChain& chain{chainman.ActiveChain()};

    // ---- H/X ANCHOR ENFORCEMENT, before ANY database mutation
    // (hardening ruling 2026-08-17). H and X are one anchor: a
    // half-configured or contradicted anchor fails closed BEFORE any
    // database write, leaving the logical marker and records unchanged.
    const std::optional<int> final_height{Consensus::LegacyFinalHeight(params)};
    if (final_height.has_value() != params.legacy_final_hash.has_value()) {
        error = "final legacy height and hash must be configured together";
        return false;
    }
    if (final_height && *final_height < 0) {
        error = "final legacy height is negative";
        return false;
    }
    if (final_height && chain.Height() >= *final_height) {
        const CBlockIndex* at_h{chain[*final_height]};
        if (!at_h || at_h->GetBlockHash() != *params.legacy_final_hash) {
            error = strprintf("active chain does not carry X at the final legacy "
                              "height %d", *final_height);
            return false;
        }
    }
    // Records exist only for heights <= H; without a configured boundary
    // the legacy era has no end yet and records may grow with the chain.
    // When the node has not reached H yet this derives the LOCAL PREFIX
    // only — it is not an anchored or completed claim set.
    const int target{final_height ? std::min(chain.Height(), *final_height) : chain.Height()};

    // Recover the marker position; rewind if its block left the chain. A
    // present-but-undecodable marker is corruption, never missing data.
    int synced{0};
    {
        PodDB::Marker marker;
        switch (db.ReadMarkerChecked(marker)) {
        case PodDB::MarkerRead::MISSING: {
            // A missing marker is valid ONLY when the record namespace is
            // empty. Records (or malformed keys) without a marker are
            // corruption: never rebuild over stale records.
            size_t count{0};
            std::string scan_error;
            if (!db.ScanRecordsStrict(
                    [&](const int, const Txid&, PodRecord&&) { ++count; }, scan_error)) {
                error = scan_error;
                return false;
            }
            if (count > 0) {
                error = "fnpod database has records without a marker";
                return false;
            }
            break;
        }
        case PodDB::MarkerRead::CORRUPT:
            error = "fnpod database marker is undecodable";
            return false;
        case PodDB::MarkerRead::OK: {
            if (marker.version != PodDB::FORMAT_VERSION) {
                error = "fnpod database has an unknown format version";
                return false;
            }
            // A valid marker never bypasses record validation: strictly
            // scan the complete namespace (canonical keys/values, exact
            // consumption, identity, sane heights, none above the
            // marker) BEFORE trusting it.
            {
                std::string scan_error;
                if (!db.ScanRecordsStrict([](const int, const Txid&, PodRecord&&) {},
                                          scan_error, marker.height)) {
                    error = scan_error;
                    return false;
                }
            }
            synced = marker.height;
            const CBlockIndex* at{chain[marker.height]};
            if (!at || at->GetBlockHash() != marker.hash) {
                // The marker's block left the active chain (a legacy-era
                // reorganization before the boundary is pinned). Only one
                // (height, hash) pair is stored, so recovery re-derives from
                // genesis: deterministic, never guessing, and cheap relative
                // to how rare pre-pin reorgs are.
                if (!db.RewindTo(0, chain[0]->GetBlockHash(), error)) return false;
                synced = 0;
            } else if (synced > target) {
                // The marker sits ABOVE the newly selected final legacy
                // height (records were derived before H was pinned, on a
                // datadir synced past the eventual boundary). Atomically
                // remove every record and the marker above H; the database
                // then holds exactly the prefix through H inclusive
                // (corrective ruling 2026-08-17).
                if (!db.RewindTo(target, chain[target]->GetBlockHash(), error)) return false;
                synced = target;
            }
            break;
        }
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
        // FAIL CLOSED: a derivation error leaves the database unchanged
        // for this height — the persisted set stays the consistent
        // prefix through the last successful marker, never partial.
        std::vector<PodRecord> records;
        if (!DerivePodRecords(block, undo, height, params, records, error)) return false;
        db.WriteHeight(height, pindex->GetBlockHash(), records);
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
        // The native carrier: the worst-case serialized FnClaimActionV1
        // payload (modern/fn.h, the single source of truth) against the
        // segregated proof-area bound. Measurement only — no validation.
        const size_t payload{modern::WorstCaseFnClaimActionPayload(record.funding_scripts.size())};
        report.max_action_payload = std::max(report.max_action_payload, payload);
        if (payload <= modern::MAX_CREATION_ACTION_PAYLOAD) {
            ++report.within_native_bound;
        } else {
            ++report.exceeding_native_bound;
        }
    }
    report.fits_native_action = report.exceeding_native_bound == 0;
    return report;
}

} // namespace node
