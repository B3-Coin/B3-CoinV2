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

//! Hard cap on a stored snapshot blob, enforced BEFORE allocation.
constexpr uint64_t SNAPSHOT_MAX_BYTES{uint64_t{1} << 28};

//! Hard production ceiling on configured lock-journal bounds.
constexpr size_t HARD_MAX_LOCK_JOURNAL_ENTRIES{4096};

std::pair<uint8_t, uint64_t> EntryKey(const uint64_t sequence)
{
    return {KEY_ENTRY, sequence};
}
std::pair<uint8_t, uint64_t> LockKey(const uint64_t sequence)
{
    return {KEY_LOCK, sequence};
}

//! Snapshot record with a TRUE pre-allocation bound on the state blob.
struct SnapshotRecord {
    uint64_t upto_sequence{0};
    std::vector<unsigned char> state_bytes;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s << upto_sequence;
        WriteCompactSize(s, state_bytes.size());
        if (!state_bytes.empty()) {
            s.write(std::as_bytes(std::span{state_bytes.data(), state_bytes.size()}));
        }
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> upto_sequence;
        const uint64_t len{ReadCompactSize(s)};
        if (len > SNAPSHOT_MAX_BYTES) {
            throw std::ios_base::failure("flowmesh snapshot blob too large");
        }
        state_bytes.resize(len);
        if (len > 0) {
            s.read(std::as_writable_bytes(std::span{state_bytes.data(), state_bytes.size()}));
        }
    }
};

//! STRICT keyed read with an explicit tri-state result: FOUND (exact
//! key, value consumed exactly), NOT_FOUND (key genuinely absent), or
//! ERROR (iterator/storage failure or an undecodable value). A storage
//! error is NEVER interpreted as "key missing".
enum class ReadResult : uint8_t { FOUND, NOT_FOUND, ERROR };

