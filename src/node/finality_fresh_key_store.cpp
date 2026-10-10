// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_fresh_key_store.h>

#include <crypto/common.h>
#include <crypto/sha256.h>
#include <logging.h>
#include <node/finality_fresh_lineage.h>
#include <random.h>
#include <span.h>
#include <streams.h>
#include <support/allocators/zeroafterfree.h>
#include <support/cleanse.h>
#include <sync.h>
#include <tinyformat.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>
#include <util/syserror.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <system_error>

namespace node {
namespace {

constexpr uint8_t STATE_HELD{static_cast<uint8_t>(FreshKeyState::HELD)};
constexpr uint8_t STATE_ERASED{static_cast<uint8_t>(FreshKeyState::ERASED)};
constexpr size_t COUNT_OFFSET{8 + 1 + 32 + 32};
constexpr size_t MAX_FILE_SIZE{FreshKeyStore::HEADER_SIZE + FreshKeyStore::MAX_RECORDS * FreshKeyStore::RECORD_SIZE +
                               FreshKeyStore::CHECKSUM_SIZE};

enum class ReadStatus { ABSENT, VALID, INVALID };

//! The decoded content of a store file (the in-memory view plus identity).
struct Decoded {
    uint256 chain_domain{};
    modern::ValidatorKeyBytes validator_key{};
    std::vector<FreshKeyRecord> records;
    //! The HELD records' secrets (locked memory, cleansed when freed).
    FreshKeyStore::SecretMap secrets;
};

std::array<unsigned char, CSHA256::OUTPUT_SIZE> Checksum(std::span<const unsigned char> bytes)
{
    std::array<unsigned char, CSHA256::OUTPUT_SIZE> out{};
    CSHA256().Write(bytes.data(), bytes.size()).Finalize(out.data());
    return out;
}

bool AllZero(std::span<const unsigned char> bytes)
{
    return std::all_of(bytes.begin(), bytes.end(), [](const unsigned char b) { return b == 0; });
}

/** Decode and verify a whole store file. */
bool Decode(std::span<const unsigned char> bytes, Decoded& out, std::string& error)
{
    if (bytes.size() < FreshKeyStore::HEADER_SIZE + FreshKeyStore::CHECKSUM_SIZE) {
        error = "truncated fresh key store";
        return false;
    }
    if (!std::equal(FreshKeyStore::MAGIC.begin(), FreshKeyStore::MAGIC.end(), bytes.begin())) {
        error = "wrong fresh key store magic";
        return false;
    }
    if (bytes[8] != FreshKeyStore::VERSION) {
        error = strprintf("unknown fresh key store version %d", bytes[8]);
        return false;
    }
    const size_t body_size{bytes.size() - FreshKeyStore::CHECKSUM_SIZE};
    const auto checksum{Checksum(bytes.first(body_size))};
    if (!std::equal(checksum.begin(), checksum.end(), bytes.begin() + body_size)) {
        error = "fresh key store checksum mismatch";
        return false;
    }
    const uint32_t count{ReadLE32(bytes.data() + COUNT_OFFSET)};
    if (count > FreshKeyStore::MAX_RECORDS) {
        error = strprintf("fresh key store holds %u records (maximum %u)", count, FreshKeyStore::MAX_RECORDS);
        return false;
    }
    if (bytes.size() != FreshKeyStore::HEADER_SIZE + size_t{count} * FreshKeyStore::RECORD_SIZE +
                            FreshKeyStore::CHECKSUM_SIZE) {
        error = "fresh key store length does not match its record count";
        return false;
    }
    std::copy_n(bytes.begin() + 9, 32, out.chain_domain.begin());
    std::copy_n(bytes.begin() + 41, 32, out.validator_key.begin());
    if (out.chain_domain.IsNull() || out.validator_key == modern::ValidatorKeyBytes{}) {
        error = "fresh key store has a null identity";
        return false;
    }
    out.records.clear();
    out.secrets.clear();
    out.records.reserve(count);
    std::set<modern::BlsPubkeyBytes> seen;
    size_t pos{FreshKeyStore::HEADER_SIZE};
    for (uint32_t i{0}; i < count; ++i) {
        FreshKeyRecord record;
        record.target_epoch = ReadLE64(bytes.data() + pos);
        const uint8_t state{bytes[pos + 8]};
        std::copy_n(bytes.begin() + pos + 9, bls::PUBKEY_SIZE, record.bls_pubkey.begin());
        const auto secret_bytes{bytes.subspan(pos + 9 + bls::PUBKEY_SIZE, bls::SECRET_SIZE)};
        std::copy_n(bytes.begin() + pos + 9 + bls::PUBKEY_SIZE + bls::SECRET_SIZE, 32, record.bind_txid.begin());
        pos += FreshKeyStore::RECORD_SIZE;

        if (state != STATE_HELD && state != STATE_ERASED) {
            error = strprintf("fresh key record %u has unknown state %d", i, state);
            return false;
        }
        record.state = static_cast<FreshKeyState>(state);
        if (!bls::PublicKey::Decode(record.bls_pubkey)) {
            error = strprintf("fresh key record %u has a non-canonical BLS public key", i);
            return false;
        }
        if (!seen.insert(record.bls_pubkey).second) {
            error = strprintf("fresh key record %u repeats a BLS public key", i);
            return false;
        }
        if (record.Held()) {
            const auto secret{bls::SecretKey::FromBytes(secret_bytes)};
            if (!secret || secret->GetPublicKey().Compressed() != record.bls_pubkey) {
                error = strprintf("fresh key record %u holds a secret that does not match its public key", i);
                return false;
            }
            std::copy(secret_bytes.begin(), secret_bytes.end(), out.secrets[record.bls_pubkey].begin());
        } else if (!AllZero(secret_bytes)) {
            error = strprintf("erased fresh key record %u still holds secret bytes", i);
            return false;
        }
        out.records.push_back(record);
    }
    return true;
}

//! Open `path` unbuffered, so no copy of a secret is left behind in a stdio
//! buffer (the node's own buffers are cleansed when freed).
std::FILE* OpenUnbuffered(const fs::path& path, const char* mode)
{
    std::FILE* file{fsbridge::fopen(path, mode)};
    if (file) std::setvbuf(file, nullptr, _IONBF, 0);
    return file;
}

/** Read and verify the file at `path`. */
ReadStatus ReadStore(const fs::path& path, Decoded& out, std::string& error)
{
    std::error_code ec;
    const auto status{std::filesystem::symlink_status(path.std_path(), ec)};
    if (status.type() == std::filesystem::file_type::not_found) return ReadStatus::ABSENT;
    if (ec) {
        error = strprintf("cannot inspect %s (%s)", fs::PathToString(path), ec.message());
        return ReadStatus::INVALID;
    }
    if (status.type() != std::filesystem::file_type::regular) {
        error = strprintf("%s is not a regular file", fs::PathToString(path));
        return ReadStatus::INVALID;
    }
    AutoFile file{OpenUnbuffered(path, "rb")};
    if (file.IsNull()) {
        error = strprintf("cannot open %s", fs::PathToString(path));
        return ReadStatus::INVALID;
    }
    SerializeData buffer;
    try {
        const int64_t size{file.size()};
        if (size < 0 || static_cast<uint64_t>(size) > MAX_FILE_SIZE) {
            error = "fresh key store file is too large";
            return ReadStatus::INVALID;
        }
        buffer.resize(static_cast<size_t>(size));
        file.read(buffer);
        if (file.tell() != file.size()) {
            error = "fresh key store file changed while being read";
            return ReadStatus::INVALID;
        }
    } catch (const std::exception& e) {
        error = strprintf("unreadable fresh key store (%s)", e.what());
        return ReadStatus::INVALID;
    }
    (void)file.fclose();
    if (!Decode(MakeUCharSpan(buffer), out, error)) return ReadStatus::INVALID;
    return ReadStatus::VALID;
}

void RemoveNoThrow(const fs::path& path)
{
    std::error_code ec;
    fs::remove(path, ec);
}

/** Durably replace `path` with `bytes`: owner-only temporary file, fsync,
 * rename-over, directory sync. */
bool WriteStore(const fs::path& path, std::span<const std::byte> bytes, std::string& error)
{
    try {
        TryCreateDirectories(path.parent_path());
    } catch (const fs::filesystem_error& e) {
        error = strprintf("cannot create finality signer directory %s (%s)",
                          fs::PathToString(path.parent_path()), e.what());
        return false;
    }
    const uint16_t random_suffix{FastRandomContext().rand<uint16_t>()};
    const fs::path tmp{fs::path{path.parent_path()} /
                       fs::u8path(strprintf("%s.%04x.tmp", fs::PathToString(path.filename()), random_suffix))};
    AutoFile out{OpenUnbuffered(tmp, "wb")};
    if (out.IsNull()) {
        error = strprintf("cannot open temporary fresh key store file %s", fs::PathToString(tmp));
        return false;
    }
#ifndef WIN32
    // Owner-only before the first secret byte is written; the rename keeps
    // the mode, so the store file is always 0600.
    {
        std::error_code ec;
        fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
        if (ec) {
            (void)out.fclose();
            RemoveNoThrow(tmp);
            error = strprintf("cannot restrict fresh key store permissions (%s)", ec.message());
            return false;
        }
    }
#endif
    try {
        out.write(bytes);
    } catch (const std::exception& e) {
        (void)out.fclose();
        RemoveNoThrow(tmp);
        error = strprintf("cannot write fresh key store (%s)", e.what());
        return false;
    }
    if (!out.Commit()) {
        (void)out.fclose();
        RemoveNoThrow(tmp);
        error = "cannot flush fresh key store file";
        return false;
    }
    if (out.fclose() != 0) {
        const int saved_errno{errno};
        RemoveNoThrow(tmp);
        error = strprintf("cannot close fresh key store file (%s)", SysErrorString(saved_errno));
        return false;
    }
    if (!RenameOver(tmp, path)) {
        RemoveNoThrow(tmp);
        error = "cannot atomically install fresh key store file";
        return false;
    }
    DirectoryCommit(path.parent_path());
    return true;
}

} // namespace

GlobalMutex FreshKeyStore::s_mutex;

const char* FreshKeyStateName(const FreshKeyState state)
{
    switch (state) {
    case FreshKeyState::HELD: return "held";
    case FreshKeyState::ERASED: return "erased";
    }
    return "unknown";
}

fs::path FreshKeyStore::StatePath(const fs::path& directory, const uint256& chain_domain,
                                  const modern::ValidatorKeyBytes& validator_key)
{
    return directory / fs::u8path(strprintf("finality_fresh_keys_%s_%s.dat", chain_domain.GetHex(),
                                            HexStr(validator_key)));
}

bool FreshKeyStore::Open(const fs::path& directory, const uint256& chain_domain,
                         const modern::ValidatorKeyBytes& validator_key, std::string& error)
{
    LOCK(s_mutex);
    m_open = false;
    m_on_disk = false;
    m_path.clear();
    m_content = Content{};
    if (directory.empty() || chain_domain.IsNull() || validator_key == modern::ValidatorKeyBytes{}) {
        error = "invalid fresh key store identity";
        return false;
    }
    const fs::path path{StatePath(directory, chain_domain, validator_key)};
    Decoded decoded;
    std::string read_error;
    switch (ReadStore(path, decoded, read_error)) {
    case ReadStatus::ABSENT:
        break;
    case ReadStatus::INVALID:
        error = strprintf("unsafe fresh key store: %s", read_error);
        return false;
    case ReadStatus::VALID:
        if (decoded.chain_domain != chain_domain || decoded.validator_key != validator_key) {
            error = "fresh key store belongs to another chain or validator";
            return false;
        }
        m_content.records = std::move(decoded.records);
        m_content.secrets = std::move(decoded.secrets);
        m_on_disk = true;
        break;
    }
    m_path = path;
    m_chain_domain = chain_domain;
    m_validator_key = validator_key;
    m_open = true;
    return true;
}

std::vector<FreshKeyRecord> FreshKeyStore::Records() const { return m_content.records; }

std::optional<size_t> FreshKeyStore::Find(const modern::BlsPubkeyBytes& bls_pubkey) const
{
    for (size_t i{0}; i < m_content.records.size(); ++i) {
        if (m_content.records[i].bls_pubkey == bls_pubkey) return i;
    }
    return std::nullopt;
}

std::optional<FreshKeyRecord> FreshKeyStore::RecordFor(const modern::BlsPubkeyBytes& bls_pubkey) const
{
    const auto i{Find(bls_pubkey)};
    if (!i) return std::nullopt;
    return m_content.records[*i];
}

bool FreshKeyStore::HoldsKeyForEpoch(const uint64_t target_epoch) const
{
    return std::any_of(m_content.records.begin(), m_content.records.end(), [&](const FreshKeyRecord& record) {
        return record.Held() && record.target_epoch == target_epoch;
    });
}

std::optional<bls::SecretKey> FreshKeyStore::SecretFor(const modern::BlsPubkeyBytes& bls_pubkey) const
{
    const auto it{m_content.secrets.find(bls_pubkey)};
    if (it == m_content.secrets.end()) return std::nullopt;
    return bls::SecretKey::FromBytes(it->second);
}

bool FreshKeyStore::Commit(const Content& next, std::string& error)
{
    AssertLockHeld(s_mutex);
    if (!m_open) {
        error = "fresh key store is not open";
        return false;
    }
    if (next.records.size() > MAX_RECORDS) {
        error = "fresh key store is full";
        return false;
    }
    // The secrets are exactly the HELD records' (an invariant of every
    // mutation below; checked so a slip can never write a HELD record
    // without its secret).
    size_t held{0};
    for (const FreshKeyRecord& record : next.records) {
        if (!record.Held()) continue;
        ++held;
        if (!next.secrets.contains(record.bls_pubkey)) {
            error = "fresh key store lost a held secret";
            return false;
        }
    }
    if (held != next.secrets.size()) {
        error = "fresh key store holds a secret without a held record";
        return false;
    }
    // Re-read immediately before replacement. A changed, missing or corrupt
    // predecessor is never overwritten: it may hold the only copy of a key
    // another instance (or process) generated and bound.
    Decoded disk;
    std::string read_error;
    const ReadStatus status{ReadStore(m_path, disk, read_error)};
    if (m_on_disk) {
        const bool same{status == ReadStatus::VALID && disk.chain_domain == m_chain_domain &&
                        disk.validator_key == m_validator_key && disk.records == m_content.records &&
                        disk.secrets == m_content.secrets};
        if (!same) {
            error = status == ReadStatus::INVALID
                        ? strprintf("refusing to replace an unsafe fresh key store: %s", read_error)
                        : "fresh key store changed or disappeared on disk; reopen it";
            return false;
        }
    } else if (status != ReadStatus::ABSENT) {
        error = status == ReadStatus::INVALID
                    ? strprintf("refusing to replace an unsafe fresh key store: %s", read_error)
                    : "fresh key store file appeared after it was opened; reopen it";
        return false;
    }
    if (m_on_disk && next == m_content) return true;

    SerializeData bytes(HEADER_SIZE + next.records.size() * RECORD_SIZE + CHECKSUM_SIZE);
    unsigned char* const p{UCharCast(bytes.data())};
    std::copy(MAGIC.begin(), MAGIC.end(), p);
    p[8] = VERSION;
    std::copy(m_chain_domain.begin(), m_chain_domain.end(), p + 9);
    std::copy(m_validator_key.begin(), m_validator_key.end(), p + 41);
    WriteLE32(p + COUNT_OFFSET, static_cast<uint32_t>(next.records.size()));
    size_t pos{HEADER_SIZE};
    for (const FreshKeyRecord& record : next.records) {
        WriteLE64(p + pos, record.target_epoch);
        p[pos + 8] = static_cast<uint8_t>(record.state);
        std::copy(record.bls_pubkey.begin(), record.bls_pubkey.end(), p + pos + 9);
        if (record.Held()) {
            const SecretBytes& secret{next.secrets.at(record.bls_pubkey)};
            std::copy(secret.begin(), secret.end(), p + pos + 9 + bls::PUBKEY_SIZE);
        } // ERASED: the zero-initialized bytes stay zero.
        std::copy(record.bind_txid.begin(), record.bind_txid.end(),
                  p + pos + 9 + bls::PUBKEY_SIZE + bls::SECRET_SIZE);
        pos += RECORD_SIZE;
    }
    const size_t body_size{bytes.size() - CHECKSUM_SIZE};
    const auto checksum{Checksum({p, body_size})};
    std::copy(checksum.begin(), checksum.end(), p + body_size);

    if (!WriteStore(m_path, bytes, error)) return false;
    m_content = next;
    m_on_disk = true;
    return true;
}

std::optional<bls::SecretKey> FreshKeyStore::Generate(const uint64_t target_epoch, std::string& error)
{
    LOCK(s_mutex);
    if (!m_open) {
        error = "fresh key store is not open";
        return std::nullopt;
    }
    if (m_content.records.size() >= MAX_RECORDS) {
        error = strprintf("fresh key store is full (%u records)", MAX_RECORDS);
        return std::nullopt;
    }
    if (HoldsKeyForEpoch(target_epoch)) {
        error = strprintf("a fresh key for epoch %u is already held; binding a second key for one epoch voids the seat",
                          target_epoch);
        return std::nullopt;
    }
    // Fresh entropy only: nothing here can be recomputed from the wallet's
    // identity secret or any backup of it.
    std::array<unsigned char, 32> ikm{};
    GetStrongRandBytes(ikm);
    std::optional<bls::SecretKey> secret{bls::SecretKey::FromIKM(ikm)};
    memory_cleanse(ikm.data(), ikm.size());
    if (!secret) {
        error = "cannot derive a BLS key from fresh entropy";
        return std::nullopt;
    }
    FreshKeyRecord record;
    record.target_epoch = target_epoch;
    record.state = FreshKeyState::HELD;
    record.bls_pubkey = secret->GetPublicKey().Compressed();
    if (Find(record.bls_pubkey)) {
        error = "generated fresh key collides with a stored key";
        return std::nullopt;
    }
    Content next{m_content};
    next.records.push_back(record);
    {
        SecretBytes bytes{secret->Bytes()};
        next.secrets[record.bls_pubkey] = bytes;
        memory_cleanse(bytes.data(), bytes.size());
    }
    if (!Commit(next, error)) return std::nullopt;
    LogInfo("Generated fresh finality key %s for epoch %u (store %s)", HexStr(record.bls_pubkey), target_epoch,
            fs::PathToString(m_path.filename()));
    return secret;
}

bool FreshKeyStore::SetBindTxid(const modern::BlsPubkeyBytes& bls_pubkey, const uint256& txid, std::string& error)
{
    LOCK(s_mutex);
    if (!m_open) {
        error = "fresh key store is not open";
        return false;
    }
    if (txid.IsNull()) {
        error = "null binding txid";
        return false;
    }
    const auto i{Find(bls_pubkey)};
    if (!i) {
        error = "unknown fresh key";
        return false;
    }
    const FreshKeyRecord& record{m_content.records[*i]};
    if (!record.Held()) {
        error = "fresh key is erased";
        return false;
    }
    if (!record.bind_txid.IsNull() && record.bind_txid != txid) {
        error = strprintf("fresh key is already bound by %s; a key is never bound twice", record.bind_txid.GetHex());
        return false;
    }
    Content next{m_content};
    next.records[*i].bind_txid = txid;
    return Commit(next, error);
}

bool FreshKeyStore::Erase(const modern::BlsPubkeyBytes& bls_pubkey, std::string& error)
{
    LOCK(s_mutex);
    if (!m_open) {
        error = "fresh key store is not open";
        return false;
    }
    const auto i{Find(bls_pubkey)};
    if (!i) {
        error = "unknown fresh key";
        return false;
    }
    const bool was_held{m_content.records[*i].Held()};
    Content next{m_content};
    next.records[*i].state = FreshKeyState::ERASED;
    next.secrets.erase(bls_pubkey); // the freed node is cleansed by secure_allocator
    if (!Commit(next, error)) return false;
    if (was_held) {
        LogInfo("Erased fresh finality key %s of epoch %u (store %s)", HexStr(bls_pubkey),
                m_content.records[*i].target_epoch, fs::PathToString(m_path.filename()));
    }
    return true;
}

std::optional<int> FreshKeyRetiredSince(const FreshLineageState& s, const modern::BlsPubkeyBytes& bls_pubkey,
                                        const uint64_t target_epoch)
{
    if (!s.bootstrapped) return std::nullopt;
    if (s.epoch == std::numeric_limits<uint64_t>::max() || target_epoch > s.epoch + 1) return std::nullopt;
    if (s.epoch_starts.size() <= s.epoch || !s.current || !s.next || !s.current->members || !s.next->members) {
        return std::nullopt; // malformed state: keep
    }
    const auto seats{[&](const FreshLineageSet& set) {
        const auto& members{set.members->Members()};
        return std::any_of(members.begin(), members.end(),
                           [&](const ValidatorSetMember& m) { return m.bls_pubkey == bls_pubkey; });
    }};
    if (seats(*s.current) || seats(*s.next)) return std::nullopt;
    // Not seated in Set'_{e-1} either, and not a key for Set'_{e+1} (fixed
    // only at start_e): its committee was fixed by start_{e-1} and its last
    // seat, if any, ended with a handover included below start_{e-1}.
    // Without a readable Set'_{e-1}, count from start_e (later, so safe).
    if (s.epoch >= 1 && target_epoch <= s.epoch && s.previous && s.previous->members && !seats(*s.previous)) {
        return s.epoch_starts[s.epoch - 1];
    }
    return s.epoch_starts[s.epoch];
}

bool FreshKeyErasable(const FreshLineageState& s, const modern::BlsPubkeyBytes& bls_pubkey,
                      const uint64_t target_epoch, const int tip_height, const int horizon)
{
    if (horizon < 1) return false;
    const auto since{FreshKeyRetiredSince(s, bls_pubkey, target_epoch)};
    return since && tip_height >= *since && int64_t{tip_height} - *since >= horizon;
}

int FreshKeyErasureHorizon(const Consensus::Params& params)
{
    if (!params.modern_pos) return std::numeric_limits<int>::max();
    return params.modern_pos->reorg_horizon.value_or(params.modern_pos->finality_epoch_blocks);
}

} // namespace node
