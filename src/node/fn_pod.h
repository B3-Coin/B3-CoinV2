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
#include <functional>
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
 *
 * INTEGRATION STATUS (honest, under the RULED model): the sync helper
 * and the offline replay path exist and are tested (unit fixtures + the
 * regtest evolution suite + b3coin-utxo-verify -podreport). Under the
 * owner's 2026-08-17 FN issuance ruling (doc/design/
 * b3-legacy-fn-issuance-proposal.md) this module is BUILDER-SIDE AND
 * AUDIT TOOLING ONLY: historical FN rights are issued by ONE archival
 * builder as proof-carrying issuance transactions that every node
 * verifies STATELESSLY against the H/X-sealed chain. Normal production
 * sync/reindex does NOT derive PodRecords and there is NO production
 * PodDB by design — the earlier "every node scans and keeps a PodDB;
 * users claim with funding-key signatures" model (this document's §8
 * scan-and-claim MVP) is SUPERSEDED, not pending.
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

/**
 * Derive every PodRecord of one connected block (block + its undo data).
 * FAILS CLOSED (corrective + hardening rulings 2026-08-17): missing,
 * truncated, malformed or mismatched undo data — a wrong per-block
 * entry count, a wrong per-transaction spent-coin count, a spent/null
 * undo Coin, an out-of-range amount or an out-of-range input-value sum
 * — returns false with `error` set and leaves the caller's `out`
 * vector COMPLETELY UNCHANGED; a partial set is never produced. This
 * validates STRUCTURE and obviously invalid Coins only: disk checksums
 * detect stored-byte corruption, and SEMANTIC provenance of undo
 * content comes from validated ConnectBlock undo data or TrustedReplay
 * reconstruction — never from this function.
 * Callers (sync, replay, -podreport) must propagate the failure and
 * leave any persisted state unchanged for the failed height.
 */
bool DerivePodRecords(const CBlock& block, const CBlockUndo& undo, int height,
                      const Consensus::Params& params, std::vector<PodRecord>& out,
                      std::string& error);

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

    //! Marker recovery distinguishes ABSENCE from DAMAGE: a present but
    //! undecodable marker (wrong bytes OR trailing bytes — decoding is
    //! full-consumption) is CORRUPT and must fail closed, never be
    //! mistaken for missing data.
    enum class MarkerRead { MISSING, OK, CORRUPT };
    MarkerRead ReadMarkerChecked(Marker& out);
    //! The marker when present and canonical; nullopt when truly
    //! missing. THROWS on CORRUPT — corruption is never flattened into
    //! "missing".
    std::optional<Marker> ReadMarker();
    //! Atomically store one height's records and advance the marker.
    void WriteHeight(int height, const uint256& block_hash, const std::vector<PodRecord>& records);
    /**
     * Strictly walk the COMPLETE record namespace from the raw 'p'
     * prefix: exact/full-consumption key and value decoding, decoded
     * record identity (height, PoDId) matching its key, and a clean
     * iterator status at termination. Returns false + error on any
     * violation without invoking `fn` further; used by every reader so
     * a partial or corrupt namespace can never masquerade as data.
     */
    bool ScanRecordsStrict(
        const std::function<void(int height, const Txid& txid, PodRecord&&)>& fn,
        std::string& error, std::optional<int> max_height = std::nullopt);
    //! Atomically delete every record above `height` and rewind the
    //! marker. The EXISTING marker and the FULL record namespace are
    //! validated BEFORE any deletion or the new marker is committed: a
    //! missing or corrupt marker, or any namespace violation, fails
    //! with nothing written and the original marker and records
    //! preserved.
    bool RewindTo(int height, const uint256& block_hash, std::string& error);
    //! All records in deterministic (height, txid) key order. THROWS on
    //! any namespace violation — never returns a partial set.
    std::vector<PodRecord> ReadAll();

private:
    CDBWrapper m_db;
};

/**
 * Bring `db` in sync with the active chain's prefix through
 * min(tip, LEGACY_FINAL_HEIGHT), deriving records from stored blocks and
 * undo data — the same bytes every sync mode (live, reindex, trusted
 * replay) produces. Detects a stale marker whose hash left the active
 * chain (legacy-era reorganization before the boundary is pinned) and
 * safely rewinds to GENESIS before re-deriving — deterministic, never
 * guessing (a fork-point rewind would be an optimization only). A marker
 * ABOVE the newly selected final legacy height (records derived before H
 * was pinned) is atomically rewound to H, so the database holds exactly
 * the prefix through H inclusive (corrective ruling 2026-08-17). Returns
 * false with `error` set if required block or undo data is unavailable
 * (e.g. pruned — reindex required) or inconsistent (DerivePodRecords
 * fails closed); on failure the database is unchanged for the failed
 * height — the persisted set is always a consistent prefix, never
 * partial.
 */
bool SyncPodRecords(ChainstateManager& chainman, PodDB& db, std::string& error);

//! Offline PoD report over a set of records. The qualifying-PoD counts
//! (the R counter against MAX_FN_EVER_ISSUED) remain the meaningful
//! pre-activation gate; the payload arithmetic below is a SUPERSEDED
//! type-1 diagnostic (see the PodCapacityReport banner) — NOT the
//! type-2 issuance activation capacity gate. Offline measurement,
//! never validation.
/**
 * SUPERSEDED MEASUREMENT — NON-AUTHORITATIVE FOR ACTIVATION (owner
 * correction 2026-08-18). This report's payload arithmetic measures
 * `WorstCaseFnClaimActionPayload`, the worst case of the ABANDONED
 * type-1 funding-signature claim encoding (`FnClaimActionV1`). It is
 * NOT the capacity gate for the live type-2 issuance carrier
 * (`LegacyFnIssuanceActionV1`), whose real encoded sizes over actual
 * mainnet history REMAIN UNMEASURED — that measurement is recorded
 * future work, and FN activation stays blocked until it exists or a
 * reviewed versioned carrier is selected. The qualifying-PoD counts
 * (the R counter against MAX_FN_EVER_ISSUED) remain meaningful.
 */
struct PodCapacityReport {
    size_t total_qualifying{0};
    size_t claimable{0};
    std::map<uint8_t, size_t> by_reason;
    size_t max_distinct_funding_scripts{0};
    //! SUPERSEDED: worst-case serialized payload of the ABANDONED
    //! FnClaimActionV1 (every record P2PKH, uncompressed key, 72-byte
    //! DER) — kept as the historical measurement record only.
    size_t max_action_payload{0};
    //! SUPERSEDED: fit of the abandoned type-1 payload against the
    //! 4,000-byte bound. Says nothing about the type-2 issuance proof.
    size_t within_native_bound{0};
    size_t exceeding_native_bound{0};
    //! SUPERSEDED verdict over the abandoned encoding.
    bool fits_native_action{true};
};

PodCapacityReport BuildPodCapacityReport(const std::vector<PodRecord>& records);

} // namespace node

#endif // B3COIN_NODE_FN_POD_H