template <typename K, typename V>
ReadResult ReadStrict(CDBWrapper& db, const K& key, V& value)
{
    std::unique_ptr<CDBIterator> it{db.NewIterator()};
    it->Seek(key);
    if (!it->Valid()) {
        return it->StatusOK() ? ReadResult::NOT_FOUND : ReadResult::ERROR;
    }
    K stored_key;
    if (!it->GetKeyExact(stored_key) || !(stored_key == key)) {
        return it->StatusOK() ? ReadResult::NOT_FOUND : ReadResult::ERROR;
    }
    if (!it->GetValueExact(value)) return ReadResult::ERROR;
    return ReadResult::FOUND;
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

FlowMeshStore::FlowMeshStore(DBParams db_params, const size_t max_lock_entries)
    : m_db{std::move(db_params)}, m_max_lock_entries{max_lock_entries}
{
    // The journal bound is a REAL hard limit: zero would brick signing,
    // an arbitrarily huge value would defeat the bound's purpose.
    if (m_max_lock_entries == 0 || m_max_lock_entries > HARD_MAX_LOCK_JOURNAL_ENTRIES) {
        throw std::invalid_argument("flowmesh lock-journal bound outside [1, 4096]");
    }
}

bool FlowMeshStore::ReadMarker(std::optional<Marker>& out, std::string& error)
{
    out.reset();
    Marker marker;
    switch (ReadStrict(m_db, KEY_MARKER, marker)) {
    case ReadResult::NOT_FOUND:
        return true; // genuinely absent
    case ReadResult::ERROR:
        error = "flowmesh log marker is corrupt or unreadable";
        return false;
    case ReadResult::FOUND:
        break;
    }
    if (marker.version != FORMAT_VERSION) {
        error = "flowmesh log marker is from an unknown format";
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
        // A missing marker means FRESH only when every FLOWMESH
        // namespace ('m'/'e'/'s'/'l' first byte) is empty — entries,
        // locks or snapshots without a marker are inconsistent (torn or
        // tampered) storage. Keys outside those namespaces (database
        // implementation metadata such as the obfuscation key) are NOT
        // FlowMesh state and are ignored.
        std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
        for (it->SeekToFirst(); it->Valid(); it->Next()) {
            uint8_t prefix{0};
            if (!it->GetKey(prefix)) continue; // not even a 1-byte-prefixed key
            if (prefix == KEY_MARKER || prefix == KEY_ENTRY || prefix == KEY_SNAPSHOT ||
                prefix == KEY_LOCK) {
                error = "flowmesh storage has data but no marker: inconsistent or corrupt";
                return false;
            }
        }
        if (!it->StatusOK()) {
            error = "flowmesh storage iterator failed while checking freshness";
            return false;
        }
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
        // Fail-closed boundary: re-judging persisted certificates under
        // a DIFFERENT quorum needs the seat-lifecycle rules — an
        // unresolved owner decision.
        error = "flowmesh log was certified under a different quorum configuration";
        return false;
    }
    // STRICT validation of the COMPLETE entry-key namespace, from the
    // RAW one-byte 'e' prefix (a malformed SHORT key sorts before every
    // well-formed entry key and must be seen, not skipped): every key
    // must decode exactly, and no key may lie at or beyond the
    // authoritative tip — stray/beyond-tip entries cannot arise from
    // the atomic append and are tamper or corruption, never silently
    // ignored. No rollback rule is defined, so any violation fails
    // closed. (Entry VALUES are fully validated by replay.)
    {
        std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
        for (it->Seek(uint8_t{KEY_ENTRY}); it->Valid(); it->Next()) {
            uint8_t prefix{0};
            if (!it->GetKey(prefix) || prefix != KEY_ENTRY) break; // past the namespace
            std::pair<uint8_t, uint64_t> entry_key;
            if (!it->GetKeyExact(entry_key)) {
                error = "flowmesh log entry namespace holds a malformed key";
                return false;
            }
            if (entry_key.second >= marker->next_sequence) {
                error = "flowmesh log holds entries beyond its authoritative tip";
                return false;
            }
        }
        if (!it->StatusOK()) {
            error = "flowmesh storage iterator failed while probing the log tail";
            return false;
        }
    }
    return true;
}

bool FlowMeshStore::Append(const flowmesh::CertifiedEntry& entry, std::string& error)
{
    const std::lock_guard<std::mutex> guard{m_write_mutex};
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
    if (ReadStrict(m_db, EntryKey(sequence), entry) != ReadResult::FOUND) return std::nullopt;
    return entry;
}

bool FlowMeshStore::WriteLock(const uint64_t sequence, const uint256& microblock_hash)
{
    // SERIALIZED COMPARE-AND-SET: the read-check-write runs under one
    // mutex so no concurrent caller can interleave, and a storage ERROR
    // is refusal — never treated as "absent" and overwritten.
    const std::lock_guard<std::mutex> guard{m_write_mutex};
    try {
        // ONE strict scan of the COMPLETE lock namespace before ANY
        // decision — including the idempotent path: every key and every
        // value must decode exactly, the entry bound is enforced, and
        // the target's existing value (if any) is captured on the way.
        // A validator never signs over a corrupt or over-full journal.
        std::optional<uint256> existing;
        size_t count{0};
        {
            std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
            for (it->Seek(uint8_t{KEY_LOCK}); it->Valid(); it->Next()) {
                uint8_t prefix{0};
                if (!it->GetKey(prefix) || prefix != KEY_LOCK) break;
                std::pair<uint8_t, uint64_t> lock_key;
                uint256 lock_hash;
                if (!it->GetKeyExact(lock_key) || !it->GetValueExact(lock_hash)) {
                    return false; // corrupt journal: never sign
                }
                if (lock_key.second == sequence) existing = lock_hash;
                if (++count > m_max_lock_entries) return false; // over-full: never sign
            }
            if (!it->StatusOK()) return false;
        }
        if (existing.has_value()) {
            return *existing == microblock_hash; // idempotent same; refuse different
        }
        if (count >= m_max_lock_entries) return false; // full: refuse a NEW entry
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
    const std::lock_guard<std::mutex> guard{m_write_mutex};
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
    // Seek to the RAW one-byte 'l' prefix so a malformed SHORT
    // 'l'-prefixed key (which sorts before every well-formed lock key)
    // is seen and rejected instead of silently skipped.
    std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    for (it->Seek(uint8_t{KEY_LOCK}); it->Valid(); it->Next()) {
        uint8_t prefix;
        if (!it->GetKey(prefix) || prefix != KEY_LOCK) break; // next namespace
        std::pair<uint8_t, uint64_t> key;
        uint256 hash;
        if (!it->GetKeyExact(key) || !it->GetValueExact(hash)) {
            error = "flowmesh lock journal is corrupt";
            out.clear();
            return false;
        }
        out.emplace(key.second, hash);
        if (out.size() > m_max_lock_entries) {
            error = "flowmesh lock journal exceeds its entry bound";
            out.clear();
            return false;
        }
    }
    if (!it->StatusOK()) {
        error = "flowmesh lock journal iterator failed (I/O or checksum error)";
        out.clear();
        return false;
    }
    return true;
}

bool FlowMeshStore::Replay(flowmesh::FlowMeshState& state, uint256& last_hash,
                           const flowmesh::ActionAuthenticator& auth,
                           const flowmesh::DepositVerifier* deposits,
                           const std::set<XOnlyPubKey>& seats, const uint64_t threshold,
                           const flowmesh::AnchorPolicy* anchors, std::string& error,
                           std::map<std::pair<int32_t, uint256>, uint64_t>* anchors_out)
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
    if (auth.DomainId() != marker->domain || auth.ExecConfigId() != state.ConfigId()) {
        error = "flowmesh replay authenticator is bound to a different domain/configuration";
        return false;
    }
    last_hash = uint256{};
    return ReplayRange(state, last_hash, /*from_sequence=*/0, *marker, auth, deposits, seats,
                       threshold, anchors, error, anchors_out);
}

bool FlowMeshStore::ReplayRange(flowmesh::FlowMeshState& state, uint256& last_hash,
                                const uint64_t from_sequence, const Marker& marker,
                                const flowmesh::ActionAuthenticator& auth,
                                const flowmesh::DepositVerifier* deposits,
                                const std::set<XOnlyPubKey>& seats, const uint64_t threshold,
                                const flowmesh::AnchorPolicy* anchors, std::string& error,
                                std::map<std::pair<int32_t, uint256>, uint64_t>* anchors_out)
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
            error = strprintf("flowmesh log entry %d relies on a non-canonical B3 anchor",
                              sequence);
            return false;
        }
        if (!flowmesh::VerifyActionEvidence(entry->mb, entry->credentials, auth)) {
            error = strprintf("flowmesh log entry %d admission evidence invalid", sequence);
            return false;
        }
        flowmesh::FlowMeshState next{state};
        flowmesh::BatchResult result;
        if (flowmesh::ExecuteCandidate(state, marker.domain, last_hash, entry->mb, deposits,
                                       next, result) != flowmesh::CandidateError::NONE) {
            error = strprintf("flowmesh log entry %d fails re-execution", sequence);
            return false;
        }
        state = std::move(next);
        last_hash = hash;
        if (anchors_out != nullptr && !entry->mb.anchor.IsNull()) {
            (*anchors_out)[{entry->mb.anchor.height, entry->mb.anchor.hash}] = sequence;
        }
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
    const std::lock_guard<std::mutex> guard{m_write_mutex};
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
    SnapshotRecord record;
    record.upto_sequence = upto_sequence;
    DataStream body;
    body << state;
    if (body.size() > SNAPSHOT_MAX_BYTES) {
        error = "flowmesh snapshot exceeds the storage bound";
        return false;
    }
    record.state_bytes.assign(UCharCast(body.data()), UCharCast(body.data()) + body.size());
    try {
        CDBBatch batch{m_db};
        batch.Write(KEY_SNAPSHOT, record);
        m_db.WriteBatch(batch, /*fSync=*/true);
    } catch (const std::exception& e) {
        error = std::string{"flowmesh snapshot write failed: "} + e.what();
        return false;
    }
    return true;
}

