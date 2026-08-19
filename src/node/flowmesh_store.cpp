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
    for (uint64_t sequence{0}; sequence < marker->next_sequence; ++sequence) {
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
        if (flowmesh::CheckCertificate(entry->cert, marker->domain, seats, threshold) !=
            flowmesh::CertificateCheck::OK) {
            error = strprintf("flowmesh log entry %d certificate invalid", sequence);
            return false;
        }
        flowmesh::FlowMeshState next{state};
        flowmesh::BatchResult result;
        if (flowmesh::ExecuteCandidate(state, marker->domain, last_hash, entry->mb, auth,
                                       deposits, next, result) !=
            flowmesh::CandidateError::NONE) {
            error = strprintf("flowmesh log entry %d fails re-execution", sequence);
            return false;
        }
        state = std::move(next);
        last_hash = hash;
    }
    if (last_hash != marker->last_hash) {
        error = "flowmesh log tip does not match its marker";
        return false;
    }
    return true;
}

} // namespace node
