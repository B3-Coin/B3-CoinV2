// Copyright (c) 2026 The B3Coin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/key_io.h>

#include <base58.h>
#include <bech32.h>
#include <chainparams.h>

#include <optional>
#include <string>
#include <vector>

namespace test {
namespace {

std::optional<std::string> TranslateBitcoinMainBase58(
    const std::string& encoded,
    const CChainParams& target_params,
    const bool wif_only)
{
    std::vector<unsigned char> data;
    if (!DecodeBase58Check(encoded, data, /*max_ret_len=*/128) || data.empty()) {
        return std::nullopt;
    }

    std::optional<CChainParams::Base58Type> type;
    if (data[0] == 128 &&
        (data.size() == 33 || (data.size() == 34 && data.back() == 1))) {
        type = CChainParams::SECRET_KEY;
    } else if (!wif_only && data[0] == 0 && data.size() == 21) {
        type = CChainParams::PUBKEY_ADDRESS;
    } else if (!wif_only && data[0] == 5 && data.size() == 21) {
        type = CChainParams::SCRIPT_ADDRESS;
    }
    if (!type) return std::nullopt;

    std::vector<unsigned char> translated{target_params.Base58Prefix(*type)};
    translated.insert(translated.end(), data.begin() + 1, data.end());
    return EncodeBase58Check(translated);
}

bool IsBase58Char(const char c)
{
    static constexpr std::string_view ALPHABET{
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"};
    return ALPHABET.find(c) != std::string_view::npos;
}

} // namespace

std::string TranslateBitcoinMainKeyIO(
    const std::string_view encoded,
    const CChainParams& target_params)
{
    const std::string source{encoded};
    if (const auto translated{
            TranslateBitcoinMainBase58(source, target_params, /*wif_only=*/false)}) {
        return *translated;
    }

    const auto decoded{bech32::Decode(source)};
    if (decoded.encoding != bech32::Encoding::INVALID && decoded.hrp == "bc") {
        return bech32::Encode(
            decoded.encoding, target_params.Bech32HRP(), decoded.data);
    }
    return source;
}

std::string TranslateBitcoinMainWIFs(
    std::string text,
    const CChainParams& target_params)
{
    size_t begin{0};
    while (begin < text.size()) {
        while (begin < text.size() && !IsBase58Char(text[begin])) ++begin;
        size_t end{begin};
        while (end < text.size() && IsBase58Char(text[end])) ++end;
        if (begin == end) break;

        const std::string token{text.substr(begin, end - begin)};
        if (const auto translated{
                TranslateBitcoinMainBase58(token, target_params, /*wif_only=*/true)}) {
            text.replace(begin, token.size(), *translated);
            end = begin + translated->size();
        }
        begin = end;
    }
    return text;
}

} // namespace test