bool FlowMeshStore::ReplayFromBestSnapshot(
    flowmesh::FlowMeshState& state, uint256& last_hash,
    const flowmesh::ActionAuthenticator& auth, const flowmesh::DepositVerifier* deposits,
    const std::set<XOnlyPubKey>& seats, const uint64_t threshold,
    const flowmesh::AnchorPolicy* anchors, std::string& error,
    std::map<std::pair<int32_t, uint256>, uint64_t>* anchors_out)
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
    if (auth.DomainId() != marker->domain || auth.ExecConfigId() != state.ConfigId()) {
        error = "flowmesh replay authenticator is bound to a different domain/configuration";
        return false;
    }

    // Try the snapshot; any defect discards it and falls back to the
    // full verified replay from genesis. A snapshot is only trusted
    // through CERTIFIED history: the tip entry's certificate is
    // re-verified, the decoded root must equal that entry's certified
    // resulting root, AND the FULL SKIPPED PREFIX is authenticated by
    // walking the parent-hash chain up to the certified tip — which
    // proves every prefix entry's bytes, so every prefix B3 anchor can
    // be (and is) revalidated without re-execution. A snapshot can
    // never hide state derived from a now-orphaned earlier anchor.
    SnapshotRecord stored;
    if (ReadStrict(m_db, KEY_SNAPSHOT, stored) == ReadResult::FOUND &&
        stored.upto_sequence > 0 &&
        stored.upto_sequence <= marker->next_sequence) {
        const uint64_t upto{stored.upto_sequence};
        const std::optional<flowmesh::CertifiedEntry> tip_entry{ReadEntry(upto - 1)};
        bool prefix_ok{tip_entry.has_value()};
        std::map<std::pair<int32_t, uint256>, uint64_t> prefix_anchors;
        if (prefix_ok) {
            prefix_ok = tip_entry->cert.microblock_hash == tip_entry->mb.GetHash() &&
                        tip_entry->cert.sequence == upto - 1 &&
                        flowmesh::CheckCertificate(tip_entry->cert, marker->domain, seats,
                                                   threshold) ==
                            flowmesh::CertificateCheck::OK;
        }
        if (prefix_ok) {
            // FULL prefix authentication (a valid tip certificate is
            // NOT proof the skipped prefix was valid): exact
            // parent-hash chain, EVERY prefix certificate against the
            // recorded quorum (the log's historical seat context —
            // rotation is unsupported pending the seat-lifecycle owner
            // decision), EVERY admission-evidence set, and EVERY B3
            // anchor dependency. Only re-EXECUTION is skipped — that is
            // exactly what the certified resulting root at the tip
            // attests.
            uint256 expect_parent{};
            for (uint64_t s{0}; prefix_ok && s < upto; ++s) {
                const std::optional<flowmesh::CertifiedEntry> prefix{ReadEntry(s)};
                if (!prefix || prefix->mb.sequence != s ||
                    prefix->mb.parent_hash != expect_parent) {
                    prefix_ok = false;
                    break;
                }
                expect_parent = prefix->mb.GetHash();
                if (prefix->cert.microblock_hash != expect_parent ||
                    prefix->cert.sequence != s ||
                    flowmesh::CheckCertificate(prefix->cert, marker->domain, seats,
                                               threshold) !=
                        flowmesh::CertificateCheck::OK) {
                    prefix_ok = false;
                    break;
                }
                if (!flowmesh::VerifyActionEvidence(prefix->mb, prefix->credentials, auth)) {
                    prefix_ok = false;
                    break;
                }
                if (anchors != nullptr && !anchors->StillCanonical(prefix->mb.anchor)) {
                    prefix_ok = false;
                    break;
                }
                if (!prefix->mb.anchor.IsNull()) {
                    prefix_anchors[{prefix->mb.anchor.height, prefix->mb.anchor.hash}] = s;
                }
            }
            // The chain must land exactly on the certified tip entry.
            prefix_ok = prefix_ok && tip_entry.has_value() &&
                        expect_parent == tip_entry->mb.GetHash();
        }
        if (prefix_ok) {
            flowmesh::FlowMeshState candidate{state};
            bool decoded{false};
            try {
                DataStream body{std::span{stored.state_bytes}};
                body >> candidate;
                decoded = body.empty(); // strict: full consumption
            } catch (const std::exception&) {
                decoded = false;
            }
            if (decoded && candidate.Root() == tip_entry->mb.resulting_state_root) {
                flowmesh::FlowMeshState next{std::move(candidate)};
                uint256 tail_hash{tip_entry->mb.GetHash()};
                std::map<std::pair<int32_t, uint256>, uint64_t> all_anchors{prefix_anchors};
                if (ReplayRange(next, tail_hash, upto, *marker, auth, deposits, seats,
                                threshold, anchors, error, &all_anchors)) {
                    state = std::move(next);
                    last_hash = tail_hash;
                    if (anchors_out != nullptr) *anchors_out = std::move(all_anchors);
                    return true;
                }
            }
        }
    }

    // Fallback: full verified replay from genesis.
    last_hash = uint256{};
    return ReplayRange(state, last_hash, 0, *marker, auth, deposits, seats, threshold, anchors,
                      error, anchors_out);
}

