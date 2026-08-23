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

std::optional<FinalityPin> ReadFinalityPin(const fs::path& path, const MessageStartChars& magic)
{
    if (!fs::exists(path)) return std::nullopt;
    AutoFile file{fsbridge::fopen(path, "rb")};
    if (file.IsNull()) {
        LogWarning("finality pin: cannot open %s", fs::PathToString(path));
        return std::nullopt;
    }
    try {
        HashVerifier verifier{file};
        MessageStartChars file_magic;
        verifier >> file_magic;
        if (file_magic != magic) {
            LogWarning("finality pin: %s belongs to another network; ignored", fs::PathToString(path));
            return std::nullopt;
        }
        uint8_t version{0};
        verifier >> version;
        if (version != FINALITY_PIN_FILE_VERSION) {
            LogWarning("finality pin: %s has unknown version %d; ignored", fs::PathToString(path), version);
            return std::nullopt;
        }
        FinalityPin pin;
        verifier >> pin.height >> pin.hash;
        uint256 checksum;
        file >> checksum;
        if (checksum != verifier.GetHash()) {
            LogWarning("finality pin: %s checksum mismatch; ignored", fs::PathToString(path));
            return std::nullopt;
        }
        if (pin.height < 0 || pin.hash.IsNull()) {
            LogWarning("finality pin: %s holds an invalid pin; ignored", fs::PathToString(path));
            return std::nullopt;
        }
        return pin;
    } catch (const std::exception& e) {
        LogWarning("finality pin: %s unreadable (%s); ignored", fs::PathToString(path), e.what());
        return std::nullopt;
    }
}

bool WriteFinalityPin(const fs::path& path, const MessageStartChars& magic, const FinalityPin& pin)
{
    if (pin.height < 0 || pin.hash.IsNull()) return false;
    // Monotone: never lower a persisted pin.
    if (const auto existing{ReadFinalityPin(path, magic)}; existing && existing->height >= pin.height) return true;

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
    LogInfo("finality pin: persisted checkpoint %d %s", pin.height, pin.hash.ToString());
    return true;
}

} // namespace node
