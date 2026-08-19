// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_store.h>

#include <flowmesh/microblock.h>
#include <hash.h>
#include <streams.h>
#include <util/strencodings.h>

#include <memory>
#include <utility>

namespace node {

namespace {
constexpr uint8_t KEY_MARKER{'m'};
constexpr uint8_t KEY_ENTRY{'e'};
constexpr uint8_t KEY_SNAPSHOT{'s'};
constexpr uint8_t KEY_LOCK{'l'};

//! Hard cap on a stored snapshot blob (strict pre-allocation bound).
constexpr uint64_t SNAPSHOT_MAX_BYTES{uint64_t{1} << 28};

std::pair<uint8_t, uint64_t> EntryKey(const uint64_t sequence)
{
    return {KEY_ENTRY, sequence};
}
std::pair<uint8_t, uint64_t> LockKey(const uint64_t sequence)
{
    return {KEY_LOCK, sequence};
}
} // namespace

uint256 QuorumHash(const std::set<XOnlyPubKey>& seats, const uint64_t threshold)
{
    HashWriter h;
    h << std::string{"b3/flowmesh/quorum/v1"} << threshold
      << static_cast<uint64_t>(seats.size());
    for (const XOnlyPubKey& seat : seats) h << seat; // std::set: canonical order
    return h.GetHash();
}

FlowMeshStore::FlowMeshStore(DBParams db_params) : m_db{std::move(db_params)} {}

namespace {
//! STRICT keyed read: the key must exist, decode to exactly the
//! requested key type, and the value must consume its stored bytes
//! exactly (trailing bytes are corruption, never ignored).
template <typename K, typename V>
bool ReadStrict(CDBWrapper& db, const K& key, V& value, bool& corrupt)
{
    corrupt = false;
    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    it->Seek(key);
    if (!it->Valid()) return false;
    K stored_key;
    if (!it->GetKeyExact(stored_key)) return false; // different/longer key: not ours
    if (!(stored_key == key)) return false;
    if (!it->GetValueExact(value)) {
        corrupt = true;
        return false;
    }
    return true;
}
} // namespace

bool FlowMeshStore::ReadMarker(std::optional<Marker>& out, std::string& error)
{
    out.reset();
    if (!m_db.Exists(KEY_MARKER)) return true;
    Marker marker;
    bool corrupt{false};
    if (!ReadStrict(m_db, KEY_MARKER, marker, corrupt) || marker.version != FORMAT_VERSION) {
        error = "flowmesh log marker is corrupt or from an unknown format";
        return false;
    }
    out = marker;
    return true;
}

bool FlowMeshStore::OpenForDomain(const uint256& domain, const std::set<XOnlyPubKey>& seats,
                                  const uint64_t threshold, std::string& error)
{
    if (!flowmesh::ValidQuorumConfig(seats.size(), threshold)) {
        error = "flowmesh log refused a nonsensical quorum configuration";
        return false;
    }
    std::optional<Marker> marker;
    if (!ReadMarker(marker, error)) return false;
    const uint256 quorum{QuorumHash(seats, threshold)};
    if (!marker) {
        Marker fresh;
        fresh.domain = domain;
        fresh.quorum_hash = quorum;
        CDBBatch batch{m_db};
        batch.Write(KEY_MARKER, fresh);
        m_db.WriteBatch(batch, /*fSync=*/true); // throws on database failure
        return true;
    }
    if (marker->domain != domain) {
        error = "flowmesh log belongs to a different domain";
        return false;
    }
    if (marker->quorum_hash != quorum) {
        // Deliberate fail-closed boundary: re-judging persisted
        // certificates against a DIFFERENT quorum needs the seat
        // lifecycle rules — an unresolved owner decision.
        error = "flowmesh log was certified under a different quorum configuration";
        return false;
    }
    return true;
}

bool FlowMeshStore::Append(const flowmesh::CertifiedEntry& entry, std::string& error)
{
    std::optional<Marker> marker;
    if (!ReadMarker(marker, error)) return false;
    if (!marker) {
        error = "flowmesh log is not initialized";
        return false;
    }
    if (entry.mb.sequence != marker->next_sequence) {
        error = "flowmesh log append out of order";
        return false;
    }
    if (entry.mb.parent_hash != marker->last_hash) {
        error = "flowmesh log append does not extend the stored tip";
        return false;
    }
    Marker next{*marker};
    next.next_sequence = marker->next_sequence + 1;
    next.last_hash = entry.mb.GetHash();

    try {
        CDBBatch batch{m_db};
        batch.Write(EntryKey(entry.mb.sequence), entry);
        batch.Write(KEY_MARKER, next);
        m_db.WriteBatch(batch, /*fSync=*/true); // atomic
    } catch (const std::exception& e) {
        error = std::string{"flowmesh log append failed: "} + e.what();
        return false;
    }
    return true;
}

std::optional<flowmesh::CertifiedEntry> FlowMeshStore::ReadEntry(const uint64_t sequence)
{
    flowmesh::CertifiedEntry entry;
    bool corrupt{false};
    if (!ReadStrict(m_db, EntryKey(sequence), entry, corrupt)) return std::nullopt;
    return entry;
}

bool FlowMeshStore::WriteLock(const uint64_t sequence, const uint256& microblock_hash)
{
    try {
        CDBBatch batch{m_db};
        batch.Write(LockKey(sequence), microblock_hash);
        m_db.WriteBatch(batch, /*fSync=*/true);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool FlowMeshStore::ClearLocksThrough(const uint64_t sequence)
{
    try {
        std::map<uint64_t, uint256> locks;
        std::string error;
        if (!ReadLocks(locks, error)) return false;
        CDBBatch batch{m_db};
        for (const auto& [seq, hash] : locks) {
            if (seq <= sequence) batch.Erase(LockKey(seq));
        }
        m_db.WriteBatch(batch, /*fSync=*/true);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool FlowMeshStore::ReadLocks(std::map<uint64_t, uint256>& out, std::string& error)
{
    out.clear();
    std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    for (it->Seek(std::make_pair(KEY_LOCK, uint64_t{0})); it->Valid(); it->Next()) {
        uint8_t prefix;
        if (!it->GetKey(prefix) || prefix != KEY_LOCK) break;
        std::pair<uint8_t, uint64_t> key;
        uint256 hash;
        if (!it->GetKeyExact(key) || !it->GetValueExact(hash)) {
            error = "flowmesh lock journal is corrupt";
            out.clear();
            return false;
        }
        out.emplace(key.second, hash);
    }
    return true;
}

bool FlowMeshStore::Replay(flowmesh::FlowMeshState& state, uint256& last_hash,
                           const flowmesh::ActionAuthenticator& auth,
                           const flowmesh::DepositVerifier* deposits,
                           const std::set<XOnlyPubKey>& seats, const uint64_t threshold,
                           const flowmesh::AnchorPolicy* anchors, std::string& error)
{
    std::optional<Marker> marker;
    if (!ReadMarker(marker, error)) return false;
    if (!marker) {
        error = "flowmesh log is not initialized";
        return false;
    }
    if (marker->quorum_hash != QuorumHash(seats, threshold)) {
        error = "flowmesh log was certified under a different quorum configuration";
        return false;
    }
    last_hash = uint256{};
    return ReplayRange(state, last_hash, /*from_sequence=*/0, *marker, auth, deposits, seats,
                       threshold, anchors, error);
}

bool FlowMeshStore::ReplayRange(flowmesh::FlowMeshState& state, uint256& last_hash,
                                const uint64_t from_sequence, const Marker& marker,
                                const flowmesh::ActionAuthenticator& auth,
                                const flowmesh::DepositVerifier* deposits,
                                const std::set<XOnlyPubKey>& seats, const uint64_t threshold,
                                const flowmesh::AnchorPolicy* anchors, std::string& error)
{
    if (!flowmesh::ValidQuorumConfig(seats.size(), threshold)) {
        error = "flowmesh replay refused a nonsensical quorum configuration";
        return false;
    }
    for (uint64_t sequence{from_sequence}; sequence < marker.next_sequence; ++sequence) {
        const std::optional<flowmesh::CertifiedEntry> entry{ReadEntry(sequence)};
        if (!entry) {
            error = strprintf("flowmesh log entry %d is missing or corrupt", sequence);
            return false;
        }
        const uint256 hash{entry->mb.GetHash()};
        if (entry->cert.microblock_hash != hash || entry->cert.sequence != sequence) {
            error = strprintf("flowmesh log entry %d certificate mismatch", sequence);
            return false;
        }
        if (flowmesh::CheckCertificate(entry->cert, marker.domain, seats, threshold) !=
            flowmesh::CertificateCheck::OK) {
            error = strprintf("flowmesh log entry %d certificate invalid", sequence);
            return false;
        }
        if (anchors != nullptr && !anchors->StillCanonical(entry->mb.anchor)) {
            // Certified history that relies on an orphaned B3 anchor
            // must not silently remain accepted; treatment beyond this
            // fail-safe refusal is an OWNER DECISION.
            error = strprintf("flowmesh log entry %d relies on a non-canonical B3 anchor",
                              sequence);
            return false;
        }
        flowmesh::FlowMeshState next{state};
        flowmesh::BatchResult result;
        if (flowmesh::ExecuteCandidate(state, marker.domain, last_hash, entry->mb, auth,
                                       deposits, next, result) !=
            flowmesh::CandidateError::NONE) {
            error = strprintf("flowmesh log entry %d fails re-execution", sequence);
            return false;
        }
        state = std::move(next);
        last_hash = hash;
    }
    if (last_hash != marker.last_hash) {
        error = "flowmesh log tip does not match its marker";
        return false;
    }
    return true;
}

bool FlowMeshStore::WriteSnapshot(const uint64_t upto_sequence,
                                  const flowmesh::FlowMeshState& state, std::string& error)
{
    std::optional<Marker> marker;
    if (!ReadMarker(marker, error)) return false;
    if (!marker) {
        error = "flowmesh log is not initialized";
        return false;
    }
    if (upto_sequence == 0 || upto_sequence > marker->next_sequence) {
        error = "flowmesh snapshot sequence is outside the stored log";
        return false;
    }
    const std::optional<flowmesh::CertifiedEntry> tip_entry{ReadEntry(upto_sequence - 1)};
    if (!tip_entry) {
        error = "flowmesh snapshot tip entry is missing";
        return false;
    }
    if (state.Root() != tip_entry->mb.resulting_state_root) {
        error = "flowmesh snapshot does not match the certified state at its sequence";
        return false;
    }
    DataStream body;
    body << state;
    if (body.size() > SNAPSHOT_MAX_BYTES) {
        error = "flowmesh snapshot exceeds the storage bound";
        return false;
    }
    try {
        CDBBatch batch{m_db};
        batch.Write(KEY_SNAPSHOT,
                    std::make_pair(upto_sequence,
                                   std::vector<unsigned char>{UCharCast(body.data()),
                                                              UCharCast(body.data()) +
                                                                  body.size()}));
        m_db.WriteBatch(batch, /*fSync=*/true);
    } catch (const std::exception& e) {
        error = std::string{"flowmesh snapshot write failed: "} + e.what();
        return false;
    }
    return true;
}

bool FlowMeshStore::ReplayFromBestSnapshot(flowmesh::FlowMeshState& state, uint256& last_hash,
                                           const flowmesh::ActionAuthenticator& auth,
                                           const flowmesh::DepositVerifier* deposits,
                                           const std::set<XOnlyPubKey>& seats,
                                           const uint64_t threshold,
                                           const flowmesh::AnchorPolicy* anchors,
                                           std::string& error)
{
    std::optional<Marker> marker;
    if (!ReadMarker(marker, error)) return false;
    if (!marker) {
        error = "flowmesh log is not initialized";
        return false;
    }
    if (marker->quorum_hash != QuorumHash(seats, threshold)) {
        error = "flowmesh log was certified under a different quorum configuration";
        return false;
    }

    // Try the snapshot; any defect discards it and falls back to the
    // full verified replay from genesis. A snapshot is only ever
    // trusted through CERTIFIED history: its tip entry's certificate is
    // re-verified against the recorded quorum, its root must equal that
    // entry's certified resulting root, and its tip anchor must still
    // be canonical.
    std::pair<uint64_t, std::vector<unsigned char>> stored;
    bool corrupt{false};
    if (ReadStrict(m_db, KEY_SNAPSHOT, stored, corrupt) && stored.first > 0 &&
        stored.first <= marker->next_sequence &&
        stored.second.size() <= SNAPSHOT_MAX_BYTES) {
        const uint64_t upto{stored.first};
        const std::optional<flowmesh::CertifiedEntry> tip_entry{ReadEntry(upto - 1)};
        if (tip_entry && tip_entry->cert.microblock_hash == tip_entry->mb.GetHash() &&
            tip_entry->cert.sequence == upto - 1 &&
            flowmesh::CheckCertificate(tip_entry->cert, marker->domain, seats, threshold) ==
                flowmesh::CertificateCheck::OK &&
            (anchors == nullptr || anchors->StillCanonical(tip_entry->mb.anchor))) {
            flowmesh::FlowMeshState candidate{state};
            bool decoded{false};
            try {
                DataStream body{std::span{stored.second}};
                body >> candidate;
                decoded = body.empty(); // strict: full consumption
            } catch (const std::exception&) {
                decoded = false;
            }
            if (decoded && candidate.Root() == tip_entry->mb.resulting_state_root) {
                flowmesh::FlowMeshState next{std::move(candidate)};
                uint256 tail_hash{tip_entry->mb.GetHash()};
                if (ReplayRange(next, tail_hash, upto, *marker, auth, deposits, seats,
                                threshold, anchors, error)) {
                    state = std::move(next);
                    last_hash = tail_hash;
                    return true;
                }
            }
        }
    }

    // Fallback: full verified replay from genesis (state still carries
    // the genesis configuration the caller constructed it with).
    last_hash = uint256{};
    return ReplayRange(state, last_hash, 0, *marker, auth, deposits, seats, threshold, anchors,
                      error);
}

} // namespace node
