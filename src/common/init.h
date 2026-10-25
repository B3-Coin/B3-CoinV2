// Copyright (c) 2023-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COMMON_INIT_H
#define BITCOIN_COMMON_INIT_H

#include <util/translation.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class ArgsManager;

namespace common {
//! The longest path Windows accepts without long-path support (MAX_PATH less
//! the terminating NUL).
inline constexpr size_t WINDOWS_MAX_PATH_LENGTH{259};
//! The longest path, in characters, that the node creates below its
//! network data directory with the default options: the asset metadata
//! index's LevelDB manifest,
//! `\indexes\asset_metadata\<64 hex>\db\MANIFEST-000002` (107 characters),
//! with 5 characters of room for LevelDB's file numbers to grow. User-named
//! wallets and the finality signer's state files (up to 186 characters,
//! staking only) can be longer and are not counted.
inline constexpr size_t DATADIR_LONGEST_RELATIVE_PATH{112};
//! The longest network data directory the node accepts on Windows when long
//! paths are disabled.
inline constexpr size_t MAX_WINDOWS_DATADIR_LENGTH{WINDOWS_MAX_PATH_LENGTH - DATADIR_LONGEST_RELATIVE_PATH};

enum class ConfigStatus {
    FAILED,       //!< Failed generically.
    FAILED_WRITE, //!< Failed to write settings.json
    ABORTED,      //!< Aborted by user
};

struct ConfigError {
    ConfigStatus status;
    bilingual_str message{};
    std::vector<std::string> details{};
};

//! Callback function to let the user decide whether to abort loading if
//! settings.json file exists and can't be parsed, or to ignore the error and
//! overwrite the file.
using SettingsAbortFn = std::function<bool(const bilingual_str& message, const std::vector<std::string>& details)>;

/** On Windows without long-path support, refuse a network data directory
 *  too long for the files the node creates below it, before anything is
 *  created. Elsewhere, and with long paths enabled, always accept. */
std::optional<ConfigError> CheckDataDirPathLength(const ArgsManager& args);

/* Read config files, and create datadir and settings.json if they don't exist. */
std::optional<ConfigError> InitConfig(ArgsManager& args, SettingsAbortFn settings_abort_fn = nullptr);
} // namespace common

#endif // BITCOIN_COMMON_INIT_H
