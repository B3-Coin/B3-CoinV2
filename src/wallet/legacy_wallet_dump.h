// Copyright (c) 2026 The B3 Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef BITCOIN_WALLET_LEGACY_WALLET_DUMP_H
#define BITCOIN_WALLET_LEGACY_WALLET_DUMP_H

#include <key.h>
#include <util/result.h>

#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace wallet {

enum class LegacyWalletDumpRole {
    LABEL,
    CHANGE,
    RESERVE,
};

struct LegacyWalletDumpEntry {
    CKey key;
    int64_t timestamp{0};
    LegacyWalletDumpRole role{LegacyWalletDumpRole::LABEL};
    std::optional<std::string> label;
    std::string address;
};

struct LegacyWalletDump {
    std::vector<LegacyWalletDumpEntry> entries;
    int64_t earliest_timestamp{0};
};

/**
 * Strictly parse the human-readable WIF dump emitted by the legacy B3
 * `dumpwallet` RPC. The complete input is checked before a result is returned.
 * Error messages contain only line numbers and never echo secret input.
 */
util::Result<LegacyWalletDump> ParseLegacyWalletDump(std::istream& input);

} // namespace wallet

#endif // BITCOIN_WALLET_LEGACY_WALLET_DUMP_H
