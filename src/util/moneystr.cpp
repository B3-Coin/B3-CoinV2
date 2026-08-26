// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <util/moneystr.h>

#include <consensus/amount.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/string.h>

#include <cstdint>
#include <optional>

using util::ContainsNoNUL;
using util::TrimString;

std::string FormatMoney(const CAmount n)
{
    // Note: not using straight sprintf here because we do NOT want
    // localized number formatting.
    static_assert(KILO_COIN > 1);
    int64_t quotient = n / KILO_COIN;
    int64_t remainder = n % KILO_COIN;
    if (n < 0) {
        quotient = -quotient;
        remainder = -remainder;
    }
    std::string str = strprintf("%d.%09d", quotient, remainder);

    // Right-trim excess zeros before the decimal point:
    int nTrim = 0;
    for (int i = str.size()-1; (str[i] == '0' && IsDigit(str[i-2])); --i)
        ++nTrim;
    if (nTrim)
        str.erase(str.size()-nTrim, nTrim);

    if (n < 0)
        str.insert(uint32_t{0}, 1, '-');
    return str;
}


std::optional<CAmount> ParseMoney(const std::string& money_string)
{
    if (!ContainsNoNUL(money_string)) {
        return std::nullopt;
    }
    const std::string str = TrimString(money_string);
    if (str.empty()) {
        return std::nullopt;
    }

    std::string strWhole;
    int64_t nUnits = 0;
    const char* p = str.c_str();
    for (; *p; p++)
    {
        if (*p == '.')
        {
            p++;
            int64_t nMult = KILO_COIN / 10;
            while (IsDigit(*p) && (nMult > 0))
            {
                nUnits += nMult * (*p++ - '0');
                nMult /= 10;
            }
            break;
        }
        if (IsSpace(*p))
            return std::nullopt;
        if (!IsDigit(*p))
            return std::nullopt;
        strWhole.insert(strWhole.end(), *p);
    }
    if (*p) {
        return std::nullopt;
    }
    if (strWhole.size() > 9) // guard against 63 bit overflow (9 digits * 1e9 < 2^63)
        return std::nullopt;
    if (nUnits < 0 || nUnits > KILO_COIN)
        return std::nullopt;
    int64_t nWhole = LocaleIndependentAtoi<int64_t>(strWhole);
    CAmount value = nWhole * KILO_COIN + nUnits;

    if (!MoneyRange(value)) {
        return std::nullopt;
    }

    return value;
}
