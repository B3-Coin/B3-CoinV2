// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_signer_store.h>

#include <hash.h>
#include <logging.h>
#include <random.h>
#include <serialize.h>
#include <streams.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <system_error>

namespace node {
namespace {

constexpr uint8_t SIGNER_STATE_VERSION{1};
constexpr std::array<unsigned char, 8> SIGNER_STATE_MAGIC{
    'B', '3', 'F', 'S', 'L', 'O', 'C', 'K'};

enum class ReadStatus { ABSENT, VALID, INVALID };

struct DiskState {
    uint256 chain_domain{};
    modern::ValidatorKeyBytes validator_key{};
    int32_t last_signed_height{-1};
    uint256 last_signed_block_hash{};
    uint256 last_signed_digest{};
    int32_t lock_height{-1};
    uint256 lock_block_hash{};
    uint256 lock_digest{};
    uint64_t lock_epoch{std::numeric_limits<uint64_t>::max()};
    uint256 lock_signing_set_hash{};
    uint256 lock_successor_set_hash{};

    SERIALIZE_METHODS(DiskState, obj)
    {
        READWRITE(obj.chain_domain, obj.validator_key,
                  obj.last_signed_height, obj.last_signed_block_hash,
                  obj.last_signed_digest, obj.lock_height,
                  obj.lock_block_hash, obj.lock_digest, obj.lock_epoch,
                  obj.lock_signing_set_hash,
                  obj.lock_successor_set_hash);
    }
};

DiskState ToDisk(const FinalitySignerState& state)
{
    return {state.chain_domain,
            state.validator_key,
            state.last_signed_height,
            state.last_signed_block_hash,
            state.last_signed_digest,
            state.lock_height,
            state.lock_block_hash,
            state.lock_digest,
            state.lock_epoch,
            state.lock_signing_set_hash,
            state.lock_successor_set_hash};
}

FinalitySignerState FromDisk(const DiskState& state)
{
    return {state.chain_domain,
            state.validator_key,
            state.last_signed_height,
            state.last_signed_block_hash,
            state.last_signed_digest,
            state.lock_height,
            state.lock_block_hash,
            state.lock_digest,
            state.lock_epoch,
            state.lock_signing_set_hash,
            state.lock_successor_set_hash};
}

bool IsValidState(const FinalitySignerState& state, std::string& error)
{
    const bool last_empty{state.last_signed_height == -1};
    const bool lock_empty{state.lock_height == -1};
    if (state.chain_domain.IsNull()) {
        error = "null chain domain";
        return false;
    }
    if (state.last_signed_height < -1 || state.lock_height < -1) {
        error = "negative height outside the empty sentinel";
        return false;
    }
    if (last_empty != (state.last_signed_block_hash.IsNull() &&
                       state.last_signed_digest.IsNull())) {
        error = "inconsistent last-signed fields";
        return false;
    }
    const bool lock_metadata_empty{
        state.lock_epoch == std::numeric_limits<uint64_t>::max() &&
        state.lock_signing_set_hash.IsNull() &&
        state.lock_successor_set_hash.IsNull()};
    if (lock_empty !=
        (state.lock_block_hash.IsNull() && state.lock_digest.IsNull() &&
         lock_metadata_empty)) {
        error = "inconsistent ancestry-lock fields";
        return false;
    }
    if (!last_empty &&
        (state.last_signed_block_hash.IsNull() ||
         state.last_signed_digest.IsNull())) {
        error = "incomplete last-signed checkpoint";
        return false;
    }
    if (!lock_empty &&
        (state.lock_block_hash.IsNull() || state.lock_digest.IsNull())) {
        error = "incomplete ancestry lock";
        return false;
    }
    if (!lock_empty &&
        (state.lock_epoch == std::numeric_limits<uint64_t>::max() ||
         state.lock_signing_set_hash.IsNull() ||
         state.lock_successor_set_hash.IsNull())) {
        error = "incomplete validator-set lineage lock";
        return false;
    }
    if (!last_empty && (lock_empty || state.lock_height <
                                          state.last_signed_height)) {
        error = "ancestry lock is behind the last signed checkpoint";
        return false;
    }
    return true;
}

ReadStatus ReadState(const fs::path& path, FinalitySignerState& out,
                     std::string& error)
{
    std::error_code ec;
    const bool exists{std::filesystem::exists(path.std_path(), ec)};
    if (ec) {
        error = strprintf("cannot inspect %s (%s)", fs::PathToString(path),
                          ec.message());
        return ReadStatus::INVALID;
    }
    if (!exists) return ReadStatus::ABSENT;

    AutoFile file{fsbridge::fopen(path, "rb")};
    if (file.IsNull()) {
        error = strprintf("cannot open %s", fs::PathToString(path));
        return ReadStatus::INVALID;
    }
    try {
        HashVerifier verifier{file};
        std::array<unsigned char, SIGNER_STATE_MAGIC.size()> magic{};
        uint8_t version{0};
        DiskState disk;
        verifier >> magic >> version >> disk;
        uint256 checksum;
        file >> checksum;
        if (magic != SIGNER_STATE_MAGIC) {
            error = "wrong signer-state file magic";
            return ReadStatus::INVALID;
        }
        if (version != SIGNER_STATE_VERSION) {
            error = strprintf("unknown signer-state version %d", version);
            return ReadStatus::INVALID;
        }
        if (checksum != verifier.GetHash()) {
            error = "signer-state checksum mismatch";
            return ReadStatus::INVALID;
        }
        if (file.tell() != file.size()) {
            error = "trailing bytes in signer-state file";
            return ReadStatus::INVALID;
        }
        FinalitySignerState decoded{FromDisk(disk)};
        if (!IsValidState(decoded, error)) return ReadStatus::INVALID;
        out = decoded;
        return ReadStatus::VALID;
    } catch (const std::exception& e) {
        error = strprintf("unreadable signer-state file (%s)", e.what());
        return ReadStatus::INVALID;
    }
}

void RemoveNoThrow(const fs::path& path)
{
    std::error_code ec;
    fs::remove(path, ec);
}

bool WriteState(const fs::path& path, const FinalitySignerState& state,
                std::string& error)
{
    const uint16_t random_suffix{FastRandomContext().rand<uint16_t>()};
    const fs::path tmp{fs::path{path.parent_path()} / fs::u8path(strprintf(
        "%s.%04x.tmp", fs::PathToString(path.filename()),
        random_suffix))};
    AutoFile out{fsbridge::fopen(tmp, "wb")};
    if (out.IsNull()) {
        error = strprintf("cannot open temporary signer-state file %s",
                          fs::PathToString(tmp));
        return false;
    }
    try {
        HashedSourceWriter writer{out};
        writer << SIGNER_STATE_MAGIC << SIGNER_STATE_VERSION << ToDisk(state);
        out << writer.GetHash();
    } catch (const std::exception& e) {
        (void)out.fclose();
        RemoveNoThrow(tmp);
        error = strprintf("cannot serialize signer state (%s)", e.what());
        return false;
    }
    if (!out.Commit()) {
        (void)out.fclose();
        RemoveNoThrow(tmp);
        error = "cannot flush signer-state file";
        return false;
    }
    if (out.fclose() != 0) {
        const int saved_errno{errno};
        RemoveNoThrow(tmp);
        error = strprintf("cannot close signer-state file (%s)",
                          SysErrorString(saved_errno));
        return false;
    }
    if (!RenameOver(tmp, path)) {
        RemoveNoThrow(tmp);
        error = "cannot atomically install signer-state file";
        return false;
    }
    // Persist the directory entry as well as the file contents. Without this,
    // a crash immediately after rename may resurrect the predecessor or no
    // file at all on filesystems that journal directory metadata lazily.
    DirectoryCommit(path.parent_path());
    return true;
}

} // namespace

fs::path FinalitySignerStore::StatePath(
    const fs::path& directory, const uint256& chain_domain,
    const modern::ValidatorKeyBytes& validator_key)
{
    return directory / fs::u8path(strprintf(
        "finality_signer_v1_%s_%s.dat", chain_domain.GetHex(),
        HexStr(validator_key)));
}

bool FinalitySignerStore::Open(
    const fs::path& directory, const uint256& chain_domain,
    const modern::ValidatorKeyBytes& validator_key, std::string& error)
{
    m_open = false;
    m_state.reset();
    m_path.clear();
    if (directory.empty() || chain_domain.IsNull()) {
        error = "invalid finality signer store identity";
        return false;
    }
    try {
        TryCreateDirectories(directory);
    } catch (const fs::filesystem_error& e) {
        error = strprintf("cannot create finality signer directory %s (%s)",
                          fs::PathToString(directory), e.what());
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(directory.std_path(), ec) || ec) {
        error = strprintf("finality signer path is not a directory: %s",
                          fs::PathToString(directory));
        return false;
    }
    m_chain_domain = chain_domain;
    m_validator_key = validator_key;
    m_path = StatePath(directory, chain_domain, validator_key);

    FinalitySignerState loaded;
    std::string read_error;
    switch (ReadState(m_path, loaded, read_error)) {
    case ReadStatus::ABSENT:
        m_open = true;
        return true;
    case ReadStatus::INVALID:
        error = strprintf("unsafe finality signer state: %s", read_error);
        return false;
    case ReadStatus::VALID:
        if (loaded.chain_domain != chain_domain ||
            loaded.validator_key != validator_key) {
            error = "finality signer state belongs to another chain or validator";
            return false;
        }
        m_state = loaded;
        m_open = true;
        return true;
    }
    return false;
}

bool FinalitySignerStore::Commit(const FinalitySignerState& next,
                                 std::string& error)
{
    if (!m_open) {
        error = "finality signer store is not open";
        return false;
    }
    if (next.chain_domain != m_chain_domain ||
        next.validator_key != m_validator_key ||
        !IsValidState(next, error)) {
        if (error.empty()) error = "finality signer identity changed";
        return false;
    }
    // Re-read immediately before replacement. A missing, corrupt, or changed
    // predecessor is never overwritten: it may be the only surviving safety
    // record from another process or an interrupted operator action.
    FinalitySignerState disk;
    std::string read_error;
    const ReadStatus status{ReadState(m_path, disk, read_error)};
    if (m_state) {
        if (status != ReadStatus::VALID || disk != *m_state) {
            error = status == ReadStatus::INVALID
                        ? strprintf("refusing to replace unsafe signer state: %s",
                                    read_error)
                        : "signer-state predecessor changed or disappeared";
            return false;
        }
    } else if (status != ReadStatus::ABSENT) {
        error = status == ReadStatus::INVALID
                    ? strprintf("refusing to replace unsafe signer state: %s",
                                read_error)
                    : "signer-state file appeared after startup";
        return false;
    }

    if (m_state && *m_state == next) return true;

    if (!WriteState(m_path, next, error)) return false;
    m_state = next;
    return true;
}

bool FinalitySignerStore::InitializeEmpty(std::string& error)
{
    if (!m_open) {
        error = "finality signer store is not open";
        return false;
    }
    if (m_state) return true;
    FinalitySignerState initial;
    initial.chain_domain = m_chain_domain;
    initial.validator_key = m_validator_key;
    return Commit(initial, error);
}

bool FinalitySignerStore::CommitSignedCheckpoint(
    const int height, const uint256& block_hash, const uint256& digest,
    const uint64_t epoch, const uint256& signing_set_hash,
    const uint256& successor_set_hash, std::string& error)
{
    if (!m_open || !m_state) {
        error = "finality signer state is not durably initialized";
        return false;
    }
    if (height < 0 || block_hash.IsNull() || digest.IsNull() ||
        signing_set_hash.IsNull() || successor_set_hash.IsNull()) {
        error = "invalid signed checkpoint";
        return false;
    }
    const FinalitySignerState& current{*m_state};
    if (height < current.last_signed_height) {
        error = "refusing to sign below the durable watermark";
        return false;
    }
    if (height == current.last_signed_height) {
        if (block_hash != current.last_signed_block_hash ||
            digest != current.last_signed_digest) {
            error = "refusing a competing digest at an already signed height";
            return false;
        }
        return Commit(current, error);
    }
    if (height <= current.lock_height) {
        error = "refusing to sign at or below the durable ancestry lock";
        return false;
    }
    if (current.lock_height >= 0) {
        const bool same_epoch{
            epoch == current.lock_epoch &&
            signing_set_hash == current.lock_signing_set_hash};
        const bool next_epoch{
            current.lock_epoch <
                std::numeric_limits<uint64_t>::max() - 1 &&
            epoch == current.lock_epoch + 1 &&
            signing_set_hash == current.lock_successor_set_hash};
        if (!same_epoch && !next_epoch) {
            error = "checkpoint does not follow the durable validator-set lineage";
            return false;
        }
    }

    FinalitySignerState next{current};
    next.last_signed_height = height;
    next.last_signed_block_hash = block_hash;
    next.last_signed_digest = digest;
    next.lock_height = height;
    next.lock_block_hash = block_hash;
    next.lock_digest = digest;
    next.lock_epoch = epoch;
    next.lock_signing_set_hash = signing_set_hash;
    next.lock_successor_set_hash = successor_set_hash;
    return Commit(next, error);
}

bool FinalitySignerStore::CommitCertifiedAnchor(
    const int height, const uint256& block_hash, const uint256& digest,
    const uint64_t epoch, const uint256& signing_set_hash,
    const uint256& successor_set_hash, std::string& error)
{
    if (!m_open || !m_state) {
        error = "finality signer state is not durably initialized";
        return false;
    }
    if (height < 0 || block_hash.IsNull() || digest.IsNull() ||
        signing_set_hash.IsNull() || successor_set_hash.IsNull()) {
        error = "invalid certified checkpoint";
        return false;
    }
    const FinalitySignerState& current{*m_state};
    if (height <= current.lock_height) {
        if (height == current.lock_height &&
            block_hash == current.lock_block_hash &&
            digest == current.lock_digest && epoch == current.lock_epoch &&
            signing_set_hash == current.lock_signing_set_hash &&
            successor_set_hash == current.lock_successor_set_hash) {
            return Commit(current, error);
        }
        error = "certified checkpoint does not advance the durable ancestry lock";
        return false;
    }
    if (current.lock_height >= 0) {
        const bool same_epoch{
            epoch == current.lock_epoch &&
            signing_set_hash == current.lock_signing_set_hash};
        if (!same_epoch) {
            error = "certificate does not use the exact validator set of the durable ancestry lock";
            return false;
        }
    }
    FinalitySignerState next{current};
    next.lock_height = height;
    next.lock_block_hash = block_hash;
    next.lock_digest = digest;
    next.lock_epoch = epoch;
    next.lock_signing_set_hash = signing_set_hash;
    next.lock_successor_set_hash = successor_set_hash;
    return Commit(next, error);
}

bool FinalitySignerStore::CommitPinnedRecoveryAnchor(
    const Consensus::FinalitySignerRecovery& recovery,
    const uint256& anchor_digest, std::string& error)
{
    if (!m_open || !m_state) {
        error = "finality signer state is not durably initialized";
        return false;
    }
    if (!recovery.Valid() || anchor_digest.IsNull()) {
        error = "invalid pinned recovery";
        return false;
    }
    if (recovery.chain_domain != m_chain_domain) {
        error = "pinned recovery belongs to another chain";
        return false;
    }
    const FinalitySignerState& current{*m_state};
    // The durable record must be the pinned incident and nothing else: the
    // orphaned checkpoint is both the last vote and the lock (they were
    // written together, with one digest), under the pinned epoch and exact
    // validator sets. Any newer vote, any moved lock, or any other
    // coordinate is a different history and fails closed.
    const bool exact_incident{
        current.last_signed_height == recovery.incident_height &&
        current.last_signed_block_hash == recovery.incident_block_hash &&
        current.lock_height == recovery.incident_height &&
        current.lock_block_hash == recovery.incident_block_hash &&
        current.lock_digest == current.last_signed_digest &&
        current.lock_epoch == recovery.incident_epoch &&
        current.lock_signing_set_hash == recovery.incident_signing_set_hash &&
        current.lock_successor_set_hash ==
            recovery.incident_successor_set_hash};
    if (!exact_incident) {
        error = "durable signer state is not exactly the pinned recovery incident";
        return false;
    }
    // Implied by Valid() (anchor above the incident) together with the exact
    // match above; kept as a belt-and-braces guard on the invariant that the
    // lock never moves backwards.
    if (recovery.anchor_height <= current.lock_height ||
        recovery.anchor_height <= current.last_signed_height) {
        error = "pinned recovery anchor does not advance the durable ancestry lock";
        return false;
    }
    FinalitySignerState next{current};
    next.lock_height = recovery.anchor_height;
    next.lock_block_hash = recovery.anchor_block_hash;
    next.lock_digest = anchor_digest;
    return Commit(next, error);
}

} // namespace node
