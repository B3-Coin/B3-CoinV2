// Copyright (c) 2026 The B3 Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <wallet/legacy_wallet_dump.h>

#include <key_io.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/translation.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <sstream>
#include <string_view>

namespace wallet {
namespace {

constexpr size_t MAX_DUMP_BYTES{64U * 1024U * 1024U};
constexpr size_t MAX_DUMP_LINE_BYTES{16U * 1024U};
constexpr size_t MAX_DUMP_LABEL_BYTES{4096U};
constexpr size_t MAX_DUMP_KEYS{250000U};

util::Result<std::string> DecodeDumpLabel(std::string_view encoded, size_t line_number)
{
    std::string decoded;
    decoded.reserve(encoded.size());
    for (size_t pos = 0; pos < encoded.size(); ++pos) {
        unsigned char value{static_cast<unsigned char>(encoded[pos])};
        if (value == '%') {
            if (pos + 2 >= encoded.size()) {
                return util::Error{Untranslated(strprintf("Invalid encoded label on line %u", line_number))};
            }
            const int high{HexDigit(encoded[pos + 1])};
            const int low{HexDigit(encoded[pos + 2])};
            if (high < 0 || low < 0) {
                return util::Error{Untranslated(strprintf("Invalid encoded label on line %u", line_number))};
            }
            value = static_cast<unsigned char>((high << 4) | low);
            pos += 2;
        }
        if (value == 0) {
            return util::Error{Untranslated(strprintf("NUL byte in label on line %u", line_number))};
        }
        decoded.push_back(static_cast<char>(value));
        if (decoded.size() > MAX_DUMP_LABEL_BYTES) {
            return util::Error{Untranslated(strprintf("Label is too long on line %u", line_number))};
        }
    }
    return decoded;
}

bool SameEntry(const LegacyWalletDumpEntry& a, const LegacyWalletDumpEntry& b)
{
    return a.key == b.key && a.timestamp == b.timestamp && a.role == b.role &&
           a.label == b.label && a.address == b.address;
}

} // namespace

util::Result<LegacyWalletDump> ParseLegacyWalletDump(std::istream& input)
{
    LegacyWalletDump dump;
    bool found_header{false};
    bool found_end{false};
    size_t total_bytes{0};
    size_t line_number{0};
    std::map<CKeyID, size_t> seen;
    std::string line;

    while (true) {
        line.clear();
        bool have_line{false};
        for (char ch; input.get(ch);) {
            have_line = true;
            if (++total_bytes > MAX_DUMP_BYTES) {
                return util::Error{Untranslated("Legacy wallet dump exceeds the 64 MiB limit")};
            }
            if (ch == '\n') break;
            if (line.size() >= MAX_DUMP_LINE_BYTES) {
                return util::Error{Untranslated(strprintf("Line %u exceeds the size limit", line_number + 1))};
            }
            line.push_back(ch);
        }
        if (!have_line) {
            if (input.bad()) return util::Error{Untranslated("Failed while reading legacy wallet dump")};
            break;
        }
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        if (found_end) {
            return util::Error{Untranslated(strprintf("Unexpected data after end marker on line %u", line_number))};
        }

        if (line.starts_with("# Wallet dump created by B3-Coin")) {
            if (found_header) {
                return util::Error{Untranslated(strprintf("Duplicate wallet-dump header on line %u", line_number))};
            }
            found_header = true;
            continue;
        }
        if (line == "# End of dump") {
            found_end = true;
            continue;
        }
        if (line.front() == '#') continue;
        if (!found_header) {
            return util::Error{Untranslated(strprintf("Key data precedes the wallet-dump header on line %u", line_number))};
        }

        std::istringstream words{line};
        std::vector<std::string> fields;
        for (std::string field; words >> field;) fields.push_back(std::move(field));
        if (fields.size() != 5 || fields[3] != "#" || !fields[4].starts_with("addr=") || fields[4].size() == 5) {
            return util::Error{Untranslated(strprintf("Malformed key record on line %u", line_number))};
        }

        CKey key{DecodeSecret(fields[0])};
        if (!key.IsValid()) {
            return util::Error{Untranslated(strprintf("Invalid private key on line %u", line_number))};
        }

        const auto timestamp{ParseISO8601DateTime(fields[1])};
        if (!timestamp || *timestamp < 0 || FormatISO8601DateTime(*timestamp) != fields[1]) {
            return util::Error{Untranslated(strprintf("Invalid timestamp on line %u", line_number))};
        }

        LegacyWalletDumpEntry entry;
        entry.key = std::move(key);
        entry.timestamp = *timestamp;
        if (fields[2].starts_with("label=")) {
            auto label{DecodeDumpLabel(std::string_view{fields[2]}.substr(6), line_number)};
            if (!label) return util::Error{util::ErrorString(label)};
            entry.role = LegacyWalletDumpRole::LABEL;
            entry.label = std::move(*label);
        } else if (fields[2] == "change=1") {
            entry.role = LegacyWalletDumpRole::CHANGE;
        } else if (fields[2] == "reserve=1") {
            entry.role = LegacyWalletDumpRole::RESERVE;
        } else {
            return util::Error{Untranslated(strprintf("Unknown key role on line %u", line_number))};
        }

        entry.address = fields[4].substr(5);
        const CKeyID key_id{entry.key.GetPubKey().GetID()};
        if (EncodeDestination(PKHash{key_id}) != entry.address) {
            return util::Error{Untranslated(strprintf("Private key and address do not match on line %u", line_number))};
        }

        const auto [it, inserted]{seen.emplace(key_id, dump.entries.size())};
        if (!inserted) {
            if (!SameEntry(dump.entries[it->second], entry)) {
                return util::Error{Untranslated(strprintf("Conflicting duplicate key on line %u", line_number))};
            }
            continue;
        }
        if (dump.entries.size() >= MAX_DUMP_KEYS) {
            return util::Error{Untranslated("Legacy wallet dump exceeds the key-count limit")};
        }
        dump.entries.push_back(std::move(entry));
    }

    if (!found_header) return util::Error{Untranslated("Not a legacy B3 wallet dump")};
    if (!found_end) return util::Error{Untranslated("Legacy wallet dump is missing its end marker")};
    if (dump.entries.empty()) return util::Error{Untranslated("Legacy wallet dump contains no private keys")};

    dump.earliest_timestamp = std::numeric_limits<int64_t>::max();
    for (const auto& entry : dump.entries) {
        dump.earliest_timestamp = std::min(dump.earliest_timestamp, entry.timestamp);
    }
    return dump;
}

} // namespace wallet
