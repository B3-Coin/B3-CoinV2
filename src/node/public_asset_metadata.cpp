// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <node/public_asset_metadata.h>

#include <consensus/amount.h>
#include <univalue.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

namespace node {
namespace {

constexpr size_t MAX_CATALOG_RECORDS{256};
constexpr size_t MAX_CATALOG_BYTES{256 * 1024};
constexpr std::array<std::string_view, 8> METADATA_FIELDS{
    "domain", "asset_id", "name", "ticker", "issuance_txid", "issuance_vout", "max_supply", "decimals"};

bool MetadataFields(const UniValue& value, std::string& error)
{
    if (!value.isObject()) {
        error = "Asset metadata must be an object";
        return false;
    }
    std::set<std::string_view> seen;
    for (const auto& key : value.getKeys()) {
        if (!seen.insert(key).second ||
            std::find(METADATA_FIELDS.begin(), METADATA_FIELDS.end(), key) == METADATA_FIELDS.end()) {
            error = "Asset metadata has an unknown or duplicate field";
            return false;
        }
    }
    if (seen.size() != METADATA_FIELDS.size()) {
        error = "Asset metadata is missing a required field";
        return false;
    }
    return true;
}

std::optional<uint256> MetadataId(const UniValue& value, const char* key, std::string& error)
{
    const auto& field{value[key]};
    if (field.isStr()) {
        const auto id{uint256::FromHex(field.get_str())};
        if (id && !id->IsNull()) return id;
    }
    error = std::string{"Asset metadata requires a nonzero 64-character hex identifier: "} + key;
    return std::nullopt;
}

std::optional<uint64_t> MetadataInteger(const UniValue& value, const char* key,
                                       uint64_t maximum, std::string& error)
{
    const auto& field{value[key]};
    if (field.isNum()) {
        const auto& text{field.getValStr()};
        uint64_t number{0};
        const auto [end, status]{std::from_chars(text.data(), text.data() + text.size(), number)};
        if (status == std::errc{} && end == text.data() + text.size() && number <= maximum) return number;
    }
    error = std::string{"Asset metadata requires a bounded nonnegative integer: "} + key;
    return std::nullopt;
}

bool SafeLabel(const std::string& text, size_t maximum, bool spaces)
{
    const auto alnum = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    return !text.empty() && text.size() <= maximum && alnum(text.front()) && alnum(text.back()) &&
           std::all_of(text.begin(), text.end(), [&](unsigned char c) {
               return alnum(c) || c == '.' || c == '-' || c == '_' || (spaces && c == ' ');
           });
}

bool ReservedLabel(const std::string& text)
{
    std::string normalized;
    for (char c : text) {
        if (c == ' ' || c == '.' || c == '-' || c == '_') continue;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        normalized.push_back(c);
    }
    return normalized == "b3" || normalized == "fn" || normalized == "fncoin" ||
           normalized == "busd" || normalized == "tusd" || normalized == "cusd" ||
           normalized == "testusd" || normalized == "bridgedusd" || normalized == "cusdunbackedtest";
}

} // namespace

std::optional<modern::AssetDisplayMetadata> ParsePublicAssetMetadata(
    const UniValue& value, const uint256& domain, const uint256& asset, std::string& error)
{
    error.clear();
    if (!MetadataFields(value, error)) return std::nullopt;
    const auto record_domain{MetadataId(value, "domain", error)};
    if (!record_domain) return std::nullopt;
    const auto record_asset{MetadataId(value, "asset_id", error)};
    if (!record_asset) return std::nullopt;
    if (*record_domain != domain || *record_asset != asset) {
        error = "Asset metadata does not match the pinned chain domain and asset id";
        return std::nullopt;
    }
    if (!value["name"].isStr() || !value["ticker"].isStr() ||
        !SafeLabel(value["name"].get_str(), 64, true) || !SafeLabel(value["ticker"].get_str(), 12, false)) {
        error = "Asset metadata name (1-64) and ticker (1-12) must use safe ASCII labels with letters or digits at both ends";
        return std::nullopt;
    }
    const auto& name{value["name"].get_str()};
    const auto& ticker{value["ticker"].get_str()};
    if (ReservedLabel(name) || ReservedLabel(ticker)) {
        error = "Asset metadata uses a reserved configured asset label";
        return std::nullopt;
    }
    const auto txid{MetadataId(value, "issuance_txid", error)};
    if (!txid) return std::nullopt;
    const auto vout{MetadataInteger(value, "issuance_vout", std::numeric_limits<uint32_t>::max(), error)};
    if (!vout) return std::nullopt;
    const auto max_supply{MetadataInteger(value, "max_supply", static_cast<uint64_t>(MAX_MONEY), error)};
    if (!max_supply) return std::nullopt;
    const auto decimals{MetadataInteger(value, "decimals", modern::ASSET_MAX_DECIMALS, error)};
    if (!decimals) return std::nullopt;

    modern::AssetDisplayMetadata metadata{
        name, ticker,
        {COutPoint{Txid::FromUint256(*txid), static_cast<uint32_t>(*vout)},
         *max_supply, static_cast<uint8_t>(*decimals)}, {}};
    if (!modern::VerifyAssetMetadataProof(domain, asset, metadata.proof)) {
        error = "Asset metadata genesis proof is invalid or does not match its asset id";
        return std::nullopt;
    }
    return metadata;
}

UniValue PublicAssetMetadataJson(const uint256& domain, const uint256& asset,
                                   const modern::AssetDisplayMetadata& metadata)
{
    UniValue value{UniValue::VOBJ};
    value.pushKV("domain", domain.GetHex());
    value.pushKV("asset_id", asset.GetHex());
    value.pushKV("name", metadata.name);
    value.pushKV("ticker", metadata.ticker);
    value.pushKV("issuance_txid", metadata.proof.issuance_prevout.hash.GetHex());
    value.pushKV("issuance_vout", metadata.proof.issuance_prevout.n);
    value.pushKV("max_supply", metadata.proof.max_supply);
    value.pushKV("decimals", metadata.proof.decimals);
    return value;
}

bool LoadPublicAssetMetadataCatalog(const fs::path& path, const uint256& domain,
                                    PublicAssetMetadataCatalog& catalog, std::string& error)
{
    error.clear();
    if (path.empty()) {
        catalog.clear();
        return true;
    }
    if (domain.IsNull()) {
        error = "Public asset metadata catalog requires a pinned chain domain";
        return false;
    }
    std::error_code file_error;
    if (!fs::is_regular_file(path, file_error) || file_error) {
        error = "Public asset metadata catalog must be a readable regular file";
        return false;
    }
    std::ifstream file{path.std_path(), std::ios::binary};
    if (!file.is_open()) {
        error = "Unable to open public asset metadata catalog";
        return false;
    }
    // Read one extra byte to reject oversized files, including files that grew
    // after the regular-file check. Never allocate based on an input file size.
    std::string contents(MAX_CATALOG_BYTES + 1, '\0');
    file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (file.bad() || (file.fail() && !file.eof())) {
        error = "Unable to read public asset metadata catalog";
        return false;
    }
    if (file.gcount() > static_cast<std::streamsize>(MAX_CATALOG_BYTES)) {
        error = "Public asset metadata catalog exceeds the 256 KiB limit";
        return false;
    }
    contents.resize(static_cast<size_t>(file.gcount()));
    return ParsePublicAssetMetadataCatalog(contents, domain, catalog, error);
}

bool ParsePublicAssetMetadataCatalog(std::string_view contents, const uint256& domain,
                                     PublicAssetMetadataCatalog& catalog, std::string& error)
{
    error.clear();
    if (domain.IsNull()) {
        error = "Public asset metadata catalog requires a pinned chain domain";
        return false;
    }
    if (contents.size() > MAX_CATALOG_BYTES) {
        error = "Public asset metadata catalog exceeds the 256 KiB limit";
        return false;
    }
    UniValue records;
    if (!records.read(contents) || !records.isArray()) {
        error = "Public asset metadata catalog must contain a JSON array";
        return false;
    }
    if (records.size() > MAX_CATALOG_RECORDS) {
        error = "Public asset metadata catalog exceeds the 256-record limit";
        return false;
    }
    PublicAssetMetadataCatalog loaded;
    for (size_t i{0}; i < records.size(); ++i) {
        const auto& record{records[i]};
        // Validate the shape before extracting the map key, including duplicates.
        if (!MetadataFields(record, error)) {
            error = "Public asset metadata catalog record " + std::to_string(i + 1) + ": " + error;
            return false;
        }
        const auto asset{MetadataId(record, "asset_id", error)};
        const auto metadata{asset ? ParsePublicAssetMetadata(record, domain, *asset, error) : std::nullopt};
        if (!metadata) {
            error = "Public asset metadata catalog record " + std::to_string(i + 1) + ": " + error;
            return false;
        }
        if (!loaded.emplace(*asset, *metadata).second) {
            error = "Public asset metadata catalog contains a duplicate asset id";
            return false;
        }
    }
    catalog = std::move(loaded);
    return true;
}

} // namespace node
