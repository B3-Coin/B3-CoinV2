// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/utxo_rows.h>

#include <consensus/amount.h>
#include <script/script.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <algorithm>
#include <istream>
#include <limits>
#include <ostream>
#include <utility>

namespace node {

namespace {
constexpr const char* FORMAT_TAG{"b3-utxo-rows/v1"};
constexpr const char* TIP_HASH_PREFIX{"tip_hash="};
constexpr const char* TIP_HEIGHT_PREFIX{"tip_height="};
constexpr const char* COUNT_PREFIX{"count="};
//! Coin::nHeight is a 30-bit field; a row claiming more is malformed.
constexpr int64_t MAX_ROW_HEIGHT{(int64_t{1} << 30) - 1};
} // namespace

std::string UtxoRowLine(const UtxoEntry& entry)
{
    const CScript& script{entry.coin.out.scriptPubKey};
    return strprintf("%s:%u %d %u %d %d %u %u %s",
                     entry.outpoint.hash.GetHex(), entry.outpoint.n,
                     entry.coin.out.nValue, unsigned{entry.coin.nHeight},
                     entry.coin.fCoinBase ? 1 : 0, entry.coin.fCoinStake ? 1 : 0,
                     entry.coin.nTime, entry.coin.nTxOffset,
                     script.empty() ? std::string{"-"} : HexStr(script));
}

bool WriteUtxoRows(std::ostream& out, UtxoRowsFile file, std::string& error)
{
    std::sort(file.entries.begin(), file.entries.end(),
              [](const UtxoEntry& a, const UtxoEntry& b) { return a.outpoint < b.outpoint; });
    out << FORMAT_TAG << '\n';
    out << TIP_HASH_PREFIX << file.tip_hash.GetHex() << '\n';
    out << TIP_HEIGHT_PREFIX << file.tip_height << '\n';
    for (const UtxoEntry& entry : file.entries) {
        out << UtxoRowLine(entry) << '\n';
    }
    out << COUNT_PREFIX << file.entries.size() << '\n';
    out.flush();
    if (!out) {
        error = "write failure";
        return false;
    }
    return true;
}

bool ReadUtxoRows(std::istream& in, UtxoRowsFile& out, std::string& error)
{
    out = UtxoRowsFile{};
    std::string line;

    const auto fail{[&error](std::string msg) {
        error = std::move(msg);
        return false;
    }};
    const auto next_line{[&in, &line]() -> bool {
        if (!std::getline(in, line)) return false;
        // Tolerate CRLF input; everything else is exact.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    }};

    if (!next_line() || line != FORMAT_TAG) {
        return fail(strprintf("missing or unsupported format tag (expected %s)", FORMAT_TAG));
    }
    if (!next_line() || line.rfind(TIP_HASH_PREFIX, 0) != 0) {
        return fail("missing tip_hash header");
    }
    if (const auto hash{uint256::FromHex(line.substr(std::string{TIP_HASH_PREFIX}.size()))}) {
        out.tip_hash = *hash;
    } else {
        return fail("malformed tip_hash header");
    }
    if (!next_line() || line.rfind(TIP_HEIGHT_PREFIX, 0) != 0) {
        return fail("missing tip_height header");
    }
    if (const auto height{ToIntegral<int>(line.substr(std::string{TIP_HEIGHT_PREFIX}.size()))};
        height && *height >= 0) {
        out.tip_height = *height;
    } else {
        return fail("malformed tip_height header");
    }

    bool have_count{false};
    uint64_t declared_count{0};
    while (next_line()) {
        if (line.rfind(COUNT_PREFIX, 0) == 0) {
            const auto count{ToIntegral<uint64_t>(line.substr(std::string{COUNT_PREFIX}.size()))};
            if (!count) return fail("malformed count line");
            declared_count = *count;
            have_count = true;
            break;
        }

        const size_t row_number{out.entries.size() + 1};
        const std::vector<std::string> fields{util::SplitString(line, ' ')};
        if (fields.size() != 8) {
            return fail(strprintf("row %d: expected 8 fields, found %d", row_number, fields.size()));
        }
        const size_t colon{fields[0].find(':')};
        if (colon == std::string::npos) {
            return fail(strprintf("row %d: malformed outpoint", row_number));
        }

        UtxoEntry entry;
        const auto txid{uint256::FromHex(fields[0].substr(0, colon))};
        const auto vout{ToIntegral<uint32_t>(fields[0].substr(colon + 1))};
        if (!txid || !vout) {
            return fail(strprintf("row %d: malformed outpoint", row_number));
        }
        entry.outpoint = COutPoint{Txid::FromUint256(*txid), *vout};

        const auto value{ToIntegral<int64_t>(fields[1])};
        if (!value || *value < 0) return fail(strprintf("row %d: malformed value", row_number));
        const auto height{ToIntegral<int64_t>(fields[2])};
        if (!height || *height < 0 || *height > MAX_ROW_HEIGHT) {
            return fail(strprintf("row %d: malformed height", row_number));
        }
        if (fields[3] != "0" && fields[3] != "1") return fail(strprintf("row %d: malformed coinbase flag", row_number));
        if (fields[4] != "0" && fields[4] != "1") return fail(strprintf("row %d: malformed coinstake flag", row_number));
        const auto ntime{ToIntegral<uint32_t>(fields[5])};
        if (!ntime) return fail(strprintf("row %d: malformed ntime", row_number));
        const auto offset{ToIntegral<uint32_t>(fields[6])};
        if (!offset) return fail(strprintf("row %d: malformed ntxoffset", row_number));

        CScript script;
        if (fields[7] != "-") {
            const auto bytes{TryParseHex<unsigned char>(fields[7])};
            if (!bytes || bytes->empty()) return fail(strprintf("row %d: malformed script hex", row_number));
            script = CScript{bytes->begin(), bytes->end()};
        }

        entry.coin = Coin{CTxOut{*value, script}, static_cast<int>(*height),
                          fields[3] == "1", fields[4] == "1", *ntime, *offset};

        if (!out.entries.empty() && !(out.entries.back().outpoint < entry.outpoint)) {
            return fail(strprintf("row %d: outpoints out of canonical order or duplicated", row_number));
        }
        out.entries.push_back(std::move(entry));
    }

    if (!have_count) return fail("missing count line");
    if (declared_count != out.entries.size()) {
        return fail(strprintf("count line says %d rows but %d were read", declared_count, out.entries.size()));
    }
    while (next_line()) {
        if (!line.empty()) return fail("unexpected content after the count line");
    }
    return true;
}

} // namespace node
