// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#ifndef B3COIN_NODE_FINALITY_RECOVERY_OPTIONS_H
#define B3COIN_NODE_FINALITY_RECOVERY_OPTIONS_H

#include <consensus/finality_signer_recovery.h>
#include <univalue.h>
#include <util/fs.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace node {

inline constexpr size_t MAX_FINALITY_RECOVERY_MANIFEST_BYTES{16 * 1024};

namespace finality_recovery_options_detail {
inline int HexDigit(const char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool ExactHex(const std::string_view value)
{
    return value.size() == 64 && std::all_of(value.begin(), value.end(),
                                            [](char c) { return HexDigit(c) >= 0; });
}

inline bool UnsignedInteger(const UniValue& value, const uint64_t maximum,
                            uint64_t& result)
{
    if (!value.isNum()) return false;
    const std::string& text{value.getValStr()};
    if (text.empty() || !std::all_of(text.begin(), text.end(),
                                    [](char c) { return c >= '0' && c <= '9'; })) return false;
    const auto parsed{std::from_chars(text.data(), text.data() + text.size(), result)};
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && result <= maximum;
}
} // namespace finality_recovery_options_detail

/** Parse public, operator-approved incident data, not a cryptographic proof.
 * No journal is opened or changed. On failure, output is unchanged.
 */
inline bool ParseFinalityRecoveryManifest(
    const std::string_view json, const std::string_view accepted_anchor,
    Consensus::FinalitySignerRecovery& output, std::string& error)
{
    using namespace finality_recovery_options_detail;
    error.clear();
    auto reject = [&](const std::string& message) {
        error = "invalid finality recovery manifest: " + message;
        return false;
    };
    if (json.empty() || json.size() > MAX_FINALITY_RECOVERY_MANIFEST_BYTES) {
        return reject("must contain between 1 and 16384 bytes");
    }
    if (!ExactHex(accepted_anchor)) {
        return reject("-acceptfinalityrecovery must be the exact 64-hex anchor hash");
    }
    UniValue object;
    if (!object.read(json) || !object.isObject()) return reject("expected one JSON object");
    const std::set<std::string> fields{
        "version", "chain_domain", "validator_key", "incident_height",
        "incident_block_hash", "incident_epoch", "incident_signing_set_hash",
        "incident_successor_set_hash", "anchor_height", "anchor_block_hash"};
    std::set<std::string> seen;
    for (const std::string& name : object.getKeys()) {
        if (!fields.contains(name)) return reject("unknown field");
        if (!seen.insert(name).second) return reject("duplicate field");
    }
    if (seen != fields) return reject("missing required field");
    uint64_t version{0}, incident_height{0}, epoch{0}, anchor_height{0};
    if (!UnsignedInteger(object["version"], 1, version) || version != 1) {
        return reject("version must be integer 1");
    }
    if (!UnsignedInteger(object["incident_height"], std::numeric_limits<int>::max(), incident_height) ||
        !UnsignedInteger(object["anchor_height"], std::numeric_limits<int>::max(), anchor_height) ||
        !UnsignedInteger(object["incident_epoch"], std::numeric_limits<uint64_t>::max(), epoch)) {
        return reject("heights and epoch must be nonnegative in-range JSON integers");
    }
    constexpr std::array<std::string_view, 6> hex_fields{
        "chain_domain", "validator_key", "incident_block_hash",
        "incident_signing_set_hash", "incident_successor_set_hash", "anchor_block_hash"};
    for (const auto name : hex_fields) {
        const UniValue& value{object.find_value(name)};
        if (!value.isStr() || !ExactHex(value.get_str())) {
            return reject(std::string{name} + " must be a 64-hex string without prefix or whitespace");
        }
    }
    Consensus::FinalitySignerRecovery candidate;
    candidate.chain_domain = *uint256::FromHex(object["chain_domain"].get_str());
    std::array<unsigned char, 32> validator{};
    const std::string& key{object["validator_key"].get_str()};
    for (size_t i{0}; i < validator.size(); ++i) {
        validator[i] = static_cast<unsigned char>(
            (finality_recovery_options_detail::HexDigit(key[2 * i]) << 4) |
            finality_recovery_options_detail::HexDigit(key[2 * i + 1]));
    }
    candidate.validator_key = validator;
    candidate.incident_height = static_cast<int>(incident_height);
    candidate.incident_block_hash = *uint256::FromHex(object["incident_block_hash"].get_str());
    candidate.incident_epoch = epoch;
    candidate.incident_signing_set_hash = *uint256::FromHex(object["incident_signing_set_hash"].get_str());
    candidate.incident_successor_set_hash = *uint256::FromHex(object["incident_successor_set_hash"].get_str());
    candidate.anchor_height = static_cast<int>(anchor_height);
    candidate.anchor_block_hash = *uint256::FromHex(object["anchor_block_hash"].get_str());
    if (!candidate.Valid()) return reject("zero fields, invalid target, or anchor not strictly after the distinct incident");
    const uint256 acknowledged{*uint256::FromHex(accepted_anchor)};
    if (candidate.anchor_block_hash != acknowledged) return reject("anchor hash does not match -acceptfinalityrecovery");
    output = std::move(candidate);
    return true;
}

/** Load at most one explicit public manifest once during startup. Relative
 * paths use the network datadir. No default path, downloads or live reload.
 * Existing output is left unchanged on any failure.
 */
inline bool LoadFinalityRecoveryOptions(
    const std::vector<std::string>& files, const std::vector<std::string>& accepted_anchors,
    const fs::path& network_datadir, const uint256& expected_domain,
    std::optional<Consensus::FinalitySignerRecovery>& output, std::string& error)
{
    error.clear();
    if (files.empty() && accepted_anchors.empty()) {
        output.reset();
        return true;
    }
    if (files.size() != 1 || accepted_anchors.size() != 1 || files.front().empty()) {
        error = "-finalityrecoveryfile and -acceptfinalityrecovery must both be supplied exactly once with explicit values";
        return false;
    }
    if (!finality_recovery_options_detail::ExactHex(accepted_anchors.front())) {
        error = "-acceptfinalityrecovery must be the exact 64-hex anchor hash";
        return false;
    }
    if (expected_domain.IsNull()) {
        error = "operator-trusted finality recovery requires a configured modern chain domain";
        return false;
    }
    fs::path file{fs::u8path(files.front())};
    if (!file.is_absolute()) file = network_datadir / file;
    std::error_code ec;
    if (!fs::is_regular_file(file, ec) || ec) {
        error = "-finalityrecoveryfile must name a readable regular file";
        return false;
    }
    const auto size{fs::file_size(file, ec)};
    if (ec || size == 0 || size > MAX_FINALITY_RECOVERY_MANIFEST_BYTES) {
        error = "-finalityrecoveryfile must contain between 1 and 16384 bytes";
        return false;
    }
    std::ifstream stream{file.std_path(), std::ios::binary};
    if (!stream) {
        error = "cannot open -finalityrecoveryfile for reading";
        return false;
    }
    // Do not trust a pre-open size check if the file grows between checks.
    std::array<char, MAX_FINALITY_RECOVERY_MANIFEST_BYTES + 1> bytes{};
    stream.read(bytes.data(), bytes.size());
    if (stream.bad() || !stream.eof() || stream.gcount() <= 0 ||
        stream.gcount() > static_cast<std::streamsize>(MAX_FINALITY_RECOVERY_MANIFEST_BYTES)) {
        error = "could not read a bounded complete finality recovery manifest";
        return false;
    }
    Consensus::FinalitySignerRecovery candidate;
    if (!ParseFinalityRecoveryManifest(
            std::string_view{bytes.data(), static_cast<size_t>(stream.gcount())},
            accepted_anchors.front(), candidate, error)) return false;
    if (candidate.chain_domain != expected_domain) {
        error = "finality recovery manifest belongs to a different modern chain domain";
        return false;
    }
    output = std::move(candidate);
    return true;
}

} // namespace node

#endif // B3COIN_NODE_FINALITY_RECOVERY_OPTIONS_H
