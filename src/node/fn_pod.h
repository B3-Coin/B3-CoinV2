// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FN_POD_H
#define B3COIN_NODE_FN_POD_H

#include <consensus/amount.h>
#include <consensus/params.h>
#include <dbwrapper.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

class CBlock;
class CBlockUndo;
class ChainstateManager;

namespace node {

/**
 * Historical Proof-of-Disintegration eligibility records
 * (doc/design/b3-fn-pod.md §8.2). Derived state: recomputable from the
 * blocks and undo data of the prefix ending at H; independent of optional
 * indexes, wallet state and local settings. This module derives and
 * persists records only — no claim transaction, no FN minting, no
 * claimed-flag mutation lives here.
 */

//! Why a qualifying PoD is or is not claimable under the MVP.
enum class PodClaimability : uint8_t {
    SUPPORTED = 0,
    //! At least one distinct funding script is not an MVP-supported
    //! key form (P2PKH / P2PK). Recorded, never claimable, no fallback.
    UNSUPPORTED_FUNDING_SCRIPT = 1,
};

/**
 * One qualifying historical PoD event == one FN eligibility event.
 * The gap is a per-transaction property, so the txid is the PoDId.
 */
struct PodRecord {
    static constexpr int32_t FORMAT_VERSION{1};

    Txid pod_id{};
    int32_t height{0};
    //! The full input/output gap (collateral + any ordinary fee portion).
    CAmount disintegrated{0};
    //! GetFNCollateral(height) at the event's height.
    CAmount tier{0};
    //! DISTINCT raw funding-input scriptPubKeys, deduplicated and sorted
    //! canonically (lexicographic by script bytes). Beneficiary authority
    //! derives from these and nothing else.
    std::vector<std::vector<unsigned char>> funding_scripts;
    bool claimable{false};
    PodClaimability reason{PodClaimability::UNSUPPORTED_FUNDING_SCRIPT};
    //! AUDIT METADATA ONLY: output indices carrying exactly 1 B3. Marker
    //! presence, destination and spent status never affect eligibility or
    //! beneficiary selection.
    std::vector<uint32_t> marker_vouts;

    SERIALIZE_METHODS(PodRecord, obj)
    {
        int32_t version{FORMAT_VERSION};
        READWRITE(version);
        if (version != FORMAT_VERSION) {
            throw std::ios_base::failure("unknown PodRecord format version");
        }
        uint8_t reason_byte{static_cast<uint8_t>(obj.reason)};
        READWRITE(obj.pod_id, obj.height, obj.disintegrated, obj.tier,
                  obj.funding_scripts, obj.claimable, reason_byte, obj.marker_vouts);
        SER_READ(obj, obj.reason = static_cast<PodClaimability>(reason_byte));
    }

    friend bool operator==(const PodRecord& a, const PodRecord& b)
    {
        return a.pod_id == b.pod_id && a.height == b.height &&
               a.disintegrated == b.disintegrated && a.tier == b.tier &&
               a.funding_scripts == b.funding_scripts && a.claimable == b.claimable &&
               a.reason == b.reason && a.marker_vouts == b.marker_vouts;
    }
};

//! Whether `script` is an MVP-supported funding form: frozen byte-exact
//! P2PKH (25 bytes) or P2PK (35/67 bytes, valid key length prefix).
bool IsSupportedFundingScript(std::span<const unsigned char> script);

/**
 * THE single PoD interpretation, shared by every producer: classify one
 * transaction against the consensus detector (non-coinbase, non-coinstake,
 * gap >= legacy::GetFNCollateral(height)). `spent_coins[i]` is the coin
 * consumed by `tx.vin[i]` — the actual spent prevouts available during
 * block processing (connect or undo data; identical bytes). Returns the
 * record for a qualifying PoD, std::nullopt otherwise.
 */
std::optional<PodRecord> ClassifyPod(const CTransaction& tx,
                                     const std::vector<CTxOut>& spent_prevouts,
                                     int height, const Consensus::Params& params);

//! Derive every PodRecord of one connected block (block + its undo data).
std::vector<PodRecord> DerivePodRecords(const CBlock& block, const CBlockUndo& undo,
                                        int height, const Consensus::Params& params);

/**
 * Durable PodRecord storage with a single sync marker (format version,
 * last processed height and block hash). Records key: ('p', height BE32,
 * txid) — deterministic and height-ordered; marker key: ('m'). Every
 * height commits its records and the marker in one atomic batch.
 */
class PodDB
{
public:
    static constexpr int32_t FORMAT_VERSION{1};

    struct Marker {
        int32_t version{FORMAT_VERSION};
        int32_t height{-1};
        uint256 hash{};
        SERIALIZE_METHODS(Marker, obj) { READWRITE(obj.version, obj.height, obj.hash); }
    };

    explicit PodDB(DBParams db_params);

    std::optional<Marker> ReadMarker() const;
    //! Atomically store one height's records and advance the marker.
    void WriteHeight(int height, const uint256& block_hash, const std::vector<PodRecord>& records);
    //! Atomically delete every record above `height` and rewind the marker.
    void RewindTo(int height, const uint256& block_hash);
    //! All records in deterministic (height, txid) key order.
    std::vector<PodRecord> ReadAll();

private:
    CDBWrapper m_db;
};

/**
 * Bring `db` in sync with the active chain's prefix through
 * min(tip, LEGACY_FINAL_HEIGHT), deriving records from stored blocks and
 * undo data — the same bytes every sync mode (live, reindex, trusted
 * replay) produces. Detects a stale marker whose hash left the active
 * chain (legacy-era reorganization before the boundary is pinned), rewinds
 * to the fork point and re-derives; recovery never guesses. Returns false
 * with `error` set if required block or undo data is unavailable (e.g.
 * pruned before derivation — reindex required).
 */
bool SyncPodRecords(ChainstateManager& chainman, PodDB& db, std::string& error);

//! Capacity-gate report over a set of records (b3-fn-pod.md §8.4).
struct PodCapacityReport {
    size_t total_qualifying{0};
    size_t claimable{0};
    std::map<uint8_t, size_t> by_reason;
    size_t max_distinct_funding_scripts{0};
    //! Upper-bound encoded size of one authorization payload for the
    //! largest eligible claim (worst case: uncompressed keys, 72-byte DER).
    size_t max_authorization_payload{0};
    bool fits_b3fp_carrier{true};
};

PodCapacityReport BuildPodCapacityReport(const std::vector<PodRecord>& records);

} // namespace node

#endif // B3COIN_NODE_FN_POD_H
