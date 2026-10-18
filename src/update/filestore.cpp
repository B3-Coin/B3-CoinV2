// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <update/filestore.h>

#include <random.h>
#include <util/fs_helpers.h>
#include <util/string.h>
#include <util/strencodings.h>

#include <cstdio>

namespace update {
namespace {

//! The record is a few dozen bytes; anything longer is not one.
constexpr size_t MAX_RECORD_BYTES{256};

} // namespace

std::string EncodeSequenceRecord(const SequenceState& state)
{
    std::string record{"v1:" + std::to_string(state.floor) + ":"};
    if (state.pending) {
        record += std::to_string(state.pending->sequence) + ":" + state.pending->payload_hash.GetHex();
    } else {
        record += "none";
    }
    return record + "\n";
}

std::optional<SequenceState> DecodeSequenceRecord(std::string_view record)
{
    if (!record.ends_with('\n')) return std::nullopt;
    record.remove_suffix(1);
    const std::vector<std::string> fields{util::SplitString(record, ':')};
    if (fields.size() < 3 || fields[0] != "v1") return std::nullopt;
    const auto floor{ToIntegral<uint64_t>(fields[1])};
    if (!floor) return std::nullopt;
    SequenceState state;
    state.floor = *floor;
    if (fields.size() == 3 && fields[2] == "none") return state;
    if (fields.size() != 4) return std::nullopt;
    const auto pending{ToIntegral<uint64_t>(fields[2])};
    const auto hash{uint256::FromHex(fields[3])};
    // The pending offer is the one exact manifest at the floor.
    if (!pending || *pending == 0 || *pending != *floor || !hash) return std::nullopt;
    state.pending = PendingOffer{*pending, *hash};
    return state;
}

bool FileSequenceStore::Load(SequenceState& state, std::string& error)
{
    error.clear();
    state = {};
    FILE* f{fsbridge::fopen(m_path, "rb")};
    if (!f) {
        std::error_code ec;
        if (!std::filesystem::exists(m_path, ec) && !ec) return true; // fresh install
        error = "update-sequence-load";
        return false;
    }
    std::string record(MAX_RECORD_BYTES + 1, '\0');
    const size_t got{std::fread(record.data(), 1, record.size(), f)};
    const bool read_error{std::ferror(f) != 0};
    std::fclose(f);
    if (read_error) {
        error = "update-sequence-load";
        return false;
    }
    record.resize(got);
    const auto decoded{got <= MAX_RECORD_BYTES ? DecodeSequenceRecord(record) : std::nullopt};
    if (!decoded) {
        error = "update-sequence-state";
        return false;
    }
    state = *decoded;
    return true;
}

bool FileSequenceStore::Store(const SequenceState& state, std::string& error)
{
    error.clear();
    if (state.pending && (state.pending->sequence == 0 || state.pending->sequence != state.floor)) {
        error = "update-sequence-state";
        return false;
    }
    const std::string record{EncodeSequenceRecord(state)};
    uint64_t rnd;
    GetStrongRandBytes(std::span<unsigned char>{reinterpret_cast<unsigned char*>(&rnd), sizeof(rnd)});
    const fs::path dir{m_path.parent_path()};
    const fs::path tmp{dir / fs::u8path(".sequence-" + std::to_string(rnd) + ".tmp")};
    FILE* f{fsbridge::fopen(tmp, "wbx")};
    if (!f) {
        error = "update-sequence-store";
        return false;
    }
    const bool written{std::fwrite(record.data(), 1, record.size(), f) == record.size() && FileCommit(f)};
    if (std::fclose(f) != 0 || !written || !RenameOver(tmp, m_path)) {
        fs::remove(tmp);
        error = "update-sequence-store";
        return false;
    }
    DirectoryCommit(dir);
    return true;
}

} // namespace update
