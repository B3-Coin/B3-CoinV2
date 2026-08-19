// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/flowmesh_store.h>

#include <flowmesh/microblock.h>
#include <streams.h>
#include <util/strencodings.h>

#include <utility>

namespace node {

namespace {
constexpr uint8_t KEY_MARKER{'m'};
constexpr uint8_t KEY_ENTRY{'e'};
constexpr uint8_t KEY_SNAPSHOT{'s'};

std::pair<uint8_t, uint64_t> EntryKey(const uint64_t sequence)
{
    return {KEY_ENTRY, sequence};
}
} // namespace

FlowMeshStore::FlowMeshStore(DBParams db_params) : m_db{std::move(db_params)} {}

bool FlowMeshStore::ReadMarker(std::optional<Marker>& out, std::string& error)
{
    out.reset();
    if (!m_db.Exists(KEY_MARKER)) return true;
    Marker marker;
    if (!m_db.Read(KEY_MARKER, marker) || marker.version != FORMAT_VERSION) {
        error = "flowmesh log marker is corrupt or from an unknown format";
        return false;
    }
    out = marker;
    return true;
}

bool FlowMeshStore::OpenForDomain(const uint256& domain, std::string& error)
{
    std::optional<Marker> marker;
    if (!ReadMarker(marker, error)) return false;
    if (!marker) {
        Marker fresh;
        fresh.domain = domain;
        CDBBatch batch{m_db};
        batch.Write(KEY_MARKER, fresh);
        m_db.WriteBatch(batch, /*fSync=*/true); // throws on database failure
        return true;
    }
    if (marker->domain != domain) {
        error = "flowmesh log belongs to a different domain";
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

    CDBBatch batch{m_db};
    batch.Write(EntryKey(entry.mb.sequence), entry);
    batch.Write(KEY_MARKER, next);
    m_db.WriteBatch(batch, /*fSync=*/true); // atomic; throws on database failure
    return true;
}

std::optional<flowmesh::CertifiedEntry> FlowMeshStore::ReadEntry(const uint64_t sequence)
{
    flowmesh::CertifiedEntry entry;
    if (!m_db.Read(EntryKey(sequence), entry)) return std::nullopt;
    return entry;
}

bool FlowMeshStore::Replay(flowmesh::FlowMeshState& state, uint256& last_hash,
                           const flowmesh::ActionAuthenticator& auth,
                           const flowmesh::DepositVerifier* deposits,
                           const std::set<XOnlyPubKey>& seats, const uint64_t threshold,
                           std::string& error)
{
    std::optional<Marker> marker;
    if (!ReadMarker(marker, error)) return false;
    if (!marker) {
        error = "flowmesh log is not initialized";
        return false;
    }
    last_hash = uint256{};
    return ReplayRange(state, last_hash, /*from_sequence=*/0, *marker, auth, deposits, seats,
                       threshold, error);
}

bool FlowMeshStore::ReplayRange(flowmesh::FlowMeshState& state, uint256& last_hash,
                                const uint64_t from_sequence, const Marker& marker,
                                const flowmesh::ActionAuthenticator& auth,
                                const flowmesh::DepositVerifier* deposits,
                                const std::set<XOnlyPubKey>& seats, const uint64_t threshold,
                                std::string& error)
{
    for (uint64_t sequence{from_sequence}; sequence < marker.next_sequence; ++sequence) {
        const std::optional<flowmesh::CertifiedEntry> entry{ReadEntry(sequence)};
        if (!entry) {
            error = strprintf("flowmesh log entry %d is missing", sequence);
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
    CDBBatch batch{m_db};
    batch.Write(KEY_SNAPSHOT,
                std::make_pair(upto_sequence,
                               std::vector<unsigned char>{UCharCast(body.data()),
                                                          UCharCast(body.data()) + body.size()}));
    m_db.WriteBatch(batch, /*fSync=*/true);
    return true;
}

bool FlowMeshStore::ReplayFromBestSnapshot(flowmesh::FlowMeshState& state, uint256& last_hash,
                                           const flowmesh::ActionAuthenticator& auth,
                                           const flowmesh::DepositVerifier* deposits,
                                           const std::set<XOnlyPubKey>& seats,
                                           const uint64_t threshold, std::string& error)
{
    std::optional<Marker> marker;
    if (!ReadMarker(marker, error)) return false;
    if (!marker) {
        error = "flowmesh log is not initialized";
        return false;
    }

    // Try the snapshot; any defect discards it and falls back to the
    // full deterministic replay from genesis.
    std::pair<uint64_t, std::vector<unsigned char>> stored;
    if (m_db.Read(KEY_SNAPSHOT, stored) && stored.first > 0 &&
        stored.first <= marker->next_sequence) {
        const uint64_t upto{stored.first};
        const std::optional<flowmesh::CertifiedEntry> tip_entry{ReadEntry(upto - 1)};
        if (tip_entry && tip_entry->cert.microblock_hash == tip_entry->mb.GetHash() &&
            tip_entry->cert.sequence == upto - 1 &&
            flowmesh::CheckCertificate(tip_entry->cert, marker->domain, seats, threshold) ==
                flowmesh::CertificateCheck::OK) {
            flowmesh::FlowMeshState candidate{state};
            bool decoded{false};
            try {
                DataStream body{std::span{stored.second}};
                body >> candidate;
                decoded = body.empty(); // full consumption, like every strict codec here
            } catch (const std::exception&) {
                decoded = false;
            }
            // The decoded state is untrusted until its root equals the
            // CERTIFIED resulting root at the snapshot sequence.
            if (decoded && candidate.Root() == tip_entry->mb.resulting_state_root) {
                flowmesh::FlowMeshState next{std::move(candidate)};
                uint256 tail_hash{tip_entry->mb.GetHash()};
                if (ReplayRange(next, tail_hash, upto, *marker, auth, deposits, seats,
                                threshold, error)) {
                    state = std::move(next);
                    last_hash = tail_hash;
                    return true;
                }
            }
        }
    }

    // Fallback: full replay from genesis (state still carries the
    // genesis configuration the caller constructed it with).
    last_hash = uint256{};
    return ReplayRange(state, last_hash, 0, *marker, auth, deposits, seats, threshold, error);
}

} // namespace node