bool StartValidator(FlowMeshStore& store, flowmesh::MeshNode::Config config,
                    const uint256& vault_commitment, const uint256& base_asset,
                    const uint256& quote_asset, const size_t max_k, ValidatorRuntime& out,
                    std::string& error)
{
    if (config.auth == nullptr || config.anchors == nullptr || config.schedule == nullptr) {
        error = "flowmesh validator startup requires auth/anchors/schedule";
        return false;
    }
    if (max_k == 0 || max_k > flowmesh::HARD_MAX_CURVE_POINTS) {
        error = "flowmesh validator startup refused a curve bound outside the hard cap";
        return false;
    }
    if (base_asset == quote_asset) {
        error = "flowmesh validator startup refused a market whose base equals its quote";
        return false;
    }
    // CANONICAL GENESIS: the initial state is constructed HERE from the
    // immutable configuration — always the canonical empty state (no
    // chain-derived initialization exists yet; deposits stay
    // fail-closed). The market config id is DERIVED, never trusted from
    // the caller.
    flowmesh::FlowMeshState genesis{vault_commitment, base_asset, quote_asset, max_k};
    config.market_config_id = genesis.ConfigId();
    // Every binding is validated BEFORE the store is touched: an
    // invalid startup must not mutate a fresh store (no marker write).
    if (config.auth->DomainId() != config.domain ||
        config.auth->ExecConfigId() != genesis.ConfigId()) {
        error = "flowmesh validator startup authenticator is bound to a different "
                "domain/configuration";
        return false;
    }
    if (!flowmesh::ValidQuorumConfig(config.seats.size(), config.threshold)) {
        error = "flowmesh validator startup refused a nonsensical quorum configuration";
        return false;
    }
    if (!store.OpenForDomain(config.domain, config.seats, config.threshold, error)) return false;

    uint256 last_hash;
    std::map<std::pair<int32_t, uint256>, uint64_t> committed_anchors;
    if (!store.ReplayFromBestSnapshot(genesis, last_hash, *config.auth, config.deposits,
                                      config.seats, config.threshold, config.anchors, error,
                                      &committed_anchors)) {
        return false;
    }
    std::map<uint64_t, uint256> locks;
    if (!store.ReadLocks(locks, error)) return false;

    if (!store.ClaimValidatorRole()) {
        error = "flowmesh store already backs a signing validator";
        return false;
    }
    out.sink = std::make_unique<StoreCommitSink>(store);
    out.journal = std::make_unique<StoreLockJournal>(store);
    out.history = std::make_unique<StoreCatchupSource>(store);
    config.sink = out.sink.get();
    config.lock_journal = out.journal.get();
    config.history = out.history.get();
    out.mesh_node = flowmesh::detail::SigningNodeFactory::Make(
        std::move(config), std::move(genesis), last_hash, locks, committed_anchors);
    if (out.mesh_node->Halted()) {
        error = "flowmesh validator startup produced an invalid node configuration";
        out = {}; // never hand back a half-built runtime
        return false;
    }
    return true;
}

} // namespace node
