// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/finality_pin.h>

#include <hash.h>
#include <logging.h>
#include <random.h>
#include <streams.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>

#include <cerrno>
#include <cstdio>

namespace node {

FinalityPinFileStatus ReadFinalityPinFile(const fs::path& path, const MessageStartChars& magic, FinalityPin& out,
                                          std::string& error)
{
    if (!fs::exists(path)) return FinalityPinFileStatus::ABSENT;
    AutoFile file{fsbridge::fopen(path, "rb")};
    if (file.IsNull()) {
        error = strprintf("cannot open %s", fs::PathToString(path));
        return FinalityPinFileStatus::INVALID;
    }
    try {
        HashVerifier verifier{file};
        MessageStartChars file_magic;
        verifier >> file_magic;
        if (file_magic != magic) {
            error = strprintf("%s belongs to another network", fs::PathToString(path));
            return FinalityPinFileStatus::INVALID;
        }
        uint8_t version{0};
        verifier >> version;
        if (version != FINALITY_PIN_FILE_VERSION) {
            error = strprintf("%s has unknown version %d", fs::PathToString(path), version);
            return FinalityPinFileStatus::INVALID;
        }
        FinalityPin pin;
        verifier >> pin.height >> pin.hash;
        uint256 checksum;
        file >> checksum;
        if (checksum != verifier.GetHash()) {
            error = strprintf("%s checksum mismatch (corrupt)", fs::PathToString(path));
            return FinalityPinFileStatus::INVALID;
        }
        if (pin.height < 0 || pin.hash.IsNull()) {
            error = strprintf("%s holds an invalid pin", fs::PathToString(path));
            return FinalityPinFileStatus::INVALID;
        }
        out = pin;
        return FinalityPinFileStatus::VALID;
    } catch (const std::exception& e) {
        error = strprintf("%s unreadable (%s)", fs::PathToString(path), e.what());
        return FinalityPinFileStatus::INVALID;
    }
}

std::optional<FinalityPin> ReadFinalityPin(const fs::path& path, const MessageStartChars& magic)
{
    FinalityPin pin;
    std::string error;
    if (ReadFinalityPinFile(path, magic, pin, error) == FinalityPinFileStatus::VALID) return pin;
    return std::nullopt;
}

bool WriteFinalityPin(const fs::path& path, const MessageStartChars& magic, const FinalityPin& pin)
{
    if (pin.height < 0 || pin.hash.IsNull()) return false;
    // Monotone, and FAIL CLOSED over an invalid file: an unreadable or
    // corrupt pin file is never silently replaced -- monotonicity could not
    // be verified against it, so the operator must intervene.
    FinalityPin existing;
    std::string read_error;
    switch (ReadFinalityPinFile(path, magic, existing, read_error)) {
    case FinalityPinFileStatus::ABSENT:
        break;
    case FinalityPinFileStatus::VALID:
        if (existing.height >= pin.height) return true;
        break;
    case FinalityPinFileStatus::INVALID:
        LogError("finality pin: refusing to overwrite an invalid pin file (%s); restore or remove it deliberately",
                 read_error);
        return false;
    }

    const uint16_t randv{FastRandomContext().rand<uint16_t>()};
    const fs::path tmp{fs::path{path.parent_path()} / fs::u8path(strprintf("%s.%04x", FINALITY_PIN_FILENAME, randv))};
    AutoFile out{fsbridge::fopen(tmp, "wb")};
    if (out.IsNull()) {
        LogError("finality pin: failed to open %s", fs::PathToString(tmp));
        return false;
    }
    try {
        HashedSourceWriter writer{out};
        writer << magic << FINALITY_PIN_FILE_VERSION << pin.height << pin.hash;
        out << writer.GetHash();
    } catch (const std::exception& e) {
        (void)out.fclose();
        fs::remove(tmp);
        LogError("finality pin: failed to serialize %s (%s)", fs::PathToString(tmp), e.what());
        return false;
    }
    if (!out.Commit()) {
        (void)out.fclose();
        fs::remove(tmp);
        LogError("finality pin: failed to flush %s", fs::PathToString(tmp));
        return false;
    }
    if (out.fclose() != 0) {
        const int errno_save{errno};
        fs::remove(tmp);
        LogError("finality pin: failed to close %s: %s", fs::PathToString(tmp), SysErrorString(errno_save));
        return false;
    }
    if (!RenameOver(tmp, path)) {
        fs::remove(tmp);
        LogError("finality pin: rename into place failed for %s", fs::PathToString(path));
        return false;
    }
    // Make the rename itself durable, not only the temporary file contents.
    // Without committing the containing directory, a sudden power loss can
    // lose the filename update even though AutoFile::Commit() succeeded.
    DirectoryCommit(path.parent_path());
    LogInfo("finality pin: persisted checkpoint %d %s", pin.height, pin.hash.ToString());
    return true;
}

} // namespace node
